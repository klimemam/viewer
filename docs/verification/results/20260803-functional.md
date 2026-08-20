# 2026-08-03 機能検証結果 — ステージ1の不変性

- 対象コミット: `e3883f5`（マージコミット `880d2ae`）
- 比較基準コミット: `e290c77`
- 実行環境: Windows / MinGW（実行機の識別情報は記録されていない）
- 種別: 実施記録。結果は後日の実装状態に合わせて書き換えない。
- 再実行仕様: [../functional.md](../functional.md)

---

# 機能検証マトリクス — frame 参照化 (ステージ1 の不変性と、以降の受入条件)

対象: `verify-func` @ `e3883f5` (merge `880d2ae` = ステージ1)
比較基線: `viewer-wt-verify-base` @ `e290c77` (ステージ1 直前)
仕様: `docs/reference-design.md` (§2 フィールド分割 / §8 表の1行目 = ステージ1 の合格基準)

ステージ1 の合格基準は仕様が一文で決めている — **「既存 suite 全部が無変更で緑」**
(§8 表 1行目「検証」欄)。共有はまだ一つも起きないので、**出力は基線とビット同一で
なければならない**。本書はそれを「suite が緑」より強く、**数値の A/B 差分ゼロ**まで
落として測る。

## 結論 (2026-08-03 実施)

- **suite**: `SUITE TOTAL c:/Users/hish/Desktop/viewer-wt-verify pass=21 fail=0`
- **A/B 数値不変性**: `AB SUMMARY identical=24 differing=1 rc_differing=0 missing=0`。
  唯一の相違 `browsekeys` は非同期待ちの進捗行のみで、**基線を自分自身と比べたときの
  ゆらぎのほうが大きい** (A-8)。**ステージ1 に帰属する数値 drift はゼロ。**
- **校正**: 捕捉ファイルへ 1 バイト (`4096`→`4097`) と有効数字5桁目 (`8.00031`→`8.00032`)
  の偽差を注入し、harness が両方を検出することを確認 (A-11)。この検査は失敗し得る。
- **測定不変条件**: 10件すべて実出力上で成立、基線と一致 (B 節)。
- **新規の欠陥**: **無し**。落ちたものは既知2件 (E-4 の A2、E-5 の layout.ini) のみ。

| 節 | 項目数 | 実施 | PASS | FAIL | 要probe | 既知 | 未実施 |
|---|---|---|---|---|---|---|---|
| A ステージ1 不変性 | 16 | 16 | 14 | 0 | 1 (A-12) | 1 (A-10) | 0 |
| B 測定不変条件 | 10 | 10 | 10 | 0 | 0 | 0 | 0 |
| C ステージ2/3/4/5 受入 | 17 | 0 | 0 | 0 | 0 | 0 | 17 |
| D 穴 (probe 提案) | 7 | 0 | 0 | 0 | 7 | 0 | 0 |
| **合計 (A-D)** | **50** | **26** | **24** | **0** | **8** | **1** | **17** |

(E 節の既知6件は検証項目ではなく記録。A-10 は「既知の失敗が基線と同一に再現する」ことの
確認なので、不変性としては合格・項目としては既知に数えた。)

## 0. この機械で「できないこと」を先に書く

- **OpenGL のスクリーンショットが撮れない** (クライアント領域が白く出る)。
  よって本書に**視覚確認は一件も無い**。判定はすべて CLI フラグ・状態表明・
  stderr・出力ファイルに依る。描画の見た目に関する項目は D 節で 要probe とする。
- `srcId` / `use_count` / `rev` / `mtime` / `fsize` を**印字する経路がコードに無い**
  (D-1 で詳述)。共有の有無を外から直接主張することは今日できない。

## 1. 方法 — A/B 出力差分の作り方と、その決定性の担保

`tools/verify/ab_capture.sh` が同一のコマンド列を1つのビルドに対して流し、
`tools/verify/ab_diff.sh` が2つの捕捉ディレクトリを突き合わせる。

**決定性の担保 (どれも「便利だから」ではなく、偽の差分を潰すために要る)**

