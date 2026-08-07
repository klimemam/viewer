# Reader 発の AnalysisSet — 返り値の形・参照・部分束縛・セッション

> **状態: 草案 (2026-08-07)。** §9 の判断リストが未回答。前提はすべて確定文書:
> [analysis-layers.md](analysis-layers.md) (§12 判断record — 特に record 2
> 「Reader で作るをとりあえず用意する」)、[terminology.md](terminology.md)
> (正典 — AnalysisSet の行と3不変条件は適用済み)、
> [reference-design.md](reference-design.md) (§6.2 identity tuple / §10 stackRev)、
> [input-adapters.md](input-adapters.md) (reader の既存契約。§番号の引用は
> 断り無き場合この文書)。本書は判断record 2 の「とりあえず用意する」側の
> 実装仕様であり、「追々作る UI」(Files / Browse から組む・暗黙 set・
> 前回束縛の事前入力) は analysis-layers.md §6 が既に形を書いている —
> 本書は §7 で境界だけ確認する。

## 0. 何を決めるか、何を決めないか

決めること:

1. reader の返り値として set を名乗る形 — 型・役割辞書・置き場所 (§1)
2. 読み直さずに既存データを役割へ束ねる参照 `Ref` (§2)
3. 束縛が満たせないときの振る舞い — 部分束縛と古さ (§3)
4. 運搬 — コンテナ予約メンバの追加と版 (§4)
5. セッション形式 — series ブロックの先例の名指しと適用 (§5)
6. Files の最小表示と Close の意味論 (§6)

決めないこと (既に決まっているか、他所の領分):

- **set の意味論そのもの** — 束縛であって包含ではない、メンバは複数の set
  から参照されてよい、メンバを閉じたら束縛を失いそう表示する — は
  analysis-layers.md §1.1 で確定済み。本書は再定義しない。
- **役割スキーマの中身** (どの解析がどの役割を要るか) は analysis-layers.md
  §6 の表と #57 の領分。
- **map を役割に束ねる形**は #49。analysis-layers.md §1.3 が要件だけ確定して
  いる (capture 役割に map は束縛できない) — 本書はそれを引き継ぐだけ (§1.2)。
- **Reader 複数選択の意味論** (docs/tasks.csv の残課題行「Reader 経由で開いた
  ものを Ctrl+click 複数選択したときの挙動が未定義」)。本書は解かない。
  set の設計が答えを強制することも無い — §7 に制約を1行だけ書く。

## 1. 返り値の形 — AnalysisSet 型

### 1.1 型と引数

`viewer_import` に5つ目の型を足す。層モデルの語がそのまま型名になる規則
(§4.2) は、正典の階層が5層になった今も同じ。名前は正典の1物1名に従い
**`AnalysisSet`** — 別名・短縮名は作らない (`set` は Python の組み込み語
でもある)。

```python
from viewer_import import AnalysisSet, Stack, Series, Ref, Values

def load(path):
    z = np.load(path)
    return AnalysisSet(
        {"image": Stack(z["flat"]),          # インライン: この呼び出しが画素を運ぶ
         "dark":  Ref("darks/dark_50ms/")},  # 参照: 読み直さない (§2)
        name="PRNU 10lx")
```

- 第1位置引数は**役割辞書** `{役割名: メンバ}` (判断 1)。正典が役割スキーマを
  書くのに使っている表記 (`{"image": Stack(), "dark": Stack()}`、
  analysis-layers.md §1) がそのままコードになり、綴りの間違いは構築行で落ちる。
- 追加フィールドは `name` `note` `meta` — 意味は §4.3 の既存表のまま。
  `range` は無い (set は画素を持たない)。`name` の既定は役割名の列
  `{image, dark}` (人が付けるまでの初期値、series の既定名の先例)。
- **返し方は2通りのまま** (素の配列 / 型 — §4.2)。AnalysisSet は型の側に
  1語増えるのであって、第3の道 (dict 返し) を開けるものではない。
  役割**辞書**は型の位置引数であり、返り値の形ではない。
- `Batch` のメンバに置ける: `Batch([stack, aset])`。1呼び出しで複数の set も可。

### 1.2 役割の値

役割の値に置けるのは4つ: `Frame` / `Stack` / `Series` (インライン) と
`Ref` (§2)。

- 正典どおり、メンバになれるのは frame / stack / series だけ
  (analysis-layers.md §1.1「メンバの frame / stack / series」)。`Batch` と
  `AnalysisSet` は役割値にならない (構築時 TypeError)。**set のネストは無い**。
