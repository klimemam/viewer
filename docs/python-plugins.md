# plugin を Python (numpy / torch) で書けるか — 測って決める

**これは検討であって実装ではない。** 製品コードは1行も書いていない。書いたのは
`tools/pyplugin-bench/` のベンチマークだけで、これは viewer のビルドに入らない
(§10 で byte 単位で確認した)。

議論の出発点 (ユーザー、2026-08-03):

> plugin も，python(numpy, torch)で書きたいなぁ．C++で書く場合もあるけど，
> python も使いたい．

**結論を先に言う。** 推奨は **A: 常駐 Python worker + 共有メモリ**。
`docs/import-adapters.md` §4 で決めた機構 (起動・信頼・エディタ・パス記憶・
`Values`・peer 実行) を**そのまま全部使い、運び屋だけを npz から共有メモリに
替える**。測った値で言うと: adapter の運び方 (プロセス起動 + npz) は
**1回 180〜190 ms** で、同梱の `noise/floor` が全面でやる仕事 (23 ms) の**8倍**。
常駐 worker + 共有メモリなら**同じ呼び出しが 0.03〜0.04 ms** で、**4700 分の1**になる。

---

## 1. 今なにが本当か

推奨の前に、コードを読んで確認した事実。**推測ではなく行番号のあるもの**だけを書く。

| # | 事実 | 場所 |
|---|---|---|
| 1 | analyzer は **UI スレッドで同期実行**される。worker も job queue も無い | `core/main.cpp:16715` / `:16718`、呼び元は `drawPanelAnalysis()` (`:30091`) |
| 2 | `analyze()` の周りに **try/catch も SEH も timeout も watchdog も無い**。契約は戻り値だけ | `core/plugin_host.cpp` 全体、`main.cpp:16720-16723` |
| 3 | plugin が segfault したら **viewer ごと死ぬ**。`crashHandler` はセッションを flush して死ぬだけ | `main.cpp:22251` |
| 4 | 自動再計算は**既定で off** (`anaAuto = false`)。ROI ドラッグ中は `annBusy` で**全部止まる** | `main.cpp:532`、`:16698`、`:10157` |
| 5 | したがって analyzer が走るのは **1ジェスチャに1回、離した次のフレーム**。ドラッグ中の毎フレームではない | `main.cpp:10125` / `:10136` / `:16691-16699` |
| 6 | 単位は **ホストがキー名から推測**している (`snr_db` → dB) | `unitForAnalysisKey()` `main.cpp:16594-16615` |
| 7 | Analysis の出所行は **plugin 名も版もファイルも記録していない** | `main.cpp:16772-16794` |
| 8 | peer は analyzer plugin を**本当に走らせる**。`serve.cpp` は `plugin_host::loadAll` を呼び、viewer は `plugins/` を peer に配る | `serve.cpp:936-942` / `:1253-1321`、`main.cpp:8372-8390` |
| 9 | ローカルと peer の**数値一致は selftest が assert している** | `main.cpp:21700-21790` |
| 10 | ただし **対話中の Analysis パネルは MOP_ANALYZER を一度も発行しない**。`q.op = rp::MOP_ANALYZER` は selftest の1箇所だけ | `main.cpp:21726` が唯一。実運用の `mEnqueue` は `MOP_TEMPORAL_STATS` のみ (`:8502` `:8555`) |
| 11 | リモートの doc も**フレーム全体がクライアントに転送される** (`RFetchDone::data` は `std::vector<float>`)。パネルは手元の画素を測っている | `main.cpp:1032-1054` |
| 12 | **import adapter はまだ実装されていない。** `tools/import/` は存在しない。`docs/import-adapters.md` は設計文書のみ | `tools/` の中身を列挙して確認 |

### 1.1 ここで2つ、矛盾と穴を見つけた

**(a) 単位の規則が2つあり、互いに反対のことを言っている。**
`docs/analyzers.md` は「**単位はキー名が自己申告する**」と書き、ホストは実際に
キー名から単位を引いている (事実6)。`docs/import-adapters.md` §4.3.1 は
「**キー名に単位を埋めない** — キー名は誰かが選んだ名前であって単位の証明ではない」
と書いている。**同じリポジトリの同じ量について、逆の規則が2本走っている。**
Python analyzer はちょうどこの2つの間に座るので、どちらかを選ばなければならない。
推奨は §9-4。

**(b) 出所行に plugin の名前が無い** (事実7)。`stats/moments` が出した数字を
TSV に貼ったとき、それを**どの plugin のどの版が出したのかがファイルに
書かれていない**。C の plugin でも既にそうなっている。Python を入れると
「ユーザーが昨日書き換えた `flatness.py`」が数字の出所になるので、
**ここは Python の前提条件であって、Python の追加要件ではない**。

**(c) 事実10 は README の記述と擦れている。** README:205 は
「analyzer は peer 側でも同じものが走ります — リモートの測定がローカルと同じ
答えを返すのはこのためです」と書く。**機構としては本当**で (事実8)、
**parity は selftest が守っている** (事実9)。ただし今日の対話パネルが
実際にやっているのは「peer で走らせる」ではなく「同じコードを同じ画素の
手元コピーに対して走らせる」(事実10・11)。**この違いは Python では消えない** —
§7 で扱う。

