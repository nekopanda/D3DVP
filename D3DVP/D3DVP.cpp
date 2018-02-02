
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#undef min
#undef max

#include <stdint.h>
#include <avisynth.h>

#include <stdio.h>

#include <initguid.h>
#include <DXGI.h>
#include <D3D11.h>
#include <comdef.h>

#include <intrin.h>

#include <algorithm>
#include <vector>
#include <memory>
#include <array>
#include <bitset>
#include <exception>

#include "Thread.hpp"

#define COUNT_FRAMES 0
#define PRINT_WAIT true

#if 0 // デバッグ用（本番はOFFにする）
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

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
	PRINTF("[COM Error] %s (code: %d)\n", _com_error(hr).ErrorMessage(), hr);
}

struct ComDeleter {
	void operator()(IUnknown* c) {
		c->Release();
	}
};
template <typename T> using PCom = std::unique_ptr<T, ComDeleter>;
template <typename T> PCom<T> make_com_ptr(T* p) { return PCom<T>(p); }

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

static std::string to_string(std::wstring str) {
	if (str.size() == 0) {
		return std::string();
	}
	int dstlen = WideCharToMultiByte(
		CP_ACP, 0, str.c_str(), (int)str.size(), NULL, 0, NULL, NULL);
	std::vector<char> ret(dstlen);
	WideCharToMultiByte(CP_ACP, 0,
		str.c_str(), (int)str.size(), ret.data(), (int)ret.size(), NULL, NULL);
	return std::string(ret.begin(), ret.end());
}

void yuv_to_nv12_c(
	int height, int width,
	uint8_t* dst, int dstPitch,
	const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
	int pitchY, int pitchUV)
{
	int widthUV = width >> 1;
	int heightUV = height >> 1;
	int offsetUV = width * height;

	uint8_t* dstY = dst;
	uint8_t* dstUV = dstY + height * dstPitch;

	for (int y = 0; y < height; ++y) {
		memcpy(&dstY[y * dstPitch], &srcY[y * pitchY], width);
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			dstUV[x * 2 + 0 + y * dstPitch] = srcU[x + y * pitchUV];
			dstUV[x * 2 + 1 + y * dstPitch] = srcV[x + y * pitchUV];
		}
	}
}

void yuv_to_nv12_avx2(
	int height, int width,
	uint8_t* dst, int dstPitch,
	const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
	int pitchY, int pitchUV);

void nv12_to_yuv_c(
	int height, int width,
	uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
	int pitchY, int pitchUV,
	const uint8_t* src, int srcPitch)
{
	int widthUV = width >> 1;
	int heightUV = height >> 1;
	int offsetUV = width * height;

	const uint8_t* srcY = src;
	const uint8_t* srcUV = srcY + height * srcPitch;

	for (int y = 0; y < height; ++y) {
		memcpy(&dstY[y * pitchY], &srcY[y * srcPitch], width);
	}

	for (int y = 0; y < heightUV; ++y) {
		for (int x = 0; x < widthUV; ++x) {
			dstU[x + y * pitchUV] = srcUV[x * 2 + 0 + y * srcPitch];
			dstV[x + y * pitchUV] = srcUV[x * 2 + 1 + y * srcPitch];
		}
	}
}

void nv12_to_yuv_avx2(
	int height, int width,
	uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
	int pitchY, int pitchUV,
	const uint8_t* src, int srcPitch);

class CPUID {
	bool avx2Enabled;
public:
	CPUID() : avx2Enabled(false) {
		std::array<int, 4> cpui;
		__cpuid(cpui.data(), 0);
		int nIds_ = cpui[0];
		if (7 <= nIds_) {
			__cpuidex(cpui.data(), 7, 0);
			std::bitset<32> f_7_EBX_ = cpui[1];
			avx2Enabled = f_7_EBX_[5];
		}
	}
	bool AVX2(void) { return avx2Enabled; }
};

// 処理の共通部分を実装したクラス
template <typename OUT_FRAME, typename ErrorHandler>
class D3DVP
{
protected:
	enum {
		NBUF_IN_FRAME = 2,
		NBUF_IN_TEX = 4,
		NBUF_OUT_TEX = 4,

		INVALID_FRAME = -0xFFFF
	};

	DXGI_FORMAT format;
	int mode, tff, quality;
	std::string deviceName;
	int resetFrames;
	int numCache;
	int debug;

	VideoInfo srcvi;

	int width, height;

	PCom<ID3D11Device> dev;
	PCom<ID3D11DeviceContext> devCtx;
	PCom<ID3D11VideoDevice> videoDev;
	PCom<ID3D11VideoProcessor> videoProc;
	PCom<ID3D11VideoProcessorEnumerator> videoProcEnum;

	D3D11_VIDEO_PROCESSOR_CAPS caps;
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
		ErrorHandler* env;
		std::exception_ptr exception;
		bool reset;
		int n;
	};

	template <typename T> struct FrameData : public FrameHeader {
		FrameData() : FrameHeader(), data() { }
		FrameData(const FrameHeader& o) : FrameHeader(o), data() { }
		T data;
	};

	class ProcessThread : public DataPumpThread<FrameData<ID3D11Texture2D*>, ErrorHandler, PRINT_WAIT> {
	public:
		ProcessThread(D3DVP* this_, ErrorHandler* env)
			: DataPumpThread(NBUF_IN_TEX - 2, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<ID3D11Texture2D*>&& data) {
			this_->processReceived(std::move(data));
		}
	private:
		D3DVP* this_;
	};

	ProcessThread processThread;

	virtual void InputFrame(int n, bool reset, ErrorHandler* env) = 0;
	virtual void OutputFrame(FrameData<ID3D11Texture2D*>& data) = 0;

#if COUNT_FRAMES
	int cntTo, cntReset, cntRecv, cntProc, cntFrom;
