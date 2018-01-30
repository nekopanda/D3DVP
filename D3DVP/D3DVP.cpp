
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#undef min
#undef max

#include <stdint.h>
#include <avisynth.h>

#include <stdio.h>

#include <DXGI.h>
#include <D3D11.h>
#include <comdef.h>

#include <algorithm>
#include <vector>
#include <memory>
#include <exception>

#include "Thread.hpp"

#define COUNT_FRAMES 0
#define PRINT_WAIT false

#define COM_CHECK(call) \
	do { \
		HRESULT hr_ = call; \
		if (FAILED(hr_)) { \
			OnComError(hr_); \
			env->ThrowError("[COM Error] %d: %s at %s:%d", hr_, \
					_com_error(hr_).ErrorMessage(), __FILE__, __LINE__); \
				} \
		} while (0)

void OnComError(HRESULT hr) {
#if 1 // デバッグ用（本番は取り除く）
	printf("[COM Error] %s (code: %d)\n", _com_error(hr).ErrorMessage(), hr);
#endif
}

struct ComDeleter {
	void operator()(IUnknown* c) {
		c->Release();
	}
};
template <typename T> using PCom = std::unique_ptr<T, ComDeleter>;
template <typename T> PCom<T> make_com_ptr(T* p) { return PCom<T>(p); }

class D3DVP : public GenericVideoFilter
{
	typedef uint8_t pixel_t;

	enum {
		NBUF_IN_FRAME = 2,
		NBUF_IN_TEX = 4,
		NBUF_OUT_TEX = 4
	};

	int mode, order;
	std::string deviceName;
	int resetFrames;
	int numCache;
	int debug;

	int logUVx;
	int logUVy;

	PCom<ID3D11Device> dev;
	PCom<ID3D11DeviceContext> devCtx;
	PCom<ID3D11VideoDevice> videoDev;
	PCom<ID3D11VideoProcessor> videoProc;
	PCom<ID3D11VideoProcessorEnumerator> videoProcEnum;

	D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rccaps;

	// planar formatのtexture arrayはサポートされていないことに注意
	std::vector<PCom<ID3D11Texture2D>> texInputCPU;
	std::vector<PCom<ID3D11Texture2D>> texOutputCPU;

	std::vector<PCom<ID3D11Texture2D>> texInput;
	std::vector<PCom<ID3D11VideoProcessorInputView>> inputViews;

	// 一応並列処理できるように出力も複数用意しておく
	std::vector<PCom<ID3D11Texture2D>> texOutput;
	std::vector<PCom<ID3D11VideoProcessorOutputView>> outputViews;

	PCom<ID3D11VideoContext> videoCtx;

	// devCtx(+videoCtx?)を呼び出すときにロックを取得する
	CriticalSection deviceLock;

	CriticalSection inputTexPoolLock;
	std::vector<ID3D11Texture2D*> inputTexPool;
	CriticalSection outputTexPoolLock;
	std::vector<ID3D11Texture2D*> outputTexPool;

	struct FrameHeader {
		IScriptEnvironment2* env;
		std::exception_ptr exception;
		bool reset;
		int n;
	};

	template <typename T> struct FrameData : public FrameHeader {
		FrameData() : FrameHeader(), data() { }
		FrameData(const FrameHeader& o) : FrameHeader(o), data() { }
		T data;
	};

	class ToNV12Thread : public DataPumpThread<FrameData<PVideoFrame>, PRINT_WAIT> {
	public:
		ToNV12Thread(D3DVP* this_, IScriptEnvironment2* env)
			: DataPumpThread(NBUF_IN_FRAME, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<PVideoFrame>&& data) {
			this_->toNV12Received(std::move(data));
		}
	private:
		D3DVP* this_;
	};

	class ProcessThread : public DataPumpThread<FrameData<ID3D11Texture2D*>, PRINT_WAIT> {
	public:
		ProcessThread(D3DVP* this_, IScriptEnvironment2* env)
			: DataPumpThread(NBUF_IN_TEX - 2, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<ID3D11Texture2D*>&& data) {
			this_->processReceived(std::move(data));
		}
	private:
		D3DVP* this_;
	};