---

## 2. 測り方

**機械**: Intel Core i7-12700K (12 コア / 20 スレッド)、RAM 64 GB、
NVMe (WD SN850 1TB)、Windows 11。Python 3.11.0、numpy 1.26.4、
**torch 2.13.0+cpu**。

> **torch は入っているが CPU ビルドで、`torch.cuda.is_available()` は False。**
> したがって **GPU / device 転送は一切測っていない**。§8-5 の torch の議論のうち
> GPU に関する部分は**すべて未測定**であり、そう書く。
> (前例: 以前の調査で `media-openexr` が何も生んでいないことを、仮定せずに
> そう言った。同じ扱いをする。)

**再現方法**:

```sh
python tools/pyplugin-bench/bench_transport.py          # 生ログ: bench_stdout.txt / bench_results.json
gcc -O2 -Iinclude tools/pyplugin-bench/bench_c_analyzer.c -o build-mingw/bench_c_analyzer.exe
build-mingw/bench_c_analyzer.exe build-mingw/plugins 15  # 生ログ: bench_c_stdout.txt
```

**方法**: `tools/pyplugin-bench/bench_transport.py` (Python 側) と
`tools/pyplugin-bench/bench_c_analyzer.c` (C の plugin を dlopen して直接叩く)。
測定結果の生ログは同ディレクトリに置いてある (`bench_stdout.txt`,
`bench_c_stdout.txt`, `bench_results.json`)。
フレームは **4000x3000 float32 = 48.0 MB (12 Mpx)**。
各項目 warmup 2 回のあと n 回、**中央値と最小・最大**を出す。
**全体を独立に2回**回し、両方の中央値を併記する (この機械のばらつきを見せるため)。

**測っていないもの・弱いところは先に言う**:

- **ファイル系の数字は Windows のページキャッシュに温まった状態**。実ディスクに
  落とす場合は `fsync` の行を見ること。冷えたキャッシュは測っていない
- **peer (ssh 越し) は一度も測っていない。** この節の数字は**全部ローカル**
- **C 案 (embedded CPython) は実装して測っていない。** §3 の C の行は
  「IPC が無いのだから §2.4 の in-process numpy と同じ」という**推論**であって
  測定ではない。そう明記する
- 12700K は 20 スレッドあり、**numpy と torch は勝手に並列化する**。C の plugin は
  全部シングルスレッド。§2.5 の C 対 numpy の比較はこの非対称を含んでいる

### 2.1 プロセス起動 + 自明な呼び出し (per-call 設計の床)

| 測ったもの | run1 中央値 | run2 中央値 | 範囲 (run2) | n |
|---|---|---|---|---|
| python 起動 → 1行印字 → 終了 | 19.5 ms | 32.4 ms | 30.6 .. 34.0 | 7 |
| + `import numpy` | 113.0 ms | 119.8 ms | 113.3 .. 156.2 | 7 |
| + numpy + json + shared_memory (harness 相当) | 131.1 ms | 120.8 ms | 120.5 .. 123.2 | 7 |
| + **`import torch`** | **1426 ms** | **1218 ms** | 1210 .. 1231 | 4 |

素の起動だけが run 間で 19→32 ms と大きく振れた (Windows のプロセス生成は
ばらつく)。**numpy を import した時点で 113〜120 ms が床**になり、
**torch では 1.2〜1.4 秒**。これは画素が1バイトも動く前の値。

### 2.2 48 MB を1回渡す (adapter が今やろうとしている運び方)

| 運び方 | run1 | run2 | 範囲 (run2) | n |
|---|---|---|---|---|
| `np.savez` 書き | 36.8 ms | 35.6 ms | 35.3 .. 36.4 | 15 |
| `np.load` 読み | 32.6 ms | 30.8 ms | 30.4 .. 34.8 | 15 |
| **npz 往復 (書き+読み)** | **69.4 ms** | **66.5 ms** | — | — |
| 生ファイル `tofile` 書き | 22.7 ms | 23.4 ms | 22.1 .. 27.1 | 15 |
| 生ファイル 書き + `fsync` | 36.6 ms | 36.2 ms | 35.1 .. 37.7 | 5 |
| 生ファイル `fromfile` 読み | 14.6 ms | 14.9 ms | 14.6 .. 15.6 | 15 |
| **生ファイル往復** | **37.3 ms** | **38.3 ms** | — | — |
| `memmap` + 全画素 `mean()` | 18.0 ms | 18.0 ms | 17.5 .. 18.4 | 15 |
| **`np.savez_compressed`** | **1451 ms** | **1452 ms** | 1452 .. 1454 | 3 |
| (参考) numpy の 48 MB コピー (確保込み) | 7.9 ms | 7.9 ms | 7.8 .. 8.2 | 15 |

