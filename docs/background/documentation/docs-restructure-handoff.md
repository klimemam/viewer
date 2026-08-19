# Fable 引き継ぎ: docs を「全体 / 機能 × 開発フェーズ」で再構成する

対象: issue #55 (`docs` の分割)

状態: **構造案の検討段階。まだファイルを移動しない。**

## 1. ユーザー要求

現在の `docs/` は文書がフラットに並び、次の区別が見えにくい。

- 製品全体の文書か、特定機能の文書か
- 要求仕様、設計仕様、検証仕様、検証結果のどの段階か
- 現行の正典か、検討経緯・却下案・過去の実施記録か

ユーザー案は、開発フェーズ（例: 要求仕様 → 設計仕様 → 検証仕様 → 検証結果）と、
ルートトップ / 各機能への分割を組み合わせた構造にすること。

## 2. 推奨する基本原則

最上位をフェーズ別にすると、1機能を理解するために複数フォルダを往復する。
そのため、**主軸は「製品全体 / 機能」、各機能の内側を開発フェーズで分ける**。

```text
docs/
├─ README.md                    # 文書ポータル、正典、読む順番
├─ project/                     # 製品全体に適用される文書
│  ├─ requirements.md
│  ├─ terminology.md
│  ├─ architecture.md
│  ├─ quality-requirements.md
│  └─ verification/
│     ├─ specification.md
│     └─ results/
├─ features/
│  ├─ compare/
│  │  ├─ README.md
│  │  ├─ requirements.md
│  │  ├─ design.md
│  │  ├─ verification.md
│  │  ├─ decisions.md
│  │  └─ results/
│  ├─ browse/
│  ├─ analysis/
│  ├─ adapters/
│  ├─ remote/
│  ├─ export/
│  ├─ watch/
│  ├─ settings/
│  ├─ media/
│  └─ plugins/
├─ guides/                      # 利用者・開発者向け手順
├─ reference/                   # API、ABI、プロトコル、形式の参照資料
├─ governance/                  # 課題台帳、文書・検証ポリシー
└─ background/                  # 調査、却下案、過去の経緯
```

すべての機能に空の定型ファイルを作る必要はない。内容があるものだけ置き、
`features/<feature>/README.md` が正典と読む順番を指す。

## 3. 分類規則

| 分類 | 答える問い | 入れる内容 | 更新規則 |
|---|---|---|---|
| Requirements | 何を満たすべきか | 利用場面、要求、受入条件、対象外 | 要求変更時に更新 |
| Design | どう実現するか | UI、データ構造、処理、制約 | 設計変更時に更新 |
| Verification | どう合否を判定するか | テスト項目、手順、期待結果 | 実装前または実装と同時に確定 |
| Results | 実際にどうだったか | 実行環境、実施結果、証跡、判定 | 原則追記。過去の実行結果を書き換えない |
| Decisions | 何を選び、何を選ばなかったか | 選択肢、裁定、理由、再訪条件 | 裁定ごとに追記 |
| Background | なぜこの検討が必要だったか | 調査、試作、却下案、議論 | 現行仕様と分離して保存 |
| Guide | 利用者が何をすればよいか | 操作手順、開発手順、例 | 現行リリースに追従 |
| Reference | 正確な契約は何か | ABI、プロトコル、形式、表 | 実装との一致を必須にする |

特に **Verification と Results を分離する**。現在の `verify-*.md` は、
再利用する試験手順と、ある時点の実施結果が同じ表に入るものがある。
仕様を更新しても、過去の証跡の意味が変わらない形にする。

## 4. 各機能の入口

`features/<feature>/README.md` は、少なくとも次を短く示す。

```markdown
# Compare

状態: 実装中
対応Issue: #60

## 正典

- 要求: requirements.md
- 裁定: decisions.md
- 設計: design.md
- 検証仕様: verification.md

## 読む順番

1. requirements.md
2. decisions.md
3. design.md
4. verification.md

## 未決事項

- ...
```

同じ事実を複数文書へ複製せず、入口から正典へリンクする。

## 5. 現在の文書の配置例

これは確定マッピングではなく、分類を検証するための叩き台。

