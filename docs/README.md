# docs — 文書ポータル

このリポジトリにある文書の入口です。重複や矛盾がある場合に優先する、唯一の記述を
本書では「正典」と呼びます。文書は、まず対象範囲を「製品全体」と「機能」に分け、
各機能の中で開発フェーズを示します。現在の課題は [tasks.csv](tasks.csv)、
テスト登録は [CMakeLists.txt](../CMakeLists.txt) が正典です。

## 最初に読む

1. [terminology.md](terminology.md) — frame / stack / series / AnalysisSet / batch の語彙と不変条件
2. [analysis-layers.md](analysis-layers.md) — データ層と解析の種類
3. [guides/manual.md](guides/manual.md) — 日常の操作
4. [tasks.csv](tasks.csv) — 現在の課題と状態
5. 下の「機能」から変更対象の索引へ進む — 仕様、検証、背景を読む順に案内

## 分類規則

| 置き場所 | 入れるもの | 入れないもの |
|---|---|---|
| docs/ 直下 | 複数機能にまたがる語彙、データモデル、全体方針、課題台帳 | 一つの機能だけで再検証できる仕様 |
| features/機能名/ | 一つの機能だけを変更・再検証すれば完結する要求、設計仕様、実装反映記録 | 過去の検討だけを残す記録 |
| verification/ | 再実行できる検証仕様。固定した実施記録は `verification/results/` | 機能固有の実装台帳 |
| guides/ | 操作・起動など利用者向け手引き | 実装内部の契約 |
| reference/ | リポジトリ外の利用者が消費する契約 | 内部だけで完結する設計 |
| background/ | 採用しなかった案、完了した計画、過去のレビュー | 現行仕様、現在の未決事項 |

機能ディレクトリでは、空の定型ファイルを増やしません。各 README の表で
「要求・方針 → 設計仕様 → 検証仕様 → 検証結果 → 判断・背景」のどのフェーズかを示し、
存在する文書だけを案内します。一つの文書が複数フェーズを担う場合も、正典は一つです。

各フェーズの意味は次のとおりです。「要求・方針」は、なぜ必要か、何を満たすかを示します。
「設計仕様」は、実装が満たす構造や振る舞いを定めます。未実装部分を含む場合は、その状態を
併記します。「検証仕様」は、再実行できる
手順と期待結果を定めます。「検証結果」は、特定の実行時点で得た事実を固定して残します。
「判断・背景」は、採用理由、採用しなかった案、完了した計画などの経緯です。

## 製品全体

| 文書 | 役割 | 状態 |
|---|---|---|
| [terminology.md](terminology.md) | frame / stack / series / AnalysisSet / batch の定義 | 現行の語彙正典 |
| [analysis-layers.md](analysis-layers.md) | 3つの順序付きデータ層、batch の管理、AnalysisSet の役割付き参照、4種類の analyzer | 現行のモデル正典 |
| [reference-design.md](reference-design.md) | frame の共有参照化 | 確定仕様 |
| [series-plan.md](series-plan.md) | series 導入の全体計画と記録 | 実装済み部分を含む全体文書 |
| [tasks.csv](tasks.csv) | 対応済み、進行中、残課題、暫定、要レビュー | 現在の課題の正典 |

## 機能

各機能の README には、優先する正典、フェーズ、読む順番だけを記します。未決事項を
重複して書かず、[tasks.csv](tasks.csv) を参照します。

| 機能 | 索引 | 主なフェーズと現在の状態 |
|---|---|---|
| adapters | [features/adapters/README.md](features/adapters/README.md) | Reader と入力形式の現行契約、確定仕様 |
| analysis | [features/analysis/README.md](features/analysis/README.md) | 統計分類、確定した平坦画像仕様、測定 UX |
| browse | [features/browse/README.md](features/browse/README.md) | 場所の意味論、画面・選択・分割設計 |
| compare | [features/compare/README.md](features/compare/README.md) | N-way compare の規範仕様と実装反映記録 |
| export | [features/export/README.md](features/export/README.md) | 測定結果の持ち出し設計 |
| histogram | [features/histogram/README.md](features/histogram/README.md) | 値域ハイライト v1 の確定設計 |
| media | [features/media/README.md](features/media/README.md) | OpenEXR と y4m の現行仕様 |
| remote | [features/remote/README.md](features/remote/README.md) | ssh / peer。remote Reader はステージ0〜5が実装済み |
| settings | [features/settings/README.md](features/settings/README.md) | 設定分類と Preferences 設計 |
| theme | [features/theme/README.md](features/theme/README.md) | 実装済み Aurora テーマ |
| watch | [features/watch/README.md](features/watch/README.md) | 変化検知、通知、再読込の設計 |

