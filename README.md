# viewer

**カメラ画質評価のための画像ビューア兼測定ツール** — `.npy` / raw Bayer の stack を開いて、見て、測る。

[![build](https://github.com/klimemam/viewer/actions/workflows/build.yml/badge.svg)](https://github.com/klimemam/viewer/actions)
Windows / Linux / macOS ・ C++17 + Dear ImGui + GLFW ・ GPU 不要 ・ 単体バイナリ + プラグイン

センサから出てきた測光値を、変換も中間ファイルも挟まずにそのまま開いて測るための道具です。
表示用に量子化された画像ではなく、**測定対象としての画素**を扱います。

想定している使い方:

- 同一条件で撮った連番フレームを **stack** として開き、**時間ノイズ σ_t** と
  **固定パターンノイズ σ_s** を分離する
- 平坦画像から **PRNU / シェーディング / 行・列 FPN** を見る
- 照度や露光を振った測定を **series (系列)** としてまとめ、**リニアリティ**と
  **PTC** (K [DN/e-]、読み出しノイズ) を出す
- 計算機に置いたままの巨大なデータを **ssh 越し**に開く。ウィンドウは手元に出て、
  流れるのは見えている領域と測定結果の数値だけ
- 数値は Bayer の **R/Gr/Gb/B を混ぜずに**、単位付きで出す

---

## ビルド

必要なもの: **CMake 3.21 以上**、**C++17 コンパイラ**、OpenGL。
GLFW / Dear ImGui / miniz / portable-file-dialogs は FetchContent が自動取得するので
事前準備は不要です (初回ビルドのみネットワークが要ります)。

### MSVC (Visual Studio 2022)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

`build\Release\viewer.exe` と `build\plugins\` ができます。

### MinGW + Ninja

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`build\viewer.exe` と `build\plugins\` ができます。

Linux / macOS も同じ2コマンドです。Linux では GLFW のために
`xorg-dev libgl1-mesa-dev libwayland-dev libxkbcommon-dev wayland-protocols` が要ります。

できるもの: **`viewer`** (GUI 本体)、**`viewer-serve`** (ssh 越しに動く peer。GL に
一切リンクしない)、`mkicon` (アイコン生成)、そして `plugins/` 以下の7つの共有ライブラリ。

---

## 開いてみる

```
viewer                                  # 空で起動して File > Open
viewer frame_0000.npy                   # 1枚開く (連番があれば stack にするか聞く)
viewer 10lx/                            # フォルダ以下の連番を stack ごとに開く
viewer --sequence always 10lx/          # 聞かずに stack として開く
viewer ssh://user@host/data/10lx        # 別マシンを見る
```

ヘッダの無い raw は形を教えます。

```
viewer --raw-dtype u16 --raw-size 1920x1080 --raw-interp bayer \
       --bayer-pattern RGGB dark_0000.raw
```

`.npy` が 1ch で実はモザイクだった場合は `--cfa bayer` を付けるか、開いた後に
Inspector の **Interpret** で切り替えます。オプションの全体は `viewer --help` が正典です。

---

## 4つの層 — frame / stack / series / batch

語彙は [docs/terminology.md](docs/terminology.md) が正典で、
`frame ⊂ stack ⊂ series ⊂ batch` の包含は厳密です。

| 層 | 定義 | そこで意味を持つもの |
|---|---|---|
| **frame** | 画素の入った1枚。測定の最小単位 | ROI 統計、プロファイル、アナライザ |
| **stack** | 同一条件で撮った frame の並び。**時間軸**を持つ | **σ_t / σ_s の分離**、per-frame 表 |
| **series (系列)** | **条件を1つ振った** stack の並び。**パラメータ軸**を持つ | **リニアリティ / PTC のフィット** |
| **batch (塊)** | 開いたものを人が管理する入れ物。構造の主張はしない | まとめて閉じる、名前を付ける |

途中の層は省略できます (単発 frame は stack を経ずに batch へ直接ぶら下がれます)。
**series は自動では作られません** — picker の「掃引として開く」、Files で stack を
選んで「Group as series」、または Linearity パネルで作ります。

---

## 主な機能

**表示**

- ズーム / パン、ピクセルグリッド、ルーラ、座標と画素値のライブ読み取り
- 表示レンジ (black/white) を frame 単位 / stack 単位 / 全体で共有
- 表示ガンマ 1.0 / 2.2、1ch 画像へのカラーマップ (display プラグイン)
- CFA の解釈 (none / Bayer / Quad Bayer) とパターン (RGGB/BGGR/GRBG/GBRG) を
  読み込み後にも変更可能
- ドッキング式パネル: Files / Browse / Inspector / ROIs / Analysis / Histogram /
  Temporal / Projection (H/V) / Linearity / Messages。Browse は複数枚開けます

**測定**

| 何を | どこで | 出るもの |
|---|---|---|
| 時間ノイズ / 固定パターン | **Temporal** (stack 単位) | `sigma_t`、`sigma_fpn`、二乗和の `sigma_tot`。CFA プレーン別 |
| フレーム毎リニアリティ | Temporal 内 `Linearity (frame axis)` | 傾き a [DN/単位]、オフセット b [DN]、R²、LE max [%] |
| 系列リニアリティ / PTC | **Linearity** (series 単位) | 感度 [DN/単位]、オフセット [DN]、R²、LE max [%]、K [DN/e-]、読み出しノイズ [DN]、SNR カーブ |
| ROI 統計 | **ROIs** | mean / std / min / max / n を ROI 毎・プレーン毎に |
| 行・列プロファイル | **Projection** | H/V 投影 (mean/max/min) と `sigma_row` / `sigma_col` / peak-to-peak |
| ヒストグラム | **Histogram** | 256 bin、log 表示、CFA プレーン選択、A/B 重ね |
| プラグイン測定 | **Measure** メニュー / **Analysis** | 下記の7つ。ROI × 指標のグリッドと曲線 |

リニアリティのフィット窓と手法は EMVA **スタイル**です
(`windowed OLS (3.1-style)` と `relative-weighted LS (rev4-style)` を `fit method` で選択)。
**EMVA 1288 準拠を主張するものではありません** — 画面上のラベルも一貫して "-style" と書きます。

**比較 (compare)**

- A (現在の画像) と B (`Shift+B` で固定) を `\` または `C` で
  off → wipe → side by side → difference (A−B) → blink と切り替え
- side by side に限り **追加スロット C…P (最大14)** をタイル表示。Files の行の
  右クリックから割り当てます
- `B follows A's frame number` で stack 間をフレーム番号を保って比較。相手に同じ
  番号が無ければ diverged として明示されます
- 表示レンジの関係を「各自 / B は A に合わせる / 両者の和集合」から選択
- Histogram / Projection / Temporal が A/B/スロットを並べて出します

**取り出す**

- `Save image as PNG...` (表示レンジ・ガンマ・カラーマップを反映した dot by dot)
- ROI を stack 全フレームにわたって並べる **montage** (横 / 縦、フレーム毎レンジ)
- 別 stack から規則で frame を選んで新しい stack を作る **derive**
  (ファイル名一致 / フレーム番号一致 / 範囲 / 手選択)
- Temporal の統一エクスポート (TSV / CSV)、Analysis の `Copy table (TSV)` と
  `Export curves (CSV)`、Projection の統計表
- セッション (`.vsession`): 何をどう開いてどこを見ていたかの再現レシピ

---

## 対応フォーマット

**読める**

| 形式 | 備考 |
|---|---|
| `.npy` | `u1 i1 b1 u2 i2 u4 i4 f4 f8`、ビッグエンディアン、fortran order に対応。先頭軸がフレーム軸なら 1 stack |
| `.npz` | zip 内の各 `.npy` メンバを1枚ずつ (stored / deflate) |
| `.bin` `.raw` `.yuv` `.dat` `.rggb` | ヘッダ無し raw。dtype (u8/u16/f32/f64)、解釈 (gray/rgb/bgr/rgba/bgra/bayer/quad-bayer)、寸法、オフセット、クロップを指定 |
| `.vsession` | 保存したセッション |

**書ける**: PNG (表示のとおり)、CSV / TSV (測定結果)、`.vsession`。

PNG は書き出し専用で、PNG/JPEG/TIFF の**読み込みはありません**。

---

## リモート (ssh)

計算機に置いたデータを、手元のウィンドウで開いて測ります。

```
viewer ssh://user@host/data/10lx/frame_0000.npy   # ファイルを開く
viewer ssh://user@host/~/data                     # そこを Browse パネルで見る
viewer ssh://user@host                            # ホームから見る
```

- 認証は ssh に任せます (`BatchMode=yes` = 公開鍵のみ)。待ち受けポートもデーモンも
  増えません — ssh の stdin/stdout がそのまま通信路です
- 相手側で動くのは `viewer-serve` で、**初回接続時に `~/.viewer/viewer-serve` へ
  自動導入されます**
- 流れるのは **見えている領域を間引いたもの**だけで、その後バックグラウンドでフル
  解像度に差し替わります。届いた frame はローカルのものと区別なく扱えます
- **測定はサーバ側で走らせられます**。300枚の stack の σ_t を、1枚も手元に持って
  こずに出せます (Browse 上のまだ開いていない group に対しても)
- `File > Browse Folder (Local)...` は同じ仕組みを手元のマシンに使います
  (`local://`)。読み込まずに一覧・プレビュー・統計まで見られます

詳細は [docs/remote.md](docs/remote.md)、構成の選び方は [docs/startup.md](docs/startup.md)。

---

## プラグイン

測定は C ABI の共有ライブラリとして足せます。契約は
[include/ps/ps_plugin.h](include/ps/ps_plugin.h) 1枚、公開シンボルは
`psRegisterPlugins` ひとつ、現行 ABI は **v2** です。起動時に実行ファイル隣の
`plugins/` を走査して読み込みます (Windows は `.dll`、macOS は `.so`/`.dylib`、他は `.so`)。

| 名前 | 種別 | 何を出すか |
|---|---|---|
| `stats/moments` | analyzer | mean/var/std/min/max/p1/p50/p99/entropy と有限値の割合 |
| `noise/floor` | analyzer | 16×16 タイル std の中央値によるノイズフロアと `snr_db` |
| `uniformity/prnu-fpn` | analyzer | `prnu_pct`、`shading_pct`、`row_fpn_pct`、`col_fpn_pct` |
| `sharpness/gradient` | analyzer | `varlap`、`tenengrad`、`grad_mean` |
| `iso12233/e-sfr` | analyzer | 傾斜エッジ SFR 曲線と MTF50 / MTF20 / SFR@Nyquist / エッジ角 |
| `viridis` | display | 1ch 画像用の 256 段カラーマップ LUT |
| `demosaic (bilinear)` | processor | Bayer → RGB 3ch (Quad Bayer は非対応) |

analyzer は ROI 対応で、複数 ROI があれば「ROI × 指標」のグリッドになります。
モザイク入力は R/Gr/Gb/B を分けたまま集計します。前提条件つきの一覧は
[docs/analyzers.md](docs/analyzers.md)。

---

## 設定ファイル

Windows は `%APPDATA%\viewer\`、それ以外は `$HOME/.config/viewer/`。

| ファイル | 中身 |
|---|---|
| `prefs.txt` | 振る舞いの設定 (テーマ、操作、メモリ予算、Browse の表示形、ブックマーク/履歴)。行志向のテキスト |
| `layout.ini` | Dear ImGui のパネル配置 |
| `autosave.vsession`, `autosave-2.vsession`, … | セッションの自動保存。インスタンス毎に lock で枠を取るので、複数ウィンドウでも互いに上書きしません |

---

## セルフテスト

GUI を出さずに走り、stderr に結果を出して終了コードを返します (0 = pass)。
フィクスチャは `python tools/gen_testdata.py` で作れます。

```
viewer --frame-lin-selftest                       # フレーム毎リニアリティ (合成データ、引数不要)
viewer --verify-selftest tools/testdata/multi     # 横断的な不変条件 (V1-V18)
viewer --abstats-selftest tools/testdata/multi    # A/B 統計キャッシュ
viewer --browse-selftest tools/testdata/rb        # Browse パネルとローカル peer
```

全部で22個あり、`--help` に載っているのはそのうち8個です。一覧と各々が何を守って
いるかは [ARCHITECTURE.md](ARCHITECTURE.md) にあります。

引数を取るものは**フィクスチャの形に要求があります** (stack がいくつ要る、
どういう連番になっている必要がある、など)。足りなければ何が足りないかを言って
終了するので、そのメッセージに従ってディレクトリを選んでください。

描画性能の確認は `viewer --bench 120 <files>` (オフスクリーンで N フレーム描いて
フレーム時間統計を出して終了)。

---

## ドキュメント

| ファイル | 内容 | 位置づけ |
|---|---|---|
| [docs/terminology.md](docs/terminology.md) | frame/stack/series/batch の定義、操作マトリクス、実装上の不変条件 | **正典** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | コードを変える人向けの構造説明 | 現行 |
| [docs/manual.md](docs/manual.md) | 画面ごとの操作マニュアル | 現行 |
| [docs/analyzers.md](docs/analyzers.md) | 測定プラグインの一覧と各々の前提条件 | 現行 |
| [docs/remote.md](docs/remote.md) | `ssh://` の設計とプロトコル仕様 | 現行 (§8 の制限事項に古い記述あり) |
| [docs/startup.md](docs/startup.md) | ローカル / リモートの起動構成 | 現行 |
| [docs/stats-taxonomy.md](docs/stats-taxonomy.md) | 「基本 stats」と「追加 measurement」の境界の判定基準 | 現行 |
| [docs/picker-ux.md](docs/picker-ux.md) | Select sequences ダイアログの設計 | 現行 |
| [docs/export-design.md](docs/export-design.md) | Temporal エクスポートの仕様 | 現行 |
| [docs/tasks.csv](docs/tasks.csv) | タスクボード (対応済み/進行中/残課題/暫定/レビュー) | 現行 |
| [docs/series-plan.md](docs/series-plan.md), [docs/layers-plan.md](docs/layers-plan.md) | 層モデルを実装に落とす計画 | 計画 |
| [docs/browse-inventory.md](docs/browse-inventory.md) ほか `browse-*.md` (4件) | Browse パネルの棚卸しと上部領域の設計 | 設計検討 |
| [docs/measure-ux.md](docs/measure-ux.md), [docs/ab-stats-plan.md](docs/ab-stats-plan.md) | Measure 体験 / A/B 統計パネルの設計 | 設計検討 |
| [docs/reference-design.md](docs/reference-design.md), [docs/watch-design.md](docs/watch-design.md) | frame の共有参照化 / ファイル変化の監視 | **未実装の設計** |
| [docs/compare-n.md](docs/compare-n.md), [docs/flat-field-stats.md](docs/flat-field-stats.md), [docs/media-support.md](docs/media-support.md) | N-way compare / 平坦画像統計 / OpenEXR・動画 | **議論用。実装しない** |
| [docs/verify-functional.md](docs/verify-functional.md), [docs/verify-ui.md](docs/verify-ui.md) | 検証マトリクス | 検証記録 |
| [docs/todo-open.md](docs/todo-open.md), [docs/review-new-code.md](docs/review-new-code.md) | 未着手の課題 / コードレビュー記録 | 記録 |

---

## 依存

FetchContent が取得します: GLFW 3.4、Dear ImGui v1.91.8-docking、miniz 3.0.2、
portable-file-dialogs (コミット固定)。
</content>
