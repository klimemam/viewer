# viewer — 画質設計のための画像ビューワ

エンジニアリングデータ (`.npy` / `.bin` / `.raw`) を開いて、画素値を確認・比較するためのネイティブビューワ。
C++ / Dear ImGui / OpenGL 製。**CUDA・GPU なしでも動作します**(表示は OpenGL、処理は CPU)。

設計方針は [ARCHITECTURE.md](ARCHITECTURE.md) を参照
(極小の Frame 契約 + すべてプラグイン、CPU フォールバック必須、将来: CUDA / NVDEC / C++ パイプライン接続)。

## v0.1 の機能

- **ファイル読み込み**
  - `.npy` — u8 / i8 / u16 / i16 / u32 / i32 / f32 / f64 / bool。
    `(H,W)`・`(H,W,C)`・`(C,H,W)` 対応(CHW は自動変換)。バッチ次元は先頭 `[0]` を表示。
    ビッグエンディアン・Fortran order 対応。
  - `.bin` / `.raw` — **2軸指定**: 画素フォーマット(u8/u16/f32/f64 × エンディアン)×
    解釈(Gray/RGB/BGR/RGBA/BGRA/**Bayer/Quad Bayer**(RGGB/BGGR/GRBG/GBRG))。
    任意の組み合わせが可能(例: RGGB float32)。ファイルサイズからの**サイズ候補自動推定**、
    ファイル名の `..._640x480_...` / `rggb` / `quad` / `f32` 表記の自動認識、オフセット指定。
  - **開いた後の解釈変更** — 1ch 画像は右パネルの Interpret で Gray ⇔ Bayer ⇔ Quad Bayer を
    即時切替(npy でも可)。raw 由来の画像は「Reinterpret raw...」で画素フォーマットから
    読み直し(表示位置・ズームは維持)。
  - **ROI 切り出し(動的)** — 読み込み時の crop 指定(`--raw-crop x,y,WxH` / ダイアログ)に加え、
    開いた後も **選択中の ROI から「Crop to selected ROI」で即切り出し**、「Restore full」で復元。
    ファイルメニューから開き直す必要なし。CFA 画像は crop 原点をパターン周期に自動スナップ。
  - Bayer 画像はホバーで **画素の CFA チャンネル (R/Gr/Gb/B) を表示**、
    「Colorize CFA pattern」でパターンを色付け表示。
  - ウィンドウへのドラッグ&ドロップ、コマンドライン引数 (`viewer.exe a.npy b.raw`) 対応。
- **ピクセル検査** — マウスホバーで座標と画素値(raw 値と正規化値)を右パネル+ステータスバーに表示。
  ズーム 2 倍以上でホバー画素をハイライト。
- **ツールモード** — `V` Navigate(ドラッグ=パン)/ `R` ROI / `P` ピン。
  どのツールでも **中ボタンドラッグ / Space+ドラッグ = パン**、**Ctrl+ホイール = ズーム**、
  ホイール = 縦パン、Shift+ホイール = 横パン(View メニューでホイール直接ズームに変更可)。
- **複数 ROI** — R ツールでドラッグ作成、掴んで移動、角ハンドルでリサイズ。
  解析プラグインが**全 ROI を列とする比較テーブル**を出力(パッチ間比較が1画面で見える)。
- **複数ピン (POI)** — P ツール(または Navigate で Ctrl+クリック)。右パネルに各ピンの
  **ch 毎の画素値**を一覧表示(Bayer は CFA チャンネル付き)。
- アノテーション(ROI/ピン)は一覧パネルで選択・改名・表示切替・削除(`Del`)、
  **セッションにも保存**される。
- **プラグイン** — `plugins/` の共有ライブラリを起動時に読み込み(C ABI, [include/ps/ps_plugin.h](include/ps/ps_plugin.h))。
  同梱: `viridis` カラーマップ(描画系)、`stats` ROI 対応統計(解析系)、
  `demosaic (bilinear)`(Process メニュー、Bayer→RGB)。
- **座標ルーラー** — 画像の上端 (X) と左端 (Y) に目盛り。ズームに応じて刻みが 1/2/5×10ⁿ で自動調整。
  ホバー位置のマーカー付き。
- **表示** — ホイールでカーソル中心ズーム (1/512〜256 倍)、ドラッグでパン、
  等倍以上は NEAREST(画素がそのまま見える)/縮小時は LINEAR。
  黒点/白点によるレンジ正規化 (Auto / 0–1 / 0–255 / 0–65535)、表示ガンマ 1.0 / 2.2、
  ズーム 8 倍以上でピクセルグリッド。

### ショートカット

| キー | 動作 |
| --- | --- |
| `Ctrl/Cmd+O` / `O` | ファイルを開く |
| `Ctrl/Cmd+W` | 表示中の画像を閉じる |
| `F` / ダブルクリック | フィット表示 |
| `1` | 等倍 (100%) |
| `+` / `-` | ズームイン / アウト |
| `G` | ピクセルグリッド切替 |
| `H` | ショートカット一覧 |
| ホイール | ズーム(カーソル中心) |
| 左ドラッグ | パン |

メニューバー (File / View / Help) からも同じ操作ができます。

## セッション (状態の保存と復元)

`File > Save Session...`(`Ctrl/Cmd+S`)で、**開いている画像・ズーム・表示位置・
黒点/白点レンジ・ガンマ・グリッド状態**を `.vsession` ファイル(テキスト)に保存できます。
復元は `File > Open...` / ドラッグ&ドロップ / `--session` のいずれでも可能です。
「昨日見ていたあの座標のあの倍率」をそのまま再現できます。

## コマンドライン

```
viewer [options] [files...]

  --session <file.vsession>   保存したセッションを復元
  --raw-dtype <t>             1サンプルの格納形式 (u8|u16|f32|f64)
  --raw-interp <i>            サンプルの解釈 (gray|rgb|bgr|rgba|bgra|bayer|quad-bayer)
  --raw-format <fmt>          旧式の複合名 (gray8|...|bayer16) も引き続き使用可
  --raw-size <WxH>            raw のサイズ (例 1920x1080)
  --raw-offset <bytes>        先頭オフセット
  --big-endian                エンディアン指定 (デフォルトはリトル)
  --bayer-pattern <p>         RGGB|BGGR|GRBG|GBRG
  --quad-bayer                Quad Bayer として解釈
  --zoom <z>                  起動時ズーム (1 = 等倍)
  --center <x,y>              起動時の表示中心 (画像座標)
```

例:
```
viewer data.npy                                          # npy を開く
viewer --raw-format bayer16 --raw-size 1920x1080 \
       --bayer-pattern rggb sensor_dump.raw              # Bayer raw を直接開く
viewer --session yesterday.vsession                      # 昨日の状態を完全復元
viewer data.npy --zoom 8 --center 512,384                # 特定座標を拡大した状態で起動
```

## ビルド

必要なもの: **Visual Studio 2022 (C++ ワークロード) + CMake 3.21+**。
依存ライブラリ (GLFW, Dear ImGui) は CMake FetchContent が自動取得します (vcpkg 不要)。

```
cmake -S . -B build
cmake --build build --config Release
build\Release\viewer.exe
```

### クラウドビルド (ローカルにツールチェーンがない場合)

このリポジトリは push 時に GitHub Actions (`windows-latest`) でビルドされます。
[Actions](../../actions) の最新 run の **Artifacts → `viewer-win64`** から `viewer.exe` をダウンロードできます。

## テストデータ

```
python tools/gen_testdata.py
```

`tools/testdata/` にゾーンプレート (f32)、グラデーション (u16)、RGB (u8)、CHW、
ビッグエンディアン npy、raw (gray16 / rgb8) が生成されます。

## リポジトリ構成

| パス | 内容 |
| --- | --- |
| `core/main.cpp` | ネイティブビューワ本体 (v0.1) |
| `CMakeLists.txt` | ビルド定義 (FetchContent で依存取得) |
| `ARCHITECTURE.md` | プラグイン設計・CPU フォールバック方針・ロードマップ |
| `pixelscope.html` | Web 版プロトタイプ。UI/UX リファレンス兼、将来のリモートフロントエンド |
| `main.py` ほか | 旧 PySide6 版 (オフライン解析・バッチ用に維持) |
| `tools/gen_testdata.py` | テストデータ生成 |

## ロードマップ (ARCHITECTURE.md より)

1. ~~骨格: 表示 + npy/raw + ルーラー + ピクセル検査~~ ← **v0.1 (いまここ)**
2. 連番/再生: 先読みリングバッファ、トランスポート、A/B 同期再生
3. 動画: ffmpeg → NVDEC + CUDA-GL interop (無い環境ではソフトデコードに自動フォールバック)
4. プラグイン公開: C ABI 凍結、DLL ホットリロード、pybind11 埋め込み
5. Web ブリッジ: WebSocket + pixelscope.html 接続
