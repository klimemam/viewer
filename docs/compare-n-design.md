# compare-n 設計 — N 比較のあるべき姿 (#60 仕様)

**状態: 仕様 (2026-08-13, issue #60 レビュー回答)。実装はしない。**

## この文書の役割

本書は、issue #60 の要約「スロット列・Aはカーソル・上限はA+6」を、
6つの問いに分けて定義した**仕様書**である。

[compare-n.md](compare-n.md) は、設計の指針と実装状況を記録する台帳である。
本書はその台帳を置き換えない。2つの文書は、次のように使い分ける。

| 書く内容 | 書く場所 |
|---|---|
| 仕様の追加・変更 | 本書 |
| 実装が完了した事実 | `compare-n.md` §11 |

両者が矛盾する場合は、本書を正とする。

本書が既存仕様から変更するのは、比較対象数の上限だけである。
従来の上限 **A+B+6（合計8面）**(compare-n.md §7, §8-3) を、
**A+6（合計7面）**へ切り下げる。
理由と計数結果は§4に示す。それ以外の判断は、`compare-n.md`から引き継ぐ。

前提とする正典: [terminology.md](terminology.md) (frame ⊂ stack ⊂ series ⊂
batch、「compare (A/B): frame 対 frame。フレーム番号を保って stack 間比較」)、
[ab-stats-plan.md](ab-stats-plan.md) (数値側の A/B 仕様)、
[settings-inventory.md](settings-inventory.md) §4.1 (「数値の意味は、どこで・
どうやって見せたかに依存してはならない」)。

---

## 0. 結論

N比較は、**順序を持つ1本のスロット列**として扱う。

- **A**は、現在表示している対象を示すカーソルであり、席ではない。
- **B以降**は**席**（stackへの参照とフレーム追従規則を持つスロット。
  コード注釈の seat）である。
- 各スロットはAのフレーム番号に追従する。対応するフレームがなければ
  `[stale]` と表示する。
- 画像の表示方法は、**laid out**（row / gridのタイル表示）と、
  **collapsed**（diff / N-blinkによる単一表示）の2系統**のみ**とする。
- wipeとdiffは**厳密に**A対Bの2対象比較を維持する。別のスロットと
  比較するには、そのスロットをBへ昇格させることが**唯一の**操作である。
- 全数値パネルは、スロット番号をキーとするキャッシュを使い、N対象へ対応する。
- 比較対象の上限は、Aを含めて7面（A+6）とする。制約の根拠は、
  `slotInk`で区別できる色が7本であること。画面幅、表の列数、RAM容量ではない。

## 1. 問い1: 今どこまで N になっているか — 台帳

現状を確認するため、次の3点を調査した。

1. 各パネルとキャンバスの描画経路を確認した。
2. スロット機構の利用箇所を検索した。`cmpExtra`、`resolveSlots()`、
   `compareExtras()`は、18ファイルの104箇所にある
   （製品コード55箇所、selftest 49箇所）。
3. Bスロットを直接読む箇所を検索した。`cmpB()`、`resolveB()`、
   `compareBUid`は、20ファイルの101箇所にある
   （製品コード44箇所、selftest 57箇所）。

3の製品コード44箇所が、§6 D3「B を slots[0] に畳む」で移行対象となる。

### もう N の場所

| 面 | 実体 | 証拠 |
|---|---|---|
| 解決規則 (follow + diverged) | 全席共通の1規則 `followFrame` / `resolveSlots` | `core/app/compare.inc:22-36, 288-306` |
| Canvas Split = タイル | A + B + 全席、1 zoom / 1 center、狭ければ言って退く | `core/ui/canvas.inc:350-371, 445-534` |
| 表示レンジ (mode 1 / mode 2) | union / A's range は armed な**全席**に効く | `core/app/export.inc:201-229` (`parts[2+14]`) |
| Histogram 表・フッタ | 全席1行ずつ | `histExtra` (`state.h:860`) + compare-n.md §11 |
| Histogram プロット | 1面に絞れば全席重ね、上限 `slotInkCount()` + 打ち切り文字の名指し | `core/ui/panel_histogram.inc:164-174, 289-310` |
| Projection **表** | 全席1行ずつ (`projExtra`) | `state.h:2102` |
| Temporal **チャート** | `TSlot` 列で全席、凡例 `slotLegendRow` | `core/ui/panel_temporal.inc:1485-1511` |
| ROIs 表 | 二形態 (#76 裁定): side-by-side は席ごとの列グループ、collapsed は blink 追従 | `core/ui/panel_rois.inc:181-188` |
| Files 文字バッジ・arming | 右クリック `slotRowItem` 経由 (selftest N5 が固定) | `core/app/compare.inc:356-370` |
| セッション | `cmpslot` (frame + path) + `refmember`、非同期復元 | `core/app/session.inc:86-91, 2157` |
| キャッシュと後始末 | `histExtra`/`projExtra`/`temporalExtra`、`forgetImage` が全席を潰す | `core/app/export.inc:273-275` |
| ステータスチップの share 文 | 画素を共有する文字を全部名指す | `abStatusChipText` (`compare.inc:549-587`) |
| 色 | `slotInk` 1箇所、7本 + 剰余 | `core/ui/inspector.inc:85-102` |

### まだ 2 (または 1) の場所

| 面 | 今 | 行き先 |
|---|---|---|
| wipe / diff 描画 | A 対 B | **仕様として2のまま** (§5)。脱出は昇格 |
| blink の状態 | `bool flipShowB` — bool は C を名乗れない | N-blink (§6 D2) |
| Temporal 表 | A\|B\|Δ\|Δ% | **仕様として2のまま** — Δ は2項演算 (compare-n.md §4) |
| Projection **曲線** | A/B のみ。取らなかった文字は画面が名指す (`g_projSideProbe`, N6) | I7 (§6 D5) |
| Analysis | A + `Run on B` | `Run on slot …` (§6 D6) |
| 既定 B の補充・Shift+B・B 押下フラッシュ・Shift+\ swap | B 席の特権 | **仕様として残る** — リスト先頭という位置の性質 (compare-n.md §1) |
| Inspector の画素読み出し | A/B の2列 | v1 は触らない (§7 裁定7) |
| テクスチャ evict の明示保護 | `cur()` + `cmpB()` のみ (`compare.inc:649`)。席のタイルは LRU (12枠) が実質守るだけ | D3 で全席に一般化 |
| チップの `A/B split 50%` 等レイアウト語彙 | 仕切りの無い画面に仕切り位置を言う | I6 (§6 D4) |

**数値処理とセッション保存は、ほぼN対象へ対応済みである。残る主な作業は、
次の4点である。**

1. B専用状態とboolean状態を、スロット列へ一本化する。
2. 画像のlaid out表示を、明示的な表示モードにする。
3. Projection曲線をN対象へ対応させる。
4. 比較対象数の上限を、合計8面から7面へ変更する。

## 2. 問い2: A = カーソルとは何を意味するか

**A はスロットではない。カーソルである** (compare-n.md §1 を継承)。比較の
持ち運べる部分は**文字 (席)** で、A は「いま見ているもの」。したがって
**カーソルを動かすと比較の意味が変わるのは仕様である**。それがこの機能の
主動線の形そのもの — stack を歩きながら、留めた参照 (dark / golden / 別撮り)
に照らす。follow-frame は「A のフレーム番号」を全席に配る規則であり
(`compare.inc:22`)、A が動くから全席が意味を持つ。

**「A も固定できるべきか」— v1 では断る。** 理由は3つ:

1. **`cur()` の単一性** (todo 項目28 の論点)。Inspector・Histogram・レンジ
   スコープ・Files の選択、アプリ全体が `cur()` 1つに紐づく。固定 A は
   第2のカーソルで、「いまどれを見ているのか」に2つの答えができる。
2. **固定したいものには既に道具がある。** いまの frame に文字を与えてから
   歩けばよい — `addCompareSlot` は A の下の画像を拒否**しない**
   (`compare.inc:326-329` はまさにこの動線のために書かれている:
   「park the frame you are on as C and go looking」)。Shift+B も同じ。
3. **§3 の禁** (compare-n.md): 第4の概念 (隠れた選択状態) は作らない。

**再訪条件**: 項目28-4 / I5 (パネルのピン留めがスロットを対象に取る) が
入ったとき。「A を固定したい」の実需は「この Histogram を D に固定したい」で
あることが多く、それはカーソルを増やさずに満たせる。

**帰結を1つ明文化する**: すべての面は「A + 席」で描く。**A を含まない比較
(B 対 C だけ) は存在しない。** B と C を比べたければ、どちらかを選択して
A にする — カーソルはそのためにある。

## 3. 問い3: 上限 A+6 の根拠 — 数える

候補を全部数えた。

| 候補 | 数 | 数え方 | 縛るか |
|---|---|---|---|
| **色 (`slotInk`)** | **7** (A..G) | `inspector.inc:86-94` の `INK[]` エントリを数えた。`slotInkCount()` = 7 | **縛る** (下記) |
| 画面幅 | 1600 px 缶バスで **13** パネ | 切点は `n·minW + (n−1)·2·gutter ≤ width` (selftest T6 が式ごと固定)。minW=110, gutter帯=6: n=13 → 1502 ≤ 1600、n=14 → 1618 > 1600 | 縛らない |
| 統計表の桁 | ROI side-by-side は 1席 5列 (mean/std/std÷mean%/min/max)。7席=37列、8席=42列 | #76 の裁定文 + `panel_rois.inc` | 縛らない (ScrollX で逃げられる) |
| `SLOT_LETTERS` | 14 (= 総面 16) | `"CDEFGHIJKLMNOP"` (`compare.inc:312`) | 根拠を持たない現行値 |
| VRAM | 46 MB/枚 @12Mpx → 7枚 322 MB | compare-n.md §7 の算出 | 縛らない |

**訂正2件** (どちらも数え直しで出た):

- compare-n.md §7 の「8 パネは 1600px にちょうど収まる (T6 の実測)」は
  **数え違い**。T6 が言っているのは「8 は収まる・16 は収まらない」という
  点検2つで、切点ではない。切点は上の式で 13。幅は 8 を選ぶ根拠にならない。
- **Temporal チャートは今日でも8面目で色が嘘をつく。** `addSlot` は
  `slotInk(2+i)` を無検査で回し (`panel_temporal.inc:1498`)、剰余で8本目は
  A の緑に戻る。ヒストグラムだけが「上限 + 打ち切り文字の名指し」を持つ
  (`panel_histogram.inc:302-310`)。上限 ≤ 7 はこの欠陥を修理ではなく
  **到達不能**にする。

**決定: 上限 = `slotInkCount()` = 総面 7 = A + 6 席 (B + C..G)。
`SLOT_LETTERS` は `"CDEFG"` (5文字) になる。**

理由: 候補のうち**色だけが「言って退く」ことのできない資源**である。幅は
tileNarrow の文で退けるし、表は横スクロールできるが、色は剰余で回った瞬間に
配列長の代わりに嘘をつく (#60 と同型の欠陥 — compare-n.md §10 の文言のまま)。
そして compare-n.md 自身の規則「画面ごとに違う上限は説明コストの方が高い」を
色に適用すると、全画面の上限は 7 以下でなければならない。8 面目には 8 色目が
要り、8 色目は用意されていない。板の要約「上限A+6」とも一致する。

付随して決める:

- 等式 `2 + strlen(SLOT_LETTERS) == slotInkCount()` を selftest で**等式のまま**
  固定する (≤ ではなく)。席より多い色は誰も armed にできない色 = 死んだ仕様で、
  2つの数は一緒に動くべきだから。палитра を8本に増やす日が来たら、上限も
  同じコミットで 7 席になる — 縛りが色であることが式に書いてある。
- 上限に当たったときの文は cap を名指す:
  `no free compare slot (A+6 is the cap - release a letter first)`。
- 旧セッション (旧ビルドは 14 席まで書けた) は先頭 5 行が席を取り、あふれた
  行は**数とパスを `logMsg` で名指し**する。黙って捨てない。

## 4. 問い4: 画面の形 — 3案を検討する

**案1: タイル (等格の並び)。採用** — 現行 + I6 (row / grid、列数明示、
ユーザー選択、枚数からの自動切替禁止)。仕様は compare-n.md §12 I6 のとおりで
本書は変えない。安定性の不変条件 (k+1 枚目を armed にして既存ペインの行・順序が
変わらない) が要求の核心。

**案2: スロット列 + 1つを大きく (フィルムストリップ + 主面)。不採用。** 理由:

- 「どれを大きくするか」という**第4の選択状態**が要る — §3 の禁そのもの。
  昇格 (文字が動く) と違い、この選択は画面のどこにも文字として残らない。
- 縮小ストリップは共有 zoom を破る。`tileMap` の「1 zoom / 1 center」は
  倍率違いの並置から誤った結論を読むことを防ぐ規則 (`canvas.inc:329-333`)
  で、サムネイル列はその例外を常設することになる。
- 「小さい一覧 + どれかを注視」の欲求は **collapsed 系が既に満たす**:
  N-blink は「1つを大きく」を時分割でやる形であり、選択状態は巡回位置として
  画面に見えている (大きく重畳される文字がそれ)。
- **再訪条件**: タイルを N≥5 で使う実運用から「並びも保ちたい + 1枚は
  大きく見たい」が具体的な作業とともに報告されたら。そのときも第4の選択を
  作らず「grid の1セルを2×2に割り当てる」型で検討する。

**案3: 画面は2つのまま (N は解析パネルの話)。真面目に検討した上で不採用。**
利点は本物である — #68 の教訓どおり、見せ方を増やすと読み手が覚える振る舞いが
増える。画像は wipe/diff/flip/split の4つのままで、N は表と曲線だけ、が
一番覚えることが少ない。しかし:

- **ユーザー裁定に反する。** 2026-08-04 の原文 (compare-n.md §10):
  「並べて表示 (side by side, もしくは今後はグリッドレイアウト) か，
  Difference/Blink の1表示に集約の二通りになると思いますので，これを
  サポートしてください」。画像側の N は指示である。
- タイルは既に着地し、selftest T 群が矩形まで固定している。戻すのは
  単純化ではなく**削除**で、「同一シーン N 撮りの一覧比較」という主要動線を
  失う。
- ただし**案3の規律は全部採る**。これが #68 の教訓の正しい適用である:
  N のために増える画像側の顔は **laid out (row/grid) と collapsed (N-blink)
  の2系統だけ**。wipe / diff は2枚の意味しか持たないから2枚のまま。
  「A と D の差分」の答えは昇格 (D→B) の1操作。モードはスロットの増減で
  勝手に変わらない。ジェスチャは1つも増えない (B/Space は blink の既存キー、
  タイルは split の既存モード)。

**モード × N の挙動表** (正典として固定):

| モード | 席が C 以降も armed のとき |
|---|---|
| Off | **常に A 単独**。文字は Files と数値側に残る (席は生きている) |
| wipe | A 対 B のまま。数値側は N |
| split | **タイル** (laid out)。row か grid はユーザーの選択、列数は明示値 |
| diff | A−B のまま。数値側は N |
| flip | **N-blink**: A → B → C → … を巡回、現在の文字を大きく重畳 |

## 5. 問い5: 範囲の共有

**前提の訂正**: `compareRangeMode` は2値ではなく**3値**である
(`state.h:715-732`): 0 = each own / 1 = every side uses A's / 2 = union auto
(既定)。そして**既に N 化済み** — `abRange` は armed な全席を union に入れ、
mode 1 は A の black/white を全文字に配る (`export.inc:201-229`)。文言も
「every side」に一掃済み (compare-n.md §11)。N で新しく増える値は無い。

正典 (settings-inventory.md §4.1) に照らして、**不変条件を4つ**仕様にする:

- **R1 — presentation 級である。** mean / sd / σ_t / projection の値は
  range mode で1桁も動かない。動いてよいのは「レンジをパラメタに取る量」
  (ヒストグラムのビン格子と clip%) だけで、それは自分のレンジを軸・フッタで
  **申告**する (M1 の適用)。range mode を変えて統計が動いたらバグである。
- **R2 — union は「A + armed な全席、compare ON の間だけ」。** 席は
  compare off では参加しない (`export.inc` の「a seat is not a comparison」を
  正典化)。frame step ごとに再フィットする — 動くのは見え方であって数値では
  ない (R1 が守る)。
- **R3 — mode はグローバル1つ。席ごとの range mode は作らない。**
  作った瞬間「この画面のレンジは何か」が1文で言えなくなる。`abStatsLayout` を
  パネル毎にしなかったのと同じ理由 (ab-stats-plan §3)。
- **R4 — mode 1 は A の stretch (black/white)、mode 2 は内容 (vmin/vmax) の
  union。** 既存実装の区別をそのまま仕様として固定する。mode 1 は「A の
  基準に照らす」、mode 2 は「誰も欠けない同じ auto」で、別の問いに答えている。

## 6. 段階分け — D1..D6

各段は単独で main に入って CI 緑、単独で意味を持つ。試験はすべて NOGL、
既存の selftest 群 (abstats N / tile T / roistats R) の拡張。**試験規律**:
selftest home は全テスト共有なので、`compareRangeMode` 等の設定を触る試験は
必ず save/restore する (既存 `saveShare` の型)。期待値はすべて手で導出し、
導出を assert の隣にコメントで書く。

### D1 — 上限 A+6 (最小・最初)

変更: `SLOT_LETTERS` → `"CDEFG"`。`abRange` の `parts[2+14]` → `[2+5]`。
上限メッセージが cap を名指す。`slotRowItem` に **`SlotRowFull`** を足す —
席が満杯の行は「何も出さない」ではなく `compare slots full (A+6)` を
disabled で**言う** (沈黙は N5 の教訓に反する)。セッションあふれは §3 の
規則。selftest N4 (палитра 超過の打ち切り検査) は cap 後は**発火不能**に
なる — else 枝で黙って読み飛ばされる (「空集合でも成り立つ等式」の再来) ので、
**N4 を構造の等式に置き換える**。

試験 (fail-first):
- **N4'**: `check(2 + strlen(SLOT_LETTERS) == slotInkCount())`。
  導出: `INK[]` は 7 エントリ (A..G を数えた)、2 = A と B、よって extras =
  7 − 2 = 5。今日の値は 2 + 14 = 16 ≠ 7 → **変更前に赤**。
- **N7a**: C..G の5席を `slotRowItem` の門を通して armed → 全部座る。
  6枚目: `slotRowItem == SlotRowFull` かつ `addCompareSlot` 後も
  `cmpExtra.size() == 5`、toast が cap を名指す。今日は6枚目が H に座る →
  **変更前に赤**。
- **N7b**: `cmpslot` 6行の fixture セッション → 復元後 5 席 + logMsg が
  「1 dropped」とパスを言う。今日は6行とも座る → **変更前に赤**。

### D2 — N-blink (flip の一般化、collapsed 系の N の顔)

変更: `bool flipShowB` → `int flipSide` (0 = A)。`blinkSideIndex()` が
それを返す (この関数は既にこの日のために置いてある — `compare.inc:142-149`)。
`B` / `Space` は +1 mod 面数、auto タイマーも同じ。現在の文字を現行 A/B と
同じ位置・同じ描き方で大きく重畳。diverged な席は [stale] バッジ付きで
巡回に残る (消すと壊れたに読める — compare-n.md §2)。2枚のときの見た目は
今と**同一** (置き換えであって追加モードではない)。巡回位置は view state:
セッションに書かない (今も `flipShowB` は書いていない)。席を外して
`flipSide` が範囲外になったら 0 (A) に戻す。

試験 (fail-first):
- C, D を armed + CmpFlip。面数の導出: A, B + |cmpExtra| = 2 + 2 = 4。
  step ×4 で `blinkSideIndex` が 0, 1, 2, 3, 0。今日の bool は 0, 1, 0, 1 —
  **step 2 回後の `== 2` が変更前に赤**。
- R 群に1本: blink 中 `flipSide = 2` で ROI 表が C を名乗る
  (`blinkSideIndex` 経由なので D2 だけで通るはず — 通らなければ追従が
  複製されている証拠)。
- clamp: 3面で `flipSide = 3` → 0。導出: 3 ≥ 面数 3 → 巻き戻し。

### D3 — B を slots[0] に畳む (挙動不変・機械的)

変更: `compareBUid`/`compareB`/`compareBSeq`/`lastCompareBUid` →
`slots[0]` + アダプタ (`resolveB` / `cmpB` / `setCompareB` / `bSeatUid` は
存置)。製品コードの読み手 **44 箇所** (§1 の grep)。セッションは `cmpslot` の
1行目が B になる。**旧 `cmpb` 系キーは読み続け、当面は書き続ける** —
downgrade したビルドが対を黙って失わないため。撤去は別コミット (退役キーの
前例 `rbadv` の型)。`abStatsFrame` の「slot 1 だけ潰す」を全席版に (compare
off の1フレーム目で `histExtra`/`projExtra`/`temporalExtra` も潰す)。
`touchTex` の evict 明示保護を `cur()` + 全席に一般化 (今は B のみ —
`compare.inc:649`。上限7 < LRU 12 なので実害は薄いが、非対称は読み手を騙す)。

試験: 挙動凍結の網は既存 A/S/T/N/R 群がそのまま。追加 (fail-first):
- 旧 `cmpb` 形式だけの fixture セッション → `slots[0]` に B が座り、
  `bSeatUid()` が一致。**この check は新形式に対して書く**ので、変更前の
  コードに掛けると cmpslot 1行目が C に化けて赤 — 移行の向きが検査で言える。
- compare off 1フレーム目で全席キャッシュの `img/uid/seqId` が無効値。
  導出: `abStatsFrame` の既存規約 (off の2フレーム目以降はコスト0) を
  席にも張っただけ。

### D4 — laid out のモード化 (= compare-n.md §12 I6、仕様は変えない)

`cmpLayout` (Row/Grid) + `cmpGridCols` (明示値、導出禁止)、セッション
`compare` 行の末尾 2 フィールド、エンジンは `tileLayout` に cols を足す1本、
チップ文の置き換え、View メニュー。**安定性の不変条件**が本体: k 枚が画面に
あるとき k+1 枚目を armed にして許される変化は、Row では等幅のまま狭くなる
こと、Grid (列数固定) では末尾にセルが増えることだけ。

試験 (fail-first): T 群に §12 の文そのもの — 「4枚のペイン矩形を記録 →
5枚目を armed → 先の4枚の (行, 順序) が不変、Grid なら矩形そのものが不変」。
Grid は実装前に存在しないので **Grid 側の assert が変更前に赤**。チップ:
タイル中に `split 50%` を含まない (今日は含む → 赤)。

### D5 — Projection の N 重ね (= compare-n.md §12 I7) 【裁定2 待ち】

plane セレクタの共有裁定 (§7-2) が出てから。中身は I7 のとおり: `slotInk`
重ね、`slotLegendRow`、軸一致は A 対各席 (`abProjOverlayable`)、不一致席は
描かず・伸ばさず・名指し。

試験: selftest N6 の固定 `curves == "AB"` が `"ABCDE"` に**動く** (N6 の
コメントに既記)。導出: C, D, E を armed、全側同一 ROI・同一軸 → 曲線は
A, B, C, D, E の5本。今日は "AB" → **N6 の書き換え自体が赤→緑を踏む**。

### D6 — 掃き出し (いつでも入る小物)

- **Release all slots** (View > Compare A/B に1項)。全席解放 = extras も
  B も外し、compare off に落とす。`lastCompareBUid` は残す (A9: 戻り道)。
  試験: 5席 armed → 1操作で `cmpExtra.empty()` かつ `compareBUid == 0` かつ
  `lastCompareBUid == 旧B` (導出: 「全部やめて A 単独から」+「C で再開したら
  同じ対に戻る」の両立がこの3値)。
- **`Run on slot …`** (Analysis): `Run on B` の一般化。自動実行しないのは
  そのまま。provenance 行が letter を名乗る。
- レイアウト非依存の2前提語彙の残りを一掃。

**依存**: D1 が最初 (上限が他の全段の枠を決める)。D2 / D3 は独立、D4 も独立、
D5 は裁定2待ち、D6 はいつでも。どの順でも各段単独で緑。

## 7. 決めたこと / 決めなかったこと

**決めた (理由は本文)**:

1. 上限 = A+6 (総面7)。縛りは色。compare-n.md §7/§8-3 の 8 を改定 (§3)。
2. A の固定は v1 で断る。再訪 = I5 / 項目28-4 (§2)。
3. 「スロット列 + 1つを大きく」は断る。再訪条件つき (§4)。
4. 画像側の顔は laid out / collapsed の2系統のみ。wipe/diff は2枚、脱出は昇格 (§4)。
5. `compareRangeMode` は3値のまま N 完了。R1..R4 を不変条件に (§5)。
6. Release all は B も含む。`lastCompareBUid` は残す (§6 D6)。
7. `cmpb` 系セッションキーは当面書き続ける (§6 D3)。
8. N-blink の巡回位置は view state — セッションに書かない (§6 D2)。

**決めなかった — ユーザーの裁定待ち** (1〜6 は compare-n.md §12 から引き継ぎ、
7 は本書が足した):

1. **統計パネルの Auto は N で何に解決するか。** 「armed なら overlay」提案は
   チャートには情報増・ROI 表には情報減 (#76 の帰結)。チャートと表で解決先を
   分けるかどうかまで含めて裁定が要る。
2. **Projection の plane セレクタを `histPlane` と共有するか。** 提案は共有。
   D5 がこれを待つ。
3. **side-by-side の大 N: 縮む (提案) か横スクロールか。** D4 は「縮む +
   言って退く」で実装し、裁定で覆れば差し替え。
4. **機能名「A/B」の改名。** terminology.md の管轄。本書は固有名として存置。
5. **フレームに収まらない ROI: 空欄 (現状) か交差部か。** 採るなら母集団の
   変化をセルに書く必要がある。
6. **blink でも side-by-side でもない collapsed の表は A を出す (現状追認)。**
7. **Inspector の画素読み出しの N 化。** v1 は A/B のまま (席の値は表の仕事)。
   タイルの上で C の画素値を読みたい、という要望が出たら再訪。
