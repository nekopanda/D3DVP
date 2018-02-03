# D3DVP
Direct3D 11 Video Processing Avisynth/AviUtl Filter

Direct3D 11 の Video API を使ったインタレ解除フィルタです。
お持ちのGPUのドライバがちゃんと実装されていれば、GPUでインタレ解除できます。

## [ダウンロードはこちらから](https://github.com/nekopanda/D3DVP/releases)

# Avisynth版

## 動作環境

- **Windows 8以降**
- Avisynth 2.6以降？（AvisynthPlus CUDAでしか試してないから不明）

## インストール

D3DVP.dllをプラグインフォルダ（plugins+/plugins64+)にコピーしてください。

## 関数

D3DVP(clip, int "mode", int "order", int "width", int "height", int "quality", bool "autop",
		int "nr", int "edge", string "device", int "cache", int "reset", int "debug")

	mode:
		インタレ解除モード
		- 0: 同じFPSで出力(half rate)
		- 1: 2倍FPSで出力(normal rate)
		デフォルト: 1

	order:
		フィールドオーダー
		- -1: Avisynthのparityを使用
		-  0: bottom field first (bff)
		-  1: top field first (tff)
		デフォルト: -1

	width:
		出力画像の幅
		0の場合はリサイズしない
		デフォルト: 0

	height:
		出力画像の高さ
		0の場合はリサイズしない
		デフォルト: 0

	quality:
		品質（ドライバによっては効果がないこともあります）
		- 0: 速度重視
		- 1: ふつう
		- 2: 品質重視
		デフォルト: 2

	autop:
		ドライバの自動画質補正を有効にするか
		- False: 無効
		- True: 有効
		デフォルト: False

	nr:
		ノイズリダクションの強度(0-100)
		-1でノイズリダクションを無効
		デフォルト: -1

	edge:
		エッジ強調の強度（0-100）
		-1でエッジ強調を無効
		デフォルト: -1

	device:
		使用するGPUのデバイス名。前方一致で比較されます。
		GPUのデバイス名はデバイスマネージャー等で確認してください。
		例) "Intel", "NVIDIA", "Radeon"
		デフォルト: ""（指定なし）

	cache:
		シーク時に処理してキャッシュする前方フレームの数です。
		小さくするとシークが高速になる反面、
		時間軸逆方向へフレームを進めるのが遅くなります。
		大きくするとこの逆になります。
		デフォルト: 15

	reset:
		初期化時やシーク時に捨てるフレーム数です。
		特に問題がなければデフォルト値でOK
		ドライバによってステートを持っていることがあるので、
		シーク直後は正しい結果が出てこないことがあるので
		処理結果を数フレームスキップします。
		その枚数の指定です。
		デフォルト: 4

	debug:
		デバッグ用です。
		1にすると処理をバイパスしてフレームをコピーします。

※nr,edgeはドライバによっては実装されていないこともあります。

## 制限

フォーマットは8bitYUV420のみ対応

処理は完全にドライバ依存なので、
PCのグラフィックス設定や、GPUに種類によって
画質が変わる可能性があります。

## サンプルスクリプト

```
# 普通にインタレ解除
srcfile="..."
LWLibavVideoSource(srcfile)
D3DVP()
```

```
# IntelとNVIDIAとRadeonのインタレ解除を比較
srcfile="..."
LWLibavVideoSource(srcfile)
intel = D3DVP(device="Intel")
nvidia = D3DVP(device="NVIDIA")
radeon = D3DVP(device="Radeon")
Interleave(intel.subtitle("Intel"),nvidia.subtitle("NVIDIA"),radeon.subtitle("radeon"))
```

```
# インタレ解除して1280x720にリサイズ
srcfile="..."
LWLibavVideoSource(srcfile)
D3DVP(width=1280,height=720)
```

# AviUtl版

## 動作環境

- **Windows 8以降**

## インストール

D3DVP.aufをコピーしてください。「Direct3D 11インタレ解除」フィルタが追加されます。

※D3DVP.aufはD3DVP.dllをリネームしただけで中身は同じです。

## パラメータ

*  品質（ドライバによっては効果がないこともあります）
   * 0: 速度重視
   * 1: ふつう
   * 2: 品質重視

* 幅、高さ
   * 「リサイズ」にチェックした場合のみ有効

* NR
   * ノイズ除去の強度。「ノイズ除去」にチェックした場合のみ有効

* EDGE
   * エッジ強調の強度。「エッジ強調」にチェックした場合のみ有効

* 2倍fps化
   * bobでインタレ解除します。
   * AviUtlはフィルタがfpsを変更することはできないため、動画は2倍のFPSで読み込ませておいてください。ソースが29.97fpsの場合は、59.94fpsで読み込んでください。「60fps読み込み」や「60fps」ではありません。60fpsで読み込むとインタレ縞が残ることがあります。

* YUV420で処理
   * 通常はYUY2で処理しますが、一部YUY2での処理に対応していないドライバがあるため、その場合はこれにチェックしてYUV420で処理してください。

* 使用GPU
   * 使用するGPUの指定です。指定がない場合は一番上のGPUを使います。

## AviUtl版の制限

AviUtlの制限から、処理速度はAviSynth版より遅くなります。AviSynth版の半分以下くらいです。

# ライセンス

D3DVPのソースコードはMITライセンスとします。
