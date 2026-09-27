# bench/pyplugin — 「plugin を Python で書けるか」を測ったハーネス

**これは `viewer` の一部ではない。** ルートの
[CMakeLists.txt](../../../CMakeLists.txt) はこのディレクトリを一切参照しておらず、
`viewer` / `viewer-serve` / plugins のどれからも import / link されない。
`bench_c_analyzer.c` は**すでに建っている** plugin DLL を `dlopen` する独立した
`main()` で、製品のビルドには入らない。装置であって製品ではないので、
意図的に CMake に繋いでいない。

## 何を裏づけた測定か

issue [#45](https://github.com/klimemam/viewer/issues/45)(Python プラグイン)の
**「常駐ワーカ + 共有メモリを推奨」という結論は、全部ここの数字で出ている。**

| 主張 | この装置が出した数字 |
|---|---|
| **毎回起動は不可** | プロセス起動 + 自明な呼び出しで 32 ms、`import numpy` を足して 120 ms、`import torch` だけで **1218 ms** |
| **常駐ワーカなら overhead は無視できる** | 起動は1セッション1回 118 ms。以後の往復は **0.03 ms**、共有メモリに置いた 48 MB フレームに対する呼び出しは **0.04 ms** |
| **運び屋はファイルではなく共有メモリ** | 48 MB を `np.savez` 35.6 ms / `np.load` 30.8 ms / pipe 経由 38.6 ms に対し、共有メモリはコピー無しで 0 ms・ホストが詰めても 2.96 ms |
| **numpy は C と同等**(Python だから遅い、ではない) | noise 相当: C 実装 23.2 ms vs numpy 29.7 ms で C の勝ち。ただし同じ桁。torch CPU の mean+std は 1.96 ms で **C より速い** |
| **overhead は「包む仕事」と並べて判断する** | `bench_c_analyzer.c` が既存 C アナライザを実測: `stats/moments` 全画面 1015 ms、`noise/floor` 23.2 ms。40 ms の転送は前者の隣では無視でき、後者の隣では支配的 |
| **ハングと例外の始末** | worker に例外を起こさせて、プロセスが生き残ることを確認 (`raise` コマンド)。**タイムアウト/強制終了の機構は測っていない** — これは #45 に残った反論のひとつ |

## 結論はどこにあるか

| 文書 | 何が載っているか |
|---|---|
| [docs/background/reviews/adapter-transport-review.md](../../../docs/background/reviews/adapter-transport-review.md) | **#44 / #45 合同レビュー = この測定の決着文書。** §3.2 が「アナライザ (#45): 行く」で、ここの数字をそのまま引いている。§3.1 は同じ数字で**リーダ側は行かない**と判断した対比 |
| [docs/features/remote/remote-reader-design.md](../../../docs/features/remote/remote-reader-design.md) | §5.4「常駐ワーカ・共有メモリはここに入れない」。起動 0.12–0.19 s / torch import 1.2–1.4 s の節約が #45 側の話であることの確認 |
| [docs/reference/abi-v3.md](../../../docs/reference/abi-v3.md) | §9.2 が Python ワーカの区画を ABI 側から見た形 |
| [docs/tasks.csv](../../../docs/tasks.csv) | 板の該当行(レビュー必要項目)。**2026-08-14 裁定で次フェーズ送り**。着手指示が出た時点で Fable 設計(W1 常駐ワーカ → W2 共有メモリ)から |

**注意。** この装置が書かれた時点の一次報告は `docs/python-plugins.md` で、これは
**main に無い**(branch `plugin-python-study` にだけある)。
[docs/README.md](../../../docs/README.md) の「未作成文書の例外」表に登録済みのパスで、
このディレクトリのソースコメントはそのままそれを引く。
読みたい場合は `git show origin/plugin-python-study:docs/python-plugins.md`。

## 由来

| | |
|---|---|
| 測定日 | 2026-08-03 15:18:24 (`bench_results.json` の `when`) |
| 元ブランチ | `origin/plugin-python-study` |
| 元パス | `tools/pyplugin-bench/` |
| 取り込んだ commit | `2b39edda` (「docs: plugin を Python で書く案を、測ってから決める」。このディレクトリを作り、以後触っていない唯一の commit) |
| 測定環境 | Windows 11 / Python 3.11.0 (MSC v.1933 x64) / numpy 1.26.4 / torch 2.13.0+cpu。**CUDA は無い機械** — GPU 転送は未測定 |
| フレーム | 4000x3000 float32 = 48.0 MB、reps=15、中央値と全幅で報告 |

ファイル本体は branch の内容を**1バイトも変えずに**持ち込んである
(この README は新規)。したがってソースコメントの自己言及パスは旧
`tools/pyplugin-bench/...` のままである。下の「再実行」が現行パスでの
正しいコマンドで、置き換えは `tools/pyplugin-bench/` → `tools/bench/pyplugin/`
の1対1。

## 中身

| | |
|---|---|
| `bench_transport.py` | 本体。4部構成 — (1) プロセス起動の床 (2) 48 MB を1回ファイル境界越しに運ぶ (3) 常駐ワーカ経由 (pipe / 共有メモリ、コピー有無) (4) 常駐配列に対する計算そのもの = 転送ゼロの床。最後に対話的 ROI sweep |
| `worker.py` | 常駐ワーカ側。**意図的に最も安い protocol**(stdin に1行1 JSON、共有メモリは名前で attach)。数字を「どんな実設計でも下回れない床」にするため |
| `child_trivial.py` | 起動の床を測るためだけの子プロセス |
| `bench_c_analyzer.c` | 既存 C アナライザ plugin を `dlopen` して同じフレームで測る。Python 側の overhead を**包む仕事の大きさ**と並べるためのものさし |
| `bench_stdout.txt` / `bench_c_stdout.txt` | 2026-08-03 の実行出力(そのまま) |
| `bench_results.json` | 同じ実行の機械可読版(`n` / 中央値 / min / p25 / p75 / max) |

## 再実行

必要なもの: Python 3.11+ / `numpy`。`torch` は任意
(無ければ torch の行が落ちるだけ)。`bench_c_analyzer.c` 側は C コンパイラと、
**先に建てた plugin DLL** が要る。

```sh
# --json を必ず渡すこと (下の注意)。--only は spawn,oneshot,worker,compute,interactive
python tools/bench/pyplugin/bench_transport.py --json out.json                 # 全部
python tools/bench/pyplugin/bench_transport.py --quick --json out.json          # reps 15->5
python tools/bench/pyplugin/bench_transport.py --only worker --json out.json    # 1 節だけ
```

**`--json` を省略すると、このディレクトリの `bench_results.json` を上書きする**
(既定値が `<このディレクトリ>/bench_results.json`)。あの1本は 2026-08-03 の
測定値の凍結なので、再実行は必ず repo 外のパスへ書くこと。
装置の振る舞いは branch のままにしてあるので、既定値も直していない。

```sh
# C アナライザ側 (repo ルートから、plugins を建てた後)
gcc -O2 -Iinclude tools/bench/pyplugin/bench_c_analyzer.c -o build-mingw/bench_c_analyzer.exe
build-mingw/bench_c_analyzer.exe build-mingw/plugins 15
```

## 今日の tree でそのまま動かないもの

- **`bench_c_analyzer.c` は 2026-08-03 の plugin 集合を測った数字を持っている。**
  出力に出る 5 本 (`stats/moments` / `noise/floor` / `uniformity/prnu-fpn` /
  `sharpness/gradient` / `iso12233/e-sfr`) は当時の登録名で、以後 main の
  analyzer は増減している。装置自体はディレクトリ内の plugin を**総当たりで**
  読むので今日も動くが、**`bench_c_stdout.txt` と行単位で比べてはいけない**。
- `bench_transport.py` は `torch 2.13.0+cpu` で測った。**CUDA のある機械では
  §4 の torch 行が変わる**(当時 `torch CUDA: NOT AVAILABLE` と印字して降りている)。
- **2026-09-27 に §3 (worker) だけ再実行して、装置がそのまま動くことを確認した**
  (`--only worker --quick`)。共有メモリ呼び出し 0.04 ms、stats 23.5 ms、
  pipe 40.2 ms、例外後もワーカ生存 — 2026-08-03 と同じ結論が出る。
- 数字はすべて laptop の1台での実測。**装置を移したら再測すること。**
  ここに置いてある `.txt` / `.json` は「当時の測定値の凍結」であって、
  回帰の基準値ではない。