- **素の ndarray は役割値にならない** (判断 2)。役割は宣言の場であり、
  そこにだけ native の形推測 (§3.1) へ戻る抜け道を開けない。`(F,H,W)` の
  dark を `Stack(...)` と書く1語が、その配列が繰り返しであるという宣言そのもの
  (§4.6 と同じ理由)。
- **同一オブジェクトは同一ノード**: 同じ `Stack` インスタンスを Batch のメンバ
  と役割値の両方 (あるいは2つの役割) に置いたら、画素は1度だけ運ばれ、束縛は
  同じノードを指す。同一 stack が1つの set の2役割を演じるのは**合法** —
  禁じる根拠が正典に無く、物理的に無意味な組み合わせは解析の側が数字で語る。
  Files はそれを言う (compare の「A and B share the same pixels」と同じ流儀:
  隠さず、畳まず、言うだけ)。
- **役割名は `[A-Za-z0-9_]+`**。構築時に検査する。理由は2つ: 役割スキーマの
  既存語彙 (image / dark / darks / sweep / flat) が識別子形であること、
  セッション行 (§5.1) が空白区切りで書けること。
- **map 役割は #49 が map の型を決めるまで束縛できない。** capture 役割
  (`Frame()` / `Stack()` / `Series()`) に map を流し込む経路が無いことは
  analysis-layers.md §1.3 の確定要件 — 型が4つしか無い今の Ref/インラインは
  構造的にこれを満たす。map 用の役割型は #49 の後にここへ追記する。

### 1.3 構築時に落ちるもの

§4.10 の「間違いは harness が走ったあとではなく、間違えた行で落ちる」を
set にも適用する。構築時 TypeError / ValueError:

- 役割辞書が空、または辞書でない
- 役割名が `[A-Za-z0-9_]+` の外
- 役割値が §1.2 の4型以外 (素の配列・Batch・AnalysisSet・その他)
- `Ref` の path が空文字列

### 1.4 どこに住むか

reader 1呼び出しは今までどおり batch を1つ作り (正典「1回の Open 操作は
batch を1つだけ作る」)、**set はその batch の1ノード**になる
(analysis-layers.md §1.1: set 自身は層のノードで、1つの batch に属す)。

- **インラインの役割メンバも同じ batch に住む。** set の「下」に住むのでは
  ない — 束縛は包含ではないから。運搬の木 (§4) では set の子として旅をするが、
  着地先は batch で、set はそれらを束縛する。
- **Ref で束ねたメンバは自分の batch に住み続ける。** 束縛が batch をまたぐ
  のは §1.1 の例外そのもので、合法。

### 1.5 役割スキーマとの照合は実行時

reader は**役割名と束縛だけ**を宣言する。「この set は PRNU 用」とは言わない
(判断 7)。SetAnalyzer が自分の役割スキーマ (analysis-layers.md §6 の表) を
宣言しており、照合は set に解析を実行しようとした時に行う:

- 必要役割がその層・その制約 (例 `Series(unit=s/ms)` — 単位は series 自身が
  持つ、正典) の束縛で満たされていれば実行可。
- 満たされなければグレーアウト+理由1文 (analysis-layers.md §4 の拒否規則の
  まま)。未束縛役割の理由文 (§3.2) がそのまま流用できる。
- **余分な役割は実行を妨げない** (判断 6)。`{image, dark, sweep}` を束ねた
  1つの set が PRNU (直接) も DSNU/PRNU 分離も実行できる — dark を1回撮って
  全解析で使う、が set の存在理由 (analysis-layers.md §6 の UI 論点と同根)。
  結果の provenance が名指すのは**使った役割だけ** (analysis-layers.md §8)。

### 1.6 前処理を書く reader との関係

「dark を引く」reader は AnalysisPreProcessor そのもの (analysis-layers.md
§5、判断record 2) で、その返り値は**生成物の Stack / Frame** — 本書の型では
なく既存契約で返す。生成物の不変条件 (由来を名前と note の両方に・撮った
ものの顔をしない・セッションにはレシピ) は §5 のまま掛かる。

生成物と「何から作ったか」の**機械的リンク**は v1 では作らない (判断 11)。
入力を記録したければ、生成物と並べて set を返す形は今日から書ける:

```python
return Batch([Stack(subtracted, name="10lx - dark", note="dark subtracted by acme.py"),
              AnalysisSet({"image": Ref("10lx/"), "dark": Ref("darks/")},
                          name="inputs of 10lx - dark")])
```

これは記録であって配線ではない — 生成物を選んでも set は光らない。配線は
PreProcessor 連鎖のレシピ機構 (#57 / #49 の後、analysis-layers.md §8 の
provenance 表がその欄を既に持っている) と一緒に作る。

## 2. Ref — 読み直さない参照

dark は1回撮って全解析で使う (analysis-layers.md §1.1 の前提)。既に開いて
いる stack を set のために読み直す・複製するのは、その前提の否定になる。
`Ref` は「この役割は、あそこに居るあれ」を**パスで**言う。名前で言わないのは
セッション形式と同じ理由 — 名前はリネーム可で一意でない
(core/app/session.inc の series メンバの注記)。

### 2.1 パスの意味論 (判断 3)

`Ref(path, member="")`。

| path | 意味 |
|---|---|
| ファイル | その1ファイルを native 規則 (§3.1) で開いたもの。コンテナ (.npz) は `member=` で1メンバを名指す |
| フォルダ | 中の連番ファイル**1グループ** = 1 stack。グループが2つ以上あれば拒否し、グループ名を列挙して言い直させる |

- **sibling スキャンはしない。** frame のパスを渡したら frame。stack が
  欲しければフォルダを指す。1枚のパスを黙って24枚に膨らませるのは推測で、
  束縛は宣言 (正典の不変条件 (c))。
- **部分参照は無い** (`frames=` のようなスライスを作らない)。部分は viewer
  主導の `only=` (§4.1) の領分で、束縛は対象を丸ごと指す。
- Ref の指す先が native で読めない形式なら、パス→reader の記憶 (§4.12) を
  引く。記憶が無ければ未束縛 (§3.1)。**ここで新しい reader を自動起動しない**
  — 「明示的に選んだものだけが走る」(§4.13) は束縛経由でも破らない。

### 2.2 解決の順序 — registry の identity tuple を再発明しない (判断 4)

Ref の解決は「開くときの共有」(reference-design.md §6.2 の source registry)
の上に乗る。順序:

1. **stat → tuple → registry 一致 → 共有。** path (+member) を stat し、
   identity tuple (path, npzMember, npyRead, mtime, fsize) で registry を
   引く。一致すれば既にある source を共有してその stack / frame へ束ねる。
   デコードもコピーも起きない — 「読み直さない」はこの1行で成立する。
2. **path 一致・tuple 不一致 → 開いている方へ束ね、申告する。** 同じ path の
   stack が開いているがディスクが変わっている (mtime / fsize 不一致) 場合、
   **開いている方**へ束ねて役割行に言う:
   `bound to the open copy - the file on disk has changed since it was opened`。
   束縛はセッション内の対象への参照であり、黙って第2の画素を積むと「同じ
   名前の違う画素」が同じ画面に並ぶ。ディスクの新しい方が欲しいのは reload
   (reference-design.md §3.2) の一手で、束縛の仕事ではない。
3. **未オープン → 開いて束ねる。** native (または記憶された reader) で開く。
   生まれた stack は **reader 呼び出しの batch** に入る — 「この set のために
   開かれた」が帳簿に残る。

Ref は mtime / fsize を**運ばない** — identity before bytes の stat は
viewer 側が束縛の時点で取る (reference-design.md §6.2 の規律のまま)。
reader の返した数字を identity に使うと、reader の実行時間ぶん古い identity
に新しいバイト列が束縛される。

### 2.3 remote

reader は peer 側で走る (§4.13.1)。peer で走った reader の Ref は peer の
名前空間のパスであり、viewer はそれを remote 側の identity
(url / remoteFrame / …) の tuple に写して**同じ順序**で解決する。set の集計が
画素の居る側で走る線は analysis-layers.md §3.5 のまま、細部は実装時
(同 §10)。本書が約束するのは「Ref の解決順序が local と remote で同型」まで。

## 3. 部分束縛 — 無いものは無いと言う

### 3.1 失敗の非対称 (判断 5)

- **インラインの失敗は呼び出しの失敗。** 役割メンバの画素が壊れていれば
  reader の実行全体が失敗し、traceback が丸ごと出る (§4.9 / §4.13.0)。
  インラインは reader 自身の出力で、半分だけ受け取ると「reader が返した
  もの」が二義になる。