**`savez_compressed` は 1.45 秒**で、非圧縮の 40 倍。adapter の harness が
これを既定にしたら、**ファイルを開くたびに 1.5 秒**が乗る。§9-11 に既定を書いた。

### 2.3 常駐 worker (1回起動して、あとは呼ぶだけ) — 候補 A

| 測ったもの | run1 | run2 | 範囲 (run2) | n |
|---|---|---|---|---|
| worker 起動 + 最初の ping (**セッションに1回**) | 117.6 ms | 117.8 ms | — | 1 |
| **往復、画素なし (IPC の床)** | **0.03 ms** | **0.03 ms** | 0.03 .. 0.07 | 150 |
| **共有メモリ、フレームは既にそこ、仕事なし** | **0.04 ms** | **0.04 ms** | 0.03 .. 0.13 | 15 |
| 共有メモリ、ホストが 48 MB を書き込む、仕事なし | 2.47 ms | 2.96 ms | 2.59 .. 3.65 | 15 |
| 共有メモリ、コピー無し + mean/std | 23.95 ms | 23.34 ms | 22.67 .. 25.32 | 15 |
| 共有メモリ、48 MB コピー + mean/std | 26.25 ms | 26.22 ms | 25.34 .. 28.05 | 15 |
| 共有メモリ、コピー無し + 16x16 タイル std 中央値 | 29.14 ms | 29.09 ms | 28.30 .. 30.15 | 7 |
| **pipe で 48 MB 流す + mean/std** | 39.17 ms | 38.63 ms | 38.27 .. 41.45 | 7 |

**この表の3行目が全部を決めている。** フレームが既に共有メモリにあるなら、
**呼び出しの往復コストは 0.04 ms**。残りは全部「計算そのもの」であって
「Python だから」ではない。

**ホストがフレームを書き込むコストは 2.5〜3.0 ms**で、しかもこれは
**フレームが変わったとき1回**であって ROI が動くたびではない。

pipe は共有メモリより **12〜13 ms 遅い** (48 MB を2回コピーするので当然)。

**例外の封じ込め (測定)**: worker の中で `raise ValueError(...)` した結果は
`'ValueError: deliberate failure from the plugin'` という**データとして返り**、
**その直後の ping に worker は生きたまま応答した** (両 run とも `True`)。

### 2.4 仕事そのもの (常駐配列上、転送ゼロ) — C 案の床でもある

| 測ったもの | run1 | run2 | n |
|---|---|---|---|
| numpy mean+std+min+max | 28.3 ms | 28.2 ms | 15 |
| numpy mean+std+**percentile(1,50,99)** | 153.5 ms | 153.0 ms | 7 |
| numpy 16x16 タイル std 中央値 | 28.7 ms | 29.6 ms | 7 |
| **torch CPU mean+std (zero-copy view)** | **2.14 ms** | **1.96 ms** | 15 |
| `torch.from_numpy` (包むだけ、コピー無し) | 0.00 ms | 0.00 ms | 150 |
| torch host→device 48 MB | **未測定 (CUDA 無し)** | — | — |

### 2.5 対話: ROI サイズを振って、温まった worker に聞く

フレームは共有メモリに置いたまま、**矩形だけが変わる**。これが
「ユーザーが ROI をドラッグしている間」に相当する状況。
右端は**同じサイズで既存の C の analyzer を叩いた**数字 (`bench_c_analyzer`, n=15)。

| ROI | py stats (mean/std) | py stats+percentile | py noise (tile std) | py torch mean/std | **C stats/moments** | **C noise/floor** |
|---|---|---|---|---|---|---|
| 256x256 | **0.13 ms** (7800/s) | 1.03 ms | 0.18 ms | 0.23 ms | 5.59 ms | **0.11 ms** |
| 512x512 | **0.53 ms** (1890/s) | 4.59 ms | 0.71 ms | 0.25 ms | 24.13 ms | **0.47 ms** |
| 1024x1024 | **1.92 ms** (521/s) | 13.73 ms | 2.71 ms | 0.37 ms | 97.08 ms | **1.93 ms** |
| 2048x2048 | 8.40 ms (119/s) | 57.96 ms | 10.64 ms | 0.88 ms | 357.80 ms | 8.01 ms |
| 全面 12 Mpx | 24.94 ms (40/s) | 153.80 ms | 29.68 ms | **2.37 ms** | **1032 ms** | 23.34 ms |

C の他の analyzer も同じ梯子で (中央値、ms): `uniformity/prnu-fpn`
0.46 / 2.27 / 10.60 / 59.40 / 142.77、`sharpness/gradient`
0.18 / 0.89 / 3.56 / 14.35 / 40.63、`iso12233/e-sfr` 1.39 / 4.41 / 15.27 / 26.99
(全面は plugin 自身が「ROI が大きすぎる」と断る)。

### 2.6 この表から読めること (そして読めないこと)

1. **転送は問題ではない。計算が全部。** 共有メモリの呼び出し往復は 0.04 ms で、
   一番安い C の analyzer (`noise/floor` の 256x256 = 0.11 ms) よりまだ小さい。
