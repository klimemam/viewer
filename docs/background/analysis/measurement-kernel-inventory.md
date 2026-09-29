現行ドキュメント: 統計の分類規則は [stats-taxonomy.md](../../features/analysis/stats-taxonomy.md)、推定量の正典は [flat-field-stats.md](../../features/analysis/flat-field-stats.md)。本書はその下にある**実装側の棚卸し**です。

# 測定カーネルの重複棚卸し — 同じ量が何箇所で計算され、どの対が試験で結ばれているか

板 99 行「共有測定カーネル+parity」の**洗い出し**である。板の指示
(2026-08-04) は「今重複している箇所をまず洗い出して、Fable に相談」であり、
本書はその一段目だけを行う。

**本書は事実の台帳であって提案ではない。** 何を共有カーネルに切り出すかは
Fable が決める。最後の §6 は候補を順位付けするが、それは「どれが一番安く、
どれが印字される数を動かすか」を並べたものであって、裁定ではない。

## 0. 読み方 — 3つの語

| 語 | 意味 |
|---|---|
| **カーネル** | 画素から数を作る算術そのもの。`mean` / `σ` / `percentile` / 行列分解 / 画素毎時間σ / detrend など |
| **実装site** | そのカーネルが書かれている場所。`file:関数` で示す |
| **対 (pair)** | 「同じ数を出すはずの2つの実装site」。対でなければ重複ではない —— 別の量を別の式で出しているだけの site は重複ではない |

**重複 = 同じ対が2つ以上の実装siteを持つこと。** 本書が数えるのはこれで、
「似た名前が2箇所にある」ことではない。この区別は
[stats-taxonomy.md §6.1](../../features/analysis/stats-taxonomy.md) が
一度払った授業料である — 板行が「パネル間の対立」と書いた件は、
全数調査すると**量の取り違え**で、対立ではなかった。

## 1. 先行調査との関係 — 本書が足すもの

| 文書 | 何を数えたか | 軸 |
|---|---|---|
| [stats-taxonomy.md §6.2](../../features/analysis/stats-taxonomy.md) (2026-08-10) | 出荷コードの分散/σ式 **全 25 箇所** + selftest の参照実装 5 箇所 | **ddof** (0 か 1 か) |
| 本書 (2026-09-27) | 同じ site 群を**カーネル単位**で束ね、対を作り、対ごとに parity 試験の有無を付けた | **重複と試験** |

§6.2 は「どの式がどの ddof か」を数えた。板 99 が要るのは
「**どの対が同じ数を出すはずで、そのうちどれが試験で結ばれているか**」であり、
ddof はその一属性にすぎない。σ 以外のカーネル (percentile / histogram bin /
detrend / 行列分解) は §6.2 の対象外だったので、本書が初めて数える。

**§6.2 の行番号は当日のうちに動いた**と同書 §8 が自ら記録している。本書も同じ
弱点を持つので、**site は式と関数名で引き、行番号は補助**として添える。

## 2. すでに解決済みの形 — 数える前に、手本を先に置く

重複の話をする前に、**このリポジトリが既に2度払って得た解**を書く。候補の
順位付け (§6) はこの形に寄せられるかどうかで決まるので、先に置く。

### 2.1 `core/setfold.h` — 1つの純関数を両端が呼ぶ

set 解析の畳み込みは header-only の純関数群
(`setfold::pixelMeanCorr` / `reduceOne` / `reducePair`) で、
**local (`core/app/setanalysis.inc`) と peer (`core/serve.cpp`) が同じ関数を呼ぶ。**
そのファイルの冒頭コメントが理由を名指ししている:

> PR #127 の rtemporal P1..P5 は、同じ推定量を2度書くと何が起きるかの標準教材である。
> peer は `sigma_tot` を「既に平方根を取った `sigma_t`」から `sqrt(st*st+fvar)` で
> 作っていて、それは viewer の `sqrt(tvar+fvar)` と binary64 では別物だった。
> そして**両者を並べて見たものが一度も無かった。**

得られた形は2点セットである:

1. 算術は**1つの関数**。呼ぶ側が2つ。
2. 等値は**構造的**に成立する。`--rset-selftest` が全欄 (値・補正量・
   クランプ・μ・標本数・遮蔽量・タグ) を**ビット単位**で突き合わせる。

同ファイルはさらに「代数的に等しい別経路」を**却下した記録**まで持つ:
per-pixel 減算を役割毎モーメントから組み直す道は代数的に等価だが
**浮動小数の順序が違うので同じ binary64 を返さない** (実測: 同じ fixture で
`3.1073286294809335 %` 対 `3.1073286294716875 %`、11 桁目で分岐)。
**「代数的に正しい」は要件ではなく、「local と最後のビットまで一致する」が要件。**

### 2.2 パネルとエクスポートは「再計算しない」で解いた

Temporal / ROIs のエクスポートは、パネルが表示している値を**再導出しない**。
`buildTemporalExport` / `buildRoiExport` が**パネルの state struct を読む**。
`--export-tsv-selftest` はそれを名指しで検査する
(「数値がパネルの state struct と同じ文字列に整形されること (再導出ではなく
同一 struct 読み)」—— [export-design.md §9](../../features/export/export-design.md))。

**したがって「パネル対エクスポート」は、この2面については重複ではない。**
本書が対として立てるのは、エクスポートが**自分で式を持っている**箇所だけである。

### 2.3 プラグイン対組み込みの parity は「宣言」で、数値照合ではない

`MOP_PLUGIN_ANALYZE` のパリティは **name + version の等値**である
([abi-v3.md §10](../../reference/abi-v3.md))。同書が明示するとおり
**「ULP 一致の実測ではなく宣言の照合」**。同じく `MOP_SET_FOLD` のパリティは
「畳み込みが宣言する form の等値」である。

**結果**: 同梱プラグインの `X.std` と組み込みの σ が同じ数を出すことを
**検査している試験は無い**。同じ量を別実装が出しているという事実だけがある。
これは §6 の候補順位を大きく動かす。

## 3. カーネル一覧 — 実装site と出る場所

行番号は HEAD `fdbf5297` 時点。**式と関数名が正で、行番号は補助**である。
「n」列は1回の計算が見る標本数の上限 (`full` = ROI 全画素)。

### K1. 領域モーメント (mean / σ / min / max / n) — 規約 ddof=0

