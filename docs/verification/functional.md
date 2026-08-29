# 機能検証仕様

本書は、現在の `main` に対して繰り返し実行する機能検証の仕様である。特定のコミットに
対する実施結果、PASS/FAIL の集計、実行環境の情報は置かない。それらは
[results/](results/) の日付付き記録に保存する。

テストの登録名、引数、テストデータ（fixture）、OpenGL 不要の指定（`NOGL`）、
タイムアウトの正典は
[`CMakeLists.txt`](../../CMakeLists.txt) の `viewer_selftest(...)` 群である。本書は、
そのテスト群が何を保証するかを項目別に定義する。

## 1. 合格条件と不変条件

全機能に共通する合格条件は次のとおり。

1. ビルドが成功し、CMake に登録されたテストが一覧に現れないまま欠落しない。
2. 実行された各テストが終了コード 0 で、内部の全アサーション（表明）が PASS になる。
3. skip、quarantine、未登録、実行不能を PASS に数えない。
4. 次の測定不変条件をどの変更でも保つ。

| 不変条件 | 正典 | 検証で見ること |
|---|---|---|
| `sigma_t` は frame ではなく stack の性質 | [用語](../terminology.md) | stack 集計にだけ現れ、per-frame 表には `sigma_t` 列がない |
| CFA のプレーンを黙って混ぜない | [用語](../terminology.md) | R / Gr / Gb / B を独立して扱う。明示的な `all` 集計は別測定として表示・出力する |
| 部分ロードを完全な stack と誤認させない | [用語](../terminology.md) | 常駐済み数（resident）と予定総数（expected）を `n/N` で示す |
| ラベルに乗った量は単位を伴う | [用語](../terminology.md) | 画素値・ノイズは `[DN]`、相対量は `[%]` と明示する |
| 数値は、その数値を作った画素と同時に無効化する | [参照設計](../reference-design.md) | reload / crop / source 共有後に古い cache、fit、平均を再利用しない |

## 2. 標準の実行方法

リポジトリのルートで、構成済みのビルドディレクトリを指定して実行する。

```bash
tools/run_selftests.sh build
```

この入口は、ビルド、テストデータの再生成、OpenGL の利用可否確認、ctest、未実行項目の
列挙、失敗ログの退避を一続きで行う。ビルドディレクトリを省略した場合の探索規則も、
同スクリプトが正典である。

実行時の制約:

- テストは逐次実行する。ローダと peer には実時間の制限があるため、並列実行を
  標準の合否判定に使わない。
- CMake がテストごとに `APPDATA` / `HOME` / `TMP` / `TEMP` / `TMPDIR` を分ける。
  操作者のレイアウト、セッション、設定、別テストの固定名テストデータを共有しない。
- OpenGL コンテキストがない環境では `nogl` のテストだけが走る。出力の
  `ran / skipped / quarantined / in no run set`（実行済み／スキップ／隔離中／実行集合外）
  を確認する。全項目の受入れには `skipped=0` が必要である。
- リモート検証は CMake に登録された `--remote-exe` を使い、client 自身ではなく
  単独実行の `viewer-serve` を相手にする。
- 失敗を再現するときは、まず標準入口が保存した
  `docs/diagnostics/auto/` のログを保全する。

単独項目の調査には、標準入口を一度通してビルドとテストデータを最新にした後で、
次の形を使う。

```bash
ctest --test-dir build -C Release -R '^selftest\.<name>$' -V
```

`tools/verify/ab_capture.sh`、`ab_diff.sh`、`invariants.sh` は
2026-08-03 の二つのビルドを比較した記録用スクリプトであり、当時のコマンド列を
固定して保持する。現在の全テスト入口としては使わない。

## 3. 判定語

| 判定 | 意味 |
|---|---|
| PASS | 手順を実行し、期待する全表明と終了コードを確認した |
| FAIL | 実行結果が期待と異なる。新しい検証結果に証拠を残す |
| 未実施 | この検証実施（run）では実行していない。PASS には含めない |
| 実行不能 | テストデータ、依存関係、OpenGL、peer などの実行条件を満たせない。理由を記録する |
| 要 probe | 合否を決める状態が外から観測できない。観測点（probe）を追加するまで、コードを読んだだけで PASS にしない |
| quarantine | 既知の失敗として合否判定（gate）の対象外に隔離している。解除条件とともに名指しし、PASS にしない |

