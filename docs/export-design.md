# Temporal export 設計 — テンポラルデータ + H/V profile statistics を1回で持ち出す

本書が仕様で、実装はここに書いた形と理由に従う。

## 0. この出力は何のためにあるか

Temporal パネルが答える質問は「この stack のノイズはいくつで、時間方向に何が
起きているか」。報告書に貼るとき、その答えは3つの表でできている:

1. **stack の性質**(σ_t / σ_fpn / σ_tot、plane 毎)— スカラ表
2. **時間方向の変化**(frame 毎の mean/σ — ドリフト・フリッカの素材)— frame 軸の表
3. **空間方向の構造**(H/V profile statistics — 行/列 FPN、シェーディング)— plane 毎のスカラ表

## 1. 誰が読むか — 主読者は Excel 貼り付け、副読者は pandas

- **主**: クリップボード TSV。Excel に貼って列が揃い、ラベル行が読めること。
- **副**: CSV ファイル(pfd の save dialog、Export curves と同じ導線)。
  pandas で `#` をコメントとして読み飛ばせば各セクションがそのまま読めること。
- 報告書の付録(appendix)はこの2つの派生であり、独自形式は作らない。

## 2. 中心決定 — 「ラベル付き矩形の積み重ね」(sectioned rectangles)

よって出力は **`#` 行で見出しを付けたセクションの列**とし、**各セクションは
それ自体が完全な矩形**(ヘッダ行1 + データ行 N、行によって列数が変わらない)
とする。

セクションの順序は **summary → profile statistics → per-frame**。スカラ表
2枚を先頭に置き、行数が伸びる per-frame を末尾に置く(貼った先で bulk が
summary を画面外へ押し出さない)。

## 3. セクション仕様

すべての数値列はヘッダに単位を持つ(`sigma_t [DN]`、`sigma_col [%]`)。
数値は `%.9g`、小数点は `.`、ASCII のみ(σ と書かず sigma と書く —
serve.cpp の key と同じ)。表示側の decimals 設定は反映しない
(Projection の Copy と同じ理由: 表示は走査のため、出力は再現のため)。

### 3.1 `# == temporal summary ==` — stack statistics

行 = side × plane(+ pooled 行は per-frame の節を参照 — この表には無い。
plane を持たない側は `all` 1行)。列:

    side  ch  n  N  sigma_t [DN]  sigma_fpn [DN]  sigma_tot [DN]  source

- `n`/`N` = 使ったフレーム数 / stack の期待フレーム数(部分ロードの正直さ。
  N が不明なら n と同値)。
- `source` = `local` | `server <host>`。パネルと同じ優先順(server 集計が
  あればそれ、無ければローカル)で、**パネルの state struct
  (`AbTemporal` ← `TemporalState`/`ServerTemporal`)から読む。再計算しない**。
- 数値の定義はパネルが計算しているものをそのまま述べる(発明しない):
  σ_t = per-pixel 時間分散の平均の平方根、σ_fpn = 時間平均の空間σ、
  σ_tot = 両者の quadrature。**σ_fpn に −σ_t²/N 補正は掛かっていない**
  (ローカルも server も掛けていない)。ROI が平坦でなければ絵柄込み。
  この注意はセクション直下の `#` 注記として出力自体が運ぶ。
- ローカル値はサンプリング格子(≦40k サンプル、CFA セル単位)上の値である
  ことも `#` 注記で言う。per-frame 表(全画素走査)と mean が微差を持つ理由を
  出力自身が説明しないと、読者が「どちらかが間違い」と読む。

### 3.2 `# == H/V profile statistics ==` — Projection パネルの数値

行 = side × plane(+ `all` 行は **Projection パネルで有効なときだけ**、
plane の後ろに置き `all` と名乗る — pooled は別測定であり5番目の plane では
ない)。列は Projection の Copy table (TSV) と同語彙:

    side  ch  mean [DN]  sigma_frame [DN]  sigma_row [DN]  sigma_col [DN]  sigma_frame [%]  sigma_row [%]  sigma_col [%]  pp_frame [DN]  pp_row [DN]  pp_col [DN]

- 値は `App::ProjState`(A/B/slot 毎)の `fStat/vStat/hStat` を**そのまま**読む。
  reduce モード(mean/max/min)はセクション見出しに明記。