| # | 実装site | n の上限 | plane 規則 | NaN | 出る場所 |
|---|---|---|---|---|---|
| 1 | `core/ui/panel_rois.inc:8` `roiBasicStatsUncached` | **200k** (モザイクセル単位ストライド) | CFA→4 / ch別 / pooled | 黙って skip (**数えない**) | ROIs パネル `mean/sd/std-mean[%]/min/max/n`、ROIs `Copy (TSV)` / `Save (CSV)` |
| 2 | `core/ui/canvas.inc:1596` `recomputeHistogramIfNeeded` | **1M** | CFA→4 / ch≤3 | skip (clip 分母からも除外) | Histogram パネル `mean/std/var` |
| 3 | `core/ui/panel_histogram.inc:773` `recomputeProjectionIfNeeded` の `fStat` | **2M** (H パスのみが供給) | CFA→4 + pooled slot | skip | Projection `mean` / `σ (frame)` / `pp` / `%`、Projection `Copy table (TSV)`、Temporal export §2 |
| 4 | `core/ui/panel_temporal.inc:454` `exportPerFrameBlock` | **full** | CFA→4 / **ch 別** | skip | Temporal `Copy (TSV)` / `Save (CSV)` §3、`--framestats-selftest` |
| 5 | `core/app/temporal_model.inc:102-115` (`T.frameStd`) | **40k 格子** | **pooled** | 数える (`T.nonFinite`、ただし 40k 分のみ) | Temporal パネルの frame std グラフ |
| 6 | `core/serve.cpp:2252-2264` (`frame std` 系列) | full | **pooled** | 数えて申告 | peer 応答 → Temporal パネル server 行 |
| 7 | `core/serve.cpp:2344` `runFrameRoiStats` (`roi var`) | full | `planeOf` (非 CFA は pooled) | 数えて申告 | peer `MOP_FRAME_ROI_STATS` |
| 8 | `plugins/analyzer_stats.c:63` | full | CFA→4 / ch≤4 | skip、`finite ratio` で申告 | Analysis パネル、`Copy table (TSV)`、peer 経由の plugin 応答 |
| 9 | `plugins/analyzer_noise.c:116` | full | 同上 | skip | 同上 (`X.mean` / `X.std`) |
| 10 | `plugins/analyzer_sharpness.c:74` (`varlap`) | full | luma 1面のみ | **0 で置換** | 同上 |

**ddof は §8 裁定で 0 に統一済み** (stats-taxonomy.md §9)。**残る食い違いは ddof ではない** ——
標本数 (200k / 1M / 2M / 40k / full の5種) と、NaN を数えるか黙るか、と
非 CFA 多チャンネルの畳み方 (ch 別 / pooled / ch0 のみ) である。

### K2. 行平均・列平均の σ (正典の A と B) — 規約 ddof=1

| # | 実装site | 分散の形 | 正規化 | n と精度 | NaN | 出る場所 |
|---|---|---|---|---|---|---|
| 1 | `core/ui/panel_histogram.inc:954-991` (`hStat` / `vStat`) | `(sum2/n − m²)·n/(n−1)` | **profile 自身の平均**で `%` | **直交方向 2M 間引き**、profile は `float` に一度丸める | profile の NaN 要素を skip | Projection `σ (axis)`、`Copy table (TSV)` の `sigma_row(ddof=1)` / `sigma_col(ddof=1)`、Temporal export §2 |
| 2 | `core/ui/panel_temporal.inc:546-558` (`profCv`) | 同じ形 | **面の平均**で `%` | full、`double` | 画素 skip、`pc<2` で 0 | Temporal export §3 の `sigma_col[_pl] [%](ddof=1)` |
| 3 | `core/app/profile_noise.inc:53-65` (`varianceOfMeans`) | **原点シフト形** `(sum2 − sum²/n)/(n−1)` | **無次元** (σ_p との比) | full、`double` | **1画素でも非有限なら面ごと拒否** | Projection `HFPN/RN` / `VFPN/RN`、同 TSV、Temporal export §2 |
| 4 | `plugins/analyzer_prnu.c:201-216` | `Σd²/(n−1)`、`d` は **boxblur 除去後の残差** | 面の平均で `%` | 面の**間引きグリッド** | **0 で置換** | Analysis `row_fpn_pct` / `col_fpn_pct`、同 TSV、peer plugin 応答 |

**「行平均のσ」が4実装ある。** ddof は揃ったが、**分子 (detrend の有無)・
分母 (無次元 / 面平均 / profile 平均)・標本 (間引き / full / 間引きグリッド)・
NaN (skip / 面拒否 / 0 置換)・精度 (`float` profile / `double`) が全部違う。**

- #1 と #3 は**同じ Projection パネルの同じ表に並んで出る**。
- #1 と #2 は**同じエクスポートファイルの §2 と §3 に並んで出る**。1ch 非 CFA では
  `sigma_col [%]` という列名が1ファイルに2回出る (stats-taxonomy.md §6.5)。
- **peer はこの量を一切計算しない** (`MOP_*` 全 op 確認済み、stats-taxonomy.md §6.2)。

### K3. T − A − B 分解と HFPN/RN・VFPN/RN — 1実装