| 担保 | 理由 |
|---|---|
| cwd を共有の中立ルート (`scratchpad/verifyagent_ab/root`) に固定。worktree では走らせない | 相対 fixture パスが両ビルドで**同じ文字列**になる。worktree で走らせるとパスに `-base` が混じり、差分が出力ではなくパスから出る |
| `APPDATA`・`HOME`・`USERPROFILE` を**パスごとに新規の空ディレクトリ**へ固定 | selftest は実ユーザの `%APPDATA%/viewer/{layout.ini,prefs.txt}` を読み、さらに **`io.IniFilename` が無防備なため実ユーザの `layout.ini` へ書き戻す** (E-5、main で修正中)。実測: 本検証中、**別エージェントの worktree `viewer-wt-verifyui` が同一の `--browse-keys-selftest` を同時実行していた**。固定しなければ隣の書き込みが偽の差分を作り、こちらの実行が操作者の状態を壊す |
| cwd の `imgui.ini`/`layout.ini` をパス開始時に削除 | 共有の実行ルートに前のパスが置いた ini を次のパスが読むと、B は A と違う初期状態から始まる |
| `TMP`/`TEMP` をパスごとに固定 | selftest が `temp_directory_path()` へ書く `.vsession` (`viewer_batchselftest.vsession` 他) がパスの中に落ちる。**同機の兄弟エージェントが同じ固定名を共有 temp に書く**ので、固定しなければ往復の対象が入れ替わり得る。※実体は selftest が終了時に自分で削除するため成果物としては残らず、往復の証跡は A-13 の表明行のほうで取る |
| 基線→ステージ1 の順で**逐次**実行 | 兄弟エージェントが同機で走る。並列にすると selftest 内部の壁時計予算 (60s/300s) を踏んで偽 FAIL になる |

**正規化 (適用した規則の全部。これ以外は本物の差分として扱う)**

| # | 規則 | 根拠 |
|---|---|---|
| N1 | `...viewer-serve.exe` の絶対パス → `<SERVE_EXE>` | `--remote-selftest` は各ビルド**自身**の serve を起動する。引数が構造上異なるだけで挙動差ではない |
| N2 | パスごとの `APPDATA`/`TMP`/捕捉ディレクトリの絶対パス → `<CAPDIR>`/`<PASS>` | 上の担保で意図的に分けたディレクトリ名 |
| N3 | `<数値> ms` / `<数値> MB/s` / `in <数値> s` → `<TIME>` | 壁時計。実測で `browse` が `in 0.07 s` / `in 0.09 s` と揺れた。**`%.2f MB on the wire` は正規化しない** — あれは通信量で決定的、比較対象に残す |
| N4 | `%p` の生ヒープアドレス → `<PTR>`。ただし**全ゼロは `<NULLPTR>`** に分けて残す | ASLR で値は実行固有だが、**null か否かは挙動**。実測で `abstats` の `slot 1 held 0000019fbe321270` は毎回変わる一方、`hist[1].img=0000000000000000` は両ビルドで一致した — この区別を潰さない |
| N5 | `generated: YYYY-MM-DD HH:MM:SS` → `<TIMESTAMP>` | `--export-tsv-selftest` の来歴ヘッダが実時刻を書く (`# app: viewer 0.1 \| generated: 2026-08-03 00:46:35`) |

正規化は **N1–N5 だけ**。数値 (mean/sigma/fit/バイト数/枚数) には一切触れていない。
N3–N5 は**先に無正規化で diff を取り、実行固有だと現物で確かめてから**足した規則で、
「合わせるために」置いたものは一つも無い。

## 2. 判定語

`PASS` = 期待どおり / `FAIL` = 期待と異なる (要修正) / `未実施` = このランでは走らせて
いない / `要probe` = 観測手段がコードに無く、作らないと測れない / `既知` = ステージ1
以前から在る欠陥で、仕様が「今は直さない」と決めているもの。