2. **numpy は同梱 C と同じ土俵にいる。** `noise/floor` 相当は C が 1.0〜1.3 倍速い
   (23.3 対 29.7 ms)。**互角**と言ってよい。
3. **`stats/moments` だけ numpy が 6.5〜9 倍速いが、これは言語の話ではない。**
   C 版は 3 つのパーセンタイルのために **1200 万個の float を `qsort` で全ソート**
   している (`plugins/analyzer_stats.c:101`、比較関数はポインタ経由)。numpy の
   `percentile` は introselect の部分選択。**アルゴリズムの差**であって
   「Python が C より速い」ではない。**この数字を『Python は速い』の根拠に
   使ってはいけない** — 使えるのは「numpy で書いても遅くならない」までである。
   (ついでに: この C の analyzer は全面で 1.0〜1.4 秒、しかも**UI スレッド**上。
   run1 の中央値は 1416 ms [1052..1733]、run2 は 1032 ms [1018..1106] と
   run 間で大きく振れた。いずれにせよ**秒のオーダーで画面が止まっている**。)
4. **対話に必要な速度は、A なら余裕で出る。** 現実的な ROI (256〜1024 px 角) で
   0.13〜2.7 ms = 370〜7800 回/秒。しかも事実5 より、UI が実際に要求する頻度は
   **1ジェスチャに1回**である。**桁が3つ余っている。**
5. **B (毎回プロセス起動) は対話には死んでいる。** 床が 113〜120 ms、npz 往復を
   足して約 180〜190 ms、torch なら **1.3〜1.5 秒/回**。
6. **torch CPU が numpy より 10 倍速いのは、20 スレッドを使っているから。**
   同じことは C の plugin をマルチスレッド化しても起きる。torch の利点は
   「速い」より「**書いた式がそのまま並列で走る**」ことにある。
7. **GPU は未測定。** CUDA デバイスがこの機械に無い。§8-5。

---

## 3. 設計の比較 — レビュアが判断するための表

| | **A. 常駐 worker + 共有メモリ** | **B. 呼び出し毎に別プロセス** | **C. CPython を viewer に埋める** | **D. Python analyzer を作らない** |
|---|---|---|---|---|
| **latency** 512x512 ROI | **0.53 ms** (実測) | ROI だけ渡すなら ≈ **125 ms**、全面を渡すなら ≈ **190 ms** (起動 120 + 転送 + 仕事、実測の合成) | ~0.5 ms (**推論**、IPC が無い分 A と同じ) | — |
| **latency** 全面 12 Mpx | **24.9 ms** (実測、stats) | ≥120 + 66 (npz 往復) + 25 ≈ **210 ms** | ~25 ms (**推論**) | — |
| **latency** torch を使う plugin | 起動時 +1.2〜1.4 s、以後 **2.4 ms** | **毎回 +1.2〜1.4 s** → 使えない | 起動時 +1.2〜1.4 s、以後 2.4 ms | — |
| **呼び出しあたりの純オーバヘッド** | **0.03〜0.04 ms** (実測) | **120〜190 ms** (実測) | 0 (GIL 取得のみ、**未測定**) | 0 |
| **peer** | .py を送って peer 側で worker を立てる。**peer に python+numpy が要る** (§7) | 同上、コストは毎回 | **viewer-serve に libpython をリンク**。peer が重くなる | **変わらない**。C だけなので parity は今のまま |
| **crash 封じ込め** | **実測: 例外はデータで返り worker は生存**。ハードクラッシュは EOF → 再起動 **118 ms** | プロセスが死ぬだけ。次の呼び出しに影響なし | **plugin の落ちが viewer を殺す** (今の C と同じ) | 変わらない |
| **ハング (無限ループ)** | timeout + kill が要る (**新規に作る機構**) | timeout + kill (同上) | **止められない。GIL を握ったまま UI が凍る** | 起きない |
| **依存の重さ** | python + numpy が**両機に**。ただし**使う人だけ** | 同じ | **viewer 本体のリンク時依存**。版・ABI・配布が全プラットフォームで問題に | **なし** |
| **最初の1個が動くまでの労力** | 中 (§8) | 小 (adapter がそのまま使える) | 大 | ゼロ |
| **adapter の機構をどれだけ再利用するか** | **ほぼ全部。** 起動・信頼・エディタ・パス記憶・`Values`・例外・peer 実行。**替えるのは運び屋 (npz→共有メモリ) だけ** | **全部そのまま** | ほとんど使えない (埋め込みは別モデル) | **これ自体が adapter** |
| **既存 ABI への影響** | 無し (C の plugin は1バイトも変わらない) | 無し | 無し | 無し |

**B は速度で落ちる**が、消えるわけではない: **一度だけ走る処理**
(バッチ測定、series 全体の集計、export 前の一括再計算) には B で十分であり、
adapter がまさにそれである。**B を否定するのではなく、B の適用範囲が
「1ファイルに1回」であることが測定で確定した**、と読むのが正しい。

---

## 4. 推奨

> **A を採る。** ただし「A を新しく作る」のではなく、
> **`docs/import-adapters.md` §4 の機構を全部そのまま使い、運び屋だけを
> npz から共有メモリに替える**。Python の道は**1本**にする。