	class FromNV12Thread : public DataPumpThread<FrameData<ID3D11Texture2D*>, PRINT_WAIT> {
	public:
		FromNV12Thread(D3DVP* this_, IScriptEnvironment2* env)
			: DataPumpThread(NBUF_OUT_TEX - 2, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<ID3D11Texture2D*>&& data) {
			this_->fromNV12Received(std::move(data));
		}
	private:
		D3DVP* this_;
	};

	ToNV12Thread toNV12Thread;
	ProcessThread processThread;
	FromNV12Thread fromNV12Thread;

	PVideoFrame GetChildFrame(int n, IScriptEnvironment2* env) {
		n = std::max(0, std::min(vi.num_frames - 1, n));
		return child->GetFrame(n, env);
	}

	int NumFramesPerBlock() {
		return (mode >= 1) ? 2 : 1;
	}

	// 先読み枚数
	int NumFramesProcAhead() {
		return NBUF_IN_FRAME + NBUF_IN_TEX + (NBUF_OUT_TEX / NumFramesPerBlock()) + rccaps.FutureFrames;
	}

	template <typename pixel_t>
	void toNV12(PVideoFrame& src, D3D11_MAPPED_SUBRESOURCE dst)
	{
		const pixel_t* srcY = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_Y));
		const pixel_t* srcU = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_U));
		const pixel_t* srcV = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_V));

		int pitchY = src->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = src->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int widthUV = vi.width >> logUVx;
		int heightUV = vi.height >> logUVy;
		int offsetUV = vi.width * vi.height;
		int dstPitch = dst.RowPitch / sizeof(pixel_t);

		pixel_t* dstY = reinterpret_cast<pixel_t*>(dst.pData);
		pixel_t* dstUV = dstY + vi.height * dstPitch;

		for (int y = 0; y < vi.height; ++y) {
			for (int x = 0; x < vi.width; ++x) {
				dstY[x + y * dstPitch] = srcY[x + y * pitchY];
			}
		}

		for (int y = 0; y < heightUV; ++y) {
			for (int x = 0; x < widthUV; ++x) {
				dstUV[x * 2 + 0 + y * dstPitch] = srcU[x + y * pitchUV];
				dstUV[x * 2 + 1 + y * dstPitch] = srcV[x + y * pitchUV];
			}
		}
	}

	template <typename pixel_t>
	void fromNV12(PVideoFrame& dst, D3D11_MAPPED_SUBRESOURCE src)
	{
		pixel_t* dstY = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_Y));
		pixel_t* dstU = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_U));
		pixel_t* dstV = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_V));

		int pitchY = dst->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = dst->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int widthUV = vi.width >> logUVx;
		int heightUV = vi.height >> logUVy;
		int offsetUV = vi.width * vi.height;
		int srcPitch = src.RowPitch / sizeof(pixel_t);

		pixel_t* srcY = reinterpret_cast<pixel_t*>(src.pData);
		pixel_t* srcUV = srcY + vi.height * srcPitch;

		for (int y = 0; y < vi.height; ++y) {
			for (int x = 0; x < vi.width; ++x) {
				dstY[x + y * pitchY] = srcY[x + y * srcPitch];
			}
		}

		for (int y = 0; y < heightUV; ++y) {
			for (int x = 0; x < widthUV; ++x) {
				dstU[x + y * pitchUV] = srcUV[x * 2 + 0 + y * srcPitch];
				dstV[x + y * pitchUV] = srcUV[x * 2 + 1 + y * srcPitch];
			}
		}
	}

#if COUNT_FRAMES
	int cntTo, cntReset, cntRecv, cntProc, cntFrom;