## A. ステージ1 の挙動不変性 (このランの本体 — 全件実施)

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| A-1 | 両 worktree がビルドできる | 環境節のレシピを両方で | どちらも成功 | 両方成功。`viewer.exe` 7,844,350 B (stage1) / 7,836,551 B (base)。`viewer-serve.exe` は**両方 3,354,273 B で同一サイズ** (serve.cpp 無変更と整合) | PASS |
| A-2 | **§8 表1行目の合格基準そのもの**: 既存 suite 21件が無変更で緑 | `bash "$SP/integ_suite_orchestrator.sh" c:/Users/hish/Desktop/viewer-wt-verify` | pass=21 fail=0 | `SUITE TOTAL c:/Users/hish/Desktop/viewer-wt-verify pass=21 fail=0` (90秒。`APPDATA`/`TMP` を固定して実行 — 理由は A-16) | PASS |
| A-3 | CLI 表面が基線と同一 | 両 `core/main.cpp` から `"--..."` 文字列を抽出して diff | 増減ゼロ | **52 フラグ / うち selftest 22件、diff 空**。フラグの追加も削除も無い | PASS |
| A-4 | 利用者に見える文言が一つも変わっていない | 両ソースの4文字以上の文字列リテラルを出現数つきで diff | 変化はフィールド移動に伴う機械的なものだけ | 差分は**セッション書き出しのストリーム式が `d->rawLE`→`d->src->rawLE` 等に変わった4件**と、**コメント内の引用句2件** (`"reload"` 2440行, `"this frame as seen by this stack"` 190行) のみ。**表示文言の変更はゼロ**。ゆえに以降の出力差分は「文言の編集」ではなく**計算値のdrift**しか有り得ない | PASS |
| A-5 | selftest 22件すべてを suite 経由でなく**直接**実行 | `ab_capture.sh` が中立ルートで25コマンドを実行 | 全件 rc=0 (既知失敗の A-10 を除く) | 全件 rc=0。終行例: `seriesselftest: ok` / `browsekeys: 241 action(s) through real frames, no crash, 0 panel check(s) failed: ok` / `verifyselftest: ok` / `deriveselftest: ok` / `texportselftest: ok` / `framelinselftest: ok` / `newwinselftest: ALL PASS` / `selftest: FRAME_ROI_STATS over 120 frames: ok` / `sweepfile: ok` / `linselftest: snr analytic check (linset): worst \|signal err\| 0.175%, worst \|SNR err\| 1.019% (tol 2% / 3%) ok` | PASS |
| A-6 | 終了コードが基線と全件一致 | `ab_diff.sh` の rc 比較 | 25件すべて一致 | `rc_differing=0` (25件中0件相違)。既知失敗も**両方 rc=1** | PASS |
| A-7 | **正規化ゼロ**での生バイト一致 | 正規化を一切かけずに全捕捉を diff | 実行固有ノイズを持つものを除き一致 | **25件中20件が正規化なしでバイト完全一致**。相違は `abstats` `abstatscfa` `browse` `browsekeys` `texport` の5件のみ | PASS |
| A-8 | **ノイズ床の測定 (対照実験)** — 相違5件は本当に実行固有か | 基線ビルドを**同一環境でもう一度**流し、基線 vs 基線を diff | 同じ5件が自分自身とも食い違うはず | **同じ5件がすべて自己不一致**。しかも `browsekeys` は自己比較のほうが churn が大きい (自己10行 vs A/B 8行) — 自己比較では**表明行まで**揺れた (`expanded on 0/24` vs `0/26 watched frame(s)`)。すなわち**ステージ1 の差は基線自身の実行間ゆらぎより小さい** | PASS |
| A-9 | **数値 A/B 不変性 (このステージ最強の検査)** | `ab_diff.sh capA capB` (正規化 N1-N5) | 数値差分ゼロ | `AB SUMMARY identical=24 differing=1 rc_differing=0 missing=0`。残る1件 `browsekeys` の相違は**`waitdir`/`waitimg` の進捗行4+4行のみ** (非同期条件の待ち中に毎フレーム出るスナップショット)。**表明行234行はバイト完全一致**。基線の自己比較(A-8)は同じ場所でより大きく揺れる。**判定: ステージ1 に帰属する数値 drift はゼロ** | PASS |
| A-10 | 既知の A2 失敗が**同一に**再現する | `viewer.exe --abstats-selftest tools/testdata/multi --cfa bayer --bayer-pattern RGGB` | 基線と同じ行が同じように落ちる | 両ビルドとも rc=1、同一行: `abstatsselftest: A2 B sigma_t matches within 1e-6 relative              FAIL` (前後の `A2 B histogram bins match…PASS` / `A2 B mean/sd match…PASS` も一致)。**新規発見ではない (E-4)**。挙動不変という意味では合格 | 既知 / 挙動不変は PASS |
| A-11 | **校正 — この検査は失敗し得るか** | 捕捉**出力ファイル**に偽の差を注入 (製品コードには一切触れない): `verify.out` の `resident 0 -> 4096 B`→`4097 B`、`sweepfile.out` の `sens=8.00031`→`8.00032` | harness が両方を検出する | 検出した。`AB SUMMARY identical=22 differing=3` (24/1 から悪化) し、両行を名指しで出力: `< …resident 0 -> 4096 B…` / `> …resident 0 -> 4097 B…`、`< …sens=8.00031…` / `> …sens=8.00032…`。**1バイトの会計差と有効数字5桁目の差の両方を捕まえる** | PASS |
| A-12 | npz の Watch 基線 (ステージ1 の新規) | コード読み + 実行経路の探索 | `statSourceFile` が npy/raw/npz の3つのローカル復号経路を**過不足なく**覆う | `statSourceFile` は `core/main.cpp:173` に1つ。呼び出しは**ちょうど3箇所** — `3666` (npz, `loadNpz` 内)、`3936` (npy, `loadNpyFrame`)、`4091` (raw)。**漏れている復号経路は無い**。montage/processor/derive/remote は非ファイル源なので 0 のままが正しい。ただし **`mtime`/`fsize` を印字する経路がコードに存在せず、実行による確認はできない** → D-2 | 要probe (コード読みは充足) |
| A-13 | セッション往復が基線と同一 | 既存 suite の機構 (`--batch-selftest`/`--verify-selftest`/`--series-selftest` が `temp_directory_path()` へ `.vsession` を書いて読み直す) を、パスごとに固定した `TMP` 上で実行 | 往復結果が基線と一致 | 3件とも A/B **バイト完全一致**。`batchselftest: reloaded session: 15 image(s), 3 stack(s), batch 'moved' restored, 0 stray frame(s)` / `verifyselftest: V15 renamed a folder stack '25C dark', saved and reloaded: 3 stack(s) [25C dark, 01/frame_000‥004.npy, 02/frame_000‥004.npy]` / `seriesselftest: session reloaded ok`・`a folder stack's user-given name survives the session ok`・`legacy (seqlevel) session loaded ok`。※ `.vsession` 実体は selftest が終了時に削除するため成果物としては残らない | PASS |
| A-14 | §7 のバイト会計 (`residentImageBytes` を srcId 和へ書き換えた箇所) が不変 | `--verify-selftest` の V14 が実バイト数を印字する | 基線と同一の数値 | A/B 一致: `verifyselftest: V14 after one stack open with nothing pumped: resident 0 -> 4096 B, claimed 0 -> 32768 B, in flight 28672 B (7 job(s))`、および `V14 after draining: in flight 0 B, claimed == resident: 1`。**共有がまだ無いので srcId 和は従来の和と同値** — 仕様の「結果は不変」を実測で確認 | PASS |
| A-15 | suite が**一度も走らせていない** 22番目の selftest | `viewer.exe --sweepfile-selftest levelfiles` (`levelfiles` は注入済み真値 sens 8.0 DN/lx, offs 64.0 DN, K 2.0, read 3.0 を持つ fixture) | fit が真値を復元し、基線と一致 | A/B **バイト完全一致**、`sweepfile: ok`。`sweepfile: fit: 7 point(s) sens=8.00031 offs=64.0674 r2=0.99999984 LEmax=0.1565 K=2.0209 read=3.0019 (dark stack)` — **注入した真値を復元**。suite の21件を超える新規カバレッジ | PASS |
| A-16 | 最初の suite 実行が `--browse-keys-selftest` で9分以上停止した件の切り分け | 実ユーザ状態のまま実行 → 停止。`APPDATA` を使い捨てに固定して単独実行 | ステージ1 の欠陥か環境かを判定する | **ステージ1 の欠陥ではない**。固定後は **35秒 / rc=0** で完走 (`241 action(s) through real frames, no crash, 0 panel check(s) failed: ok`)。原因は E-5 (scripted selftest が実ユーザの `layout.ini` を書く既知欠陥) + 別エージェントの worktree `viewer-wt-verifyui` が**同一テストを同時実行**して同じ状態を奪い合ったこと。なお当該テストの待ち段階には**壁時計の上限が無い** (listing 段階のみ60秒。`core/main.cpp:28658`)、ゆえに停止は無限に続き得る | PASS (切り分け完了) / 原因は E-5 |

