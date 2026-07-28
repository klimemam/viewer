# Stats Taxonomy — 「基本 stats」と「追加 measurement」の境界定義

機能リクエスト(例:「フレーム毎の stats を Excel に貼りたい」)を即座に分類するための
基準文書。判断基準はただ一つ: **その数値を出すのに何を入力する必要があるか**。
実装の所在は [manual.md](manual.md) / [analyzers.md](analyzers.md) /
[remote.md](remote.md)、コードは `core/main.cpp`・`core/serve.cpp`・`plugins/*.c`。

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
| min / max / mean / std / var | S1 | 1フレーム(+ROI) | sc | ROIs パネル、Inspector Statistics | 常時表示。sampling 上限 200k/ROI |
| percentile / entropy / finite ratio | S1 | 同上 | sc | plugin `stats/moments` | NaN/Inf 検出込み |
| histogram(CFA 4系列) | S1 | 同上 | cv | Histogram パネル | 表示レンジ 256bin、クリップ率付き |
| H/V projection + profile 統計 | S1 | 同上 | cv+sc | Projection パネル | σ(列平均)= 列 FPN の S1 近似 |
| POI / 行・列バンド画素値 | S1 | 1フレーム+座標 | sc | Annotations、`X`/`Y` キー | |
| noise floor(タイル中央値)/ SNR点 | S1 | 平坦め ROI | sc | plugin `noise/floor` | σ_t+FPN 混合。純時間ノイズではない |
| PRNU proxy / 行・列 FPN % / shading | S1 | 明るい flat 1枚 | sc | plugin `uniformity/prnu-fpn` | **温度ノイズ混入**と明記済み。EMVA 値ではない |
| sharpness(varlap / tenengrad) | S1 | demosaic 済 1枚 | sc | plugin `sharpness/gradient` | 相対値のみ(a.u.) |
| e-SFR / MTF50 | S1 | 斜めエッジ ROI | cv+sc | plugin `iso12233/e-sfr` | シーン内容(エッジ)は前提だが入力は1枚 |
| A−B 差分・画素比較 | S1+r | 1フレーム+**参照画像 B** | mp+sc | A/B compare(`\`) | B が参照データ。クラス上は +ref の最小例 |
| defect pixel 検出(閾値法) | S1 | 1フレーム+閾値 | sc+mp | — | 簡易版は S1。dark/flat 基準の正式版は +ref |
| **temporal noise σ_t** | **TN** | 同条件 ≥2 フレーム | sc | Temporal パネル / `MOP_TEMPORAL_STATS` | 画素毎時間 var の平均。stack の性質 |
| FPN(σ_fpn = 時間平均の空間σ) | TN | 同上 | sc | 同上 | 平坦でなければ絵柄込み(注記あり) |
| frame mean ドリフト / フリッカ | TN | 同上 | cv | Temporal パネルのグラフ | 横軸 frame、縦軸 ROI mean |
| **per-frame ROI 統計テーブル** | TN(中身は S1×N) | 同上 | cv/表 | server `MOP_FRAME_ROI_STATS`(plane 毎 mean/var 系列) | §3 参照。Excel 貼り付け要求の正体 |
| 時間平均フレーム(ノイズ低減画像) | TN | 同上 | mp | —(serve 内部では計算済み、出力なし) | EMVA 系 +ref の素材になる |
| blink / RTS 画素検出 | TN | 多め(≥50〜)のフレーム | sc+mp | — | 画素毎の時系列が要る。map 出力 |
| dark noise(EMVA) | TN+m | **dark stack** + 露光条件 | sc | Linearity の level-0 ルールが部分実装 | 「dark である」ことはメタ情報 |
| DSNU(EMVA 1288) | TN+r | dark stack の時間平均 → 空間σ | sc+mp | — | σ_fpn の dark 限定版。正式には基準条件指定 |
| PRNU(EMVA 1288) | TN+r | **flat stack + dark stack** の各時間平均 | sc | —(`emva1288/*` 予定) | 現 plugin は proxy と明記 |
| **linearity fit(感度・offset・LE max)** | **LM+m** | レベル別 stack ×3+ + **level 値と単位** | sc+cv | Linearity パネル | level は folder 名から auto、編集可 |
| conversion gain K(PTC) | LM | 同上(level 値自体は不要: σ_t² vs mean) | sc | Linearity パネル(K [DN/e-]) | |
| read noise | LM / TN+m | PTC 外挿、または **dark stack で実測** | sc | Linearity パネル(extrap. 明示) | dark 有無で信頼度が変わる設計済み |
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

## 4. リクエスト判定フロー

上から順に質問する。最初に Yes になった行がクラスと実装コストを決める。

| # | 質問 | Yes → クラス | 実装コスト帯 |
|---|---|---|---|
| 1 | 2枚目のフレームが要るか? | No → **S1** | 既存の数値の見せ方なら **UI-only**(TSV copy 等)。新統計なら **local compute**(plugin か host)。remote stack 全体に効かせるなら **MEASURE op**(`MOP_ANALYZER` 経由なら protocol 変更なし) |
| 2 | 同一条件の N フレームか? | **TN** | 常駐フレームだけなら local compute(`computeStackStats` 系)。**全フレーム対象なら必ず protocol MEASURE op**(remote stack は手元に全画素がない。processing policy `auto/server/local-fetch` に従う) |
| 3 | 条件(レベル)を変えた複数 stack か? | **LM** | Linearity パネルの拡張が第一候補。新パネルは最後の手段 |
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
