# 読めない形式をどう読むか — native の範囲とリーダ

**名前について (2026-08-03 決定)。** ユーザーが書く Python の関数を **リーダ
(reader)** と呼ぶ。UI もこの語で統一されている (`Open With a Reader...`、
Inspector の `reader native` / `reader <spec>`)。この文書は当初「入力アダプタ」
と呼んでいたが、機構の名前と、ユーザーが実際に書くものの名前は同じでよい —
**ユーザーが書くのは関数1つ**で、それ以外は viewer 側の都合だからである。
ファイル名 `input-adapters.md` は未マージのブランチ5本が参照しているため、
それらが着地してから `readers.md` に改める。

**この文書は 2026-08-03 に全面改訂した。** 初版は JSON manifest を契約にし、
そこへ「関数にする」「層の宣言を足す」「型を付ける」と追記を重ねた結果、
前の節と後の節が矛盾した (露光値を一方では `axis`、他方では `sweep` と書いていた)。
**仕様が自分と矛盾しているのは、仕様が無いより悪い。**
以下は追記の履歴ではなく、1つの仕様として読める形に書き直したもの。

議論の出発点 (ユーザー、2026-08-03):

> そもそも npy も (F,H,W,C) と類似以外は読めないし、色々なフォーマット対応を
> 考えると提案の内容だけだと弱い。何かドメイン特化な変換スクリプトを書くか、
> python のプログラムを書いておいてそれをかますかしたい。

> json だと学習コストがある。numpy か torch で、ある形で出力する関数を
> ユーザーが書く。inspector でその関数を指定する。一旦指定されたものは
> パス名と関数のペアを記憶しておいてそれを適用する。

> そもそも、adapter 無の場合の読み込みは決めちゃった方がよいと思う。
> 今の C の数で判定は複雑さを加速させる。対応フォーマットを明記して、
> それ以外の場合は input_adapter を使うことを明記したい。

---

## 1. なぜ推測を増やし続けるのが負け戦か

今の loader は `decodeNpyBuffer` の形の**推測**で成り立っている:

- 3次元 `(3,H,W)` は「3ch の画像」か「3枚の stack」か **決められない** —
  先頭が4以下ならチャンネル、という heuristic と `--npy-axis` の手動上書きで凌いでいる
- 4次元より深いものは**先頭を黙って取る**
- 1次元は「高さ1の画像」になる (docs/npz-design.md の欠陥)

形式が増えるたびにこの推測表が伸び、**推測が外れたときに黙って間違った絵を出す**。
測定器としては最悪の失敗の仕方で、しかも増え続ける。

**根本原因は「ファイルは自分が何であるかを言わない」こと。** 層 (frame ⊂ stack ⊂
series ⊂ batch)・軸・単位・CFA はドメインの知識であって、バイト列からは復元できない。

## 2. その仕組みは既にこのリポジトリにある

リモートは**別プロセスに stdio で喋る**構造で動いている (`core/remote.cpp` /
`core/serve.cpp` / `remote_proto.h`、VERSION=5)。ローカルの Browse すら
`local://` で同じ経路を通る。つまり:

> **「外部プロセスが読んで、正典の形で返す」配線は、実装済みで実運用中。**

アダプタはこの形をもう一度使うだけ。新しい機構ではない。

---

## 3. native の範囲 — 決め打ちにする

**判定は次元数と、最後の軸だけ。先頭の軸の大きさは見ない。**
「先頭が C ≤ 4 ならチャンネル」は、3枚の stack と 3ch の画像という
**実際によくある2つ**を取り違える。最後の軸で見ればその衝突は起きない
(幅が3ピクセルの画像は現実に無い)。**例外は明文化された1つだけにする。**

### 3.1 native が読む形 (`.npy` / `.npz` メンバ / raw)

**判定は次元数と、最後の軸だけ**を見る。**先頭の軸の大きさは見ない** —
「先頭が4以下ならチャンネル」が、3枚の stack と 3ch の画像を取り違える元だった。

| shape | 読み方 |
|---|---|
| `(H,W)` | 1枚・1ch (frame) |
| `(H,W,C)` C ≤ 4 | **1枚**・C ch (`(H,W,1)` はモノクロ1枚、`(H,W,3)` はカラー) |
| `(F,H,W)` | **F 枚の stack**・各1ch (`(3,H,W)` も3枚) |
| `(F,H,W,C)` C ≤ 4 | F 枚 × C ch |
| 上記以外 (1次元・スカラー・5次元以上・C > 4) | **読まない。** adapter を促す |

**例外は1つだけ、明文化されている**: **最後の軸が 4 以下ならチャンネル**。
それ以外の3次元は stack。これで実用上のカラーもモノクロの
`(H,W,1)` も読めたまま、**`(3,H,W)` は3枚の stack** になる
(以前は先頭の軸で判定していたので3chカラーと誤読されていた)。

犠牲になるのは「**幅が1〜4ピクセルの画像の stack**」だけで、現実には存在しない。
あっても §3.3 の言い直しで直せる。

**この規則は `core/serve.cpp` (peer) と同じ**。同じファイルがローカルとリモートで
違う読まれ方をするのは、この道具では欠陥として扱う。

### 3.2 読まなかったときに何を言うか

黙って諦めない。**形と、次にすることを言う**:

```
odd.npy: shape (4, 8, 8, 8, 2) is not a native form
  native reads (H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)
  [ choose a reader... ]        ← その場から adapter を指定できる
```

**stack に入れなかったときも同じ**。連番のグループ分けは**名前**だけで決まる
(`.raw` / `.bin` は形を名乗らず、300ファイルのヘッダを覗くのはフォルダを2度読む
ことになる)ので、形が判るのは**デコードした後**。そこで先頭フレームと形が違った
兄弟は、両方の形と規則を名乗って断る — 落とすのはその1ファイルだけで、
残りは stack になり、何枚中何枚かは Files パネルに残る:

```
shape_2.npy: 8x8 3ch and shape_1.npy is 8x8 1ch: a run of numbered files is a
stack, and a stack's frames are one shape - open it on its own
```

複数ページ TIFF が形の違うページを断る文 (§3.6.1) と同じ規則の、同じ言い方である。
`terminology.md` では stack は**時間軸**を持つ層 —— 形が揃っていない並びは stack
ではないので、σ_t も FPN 分離も per-frame 表も量にならない。逃げ道は最後の句が
言うとおりで、単独で開けばそのファイルが先頭フレームになる。

**リモートのフォルダ stack も同じ規則・同じ文**。§3.1 末尾の「同じファイルが
ローカルとリモートで違う読まれ方をするのは欠陥」がここにも掛かる —— peer 越しに
届いたフレームは TILE の応答が形を名乗るので、先頭フレーム (peer の META が言う
本来の寸法。大きい画像は間引きタイルで表示されているので画面上の寸法ではない) と
違えばそこで断られる。落ちるのは1フレームで stack ではなく、`expectedFrames` は
フォルダのファイル数のままなので、Files 行と canvas フッタの **n / N** が
「何枚中何枚か」を言い続ける。

### 3.3 取り違えたときの逃げ道 — Inspector で言い直す

ユーザー提案 (2026-08-03):「Inspector に ch 数を明示すれば、ch=1 に変更して
読み直す、ってことができる?」— **できる。そしてこれが `--npy-axis` の後継。**

Inspector に「どう読んだか」を出し、**その場で言い直せる**:

```
read as   1 frame x 3 ch   (H,W,C)         [ re-read as... ]
                                            └ 480 frames x 1 ch  (F,H,W)
```

選択肢は**その形から計算できる読み方だけ**。「先頭の面だけ」のような
**スライスは出さない** — どの軸を切るかが形からは決まらないので、
それは adapter が宣言する仕事 (§4)。

- **選択肢はその配列の形から計算される**。ありえない読み方は出さない
- 言い直すと**同じファイルを読み直す** (path は FrameSource が持っている)。
  推測ではなく**ユーザーの宣言**なので、以後その doc はその読み方で扱われる
- セッションに保存され、次に開いても同じ読み方になる
- **失うものは明示する**: 読み直しは doc を作り直すので、その doc に紐づく
  比較スロットのピンや、フレーム番号に依存する状態は張り直しになる。
  何が失われるかを実行前に言う
