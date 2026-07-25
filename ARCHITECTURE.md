# Viewer アーキテクチャ設計

## 設計原理 — PixelScope から継承するもの

PixelScope (pixelscope.html) の本質は、`{name, w, h, ch, data, dtype}` という**極小の画像記述子を契約として、
ローダも解析もカラーマップもすべてプラグインにした**ことにある。C++ 版はこの構造を ABI として固定する。

原則:
1. **コアは「Frame を表示・比較する」ことしかしない。** デコードも処理も解析もプラグイン。
2. **組み込み機能も同じプラグイン API を通す**(npy ローダも NVDEC も dogfooding)。
3. **パラメータは宣言、UI は自動生成。** プラグインが JSON Schema でパラメータを宣言し、
   ImGui のスライダー/コンボは自動で生える。UI コードをプラグイン作者に書かせない。

## Frame 記述子 (C ABI)

```c
typedef enum { PS_U8, PS_U16, PS_F16, PS_F32, PS_BF16 } psDtype;
typedef enum { PS_MEM_CPU, PS_MEM_CUDA } psMemLoc;

typedef struct psFrame {
    uint32_t    w, h, ch;
    psDtype     dtype;
    psMemLoc    loc;        // CPU ポインタ or CUdeviceptr
    void*       data;
    size_t      pitch;      // 行バイト数 (パディング対応)
    int64_t     pts_us;     // 動画/連番の時刻 (μs)。静止画は -1
    const char* name;
    const char* meta_json;  // 自由メタデータ
} psFrame;
```

- CUDA デバイスメモリを一級市民にする (`loc`)。CPU に降ろさず表示 (GL interop)・処理ができる。
- PixelScope の `desc` と 1:1 対応。Web 連携時はそのまま JSON + バイナリにシリアライズ。

## プラグイン種別

| 種別 | 役割 | 例 |
|---|---|---|
| **Loader** | バイト列 → Frame(列) | npy/npz, RAW/YUV, EXR, DNG |
| **Source** | 時間軸を持つ Frame 供給 | ffmpeg/NVDEC 動画, 連番ディレクトリ, カメラ/ソケット受信 |
| **Processor** | Frame → Frame | **社内 C++ パイプラインの各段**, デモザイク, NR, トーンマップ |
| **Analyzer** | Frame → 指標/オーバーレイ | PSNR/SSIM, ヒストグラム, エッジ強度マップ |
| **Colormap** | 1ch → RGB LUT | viridis, ironbow, 自作 |

```c
typedef struct psProcessorV1 {
    uint32_t    abi_version;      // = 1
    const char* name;
    const char* params_schema;    // JSON Schema → ImGui 自動生成
    void* (*create)(void);
    int   (*process)(void* self, const psFrame* in, psFrame* out, const char* params_json);
    void  (*destroy)(void* self);
} psProcessorV1;

// DLL は psRegisterPlugins(psRegistry*) を export するだけ
```

## パイプラインと A/B 比較

処理はノードチェーンとして構成する:

```
Source ─▶ [Proc: 社内NR v1] ─▶ [Proc: トーンマップ] ─▶ 表示 A
   └────▶ [Proc: 社内NR v2] ─▶ [Proc: トーンマップ] ─▶ 表示 B
```

- **同一ソースに2系統のチェーン(またはパラメータ違い)を張って A/B**。ワイプ/差分/PSNR は表示層の仕事。
- 各ノード出力は (frame_id, chain_hash, params_hash) でキャッシュ。スライダーを動かした段以降だけ再計算。
- 動画/連番でも同じ: 再生しながらパラメータを動かして差分を見る。

## GPU はオプション — CPU フォールバックの原則

CUDA / NVIDIA GPU が無い環境でもフル機能(速度以外)で動くことを ABI レベルで保証する:

1. **コアは CUDA をリンクしない。** 表示は OpenGL のみ(iGPU で動く)。CUDA は起動時に
   `LoadLibrary("nvcuda.dll")` で動的検出し、無ければ CPU モードで続行する。
2. **すべての Processor は CPU フレーム (`PS_MEM_CPU`) を受けられることが必須。**
   CUDA 対応は `caps = PS_CAP_CPU | PS_CAP_CUDA` の宣言によるオプション扱い。
   CUDA しか実装しないプラグインは登録時に拒否される (契約違反)。
3. **デバイス境界はコアが吸収する。** CPU プラグインと CUDA プラグインが混在するチェーンでは、
   コアが境界で D2H/H2D 転送を自動挿入する。プラグイン側は自分のメモリ種別だけ見ればよい。
4. **動画デコードのフォールバック**: NVDEC が無ければ ffmpeg ソフトウェアデコードに自動切替。
   Source プラグインも同じ caps 機構を使う。

```c
typedef enum { PS_CAP_CPU = 1, PS_CAP_CUDA = 2 } psCaps;
// psProcessorV1 に uint32_t caps; を追加。CPU 実装は必須、CUDA 実装は加点。
```

## プラグインの2層構造

| 層 | 用途 | 速度 |
|---|---|---|
| **C ABI DLL** | 本番パイプライン, CUDA カーネル | ネイティブ |
| **埋め込み Python (pybind11)** | プロトタイピング, 使い捨て解析 | numpy 速度 |

Python 層は PixelScope の JS API と同じ思想:

```python
# ~/.viewer/plugins/my_sharpen.py
import viewer, numpy as np

@viewer.processor(name="my_sharpen", params={"amount": viewer.slider(0.0, 4.0, 1.0)})
def sharpen(frame: np.ndarray, amount: float) -> np.ndarray:
    ...
```

試作は Python で書き、確定したら C++/CUDA に落として同じ枠に差し直す。**契約が同じなので UI 側は無変更。**

