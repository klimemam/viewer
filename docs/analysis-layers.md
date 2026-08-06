# 解析の層 — Frame / Stack / Series / AnalysisSet と4種のアナライザ (#48)

> **状態: 仕様 v2。**
> v1 (PR #84 初版) はデータ3層に解析を割り当て、8つの判断を諮った。
> ユーザー提案 (2026-08-06, #48) が**役割で束ねる型 AnalysisSet と解析の4種**
> という再定式化を出し、8判断のうち半分がそこで解けた (§9 に対応表)。
> 本書はその提案を骨格に据えて書き直したもの。残る判断は §12。
> 個々の推定量の数式・補正・表示の細部はここでは決めない (§10 — #57 の領分)。

## 0. なぜ層か、そしてなぜ層だけでは足りないか

正典 ([terminology.md](terminology.md)) は「**σ_t は stack の属性。frame の属性
として出力しない**」を不変条件に持ち、操作マトリクスの測定 (analyzer) 行は既に
層で分かれている。ところが解析の側にはこの区別を言う場所が無い — プラグイン
ABI は `analyze(const psFrame* in, …)` の1枚受け取り
([ps_plugin.h](../include/ps/ps_plugin.h)) で、stack から欠陥マップを作る解析は
**表現できない**。層を型にすると、間違った測定が表現できなくなる。
ここまでが v1 の出発点で、変わらない。

v2 で加わった認識 (ユーザー提案の核):
**Frame / Stack / Series は「dark が要る」「固定パターン除去の前処理が要る」を
表現できない。** それはデータがどれだけ並んでいるかの話ではなく、解析への
**入力の役割**の話だから。役割を表現する型をデータ階層に1つ足すと、
3層は「一般的な解析だけを、余計なことを考えずに」担えるようになる —
参照や前処理を含意する解析は全部、役割を束ねた型の上でだけ始められる。

## 1. データの階層 — AnalysisSet が入る

Files 内のデータ型の階層 (ユーザー提案どおり):

    Frame < Stack < Series < AnalysisSet < Batch

**AnalysisSet = 解析への入力を役割 (role) で束ねる型。** 例:

    PRNU 用:   {"image": Frame()|Stack(), "dark": Frame()|Stack()}
    DSNU 用:   {"dark": Stack()}
    暗電流用:  {"darks": Series(unit=s/ms)}

役割スキーマは解析ごとに定義され、AnalysisSet はそのベース型。名前は
ユーザーが仮置きした語をそのまま採る (改名はいつでも安い — 型が先、名は後)。

### 1.1 メンバは包含ではなく束縛 — 厳密包含の唯一の例外

正典の包含は厳密 (frame ⊂ stack ⊂ series ⊂ batch、例外を作らない)。
AnalysisSet をそのまま厳密包含 (メンバは高々1つの set に属す) にすると
壊れる具体例が最初からある: **1本の dark stack を、PRNU の set と欠陥画素の
set が同時に参照する。** 参照データの再利用は例外ではなく通常運用 —
dark は1回撮って全解析で使う — なので、「高々1つ」は置けない。よって:

- **set 自身は層のノード**: 1つの batch に属し、Files に1ノードとして現れ、
  リネーム可、セッション保存 (形式は実装仕様 — series ブロックの先例に従う)。
- **メンバは束縛 (参照)**: メンバの frame / stack / series は自分の batch の
  下に住み続け、**複数の set から参照されてよい**。束縛は所有でも移動でもない。
  メンバを閉じたら set は束縛を失い、そう表示する (黙って空にしない)。
- これは階層の中で AnalysisSet だけが持つ性質で、正典には**例外として明記**
  する (§11、判断1)。黙って混ぜない。

### 1.2 役割の束縛が宣言である — 「dark は stack の属性」を取り下げる

v1 は「dark / flat は stack の宣言属性」を提案していた (旧判断4)。**取り下げる。
役割束縛の方が正しい**: stack は「dark である」のではなく、**この set で dark を
演じる**。同じ stack が別の set で image を演じてもよい (低照度 stack を
基準に使い回す、が実例)。属性案より優れる点:

- 宣言の原則 (推論しない) はそのまま生きる: dark の穴に stack を**束ねる行為**
  が宣言で、フォルダ名や level 0 から推測しない。束縛は set の画面に見える。
- stack に属性が残らないので、「dark と印の付いた stack をうっかり flat に
  使う」類の、属性と用途の齟齬が構造的に無い。
- 単独 dark stack の DSNU に series の儀式も要らない (旧判断4の対立点は消滅):
  `{"dark": Stack()}` という1役割の set で済む (§6)。

「level 0 = dark」の既存則は linearity の後方互換に閉じ、新しい解析へは
広げない ([flat-field-stats.md](flat-field-stats.md) (c) の原則、据え置き)。

### 1.3 評価値マップ (#49) の家

読み込んだ評価値マップ (欠陥マップ・ゲインマップ・マスク・信頼度…) は
**AnalysisSet に役割付きで束ねる**。生成は §5 / §6 が担う。画素が何の量かは
役割スキーマが宣言するので、「map に frame 解析を掛けると [DN] が嘘になる」
(#49 の要決定 (3)) は入口で型が塞ぐ。語彙の確定・表示レンジ・セッション・
export の細部は #49 のまま — ただし**帰属先の問い (#49 の要決定 (1)) は
これで答えが出た**: map は set に付く。

## 2. 解析の4種

| 種 | 入力 | 出力 | 宣言のしかた | 住処 |
|---|---|---|---|---|
| **1 General Analyzer** | 選択中の frame / stack / series **1つ** | 中立な量 (mean, σ, σ_t, histogram, projection, …) | 層ごとの定義 (§3) | Histogram / Projection / ROIs / Temporal / Series Analysis の各パネル |
| **2 Specific Analyzer** | シグネチャが要求する層の対象**1つ** | 解析固有の量 | **シグネチャ** (frame / stack / series) | Analysis パネル (プラグイン) |
| **3 AnalysisPreProcessor** | **AnalysisSet** | frame / stack / series / batch (中間生成物。ストレージ可) | 役割スキーマ | set の操作 (§5) |
| **4 SetAnalyzer** | **AnalysisSet** | 最終 KPI (パネル表示)、評価値マップ | 役割スキーマ | set の操作 (§6)。**実装は 1–3 の再利用** |

境界は1問で引ける: **選択中の対象1つの外に、役割付きの入力が要るか?**
No なら 1 か 2 (層の中の解析)。Yes なら 3 か 4 (set の解析)。

v1 の中心定義はそのまま生きる: **解析の層 = その解析の入力が丸ごと必要とする
最小の層 = 結果が属性として付く層**。入力の広がりと結果の帰属がずれる解析は
存在しない。4種はこの定義の上の分類で、「層」の列に AnalysisSet が加わった形。
[stats-taxonomy.md](stats-taxonomy.md) との対応も素直になった:
S1/TN/LM = 層 (General/Specific の縦軸)、**+meta/+ref はこれまで行き場の
無かった軸で、それが役割スキーマそのもの**。+cal はツールの外 (据え置き)。

batch に解析は無い (正典どおり)。PreProcessor が**生成先の入れ物**として
batch を使うことはある — 構造は主張しないまま、複数の生成物を収めるだけ。

**名前の規則** (ユーザー提案を規則化): General は中立な量の名前だけを出す。
**前処理・参照を含意して広く周知されている名前 (PRNU, DSNU, 欠陥画素数, …) は
SetAnalyzer だけが名乗る。** 役割が束ばれていなければその名前の計算が
始められない — 名前の要求と型の要求が一致する、が v1 の「裸名の予約」の
v2 での姿である。なお基準は「**参照役割の有無**」であって「規格に載って
いるか」ではない: e-SFR/MTF50 はチャートを**シーンとして**要するが参照役割を
持たないので Specific のままでよい。

## 3. General Analyzer — 層ごとのパネル

### 3.1 Frame — 確定 (再掲)

ユーザー決定 (#48):

> Frame解析の層は必ず必要になるので，ここは確定しよう．で，Frame解析は，
> 今と同じで，Histogram, Projection, ROIsで．

Histogram / Projection / ROIs、今と同じ。frame 層が正直に言えることの限界も
確定済み ([flat-field-stats.md](flat-field-stats.md) (a)): 1枚の空間σは常に
`σ_t² + σ_fpn²` の合成で、固定パターンとしては**上界**にしかならない。

### 3.2 Stack — Stack Histogram / Projection / ROIs / Stack Temporal (新規スコープ)

Stack Temporal は既存の Temporal パネル (σ_t / σ_fpn / ドリフト / per-frame 表)。
新しいのは**時間軸を持たない解析 (Histogram / Projection / ROIs) を stack に
掛ける**部分で、時間方向の畳み方の定義が要る (判断3):

- **既定 = 時間 Mean (画素ごと)。** `0e4d4ec` の frame 平均が既に作った機構
  そのもの (`computeStackStats` の蓄積器・double 蓄積・NaN 画素毎除外・n/N)。
  統計的にもこれが正しい既定: 時間平均画像の空間σが σ_fpn なので、
  **Stack ROIs の σ 列は Temporal パネルの σ_fpn と同じ蓄積器から出る同じ数**
  になり (絵の下の数と表の数が食い違えない、の原則)、Stack Projection は
  平均画像の行/列 FPN ([flat-field-stats.md](flat-field-stats.md) (b) の
  「列 FPN は平均画像に (a) の分解を掛ければ出る」がそのまま画面になる)。
- **per-frame の重ね / 並べ表示**: N 枚それぞれの frame 解析を重ねる。
  これは S1×N の**見せ方** ([stats-taxonomy.md](stats-taxonomy.md) §3) で、
  ドリフト・外れ frame の発見用。新しい量は出さない。
- **Sum は採らない。** NaN を画素ごとに除外する規約の下では分母が画素ごとに
  違い、Sum は「何枚分の和か」が画素ごとに違う数になる。Mean は画素ごとの
  分母で正直に定義できるが、Sum はできない (完全 stack なら Mean×n で
  読者が1セルで作れる、という export の delta 列と同じ理屈でも不要)。
- **pooled (全 frame 全画素を1母集団に) は保留**: σ_tot の分布として意味は
  あるが、用途が実物として出るまで作らない。

表示列・出力列の名前には畳み方が付く (例 `sigma [DN] (mean_t)`)。同じ表の
顔で別の量を出さない。畳んだ表の σ/mean 列が本物の PRNU に**ならない**こと
(dark 役割が無い) は §7 のとおり — 列名は修飾のまま。

### 3.3 Series — Series Analysis パネル

Linearity パネルの一般化。Stack Temporal とは別パネル (ユーザー提案どおり —
層が違うものを1枚に畳まない)。担当は**役割参照の要らない** series 解析:
linearity fit / PTC (K) / read noise (外挿) / SNR カーブ / DR (飽和定義は meta)。
役割参照が要るもの (DSNU/PRNU 分離、dark 実測 read noise、暗電流) は §6 へ。

series 解析の要件は v1 から変更なし (すべて既存正典の再掲):
**単位と値が無ければ fit しない** / **kind が fit の式を決める** /
**単調性は要求しない** (温度は往復する。同一レベルの重複は独立測定で合法) /
**series を編集したら計算済みの数値は破棄**。

フレームリニアリティ (暫定実装 `frameLinCollect`) の最終形もここに落ちる:
条件を振った並びは定義上 series であり、正典の series 定義「条件を1つ振った
**stack / frame** の並び」に **frame をメンバとする series は最初からある**。
暫定の stack + frame 軸という器は移行まで維持、frame 軸は monitor 用途
(経過時間・温度に対する mean の曲線 = §3.2 の cv) に純化する (判断7)。

### 3.4 パネルは分裂しない — Files の選択が層を選ぶ

パネルの実体は解析の族 (Histogram / Projection / ROIs / Temporal / Series
Analysis) で、**Files パネルの選択が応答の層を切り替える** (ユーザー提案
どおり)。階層は包含なので、series の中の stack を選べば stack の応答。
単発 frame を選んだ Temporal は「1枚に時間軸はない」を言う (グレーアウト+
理由の既存規則)。

**v1 の「下の層では拒否」は、General ではこうして構造的に消える** — 選択が
層を決めるので、「stack 解析を frame に掛ける」という操作がそもそも存在
しない。拒否が残るのは Specific / SetAnalyzer の署名不充足だけ (§4, §6)。

### 3.5 stack に触れる解析の共通要件 (v1 から存続。種を問わず適用)

- **最小枚数は解析が宣言する** (`min_frames`)。σ_t は 2、RTS は桁が違う。
  満たさなければ実行前に拒否 — 理由1文つき。
- **n of N**: 部分ロードでも実行し「何枚中何枚か」を**必ず**併記 (正典の
  不変条件)。数えるのは実際に集計した枚数 — preview・寸法違いは入れない
  (frame 平均の先例)。
- **ロード中は実行しない**: settle を待つ (`requestStackAverage` の先例)。
  n/N が出るのは「ロードが本当にそこで止まった」= stack の事実のときだけ。
- **NaN は画素ごとに除外して数える** — 分母に畳まない (`computeStackStats` /
  serve.cpp で確立済み)。除外数は結果が運ぶ。
- **remote の stack 集計は画素の居る側 (peer) で走る** (`MOP_TEMPORAL_STATS`
  の先例。未オープン stack は `not opened` タグ)。プラグインの stack 解析と
  set の集計も同じ線 — 細部は実装時 (§10)。

## 4. Specific Analyzer — シグネチャが要求を宣言する

**解析の層は登録シグネチャで表す。layer フィールドは持たない** — この選択は
v1 の推奨だったが、ユーザー提案が「Specific Analyzer のシグネチャで判断」
「(SetAnalyzer は) 役割スキーマ」と自ら署名宣言を採ったので、**確定**として
書く。フィールドは「stack と名乗って1枚しか見ない」を書けてしまうが、
シグネチャなら書く場所が無い — 規則がコンパイル時の事実になる。

frame シグネチャは今の `analyze(const psFrame*, …)` のまま (V1/V2 は永久に
ロード可能 — ヘッダの約束)。現行プラグインは全部 frame の Specific である。
stack シグネチャは **ABI v3** の新しい登録型:

```c
/* 方向を示す骨格。フィールドの確定は実装仕様で行う (§10) */
typedef struct psStack {
    uint32_t frames;            /* n: 実際に見せる枚数 */
    uint32_t expected;          /* N: stack の期待枚数 (n < N = 部分ロード) */
    /* pull 型アクセス: ホストが常駐/転送を管理でき、共有メモリ運搬層と同じ
       形に載る。全枚の配列を渡すと「全部メモリに居る」を ABI が約束して
       しまうので、それはしない */
    const psFrame* (*get_frame)(void* ctx, uint32_t index);
    void* ctx;
    /* … name, meta_json, reserved … */
} psStack;

typedef struct psStackAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 */
    uint32_t    min_frames;     /* これ未満ならホストが実行前に拒否する */
    const char* name;           /* "category/name" */
    const char* version;        /* provenance 用 (#46)。V3 で新設 */
    const char* description;    /* 前提1行 (平坦・静止など機械に見えない前提) */
    int32_t (*analyze_stack)(const psStack* in, const psRect* roi,
                             const psAnalyzeSink2* sink, char* err, size_t err_cap);
    void*       reserved[4];
} psStackAnalyzerV3;
```

- RTS / blink は stack シグネチャの Specific (役割不要、min_frames 大)。
- series シグネチャと役割スキーマ (kind 3/4) のプラグイン口は今回**作らない**
  (§10)。組み込みが先に §5–§6 を張り、外に開くのは形が固まってから。
- 拒否の残り面はここ: 署名不充足 (層違い・`min_frames` 未満・settle 前) は
  グレーアウト+理由1文 (measure-ux.md の既存規則)。前提のうち機械に見えない
  もの (平坦・静止・未飽和) は従来どおり description → ツールチップ。

## 5. AnalysisPreProcessor — set からデータを作る

AnalysisSet → frame / stack / series / batch。**生成物は一級のデータで、
§3 / §4 がそのまま掛かる** — それがこの設計の配当で、中間生成物のために
特別な解析を書かない。

- **frame 平均は最初の PreProcessor** と型づけ直せる: `{"image": Stack()} →
  Frame`。`0e4d4ec` が確立した不変条件 — **計算で作った frame は撮ったものの
  顔をしてはならない**。由来を名前と note の両方に、ファイルは持たず、
  セッションにはレシピを書いて再計算する — は、**PreProcessor の生成物
  すべての規約**に昇格する (dark 減算画像・detrend 済み画像・時間平均、全部)。
- 生成物をストレージへ書き出すときは export の家風 (provenance ブロック付き)
  に従う。読み戻したものは「撮ったもの」ではないので、その旨をファイル自身が
  運ぶ形式にする — 細部は実装仕様と #49。
- 前処理の順序 (黒レベル → 飽和マスク → dark 減算 → detrend → …) は
  [flat-field-stats.md](flat-field-stats.md) の「順序」節が既に持っている。
  PreProcessor 連鎖はそれを型にする器で、順序の中身は #57 の凍結解除後に
  そちらで確定する。

## 6. SetAnalyzer — set から KPI まで一発

**kind 4 は 1–3 の合成で、独自の算術を持たない** (ユーザー提案の
「1,2,3のコードは再利用する形では実装したい」を規則に昇格):

    SetAnalyzer = PreProcessor 連鎖 (§5) → General / Specific の適用 (§3, §4) → KPI の取り出し

これは v1 の合成則 — series 解析はメンバ stack の stack 解析結果から作る、
`linRecompute` が既にこの形 — の一般化で、理由も持ち上がる:
**数が食い違えない** (同じ蓄積器から出る)、**remote で自然に分業する**
(画素に触る段は peer、fit・KPI はスカラの上で手元)。

有名名の表 (役割スキーマが要求を語る):

| SetAnalyzer | 役割スキーマ | 実装 (1–3 の再利用) | 出力 |
|---|---|---|---|
| **DSNU [DN]** | `{"dark": Stack()}` | 時間 Mean (§5) → 空間σ (§3.2) | sc (+map) |
| **PRNU (直接)** | `{"image": Stack(), "dark": Stack()}` | Mean → dark 減算 (§5) → σ/μ (§3.2) | sc (+map) |
| **DSNU / PRNU 分離** | `{"sweep": Series(), "dark": Stack()}` | level 毎 σ_fpn → σ_fpn²–μ² 線形回帰 | sc |
| read noise (dark 実測) | `{"dark": Stack()}` | σ_t (§3.2 Temporal) | sc |
| **暗電流 [DN/s]** | `{"darks": Series(unit=s/ms)}` | level 毎 Mean → 線形 fit (§3.3) | sc + cv |
| **欠陥画素 (正式)** | `{"dark": Stack(), "flat"?: Stack()}` | Mean → 閾値判定 → **map** | sc + map |
| full well | `{"sweep": Series()}` | PTC の K + 飽和レベル | sc |
| DSNU [e⁻] / PRNU の e⁻ 系 | 上に `{"sweep": Series()}` を足す | K が要る、を役割が語る | sc |

- **"EMVA 1288" の文字列は、規格原文と照合するまでどの画面にも出さない**
  ([flat-field-stats.md](flat-field-stats.md) の確定規則の再掲。フレーム
  リニアリティの「-style を必ず付ける」と同じ線)。照合が済むまでの名乗りは
  ツール固有カテゴリ。
- 1役割の set (DSNU など) の実行は**メニュー1クリックでよい**: クリックの場で
  役割を埋めさせる (= その場の束縛が宣言)。その set を常に Files ノードに
  するかは判断2 — 実行のたびにノードが増えるのは Files を散らかす。

## 7. PRNU を名乗る数の住み分け — #66 の解 (v2)

「General に有名名を付けない」というユーザー自身の規則により、v1 の
「裸名の予約」は型の帰結になった: **裸の `PRNU` / `DSNU` を出せるのは、
役割が束ばれた SetAnalyzer だけ**。4つの数の配置:

| 量 | 種と層 | 名前 | 扱い |
|---|---|---|---|
| ROI の σ/mean (そのまま) | General / frame (ROIs 列) | `PRNU [σ %]` | **唯一の例外** — General に居る有名名。ユーザー決定 (#65) の名前で、修飾 `[σ %]` とツールチップが層を言う。維持を推奨するが、新規則の作者に明示的に問う (判断4) |
| 9×9 ローパス残差の σ/mean | Specific / frame | `uniformity/prnu-fpn` の `prnu_pct` | 現名のまま。proxy と自己申告済み ([analyzers.md](analyzers.md))。改名は報告書の連続性を壊すだけ |
| dark を引いた flat の固定パターン | **SetAnalyzer** | 裸の `PRNU` (+ set タグ: `— set: image n/N, dark n/N`) | 未実装。ここが「本物」の住所 |
| σ_fpn²(μ) フィットの傾き / 切片 | **SetAnalyzer** | 裸の `PRNU` / `DSNU` (+ set タグ: `— sweep: <series>, M levels`) | 未実装。加法・乗法の**分離**はここでしか出来ない ([flat-field-stats.md](flat-field-stats.md) (c)) |

直接推定と分離フィットは**推定量が違う**ので、set タグがそのまま区別になる。
同じ表に並べるときはタグを省略しない。manual の相互注記 (frame 層の2つ) は
残す。

## 8. Provenance — 結果が運ぶもの (#46)

measure-ux.md の原則 (出力はそれ単体で証拠になる) の層別具体化。
**下の表の欄を運ばない結果は、報告書に引用できない。**

全層共通:

| 欄 | 中身 |
|---|---|
| 量と単位 | 既存規則のまま (キー名 / dtype 由来。単位は宣言、推測しない) |
| 実行主体 | `[local]` / `[server <host>]` |
| 時刻・所要 | 既存の出所行のまま |
| 計算者 | viewer 版、または**プラグイン name + version + ファイル** (#46。version は ABI v3 の新設欄 §4、**dll パスはホスト側の帳簿で足りるので ABI を待たない** — 判断6) |
| 数値を変えるパラメータ | detrend の方式と窓、畳み方 (mean_t 等)。「カットオフを必ず言う」(flat-field-stats) はこの欄の話 |

層ごとに足すもの:

| 層 | 追加で運ぶもの |
|---|---|
| frame | frame 名 (path)、領域 (whole / ROI 矩形)、dtype、CFA 解釈 |
| stack | stack 名 + **先頭フレームの path** (セッションを跨ぐ同定の既定 — series メンバ・frame 平均レシピの先例)、**n of N**、除外した非有限サンプル数、領域、全画素かサンプリング格子か (export-design §3.1 と同じ理由) |
| series | series 名、kind、パラメータ名+単位、メンバ表 (stack・値・include)、fit の式 — **測定時点の series の中身** (編集で数値を破棄する規則の裏面: 破棄されずに残る唯一のものが provenance の写し) |
| **AnalysisSet** | set 名、**役割 → メンバの対応** (各メンバは上の各層の欄を再帰的に運ぶ)、適用した PreProcessor 連鎖と版、中間生成物の所在 (保存したなら) |
| 画像出力 (frame / map) | 名前と note に由来、セッションにはレシピ (§5。`0e4d4ec` の不変条件) |

**AnalysisSet は provenance の構造化そのものである。** #46 が結果に持たせた
かったもの — 何を、どの役割で、何から計算したか — は set の定義と一致する。
SetAnalyzer の結果は自分の set を指せば provenance の大半が済む。
これが v1 の「dark/flat 宣言」を set に移したもう1つの利得で、
宣言と証拠が同じ場所に居る。

## 9. v1 の8判断との対応

ユーザー提案が何を解いたかの帳簿。**解消 = もう訊かない / 変換 = 形を変えて
§12 に残る / 存続 = そのまま §12 に残る**:

| v1 判断 | 帰結 |
|---|---|
| 1 シグネチャ vs フィールド | **解消 (確定)** — 提案自身が Specific は「シグネチャで判断」、set 系は「役割スキーマ」と署名宣言を採った。ABI v3 の方向 (§4) はそのまま |
| 2 下位層での拒否 vs 格下げ | **解消 (構造的)** — General は Files の選択が層を選ぶので、間違った層で実行する操作が存在しない (§3.4)。残るのは Specific / Set の署名不充足の拒否のみで、これは論点ではなく帰結 |
| 3 裸名 PRNU/DSNU の予約 | **解消 (一致)** — 「General に有名名を付けない」はこの規則そのもの。残るのは ROIs `PRNU [σ %]` の唯一例外の扱い (新判断4)。EMVA ゲートは従前どおり (flat-field-stats の確定規則) |
| 4 dark/flat 宣言の所在 | **解消 (置換)** — stack 属性 (v1 推奨) より set の役割束縛が優る (§1.2)。v1 案は取り下げ |
| 5 部分ロード n/N | **存続** (新判断5) — 再構成の影響なし。§3.5 のまま |
| 6 provenance 2段 (#46) | **存続** (新判断6) — set が構造化 provenance になる分、内容はむしろ強まった (§8) |
| 7 フレームリニアリティ | **存続** (新判断7) — Series Analysis パネル担当という着地が明確になった (§3.3) |
| 8 正典への3行追記 | **変換・拡大** (新判断8) — 階層挿入と束縛例外を含む正典修正パッケージになった (§11) |

## 10. この文書が決めないこと

- **map の細部** (#49): 語彙の確定 (評価値マップ / map)、表示レンジ、
  セッション、export、読み込み経路 (リーダが set の役割へ届ける形)。
  帰属先 (set) は決まった (§1.3)。
- **推定量の細部** (#57): σ_fpn の −σ_t²/N 補正、detrend の既定と置き場所
  (B2/B4/B5)、直接 PRNU の式 (分散差か画素毎減算か)。前処理の**順序**の中身も
  flat-field-stats の節をそのまま凍結解除で。本書は住所と名前だけ確定した。
- **EMVA 1288 準拠の照合**: 規格原文に当たる作業そのもの (§6 のゲート)。
- **ABI v3 の構造体の確定**、stack をプラグインに見せる運搬 (共有メモリ、
  Python 常駐ワーカ #45)、remote MEASURE op の拡張。§4 は方向であって寸法
  ではない。kind 3/4 のプラグイン口も同じ棚。
- **AnalysisSet の実装細部**: セッション形式 (series ブロックの先例に従う、
  までを方針として)、Files の見え方、束縛 UI、picker との関係。
- **series 横断・batch 横断の表** (正典が「将来」と記す欄)。2軸掃引
  (温度×PTC) を set の役割で表す誘惑もここに置く — 役割は名義 (nominal) で
  あって軸 (ordinal) ではないので、安易にやると series をもう1回発明する
  ことになる。必要が実物として現れるまで作らない。

## 11. 正典 ([terminology.md](terminology.md)) への修正提案

いずれも**修正として明示的に**入れる (判断8。黙って混ぜない):

1. **階層の挿入**: 4つの層の表に **AnalysisSet** を加え、包含関係を
   `frame ⊂ stack ⊂ series ⊂ AnalysisSet ⊂ batch` に改める。ただし
   **AnalysisSet のメンバ関係だけは束縛 (参照)** で、メンバは複数の set から
   参照されてよい — 厳密包含の唯一の例外として、理由 (参照データの再利用が
   通常運用) と共に明記する。set 自身は1つの batch に属する。
2. **用語の追加**: AnalysisSet と解析の4種 (General Analyzer / Specific
   Analyzer / AnalysisPreProcessor / SetAnalyzer) を用語表に載せ、
   1物1名を守る (それぞれの和名を与えるかは表で決める)。
3. **不変条件の追加 (v1 の3行の言い換え)**:
   (a) **解析結果は自分の層と (set 解析なら) set を名乗る** — provenance が
   層・対象・役割を運ぶ。運ばない数値は報告書に引用できない。
   (b) **前処理・参照を含意する有名名は SetAnalyzer だけが名乗る。** General
   は中立な量の名前だけを出す (frame 層の既存の修飾名 `PRNU [σ %]` /
   `prnu_pct` は明示された例外)。
   (c) **役割の束縛が宣言である。推論しない** — フォルダ名・level 0 から
   dark を推測して束ねない (「level 0 = dark」は linearity の後方互換に閉じる)。

## 12. 判断 — ユーザーに諮る項目 (v2)

1. **AnalysisSet のメンバは包含か、束縛か。**
   推奨: **束縛** (§1.1 — メンバは複数 set から参照可、set 自身は1 batch)。
   厳密包含の唯一の例外として正典に明記。
   別を選ぶと (厳密包含): 同じ dark を2つの解析で使うたびに stack を開き
   直すか複製することになる — 参照データの通常運用と正面衝突する。

2. **set の実体化のタイミング。**
   推奨: SetAnalyzer の1クリック実行は**暗黙 set** (結果の provenance が set の
   中身を運ぶ) でよく、**Files ノードになるのは保存したとき・中間生成物を
   残したとき・ユーザーが作ったとき**。
   別を選ぶと (常にノード化): DSNU を3回走らせると Files に3ノード —
   測定のたびに帳簿が散らかり、束縛のやり直しも億劫になる。

3. **Stack パネル (Histogram / Projection / ROIs) の時間方向の畳み方。**
   推奨: **既定 = 時間 Mean** (`0e4d4ec` の機構を再利用。σ_fpn と同じ蓄積器
   から出るので Temporal と数が一致する) + **per-frame の重ね/並べ表示**
   (S1×N の見せ方)。**Sum は採らない** (NaN 画素毎除外の下で正直に定義
   できない)。pooled は保留。列名に畳み方を付す (`(mean_t)`)。
   別を選ぶと: Sum を出すなら「画素ごとに何枚の和か」を運ぶ列が要る。
   pooled を今作るなら用途の実例が先。

4. **ROIs の `PRNU [σ %]` 列 — General に残る唯一の有名名。**
   推奨: **現名のまま維持** (ユーザー決定 #65 + 修飾とツールチップ)。正典には
   明示された例外として書く。
   別を選ぶと (新規則に完全準拠): `NU [σ/mean %]` 等へ改名 — あなた自身が
   一度退けた名前に戻ることになるが、規則の作者が選ぶなら成立する。

5. **部分ロード stack への解析 (v1 判断5 のまま)。**
   推奨: 実行して **n/N を必ず併記** + 解析毎の `min_frames` + settle 待ち
   (§3.5)。
   別を選ぶと (完全ロードのみ): メモリ予算で切れた大 stack と remote stack が
   測定不能になり、正典の「何枚中何枚かを必ず併記」の文言変更も伴う。

6. **provenance のプラグイン情報 (#46。v1 判断6 のまま)。**
   推奨: **dll パス+名前はホスト側の帳簿で即対応** (ABI 変更なし)、
   **version 欄は ABI v3 と同時** (§4 の版上げに載せ、版上げを1回で済ます)。
   別を選ぶと: version だけ先行なら版上げが1回増える。

7. **フレームリニアリティの最終形 (v1 判断7 のまま)。**
   推奨: **frame をメンバとする series** へ移行 (§3.3。正典の series 定義に
   最初からある形)。現行の stack + frame 軸は移行までの暫定、frame 軸は
   monitor 用途に純化。
   別を選ぶと: 「同一条件で撮られた frame の並び」という stack の定義に
   例外を書き足すことになる。

8. **正典修正パッケージ (§11) の承認。**
   推奨: 承認。挿入 (階層) と例外 (束縛) は**反転ではないが拡張**なので、
   v1 の「3行の追記」より大きい修正である — だから明示的に諮る。
   別を選ぶと: AnalysisSet は仕様書ローカルの概念に留まり、Files・セッション・
   正典の三者が同じ階層を語れない。
