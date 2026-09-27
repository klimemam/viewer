# verification — 検証仕様と検証結果

再実行するための検証仕様と、実施後に書き換えない検証結果を分離します。
テスト登録の正典は [CMakeLists.txt](../../CMakeLists.txt) です。

## 検証仕様

| 読む順 | 文書 | 役割 |
|---:|---|---|
| 1 | [matrix.md](matrix.md) | 形式 × 入口 × 操作の3軸と状態の定義（§0・§1）＋落穂の現況台帳（§7・§8・§10）。2026-08-11 の測定そのものは [results/20260811-matrix.md](results/20260811-matrix.md) |
| 2 | [functional.md](functional.md) | 機能の手順・期待結果・合格条件 |
| 3 | [ui.md](ui.md) | UI の手順・期待結果・自動検証と実機確認の境界 |

## 検証結果

新規結果は、1つのコミット、識別できる1台の実行機、1回の実施を単位として固定します。
移送元に実行機の識別子が残っていない過去記録は `unknown` と明記して凍結し、推測で
補いません。後日の実装状況を、過去の検証結果へ追記しません。

| 実施日 | 文書 | 対象 |
|---|---|---|
| 2026-08-03 | [results/20260803-functional.md](results/20260803-functional.md) | frame 参照化の機能検証 |
| 2026-08-03 | [results/20260803-ui.md](results/20260803-ui.md) | frame 参照化の UI 検証 |
| 2026-08-04 | [results/20260804-functional-probes.md](results/20260804-functional-probes.md) | 機能 probe（観測点）の追試 |
| 2026-08-04 | [results/20260804-ui-probes.md](results/20260804-ui-probes.md) | UI probe（観測点）の追試 |
| 2026-08-11 | [results/20260811-matrix.md](results/20260811-matrix.md) | 形式 × 入口 × 操作 299セルのマトリクス測定と `[P]` 実測 |
| 2026-08-17 | [results/20260817-open-with-reader-ui.md](results/20260817-open-with-reader-ui.md) | Browse「Open with reader...」の UI 検証 |
| 2026-09-08 | [results/20260908-model-boundaries.md](results/20260908-model-boundaries.md) | #230 phase④ model boundary |
| 2026-09-12 | [results/20260912-typed-channels.md](results/20260912-typed-channels.md) | #259 C=1連続コピー・C=4両経路検査 |

現在の残課題は [tasks.csv](../tasks.csv) で管理します。現行仕様の変更は各仕様文書に、
実装への反映状況は必要に応じて機能別の実装反映記録に記し、過去の結果とは分けます。
