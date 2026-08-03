現行ドキュメント: [export-design.md](../export-design.md) の背景 — 1回で持ち出す動機と、却下した表の畳み方。

# Temporal export 設計 — 背景

「Temporal で出力するときに、テンポラルのデータと H/V Profile の Statistic を
両方出すようにして。今のやつは一回ないものとしてあるべき姿を考えて出す」への回答。
現行の
`copyPerFrameStats`(per-frame TSV)は**前提にしない** — 生き残る部分は §7 で
明示的に選び直す。

## 0. この出力は何のためにあるか

今は 1 が画面写し(スクリーンショット)でしか出せず、2 が per-frame TSV、3 が
Projection パネルの別ボタンで、**3回の操作と3つの断片**になる。同じ画像・同じ
ROI の測定なのだから、1回の Export が3つとも運ぶべきである。これが「両方出す」の
正体で、measure-ux.md の原則(出力はそれ単体で証拠になる)をここにも適用する。

## 1. 誰が読むか — 主読者は Excel 貼り付け、副読者は pandas

この codebase の既存動線は「TSV をクリップボードへ → Excel(資料)に貼る」
(Analysis の Copy table、Projection の Copy table)と「CSV をファイルへ」
(Export curves)。ユーザーの実際の行き先は**資料**である。よって:

## 2. 中心決定 — 「ラベル付き矩形の積み重ね」(sectioned rectangles)

§0 の3つの表は**軸が違う**: 1 と 3 は行 = side×plane、2 は行 = side×frame。
1つの矩形に畳む方法は2つしかなく、どちらも捨てる:

- frame 行に σ_t を持たせる → **カテゴリエラー**(σ_t は stack の属性。
  stats-taxonomy.md §3 と terminology.md の操作マトリクスが明文で禁止)。
- 全列の直積 + 空セル埋め → Excel でも pandas でも「何が何の値か」が列名から
  読めなくなる。空欄の海は矩形ではなくただの穴。

Excel は矩形を欲しがる — 各セクションが矩形である。人間はラベルを
欲しがる — `#` 見出しがそれである。pandas はどちらでも読める —
`comment='#'` で1セクション、または `#` 見出しで split。3読者全員が
既存の家風(`#` provenance 行つき TSV/CSV)のまま満足する。これが中心決定。