#endif

	void toNV12Received(FrameData<PVideoFrame>&& data) {
		auto env = data.env;
		FrameData<ID3D11Texture2D*> out = static_cast<FrameHeader>(data);
		out.data = nullptr;

		if (data.exception == nullptr) {
			try {
				{
					auto& lock = with(inputTexPoolLock);
					out.data = inputTexPool.back();
					inputTexPool.pop_back();
				}

				D3D11_MAPPED_SUBRESOURCE res;
				{
					auto& lock = with(deviceLock);
					COM_CHECK(devCtx->Map(out.data, 0, D3D11_MAP_WRITE, 0, &res));
				}
				toNV12<pixel_t>(data.data, res);
#if COUNT_FRAMES
				++cntTo;
#endif
				{
					auto& lock = with(deviceLock);
					devCtx->Unmap(out.data, 0);
				}
			}
			catch (...) {
				out.exception = std::current_exception();
			}
		}

		processThread.put(std::move(out));
	}

	// processReceived用データ
	std::deque<int> inputTexQueue;
	int processStartFrame;
	int nextInputTex;
	int nextOutputTex;
	bool resetOutput;
	std::vector<ID3D11VideoProcessorInputView*> pInputViews; // 引数用ポインタ配列

	void processReceived(FrameData<ID3D11Texture2D*>&& data) {
		auto env = data.env;
		FrameData<ID3D11Texture2D*> out = static_cast<FrameHeader>(data);

#if COUNT_FRAMES
		++cntRecv;
#endif
		if (data.exception == nullptr) {
			try {
				if (data.reset) {
					inputTexQueue.clear();
					processStartFrame = data.n + rccaps.PastFrames;
					nextInputTex = 0;
					nextOutputTex = 0;
					resetOutput = true;
#if COUNT_FRAMES
					++cntReset;
#endif
				}

				// 新しいフレームを追加してGPUにコピー
				inputTexQueue.push_back(nextInputTex);
				if (++nextInputTex >= (int)texInput.size()) {
					nextInputTex = 0;
				}
				{
					auto& lock = with(deviceLock);
					devCtx->CopySubresourceRegion(
						texInput[inputTexQueue.back()].get(), 0, 0, 0, 0, data.data, 0, NULL);
				}

				int numInputTex = rccaps.FutureFrames + rccaps.PastFrames + 1;
				if (inputTexQueue.size() == texInput.size()) {
					// 必要フレームが集まった

					// pInputViews作成
					pInputViews.resize(texInput.size());
					for (int i = 0; i < (int)texInput.size(); ++i) {
						pInputViews[i] = inputViews[inputTexQueue[i]].get();
					}

					// stream作成
					D3D11_VIDEO_PROCESSOR_STREAM stream = { 0 };
					stream.Enable = TRUE;
					stream.pInputSurface = inputViews[0].get();
					stream.PastFrames = rccaps.PastFrames;
					stream.FutureFrames = rccaps.FutureFrames;

					int findex = 0;
					stream.ppPastSurfaces = pInputViews.data() + findex;
					findex += rccaps.PastFrames;
					stream.pInputSurface = pInputViews[findex++];
					stream.ppFutureSurfaces = pInputViews.data() + findex;
					findex += rccaps.FutureFrames;

					int numFields = NumFramesPerBlock();
					for (int parity = 0; parity < numFields; ++parity) {
						stream.OutputIndex = parity;
						stream.InputFrameOrField = (data.n - processStartFrame) * 2 + parity;

						{
							auto& lock = with(deviceLock);
							if (debug) {
								// 性能評価用
								devCtx->CopyResource(
									texOutput[nextOutputTex].get(),
									texInput[inputTexQueue[rccaps.PastFrames]].get());
							}
							else {
								// 処理実行
								COM_CHECK(videoCtx->VideoProcessorBlt(
									videoProc.get(), outputViews[nextOutputTex].get(), parity, 1, &stream));
							}
#if COUNT_FRAMES
							++cntProc;
#endif
						}
						{
							auto& lock = with(outputTexPoolLock);
							out.data = outputTexPool.back();
							outputTexPool.pop_back();
						}

						{
							auto& lock = with(deviceLock);
							// CPUにコピー
							devCtx->CopyResource(out.data, texOutput[nextOutputTex].get());
						}

						// 下流に渡す
						out.n = (data.n - rccaps.FutureFrames) * numFields + parity;
						out.reset = resetOutput;
						fromNV12Thread.put(std::move(out));

						resetOutput = false;
						if (++nextOutputTex >= (int)texOutput.size()) {
							nextOutputTex = 0;
						}
					}

					inputTexQueue.pop_front();
				}
			}
			catch (...) {
				out.exception = std::current_exception();
			}
		}

		// 入力フレームを解放
		if(data.data != nullptr) {
			auto& lock = with(inputTexPoolLock);
			inputTexPool.push_back(data.data);
		}

		// 例外が発生していたら下に流す
		if (out.exception != nullptr) {
			fromNV12Thread.put(std::move(out));
		}
	}

	CriticalSection receiveLock;
	CondWait receiveCond;
	int waitingFrame;
	std::deque<FrameData<PVideoFrame>> receiveQ;

	void fromNV12Received(FrameData<ID3D11Texture2D*>&& data) {
		auto env = data.env;
		FrameData<PVideoFrame> out = static_cast<FrameHeader>(data);

		if (data.exception == nullptr) {
			try {
				out.data = env->NewVideoFrame(vi);
				D3D11_MAPPED_SUBRESOURCE res;
				{
					auto& lock = with(deviceLock);
					COM_CHECK(devCtx->Map(data.data, 0, D3D11_MAP_READ, 0, &res));
				}
				fromNV12<pixel_t>(out.data, res);
#if COUNT_FRAMES
				++cntFrom;
#endif
				{
					auto& lock = with(deviceLock);
					devCtx->Unmap(data.data, 0);
				}
			}
			catch (...) {
				out.exception = std::current_exception();
			}
		}

		// 入力フレームを解放
		if (data.data != nullptr) {
			auto& lock = with(outputTexPoolLock);
			outputTexPool.push_back(data.data);
		}

		auto& lock = with(receiveLock);
		if (data.reset) {
			receiveQ.clear();
		}
		receiveQ.push_back(out);
		if (out.n == waitingFrame) {
			receiveCond.signal();
		}
	}

	int cacheStartFrame; // 出力フレーム番号
	int nextInputFrame;  // 入力フレーム番号

	void PutInputFrame(int n, IScriptEnvironment2* env) {
		int numFields = NumFramesPerBlock();
		int procAhead = NumFramesProcAhead();
		bool reset = false;
		int inputStart = nextInputFrame;
		int nsrc = n / numFields;
		if (cacheStartFrame == -1 || n < cacheStartFrame || nsrc > nextInputFrame + (procAhead + resetFrames)) {
			// リセット
			reset = true;
			nextInputFrame = nsrc - resetFrames;
			cacheStartFrame = nextInputFrame * numFields;
			inputStart = nextInputFrame - rccaps.PastFrames;
		}
		nextInputFrame = nsrc + procAhead;
		for (int i = inputStart; i < nextInputFrame; ++i, reset = false) {
			FrameData<PVideoFrame> data;
			data.env = env;
			data.reset = reset;
			data.n = i;
			data.data = GetChildFrame(i, env);
			toNV12Thread.put(std::move(data));
		}
	}

	PVideoFrame WaitFrame(int n, IScriptEnvironment2* env) {
		//Sleep(1000000);
		while (true) {
			auto& lock = with(receiveLock);
			int idx = n - receiveQ.front().n;
			if (idx < receiveQ.size()) {
				auto data = receiveQ[idx];
				if (data.n != n) {
					env->ThrowError("[D3DVP Error] frame number unmatch 1");
				}
				while (numCache < receiveQ.size()) {
					if (receiveQ.front().n != cacheStartFrame) {
						env->ThrowError("[D3DVP Error] frame number unmatch 2");
					}
					receiveQ.pop_front();
					++cacheStartFrame;
				}
				if (data.exception) {
					std::rethrow_exception(data.exception);
				}
				return data.data;
			}
			waitingFrame = n;
			receiveCond.wait(receiveLock);
		}
	}

	static std::vector<wchar_t> to_wstring(std::string str) {
		if (str.size() == 0) {
			return std::vector<wchar_t>(1);
		}
		int dstlen = MultiByteToWideChar(
			CP_ACP, 0, str.c_str(), (int)str.size(), NULL, 0);
		std::vector<wchar_t> ret(dstlen + 1);
		MultiByteToWideChar(CP_ACP, 0,
			str.c_str(), (int)str.size(), ret.data(), (int)ret.size());
		ret.back() = 0; // null terminate
		return ret;
	}

	void CreateProcessor(IScriptEnvironment2* env)
	{
		auto wname = to_wstring(deviceName);
		bool bob = (mode >= 1);
		bool parity = (order == -1) ? child->GetParity(0) : (order != 0);

		// DXGIファクトリ作成
		IDXGIFactory1 * pFactory_;
		COM_CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory_));
		auto pFactory = make_com_ptr(pFactory_);

		// アダプタ列挙
		IDXGIAdapter * pAdapter_;
		for (int i = 0; pFactory->EnumAdapters(i, &pAdapter_) != DXGI_ERROR_NOT_FOUND; ++i) {
			auto pAdapter = make_com_ptr(pAdapter_);

			DXGI_ADAPTER_DESC desc;
			COM_CHECK(pAdapter->GetDesc(&desc));

			// printf("%ls\n", desc.Description);
			if (deviceName.size() > 0) { // 指定がある
				if (memcmp(wname.data(), desc.Description, std::min(wname.size(), sizeof(desc.Description) / sizeof(desc.Description[0])))) {
					continue;
				}
			}

			// D3D11デバイス作成
			ID3D11Device* pDevice_;
			ID3D11DeviceContext* pContext_;
			const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1 };