#endif

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

						PRINTF("VideoProcessorBlt %d\n", data.n);
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
							if (outputTexPool.size() > 0) {
								out.data = outputTexPool.back();
								outputTexPool.pop_back();
							}
						}
						if (out.data == nullptr) {
							// 無かったら新しく確保
							auto& lock = with(deviceLock);
							texOutputCPU.push_back(AllocOutputCPUTexture(env));
							out.data = texOutputCPU.back().get();
						}

						{
							auto& lock = with(deviceLock);
							// CPUにコピー
							devCtx->CopyResource(out.data, texOutput[nextOutputTex].get());
						}

						// 下流に渡す
						out.n = (data.n - rccaps.FutureFrames) * numFields + parity;
						out.reset = resetOutput;
						OutputFrame(out);

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
		if (data.data != nullptr) {
			auto& lock = with(inputTexPoolLock);
			inputTexPool.push_back(data.data);
		}

		// 例外が発生していたら下に流す
		if (out.exception != nullptr) {
			OutputFrame(out);
		}
	}

	int cacheStartFrame; // 出力フレーム番号
	int nextInputFrame;  // 入力フレーム番号

	void CreateProcessor(ErrorHandler* env)
	{
		auto wname = to_wstring(deviceName);

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

			PRINTF("%ls\n", desc.Description);
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

			D3D11_VIDEO_PROCESSOR_CONTENT_DESC vdesc = {};
			vdesc.InputFrameFormat = tff
				? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
				: D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
			vdesc.InputFrameRate.Numerator = srcvi.fps_numerator;
			vdesc.InputFrameRate.Denominator = srcvi.fps_denominator;
			vdesc.InputHeight = srcvi.height;
			vdesc.InputWidth = srcvi.width;
			vdesc.OutputFrameRate.Numerator = srcvi.fps_numerator * NumFramesPerBlock();
			vdesc.OutputFrameRate.Denominator = srcvi.fps_denominator;
			vdesc.OutputHeight = height;
			vdesc.OutputWidth = width;

			if (quality == 0) {
				PRINTF("[D3DVP] Quality: Speed\n");
				vdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
			}
			else if (quality == 1) {
				PRINTF("[D3DVP] Quality: Normal\n");
				vdesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
			}
			else { // quality == 2
				PRINTF("[D3DVP] Quality: Quality\n");
				vdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
			}

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
					this->caps = caps;
					this->rccaps = rccaps;

					ID3D11VideoContext* pVideoCtx_;
					COM_CHECK(devCtx->QueryInterface(&pVideoCtx_));
					videoCtx = make_com_ptr(pVideoCtx_);

					return;
				}
				/*
				for (int k = 0; k < rccaps.CustomRateCount; ++k) {
				D3D11_VIDEO_PROCESSOR_CUSTOM_RATE customRate;
				COM_CHECK(pEnum->GetVideoProcessorCustomRate(rci, k, &customRate));
				PRINTF("%d-%d rate: %d/%d %d -> %d (interladed: %d)\n", rci, k,
				customRate.CustomRate.Numerator, customRate.CustomRate.Denominator,
				customRate.InputFramesOrFields, customRate.OutputFrames, customRate.InputInterlaced);
				}
				*/
			}
		}

		env->ThrowError("No such device ...");
	}

	PCom<ID3D11Texture2D> AllocOutputCPUTexture(ErrorHandler* env) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;

		ID3D11Texture2D* pTexOutputCPU_;
		COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexOutputCPU_));
		return make_com_ptr(pTexOutputCPU_);
	}

	void CreateResources(ErrorHandler* env)
	{
		// 必要なテクスチャ枚数
		int numInputTex = rccaps.FutureFrames + rccaps.PastFrames + 1;
		PRINTF("[D3DVP] PastFrames: %d, FutureFrames: %d\n", rccaps.PastFrames, rccaps.FutureFrames);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = srcvi.width;
		desc.Height = srcvi.height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
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
		desc.Width = width;
		desc.Height = height;
		// output must be D3D11_BIND_RENDER_TARGET
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;

		texOutput.resize(NBUF_OUT_TEX);
		for (int i = 0; i < NBUF_OUT_TEX; ++i) {
			ID3D11Texture2D* pTexOutput_;
			COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexOutput_));
			texOutput[i] = make_com_ptr(pTexOutput_);
		}

		// 入力用CPUテクスチャ
		desc.Width = srcvi.width;
		desc.Height = srcvi.height;
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
		texOutputCPU.resize(NBUF_OUT_TEX);
		for (int i = 0; i < NBUF_OUT_TEX; ++i) {
			texOutputCPU[i] = AllocOutputCPUTexture(env);
			outputTexPool.push_back(texOutputCPU[i].get());
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
	D3DVP(VideoInfo srcvi, DXGI_FORMAT format, int mode, int tff, int width, int height, int quality,
		const std::string& deviceName, int reset, int debug, ErrorHandler* env)
		: format(format)
		, mode(mode)
		, tff(tff)
		, width(width)
		, height(height)
		, quality(quality)
		, deviceName(deviceName)
		, resetFrames(reset)
		, debug(debug)
		, srcvi(srcvi)
		, processThread(this, env)
		, waitingFrame(INVALID_FRAME)
		, cacheStartFrame(INVALID_FRAME)
		, nextInputFrame(INVALID_FRAME)
	{
		if (mode != 0 && mode != 1) env->ThrowError("[D3DVP Error] mode must be 0 or 1");
		if (quality < 0 || quality > 2) env->ThrowError("[D3DVP Error] quality must be between 0 and 2");
		if (reset < 0) env->ThrowError("[D3DVP Error] reset must be >= 0");

		CreateProcessor(env);
		CreateResources(env);

		numCache = std::max(NumFramesProcAhead() * NumFramesPerBlock(), 15);

#if COUNT_FRAMES
		cntTo = 0;
		cntReset = 0;
		cntRecv = 0;
		cntProc = 0;
		cntFrom = 0;
#endif

		processThread.start();
	}

	virtual ~D3DVP() {
		processThread.join();
#if COUNT_FRAMES
		PRINTF("%d,%d,%d,%d,%d\n", cntTo, cntReset, cntRecv, cntProc, cntFrom);
#endif
#if 0
		// リーク検査
		outputViews.clear();
		texOutput.clear();
		inputViews.clear();
		texInput.clear();
		texOutputCPU.clear();
		texInputCPU.clear();

		videoProcEnum = nullptr;
		videoProc = nullptr;
		videoDev = nullptr;
		devCtx = nullptr;

		ID3D11Debug* pDebug;
		HRESULT hr = dev->QueryInterface(__uuidof(ID3D11Debug), (void**)&pDebug);
		dev = nullptr;

		if (SUCCEEDED(hr)) {
			pDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
			pDebug->Release();
		}
#endif
	}

	int NumFramesPerBlock() {
		return (mode >= 1) ? 2 : 1;
	}

	// 先読み枚数
	int NumFramesProcAhead() {
		return NBUF_IN_FRAME + NBUF_IN_TEX + (NBUF_OUT_TEX / NumFramesPerBlock()) + rccaps.FutureFrames;
	}

	void SetFilter(bool autop, int nr, int edge, ErrorHandler* env)
	{
		PRINTF("SetFilter\n");
		if (nr < -1 || nr > 100) env->ThrowError("D3DVP Error] nr must be in range 0-100, or -1 to disable");
		if (edge < -1 || edge > 100) env->ThrowError("D3DVP Error] edge must be in range 0-100, or -1 to disable");

		bool bob = (mode >= 1);

		// D3D11_VIDEO_PROCESSOR_CONTENT_DESCの指定は反映されていないっぽいので
		// VideoProcessorを設定
		videoCtx->VideoProcessorSetStreamOutputRate(
			videoProc.get(), 0, bob
			? D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL
			: D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_HALF, FALSE, NULL);
		videoCtx->VideoProcessorSetStreamFrameFormat(
			videoProc.get(), 0, tff
			? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
			: D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST);

		BOOL enableNR = (nr >= 0) && (caps.FilterCaps & D3D11_VIDEO_PROCESSOR_FILTER_CAPS_NOISE_REDUCTION);
		BOOL enableEE = (edge >= 0) && (caps.FilterCaps & D3D11_VIDEO_PROCESSOR_FILTER_CAPS_EDGE_ENHANCEMENT);

		D3D11_VIDEO_PROCESSOR_FILTER_RANGE nrRange = { 0 }, edgeRange = { 0 };

		if (enableNR) {
			COM_CHECK(videoProcEnum->GetVideoProcessorFilterRange(
				D3D11_VIDEO_PROCESSOR_FILTER_NOISE_REDUCTION, &nrRange));
			PRINTF("NR: [%d,%d,%d,%f]\n", nrRange.Minimum, nrRange.Maximum, nrRange.Default, nrRange.Multiplier);
			nr = (int)std::round((double)nr * 0.01 *
				(nrRange.Maximum - nrRange.Minimum) + nrRange.Minimum);
			PRINTF("NR: %d %d\n", enableNR, nr);
			videoCtx->VideoProcessorSetStreamFilter(
				videoProc.get(), 0, D3D11_VIDEO_PROCESSOR_FILTER_NOISE_REDUCTION, enableNR, nr);
		}

		if (enableEE) {
			COM_CHECK(videoProcEnum->GetVideoProcessorFilterRange(
				D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &edgeRange));
			PRINTF("EE: [%d,%d,%d,%f]\n", edgeRange.Minimum, edgeRange.Maximum, edgeRange.Default, edgeRange.Multiplier);
			edge = (int)std::round((double)edge * 0.01 *
				(edgeRange.Maximum - edgeRange.Minimum) + edgeRange.Minimum);
			PRINTF("EE: %d %d\n", enableEE, edge);
			videoCtx->VideoProcessorSetStreamFilter(
				videoProc.get(), 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, enableEE, edge);
		}

		// auto processing mode
		videoCtx->VideoProcessorSetStreamAutoProcessingMode(videoProc.get(), 0, (BOOL)autop);
	}

	void Reset() {
		PRINTF("Reset\n");
		cacheStartFrame = INVALID_FRAME;
	}

	void PutInputFrame(int n, ErrorHandler* env) {
		int numFields = NumFramesPerBlock();
		int procAhead = NumFramesProcAhead();
		bool reset = false;
		int inputStart = nextInputFrame;
		int nsrc = n / numFields;
		if (cacheStartFrame == INVALID_FRAME || n < cacheStartFrame || nsrc > nextInputFrame + (procAhead + resetFrames)) {
			// リセット
			if (nextInputFrame != INVALID_FRAME) {
				// 入れたフレームの処理が全部終わるまで待つ
				WaitFrame((nextInputFrame - rccaps.PastFrames) * numFields - 1, env);
				auto& lock = with(receiveLock);
				receiveQ.clear();
			}
			reset = true;
			nextInputFrame = nsrc - resetFrames;
			cacheStartFrame = nextInputFrame * numFields;
			inputStart = nextInputFrame - rccaps.PastFrames;
			PRINTF("Input Reset %d\n", n);
		}
		nextInputFrame = std::max(nextInputFrame, nsrc + procAhead);
		for (int i = inputStart; i < nextInputFrame; ++i, reset = false) {
			InputFrame(i, reset, env);
		}
	}

	CriticalSection receiveLock;
	CondWait receiveCond;
	int waitingFrame;
	std::deque<FrameData<OUT_FRAME>> receiveQ;

	OUT_FRAME WaitFrame(int n, ErrorHandler* env) {
		while (true) {
			auto& lock = with(receiveLock);
			int idx = n - receiveQ.front().n;
			if (idx < (int)receiveQ.size()) {
				auto data = receiveQ[idx];
				if (data.exception) {
					std::rethrow_exception(data.exception);
				}
				if (data.n != n) {
					env->ThrowError("[D3DVP Error] frame number unmatch 1");
				}
				while (numCache < (int)receiveQ.size()) {
					if (receiveQ.front().n != cacheStartFrame) {
						env->ThrowError("[D3DVP Error] frame number unmatch 2");
					}
					receiveQ.pop_front();
					++cacheStartFrame;
				}
				return data.data;
			}
			waitingFrame = n;
			receiveCond.wait(receiveLock);
		}
	}
};