`core/app/profile_noise.inc:4` `computeProfileNoise` (PR #261、`f568ed5b`、2026-09-16)。
`T` (領域分散)・`A` (列平均の分散)・`B` (行平均の分散) を**全画素・原点シフト・
ddof=1** で取り、`p=(T−A−B)/(1−1/H−1/W)`、`rowVariance=B−p/W`、`colVariance=A−p/H`。
比は `sqrt(rowVariance/pVariance)` = **HFPN/RN**、`sqrt(colVariance/pVariance)` = **VFPN/RN**。
クランプは成分別に申告する (`pClamped` / `rowClamped` / `colClamped`)。
呼び出しは `core/ui/panel_histogram.inc:793` の1箇所のみ。

**重複ではない。意図的な二本立てである。**
`core/app/profile_noise.inc:1-3` と `core/app/state.h:2511-2513` が理由を書いている:
表示 profile は reduce mode (mean/max/min) と直交方向の間引きを持つので
**その統計は分解の成分になれない**。そして正典
[flat-field-stats.md](../../features/analysis/flat-field-stats.md) が
「従来の `sigma_frame` / `sigma_row` / `sigma_col` 列は改名も再定義もしない」と
明示している。**したがって K2#1 と K3 を統合してはならない。**

### K4. 画素毎の時間分散 σ_t — 規約 ddof = n_i − 1

| # | 実装site | n | CFA 規則 | NaN の報告 | 出る場所 |
|---|---|---|---|---|---|
| 1 | `core/app/temporal_model.inc:131-155` `recomputeTemporalIfNeeded` | **40k 格子** | `cfa!=0 && ch==1` **かつ `rw>=cell && rh>=cell`**。非 CFA 多ch は **ch0 のみ** | `nonFinite` (40k 分のみ) / `dropped` | Temporal パネル、Temporal export §1 |
| 2 | `core/ui/panel_projection.inc:1123-1141` `computeStackStats` | full (上限 32M) | **`first->cfa ? 4 : 1`、ch 条件なし**。全 ch を位置由来の面に畳む | `nonFinite` / `dropped` / `unknown` / `cntMin`-`cntMax` | Series Analysis パネル `sigma_t`、SNR TSV、PTC/K fit、frame average/sum、detrend 製品 |
| 3 | `core/serve.cpp:2269-2282` `runTemporalStats` | full (32M) | `cfaType?4:1`。`ch!=1` は**事前に拒否** (`CFA planes need a 1-channel frame`) | `non-finite samples (excluded)` のみ。**`dropped` を数えない** | peer 応答 → Temporal パネル server 行、Browse 群行 |
| 4 | `core/app/setanalysis.inc:222-238` `setFoldStack` | full (32M) | `cfa!=0 && C==1` | `F.dropped` | Set Analysis パネル (DSNU / PRNU / 分離フィット) |
| 5 | **`core/setfold.h:95` `setfold::pixelMeanCorr`** | 純関数 | 呼び側が `plane[]` を渡す | 呼び側 | **#4 と `core/serve.cpp:2452` `foldOneRole` の両方が呼ぶ** = 唯一の共有カーネル |
| R | `core/app/cli.inc:1564-1596` (出荷参照実装) | full | **なし** (pooled) | **なし** (fixture に NaN が無いため) | `--rmeasure-selftest` の stderr |

clamp 位置 (`max(0, …)` を ddof スケールの**前**) は全 site で一致している。
4箇所のコメントが互いを名指ししており (`temporal_model.inc:136-150`、
`panel_projection.inc:1047-1061`、`serve.cpp:2199-2211`、`setanalysis.inc:144-156`)、
**このカーネルの重複はコードが自分で申告している。**

### K5. σ_fpn (時間平均画像の空間分散 − C) — ddof=1

`core/app/temporal_model.inc:174-213` (local) / `core/serve.cpp:2287-2323` (peer) /
`core/app/setanalysis.inc:505` `setPlaneFpn` (合成器、local と peer の両方が使う)。
前2つが local/peer の鏡写し対で、`sigma_tot` の形まで一致させてある
(`serve.cpp:2308-2312` が「旧形 `sqrt(st*st+fvar)` は viewer の `sqrt(tvar+fvar)` と
最終 ulp で違う。比べたものが一度も無かったから生き残った」と記録)。

### K6. ヒストグラム binning — 3つの異なる規約

| # | 実装site | bin 数 | どのレンジ | 端の扱い | n |
|---|---|---|---|---|---|
| 1 | `core/ui/canvas.inc:1560` `histBinGrid` + `:1596` | 256 | **表示レンジ** (black/white) | **外向きスナップ** (整数は `ceil`、float は 2 の冪)、bin 0 と 255 は catch-all | 1M 間引き |
| 2 | `core/app/compare.inc:706` `accumulatePercentileBins` | **65536** | **データレンジ** (`vmin..vmax`) | bin 0 が rlo 以下を吸収、最終 bin は閉区間 | **full** |
| 3 | `plugins/analyzer_stats.c:141-158` (entropy 用) | 256 | host が渡す black/white | スナップ無し・丸い bin 幅無し | full |

**3つとも別の規則で、どれも互いを参照していない。**
`core/selftest/abstats.inc:49-122` の `RefHist` は #1 の**意図的な独立再実装**で、
「`histBinGrid` を共有したらこの参照は定義上一致してしまう —— それだけはさせない」と
書いてある (`abstats.inc:44-48`)。**これは重複ではなく試験装置である。**

### K7. percentile / median — 6 site、3つの median 規約

| # | 実装site | 厳密性 | 方式 | 分位 | 出る場所 |
|---|---|---|---|---|---|
| 1 | `core/app/compare.inc:737` `percentileFromBins` / `:835` `medianFromBins` | **近似** (分解能 = `(vmax−vmin)/65536`) | 65536 bin の累積、値は **bin の端** | 0.001 / 0.5 / 0.999 | Auto % / Med ボタンの表示レンジ、Inspector、動画書き出し、sequence の毎フレーム refit |
| 2 | `plugins/analyzer_stats.c:53` `selectNth` | **厳密** | quickselect (Lomuto、median-of-3)、3段連鎖 | 0.01 / 0.5 / 0.99 | Analysis パネル `p1/p50/p99`、TSV、peer plugin 応答 |
| 3 | `plugins/analyzer_noise.c:121` | 厳密 | `qsort` → `[n/2]` (偶数で上側、平均しない) | 0.5 | `X.noise` (16x16 タイル σ の中央値) = ノイズ床 |
| 4 | `core/app/detrend.inc:183` | 厳密 | `nth_element` → `[n/2]` (同規約) | 0.5 | block median detrend 面 |
| 5 | `core/app/compare.inc:997-1000` | 厳密 (**標本の**) | `nth_element`、200k 標本・**奇数ストライド** | 0.999 | A/B 差分ビューの auto gain |
| 6 | `core/main.cpp:3494-3502` | 厳密 | `sort` → `[n/2]` / `[n·0.95]` | 0.5 / 0.95 | `--bench` の stderr (画素ではなくフレーム時間) |

**#1 と #2 は同じ画素の同じ問いに別の方式で答える。** しかも分位そのものが
0.001/0.999 と 0.01/0.99 で違う。どちらも「1% / 99%」とは名乗っていないが、
`p50` と Med ボタンの中央値は**同じ名前の量**である。

### K8. detrend / shading — 共有済み (重複なし)

- **除去**: `core/app/detrend.inc:214` `detrendFitT` (`DTR_POLY` 次数1–4 / `DTR_BLOCKMED`)。
  `core/app/preprocess.inc` (製品) と `core/app/setanalysis.inc` (読み出し語) が
  **同じ関数と同じ文言**を呼ぶ (`detrend.inc:6-10`)。
- **測定 (シェーディング量)**: `core/shading_probe.h:170` `detrendProbeT`、次数固定 2。
  **local と peer が同じ関数を呼ぶ。** ヘッダ `:11-14` が
  「これはコピーではなく移動である。1つの推定量の2実装は、起こるのを待っている
  2つ目の答えである (ROIs の列とその tooltip は 11 週間ずれた、PR #130)」と宣言。
- **時間方向の detrend は存在しない。** detrend は fold の**後**で、σ_t の前ではない
  (時間平均に1枚だけ面を当て、全フレームから同じ面を引くので σ_t はビット単位で不変)。
- ただし `plugins/analyzer_prnu.c` は**自前の shading モデル**を持つ
  (9x9 boxblur の低域、分母は面の平均)。`shading_probe.h` は次数2多項式で
  分母は**フィットした視野中心**。→ K9。

### K9. 「PRNU」と名乗る量 — 3つ、命名で決着済み

| 実装site | 分子 | 状態 |
|---|---|---|
| `plugins/analyzer_prnu.c:167-175` `prnu_pct` | 9x9 高域残差の σ / 面平均 | plugin の proxy。単一フレームなので温度ノイズ混入を自己申告 |
| `core/ui/panel_rois.inc:71` `roiPrnuPct` | ROI の素の σ / 平均 | **`PRNU [σ %]` → `std / mean [%]` に改名済み** (`5d8cb72`) |
| `core/app/setanalysis.inc` `composeDirectPrnu` | 画素毎差分 → 空間σ (dark 役割が必須) | 名前を名乗る資格を持つ唯一の実装 |

**この対は「共有カーネル」ではなく「名前の分離」で閉じた。**
`--setanalysis-selftest` が出荷ソースを走査して famous name の漏れを落とす
(`core/app/setanalysis.inc:39-48`)。**手本の3つ目**である:
同じ数に統一できないときは、**違う名前を強制する**。

### K10. min / max のみ

| # | 実装site | 備考 |
|---|---|---|
| 0 | `core/app/compare.inc` `minMaxOf` | **カーネル本体**。ImageDoc を知らない純関数で、3つの規約 (非有限は値でない / 有限標本ゼロは 0..1 / 平坦は +1 で広げる) はここにしか無い |
| 1 | `core/app/compare.inc` `computeMinMax` | 正規の入口。#0 を呼び、`src->vmin/vmax` を書き、percentile / median キャッシュを落とす |
| 2 | `core/app/remote_client.inc` (`rfWorker` 内) | **#0 を呼ぶ**。UI スレッド外で測るため ImageDoc がまだ無い —— 写しが存在した理由がそれで、純関数化で消えた (板 299) |
| 3 | `core/app/plugin_glue.inc:351-356` | ROI montage のタイル毎正規化 (面別にしないことを明記) |
| 4 | `core/ui/panel_projection.inc:399-401` | プロット間引きの min-max バー (表示のみ) |
| 5 | `core/shading_probe.h:185-190` | フィット面の p-p (派生面のモーメント) |

**#1 と #2 が本書で見つかった唯一の「無言の重複」であった。**
**【解消 2026-09-28 板 299】** #2 は #1 のループと縮退ガードの写しで、どのコメントも
重複に触れていなかった。純粋な重複削除: カーネルを `minMaxOf` (#0) に出し、#1 と #2 の
両方がそれを呼ぶ。数は動かない (同一コードだった)。
他のすべての重複site は、コードかドキュメントのどこかで自分が重複であると申告している。

## 4. 対 (pair) と parity 試験の有無

「一致すべき」= 同じ名前・同じ単位の数として同じ画面や同じファイルに出るか、
正典が同一量と定めているもの。**一致しなくてよい対は表に入れない。**

| # | 対 | 一致すべきか | parity 試験 | 試験の強さ |
|---|---|---|---|---|
| P1 | σ_t: local (K4#1) ↔ peer (K4#3) | **はい** (正典: 「local と server は転送路であって量ではない」) | **あり** `--rtemporal-selftest` P1 (+ `-png` 版)、`--rmeasure-selftest` M1f/M2d/M2f/M6d/M6f/M6h/M7c | **ビット単位** (`==`、許容を明示的に拒否)。加えて独立 f64 参照と rel 1e-9 |
| P2 | σ_fpn / fpn_corr / clamp / σ_tot: local ↔ peer | **はい** | **あり** `--rtemporal-selftest` の P2/P3/P4/P5 | ビット単位。**補正量とクランプ旗まで**比べる (両側がクランプして 0 同士で一致するのを防ぐため) |
| P3 | set fold: local (K4#4) ↔ peer (`foldOneRole`) | **はい** | **あり** `--rset-selftest` RS3/RS5c/RS7/RS9/RS10/RS11/RS24 | ビット単位、全欄 (値・補正量・非補正上界・μ・σ_d・クランプ・標本数・遮蔽 p-p / % / 可否) + タイトル/form/cutoff/拒否文の文字列。fixture に**非2進小数の項を意図的に混ぜて**「厳密一致が入力のせいで成立する」のを防いでいる |
| P4 | plugin 結果: local 実行 ↔ peer 実行 | **はい** | **あり** `--rplugin-selftest` RP24 (出荷 `stats/moments`)、RP3/RP21、`--remote-selftest` MEASURE (pooled と Bayer 宣言の2回) | **`%.9g` の文字列一致**。同じ dll を両側で走らせるので算術は同一。**`mean/var/std/min/max/p1/p50/p99/entropy/finite ratio` がこの1本でだけ2回計算されて比べられている** |
| P5 | 領域 mean: `fStat.mean` (K1#3) ↔ 列平均の平均 `hStat.mean` (K2#1) | **はい** (同じ標本の同じ平均を2回計算している) | **あり** `--precision-selftest` P7c/P7d | **float 1 ulp** (`\|m\|·2⁻²³ + 1e-12`)。P7e は**V profile の平均は別の数**であることを逆に固定する (#145) |
| P6 | Temporal export §3 の per-frame σ ↔ ddof=0 の閉形式 | はい | **あり** `--export-tsv-selftest` E11 | `%.9g` で最終桁まで一致し、**ddof=1 とは一致しないこと**も検査 |
| P7 | パネル表示 ↔ エクスポート (ROIs / Projection / Analysis / Set / Series / Temporal §1-2) | **はい** | **あり (構造的)** `--export-tsv-selftest` E2/E9/E13、`--roi-export-selftest` X4 | **再計算しないので定義上一致**。E2 は σ_t / σ_fpn / `fStat.mean` / `fStat.sd` / `vStat.sd` / `hStat.sd` / HFPN-RN / VFPN-RN / status を `%.9g` で突き合わせる。E9 は「B 列は A を貼り替えたものではない」 |
| P8 | σ_fpn: Temporal パネル (K5 local) ↔ Set Analysis の DSNU (K4#4→`setPlaneFpn`) | **はい** (DSNU は同じ推定量を dark に当てたもの) | **あり** `--setanalysis-selftest` S2 (+ F2 / F10) | **ビット単位**。値・補正量・クランプ旗。許容を使わない理由をコメントが述べている |
| P9 | K3 の HFPN/RN・VFPN/RN: `ProjState` キャッシュ ↔ `computeProfileNoise` 直呼び | はい | **あり** `--profile-noise-selftest` P7 | **厳密 `==`**、プロット reduce mode 3種すべてで。値そのものは同 selftest の P1–P6 が**手で導いた有理数**に 1e-10 で当てる |
| P10 | σ_t: K4#1 ↔ K4#2 (`computeStackStats`) | **はい** (正典: 3実装すべて同じ式) | **間接のみ** | `--verify-selftest` V13 は同じ NaN fixture を**両実装で測るが、各々を解析真値に当てるだけで互いを比べない**。`--abstats-selftest` A2 は K4#1 を試験内参照 `refSigmaT` に面別 1e-6 で当てる (K4#2 には相方が無い)。**2つを並べる assert は無い** |
| P11 | σ_t: K4#2 ↔ K4#3 (peer) | はい | **なし** (P1 は #1↔#3 のみ) | — |
| P12 | K4 の **CFA 面規則** 3種 | **はい** | **なし** | P1 の fixture は `T.nPl == 1` を assert している = **非 CFA でしか local/peer parity を見ていない**。`--rmeasure-selftest` M7b/M7d はモザイクを peer 側で4面に割ることは見るが、**local の面規則と突き合わせない** |
| P13 | K4 の **NaN 報告** (dropped / unknown / cntMin-cntMax) | はい (正典: 「除外数は結果が運ぶ」) | **部分的** `--stackavg-selftest` A4/A8、`--verify-selftest` V13 (`nonFinite==2`, `dropped==0`) | peer は `dropped` を数えないので、**対として成立しない** |
| P14 | 行/列平均σ: K2#1 ↔ K2#2 (同じ export ファイル内) | **はい** (同じ列名で出る) | **なし** | E11 は per-frame **領域**σだけを守る。**§2 と §3 の `sigma_col [%]` を突き合わせる assert は無い** |
| P15 | 行/列平均σ: K2#1 ↔ K2#3 (同じ Projection 表) | 一致しなくてよい | — | 正典が「改名も再定義もしない」と明示 (§K3)。**ただしその旨を読者に言う責任は表側にある** |
| P16 | 行/列 FPN: K2#4 (plugin) ↔ K2#1/#2 (組み込み) | 一致しなくてよい (分子が違う) | — | 名前が `row_fpn_pct` と `sigma_row [%]` で分かれている |
| P17 | 領域 σ: K1#1 (200k) ↔ K1#2 (1M) ↔ K1#3 (2M) ↔ K1#4 (full) | **はい** (全部 `sd` / `σ` / `sigma` と名乗る) | **なし** | ddof は揃ったが**標本数が違うので値が違う**。突き合わせる試験は1本も無い |
| P18 | 領域 σ / var: local (K1#1) ↔ peer (K1#7 `roi var`) | はい | **なし** (`roi mean` のみ `--rmeasure-selftest` M5c と `--remote-selftest` が rel 1e-6 で見る) | 両方 ddof=0 で規約は一致。peer は var、local は σ、面の畳み方も違う |
| P19 | plugin `X.std` (K1#8/#9) ↔ 組み込み `sd` (K1#1/#2) | **はい** (同じ ddof、同じ CFA 規則) | **なし** | plugin parity は **name+version の宣言照合**で数値照合ではない (abi-v3.md §10 が明言)。P4 は「同じ dll を2つの転送路で」比べるだけで、組み込みとは比べない |
| P20 | percentile: `p50` (K7#2 厳密) ↔ Med ボタン (K7#1 近似) | **はい** (同じ中央値) | **なし** | `--range-selftest` P4/P10/P12 は K7#1 を**自分自身の別の駆動**と突き合わせるだけ。分位そのものも 0.01/0.99 対 0.001/0.999 で違う |
| P21 | histogram bin: K6#1 ↔ K6#3 (plugin entropy) | 一致しなくてよい | — | 用途が違う (表示 / エントロピー)。ただし規則が3つあることは記録に値する |
| P22 | min/max: K10#1 ↔ K10#2 | **はい** (同じ `vmin/vmax` を書く) | **あり** (板 299 で追加) `--rnpz-selftest` R28: local:// peer で 2000px の frame を開き、preview → 全解像度の着地まで駆動して **(a) 着地の `vmin/vmax` = 試験が自分で書いた独立ループの答え、(b) 同じ画素に対する `computeMinMax` の答えと bit 一致、(c) 非有限3標本・奇数列だけが持つ極値・全 NaN → 0..1・平坦 → +1** を固定 | **画素から再計算して製品と比べる試験**。較正2通りを実測: (A) `minMaxOf` を書き換えると R28b/c/f が赤・**R28d は緑**(両方が同時に動く = 1カーネル)、(B) worker 側にだけ差分を戻すと **R28d が赤**。両方を1本に統一したのが板 299 |
| P23 | Histogram 値域ハイライトの件数 ↔ bin の件数 | **一般には一致しない。母集団が一致する場合だけ一致する** | **あり** `--histhl-selftest` T5/T6 が**厳密 `==`** | `core/app/state.h:1134-1138` が「母集団が違う (bin は ~1M 間引き + ROI 限定、paint は全画素)。bin から導けば分母の違う2つの数が1つの文に並ぶ」と明記。T5/T6 は**間引きも ROI も効かない fixture で「2経路・1つの等式」を固定する**。片方を他方から導く実装にした瞬間、この試験は無意味になる |

### 4.1 数えると

- **parity 試験がある対: 10 (P1–P9、P22)。** うち 4 本が local/peer、2 本が client 内部の
  2経路、1 本が閉形式、1 本が構造的 (再計算しない)、1 本がキャッシュ対直呼び、
  **1 本が画素からの再計算 (P22、板 299 で追加)**。
- **parity 試験が無い / 間接だけの対: 9 (P10–P14、P17–P20)。**
- **local/peer 軸はほぼ守られている。** 守られていないのは
  **client 内部の対 (P10、P14、P17)**、**plugin 対 組み込み (P19)**、
  **percentile の2方式 (P20)** である。**min/max の写し (P22) は板 299 で解消**。
- 板 99 行が名指した σ_t は、**最も試験が厚い対 (P1) と、最も薄い対 (P10/P12) を
  同時に持っている。**

### 4.2 数値 assert を1つも持たない測定量

| 量 | 状態 |
|---|---|
| `uniformity/prnu-fpn` の `prnu_pct` / `row_fpn_pct` / `col_fpn_pct` / `shading_pct` | `core/selftest/bundled.inc` が `key:unit` の一覧と version `1.0.0` だけを固定する。**値はどこにも assert されていない** (`plugins/test/test_prnu.c` は `boxblur2d` 単体の応答を 1e-5 で見るだけ) → **板 302/303 で閉じた** (`--anavalue-selftest` U1/U2 が bilinear ramp 上の4量を閉形式で当てる) |
| `sharpness/gradient` の `varlap` / `tenengrad` / `grad_mean` | 同上 → **閉じた** (`--anavalue-selftest` G1-G4。ramp と checkerboard が互いの零を埋めるので3量すべてが `==` で固定) |
| `iso12233/e-sfr` の `mtf50` / `mtf20` / `sfr@nyquist` | 同上。カーブは stdout に出るだけで、**commit 間の人間の byte-diff** が唯一の防御 → **閉じた** (`--anavalue-selftest` S1-S3。理想エッジの `\|sinc(f)\|*sinc(f/4)^2` モデルに曲線 1%・交点 1%。副産物として `DEFECT(esfr-1)`: 暗レベル≠0 だと先頭空 bin の 0 埋めが LSF に偽インパルスを作り mtf50 が −5.4% ずれる。analyzer 側の欠陥なので別行) |
| Temporal の per-frame TSV 全体 | `core/selftest/framestats.inc` は**全 21 行で assert が 0 本**。TSV を stdout に印字して 0 を返す。ヘッダは「独立した numpy 実装が全数値を再現しなければならない」と書くが、**その実装はリポジトリにも CI にも無い** → **閉じた** (同ファイルの F1-F5。stdout は不変のまま、8x8 x5 枚の合成 stack の mean / σ / σ_col / σ_row / NaN 1画素 / 非常駐枚を有理数で固定) |

これは `stats-taxonomy.md` §6.6 が記録した事故の形そのものである:
ddof=1 の2式を ddof=0 に反転して **42 テスト全 PASS**。その後
`--export-tsv-selftest` E11 が値 assert を1本足して per-frame **領域**σだけが守られた
(P6)。**上の4量は板 302/303 (PR #273) で閉じた。P14 / P17 の一般形は
`--rowcol-sigma-selftest` が「現状の差を数値で固定」した段までで、統一そのものは
§6.1 順位 5 の裁定待ちである。**

### 4.3 「試験があるが、どこでも走るわけではない」

`tools/run_selftests.sh` は ctest のラベルで走らせる集合を選ぶ。
**`NOGL` を付けずに登録された試験は、GL コンテキストの無い機械では走らない。**
該当するのは `browse-keys` / `browse-dbl` / `browse-stop` / `browse-scan-async` /
**`abstats`** / **`verify`** / `tile` / **`abstats-cfa-bayer`** / `benchcov`。

**本書に関わる帰結**:

- **`abstats` A2 / A3 / A1p** —— ヒストグラム bin (4x256 の `memcmp`)、mean / sd、
  **面別 σ_t** を試験内の独立再実装に当てる唯一の parity —— は
  **GL コンテキストの無い機械では走らない**。
- **`verify` V13** (K4#1 と K4#2 を同じ NaN fixture で測る、P10 の間接分) も同様。
- CI の Linux ジョブは xvfb を入れ、`VIEWER_SELFTEST_REQUIRE_GL` で「コンテキスト
  無し」を失敗に変える。**つまりこの2本は3 OS のうち1つでしか効いていない。**

**板 302/303 (PR #273) で閉じた。** GL が要るのは `abstats` では T / S4-S6 / N の
3群、`verify` では V19 の1群だけだったので (PR #275 で V29 が加わり2群。
それぞれが自分の `glGroup()` の門を持つので、増やす代価は1行)、**GL 行はそのまま残したまま `-nogl`
兄弟を並べて登録**する形にした (`abstats-nogl` / `verify-nogl` /
`abstats-cfa-bayer-nogl`)。土台は `glGroup()` (`core/selftest/util.inc`) と
`--nogl-groups-skipped` で、旗が無ければ従来の `needWindow()` そのままなので
NOGL の誤付与は今までどおり赤になる。これで A1 / A2 / A1p / A3 / P1-P3 / S1-S3 の
**106 assert** と `verify` の **227 assert** (V13 を含む) が3 OS で走る。
`tile` は分割しない —— 2群が描き、うち1つは framebuffer を読み戻す。

**この形を今後の GL 混在テストの標準とする** (裁定, PR #273 レビュー)。代償は
GL ランナーで CPU 群が2回走ることで、CI 時間が問題になったら GL ランナー側で
`ctest -E '-nogl$'` として `-nogl` 兄弟を除外する。

参考として、`abstats-cfa-bayer` は `6308888` から一時 `DISABLED` にされた履歴を持ち、
原因は**製品ではなく selftest 側の `refSigmaT` が4面を pooled にしていたこと**だった
(キャッシュ 25.0850055866 対 再計算 25.0045948567、相対 3.2e-3 が 1e-6 の許容に対して赤)。
**参照実装も間違える。**「2つのコピーが同じ間違いをしていると確認に見える」
(`abstats.inc:199-210`) は、この件の教訓として残っている。

## 5. 板行が名指した「σ_t の CFA / NaN 二重実装」の実体

板 99 行の本文は `σ_t CFA/NaNの二重実装解消` である。調べた結果を正確に書く。

### 5.1 CFA — 二重ではなく**三重**で、規則が互いに両立しない

| 実装 | 面の判定 | `cfa!=0, ch==3` のとき | モザイク上の小 ROI のとき |
|---|---|---|---|
| K4#1 `temporal_model.inc:63-65` | `cfa!=0 && ch==1` **かつ `rw>=cell && rh>=cell`** | `nPl=1` → **黙って pooled** | `rw<cell` → `nPl=1` → **R/Gr/Gb/B を1つの σ_t に混ぜる** (Quad Bayer は `cell=4` なので 4px 未満の ROI 全部) |
| K4#2 `panel_projection.inc:1119` | `first->cfa ? 4 : 1` (**ch 条件なし**) | `nPl=4`。内側の ch ループが**全 ch を位置由来の面に畳む** → R/Gr/Gb/B と名札を付けた中身が ch 混合 | 常に 4 面 |
| K4#3 `serve.cpp:2219-2221` | `cfaType?4:1`、ただし `ch!=1` は**事前拒否** | **要求を断る** (`CFA planes need a 1-channel frame`) | 常に 4 面 |

**到達可能である。** `App::SeqInfo::cfaType` は ch を検査せずに doc の `cfa` から
写され (`core/app/open_dispatch.inc:582` ほか)、そのまま送られる (`:505`)。
processing plugin の出力 doc も ch 検査なしに `doc->cfa = out.cfa_type` を受ける
(`core/app/loader_npz.inc:2654`)。つまり**同じ stack が、Temporal パネルでは
pooled な σ_t、Series Analysis パネルでは面名札つき ch 混合の σ_t、peer では
エラー**を返しうる。

K4#4 (`setFoldStack:221`) と `foldOneRole` (`serve.cpp:2468`) だけが一致しており、
一致しているのは **2468 のコメントが「`runTemporalStats` からではなく local fold から
写した」と明記しているから**である。

### 5.2 NaN — 規則は同じ、**申告が違う**

per-pixel の除外規則 (自分の画素の母集団から外す。分母に畳まない。画素自体は
stack から落とさない) は K4 の全 site で同じである。違うのは**何を報告するか**で、
正典 ([analysis-layers.md](../../analysis-layers.md)) は
「NaN は画素ごとに除外して数えて … 除外数は結果が運ぶ」と要求している。

| 実装 | `nonFinite` の範囲 | `<2` 枚 | `0` 枚 | 画素毎枚数の幅 |
|---|---|---|---|---|
| K4#1 | **40k 標本分のみ** | `dropped` (引き算で導出) | 区別しない | — |
| K4#2 | ROI 全画素 | `dropped` | `unknown` | `cntMin` / `cntMax` |
| K4#3 (peer) | ROI 全画素 | **黙って `continue`、数えない** | 区別しない | — |
| K4#4 / `foldOneRole` | 全画素 | `F.dropped` | 区別しない | — |
| 参照 (`cli.inc`) | **NaN 処理なし** | — | — | — |

**同じ stack で、Temporal パネルの「除外数」は間引き部分集合の数、
Series Analysis 経路は全画素の数、peer の応答は何画素が分散を失ったかを
そもそも言えない。**

### 5.3 板行に無いが同じ対にある3つ目の食い違い: 標本と在室判定

- K4#1 は `remoteStep > 1` (decimated preview) と `px().empty()` を**濾さない**。
  dims だけを現在の doc と比べる。preview doc は**間引き後の w/h** を持つので、
  preview だけの stack に対して Temporal パネルは**間引き画素の σ_t** を出し、
  一方 `computeStackStats` は `needs >= 2 loaded frames` で断る。
  正典は「数えるのは実際に集計した枚数 —— preview・寸法違いは入れない」。
- K4#1 は 40k 格子、他は全画素。エクスポートはこの非対称を文面で申告している
  (`core/ui/panel_temporal.inc:903-906`)。
- K4#1 は非 CFA 多チャンネルで **ch0 のみ**、K4#3 は**全 ch pooled**。

## 6. 共有カーネル候補の順位付けと、各案のリスク

**並べる軸は2つ**: (a) 1つにしたときに**印字される数が動くか**、
(b) 動かす前に**裁定が必要か**。Fable が決めるのは (b) である。

### 6.1 順位

| 順 | 候補 | 何を1つにするか | 数は動くか | 要る裁定 |
|---|---|---|---|---|
| ~~**1**~~ **済** | **K10 min/max** (P22) | 純関数 `minMaxOf` を切り出し、`computeMinMax` と `rfWorker` の両方がそれを呼ぶ形にした | **動かなかった** (同一コードだった) | 不要だった。**【完了 2026-09-28 板 299】** parity 試験も同時に追加 (`--rnpz-selftest` R28) |
| **2** | **K4 σ_t の蓄積器** (P10/P11/P12/P13) | `setfold::pixelMeanCorr` の形を temporal 経路にも広げ、**CFA 面規則と NaN 報告構造体を関数の一部にする** | **動く**: 規則が食い違っている場所でだけ (モザイク上の小 ROI、`cfa!=0 && ch>1`、preview stack)。1面の場合は**ビット不変** (P1 が既に保証) | **要る**: 3つの CFA 規則のどれを正とするか。ch>1 + CFA 宣言を「拒否」(peer の今) か「pooled」(local の今) か |
| **3** | **K4 の在室・標本規則** | `remoteStep>1` / `px().empty()` の濾しを1箇所に。40k 格子を残すか撤去するか | **動く**: preview stack と小 ROI で | **要る**: Temporal パネルの応答性 (40k の理由) と正典の「実際に集計した枚数」の衝突 |
| **4** | **K1 領域モーメント + 標本上限の申告** (P17/P18/P19) | 1つの蓄積器 + **結果構造体に n と cap を必須フィールドで持たせる** | **動く**: 上限を揃えるなら、最小の cap より大きい ROI の**全部**で | **要る**: 5つの cap (200k/1M/2M/40k/full) をどれに寄せるか。統一せず「n を出す」だけなら数は動かない |
| **5** | **K2 行/列平均σ** (P14) | `varianceOfMeans` を1つにし、**正規化を宣言フィールドにする** | **動く**: n が profile 長 (桁が2つ小さい) なので**有効数字3桁目**。実測で全画面 +0.02%、n=8 で +6.9%、n=2 で +41% の桁の話 | **要る**: K3 との住み分けが正典で固定済みなので、**どの面がどの正規化を保つか**の裁定。加えて plugin の boxblur 残差に `n−1` を使う留保 (`analyzer_prnu.c:188-200`) |
| **6** | **K7 percentile / median** (P20) | 厳密 (quickselect) と近似 (65536 bin) のどちらかに寄せる | **動く**: Med / Auto % が動くと**書き出した PNG と動画の見た目が変わる**。`p50` を bin 化すると測定値が粗くなる | **要る**: 「表示レンジは近似でよい、測定値は厳密」を明文化するか、分位 (0.001/0.999 対 0.01/0.99) を揃えるか |
| **7** | **K6 histogram bin 規約** (P21) | 3つの規則を1つに | 動く (entropy が変わる) | 要る。ただし**払いが最も小さい** —— #3 は plugin 内部にしか出ない |

### 6.2 各案のリスク (印字される数が動く形)

- **候補2 (σ_t CFA)**: 現に食い違っている場所は、**どれかが既に間違っている**。
  `computeStackStats` の「面名札つき ch 混合」は名前が中身を偽っており、
  `temporal_model.inc` の「小 ROI で黙って pooled」は申告が無い。
  したがって「数が動く」ことは**欠陥の修正**であり、
  PR #123 と同じ形の裁定 (「挙動変更ではなく parity 違反の解消」) になる見込み。
  ただし **peer が絡む面は `rp::VERSION` を上げる必要がある**
  (ワイヤのキーの意味が変わるため。PR #127 が 5→6 を上げたのと同じ条件)。
- **候補4 (標本上限)**: ここは**欠陥ではない**。cap は応答性のための設計であり、
  正典はどれとも言っていない。**一番安い形は統一ではなく「n の申告」**で、
  それなら数は1つも動かない。統一に踏み込むと ROIs パネルの `sd` が全画面 ROI で
  動く (n 204800 → 12.3M)。
- **候補5 (行/列平均σ)**: **最も高リスク**。同じ export ファイルに2回出る列名を
  1つにすると、**既に配布された数値と食い違う**。`stats-taxonomy.md` §8 裁定6 は
  同種の件で「既配布の数値は遡及訂正しない。換算式を周知」を採った。
  加えて K3 との住み分けを壊してはならない (§K3)。
- **候補6 (percentile)**: 測定値ではなく**表示**が動く。`refitByFlavor` が
  sequence の毎フレームで走るので、動画書き出しの見た目が変わる。
  「測定値ではない」ことが逆にリスクで、**回帰に気付く試験が無い**。
- **候補1 (min/max)**: リスクはゼロに近い。**最初の題材として最も筋が良い。**

### 6.3 「触ってはならない」もの (取り違え防止)

| 対象 | 理由 |
|---|---|
| `core/selftest/abstats.inc:49-122` `RefHist` | 意図的な独立再実装。共有したら定義上一致してしまう |
| `core/selftest/abstats.inc` `refSigmaT`、`core/selftest/rtemporal.inc:37-94`、`core/selftest/rmeasure.inc:183-208`、`core/app/cli.inc:1564-1596` | 製品とずれるために存在する参照実装。**製品と同じ関数を呼ばせたら試験が消える**。`cli.inc:1591-1592` が自分でそう書いている |
| `core/app/profile_noise.inc` の A/B と `core/ui/panel_histogram.inc:983` の `sigma_row/col` | 正典が「改名も再定義もしない」と明示 (§K3) |
| `core/app/compare.inc:1172` `histHlPaint` の件数と bin の合計 | 母集団が違う (P23)。**片方を他方から導いた瞬間、`--histhl-selftest` T5/T6 の「2経路・1つの等式」が無意味になる** |
| `plugins/*` の famous name 規則 | `--setanalysis-selftest` が出荷ソースを走査して守っている。共有カーネル化で名前が漏れないこと |

### 6.4 試験を先に足せる場所 (裁定を待たずに赤にできるもの)

`stats-taxonomy.md` §6.6 が示した教訓は「異常を検出できない回帰テストなら
無い方がよい」であり、そこでは**二節のσを突き合わせる assert が書けなかった**
(標本が違うため閾値が勘になる)。本書の棚卸しでは、**同じ標本を測っている対**が
3つ見つかっている。それらは裁定を待たずに書ける:

1. **P10 (K4#1 ↔ K4#2)**: 同じ stack・同じ ROI で、**K4#1 の 40k 格子と同じ格子を
   K4#2 に渡せば**同じ標本になる。片方を格子に合わせられるので閾値が勘にならない。
   V13 は既に両方を同じ fixture で走らせているので、**assert を1行足すだけ**である。
2. ~~**P22 (min/max)**: 同じ画素・同じ式。**ビット一致を要求できる。**~~
   **【完了 2026-09-28 板 299】** `--rnpz-selftest` R28 が、試験自身の手書きループで
   画素から再計算した答えと、着地が書いた `vmin/vmax` と、同じ画素に対する
   `computeMinMax` の答えを**ビット一致**で突き合わせる。参照ループはカーネルを
   呼ばない (呼べば一緒に動いて永久に緑になるため)。較正2通りを実測済み。
3. **P17 のうち K1#1 ↔ K1#3**: どちらもモザイクセル単位ストライドなので、
   cap を揃えた呼び出しを1本立てれば同じ標本になる。

もう1つ、**カーネルを1行も触らずに閉じられる穴**がある: §4.2 の4量
(`prnu-fpn` / `sharpness` / `e-SFR` / per-frame TSV) に**値 assert を入れる**こと。
これは parity ではなく単体試験だが、板 99 行の後半
「単体テスト必須化」がまさにこれを指している。`--framestats-selftest` は
**assert が 0 本**なので、E11 と同じ形 (fixture から式で再導出した値を1本) を
足すのが最小手である。

残る P14 / P17 の一般形 / P19 / P20 は、**標本か規約が違うので、裁定の前に
書ける試験が無い**。これは §6.1 の順位がそのまま「試験を書ける順」でもある。

### 6.5 本書が数えたもの

| | 数 |
|---|---|
| カーネル種 | 10 (K1–K10) |
| 実装site (出荷コード、selftest を除く) | **41** (K1 10 / K2 4 / K3 1 / K4 5 / K5 3 / K6 3 / K7 6 / K8 2 / K9 3 / K10 4) —— 初版は 42・K10 5、板 299 で K10 の写し1件が消えた |
| うち共有で解決済み | 6 (`setfold.h` / `shading_probe.h` / `detrend.inc` の語と面 / `setPlaneFpn` / panel↔export の struct 読み / **`minMaxOf`(板 299)**) |
| 立てた対 | 23 (P1–P23) |
| parity 試験がある対 | 10 (初版 9、板 299 で P22 を追加) |
| 間接だけ / 無い対 | 9 (初版 10) |
| 一致しなくてよい・してはならない対 | 4 (P15 / P16 / P21 / P23) |
| 数値 assert を1つも持たない出荷測定量 | 4 群 (§4.2) |

**先行調査との突き合わせ**: stats-taxonomy.md §6.2 は**分散/σ を計算する式だけ**を
数えて 25 箇所だった。本書の 41 (初版 42) はその集合に percentile / histogram bin / median /
min-max / detrend を足したもので、**σ 系 (K1–K5 + K9 の `prnu_pct`) だけを取れば
§6.2 の 25 と同じ範囲を指す**。差は数える境界だけで、**site の集合に食い違いは
見つからなかった**。§6.2 の行番号は 2026-08-10 時点、本書は `fdbf5297` 時点である。