#ifndef _DEBUG
			int flags = 0;
#else
			int flags = D3D11_CREATE_DEVICE_DEBUG;
#endif
			COM_CHECK(D3D11CreateDevice(pAdapter.get(),
				D3D_DRIVER_TYPE_UNKNOWN, // アダプタ指定の場合はUNKNOWN
				NULL,
				flags,
				featureLevels,
				sizeof(featureLevels) / sizeof(featureLevels[0]),
				D3D11_SDK_VERSION,
				&pDevice_,
				NULL,
				&pContext_));
			auto pDevice = make_com_ptr(pDevice_);
			auto pContext = make_com_ptr(pContext_);

			// ビデオデバイス作成
			ID3D11VideoDevice* pVideoDevice_;
			if (FAILED(pDevice->QueryInterface(&pVideoDevice_))) {
				// VideoDevice未サポート
				continue;
			}
			auto pVideoDevice = make_com_ptr(pVideoDevice_);

			// 2倍FPS化インタレ解除設定
			D3D11_VIDEO_PROCESSOR_CONTENT_DESC vdesc = {};
			vdesc.InputFrameFormat = parity
				? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
				: D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
			vdesc.InputFrameRate.Numerator = vi.fps_numerator;
			vdesc.InputFrameRate.Denominator = vi.fps_denominator;
			vdesc.InputHeight = vi.height;
			vdesc.InputWidth = vi.width;
			vdesc.OutputFrameRate.Numerator = vi.fps_numerator * (bob ? 2 : 1);
			vdesc.OutputFrameRate.Denominator = vi.fps_denominator;
			vdesc.OutputHeight = vi.height;
			vdesc.OutputWidth = vi.width;
			vdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY; // 品質重視

			// VideoProcessorEnumerator作成
			ID3D11VideoProcessorEnumerator* pEnum_;
			COM_CHECK(pVideoDevice->CreateVideoProcessorEnumerator(&vdesc, &pEnum_));
			auto pEnum = make_com_ptr(pEnum_);

			D3D11_VIDEO_PROCESSOR_CAPS caps;
			COM_CHECK(pEnum->GetVideoProcessorCaps(&caps));
			for (int rci = 0; rci < (int)caps.RateConversionCapsCount; ++rci) {
				D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rccaps;
				COM_CHECK(pEnum->GetVideoProcessorRateConversionCaps(rci, &rccaps));
				if (rccaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE) {
					ID3D11VideoProcessor* pVideoProcessor_;
					COM_CHECK(pVideoDevice->CreateVideoProcessor(pEnum.get(), rci, &pVideoProcessor_));
					auto pVideoProcessor = make_com_ptr(pVideoProcessor_);

					dev = std::move(pDevice);
					devCtx = std::move(pContext);
					videoDev = std::move(pVideoDevice);
					videoProcEnum = std::move(pEnum);
					videoProc = std::move(pVideoProcessor);
					this->rccaps = rccaps;

					// D3D11_VIDEO_PROCESSOR_CONTENT_DESCの指定は反映されていないっぽいので
					// VideoProcessorを設定
					ID3D11VideoContext* pVideoCtx_;
					COM_CHECK(devCtx->QueryInterface(&pVideoCtx_));
					videoCtx = make_com_ptr(pVideoCtx_);

					videoCtx->VideoProcessorSetStreamOutputRate(
						videoProc.get(), 0, bob
							? D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL
							: D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_HALF, FALSE, NULL);
					videoCtx->VideoProcessorSetStreamFrameFormat(
						videoProc.get(), 0, parity
							? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
							: D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST);

					return;
				}
				/*
				for (int k = 0; k < rccaps.CustomRateCount; ++k) {
					D3D11_VIDEO_PROCESSOR_CUSTOM_RATE customRate;
					COM_CHECK(pEnum->GetVideoProcessorCustomRate(rci, k, &customRate));
					printf("%d-%d rate: %d/%d %d -> %d (interladed: %d)\n", rci, k,
						customRate.CustomRate.Numerator, customRate.CustomRate.Denominator,
						customRate.InputFramesOrFields, customRate.OutputFrames, customRate.InputInterlaced);
				}
				*/
			}
		}

		env->ThrowError("No such device ...");
	}

	void CreateResources(IScriptEnvironment2* env)
	{
		// 必要なテクスチャ枚数
		int numInputTex = rccaps.FutureFrames + rccaps.PastFrames + 1;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = vi.width;
		desc.Height = vi.height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_NV12;
		// restriction: no anti-aliasing
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		// restriction: D3D11_USAGE_DEFAULT
		desc.Usage = D3D11_USAGE_DEFAULT;
		// no bind flag is OK for video processing input
		desc.BindFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		// 入力用テクスチャ
		texInput.resize(numInputTex);
		for (int i = 0; i < numInputTex; ++i) {
			ID3D11Texture2D* pTexInput_;
			COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexInput_));
			texInput[i] = make_com_ptr(pTexInput_);
		}

		// 出力用テクスチャ
		// output must be D3D11_BIND_RENDER_TARGET
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;

		texOutput.resize(NBUF_OUT_TEX);
		for (int i = 0; i < NBUF_OUT_TEX; ++i) {
			ID3D11Texture2D* pTexOutput_;
			COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexOutput_));
			texOutput[i] = make_com_ptr(pTexOutput_);
		}

		// 入力用CPUテクスチャ
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		texInputCPU.resize(NBUF_IN_TEX);
		for (int i = 0; i < NBUF_IN_TEX; ++i) {
			ID3D11Texture2D* pTexInputCPU_;
			COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexInputCPU_));
			texInputCPU[i] = make_com_ptr(pTexInputCPU_);
			inputTexPool.push_back(pTexInputCPU_);
		}

		// 出力用CPUテクスチャ
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		texOutputCPU.resize(NBUF_OUT_TEX);
		for (int i = 0; i < NBUF_OUT_TEX; ++i) {
			ID3D11Texture2D* pTexOutputCPU_;
			COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexOutputCPU_));
			texOutputCPU[i] = make_com_ptr(pTexOutputCPU_);
			outputTexPool.push_back(pTexOutputCPU_);
		}

		// InputView作成
		for (int i = 0; i < numInputTex; ++i) {
			ID3D11VideoProcessorInputView* pInputView_;
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = { 0 };
			inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			COM_CHECK(videoDev->CreateVideoProcessorInputView(
				texInput[i].get(), videoProcEnum.get(), &inputViewDesc, &pInputView_));
			inputViews.emplace_back(pInputView_);
		}

		// OutputView作成
		for (int i = 0; i < NBUF_OUT_TEX; ++i) {
			ID3D11VideoProcessorOutputView* pOutputView_;
			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = { D3D11_VPOV_DIMENSION_TEXTURE2D };
			outputViewDesc.Texture2D.MipSlice = 0;
			COM_CHECK(videoDev->CreateVideoProcessorOutputView(
				texOutput[i].get(), videoProcEnum.get(), &outputViewDesc, &pOutputView_));
			outputViews.emplace_back(pOutputView_);
		}
	}

