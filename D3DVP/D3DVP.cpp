
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

	int logUVx;
	int logUVy;

	PCom<ID3D11Device> dev;
	PCom<ID3D11DeviceContext> devCtx;
	PCom<ID3D11VideoDevice> videoDev;
	PCom<ID3D11VideoProcessor> videoProc;
	PCom<ID3D11VideoProcessorEnumerator> videoProcEnum;

	D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rccaps;

	void toNV12(PVideoFrame& src, pixel_t* dst)
	{
		const pixel_t* srcY = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_Y));
		const pixel_t* srcU = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_U));
		const pixel_t* srcV = reinterpret_cast<const pixel_t*>(src->GetReadPtr(PLANAR_V));

		int pitchY = src->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = src->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int widthUV = vi.width >> logUVx;
		int heightUV = vi.height >> logUVy;
		int offsetUV = vi.width * vi.height;

		for (int y = 0; y < vi.height; ++y) {
			for (int x = 0; x < vi.width; ++x) {
				dst[x + y * vi.width] = srcY[x + y * pitchY];
			}
		}
		
		for (int y = 0; y < heightUV; ++y) {
			for (int x = 0; x < widthUV; ++x) {
				dst[x * 2 + 0 + y * widthUV] = srcU[x + y * pitchUV];
				dst[x * 2 + 1 + y * widthUV] = srcV[x + y * pitchUV];
			}
		}
	}

	void fromNV12(PVideoFrame& dst, const pixel_t* src)
	{
		pixel_t* dstY = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_Y));
		pixel_t* dstU = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_U));
		pixel_t* dstV = reinterpret_cast<pixel_t*>(dst->GetWritePtr(PLANAR_V));

		int pitchY = dst->GetPitch(PLANAR_Y) / sizeof(pixel_t);
		int pitchUV = dst->GetPitch(PLANAR_U) / sizeof(pixel_t);
		int widthUV = vi.width >> logUVx;
		int heightUV = vi.height >> logUVy;
		int offsetUV = vi.width * vi.height;

		for (int y = 0; y < vi.height; ++y) {
			for (int x = 0; x < vi.width; ++x) {
				dstY[x + y * pitchY] = src[x + y * vi.width];
			}
		}

		for (int y = 0; y < heightUV; ++y) {
			for (int x = 0; x < widthUV; ++x) {
				dstU[x + y * pitchUV] = src[x * 2 + 0 + y * widthUV];
				dstV[x + y * pitchUV] = src[x * 2 + 1 + y * widthUV];
			}
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

	void CreateProcessor(const std::string& name, IScriptEnvironment2* env)
	{
		auto wname = to_wstring(name);

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
			
			if (memcmp(wname.data(), desc.Description, std::min(wname.size(), sizeof(desc.Description) / sizeof(desc.Description[0])))) {
				continue;
			}
			//printf("%ls\n", desc.Description);

			// D3D11デバイス作成
			ID3D11Device* pDevice_;
			ID3D11DeviceContext* pContext_;
			const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1 };
			COM_CHECK(D3D11CreateDevice(pAdapter.get(),
				D3D_DRIVER_TYPE_UNKNOWN, // アダプタ指定の場合はUNKNOWN
				NULL,
				0,
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
			vdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST; // TFF
			vdesc.InputFrameRate.Numerator = vi.fps_numerator;
			vdesc.InputFrameRate.Denominator = vi.fps_denominator;
			vdesc.InputHeight = vi.height;
			vdesc.InputWidth = vi.width;
			vdesc.OutputFrameRate.Numerator = vi.fps_numerator * 2;
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
			for (int rci = 0; rci < caps.RateConversionCapsCount; ++rci) {
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

public:
	D3DVP(PClip child, IScriptEnvironment2* env)
		: GenericVideoFilter(child)
		, logUVx(vi.GetPlaneWidthSubsampling(PLANAR_U))
		, logUVy(vi.GetPlaneHeightSubsampling(PLANAR_U))
	{
		CreateProcessor("NVIDIA", env);

		// 必要なテクスチャ枚数
		int numInputTex = rccaps.FutureFrames + rccaps.PastFrames + 1;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = vi.width;
		desc.Height = vi.height;
		desc.MipLevels = 1;
		desc.ArraySize = numInputTex;
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

		ID3D11Texture2D* pTexInput_;
		COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexInput_));
		auto pTexInput = make_com_ptr(pTexInput_);

		// 出力用テクスチャ
		desc.ArraySize = 1;
		// output must be D3D11_BIND_RENDER_TARGET
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;

		ID3D11Texture2D* pTexOutput_;
		COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexOutput_));
		auto pTexOutput = make_com_ptr(pTexOutput_);

		// 出力でCPUで読むためのテクスチャ
		desc.ArraySize = 1;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		ID3D11Texture2D* pTexCPU_;
		COM_CHECK(dev->CreateTexture2D(&desc, NULL, &pTexCPU_));
		auto pTexCPU = make_com_ptr(pTexCPU_);

		// InputView作成
		std::vector<PCom<ID3D11VideoProcessorInputView>> inputViews;
		for(int i = 0; i < numInputTex; ++i) {
			ID3D11VideoProcessorInputView* pInputView_;
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = { 0 };
			inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			inputViewDesc.Texture2D.ArraySlice = i;
			inputViewDesc.Texture2D.MipSlice = 0;
			COM_CHECK(videoDev->CreateVideoProcessorInputView(
				pTexInput.get(), videoProcEnum.get(), &inputViewDesc, &pInputView_));
			inputViews.emplace_back(pInputView_);
		}

		// OutputView作成
		ID3D11VideoProcessorOutputView* pOutputView_;
		{
			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = { D3D11_VPOV_DIMENSION_TEXTURE2D };
			outputViewDesc.Texture2D.MipSlice = 0;
			COM_CHECK(videoDev->CreateVideoProcessorOutputView(
				pTexOutput.get(), videoProcEnum.get(), &outputViewDesc, &pOutputView_));
		}
		auto outputView = make_com_ptr(pOutputView_);

		ID3D11VideoContext* pVideoCtx_;
		COM_CHECK(devCtx->QueryInterface(&pVideoCtx_));
		auto pVideoCtx = make_com_ptr(pVideoCtx_);

		// Setup streams
		D3D11_VIDEO_PROCESSOR_STREAM streams = { 0 };
		streams.Enable = TRUE;
		streams.pInputSurface = inputViews[0].get();

		// Perform VideoProc Blit Operation (with color conversion)
		COM_CHECK(pVideoCtx->VideoProcessorBlt(videoProc.get(), outputView.get(), 0, 1, &streams));

		// 出力を読み取る
		D3D11_MAPPED_SUBRESOURCE res;
		COM_CHECK(devCtx->Map(pTexCPU.get(), 0, D3D11_MAP_READ, 0, &res));
		// TODO:
		devCtx->Unmap(pTexCPU.get(), 0);

		// TODO: 出力フレームをキャッシュする
		// TODO: 入力フレームを用意する機構を作る
		// TODO: シーク可能なGetFrameロジック
		// TODO: オプション・パラメータ
	}

	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env_)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);

		PVideoFrame dst = env->NewVideoFrame(vi);

		return dst;
	}

	static AVSValue __cdecl Create(AVSValue args, void* user_data, IScriptEnvironment* env_)
	{
		IScriptEnvironment2* env = static_cast<IScriptEnvironment2*>(env_);
		return new D3DVP(
			args[0].AsClip(),
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

	env->AddFunction("D3DVP", "c", D3DVP::Create, 0);

	return "Direct3D VideoProcessing Plugin";
}
