# D3DVP
Direct3D 11 Video Processing Avisynth Filter

Direct3D 11 の Video API を使ったインタレ解除フィルタです。
お持ちのGPUのドライバがちゃんと実装されていれば、GPUでインタレ解除されます。

## 動作環境

- Windows 8以降
- Avisynth 2.6以降？（AvisynthPlus CUDAでしか試してないから不明）

## 関数

D3DVP(clip, int "mode", int "order", int "quality", bool "autop",
		int "nr", int "edge", string "device", int "reset", int "debug")

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

	reset:
		初期化時やシーク時に読み込ませるフレーム数です。
		ドライバによってステートを持っていることがあるので、
		シーク時におかしくならないように、シーク時に
		ある程度、シーク位置前方のフレームを読み込ませます。
		その枚数の指定です。
		デフォルト: 30

	debug:
		デバッグ用です。
		1にすると処理をバイパスしてフレームをコピーします。

※nr,edgeはドライバによっては実装されていないこともあります。

## 制限

処理は完全にドライバ依存なので、
PCのグラフィックス設定や、GPUに種類によって
画質が変わる可能性があります。

## サンプルスクリプト

```
# IntelとNVIDIAとRadeonのインタレ解除を比較
srcfile="..."
LWLibavVideoSource(srcfile)
intel = D3DVP(device="Intel")
nvidia = D3DVP(device="NVIDIA")
radeon = D3DVP(device="Radeon")
Interleave(intel.subtitle("Intel"),nvidia.subtitle("NVIDIA"),radeon.subtitle("radeon"))
```

## ライセンス

D3DVPのソースコードはMITライセンスとします。
