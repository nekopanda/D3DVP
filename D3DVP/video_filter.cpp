//----------------------------------------------------------------------------------
//		サンプルビデオフィルタ(フィルタプラグイン)  for AviUtl ver0.99e以降
//----------------------------------------------------------------------------------
#include <windows.h>
#undef max
#undef min

#include <algorithm>
#include <string>
#include <mutex>
#include <condition_variable>

// avisynthにリンクしているので
#define AVS_LINKAGE_DLLIMPORT
#include "avisynth.h"
#pragma comment(lib, "avisynth.lib")

#include "filter.h"

// デバッグ用
#define THROUGH_VIDEO 0

HMODULE g_DllHandle;

//---------------------------------------------------------------------
//		フィルタ構造体定義
//---------------------------------------------------------------------
#define	TRACK_N	2														//	トラックバーの数
TCHAR	*track_name[] =		{	"Preset", "Threads"	};	//	トラックバーの名前
int		track_default[] =	{	2, 4 };	//	トラックバーの初期値
int		track_s[] =			{	-4, 0 };	//	トラックバーの下限値
int		track_e[] =			{	6, 16 };	//	トラックバーの上限値

#define	CHECK_N	4													//	チェックボックスの数
TCHAR	*check_name[] = 	{	"2倍fps化（2倍fpsで入力してね）", "YUV420で処理", "BFF", "処理しない（デバッグ用）" };				//	チェックボックスの名前
int		check_default[] = 	{	1, 1, 0, 0			};				//	チェックボックスの初期値 (値は0か1)

FILTER_DLL filter = {
	FILTER_FLAG_EX_INFORMATION,	//	フィルタのフラグ
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
	0,0,						//	設定ウインドウのサイズ (FILTER_FLAG_WINDOW_SIZEが立っている時に有効)
	"QTGMC(Avisynth)",			//	フィルタの名前
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
	NULL,						//	設定ウィンドウにウィンドウメッセージが来た時に呼ばれる関数へのポインタ (NULLなら呼ばれません)
	NULL,NULL,					//	システムで使いますので使用しないでください
	NULL,						//  拡張データ領域へのポインタ (FILTER_FLAG_EX_DATAが立っている時に有効)
	NULL,						//  拡張データサイズ (FILTER_FLAG_EX_DATAが立っている時に有効)
	"サンプルフィルタ version 0.06 by ＫＥＮくん",
								//  フィルタ情報へのポインタ (FILTER_FLAG_EX_INFORMATIONが立っている時に有効)
  func_save_start,						//	セーブが開始される直前に呼ばれる関数へのポインタ (NULLなら呼ばれません)
  func_save_end,						//	セーブが終了した直前に呼ばれる関数へのポインタ (NULLなら呼ばれません)
};


//---------------------------------------------------------------------
//		フィルタ構造体のポインタを渡す関数
//---------------------------------------------------------------------
EXTERN_C FILTER_DLL __declspec(dllexport) * __stdcall GetFilterTable( void )
{
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

void ConvertYC48toYUY2(PVideoFrame& dst, const PIXEL_YC* src, int w, int h, int max_w)
{
  BYTE* dstptr = dst->GetWritePtr();
  int pitch = dst->GetPitch();

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; x += 2) {
      short y0 = src[x + y * max_w].y;
      short y1 = src[x + 1 + y * max_w].y;
      short cb = src[x + y * max_w].cb;
      short cr = src[x + y * max_w].cr;

      BYTE Y0 = clamp(((y0 * 219 + 383) >> 12) + 16, 0, 255);
      BYTE Y1 = clamp(((y1 * 219 + 383) >> 12) + 16, 0, 255);
      BYTE U = clamp((((cb + 2048) * 7 + 66) >> 7) + 16, 0, 255);
      BYTE V = clamp((((cr + 2048) * 7 + 66) >> 7) + 16, 0, 255);

      dstptr[x * 2 + 0 + y * pitch] = Y0;
      dstptr[x * 2 + 1 + y * pitch] = U;
      dstptr[x * 2 + 2 + y * pitch] = Y1;
      dstptr[x * 2 + 3 + y * pitch] = V;
    }
  }
}

