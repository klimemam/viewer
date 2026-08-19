# docs 再構成 — 設計判断 (issue #55、handoff §7 への回答)

判断: Fable。実施日 2026-08-17。材料: [docs-restructure-handoff.md](docs-restructure-handoff.md)
(codex 案)、[docs-split-review.md](docs-split-review.md) (旧分割案の全件レビュー、2026-08-13)、
`docs/tasks.csv`、現行 docs 全 44 本、README.md の docs 表 (読むだけ — README は合図まで触らない)。

本書の数字は**すべて 2026-08-17 に数え直した**。docs-split-review の数字を引くときは
その旨を書く。再現手順は付録。

---

## 0. 結論 (6 判断の答え)

| # | handoff §7 の問い | 答え |
|---|---|---|
| 1 | 「全体 / 機能」主軸 + 機能内フェーズ分割 | **妥当。採用。** ただし修正 3 点: (a) `project/` ディレクトリは**作らず docs/ 直下を project 層とする** (b) フェーズは**分類規則**であってファイル定型ではない — 機能ごとの必須ファイルは `README.md` 1 本だけ (c) Results は「検証 run 記録」と「機能内着地台帳 (compare-n §11 型)」の 2 形態を公認 |
| 2 | `project(直下)` と `features/*/design` の境界 | **「書き換えたとき、影響が 1 機能の検証に閉じるか」で切る。** 機能間の契約・語彙・データモデルを定めるものは直下 (terminology / analysis-layers / reference-design)。推定量や UI の仕様は、被参照がどれだけ広くても機能側 (flat-field-stats は features/analysis)。迷ったら features に置き直下から参照 |
| 3 | `reference/` を横断領域に置くか | **置く。ただし基準は「消費者がこのリポジトリの外にいる契約」**(プラグイン作者・リーダ作者)。v1 の中身は abi-v3.md と analyzers.md の 2 本のみ。input-adapters §4 の契約抽出は**第一波ではやらない** (C++ 44 箇所が §番号で引く) |
| 4 | Results の単位 | **検証 run** (= 1 コミット × 1 機械 × 1 マトリクス実施)。`verification/results/<YYYYMMDD>-<何を>.md`、冒頭に commit / 機械 / 走らせた物を必須記載。日付は識別子であって単位ではない。リリース単位は不成立 (この repo にリリースが無い — CI は merge ごとに binaries を更新)、PR 単位も不成立 (1 PR が複数 run を運んだ実例: verify-probes が verify-functional D 節と verify-ui E 節を同時更新) |
| 5 | 旧パス案内ファイルの残存期間 | **期限ではなく条件 3 つ**: (1) リンク検査で旧パスの被参照が 0 (対応済み行・background 内の記録は数えない — 歴史は書き換えない) (2) README.md の一括更新 (合図) が済んでいる — README が今引く 11 本の stub はそれより前に消せない (3) 旧→新対応表は docs/README.md に**恒久残置** (stub が消えても歴史側の参照が解決できる) |
| 6 | docs-split-draft を rebase するか作り直すか | **作り直し + 正しい分割線の移植。両推奨に賛成。** 今日の実測で main は原案の **587 コミット先** (2026-08-13 時点の 564 [docs-split-review §0] からさらに 23)。しかも rebase の到達点である `.background/` フラット構造そのものを本案が廃止する — conflict 15 件を解いた先が捨てる形。移植する資産は 3 つ (§2-判断6) |

---

## 1. 地面 — この判断が立っている実測 (2026-08-17)