### A 節の証跡について

比較したコマンドは25件 — suite の21件 (中立ルートのパスに置換) + `--sweepfile-selftest`
+ `--scan-selftest levelfiles` + `--abstats-selftest --cfa bayer` (既知失敗) + `--help`。
両ビルドとも**同一の cwd・同一の fixture 実体**に対して実行しているので、パス文字列は
比較対象にならない。

## B. 測定不変条件 (どのステージでも跨いではならない — 全件実施)

いずれも「捕捉した実出力に、その不変条件を述べる文字列が実在するか」で判定する。
`tools/verify/invariants.sh <capdir>` が実行する。**基線・ステージ1 の両方で pass=10 fail=0。**

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| B-1a | σ_t は **stack** の性質であって frame の性質ではない | `invariants.sh` (対象: `--export-tsv-selftest` の出力) | 出力が σ_t を stack 統計として名乗る | 実出力に存在: `# == temporal summary (stack statistics; sigma_t is a property of the stack, never of a frame) ==` | PASS |
| B-1b | ゆえに**毎フレーム表に σ_t 列があってはならない** | 同上 (対象: `--framestats-selftest`) | frame 表に σ_t 列が無い | 無い。ヘッダは `frame	file	mean [DN]	sigma [DN]	sigma_col [%]	sigma_row [%]` | PASS |
| B-1c | 毎フレーム表のヘッダ形が期待どおり | 同上 | mean/sigma 形 | 上記ヘッダで一致 | PASS |
| B-2a | CFA プレーンを混ぜない | 同上 (mosaic 有効の TSV) | 列がプレーンごとに分かれる | `frame	file	mean_R [DN]	sigma_R [DN]	…	mean_Gr [DN]	…	mean_Gb [DN]	…	mean_B [DN]	…` — **R/Gr/Gb/B が独立列**。プールされた列は無い | PASS |
| B-2b | 片側の σ_t を他方に流用しない | 同上 | B 側は B 側の数 | `texportselftest: E9 B's sigma_t is temporal[1]'s number, not A's relabelled PASS` | PASS |
| B-3a | 部分ロードは n/N で言う | 同上 | 素の枚数でなく n of N | `# -- side A: 00/frame_000‥004.npy  \|  region: whole frame (80x64)  \|  resident 5 of 5 frame(s) --` | PASS |
| B-3b | 集計表自体が n と N の列を持つ | 同上 | n・N 列が在る | `side	ch	n	N	sigma_t [DN]	sigma_fpn [DN]	sigma_tot [DN]	source` | PASS |
| B-4a | 軸ラベルは量と単位を運ぶ | 同上 | ノイズ量に [DN] | `sigma_t [DN]	sigma_fpn [DN]	sigma_tot [DN]` | PASS |
| B-4b | 相対量は [%] で、[DN] と区別される | 同上 | [%] が別に在る | `sigma_frame [%]` / `sigma_col [%]` / `sigma_row [%]` が [DN] 列と併存 | PASS |
| B-5 | 画素値は [DN] | 同上 | mean が [DN] | `mean [DN]` | PASS |