## 検証・手引き・外部契約・背景

| 区分 | 索引 | 内容 |
|---|---|---|
| 検証 | [verification/README.md](verification/README.md) | 再実行できる検証仕様と、固定した実施記録 |
| 手引き | [guides/README.md](guides/README.md) | 起動と操作 |
| 外部契約 | [reference/README.md](reference/README.md) | plugin ABI と analyzer |
| 背景 | [background/README.md](background/README.md) | 完了した計画、比較、レビュー |

保存済みの検証結果は、次の5回の検証実施（run）です。

- [verification/results/20260803-functional.md](verification/results/20260803-functional.md)
- [verification/results/20260803-ui.md](verification/results/20260803-ui.md)
- [verification/results/20260804-functional-probes.md](verification/results/20260804-functional-probes.md)
- [verification/results/20260804-ui-probes.md](verification/results/20260804-ui-probes.md)
- [verification/results/20260817-open-with-reader-ui.md](verification/results/20260817-open-with-reader-ui.md)


再構成より前から存在する背景文書のうち、文書全体の移動（whole-file move）の
対応表に現れないものは次の2本です。

- [background/analysis/flat-field-stats.md](background/analysis/flat-field-stats.md)
- [background/media/media-support.md](background/media/media-support.md)

## 文書の更新規則

1. 現行仕様は一か所だけに置きます。背景へ移した記述を現在形で複製しません。
2. 機能文書にはフェーズを示し、未決事項は [tasks.csv](tasks.csv) の行を参照します。
3. 検証仕様には、再実行できる手順と期待結果を記します。新規の検証結果は、1つの
   コミット、識別できる1台の実行機、1回の実施を単位として固定します。移送元に
   実行機の識別子が残っていない過去記録は `unknown` と明記して凍結し、推測で補わず、
   後日の状態でも書き換えません。
4. 文書全体を移動した場合（`whole-file`）、下の恒久対応表に登録します。`stub` が
   `required` の間だけ旧パスに3行の転送案内を残し、本文は現在パスにだけ置きます。
   現行参照をすべて移行したら転送案内を削除し、対応表を `none` にします。
5. 移動後も、コード、CI、ツール、課題台帳、README の参照を現在パスへ更新します。
   旧パスを歴史的記録として残す場合は、下の恒久対応表に登録して追跡可能にします。
6. 外部向け契約は、公開ヘッダと実装の両方に照合します。
7. 採用しなかった案や完了した計画は `background/` へ移し、現在の再訪条件は現行文書か
   [tasks.csv](tasks.csv) に残します。

## 旧パス → 現在パス

この表は、転送案内（stub）を削除した後も残す恒久対応表です。パス欄に表示する
リンク名は、リポジトリルートからの相対パスです。`stub` が `required` の行では旧パスに
転送案内を残し、`none` の行では旧パスを削除済みです。

