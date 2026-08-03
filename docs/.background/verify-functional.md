現行ドキュメント: [verify-functional.md](../verify-functional.md) の背景 — ステージ1 マージ直後に回した A/B 不変性ランの実施記録。

# 機能検証マトリクスの実施記録 (2026-08-03 実施)

## 対象としたラン

対象: `verify-func` @ `e3883f5` (merge `880d2ae` = ステージ1)
比較基線: `viewer-wt-verify-base` @ `e290c77` (ステージ1 直前)

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

## A. ステージ1 の挙動不変性 (このランの本体 — 全件実施)

| 項番 | 実施結果 | 判定 |
|---|---|---|
| A-1 | 両方成功。`viewer.exe` 7,844,350 B (stage1) / 7,836,551 B (base)。`viewer-serve.exe` は**両方 3,354,273 B で同一サイズ** (serve.cpp 無変更と整合) | PASS |
| A-2 | `SUITE TOTAL c:/Users/hish/Desktop/viewer-wt-verify pass=21 fail=0` (90秒。`APPDATA`/`TMP` を固定して実行 — 理由は A-16) | PASS |
| A-3 | **52 フラグ / うち selftest 22件、diff 空**。フラグの追加も削除も無い | PASS |
| A-4 | 差分は**セッション書き出しのストリーム式が `d->rawLE`→`d->src->rawLE` 等に変わった4件**と、**コメント内の引用句2件** (`"reload"` 2440行, `"this frame as seen by this stack"` 190行) のみ。**表示文言の変更はゼロ**。ゆえに以降の出力差分は「文言の編集」ではなく**計算値のdrift**しか有り得ない | PASS |
| A-5 | 全件 rc=0。終行例: `seriesselftest: ok` / `browsekeys: 241 action(s) through real frames, no crash, 0 panel check(s) failed: ok` / `verifyselftest: ok` / `deriveselftest: ok` / `texportselftest: ok` / `framelinselftest: ok` / `newwinselftest: ALL PASS` / `selftest: FRAME_ROI_STATS over 120 frames: ok` / `sweepfile: ok` / `linselftest: snr analytic check (linset): worst \|signal err\| 0.175%, worst \|SNR err\| 1.019% (tol 2% / 3%) ok` | PASS |
| A-6 | `rc_differing=0` (25件中0件相違)。既知失敗も**両方 rc=1** | PASS |
| A-7 | **25件中20件が正規化なしでバイト完全一致**。相違は `abstats` `abstatscfa` `browse` `browsekeys` `texport` の5件のみ | PASS |
| A-8 | **同じ5件がすべて自己不一致**。しかも `browsekeys` は自己比較のほうが churn が大きい (自己10行 vs A/B 8行) — 自己比較では**表明行まで**揺れた (`expanded on 0/24` vs `0/26 watched frame(s)`)。すなわち**ステージ1 の差は基線自身の実行間ゆらぎより小さい** | PASS |
| A-9 | `AB SUMMARY identical=24 differing=1 rc_differing=0 missing=0`。残る1件 `browsekeys` の相違は**`waitdir`/`waitimg` の進捗行4+4行のみ** (非同期条件の待ち中に毎フレーム出るスナップショット)。**表明行234行はバイト完全一致**。基線の自己比較(A-8)は同じ場所でより大きく揺れる。**判定: ステージ1 に帰属する数値 drift はゼロ** | PASS |
| A-10 | 両ビルドとも rc=1、同一行: `abstatsselftest: A2 B sigma_t matches within 1e-6 relative              FAIL` (前後の `A2 B histogram bins match…PASS` / `A2 B mean/sd match…PASS` も一致)。**新規発見ではない (E-4)**。挙動不変という意味では合格 | 既知 / 挙動不変は PASS |
| A-11 | 検出した。`AB SUMMARY identical=22 differing=3` (24/1 から悪化) し、両行を名指しで出力: `< …resident 0 -> 4096 B…` / `> …resident 0 -> 4097 B…`、`< …sens=8.00031…` / `> …sens=8.00032…`。**1バイトの会計差と有効数字5桁目の差の両方を捕まえる** | PASS |
| A-12 | `statSourceFile` は `core/main.cpp:173` に1つ。呼び出しは**ちょうど3箇所** — `3666` (npz, `loadNpz` 内)、`3936` (npy, `loadNpyFrame`)、`4091` (raw)。**漏れている復号経路は無い**。montage/processor/derive/remote は非ファイル源なので 0 のままが正しい。ただし **`mtime`/`fsize` を印字する経路がコードに存在せず、実行による確認はできない** → D-2 | 要probe (コード読みは充足) |
| A-13 | 3件とも A/B **バイト完全一致**。`batchselftest: reloaded session: 15 image(s), 3 stack(s), batch 'moved' restored, 0 stray frame(s)` / `verifyselftest: V15 renamed a folder stack '25C dark', saved and reloaded: 3 stack(s) [25C dark, 01/frame_000‥004.npy, 02/frame_000‥004.npy]` / `seriesselftest: session reloaded ok`・`a folder stack's user-given name survives the session ok`・`legacy (seqlevel) session loaded ok`。※ `.vsession` 実体は selftest が終了時に削除するため成果物としては残らない | PASS |
| A-14 | A/B 一致: `verifyselftest: V14 after one stack open with nothing pumped: resident 0 -> 4096 B, claimed 0 -> 32768 B, in flight 28672 B (7 job(s))`、および `V14 after draining: in flight 0 B, claimed == resident: 1`。**共有がまだ無いので srcId 和は従来の和と同値** — 仕様の「結果は不変」を実測で確認 | PASS |
| A-15 | A/B **バイト完全一致**、`sweepfile: ok`。`sweepfile: fit: 7 point(s) sens=8.00031 offs=64.0674 r2=0.99999984 LEmax=0.1565 K=2.0209 read=3.0019 (dark stack)` — **注入した真値を復元**。suite の21件を超える新規カバレッジ | PASS |