**A/B 不変性**: これら10件の根拠となる `texport` / `framestats` の捕捉は
**両ビルドでバイト完全一致** (A-7)。すなわち不変条件はステージ1 を跨いで動いていない。

## C. ステージ2+3・ステージ5 の受入行 (未実施 — 未マージ)

仕様 §8 の表が各ステージの「検証」欄に書いた内容を、**実際に叩けるコマンド**まで
降ろしたもの。マージされたら本書のこの節をそのまま実行する。

### C-1 ステージ2 (共有が起き得るようにする — registry / Files の共有印 / close の生存 / compare の same-pixels 文)

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| C1-1 | 同じフォルダを2回開くと source が共有される | `viewer.exe --verify-selftest tools/testdata/multi` (拡張後) | 2回目の open 後、両 stack の frame が同一 `srcId` を指す旨を selftest が印字し ok | — | 未実施(未マージ) |
| C1-2 | 共有時バイトは1倍 | 同上。`residentImageBytes()` は既に srcId 和 (`core/main.cpp:5423-5433`) | 2回開いても resident バイトが1回分のまま (2倍にならない) | — | 未実施(未マージ) |
| C1-3 | 片方の batch を閉じても画素が残る | 同上 | 片側 close 後も残った membership が画素を読めて統計が出る | — | 未実施(未マージ) |
| C1-4 | Ctrl+Alt+W が membership だけを消す | 同上 | 閉じた側の doc だけ消え、source は生存 (最後の参照で死ぬ = §4) | — | 未実施(未マージ) |
| C1-5 | compare が「同じ画素を見ている」と言う | 同上 | A と B が同一 source のとき `A and B share the same pixels` 相当の一文 (§3.1) | — | 未実施(未マージ) |