| 現在 | 移行候補 | 注記 |
|---|---|---|
| `terminology.md` | `project/terminology.md` | 全体の正典 |
| `analysis-layers.md` | `project/architecture/analysis-model.md` または `features/analysis/design.md` | 全体モデルか機能設計かを裁定する |
| `compare-n.md` | `features/compare/requirements.md` + `decisions.md` | 要求と経緯・未決を分離 |
| `compare-n-design.md` | `features/compare/design.md` | 現行設計 |
| `browse-inventory.md` | `background/browse/inventory.md` | 棚卸し素材 |
| `browse-topbar-design.md` | `features/browse/design/topbar.md` | Browse内の部分設計 |
| `browse-extract-design.md` | `features/browse/design.md` | Browseの現行設計候補 |
| `input-adapters.md` | `features/adapters/requirements.md` + `design.md` + `reference/reader-contract.md` | 1427行を責務で分ける |
| `remote-headerless-design.md` | `features/remote/design/headerless-raw.md` | RemoteとRAWの境界をREADMEで案内 |
| `verify-matrix.md` | `project/verification/specification.md` + `results/<run>.md` | 手順・期待と実施結果を分離 |
| `manual.md` | `guides/user-manual.md` | 利用者向け |
| `docs-split-review.md` | `background/documentation/docs-split-review.md` | 今回の判断材料として先に読む |
| `todo-open.md` | `governance/tasks.csv`へ統合後、背景へ | 課題の正典を二つにしない |

## 6. 移行方針

一括移動はしない。現状は Markdown 内だけでも多数の文書参照があり、C++、テスト、
`tasks.csv` からもパスが参照されている。既存の `docs-split-draft` は main から大きく
離れており、`docs/docs-split-review.md` が、現行仕様を黙って背景へ移す危険を報告している。

推奨する段階:

1. 本案と `docs/docs-split-review.md` を照合し、文書ポリシーを確定する。
2. `docs/README.md` を作り、現状のファイルを動かさず入口だけ整える。
3. **Compare 1機能だけ**を試験移行する。
4. 旧パスには一時的な案内文を残し、正典が二つにならないよう本文は複製しない。
5. C++、テスト、README、`tasks.csv` の参照を全数更新する。
6. 相対リンク、コード中のdocs参照、孤児文書を検査する仕組みを追加する。
7. Compareで読みやすさと更新しやすさを確認してから他機能へ展開する。
8. 移行完了後に旧パスの案内ファイルを削除する。

Compareを最初にする理由は、要求 (`compare-n.md`)、設計 (`compare-n-design.md`)、
裁定、未決事項、検証がすでに存在し、分類規則を実物で評価できるため。

## 7. Fable に依頼する判断

1. 「全体 / 機能」を主軸にし、機能内をフェーズ分割する方針が妥当か。
2. `project/architecture` と `features/*/design` の境界をどう定義するか。
3. `reference/` を機能配下に置かず横断領域にするか。
4. Results の単位を日付、リリース、PR、検証runのどれにするか。
5. 旧パスの案内ファイルをどの時点まで残すか。
6. `docs-split-draft` をrebaseして利用するか、構造案だけ継承して作り直すか。

推奨は、古いブランチを機械的にrebaseせず、`docs-split-review.md` で正しいと判定された
分割線だけを新構造へ移植すること。

## 8. 最初の成果物

Fableには、全移行ではなく次を最初の成果物として期待する。

1. 提案構造へのレビューと修正版ツリー。
2. 既存全Markdownの移行マッピング表（現行 / 背景 / 分割 / 廃止候補を明記）。
3. Compare試験移行の差分計画。
4. 参照更新とリンク検査の方法。
5. 移行中も正典が一意であることを守る規則。

## 9. 完了条件

- `docs/README.md` から全現行文書へ到達できる。
- 各機能の入口から、要求・裁定・設計・検証の正典が判別できる。
- 検証仕様と過去の検証結果が分離されている。
- 現行仕様と背景資料が混ざらない。
- 同じ仕様を本文として二重管理しない。
- リポジトリ内の旧パス参照と相対リンクに切れがない。
- `docs/tasks.csv` と文書内の未決事項が食い違わない。

## 10. 関連資料

- `docs/docs-split-review.md`: 旧分割案の全件レビューと危険箇所
- `docs/tasks.csv`: 課題の正典
- GitHub issue #55: docs分割の判断を書く場所
- `README.md` §ドキュメント: 現在の主要文書への入口
