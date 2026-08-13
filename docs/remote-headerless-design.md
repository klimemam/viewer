# リモートのヘッダ無し RAW — レシピがリンクを渡る設計 (G1)

**出発点はユーザの実報告 (2026-08-11)「remoteで、.rawが開けないね」。**
[verify-matrix.md](verify-matrix.md) §7 G1 が確定したとおり、これは判断ではなく
**落穂**である — ヘッダ無し RAW (`.bin .raw .yuv .dat .rggb`) は
`core/imagefile.h` の表に行が無く、`imagefile::peerServes()` が false を返し、
拒否文は generic な fall-through に落ちる。**誰もこの形式がリンクを渡れないと
決めていない。** この文書はその道を開く設計であり、G1 の選択肢 (b)
「レシピを wire に載せる」を採る。ただし (a) の半分 — **断りに名前を付ける** —
を第1段として先に出荷する (§11)。

対象セルは 4つ: 表A の F9×D7 / F10×D7、表C の F9×O1(ssh) / F10×O1(ssh)
(verify-matrix §5 の数え方どおり、`local:// | ssh://` で割れたセルは 1と数え、
G2 [PR #174 済] と共有する local 側 2セルは含まない)。§1–§6 のセルは測定の
記録なので書き換えない — 変わるのは §7 G1 の状態行である。

---

## 0. 前提 — 動かないもの

この設計が**前提として受け入れ、動かさない**もの。以降の全決定はこの上に立つ。

1. **ヘッダ無しファイルの幾何は人の宣言である。** `core/imagefile.cpp` が
   `.raw` に表の行を与えなかった理由そのもの (「ヘッダの無いファイルについて
   ライブラリが『自分のほうが知っている』と言う場所ではない」、
   `core/imagefile.cpp` の vendor RAW 行のコメント)。ライブラリにも **peer にも**
   推測させない。peer が知ってよいのは「言われたこと」だけである。
2. **`MSG_TILE` は復号済み画素を返す** (`core/remote_proto.h` 冒頭、および
   VERSION=10 の注記「there has never been a send-a-file op」)。
   「生バイトを送って手元で解釈する」は既存規約に反するし、要らない —
   ヘッダ無し RAW の「復号」は宣言された幾何での seek + サンプル読みであり、
   peer 側で `.npy` と同じ形 (`readNpyRegion`) にそのまま載る (§7.2)。
3. **拒否は三部構成** — 名指し・理由・逃げ道 ([input-adapters.md](input-adapters.md) §3.2)。
4. **measuring 級は結果に自分を書く (M1)** ([settings-inventory.md](settings-inventory.md) §4.1)。
   幾何・ヘッダ長・バイト順・dtype は画素の値そのものを変えるので、リモートで
   開いた doc も宣言文 (`rawRecipeSummary` の一句) を持つ (§9)。
5. **古い peer は名指しで断る** — `rp::pictureTooOldText` の形。番号から、
   **送る前に**、どのミスマッチかを言う (§8)。
6. **同じファイルがローカルとリモートで違う読まれ方をするのは欠陥**
   (input-adapters.md §3.1 末尾、#71 / #148 の教訓)。この文書の検証はすべて
   「local の `decodeRawFrame` とビット一致」を軸にする。

---

## 1. 決定の要約

| 問い | 決定 | 節 |
|---|---|---|
| 1. レシピはどこに載るか | **(a) 各要求に毎回積む** (META / TILE / MEASURE のトレーラ)。セッション登録の口は作らない。識別子: レシピは `srcIdentityKey` に**既に**入っている (raw 枝)。リモート raw doc が同じ欄を埋めるだけで、新機構ゼロ | §3 |
| 2. `peerServes` を割るか | **割る。** `peerServes` (無宣言で答える) はそのまま、`peerServesDeclared` (宣言が届けば答える) を足す。`peerRefusal` はヘッダ無し専用の一節を得る | §4 |
| 3. 戸 (UI) | ダブルクリック → #166 の size 束縛 → 無ければ RawDialog (サイズは `MSG_LIST` が**既に**返している)。1クリックは選択のまま (preview 無し、判断として記録) | §5 |
| 4. stack と preview | ファイル内 frame 軸は**無し** (ローカルの戸が無いから)。stack はフォルダ連番 (peer の SCAN/LIST grouping を widening)。preview の step 循環は「幾何は人から、META より前に来る」ので**存在しない** | §6 |
| 5. プロトコル番号 | **10 → 11 で足りる。** 4通りの組み合わせは §8 の表 | §8 |
| 6. MEASURE | **要る。** 同じ経路 (トレーラ、`head.flags` bit0)。v1 は `MOP_TEMPORAL_STATS` / `MOP_FRAME_ROI_STATS` のみ、set/plugin は名指しで断って記録 | §7.3 |
| 7. 段階分け | **4段。** ①断りに名前 (wire 不変) ②protocol 11 の wire + peer + CLI 戸 ③MEASURE + stack ④Browse 戸 + 束縛 + セッション | §11 |

---

## 2. 語彙

既存の語だけで書く。新語は 1つも作らない。

- **レシピ (recipe)** — #166 の `RawRecipe` そのもの: (w, h, offset, dtype,
  interp, littleEndian, cfaPattern, crop)。`core/app/loader_npy_raw.inc` の
  3つの表 (`RAW_DTYPE_NAMES` / `RAW_INTERP_CLI` / `CFA_PATTERNS`) の語彙。
- **宣言 (declaration)** — #124 の declared reading と同じ意味: 推測ではなく
  ユーザが言ったこと。レシピは宣言の一種である。
- **wire レシピ** — レシピのうちリンクを渡る部分集合 (§7.1)。crop と
  cfaPattern は渡らない (§3.2 に理由)。
- stack / frame / series / batch は [terminology.md](terminology.md) のまま。
  ヘッダ無し RAW の 1ファイルは常に 1 frame (§6.1)。

---

## 3. 問い1 — レシピはどこに載るか

### 3.1 決定: (a) 各要求に毎回積む。セッション登録の口 (b) は作らない

**(a) にする。(b) だと peer が状態を持つことになり、今日ゼロである状態が
生えるから。** 具体的に:

- 今日の peer は**要求ごとに無状態**である。`ServedFile` は 1要求の寿命しか
  持たず (`core/serve.cpp` の struct コメント)、handleMeta / handleTile /
  handleMeasure はどれも毎回ファイルを開き直す。「この path はこの形で読む」と
  いう登録を作った瞬間、(i) client が落ちたら誰が消すのか、(ii) 再接続で
  登録は残るのか、(iii) 2つの client が同じ peer に別のレシピを登録したら
  どちらが勝つのか、という**寿命と所有の問い**が 3つ生える。毎回積めば 3つとも
  存在しない。
- **前例がそう決めている。** #124 の declared reading はまさにこの形で入った:
  `[str path][TileReq][u32 read]`、「the reading has to travel with them or
  they mean a different region than the caller meant」(remote_proto.h の
  TileReq コメント)。レシピはこの論理の**強い版**である — TILE の rect と
  step は幾何の**内側の座標**であり、その幾何はレシピが決める。レシピを
  別便で送れば、rect が何を指すかが要求だけからは読めなくなる。
- **コストは測って小さい。** wire レシピは u32×6 = **24 バイト/要求** (§7.1)。
  4K フレーム 1枚の TILE 応答 (u16 で 16.6 MB) の 0.00014% であり、
  節約する価値のある数字ではない。

(c)「両方」は (b) を含むので同じ理由で落ちる。

### 3.2 識別子との関係 — レシピは §6.2 の鍵に既に入っている

**同じファイルを違うレシピで開いたら別の絵である。したがってレシピは
identity に入らなければならない — そして既に入っている。**
`srcIdentityKey` (`core/app/state.h`) の raw 枝:

```cpp
if (s.rawDtype >= 0)    // the raw recipe (incl. its dims) decides the pixels
    snprintf(t, ..., "\n%d|%d\nraw%d,%d,%d,%d,%dx%d\n%lld,%llu",
             fileFr, peerFr, s.rawDtype, s.rawInterp, s.rawOffset,
             (int)s.rawLE, s.srcW, s.srcH, ...);
```

鍵には (dtype, interp, offset, LE, srcW×srcH) — **wire レシピと同じ集合** —
が入る。ローカルの raw open はこの枝を通っており、2つのレシピは 2つの tuple、
決して共有しない。**この設計がやることは、リモート raw doc が同じ欄
(`FrameSource::rawDtype/rawInterp/rawOffset/rawLE/srcW/srcH`) を埋めることだけ**
である。すると:

- 第1欄は `srcKeyPath(url)` — ssh:// は逐語、local:// はディスクパスに解決
  (reference-design.md §6.2 の 2026-08-11 追記のまま)。つまり **local:// の
  raw open と File ▸ Open の raw open は同じ tuple** に落ちる — が、実際には
  local:// の raw は #111 分岐で `openPath` に再ルートされる (§5.4) ので、
  この一致は保険であって経路ではない。
- ssh:// は mtime/fsize 0/0 のまま (peer のディスクは stat できない)。同じ
  url + 同じレシピの 2度目の open は共有する — `.npy` のリモートと同じ意味論で、
  サーバ側の変化検出は Watch の仕事 (state.h のコメントどおり)。**新しい穴では
  ない**が、raw で初めて効く場所なので記録しておく。

**鍵に入れないものが 2つあり、どちらも理由がある:**

- **cfaPattern。** RGGB と BGGR は**画素の値を 1ビットも変えない** — 変わるのは
  プレーンの名札だけである。現実装がそう扱っている: `decodeRawFrame` は
  pattern を `ImageDoc` (membership 層) に置き、`FrameSource` には置かず、
  鍵にも入れていない。MEASURE は `MeasureReqHead.cfaType/cfaPattern` で要求ごとに
  運ぶ (これも現行のまま)。wire レシピに pattern を入れないのは同じ判断の
  wire 側であって、新しい判断ではない。
- **crop。** crop は「同じ frame の再スコープ」であって別の frame ではない —
  `srcShareable` が crop 済み raw source を registry から外す
  (`state.h`: `s.w == s.srcW && ...`) のが現行の答えで、wire にも載せない
  (§6.3)。

### 3.3 #166 の size 束縛は client に留まる

束縛 (`g_rawSizeRecipe`) は**プロセス内のみ・書き残さない**が #166 の核心
(loader_npy_raw.inc の長コメント: 「安全なのはユーザが数分前に自分で選んだ
ことをまだ憶えているから」)。この性質は**ユーザについての事実**であって、
バイト列がどちらのディスクに在るかと無関係である。だから:

- 束縛は**リモートのファイルにも効く** (§5.2)。鍵は同じくバイト数。
- 束縛は **peer に送らない・peer は憶えない**。憶える peer は (b) の
  セッション登録を裏口から作ることになる。

---

## 4. 問い2 — 述語を割る

### 4.1 現状と第三の状態

`imagefile::peerServes(name)` は今、2つの問いに 1つで答えている:
(i) Browse の行を活かすか、(ii) META/TILE に答えが存在するか。
`.raw` は「**一覧には出るが、宣言無しでは答えない**」という第三の状態であり、
1つの bool では言えない。#111 (§3.6.4「述語は2つある」) と同じ形の解で割る。

### 4.2 決定: 述語は 2つ + 拡張子の表は 1つ

```
imagefile::isHeaderless(name)      // 新設: .bin .raw .yuv .dat .rggb (小文字比較)
imagefile::headerlessExts()        // 新設: その一覧、散文とフィルタ用
imagefile::peerServes(name)        // 不変: .npy + 表の overLink 行 = 無宣言で答える
imagefile::peerServesDeclared(name)// 新設: peerServes(name) || isHeaderless(name)
```

**なぜ表 (`HEADERLESS_EXTS`) が `imagefile` に移るか。** 今この配列は
`core/app/sequence.inc` の頭にいる (G8 / PR #175 が 6箇所のリテラルを畳んだ
場所) — が、**peer のバイナリは sequence.inc をコンパイルしない**。peer は
SCAN の grouping (`seqSegKey`) と openServed の分岐で同じ問いを訊くので、
表が client 側にしか無ければ #148 が潰した「一覧が2つあればずれる」を
そのまま再演する。`imagefile.cpp` は**両方のバイナリがコンパイルする唯一の
形式の家**なので、そこへ移す。`sequence.inc` の `extIsHeaderless` /
`headerlessExtList` は**呼ぶだけ**になる (G8 の F4c は配列を歩くので、移設は
F4c が緑のまま通ることが移設の正しさの証明になる)。`SELFDESC_EXTS`
(`.npy .npz`) は動かさない — あれは client 自身の戸の話で、peer は `.npy` を
inline に読み `.npz` を断る現状のまま。

### 4.3 各所が引く述語

| 場所 | 述語 | 変更 |
|---|---|---|
| Browse `rbRowOpenable(host, name)` (host 非空) | `peerServesDeclared` | **変更** — `.raw` の行が生きる |
| Browse 1クリック preview 門 (`rbActivateRow` の `peerServesName` 門) | `peerServes` | 不変 — raw は 1クリック選択のみ (§5.3) |
| `openRemote` の門 (`open_dispatch.inc:1771`) | `peerServesDeclared` + レシピの有無 | **変更** (§5) |
| peer の walk/grouping (`isServedSuffix` / `seqSegKey`) | `peerServes` ∪ (client≥11 なら `isHeaderless`) | **変更** (§6.2) |
| `viewerReadsName` | 不変 (G2/PR #174 で既にヘッダ無しを含む) | — |

### 4.4 `peerRefusal` の文がどう変わるか

`core/imagefile.cpp` の fall-through (`return "the peer serves " + servedList()...`)
の**直前**に、ヘッダ無し専用の一節が入る。三部構成 (名指しは呼び出し側が
前置する):

**第1段 (wire 変更前) の文** — 事実は「まだ運べない」:

```
a headerless .raw states its shape in a recipe on this machine, and this
link cannot yet carry that declaration
  copy the file here and open it with File > Open, or update both ends
  when a recipe-carrying build ships
```

**第2段以降 (protocol 11) の文** — 事実が変わるので文も変わる。v11 peer が
レシピ**無しの**要求 (旧 client) に断るとき:

```
a headerless .raw carries no header to state its shape - this request
carried no recipe (a protocol-11 client sends one; update this viewer)
```

client 側 (新 client / 旧 peer) は §8 の `rawTooOldText` で**送る前に**断る。
G9 (PR #176) の裁定を引き継ぎ、逃げ道は**リンクの限界**を述べる形を保つ —
「browse it locally」はこの分岐に落ちる名前について常に真とは限らない、が
あの PR の学びだった。ヘッダ無しはローカルで開く名前だと**分かっている**ので、
第1段の文は File ▸ Open を名指しできる (fall-through より一段具体的に言える
ことが、この一節を生やす価値である)。

---

## 5. 問い3 — 戸 (UI)

### 5.1 ダブルクリックの流れ

Browse (ssh://) で `.raw` をダブルクリック (`rbOpenRow`):

```
1. rbRowOpenable → peerServesDeclared → true (行は生きている)
2. peer が古い (HELLO < 11) → rawTooOldText を toast、終わり
3. バイト数 sz = 行の Entry::size (MSG_LIST v3 が既に運んでいる)
4. rawRecipeForSize(sz) が答える → そのレシピで openRemote(url, recipe)。
   rawRecipeAppliedLine を toast (ローカルと同文 — 「この session の
   N バイトのレシピ」はディスクの場所を言わないので、そのまま真)
5. 束縛が無い → RawDialog を立てる (fileSize = sz、path = url)。
   Load → openRemote(url, recipe) → 成功なら rawRecipeBindSize(sz, recipe)
```

**ローカルの RAW ダイアログをそのまま立てる。** 別のダイアログを作らない —
候補寸法の推定 (`rawGuessDims`) はバイト数だけで動き、バイト数はある。
変わるのは Load の着地 (`rawDialogAccept`) が path の「://」で分岐して
`loadRaw` ではなく `openRemote(url, recipe)` を呼ぶことだけ。束縛の成立条件
(「成功した decode だけが束縛を作る」) も同文のまま — openRemote の成功が
それに当たる。

### 5.2 ファイルサイズはどこから来るか — **新しい wire は要らない**

- **単独ファイルの行**: `MSG_LIST` v3 のエントリは `[u32 szLo][u32 szHi]` を
  **protocol 3 から**運んでいる (`remote::Entry::size`)。`.raw` は表に行が
  無いので grouping に拾われず**素の行**として返る — つまり今日の peer の
  一覧に既にサイズ付きで載っている。ダイアログにはこれを渡す。
- **グループの行** (§6.2 で grouping が widening された後): グループの size は
  **メンバ総和** (`SeqGroup::bytes`) なので先頭メンバのサイズにならない。
  先頭メンバのサイズは **`MSG_LIST` をそのファイルの path に対して 1回引く**
  ことで取る — handleList はディレクトリでない path を「その 1エントリ」として
  返す (`core/serve.cpp` handleList 冒頭の else 枝)。既存の口・既存の形で、
  round trip 1回。総和 ÷ 枚数で代用しない — メンバのサイズが揃っている保証は
  無く、割り切れたときだけ動く推定は「もっともらしい絵が出る」側の失敗
  (G3 の教訓) だから。
- **peer 側の検算が最後の門**: レシピの `offset + W×H×ch×elem` がファイルの
  実サイズを超えれば、両方の数を名指しして断る (§7.2)。LIST のサイズが
  古かった (書き込み中だった) 場合もここで捕まる。

### 5.3 1クリック preview は付けない (v1 の判断、記録)

**raw の行は 1クリック = 選択、ダブルクリック = 開く。preview は作らない。**

- preview はレシピが無いと**作れない** (幾何が無ければ TILE の rect が無い)。
  レシピを訊いてから preview するなら、1クリックがモーダルを立てることになり、
  「1クリックは選択・ダブルクリックで確定」という一覧の規則
  (input-adapters.md §3.6.4、ローカル picture 行と同じ) を破る。
- 束縛が**ある時だけ** preview する案は、1クリックの挙動がデータ依存で変わる
  第三の振る舞いを作る — §3.6.4 が「読み手が覚える第3の振る舞いを増やさない」
  ために picture 行で棄てた形そのもの。
- 再訪条件も記録する: ユーザが「束縛が効いている間は preview が欲しい」と
  言った日に、この節を読み直して 1クリックの規則ごと裁定する (§10)。

### 5.4 local:// は今までどおり wire を通らない

`local://…/x.raw` は #111 分岐 (`openRemote:1771`: host 空 + `viewerReadsName`)
で `openPath` に再ルートされ、**ローカルの** RAW ダイアログ / 束縛が立つ
(G2 / PR #174 で解決済み)。この設計は ssh:// だけを増やす。表C の
F9/F10 × O1 の `local://` 側が「再ルート後は表B」になるのは `.npz` や
vendor RAW の行と同じ形。

### 5.5 CLI の戸

`viewer ssh://host/data/x.raw --raw-size 4096x3000 --raw-dtype u16
--raw-interp bayer` — `--raw-*` 群 (cli.inc:620 の蓄積) がそのまま wire
レシピになり、ダイアログ無しで開く。**`--raw-size` が無い ssh:// の raw は
断る** (「state the geometry with --raw-size, or open it from the Browse
panel」) — CLI は非対話の戸なので、LIST を引いてダイアログを立てるより
名指しで断るほうが正直 (第2段で入るのはここまで。Browse の戸は第4段)。

---

## 6. 問い4 — stack と preview

### 6.1 ファイル内 frame 軸は**持たない** (判断、記録)

ローカルの戸がそう決めている — 「a raw file is one frame, always」
(`core/app/sequence.inc:1941`)。`decodeRawFrame` に frame パラメータは無く、
offset は「ヘッダを飛ばす」ためであって「N 枚目に seek する」ためではない。
リモートがローカルより多く読めるのは §0-6 (同じファイルに2つの答え) の
違反なので、**META は常に frames=1 を返す**。

- 多フレーム `.yuv` を stack にしたい要望が来たら: それは**ローカルの戸の
  機能追加** (レシピに frames / frameStride を足す) であって、wire はその日に
  フィールドを**追記**する (remote_proto.h の規律「append, never renumber」。
  wire レシピのトレーラは末尾追記が既定の進化路 — §7.1)。v1 では作らない。
  逃げ道は reader (§4.13.1、G11 裁定待ち) か、ファイル分割。

### 6.2 stack = フォルダ連番。grouping の widening は client 版数で門する

peer の walk は 1述語 (`isServedSuffix` = `imagefile::peerServes`) を
`handleScan` の収集と `seqSegKey` (LIST/SCAN 両方の grouping の門) で引く。
これを「`peerServes` ∪ (**HELLO の client 版数 ≥ 11** なら `isHeaderless`)」に
widening する。

**なぜ client 版数で門するか。** 門しないと、v10 の client の一覧に `.raw` の
グループ行が現れる。旧 client はそれを「Open as stack」と申し出て、押すと
generic 拒否に落ちる — 「出して押させて断るより出さない」(§3.6.4 の stack 動詞の
裁定) に反する。LIST の形を `g_clientVersion` で出し分ける前例は v2/v3 で
確立済み (`handleList` 冒頭)。**旧 client の一覧は今日とバイト同一**が門の
仕様である。

開く側: グループ行のダブルクリック →「レシピは Open ごとに 1回」— ローカルの
`g_folderRecipe` + `openId` の規律 (sequence.inc:1573「The recipe belongs to
the OPEN that answered for it」) をそのまま使う。束縛が先に答えるのも同じ
(startNextQueuedGroup:1578 と同文の分岐)。全メンバの META/TILE/MEASURE に
同じレシピが載る。**サイズの合わないメンバは peer が名指しで断り** (§7.2)、
client はその 1 frame を落として stack は n/N を言い続ける — §3.2 の
「形の違う兄弟」の規則の raw 版で、文もその形に倣う。

### 6.3 preview の間引きの循環は存在しない

「step は幅を知らないと決まらないが、幅はレシピが来るまで分からない」— この
順序問題は、**幾何が peer からではなく人から来る**ことで消える。時系列:

```
束縛/ダイアログ (バイト数だけが要る。バイト数は LIST が持っている)
  → レシピ確定
  → META (レシピ同乗) → w/h/ch/frames=1
  → step = ceil(max(w,h)/1600)   ← openRemote:1826 の式そのまま
  → TILE (レシピ同乗、step 付き)
  → 全解像度の follow-up (requestFullRemote、レシピは FrameSource の
    raw 欄から再構成 — RFetchJob が npyRead を rfInheritRead で運ぶのと同型)
```

META がレシピを受けるのは #124 と同じ理由 — 「META's whole job is to say how
big the picture is before a pixel moves, and a reading changes exactly that」
(remote_proto.h の META 注記)。レシピは reading の強い版なので、META に
載らない設計はそもそも成立しない。

crop 付きレシピ: wire には載せない (§3.2)。全解像度が着地してから
`cropInPlace` — `remoteStep > 1` の拒否と G10 (PR #177) の RestoreWait park を
**そのまま**使う。同じ状況に 2つの答えを作らない (G10 の裁定の言葉のまま)。

preview export の嘘は G4 (PR #170) が既に全形式で塞いでいる。継承のみ、作業なし。

---

## 7. wire — protocol 11 の中身

### 7.1 wire レシピの形

`remote_proto.h` に置く。**両方のバイナリが include する 1つの定義** —
NpyRead / NPY_NATIVE_FORMS と同じ理由 (#71: 「A second copy in serve.cpp is
a copy that drifts」)。

```cpp
// The declared geometry of a headerless file, protocol 11. The values of
// dtype/interp are the INDICES of the client's RAW_DTYPE_NAMES / RAW_INTERP_CLI
// tables, frozen here: append, never renumber (session files and the wire
// both carry them).
struct RawWire {
    uint32_t dtype;     // 0 u8, 1 u16, 2 f32, 3 f64
    uint32_t interp;    // 0 gray, 1 rgb, 2 bgr, 3 rgba, 4 bgra, 5 bayer, 6 quad-bayer
    uint32_t w, h;      // 1..MAX_DIM (32768) — serveLayout と同じ天井を peer が検査
    uint32_t offset;    // bytes to skip before the frame
    uint32_t flags;     // bit0 = little-endian; 残りは 0 (将来の追記用)
};                      // 24 bytes
```

数え: u32×6 = 24B/要求。crop (§3.2) と cfaPattern (§3.2)、frames (§6.1) は
**入れない**、それぞれ理由と再訪条件を本文に記録済み。

**載る場所 — 全部「既存の末尾に追記」**。older peer が読む要求は 1バイトも
動かない (このヘッダの全 append が守ってきた規律):

```
META    -> [str path][u32 read][RawWire?]
TILE    -> [str path][TileReq][u32 read][RawWire?]
MEASURE -> head.flags bit0 が立っていれば rois の直後に [RawWire]
```

META/TILE の有無判定は `getRead` と同じ「残りバイトがあれば読む」
(`getRecipe`)。MEASURE だけ flags ビットなのは、op によって rois の後ろに
既に別のブロック (parity / role) が続くから — 位置を「rois の直後」に固定し、
存在を head が宣言する形が、既存パーサに一番安い。`MeasureReqHead.flags` は
「reserved, 0」と宣言済みの欄で、v10 以前の client は 0 しか書かない。

**組み合わせの検査 (peer 側、どれも文を持つ):**

| 要求 | peer の答え |
|---|---|
| ヘッダ無し名 + レシピ有り | 開く (§7.2) |
| ヘッダ無し名 + レシピ無し | 拒否: §4.4 第2段の文 |
| ヘッダ有り名 (npy/picture) + レシピ有り | 拒否: `a raw recipe does not apply to a file that states its own shape` — openPicture の declared-reading 拒否 (serve.cpp:448) の鏡像。黙って捨てない: 「a request whose trailer is silently dropped comes back as a successful answer to a question that was not asked」(protocol 9 の理由文そのまま) |
| レシピ有り + `read != NR_NATIVE` | 拒否: declared reading は .npy の概念 (同上の文) |
| dtype/interp が表の外、W/H が 0 か MAX_DIM 超 | 拒否: 値を quote する。clamp しない (`getRead` の「refused rather than clamped」と同じ理由 — 両端の解釈がずれた印であり、そこから絵を返してはならない) |
| offset + W×H×ch×elem > 実ファイルサイズ | 拒否: `file is N bytes: this recipe needs M (offset O + WxH x C ch x E B/sample)` — 両方の数を言う。ローカルの `file too small for this size/format` より具体的なのは、リモートは LIST のサイズが古び得る分、数字が診断になるから |

### 7.2 peer 側の読み — `.npy` の器で読む

`openRaw(ServedFile&, path, RawWire)` は `openNpy` より**簡単**である
(verify-matrix G1 (b) の見立てどおり):

- `dataOffset = offset`、`dtype` は RD→DT の直訳 (u8/u16/f32/f64 は全部
  `rp::DType` に既在)、`elemSize = dtypeSize`、`w/h` はレシピ、
  `ch = RAW_INTERP_CH[interp]`、`frames = 1`、`fortran = false`、
  `bigEndian = !(flags & 1)`。
- strides は plain C-order (`sCh=1, sX=ch, sY=w*ch`) — つまり
  **`readNpyRegion` がそのまま使える**。row-contiguous 経路・endian 正規化
  (serve.cpp:370)・step 間引き・端の clamp、全部既存のコードで、raw 専用の
  読みループは書かない。
- **1つだけ後処理が要る**: interp が BGR/BGRA のとき channel 0↔2 の swap。
  ローカルの `decodeRawFrame` は swap 済みの並びを doc に置く (`bool sw = ...`)
  ので、peer が swap しなければ同じファイルがリンク越しだけ色が入れ替わる —
  #148 の「1つのフォルダが2つの答え」を 1段下で作ることになる。tile buffer
  への小さな post-pass (ch==3/4 のみ)。
- `ndim = 0` のまま → META に MR_SHAPE は付かない。**宣言済みの幾何に §3.3 の
  「read as」menu は出さない** — picture が `ndim = 0` を保つ理由 (serve.cpp:509
  「a picture declares none」) と同じで、レシピの言い直しは Inspector の
  npyRead menu ではなく #166 の recipe panel の仕事 (`rawRecipeReinterpretCurrent`
  の remote 版が第4段で繋がる)。

dispatch は openServed の 1行:

```cpp
if (!isNpySuffix(path) && imagefile::isHeaderless(path)) return openRaw(n, path, err, recipe);
if (!isNpySuffix(path) && imagefile::forPath(path))      return openPicture(n, path, err, read);
return openNpy(n, path, err, read);
```

**openServed を通すことが設計の半分である** — serve.cpp 自身が書いている:
「teaching those two a second format teaches LIST, SCAN, META, TILE, MEASURE
and the plugin mouth at once. TILE without MEASURE would be a peer that can
show a TIFF and cannot measure it - two answers for one file」。だから peer 側
は META/TILE/MEASURE を**1段で**覚える (§11 段2)。client 側の MEASURE 配線が
別段 (段3) なのは実装量の分割であって、peer の能力の分割ではない。

### 7.3 問い6 — MEASURE

**peer はレシピを知る必要がある。** `runTemporalStats` / `runFrameRoiStats` の
frame 読みは openServed / readRegion を通る (§7.2 で自動的に学ぶ) が、
**要求にレシピが載らなければ開く手段が無い**。経路は同じ (§7.1 のトレーラ)。

- **v1 の範囲は `MOP_TEMPORAL_STATS` と `MOP_FRAME_ROI_STATS`** —
  `maybeRequestServerTemporal` と temporal パネルが発射する 2つで、表C の
  O4 列の実体。head の cfaType/cfaPattern が今日どおりプレーン分割を運ぶので、
  レシピ側に CFA は要らない (§3.2)。
- **`MOP_ANALYZER` / `MOP_PLUGIN_ANALYZE` / `MOP_SET_FOLD` は v1 では断る**
  (client 側で、送る前に、名指しで)。理由を記録する: plugin 2op は rois の
  後ろに自分のブロックを持ち、set は **role ごとに別の stack** を運ぶ —
  つまり role ごとに別のレシピが要り得る。「1要求1レシピ」の v1 文法では
  言えないので、言えない文法で近似せず断る。文: `a set names several stacks,
  and this build sends one recipe per request - run it on copied files, or
  wait for per-role recipes`。再訪条件: per-role レシピは role block と同じ
  形の追記で足せる (§10)。
- 部分 stack の規律は不変: framesUsed / expected は今日の器のまま。

---

## 8. 問い5 — プロトコル番号

**10 → 11 で足りる。** これは 2/3/7/8/9 と同族の **FRAMING** の版上げ
(新しい要求文法の追加) であり、かつ 9 と同じ「**拒否が読めない**」問題を持つ:
v10 の peer は META `x.raw` を `openNpy` に落として **「not a .npy file」** と
答える — ファイルを咎める文で、限界は peer のビルドのものである
(`pictureTooOldText` が protocol 10 で潰したのと同型)。だから番号で、送る前に、
client が断る。

新しい文 (remote_proto.h、`pictureTooOldText` の隣):

```cpp
inline std::string rawTooOldText(int peerVersion, const std::string& name) {
    return name + ": the remote peer cannot be told a raw recipe - it speaks "
           "protocol " + std::to_string(peerVersion) + ", and headerless RAW "
           "needs 11 (update viewer-serve). Nothing was opened: this file IS "
           "readable here through File > Open once copied, and every other "
           "format this peer serves still works.";
}
```

**4通りの組み合わせ:**

| client | peer | 振る舞い |
|---|---|---|
| 11 | 11 | 全機能。fmtgate 型のビット一致検査が守る |
| 11 | ≤10 | client が **HELLO の数から送る前に** `rawTooOldText` で断る (`Session::formatServable` の拡張 — picture の protocol-10 門と同じ場所・同じ規律)。番号は接続時に入った peer を自己更新させる — それがこの番号の存在理由の半分 (VERSION 注記の定型) |
| ≤10 | 11 | 旧 client は `.raw` を自分の `peerServesName` で門前拒否する (今日の挙動そのまま)。peer 側: LIST/SCAN の grouping widening は client 版数 ≥ 11 で門する (§6.2) ので、**旧 client の一覧はバイト同一**。万一旧 client が META を送っても (送らないが)、レシピ無しの §4.4 第2段の文が返る |
| ≤10 | ≤10 | 今日の G1 の実測のまま |

`VIEWER_SERVE_PROTOCOL=10` は v11 ビルドを v10 peer として振る舞わせる —
トレーラを読まず、grouping を widening せず、raw の META を今日の文で断う。
「a refusal nobody runs is a refusal that rots」(servedVersion の注記) —
新旧文の両側を selftest が実 peer で踏む (§11)。

**なぜ HELLO に拡張子リストを足さないか** — VERSION=10 の注記が既に裁定して
いる: wire 上の suffix リストは「どの形式がリンクを渡るか」を知る**2つ目の
場所**であり、それ自体が #148 の欠陥。同じ理由がレシピにも掛かる。

---

## 9. M1 — 宣言が結果に付いて回る

幾何・offset・バイト順・dtype は数値を変える measuring 級。リモートで開いた
doc が持つもの:

- **note**: `"remote <host>"` の既存行に、ローカルの `decodeRawFrame` が書く
  宣言句と同じものを **`rawRecipeSummary`** の一句で足す —
  `remote trc2  -  Bayer RGGB u16 4096x3000 +64B`。1つの綴りを 3箇所 (panel /
  applied line / note) で使う #166 の規律に 4箇所目として乗る。note は
  stack-constant (openRemote の note 規律のまま — レシピは Open ごとに 1つ
  なので構造的に定数)。
- **FrameSource の raw 欄** (§3.2): identity と session と reinterpret が
  これを読む。
- **session**: `raw3` 行 (session.inc:177) は path 欄に url を持つだけで
  **形式は不変**。復元側の dispatch が「path に `://` があれば
  `openRemote(url, recipe)`」を覚える。M2 (セッションが勝つ) はこれで成立 —
  復元はダイアログも束縛も通らない。復元直後の crop / seqframe は G5/G10 の
  RestoreWait に乗る (§6.3)。
- **export の provenance**: 既存機構が FrameSource から printしており、raw 欄が
  埋まれば追加作業なし。preview 中の export は G4 の門がそのまま塞ぐ。

---

## 10. 決めなかったこと (v1 で断る・再訪の鍵付き)

| 断ったもの | 理由 | 再訪の鍵 |
|---|---|---|
| ファイル内 frame 軸 (多フレーム .yuv) | ローカルの戸に無い。リモートが先行すれば「同じファイルに2つの答え」 | ローカルに frames/stride がレシピとして入った日。wire は末尾追記 (§6.1) |
| set / plugin MEASURE のレシピ | 1要求1レシピの文法で role 別レシピは言えない。近似しない | role block と同型の per-role 追記 (§7.3) |
| raw 行の 1クリック preview | レシピ無しでは作れず、束縛条件付きは第3の挙動 | ユーザ裁定。§5.3 を読み直す |
| crop を wire に載せる | crop は client の再スコープ (cropInPlace) で、G10 の機構が既にある | 載せる動機が観測されたら §3.2 と §6.3 を読み直す |
| cfaPattern を wire レシピ / identity に載せる | 画素値を変えない。MEASURE head が既に運ぶ | — (構造的に不要) |
| peer 側のセッション登録 (問い1の (b)) | 無状態の peer に寿命と所有の問いを 3つ生やす | 毎回 24B が問題になる測定が出たら (出ない) |
| `--raw-size` 無しの CLI ssh raw を LIST→ダイアログで救う | CLI は非対話の戸。断って Browse を指す方が正直 | Browse 戸 (段4) が入れば実害が無い |

これらは着地時に verify-matrix §8 (判断が記録されている拒否の一覧) へ 1行ずつ
足すこと。

---

## 11. 問い7 — 段階分け

4段。各段は単独で main に入って CI 緑、単独で意味がある。試験は fail-first —
何が赤で、直すと何が緑になるかを各段に書く。期待値は手で導出して assert の
隣に導出を書く (この repo の流儀)。

### 段1 — 断りに名前を付け、拡張子の表を両バイナリの家に移す (wire 不変)

**変更**: `core/imagefile.h/.cpp` に `isHeaderless` / `headerlessExts` と
`peerRefusal` のヘッダ無し一節 (§4.4 第1段の文)。`core/app/sequence.inc` の
`HEADERLESS_EXTS` / `extIsHeaderless` / `headerlessExtList` を imagefile 呼び
出しに置換 (配列の実体が移るだけ、答えは 1つも変わらない)。

**単独の意味**: G1 の「×落」が「×決」になる — 道が開く前でも、拒否文が
偽 (generic) から真 (名指し + 実の逃げ道) に変わる。verify-matrix G1 の
選択肢 (a) の実装であり、(b) が入った日に文を差し替える前提も §4.4 に明記済み。

**赤→緑** (`selftest.fmtgate` に F4d を足す):
- 赤: `imagefile::peerRefusal("a.raw")` が `headerless` を含まない (今日の実測
  は generic 文 — verify-matrix §9 の `[P]` のとおり)。
- 緑: `headerlessExts()` の**全要素**を歩き (反空虚: 要素数 ≥ 5 を併せて
  assert)、各 `x<ext>` について (i) `peerServes` = false のまま、
  (ii) `peerRefusal` が `headerless` と `File > Open` を含む、(iii) generic の
  `"the peer serves"` で**始まらない**。
- 回帰の守り: 既存 F4c (openImagesFilter が両配列+表の全拡張子を含む) が
  **無編集で**緑のまま — 移設が答えを変えていない証明。

### 段2 — protocol 11: wire + peer + client の版門 + CLI の戸

**変更**: `remote_proto.h` (VERSION=11、RawWire、rawTooOldText、拒否文群)。
`serve.cpp` (`getRecipe`、`openRaw`、openServed dispatch、§7.1 の検査表、
`VIEWER_SERVE_PROTOCOL≤10` で今日の振る舞い)。`remote.h/.cpp`
(`Session::meta/tile` にレシピ引数、`formatServable` の protocol-11 門)。
`open_dispatch.inc` (`openRemote` にレシピ引数、FrameSource raw 欄 + note の
宣言句 §9)。`cli.inc` (ssh:// positional raw + `--raw-*` → レシピ、
`--raw-size` 無しは名指しで断る §5.5)。

**単独の意味**: CLI から ssh の raw が開く (最初のユーザ可視の道)。wire が
確定し、以降の段は client 側の配線だけになる。

**赤→緑** (新 `--rawremote-selftest`、fmtgate P1/P2 の型に倣い local:// で実
peer を spawn):
- fixture は試験が書く。導出を assert の隣に書く:
  - `g1.raw`: 8×6 u16 LE gray、v(x,y) = y*8+x (48 サンプル、全値 < 2^16)。
  - `g2.raw`: 4×4 u8 BGR、B面=1 G面=2 R面=3 — **swap の検出専用** (peer が
    swap を忘れると ch0 が 1 のまま返り、期待値 3 と割れる)。
  - `g3.raw`: 8×6 u16 **BE**、値は g1 と同じ — byteswap の検出。
  - `g4.raw`: g1 の前に 64B のゴミ — offset の検出。
- 赤 (今日): client が `.raw` を `formatServable` 以前に generic 拒否。
- 緑:
  1. 各 fixture: META (レシピ同乗) の w/h/ch/dtype/frames がレシピの宣言と
     一致、frames == 1。
  2. TILE step=1 全域の画素が、同じファイル + 同じ値の RawDialog を
     `decodeRawFrame` に通した結果と**ビット一致** (fmtgate の local-vs-peer
     assert と同じ器)。
  3. step=2 の間引きが `readNpyRegion` の式 `(x1-x0+step-1)/step` から手で
     導いた寸法・サンプル位置と一致。
  4. `VIEWER_SERVE_PROTOCOL=10` の peer: client が送る**前に**断り、文が
     `protocol 10` と `11` を両方含む。
  5. v11 peer に `a.npy` + レシピ: `does not apply` を含む拒否。
  6. v11 peer に `.raw` + レシピ無し (Session を直接駆動): `recipe` を含む
     拒否 (§4.4 第2段)。
  7. サイズ嘘 (w×h を実ファイルの 2倍に): 拒否文が実バイト数と要求バイト数の
     **両方の数字**を含む。
  8. identity: 同 url を同レシピで 2度開いて source が共有される (registry
     の refs を数える — scan S5d の型)、幅だけ変えたレシピで開くと**別 tuple**
     (共有されない)。導出: §3.2 の鍵の raw 枝がフィールド毎に割れるから。

### 段3 — MEASURE: server σ_t が raw stack に効く

**変更**: `remote_proto.h` (head.flags bit0 の意味を注記)。`serve.cpp`
(`handleMeasure` が bit0 で rois 直後のトレーラを読み、measure 側の
FrameSource 読みが openServed 経由でレシピを受ける)。`remote.h/.cpp`
(`MeasureReq` にレシピ)。`open_dispatch.inc` (`maybeRequestServerTemporal` が
stack 頭 doc の raw 欄からレシピを積む、`RFetchJob` にレシピ継承 —
`rfInheritRead` の隣に同型の `rfInheritRecipe`)。plugin/set/analyzer op の
client 側事前拒否 (§7.3 の文)。

**単独の意味**: 表C O4 が raw に開く — 「300 frame の統計が画素でなく数百
バイトで渡る」という remote の存在理由が raw stack にも効く。段4 の Browse が
無くても、段2 の CLI 戸 + セッションで作った stack が server 集計できる。

**赤→緑** (新 `--rtemporal-raw-selftest`、rtemporal-png の型):
- fixture: 8 frame の u16 raw フォルダ、画素 (x,y) の時系列を
  `mean + (f が偶数 ? +d : -d)` で書く (d = 8)。導出を隣に書く:
  ddof=1 で σ_t = d·√(N/(N−1)) = 8·√(8/7)、f64 で閉形式。
- 赤: MEASURE が raw パスを開けない (peer 拒否)。
- 緑: (i) server σ_t が f64 独立参照と一致 (許容は rtemporal と同じ器)、
  (ii) 同じ stack をローカルで測った値と**ビット一致**、(iii) plugin op を
  raw パスに要求すると送る前に断られ、文が `recipe` を含む。

### 段4 — Browse の戸 + size 束縛 + セッション復元

**変更**: `imagefile` の `peerServesDeclared` を `browse/host.h` 経由で公開、
`rbRowOpenable` の remote 枝 (§4.3)。`rbOpenRow` の raw 分岐 (§5.1: 束縛 →
ダイアログ、fileSize は行の size / グループは先頭メンバへの LIST 1回)。
`rawDialogAccept` の url 分岐。peer の grouping widening (client≥11 門、
§6.2)。`openRemoteStack` のレシピ 1回/Open。session 復元の url-raw3 dispatch
(§9)。crop park は G10 の機構をそのまま (§6.3)。

**単独の意味**: G1 の報告者の動線そのもの — Browse で `.raw` をダブル
クリックして開く — が閉じる。ここで表A F9/F10×D7、表C F9/F10×O1(ssh) の
後継状態が ○ になる。

**赤→緑** (`selftest.browse` 拡張 + fmtgate 1本):
- 赤: remote `.raw` 行が淡色で無反応 (今日)。
- 緑:
  1. 行が openable、ダブルクリックで `g_rawPopupAlive` (RawDialog が立つ)、
     dialog の fileSize == LIST の size (fixture のバイト数から手導出)。
  2. accept で開き、doc の note が `rawRecipeSummary` の句を含む (M1)。
  3. 同サイズの 2本目: ダイアログ**無し**で開き、toast が `this session's
     recipe for <N>-byte files` を含む (N は fixture サイズ)。
  4. グループ行: レシピ質問が Open につき 1回 (2グループ目はダイアログ無し
     ではなく**別 Open なら再度訊く** — openId の規律の assert)。
  5. セッション保存 → 復元: ダイアログを経ずに同レシピで開き直る (raw3 行の
     往復。導出: 行のフィールド = レシピの全フィールド)。
  6. 旧 client の一覧パリティ: HELLO を 10 で送る駆動で SCAN 応答に raw
     グループが**含まれない** (バイト同一の門、§6.2)。

段の依存: 1←2←3←4 の直列。どの段も前段まで入った main で CI 緑、後段を
待たずに出荷できる。

---

## 12. 着地後の帳簿

- verify-matrix §7 G1 の状態行を段ごとに更新する (§1–6 のセルは 2026-08-11 の
  記録なので触らない)。全段着地で「済」、試験名は §11 の各段のもの。
- §10 の表の各行を verify-matrix §8 に転記する。
- `docs/remote.md` にレシピの節を 1つ (wire の形は remote_proto.h が正、
  remote.md は動線の説明のみ — 二重定義を作らない)。
- #166 の recipe panel の説明文に「レシピはリンクも渡る」の一文 (第4段で)。
