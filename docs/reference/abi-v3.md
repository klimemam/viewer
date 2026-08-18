# プラグイン ABI v3 — 層で型付いたアナライザ・version 欄・stack の運搬

> **状態: 確定 (2026-08-09, #104)。** [analysis-layers.md](../analysis-layers.md)
> §4 の骨格 (「方向であって寸法ではない」) に寸法を与える実装仕様。§10 が
> ABI v3 に預けた4件 — 構造体の確定・stack の運搬・remote MEASURE の拡張
> 方向・kind 3/4 の口 — と、判断record 6 が v3 と同時と決めた #46 の
> version 欄をここで確定する。**開いていた選択8項は #104 で全部閉じた**
> (「推奨で。」= 全項推奨どおり) ので、§13 は判断リストではなく
> **判断record** であり、本文は全文が決定文である。コードはまだ無い —
> この文書が先で、実装が後 (ヘッダ差分の全景は §12)。

> **実装状況 (2026-08-10) — 段階3 が入った。** 本文は仕様のまま (実装が
> 仕様に追いつく、逆ではない)。段階1 で **§2 (版交渉と互換)・§3
> (`version` / `headline`)・§11 (ホスト帳簿との接続)** と、それを載せる
> **§4 の `psAnalyzerV3` / `psAnalyzeSink3` / `psHostApi::register_analyzer3`**
> が入り (#46 段階2 はこれで閉じた)、段階2 で **§5 (`psStack` /
> `psStackAnalyzerV3` — pull 型アクセス)・§6 (`min_frames` の宣言と n/N の
> 事実の両建て)** と、それを載せる `psHostApi::register_stack_analyzer3`
> が入り、段階3 で **§10 (`MOP_PLUGIN_ANALYZE` — name+version パリティ、
> 不一致は両版併記の拒否、provenance は peer の帳簿から)** が入った
> (`rp::VERSION` 6→7)。ABI は段階3 で **1バイトも動いていない** —— §10 は
> ワイヤの話であり、ヘッダの話ではない。
> 運搬は **§9.1 (in-process、常駐フレームへの参照)** と **§9.3 (remote —
> plugin が画素の側へ行く)** の2つが実物になった。§9.3 の peer 側 `get_frame`
> は要求された時点でそのフレームを読み、`release_frame` は区画を本当に解放する
> —— **§9.2 の窓を、共有メモリではなく peer のストリーミング読み出しで実装した
> もの**であり、pull の口を先に固定しておいたことの2つめの配当である
> (新しい運搬コードはゼロ)。
> **まだ無い**のは §7 (series の形と席)・§8 (`emit_number_u` / `emit_map`)・
> §9.2 (#45 の Python ワーカ区画)、および §5.2 末尾の住処 (Analysis パネル /
> Measure メニューへの stack アナライザの掲示とグレーアウト) —— UI はまだ
> 張っていない。§10 も**ホスト側の口とプロトコルまで**で、Analysis パネルから
> peer 実行を選ぶ導線 (どちら側で走るかの提示と `--remote-policy` との接続) は
> 未着手である。
> 未実装分は §2.2 の探針規則どおり**予約席**に居る: `psHostApi` の
> `register_stack_analyzer3` の後ろ、`psAnalyzeSink3` の `reserved[8]` の
> 先頭2席。**ヘッダに名前を書くのは実装する段階** —— 名前が書かれた瞬間に
> 形の約束が始まるので、席は番地だけ確保して名前は伏せてある (§7.2 が
> kind 3/4 に対して採った線をそのまま全未実装席に適用した)。どれも
> **版上げ無しに**埋まる。
>
> **同梱アナライザの V3 移行 (§1 の機械的作業) は済んだ。** 5本すべてが
> `register_analyzer3` に移り、`version` (いずれも `1.0.0` —— 初めて版を
> 名乗ったという意味であり、新しいという主張ではない) と、それまでホスト側の
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

## 0. 何を決めるか、何を決めないか

決めること:

1. 版交渉 — PS_ABI_VERSION 3 と、以後版上げを繰り返さないための探針規則 (§2)
2. 新設の共通欄 — `version` (#46 段階2) と `headline` (§3)
3. frame シグネチャの v3 形 `psAnalyzerV3` (§4)
4. stack シグネチャ — `psStack` (pull 型) と `psStackAnalyzerV3` (§5)
5. 部分ロードの契約 — `min_frames` (宣言) と n/N (事実) の両建て (§6)
6. series シグネチャ — 形の凍結と、口を開けない理由 (§7)
7. 結果の語彙 — `psAnalyzeSink3`: 単位の宣言欄と `emit_map` (§8)
8. stack をプラグインに見せる運搬 — 3つの居場所と却下案 (§9)
9. remote MEASURE op の拡張方向 (§10)
10. ホスト帳簿 (#46 段階1) との接続 — provenance の表示 (§11)

決めないこと (§1 非目標に理由つきで列挙): map の意味論 (#49)、Python
ワーカのプロセス管理 (#45)、kind 3/4 の役割スキーマ口の中身、remote の
ワイヤ形式、推定量の細部 (#57)。

用語と前提はすべて確定文書: [analysis-layers.md](../analysis-layers.md)
(§12 判断record)、[terminology.md](../terminology.md) (正典)、
[ps_plugin.h](../../include/ps/ps_plugin.h) (現行 ABI v2 — 「never break it,
only extend」がヘッダ自身の約束)、
[plugin_host.h](../../core/plugin_host.h) / [analyzers.md](analyzers.md)
(#46 段階1 の帳簿、PR #98)。

## 1. 非目標

- **map の意味論 (#49)**: 語彙 (欠陥/マスク/ゲイン/信頼度)、表示レンジ、
  セッション保存、export、Files での見え方、set への帰属 UI。v3 が決めるのは
  **画素形の結果が ABI を渡る欄の最小集合だけ** (§8.3)。欄は #49 のどの
  選択肢も塞がないことを確認して選んである。
- **Python 常駐ワーカのプロセス管理 (#45)**: 起動・常駐・ハングの始末・
  共有メモリ区画の後片付け。ABI はプロセスの生死を知らない — §9 は
  「pull 型が共有メモリ運搬にそのまま載る」ことだけを保証する。
- **kind 3/4 (AnalysisPreProcessor / SetAnalyzer) のプラグイン口**:
  analysis-layers.md §4 の確定どおり今回作らない。役割スキーマがまだ
  組み込み実装 (§5–§6) で動いていないものを ABI に凍結するのは順序が逆。
  §7.2 に指定席だけ書く。
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
 *   3 - layer-typed analyzers (docs/reference/abi-v3.md): psAnalyzerV3 (frame,
 *       +version/+headline), psStack + psStackAnalyzerV3 (pull access,
 *       min_frames), psAnalyzeSink3 (emit_number_u, emit_map).
 *       psSeriesAnalyzerV3 is SHAPE-frozen but its register slot may be
 *       NULL - probe it (capability = non-NULL slot). V1/V2 loadable
 *       forever. */
```

互換の不変条件 (すべて現行ヘッダの約束の継続):

- **V1/V2 構造体と登録経路は永久に不変**。同梱アナライザの V3 移行後も
  `register_analyzer` / `register_analyzer2` は残る — サードパーティの
  ビルド済み dll がそこに居る。
- **psHostApi の寸法は変わらない**: v3 の新関数は `reserved[7]` の先頭
  3席を名前に変えるだけ (`register_analyzer2` が v2 でやったのと同じ手)。
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
  (`register_analyzer3` と `register_stack_analyzer3` の2席が非 NULL)。
- **それ以外の新しい席は「指定席 + NULL 探針」**: ヘッダに名前と契約を書き、
  ホストが実装するまで NULL のままにできる。プラグインは「NULL かもしれ
  ない」と文書化された席を**必ず NULL 検査してから**呼ぶ。予約欄は v1 から
  ゼロ充填が義務なので、この探針は ABI 1 のホストに対してさえ安全。
- 席が埋まることは版上げではない (v2→v3 の `register_analyzer2` は
  版上げと同時だったが、それは sink の意味も変えたから)。**版を上げるのは
  既存の意味が変わるときだけ** — 以後この文書の系譜では起きない想定で、
  起きたらそれは v4 の文書が要るということ。

## 3. 新設の共通欄 — version と headline

### 3.1 `const char* version` (#46 段階2)

- **全 v3 記述子の必須欄**。NULL / 空文字は登録拒否 (理由1行)。版を名乗ら
  ない自由は V1/V2 に残っている — v3 を名乗ることが「provenance を運ぶ」
  という宣言なので、v3 で無版は矛盾であり、黙って通さない。
- **静的寿命の UTF-8 自由書式**。ホストは**解釈しない** — semver 順序も
  日付も読まない。表示は逐語、比較は**等値のみ** (§10 のパリティ検査が
  唯一の比較で、それも等しいか否かだけ)。宣言は宣言のまま運ぶ、が
  input-adapters 以来の家風 — 推測も正規化もしない。
- 粒度は**記述子ごと** (dll ごとの第2エクスポートではない): provenance の
  行は「アナライザ名 + version + ファイル」で並び
  (analysis-layers.md §8)、名前の隣に版が住むのが照合の最短距離。
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
  frame の個体識別は `psFrame::name` / `pts_us` が運ぶ。
- **保証**: settle 前には呼ばない (analysis-layers.md §3.5 「ロード中は
  実行しない」)。`frames` は「ロードが本当にそこで止まった」事実の枚数。
- **義務**: 時間方向の集計は **NaN を画素ごとに除外して数え、除外数を
  結果で運ぶ** (`computeStackStats` で確立済みの規律 — 黙って除外だけ
  しない)。分母に畳まない。
- **義務**: get_frame の NULL は err で報告して非0で帰る。部分結果を
  成功の顔で出さない。
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

住処は Analysis パネルのまま (kind 2 Specific の家)。Files の選択が stack
のとき Measure メニューに stack アナライザが並び、frame 選択では
グレーアウト + 理由1文 (署名不充足 — analysis-layers.md §3.4 の残り面、
[measure-ux.md](../features/analysis/measure-ux.md) の既存規則)。

## 6. 部分ロードの契約 — 宣言と事実の両建て

「`min_frames` を宣言させるか、n of N を渡すか」は二者択一ではない。
**両方で、役割が違う**:

| | 誰が | いつ | 何をする |
|---|---|---|---|
| `min_frames` | プラグインが**宣言** | 登録時 | 呼ぶ**前**にホストが裁く。σ_t は 2、RTS は桁違い — 満たさなければ実行前拒否 + 理由1行 (正典 §3.5) |
| `frames` / `expected` | ホストが**事実**を渡す | 呼び出し毎 | 呼ばれた**後**にプラグインが知る。n/N の併記は正典の不変条件で、**ホストの義務** — プラグインが emit しなくても provenance 行は n/N を運ぶ |

- 宣言は呼ぶ前に裁き、事実は呼ばれた後に語る。片方では代替できない:
  min_frames だけだと 8/24 を 8/8 と区別できず provenance が嘘になり、
  n/N だけだと全プラグインが自前の拒否コードを重複実装する。
- **プラグインは `frames < expected` を理由に自分から拒否しない**。部分
  ロードで測ってよいは裁定済み (判断record 5) で、その veto は min_frames
  という形で登録時に一度だけ言う。呼ばれた時点で条件は満たされている。
  警告 (「n が小さく信頼区間が広い」等) を emit するのは自由。
- `min_frames == 0` は登録拒否。既定でごまかさず宣言させる — 1枚でよい
  なら 1 と書く (それは「なぜ stack アナライザなのか」を作者に一度
  問う摩擦でもあり、意図的に残す)。→ 判断 3

## 7. Series — 形は凍結、口は指定席

### 7.1 凍結する形

正典の series 定義 (「条件を1つ振った stack / frame の並び」) と
判断record 7 (frame | stack 混在) をそのまま C にする。**frame メンバは
`frames == 1` の psStack として渡す** — メンバ型の分岐が ABI から消え、
プラグインは1つの型だけ読む:

```c
/* SHAPE-frozen in v3; the register slot may be NULL until the host opens
 * the series mouth - probe it (§2.2, §7.2). */
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
- 「単位と値が無ければ fit しない」は入口で構造化済み: param_unit が
  宣言で、series 側に値の無い状態はこの型では表現できない。

### 7.2 口を開けない — 正典との張力をどう解いたか

ここに正典間の張力が1つある。analysis-layers.md §4/§10 は「series
シグネチャのプラグイン口は今回作らない (組み込みが先に §5–§6 を張り、
外に開くのは形が固まってから)」と確定済み。一方、判断record 6 の原理
(版上げを2回にしない) は「v3 で開けてしまえ」を示唆する。

**確定は両立**: 形 (§7.1) は v3 ヘッダで凍結し、`register_series_analyzer3`
は**指定席のまま NULL** で出荷する。§2.2 の探針規則により、Series
Analysis パネル (PR #102) の組み込み解析が形を検証し終えた時点で
**版上げなしに**席を埋められる。正典の順序 (組み込みが先) と record 6
(2回上げない) を NULL 探針が同時に満たす。→ 判断 4

kind 3/4 (役割スキーマの口) は形すら凍結しない (§1 非目標)。指定席の
**予約番地だけ**確保する: psHostApi の残り予約 4 席のうち末尾 2 席を
「set 系の口のために名前を付けずに取り置く」と本文で言明する (ヘッダには
書かない — 名前を書いた瞬間に形の約束が始まるため)。

## 8. 結果の語彙 — psAnalyzeSink3

### 8.1 継承と新設

```c
typedef struct psMapOut {
    const char*  key;           /* result name; same namespace as number keys */
    uint32_t     x, y, w, h;    /* placement in INPUT pixel coordinates
                                   (whole frame or the analyzed ROI)          */
    const float* data;          /* row-major w*h floats; row i at
                                   (char*)data + i*pitch_bytes.
                                   NaN = "no value at this pixel"             */
    size_t       pitch_bytes;   /* >= w * sizeof(float)                       */
    const char*  unit;          /* REQUIRED, never NULL. "" = declared
                                   unitless (mask). Declared, not inferred    */
    uint64_t     reserved[4];
} psMapOut;

typedef struct psAnalyzeSink3 {
    void* ctx;
    /* unchanged from sink2: */
    void (*emit_number)(void* ctx, const char* key, double value);
    void (*emit_text)  (void* ctx, const char* key, const char* value);
    void (*emit_series)(void* ctx, const char* name, const char* x_label,
                        const char* y_label, const float* x, const float* y,
                        uint32_t n);
    /* v3: scalar with a DECLARED unit. Where a declaration slot exists,
     * declaration beats key-name inference (input-adapters house rule). */
    void (*emit_number_u)(void* ctx, const char* key, double value,
                          const char* unit);
    /* v3: pixel-shaped result. Host copies during the call; returns 0 if
     * accepted (nonzero: host cannot take maps in this context). */
    int32_t (*emit_map)(void* ctx, const psMapOut* m);
    void* reserved[6];
} psAnalyzeSink3;
```

すべての emit は従来どおり**呼び出し中にホストがコピー**する。sink3 は
v3 の analyze 関数にだけ渡る — V1/V2 の sink は永久に不変。

### 8.2 emit_number_u — キー名規約を fallback に降ろす

emit_number のキー名単位規約 (`snr_db` → dB 等) は「**ABI に単位の欄が
無いから**」が文書化された成立理由 ([analyzers.md](analyzers.md))。
新しい口には欄が作れるので、家訓 (宣言できるなら宣言が勝つ —
[input-adapters.md](../features/adapters/input-adapters.md) §4.3.1) に従い宣言欄を設ける:

- 優先順位: `emit_number_u` の unit > キー名規約 (`unitForAnalysisKey`) >
  画像値系の dtype 由来。v3 プラグイン内でも emit_number は合法のまま
  (規約が fallback として働く) — 移行を強制しない。
- unit も逐語。ホストは換算しない (dB を倍率に直したりしない)。
- float の物理単位問題 (ファイルは反射率か e⁻ かを言わない) はこれで
  **プラグインが知っている場合に限り**閉じる: 暗電流 [DN/s] のような
  計算が単位を確定する量は、キー名を汚さず宣言できる。→ 判断 5

### 8.3 emit_map — 画素形の結果に口を開ける (#49 との境界)

#49 が名指しした欠落 — 「analyzer は emit_number/emit_series しか持たず
画素を返せないので、stack から欠陥マップを作る解析の置き場所が無い」 —
の ABI 側の半分がこれ。**入れないと v3 の主役 (stack アナライザ) が主産物
(欠陥マップ・有効枚数マップ — §3.2 の Sum 申告要件) を出せず、次の版上げ
が確定する** (record 6 の原理に反する)。

正典との線引き: analysis-layers.md §1.3 は「map の型の形そのものは #49 で
決める」と言う。これは**データ層としての map** (Files での型・帰属・
表示) の話で、v3 が決めるのは**運搬中の欄**だけ。欄は #49 のどの候補も
塞がない最小集合に絞った:

- `key` — 結果名。number キーと同じ名前空間 (`R.defect` のように
  チャンネルはキーで分ける。map は常に 1ch — 量が違えば map を分ける)。
- `x, y, w, h` — 入力画素座標での placement。ROI 解析は ROI 分だけの
  map を出してよい (whole を強制すると ROI 解析が無意味に膨らむ)。
- `data` — float32 固定 + `NaN = その画素に値なし`。NaN の画素毎除外の
  規律と同じ語彙で「欠測」を運ぶ。
- `unit` — **必須・NULL 不可**。`""` が「明示的に単位なし (マスク)」。
  #49 の「自前の量と単位 (またはマスクとして明示的に単位無し)」の要件を
  構造にしたもの — 無宣言という状態を型から消す。
- 意味種別 (defect / mask / gain / confidence …) の欄は**置かない**:
  それは #49 の語彙で、確定前に ABI へ写すと #49 の手を縛る。確定後に
  必要なら psMapOut の reserved を名前に変える (探針不要 — 記述子でなく
  値なので、NULL/0 のままなら「無宣言」と読める)。

受け側の最小動作 (これも #49 を待たない範囲): ホストは map を受理して
**結果と一緒に保持し、provenance に載せる**。表示・保存・set への帰属は
#49 確定まで「結果グリッドに『map: key (w×h, unit)』の行が出る」以上を
約束しない。emit_map の戻り値非0は「このホスト文脈では map を受け取れ
ない」 (例: 将来の縮退ホスト) — プラグインは主結果が map しか無いなら
err で失敗し、副産物なら黙って続行してよい。→ 判断 6

## 9. 運搬 — stack をプラグインに見せる3つの居場所

問いは板の行16 (「stack 全体をプラグインに見せる方法 = 共有メモリ運搬層と
同じ問題」) から。**決定: ABI に固定するのは pull 型の口 (§5.1) だけ。
運搬はその裏で3態を取り、プラグインからは見えない。**

### 9.1 in-process C プラグイン (v3 出荷時の唯一の実装)

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

stack を回線に流してプラグインに見せる経路は**作らない**。remote.md の
言葉どおり「答えを得るために原料を輸送」であり、24×48MB は回線で約1.1GB、
返る答えは数十バイト〜map 1枚。stack 集計は peer 側 (`MOP_TEMPORAL_STATS`
の既存の線 — analysis-layers.md §3.5) で、プラグインも同じ線に乗る。

### 9.4 却下した代案 (理由つき)

| 案 | 却下理由 |
|---|---|
| 全枚の `psFrame*` 配列を渡す | 「全部メモリに居る」を ABI が約束してしまう (骨格自身が却下済み)。9.2 の窓運搬と 9.3 が構造的に不可能になる |
| push 型逐次 (begin / on_frame / end をプラグインに実装させる) | 乱択アクセスが消える。2パス解析 (RTS の再走査等) はプラグイン内バッファ = 同じメモリをホストの管理外で持つだけ悪化。制御の反転で min_frames 前拒否との整合も崩れる |
| pull だが release 無し (返るまで全 pin 固定) | in-process では同じだが、9.2 の区画が stack 全体サイズを強制される。release 1本 (呼ばなくても正しい) の追加費用でこの天井が外れる |
| stack を回線で手元に運んで local 実行 | 上記 9.3。既存の `--remote-policy local-fetch` が明示選択の逃げ道として残るだけで、既定経路にはしない |

→ 判断 2 (アクセスモデル)、判断 7 (運搬の3態)。

## 10. Remote — MEASURE op の拡張方向

方向だけ確定する (ワイヤ形式は remote.md の領分 — §1):

- `MSG_MEASURE` に **`MOP_PLUGIN_ANALYZE`** を足す。要求 = アナライザ
  name + version + 対象パス (+frame/stack の別) + roi。応答 = sink
  ストリームの直列化 (number / text / series / map)。map は結果として
  1枚分のバイトが返る — 原料 N 枚を送らないための対価としては安い。
- **パリティは name + version の等値**。peer は自分の plugins ディレクトリ
  から同名を探し、version が一致しなければ**両方の版を並べて理由つき
  拒否** — 黙って local 実行に振り替えない、黙って古い方で計算しない。
  #45 の反論「peer の配備問題はパリティが実測不能」への構造的回答が
  この欄で、version (#46) が生まれて初めて「同じ計算機か」が回線越しに
  **問える**ようになる。ULP 一致の実測ではなく宣言の照合 — 提示の
  規律はここでも宣言で担保する。
- provenance は実行主体 `[server <host>]` + peer 側の name/version/
  ファイル (peer の帳簿から返す)。手元の同名 dll の版を**書かない** —
  計算していないものは名乗らない。
- 部分ロードの n/N は peer 側の事実 (peer が数えた枚数) を運ぶ。
  min_frames の実行前拒否も peer 側で同じ規則。→ 判断 8

## 11. ホスト帳簿との接続 — provenance の表示

#46 の2段が揃う。段階1 (PR #98: `AnalyzerPluginInfo::file/path`、ホスト
帳簿、ABI 不関与) は不変のまま、v3 記述子から version が合流する:

- 帳簿に `version` 列が増える (v3 記述子のみ。V1/V2 は空のまま —
  **推測して埋めない**。dll のファイル版リソースも読まない: 宣言では
  ないので)。
- ステータス行 ([measure-ux.md](../features/analysis/measure-ux.md) §2): `アナライザ名
  (dll名)` → **`アナライザ名 <version> (dll名)`** (version は逐語、v3 のみ)。
  ツールチップのフルパス、Copy (TSV) / Export (CSV) の `# plugin:` 行も
  同様に version を挟む。
- builtin (Temporal export 等) は従来どおり `app: viewer <版>` のみ。
  プラグインタグを着ない (段階1の既存規則、AP11/12 の selftest が守る)。
- analysis-layers.md §8 の計算者欄 (「viewer 版、またはプラグイン name +
  version + ファイル」) がこれで全欄埋まり、**#46 はこの仕様の実装を
  もって閉じられる**。

## 12. ヘッダ差分の全景

実装者向けの一覧 (規範は各節、ここは索引):

| 追加 | 節 |
|---|---|
| `PS_ABI_VERSION` 2u → 3u + 履歴コメント | §2.1 |
| `psAnalyzerV3` (frame; +version/+headline, sink3) | §4 |
| `psStack` (pull: get_frame / release_frame, n/N) | §5.1 |
| `psStackAnalyzerV3` (min_frames) | §5.2 |
| `psSeriesMember` / `psSeries` / `psSeriesAnalyzerV3` (形のみ凍結) | §7.1 |
| `psMapOut` / `psAnalyzeSink3` (emit_number_u, emit_map) | §8.1 |
| `psHostApi`: `reserved[7]` → `register_analyzer3` + `register_stack_analyzer3` + `register_series_analyzer3` (NULL 可・探針) + `reserved[4]` (末尾2席は kind 3/4 取り置き — 本文言明のみ) | §2, §7.2 |

変えないもの: psFrame / psRect / 全 V1/V2 構造体と sink / psDtype 等の
enum / `psRegisterPlugins` の1シンボル規約 / frame_alloc・frame_free の
唯一性 / struct_size。

## 13. 判断record (2026-08-09 確定 — もう「待ち」ではない)

#104 でユーザーが全8項に回答した:「推奨で。」— **全項、推奨どおりに確定**。
番号は草案時の判断リスト (8項目版) のもの。各項が言うのは**何が決まって、
それが今どの節に住んでいるか**だけである — 理由は当の節が持っているので
ここでは繰り返さない。

1. **version の粒度** — 推奨どおり確定 (2026-08-09, #104)。記述子ごとの
   `const char* version` 欄で、dll 単位の第2エクスポートは採らない。
   規範は §3.1、欄そのものは §4 / §5.2 / §7.1 の各構造体、帳簿との合流は
   §11。
2. **stack アクセスモデル** — 推奨どおり確定 (2026-08-09, #104)。pull
   (`get_frame` + `release_frame`)。規範は §5.1、却下した2案 (全枚配列 /
   push 型逐次) は §9.4 の表に理由つきで残る。
3. **部分ロード契約** — 推奨どおり確定 (2026-08-09, #104)。`min_frames`
   (宣言) と `frames`/`expected` (事実) の両建て。規範は §6、欄は §5.1 と
   §5.2。
4. **series の口** — 推奨どおり確定 (2026-08-09, #104)。形は v3 ヘッダで
   凍結し、`register_series_analyzer3` は NULL 席のまま出荷する。形は §7.1、
   席と探針規則は §7.2 と §2.2、ヘッダ差分は §12。正典 §10 (組み込みが先)
   と判断record 6 (版上げを2回にしない) の張力はこの形で解けており、
   **analysis-layers.md 側の修正は要らない**。
5. **単位の宣言欄 emit_number_u** — 推奨どおり確定 (2026-08-09, #104)。
   追加する。優先順位 (宣言 > キー名規約 > dtype) の規範は §8.2、欄は §8.1。
6. **emit_map** — 推奨どおり確定 (2026-08-09, #104)。最小欄で v3 に入れる。
   規範は §8.3、`psMapOut` は §8.1。意味種別の欄は置かない — map の語彙・
   型・表示・帰属は #49 の手に残る (§1 非目標)。
7. **運搬の3態** — 推奨どおり確定 (2026-08-09, #104)。in-process は常駐
   参照、#45 ワーカは共有メモリ窓、remote は peer 側実行。ABI が固定するのは
   pull の口だけ。規範は §9.1–§9.3。
8. **remote 拡張** — 推奨どおり、**方向として**確定 (2026-08-09, #104)。
   `MOP_PLUGIN_ANALYZE` を name+version 等値パリティで足す (不一致は両版
   併記の理由つき拒否)。規範は §10。ワイヤ形式は remote.md の領分のまま
   (§1) — この項が確定させたのは op の存在と成立条件だけである。

これで本書は**実装可能**になった: §12 のヘッダ差分がそのまま
[ps_plugin.h](../../include/ps/ps_plugin.h) への作業指示で、同梱アナライザの
V3 移行は判断の外の機械的作業 (§1)。
