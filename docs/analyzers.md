# Analyzers — 測定プラグイン一覧

命名規約: `category/name`。規格由来の測定は規格番号をカテゴリにする(例: `iso12233/e-sfr`)。
すべて ROI 対応で、複数 ROI があれば「ROI × 指標」の比較グリッドに出力される。
各アナライザは `method` 行に手法と制約を1行で明示する。
同梱アナライザはすべて ABI v2 登録で、`description`(前提条件1行)を自己申告する。
Measure メニューはそれをツールチップに出す(UX 設計は [measure-ux.md](measure-ux.md))。

## 出力キーの規約(単位・見出し)

- **単位はキー名が自己申告する**: `snr_db`(dB)、`*_pct`(%)、`mtf50 (cy/px)`、
  `edge_angle_deg`。ホストはこれを unit 列に展開する(`unitForAnalysisKey`)。
  **これは `emit_number` に単位のフィールドが無いからで**、キー名がこの経路での
  唯一の宣言だからです。宣言する場所がある側 —— adapter の `meta` —— では逆に
  キー名に単位を埋めず `{"value":…, "unit":…}` を使います
  ([input-adapters.md §4.3.1](input-adapters.md))。**宣言できるなら宣言が勝つ。**
- 画像値系のキー(`.mean` `.std` `.noise` `.min/.max/.p1/.p50/.p99` `.var`)は
  **ファイルの単位**: ホストが dtype から決める(整数 → DN、float → dtype 名。
  float ファイルの物理単位はファイルに書かれていないので断定しない)
- 相対指標(`varlap` `tenengrad` `grad_mean`)は a.u.(絶対値に意味なし)
- **見出し数値**(そのアナライザの主目的の数値)はホスト側の対応表で
  アクセント強調される: `.noise` / `.prnu_pct` / `tenengrad` / `mtf50 (cy/px)`。
  ABI v3 でプラグイン申告に移す予定

## stats/moments

基本統計。任意の画像/ROI に使える。

| キー | 内容 |
|---|---|
| `chN.mean / var / std` | チャンネル毎の平均・分散・標準偏差(有限値のみ) |
| `chN.min / max / p1 / p50 / p99` | 範囲とパーセンタイル |
| `chN.entropy` | 黒点/白点レンジ 256bin のエントロピー (bit) |
| `finite ratio` | 有限値の割合(NaN/Inf 検出) |

## noise/floor

ノイズ解析。**平坦めの領域に ROI** を置くのが前提(テクスチャ混入には頑健)。

| キー | 内容 |
|---|---|
| `X.mean / std` | チャンネル毎(CFA 画像は R/Gr/Gb/B 別)の全体統計 |
| `X.noise` | **16×16 タイル std の中央値** = ノイズフロア推定。テクスチャのあるタイルは上位に外れるため中央値が純ノイズに漸近 |
| `X.snr_db` | 20·log10(mean / noise) |

制約: タイル内 16 サンプル未満のタイルは無視。

## uniformity/prnu-fpn

フラットフィールド非均一性。**明るい(未飽和の)フラット画像**が前提。

| キー | 内容 |
|---|---|
| `X.mean` | 信号レベル |
| `X.prnu_pct` | ハイパス残差(9×9 ボックスでシェーディング除去)の σ/μ×100。画素ゲインばらつき |
| `X.row_fpn_pct` / `X.col_fpn_pct` | 行/列平均をデトレンドした残差の σ/μ×100 = 横/縦バンディング |
| `X.shading_pct` | ローパス面の peak-to-peak / μ×100 |

**ROIs パネルの `PRNU [σ %]` とは別物**: あちらは ROI の σ/mean を素で出したもので、
ローパス除去をしないぶんシェーディングと絵柄を含みます。ここの `prnu_pct` は
9×9 ボックスのローパスを引いた**残差**の σ/mean なので、同じ ROI でも一致しません。

制約: **単一フレーム法のため prnu_pct は温度ノイズを含む**。EMVA 1288 厳密値には
複数フレーム平均が必要(マルチフレーム ABI 導入後に `emva1288/` 系として実装予定)。
CFA は Bayer=パリティ分離 / Quad=4px 周期サブサンプルで各チャンネル独立に処理。

## sharpness/gradient

フォーカス・解像感の相対指標。同一シーン・同一正規化での比較に使う(絶対値に意味はない)。

| キー | 内容 |
|---|---|
| `varlap` | 4近傍ラプラシアンの分散(古典的フォーカス尺度) |
| `tenengrad` | Sobel 勾配強度² の平均 |
| `grad_mean` | Sobel 勾配強度の平均 |

制約: CFA モザイクのままでは勾配が過大評価される(警告を emit)。demosaic 後に使うこと。

## iso12233/e-sfr (ABI v2)

スランテッドエッジ SFR。**傾き 5〜40° のエッジ1本を含む ROI** が前提。曲線は emit_series
で出力され、Analysis パネル下部の折れ線プロットに表示される(複数 ROI は色分け重畳)。

| 出力 | 内容 |
|---|---|
| series `sfr` | SFR カーブ(0〜1.0 cycles/px、0.01 刻み) |
| `mtf50 / mtf20 (cy/px)` | 50%/20% コントラスト周波数(線形補間) |
| `sfr@nyquist` | f=0.5 cy/px での SFR |
| `edge_angle_deg` / `lines_used` | エッジ角と使用ライン数(妥当性確認用) |

手法: ライン毎の微分重心 → エッジ直線フィット → 4x オーバーサンプリング ESF →
中心差分 LSF → Hamming 窓 → DTFT。**簡易実装**(微分の sinc 補正なし)のため
高周波側に僅かな負バイアスあり。画像間の相対比較には十分。
制約: CFA モザイクは拒否(先に demosaic)。エッジ傾き <2° はビニングアーティファクト警告。

## 予定 (ABI v2 以降)

- `iso14524/oecf` — グレーチャートのパッチ列 → OECF カーブ
- `emva1288/*` — 複数フレームによる温度/固定パターン分離(analyze_seq 前提)
- `iso19567/dead-leaves` — テクスチャ SFR
