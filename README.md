# viewer

**画像データを開いて、見て、測る道具。** そして**足せる**道具です。

[![build](https://github.com/klimemam/viewer/actions/workflows/build.yml/badge.svg)](https://github.com/klimemam/viewer/actions)
Windows / Linux / macOS ・ C++17 + Dear ImGui + GLFW ・ GPU 不要 ・ 単体バイナリ

`.npy` / `.npz` / ヘッダ無し raw / ベンダ RAW / TIFF / EXR などを、変換も中間ファイルも
挟まずにそのまま開きます。表示用に丸められた画像ではなく、**元の数値**を扱います。
連番は1つの **stack** として開き、フレーム間の統計や比較がそのまま効きます。

足せる場所が3つあります:

- **プラグイン** — 測定を C の共有ライブラリで足す (同梱7本。frame でも stack でも)
- **リーダ** — 読めない形式を Python の関数1つで読ませる
- **リモート** — 計算機側に peer を置き、ssh 越しに開く。回線を流れるのは
  見えている領域と測定結果だけ

---

## インストール

ビルド済みバイナリが `binaries` ブランチにあります。ビルド環境は要りません。

```
git clone -b binaries --single-branch https://github.com/klimemam/viewer.git viewer-bin
```

以後の更新は `update.cmd` (Windows) / `./update.sh` (Linux・macOS)。
このブランチは公開のたびに履歴を差し替えるので、**`git pull` では更新できません**。

| | |
|---|---|
| `win64/viewer.exe` | GUI 本体 |
| `win64/viewer-serve.exe` | ヘッドレスの peer (計算機側に置くもの) |
| `win64/plugins/` | 測定プラグイン。**exe の隣に置く** |
| `linux-x64/` `macos-arm64/` | 同じ一式 |

デスクトップのアイコンから起動したいときは、同梱のスクリプトを1回実行します。

```
win64\install_shortcut.cmd                    Windows
./linux-x64/install_shortcut.sh               Linux / macOS
```

`--host user@server` (Windows は `-RemoteHost`) を付けると、**起動時にその計算機へ
つなぐショートカット**が別に作られます。

**計算機側**には `viewer-serve` だけを置きます。GL も X11 もリンクしていないので、
画面の無いマシンでも動きます。

---

## 用語 — frame / stack / series / AnalysisSet / batch

この5語がこの道具の骨格で、パネルもファイルも測定もこの語彙で動きます。
構造の順序は `frame ≼ stack ≼ series` です。`≼` は集合包含ではなく、存在する
データ層がこの順序を守るという意味です。各層は単独でも存在でき、途中の stack は
省略できますが、逆転はできません。
batch はこれらを管理する単位 (`managed-by`) で、構造の層ではありません。
AnalysisSet は frame / stack / series を役割付きで参照 (`role-ref`) します。同じ dark を
複数の set から参照できないと、「1回撮った dark を全解析で使う」が成立しないからです。

| 語 | 何か | 例 |
|---|---|---|
| **frame** | 画面に見えている最小単位。1枚 | `dark_0007.npy` |
| **stack** | **同一条件で繰り返し**撮った frame の集まり | `dark_0000‥0479` (480枚) |
| **series** | **条件を振った** stack / frame の並び。振った量と単位を持つ | 露光 1,2,5,10,20 ms の5つのメンバ |
| **AnalysisSet** | 解析の**入力の役割**を束ねたもの。データが dark 「である」のではなく、set の中で dark を**演じる** | `{"image": 10lx, "dark": dark}` |
| **batch** | 一緒に開いたもの。構造の主張はしない | あるフォルダを開いた結果 |

この区別が効くのはここです:

- **時間ノイズ σ_t は stack の性質**です。条件の違うものを混ぜて計算すると、
  「振った条件」を「ノイズ」として報告してしまいます。だから series では
  σ_t を条件またぎで計算しません
- **series の値 (露光・照度) と単位は series が持ちます**。stack にもアプリにも
  持たせません。単位が無ければフィットしません — 単位は推測しないからです
- **batch は構造を主張しません**。「一緒に開いた」だけなので、閉じる単位に使えます
- **役割は宣言です。推論しません** — フォルダ名や「level 0」から dark を推測して
  束ねることはしません

正典は [docs/terminology.md](docs/terminology.md) です (Close の意味論もそこ)。
解析モデルと4種の設計は [docs/analysis-layers.md](docs/analysis-layers.md)。

---

## 使い方

### 開く

```
viewer                                  そのまま起動して、あとは画面から
viewer frame_0000.npy                   1枚
viewer dark_0000.npy dark_0001.npy      連番は 1 stack にまとまる
viewer C:\capt\run42                    フォルダ (中身を選ぶ画面が出る)
viewer session.vsession                 保存したセッション
viewer ssh://user@host/data/run42       計算機の上のフォルダ
```

全オプションは `--help`。

### 見る

- **Image View** — 中央。ホイールで拡大、ドラッグで移動。値は [DN] のまま表示され、
  トーンマップはしません
- **Files** — batch の管理下に frame / stack / series がデータ順序どおり並び、
  AnalysisSet はそれらを役割参照する別ノードとして見えます
- **Browse** — ファイルを探すパネル。ローカルもリモートも同じ形で扱います。
  **クリックで選択、ダブルクリックで確定** (開く・降りる)
- **Inspector** — 座標・画素値・そのファイルがどう読まれたか

表示レンジ (black/white) は **stack ごとに揃える**のが既定です。フレームごとに
自動で合わせたいときは Range を `Auto per frame` に。

### 測る

| パネル | 何が出るか |
|---|---|
| **Histogram** | 分布。CFA なら R/Gr/Gb/B を**混ぜずに** |
| **Projection** | 行・列の平均と σ。行 FPN / 列 FPN が見えます |
| **Temporal** | stack のフレーム方向。**σ_t** と σ_fpn、フレームごとの平均の推移 |
| **ROIs** | 矩形と点。mean / σ / min / max、そして n。比較中は blink で追随、side-by-side で横並び |
| **Series Analysis** | series のフィット (旧 Linearity)。メンバは frame と stack の混在可 |
| **Set Analysis** | AnalysisSet の SetAnalyzer。**DSNU / PRNU** と、掃引からの分離フィット |
| **Analysis** | プラグインの測定結果 |

数値には**必ず量と単位**が付きます。画素値は保存形式が何であれ [DN] です。
部分的にしか読めていない stack は **n/N** と言います。

stack を1枚に畳むときは **mean と sum を選べます**。NaN が混ざっていれば警告し、
除外数と有効枚数を併記します (畳んだ結果は frame として series に入れられます)。
σ_fpn は時間残差 `−mean(σ_t²/n)` を引いた**補正済み**の量で、補正量とクランプを
申告します。**PRNU / DSNU の名は SetAnalyzer だけが名乗ります** — General の列は
`std / mean [%]` のような中立な名前です。

### 比べる

`\`（または `C`）で比較モードを巡回し、`Shift+\` で A/B を入れ替え、
`Shift+C` で C, D, … の比較スロットを表示します。比較中は既定で
**全比較枠の表示レンジを揃えます** (各画像を違う伸ばし方にすると比較できないため)。
`ESC` で一段ずつ外側に戻ります。

### 取り出す

- 表示のとおりの PNG (dot by dot)
- 測定結果の CSV / TSV。**出所** (どのビルドが・いつ・何を測ったか) が先頭に付きます
- セッション (`.vsession`) — 開いているもの・レンジ・レイアウトごと

---

## 読める形式

| 形式 | 備考 |
|---|---|
| `.npy` | `u1 i1 b1 u2 i2 u4 i4 f4 f8`、ビッグエンディアン、fortran order |
| `.npz` | zip 内の各メンバ。名前は `file.npz:key`。画像でないメンバは開かず、フレーム数と長さの合う1次元配列は**軸として使えます** ([docs/features/adapters/npz-design.md](docs/features/adapters/npz-design.md)) |
| `.bin` `.raw` `.yuv` `.dat` `.rggb` | ヘッダ無し。dtype・解釈 (gray/rgb/bayer/quad-bayer)・寸法・オフセット・クロップを指定 |
| `.png` `.jpg` | 8/16-bit。値は保存されたまま、スケールもトーンカーブも掛けません |
| `.tif` | 自前実装。8/16-bit 整数と 32-bit float、strip、LZW/Deflate/PackBits。**multi-page は stack** |
| `.exr` | OpenEXR。**layer は別々の文書**になります (page = stack と区別します) |
| `.y4m` | 実体は画像形式 (テキストヘッダ + 生プレーン) なので、フレーム列が stack になります |
| ベンダ RAW | NEF / ARW / ORF / CR3 / RW2 / DNG ほか。**CFA モザイクを数えたまま**1面で読み、パターン・黒/白レベル・ビット深度は**申告どおり運びます** |
| `.vsession` | 保存したセッション |

**RAW は「見せて、引かない」**: 黒レベル 0 と申告しながら画素が 80 から始まるファイルが
実在するので、宣言された値は note に載せるだけで**引き算はしません**。デモザイクも
ホワイトバランスも色行列もトーンカーブも掛けません。読めないもの (X-Trans、JPEG-XL 圧縮
DNG など) は**画素を展開する前に**理由を名指しして断ります。

**配列の形の読み方**:

判定は**次元数と、最後の軸だけ**です。先頭の軸の大きさは見ません。

| shape | 読み方 |
|---|---|
| `(H,W)` | 1枚・1ch |
| `(H,W,C)` C ≤ 4 | **1枚**・C ch (`(H,W,1)` はモノクロ1枚、`(H,W,3)` はカラー) |
| `(F,H,W)` | **F 枚の stack** (`(3,H,W)` も3枚) |
| `(F,H,W,C)` C ≤ 4 | F 枚 × C ch |
| 上記以外 | **読みません。**形と読める形を名指しして断ります |

**取り違えたときは Inspector で言い直せます** — 「read as / re-read as」に、
その配列の形から計算できる読み方だけが並びます。選んだ読み方は**そのファイルに
覚えられ**、セッションにも残ります。

CFA は `--cfa bayer --bayer-pattern RGGB` の**順**で書いてください
(`--bayer-pattern` を後ろに置くと無視されます)。

**書ける**: PNG (表示のとおり)、CSV / TSV、`.vsession`。

**Browse の一覧は、ローカルでは「この viewer が読めるか」、リモートでは「この peer が
その形式または宣言を扱えるか」を確認します**。対応範囲は protocol と形式表で判定します。
開けない行も消さずに残し、理由を表示します。

### 読めない形式を読ませる — リーダ

**リーダは、あなたが書く Python の関数1つ**です。viewer はそれにパスを渡し、
返ってきたものを開きます。

```python
def load(path):
    z = np.load(path)
    return Series([Stack(z["data"][i], name=f"{e:g}ms") for i, e in enumerate(z["exp"])],
                  conditions=Values(z["exp"], "exposure", "ms"))
```

返り値の**型がデータ層や関係を明示する**ので、形だけに意味を推測させません
(配列をそのまま返す互換経路もあります)。

```
File > Open With a Reader...        (Browse で読めないファイルをダブルクリックしても同じ)
[New reader...]                     テンプレートを書き出して、あなたのエディタで開く
[Load]                              走らせる。失敗すれば traceback がそのままパネルに出る
```

- 一度読めた **パスと関数の対応は憶えます**。次からは黙って同じリーダで開きます
- リーダのファイルを編集すると、**次に開くとき自動で走り直します**
- 結果はキャッシュするので、2回目は Python を起動しません
- Inspector がそのファイルを「native で読んだか、どのリーダで読んだか」を言い、
  `[change...]` `[edit]` `[native]` でいつでも変えられます

Python は `numpy` があるものを探して使います。無ければ**どれを試して何が足りなかったか**を
名指しで言います。仕様は [docs/features/adapters/input-adapters.md](docs/features/adapters/input-adapters.md)。

---

## リモート (ssh)

```
viewer ssh://user@host/data/10lx/frame_0000.npy    ファイルを開く
viewer ssh://user@host/~/data                      そこを Browse で見る
viewer ssh://user@host                             ホームから
```

- 認証は ssh に任せます (`BatchMode=yes` = 公開鍵のみ)。待ち受けポートも
  デーモンも増えません — ssh の stdin/stdout がそのまま通信路です
- **回線を流れるのは、見えている領域と測定結果だけ**です。σ_t のような
  stack 全体の測定は**計算機側で走り**、返るのは数値です
- 計算機に `viewer-serve` が無ければ、初回に置きにいきます

詳しくは [docs/features/remote/remote.md](docs/features/remote/remote.md)。

---

## プラグイン

`plugins/` に置いた共有ライブラリを起動時に読みます。ABI は
[include/ps/ps_plugin.h](include/ps/ps_plugin.h) の1ヘッダだけで、C で書けます。

| 種別 | すること | 同梱 |
|---|---|---|
| analyzer | 測る (結果は Analysis パネルへ) | stats / noise / prnu / sharpness / esfr |
| processor | 画素を作る | demosaic |
| display | 表示のしかたを足す | falsecolor |

**ABI は v3** です。v1 / v2 のプラグインはそのまま動きます (版欄が無いので版は空欄のまま
= 推測しません)。v3 で増えたのは:

- **層で型の付いた解析** — frame だけでなく **stack をそのまま受け取れます**。フレームは
  `get_frame` / `release_frame` で必要な分だけ引き、全部の常駐は約束しません
- **版と headline の宣言** — 結果の出所行が `名前 <版> (dll名)` になります
- **部分ロードの契約** — `min_frames` は**呼ぶ前に**ホストが裁く宣言、`frames`/`expected` は事実

analyzer は **peer 側でも同じものが走ります**。リモート実行は **name + 版の等値**で
照合し、食い違えば**両方の版を引用して断ります** — 黙ってローカルに振り替えることは
しません。仕様は [docs/reference/abi-v3.md](docs/reference/abi-v3.md)、書き方は [docs/reference/analyzers.md](docs/reference/analyzers.md)。

---

## 設定ファイル

| 置き場所 | 中身 |
|---|---|
| exe の隣 `prefs.txt` | 振る舞いの好み (既定のレンジ、キー、Browse の見せ方) |
| `%APPDATA%/viewer/layout.ini` (Linux・macOS は `~/.config/viewer/`) | パネルの配置 |
| 同 `autosave.vsession` | 落ちても失わないための自動保存 |

セッション形式はキーの**追加のみ**で拡張します。古いビルドは知らないキーを読み飛ばすので、
新しいセッションも開けます。

---

## セルフテスト

CMake に selftest として 55 本登録され、別に GL capability gate の `glprobe` が1本あります。
登録数と一覧の正典は [CMakeLists.txt](CMakeLists.txt) です。CI の3つの OS と手元は
同じ入口を使い、GL が無い環境では該当する検査を名指しで skip します。

```
tools/run_selftests.sh [build-dir]
```

GUI を実際に描いてクリックを注入するものも含みます (Browse のキー操作、比較、タイル表示)。
測定の不変条件 — **σ_t は stack の性質・CFA プレーンは個別系列で保つ・明示的な
`all` 集計は別測定と名乗る・部分ロードは n/N と言う** — は、この検査で守られています。
さらに、PRNU / DSNU などの定義済みの指標名が許可された表以外に現れていないか、
ローカルと peer が同じ数値を返すか、実行されなかったテストが黙って成功扱いに
なっていないかも検査します。

**走らなかったものは必ず名指しします** — skip も quarantine も、緑の中に埋もれません。

---

## ドキュメント

| | |
|---|---|
| [docs/terminology.md](docs/terminology.md) | データモデルの正典。Close の意味論も |
| [docs/guides/manual.md](docs/guides/manual.md) | 操作の手引き |
| [docs/analysis-layers.md](docs/analysis-layers.md) | AnalysisSet と解析の4種 (General / Specific / PreProcessor / SetAnalyzer) |
| [docs/features/analysis/flat-field-stats.md](docs/features/analysis/flat-field-stats.md) | σ_fpn・DSNU・PRNU の推定量と detrend |
| [docs/features/adapters/npz-design.md](docs/features/adapters/npz-design.md) | **他人が作った** `.npz` をどう読むか (メンバの分類・軸候補) |
| [docs/features/adapters/input-adapters.md](docs/features/adapters/input-adapters.md) | リーダの仕様。§4.11 は **viewer 自身が書く** npz 容器 |
| [docs/features/adapters/reader-analysisset.md](docs/features/adapters/reader-analysisset.md) | リーダから AnalysisSet を書く |
| [docs/features/remote/remote.md](docs/features/remote/remote.md) | ssh プロトコルと peer |
| [docs/reference/abi-v3.md](docs/reference/abi-v3.md) | プラグイン ABI v3 の寸法と契約 |
| [docs/reference/analyzers.md](docs/reference/analyzers.md) | プラグインの書き方 |
| [docs/features/analysis/stats-taxonomy.md](docs/features/analysis/stats-taxonomy.md) | どの統計が何の性質か |
| [docs/tasks.csv](docs/tasks.csv) | 課題表 (対応済み / 進行中 / 残課題 / 暫定 / 要レビュー) |
| [ARCHITECTURE.md](ARCHITECTURE.md) | データモデル・フレームループ・不変条件 |

---

## ソースからビルド

**MSVC (Visual Studio 2022)**

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

**MinGW + Ninja**

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

依存 (GLFW 3.4 / Dear ImGui 1.91.8-docking / miniz / portable-file-dialogs / OpenEXR +
Imath / LibRaw) は CMake が取ってきます。**初回はネットワークが要ります** — PNG・JPEG・
TIFF は自前と同梱で読むので依存を増やしませんが、EXR と RAW は外部ライブラリです。
`viewer` と `viewer-serve`、プラグイン、アイコンが同時に建ちます。

MinGW / WinLibs 同梱の CMake は CA ストアを持たないので、取得が証明書エラーになったら
CA バンドルを渡してください (環境変数の形は FetchContent のサブビルドに伝わりません):

```
cmake -S . -B build -G Ninja -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
```

テスト用の fixture は `python tools/gen_testdata.py` が生成します (numpy が要ります)。

---

## ライセンス

Apache License 2.0 ([LICENSE](LICENSE))。同梱・リンクする第三者コードの表示義務は
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) にまとめてあり、配布バイナリにも同梱します。
LibRaw は CDDL-1.0 を選択しています (単一静的 exe を CI が自動公開する形態に LGPL の
再リンク義務が合わないため)。