<!-- DOCS-MOVE-MAP:START -->
| 旧パス | 現在パス | 種別 | stub |
|---|---|---|---|
| [docs/ab-stats-plan.md](ab-stats-plan.md) | [docs/features/compare/ab-stats-plan.md](features/compare/ab-stats-plan.md) | whole-file | none |
| [docs/abi-v3.md](abi-v3.md) | [docs/reference/abi-v3.md](reference/abi-v3.md) | whole-file | none |
| [docs/adapter-transport-review.md](adapter-transport-review.md) | [docs/background/reviews/adapter-transport-review.md](background/reviews/adapter-transport-review.md) | whole-file | none |
| [docs/analyzers.md](analyzers.md) | [docs/reference/analyzers.md](reference/analyzers.md) | whole-file | none |
| [docs/browse-as-file-manager.md](browse-as-file-manager.md) | [docs/features/browse/browse-as-file-manager.md](features/browse/browse-as-file-manager.md) | whole-file | none |
| [docs/browse-display-candidates.md](browse-display-candidates.md) | [docs/background/browse/browse-display-candidates.md](background/browse/browse-display-candidates.md) | whole-file | none |
| [docs/browse-extract-design.md](browse-extract-design.md) | [docs/features/browse/browse-extract-design.md](features/browse/browse-extract-design.md) | whole-file | none |
| [docs/browse-inventory.md](browse-inventory.md) | [docs/background/browse/browse-inventory.md](background/browse/browse-inventory.md) | whole-file | none |
| [docs/browse-topbar-design.md](browse-topbar-design.md) | [docs/features/browse/browse-topbar-design.md](features/browse/browse-topbar-design.md) | whole-file | none |
| docs/browse-topbar-mockup.html | [docs/features/browse/browse-topbar-mockup.html](features/browse/browse-topbar-mockup.html) | asset | none |
| [docs/compare-n-design.md](compare-n-design.md) | [docs/features/compare/compare-n-design.md](features/compare/compare-n-design.md) | whole-file | none |
| [docs/compare-n.md](compare-n.md) | [docs/features/compare/compare-n.md](features/compare/compare-n.md) | whole-file | none |
| [docs/docs-restructure-handoff.md](docs-restructure-handoff.md) | [docs/background/documentation/docs-restructure-handoff.md](background/documentation/docs-restructure-handoff.md) | whole-file | none |
| [docs/docs-restructure-review.md](docs-restructure-review.md) | [docs/background/documentation/docs-restructure-review.md](background/documentation/docs-restructure-review.md) | whole-file | none |
| [docs/docs-split-review.md](docs-split-review.md) | [docs/background/documentation/docs-split-review.md](background/documentation/docs-split-review.md) | whole-file | none |
| [docs/export-design.md](export-design.md) | [docs/features/export/export-design.md](features/export/export-design.md) | whole-file | none |
| [docs/flat-field-stats-review.md](flat-field-stats-review.md) | [docs/background/reviews/flat-field-stats-review.md](background/reviews/flat-field-stats-review.md) | whole-file | none |
| [docs/flat-field-stats.md](flat-field-stats.md) | [docs/features/analysis/flat-field-stats.md](features/analysis/flat-field-stats.md) | whole-file | none |
| [docs/histogram-select-design.md](histogram-select-design.md) | [docs/features/histogram/histogram-select-design.md](features/histogram/histogram-select-design.md) | whole-file | none |
| [docs/imgui_modern_design.md](imgui_modern_design.md) | [docs/features/theme/imgui_modern_design.md](features/theme/imgui_modern_design.md) | whole-file | none |
| [docs/input-adapters.md](input-adapters.md) | [docs/features/adapters/input-adapters.md](features/adapters/input-adapters.md) | whole-file | none |
| [docs/layers-plan.md](layers-plan.md) | [docs/background/project/layers-plan.md](background/project/layers-plan.md) | whole-file | none |
| [docs/manual.md](manual.md) | [docs/guides/manual.md](guides/manual.md) | whole-file | none |
| [docs/measure-ux.md](measure-ux.md) | [docs/features/analysis/measure-ux.md](features/analysis/measure-ux.md) | whole-file | none |
| [docs/media-support.md](media-support.md) | [docs/features/media/media-support.md](features/media/media-support.md) | whole-file | none |
| [docs/npz-design.md](npz-design.md) | [docs/features/adapters/npz-design.md](features/adapters/npz-design.md) | whole-file | none |
| [docs/picker-ux.md](picker-ux.md) | [docs/features/browse/picker-ux.md](features/browse/picker-ux.md) | whole-file | none |
| [docs/preferences-panel-design.md](preferences-panel-design.md) | [docs/features/settings/preferences-panel-design.md](features/settings/preferences-panel-design.md) | whole-file | none |
| [docs/reader-analysisset.md](reader-analysisset.md) | [docs/features/adapters/reader-analysisset.md](features/adapters/reader-analysisset.md) | whole-file | none |
| [docs/remote-headerless-design.md](remote-headerless-design.md) | [docs/features/remote/remote-headerless-design.md](features/remote/remote-headerless-design.md) | whole-file | none |
| [docs/remote-reader-design.md](remote-reader-design.md) | [docs/features/remote/remote-reader-design.md](features/remote/remote-reader-design.md) | whole-file | none |
| [docs/remote.md](remote.md) | [docs/features/remote/remote.md](features/remote/remote.md) | whole-file | none |
| [docs/review-lenses/session-20260806.md](review-lenses/session-20260806.md) | [docs/background/reviews/session-20260806.md](background/reviews/session-20260806.md) | whole-file | none |
| [docs/review-new-code.md](review-new-code.md) | [docs/background/reviews/review-new-code.md](background/reviews/review-new-code.md) | whole-file | none |
| [docs/settings-inventory.md](settings-inventory.md) | [docs/features/settings/settings-inventory.md](features/settings/settings-inventory.md) | whole-file | none |
| [docs/split-plan.md](split-plan.md) | [docs/background/project/split-plan.md](background/project/split-plan.md) | whole-file | none |
| [docs/startup.md](startup.md) | [docs/guides/startup.md](guides/startup.md) | whole-file | none |
| [docs/stats-taxonomy.md](stats-taxonomy.md) | [docs/features/analysis/stats-taxonomy.md](features/analysis/stats-taxonomy.md) | whole-file | none |
| [docs/todo-open.md](todo-open.md) | [docs/background/project/todo-open.md](background/project/todo-open.md) | whole-file | none |
| [docs/verify-functional.md](verify-functional.md) | [docs/verification/functional.md](verification/functional.md) | whole-file | none |
| [docs/verify-matrix.md](verify-matrix.md) | [docs/verification/matrix.md](verification/matrix.md) | whole-file | none |
| [docs/verify-ui.md](verify-ui.md) | [docs/verification/ui.md](verification/ui.md) | whole-file | none |
| [docs/video-support.md](video-support.md) | [docs/features/media/video-support.md](features/media/video-support.md) | whole-file | none |
| [docs/watch-design.md](watch-design.md) | [docs/features/watch/watch-design.md](features/watch/watch-design.md) | whole-file | none |
<!-- DOCS-MOVE-MAP:END -->