// AviSynth用ロジックを実装したクラス
class D3DVPAvsWorker : public D3DVP<PVideoFrame, IScriptEnvironment2>
{
	typedef uint8_t pixel_t;

	PClip child;
	VideoInfo vi;

	int logUVx;
	int logUVy;

	void(*yuv_to_nv12)(
		int height, int width,
		uint8_t* dst, int dstPitch,
		const uint8_t* srcY, const uint8_t* srcU, const uint8_t* srcV,
		int pitchY, int pitchUV);

	void(*nv12_to_yuv)(
		int height, int width,
		uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
		int pitchY, int pitchUV,
		const uint8_t* src, int srcPitch);

	class ToNV12Thread : public DataPumpThread<FrameData<PVideoFrame>, IScriptEnvironment2, PRINT_WAIT> {
	public:
		ToNV12Thread(D3DVPAvsWorker* this_, IScriptEnvironment2* env)
			: DataPumpThread(NBUF_IN_FRAME, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<PVideoFrame>&& data) {
			this_->toNV12Received(std::move(data));
		}
	private:
		D3DVPAvsWorker* this_;
	};

	class FromNV12Thread : public DataPumpThread<FrameData<ID3D11Texture2D*>, IScriptEnvironment2, PRINT_WAIT> {
	public:
		FromNV12Thread(D3DVPAvsWorker* this_, IScriptEnvironment2* env)
			: DataPumpThread(NBUF_OUT_TEX - 2, env)
			, this_(this_) { }
	protected:
		virtual void OnDataReceived(FrameData<ID3D11Texture2D*>&& data) {
			this_->fromNV12Received(std::move(data));
		}
	private:
		D3DVPAvsWorker* this_;
	};

	ToNV12Thread toNV12Thread;
	FromNV12Thread fromNV12Thread;

	PVideoFrame GetChildFrame(int n, IScriptEnvironment2* env) {
		n = std::max(0, std::min(srcvi.num_frames - 1, n));
		return child->GetFrame(n, env);
	}