| 事実 | 値 | 出どころ |
|---|---|---|
| docs/ の Markdown | 直下 44 本 + review-lenses/ 1 本 | ls |
| tasks.csv | 251 行: 対応済み 206 / 残課題 23 + 残課題【Fable】13 / レビュー必要項目 7 / 暫定 2 / 進行中 0 | csv パース |
| C++/tools からの docs 引用・上位 | analysis-layers **67** / terminology **64** / flat-field-stats **49** / input-adapters **44** / abi-v3 **28** | grep (付録) |
| main.cpp 単体の docs 引用 | 17 行 (docs-split-review 付録と同数のまま) | grep |
| 板の**参照列**が引く docs・上位 | verify-matrix **14** (うち凍結 12 / live 2) / input-adapters **13** / browse-topbar-design **8** / manual・terminology・reference-design・watch-design 各 **7** | csv パース、参照列のみ |
| docs 内の相対 Markdown リンク | **122 本、切れ 0** | スクリプト (§6) |
| **存在しないのに引かれている** docs パス | `media-formats.md` (branch native-media。**C++ 3 箇所**: rawread.h:27, media.inc:963 ほか + 板参照列 3) / `python-plugins.md` (branch plugin-python-study。板参照列 2) / `remote-reader-design.md` (**未作成の成果物** — 板 240 行、#180 裁定 B、Fable 実行中) | §6 の検査を先行実施 |
| tasks.csv の消費者 | tools/board.py:18 が `docs/tasks.csv` を直読み | 読んだ |
| CI と docs の結合 | build.yml:107 が `docs/diagnostics/auto/` へ成果物を書く | 読んだ |
| 検証の正典の在処 | テスト一覧の正典は CMakeLists (run_selftests.sh 冒頭が明記「there is no second copy」) / verify-matrix の門の列は selftest fmtgate F4 が保つ (verify-matrix §10) | 読んだ |

板の参照列には `docs/` 前置きの無い素の引用もある (行 103 の「compare-n.md」)。
リンク検査 (§6) は両形を正規化して数える。

---

## 2. 各判断の根拠

### 判断1 — 「全体 / 機能」主軸: 妥当。ただし project/ は作らない

**主軸そのものの根拠 (この repo の実情で)。**

- **板は機能単位で docs を引く。** 参照列の集まり方 (browse-topbar-design 8 行、
  watch-design 7 行、input-adapters 13 行) は課題が機能に属することの実測で、機能主軸なら
  1 つの課題の参照が 1 ディレクトリに閉じる。フェーズ主軸だと compare 1 機能を理解するのに
  requirements/ → design/ → verification/ → results/ の 4 ディレクトリを往復する — handoff §2 の
  懸念はこの repo で実際に起きる。
- **1 文書の中にフェーズが共存している現行文化と衝突しない。** compare-n.md は §1–9 指針 /
  §10 ユーザー判断 / §11 着地台帳 / §12 仕様、と 1 本の中にフェーズが積まれ、C++ 8 箇所が
  §番号で引く (compare.inc:144 → §12、panel_rois.inc:84 → §10 ほか)。フェーズ主軸は
  この形を禁止し、§番号参照を全部壊す。機能主軸 + 「フェーズは分類規則」なら壊さない。

**修正 (a): docs/ 直下 = project 層。`project/` ディレクトリを作らない。**
C++ 引用の上位 2 本 (analysis-layers 67 + terminology 64 = **131 箇所**) と
reference-design (16 箇所) はどれも project 層である。codex 案のとおり `project/` へ動かすと、
この repo で最も引かれているパスを動かすことになる — docs-split-review §5 の教訓 2
(「被参照の全数を先に数える」。flat-field-stats 92 箇所の事故の型) にまっすぐ抵触する。
直下残置なら**この 3 本は 1 箇所も更新せずに済む**。分類の可視性は失われない:
「直下 = 製品全体の正典」「features/ = 機能」「background/ = 経緯」はディレクトリ一覧だけで
読める。これはユーザーが docs-split で示した規準「直下 = 今それを理解・変更するのに
要るもの」の素直な延長でもある。
**再訪条件**: 直下の project 文書が 10 本を超える、または requirements 系の文書群が
実際に生まれたとき、`project/` の新設を再議する。

**修正 (b): 機能ごとの必須ファイルは README.md だけ。** codex 案の
requirements/design/verification/decisions 定型を課さない。理由:

- **判断 record は所有文書の中に置くのが現行文化** (watch-design §9、reference-design §9、
  flat-field-stats)。decisions.md へ切り出すと、docs-split-review §5 教訓 1
  「再訪条件は現在の縛り — 設計から切り離さない」に反する。切り出しは、機能が
  文書横断の裁定を溜め始めたときだけ。
- **verification.md を機械的に作ると検証一覧の二冊目になる。** compare の検証は
  selftest N 群 / R 群 / T 群で、一覧の正典は CMakeLists (run_selftests.sh 冒頭)。
  feature README は**テスト群の名前を指す**に留める。
- 空の定型ファイルは「正典が二つ」の温床 (handoff §9 の完了条件そのものに反する)。

**修正 (c): Results の 2 形態** — 判断 4 で述べる。

### 判断2 — 直下 (project) と features/*/design の境界

**定義: その文書を書き換えたとき、再検証が 1 機能に閉じるなら features。
複数機能の検証に波及する、または機能間の契約・語彙・データモデルを定めるなら直下。**
被参照の「広さ」を基準にしないこと — それは実装の散らばりであって文書の管轄ではない。

適用 (handoff §5 が裁定を求めた件を含む):

- **analysis-layers.md → 直下 (project)。** Frame/Stack/Series/AnalysisSet の 4 層と
  アナライザ 4 種は全パネル・プラグイン・アダプタが従う語彙で、変更は全機能の検証に波及する。
  C++ 67 箇所の引き元も core/app・core/ui・selftest に横断している。
- **flat-field-stats.md → features/analysis。** σ_fpn/DSNU/PRNU の推定量は解析機能の仕様。
  C++ 49 箇所と広いが、引き元 (setanalysis, detrend, panel_temporal, …) はどれも
  「解析を実装している場所」であり、書き換えて再検証するのは解析だけ。
- **verify-matrix.md → verification/ (project 層の下)。** 形式 13 × 戸 9 × 操作 7 は
  adapters/browse/remote/media を跨ぐ — 定義上 project。
- 迷ったら features に置き、直下の文書からリンクする (正典は一つ、の側に倒す)。

**再訪条件**: この規則で 2 回続けて置き場所に迷った実例が出たら、規則を書き直す。

### 判断3 — reference/ は横断に置く。ただし狭く

**基準: 消費者がこのリポジトリの外にいる契約。** abi-v3.md の読者はプラグイン作者、
analyzers.md は出荷プラグインの目録 (実装との一致必須)。この 2 本だけで開始する。

**入れないもの**: input-adapters §4 (リーダ契約) の抽出は第一波ではやらない。
契約部 (§4) と「なぜこの契約か」(§1–3) が 1 本に居るのが現行の形で、C++ 44 箇所が
§4.x を番号で引く (main.cpp:28 「§4」ほか)。切り出しは §番号の張り替え 44 箇所と
セットでないと docs-split-review §4-A の ⚠ (ファイルは在るが節が空を指す) を量産する。
さらに §4.13.1 は #180 裁定 B の作業 (remote-reader-design、板 240 行、実行中) の根拠で、
**進行中の設計の足元は動かさない**。
**再訪条件**: remote-reader-design.md 着地後、adapters 波 (§5 の波 3) で
`reference/reader-contract.md` 抽出を §参照更新とセットで再議。

### 判断4 — Results の単位は「検証 run」。ただし 2 形態

**形態 1: run 記録** — `verification/results/<YYYYMMDD>-<何を>.md`。
1 run = 1 コミット × 1 機械 × 1 マトリクス。根拠はすべて現行の実物:

- verify-functional は「結論 (2026-08-03 実施)」という **run の塊**で書かれている。
  同 D 節には 2026-08-04 の別 run (verify-probes) が**追記**されている — run が単位である
  ことの実証。
- verify-ui は冒頭が「この機械でできること・できないこと」— **機械が run の属性**である
  ことの実証。日付だけでは同日 2 機械で衝突する。
- docs/diagnostics/ は既に日付名の証跡置き場 (`abstats-cfa-bayer-20260804.txt` ほか)。
  **diagnostics/ は動かさない** (build.yml:107 が auto/ へ書く — CI の契約)。results/ は
  そこへの参照で証跡を指す。
- リリース単位は不成立: リリースという概念がこの repo に無い。PR 単位も不成立:
  「文書のみ PR」があり (verify-matrix §9 が自己申告)、逆に 1 PR が複数表を更新した実例がある。

**形態 2: 機能内着地台帳** — compare-n §11 型 (「済 (2026-08-04, branch compare-n)」を
日付 + branch/PR 付きで**追記**する節)。これは docs-split-review §7-2 が裁定を求めた
「実装から返ってきたもの」の正式な置き場である。規則:
**本文 (仕様) に勝っている間は現行文書内の台帳に置く。本文へ吸収された時点で background へ。**
run 記録との違いは、台帳が「何が着地したか」(事実の集積) で、run 記録が
「ある時点で検証がどう出たか」(証跡) であること。

verify-matrix.md は**どちらでもない**ことに注意 — セルが根拠キー ([T:]/[C:]/[P]) を運ぶ
生きた表で、門の列は fmtgate F4 が保つ (§10)。仕様と結果に**割らない**。
G 節 (落穂の発見録) だけが台帳の型で、閉じた G は将来 background 候補。

### 判断5 — 案内ファイル (stub) は条件で消す。対応表は恒久

期限 (例: 3 ヶ月) にしない理由: この repo の旧パス参照は**書き換えないと決めている場所**に
残り続ける。板の対応済み 206 行の参照列は裁定時点の記録で、書き換えは歴史の改変
(Results「過去の実行結果を書き換えない」と同じ規律)。だから消せる条件は時間ではなく:

1. **リンク検査 (§6) が旧パスの被参照 0 を報告する。** ただし対応済み行・background 内の
   記録・git 履歴上の参照は数えない — それらは (3) の対応表が解決する。
2. **README.md の一括更新が済んでいる。** README は合図まで触らない規約なので、README が
   今引く 11 本 (terminology, analysis-layers, npz-design, input-adapters, remote, abi-v3,
   analyzers, manual, flat-field-stats, reader-analysisset, stats-taxonomy) の stub は
   合図より前に消してはならない。
3. **旧→新対応表を docs/README.md に恒久残置。** stub は消えても対応表は消さない。
   凍結された行 (板・background・issue 本文) が旧パスで引き続けても、1 ホップで解決できる。

stub の形式は 3 行固定 (§7 規則 1)。検査が stub 形式を機械判定する (§6 宣言 4) ので、
stub に本文が生えて正典が割れることはない。

### 判断6 — 作り直し + 移植。rebase 反対 (両推奨に賛成)

docs-split-review §6 の結論に今日の事実を 2 つ足す:

1. **587 コミット差** (今日実測。2026-08-13 の 564 [docs-split-review §0] から 23 増)。
   差は開く一方で、conflict 15 + 無警告 auto-merge 3 (同 §4-E) は好転していない。
2. **rebase の到達点が死んでいる。** 原案は `.background/` フラット 1 段の構造へ着地する。
   本案は features/ 主軸 + 非隠し background/ で、その構造自体を廃止する。conflict を
   全部解いても、直後に全ファイルをもう一度動かすことになる。

**移植する資産 3 つ** (docs-split-review §0 が「捨てるのは惜しい」と判定したもの):

- 規約: 背景側冒頭「現行ドキュメント: X の背景 — 要旨」/ 現行末尾「経緯と検討: […]」。
- 検証表の**列分割** (手順・期待 = 現行 / 実施結果・判定 = 背景) — §3 で全セル現存を
  項番単位で確認済みの型。verification/ + results/ にそのまま使う。
- **24 塊中 17 の ○ 判定** (§2 の表) — 分割線の答え合わせ済みの台帳。本書 §4 の
  マッピング表で判定番号を再利用する。

**避ける誤り 3 系統** (同 §2 で原案自身の判定ミスとされたもの): 未決リストを背景に
落とす (browse-topbar §5、watch-design 4 項) / 機構の「どうやって」を背景に落とす
(series-plan §5) / 被参照の節を参照ごと数えずに動かす (§10.8)。

---

## 3. 修正版ツリー (codex 案からの差分を明記)

```text
docs/
├─ README.md                  # 新設: ポータル + 正典宣言 + 旧→新対応表 (恒久)
├─ terminology.md             # 据え置き (C++ 64) — project 層は直下          [差分1]
├─ analysis-layers.md         # 据え置き (C++ 67) — 判断2 の裁定: project
├─ reference-design.md        # 据え置き (C++ 16) — frame 共有 = データモデル
├─ tasks.csv                  # 据え置き (board.py:18 が直読み)               [差分2]
├─ features/
│  ├─ compare/                # README.md + compare-n.md + compare-n-design.md
│  │                          #   (+ ab-stats-plan.md を波2で)   ← 試験移行 (§5)
│  ├─ browse/                 # browse-as-file-manager / browse-topbar-design (+mockup.html)
│  │                          #   / browse-extract-design / picker-ux
│  ├─ analysis/               # flat-field-stats / stats-taxonomy / measure-ux
│  ├─ histogram/              # histogram-select-design
│  ├─ adapters/               # input-adapters / npz-design / reader-analysisset
│  ├─ remote/                 # remote / remote-headerless-design
│  │                          #   (+ remote-reader-design — 板240の成果物はここに生まれる)
│  ├─ export/                 # export-design
│  ├─ watch/                  # watch-design
│  ├─ settings/               # settings-inventory / preferences-panel-design
│  ├─ media/                  # media-support / video-support (+ media-formats 上陸後)
│  └─ theme/                  # imgui_modern_design
├─ verification/              # project 層の検証                              [差分4]
│  ├─ matrix.md               # 現 verify-matrix.md — 生きた表のまま割らない
│  ├─ functional.md / ui.md   # 手順・期待の列 (列分割は draft から移植)
│  └─ results/                # <YYYYMMDD>-<何を>.md (判断4)
├─ guides/                    # manual / startup
├─ reference/                 # abi-v3 / analyzers — repo 外の消費者の契約    [差分5]
├─ background/                # 非隠し (draft の .background を廃止)          [差分7]
│  ├─ project/  <feature>/  verification/  reviews/  documentation/
├─ img/                       # 据え置き
└─ diagnostics/               # 据え置き (build.yml:107 の書き先 — CI の契約)
```

**codex 案 (handoff §2) からの差分一覧**:

| # | 差分 | 根拠 |
|---|---|---|
| 1 | `project/` を作らない。直下 = project 層 | C++ 131 箇所 (analysis-layers 67 + terminology 64) の移動を消す。判断1 |
| 2 | `governance/` を作らない。tasks.csv 据え置き | board.py:18 の直読み。文書ポリシーは docs/README.md の 1 節で足りる。todo-open の統合先も「板そのもの」であり新領域は要らない |
| 3 | features/* の定型 4 ファイルを課さない。必須は README.md のみ。ファイル名も定型に改名しない (compare-n.md は compare-n.md のまま) | 判断1-(b)。§番号・ファイル名で引く参照 (C++ 8 + docs + 板) を壊さない。役割はREADME が宣言する |
| 4 | `project/verification/specification.md` への統合をしない。matrix / functional / ui の 3 本並置 | verify-matrix は fmtgate F4 が保つ生きた表 (§10)。スナップショット仕様に畳むと根拠キーの設計が死ぬ |
| 5 | `reference/reader-contract.md` の抽出を第一波から外す | input-adapters §4 を C++ 44 箇所が引き、§4.13.1 は #180 の進行中設計の根拠。判断3 |
| 6 | `project/requirements.md` / `quality-requirements.md` を作らない | 対応する中身が現行 docs に存在しない。空の定型は正典二重化の温床 |
| 7 | background/ は非隠しディレクトリ | docs-split-review §7-4 の解消。grep・一覧・レビュー動線から消えない |
| 8 | features に histogram / theme を追加、analysis と media を実在の文書で埋める | 現物 44 本の全数マッピング (§4) の帰結。空フォルダは 1 つも作らない |

---

## 4. 移行マッピング表 — 全 Markdown

被参照は「C++/tools 引用行数 ・ docs 参照ファイル数 ・ 板参照列行数」(数え方は付録)。
処置の波: **0** = 動かさず入口整備 / **1** = Compare 試験 (§5) / **2** = 低被参照の機能 +
verification + guides / **3** = 高被参照 (adapters・analysis・reference) + background 移送。
判定番号は docs-split-review §2 の再利用。

**据え置き (直下 = project 層)**

| 現行 | 被参照 | 処置 |
|---|---|---|
| terminology.md | 64・21・7 | 据え置き。背景抽出 (判定#20 ○) は波3 で background/project/ へ。series/batch 判定 1 問は現行に残す (#20 の△) |
| analysis-layers.md | 67・11・0 | 据え置き (判断2 裁定: project) |
| reference-design.md | 16・7・7 | 据え置き。背景抽出 (判定#15 ○要rebase) は波3。判断2 (CFA) の再訪条件は現行に残す |
| tasks.csv | board.py・—・— | 据え置き |
| series-plan.md | 8・4・1 | 当面据え置き。波3 で分割: 機構の現行文 (§5 token、main 文を正 — 判定#18) は直下に残すか analysis-layers へ吸収、経緯 → background/project/ |
| split-plan.md | 9・2・1 | 当面据え置き。TU 分割の完了確認後 background/project/ へ (実施済み計画) |

**features/ へ (波 1–3)**

| 現行 | 被参照 | 行き先 | 波 | 注記 |
|---|---|---|---|---|
| compare-n.md | 8・5・2(+素引用1) | features/compare/compare-n.md | **1** | 分割しない (判定#6 ×の教訓: §10–12 が現行参照網の中心)。移動のみ |
| compare-n-design.md | 0・1(handoffのみ)・0 | features/compare/compare-n-design.md | **1** | 正典 (冒頭に「衝突したら本書が勝つ」宣言あり — README が引き写す) |
| ab-stats-plan.md | 12・3・2 | features/compare/ab-stats-plan.md | 2 | 数値側 A/B 仕様。背景抽出 (判定#1 ○) は移動後 |
| browse-as-file-manager.md | 1・2・0 | features/browse/ | 2 | 判定#2 ○ (模範) — 背景抽出も移植 |
| browse-topbar-design.md | 9・5・8 | features/browse/ | 2 | 判定#5: §1–4 の背景抽出は移植、**§5 未決残 4 件と §10.8 は現行に残す** (settings.inc:1317 / preferences-panel-design.md:19 が名指し) |
| browse-topbar-mockup.html | —・design が参照・— | features/browse/ | 2 | design と同時に |
| browse-extract-design.md | 0・1・1 | features/browse/ | 2 | draft 後の新文書 (判定なし)。現行設計 |
| picker-ux.md | 0・0・0 | features/browse/ | 2 | Open Folder 後のダイアログ = browse 系の入口 |
| flat-field-stats.md | **49**・8・2 | features/analysis/ | **3** | 現行仕様 (2026-08-09 #57 確定)。移動は C++ 49 箇所の Opus PR とセット。**第二段分割 (経緯抽出) は §8-1 のユーザー裁定待ちのまま** |
| stats-taxonomy.md | 9・7・1 | features/analysis/ | 3 | 境界定義 = 現行正典 |
| measure-ux.md | 0・6・1 | features/analysis/ | 2 | 判定#13 ○ |
| histogram-select-design.md | 0・0・0 | features/histogram/ | 2 | #68 v1 仕様 |
| input-adapters.md | **44**・16・13 | features/adapters/ | **3** | remote-reader-design 着地後 (判断3)。reference/ への契約抽出は移動と別議 |
| npz-design.md | 10・3・1 | features/adapters/ | 3 | draft の背景コピー (判定#14 △) は捨て、必要なら再抽出 |
| reader-analysisset.md | 18・4・2 | features/adapters/ | 3 | |
| remote.md | 0・5・1 | features/remote/ | 2* | 判定#16 ○ (模範) — 背景抽出も移植 |
| remote-headerless-design.md | 11・2・0 | features/remote/ | 2* | *remote 波は **remote-reader-design.md (板240) の作成と同時**に行う — 新文書が最初から新配置で生まれ、既存 2 本が付いて動く |
| export-design.md | 19・4・3 | features/export/ | 2 | 判定#7 ○ (模範) |
| watch-design.md | 13・5・7 | features/watch/ | 2 | 判定#23: 判断record の背景抽出は移植、**未決 4 項は現行に残し板と突合** (項2 は #50 P11 で決着済み — docs-split-review §2#23) |
| settings-inventory.md | 12・6・1 | features/settings/ | 2 | |
| preferences-panel-design.md | 5・1・0 | features/settings/ | 2 | |
| media-support.md | 3・3・0 | features/media/ | 2 | 判定#24 × は「背景化」への× — features への移動は現行のまま動かすので抵触しない。video-support の §2 参照 3 箇所を同時更新 |
| video-support.md | 10・4・1 | features/media/ | 2 | |
| imgui_modern_design.md | 2・0・0 | features/theme/ | 2 | 判定#9 ○ — 背景抽出も移植 |

**verification/ へ (波 2)**

| 現行 | 被参照 | 行き先 | 注記 |
|---|---|---|---|
| verify-matrix.md | 1・2・**14** (凍結12/live2) | verification/matrix.md | 割らない (判断4)。live 2 行 (残課題【Fable】) の参照列を更新。凍結 12 行は stub + 対応表で解決 |
| verify-functional.md | 6・0・2 | verification/functional.md + results/20260803-functional.md | 列分割を draft から移植 (判定#21 ○要rebase)。D 節 probe 追記 (2026-08-04) も同じ型で処理 |
| verify-ui.md | 8・2・2 | verification/ui.md + results/20260803-ui.md | 判定#22 ○要rebase。main.cpp:928 (D-1) と compare.inc:478 / main.cpp:2945 (E7) の参照先が結果側へ移る — Opus 更新リスト入り (docs-split-review §4-A の ⚠ の型) |

**guides/ ・ reference/ へ (波 2–3)**

| 現行 | 被参照 | 行き先 | 波 |
|---|---|---|---|
| manual.md | 1・11・7 | guides/manual.md | 2 (判定#12 ○。README が引く — stub は合図まで必須) |
| startup.md | 0・4・0 | guides/startup.md | 2 (判定#19 ○) |
| abi-v3.md | 28・6・6 | reference/abi-v3.md | 3 (C++ 28 箇所の Opus PR とセット) |
| analyzers.md | 0・5・3 | reference/analyzers.md | 3 |

**background/ へ (波 2–3。現行の役目を終えた記録)**

| 現行 | 被参照 | 行き先 | 注記 |
|---|---|---|---|
| browse-inventory.md | 0・4・2 | background/browse/ | 判定#4 ○。板の残課題【Fable】行 15 の参照を更新 (live) |
| browse-display-candidates.md | 0・0・0 | background/browse/ | 判定#3 ○ |
| layers-plan.md | 0・2・0 | background/project/ | 判定#11 ○ |
| review-new-code.md | 0・1・1 | background/reviews/ | 判定#17 ○。板行 125 (残課題【Fable】= live) の参照を更新 |
| adapter-transport-review.md | 0・0・0 | background/reviews/ | #44 は 2026-08-14 裁定で closed (板行 4 備考) |
| flat-field-stats-review.md | 0・0・0 | background/reviews/ | #57 の締めの記録 |
| review-lenses/session-20260806.md | 0・0・0 | background/reviews/ | |
| todo-open.md | **17**・7・2 | background/project/ | **凍結 → 統合監査 → 移送**の 3 段。板に無い項目の吸い上げ監査が先 (§8-3)。C++ 17 箇所は stub + 対応表で解決 (書き換え不要 — 引かれているのは歴史的文脈) |
| docs-split-review.md | 0・1・0 | background/documentation/ | 判断確定後 |
| docs-restructure-handoff.md | 0・0・0 | background/documentation/ | 同上 |
| docs-restructure-review.md (本書) | — | background/documentation/ | 同上 |

**branch 上・未作成 (移行対象外だが対応表に予約行を置く)**

| パス | 実体 | 予約先 |
|---|---|---|
| media-formats.md | branch native-media。C++ 3 (rawread.h:27 ほか)・板参照列 3 | 上陸は現行パスで受け、media 波で features/media/ へ |
| python-plugins.md | branch plugin-python-study。板参照列 2 | 上陸後 features/plugins/ (このとき features/plugins を新設) |
| remote-reader-design.md | 未作成 (板 240、#180 裁定 B、Fable 実行中) | **features/remote/ に直接生む** (remote 波の引き金) |

---

## 5. Compare 試験移行の差分計画 (波 1。実行はしない — 計画のみ)

**なぜ Compare か** (handoff §6 に同意): 指針 (compare-n)・正典仕様 (compare-n-design)・
着地台帳 (§11)・未決 (§12 開いている判断 1–7) が実在し、分類規則を実物で評価できる。
加えて compare-n-design.md は**実質被参照 0** (handoff 以外から引かれていない — 今日実測)
なので、移動の半分はゼロコストで試せる。

**PR-A (docs のみ。1 PR)**

1. 新設 `docs/features/compare/README.md`:
   - 状態: 実装中 / 対応 Issue: #60 (+#76 済)
   - 正典宣言: 仕様 = compare-n-design.md (**衝突したら勝つ** — 同文書冒頭の宣言を引き写す)。
     指針 + 着地台帳 = compare-n.md (§11 が追記先)。数値側 = ../../ab-stats-plan.md
     (波 2 で合流予定と明記)。
   - 検証: selftest **N 群** (--abstats-selftest) / **R 群** (--roistats-selftest) /
     **T 群** (tile)。一覧の正典は CMakeLists — ここには群の名前だけ置く。
   - 未決: compare-n.md §12「開いている判断」1–7 を**番号で**列挙し、板行 114
     (レビュー必要項目) を指す。
2. `git mv docs/compare-n.md docs/features/compare/compare-n.md`、
   `git mv docs/compare-n-design.md docs/features/compare/compare-n-design.md`。
   **move-only** — 内容編集は同一コミットに入れない (§7 規則 5)。
3. 移動した 2 本の**出て行く**相対リンクを次のコミットで修正:
   compare-n-design.md 冒頭の terminology.md / ab-stats-plan.md / settings-inventory.md /
   compare-n.md への 4 リンク (`../../` 化。compare-n.md へのものは同階層のまま)。
4. 旧パスに stub 2 本 (§7 規則 1 の 3 行形式)。
5. docs 側の参照更新 (live のみ):
   - histogram-select-design.md:223, 317 (`compare-n 10` の 2 箇所)
   - settings-inventory.md:245 (`docs/compare-n.md §12`)
   - todo-open.md:32, 861, 867 は**更新しない** (歴史的記録 — stub が解決)
6. tasks.csv: 行 114 (レビュー必要項目 = live) の参照列を
   `docs/features/compare/compare-n.md` へ。行 103・160 (対応済み) は**触らない**。
7. `docs/README.md` (ポータル) を同時に新設し、features/compare 節 + 旧→新対応表 2 行を置く。
   (ルートの README.md は触らない — 合図待ちキューに「docs 表の張り替え」を積む。)

**PR-B (C++ コメントのみ。Opus へ — docs-split-review §6 と同じ分業)**

8 箇所 (今日実測): core/app/compare.inc:144 / core/selftest/abstats.inc:1603
(素の「compare-n.md §12」表記) / core/ui/file_list.inc:750 /
core/ui/panel_histogram.inc:274, 286 / core/ui/panel_projection.inc:320 /
core/ui/panel_rois.inc:84, 175。§番号は 1 つも変えない (ファイル移動のみなので
パス張り替えだけで済む — 分割しない判断の配当)。

**検証** (この波の合否):

- リンク検査 (§6) が緑 (既知例外 3 件は今日と同一のまま増えない)。
- PR-A は文書のみなので `tools/run_selftests.sh` の結果は base と一致すること
  (verify-matrix §9 の前例と同じ言い方で PR に書く)。
- 読みの試験: 「#60 の次の増分 I6 を実装する人が features/compare/README.md から
  仕様 → 台帳 → 未決の順で迷わず到達できるか」をユーザーがレビュー。
  これが通ってから波 2 へ (handoff §6-7 と同じ関門)。

---

## 6. リンク検査 — あるべき姿の宣言と、grep で書ける形

selftest 文化に合わせる: 検査は**宣言**であり、最初は check_table_columns.py と同じ
「読む物」(gate しない)、**わざと 1 本壊して赤を確認**(較正 — verify-ui「校正」節の型)
してから run_selftests.sh 末尾または CI の独立ステップで gate 化する。
実装は `tools/check_doc_links.py` 1 本 (実装は本書の範囲外。宣言は以下)。

**宣言 1 (到達)**: docs/ 配下の全 .md は docs/README.md から 1 ホップで到達できる。
孤児 = 表に無いファイル。今日この検査は README 不在で全滅する — 波 0 の完了条件。

**宣言 2 (実在)**: リポジトリが引く `docs/...` パスは、存在するか、docs/README.md の
**例外表** (branch 上 / 未作成の成果物) に載っているかのどちらか。例外表を検査スクリプト内
ではなく README に置くのは、例外自体を文書化するため。今日の例外は 3 件で固定:
media-formats.md / python-plugins.md / remote-reader-design.md (§1 の表)。
例外が増えたら検査が名指しで報告する。

```bash
grep -rIhoE 'docs/[A-Za-z0-9._/-]+\.(md|csv|html)' core tools docs README.md .github \
  | sort -u | while read p; do [ -e "$p" ] || echo "MISSING $p"; done
```

(今日の実行結果: 上記 3 件 + 引用例文ノイズ 2 件 (docs-split-review 内の `docs/x.md` /
`.background/import-adapters.md`) + 未作成の docs/README.md。ノイズはスクリプト側で
コードフェンス内を除外する。)

**宣言 3 (相対リンク)**: docs 内の相対 Markdown リンクは解決する。今日 **122 本、切れ 0**
— この数が基線。検査は各ファイルの親ディレクトリ基準で解決して数える (付録のスクリプト)。

**宣言 4 (stub の純潔)**: 対応表に載る旧パスのファイルは stub 形式 (1 行目が
`# (moved)`、本文見出しなし) を守る。stub に本文が生えたら正典が割れた合図 —
`grep -L '^# (moved)'` と `grep -c '^## '` で機械判定できる。

**宣言 5 (§参照、第二段)**: `docs/x.md §N.M` 型の参照は、対象ファイルに `## N.` /
`### N.M` 見出しが実在する。C++ からの引用は §番号が主 (compare.inc:144 → §12 ほか) なので、
パス実在だけでは docs-split-review §4 の ⚠ (在るが空を指す) を検出できない。
第二段として板の素引用 (`compare-n.md` — 行 103 の型) の正規化もここに含める。

**宣言 6 (板の live 行)**: 参照列の docs パス検査は**live 行のみ** (残課題 / 進行中 /
暫定 / レビュー必要項目)。対応済み行は対応表で解決できればよい (判断5)。

---

## 7. 移行中も正典が一意であることを守る規則

1. **move は git mv + stub を同一コミット。** stub は 3 行固定:
   `# (moved) <旧名> → <新パス>` / 移動日 / 「本文はここに無い」。本文複製ゼロ。
2. **分割は節単位の移動であって要約の複製ではない。** 旧位置には節名と新所在だけ残す。
   背景側は draft の規約ヘッダ (「現行ドキュメント: X の背景 — 要旨」) を継承する。
3. **勝敗宣言。** 1 機能に仕様文書が 2 本ある間は、どちらが勝つかを両方の冒頭と feature
   README に書く (compare-n-design.md 冒頭「衝突したら本書が勝つ」が現行の実例 — これを規範化)。
4. **板の規律。** 対応済み行の参照列は書き換えない (歴史)。live 行は移動と同一 PR で更新。
   live 行が名指す**未作成**成果物 (行 240 の remote-reader-design.md) は、作成時点の
   正典配置で生む。
5. **移動 PR は move-only。** 内容編集を混ぜない。docs-split-review §0 が示した危険
   (rename 追跡が新規内容を黙って運ぶ) は move と edit の混在で起きる — 分ければ
   rename 追跡は味方になり、diff が「移動」として読める。
6. **C++ 参照は別 PR (Opus)。** ただし文書 PR と C++ PR の両方が main に入るまで
   stub を消さない (判断5 条件 1 はこの状態でしか成立しない)。
7. **README (ルート) は合図まで触らない。** 従って README が引く 11 パスの stub は
   合図より前に消せない (判断5 条件 2)。README への変更 (docs 表の張り替え) は
   合図待ちキューに積み、波ごとに追記する。
8. **各波の完了条件** = リンク検査緑 + 板 live 行の参照が全て新パス + docs/README.md の
   対応表に当該波の全行が載っている。

---

## 8. ユーザー裁定に残るもの

docs-split-review §7 の 4 件について、本構造案での解消 / 残存:

1. **flat-field-stats / media-support の第二段分割 — 残る。** ただし縮む: 移動先
   (features/analysis / features/media) と背景先 (background/analysis / background/media) が
   本案で確定するので、裁定は「#57/#53 の判断record を含む経緯部を、確定後の姿に対して
   改めて背景へ抜くか」の一点だけになる。指示なしにはやらない (原文どおり)。
2. **「実装から返ってきたもの」節の扱い — 解消。** 判断4 の形態 2 (機能内着地台帳) として
   分類規則に載せた: 本文に勝っている間は現行、本文へ吸収された時点で背景へ。
   これは docs-split-review の私見と同じ内容の正式化 — 異議があれば言葉のうちに。
3. **未決の板吸い上げ — 半分解消。** feature README の未決節は「板 row 番号を指す」か
   「板未登載」を名乗ることを完了条件にした (handoff §9「食い違わない」の機械化)。
   **残る**のは「板未登載」の未決 (browse-topbar §5 残 4 件・watch-design 残項ほか) を
   実際に板へ行として足すか — 板の行数はユーザーの帳簿なので裁定待ち。
   todo-open.md の統合監査 (板に無い項目の洗い出し) も同じ裁定に含める。
4. **.background の可視性 — 解消。** background/ は非隠し (差分 7)。README の docs 表は
   合図待ちキューへ (規則 7)。

本案が新たに開く裁定は 2 つ:

5. **「直下 = project 層」の採用** (codex 案からの最大の差分)。根拠は参照コスト
   (判断1) だが、見た目は codex 案と明確に違うので名指しで確認を求める。
6. **flat-field-stats を波 3 で本当に動かすか。** C++ 49 箇所の更新とセットなら安全に
   できるが、「stub 恒久 + 直下残置」も守れる選択肢ではある。私の推しは移動
   (解析機能の文書が 1 箇所に揃う価値が 49 箇所の一括更新に勝つ)。ただし 1 の
   第二段分割と同時にやると 1 回の帳簿替えで済む — 裁定 1 と束ねるのを推す。

---

## 付録 — 数えの再現手順 (すべて 2026-08-17 実測)

- 文書数: `ls docs/*.md | wc -l` → 44、+ `docs/review-lenses/session-20260806.md`
- 板: python csv パース (utf-8-sig)。251 データ行。分類は第 1 列、参照は第 4 列のみを
  `docs/[A-Za-z0-9._-]+\.(md|csv)` で抽出 (素引用 — 行 103 の「compare-n.md」型 — は
  別掲で注記)
- C++/tools 被参照 (行数): `grep -rhoE 'docs/[A-Za-z0-9._/-]+\.md' core tools`
  (*.cpp *.h *.inc *.sh *.py) `| sort | uniq -c`。compare-n は素引用 (abstats.inc:1603) を
  +1 して 8
- docs 被参照 (ファイル数): 各 `<名前>.md` について `grep -l` を docs/*.md に当て自分を除外
- main.cpp 単体: `grep -c 'docs/' core/main.cpp` → 17
- branch 差: `git log docs-split-draft..main --oneline | wc -l` → 587
- 相対リンク: `\]\((パス)\)` を docs/**/*.md から抽出し親ディレクトリ基準で解決 → 122 本、
  切れ 0
- 実在しない被引用パス: §6 宣言 2 のコマンド → media-formats / python-plugins /
  remote-reader-design (+ ノイズ 2 + 未作成 README)
- tasks.csv 消費者: tools/board.py:18。CI と diagnostics: .github/workflows/build.yml:107