**再利用するもの (import-adapters.md から、そのまま)**:

| 何を | どこ由来 |
|---|---|
| 「ユーザーが書くのは関数」。契約は返り値 | §4.1 |
| `Values(values, name, unit)`、**単位は必須** | §4.3.2 |
| 失敗は例外。メッセージがそのまま画面に出る | §4.9 |
| 明示的に選んだものだけ走る。自動探索も自動適用もしない | §4.13 |
| 起動前に**実際のコマンドを表示**する | §4.13 |
| 「New...」がテンプレートを書き出して**ユーザーのエディタで開く**、「Edit」が今のを開く | §4.13 |
| パス→関数を憶える。キャッシュ鍵に**モジュールの `VERSION` 属性**を含める | §4.12 |
| **peer 側で走らせる**。無ければ「無い」と言い、手元実行を明示的に選ばせる | §4.13.1 |
| **同じ Python プロセス**を adapter と analyzer で共用する (環境ごとに1つ) | 本文書の追加 |

**替えるもの、そしてなぜ替えざるを得ないか (これが本題の答え)**:

adapter は **1ファイルに1回**走る。analyzer は **ROI が動くたび・フレームが
変わるたび**走る。この違いが運び屋の要求を変える。測った値で言うと:

- adapter の運び方 (起動 + npz) の実測コストは **約 180〜190 ms/回**。
  ファイルを1つ開くのに 0.19 秒なら**誰も気づかない**
- 同じ運び方を analyzer に使うと、**1回の測定が 0.19 秒**になる。
  同梱の `noise/floor` は全面 23 ms なので、**運び屋が仕事の 8 倍**になる。
  ROI を1つ動かすたびにこれを払う
- 共有メモリなら**同じ呼び出しが 0.04 ms**。**4700 分の1**

**これが「reuse するか、できない理由を精密に述べよ」への答え**:
機構は再利用できる。**運び屋だけが再利用できない**、その理由は
「adapter は1回、analyzer は繰り返し」という**呼ばれ方の差**であって、
Python の性質でも npz の欠陥でもない。

**加えて、A でなければならない torch 固有の理由**: `import torch` は
**1.2〜1.4 秒** (実測)。B ではこれを**毎回**払う。torch を使う plugin は
**常駐 worker でしか成立しない**。ユーザーの言葉に torch が入っている以上、
これは決定的である。

**もう1つ、A が「ついでに直す」もの**: 事実1 より analyzer は UI スレッドで
同期実行されている。**A は本質的にプロセス外**なので、既にある MEASURE worker の
非同期パターン (`mWorker` `main.cpp:2633-2677`、`pumpMeasure` `:2711`) に
自然に乗る。**Python analyzer は最初から非同期にする**こと。これは
Python のための追加コストではなく、**今 C が抱えている 1 秒のフリーズを
Python 側では最初から作らない**という話である。

---

## 5. 自分の推奨に対する、いちばん強い反論2つ

### 反論1: peer の話は**コードの問題ではなく配備の問題**で、§2 の数字は1つも効かない

この文書の数字は**全部ローカルで測った**。peer には ssh 越しに一度も触れていない。
そして A の弱点はレイテンシではなく、**測定器としての parity** にある:

- 今の C plugin は viewer 自身が `plugins/*.so` を peer に配る (`main.cpp:8372`)。
  **同じバイナリが両側にある**から、selftest が数値一致を assert できる (事実9)
- Python analyzer の parity は、**独立にインストールされた2つの科学計算スタックが
  一致するかどうか**に化ける。手元 numpy 1.26 と peer numpy 2.x が
  `np.percentile` の補間や `std` の集約順序で最後の1 ULP を違えたら、
  **parity selftest は落ちる**。しかもそれは正しく落ちている
- **リポジトリの中にこれを強制する手段は無い。** viewer は peer の numpy の版を
  選べないし、`pip install` を代行するのは viewer の仕事ではない
- **D にはこの問題が一切無い。** Python は adapter を通して**データ**を作り、
  測るのは C のまま。parity は今日のまま無傷で残る

**この反論に対する私の答え**: 弱い。認めた上で、(a) 版を出所行に必ず刻む
(§9-9)、(b) parity selftest を Python analyzer にも1本足し、**落ちたら
落ちたと言う**、(c) 一致しない環境では**手元で走らせる選択肢を明示的に出す**
(§4.13.1 と同じ逃げ道)。**「黙って違う数字を出す」だけは絶対にさせない。**
それでも、**これが A の一番弱いところである**ことは変わらない。

### 反論2: 「セッション中ずっと生きている第2のプロセス」を、
クラッシュ設計が「死んでセッションを flush する」だけの プログラムに足すことになる

- **例外は測って封じ込められた**が、**ハングは封じ込めていない**。
  ユーザーの Python が `while True:` に入ったら、返り値は永遠に来ない。
  必要なのは timeout + kill + 「plugin が応答しません」と言う UI で、
  **これは今このリポジトリに存在しない機構**である。§3 の表で「新規に作る機構」と
  書いたのはそこ。**A の見積りはこれを含めて出す必要があり、私はそれを
  精密には見積もれていない**