### C-2 ステージ3 (derive をコピーから参照へ)

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| C2-1 | 既存の derive 表明が無傷 | `viewer.exe --derive-selftest tools/testdata` | 既存の D0/D1/D3/D4/D7 (両方向の数・seqIndex 順・元 stack 無傷・follow 収束) が今日と同じ値で ok | — | 未実施(未マージ) |
| C2-2 | 派生がバイトを増やさない | 同上 (新規表明) | derive 前後で resident バイトが不変 | — | 未実施(未マージ) |
| C2-3 | 派生後に元を crop しても派生は変わらない (CoW) | 同上 (新規表明) | `cropInPlace` の `use_count() > 1` 分岐 (`core/main.cpp:4127`) が発火し、派生側の画素・統計が不変 | — | 未実施(未マージ) |

### C-3 ステージ5 (reload の walk / 鍵の stackRev / temporalExtra / 手動 Reload)

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| C3-1 | 画素差し替えで uid が不変 | `viewer.exe --reload-selftest <dir>` (新設予定) | reload 後も membership の uid が同じ (compare のピンが外れない) | — | 未実施(未マージ) |
| C3-2 | 全キャッシュが再計算される | 同上 | hist/proj/ROI/diff/autoRange が `dataRev` で鍵から外れる (§3.2) | — | 未実施(未マージ) |
| C3-3 | σ_t が変わる (古い値を返さない) | 同上 | **D-3 の既知穴の回帰テスト**: 枚数も ROI も変えずに画素だけ差し替え、σ_t が新しい値になる | — | 未実施(未マージ) |
| C3-4 | fit が落ちる | 同上 | `linFitStale()` + 係る series の fit 破棄 | — | 未実施(未マージ) |
| C3-5 | 共有先 stack も同じ一歩で更新される | 同上 | source を持つ**全** membership が walk される | — | 未実施(未マージ) |
| C3-6 | `temporalExtra` も忘れられる | 同上 | **D-4 の既知穴の回帰テスト**: スロット C/D/E の temporal も落ちる | — | 未実施(未マージ) |
| C3-7 | 手動 Reload from disk | Files 右クリック → Reload | GUI 操作。この機械では**スクリーンショット不可**のため selftest 側に表明が要る | — | 未実施(未マージ) + 要probe |

### C-4 ステージ4 (セッションの明示メンバー) — 参考

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| C4-1 | 派生 stack の保存→全撤去→復元でメンバー一致 | `--derive-selftest` 拡張 | §5.1 の既存欠陥 (D-5) の回帰テスト。`stackmember`/`stackrule` が書かれ読まれる | — | 未実施(未マージ) |
| C4-2 | 旧キーのみのセッションが今日と同じに開く | `--series-selftest` の legacy セッション経路 (`viewer_serieslegacy.vsession`) | 未知キー読み飛ばしで劣化なく開く | — | 未実施(未マージ) |

## D. 穴 — 今日は観測手段が無い項目 (probe の提案。実装はしない)

