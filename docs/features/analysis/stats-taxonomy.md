# Stats Taxonomy — 「基本 stats」と「追加 measurement」の境界定義

機能リクエスト(例:「フレーム毎の stats を Excel に貼りたい」)を即座に分類するための
基準文書。判断基準はただ一つ: **その数値を出すのに何を入力する必要があるか**。
実装の所在は [manual.md](../../guides/manual.md) / [analyzers.md](../../reference/analyzers.md) /
[remote.md](../remote/remote.md)、コードは `core/main.cpp`・`core/serve.cpp`・`plugins/*.c`。

## 1. 軸の定義

主軸は**入力要件**。クラスは以下の6つで、上から下へ「必要なものが増える」:

| クラス | 入力 | 例 |
|---|---|---|
| **S1** | **1フレームだけ**。追加情報ゼロ | mean/std、histogram、projection |
| **TN** | **同一条件の N フレーム**(塊/stack) | σ_t、FPN 分離、blink/RTS |
| **LM** | **条件を変えた M 個の stack**(レベルスイープ) | linearity、PTC gain K、SNR curve |
| **+meta** | 上記+**外部メタデータ**(照度、露光時間、gain、単位…)。ファイルには書かれていない | linearity の level [lx]、dark current [DN/s] |
| **+ref** | 上記+**参照データ**(dark/flat 基準画像、チャート形状・パッチ位置) | EMVA DSNU/PRNU、OECF、defect map 差分判定 |
| **+cal** | 上記+**校正されたハードウェア**(照度計、光源の絶対値) | 絶対感度、QE、EMVA 絶対値系 |

S1 が「基本 stats」、TN 以降が「追加 measurement」。境界は S1/TN の間にあり、
**「2枚目のフレームが要るか」が最初の質問**である。

直交する副軸(クラスとは独立に決まる):

- **出力形**: scalar(plane 毎の数値)/ curve(系列)/ map(画素マップ)。
  map は転送・表示コストがクラスと独立に跳ねる(remote では画素を返す話になる)
- **ROI 依存**: ほぼ全 stats が「選択 ROI、なければ全体」規約に従う(host 共通)
- **CFA plane 分離**: 本ツールの**鉄則 — plane は決して混ぜない**。R/Gr/Gb/B は
  常に別系列・別 fit(`drawPanelLinearity` のコメント、serve.cpp の `planeKey` 参照)。
  新規統計もこの規約に従うこと。plane 混合平均は「別の測定」であり既定にしない

## 2. 分類表

class 列: S1 / TN / LM(+m = +meta、+r = +ref)。「実装」列は現在の所在
(空欄 = 未実装)。出力形: sc = scalar per plane、cv = curve、mp = map。