### 節移動履歴

次の表は文書全体の移動ではなく、旧文書の一部を現行仕様と履歴へ分けた記録です。
stub の有無を自動判定する契約には含めません。

<!-- DOCS-SPLIT-HISTORY:START -->
| 元文書 | 分離先 | 内容 |
|---|---|---|
| [docs/flat-field-stats.md](flat-field-stats.md) | [docs/background/analysis/flat-field-stats.md](background/analysis/flat-field-stats.md) | 判断記録と実施済み計画 |
| [docs/media-support.md](media-support.md) | [docs/background/media/media-support.md](background/media/media-support.md) | 候補比較と採用しなかった tinyexr 案 |
<!-- DOCS-SPLIT-HISTORY:END -->

## 未作成文書の例外

存在しない `docs/` パスを許す例外は、別ブランチにだけある次の2本です。
新しい例外を自動検査ツールへ直接追加せず、理由と将来の配置先をこの表でレビューします。

<!-- DOCS-PATH-EXCEPTIONS:START -->
| path | 理由 | 作成時の分類 |
|---|---|---|
| docs/media-formats.md | 未マージ branch にだけ存在 | docs/features/media/ |
| docs/python-plugins.md | plugin-python-study branch にだけ存在 | docs/features/plugins/ |
<!-- DOCS-PATH-EXCEPTIONS:END -->

## 自動検査

[tools/check_doc_links.py](../tools/check_doc_links.py) が次の項目を一括して検査します。
検査用のシェル断片を文書へ複製せず、このツールをローカル実行と CI の共通入口にします。

- docs 配下の全 Markdown が、このポータルから直接リンクされていること
- Markdown の相対リンクと見出しアンカー（fragment）が解決できること
- Git 管理下のテキストが参照する `docs/` パスが実在するか、例外表にあること
- `whole-file` の全 `required` 行について、旧パスが正確な3行の stub であること
- stub の欠落、余分な本文、現在パスの欠落、対応表にない stub がないこと
- コードと文書にある節参照が、現在パスの実在見出しへ解決すること
- [tasks.csv](tasks.csv) の未完了行（live 行）が参照する文書と見出しが解決すること

再構成前の元文書（Markdown）は50本でした。stub、索引を加えると総数は
変わるため、合格条件に固定件数を埋め込まず、ツールが毎回 Git 管理下のツリーから
数えます。

画像は [img/](img/)、CI と検証の証跡は [diagnostics/](diagnostics/) に置きます。
