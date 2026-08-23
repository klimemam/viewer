# Analyzers — 測定プラグイン一覧

命名規約: `category/name`。規格由来の測定は規格番号をカテゴリにする(例: `iso12233/e-sfr`)。
すべて ROI 対応で、複数 ROI があれば「ROI × 指標」の比較グリッドに出力される。
各アナライザは `method` 行に手法と制約を1行で明示する。
同梱アナライザはすべて **ABI v3 登録**で、`description`(前提条件1行)・
`version`(§version)・`headline`(見出しキー)を自己申告する
([abi-v3.md §1](abi-v3.md) の移行作業が済んだ)。
Measure メニューは `description` をツールチップに出す
(UX 設計は [measure-ux.md](../features/analysis/measure-ux.md))。
**V1/V2 の登録経路は永久に不変**で、サードパーティのビルド済み dll は
そのままロードでき続ける([abi-v3.md §2.1](abi-v3.md))。同梱分が V3 に
移ったことは、その経路が使われなくなったという意味ではない —— 経路の
実働確認は比較 fixture (`plugins/test/abi_v[23]_twin.c`) が
`--anaprov-selftest` で持っている。

## version — 何をもって上げるか

`version` は**その計算の名前**であって、ビルドの名前ではない。

- **上げるのは**、ある入力が違う結果ドキュメントを生みうるようになったとき
  だけ: キーが増えた/減った/改名された、単位が変わった、同じ画素に対する値が
  変わった、エラー文が変わった。推定量の定義を変えるもの(e-SFR の窓関数、
  ノイズフロアのタイル寸法…)は当然これに当たる。
- **上げないのは**、コメント・整形・登録経路・コンパイラ・コミット。
- **ビルド id を使わない**: viewer 本体の版は `cmake/gitversion.cmake` が
  出すコミットハッシュ(`<hash>` / `<hash>+local`)で、バイナリの出自として
  は正しいが version 欄には誤り。[abi-v3.md §10](abi-v3.md) の parity は
  version を**等値比較**するので、計算が1行も違わない2台がコミット違いだけで
  互いを拒否することになる —— parity が防ごうとしている事故の裏返しが起きる。
  逆に、誰も上げない定数は「違うのに一致」を作る。**上げ過ぎと上げなさすぎの
  両方が parity を壊す**、というのがこの欄の難しさである。
- 粒度は**記述子ごと**(判断record 1)。同梱5本はいずれも
  **`1.0.0`** —— これは「新しい」という主張ではなく、「version を初めて宣言した」
  という意味である。
- 機械的な歯止めは `--bundled-selftest` の B4: 各 version を、その版が宣言して
  いる**キー/単位の契約と並べて**固定してある。キーや単位が変わればテストが失敗し、
  version 行と同時に直せと言う。値の変化まではここでは掴めない(丸めは3つの
  コンパイラで一致しないので、可搬な期待値が書けない)ので、そちらは同 selftest
  の stdout を移行前後で diff する形で担保した。

検証: `--bundled-selftest` (B1–B10)。remote 側の実働は
`--rplugin-selftest` RP23–RP26(同梱アナライザが name+version 一致で peer で
走り、`+dirty` を付けた版は拒否される)。

## 出力キーの規約(単位・見出し)

- **単位はキー名が自己申告する**: `snr_db`(dB)、`*_pct`(%)、`mtf50 (cy/px)`、
  `edge_angle_deg`。ホストはこれを unit 列に展開する(`unitForAnalysisKey`)。
  **これは `emit_number` に単位のフィールドが無いからで**、キー名がこの経路での
  唯一の宣言だからです。宣言する場所がある側 —— adapter の `meta` —— では逆に
  キー名に単位を埋めず `{"value":…, "unit":…}` を使います
  ([input-adapters.md §4.3.1](../features/adapters/input-adapters.md))。**宣言できるなら宣言が勝つ。**
- 画像値系のキー(`.mean` `.std` `.noise` `.min/.max/.p1/.p50/.p99` `.var`)は
  ホスト共通の画素値単位 `[DN]`（`.var` は `DN^2`）。保存 dtype によって
  単位名を変えない