- **peer 越しでも同じ** (2026-08-10, issue #124)。`FrameSource::npyShape` は
  長らくローカルのデコーダしか埋めず、ssh 越しに開いた doc には行も menu も
  無かった —— §4.13.1 が「remote 込みで v1 から」と決めた逃げ道が、
  データが向こうにある**まさにその場合に**存在しないという非対称。
  プロトコル 9 で META が**ファイルの宣言した shape** を運び、META と TILE が
  **宣言された読み方**を受けるようになった。読み直しは **peer 側で起きる**
  (画素は渡らないまま) で、返る絵はローカルで同じ宣言をしたときと同一。
  古い peer は**この要求を拒否しない** —— 末尾の4バイトを読まずに自分の
  読み方で成功を返すので、client が HELLO の数から**送る前に**断り、
  Inspector は行を出したうえで menu の代わりに理由を出す。
  **押せない menu は空欄より悪い**、が判断の基準

### 3.4 これで消えるもの

- **`--npy-axis`** — 3次元の意味を人が上書きするための**グローバルな旗**。
  同じ実行の中に「stack として読みたいファイル」と「カラーとして読みたい
  ファイル」が混在していても、旗は1つしかなかった。
  §3.3 の言い直しは**ファイルごと**で、しかも画面に見えている
- `decodeNpyBuffer` の `firstIsChannels` 分岐と、暗黙の `CHW->HWC` 変換

**`(F,C,H,W)` (torch の NCHW をそのまま保存したもの) は native では読まない。**
最後の軸がチャンネルではないので `C > 4` として断られ、doc が開かないため
§3.3 の言い直しにも到達しない。**逃げ道は adapter** (§4) — 軸の順序を
知っているのは書いた人なので、そこで `permute` して返す。

### 3.5 native のままにするもの

`.npy` / `.npz` / raw の**形式**そのものは native のまま。日常の9割で外部プロセスを
挟む理由がない。狭めたのは「どの *形* を受けるか」であって、扱う形式ではない。
raw (ヘッダ無し) は元々ユーザーが dtype・寸法・解釈を明示するので、
そもそも推測が無い — 今のままでよい。

### 3.6 PNG / JPEG / ベンダ RAW / TIFF / OpenEXR — 形を推測しない形式は、器に訊く

**決定 (2026-08-04、ユーザー)**:

> PNG/JPEG/TIFF を native で読む。提案のライブラリでよし、**自前でラッパーを
> 作っておいて後で中身を差し替えられる**ようにしておいてください。

これらの形式は `.npy` と違って**寸法もチャンネル数もビット深度も器が名乗る**ので、
§3.1 の形の判定は要らない。代わりに決めることは「**何のライブラリが読むか**」と
「**その値をどう扱うと宣言するか**」の2つで、前者は変わりうるので隠す。

**入口は1つ**: [`core/imagefile.h`](../core/imagefile.h) の `imagefile::decode()`。
返るのは `.npy` が返すのと同じもの (w/h/ch・dtype 名・float32 の画素・note) で、
`loadImageFile()` がそれを `ImageDoc` にする。**ライブラリの名前を知っている
翻訳単位は `core/imagefile.cpp` ただ1つ**で、差し替えは `backends()` の表の1行と、
その行が指す関数1つの入れ替えで済む。

| 形式 | いま読んでいるもの | 状態 |
|---|---|---|
| PNG | stb_image 2.30 (1ヘッダ、MIT/PD、`third_party/stb/` に vendor) | 8/16bit、grey/GA/RGB/RGBA/palette |
| JPEG | 同上 | baseline / progressive、8bit |
| ベンダ RAW | LibRaw 0.22.2 (`core/rawread.cpp`、ソース直コンパイル) | `.dng .cr2 .cr3 .nef .arw .orf .rw2 .raf .pef .srw` ほか。**CFA モザイク 1面をそのまま**、パターン・ブラック・ホワイト・ビット深度は**ファイルの宣言のまま note へ**。デモザイクも WB も黒引きも**しない** (§3.6.5) |
| TIFF | **自前** (`core/tiffread.cpp`、`tiffread 1`) | classic TIFF (magic 42)、II/MM 両方、strip、8/16bit 符号なし整数と 32bit IEEE float、grey (WhiteIsZero/BlackIsZero) と RGB (±alpha)、none/PackBits/LZW/Deflate、predictor 1/2、**複数ページ = stack** |
| OpenEXR | 公式 OpenEXR 3.4.13 + Imath 3.2.2 (`core/exrread.cpp`、**常にリンクされる**) | scanline と 1レベル tiled、half/float、全圧縮 (NONE/RLE/ZIP/ZIPS/PIZ/PXR24/B44/DWA)、**レイヤ = document** |
| y4m (YUV4MPEG2) | **自前** (`core/y4mread.cpp`、`y4mread 1`) | progressive、8〜16bit の**輝度プレーンのみ**、Cmono/C444/C422/C420/C411 (jpeg/mpeg2/paldv 込み)、**1ファイル = 1 stack**。他の動画コンテナは名指しで拒否 |

**値の扱い — 宣言する、推測しない。**

- **保存されたままの値**。8bit は 0..255、16bit は 0..65535。**0..1 に割らない。**
  画素の単位は [DN] 固定 (§8-9) で、整数は値を保ったまま f32 に載る
  (§4.8 — ただし 2^24 まで。u32/i32 はそこから先が別の整数になり、その事実は
  数えて申告される)。
  ここで正規化すると「**リンクされたライブラリによって同じファイルの測定値が
  変わる**」ことになり、ラッパーが防ぐべき最大の事故がそれである
- **16bit が主役**。この分野の PNG は 8bit より 16bit のほうが多い。ファイルの
  ビット深度がどちらの経路を通るかを決めるのであって、8bit が「普通」で 16bit が
  その変種、という作りにはしない
- **伝達特性は当てない**。PNG も JPEG も TIFF も「この数字が何か」を名乗らないので、
  viewer は**自分が何をしなかったか**を note に書く
  (`values as stored, no scaling or transfer curve applied`)。Inspector に出る。
  この一文は**継ぎ目 (`imagefile::decode`) が付ける**のであって、各デコーダが
  付けるのではない —— 規則1の約束であってライブラリの事実ではないので、
  新しいバックエンドが書き忘れることも、少し違う言い方をすることもできない
- **唯一スケールが変わる場所は宣言する**: 1/2/4bit PNG はデコーダが 0..255 へ
  展開する (×255/×85/×17)。note が深度と倍率を名指しする
- **JPEG は codec の色変換を通った値**である旨を note に書く (`YCbCr->RGB`)。
  可逆/非可逆で振る舞いは変えない (2026-08-03 決定) が、**何が起きたかは書く**
- チャンネル数は §3.1 と同じ規則: **grey は 1ch、RGBA は 4ch**。デコーダに
  「常に RGBA でくれ」と頼めば実装は短くなるが、モノクロ画像が 4ch になり
  per-channel 統計が無意味になる

**断り方は §3.2 の体裁のまま** (ファイル名・理由・次にすること)。理由は
`imagefile::decode` が**形式名とリーダ名**を、呼び出し側が**ファイル名**を付ける:

```
scan_tiled.tif: TIFF: tiled layout (TileWidth 256, TileLength 256): this build
  reads strip layouts (tiffread 1, core/tiffread.cpp)
  choose a reader to read it another way
```

### 3.6.1 TIFF — 読むもの・断るもの (2026-08-09)

**複数ページ = stack。** `core/imagefile.h` がそう書いていたとおり、IFD 1つが
frame 1枚になり、`loadImageFile` が `loadNpyBuffer` と同じ形で `App::SeqInfo` を
組み立てる。**`NewSubfileType` bit 0 (縮小版=サムネイル) のページは frame では
ない** —— 落としたことは残る frame の note が言う。ページ間で形が違うファイルは
**stack ではないので断る** (黙って一部だけ開かない)。

**読むもの**: classic TIFF (magic 42) / II・MM / strip / 8・16bit 符号なし整数と
32bit IEEE float / grey (photometric 0・1) と RGB (2)、alpha 付きも可 /
compression 1 (none)・5 (LZW)・8 と 32946 (Deflate)・32773 (PackBits) /
predictor 1・2 / 1ページでも複数ページでも。

**断るもの (すべて名指しで、タグと値を言う)**: BigTIFF (magic 43) / tiled
(`TileWidth`) / `PlanarConfiguration 2` / CCITT・JPEG・JPEG2000・LZMA・Zstd・WebP
などの compression / palette (photometric 3)・CMYK (5)・YCbCr (6) など /
**CFA (photometric 32803) と linear raw (34892)** / 1・4・12bit や符号付き整数や
half などの `BitsPerSample`×`SampleFormat` の組 / `Predictor 3` (float 用) /
サンプルごとに幅や形式が違うファイル / ページ数 4096 超・総サンプル数 2^30 超。

**なぜ「読めるだけ読む」ではないか**: TIFF は3形式で唯一**測定値を運ぶ**器
(16bit/float、黒レベル、CFA パターン) で、**中途半端に読むのは読まないより悪い**。
とくに CFA (`PhotometricInterpretation=32803`) のパターンを読み違えれば
**色も per-plane 統計も静かに間違う**。したがって **RGGB と決め打ちして開くくらい
なら断る** —— これは規則3そのもので、リーダが書かれた後も変わっていない。

**なぜ自前で、libtiff ではないか**: libtiff は vendor できない (configure 前提の
多ファイル構成) し、FetchContent は `third_party/stb/` を作ったときの決定
—— *オフラインの clean clone がビルドできること* —— をそのまま壊す
([CMakeLists.txt](../CMakeLists.txt) の stb ブロック)。そして上の「断るもの」を
全部除いた後に残る仕事は小さい: IFD の鎖を歩き、strip を展開し、差分を戻し、
float に広げる。**断り文そのものが成果物**なので、エラーコードを返す
ライブラリより自分で書くほうが向いている。外部コードは **miniz のみ**
(Deflate、`.npz` で既にリンク済み) で、THIRD-PARTY-NOTICES に足す行は無い。

**CFA の規則**: `cfa` は**読めたときだけ**立てる。PNG/JPEG は CFA を運ばず、
TIFF は CFA ページを**断る**ので、この3形式はいずれも常に 0。`--cfa bayer` は
ユーザーの宣言なので従来どおり効く (推測ではない)。
**ベンダ RAW だけは違う** —— ファイルがパターンを名乗るので、そこから立てる
(§3.6.5)。「読めたときだけ」の**読めた側の最初の1件**である。

**Browse から開ける** (#111 で解決)。かつてここには「Browse の一覧には出ない」と
書いてあったが、正確には**行は最初から出ていた** —— 淡色で、クリックにも
ダブルクリックにも反応せず、理由もどこにも出ない状態だった。原因は
`isNpyName` が**2つの別の問いを1つに畳んでいた**こと。§3.6.4 を見よ。

### 3.6.2 OpenEXR — レイヤは frame ではなく document (2026-08-09)

**レイヤ = document、ページ = frame。** `core/imagefile.h` が引く区別がここで
初めて両側から効く。1ファイルから複数の絵が出たとき、それが何なのかは
**デコーダが `Image::member` で言う**:

- **名前が無い** → 順序があり、同じ形の絵の順序は frame 軸である。TIFF の
  3ページは **1 stack の 3 frame** (§3.6.1)。
- **名前がある** → 順序は無く、形が揃っている必要も無い (`diffuse` は 3ch、
  `Z` は 1ch)。EXR の 2レイヤは **2つの document** で、`.npz` の 2メンバと
  同じもの。`FrameSource::member` に名前が載り、**セッションは保存した
  レイヤだけを開き直す** (`loadImageFile(path, member)`、`loadNpz` と同じ形)。

枚数からは区別できない (「2枚」はどちらでも起きる) ので、**推測せずデコーダが
宣言する**。

**チャンネルのまとめ方**: EXR のチャンネル名は `layer.LEAF`。同じ layer に
`R`,`G`,`B` が揃っていれば 1つの 3ch (あれば `A` を足して 4ch) にし、
**それ以外のチャンネルは 1枚 1ch** にする。`R,G,B,A` 以外の並びには形式が
定める意味が無いので、`X,Y,Z` を色として詰めるのは推測になる。順序は
**ファイルの順序** (EXR の ChannelList はソート済み) のままで、並べ替えない。

**読むもの**: single-part / scanline と **1レベル tiled** / half と float /
圧縮は OpenEXR が読めるものすべて (NONE・RLE・ZIP・ZIPS・PIZ・PXR24・B44・DWAA・
DWAB) / data window が原点でなくてもよい (**document は data window**、
display window との差は note が言う)。

**断るもの (すべて名指しで)**: multi-part / deep (scanline・tile とも) /
**multi-resolution (mipmap・ripmap)** / chroma subsample されたチャンネル /
`Y`/`RY`/`BY` の輝度色差レイアウト / **UINT チャンネル** / チャンネルが 1つも
無いファイル / data window が viewer の上限 (32768) を超えるもの。

**tiled を読み、mipmap を断る理由** (#53 判断 3 の解決): tiling は**格納形式**で
あって、リンク済みのライブラリが可逆に解く。断れば「このビルドが正確に開ける
ファイル」を利用者から取り上げることになる。mipmap は格納形式ではなく
**同じ絵の複数解像度**で、どれが「その絵」かはファイルが投げた問いだから、
loader が黙って答えてはいけない。TIFF が tiled を断るのは**自前リーダに
tile のコードが無いから**であって、同じ語で違う理由である。

**なぜ自前ではなくライブラリか** (TIFF と逆になる理由): TIFF は「この program が
読むべきでないもの」を断ると**小さくなる**。EXR は小さくならない —— tiled と
deep を断いても scanline half/float は残り、その中身は PIZ (wavelet + Huffman) と
DWA (DCT) である。レンダラが実際に書くのはそれなので、NONE と ZIP だけの
自前リーダは**開けないファイルの山**を作る。つまりここには「小さくて正直な
リーダ」という選択肢が無い。代償は隠さない: OpenEXR + Imath はこの木で最も重い
依存で、実測は `docs/media-support.md` §1、ライセンスは THIRD-PARTY-NOTICES.md。

**外し方は無い** (#53 の裁定、2026-08-09 —— 「既定ONで。OFFにするパスは
不要です」)。`VIEWER_WITH_EXR` という option は**削除**した。`.exr` を読むのは
この viewer の性質であって、ビルドの構成項目ではない。したがって
`--media-selftest` の EXR assert (M16-M23) は**常に走る** —— 「この構成では
skip する」という出口が無くなったので、skip する構成も無い。

**その代償を正直に書く**: OFF の道が無くなったので、**ネットワークの無い
clean clone は、OpenEXR/Imath が既にマシンに入っていない限りビルドできない**。
FetchContent が無条件になったからである。これはまさに `third_party/stb/` と
自前 TIFF リーダ (§3.6.1) が守っている性質で、そこと逆の結論になっているのは
承知の上。和らげるものは 2つあり、どちらも意図的に残してある:
`find_package(OpenEXR)` を先に試すこと (システムに入っていればダウンロード
しない) と、`-DFETCHCONTENT_SOURCE_DIR_IMATH` /
`-DFETCHCONTENT_SOURCE_DIR_OPENEXR` でローカルの source tree を指せること。
vendor 化や CI キャッシュはこれを完全に消すが、それは持ち主の判断。

**フォルダ = stack**: `.exr` が並んだフォルダは 1 stack になる。判定は
`imagefile::forPath` で行うので、この表に足された形式は自動的にそうなる
(`core/app/sequence.inc` の `SELFDESC_EXTS` / `HEADERLESS_EXTS` に書き足す必要は無い)。**1ファイルが複数の絵を持つ場合は
frame として断る** —— 複数ページ TIFF の並びは「stack の stack」であり、
黙って 1ページ目だけ取るのは §3.2 に反する。

**Browse も EXR を開く** (#53 判断 4 → #111 で解決)。EXR 固有の話ではなく
PNG/JPEG/TIFF/y4m と同じ 1件だったので、形式ごとに回避せず述語の側で 1回
解いた。§3.6.4 を見よ。

### 3.6.3 y4m — 動画のうち、数値が生き残った部分だけ (2026-08-09)

議論と実測は [video-support.md](video-support.md)。ここには結論だけ置く。

**問いは「デコードできるか」ではなく「その数値はまだ数値か」。** 実測: 8bit の
lossy 往復は既知の σ_t = 40 DN16 を **0.00 にする**(劣化ではなく消滅)。8bit で
表現できるノイズも 11% 減衰し、GOP 周期のバイアスが乗る (I frame が P frame より
3.9% 多く残す)。**lossy な stack の per-frame ノイズ図はエンコーダの測定である。**

結論は普通と逆になる: **libavcodec が開けるようにしてくれる形式は、まさにこの
ツールが測定に使ってはいけない形式**。正直な部分集合は依存ゼロで届く。

**なぜ y4m が画像形式の表にいるのか。** §2 の問いを通した後の y4m は画像形式
だから: テキスト 1行 + 生プレーン、圧縮なし・フレーム間予測なし。1ファイル =
**同じ形の絵が N 枚、順番付き**で、これは複数ページ TIFF と同じものである。
`Image::member` を空のまま返すだけで `loadImageFile` が §3.6.2 の規則で
**1 stack** を組む —— y4m 専用の経路は 1本も無い。

**読むもの**: progressive のみ / 8〜16bit / Cmono・Cmono<N>・C444・C422・C420・
C411 (jpeg・mpeg2・paldv の siting 差は輝度に影響しないので読む) / pNN の深度
接尾辞 / alpha プレーン付きも可 (数えるだけ)。**輝度プレーンのみ。**

**断るもの**: interlaced (`I` が `p` 以外) —— **1フレームが2フィールド = 2瞬間**で、
その stack の σ_t は時間ノイズではない / 知らない colour space (名指し) / 8〜16bit
以外の輝度深度 / `MAX_DIM` 超の W・H / 100000 フレーム超。

**クロマは読まない。** 2x2 に 1サンプルから全解像度 3ch を作るのは補間で、
**補間値は測定値ではない**。「クロマはあったが読んでいない」ことを note が言う。

**n of N は割り算である。** フレームは固定長なので、枚数は**バイト数から出る** ——
この形式はフレーム数をどこにも宣言しない。したがって途中で切れたファイルは
「生き残ったバイトが示す N」で報告する (5枚書いたものを半分にすると
「2 of 3」であって「2 of 5」ではない。5 は**ファイルに無い数**)。

**他の動画コンテナは名指しで拒否する** (`imagefile::videoRefusal`、21拡張子)。
形式名・**実測の理由**・正直に取り込む ffmpeg コマンドの3点セット。可逆 codec を
入れうるコンテナ (.mov/.mkv/.avi) にはその旨も言う。**raw ダイアログより先に**
判定するので、H.264 のビットストリームに幅と高さを入力させる誘導は起きない。
`Backend` の行に**しない**理由: `absent` は「何かをリンクすれば読める」状態で、
ここでの答えはリンクしても変わらない。表はバイトで判定し `dialogPattern()` を
作るので、21行足すと `*.mp4` が画像ダイアログに並んでしまう。

**未解決 (旧ブランチから変わった1点)**: 旧実装は `seqMemBudget()` で読む枚数を
切っていた。継ぎ目の `decode` はバイト列から `vector<Image>` を返す形なので
途中で予算を見る場所が無く、**黙って切るより切らない方**を選んだ (予算切れと
ファイル切れが同じ「n of N」を出すと診断が潰れるため)。上限は敵対的ヘッダ用の
`MAX_FRAMES` のみ。巨大 y4m の予算付き読みは継ぎ目に streaming を教える別件。

### 3.6.4 Browse の一覧 — 述語は 2つある (#111, 2026-08-09)

**1つの述語が 2つの問いに答えていた。** `isNpyName` は Browse パネルと remote
peer の**共有**で、パネルは両方の意味でこれを引いていた:

| 問い | 誰が答えるべきか | 答え |
|---|---|---|
| **この viewer はこのファイルを開けるか** | 形式表 (`core/imagefile.h`) | npy・npz + 表の全行 |
| **この peer はこのファイルを配れるか** | `core/serve.cpp` | npy のみ |

結果、**このマシンのディスクを見ている一覧が peer の問いに答えていた**。
`.png .jpg .tif .exr .y4m` の 5形式が、まさにそのファイルが置いてあるフォルダの
一覧の中で**死んだ行**になっていた。

**「一覧に出ない」は不正確だった。** 行は最初から描かれていた —— 淡色 (`servable`
が false) で、クリックにもダブルクリックにも**無反応**で、右クリックに Open が
無く、理由がどこにも出ない。欠けていたのは行ではなく**申し出**である。

**直し方 — 形式ごとの回避ではなく述語を割る:**

```
viewerReadsName(n)   -> imagefile::forPath(n) != nullptr、+ .npy / .npz
peerServesName(n)    -> .npy
rbRowOpenable(host, n) = host.empty() ? viewerReadsName(n) : peerServesName(n)
```

**どちらの述語も形式名を持たない。** 表に 1行足せば Browse は編集ゼロでそれを
一覧に出す —— これが「次の形式で同じことが起きない」の中身である。

**リモート一覧の裁定 (「見せて理由を言う」)**: peer が配れないファイルも**一覧から
落とさない**。淡色のまま残し、hover で理由を出し、ダブルクリックしたら同じ文を
toast で言う。§3.2 の三部構成 (名指し・理由・逃げ道) で:

> `PNG is read on this machine, but the peer serves .npy only`
> `  browse it locally (File > Browse Folder), or copy it here first`

黙って消すのはこの repo の流儀ではない (`imagefile.h` の `absent` が同じ理由で
存在する)。**行があって理由が無い**のは、黙って消すのと半分同じ失敗である。

**stack 系の動詞は peer の問いのまま。** 「Open as stack」「Open as frame average」
「Temporal stats (server) for stack "…"」は**ローカル一覧でもプロトコル越し**に走るので、
開ける `.png` の行でもこの 3つは出ない (出して押させて断るより出さない)。

**プレビューは付かない。** 1クリックのプレビューは**間引いた TILE** で、tile 経路は
peer のもの。ローカルの絵にはその安価な部分デコードが無い (200 MB の `.exr` を
次の矢印キーで捨てるスロットのために全部デコードすることになる) ので、**1クリックは
選択・ダブルクリックで開く** —— フォルダ行とまったく同じ挙動で、読み手が覚える
第3の振る舞いを増やさない。

**`viewer-serve` は 1バイトも変わっていない** (media 3本と同じ前例)。この問題は
最初から**クライアント側の述語**の問題だった。

#### 3.6.4b peer も同じ形式を配る (#148 判断 B, 2026-08-10)

上の `peerServesName(n) -> .npy` は**この日で終わった**。#148 の判断 B により
`viewer-serve` は `core/imagefile.h` をリンクし、**このビューアが読む絵の形式を配る**。

```
peerServesName(n) -> imagefile::peerServes(n)   // .npy + 表の overLink 列
```

**述語は 1つ、表も 1つ。** 「どの形式がリンクを渡るか」は `Backend::overLink` という
表の 1列で、client の門も peer の限界も**同じ関数**を読む —— 一覧が 2つあれば
ずれる、というのが #148 が 1段上で言っていることだからである。

**RAW だけが false**。理由は reader ではなく**配布**で、LibRaw は CDDL-1.0、
`viewer-serve` は ssh で相手のマシンに自分をインストールする別バイナリである
(`core/rawread.h` の licence note)。peer のビルドでは表の RAW 行は**拡張子と
sniff を保ったまま decoder だけ失う** (`absent`) ので、`.NEF` は名指しで断られ、
下の TIFF 行に落ちて先頭 IFD を「測定値」として解かれることはない。

拒否文は §3.2 の三部構成のまま、**理由が名指しになった**:

> `vendor RAW is read on this machine, but the peer does not serve it: LibRaw is`
> `CDDL-1.0 and viewer-serve installs itself onto another machine over ssh`
> `  browse it locally (File > Browse Folder), or copy it here first`

**ワイヤの形は変わっていない** —— `MSG_TILE` は元から画素を運ぶ。送るのは
**ファイル自身の dtype** で、viewer の内部表現 (float32) ではない: この 5形式の
dtype (u8/u16/f16/f32) は**すべて float32 に厳密に載る**ので往復で 1ビットも
変わらない。`f16` は wire に型を 1つ足した (`rp::DT_F16`) —— f32 に広げると
同じ `.exr` が local で `f16`、リンク越しで `f32` と表示され、それこそ #148 の
「1つのフォルダが 2つの答え」を 1段下で作ることになる。

`rp::VERSION` は **10**。形は動かないので 4/5/6 と同じ**意味**の版上げで、
入っている peer を接続時に更新させるのもこの番号である。

### 3.6.5 ベンダ RAW — ファイルが数値の意味を名乗る唯一の形式 (2026-08-10)

**裁定 (2026-08-09、ユーザー、#52)**:

> ベンダ Raw は reader で実装しろというのも酷なので native 対応しときたいなぁ

`core/imagefile.h` の継ぎ目の内側 = native、reader = アダプタ、という線引きの上で
**ベンダ RAW は native 側**に置く。実装は `core/rawread.cpp`、表の1行。

#### 何を読むか — `raw_image` だけ

`LibRaw::unpack()` が返す `rawdata.raw_image`、すなわち**センサが数えた CFA
モザイク 1面**だけを取る。`dcraw_process()` も `dcraw_make_mem_image()` も
**呼ばないどころかリンクされていない** (下の「ビルド」)。したがって:

| ファイルが宣言するもの | どこへ行くか |
|---|---|
| CFA パターン | `Image::cfa = 1` と `cfaPattern` (RGGB/BGGR/GRBG/GBRG)。**読めた値**であって推測ではない |
| ブラックレベル | **`note` に表示**。**引かない。** 面別 (`cblack`) があればそれも並べる |
| ホワイトレベル | `note` |
| ビット深度 | `note` (`color.raw_bps` = ファイルの宣言) |
| 可視領域と raw フレームの差 | `note`。**マスク境界は読まない**が、あることは言う |
| 画素 | `dtype = "u16"`、値は**カウントそのまま** |

**なぜ黒を引かないか。** #52 の調査 (branch `media-format-strategy` の `docs/media-formats.md`) §4.7 の実測 ——
Nikon 1 AW1 の NEF は**ブラックレベル 0 を宣言し、最も暗い画素が 80** だった。
宣言値を引く実装はここで間違い、他の全部で無言である。逆に「観測した床の 80 を
ブラックとして採用する」のは**ファイルが言っていない数を作る**ことで、正典が
禁じている側。したがって **0 と表示し、80 を渡す**。
`--media-selftest` の M31 がこれを assert している。

**なぜ可視領域なのか。** LibRaw の `COLOR(row,col)` は**可視画像の座標**に対する
答えである (`raw2image()` がまさにその添字でモザイクを読む)。raw フレーム全体を
渡すなら別の原点のパターンを**こちらで導出**することになり、それは規則3
(「読めるか、無いか。推測しない」) の禁じる算術である。マスク境界 (光学黒) は
測定に価値があるが、**それを渡すのはパターンを自分で計算し直すこととセット**なので
この PR の外に置き、note で「そこにある」と言うだけにした。

#### 何を断るか — 名指しで

```
sensor_jpegxl.dng: vendor RAW: JPEG-XL compression (DNG 1.7): decoding it needs
  the Adobe DNG SDK, which is not built into this viewer (LibRaw 0.22.2)
  choose a reader to read it another way

sensor_xtrans.dng: vendor RAW: a non-Bayer mosaic (LibRaw filters code 9, an
  X-Trans 6x6 pattern): this viewer names four 2x2 Bayer orders and cannot
  describe it, and describing it wrongly would make every per-plane statistic
  wrong in silence (LibRaw 0.22.2)
  choose a reader to read it another way
```

| 断るもの | 文面が名指しするもの |
|---|---|
| Nikon HE / HE\* | `Nikon High Efficiency (HE, NEFCompression 13)` + 「上流が not supported yet と書いている」 |
| JPEG-XL DNG (1.7) | `Adobe DNG SDK` が要る |
| GoPro VC-5 | `GPR SDK` が要る |
| X-Trans などの非 Bayer | `filters` コードと 6x6 の別 |
| Foveon X3 | 「画素ごとに 3つ積む器であってモザイクではない」 |
| float DNG / 3面・4面 RAW | 「この reader が渡すのは CFA モザイクだけ」 |
| `filters == 0` | 「ファイルが CFA を名乗っていない」 |
| RGGB/BGGR/GRBG/GBRG 以外の 2x2 | 読めた4文字と `cdesc` を出す |

**どれも復号前か、LibRaw 自身の返り値から**出る。`get_decoder_info()` は
`unpack()` の前に呼べるので、復号できないコーデックは**1画素も触らずに**断れる。

#### 振り分け — なぜ TIFF 行より前なのか

**ベンダ RAW の器はたいてい TIFF である。** `.NEF` も `.ARW` も `.PEF` も `.DNG` も
先頭 4バイトはスキャナの `.tif` と同じなので、**先に訊かれた行が全部持っていく**。
そこで `rawSniff` は「ファイル自身がカメラのものだと言っているか」だけに答える:

- 自前の署名を持つ器 —— CR3 (`ftypcrx`)、ORF (`IIRO`/`IIRS`/`MMOR`)、
  RW2 (`IIU\0`)、RAF (`FUJIFILM`)、X3F (`FOVb`)、MRW (`\0MRM`)、CR2 (`CR` @8)
- 素の TIFF —— **IFD0 に DNGVersion (50706) がある**、または
  **Make (271) がカメラメーカ名で、かつ SubIFDs (330) がある**

**両方を要求するのが要点**である。`cameramade.tif` (16bit グレー + `Make =
"NIKON CORPORATION"`、SubIFD 無し) は TIFF リーダに届かなければならない ——
そしてこれは机上の心配ではない: **LibRaw はその TIFF を喜んで「Bayer」として開く**
(実測)。門が無ければ、測定用 16bit TIFF が推測パターンのモザイクとして開いていた。
M34 がこの1件を守っている。

`.raw` は**取らない**。あれはこの viewer 自身のヘッダ無しダイアログのもので、
形も深度も人が宣言する —— ヘッダの無いファイルについてライブラリが
「自分のほうが知っている」と言う場所ではない。

#### ビルド — 4分の1だけコンパイルする

LibRaw に CMakeLists は**無い** (`README.cmake`: 2014年に公式サポート終了)。
なのでソースを直に並べる —— imgui と miniz にこの木がやっているのと同じこと。

**そのうえで `src/demosaic` `src/postprocessing` `src/preprocessing` `src/write`
を落とし、`*_ph.cpp` プレースホルダのほうを採る。** これは思いつきではなく
**上流自身の `Makefile.devel.noppr2i` 構成**で、`*_ph.cpp` はまさにそのために
存在する。効果は秒数より大きい:

- `dcraw_process()` も全デモザイクも **`subtract_black()` も binary に無い**。
  「デモザイクしない・宣言された黒を引かない」が**呼ばない約束**から
  **リンクされていない事実**になる
- LibRaw 内の第三者コード (DCB / FBDD demosaic、BSD 系) を**配布しない**ので
  THIRD-PARTY-NOTICES が正しくあるべき対象が1つ減る
- 素直に `src/**.cpp` を glob すると `*_ph.cpp` が実体版と**同じシンボル**を
  定義して multiple definition で落ちる (実測4件)。プレースホルダ側を採るのは
  その罠の裏返しであり、上流が支持している側でもある

`LIBRAW_NOTHREADS` は**定義しない**。あれは `decoders_dcraw.cpp` のビット読みと
`sony_decrypt()` の pad を関数内 static にする —— この viewer はフォルダの
フレームを sequence loader のスレッドで復号しながら UI スレッドで別のファイルを
開けるので、インスタンス毎 TLS の側が正しい。Windows では `ws2_32` が要る
(メタデータパーサの `ntohl`/`htons`。この repo が初めてリンクするソケットライブラリ)。

#### 実測コスト

MinGW/GCC 16.1 UCRT、Ninja、Release、20 論理コア。**A/B/A/B/A/B と交互に、
連続して 3 ペア**の clean cold build を測った (依存の取得時間は含まない ——
両側とも同じローカルソースを使う)。**1 ペアでは足りなかった**: 別作業と並行して
測った最初のペアは「増分なし」に見え、機械を空けて測り直したら +100 s 前後だった。
中央値と幅で読むこと。

| | なし (main 4a41302) | あり | 差 |
|---|---|---|---|
| configure | 9–10 s | 9–10 s | **0** |
| **build (中央値)** | **307 s** (290 / 316 / 307) | **401 s** (405 / 391 / 401) | **+94 s (+31%)** |
| `libraw_lib.a` (63 TU) | — | **1,320,272 B** | 単独 cold build 61.9 s (中央値、3回) |
| `viewer.exe` | 12,506,742 B | **13,434,806 B** | **+928,064 B (+0.885 MiB、+7.4%)** |
| `viewer-serve.exe` | 3,357,701 B | 3,357,701 B | **0 —— md5 まで同一** |

同調査 §4.2 の予測 (+11.8 s / +0.95 MiB) との対応:

- **バイナリは +0.885 MiB で、予測より小さい** —— demosaic と postprocessing を
  落としたぶん。EXR の **+3.01 MiB の 3分の1以下**である。
- **ビルド時間は予測の 8 倍**。予測は `libraw_lib` を**単体で**建てた 11.8 s で、
  ここで測ったのは**すでに 20 コアを飽和させている cold build に足したときの
  壁時計**である。63 TU (`crx.cpp` / `identify.cpp` / `cameralist.cpp` はどれも
  大きい) が既存の臨界路と席を奪い合うので、単体の秒数はそのまま足せない。
  **単体で測ると誤って安く見える**、というのがこの差の中身である。

`viewer.exe` は clean build と incremental build で **md5 が一致**した
(`-Wl,--no-insert-timestamp` の効果) ので、上のサイズは 1 回の偶然ではない。

`viewer-serve` に載せないことは 1つの事実が保証している: あれは
`core/imagefile.cpp` をコンパイルしない。上の byte 同一がその証拠で、
Ubuntu 20.04 コンテナの手書き `g++` 1行も無傷である。

> **保証の根拠だけが 2026-08-10 に失効した (#148 判断 B)。結論は変わらない。**
> peer は `core/imagefile.cpp` をコンパイルするようになり、20.04 コンテナも
> `g++` 1行では済まなくなった (OpenEXR を静的に建てる)。LibRaw を載せない
> 理由はもう「コンパイルしないから」ではなく `VIEWER_NO_LIBRAW` という
> **明示の指定**で、その理由は CDDL-1.0 という**ライセンス**である
> (THIRD-PARTY-NOTICES.md)。上のサイズ測定はその時点の記録として残す。

#### fixture — 合成 DNG。そして合成できないもの

`tools/gen_testdata.py` が `media/sensor_*.dng` を書く。**DNG はカメラ無しで
書ける唯一のベンダ RAW** で、CFA パターン・ブラック・ホワイト・深度を
NEF や CR3 と**同じタグ**で宣言する。だから「宣言した数が document に届くか」は
リポジトリの中で完結して検証できる (M29-M34)。

**合成できないのはベンダの伸張器**である。Canon のウェーブレット、Nikon の
Huffman、Sony のデルタ、Panasonic の圧縮 —— これらは LibRaw のもので、
CI では 1バイトも通らない。**そこは手で確かめた**。この PR で実際に開いた
実機ファイル (すべて [raw.pixls.us](https://raw.pixls.us/) の CC0 データセット、
**リポジトリには入れない**):

| ファイル | 実測されたもの |
|---|---|
| `Nikon/1 AW1/_DSC0521.NEF` (12,781,061 B) | 4620x3082、RGGB、12bit、**black 0 / 画素 80..306** |
| `Sony/DSC-HX95/DSC00018.ARW` (19,579,648 B) | 4928x3708、RGGB、14bit、black 800 |
| `Olympus/E-M1MarkIII/_3160529.ORF` (63,433,262 B) | 10388x7792、RGGB、12bit、black 256 |
| `Canon/EOS R5/Canon_EOS_R5_RAW_ISO_100_crop_dual.CR3` (30,435,672 B) | 5087x3391 を 5248x3510 から、black 511 + 面別、**2 shot と申告** |
| Panasonic DC-GH5S `.RW2` (14,750,208 B) | 2776x2768、RGGB、14bit、black 511 |

clean clone がこれを再現する手順:

```sh
curl -L -o aw1.NEF "https://raw.pixls.us/data/Nikon/1%20AW1/_DSC0521.NEF"
# 以下同様。curl -L は必須 (/data/... は /download/data/... へ 301)
```

#### この PR が**やっていない**こと (切り口)

- **EXIF を `meta` に載せない。** LibRaw は shutter / ISO / aperture / focal /
  timestamp を追加ライブラリ 0 で出すが、`ImageDoc` に `meta` はまだ無い。
  そして 同調査 §9.1 の実測 ——
  **Olympus は aperture 0 / focal 0 を返す** —— のとおり、欠測が 0 として来る。
  正典の「読めなければ**未設定**、0 ではない」に従い、**EXIF から series を
  自動生成することは今後もしない**。載せるとしても `meta` へ、人が確定する提案として
- **マスク境界 (光学黒) を渡さない** (上記の理由)
- **表示レンジは dtype 由来のまま** (u16 → 0..65535)。14bit の RAW は暗く開く。
  ファイルが言うホワイトレベルを初期レンジにするのは筋が良いが、
  `defaultRange()` は dtype しか見ておらず、そこを変えるのは別件
- **圧縮方式で振る舞いを変えない** (2026-08-03 のユーザー裁定、
  同調査 §4.8)。可逆でも非可逆でも開いて `dn`

---

## 4. リーダの仕様

### 4.1 契約はユーザーが書く「関数」

JSON は viewer の都合の書式で、書く人にとっては外国語。この分野の人が毎日書いて
いるのは numpy であって書式ではない。**契約は関数の返り値**。

```python
def load(path):            # path: str。この1引数だけが必須
    ...
```

- 関数名は自由。viewer には `adapters/acme.py:load` の形で指定する
  (1ファイルに複数の reader を置ける)
- **`path` はフォルダのこともある** (2026-08-03 決定)。series は「1条件1フォルダ」
  「1条件1ファイルのフォルダ」で保存されることが多く、**フォルダを1つの
  `Series` にする**のが自然な単位。adapter 側で `os.path.isdir` を見て分ければよく、
  API は増やさない。viewer 側は Browse のフォルダ行と picker から reader を指定できる
- モジュールは**自分のディレクトリを sys.path に載せて**実行される
- **元ファイルを書き換えてはならない。** 読むだけ
- **将来のための予約** (今決めておけば後で破壊的変更にならない):
  関数が `only` キーワード引数を受けるなら、viewer は「この範囲だけ欲しい」と
  渡すことがある。受けない関数はこれまでどおり全部返す。

  ```python
  def load(path, only=None):   # only: range | list[int] | None (全部)
  ```

  **なぜ要るか** — adapter が勝手に一部だけ返しても「部分読み」にはならない。
  viewer には**それが全部だと見える**ので、10000 枚のファイルから 20 枚返せば
  「20 枚の stack」として扱われ、n/N も 20/20 と表示される。正典の
  「部分ロードは n/N を言う」が破れる。`only` は viewer 側が主導して
  「10000 枚のうち今見る 20 枚」と言えるようにするためのもの。
  **adapter が自分の判断で一部だけ返すときは、その事実を `note` に残すこと。**

### 4.2 返り値が層を名乗る

**返り値の型が、frame / stack / series / batch / AnalysisSet のどれかを
名乗る。** 層モデルの5語以外の語を持ち込まない (`frames` のような新語を
作らない)。5語目 (役割束縛の AnalysisSet — 正典の5層化に伴い 2026-08-07 追加)
の返し方は [reader-analysisset.md](reader-analysisset.md) が仕様で、本節の
以下は最初の4語のまま。

```python
from viewer_import import Frame, Stack, Series, Batch

return Frame(img)                       # 1枚
return Stack(arr)                       # 複数フレーム、同一条件
return Series(stacks, conditions=...)   # 条件を振ったもの
return Batch([dark, flat])              # 一緒に開くもの
```

**返し方は2通りだけ。** 素の配列か、上の型か。dict は用意しない —
同じことを言う道が3つあれば3通りに食い違い、どれが正なのかを
harness が調停する羽目になる。

```python
def load(path):
    return np.load(path)["data"]        # 1. 素の配列: native と同じ読み方。1行のまま
```

素の配列は §3.1 の native と同じ規則で読む (覚えることを増やさない)。
**曖昧さを自分で潰したくなった時点で型を名乗る** — それが2つ目の道。

### 4.3 層ごとのフィールド

| 層 | 位置引数 | 追加のフィールド |
|---|---|---|
| `Frame` | 画素 | `cfa` `name` `note` `meta` `range` |
| `Stack` | 画素 | `timestamps` `cfa` `name` `note` `meta` `range` |
| `Series` | `Stack` の列 (または画素) | **`conditions` (必須)** `name` `note` `meta` `range` |
| `Batch` | 上記の列 | `name` |

| フィールド | 型 | 意味 |
|---|---|---|
| `timestamps` | `Values` | 各フレームを**いつ撮ったか**。条件は同じまま。§4.3.3 |
| `conditions` | `Values` | stack 間で**変えた条件** (露光・照度)。series の存在理由。§4.3.3 |
| `cfa` | str | `"RGGB"` `"BGGR"` `"GRBG"` `"GBRG"`、quad は `"quad:RGGB"` |
| `name` | str | 表示名。省略時はファイル名 |
| `note` | str | **素性の一文**。画面に出る散文。§4.3.1 |
| `meta` | dict | **撮影条件の事実**。key-value。provenance に載る。§4.3.1 |
| `range` | (lo,hi) または配列 | 表示レンジ。**その層の形に合わせられる**。§4.3.3 |

`timestamps` / `conditions` の値の型はどちらも `Values` 1つ (§4.3.2)。
**値は全桁保持。単位は推測せず、必須** — 空のまま渡された軸は**適用されない**
(貼り付けの規則と同じ)。

綴りを間違えたフィールドは**その場で TypeError になる** — 型を名乗る利点そのもの
(dict なら「知らないキー `foo`」を実行後に報告するしかなかった)。

#### 4.3.1 `note` と `meta` の違い

| | `note` | `meta` |
|---|---|---|
| 何か | **一文の散文**。「これは何で、何が起きたか」 | **key-value の事実**。「どんな条件で撮ったか」 |
| 例 | `"CHW から並べ替えた"` / `"24 中 18 フレーム"` | `{"gain_db": 6.0, "sensor": "IMX999"}` |
| 誰が書くか | adapter **と viewer の両方**。viewer は追記する | adapter だけ。**viewer は運ぶだけで書き換えない** |
| どこに出るか | 画像の隣 (既存の `ImageDoc::note`) | Inspector と**エクスポートの出所行** |
| 増えるか | 合成される (`"frame axis, CHW->HWC"` のように) | key が増えるだけ |

viewer 自身が note を書く場面が既にある — dtype を変換した、n/N しか読めなかった、
並びを直した。**adapter はその同じ一行に足す**のであって、別の場所に書くのではない。

逆に **`meta` に散文を入れない**こと。「暗いので撮り直した」は note であって
meta ではない。meta は後で機械が読む (provenance・将来の series 値)。

**単位が要る量を meta に入れるとき**は値だけ置かない:

```python
"meta": {"gain": {"value": 6.0, "unit": "dB"}, "sensor": "IMX999"}
```

正典の「量には必ず単位」がここにも効く。**宣言が勝つ**: `meta` には単位を置く場所が
あるのだから、`"gain_db": 6.0` のように**キー名に単位を埋めない**。キー名は誰かが
選んだ名前であって、宣言できる場所があるときは単位の証明にならない。

**これは「単位をキー名から推測しない」という全体規則ではありません。** 宣言する場所が
無いところでは、キー名の規約がその宣言そのものです —— アナライザの `emit_number` は
ABI に単位のフィールドを持たない(`ps_plugin.h` の `psAnalyzeSink` / `psAnalyzeSink2`)ので、
`snr_db` / `*_pct` / `mtf50 (cy/px)` というキー名が唯一の申告経路で、host は
`unitForAnalysisKey` でそれを unit 列へ展開します([analyzers.md](analyzers.md))。
Python アナライザは両方に触るので、境目はこう引きます:
**測定結果の行はキー名の規約に従い、撮影条件の `meta` は `{"value":…, "unit":…}` を使う。**

#### 4.3.2 `timestamps` と `conditions` — 値の型は1つ

```python
from viewer_import import Stack, Series, Values

Stack(arr,  timestamps=Values(t, unit="s"))                    # 名前は "time" が既定
Series(sts, conditions=Values(exposures, "exposure", "ms"))    # 名前は必須
```

| フィールド | 付く層 | 長さ | `name` | `unit` |
|---|---|---|---|---|
| `timestamps` | `Stack` | **フレーム数** | **省略可** (既定 `"time"`)。`"elapsed"` 等で上書き可 | **必須** |
| `conditions` | `Series` | **stack 数** | **必須** — 何を振ったかは viewer には分からない | **必須** |
| `covariates` (将来) | `Stack` | フレーム数 | **キーが名前** | **必須** |

- **型は `Values(values, name="", unit="")` の1つだけ。** 専用型 (`Times` / `Sweep`) は
  作らない — `covariates` を足すときも同じ型で足りる
- **`name` の冗長は、フィールド側が既定を持つことで消す。** `timestamps` に
  `"time"` と書き足す必要はない (書きたければ書ける)
- **`unit` は常に必須。** 時刻ですら `"s"` か `"ms"` かは推測しない。
- **単位が空でも値は捨てない。** 軸としては適用されないが、値はそのまま viewer に
  渡り、**UI で単位を入れれば効く**。viewer 側は元々この状態を持てる
  (`Series::unit` は空が既定、`Member::value` は NaN = 未設定、
  単位が無ければフィットしない)。
  **「単位が無い」は「データが無い」ではない** — 捨ててよいのはラベルだけで、
  測定値を捨てるとユーザは数値を打ち直す羽目になる
- **複数形は「メンバ1つにつき1つ」を語形が言う。** 長さの規則が名前から読める

将来 `covariates` を足すときの形 (v1 では受けない、名前だけ予約):

```python
Stack(arr, timestamps=Values(t, unit="s"),
           covariates={"temperature": Values(temp, unit="C")})
```

**なぜ `timestamps` を汎用化しないか**: 温度ドリフトを時刻欄に入れさせると、
「いつ」と「何が観測されたか」が同じ場所に混ざる。狭く始めて、必要になったら
別のフィールドで足すほうが、後から意味を分離するより安い。

#### 4.3.3 `range` は、それが属する層の形に合わせられる

ユーザー提案 (2026-08-03):「stack でフレーム毎に変えたい場合、series で
series 毎に変えたい場合は、画像側の shape と合わせて作ったらどうだろう?」

| 書き方 | 形 | 意味 |
|---|---|---|
| `range=(lo, hi)` | 2 | その層の全部に同じレンジ |
| `Stack` に `range=arr` | `(F,2)` | **フレームごと** |
| `Series` に `range=arr` | `(S,2)` | **stack ごと** |
| `Series` に `range=arr` | `(S,R,2)` | stack ごと × フレームごと |

長さが合わなければ**拒否** (`conditions` と同じ規則)。

**ただし警告を1つ**: フレームごとに違うレンジを与えると、**並べても目で比較
できなくなる**。同じ明るさに見える2枚が、実は違う伸ばし方をされている。
viewer には既に同じ性質の機能があり (`Auto per frame`、montage の per-frame
range は「values are NOT DN, view only」と明記している)、それと同じ扱いにする:

- レンジが一様でない stack / series は、**そうであることを画面が言う**
  (note に `display range varies per frame`)
- **測定値には一切触らない**。σ_t も平均も生の値のまま。range は表示だけ

### 4.4 形の判定は、名乗った層の下で行う

**自分より下の層の数だけ軸があり、最後にチャンネル軸を足してもよい。** これだけ。

| 層 | 受ける shape | 意味 |
|---|---|---|
| `Frame` | `(H,W)` `(H,W,C)` | 1枚 |
| `Stack` | `(F,H,W)` `(F,H,W,C)` | F 枚。`(3,H,W)` は3枚 |
| `Series` | `(S,R,H,W)` `(S,R,H,W,C)` | S 条件 × R 繰り返し |
| `Series` | `Stack` / `Frame` の列 | そのまま |
| どれでも | 合わない shape | **拒否**。層と shape を両方名指しして言う |

- **`(S,H,W)` は受けない。** 「S 条件 × 各1枚」は `(S,1,H,W)` と **1 を明示**して書く。
  これで4次元の Series に解釈の分岐が無くなる
- 型を名乗っているので、**チャンネル軸の有無はランクで一意に決まる** —
  native の「最後の軸 ≤ 4」のような判定すら要らない
- **`layout` は無い。** 軸の順序を canonical に合わせるのは書いた人の仕事
  (`t.permute(0,2,3,1)` の1行)。宣言フィールドにしてもコピーは1回のままで
  「誰が転置を書くか」が動くだけなので、契約に置く価値がない

### 4.5 `timestamps` と `conditions` の違い — 「経っただけ」か「設定を変えた」か

一文で言うと:

> **オペレータが設定を変えたなら `conditions`。ただ時間が経っただけなら `timestamps`。**

| | `timestamps` (Stack につく) | `conditions` (Series につく) |
|---|---|---|
| 何を表すか | 各フレームを**いつ撮ったか** | stack 間で**変えた条件** |
| 例 | 時刻 [s]、開始からの経過 [s] | 露光 [ms]、照度 [lx]、ゲイン [dB] |
| 撮影条件は | **同じ** | **各点で違う** |
| σ_t | **意味を持つ** (繰り返しなので) | **条件をまたぐ計算は拒否** |
| 何が描けるか | ドリフト (時間に対する平均値の変化) | リニアリティ・PTC |
| 層 | stack の中の座標 | series を series たらしめるもの |

**なぜ分けるか**: 露光を振った8枚を「1つの stack」として渡されると、
viewer は σ_t を計算できてしまい、**露光の掃引を「時間ノイズ」として報告する**。
数字は出るが意味は無い — 測定器として最悪の失敗。
`Stack` と `Series` という型がその2つを分け、`timestamps` / `conditions` はそれぞれの
層に属するので、**構造的に取り違えられない**。

判断に迷う例:

- **温度が勝手に上がった連続撮影** → `timestamps` (時刻)。設定は変えていない。
  温度そのものは観測された共変量で、v1 では受けない (将来 `covariates`)
- **温度を段階的に設定した測定** → `conditions`。各点が別の条件
- **撮影時刻を記録した露光掃引** → `Series` + `conditions`(露光)。時刻も残したければ
  各 `Stack` の `timestamps` に入れる (両方持てる)

### 4.6 層の宣言が、そのまま σ_t の可否になる

**同じ `(F,H,W)` が2つの違うものを表しうる**:

- **同一条件の F 回の繰り返し** → `Stack`。σ_t (時間ノイズ) が意味を持つ
- **条件を振った F 枚** (露光 1,2,5,10 ms) → `Series`。ここで σ_t を計算すると
  **露光の掃引を「時間ノイズ」として報告する**ことになる

正典が「σ_t は stack の性質」と言っているのはこのため。形からは決まらない。
**型がその答えそのもの**なので、書く人が意識するのは「繰り返しか、振ったか」の1点:

| 書くもの | できる測定 |
|---|---|
| `Stack(arr)` | σ_t、σ_s、欠陥画素 |
| `Stack(arr, timestamps=...)` | 上 + 時間に対するドリフトの図 (条件は同じまま) |
| `Series(..., conditions=...)` | リニアリティ・PTC。**条件をまたぐ σ_t は拒否** |

- `Series` の各 `Stack` が繰り返しを持てる (EMVA 的な撮り方はこれ)
- series メンバシップは正典どおり明示的に作られ、**後から自動で継がれない**
- `conditions` の長さが合わなければ**拒否** (黙って辻褄を合わせない)

素の配列を返したときは `Stack` (繰り返し) として扱われる。それが違えば σ_t は
嘘になるので、note に `repeats (returned as a bare array)` を残す —
数値の出所が後から辿れること。

### 4.7 よくある保存形の書き方

```python
# 1. 同一条件で 24 枚
return Stack(arr)                                  # (24, H, W)

# 2. 露光 8 通り x 各 16 枚
return Series(arr, conditions=Values(exposures, "exposure", "ms"))   # (8, 16, H, W)

# 3. 露光 8 通り x 各 1 枚 (繰り返し 1 を明示する)
return Series(arr, conditions=Values(exposures, "exposure", "ms"))   # (8, 1, H, W)

# 4. 同一条件の 24 枚 + 撮影時刻 (σ_t は有効なまま)
return Stack(arr, timestamps=Values(t, unit="s"))  # (24, H, W)

# 5. カラー1枚
return Frame(img)                                  # (H, W, 3)

# 6. 3枚の stack (以前は 3ch と誤読された形)
return Stack(arr)                                  # (3, H, W)

# 7. 別々に撮った dark と flat
return Batch([Stack(d, name="dark"), Stack(f, name="flat")])
```

### 4.8 dtype

- 配列の dtype がそのまま「このデータの型」として表示される (`u8/u16/i32/f32/f64`)
- viewer 内部は f32。**u8/i8/bool/u16/i16/f32 は値を保ったまま f32 に載る**
  (画素値は [DN])
- **`u32`/`i32`/`f64` は載りきらない。** f32 の仮数は 24bit なので、整数は
  2^24 = 16,777,216 まで正確で、そこから先は別の整数に落ちる (ファイルの
  16777217 は 16777216 になり、u32 の最大値 4294967295 は u32 が取り得ない
  4294967296 になる)。f64 は無条件に桁を失い、有限の 1e300 は inf、1e-300 は 0
  になる。
  **落ちた分は数えて申告する** — 変換のその場で「この配列の何サンプルが
  ファイルの値でないか」を実測し (`rp::F32Loss`、core/remote_proto.h)、
  Inspector が min/max の直下に琥珀色で出す。値が全部収まる f64 ファイルは
  **何も言わない**: dtype を見て警告するのではなく、配列を測って報告する。
  そして表示側は、**保証できない桁を保証できるかのように出さない** —
  整数 dtype で |値| >= 2^24 は `~16777216` のように印が付く (`fmtVal`,
  core/main.cpp)。ROI 統計・ヒストグラム・エクスポートは f32 の標本を畳むので
  依然としてファイルの値ではなく、**それを黙らないことが今の約束**である
  (`--precision-selftest`)
- `bfloat16` など numpy に無いものは f32 に変換し、**変換したと note に残す**
- 非数値 (文字列・object・complex) は拒否。理由を言う

### 4.9 失敗のしかた

例外を投げる。メッセージがそのままユーザーに出る。

```python
raise ValueError("header says 12 planes but the file holds 9")
```

**黙って空を返さない。**「開けませんでした」だけの表示にはしない — 理由が要る。

### 4.10 なぜ2通りなのか

**素の配列** — 学ぶことがゼロ。native と同じ読み方なので、既に知っている規則しか
使わない。1行で書ける。

**dataclass** — 型名が層モデルそのもの (`Frame` / `Stack` / `Series` / `Batch`)
なので、書きながら語彙が身につく。そして**構築した行で落ちる**:

```python
from viewer_import import Stack, Series, Frame, Batch, Values

def load(path):
    z = np.load(path)
    return Series([Stack(z["data"][i], name=f"{e:g}ms") for i, e in enumerate(z["exp"])],
                  conditions=Values(z["exp"], "exposure", "ms"))
```

構築時に検査するもの: 名乗った層に対して shape が合っているか (§4.4) /
`conditions` 長 == stack 数 / `timestamps` 長 == frame 数 / `range` の形 (§4.3.3) /
`cfa` が既知のパターンか / 非数値 dtype の拒否。

**これが型の最大の利得**: 間違いが *harness が走ったあと* ではなく
**間違えた行**で、その行の文脈つきで報告される。

```
File "adapters/acme.py", line 12, in load
    conditions=Values(z["exp"], "exposure", "ms"))
ValueError: Series: conditions has 8 value(s) but there are 16 stack(s)
```

`viewer_import.py` の約束:

- **単一ファイル・標準ライブラリのみ**。numpy すら import しない
  (配列は触らず持ち回るだけ)。**コピーして持っていける**こと
- torch も import しない。`__array__` / `.numpy()` を duck typing で見るだけ
- dataclass は `frozen=True`。返したあとに誰も書き換えない
- **import できない環境でも素の配列は返せる** — これが2通りにしておく実利。
  型は表現力のために足すのであって、依存を増やすためではない
- **素の配列と `Stack(arr)` は、画素も構造も同一**。違うのは `__note_0` だけで、
  素の配列には「どう読んだか」の note が付く。harness の検査項目にする —
  2通りが食い違うくらいなら1通りのほうがマシ

### 4.11 harness が引き受けること (ユーザーが書かなくてよいこと)

- torch → numpy (`.detach().cpu().numpy()` 相当、duck typing)
- 非連続配列の連続化、endianness の正規化
- 返り値の検査 (形・dtype・`conditions`/`timestamps` の長さの整合)
- **.npz への書き出し**。予約メンバ:
  - 木: `__n` (ノード数) / `__layer_<i>` (`frame` `stack` `series` `batch`
    `analysisset`) / `__parent_<i>` (親のノード番号、根は -1)。深さ優先で
    並べ、ノード 0 が adapter の返り値。**series と stack を区別できる唯一の
    手段なので必須**
  - set の束縛 (2026-08-07 追加、[reader-analysisset.md](reader-analysisset.md)
    §4): `__role_<i>` (親が analysisset のメンバが演じる役割名) /
    `__refs_<i>` (set ノードに置く JSON の参照 —
    `{"dark": {"path": "...", "member": ""}}`)
  - 画素と付随物: `__pixels_<i>` / `__cfa_<i>` / `__name_<i>` / `__note_<i>` /
    `__range_<i>`
  - 軸: `__conditions_values_<i>` / `__conditions_name_<i>` / `__conditions_unit_<i>` /
    `__timestamps_values_<i>` / `__timestamps_name_<i>` / `__timestamps_unit_<i>`
  - metadata: 根は `__meta_<k>`、ノード i>0 は `__meta_<i>_<k>`。値は JSON で
    書く (`{"value":6.0,"unit":"dB"}` は配列ではない)
- 1行の要約を stderr に出す (viewer がそのまま中継する)

**単位が空の軸の書き方 (2026-08-03 に確定)**: §4.3.2 の「単位が空でも値は捨てない」
はここにも効く。harness は**値と名前を書き、単位を空のまま**書く。
**空の単位が「適用されていない」という記録そのもの**であって、
メンバごと落とすことではない — 落とすとユーザーが測定値を打ち直す羽目になる。
要約では従来どおり skipped に数える (両方向を数えるため)。

**npz を運び屋にするのは、viewer が既に読める形式だから** — 転送のために
新しいパーサを増やさない。これは実装中の「key 付き npz」対応
(docs/npz-design.md) と同じ土台に乗る。二つは競合ではなく、後者が前者を運ぶ。

### 4.11.1 これは「viewer が読める npz」の定義になっている

harness の予約メンバを決めた時点で、**副産物として1つの容器形式が定義された**。
これは転送の都合ではなく、**この道具にとって意味のある形式**になる:

```
__viewer 1                     ← これがあれば viewer 容器、無ければただの npz
__n 3
__layer_0 'series'   __parent_0 -1   __conditions_values_0 ...
__layer_1 'stack'    __parent_1  0   __pixels_1 ...
__layer_2 'stack'    __parent_2  0   __pixels_2 ...
```

**なぜこれが効くか**:

- **adapter は一度だけでよくなる。** 変換して書き出せば、以後そのファイルは
  **native に開く**。毎回 Python を起動する必要がない
- **層と軸と単位と CFA が、ファイル自身に入る。** §1 で「ファイルは自分が何で
  あるかを言わない」と書いた問題が、**このファイルに限っては解決している**
- **新しいパーサが要らない。** npz は既に読める。増えるのは予約メンバの解釈だけ
- **peer もこれを配れる。** 変換結果を peer に置く話 (§4.13.1) が、
  そのままキャッシュにも共有にもなる

**素の npz との区別は `__viewer` の有無ひとつ**。無ければ今日の分類規則
(docs/npz-design.md: メンバを形で分類し、1次元は軸候補) がそのまま働く。
両者が混ざることはない。

**必要になるもの** (別タスク):

| # | 内容 | 状態 |
|---|---|---|
| 1 | `__viewer <version>` を仕様に入れ、読む側が版で分岐できるようにする | **済** (2026-08-03) |
| 2 | viewer 側の**読み手** (adapter 経由でなく直接開く) | **済** (stage 3) |
| 3 | viewer 側の**書き手** (`Save as viewer npz...`)。stack/series をそのまま保存 | 未 |
| 4 | 往復の検査 — 書いて読んで、層・軸・単位・CFA・note が同一であること | 片道のみ (V25) |
| 5 | 版を上げるときの規則 (追加は互換、意味の変更は版を上げる) | **済** |

**1 と 5 について (2026-08-03、stage 3 の実装時に確定)**: harness は
`__viewer 1` を**必ず最初に書く**。読む側は `__viewer` の**有無だけ**で分岐し、
**自分より新しい版は読まずに断る** (意味が変わっているかもしれないため)。
メンバの**追加は互換**、意味の変更は版を上げる。
(2026-08-07 追記) **AnalysisSet を含むコンテナだけが `__viewer 2` を名乗る**
— 版はファイル単位で、set の無いファイルは 1 のまま。旧 viewer が set を
黙って落とす代わりに断るための版上げである
([reader-analysisset.md](reader-analysisset.md) §4)。

**注意**: これを「汎用の交換形式」だと名乗らないこと。npz は zip + npy であって、
`__layer_<i>` の意味を知っているのはこの道具だけ。**この道具のための容器**であり、
外に配るなら仕様書ごと配る必要がある。

### 4.12 パスと関数の対応を憶える

一度指定したら憶える。ただし**1ファイル1エントリでは増える一方**なので、
記憶するのは「規則」:

| 記憶する形 | 例 |
|---|---|
| ディレクトリ + 拡張子 | `//nas/share/2026/** : *.npz` → `adapters/acme.py:load` |
| glob | `*_raw.bin` → `adapters/acme.py:load_raw` |
| 単一ファイル (最後の手段) | `.../odd_one.dat` → … |

- **より具体的な規則が勝つ**
- **有限に抑える**: 単一ファイル規則だけ LRU (既定 64 件)。規則は残る
- **prefs に保存し、一覧で見えて消せる**。見えない魔法にしない —
  後から「なぜこのファイルだけ違う読まれ方をするのか」が説明できること

**キャッシュの鍵**: `(元ファイルの path, mtime, size, adapter ファイルの path,
その mtime, 関数名, モジュールの VERSION 属性があればその値)`。
adapter を編集したら次に開いたときに効く。

### 4.13 どこで指定するか / どこで書くか / 信頼

- **Inspector に「reader」欄**。今このファイルが何で読まれたかを表示し、
  その場で差し替えられる
- **開けなかったときのエラーからも直接**指定できる (§3.2)。読めない理由を告げた
  同じ場所に「reader を選ぶ…」を置く — 探しに行かせない
- 選択肢は**登録した場所**すべてから (§4.13.2)。同梱 (`tools/import/adapters/*.py`)
  はその列の**最後**に必ず居る
- **新規作成/編集はユーザーのエディタで、v1 から**する (2026-08-03 決定)。
  「New reader...」がテンプレートを書き出して開き、「Edit reader」が今の
  reader を開く。起動は `$EDITOR` → `code -g <file>:<line>` → OS 関連付け の順に試す。
  内蔵エディタは作らない (ImGui でテキストを書かせる仕事ではない)
- **明示的に選んだものだけが走る。** 自動探索も自動適用もしない
  (記憶された規則は「一度ユーザーが選んだ」ことの記録であって発見ではない)
- 起動前に**実際のコマンドを表示**する。Python が無ければその事実を言う

#### 4.13.0 Reader パネル — 書いて・読ませて・直す場所 (2026-08-03 決定)

ユーザー提起:「file browse をダブルクリック → 読めないやつは別パネルが開く →
そのパネルで関数選ぶか、別 Editor 起動したコードを書く → 読み込みで、読む」。

**採用する。** 初版は Inspector の欄とエラー行からの選択だけを書いていたが、
それは**一度きりの選択**を想定した形だった。adapter を書く作業は
**書く → 読ませる → 失敗を見る → 直す → もう一度読ませる**の反復で、
モーダルはその輪を毎回断ち切る。**閉じないパネル**なら輪がそのまま回る。

```
+- Reader ---------------------------------------------------+
| capture_0001.mrw                                            |
| shape (1, 3648, 5472) is not a native form                  |
|   native reads (H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)  |
|                                                             |
| v where readers are looked for (3 registered)               |
|   [Add folder...] [Add file...]  first match wins           |
|   [^][v][x] C:/proj/readers      (folder, 2 reader(s))      |
|                acme.py                                      |
|                util.py                                      |
|   [^][v][x] //nas/team/readers   (folder, 9 reader(s))      |
|                util.py  (shadowed by C:/proj/readers/…)     |
|   [^][v][x] D:/old/share         (MISSING)                  |
|             .../tools/import/adapters                       |
|                     (folder, 3, ships with the viewer)      |
|                                                             |
| reader  [ C:/proj/readers/acme.py ] [load v] [Edit] [New…]  |
|                                                             |
|                                    [ 読み込み ]              |
| ----------------------------------------------------------- |
| acme.py:load(capture_0001.mrw): read 1 stack, 24 frames     |
|                                                             |
| Traceback (most recent call last):                          |
|   File "adapters/acme.py", line 12, in load                 |
| ValueError: Series: conditions has 8 value(s) but there     |
|             are 16 stack(s)                                 |
+-------------------------------------------------------------+
```

- **開き方**: 読めないファイルをダブルクリック / 拒否メッセージの「reader を選ぶ…」/
  Inspector の reader 欄。**入口は3つ、行き先は1つ**
- **[New…]** はテンプレートを書き出して**外部エディタで開く**。**[Edit]** は今の
  reader を開く。エディタは `$EDITOR` → `code -g <file>:<line>` → OS 関連付け
- **[読み込み] は何度でも押せる**。キャッシュの鍵に adapter の mtime が入って
  いるので (§4.12)、**編集して押せば必ず再実行される**
- **失敗は traceback を丸ごと出す。** ここが adapter 作者の唯一のデバッグ面で、
  「開けませんでした」に丸めたらこの機能は使い物にならない
- **成功しても閉じない。** 続けて別のファイルを適用することが多いので、
  パネルは開いたまま、対応づけは記憶される (§4.12)
- フォルダにも効く (§4.1 で `path` はフォルダのこともある)
- **選べるものの一覧は「探索パスそのもの」** (§4.13.2、2026-08-10)。
  「どこを探すか」と「何を選べるか」は同じ問いなので、一覧は1つ。
  以前ここは *同梱の木* と *ローカル1フォルダ* という**半分ずつの答え**2つで、
  そのせいで「2つ目の置き場を言う場所が無い」(#51) が起きていた

#### 4.13.1 リモートでも v1 から動かす (2026-08-03 決定)

データが向こうにあるのに adapter を手元で走らせると、**生ファイルを転送してから
変換する**ことになり、この道具の設計思想 (見えているものと数値だけが流れる) に
反する。したがって **adapter は peer 側で走る**。

- adapter は**テキストなので protocol で送れる**。peer は受け取った関数を
  一時ディレクトリで実行し、**npz を peer 側に置く**
- **変換結果は回線を渡らない。** peer は今 npy に対してやっているのと同じく、
  TILE (見えている領域) と MEASURE (測定結果) だけを返す。
  「見えているものと数値だけが流れる」は adapter 経由でも成立する
- peer に Python / numpy が無ければ**そう言う** (推測して黙って失敗しない)。
  そのときの逃げ道は「手元で走らせる (ファイルが流れます)」を明示的に選ばせる
- 選択はローカルで行われたまま。**peer が勝手に adapter を探すことはない**

#### 4.13.2 リーダの置き場所は「登録する」(2026-08-10 裁定、issue #51)

**裁定 (ユーザー、2026-08-10)**:

> reader のファイルもしくはフォルダを登録できるようにすればよいですかね．

**それまでの形が欠陥だった**: 探索パスは**単数かつ決め打ち**で、同梱リーダは
`run_adapter.py` の隣の `adapters/` 固定、ユーザー側は `app.readerLocalDir` の
**1フォルダだけ**。プロジェクト毎・共有・個人という**当たり前に複数ある置き場**を
言う場所がどこにも無く、2つ目の場所に置いたリーダは**存在しないのと同じ**だった。

**決め**: 置き場所は**列 (list)** になる。1つの場所 (place) は

| 種類 | 意味 |
|---|---|
| **ファイル** | そのファイル1つがリーダ (scratch に転がっている1本。フォルダを作る価値が無い場合) |
| **フォルダ** | その中の `.py` すべて (名前順)。**再帰はしない** |

**ファイルかフォルダかは記憶しない。** 見に行った瞬間にディスクに訊く ——
種類を保存すると、フォルダをファイルに置き換えた日に prefs と実体が食い違う。
ファイルシステムが既に持っている事実の2つ目の写しは作らない。

##### 探索順 — 登録順、同梱は最後

**登録した順に見て、最後に同梱の `adapters/` を見る。**

- **なぜ「登録が同梱より先」か**: 同梱リーダは**例**であり、それが置かれた
  install tree は**更新が入れ替える**。同梱が勝つ順序だと、更新で
  `npz_keys.py` が同梱に増えた日に、ユーザー自身の `npz_keys.py` が
  **誰も設定を触っていないのに**負ける。逆順なら更新が名前を奪うことは
  構造的に起きない。パッケージ版の install tree が読み取り専用でも
  上書きの道が残る、という実利もこちら側にある
- **なぜアルファベット順や更新順ではなく「ユーザーの順」か**: 順序は
  **ユーザーが見ることも変えることもできる唯一の順序**であり、
  「先勝ち」は順序が動かないときにしか答えになれない。だから順序は
  設定そのもので、UI で上下に動かせて prefs に順序ごと保存される

##### 同名衝突 — **先勝ち。ただし負けた側を名指しする**

`alpha.py` が2つの場所にあるとき、**先に登録された場所が勝つ**。

**「衝突したら拒否する」を採らなかった理由**: それは**場所を登録する行為を
破壊的にする**。たまたま `util.py` を含むフォルダを1つ足しただけで、
今まで動いていた `util.py` が全部止まる。**登録は安全な操作でなければならない** ——
安全でなければ人は登録しなくなり、この issue は元に戻る。

**代わりに払う代償は「黙らないこと」**。この repo が他の場所で拒んでいる失敗は
「衝突すること」ではなく「**推測が黙って別の答えを出すこと**」なので、そこだけを
潰す:

- Reader パネルの場所一覧で、負けた側の行に `(shadowed by <勝った方>)` が付く
- 実行時、負けた候補があった場合は**どれが勝ちどれを蹴ったか**を Messages に残す
- 断り文 (下) には**探した場所が全部**並ぶ

##### 断り方 — 「探した場所の一覧」であって1つの推測ではない

§3.2 の三部構成 (名指し・理由・次にすること) をそのまま探索パスに適用する。
**「reader が見つからない」は、どこを見たかを言わなければ直しようがない**:

```
nowhere.py: no registered place holds a reader by that name
  looked in 4 place(s): 3 registered, in the order they were registered,
  then the one that ships with the viewer:
    1. C:/proj/readers          (folder, 4 reader(s))
    2. //nas/team/readers       (MISSING - registered, but not there now)
    3. C:/scratch/odd_one.py    (file)
    4. C:/viewer/tools/import/adapters  (folder, 3 reader(s), ships with the viewer)
  register the file, or the folder it is in: Reader panel >
  where readers are looked for > Add file... / Add folder...
```

**消えた場所は落とさず MISSING と言う。** 黙って捨てると、先週まで見つかっていた
リーダが今日見つからない理由がどこにも無くなる。共有が今朝落ちていて午後には
戻る、はこの分野の日常であって、こちらが勝手に登録を取り消してよい事件ではない。

##### 憶えている対応 (§4.12) との関係 — **登録は記憶と争わない**

issue #51 のもう半分「リーダのファイルが消えたとき憶えている対応をどうするか」:

- **記憶が持つのはパスである。** そしてパスを名乗る spec は**探索を通らない** ——
  したがって**場所を足しても、既に憶えているファイルの開き方は絶対に変わらない**。
  これは約束ではなく構造 (`resolveReaderFile` の最初の分岐)
- **憶えているリーダが消えていたら、断って報告する。** 同名のリーダが登録済みでも
  **黙って差し替えない** —— §4.13 の「明示的に選んだものだけが走る」は、
  ユーザーの背中側での置換を含めて禁じている。文面は「消えた」ことと、
  「同名のものがここに登録されている」ことの両方を言い、**選ぶのは人**:

```
mine_alpha.py: the reader this file was opened with is not there any more
  C:/old/readers/mine_alpha.py  (remembered, gone)
  looked in 2 place(s): ...
  a file named mine_alpha.py IS registered:
    C:/proj/readers/mine_alpha.py
  choose it in the Reader panel - nothing runs until you do
```

選び直せば §4.12 の記憶がその場で新しい方に更新される (1クリック)。

##### 保存場所と上限

`prefs.txt` に **`readerplace <path>` を登録順に**書く (`savePrefs`、書き込みは
PR #114 以来アトミック)。**順序が設定の一部**なので行の順序がそのまま意味を持つ。
上限 32。置き換えられた `readerdir` (単数フォルダ) は**読むだけ続ける** ——
最初の place として読み込まれるので、既存の `prefs.txt` は昨日と同じリーダを
見つけ続け、次の保存で新しい書式に移行する。書き出しはもうしない。

**登録はべき等で、その場に無いパスは登録できない** (登録の瞬間だけが、それについて
有益なことを言える唯一の機会だから)。後から消えた場合は上の MISSING で報告する。

##### Preferences パネル (#50) との関係

ここで作った UI は **Reader パネルの中の一覧 + 追加/削除/並べ替えだけ**で、
設定パネルは作っていない。#50 で JSONC の設定が着地したら、
**`readerplace` の列と `pythonexe` はそちらへ移る**べき設定である
(順序が意味を持つので、JSON の**配列**がそのまま正しい表現になる)。
**`readerfor` (パス→リーダの記憶) は移さない** —— あれは設定ではなく
**ユーザーが行った選択の記録**で、手で編集する設定ファイルの住人ではない。

---

## 5. 何が良くなるか

- **推測が消える。** native は次元数だけで決まり、`--npy-axis` は要らなくなる
- **形式追加が C++ の変更でなくなる。** ビルドもリリースも要らない
- **provenance が正確になる。** どの adapter の何版が作った数値かが、
  エクスポートの出所行 (いまコミットハッシュを出している行) に並ぶ
- **Watch と噛み合う。** 監視対象は**元ファイル**、変化したら adapter を通して
  読み直す (docs/watch-design.md の再読込がそのまま使える)
- **1形式1ファイルの責務。** 壊れたときにどこを直すか自明

## 6. コスト・危険

- **信頼**: 外部プロセスを起動する。明示設定のみ、実行前に何を起動するか表示する
- **速度**: 起動 + シリアライズのオーバーヘッド。大きな stack はキャッシュが要る
- **Python 依存**: viewer 本体は C++ だけで動く。adapter を使う人だけが Python を要る
- **読めるものが狭くなる**: §3.4 の移行。**これは意図した代償** —
  黙って間違えるより、読まずに理由を言うほうがよい
- **嘘の宣言は検証できない**: adapter が「これは 8 条件だ」と言えば viewer は信じる。
  検証できるのは形の整合だけ (長さ・次元・dtype)。それは harness が必ず行う

## 7. 段階

| # | 内容 | 検証 |
|---|---|---|
| 0 | **native を §3.1 に絞る** + 読まないときの言い方 (§3.2) + HWC 猶予 | 既存 selftest が緑のまま / `(H,W,3)` が note つきで開くこと / 1次元と5次元が理由つきで拒否されること |
| 1 | 返り値の契約 (§4.1-4.8) + `tools/import/viewer_import.py` (型3段) | Python 側だけで完結。3段が同じ結果を出すこと |
| 2 | harness `run_adapter.py` + 同梱 adapter (key 付き npz / HWC) + テンプレート | 手で叩いて npz が出ること |
| 3 | viewer 側: 起動・npz 受け取り・Inspector の reader 欄 | **済** V25a-h |
| 4 | パス→関数の規則 (prefs 保存・一覧・LRU) | **済** V25k。v1 は**1ファイル1エントリのみ** (§8-7)、規則は未 |
| 4b | リーダの置き場所の登録 (ファイル/フォルダ、順序、断り文) §4.13.2 | **済** V25n / V25o (#51) |
| 5 | キャッシュ (元ファイル + adapter の mtime で無効化) | **済** V25i |
| 6 | 返り値の検査と断り方 | **済** V25g / V25j |
| 7 | peer 側での実行 (リモート) | 既存の remote selftest に1本足す |

## 8. 決めてほしいこと

1. **この方向で進めるか** (推奨: 進める。npz native 修正は独立で先行)
2. ~~native を3形に絞るか~~ **決定 (2026-08-03)**: `(H,W)` / `(H,W,C≤4)` /
   `(F,H,W)` / `(F,H,W,C≤4)` の4形。カラーは残し、判定は**最後の軸だけ**を見る
   —— **最後の軸が 4 以下なら channel** (2026-08-05, issue #71 で確定。
   ここは以前 `(H,W,3|4)` と書いていたが、規則を表として述べている箇所は
   §3.1 も §4.13.0 も一貫して「4 以下」で、`3|4` は拒否メッセージの
   写しでしかなかった。`(H,W,1)` は**モノクロ1枚**、`(H,W,2)` は**2ch 1枚**)
3. ~~HWC の猶予~~ **決定**: 猶予ではなく**恒久的に native**。deprecated にしない
4. ~~`--npy-axis` を廃止してよいか~~ **決定**: 廃止。後継は §3.3 の
   Inspector での言い直し (ファイルごと・可視・セッションに残る)
5. ~~層の宣言の語~~ **決定 (2026-08-03)**: `Stack` に **`timestamps`**、
   `Series` に **`conditions`** (複数形 = メンバ1つにつき1つ = 長さの規則)。
   値の型は **`Values(values, name, unit)` の1つ**で、`timestamps` の名前は
   `"time"` が既定。共変量は v1 では受けず、将来 `covariates` を別に足す。
   `layout` は廃止 (軸の順序を合わせるのは書いた人の仕事)
6. ~~bfloat16~~ **決定**: f32 に変換し、変換したことを note に残す
7. ~~記憶する規則~~ **決定 (暫定)**: v1 は**1ファイルごとのキャッシュ**だけ。
   規則 (フォルダ・glob) は作らない。locality が高い運用なので、将来は
   **pyenv 的な「親フォルダを辿って設定を見つける」形**を検討する —
   ただしそのときは**信頼の規則が要る**: データフォルダ (共有・NAS のことがある)
   に置かれた設定が、他人の書いた Python を黙って実行させうる。
   設定ファイルごとに一度承認させ、path+内容ハッシュで覚える形になる
8. ~~内蔵エディタ~~ **決定**: 内蔵は作らないが、**エディタ起動は v1 から入れる**
   (`$EDITOR` → `code -g` → OS 関連付け)
9. ~~画素値の単位~~ **決定**: 不要。**[DN] 固定**
10. ~~部分読み込み~~ **決定**: `only=` の契約は v1 で決め、実装は後。
    adapter が勝手に一部を返すのは「部分読み」ではない (§4.1 の理由)
11. ~~ローカル先行か~~ **決定**: **remote 込み**。adapter は peer 側で走る (§4.13.1)
12. ~~リーダの探索パスが単数かつ決め打ち~~ **決定 (2026-08-10, issue #51)**:
    **ファイルまたはフォルダを登録**する列にする。探索順は**登録順 → 同梱が最後**、
    同名は**先勝ちで負けた側を名指し** (拒否にすると登録が破壊的操作になるため)、
    消えた場所は **MISSING と報告して落とさない**、憶えているリーダが消えたときは
    **報告するだけで同名への差し替えはしない**。仕様は §4.13.2