- **Ref の失敗は未束縛の役割。** 指した先が無い・読めない・フォルダが曖昧 —
  set は開き、その役割は**未束縛**として理由1文つきで表示する。Ref は世界の
  状態への参照で、この機械にその dark が無いことは、束縛の宣言 (set) を
  捨てる理由にならない。「メンバを閉じたら束縛を失い、そう表示する (黙って
  空にしない)」(analysis-layers.md §1.1) の、入口側の同じ規則である。

### 3.2 未束縛の set で何ができるか

- set ノードは Files に出る (§6)。未束縛役割は理由つきで見える:
  `dark: unbound (darks/dark_50ms/ not found)`。
- SetAnalyzer は役割スキーマ照合 (§1.5) で拒否 — グレーアウト+理由1文。
  未束縛の理由文がそのまま拒否の理由になる。
- **部分ロードの stack への束縛は正常。** 束縛は対象への参照であって枚数への
  参照ではない。実行の可否は解析側の `min_frames` / settle 待ち / n of N
  (analysis-layers.md §3.5) がそのまま裁く — 本書は何も足さない。

### 3.3 古さは結果に付く、束縛には付かない

束縛は membership (stack) を指すので、メンバが reload されても束縛は正しい
まま。古くなるのは**計算済みの結果**で、それは既存機構の再利用:

- SetAnalyzer / PreProcessor の結果は実行時に各メンバの **(seqId, stackRev)**
  を記録し、`stackRev` の不一致で stale マークを立てる。絵も数も勝手に
  再計算しない — 再計算はユーザーの一手 (reference-design.md §10.1、
  stackavg の規律そのまま)。
- セッションを跨ぐ世代は identity tuple で言う (同 §10.2)。世代の語彙を
  ここで3つ目に増やさない。

## 4. 運搬 — コンテナ予約メンバの追加 (判断 8)

harness の木 (§4.11) に足すもの:

- `__layer_<i>` の新しい値 **`analysisset`**。set はノードで、親は batch
  (または根 — set 単体の返り値ではノード 0)。
- インラインの役割メンバは set ノードの**子ノード** + **`__role_<i>`**
  (役割名の文字列)。木は木のまま — 「束縛であって包含ではない」は viewer 側
  のモデルの話で、運搬の木が先取りする必要は無い (着地の規則が §1.4)。
- Ref は **`__refs_<i>`** — set ノードに置く JSON
  (`{"dark": {"path": "...", "member": ""}}`)。参照は層ではないので
  `__layer` の語彙を汚さない。JSON は meta の先例 (§4.11) に従う。
- 表示順 = 宣言順。インラインはノード順、Ref は JSON のキー順で運ぶ。
- viewer は set ノードの meta に reader の spec を記録する (loader が origin
  の meta に `reader` を足している先例)。set の provenance の「計算者」欄
  (analysis-layers.md §8) はこれで埋まる。

**版**: set を含む stream / viewer-npz **だけ**が `__viewer 2` を名乗る。
含まなければ 1 のまま — set の無いファイルの互換は1ミリも動かない。旧 viewer
は「自分より新しい版は読まずに断る」(§4.11.1) ので、set が黙って落ちた
不完全な復元は起きない — 断られるのは仕様どおりの正しい失敗である。

## 5. セッション — series ブロックの先例に従う

analysis-layers.md §10 は「series ブロックの先例に従う、までを方針として」と
書いた。従う先例を名指しする (core/app/session.inc の series ブロックと
reference-design.md §5):

1. **フラットな一意キーのブロック。** 旧 viewer は知らない行を読み飛ばして
   セッションを開ける — 形式の変更はキーの追加のみ。
2. **メンバは先頭フレームのパスで旅をする。** 名前はリネーム可・非一意なので
   キーにならない。
3. **解決できないメンバは数えて言う。** 読む側が数えるなら書く側も数える。
4. **復元待ちの間も verbatim に書き戻す。** 待ち窓での保存が series を消した
   教訓 (`seriesRestore` の由来) — set も同じ `setRestore` の器で待つ。

### 5.1 ブロック形式

image 行と series ブロックの**後** (set は series を参照しうるので、参照
される側が先):

```
analysisset <名前>
setbatch <batch 名>
setreader <spec>                          ... 作った reader (reader 発でなければ行ごと省略)
setorigin <path>                          ... reader 呼び出しの元ファイル (同上)
setrole <役割> frame <batch 名>\t<path>
setrole <役割> stack <batch 名>\t<先頭フレームの path>
setrole <役割> series <batch 名>\t<series 名>
setrole <役割> unbound <理由>             ... 保存時点で未束縛だった役割
setend
```