## 4. 現行の機能検証項目

表の「絞り込み」は CTest 名の候補である。`viewer_selftest` で登録した項目は共通の
`selftest.` 接頭辞を省き、`test_prnu` など `test_` で始まる項目は完全な CTest 名を
記す。単独実行では、前者に `selftest.` を補って `ctest -R` で選ぶ。標準の受入れでは、
絞り込みだけでなく §2 の全テストを実行する。

この表では、文書と `source` の所属関係を `membership`、同じ `source` かどうかを
判定する識別情報の組を `identity tuple` と呼ぶ。コード上の名称を示す場合は、英語表記を
そのまま残す。

| 項番 | 検証項目 | 手順 / 絞り込み | 期待結果 |
|---|---|---|---|
| F1 | 合否判定（gate）の完全性 | `tools/run_selftests.sh <build-dir>` | ビルドとテストデータ生成が成功する。登録されたテストは実行、skip、quarantine、実行集合（run set）外のいずれかに必ず名指しされ、無言の欠落がない |
| F2 | 測定不変条件 | `framestats`, `export-tsv`, `abstats`, `abstats-cfa-bayer`, `roistats`, `setanalysis` | §1 の `sigma_t`、CFA、`n/N`、`[DN]` と比率単位を満たす。A/B の各側は自分の数値を使い、隣側の値を付け替えない |
| F3 | source の同一性と共有 | `srcmap`, `scan`, `fmtreg`, `verify` | 同じ `identity tuple` を持つ二つの `membership` は同じ `source` を指し、復号と常駐メモリの会計を二重に行わない。別の recipe / member / file-frame は共有しない |
| F4 | Close の意味論 | `close`, `batch`, `verify` | stack / frame を閉じると、その `membership` だけが外れる。`source` は最後の参照がなくなるまで生存し、残った `membership` は画素と統計を読める |
| F5 | 派生（derive）と CoW | `derive` | derive は `source` を参照し、常駐バイト数を増やさない。共有後に片側を crop すると新しい `source` へ分離し、他方の画素・identity・統計を変えない |
| F6 | セッションの往復 | `series`, `seriespanel`, `batch`, `derive`, `verify`, `stackavg` | stack への明示的な `membership`、名前、順序、compare の参照、ファイル内 member、解析 recipe を保存・復元する。旧キーだけの session も定義済みの互換動作で開く |
| F7 | reload 時の一括無効化 | `reload` | `membership` の `uid` を保ったまま `source` の内容と識別情報を更新し、全共有先の `dataRev` / `stackRev`、cache、texture、temporal、fit を一度の処理で更新する。次の open は更新後の `source` を共有する |
| F8 | reload 失敗の可視状態 | `reload`, `stackavg` | 一部 member の reload 失敗を stack にラッチし、成功する stack 全体 reload まで黙って消さない。派生平均の stale と reload failure を混同しない |
| F9 | local / remote の Watch | `watch`, `rwatch` | appeared / changed / vanished、mtime が古くても非等値なら変更、複数回の観測（reading）、remote group を検出する。reload は F7 と同じ入口を通る |
| F10 | ファイル形式と adapter | `media`, `fmtgate`, `fmtreg`, `rawrecipe`, `rnpz`, `precision` | 形式の可否判定、bit depth、multi-frame / layer / member identity、RAW recipe、reload と二回目の open による共有が、バックエンド間で一貫する |
| F11 | remote reader / container / measurement | `remote`, `rreader`, `rnpz`, `rmeasure`, `rtemporal`, `rtemporal-png` | 単独実行の peer とプロトコルの可否判定を通り、local と remote の画素・member row・測定値が契約どおり一致する。古いプロトコルや閉じた reader の可否判定は理由付きで拒否する |
| F12 | 解析層と集合解析 | `aset`, `setanalysis`, `detrend`, `seriespanel`, `stackavg`, `lin`, `sweepfile`, `test_prnu` | AnalysisSet の束縛（binding）、層ごとの入力、DSNU / PRNU、detrend、リニアリティ、frame / stack 混合集合の拒否と計算を仕様どおり行う |
| F13 | plugin ABI と由来情報 | `anaprov`, `stackana`, `bundled`, `rplugin`, `rset`, `test_abi_probe`, `test_display_falsecolor` | ABI v3 の frame / stack 登録 API、部分ロード、release、name+version、local / remote の同等性、builtin と plugin の由来情報（provenance）を区別する |
| F14 | export と報告面 | `framestats`, `export`, `export-tsv`, `roi-export` | 画面の数値と export の数値・単位・由来情報が一致し、ROI、stack、series、比較側を取り違えない |
| F15 | UI を伴う機能 | [UI 検証仕様](ui.md) | 操作、レイアウト、表示文字列まで含む項目は UI 仕様の該当行を満たす |