### A-16 — 9分停止の切り分け (項目ごと背景に置く)

| 項番 | 検証項目 | 手順(コマンド) | 期待結果 | 実施結果 | 判定 |
|---|---|---|---|---|---|
| A-16 | 最初の suite 実行が `--browse-keys-selftest` で9分以上停止した件の切り分け | 実ユーザ状態のまま実行 → 停止。`APPDATA` を使い捨てに固定して単独実行 | ステージ1 の欠陥か環境かを判定する | **ステージ1 の欠陥ではない**。固定後は **35秒 / rc=0** で完走 (`241 action(s) through real frames, no crash, 0 panel check(s) failed: ok`)。原因は E-5 (scripted selftest が実ユーザの `layout.ini` を書く既知欠陥) + 別エージェントの worktree `viewer-wt-verifyui` が**同一テストを同時実行**して同じ状態を奪い合ったこと。なお当該テストの待ち段階には**壁時計の上限が無い** (listing 段階のみ60秒。`core/main.cpp:28658`)、ゆえに停止は無限に続き得る | PASS (切り分け完了) / 原因は E-5 |

### A 節の証跡について

比較したコマンドは25件 — suite の21件 (中立ルートのパスに置換) + `--sweepfile-selftest`
+ `--scan-selftest levelfiles` + `--abstats-selftest --cfa bayer` (既知失敗) + `--help`。
両ビルドとも**同一の cwd・同一の fixture 実体**に対して実行しているので、パス文字列は
比較対象にならない。

## B. 測定不変条件 (どのステージでも跨いではならない — 全件実施)

**基線・ステージ1 の両方で pass=10 fail=0。**

| 項番 | 実施結果 | 判定 |
|---|---|---|
| B-1a | 実出力に存在: `# == temporal summary (stack statistics; sigma_t is a property of the stack, never of a frame) ==` | PASS |
| B-1b | 無い。ヘッダは `frame	file	mean [DN]	sigma [DN]	sigma_col [%]	sigma_row [%]` | PASS |
| B-1c | 上記ヘッダで一致 | PASS |
| B-2a | `frame	file	mean_R [DN]	sigma_R [DN]	…	mean_Gr [DN]	…	mean_Gb [DN]	…	mean_B [DN]	…` — **R/Gr/Gb/B が独立列**。プールされた列は無い | PASS |
| B-2b | `texportselftest: E9 B's sigma_t is temporal[1]'s number, not A's relabelled PASS` | PASS |
| B-3a | `# -- side A: 00/frame_000‥004.npy  \|  region: whole frame (80x64)  \|  resident 5 of 5 frame(s) --` | PASS |
| B-3b | `side	ch	n	N	sigma_t [DN]	sigma_fpn [DN]	sigma_tot [DN]	source` | PASS |
| B-4a | `sigma_t [DN]	sigma_fpn [DN]	sigma_tot [DN]` | PASS |
| B-4b | `sigma_frame [%]` / `sigma_col [%]` / `sigma_row [%]` が [DN] 列と併存 | PASS |
| B-5 | `mean [DN]` | PASS |

**A/B 不変性**: これら10件の根拠となる `texport` / `framestats` の捕捉は
**両ビルドでバイト完全一致** (A-7)。すなわち不変条件はステージ1 を跨いで動いていない。

## C. ステージ2+3・ステージ5 の受入行 (未実施 — 未マージ)

| 項番 | 実施結果 | 判定 |
|---|---|---|
| C1-1 | — | 未実施(未マージ) |
| C1-2 | — | 未実施(未マージ) |
| C1-3 | — | 未実施(未マージ) |
| C1-4 | — | 未実施(未マージ) |
| C1-5 | — | 未実施(未マージ) |
| C2-1 | — | 未実施(未マージ) |
| C2-2 | — | 未実施(未マージ) |
| C2-3 | — | 未実施(未マージ) |
| C3-1 | — | 未実施(未マージ) |
| C3-2 | — | 未実施(未マージ) |
| C3-3 | — | 未実施(未マージ) |
| C3-4 | — | 未実施(未マージ) |
| C3-5 | — | 未実施(未マージ) |
| C3-6 | — | 未実施(未マージ) |
| C3-7 | — | 未実施(未マージ) + 要probe |
| C4-1 | — | 未実施(未マージ) |
| C4-2 | — | 未実施(未マージ) |