| 名称 | class | 最小入力 | 出力 | 実装 | 備考 |
|---|---|---|---|---|---|
| min / max / mean / std / var | S1 | 1フレーム(+ROI) | sc | ROIs パネル、Inspector Statistics | 常時表示。sampling 上限 200k/ROI。**σ の ddof は裁定待ち → §6** |
| percentile / entropy / finite ratio | S1 | 同上 | sc | plugin `stats/moments` | NaN/Inf 検出込み |
| histogram(CFA 4系列) | S1 | 同上 | cv | Histogram パネル | 表示レンジ 256bin、クリップ率付き |
| H/V projection + profile 統計 | S1 | 同上 | cv+sc | Projection パネル | σ(列平均)= 列 FPN の S1 近似。**ddof は正典と不一致 → §6** |
| POI / 行・列バンド画素値 | S1 | 1フレーム+座標 | sc | Annotations、`X`/`Y` キー | |
| noise floor(タイル中央値)/ SNR点 | S1 | 平坦め ROI | sc | plugin `noise/floor` | σ_t+FPN 混合。純時間ノイズではない |
| PRNU proxy / 行・列 FPN % / shading | S1 | 明るい flat 1枚 | sc | plugin `uniformity/prnu-fpn` | **温度ノイズ混入**と明記済み。EMVA 値ではない |
| sharpness(varlap / tenengrad) | S1 | demosaic 済 1枚 | sc | plugin `sharpness/gradient` | 相対値のみ(a.u.) |
| e-SFR / MTF50 | S1 | 斜めエッジ ROI | cv+sc | plugin `iso12233/e-sfr` | シーン内容(エッジ)は前提だが入力は1枚 |
| A−B 差分・画素比較 | S1+r | 1フレーム+**参照画像 B** | mp+sc | A/B compare(`\`) | B が参照データ。クラス上は +ref の最小例 |
| defect pixel 検出(閾値法) | S1 | 1フレーム+閾値 | sc+mp | — | 簡易版は S1。dark/flat 基準の正式版は +ref |
| **temporal noise σ_t** | **TN** | **1 つの stack**(同条件 ≥2 フレーム) | sc | Temporal パネル / `MOP_TEMPORAL_STATS` | 画素毎時間 var(**ddof=1**)の平均。stack の性質 |
| **FPN(σ_fpn)** | TN | 同上 | sc | 同上 | `sqrt(max(0, var_spatial(時間平均, **ddof=1**) − C))`、C = `mean(s_t,i²/n_i)`。**補正量 `fpn_corr` とクランプを併記**(#57 判断2)。平坦でなければ絵柄込み |
| frame mean ドリフト / フリッカ | TN | 同上 | cv | Temporal パネルのグラフ | 横軸 frame、縦軸 ROI mean |
| **per-frame ROI 統計テーブル** | TN(中身は S1×N) | 同上 | cv/表 | server `MOP_FRAME_ROI_STATS`(plane 毎 mean/var 系列) | §3 参照。Excel 貼り付け要求の正体 |
| 時間平均フレーム(ノイズ低減画像) | TN | 同上 | mp | —(serve 内部では計算済み、出力なし) | EMVA 系 +ref の素材になる |
| blink / RTS 画素検出 | TN | 多め(≥50〜)のフレーム | sc+mp | — | 画素毎の時系列が要る。map 出力 |
| dark noise(EMVA) | TN+m | **dark stack** + 露光条件 | sc | Linearity の level-0 ルールが部分実装 | 「dark である」ことはメタ情報 |
| DSNU(EMVA 1288) | TN+r | dark stack の時間平均 → 補正済み空間σ | sc+mp | — | **補正込み** σ_fpn の dark 限定版(同じ `−C`)。正式には基準条件指定 |
| PRNU(EMVA 1288) | TN+r | **flat stack + dark stack** の各時間平均 | sc | —(`emva1288/*` 予定) | 現 plugin は proxy と明記 |
| **linearity fit(感度・offset・LE max)** | **LM+m** | レベル別 stack / frame ×3+ + **level 値と単位** | sc+cv | Series Analysis パネル | level は folder 名から auto、編集可 |
| conversion gain K(PTC) | LM | 同上(level 値自体は不要: σ_t² vs mean。σ_t を持つ stack メンバのみ) | sc | Series Analysis パネル(K [DN/e-]) | |
| read noise | LM / TN+m | PTC 外挿、または **dark stack で実測**(dark frame メンバは黒レベルのみ) | sc | Series Analysis パネル(extrap. 明示) | dark 有無で信頼度が変わる設計済み |
| SNR curve(mean vs SNR) | LM | レベル別 stack | cv | —(素材 = linearity rows) | |
| dynamic range | LM+m | 飽和までのスイープ+飽和定義 | sc | — | |
| dark current | LM+m | 露光時間違いの dark stack 群+**時間** | sc | — | 横軸が時間になった linearity |
| OECF(ISO 14524) | S1+r | チャート1枚+**パッチ位置・濃度** | cv | —(`iso14524/oecf` 予定) | チャート形状が参照データ |
| dead-leaves / texture SFR | S1+r | チャート1枚+チャート統計 | cv | —(`iso19567` 予定) | |
| 絶対感度 / QE | LM+cal | スイープ+**校正光源/照度計** | sc | — | ツール外の校正が支配的 |

## 3. 「フレーム毎テーブル」の位置づけ

「stack の各フレームの stats を Excel に貼りたい」は、**S1 統計を stack の
N フレームに順に適用したもの**であり、S1/TN 境界を表として可視化したものである。
サーバ実装は既にこの形をしている: `MOP_FRAME_ROI_STATS` は「per-frame spatial
mean/var of the ROI」を frame 番号を x とする系列で返す(serve.cpp)。

**行(= 1フレーム)に入ってよいもの — S1 統計のみ**:
frame 番号 / plane 毎 mean / var(std) / min / max / percentile / クリップ率など、
**そのフレーム単独から計算できる値だけ**。

**行に入れてはならないもの**: σ_t、σ_fpn、K、linearity error など TN/LM の値。
これらは **stack(あるいは stack 群)の性質であって、フレームの性質ではない**。
「フレーム 7 の σ_t」は**カテゴリエラー**である(σ_t の定義に時間軸が要る)。
要求されたら、テーブルの**フッター(stack 全体の行)**か別表に置く。
唯一の例外的な見せ方は「先頭からフレーム i までの累積 σ_t」だが、それは
明示的に別の列名(cumulative)を持つ別の統計として扱う。

### 3.1 σ_t は「1 つの stack につき 1 つ」で、推定量も 1 つ

同じカテゴリエラーが**逆向き**にも起きる。フレーム行に stack の量を置くのが
誤りであるのと同様に、**複数の stack を 1 本の時間軸として σ_t を取るのも
誤り**である。露光・ゲインの違う 3 つの stack(各 8 枚)を選んで 24 枚の
σ_t を出しても、それは何の測定値でもない。したがって **σ_t の入口は常に
「1 つの stack」**: 開いた stack の Temporal パネル、または Browse の
**連番(group)行**の `Temporal stats (server) for stack "…"`。複数選択から
σ_t を計算する暫定経路は**ユーザー裁定 (2026-08-09, #107) で削除**した
(拒否ではなく削除 —— 正しく計算できる対象が存在しないため)。

**推定量も 1 つ。** 画素毎の時間分散は **ddof=1(不偏)** で取る。local
(`recomputeTemporalIfNeeded`)・server (`MOP_TEMPORAL_STATS`)・
`computeStackStats` の 3 実装すべてが同じ式でなければならない
(#57 項目3, 2026-08-09 裁定)。local だけが ddof=0 だった期間があり、同じ
stack の σ_t が測る場所によって √(N/(N−1)) 倍(N=8 で 6.9%、N=16 で 3.3%)
食い違っていた。**local / server は転送路であって量ではない。**
`docs/features/analysis/flat-field-stats.md` の `− ⟨σ_t²⟩/N` 補正もこの ddof=1 を前提に書かれている。

## 4. リクエスト判定フロー

上から順に質問する。最初に Yes になった行がクラスと実装コストを決める。

| # | 質問 | Yes → クラス | 実装コスト帯 |
|---|---|---|---|
| 1 | 2枚目のフレームが要るか? | No → **S1** | 既存の数値の見せ方なら **UI-only**(TSV copy 等)。新統計なら **local compute**(plugin か host)。remote stack 全体に効かせるなら **MEASURE op**(`MOP_ANALYZER` 経由なら protocol 変更なし) |
| 2 | 同一条件の N フレームか? | **TN** | 常駐フレームだけなら local compute(`computeStackStats` 系)。**全フレーム対象なら必ず protocol MEASURE op**(remote stack は手元に全画素がない。processing policy `auto/server/local-fetch` に従う) |
| 3 | 条件(レベル)を変えた複数 stack か? | **LM** | Series Analysis パネルの拡張が第一候補。新パネルは最後の手段 |
| 4 | ファイルに無い数値(照度・時間・gain)が要るか? | **+meta** | **新 user-input field**: stack 単位の編集可能フィールド+auto 抽出+session 保存(`SeqInfo::level` が先例) |
| 5 | 参照画像・チャート定義が要るか? | **+ref** | 参照の指定 UI+session 保存+検証(サイズ/dtype 一致)。コスト最大 |
| 6 | 校正器材が要るか? | **+cal** | ツールの外。数値の**入力欄と単位表記**だけ提供する |

コスト帯の目安: UI-only < local compute < MEASURE op(protocol 版数管理が付く)
< user-input field(永続化と provenance 表記が付く)。複合する場合は加算。
どのクラスでも **CFA plane 分離**と **provenance 行**(measure-ux.md)は必須要件。

## 5. ギャップ(taxonomy が露呈させる欠落)

センサ評価ワークフローへの価値順。class タグ付き:

1. **per-frame テーブルの持ち出し**(TN、中身 S1×N) — `MOP_FRAME_ROI_STATS` は
   実装済みだが、**ローカル stack 用の同型計算と「表としての TSV/CSV export」が無い**。
   曲線ではなく行列で出すだけ。コスト帯: UI-only+小さな local compute。最安で最需要
2. **SNR curve / dynamic range**(LM/+meta) — 素材(level 毎 mean と σ_t)は
   Linearity が既に持っている。派生曲線1本と飽和定義の入力欄で済む
3. **EMVA 準拠 DSNU / PRNU**(TN+ref) — 現 PRNU は単一フレーム proxy と自認済み。
   dark/flat stack の指定 UI(「この stack を dark として使う」)が本体で、
   計算は時間平均後の既存統計。`emva1288/*` 予定の中で最初に着手すべき項目
4. **defect pixel カウント/マップ**(S1 簡易 / TN+ref 正式) — 現状ゼロ。
   閾値法(S1)だけでも歩留まり確認に有用。map 出力の表示経路(オーバーレイ)が
   新規要素
5. **blink / RTS 検出**(TN、map) — 画素毎時系列が必要で、remote では
   MEASURE op 必須。長い stack の主用途になり得るが実装コストは高い
6. **dark current**(LM+meta) — 横軸を露光時間にした linearity。level 単位
   フィールドが既に自由記述(`ms` 可)なので、実は「dark stack 群+単位 ms」で
   近い数値は出る。正式化は fit の再解釈のみ
7. **OECF / dead-leaves**(S1+ref) — チャート定義の入力という新カテゴリを開く。
   analyzers.md の予定リストにあり、優先度は上記より下

逆に、taxonomy 上「安いのに未提供」なもの: S1 統計の**クリップ率・飽和画素率**の
グリッド常設(histogram 内には表示済み)。UI-only で拾える。

## 6. 空間σの ddof — 全数調査と裁定材料 (board 193行、PR #123 の報告)

> **本節は裁定待ちであり、実装は一切変えていない。** やったのは「どこがどう
> なっているか」を数えて測ることだけ。答えは §7 の判断リストに**番号で**返せる。

PR #123 は σ_t の parity 破れ (local ddof=0 / peer ddof=1) を直したとき、
**別の**食い違いを見つけて意図的に触らずに報告した: **1フレームの空間σ**が
`core/ui/panel_temporal.inc` では ddof=1、`panel_rois.inc` / `canvas.inc` /
`panel_histogram.inc` では ddof=0。これは #57 判断3 が裁定した σ_t とは
**別の量** (1フレームの S1) なので、同じ PR で動かさなかった。

これが問題になるのは、本プロジェクトが既に持つ規則のためである:
**同じ名前の数が、表示される場所によって違ってはならない。** PR #127 は
`sigma_s` / `sigma_fpn` の一量二名を潰すときにこの論を立て、PR #130 の
有名名スキャナはその一般形を機械化した。

### 6.1 結論を先に — これは「パネル間の対立」ではなく「量の取り違え」である

board 行は「panel_temporal は 1、rois/canvas/histogram は 0」と書くが、
全数調査するとこの読みは**二重に不正確**だった:

1. **ddof=1 側は1パネルではなく1関数**。`exportPerFrameBlock` 内の2式だけで、
   同じ Temporal パネルの**グラフ** (`T.frameStd`) は ddof=0 である。つまり
   Temporal パネルは**自分自身と食い違っている** —— 同じフレームの同じσを、
   グラフでは 0、エクスポートでは 1 で出す。
2. **ddof=0 側は一枚岩ではない**。Projection パネルの `sigma_row` / `sigma_col`
   は「行/列平均のσ」であり、これは正典 [flat-field-stats.md](flat-field-stats.md)
   が **ddof=1 と明示的に指定している A と B そのもの**である(「**ddof は全部
   ddof=1 で統一する** —— T も A も B も」「A, B は ddof=1 で不偏。T を ddof=0 で
   取ると O(1/W) の不整合が戻ってくる」)。ここは「どちらでもよい記述統計」では
   なく、正典が理由つきで既に答えている。

したがって裁定すべきは「0 か 1 か」の一問ではなく、**二つの量それぞれについて
一問ずつ**である。

### 6.2 全数調査

`core/` と `plugins/` の**出荷コード**(`core/selftest/` を除く)で分散/σを
計算している式を**全数 25 箇所**確認した。うち**空間σまたはその派生が 16 箇所**
(I 群 12 + II 群 4)。残り 9 箇所は推定量の入力 (III 群 3) と時間σ (IV 群 6) で、
どちらも #57 で確定済み・全実装一致であり本件の対象外。

`core/selftest/` の**参照実装が別に 5 箇所**ある(`abstats.inc:94` が ddof=0 で
canvas を、`detrend.inc:124` / `rtemporal.inc:69` / `setanalysis.inc:371` /
`abstats.inc:156` が ddof=1 で推定量側を写す)。product が動けば同数が動く。

#### I 群 —— 領域そのものの画素の σ (S1 記述統計)

| # | 実装 | ddof | 出る場所 |
|---|---|---|---|
| 1 | `core/ui/panel_rois.inc:50` | **0** | ROIs パネル `sd` 列、および `std / mean [%]` (:73) |
| 2 | `core/ui/canvas.inc:1424` | **0** | Histogram パネルの `sd` / `var` 列 |
| 3 | `core/ui/panel_histogram.inc:654` | **0** | Projection パネル `σ (frame)`、TSV `sigma_frame` |
| 4 | `core/ui/panel_temporal.inc:472` | **1** | Temporal エクスポート第3節 `sigma[_ch] [DN]` |
| 5 | `core/app/temporal_model.inc:96` | **0** | Temporal パネルの frame std グラフ (`T.frameStd`) |
| 6 | `core/serve.cpp:1229` | **0** | peer `frame std` 系列 (`MOP_TEMPORAL_STATS`) |
| 7 | `core/serve.cpp:1354` | **0** | peer `roi var` 系列 (`MOP_FRAME_ROI_STATS`) |
| 8 | `plugins/analyzer_stats.c:136` | **0** | `<ch>.std` / `<ch>.var` |
| 9 | `plugins/analyzer_noise.c:117` | **0** | `<ch>.std` |
| 10 | `plugins/analyzer_noise.c:101` | **0** | 16x16 タイル std → ノイズ床 (中央値) |
| 11 | `plugins/analyzer_prnu.c:173` | **0** | `prnu_pct` (高域残差の σ) |
| 12 | `plugins/analyzer_sharpness.c:75` | **0** | `varlap` (a.u.。σ ではないが同じ二次モーメント) |

**12 のうち 11 が ddof=0。外れ値は #4 ひとつ。**

#### II 群 —— 行平均 / 列平均の σ (正典の A と B)

| # | 実装 | ddof | 出る場所 |
|---|---|---|---|
| 13 | `core/ui/panel_histogram.inc:679` | **0** | Projection `σ (axis)`、TSV `sigma_row` / `sigma_col` (+ `_pct`) |
| 14 | `core/ui/panel_temporal.inc:484` | **1** | Temporal エクスポート第3節 `sigma_col[_ch] [%]` / `sigma_row[_ch] [%]` |
| 15 | `plugins/analyzer_prnu.c:190` | **0** | `row_fpn_pct` |
| 16 | `plugins/analyzer_prnu.c:198` | **0** | `col_fpn_pct` |

**正典が ddof=1 と指定している群で、4 のうち 3 が ddof=0。** #14 だけが正典に
合っている —— I 群の外れ値と同じ関数が、II 群では唯一の正解である。

#### III 群 —— 推定量の入力としての空間σ (#57 判断2/4 で確定、対象外)

`core/app/temporal_model.inc:164` (σ_fpn local) / `core/serve.cpp:1264`
(σ_fpn peer) / `core/app/setanalysis.inc:488` (`setPlaneFpn` = DSNU / PRNU)。
**3 箇所すべて ddof=1**、local と peer は同じ順序・同じクランプ位置。

#### IV 群 —— 画素ごとの時間σ (#57 判断3 で確定、対象外)

`core/app/temporal_model.inc:135` / `core/serve.cpp:1242` /
`core/ui/panel_projection.inc:814` (`computeStackStats`) / `core/setfold.h:100`
(`pixelMeanCorr`) / `core/app/setanalysis.inc:234` / `core/app/cli.inc:1315`。
**6 箇所すべて ddof = n_i − 1**、PR #123 以降一致。

#### local / peer の分裂は **無い** —— これが方向を決める

#123 が直したのと同じ級の欠陥(local と peer が同じ量に別の ddof)が空間σにも
あるかを個別に確認した。**無い。** peer が per-frame の空間統計を出すのは 2 箇所
(`frame std` #6、`roi var` #7)で、**どちらも ddof=0**、local の相方(#5
`T.frameStd`、および #1 の ROI σ)と一致する。peer は行/列プロファイル統計を
**一切計算していない**(`MOP_*` 5 op すべて確認)。

**つまり今のパリティは ddof=0 側で成立している。** これは裁定の方向を片側だけ
高くする: I 群を ddof=1 に寄せると**今成立しているパリティを自分から壊し**、
`rp::VERSION` 上げが要る(枠は不変で既存キーの意味だけが変わる —— PR #127 が
5→6 を上げたのと同じ条件)。ddof=0 に寄せると **local 1 箇所で済み、ワイヤは
一切動かない。**

### 6.3 実測 —— どちらに動かすと何がどれだけ動くか

決定論的な 4096x3000 RGGB フレーム 1 枚(台座 ~2000 DN、PRNU 0.5%、列 FPN
1.8 DN、ショット+リードノイズ)の上で、**各サイトの式を間引き・モザイクセル
丸めまで含めて逐語的に写して**両 ddof を計算した。式が同じでも **n がサイト
ごとに全く違う**ことが本節の要点である。

#### 領域の σ (I 群) —— n は画素数**ではない**

`roiBasicStats` は 200k、`canvas` は 1M サンプルで間引く。全画面
4096x3000 = 12.3M 画素の ROI でも実際の n は 204800 / 256000 しかない。

| ROI | 行 | n | ddof=0 | ddof=1 | 差 |
|---|---|---|---|---|---|
| 全画面 4096x3000 | General | 204800 | 53.5649754 | 53.5651062 | **+0.0002 %** |
| 全画面 | R plane | 51200 | 46.0189253 | 46.0193747 | +0.0010 % |
| 512x512 | General | 262144 | 53.6608078 | 53.6609101 | +0.0002 % |
| 64x64 | General | 4096 | 54.3010657 | 54.3076955 | +0.0122 % |
| 64x64 | R plane | 1024 | 46.2856836 | 46.3083006 | +0.0489 % |
| 16x16 | R plane | 64 | 54.0048889 | 54.4318116 | +0.7905 % |
| 8x8 | R plane | 16 | 62.0236296 | 64.0577292 | +3.2796 % |
| 3x3 | General | 9 | 71.1939401 | 75.5125767 | **+6.0660 %** |
| 3x3 | R plane | 4 | 64.0423080 | 73.9496875 | **+15.4701 %** |

#### 行/列平均の σ (II 群) —— n は**プロファイル長**で、桁が二つ小さい

ここが「見えるようになる」場所である。全画面でも n は 2048 / 1500 しかなく、
3x3 の CFA プレーンでは **n=2** になる。

| ROI | 量 | n | ddof=0 | ddof=1 | 差 |
|---|---|---|---|---|---|
| 全画面 | `sigma_col` | 2048 | 9.4088283 | 9.4111262 | +0.0244 % |
| 全画面 | `sigma_row` | 1500 | 7.7472443 | 7.7498280 | +0.0334 % |
| 512x512 | `sigma_col` | 256 | 3.8018739 | 3.8093212 | +0.1959 % |
| 64x64 | `sigma_col` | 32 | 7.2282596 | 7.3439191 | +1.6001 % |
| 16x16 | `sigma_col` | 8 | 11.4452071 | 12.2354411 | **+6.9045 %** |
| 8x8 | `sigma_col` | 4 | 29.6589296 | 34.2471820 | +15.4701 % |
| 3x3 | `sigma_col` | 2 | 32.4767151 | 45.9290109 | **+41.4214 %** |

peer の `roi var` は**分散**なので比は二乗で効く: 3x3 の R プレーンで
4101.41721 → 5468.55628、**+33.3333 %**。

#### 同じフレーム・同じ ROI を、いまの各面が答える値

128x128 ROI、R プレーン、同一フレーム。**「同じ名前の数が場所で違う」の実物**:

| 面 / 列 | n | 値 [DN] | 今の ddof |
|---|---|---|---|
| ROIs パネル `sd` | 4096 | 45.9487527 | 0 |
| Histogram パネル `sd` | 4096 | 45.9487527 | 0 |
| Projection `sigma_frame` | 4096 | 45.9487527 | 0 |
| peer `roi var` → σ | 4096 | 45.9487527 | 0 |
| **Temporal エクスポート `sigma [DN]`** | 4096 | **45.9543627** | **1** |
| Projection `sigma_col` | 64 | 6.1745032 | 0 |
| **Temporal エクスポート `sigma_col`** | 64 | **6.2233143** | **1** |

領域σの食い違いは 0.012 %(有効数字4桁目)。**行/列σの食い違いは 0.79 % ——
有効数字3桁目、報告書が引用する桁である。**

#### 精度の側から見ると、この選択は**測定の不確かさを一度も支配しない**

σ 自身の相対標準誤差は約 `1/sqrt(2(n−1))`、ddof の差は約 `1/(2n)`。比は
`1/sqrt(2n)` で、**n がいくつでも ddof 差は σ 自身のばらつきの数分の一以下**:

| n | ddof 差 | σ の相対標準誤差 | 比 |
|---|---|---|---|
| 65536 | 0.0008 % | 0.28 % | 0.003 |
| 64 | 0.79 % | 8.9 % | 0.089 |
| 9 | 6.07 % | 25.0 % | 0.24 |
| 4 | 15.47 % | 40.8 % | 0.38 |
| 2 | 41.42 % | 70.7 % | 0.59 |

**ddof の選択が見える n では、その数字自体がその桁まで信用できない。数字が
信用できる n では、ddof の選択は見えない。** よってこれは**精度の問題ではなく
一貫性の問題**である。どちらを選んでも測定は壊れない —— 壊れているのは
**混在**という規則違反そのものであり、そう扱えば安く決着する。

### 6.4 どちらが正しいか —— 量ごとに答えが違う

二つの主張はどちらも本物であり、正面からぶつかる:

- **母集団側 (ddof=0)**: あるフレームの画素は、そのフレームの**全体**である。
  「この領域の数値のばらつきは何か」への答えは推定ではなく確定値で、n−1 で
  割る理由がない。
- **標本側 (ddof=1)**: 誰もそのフレームを知るために撮らない。同じ画素は
  **センサの振る舞いの標本**であり、読む人は必ず「このセンサのノイズ」として
  読む。正典も EMVA 系の式も、時間側の確定判断 (#57 判断3) も ddof=1 に寄る。

**推奨: 量で分ける。** 全数調査が示したのは、この分裂が既に「パネル別」ではなく
「量別」に走っているという事実だからである。

#### 推奨 A —— I 群 (領域の σ) は **ddof=0 に統一**

動かすのは `core/ui/panel_temporal.inc:472` の**1 式のみ**。理由:

1. **12 のうち 11 が既に 0。** 規約を変えるのではなく外れ値を直すのであって、
   #123 と同じ形の裁定になる(「挙動変更ではなく parity 違反の解消」)。
2. **peer が 0 側にいる。** 0 に寄せれば local/peer は全サイトで一致し、ワイヤも
   `rp::VERSION` も動かない。1 に寄せると**いま成立しているパリティを壊して**
   版上げが要る。
3. **これらの面は間引いている。** 200k / 1M 上限のストライド標本は i.i.d. では
   なく(モザイクセル単位で、列 FPN と相関する)、ddof=1 が約束する不偏性を
   そもそも供給できない。**払えない保証に systematic な代金を払うことになる。**
4. **Temporal パネルの自己矛盾が同時に消える。** グラフ (`frameStd`、ddof=0) と
   第3節エクスポート (ddof=1) が、同じフレームの同じσで一致する。
5. **プールされた `all` 行の恒等式が厳密なまま残る。** `verify.inc` V17 は
   「pooled var = プレーン内分散の平均 + プレーン平均の分散」を assert する。
   この分解は **ddof=0 で厳密**、ddof=1 では O(1/n_plane) ずれる(V17 の
   1e-3 許容は全画面では隠すが、小領域では隠さない)。
6. taxonomy 上これは S1 =「1フレームだけ、追加情報ゼロ」の記述統計であり、
   正典の ddof=1 規約はこの層に**掛かっていない**(掛かる範囲は「T も A も B も」
   +「(b)(c) の空間分散」= 推定量の中)。

#### 推奨 B —— II 群 (行/列平均の σ) は **ddof=1 に統一**

動かすのは `panel_histogram.inc:679` と `plugins/analyzer_prnu.c:190,198`。理由:

1. **正典が既に理由つきで答えている。**「A, B は ddof=1 で不偏。T を ddof=0 で
   取ると O(1/W) の不整合が戻ってくる」「ddof を混ぜない」。
2. **これは記述統計ではなく推定量の部品である。** `T − A − B` で σ_p を出す分解
   (判断record の (a)) が着地したとき、A と B が ddof=0 だと分解が合わない。今
   0 のままにすると、(a) 実装時に**出荷済みの数が黙って動く**か、(a) が不整合な
   A を継ぐかの二択になる。
3. **n がプロファイル長で小さい。** 実際に見える群である(n=8 で +6.9 %、n=2 で
   +41 %)。正典の警告「小さな ROI や Bayer プレーンでは効く」はこの群のこと。
4. peer はこの量を計算していないので、**ワイヤは動かない**。

#### 何を差し出すことになるか (推奨の代償)

- **「ddof は全部 ddof=1」という一行が、プログラム全体については言えなくなる。**
  規則が二行になる:「推定量の中は ddof=1。1フレームの記述統計は ddof=0。ただし
  行/列平均のσは推定量の部品なので ddof=1」。`flat-field-stats.md` しか読んで
  いない人には ROIs パネルがバグに見え、「直され」る。**この推奨は、語彙が
  それを書く場合にのみ安全である**(判断8)。
- **同じ表の隣り合う列で ddof が違う。** Projection の TSV は `sigma_frame`
  (ddof=0) と `sigma_col` (ddof=1) を並べることになる。一見、今潰そうとしている
  不整合そのものに見える。差は「別の量だから」であり、**列の説明がそれを言わない
  限り弁護できない**(判断4)。
- **3x3 ROI の σ は、統計家が 6 % 低いと言う値のまま残る。** 受け入れる根拠は
  §6.3 の最終表 —— その n では数字自体が 25 % ばらついており、供給できない
  不偏性のために systematic なずれを入れる取引は悪い。

#### 採らなかった案

| 案 | 却下理由 |
|---|---|
| **全部 ddof=1** | 16 サイト中 14 が動く。peer 2 サイトが動くので `rp::VERSION` 上げが要り、**いま成立しているパリティを自分から壊す**。間引き標本に不偏補正を掛ける正当化も無い |
| **全部 ddof=0** | 正典(「A, B は ddof=1 で不偏」)に正面から反し、(a) の分解が O(1/W) ずれる。#14 を「直す」ことで正典準拠の唯一のサイトを壊す |
| **現状維持** | 規則違反が残るだけでなく、**1つのエクスポート文書の中に同名列が両 ddof で並ぶ**(§6.5)。放置は「読者に判別できない」を確定させる |

### 6.5 エクスポート —— 既に配られた数値をどうするか

**まず、いま既に壊れている点を明示する。** `buildTemporalExport` の吐く TSV は
第2節(H/V profile statistics、`ProjState` 由来 = **ddof=0**)と第3節
(per-frame statistics = **ddof=1**)を**一つのファイルに**持つ。1ch 非 CFA の
stack では第3節の列名にプレーン接尾辞が付かないため、**同一ファイル内に
`sigma_col [%]` という列名が二度、別の ddof で現れる**。既存の
`--export-tsv-selftest` の assert が `has(tsv, "sigma_col [%]")` で
**どちらの節に当たったか区別できていない**のはその証拠である。

σ を運ぶエクスポート面は次の通り:

| 面 | 運ぶ量 | 今の ddof | 版・日時 |
|---|---|---|---|
| Temporal `Copy (TSV)` / CSV 第1節 | σ_t / σ_fpn / σ_tot | 1 (確定済み) | **持つ** (`app: viewer <ver>` + 生成日時) |
| 同 第2節 | `sigma_frame` / `sigma_row` / `sigma_col` (+ %) | **0** | 同上 |
| 同 第3節 | `sigma[_ch]` / `sigma_col[_ch] [%]` / `sigma_row[_ch] [%]` | **1** | 同上 |
| Projection `Copy table (TSV)` | `sigma_frame` / `sigma_row` / `sigma_col` (+ `_pct`) | **0** | **持たない**(ROI と reduce のみ) |
| Analysis `Copy table (TSV)` | `<ch>.std` / `.var` / `prnu_pct` / `row_fpn_pct` / `col_fpn_pct` | **0** | 持つ (`ana.prov` + plugin 版) |
| peer measure 応答 | `frame std` / `roi var` | **0** | 送信側の版で決まる |

**ROIs パネルにはエクスポートが無い** —— 画面から手で写されている。そこの数は
遡って照合できない。

**announce すべきこと(裁定がどちらでも):**

1. **各σ列は自分の ddof を名乗るべきである。これは移行注記ではなく、今欠けて
   いる申告である。** 現状、第2節と第3節のどちらを貼ったのかを読者が判別する
   手段が無い。#123 が σ_t の method 行に推定量を名乗らせたのと同じ理由
   (「local 行と server 行が並ぶ表なので」)が、ここでは**同じ文書の隣の節**に
   対して成立している。
2. **Projection の TSV には版も日時も無い。** II 群を動かすなら、この面だけは
   「いつの規約か」を後から言えない。**動かす前に provenance 行を足すべき** ——
   Temporal エクスポートが既に持っているので形はある。
3. **遡って書き換えない。** 既に配られた表は ddof=0 の ROI 値と ddof=1 の
   Temporal 値で成立している。訂正すべきは**数値ではなく、その数値がどちらの
   規約かを言えなかったこと**。announce は「この版から `sigma_col` は ddof=1
   (以前は ddof=0)、差は `sqrt(n/(n−1))`、n は列数」の形で、**再測定なしに
   換算できる**ように書けば足りる(#127 が uncorrected upper bound を
   `sqrt(sigma_fpn^2 + fpn_corr^2)` で復元可能にしたのと同じ流儀)。
4. 影響の大きさは正直に言う: **全画面・通常 ROI では 0.05 % 未満で、実務上
   どの報告書も書き換わらない。** 書き換わるのは小 ROI と CFA プレーンスライス、
   そして行/列σの全域である(§6.3)。

### 6.6 ピン(テスト)を今回入れなかった理由

裁定が出るまで不整合が**広がらない**ようにするピンを検討したが、**入れていない**。
理由は「静かに落ちるピンなら無い方がまし」だからである。

まず、実測した事実を二つ:

- **`--framestats-selftest` は TSV を stdout に印字するだけで、数値を一つも
  assert していない。** ゆえに #4 / #14 (ddof=1 の2式) は**どのテストにも
  守られていない**。実験として #4 と #14 を ddof=0 に反転してスイートを回したところ
  **42 テスト全 PASS、失敗ゼロ**だった(実験は測定後に revert 済み)。
  このサイトが隣と食い違ったまま残れたのは、これが理由である。
- 逆に **ddof=0 側は守られている**: `--roistats-selftest` が解析フィクスチャの
  真値に対して **8 本**の exact assert を持つ(`sqrt(250004.0)`、`2.0`、`0.5`、
  `sqrt(1250009.0)`、`3.0` …)。`verify.inc` V17 のプール恒等式も効く。

**守りが要るのは ddof=1 側であり、そこに loud なピンを今日は書けない。** 二節の
σを突き合わせる assert は書けない —— 第2節は間引き標本、第3節は全画素走査で、
**同じ標本を測っていない**ので比が `sqrt(n/(n−1))` にならず、閾値は勘になる。
残る手はテスト内に推定量を書き直すことだが、**自分が守る対象を再実装した
テストは、両方を一緒に編集された日に黙って通る** —— PR #130 のスキャナが
存在する理由がまさにその失敗である。

したがって推奨は、判断1/2 の**実装 PR の中で**、per-frame エクスポートのσに
**値の assert を1本**(フィクスチャから式で再導出したもの)足すこと。裁定と
同じコミットで入れば、守る値が確定しているので loud に書ける。判断7 を参照。

## 7. 判断リスト (空間σの ddof)

各項は**推奨と一行の理由**。番号で答えられる。根拠は §6 の当該節にある。

1. **I 群(領域そのものの σ)の統一先** —— 推奨: **ddof=0**。12 サイト中 11 が
   既に 0 で peer もそこにおり、動くのは `panel_temporal.inc:472` の1式・ワイヤ
   不動で済むから(§6.4 推奨A)。
2. **II 群(行/列平均の σ)の統一先** —— 推奨: **ddof=1**。正典が A/B を理由つきで
   ddof=1 に指定済みで、ここを 0 のままにすると σ 分解 (a) の着地時に出荷済みの
   数が黙って動くから(§6.4 推奨B)。
3. **同梱プラグインの扱い** —— 推奨: **判断1/2 にそのまま従う**
   (`prnu_pct`・`.std`・タイル std は 0 のまま、`row_fpn_pct`/`col_fpn_pct` は
   1 へ)。プラグインだけ別規約にする理由が無く、`varlap` は a.u. で σ ではない
   から。
4. **σ 列の申告義務** —— 推奨: **各σ列が自分の ddof を名乗る。裁定がどちらでも
   実施する。** 今は同一 TSV 内に同名列が両 ddof で並び、読者に判別手段が無い ——
   これは移行注記ではなく欠けている申告だから(§6.5)。
5. **Projection TSV の provenance** —— 推奨: **II 群を動かす前に
   `app: viewer <ver>` + 生成日時を足す**。この面だけ「いつの規約か」を後から
   言えず、Temporal 側に既に形があるから(§6.5-2)。
6. **既配布の数値** —— 推奨: **遡及訂正しない。換算式だけ announce する。**
   全画面・通常 ROI では 0.05 % 未満で報告書は書き換わらず、変わる範囲は
   `sqrt(n/(n−1))` で再測定なしに戻せるから(§6.5-3)。
7. **ピン(テスト)** —— 推奨: **本 PR では入れない。判断1/2 の実装 PR で
   per-frame エクスポートのσに値 assert を1本足す。** 今日書ける形は
   閾値が勘になるか推定量の再実装になり、どちらも黙って通るから(§6.6)。
8. **正典の書き換え** —— 推奨: **`flat-field-stats.md` の「ddof は全部 ddof=1 で
   統一する」に適用範囲の一文を足す**(推定量の中の空間分散に掛かり、S1 の記述
   統計には掛からない)。この一行だけを読むと ROIs パネルはバグに見え、
   「直され」るから(§6.4 の代償)。

---

## 8. 裁定 (2026-08-10)

ユーザー裁定: 「裁定１，２の話はMakeSenseです．推奨でよさそうですね．」

**§7 の8件すべて推奨どおりで確定。** 番号は §7 のまま。

| # | 確定 |
|---|---|
| 1 | **I 群 (領域そのものの画素の σ) は ddof=0 に統一** |
| 2 | **II 群 (行/列平均の σ) は ddof=1 に統一** |
| 3 | 同梱プラグインは 1/2 に従う |
| 4 | 各σ列が自分の ddof を名乗る |
| 5 | Projection TSV の provenance を **II 群を動かす前に**足す |
| 6 | 既配布の数値は遡及訂正しない。換算式 `sqrt(n/(n−1))` を announce |
| 7 | per-frame エクスポートのσに値 assert を1本 |
| 8 | 正典 (`flat-field-stats.md`) に適用範囲の一文 |

### 実施上の拘束

**1つの変更として入れる。** 半分だけ適用した状態は「同じ名前の数が、表示される
場所によって違う」——この裁定が潰そうとしている欠陥そのものになる。

**4 と 8 は 1/2 と不可分**である。1/2 を採ると「ddof は全部 ddof=1」が
プログラム全体については言えなくなり、規則が2行になる:

> 推定量の中は ddof=1。1フレームの記述統計は ddof=0。ただし行/列平均のσは
> 推定量の部品なので ddof=1。

`flat-field-stats.md` しか読んでいない人にはこの状態で ROIs パネルがバグに
見え、「直され」る (8 が要る理由)。そして Projection の TSV は `sigma_frame`
(ddof=0) と `sigma_col` (ddof=1) を**隣り合う列に並べる**ことになり、列の説明
がそれを言わなければ、一見いま潰している不整合そのものに見える (4 が要る理由)。

**5 の「前に」は順序の指定である。** この面だけ「いつの規約で出した数か」を
後から言えないので、II 群を動かす前に provenance が要る。

### 動く箇所 (§6.2 の全数調査より)

- **I 群 → 0**: `core/ui/panel_temporal.inc:472` の**1式のみ**。12箇所中11は既に
  0 で、peer も 0 側にいる。**ワイヤ不動・`rp::VERSION` 不動** (1 に寄せると
  いま成立しているパリティを自分から壊して版上げが要る、が推奨Aの根拠)
- **II 群 → 1**: `core/ui/panel_histogram.inc:679`、`plugins/analyzer_prnu.c:190`
  (`row_fpn_pct`)、`plugins/analyzer_prnu.c:198` (`col_fpn_pct`)。peer はこの量を
  計算していないので**ここもワイヤ不動**

`core/selftest/` の参照実装5箇所は product が動けば同数動く (§6.2)。

### 実装直前の再特定 (2026-08-10、`563e7c2` 時点)

§6.2 の全数調査は行番号つきだが、**その行番号は当日のうちに動いた** —
`panel_histogram.inc` だけで PR #144 (float 累算器) と #157 (bin スナップ) の
2本が触っている。I 群と II 群は**逆方向に動かす**ので、行番号で当てにいくと
取り違えたとき片方が二重に間違う。式で引き直した結果が下である。**箇所数は
4 で §6.4 と一致**した (行番号だけが動いていた)。

| # | 今の場所 | 何 | 今 | 裁定後 |
|---|---|---|---|---|
| 1 | `core/ui/panel_temporal.inc:473` | I 群 —— per-frame エクスポートの領域σ | ddof=1 | **0** |
| 2 | `core/ui/panel_histogram.inc:729` | II 群 —— プロファイルσ (`P.h`=列平均 / `P.v`=行平均、**両軸が1ループ**) | ddof=0 | **1** |
| 3 | `plugins/analyzer_prnu.c` `rowFpn` | II 群 —— `row_fpn_pct` | `sqrt(Σd²/ph)` | **`/(ph−1)`** |
| 4 | `plugins/analyzer_prnu.c` `colFpn` | II 群 —— `col_fpn_pct` | `sqrt(Σd²/pw)` | **`/(pw−1)`** |

同じ関数の中で**触らない**もの (取り違えやすいので明記する):

- `panel_temporal.inc:484` (`profCv` の中) —— II 群で、**既に ddof=1**。
  つまり `panel_temporal.inc` は「I 群では唯一の外れ値、II 群では唯一の正解」を
  同じ関数の中に両方持っている。
- `panel_projection.inc:822` —— 名前が似ているが **IV 群 (画素ごとの時間分散、
  `sigmaT`)** で、#57 判断3 で確定済み・対象外。
- `setanalysis.inc:234`/`:509`、`temporal_model.inc:135`、`cli.inc:1381`、
  `serve.cpp:1318`/`:1340` —— III 群 (推定量の入力)、対象外。
- `panel_rois.inc:50`、`canvas.inc:1424`、`temporal_model.inc:96`、
  `serve.cpp:1305`/`:1430`、`analyzer_stats.c:136`、`analyzer_noise.c:101`/`:117`
  —— I 群で**既に ddof=0**。裁定1 の統一先なので変更なし。

#### 留保 —— #3/#4 の分母は本当に n−1 か

`analyzer_prnu.c` の row/col FPN は**デトレンド残差**である
(`m1 − m2`、`m2` は `boxblur1d` をかけた低域)。つまり `Σd²/n` は「平均まわりの
分散」ではなく、平均は既にデトレンドが落としている。**boxblur が消費する自由度は
1 ではない**ので、素直に n−1 にするのは裁定2 の文言どおりではあるが、統計的に
正確な分母とは限らない。

実装時はこの2件について、**n−1 にした上でその留保を PR に書く**。ここを黙って
n−1 にすると、「不偏にした」という主張が実際より強くなる —— 本書がまさに
潰そうとしている種類の齟齬である。
