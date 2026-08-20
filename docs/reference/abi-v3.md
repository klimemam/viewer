# プラグイン ABI v3 — 現行契約と決定履歴

> **状態: 中核を実装済み (2026-08-18)。** 公開契約の正典は
> [`include/ps/ps_plugin.h`](../../include/ps/ps_plugin.h) である。本書はその契約を
> 解説し、実装前に検討した案を履歴として残す。
>
> 現行 ABI v3 には `psAnalyzerV3`、`psAnalyzeSink3`、`psStack`、
> `psStackAnalyzerV3`、`register_analyzer3`、`register_stack_analyzer3` がある。
> `MOP_PLUGIN_ANALYZE` の name+version パリティと peer 側実行も実装済みで、
> 現在の remote protocol 版は `core/remote_proto.h::VERSION` を正典とする。
>
> 一方、`psSeriesMember` / `psSeries` / `psSeriesAnalyzerV3` と
> `register_series_analyzer3` は公開ヘッダに無い。`emit_number_u` もまだ名前の
> 付いた callback ではなく、将来候補として `psAnalyzeSink3::reserved[0]` が
> 意図されているだけである。`psMapOut` / `emit_map` は 2026-08-11 に撤回され、
> **どの reserved field も map 用とは約束されていない** (#49)。§7 と §8.2 はこのため
> 非規範の設計履歴、§8.3 は撤回記録として読む。
>
> 未実装なのは Python 常駐ワーカの共有メモリ区画 (#45)、stack アナライザを
> Analysis パネルへ掲示する UI、および peer 実行を UI から選ぶ導線である。
> これらは現行 ABI の存在とは分けて扱う。
>
> **同梱アナライザの V3 移行は済んだ。** 5本すべてが
> `register_analyzer3` に移り、`version` (いずれも `1.0.0` —— version を
> 初めて宣言したという意味であり、新しいという主張ではない) と、それまでホスト側の
> 対応表が供給していた `headline` を宣言する (`stats/moments` だけは
> 「見出しなし」を宣言する)。**ABI はここでも1バイトも動いていない** ——
> 移行は登録経路の変更であって計算の変更ではなく、`--bundled-selftest` が
> 5本 × 4 fixture × (全面/ROI) の結果ドキュメントを stdout に出し、それが
> 移行前後で**バイト一致**することがその証拠である。version の建て方
> (何をもって上げるか、なぜビルド id ではないか) は
> [analyzers.md](analyzers.md) の §version が正典。これで §10 の
> name+version parity が**出荷物に対して初めて使える**ようになり
> (`--rplugin-selftest` RP23–RP26)、#134 判断2 が記録した代償 ——
> 「厳密解釈は正しいが、今日どの同梱アナライザにも `MOP_PLUGIN_ANALYZE` を
> 使えない」 —— は解消された。V1/V2 の登録経路は約束どおり不変で、その
> 実働確認は比較 fixture (`plugins/test/abi_v[23]_twin.c`) が持つ。

## 0. 本書が記述する範囲

決めること:

1. 版交渉 — PS_ABI_VERSION 3 と、以後版上げを繰り返さないための探針規則 (§2)
2. 新設の共通欄 — `version` (#46 段階2) と `headline` (§3)
3. frame シグネチャの v3 形 `psAnalyzerV3` (§4)
4. stack シグネチャ — `psStack` (pull 型) と `psStackAnalyzerV3` (§5)
5. 部分ロードの契約 — `min_frames` (宣言) と n/N (事実) の両建て (§6)
6. series シグネチャ — 当時の提案と、現行ヘッダに採用していない境界 (§7)
7. 結果の語彙 — 現行 `psAnalyzeSink3`、単位 callback の候補、map 撤回 (§8)
8. stack をプラグインに見せる運搬 — 3つの実装形態と却下案 (§9)
9. remote MEASURE op の拡張方向 (§10)
10. ホスト帳簿 (#46 段階1) との接続 — provenance の表示 (§11)

現行 ABI が決めないこと (§1 に理由つきで列挙): map の意味論と運搬 (#49)、Python
ワーカのプロセス管理 (#45)、kind 3/4 の公開役割スキーマ API の内容、remote の
ワイヤ形式、推定量の細部 (#57)。

用語と前提はすべて確定文書: [analysis-layers.md](../analysis-layers.md)
(§12 判断record)、[terminology.md](../terminology.md) (正典)、
[ps_plugin.h](../../include/ps/ps_plugin.h) (現行 ABI v3 — 「never break it,
only extend」がヘッダ自身の約束)、
[plugin_host.h](../../core/plugin_host.h) / [analyzers.md](analyzers.md)
(#46 段階1 の帳簿、PR #98)。

## 1. 非目標

- **map の意味論と ABI 運搬 (#49)**: 語彙 (欠陥/マスク/ゲイン/信頼度)、
  表示レンジ、セッション保存、export、Files での見え方、set への帰属 UI。
  初期案の `psMapOut` / `emit_map` は撤回済みで、現行 v3 は画素形の結果を
  受け取らない (§8.3)。再導入時は reserved field の割当てだけで済ませず、これらの製品契約を
  決めた明示的な新仕様が必要になる。
- **Python 常駐ワーカのプロセス管理 (#45)**: 起動・常駐・ハングの始末・
  共有メモリ区画の後片付け。ABI はプロセスの生死を知らない — §9 は
  「pull 型が共有メモリ運搬にそのまま載る」ことだけを保証する。
- **kind 3/4 (AnalysisPreProcessor / SetAnalyzer) の公開プラグイン API**:
  [analysis-layers.md](../analysis-layers.md) §4 の確定どおり今回作らない。
  組み込みの detrend と Set Analysis は実装済みだが、それは外部向けの
  役割スキーマ、所有権、失敗契約まで凍結したことを意味しない。現行ヘッダには
  名前付きの登録関数も無い (§7.2)。
- **remote のワイヤ形式**: メッセージのバイト配置・版は
  [remote.md](../features/remote/remote.md) の領分。§10 は op の存在と成立条件 (パリティ) の
  方向だけを決める。
- **display / processor の v3 版**: 要求が実物として出ていない。V1 のまま。
  version 欄が欲しくなったら v3 と同じ形 (§3) を写す — 探針規則 (§2.2) が
  あるので版上げは要らない。
- **params_schema の中身**: 全 v3 構造体で引き続き予約 (NULL)。
- **CUDA / ホットリロード / 同梱アナライザの V3 移行作業そのもの**
  (移行は機械的作業 — 判断の外)。

## 2. 版交渉と互換

### 2.1 PS_ABI_VERSION = 3

```c
#define PS_ABI_VERSION 3u
/* Host API version history:
 *   1 - display / analyzer / processor V1 structs
 *   2 - adds psAnalyzerV2 (emit_series curves + description) via
 *       psHostApi::register_analyzer2. V1 structs keep abi_version = 1
 *       and remain loadable forever.
 *   3 - psAnalyzerV3 (+version/+headline) over psAnalyzeSink3, and
 *       psStack + psStackAnalyzerV3 (pull access, min_frames).
 *       The remaining ideas use reserved seats only after a public name and
 *       contract are added. emit_map was withdrawn (#49). */
```

これは現行ヘッダの要約である。`psAnalyzeSink3` に名前の付いた emit は
`emit_number` / `emit_text` / `emit_series` の3つだけで、series 入力用の型や
登録 slot もまだ宣言されていない。

互換の不変条件 (すべて現行ヘッダの約束の継続):

- **V1/V2 構造体と登録経路は永久に不変**。同梱アナライザの V3 移行後も
  `register_analyzer` / `register_analyzer2` は残る — サードパーティの
  ビルド済み dll がそこに居る。
- **psHostApi のサイズは変わらない**: v3 の2関数は v2 時点の reserved field を
  `register_analyzer3` / `register_stack_analyzer3` に変え、残りは
  `reserved[5]` のままである (`register_analyzer2` が v2 で行ったのと同じ手)。
  `struct_size` は同値のまま — 探針としての意味も変わらない。
- **v2 でコンパイルされたプラグイン**は `host->abi_version < 2` を検査して
  3 を合格させる (既存バイナリの動作そのもの)。何も要らない。
- **v3 でコンパイルされたプラグインが v2 ホストに刺さった場合**: ヘッダの
  既存規則のまま — `host->abi_version < PS_ABI_VERSION` (=3) なら何も登録
  せず非0で帰る。v2 ホストで動きたい作者は v2 ヘッダでビルドする自由を
  失っていない。
- 記述子の検査は現行 `validCommon` の線: v3 構造体は `abi_version == 3` の
  完全一致、`PS_CAP_CPU` 必須、name / 関数ポインタ非 NULL。v3 で加わる
  検査は §3 (version 非空) と §5 (min_frames ≥ 1) の2つ。

### 2.2 探針規則 — 版上げを繰り返さないために

判断record 6 の理由 (「ABI の版上げを2回にしない」) を一般規則に昇格する:

- **`abi_version >= 3` が保証するのは v3 の中核だけ**
  (`register_analyzer3` と `register_stack_analyzer3` の2関数が非 NULL)。
- **それ以外の機能は「名前付き field + NULL 検査」で検出する**: ヘッダに名前と契約を書き、
  ホストが実装するまで NULL のままにできる。プラグインは「NULL かもしれ
  ない」と文書化された field を**必ず NULL 検査してから**呼ぶ。reserved field は v1 から
  ゼロ充填が義務なので、この探針は ABI 1 のホストに対してさえ安全。
- **現時点では中核以外に名前の付いた field は無い**。設計文書だけに現れる
  `register_series_analyzer3` や `emit_number_u` を、予約配列からキャストして
  呼んではならない。公開ヘッダに名前と契約が入るまでは能力ではない。
- reserved field に名前付き機能を割り当てることは版上げではない (v2→v3 の `register_analyzer2` は
  版上げと同時だったが、それは sink の意味も変えたから)。**版を上げるのは
  既存の意味が変わるときだけ** — 以後この文書の系譜では起きない想定で、
  起きたらそれは v4 の文書が要るということ。

## 3. 新設の共通欄 — version と headline

### 3.1 `const char* version` (#46 段階2)

- **全 v3 記述子の必須欄**。NULL / 空文字は登録拒否 (理由1行)。版を宣言し
  ない自由は V1/V2 に残っている — v3 を使用することが「provenance を記録する」
  という宣言なので、v3 で無版は矛盾であり、黙って通さない。
- **静的寿命の UTF-8 自由書式**。ホストは**解釈しない** — semver 順序も
  日付も読まない。表示は文字列をそのまま使い、比較は**等値のみ**とする (§10 のパリティ検査が
  唯一の比較で、それも等しいか否かだけ)。宣言を解釈せずそのまま扱うことが、
  input-adapters 以来の規則であり、推測も正規化もしない。
- 粒度は**記述子ごと** (dll ごとの第2エクスポートではない): provenance の
  行は「アナライザ名 + version + ファイル」で並び
  (analysis-layers.md §8)、名前の隣に版を置くのが最も照合しやすい。
  ファイル列は帳簿 (#46 段階1) が既に持っていて ABI を要らない —
  ABI に足すのは**プラグインしか知り得ないもの**だけ、という段階1の
  切り分け ([plugin_host.h](../../core/plugin_host.h) のコメント) の裏面が
  この欄である。→ 判断 1

### 3.2 `const char* headline`

見出し数値 (アクセント強調される主目的キー) は現在ホスト側の対応表
(`.noise` / `.prnu_pct` / `tenengrad` / `mtf50 (cy/px)`) で、
[analyzers.md](analyzers.md) が「ABI v3 でプラグイン申告に移す予定」と
既に予告している。v3 記述子に `headline` を新設する:

- チャンネル接頭辞 (`chN.` / `R.` 等) を剥いだ後のキー名1つ。ホストは
  emit されたキーの接頭辞剥ぎ後がこれに一致したらアクセントする。
- NULL 可 (見出し無し)。ホスト対応表は V1/V2 の後方互換としてだけ残り、
  v3 記述子には適用しない — 宣言できるなら宣言が勝つ。

## 4. Frame — psAnalyzerV3

シグネチャは V2 と同じ1枚受け取り (analysis-layers.md §4: 「frame
シグネチャは今の `analyze(const psFrame*, …)` のまま」)。変わるのは
記述子の欄と sink の型だけ:

```c
typedef struct psAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 (version of THIS struct)              */
    uint32_t    caps;           /* PS_CAP_CPU mandatory                      */
    const char* name;           /* "category/name", static lifetime          */
    const char* version;        /* REQUIRED non-empty (#46 stage 2), static  */
    const char* description;    /* one-line precondition hint; may be NULL   */
    const char* params_schema;  /* reserved: pass NULL                       */
    const char* headline;       /* accent key, channel-stripped; may be NULL */
    int32_t (*analyze)(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink3* sink, char* err, size_t err_cap);
    void*       reserved[4];
} psAnalyzerV3;
```

frame 用に v3 構造体を**作る**理由: #46 が問題にしたのは今日の Analysis
パネル = frame アナライザの provenance であり、stack だけに version 欄を
作っても #46 は閉じない。同梱アナライザの移行は analyze の型を sink3 に
変えて2欄埋めるだけの機械的作業 (別タスク)。

呼び出し契約は V2 から不変: roi NULL = 全面、sink に渡したものは呼び出し
中にホストがコピー、失敗は err に UTF-8 の理由 + 非0。ホストは1記述子への
呼び出しを直列化する (V2 で暗黙だった約束の明文化 — 再入対応を書かなくて
よい)。

## 5. Stack — psStack と psStackAnalyzerV3

### 5.1 psStack — pull 型アクセス

analysis-layers.md §4 の骨格に寸法を与える。全枚配列を渡さない理由は
骨格に書いてあるとおり — 「全部メモリに居る」を ABI が約束してしまう:

```c
/* A stack as shown to a plugin. The host serves frames on demand (pull);
 * what lives behind get_frame - resident memory, a shared-memory segment,
 * a file - is host business and invisible here (docs/reference/abi-v3.md §9). */
typedef struct psStack {
    uint32_t    frames;         /* n: frames the host will serve, 0..frames-1  */
    uint32_t    expected;       /* N: declared stack size. frames < expected
                                   = partial load (§6); never the reverse      */
    uint32_t    w, h, ch;       /* every served frame has this geometry        */
    psDtype     dtype;          /* v3 host: always PS_DTYPE_F32                */
    const char* name;           /* stack name, UTF-8, valid for the call only  */
    const char* meta_json;      /* reserved; may be NULL                       */
    void*       ctx;            /* opaque; pass to get_frame / release_frame   */
    /* Returned pointer is valid until release_frame(ctx, index) or until
     * analyze_stack returns, whichever comes first. May return NULL (frame
     * lost, transport failure, pin budget exceeded): then write err and
     * return nonzero - never fabricate a frame.                              */
    const psFrame* (*get_frame)(void* ctx, uint32_t index);
    /* Optional to call. Releasing lets the host recycle the slot (bounded
     * shared-memory window, §9.2). Never-released frames stay valid until
     * return - naive sequential plugins are correct, just greedy.           */
    void (*release_frame)(void* ctx, uint32_t index);
    uint64_t    reserved[4];
} psStack;
```

ホストの保証 / プラグインの義務:

- **保証**: 全 served frame は同一の w/h/ch/dtype (寸法違い・preview は
  ホストが数える前に除外する — frame 平均の先例)。index は stack 順。
  frame の個体識別には `psFrame::name` / `pts_us` を使う。
- **保証**: settle 前には呼ばない (analysis-layers.md §3.5 「ロード中は
  実行しない」)。`frames` は「ロードが本当にそこで止まった」事実の枚数。
- **義務**: 時間方向の集計は **NaN を画素ごとに除外して数え、除外数を
  結果に記録する** (`computeStackStats` で確立済みの規律 — 黙って除外だけ
  しない)。分母に畳まない。
- **義務**: get_frame の NULL は err で報告して非0で帰る。部分結果を
  成功した結果として出さない。
- roi の意味は frame と同じ (NULL = 全面)。get_frame は常に全面の frame を
  見せる — 運搬の最適化 (ROI 行だけ転送する等) は座標系が見かけ上変わら
  ない限りホストの自由。

→ アクセスモデルの選定理由と却下案は §9。判断 2。

### 5.2 psStackAnalyzerV3

```c
typedef struct psStackAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 (version of THIS struct)               */
    uint32_t    caps;           /* PS_CAP_CPU mandatory                       */
    uint32_t    min_frames;     /* >= 1. Host refuses BEFORE calling when
                                   frames < min_frames (one-line reason).
                                   0 is rejected at registration - declare,
                                   don't default (§6)                         */
    uint32_t    _pad;           /* keep 8-byte alignment of pointers          */
    const char* name;           /* "category/name", static lifetime           */
    const char* version;        /* REQUIRED non-empty (#46 stage 2), static   */
    const char* description;    /* one-line precondition hint; may be NULL    */
    const char* params_schema;  /* reserved: pass NULL                        */
    const char* headline;       /* accent key; may be NULL                    */
    int32_t (*analyze_stack)(const psStack* in, const psRect* roi,
                             const psAnalyzeSink3* sink,
                             char* err, size_t err_cap);
    void*       reserved[4];
} psStackAnalyzerV3;
```

骨格 (analysis-layers.md §4) からの差分は3つ、いずれも既存規約の合流:
`caps` (validCommon が CPU 必須を検査する現行の線)、`params_schema`
(V2 と同じ予約欄 — 将来 UI を1系統で張るため)、`headline` (§3.2)。

表示・操作箇所は Analysis パネルのままとする (kind 2 Specific)。Files の選択が stack
のとき Measure メニューに stack アナライザが並び、frame 選択では
グレーアウト + 理由1文 (署名不充足 — analysis-layers.md §3.4 の残るケース、
[measure-ux.md](../features/analysis/measure-ux.md) の既存規則)。

## 6. 部分ロードの契約 — 宣言と事実の両建て

「`min_frames` を宣言させるか、n of N を渡すか」は二者択一ではない。
**両方で、役割が違う**:

| | 誰が | いつ | 何をする |
|---|---|---|---|
| `min_frames` | プラグインが**宣言** | 登録時 | 呼ぶ**前**にホストが裁く。σ_t は 2、RTS は桁違い — 満たさなければ実行前拒否 + 理由1行 (正典 §3.5) |
| `frames` / `expected` | ホストが**事実**を渡す | 呼び出し毎 | 呼ばれた**後**にプラグインが知る。n/N の併記は正典の不変条件で、**ホストの義務** — プラグインが emit しなくても provenance 行に n/N を記録する |

- 宣言は呼ぶ前に裁き、事実は呼ばれた後に語る。片方では代替できない:
  min_frames だけだと 8/24 を 8/8 と区別できず provenance が実際の入力を表さず、
  n/N だけだと全プラグインが自前の拒否コードを重複実装する。
- **プラグインは `frames < expected` を理由に自分から拒否しない**。部分
  ロードで測ってよいは裁定済み (判断record 5) で、その veto は min_frames
  という形で登録時に一度だけ言う。呼ばれた時点で条件は満たされている。
  警告 (「n が小さく信頼区間が広い」等) を emit するのは自由。
- `min_frames == 0` は登録拒否。既定でごまかさず宣言させる — 1枚でよい
  なら 1 と書く (それは「なぜ stack アナライザなのか」を作者に一度
  問う摩擦でもあり、意図的に残す)。→ 判断 3

## 7. Series — 設計履歴 (非規範、現行 ABI には未採用)

### 7.1 当初提案した形

以下は #104 時点に、正典の series 定義 (「条件を1つ振った stack / frame の
並び」) と判断record 7 (frame | stack 混在) を C に写した**設計案**である。
**公開ヘッダにはこれらの型が存在せず、プラグインはこの形を契約として
コンパイルしてはならない。** 提案では frame メンバを
**`frames == 1` の psStack として渡す** — メンバ型の分岐が ABI から消え、
プラグインは1つの型だけ読む:

```c
/* NON-NORMATIVE historical sketch; absent from the public header. */
typedef struct psSeriesMember {
    double         value;       /* swept-parameter value for this member      */
    const psStack* stack;       /* frame member = psStack with frames == 1    */
    uint64_t       reserved[2];
} psSeriesMember;

typedef struct psSeries {
    uint32_t    count;          /* included members only, series order        */
    const char* kind;           /* series kind (canon vocabulary)             */
    const char* param_name;     /* axis name  - declared, never inferred      */
    const char* param_unit;     /* axis unit; "" = declared unitless          */
    const psSeriesMember* members;
    const char* name;           /* series name, valid for the call only      */
    void*       ctx;
    uint64_t    reserved[4];
} psSeries;

typedef struct psSeriesAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 */
    uint32_t    caps;
    uint32_t    min_members;    /* same contract as min_frames (§6)           */
    uint32_t    _pad;
    const char* name;
    const char* version;        /* REQUIRED non-empty                         */
    const char* description;
    const char* params_schema;  /* reserved: NULL                             */
    const char* headline;
    int32_t (*analyze_series)(const psSeries* in, const psRect* roi,
                              const psAnalyzeSink3* sink,
                              char* err, size_t err_cap);
    void*       reserved[4];
} psSeriesAnalyzerV3;
```

- include を外されたメンバはそもそも渡らない (`count` は included のみ)。
  編集状態はホストの事実で、プラグインに flag を読ませない。
- 「単位と値が無ければ fit しない」という条件は入力型に組み込み済みである: param_unit が
  宣言で、series 側に値の無い状態はこの型では表現できない。

### 7.2 当初案と現在の適用結果

ここに正典間の張力が1つある。analysis-layers.md §4/§10 は「series
シグネチャの公開プラグイン API は今回作らない (まず組み込み実装で §5–§6 を検証し、
外に開くのは形が固まってから)」と確定済み。一方、判断record 6 の原理
(版上げを2回にしない) は「v3 で開けてしまえ」を示唆する。

当初案は、§7.1 の形を v3 ヘッダで凍結し、
`register_series_analyzer3` を NULL 検査可能な名前付き field として出荷するものだった。
**この部分は現行公開ヘッダには適用されていない。** 現在の `psHostApi` は
`register_stack_analyzer3` の後ろが単なる `reserved[5]` で、series や kind 3/4
に割り当てた番地は無い。組み込み Series Analysis が実装済みであることも、
プラグイン ABI の型や登録関数が存在することを意味しない。

将来追加する場合は、§2.2 の探針規則に従い、公開ヘッダへ型・slot 名・NULL 時の
縮退動作を同時に記載する。その時点までは §7.1 は比較材料に留まる。→ 判断 4 の
**歴史的提案**

## 8. 結果の語彙 — 現行 `psAnalyzeSink3` と撤回済み提案

### 8.1 現行公開契約

```c
typedef struct psAnalyzeSink3 {
    void* ctx;
    void (*emit_number)(void* ctx, const char* key, double value);
    void (*emit_text)  (void* ctx, const char* key, const char* value);
    void (*emit_series)(void* ctx, const char* name, const char* x_label,
                        const char* y_label, const float* x, const float* y,
                        uint32_t n);
    void* reserved[8];
} psAnalyzeSink3;
```

名前の付いた3つの emit は、従来どおり**呼び出し中にホストがコピー**する。
sink3 は v3 の analyze 関数にだけ渡り、V1/V2 の sink は永久に不変である。
予約配列を独自に関数 pointer へ読み替えることは契約外である。

### 8.2 `emit_number_u` — 将来候補 (まだ公開 API ではない)

`emit_number` のキー名単位規約 (`snr_db` → dB 等) は「ABI に単位欄が無い」
ための現行 fallback である ([analyzers.md](analyzers.md))。設計時には
`emit_number_u(ctx, key, value, unit)` を `reserved[0]` に置き、宣言単位を
キー名規約より優先する案を採った。現行ヘッダのコメントも最初の reserved field をこの候補に
意図しているが、**field 名も関数型もまだ公開されていないため呼べない**。

実装する場合は、公開ヘッダへ名前・型・NULL 時の fallback を同時に追加する。
その時の優先順位候補は「宣言単位 > `unitForAnalysisKey` > dtype 由来」、単位は
文字列をそのまま扱い換算しない、という当初案である
([input-adapters.md](../features/adapters/input-adapters.md) §4.3.1)。→ 判断 5 の
**未適用部分**

### 8.3 `psMapOut` / `emit_map` — 撤回済み (#49)

初期案では、`key`、入力座標 `x/y/w/h`、pitch 付き float32 `data`、`unit` を
持つ `psMapOut` と `emit_map` を検討した。しかし map の語彙・型・表示・保存・
set への帰属を決めないまま ABI の運搬だけを固定できないため、2026-08-11 に
撤回した。

現行公開ヘッダには `psMapOut` も `emit_map` callback も無い。さらにヘッダは
`psAnalyzeSink3::reserved[8]` のどの field も map 用ではないと明記している。
したがって現行プラグインは画素形の結果を返せず、remote 応答も map を
直列化しない。再導入するなら #49 で意味論・所有権・寿命・失敗時の扱いまで
決め、新たな明示契約としてレビューする。→ 判断 6 の**撤回記録**

## 9. 運搬 — stack をプラグインに見せる3つの実装形態

問いは板の行16 (「stack 全体をプラグインに見せる方法 = 共有メモリ運搬層と
同じ問題」) から。**決定: ABI に固定するのは pull 型のアクセス API (§5.1) だけ。
運搬はその裏で3態を取り、プラグインからは見えない。**

### 9.1 in-process C プラグイン (実装済み)

get_frame は viewer が既に常駐させている float32 フレームへの**ポインタを
返すだけ** (FrameSource は全ロード済みフレームを常駐で持つ — 運搬コスト0)。
release_frame は no-op。v3 の初期実装に運搬層の新規コードはほぼ無い —
これが pull 型の第1の配当: **今日の形をそのまま ABI にできる**。

### 9.2 Python 常駐ワーカ (#45) — 共有メモリ区画 (方向)

実測 (plugin-python-study): 区画に居るフレームの提示 0.04ms、ホストが
書く場合 0.051ms/MB。ワーカ側シムが psStack と同形の pull API を Python に
見せ、get_frame = 「frame i を区画に載せて offset を返す」、release_frame
= 「slot を再利用してよい」になる。**release があるから区画は stack
全体でなく窓で済む** — 24×48MB の stack を 2–3 slot の区画で流せる。
逐次で書く素朴なプラグインは全 pin のまま動く (正しいが貪欲、§5.1) ので、
区画ホストは pin 予算超過を get_frame の NULL で言う。プロセス管理
(常駐・ハング・後片付け) は #45 の領分のまま (§1)。

### 9.3 remote — plugin が画素の側へ行く (§10)

stack を回線に流してプラグインに見せる経路は**作らない**。
[remote.md](../features/remote/remote.md) の
言葉どおり「答えを得るために原料を輸送」であり、24×48MB は回線で約1.1GB、
返る答えは数値・テキスト・曲線である。stack 集計は peer 側 (`MOP_TEMPORAL_STATS`
の既存経路 — analysis-layers.md §3.5) で、プラグインも同じ経路を使う。

### 9.4 却下した代案 (理由つき)

| 案 | 却下理由 |
|---|---|
| 全枚の `psFrame*` 配列を渡す | 「全部メモリに居る」を ABI が約束してしまう (骨格自身が却下済み)。9.2 の窓運搬と 9.3 が構造的に不可能になる |
| push 型逐次 (begin / on_frame / end をプラグインに実装させる) | 乱択アクセスが消える。2パス解析 (RTS の再走査等) はプラグイン内バッファ = 同じメモリをホストの管理外で持つだけ悪化。制御の反転で min_frames 前拒否との整合も崩れる |
| pull だが release 無し (返るまで全 pin 固定) | in-process では同じだが、9.2 の区画が stack 全体サイズを強制される。release 1本 (呼ばなくても正しい) の追加費用でこの天井が外れる |
| stack を回線で client 側へ転送して local 実行 | 上記 9.3。既存の `--remote-policy local-fetch` が明示選択の代替経路として残るだけで、既定経路にはしない |

→ 判断 2 (アクセスモデル)、判断 7 (運搬の3態)。

## 10. Remote — 実装済みの MEASURE op

現行の成立条件を記述する (ワイヤ形式は
[remote.md](../features/remote/remote.md) の領分 — §1):

- `MSG_MEASURE` に **`MOP_PLUGIN_ANALYZE`** を足す。要求 = アナライザ
  name + version + 対象パス (+frame/stack の別) + roi。応答 = sink
  ストリームの直列化 (number / text / series)。撤回済みの `emit_map` は
  応答語彙に含まれない (§8.3)。
- **パリティは name + version の等値**。peer は自分の plugins ディレクトリ
  から同名を探し、version が一致しなければ**両方の版を並べて理由つき
  拒否** — 黙って local 実行に振り替えない、黙って古い方で計算しない。
  #45 の反論「peer の配備問題はパリティが実測不能」への構造的回答が
  この欄で、version (#46) が生まれて初めて「同じ計算機か」が回線越しに
  **問える**ようになる。ULP 一致の実測ではなく宣言の照合 — 提示の
  規律はここでも宣言で担保する。
- provenance は実行主体 `[server <host>]` + peer 側の name/version/
  ファイル (peer の帳簿から返す)。client 側の同名 dll の版を**書かない** —
  計算していない実装の情報は表示しない。
- 部分ロードの n/N には peer 側の事実 (peer が数えた枚数) を含める。
  min_frames の実行前拒否も peer 側で同じ規則。→ 判断 8

## 11. ホスト帳簿との接続 — provenance の表示

#46 の2段が揃う。段階1 (PR #98: `AnalyzerPluginInfo::file/path`、ホスト
帳簿、ABI 不関与) は不変のまま、v3 記述子から version が合流する:

- 帳簿に `version` 列が増える (v3 記述子のみ。V1/V2 は空のまま —
  **推測して埋めない**。dll のファイル版リソースも読まない: 宣言では
  ないので)。
- ステータス行 ([measure-ux.md](../features/analysis/measure-ux.md) §2): `アナライザ名
  (dll名)` → **`アナライザ名 <version> (dll名)`** (version はそのまま表示、v3 のみ)。
  ツールチップのフルパス、Copy (TSV) / Export (CSV) の `# plugin:` 行も
  同様に version を挟む。
- builtin (Temporal export 等) は従来どおり `app: viewer <版>` のみ。
  プラグインタグを着ない (段階1の既存規則、AP11/12 の selftest が守る)。
- analysis-layers.md §8 の計算者欄 (「viewer 版、またはプラグイン name +
  version + ファイル」) がこれで全欄埋まり、**#46 はこの仕様の実装を
  もって閉じられる**。

## 12. 現行ヘッダの全景

実装者向け索引。規範は常に
[ps_plugin.h](../../include/ps/ps_plugin.h) であり、この表は照合用である。

| 現行要素 | 契約 | 節 |
|---|---|---|
| `PS_ABI_VERSION` | `3u`。V1/V2 は互換維持 | §2.1 |
| `psAnalyzerV3` | frame、`version` / `headline`、sink3 | §4 |
| `psStack` | pull (`get_frame` / `release_frame`)、n/N | §5.1 |
| `psStackAnalyzerV3` | `min_frames`、sink3 | §5.2 |
| `psAnalyzeSink3` | number / text / series + `reserved[8]` | §8.1 |
| `psHostApi` | `register_analyzer3` + `register_stack_analyzer3` + `reserved[5]` | §2 |
| 現行ヘッダに無いもの | series 入力型/登録関数、`emit_number_u`、`psMapOut` / `emit_map` | §7、§8 |

変えないもの: psFrame / psRect / 全 V1/V2 構造体と sink / psDtype 等の
enum / `psRegisterPlugins` の1シンボル規約 / frame_alloc・frame_free の
唯一性 / struct_size。

## 13. 判断 record と現在の適用結果

#104 では 2026-08-09 のユーザー回答「推奨で。」により8項目を推奨どおり採用した。その後、実装時の検証で
一部を遅延・撤回したため、**過去の採用判断と現行公開契約を同一視しない**。

| # | 当時の判断 | 2026-08-18 時点の適用結果 |
|---:|---|---|
| 1 | version は記述子単位 | **適用済み**。frame / stack V3 と provenance に実装 (§3、§11) |
| 2 | stack は pull 型 | **適用済み**。`get_frame` + `release_frame` (§5、§9) |
| 3 | `min_frames` と n/N を併用 | **適用済み** (§5、§6) |
| 4 | series の形と NULL 検査可能な登録 field を v3 に置く | **未適用**。公開ヘッダに型も名前付き field も無い (§7) |
| 5 | `emit_number_u` を追加 | **保留**。`reserved[0]` は将来候補だが名前付き callback ではない (§8.2) |
| 6 | `emit_map` を追加 | **2026-08-11 に撤回**。`psMapOut` も map 用 reserved field も無い (#49、§8.3) |
| 7 | in-process / Python 窓 / remote peer の3態 | in-process と remote peer は**適用済み**、Python 窓は未実装 (§9) |
| 8 | `MOP_PLUGIN_ANALYZE` と name+version parity | **プロトコル実装済み**。UI の peer 選択導線は未実装 (§10) |

新規プラグインは §7 の型案や撤回済み §8.3 を再現せず、§12 と公開ヘッダだけを
現在のビルド指示として使う。