- 役割名が識別子形 (§1.2) なので空白区切りで書ける。batch 名とパス /
  series 名は空白を含み得るので TAB で区切る (`readerfor` の先例)。
- stack は**先頭フレームのパス** (series メンバの先例)。batch 名を添えるのは
  「同じフォルダを2回開いた」の既存解決 (正典: その series の batch にある
  stack を選ぶ) を束縛にも効かせるため — 束縛は batch をまたげるので、
  どの batch の stack かはブロックが言う。
- series 役割は **(batch 名, series 名)** で指す (判断 10)。セッション形式が
  series を復元するのに使っている同一性そのもので、新しい鍵を発明しない。
- **未束縛役割も書く。** 束縛の宣言は記録であり、保存を跨いで黙って消えない
  (§3.1 と同じ線)。理由文は行末 (restOfLine)。
- このブロックは set の作られ方を問わない — 追々作る UI の set は
  `setreader` / `setorigin` 行が無いだけで同じ形式に乗る。

### 5.2 復元 — reader 再実行との二重宣言 (判断 9)

reader で開いたファイルは復元時も同じ reader が読む (§4.12 — session.inc の
既存動作: 1回の再実行がそのファイル由来の全 doc を作り直す)。set を返す
reader は再実行で set をもう一度作る。するとブロックと再実行が同じ set を
二重に宣言する。規則:

- **(setbatch, 名前) が一致する set が再実行で既に出来ていたら、ブロックは
  それを採用 (adopt) する** — 2つ目のノードを作らない。リネームの持ち主は
  ブロック (seqname が SeqRestore に乗る先例)、役割の真実は再実行側。
- 役割が食い違えば (reader が編集された等)、**再実行側を真**とし、食い違った
  数を数えて言う (先例 3 の同型)。
- reader が再実行できない (ファイル消失・Python 不在) 場合、**ブロックだけで
  set を復元する** — 全役割を §5.1 の記録から §2.2 の順序で解決し、解けない
  役割は未束縛+理由。「保存したものを開き直せないセッションはセッションでは
  ない」(session.inc) がここでも効く。

## 6. Files の最小表示と Close

「Files の見え方」の全体設計は据え置き (analysis-layers.md §10) のまま、
reader で set が作れる日に**最低限**必要な分だけを決める:

- **set は batch の下の1ノード** (§1.1 で確定済み)。行は名前+束縛の充足:
  `PRNU 10lx  (2/2 roles)`、欠けがあれば `(1/2 roles - dark unbound)`。
  **バイト数は出ない、永遠に** — 束縛は所有ではない (unique source の和に
  set の行が足すものは無い、reference-design.md §7 の一行の教訓)。
- **ノードの下に役割1行ずつ**: `dark → 10lx/dark_000‥023.npy (24)`。未束縛
  行は理由を出す。画面の順 = 読みの順 (set → 役割 → メンバ)。クリック =
  選択、ダブルクリック = 束縛先メンバへ移動 (選択と確定の既存規約)。役割行は
  **束縛の表示**であって、メンバの家はメンバ自身の stack 行のまま — 木を
  複製しない。
- **メンバ側の行**には列も印も足さず、tooltip だけ:
  `bound as "dark" in "PRNU 10lx"` (共有 frame の ⧉ tooltip の先例、
  reference-design.md §4)。
- **Close (set ノード)**: 束縛の記録を破棄。メンバは無傷 — 所有ではないから。
  報告は数を言う: `closed set "PRNU 10lx" - 2 binding(s) dropped, members
  untouched` (closeStack の生存通知と同じ形)。
- **rename**: 可、セッション保存 (正典の表の行のまま)。
- **SetAnalyzer の実行**は set ノードの選択から (analysis-layers.md §2 の
  「住処」列)。拒否は §1.5 / §3.2。

## 7. 追々作る UI との境界

以下は本書の範囲外で、置き場所は決まっている:

- **Files / Browse から set を組む UI・暗黙 set (1クリック実行の場の束縛)・
  保存時ノード化・前回束ねた dark/flat の事前入力** — analysis-layers.md §6
  がその UI の仕様。本書の Ref 解決 (§2.2)・セッション形式 (§5)・Files 表示
  (§6) はその UI の set にもそのまま効く。