public:
	D3DVP(PClip child, int mode, int order, const std::string& deviceName, int reset, int debug, IScriptEnvironment2* env)
		: GenericVideoFilter(child)
		, mode(mode)
		, order(order)
		, deviceName(deviceName)
		, resetFrames(reset)
		, debug(debug)
		, logUVx(vi.GetPlaneWidthSubsampling(PLANAR_U))
		, logUVy(vi.GetPlaneHeightSubsampling(PLANAR_U))
		, toNV12Thread(this, env)
		, processThread(this, env)
		, fromNV12Thread(this, env)
		, waitingFrame(-1)
		, cacheStartFrame(-1)
		, nextInputFrame(-1)
	{
		if (mode != 0 && mode != 1) env->ThrowError("[D3DVP Error] mode should be 0 or 1");
		if (order < -1 || order > 1) env->ThrowError("[D3DVP Error] invalid order parameter");
		if (reset < 0) env->ThrowError("[D3DVP Error] invalid reset parameter");

		CreateProcessor(env);
		CreateResources(env);

#if COUNT_FRAMES
		cntTo = 0;
		cntReset = 0;
		cntRecv = 0;
		cntProc = 0;
		cntFrom = 0;
#endif

		numCache = std::max(NumFramesProcAhead() * NumFramesPerBlock(), 15);

		vi.MulDivFPS(NumFramesPerBlock(), 1);
		vi.num_frames *= NumFramesPerBlock();

		toNV12Thread.start();
		processThread.start();
		fromNV12Thread.start();
	}

	~D3DVP() {
		toNV12Thread.join();
		processThread.join();
		fromNV12Thread.join();
#if COUNT_FRAMES
		printf("%d,%d,%d,%d,%d\n", cntTo, cntReset, cntRecv, cntProc, cntFrom);
#endif
#if PRINT_WAIT
		double toP, toC, proP, proC, fromP, fromC;
		toNV12Thread.getTotalWait(toP, toC);
		processThread.getTotalWait(proP, proC);
		fromNV12Thread.getTotalWait(fromP, fromC);
		printf("toNV12Thread: %f,%f\n", toP, toC);
		printf("processThread: %f,%f\n", proP, proC);
		printf("fromNV12Thread: %f,%f\n", fromP, fromC);
#endif
	}

	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);
		PutInputFrame(n, env);
		return WaitFrame(n, env);
	}

	static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env_)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);
		return new D3DVP(
			args[0].AsClip(),
			args[1].AsInt(1),     // mode
			args[2].AsInt(-1),    // order
			args[3].AsString(""), // device
			args[4].AsInt(30),    // reset
			args[5].AsInt(0),     // debug
			env);
	}
};

static void init_console()
{
	AllocConsole();
	freopen("CONOUT$", "w", stdout);
	freopen("CONIN$", "r", stdin);
}

const AVS_Linkage *AVS_linkage = 0;

extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors)
{
	AVS_linkage = vectors;
	//init_console();

	env->AddFunction("D3DVP", "c[mode]i[order]i[device]s[reset]i[debug]i", D3DVP::Create, 0);

	return "Direct3D VideoProcessing Plugin";
}