- **共有メモリはメモリ予算と噛み合っていない。** 今の残留予算
  (`seqMemBudget()` / `claimedImageBytes()`) は `app.images` しか数えない。
  測っているフレームごとに 48 MB の共有セグメントが増えると、
  **予算の外で RAM が増える**。stack を開いたまま複数フレームを測る運用で効く
- **プロセスが増えると「どこで動いているか」が増える。** 今は
  「手元」か「peer」の2つ。A のあとは「手元の viewer」「手元の worker」
  「peer の viewer-serve」「peer の worker」の4つになる。
  出所行がそれを言えないなら、**ユーザーはどこで出た数字か分からなくなる**

**この反論に対する私の答え**: (a) timeout は既定 30 秒・可視・キャンセル可能に
する (§9-6)、(b) 共有セグメントを予算に数える (§9-8)、(c) 出所行に
**どこで走ったか**を必ず書く (§9-9)。ただし**これは3つとも新しい仕事**であり、
「adapter の機構を再利用するだけ」という §4 の売り文句を、**その分だけ
割り引いて読むべき**である。

---

## 6. plugin 作者に渡す API (これで良いか、が判断してほしいこと)

```python
# ~/.viewer/analyzers/flatness.py     (Edit analyzer... で開くのはこのファイル)
import numpy as np
from viewer_analyzer import analyzer, Values, Curve

@analyzer("uniformity/flatness", description="未飽和のフラットに ROI を置くこと")
def flatness(frame, roi):               # frame.px: 読み取り専用 f32 view (コピー無し)
    a = frame.px[roi.slice]             # roi.slice == (slice(y,y+h), slice(x,x+w))
    prof = a.mean(axis=0)
    return {
        "mean":           Values(a.mean(), unit="DN"),
        "flatness_pct":   Values(100 * a.std() / a.mean(), unit="%", headline=True),
        "column profile": Curve(x=Values(np.arange(prof.size), "column", "px"),
                                y=Values(prof, "mean", "DN")),
    }
```

**この形にした理由**:

- **返り値は dict 1つ。** 数値と曲線が**同じ辞書に同居する**のは、ホスト側の
  sink が `emit_number` と `emit_series` の**両方とも「名前の付いたものを1つ置く」**
  だからで、Python 側で2つに割る理由が無い。`(dict, list)` のタプルを返させる案は
  却下した — 曲線を持たない plugin が毎回 `, []` と書くことになる
- **`Values` は `import-adapters.md` §4.3.2 のものをそのまま使う。**
  違うのは1点だけ: **スカラを受ける**ようにし、**`name` を省略したら辞書のキーが
  名前になる**。これは §4.3.2 が `timestamps` について既に決めている
  「**フィールド側が既定の名前を持つ**」の同じ手であって、新しい発明ではない
- **`Curve` は新しい名前が要る。** `Series` は正典 (`docs/terminology.md`) で
  **層の名前**として押さえられている。曲線を `Series` と呼ぶと
  `App::Series` (掃引) と `App::AnalysisState::Series` (曲線) が既に起こしている
  衝突を Python 側にも輸入することになる。**`Curve` はこの衝突を作らない**
- **`Curve` の軸は `Values`。** `emit_series(name, x_label, y_label, x, y)` に
  1対1で落ちる。ラベルは `name` と `unit` から組む (`"column [px]"`)。
  **軸に単位が付いていないカーブを作れない**
- **ROI は `roi.slice`。** `frame.px[roi.y:roi.y+roi.h, ...]` を毎回書かせない。
  ただし**フレーム全体は見える** — `prnu` の 9x9 ボックスのように、ROI の外を
  読む必要のある測定があるため。測定上、全面を渡す追加コストは**ゼロ**
  (どうせ共有メモリに全部ある)
- **失敗は `raise`。** §4.9 と同じ。メッセージがパネルに出る。実測で worker は生きる

**登録も1本にする**: `@analyzer("category/name", description=...)` の
`"category/name"` は `docs/analyzers.md` の命名規約そのもの。ABI の
`psAnalyzerV2::name` / `description` に1対1で対応する。

---

## 7. peer の答え

**規則は adapter と同じにする** (`import-adapters.md` §4.13.1 は既に決定済み):

1. **Python analyzer は、画素がある側で走る。** リモートの doc を測るなら
   **peer 側の worker**が走る。手元の doc なら手元
2. **.py はテキストなので protocol で送れる** (§4.13.1 が adapter について
   既に言っている)。peer は一時ディレクトリに置いて worker に読ませる。
   **plugin をユーザーが peer に配備する必要は無い**
3. **peer に python / numpy が無ければ、そう言う。** 推測して黙って失敗しない。
   逃げ道は「手元で走らせる (**フレームが回線を流れます**)」を明示的に選ばせる
4. **選択は常にローカル。** peer が勝手に analyzer を探すことはしない