- **picker との関係、束縛の編集 UI** — analysis-layers.md §10 のまま。
- **Reader 複数選択** (docs/tasks.csv の残課題行): 本書は解かない。set が
  加える制約は1つだけ — どう決めても、複数選択から**役割を推測して自動で
  set を組んではならない** (正典の不変条件: 役割の束縛は宣言である。推論
  しない)。N 個の選択が N 個の返り値になる案も 1 本の series になる案も、
  この制約と両立する。

## 8. 既存文書への適用ノート (機械的追記 — 判断ではない)

判断リストの承認後、別コミットの機械的作業として:

1. input-adapters.md §4.2 の「層モデルの**4語**以外の語を持ち込まない」を、
   正典の5層化 (terminology.md 適用済み) に合わせ「5語」に改める。同 §4.11
   の予約メンバ表に §4 の3つ (`analysisset` / `__role_<i>` / `__refs_<i>`) と
   版の規則を足す。
2. terminology.md の操作マトリクスに AnalysisSet 列を足す (Close = 束縛の
   破棄・メンバ無傷、rename = 可・セッション保存、他は空欄)。§11 の適用
   パッケージと同じく、修正として明示的に。
3. `tools/import/viewer_import.py` の docstring と README の型一覧に
   `AnalysisSet` / `Ref` を足す (実装時)。

## 9. 判断リスト

各項、推奨と理由1行。番号で答えられる形。

1. **AnalysisSet 型の形** — `AnalysisSet(役割辞書, name=, note=, meta=)`、
   第1位置引数が `{役割名: メンバ}`。
   **推奨: 採用** — 正典の役割スキーマ表記がそのままコードになり、間違いが
   構築行で落ちる (§4.10 の利得を set にも)。
2. **役割値に素の配列を許すか** — 許さず、`Frame` / `Stack` / `Series` /
   `Ref` のみとする。
   **推奨: 許さない** — 役割は宣言の場で、native の形推測へ戻る抜け道を
   そこにだけ開けない。
3. **Ref のパス意味論** — ファイル = native 読み、フォルダ = 連番1グループ
   のみ (複数グループは名指しで拒否)、sibling スキャン無し。
   **推奨: 採用** — 黙った推測より、読まずに理由を言う (input-adapters §1 の
   原則を束縛にも)。
4. **Ref の解決順** — tuple 一致で共有 → path 一致・tuple 不一致は開いて
   いる方へ束縛+申告 → 未オープンは開いて束ねる。
   **推奨: 採用** — 束縛はセッション内の対象への参照で、黙って第2の画素を
   作らない。
5. **失敗の非対称** — インラインの失敗 = 呼び出し全体の失敗、Ref の失敗 =
   未束縛役割として開く。
   **推奨: 採用** — インラインは reader 自身の出力、Ref は世界の状態。
   壊れた出力は直させ、無いデータは記録して見せる。
6. **余分な役割を持つ set での実行** — SetAnalyzer は必要役割が満たされて
   いれば実行可、余分は妨げない。
   **推奨: 許す** — dark を1回撮って全解析、が set の存在理由。provenance は
   使った役割だけを名指す。
7. **対象アナライザの名指し欄** — `AnalysisSet(..., for_="prnu")` のような
   欄は作らない。
   **推奨: 作らない** — 束縛は宣言、実行は人の一手。データをコードの登録簿に
   結び付けると、アナライザ改名のたびにデータが古くなる。
8. **コンテナの版** — set を含む stream / viewer-npz だけ `__viewer 2`。
   **推奨: 採用** — set の無いファイルの互換を動かさず、旧 viewer は黙って
   欠けを出す代わりに断る。
9. **セッションの二重宣言** — reader 再実行と set ブロックは (setbatch, 名前)
   一致で adopt、役割の真実は再実行側、不一致は数えて言う。
   **推奨: adopt** — 真実は1つ (再実行)、ブロックはリネームと記録の担い手。
   2ノード化は「同じ set が2つ」という嘘になる。
10. **series 役割の参照キー** — (batch 名, series 名)。
    **推奨: 採用** — 既存形式が series をこの語彙で復元しており、新しい
    同一性を発明しない。
11. **生成物↔set の機械的リンク** — v1 では作らず、note / meta の由来
    (analysis-layers.md §5 の不変条件) までとする。
    **推奨: 見送り** — レシピ機構 (#57 / #49 後の PreProcessor 連鎖) 無しの
    リンクは、半端な provenance を2つ作る。