void ConvertYUY2toYC48(PIXEL_YC* dst, PVideoFrame& src, int w, int h, int max_w)
{
  const BYTE* srcptr = src->GetReadPtr();
  int pitch = src->GetPitch();

  for (int y = 0; y < h; ++y) {

    // まずはUVは左にそのまま入れる
    for (int x = 0, x2 = 0; x < w; x += 2, ++x2) {
      BYTE Y0 = srcptr[x * 2 + 0 + y * pitch];
      BYTE U = srcptr[x * 2 + 1 + y * pitch];
      BYTE Y1 = srcptr[x * 2 + 2 + y * pitch];
      BYTE V = srcptr[x * 2 + 3 + y * pitch];

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

#define SOURCE_FILTER_NAME "AviUtlFilterSource"

class QTGMCTest
{
  struct Config {
    int preset;
    int threads;
    bool doublefps;
    bool yv12;
    bool bff;
    bool through;

    bool operator==(Config o) {
      return preset == o.preset &&
        threads == o.threads &&
        doublefps == o.doublefps &&
        yv12 == o.yv12 &&
        bff == o.bff &&
        through == o.through;
    }
    bool operator!=(const Config& o) {
      return !(*this == o);
    }
  };

  Config conf;

  IScriptEnvironment2* env_;

  PClip filter_;

  std::mutex mutex_;
  std::condition_variable getframe_cond_;
  std::condition_variable proc_cond_;
  bool allowGetFrame_;
  bool cancelGetFrame_;
  int enterCount;

  void CreateFilter() {
    const char* presets[] = {
      "Placebo",
      "Very Slow",
      "Slower",
      "Slow",
      "Medium",
      "Fast",
      "Faster",
      "Very Fast",
      "Super Fast",
      "Ultra Fast",
      "Draft"
    };

    env_ = CreateScriptEnvironment2();

    // 自分をプラグインとしてロード
    AVSValue result;
    std::string modulepath = GetModulePath();
    env_->LoadPlugin(modulepath.c_str(), true, &result);

    AVSValue last = env_->Invoke(SOURCE_FILTER_NAME, AVSValue(nullptr, 0), 0);
    if (conf.yv12) {
      AVSValue args[] = { last, true };
      const char* arg_names[] = { nullptr, "interlaced", };
      last = env_->Invoke("ConvertToYV12", AVSValue(args, 2), arg_names);
    }
    if (conf.through == false) {
      if (conf.doublefps) {
        // 2倍fps化の場合は、ソースも2倍fpsされているので戻す
        AVSValue args[] = { last, 2, 0 };
        last = env_->Invoke("SelectEvery", AVSValue(args, 3), 0);
      }
      AVSValue args[] = { last, presets[conf.preset + 4], (conf.doublefps ? 1 : 2) };
      const char* arg_names[] = { nullptr, "Preset", "FPSDivisor" };
      last = env_->Invoke("QTGMC", AVSValue(args, 3), arg_names);
    }
    if (conf.yv12) {
      AVSValue args[] = { last };
      last = env_->Invoke("ConvertToYUY2", AVSValue(args, 1), 0);
    }
    if (conf.threads > 0) {
      AVSValue args[] = { last, conf.threads };
      last = env_->Invoke("Prefetch", AVSValue(args, 2), 0);
    }
    filter_ = last.AsClip();
  }

  Config ReadConfig() {
    Config c;
    c.preset = fp_->track[0];
    c.threads = fp_->track[1];
    c.doublefps = (fp_->check[0] != 0);
    c.yv12 = (fp_->check[1] != 0);
    c.bff = (fp_->check[2] != 0);
    c.through = (fp_->check[3] != 0);
    return c;
  }

  std::string GetModulePath() {
    char buf[MAX_PATH];
    GetModuleFileName(g_DllHandle, buf, MAX_PATH);
    return buf;
  }

  void EnterProc()
  {
    // GetFrameで待っている人を起こす
    std::lock_guard<std::mutex> lock(mutex_);
    allowGetFrame_ = true;
    getframe_cond_.notify_all();
  }

  void ExitProc()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (enterCount > 0) {
      // GetFrameから抜けるまで待つ
      proc_cond_.wait(lock);
    }
    allowGetFrame_ = false;
  }

  class ProcGuard
  {
    QTGMCTest* pThis;
  public:
    ProcGuard(QTGMCTest* pThis) : pThis(pThis)
    {
      pThis->EnterProc();
    }
    ~ProcGuard()
    {
      pThis->ExitProc();

      // 安全のためアクセス出来ないようにしておく
      pThis->fp_ = nullptr;
      pThis->fpip_ = nullptr;
    }
  };

public:

  FILTER *fp_;
  FILTER_PROC_INFO *fpip_;

  QTGMCTest()
    : conf()
    , env_(nullptr)
    , allowGetFrame_(false)
    , cancelGetFrame_(false)
    , enterCount(0)
  {
    //
  }

  ~QTGMCTest()
  {
    DeleteFilter();
  }

  void DeleteFilter() {
    if (env_ != nullptr) {
      {
        // GetFrameで待っている人を起こす
        std::lock_guard<std::mutex> lock(mutex_);
        cancelGetFrame_ = true;
      }
      getframe_cond_.notify_all();

      filter_ = nullptr;
      env_->DeleteScriptEnvironment();
      env_ = nullptr;
      allowGetFrame_ = false;
      cancelGetFrame_ = false;
    }
  }

  BOOL Proc(FILTER *fp, FILTER_PROC_INFO *fpip)
  {
    fp_ = fp;
    fpip_ = fpip;

    try {
      // 設定が変わっていたら作り直す
      Config newconf = ReadConfig();
      if (conf != newconf) {
        DeleteFilter();
        conf = newconf;
      }
      if (!filter_) {
        CreateFilter();
      }

      if (!fp->exfunc->set_ycp_filtering_cache_size(fp, fpip->max_w, fpip->h, 3, NULL)) {
        MessageBox(fp->hwnd, "キャッシュ設定に失敗", "QTGMCTest", MB_OK);
        return FALSE;
      }

      ProcGuard guard(this);

      PVideoFrame frame = filter_->GetFrame(fpip_->frame, env_);

      // YC48に変換
      ConvertYUY2toYC48(fpip_->ycp_temp, frame, fpip_->w, fpip_->h, fpip_->max_w);

      // tempとeditを入れ替え
      std::swap(fpip_->ycp_edit, fpip_->ycp_temp);
    }
    catch(AvisynthError& err) {
      MessageBox(fp->hwnd, err.msg, "QTGMCTest", MB_OK);
      return FALSE;
    }
    catch (IScriptEnvironment::NotFound&) {
      MessageBox(fp->hwnd, "関数が見つかりません", "QTGMCTest", MB_OK);
      return FALSE;
    }
    return TRUE;
  }

  // 戻り値: キャンセルされたか
  bool EnterGetFrame()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (allowGetFrame_ == false && cancelGetFrame_ == false) {
      // procが呼ばれるまで待つ
      getframe_cond_.wait(lock);
    }
    ++enterCount;
    return cancelGetFrame_;
  }

  void ExitGetFrame()
  {
    std::unique_lock<std::mutex> lock(mutex_);

    //if (cancelGetFrame_ == false) {
    //  if (fp_ == nullptr || fpip_ == nullptr) {
    //    MessageBox(NULL, "バグってます！", "QTGMCTest", MB_OK);
    //  }
    //}

    --enterCount;
    proc_cond_.notify_all();
  }

  const Config& GetConf() { return conf; }
};

class AviUtlFilterSource : public IClip
{
  QTGMCTest* pThis;
  VideoInfo vi;
  bool istff;

  class GetFrameGuard
  {
    QTGMCTest* pThis;
    bool isCanceled;
  public:
    GetFrameGuard(QTGMCTest* pThis)
      : pThis(pThis)
    {
      isCanceled = pThis->EnterGetFrame();
    }
    ~GetFrameGuard()
    {
      pThis->ExitGetFrame();
    }
    bool IsCanceled()
    {
      return isCanceled;
    }
  };

public:
  AviUtlFilterSource(QTGMCTest* pThis)
    : pThis(pThis)
    , vi()
  {
    istff = !pThis->GetConf().bff;

    vi.width = pThis->fpip_->w;
    vi.height = pThis->fpip_->h;
    vi.pixel_type = VideoInfo::CS_YUY2;
    vi.num_frames = pThis->fpip_->frame_n;

    // FPSは使わないので適当な値にしておく
    vi.SetFPS(30000, 1001);
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env)
  {
    // nが範囲外に行かないようにする
    n = clamp(n, 0, vi.num_frames - 1);

    GetFrameGuard guard(pThis);

    if (guard.IsCanceled()) {
      return env->NewVideoFrame(vi);
    }

    // AviUtlからフレーム取得
    PIXEL_YC* frame_ptr;
    if (n == pThis->fpip_->frame) {
      // 今のフレーム
      frame_ptr = pThis->fpip_->ycp_edit;
    }
    else {
      // 今のフレームでない
      frame_ptr = pThis->fp_->exfunc->get_ycp_filtering_cache_ex(pThis->fp_, pThis->fpip_->editp, n, NULL, NULL);
      if (frame_ptr == NULL) {
        // メモリ不足
        env->ThrowError("メモリ不足");
      }
    }

    PVideoFrame dst = env->NewVideoFrame(vi);

    // YUY2に変換
    ConvertYC48toYUY2(dst, frame_ptr, pThis->fpip_->w, pThis->fpip_->h, pThis->fpip_->max_w);

    return dst;
  }

  void __stdcall GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) { }
  const VideoInfo& __stdcall GetVideoInfo() { return vi; }
  bool __stdcall GetParity(int n) { return istff; }
  int __stdcall SetCacheHints(int cachehints, int frame_range) { return 0; };

  static AVSValue Create(AVSValue args, void* user_data, IScriptEnvironment* env) {
    return new AviUtlFilterSource(static_cast<QTGMCTest*>(user_data));
  }
};

QTGMCTest* g_filter;

BOOL func_init(FILTER *fp)
{
  g_filter = new QTGMCTest();
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
BOOL func_proc( FILTER *fp,FILTER_PROC_INFO *fpip )
{
  return g_filter->Proc(fp, fpip);
}

extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {
  // 直接リンクしているのでvectorsを格納する必要はない

  env->AddFunction(SOURCE_FILTER_NAME, "", AviUtlFilterSource::Create, g_filter);
  
  return "QTGMCAviUtlPlugin";
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH) g_DllHandle = hModule;
  return TRUE;
}
