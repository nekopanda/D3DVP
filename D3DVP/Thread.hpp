#pragma once

#include <Windows.h>
#include <process.h>

#include <deque>

// コピー禁止オブジェクト
class NonCopyable
{
protected:
	NonCopyable() {}
	~NonCopyable() {} /// protected な非仮想デストラクタ
private:
	NonCopyable(const NonCopyable &);
	NonCopyable& operator=(const NonCopyable &) { }
};

// with idiom サポート //

template <typename T>
class WithHolder
{
public:
	WithHolder(T& obj) : obj_(obj) {
		obj_.enter();
	}
	~WithHolder() {
		obj_.exit();
	}
private:
	T& obj_;
};

template <typename T>
WithHolder<T> with(T& obj) {
	return WithHolder<T>(obj);
}

class CondWait;

class CriticalSection : NonCopyable
{
public:
	CriticalSection() {
		InitializeCriticalSection(&critical_section_);
	}
	~CriticalSection() {
		DeleteCriticalSection(&critical_section_);
	}
	void enter() {
		EnterCriticalSection(&critical_section_);
	}
	void exit() {
		LeaveCriticalSection(&critical_section_);
	}
private:
	CRITICAL_SECTION critical_section_;

	friend CondWait;
};

class CondWait : NonCopyable
{
public:
	CondWait()
		: cond_val_(CONDITION_VARIABLE_INIT)
	{ }
	void wait(CriticalSection& cs) {
		SleepConditionVariableCS(&cond_val_, &cs.critical_section_, INFINITE);
	}
	void signal() {
		WakeConditionVariable(&cond_val_);
	}
	void broadcast() {
		WakeAllConditionVariable(&cond_val_);
	}
private:
	CONDITION_VARIABLE cond_val_;
};

class Stopwatch
{
	int64_t sum;
	int64_t prev;
public:
	Stopwatch()
		: sum(0)
	{
		//
	}

	void reset() {
		sum = 0;
	}

	void start() {
		QueryPerformanceCounter((LARGE_INTEGER*)&prev);
	}

	void stop() {
		int64_t cur;
		QueryPerformanceCounter((LARGE_INTEGER*)&cur);
		sum += cur - prev;
		prev = cur;
	}

	double getTotal() const {
		int64_t freq;
		QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		return (double)sum / freq;
	}

	double getAndReset() {
		stop();
		double ret = getTotal();
		sum = 0;
		return ret;
	}
};

// スレッドはstart()で開始（コンストラクタから仮想関数を呼ぶことはできないため）
// run()は派生クラスで実装されているのでrun()が終了する前に派生クラスのデストラクタが終了しないように注意！
// 安全のためjoin()が完了していない状態でThreadBaseのデストラクタに入るとエラーとする
template <typename ErrorHandler>
class ThreadBase
{
public:
	ErrorHandler* env;

	ThreadBase(ErrorHandler* env) : env(env), thread_handle_(NULL) { }
	~ThreadBase() {
		if (thread_handle_ != NULL) {
			env->ThrowError("finish join() before destroy object ...");
		}
	}
	void start() {
		if (thread_handle_ != NULL) {
			env->ThrowError("thread already started ...");
		}
		thread_handle_ = (HANDLE)_beginthreadex(NULL, 0, thread_, this, 0, NULL);
		if (thread_handle_ == (HANDLE)-1) {
			env->ThrowError("failed to begin pump thread ...");
		}
	}
	void join() {
		if (thread_handle_ != NULL) {
			WaitForSingleObject(thread_handle_, INFINITE);
			CloseHandle(thread_handle_);
			thread_handle_ = NULL;
		}
	}
	bool isRunning() { return thread_handle_ != NULL; }

protected:
	virtual void run() = 0;

private:
	HANDLE thread_handle_;

	static unsigned __stdcall thread_(void* arg) {
		try {
			static_cast<ThreadBase*>(arg)->run();
		}
		catch (const AvisynthError& e) {
			throw e;
		}
		return 0;
	}
};

template <typename T, typename ErrorHandler, bool PERF = false>
class DataPumpThread : private ThreadBase<ErrorHandler>
{
public:
	DataPumpThread(size_t maximum, ErrorHandler* env)
		: ThreadBase(env)
		, maximum_(maximum)
		, current_(0)
		, finished_(false)
	{ }

	~DataPumpThread() {
		if (isRunning()) {
			env->ThrowError("call join() before destroy object ...");
		}
	}

	void put(T&& data)
	{
		auto& lock = with(critical_section_);
		while (current_ >= maximum_) {
			if (PERF) producer.start();
			cond_full_.wait(critical_section_);
			if (PERF) producer.stop();
		}
		if (data_.size() == 0) {
			cond_empty_.signal();
		}
		data_.emplace_back(data);
		current_ += 1;
	}

	void start() {
		finished_ = false;
		producer.reset();
		consumer.reset();
		ThreadBase::start();
	}

	void join() {
		{
			auto& lock = with(critical_section_);
			finished_ = true;
			cond_empty_.signal();
		}
		ThreadBase::join();
	}

	bool isRunning() { return ThreadBase::isRunning(); }

	void getTotalWait(double& prod, double& cons) {
		prod = producer.getTotal();
		cons = consumer.getTotal();
	}

protected:
	virtual void OnDataReceived(T&& data) = 0;

private:
	CriticalSection critical_section_;
	CondWait cond_full_;
	CondWait cond_empty_;

	std::deque<T> data_;

	size_t maximum_;
	size_t current_;

	bool finished_;

	Stopwatch producer;
	Stopwatch consumer;

	virtual void run()
	{
		while (true) {
			T data;
			{
				auto& lock = with(critical_section_);
				while (data_.size() == 0) {
					if (finished_) return;
					if (PERF) consumer.start();
					cond_empty_.wait(critical_section_);
					if (PERF) consumer.stop();
					if (finished_) return;
				}
				data = std::move(data_.front());
				data_.pop_front();
				size_t newsize = current_ - 1;
				if ((current_ >= maximum_) && (newsize < maximum_)) {
					cond_full_.broadcast();
				}
				current_ = newsize;
			}
			OnDataReceived(std::move(data));
		}
	}
};