**ただし、正直に言っておくべきことが2つある**:

- **今日の対話パネルは、リモートの doc でも手元で測っている** (事実10・11)。
  フレームは既に `RFetchJob` でクライアントに来ている。だから Python analyzer を
  **v1 で手元固定にしても、今日の挙動と何も変わらない** — README が謳う parity は
  「同じコードが同じ画素に対して走る」という形で保たれる。
  **これは「peer 対応を後回しにしてよい」という意味であって、
  「parity を諦める」という意味ではない**
- **本当に難しくなるのは、パネルが MOP_ANALYZER を使い始めたとき。** そのとき
  C の analyzer は「viewer が配った同じ .so」だが、Python の analyzer は
  「viewer が送った .py を、peer が自分の numpy で走らせたもの」になる。
  **バイト同一性が版一致に格下げされる。** 反論1 はこの一点である

**推奨**: v1 は **peer 実行を設計に入れたまま、実装は adapter の §4.13.1 と
同時**にする。別々に作ると同じ配線を2回作ることになる。

---

## 8. コスト

**前提**: `docs/import-adapters.md` の段階 1〜3 (`viewer_import.py`、harness、
viewer 側の起動と受け取り) が**先に要る**。現状 `tools/import/` は存在しない
(事実12)。**この検討の推奨は、まだ始まっていない仕事の上に乗っている。**

| # | 内容 | 依存 | 大きさ |
|---|---|---|---|
| 0 | (前提) adapter 段階 1〜3 | — | 別タスク |
| 1 | `tools/analyze/viewer_analyzer.py` — `@analyzer` / `Values` スカラ対応 / `Curve` / 構築時検査 | 段階1 | 小。Python だけで閉じる |
| 2 | worker 本体 (共有メモリ attach、呼び出しループ、例外→エラー文字列) | 1 | 小。ベンチの `worker.py` が原型 |
| 3 | viewer 側: 共有メモリの確保とフレーム書き込み、`dataRev` でのキャッシュ | 2 | **中**。予算計上を含む (反論2) |
| 4 | viewer 側: 非同期呼び出し (`mWorker`/`pumpMeasure` パターンの流用) と結果のグリッド流し込み | 3 | 中 |
| 5 | **timeout / kill / 再起動 / 「応答しません」の UI** | 4 | **中。既存の機構が無い** |
| 6 | 登録 UI (analyzer の追加・編集・エディタ起動)、Measure メニューへの合流 | 4 | 中。adapter の reader 欄と同じ形 |
| 7 | 出所行に plugin 名・版・実行場所を刻む (**C の plugin も同時に直る**) | 4 | 小。ただし export の期待値 selftest を触る |
| 8 | peer 実行 (.py 送出、peer 側 worker、parity selftest 1本) | adapter §4.13.1 | 中 |

**最初の1個が動くまで**は 1〜4 で足りる (5〜8 無しでも「動く」)。
**出荷できる**のは 5 と 7 まで来てから。

**捨てるものはゼロ**: C の ABI は1バイトも変わらない。既存 7 個の plugin は
そのまま。**Python は増える道であって、置き換えではない** — ユーザーの
「C++ で書く場合もあるけど」に対応する。

---

## 9. 未決事項と、推奨する既定値

| # | 決めること | **推奨既定値** | 理由 |
|---|---|---|---|
| 1 | A / B / C / D のどれか | **A** (機構は adapter 再利用、運び屋のみ共有メモリ) | §2.5 / §4 |
| 2 | v1 の対象は何 plugin か | **analyzer だけ**。display / processor は後 | display は LUT を1回埋めるだけで急がない。processor は 48 MB を返す経路の設計が要る |
| 3 | worker は plugin ごとか、環境ごとか | **環境ごとに1つ**。adapter と analyzer で共用 | 起動 118 ms を何度も払わない。torch の 1.2〜1.4 秒は特に |
| 4 | 単位: キー名推測か、宣言か (§1.1-a の矛盾) | **Python analyzer では宣言が勝つ。ホストは `unitForAnalysisKey()` を通さない。** C の path は ABI v3 まで現状維持 | `import-adapters.md` §4.3.1 が正しい。キー名は名前であって単位の証明ではない |
| 5 | 単位の空文字を許すか | **許さない。無次元は `"a.u."`、画素値は `"DN"` と明記させる** | 正典「量には必ず単位」。`docs/analyzers.md` が既に a.u. と書いている |
| 6 | ハング時の timeout | **既定 30 秒、画面に出す、キャンセルできる、kill して再起動 (実測 118 ms)** | 反論2。無限に待つ UI は作らない |
| 7 | 見出し数値 (アクセント強調) の申告 | **`Values(..., headline=True)`**。今ホスト側にある対応表 (`main.cpp:16620`) は C 用に残す | `docs/analyzers.md` が「ABI v3 でプラグイン申告に移す予定」と既に書いている。Python が先に正しい形を採る |
| 8 | 共有メモリの寿命と予算 | **測っているフレームにつき1セグメント。`dataRev` が変わったら書き直す (2.5〜3.0 ms)。残留予算に計上する** | 反論2 |
| 9 | 出所行に何を刻むか | **plugin 名 / .py の path と内容ハッシュ / モジュールの `VERSION` / python・numpy・torch の版 / どこで走ったか (local か peer のホスト名) / viewer の commit** | 事実7 の穴。`import-adapters.md` §4.12 のキャッシュ鍵がそのまま使える |
| 10 | peer 対応は v1 からか | **設計には入れる。実装は adapter §4.13.1 と同時。** v1 が手元固定でも今日の挙動と同じ (§7) | 同じ配線を2回作らない |
| 11 | harness の npz 圧縮 | **非圧縮を既定にする** (`savez`、`savez_compressed` ではない) | 実測 1.45 秒 対 36 ms、**40 倍**。adapter 側の決定だがここで測れたので書く |
| 12 | GPU / torch device | **v1 は対象外。** ABI は `PS_MEM_CUDA` / `PS_CAP_CUDA` を既に予約済み | **この機械に CUDA が無く、一切測っていない。** §8-5 |
| 13 | stack (複数フレーム) を測る Python analyzer | **v1 は 1 フレーム。** ただし共有メモリなら複数フレームを置くのは自然 | `docs/analyzers.md` の `emva1288/*` は `analyze_seq` 前提と既に書いてある。**Python 側が先に実験する場所として妥当** |
| 14 | C の plugin にも非同期化・timeout を入れるか | **今回は入れない。別タスクとして起票する** | 事実1〜3。`stats/moments` の 1 秒フリーズは Python とは独立の既存問題 |