## Web フロントエンド接続 (PixelScope の生存戦略)

コアに WebSocket サーバ(任意起動)を持たせ、Frame 契約をそのまま流す:

- pixelscope.html は「リモートフロントエンド」として接続できる (閲覧・軽い操作)。
- 会議で画面を配る、リモートの計測マシンの結果を手元で見る、といった Web の配布力を捨てない。
- プロトコルは Frame 記述子 JSON + バイナリペイロードのみ。UI 状態は各フロントエンドが持つ。

## Plugin ABI v1 (実装済み)

`include/ps/ps_plugin.h` が契約のすべて。実装済みの3種:

| 種別 | 形 | 実例 |
|---|---|---|
| **Display (描画系)** | 256エントリ RGB LUT を登録時に1回生成 (ホットパスにプラグイン呼び出しなし。将来 GPU テクスチャに直結) | `display_falsecolor` (viridis) |
| **Analyzer (解析系)** | `analyze(frame, roi, sink)` — key/value を emit するコールバック方式 (JSON パーサ不要)。ROI 対応 | `analyzer_stats` |
| **Processor** | `process(in, out, host)` — 出力メモリは**必ず** `host->frame_alloc` で確保、解放はホスト (CRT 境界対策) | `proc_demosaic` (Bayer のみ、Quad は明示エラー) |

- 読み込み: 起動時に `<exe_dir>/plugins/` と `<exe_dir>/../plugins/` から `*.dll` / `*.so` / `*.dylib` をスキャン。ホットリロードは v2。
- 検証: ABI バージョン不一致・CPU 実装なし・重複名は登録拒否 (警告トースト、クラッシュさせない)。
- Loader / Source は enum 値 4/5 を予約済み (v2)。
- メモリ規則: **ABI を越えるピクセルメモリは frame_alloc/frame_free のみ。文字列は emit 中にホストがコピー。記述子の文字列はプロセス生存期間。**

## 機能追加の建付け (governance)

新機能は次の3層のどこに入るかをまず決める。迷ったら L3。

| 層 | 中身 | 追加基準 |
|---|---|---|
| **L1 コア** | 表示・操作・アノテーション・セッション・入出力 | 全ユーザーが毎回触るものだけ。最も抑制的に |
| **L2 ホストサービス** | 描画プリミティブ (ヒストグラム、折れ線プロット、波形) と UI 枠 | プラグインで表現できない「見せ方」だけ |
| **L3 プラグイン** | 測定・処理のすべて | **デフォルトの行き先**。Frame を受けて数値/系列/Frame を返すものは必ずここ |

**出力の型は2種類に固定**: 数値 → ROI×指標の比較グリッド、曲線 → L2 プロット (ABI v2)。
アナライザごとの専用 UI は作らない。

### 命名・分類規約 (ABI 変更なし、v1 から適用)

- アナライザ名は `"category/name"` 形式。ホストが `/` で分割し、Analysis パネルは
  カテゴリ → 測定の2段選択、Measure メニューはカテゴリ別グルーピングになる
- 規格由来の測定は規格番号をカテゴリに: `iso12233/e-sfr`、`emva1288/...`
- 近似実装は名前で明示 (例: `uniformity/prnu-fpn` は単一フレーム法を method 行で明記)
- 各アナライザは `method` 行 (手法と制約の1行) を必ず emit する
- 1プラグイン1節のドキュメントを `docs/analyzers.md` に置く

### ABI v2 計画 (v1 は永久サポート)

1. **emit_series(name, x[], y[], n)** — 曲線を返す口。ホストの折れ線プロット部品が描画 **(済)**
2. **params_schema 有効化** — 測定パラメータを JSON Schema で宣言 → ホストが UI 自動生成 (未)
3. **description フィールド** — Measure メニューの前提条件ヒントをプラグイン側が宣言 **(済)**
4. **マルチフレーム analyze_seq** — EMVA 1288 用。フェーズ2 (連番) と同時に導入 (未)

実装状態: ホスト ABI = 2 (`register_analyzer2`、旧 reserved スロット使用で v1 プラグインと
バイナリ互換)。v1 構造体の abi_version は構造体自身の版 (=1) を入れる規約。
`iso12233/e-sfr` が v2 の参照実装。

## リポジトリ構成 (予定)

```
viewer/
  core/            # C++ コア: ウィンドウ, GL 表示, パイプライン実行, プラグイン管理
  plugins/         # 同梱プラグイン (npy, raw/yuv, ffmpeg, nvdec, metrics)
  include/ps/      # 公開 C ABI ヘッダ (これが契約のすべて)
  python/          # 埋め込み Python ランタイム + デコレータ API
  web/             # pixelscope.html (リモートUI / 仕様リファレンス)
  tools/           # 旧 PySide6 版 (オフライン解析・バッチ用に維持)
```

## 実装フェーズ

1. **骨格**: GLFW + ImGui + float テクスチャ表示。PixelScope のシェーダ (EV/γ/レンジ/差分) を GLSL 移植。npy/RAW Loader。
2. **連番/再生**: 先読みリングバッファ, トランスポート, A/B 同期再生。
3. **動画/CUDA**: ffmpeg → NVDEC + CUDA-GL interop でゼロコピー表示。
4. **プラグイン公開**: C ABI 凍結, DLL ホットリロード, pybind11 埋め込み, params_schema → ImGui 自動 UI。
5. **Web ブリッジ**: WebSocket + pixelscope.html 接続。

## ツールチェーン

VS Build Tools 2022 (MSVC) / CMake / vcpkg (glfw3, imgui[docking], implot, ffmpeg) / CUDA Toolkit 12.x。
GPU: RTX 3090 24GB (確認済, driver 576.02)。