| 項番 | 観測したいこと | なぜ今日測れないか (コードの実測) | 提案する probe |
|---|---|---|---|
| D-1 | 2つの doc が1つの source を共有しているか | `use_count()` はファイル全体で**1箇所** (`core/main.cpp:4127`、crop の CoW 判定) でしか読まれず、印字されない。`srcId` の読み手も `residentImageBytes()` (`5431`) の重複排除のみで、値を外へ出さない。`%p` による source ポインタの印字もゼロ | `--srcmap-selftest <dir>`: 開いた全 doc について `uid seqId seqIndex srcId rev use_count` を1行ずつ stderr に出して exit。ステージ2 の共有・ステージ3 の derive 参照・ステージ5 の walk が**1つの probe で**測れるようになる |
| D-2 | Watch 基線 (`mtime`/`fsize`) が実際に入っているか | `statSourceFile` (`core/main.cpp:173-180`) は `FrameSource::mtime/fsize` を書くだけで、**印字経路が無い**。stderr に出る `mtime` はリモート LIST 応答の別構造体 (`20112-20121`) で、`FrameSource` のものではない | D-1 の probe 行に `mtime fsize` を足す。これが無い限り「npz にも基線が入った」は**コード読みでしか**言えない (本ランはそうした — A-12) |
| D-3 | σ_t の鍵に `dataRev`/`stackRev` が無い件 | `TemporalState` の鍵は `seqId/frames/ROI/cfa/cfaPattern/nPl` のみ (`core/main.cpp:1098-1113`)。今日は `forgetImage` が毎回リセットするので露出しない | ステージ5 の `--reload-selftest` (C3-3)。**既知**: 仕様 §3.2-1 が「ステージ5 で塞ぐ」と明記 |
| D-4 | `forgetImage` が `temporalExtra` を忘れない件 | `forgetImage` (`core/main.cpp:2232-2243`) は `k<2` で `temporal[0..1]` だけを触り、`app.temporalExtra` (`1117`) に手を出さない | ステージ5 の `--reload-selftest` (C3-6)。**既知**: 仕様 §3.2-2 |
| D-5 | derive stack のセッション往復 | `writeSessionTo` にメンバーシップを書く行が無い (仕様 §5.1)。保存時点で膜だけになる | ステージ4 (C4-1)。**既知**: 仕様 §5.1 が「新形式はこれを直すものでもある」と明記 |
| D-6 | 描画・テクスチャ・VRAM・Files の共有印の見た目 | この機械は OpenGL のクライアント領域が白く出てスクリーンショットが撮れない | 表示文字列を stderr にも吐く selftest (既存 `--browse-keys-selftest` と同型のスクリプト化クリック)。**視覚確認は本書の範囲外** |
| D-7 | `rev` が preview→full 差し替えで +1 されているか | `S.rev++` (`core/main.cpp:2440`) はステージ1 唯一の rev 書き手だが、印字経路が無い | D-1 の probe 行に `rev` を含める (提案済み) |

## E. 既知 — 本ランの発見ではないもの

| 項番 | 内容 | 根拠 |
|---|---|---|
| E-1 | `TemporalState` の鍵に `dataRev` が無い | 仕様 §3.2-1。ステージ5 で塞ぐ。コードで再確認 (`core/main.cpp:1098-1113`) |
| E-2 | `forgetImage` が `temporalExtra` を忘れる | 仕様 §3.2-2。ステージ5 で塞ぐ。コードで再確認 (`core/main.cpp:2232-2243`) |
| E-3 | derive stack がセッションを正しく往復しない | 仕様 §5.1。ステージ4 で塞ぐ |
| E-4 | `--abstats-selftest` の A2 が `--cfa bayer` で落ちる | 本ラン以前からの既知失敗。A-10 で**基線と同一に落ちる**ことを確認済み (挙動不変) |
| E-5 | scripted selftest が `io.IniFilename` を塞がないまま走り、**操作者の実 `%APPDATA%/viewer/layout.ini` を書く** | 別の UI 検証エージェントが確認・記録済みで、**main で修正中** (既知(修正中))。本ランはこれを独立に踏んだ — A-16 の9分停止の原因。本書の harness は `APPDATA`/`HOME`/`USERPROFILE` を使い捨てへ向けて回避している |
| E-6 | `--browse-keys-selftest` の待ち段階に壁時計の上限が無い | listing 段階のみ60秒の脱出があり (`core/main.cpp:28658`)、その後の action 段階は `reproIdle` の計数だけで進む。条件が成立しなければ停止は無限に続く。E-5 と組むと A-16 の形になる。**ステージ1 とは無関係**の既存性質 |