**この検討で決めていないこと (レビュアに投げる)**: #1 と #4 と #10。
残りは #1 が決まれば従属して決まる。

---

## 10. 検証: ベンチマークは出荷物に1バイトも入っていない

`tools/pyplugin-bench/` は `CMakeLists.txt` から参照されていない。
それを主張ではなく**測定**で示す。

version 文字列は `git status --porcelain` が空でないと `+local` が付く
(`cmake/gitversion.cmake:20-26`) ので、**両方のビルドで tree を dirty に保ち、
version 文字列を同一にした上で**比較した (そうしないと version の差で
バイナリが違って当たり前になり、テストにならない)。

- ビルド A: `tools/pyplugin-bench/` **あり**
- ビルド B: `tools/pyplugin-bench/` を退避して **なし** (`docs/python-plugins.md` は両方にあるので dirty は維持)
- **同じビルドディレクトリのパス**を使い、間で完全に消す (パスがバイナリに
  埋まる可能性を潰すため)

### 10.1 最初の試みは失敗した — そして失敗の理由のほうが重要だった

素直に2回ビルドして比べたら、**9個すべてが違った**。version 文字列は
両方 `1c1acbc49c86+local` で同一、しかも**純 C の plugin DLL まで違っていた** —
plugin は `tools/` に依存しようがないので、**これは検証方法が壊れている**印である。

原因は **PE の TimeDateStamp**。mingw の `ld` は既定でビルド時刻をヘッダに書く。
つまり **このプロジェクトの既定ビルドは、そもそも再現可能ではない。**
「入力が同じなら出力も同じ」が成り立たないので、この方法では**何も証明できない**。

**対照実験を先にやるべきだった。** やり直した:

```
リンカに -Wl,--no-insert-timestamp を渡す (コマンドラインのみ。CMakeLists.txt は触っていない)
対照: 同じツリーを2回ビルド -> 9個すべて byte 一致  => 方法が成立する
```

### 10.2 結果

対照が通ったので、同じ決定的フラグで A と B を比較した:

| 成果物 | A (harness あり) と B (harness なし) |
|---|---|
| `viewer.exe` | `50c9acc16c7475cf...` **一致** |
| `viewer-serve.exe` | `4dfb0be860841c5a...` **一致** |
| `plugins/analyzer_esfr.dll` | `b4f1f37e08056e48...` **一致** |
| `plugins/analyzer_noise.dll` | `b35efbff9d584f9a...` **一致** |
| `plugins/analyzer_prnu.dll` | `279c0961592e068b...` **一致** |
| `plugins/analyzer_sharpness.dll` | `8feeb1bf5b69e877...` **一致** |
| `plugins/analyzer_stats.dll` | `eaa57f0874ee7cb6...` **一致** |
| `plugins/display_falsecolor.dll` | `2556686c72055d20...` **一致** |
| `plugins/proc_demosaic.dll` | `ce9fbfe88a73df2d...` **一致** |

**9個すべて byte 単位で同一。** ベンチマークは出荷物に入っていない。

### 10.3 ついでに見つかったこと (別タスクとして起票する価値がある)

**既定のビルドは再現可能ではない** (§10.1)。`-Wl,--no-insert-timestamp` を
足すだけで再現可能になる。出所行が commit hash を刻む道具として、
**「同じ commit から同じバイナリが出る」が今は成り立っていない**のは、
provenance の主張として弱い。**この検討の範囲外なので直していない** —
`CMakeLists.txt` は1文字も触っていない。