	void toNV12(PVideoFrame& src, D3D11_MAPPED_SUBRESOURCE dst)
	{
		const pixel_t* srcY = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_Y));
		const pixel_t* srcU = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_U));
		const pixel_t* srcV = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_V));
		int pitchY = src->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = src->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int dstPitch = dst.RowPitch / sizeof(pixel_t);
		pixel_t* dstY = reinterpret_cast<pixel_t*>(dst.pData);
		yuv_to_nv12(srcvi.height, srcvi.width, dstY, dstPitch, srcY, srcU, srcV, pitchY, pitchUV);
	}

	void fromNV12(PVideoFrame& dst, D3D11_MAPPED_SUBRESOURCE src)
	{
		pixel_t* dstY = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_Y));
		pixel_t* dstU = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_U));
		pixel_t* dstV = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_V));
		int pitchY = dst->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = dst->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int srcPitch = src.RowPitch / sizeof(pixel_t);
		const pixel_t* srcY = reinterpret_cast<const pixel_t*>(src.pData);
		nv12_to_yuv(height, width, dstY, dstU, dstV, pitchY, pitchUV, srcY, srcPitch);
	}

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
				toNV12(data.data, res);
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
				fromNV12(out.data, res);
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

	virtual void InputFrame(int n, bool reset, IScriptEnvironment2* env) {
		FrameData<PVideoFrame> data;
		data.env = env;
		data.reset = reset;
		data.n = n;
		data.data = GetChildFrame(n, env);
		toNV12Thread.put(std::move(data));
	}

	virtual void OutputFrame(FrameData<ID3D11Texture2D*>& data) {
		fromNV12Thread.put(std::move(data));
	}

public:
	D3DVPAvsWorker(PClip child, DXGI_FORMAT format, int mode, int tff, int width, int height, int quality,
		const std::string& deviceName, int reset, int debug,
		IScriptEnvironment2* env)
		: D3DVP(child->GetVideoInfo(), format, mode, tff, width, height, quality, deviceName, reset, debug, env) 
		, child(child)
		, vi(child->GetVideoInfo())
		, toNV12Thread(this, env)
		, fromNV12Thread(this, env)
	{
		logUVx = vi.GetPlaneWidthSubsampling(PLANAR_U);
		logUVy = vi.GetPlaneHeightSubsampling(PLANAR_U);

#if COUNT_FRAMES
		cntTo = 0;
		cntReset = 0;
		cntRecv = 0;
		cntProc = 0;
		cntFrom = 0;
#endif

		if (CPUID().AVX2()) {
			yuv_to_nv12 = yuv_to_nv12_avx2;
			nv12_to_yuv = nv12_to_yuv_avx2;
		}
		else {
			yuv_to_nv12 = yuv_to_nv12_c;
			nv12_to_yuv = nv12_to_yuv_c;
		}

		toNV12Thread.start();
		fromNV12Thread.start();
	}

	~D3DVPAvsWorker() {
		toNV12Thread.join();
		fromNV12Thread.join();
#if PRINT_WAIT
		double toP, toC, proP, proC, fromP, fromC;
		toNV12Thread.getTotalWait(toP, toC);
		processThread.getTotalWait(proP, proC);
		fromNV12Thread.getTotalWait(fromP, fromC);
		PRINTF("toNV12Thread: %f,%f\n", toP, toC);
		PRINTF("processThread: %f,%f\n", proP, proC);
		PRINTF("fromNV12Thread: %f,%f\n", fromP, fromC);
#endif
	}

	PVideoFrame GetFrame(int n, IScriptEnvironment2* env) {
		return WaitFrame(n, env);
	}
};

#ifndef _NDEBUG
#include <DXGIDebug.h>
#endif

// AviSynth用トップレベルプラグインクラス
class D3DVPAvs : public GenericVideoFilter
{
	int mode, tff, quality;
	bool autop;
	int nr, edge;
	const std::string deviceName;
	int reset, debug;

	std::unique_ptr<D3DVPAvsWorker> w;

	PCom<IDXGIDebug> dxgiDebug;

	void GetDebugInterface(IScriptEnvironment2* env) {
		if (!dxgiDebug)
		{
			// 作成
			typedef HRESULT(__stdcall *fPtr)(const IID&, void**);
			HMODULE hDll = GetModuleHandleW(L"dxgidebug.dll");
			fPtr DXGIGetDebugInterface = (fPtr)GetProcAddress(hDll, "DXGIGetDebugInterface");

			IDXGIDebug* pDebug;
			DXGIGetDebugInterface(__uuidof(IDXGIDebug), (void**)&pDebug);
			dxgiDebug = make_com_ptr(pDebug);
		}
	}

	void CheckResourceLeak(IScriptEnvironment2* env) {
		GetDebugInterface(env);
		dxgiDebug->ReportLiveObjects(DXGI_DEBUG_D3D11, DXGI_DEBUG_RLO_DETAIL);
	}

	void ResetInstance(IScriptEnvironment2* env) {
		w = nullptr;
		//CheckResourceLeak(env);
		w = std::unique_ptr<D3DVPAvsWorker>(new D3DVPAvsWorker(child,
			DXGI_FORMAT_NV12, mode,
			tff, vi.width, vi.height, quality, deviceName, reset, debug, env));
		w->SetFilter(autop, nr, edge, env);
	}

	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_, int retry)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);
		w->PutInputFrame(n, env);
		try {
			return w->WaitFrame(n, env);
		}
		catch (const AvisynthError&) {
			if (retry >= 2) {
				throw;
			}
			// リトライしてみる
			ResetInstance(env);
			return GetFrame(n, env_, retry + 1);
		}
	}

public:
	D3DVPAvs(PClip child, int mode, int order, int width, int height, int quality,
		bool autop, int nr, int edge, const std::string& deviceName, int reset, int debug,
		IScriptEnvironment2* env)
		: GenericVideoFilter(child)
		, mode(mode)
		, quality(quality)
		, autop(autop)
		, nr(nr)
		, edge(edge)
		, deviceName(deviceName)
		, reset(reset)
		, debug(debug)
	{
		if (mode != 0 && mode != 1) env->ThrowError("[D3DVP Error] mode must be 0 or 1");
		if (order < -1 || order > 1) env->ThrowError("[D3DVP Error] order must be between -1 and 1");
		if (quality < 0 || quality > 2) env->ThrowError("[D3DVP Error] quality must be between 0 and 2");
		if (reset < 0) env->ThrowError("[D3DVP Error] reset must be >= 0");
		if (nr < -1 || nr > 100) env->ThrowError("D3DVP Error] nr must be in range 0-100, or -1 to disable");
		if (edge < -1 || edge > 100) env->ThrowError("D3DVP Error] edge must be in range 0-100, or -1 to disable");

		tff = (order == -1) ? child->GetParity(0) : (order != 0);
		
		vi.width = (width > 0) ? width : vi.width;
		vi.height = (height > 0) ? height : vi.height;

		ResetInstance(env);

		vi.MulDivFPS(w->NumFramesPerBlock(), 1);
		vi.num_frames *= w->NumFramesPerBlock();
	}

	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_)
	{
		return GetFrame(n, env_, 0);
	}

	int __stdcall SetCacheHints(int cachehints, int frame_range)
	{
		if (cachehints == CACHE_GET_MTMODE) return MT_SERIALIZED;
		return 0;
	}

	static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env_)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);
		return new D3DVPAvs(
			args[0].AsClip(),
			args[1].AsInt(1),     // mode
			args[2].AsInt(-1),    // order
			args[3].AsInt(0),     // width
			args[4].AsInt(0),     // height
			args[5].AsInt(2),     // quality
			args[6].AsBool(false),// autop
			args[7].AsInt(-1),    // nr
			args[8].AsInt(-1),    // edge
			args[9].AsString(""), // device
			args[10].AsInt(30),   // reset
			args[11].AsInt(0),    // debug
			env);
	}
};