- 行の順序は side-major 固定(A の plane 全部 → B の…)。画面の
  `order` トグルは表示の都合であり、ファイルは常に同じ順で出る。

### 3.3 `# == per-frame statistics ==` — S1×N(ドリフト・フリッカの素材)

**side 毎に1ブロック**(`# -- side A: <stack> --` 見出し + 矩形)。理由:
列が plane 毎に立つので、A が 4-plane で B が 1-plane のとき1つの矩形に
できない。side 内は完全な矩形。列(plane 毎に4列):

    frame  file  mean_<p> [DN]  sigma_<p> [DN]  sigma_col_<p> [%]  sigma_row_<p> [%]

- 中身は現行 `copyPerFrameStats` の計算(全画素走査、per-frame S1 のみ、
  σ_t 列は**持たない**)。列名だけ §3.2 と同語彙に揃える(旧
  `Hproj sigma [%]` → `sigma_col [%]`: H profile = 列平均、そのσは列 FPN)。
- 常駐フレームのみ。欠けは行を偽造せず `#` 注記で「resident n of N」と言う。
- `path` 列は落とす(stack の path は provenance が1回言う。行毎の反復は
  Excel の横幅を食うだけで情報が増えない)。`file` 列は残す — フレームの
  同一性は行の性質である。

## 4. Provenance — 先頭の `#` ブロック

数値は出所なしに報告書へ入れない(measure-ux.md)。先頭に:

    # temporal + H/V profile statistics
    # app: viewer 0.1  |  generated: <YYYY-MM-DD HH:MM:SS>
    # sides: A, B
    # A: <stack名>  |  <W>x<H> <dtype>  |  CFA: RGGB (4 planes)|none  |  frames n=5/5  |  temporal: local|server <host>
    # A temporal region: whole image (0,0 80x64) | ROI (x,y) WxH
    # A profile region:  ROI (x,y) WxH  |  reduce: mean

を side 毎に出す。**temporal の測定領域と projection の測定領域は別々に
言う**。server temporal は ROI を送らない(全フレーム全面)ので、ROI 選択中は
2つの region が**実際に食い違う** — 過去に「server 全面なのにラベルは ROI」を
やった箇所であり、出力はこれを塗り潰さず、食い違う side には

    # NOTE: side A: temporal and profile were measured over DIFFERENT regions (see above)

を必ず添える。dark/level 等のメタは series が持つ(terminology.md)。stack が
series に属するときのみ `# A series: <name>, <param> = <value> <unit>` を1行
添える(無ければ出さない — 空欄の約束はしない)。

## 5. Sides — A だけでなく画面にいる全員

compare 中の Temporal パネルは A/B(+ C,D,… slot)を描く。出力が A だけを
運ぶと画面と食い違う。**Projection の Copy と同じ規則**を採る: 見えている
side 全部、`side` 列で区別、compare off なら A の1側(それでも `side` 列は
置く — 列集合が状態で変わらないことが矩形の価値)。stack でない side は
数値行を持たず、provenance に `A: not a stack (single frame)` と理由を言う。
delta 列は**持たない**(delta は2項の画面上の演算。ファイルには両辺の生値が
あり、引き算は読者の1セルで済む — 引けない条件の警告体系を出力に複製しない)。

## 6. 2つの出口、1つのビルダー

- ボタンは Temporal パネルに **`Copy (TSV)` / `Save (CSV)...`** の2つ。
  1つのビルダーがセル列を作り、区切り文字だけ差し替える。**内容は同一**。
- TSV: セル内の tab は space に潰す(Windows のパスに tab は来ないが、規則と
  して置く)。クリップボードへ。
- CSV: RFC 4180 最小 — `,` `"` 改行を含むセルだけ `"` で括り `"` を二重化。
  `pfd::save_file`(Export curves と同じ pump)でファイルへ。既定名
  `temporal_stats.csv`。
- 小数点は常に `.`、桁区切りなし、エンコーディングは ASCII。ロケールに
  従わない(`snprintf` の C ロケール前提をそのまま契約にする)。

## 7. `copyPerFrameStats` から何が生き残るか

