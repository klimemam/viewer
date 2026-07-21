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
  - `.bin` / `.raw` — Gray 8/16bit・float32/64、RGB/BGR/RGBA/BGRA 8bit、RGB 16bit/float32。
    読み込みダイアログでファイルサイズからの**サイズ候補自動推定**、
    ファイル名の `..._640x480_...` 表記の自動認識、オフセット・エンディアン指定。
  - ウィンドウへのドラッグ&ドロップ、コマンドライン引数 (`viewer.exe a.npy b.raw`) 対応。
- **ピクセル検査** — マウスホバーで座標と画素値(raw 値と正規化値)を右パネル+ステータスバーに表示。
  ズーム 2 倍以上でホバー画素をハイライト。
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