const AVS_Linkage *AVS_linkage = 0;

extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors)
{
	AVS_linkage = vectors;

	env->AddFunction("D3DVP", "c[mode]i[order]i[width]i[height]i[quality]i[autop]b[nr]i[edge]i[device]s[reset]i[debug]i", D3DVPAvs::Create, 0);

	return "Direct3D VideoProcessing Plugin";
}

#pragma region AviUtl用

#include "filter.h"

//---------------------------------------------------------------------
//		フィルタ構造体定義
//---------------------------------------------------------------------
#define	TRACK_N	5													//	トラックバーの数
TCHAR	*track_name[] = { "品質", "幅", "高さ", "NR", "EDGE" };	//	トラックバーの名前
int		track_default[] = { 2, 32, 32, 0, 0 };	//	トラックバーの初期値
int		track_s[] = { 0, 32, 32, 0, 0 };	//	トラックバーの下限値
int		track_e[] = { 2, 2200, 1200, 100, 100 };	//	トラックバーの上限値

#define	CHECK_N	8													//	チェックボックスの数
TCHAR	*check_name[] = { "2倍fps化（2倍fpsで入力してね）", "BFF", "リサイズ", "自動補正", "ノイズ除去", "エッジ強調", "YUV420で処理", "処理しない（デバッグ用）" };				//	チェックボックスの名前
int		check_default[] = { 0, 0, 0, 0, 0, 0, 0, 0 };				//	チェックボックスの初期値 (値は0か1)

FILTER_DLL filter = {
	FILTER_FLAG_EX_INFORMATION | FILTER_FLAG_WINDOW_SIZE,	//	フィルタのフラグ
															//	FILTER_FLAG_ALWAYS_ACTIVE		: フィルタを常にアクティブにします
															//	FILTER_FLAG_CONFIG_POPUP		: 設定をポップアップメニューにします
															//	FILTER_FLAG_CONFIG_CHECK		: 設定をチェックボックスメニューにします
															//	FILTER_FLAG_CONFIG_RADIO		: 設定をラジオボタンメニューにします
															//	FILTER_FLAG_EX_DATA				: 拡張データを保存出来るようにします。
															//	FILTER_FLAG_PRIORITY_HIGHEST	: フィルタのプライオリティを常に最上位にします
															//	FILTER_FLAG_PRIORITY_LOWEST		: フィルタのプライオリティを常に最下位にします
															//	FILTER_FLAG_WINDOW_THICKFRAME	: サイズ変更可能なウィンドウを作ります
															//	FILTER_FLAG_WINDOW_SIZE			: 設定ウィンドウのサイズを指定出来るようにします
															//	FILTER_FLAG_DISP_FILTER			: 表示フィルタにします
															//	FILTER_FLAG_EX_INFORMATION		: フィルタの拡張情報を設定できるようにします
															//	FILTER_FLAG_NO_CONFIG			: 設定ウィンドウを表示しないようにします
															//	FILTER_FLAG_AUDIO_FILTER		: オーディオフィルタにします
															//	FILTER_FLAG_RADIO_BUTTON		: チェックボックスをラジオボタンにします
															//	FILTER_FLAG_WINDOW_HSCROLL		: 水平スクロールバーを持つウィンドウを作ります
															//	FILTER_FLAG_WINDOW_VSCROLL		: 垂直スクロールバーを持つウィンドウを作ります
															//	FILTER_FLAG_IMPORT				: インポートメニューを作ります
															//	FILTER_FLAG_EXPORT				: エクスポートメニューを作ります
															320,380,						//	設定ウインドウのサイズ (FILTER_FLAG_WINDOW_SIZEが立っている時に有効)
															"Direct3D 11インタレ解除",			//	フィルタの名前
															TRACK_N,					//	トラックバーの数 (0なら名前初期値等もNULLでよい)
															track_name,					//	トラックバーの名前郡へのポインタ
															track_default,				//	トラックバーの初期値郡へのポインタ
															track_s,track_e,			//	トラックバーの数値の下限上限 (NULLなら全て0～256)
															CHECK_N,					//	チェックボックスの数 (0なら名前初期値等もNULLでよい)
															check_name,					//	チェックボックスの名前郡へのポインタ
															check_default,				//	チェックボックスの初期値郡へのポインタ
															func_proc,					//	フィルタ処理関数へのポインタ (NULLなら呼ばれません)
															func_init,						//	開始時に呼ばれる関数へのポインタ (NULLなら呼ばれません)
															func_exit,						//	終了時に呼ばれる関数へのポインタ (NULLなら呼ばれません)
															func_update,						//	設定が変更されたときに呼ばれる関数へのポインタ (NULLなら呼ばれません)
															func_WndProc,						//	設定ウィンドウにウィンドウメッセージが来た時に呼ばれる関数へのポインタ (NULLなら呼ばれません)
															NULL,NULL,					//	システムで使いますので使用しないでください
															NULL,						//  拡張データ領域へのポインタ (FILTER_FLAG_EX_DATAが立っている時に有効)
															NULL,						//  拡張データサイズ (FILTER_FLAG_EX_DATAが立っている時に有効)
															"Direct3D 11 インタレース解除フィルタ",
															//  フィルタ情報へのポインタ (FILTER_FLAG_EX_INFORMATIONが立っている時に有効)
															func_save_start,						//	セーブが開始される直前に呼ばれる関数へのポインタ (NULLなら呼ばれません)
															func_save_end,						//	セーブが終了した直前に呼ばれる関数へのポインタ (NULLなら呼ばれません)
};

static void init_console()
{
	AllocConsole();
	FILE* fp;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONIN$", "r", stdin);
}

//---------------------------------------------------------------------
//		フィルタ構造体のポインタを渡す関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) FILTER_DLL * __stdcall GetFilterTable(void)
{
	//MessageBox(NULL, "!!!", "D3DVP", MB_OK);
	init_console();
	return &filter;
}
//	下記のようにすると1つのaufファイルで複数のフィルタ構造体を渡すことが出来ます
/*
FILTER_DLL *filter_list[] = {&filter,&filter2,NULL};
EXTERN_C FILTER_DLL __declspec(dllexport) ** __stdcall GetFilterTableList( void )
{
return (FILTER_DLL **)&filter_list;
}
*/

template<typename T>
T clamp(T n, T min, T max)
{
	n = n > max ? max : n;
	return n < min ? min : n;
}

struct YC48toX {
	uint8_t* dst;
	int pitch;
	const PIXEL_YC* src;
	int w, h, max_w;
};