### 4.1 phase④ の受け入れ条件（未実装）

次は現行 selftest の合格条件ではなく、[tasks.csv](../tasks.csv) の #230 phase④ 行を
実装するときに追加する条件である。

- mixed Series の Move / Close は stack と standalone frame の全memberへ作用し、順序・identity・
  同一batch不変条件を保つ。Close後は全memberが消え、無関係controlとrole-refを壊さない
- Reader由来のFrame+Stack mixed SeriesはNPZ/stream双方でkindと順序を保ち、sessionを往復する
- typed ReaderはFrame→CHW、Stack→FCHW、`C=1..4`だけを受け、cross-layer・虚偽layout・
  `C>4`を layer / layout / shape 付きの理由で拒否する。Seriesのkindをshapeから推定しない

## 5. probe の契約

probe（観測点）は、別実装で期待値を再計算するのではなく、製品が実際に使った
identity、状態、文字列を観測する。値を出せるようになっただけでは PASS ではなく、
独立した期待値との比較が必要である。

| probe / 観測面 | 契約 | 主な検証先 |
|---|---|---|
| `srcMapDump()` / `--srcmap-selftest` | `membership` ごとに `uid / seq / idx / src=#K / refs / rev / datarev / shape / bytes / mtime / fsize / name / member / path` を出す。`#K` は同一 dump 内の連番であり、実行ごとに変わる生の `srcId` を期待値にしない | F3–F7 |
| reload selftest の R 群 | 開いている実 stack の下で実ファイルを書き換え、identity、会計、cache、temporal、fit、共有先、失敗状態の保持（latch）をそれぞれ表明する | F7–F9 |
| format registry の F 群 | backend ごとに「登録できる / reload が届く / 二回目 open が共有する」を同じ語彙で表明する | F3、F10 |
| remote 系 selftest | `--remote-exe` の peer を使い、client 内の同一コードを peer の代用にしない。local との一致だけでなく、プロトコルや可否判定による拒否も表明する | F9–F11、F13 |
| export / stats の文字列 | UI や export が実際に生成したヘッダーと由来情報を読む。ソースコード中に定数が存在するだけでは合格にしない | F2、F12、F14 |

## 6. 結果記録の作り方

新しい検証実施（run）は `results/YYYYMMDD-<scope>.md` に分け、少なくとも次を記録する。

- 対象コミットと、比較する場合の基準コミット
- OS / ツールチェーン。実行機の識別情報が未記録なら、その旨を明記する
- 実行コマンドと環境上の制約
- 実施した項番、実際の結果、判定、失敗時の証拠へのパス
- skip / quarantine / 実行不能を含む未実施項目

過去の実施記録は後日の実装状態に合わせて書き換えない。現在の仕様を変えた場合は
本書を更新し、過去結果から本書へのリンクだけを維持する。

## 7. 保存済みの実施記録

- [2026-08-03 ステージ1 機能検証](results/20260803-functional.md)
- [2026-08-04 機能 probe 検証](results/20260804-functional-probes.md)