- 相対指標(`varlap` `tenengrad` `grad_mean`)は a.u.(絶対値に意味なし)
- **見出し数値**(そのアナライザの主目的の数値)は**プラグインが申告する**:
  ABI v3 記述子の `headline` 欄(チャンネル接頭辞を剥いだキー名1つ、NULL 可)。
  ホストは emit されたキーの接頭辞(`chN.` / `R.` / `Gr.` …)を剥いた形が
  これに一致した行をアクセント強調する。**宣言できるなら宣言が勝つ** ので、
  v3 記述子にホスト側の対応表は適用しない(申告なし = 見出しなし)。
  同梱5本の申告は `noise` / `prnu_pct` / `tenengrad` / `mtf50 (cy/px)` と
  `stats/moments` の**申告なし**で、これは移行前にホスト側の対応表が選んで
  いた行と**同じ行**である(`--bundled-selftest` B8 が、対応表が出す答えと
  パネルが実際にアクセントした行を突き合わせて守る)。
  V1/V2 記述子は従来どおりホスト側の対応表
  (`.noise` / `.prnu_pct` / `tenengrad` / `mtf50 (cy/px)`)で、これは
  永久に残る —— サードパーティの V1/V2 dll がロードでき続けるため
  ([abi-v3.md §3.2](abi-v3.md))

## Provenance — 誰が計算したか (#46)

ホストは登録時に「どのファイルがどのアナライザを登録したか」を帳簿に記録する
(`AnalyzerPluginInfo::file / path`。登録は dll のロード中に起こるので、その
瞬間のファイルが出自 — ABI には何も足さない)。**版は帳簿がホストから知る
ことができない唯一の欄**なので ABI v3 の記述子欄から合流する
(`AnalyzerPluginInfo::version`、[abi-v3.md §3.1/§11](abi-v3.md))。

- Analysis パネルの provenance 行は末尾に
  **`アナライザ名 <version> (ファイル名)`** を表示する。
- 行のホバー(ツールチップ)と Copy (TSV) / Export (CSV) の `# plugin:` 行は
  **`<version> フルパス`**。version 前置はパスに空白がありうるため
  (後置だとパスの一部と区別できない)。
- **V1/V2 記述子は version 欄を持たないので空のまま**で、行からは `<…>` ごと
  消える。ホストは**推測しない** —— dll のファイル版リソースも読まない
  (それは宣言ではないので)。空欄は「宣言なし」であって「不明」ではない。
  同梱5本はもう空欄ではない: `stats/moments <1.0.0> (analyzer_stats.dll)`。
- builtin の統計(Temporal エクスポート等)は従来どおり `app: viewer <版>`
  のみで、プラグインタグを着ない。

検証は `--anaprov-selftest` (AP1–AP27)。V2/V3 の同一計算を2つの dll で登録した
比較 fixture (`plugins/test/abi_v[23]_twin.c`) がこの表の両方の行を1つの
グリッドで確かめる。

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

**ROIs パネルの `std / mean [%]` とは別物**: あちらは ROI の σ/mean を素で出したもので、
ローパス除去をしないぶんシェーディングと絵柄を含みます。ここの `prnu_pct` は
9×9 ボックスのローパスを引いた**残差**の σ/mean なので、同じ ROI でも一致しません。

制約: **このアナライザは単一フレーム法のため prnu_pct に時間ノイズを含む**。
公開 ABI には現在 `psStackAnalyzerV3` があり、組み込みの Set Analysis には
dark-referenced DSNU/PRNU の直接推定と分離フィットが実装済みである。ただし、
それらを EMVA 1288 準拠値と呼ぶには参照規格との照合が残る。現行機能と残件は
[flat-field-stats.md](../features/analysis/flat-field-stats.md) を正典とする。
時間ノイズを下げたい場合は、Files で stack を右クリック > **Open frame average**
(時間平均を1枚の frame として開く — [manual.md §5.2c](../guides/manual.md))を使う方法が
ある: σ_t は元の 1/√n になる。dark 減算はしないため、EMVA 準拠とは表記しない。
CFA は Bayer=パリティ分離 / Quad=4px 周期サブサンプルで各チャンネル独立に処理。

## sharpness/gradient

フォーカス・解像感の相対指標。同一シーン・同一正規化での比較に使う(絶対値に意味はない)。

| キー | 内容 |
|---|---|
| `varlap` | 4近傍ラプラシアンの分散(古典的フォーカス尺度) |
| `tenengrad` | Sobel 勾配強度² の平均 |
| `grad_mean` | Sobel 勾配強度の平均 |

制約: CFA モザイクのままでは勾配が過大評価される(警告を emit)。demosaic 後に使うこと。

## iso12233/e-sfr

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

## 予定

- `iso14524/oecf` — グレーチャートのパッチ列 → OECF カーブ
- `emva1288/*` — 規格原文との式・前提条件の照合後に命名する候補。実装経路は
  組み込み Set Analysis または現行 `psStackAnalyzerV3` であり、未定義の
  `analyze_seq` を前提にしない
- `iso19567/dead-leaves` — テクスチャ SFR