struct YC48toYUY2 : public YC48toX {
	YC48toYUY2(const YC48toX& d) : YC48toX(d) { }
	void operator()(int y_start, int y_end) const {
		uint8_t* dstptr = dst;

		for (int y = y_start; y < y_end; ++y) {
			for (int x = 0; x < w; x += 2) {
				short y0 = src[x + y * max_w].y;
				short y1 = src[x + 1 + y * max_w].y;
				short cb = src[x + y * max_w].cb;
				short cr = src[x + y * max_w].cr;

				uint8_t Y0 = clamp(((y0 * 219 + 383) >> 12) + 16, 0, 255);
				uint8_t Y1 = clamp(((y1 * 219 + 383) >> 12) + 16, 0, 255);
				uint8_t U = clamp((((cb + 2048) * 7 + 66) >> 7) + 16, 0, 255);
				uint8_t V = clamp((((cr + 2048) * 7 + 66) >> 7) + 16, 0, 255);

				dstptr[x * 2 + 0 + y * pitch] = Y0;
				dstptr[x * 2 + 1 + y * pitch] = U;
				dstptr[x * 2 + 2 + y * pitch] = Y1;
				dstptr[x * 2 + 3 + y * pitch] = V;
			}
		}
	}
};

struct YC48toNV12 : public YC48toX {
	YC48toNV12(const YC48toX& d) : YC48toX(d) { }
	void operator()(int y_start, int y_end) const {
		uint8_t* dstY = dst;
		uint8_t* dstUV = dstY + h * pitch;

		for (int y = y_start; y < y_end; ++y) {
			for (int x = 0; x < w; x += 2) {
				short y0 = src[x + y * max_w].y;
				short y1 = src[x + 1 + y * max_w].y;
				short cb = src[x + y * max_w].cb;
				short cr = src[x + y * max_w].cr;

				uint8_t Y0 = clamp(((y0 * 219 + 383) >> 12) + 16, 0, 255);
				uint8_t Y1 = clamp(((y1 * 219 + 383) >> 12) + 16, 0, 255);
				uint8_t U = clamp((((cb + 2048) * 7 + 66) >> 7) + 16, 0, 255);
				uint8_t V = clamp((((cr + 2048) * 7 + 66) >> 7) + 16, 0, 255);

				dstY[x + 0 + y * pitch] = Y0;
				dstY[x + 1 + y * pitch] = Y1;
				dstUV[x + 0 + (y >> 1) * pitch] = U;
				dstUV[x + 1 + (y >> 1) * pitch] = V;
			}
		}
	}
};

struct XtoYC48 {
	PIXEL_YC* dst;
	const uint8_t* src;
	int pitch;
	int w, h, max_w;
};

struct YUY2toYC48 : public XtoYC48 {
	YUY2toYC48(const XtoYC48& d) : XtoYC48(d) { }
	void operator()(int y_start, int y_end) const {
		const uint8_t* srcptr = src;

		for (int y = y_start; y < y_end; ++y) {

			// まずはUVは左にそのまま入れる
			for (int x = 0, x2 = 0; x < w; x += 2, ++x2) {
				uint8_t Y0 = srcptr[x * 2 + 0 + y * pitch];
				uint8_t U = srcptr[x * 2 + 1 + y * pitch];
				uint8_t Y1 = srcptr[x * 2 + 2 + y * pitch];
				uint8_t V = srcptr[x * 2 + 3 + y * pitch];

				short y0 = ((Y0 * 1197) >> 6) - 299;
				short y1 = ((Y1 * 1197) >> 6) - 299;
				short cb = ((U - 128) * 4681 + 164) >> 8;
				short cr = ((V - 128) * 4681 + 164) >> 8;

				dst[x + y * max_w].y = y0;
				dst[x + 1 + y * max_w].y = y1;
				dst[x + y * max_w].cb = cb;
				dst[x + y * max_w].cr = cr;
			}

			// UVの入っていないところを補間
			short cb0 = dst[y * max_w].cb;
			short cr0 = dst[y * max_w].cr;
			for (int x = 0; x < w - 2; x += 2) {
				short cb2 = dst[x + 2 + y * max_w].cb;
				short cr2 = dst[x + 2 + y * max_w].cr;
				dst[x + 1 + y * max_w].cb = (cb0 + cb2) >> 1;
				dst[x + 1 + y * max_w].cr = (cr0 + cr2) >> 1;
				cb0 = cb2;
				cr0 = cr2;
			}
			dst[w - 1 + y * max_w].cb = cb0;
			dst[w - 1 + y * max_w].cr = cr0;
		}
	}
};

struct NV12toYC48 : public XtoYC48 {
	NV12toYC48(const XtoYC48& d) : XtoYC48(d) { }
	void operator()(int y_start, int y_end) const {
		const uint8_t* srcY = src;
		const uint8_t* srcUV = srcY + h * pitch;

		for (int y = y_start; y < y_end; ++y) {

			// まずはUVは左にそのまま入れる
			for (int x = 0, x2 = 0; x < w; x += 2, ++x2) {
				uint8_t Y0 = srcY[x + 0 + y * pitch];
				uint8_t Y1 = srcY[x + 1 + y * pitch];
				uint8_t U = srcUV[x + 0 + (y >> 1) * pitch];
				uint8_t V = srcUV[x + 1 + (y >> 1) * pitch];

				short y0 = ((Y0 * 1197) >> 6) - 299;
				short y1 = ((Y1 * 1197) >> 6) - 299;
				short cb = ((U - 128) * 4681 + 164) >> 8;
				short cr = ((V - 128) * 4681 + 164) >> 8;

				dst[x + y * max_w].y = y0;
				dst[x + 1 + y * max_w].y = y1;
				dst[x + y * max_w].cb = cb;
				dst[x + y * max_w].cr = cr;
			}

			// UVの入っていないところを補間
			short cb0 = dst[y * max_w].cb;
			short cr0 = dst[y * max_w].cr;
			for (int x = 0; x < w - 2; x += 2) {
				short cb2 = dst[x + 2 + y * max_w].cb;
				short cr2 = dst[x + 2 + y * max_w].cr;
				dst[x + 1 + y * max_w].cb = (cb0 + cb2) >> 1;
				dst[x + 1 + y * max_w].cr = (cr0 + cr2) >> 1;
				cb0 = cb2;
				cr0 = cr2;
			}
			dst[w - 1 + y * max_w].cb = cb0;
			dst[w - 1 + y * max_w].cr = cr0;
		}
	}
};

template <typename T>
void multi_thread_convert_func(int thread_id, int thread_num, void *param1, void *param2)
{
	//    thread_id    : スレッド番号 ( 0 ～ thread_num-1 )
	//    thread_num    : スレッド数 ( 1 ～ )
	//    param1        : 汎用パラメータ
	//    param2        : 汎用パラメータ
	//
	//    この関数内からWin32APIや外部関数(rgb2yc,yc2rgbは除く)を使用しないでください。
	//
	const T& cvt = *static_cast<T*>(param1);

	int sz = (cvt.h + thread_num - 1) / thread_num;
	int y_start = std::min(cvt.h, sz * thread_id);
	int y_end = std::min(cvt.h, y_start + sz);

	cvt(y_start, y_end);
}