- **生き残る**: per-frame S1 行の計算そのもの(全画素走査、plane 分離、
  NaN 除外、ROI 規約、per-frame の行/列プロファイル CV)。§3.3 の中身は
  この計算の移設であり、`--framestats-selftest` の「numpy で全数値再現可能」
  という約束も per-frame セクションがそのまま引き継ぐ。
- **死ぬ**: 単独ボタンとしての存在(統合 Export に置換)、`path` 列、
  `Hproj/Vproj` という列名、provenance 無しで裸の表が出ること。
- Projection パネルの Copy table (TSV) は**そのまま残す**。あれは「その表を
  その形で」持ち出す表のコピーであり、こちらは「Temporal の測定一式」の
  持ち出し。共有すべきは値の**出所**(同じ ProjState)と**語彙**(同じ列名)
  であり、実装の関数まで1本化して2つのボタンの寿命を縛り合わせる利益がない。
  (projForEachRow はパネル内 lambda であり、walk の規則 — side-major、
  all は最後 — を仕様として共有する。)

## 8. per-frame x 軸 — フレーム番号を物理量に置き換える

stack のフレームは大抵、物理量(経過時間・露光・温度)に対応する。Temporal の
per-frame チャートに **カンマ / 空白 / タブ区切りの数値リストを貼り付けて
x 軸にできる**ようにする。設計は家風が決める:

- **軸は「単位を持つ量」**。入力は NAME + UNIT + 値リストの3点セット
  (series が stack レベルのパラメータに持つものの per-frame 版、
  docs/series-plan.md と同じ語彙)。**単位は決して既定しない** — name/unit の
  無い Apply は拒否する。
- **パース**: `,`・空白・タブ・改行すべて区切り(Excel 列とスクリプト出力の
  両方が来る)。非数値トークンは**位置つきエラー**で列挙し、黙って飛ばさない。
  読めない値は 0 ではなく「不成立」(canon)。
- **個数の正直さ**: リスト長 ≠ フレーム数なら「12 values for 24 frames」と
  **両方の数字**を言って適用を拒否。部分ロード stack は **expected total** と
  比較し、そう言う。値 i は seqIndex i に付く。非単調は正常(温度は往復する)
  — 折れ線はフレーム順に辿るだけ。
- **スコープ**: stack 毎(`SeqInfo::axisName/axisUnit/axisVals`)。セッションに
  追加キー `seqaxisvals/seqaxisunit/seqaxisname` で保存(古い viewer は
  読み飛ばす — 形式の常道)。A/B/slot はそれぞれ自分の軸を持つ。**重ねる
  チャートの x 軸は1本**: 曲線を持つ全 slot が同じ量(name+unit 一致。値は
  違ってよい — それが重ねる意味)を持つときだけ使い、そうでなければフレーム
  番号に落として理由を言う。
- **解決は1関数** `frameAxisOf(seqId)`: チャートも export も selftest も
  ここから読む。設定済みでもフレーム数と合わなくなった軸は「なぜ使わないか」
  つきでフレーム番号に落ちる。
- **export が運ぶ**(§3.3 に追記): per-frame セクションの `frame` 列の隣に
  `<name> [<unit>]` 列として乗る(軸が有効なときだけ)。`frame` 列は残す —
  マッピングの主キーはフレーム番号である。
- **UI**: チャート近くの `x axis...` ボタン → popup(name / unit / 値の
  multiline、Apply=検証、Clear=フレーム番号へ)。チャートの x ラベルは有効時
  `<name> (<unit>)`、無効時 `frame number (index in sequence)`。

## 9. 検証 — `--export-tsv-selftest <dir>`

文字列に対して assert する(この機体は GL を screenshot できない):
provenance 行のフィールド、セクションの並びと各矩形の列数、CFA stack での
plane 行 R/Gr/Gb/B、ヘッダの単位、部分 stack での n=X/Y、**数値がパネルの
state struct と同じ文字列に整形されること**(再導出ではなく同一 struct 読み)、
temporal/profile の region 食い違いが NOTE になること、CSV の quoting。
per-frame x 軸: 3種の区切りのパース、位置つきエラー、個数不一致の拒否文言
(両方の数字)、単位必須、`frameAxisOf` が適用値を返すこと、export 列、
セッション往復。壊した時に落ちることも証明する(region ラベル偽装、plane 欠落)。

経緯と検討: [.background/export-design.md](.background/export-design.md)