template <typename T>
void convert_format(uint8_t* dst, int pitch, const PIXEL_YC* src, int w, int h, int max_w, FILTER *fp)
{
	YC48toX d = { dst, pitch, src, w, h, max_w };
	T cvt = d;
	fp->exfunc->exec_multi_thread_func(
		multi_thread_convert_func<T>, &cvt, NULL);
}

template <typename T>
void convert_format(PIXEL_YC* dst, const uint8_t* src, int pitch, int w, int h, int max_w, FILTER *fp)
{
	XtoYC48 d = { dst, src, pitch, w, h, max_w };
	T cvt = d;
	fp->exfunc->exec_multi_thread_func(
		multi_thread_convert_func<T>, &cvt, NULL);
}

struct AviUtlErrorHandler {
	void ThrowError(const char* str) {
		throw std::string(str);
	}
	void ThrowError(const char* str, int a, const char* b, const char* c, int d) {
		char buf[1024];
		sprintf_s(buf, str, a, b, c, d);
		throw std::string(buf);
	}
};

class SectionTime {
	int64_t prev;
public:
	SectionTime() {
		QueryPerformanceCounter((LARGE_INTEGER*)&prev);
	}
	~SectionTime() {
		int64_t now, freq;
		QueryPerformanceCounter((LARGE_INTEGER*)&now);
		QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		double avg = (double)(now - prev) / freq * 1000.0;
		PRINTF("%f ms\n", avg);
	}
};

struct ITexurePool {
	virtual void ReleaseTexture(ID3D11Texture2D* tex) = 0;
};
struct OutputTexture
{
	ID3D11Texture2D* tex;
	ITexurePool* pool;
	OutputTexture(ID3D11Texture2D* tex, ITexurePool* pool)
		: tex(tex), pool(pool) { }
	~OutputTexture()
	{
		if (tex != nullptr) {
			pool->ReleaseTexture(tex);
		}
	}
};

// AviUtl用ロジックを実装したクラス
class D3DVPAviUtlWork : public D3DVP<std::shared_ptr<OutputTexture>, AviUtlErrorHandler>, public ITexurePool
{
	bool is420;

	AviUtlErrorHandler errorHandler;
	FILTER *fp_;
	FILTER_PROC_INFO *fpip_;

	PIXEL_YC* GetChildFrame(int n, AviUtlErrorHandler* env)
	{
		n = std::max(0, std::min(fpip_->frame_n - 1, n));

		PIXEL_YC* frame_ptr;
		if (n == fpip_->frame) {
			// 今のフレーム
			frame_ptr = fpip_->ycp_edit;
		}
		else {
			// 今のフレームでない
			frame_ptr = (PIXEL_YC*)fp_->exfunc->get_ycp_filtering(fp_, fpip_->editp, n, NULL);
			if (frame_ptr == NULL) {
				// メモリ不足
				env->ThrowError("メモリ不足");
			}
		}
		return frame_ptr;
	}

	virtual void ReleaseTexture(ID3D11Texture2D* tex) {
		auto& lock = with(outputTexPoolLock);
		outputTexPool.push_back(tex);
	}

	virtual void InputFrame(int n, bool reset, AviUtlErrorHandler* env)
	{
		PRINTF("InputFrame %d\n", n);
		ID3D11Texture2D* tex;
		{
			auto& lock = with(inputTexPoolLock);
			tex = inputTexPool.back();
			inputTexPool.pop_back();
		}

		D3D11_MAPPED_SUBRESOURCE res;
		{
			auto& lock = with(deviceLock);
			COM_CHECK(devCtx->Map(tex, 0, D3D11_MAP_WRITE, 0, &res));
		}

		int nsrc = n * NumFramesPerBlock();
		auto src = GetChildFrame(nsrc, env);
		uint8_t* dstY = reinterpret_cast<uint8_t*>(res.pData);

		if (is420) {
			convert_format<YC48toNV12>(dstY, res.RowPitch, src, fpip_->w, fpip_->h, fpip_->max_w, fp_);
		}
		else {
			convert_format<YC48toYUY2>(dstY, res.RowPitch, src, fpip_->w, fpip_->h, fpip_->max_w, fp_);
		}

#if COUNT_FRAMES
		++cntTo;
#endif
		{
			auto& lock = with(deviceLock);
			devCtx->Unmap(tex, 0);
		}

		FrameData<ID3D11Texture2D*> data;
		data.env = env;
		data.reset = reset;
		data.n = n;
		data.data = tex;
		//processThread.put(std::move(data));
		processReceived(std::move(data));
	}

	virtual void OutputFrame(FrameData<ID3D11Texture2D*>& data)
	{
		FrameData<std::shared_ptr<OutputTexture>> out = (FrameHeader)data;
		out.data = std::make_shared<OutputTexture>(data.data, this);
		PRINTF("OutputFrame %d\n", data.n);
		auto& lock = with(receiveLock);
		if (out.reset) {
			receiveQ.clear();
		}
		int n = out.n;
		receiveQ.emplace_back(std::move(out));
		if (n == waitingFrame) {
			receiveCond.signal();
		}
	}

public:
	D3DVPAviUtlWork(VideoInfo srcvi, bool is420, int mode, int tff, int width, int height, int quality,
		const std::string& deviceName, int reset, int debug,
		AviUtlErrorHandler* env)
		: D3DVP(srcvi, is420 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_YUY2,
			mode, tff, width, height, quality, deviceName, reset, debug, env)
		, is420(is420)
	{
	}

	~D3DVPAviUtlWork() {
#if PRINT_WAIT
		double proP, proC;
		processThread.getTotalWait(proP, proC);
		PRINTF("processThread: %f,%f\n", proP, proC);
#endif
	}

	void GetFrame(FILTER *fp, FILTER_PROC_INFO *fpip, AviUtlErrorHandler* env) {
		fp_ = fp;
		fpip_ = fpip;

		PutInputFrame(fpip->frame, env);
		auto out = WaitFrame(fpip->frame, env);
		D3D11_MAPPED_SUBRESOURCE res;
		{
			auto& lock = with(deviceLock);
			COM_CHECK(devCtx->Map(out->tex, 0, D3D11_MAP_READ, 0, &res));
		}

		// YC48に変換
		const uint8_t* srcY = reinterpret_cast<const uint8_t*>(res.pData);
		if (is420) {
			convert_format<NV12toYC48>(fpip_->ycp_temp, srcY, res.RowPitch, width, height, fpip_->max_w, fp_);
		}
		else {
			convert_format<YUY2toYC48>(fpip_->ycp_temp, srcY, res.RowPitch, width, height, fpip_->max_w, fp_);
		}

#if COUNT_FRAMES
		++cntFrom;
#endif
		{
			auto& lock = with(deviceLock);
			devCtx->Unmap(out->tex, 0);
		}

		fpip_->w = width;
		fpip_->h = height;

		// tempとeditを入れ替え
		std::swap(fpip_->ycp_edit, fpip_->ycp_temp);

		PRINTF("GetFrame Finished %d\n", fpip->frame);
	}
};

enum {
	ID_GPU_SELECT_COMBO = 37555
};

// AviUtl用トップレベルプラグインクラス
class D3DVPAviUtl
{
	std::vector<std::string> deviceNames;
	std::unique_ptr<D3DVPAviUtlWork> w;
	int gpuindex;
	std::array<int, TRACK_N> track_c;
	std::array<int, CHECK_N> check_c;
	HWND combo;

	bool IsReset(FILTER *fp) {
		if (track_c[0] != fp->track[0] || // 品質
			track_c[1] != fp->track[1] || // 幅
			track_c[2] != fp->track[2]) // 高さ
		{
			return true;
		}
		for (int i = 0; i < (int)check_c.size(); ++i) {
			if (check_c[i] != fp->check[i]) return true;
		}
		return false;
	}

	bool IsFilterChanged(FILTER *fp) {
		return track_c[3] != fp->track[3] || track_c[4] != fp->track[4];
	}

	void StoreSetting(FILTER *fp) {
		for (int i = 0; i < (int)track_c.size(); ++i) {
			track_c[i] = fp->track[i];
		}
		for (int i = 0; i < (int)check_c.size(); ++i) {
			check_c[i] = fp->check[i];
		}
	}

	void InitDialog(HWND hwnd)
	{
		CreateWindow(
			TEXT("STATIC"), "使用GPU",
			WS_CHILD | WS_VISIBLE,
			10, 300, 300, 20,
			hwnd, NULL, NULL, NULL);

		combo = CreateWindow(
			TEXT("COMBOBOX"), NULL,
			WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
			10, 320, 300, 300, hwnd, (HMENU)ID_GPU_SELECT_COMBO, NULL, NULL
		);
		for (int i = 0; i < (int)deviceNames.size(); i++) {
			SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)deviceNames[i].c_str());
		}
	}

	void SetupRange(FILTER *fp, void *editp)
	{
		SYS_INFO si;

		if (fp->exfunc->get_sys_info) {
			fp->exfunc->get_sys_info(editp, &si);

			track_s[1] = si.min_w;
			track_s[2] = si.min_h;
			track_e[1] = si.max_w;
			track_e[2] = si.max_h;

			fp->exfunc->filter_window_update(fp);
		}
	}

public:
	D3DVPAviUtl()
		: gpuindex(-1)
		, track_c()
		, check_c()
	{
		AviUtlErrorHandler eh;
		auto env = &eh;

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

			deviceNames.push_back(to_string(desc.Description));
		}
	}

	BOOL WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp)
	{
		switch (message) {
		case WM_FILTER_INIT:
			InitDialog(hwnd);
			SetupRange(fp, editp);
			return TRUE;
		case WM_COMMAND:
			if (LOWORD(wparam) == ID_GPU_SELECT_COMBO) {
				if (HIWORD(wparam) == CBN_SELENDOK) {
					int index = (int)SendMessage(combo, CB_GETCURSEL, (WORD)0, 0L);
					if (gpuindex != index) {
						gpuindex = index;
						w = nullptr;
						fp->exfunc->filter_window_update(fp);
						return fp->exfunc->is_filter_active(fp);
					}
				}
			}
			return FALSE;
		//case WM_FILTER_EXIT:
		//	break;
		case WM_KEYUP:
		case WM_KEYDOWN:
		case WM_MOUSEWHEEL:
			SendMessage(GetWindow(hwnd, GW_OWNER), message, wparam, lparam);
			break;
		}

		UNREFERENCED_PARAMETER(editp);
		UNREFERENCED_PARAMETER(lparam);

		return FALSE;
	}

	void Proc(FILTER *fp, FILTER_PROC_INFO *fpip, int retry) {
		AviUtlErrorHandler eh;

		if (IsReset(fp)) {
			w = nullptr;
		}
		bool needSetFilter = false;
		if (w == nullptr) {
			VideoInfo srcvi = { 0 };
			srcvi.width = fpip->w;
			srcvi.height = fpip->h;
			srcvi.fps_numerator = 30000;
			srcvi.fps_denominator = 1001;

			// リサイズ設定
			int width = clamp(fp->track[1], track_s[1], track_e[1]) & ~3;
			int height = clamp(fp->track[2], track_s[2], track_e[2]) & ~3;

			w = std::unique_ptr<D3DVPAviUtlWork>(new D3DVPAviUtlWork(srcvi,
				fp->check[6] != 0,
				fp->check[0],
				!fp->check[1],
				fp->check[2] ? width : srcvi.width,
				fp->check[2] ? height : srcvi.height,
				fp->track[0],
				(gpuindex == -1) ? "" : deviceNames[gpuindex],
				30, // reset: 32bitでメモリが少ないので小さめにする
				fp->check[7],
				&eh));
			needSetFilter = true;
		}
		else if (IsFilterChanged(fp)) {
			needSetFilter = true;
		}
		if (needSetFilter) {
			w->SetFilter(fp->check[3] != 0,
				fp->check[4] ? fp->track[3] : -1,
				fp->check[5] ? fp->track[4] : -1,
				&eh);
			w->Reset();
		}
		StoreSetting(fp);

		try {
			w->GetFrame(fp, fpip, &eh);
		}
		catch (const std::string&) {
			if (retry > 2) {
				throw;
			}
			// リトライしてみる
			w = nullptr;
			Proc(fp, fpip, retry + 1);
			return;
		}
	}

	void DeleteFilter() {
		w = nullptr;
	}
};

D3DVPAviUtl *g_filter;


BOOL func_init(FILTER *fp)
{
	g_filter = new D3DVPAviUtl();
	return TRUE;
}

BOOL func_exit(FILTER *fp)
{
	delete g_filter;
	return TRUE;
}

BOOL func_update(FILTER *fp, int status)
{
	return TRUE;
}

/*------------------------------------------------------------------
window procedure
------------------------------------------------------------------*/
BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp)
{
	return g_filter->WndProc(hwnd, message, wparam, lparam, editp, fp);
}

BOOL func_save_start(FILTER *fp, int s, int e, void *editp)
{
	if (g_filter != nullptr) {
		// ゴミフレームが混ざるのを避けるためキャッシュクリアしておく
		g_filter->DeleteFilter();
	}
	return TRUE;
}

BOOL func_save_end(FILTER *fp, void *editp)
{
	return TRUE;
}

//---------------------------------------------------------------------
//		フィルタ処理関数
//---------------------------------------------------------------------
BOOL func_proc(FILTER *fp, FILTER_PROC_INFO *fpip)
{
	try {
		SectionTime timer;
		g_filter->Proc(fp, fpip, 0);
		return TRUE;
	}
	catch (std::string& mes) {
		MessageBox(NULL, mes.c_str(), "D3DVP AviUtl Plugin", MB_OK);
		return FALSE;
	}
}


#pragma endregion
