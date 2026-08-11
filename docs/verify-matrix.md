# 形式 × 戸 × 操作 の検証マトリクス

このリポジトリは「どの形式が読めるか」を `core/imagefile.h` の表で、「どの戸から
入れるか」を `openPath` / `openRemote` / `scanFolderGroups` / `serve.cpp` の述語で、
「入ったあと何ができるか」を各パネルで決めている。**その3つを一度に見る場所が
無かった。**

無かったことの代償は 2026-08-11 に現れた——「remote で `.raw` が開けないね」。
調べると、これは**判断ではなく落穂**だった。ベンダ RAW (`.nef` / `.cr2` / `.dng`) が
リンクを渡らないのは `Backend::overLink = false` という**表の列**で、理由 (LibRaw が
CDDL-1.0) も拒否文も付いている——**決めてある**。ヘッダ無し RAW (`.raw` / `.bin`) は
そもそも表に**行が無い**。行が無いのは正しい (ヘッダが無いファイルの形は人が宣言する
ものなので、ライブラリに渡すのは「そのライブラリがファイルより物を知っている」と
言うことになる、`core/imagefile.cpp:230-234`)。しかしその結果として
`imagefile::peerServes()` が false を返し、拒否文は generic な
`"the peer serves " + servedList()` に落ちる。**誰もヘッダ無し RAW がリンクを
渡れないと決めていない。表の端から落ちただけである。**

判断の服を着た落穂は、マトリクスにしないと見えない。この文書はそのマトリクスで
ある。

---

## 0. 読み方

セルは必ず次の 4つのどれかで、**どれかを必ず名乗る**。

| 記号 | 意味 |
|---|---|
| **○** | 通る。根拠キー付き (selftest 名 = 最強、無ければ file:line か手で確かめた記録) |
| **×決** | 断る。**判断が記録されている**——issue 番号 / 文書の節 / コードのコメント |
| **×落** | 断る。**何も決めていない。届かないだけ。**←これが見つけたいもの |
| **—** | 対象外。理由を添える |

根拠キーの体系:

- `[T:name]` — selftest。`viewer_selftest(name ...)` (CMakeLists.txt) で CI が走らせて
  いるもの。**セルの証拠としては最強**
- `[C:file:line]` — コードを読んだ。行は 9d307b8 時点
- `[D:...]` — 文書の節 / issue 番号
- `[P]` — **手で確かめた**。§9 に何をどう走らせたか

`[C:...]` の略記 (行番号は 9d307b8 時点):

| 略記 | ファイル |
|---|---|
| `[C:NNNN]` (略記無し) | `core/app/open_dispatch.inc` |
| `[C:od:NNNN]` | 同上 (他と並べるとき) |
| `[C:seq:NNNN]` | `core/app/sequence.inc` |
| `[C:cli:NNN]` | `core/app/cli.inc` |
| `[C:sess:NNNN]` | `core/app/session.inc` |
| `[C:watch:NN]` | `core/app/watch.inc` |
| `[C:menus:NNN]` | `core/ui/menus.inc` |
| `[C:pt:NNN]` | `core/ui/panel_temporal.inc` |
| `[C:serve:NNN]` | `core/serve.cpp` |
| `[C:imagefile:NNN]` | `core/imagefile.cpp` |

---

## 1. 軸

### 1.1 形式 (13)

| # | 形式 | 拡張子 | 表の行 |
|---|---|---|---|
| F1 | `.npy` | `.npy` | 無 (`core/main.cpp` 自身の戸) |
| F2 | `.npz` | `.npz` | 無 (同上) |
| F3 | PNG | `.png` | 有 `overLink=1` |
| F4 | JPEG | `.jpg .jpeg .jpe` | 有 `overLink=1` |
| F5 | TIFF | `.tif .tiff` | 有 `overLink=1` |
| F6 | OpenEXR | `.exr` | 有 `overLink=1` |
| F7 | y4m | `.y4m` | 有 `overLink=1` |
| F8 | ベンダ RAW | `.dng .cr2 .nef ...` (28) | 有 `overLink=0` |
| F9 | **ヘッダ無し RAW** | `.raw .bin .yuv .dat` | **無** |
| F10 | **ヘッダ無し RAW (`.rggb`)** | `.rggb` | **無** |
| F11 | 動画コンテナ | `.mp4` ほか | 無 (名指しで断る) |
| F12 | `.vsession` | `.vsession` | 無 |
| F13 | reader 出力 / viewer container | ディスク上は `.npz` (`__viewer` ツリー)、メンバ名 `__pixels_*` | 無 |

F9 と F10 を分けたのは、**`.rggb` だけ `SEQ_EXTS` にあって File ▸ Open の
フィルタに無い**からである (§7 G8)。同じ形式が戸によって違う答えを返すなら、
それは別の行として数えるべきである。

### 1.2 戸 (9)

| # | 戸 | 入口 |
|---|---|---|
| D1 | CLI 引数 | `core/app/cli.inc:821-845` |
| D2 | ファイルの D&D | `core/main.cpp:359` → `openPath` |
| D3 | フォルダの D&D | 同上 → `openPath` → `openFolder` |
| D4 | File ▸ Open | `openFileDialog` `core/app/open_dispatch.inc:2359` |
| D5 | File ▸ Open Folder | `openFolderDialog` → `openFolder` → `scanFolderGroups` |
| D6 | Browse (このマシン, `local://`) | `browseLocalFolder` → `openRemote` |
| D7 | Browse (peer, `ssh://`) | `startRemote` → `openRemote` |
| D8 | セッション復元 | `loadSession` `core/app/session.inc:2530` |
| D9 | 登録済み reader | `readerFor` → `openWithReader` `core/app/session.inc:1504` |

### 1.3 操作 (7)

| # | 操作 | 入口 |
|---|---|---|
| O1 | 1フレームを見る | `ImageDoc` が1つ立つ |
| O2 | stack にまとめる | `scanFolderGroups` / `startSequenceLoad` / peer `SCAN` |
| O3 | フレーム毎統計 | `copyPerFrameStats` `core/ui/panel_temporal.inc:527` |
| O4 | peer 側 MEASURE | `handleMeasure` `core/serve.cpp:2341` |
| O5 | Watch 検出 | `watchTargetsNow` `core/app/watch.inc:69` |
| O6 | Reload (メンバ再構成) | `reloadSource` `core/app/open_dispatch.inc:586` / `planStackMembership` |
| O7 | export | `exportDocRGBA` `core/app/export.inc:77` ほか |

**由来で表を割る。** O4 は `serverComputes` (`core/app/open_dispatch.inc:408`) が
「`remoteUrl` も `remoteFiles` も空なら false」と決めているので、直接の戸から入った
doc には**構造上あり得ない**。したがって操作の表は 2枚になる:

- **表B** = 直接の戸 (D1–D5, D8, D9) から入った doc = このディスクのファイル
- **表C** = Browse 経由 (D6, D7) = `remoteUrl` を持つ doc

---

## 2. 表A — 形式 × 戸 (117セル)

| | D1 CLI | D2 D&D file | D3 D&D folder | D4 Open | D5 Open Folder | D6 Browse local | D7 Browse peer | D8 session | D9 reader |
|---|---|---|---|---|---|---|---|---|---|
| **F1 .npy** | ○ `[T:remote]` | ○ `[C:main.cpp:359]` | ○ `[C:2339]` | ○ `[C:2369]` | ○ `[T:scan]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2]` | ○ `[T:verify V15]` | ○ `[T:verify V25m]` |
| **F2 .npz** | ○ `[C:2311]` | ○ `[C:2311]` | **×落 G3** | ○ `[C:2369]` | **×落 G3** `[C:seq:1454]` | ○ `[C:1734]` | ×決 `[P]` `[D:npz-design:91]` | ○ `[T:verify V21]` | ○ `[C:sess:2591]` |
| **F3 PNG** | ○ `[T:media]` | ○ | ○ `[T:scan S3]` | ○ `[P]` | ○ `[T:scan S3]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2/P7]` | ○ `[T:media M9]` | ○ |
| **F4 JPEG** | ○ `[T:media]` | ○ | ○ | ○ `[P]` | ○ `[C:seq:1383]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2]` | ○ `[C:sess:2604]` | ○ |
| **F5 TIFF** | ○ `[T:media]` | ○ | ○ | ○ `[P]` | ○ `[C:seq:1383]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2]` | ○ `[T:media M12]` | ○ |
| **F6 OpenEXR** | ○ `[T:media]` | ○ | ○ | ○ `[P]` | ○ `[C:seq:1383]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2]` | ○ `[T:media M18]` | ○ |
| **F7 y4m** | ○ `[T:media]` | ○ | ○ | ○ `[P]` | ○ `[C:seq:1383]` | ○ `[T:fmtgate F1]` | ○ `[T:fmtgate P1/P2]` | ○ `[C:sess:2604]` 無試験 | ○ |
| **F8 ベンダ RAW** | ○ `[T:fmtreg]` | ○ | ○ | ○ `[P]` | ○ `[C:seq:1383]` | ○ `[T:fmtgate F1]` | ×決 `[T:fmtgate F3/P6]` `[D:#148 B]` | ○ `[C:sess:2604]` 無試験 | ○ |
| **F9 ヘッダ無し RAW** | ○ `[C:cli:836]` | ○ `[C:2355]` | ○ `[C:seq:1509]` | ○ `[C:2369]` | ○ `[C:seq:1380]` | **×落 G2** `[P]` | **×落 G1** `[P]` | ○ `[C:sess:2537]` 無試験 | ○ `[D:§4.12]` |
| **F10 `.rggb`** | ○ `[C:cli:836]` | ○ | ○ | **×落 G8** `[C:2369]` | ○ `[C:seq:1380]` | **×落 G2** `[P]` | **×落 G1** `[P]` | ○ `[C:sess:2537]` 無試験 | ○ |
| **F11 動画** | ×決 `[T:fmtgate F2]` | ×決 | — 束ねない `[C:seq:1383]` | ×決 `[C:2341]` | — 同上 | ×決 `[T:fmtgate F3 local]` | **×落 G7** `[P]` | — | ○ `[C:2353]` |
| **F12 `.vsession`** | ○ `[C:2317]` | ○ `[C:2317]` | — 画像ではない | ○ `[C:2372]` | — `SEQ_EXTS` に無い | **×落 G2** `[P]` | — peer にセッションは無い | — | ○ `[C:2289]` |
| **F13 container / reader 出力** | ○ `[T:fmtreg F6]` | ○ | **×落 G3** (`.npz` として) | ○ `[C:2369]` | **×落 G3** | ○ `[C:menus:669]` | ×決 `[C:imagefile:422]` | **×落 G6** `[C:sess:2591]` | ○ `[T:verify V25m]` |

**D9 はこのマシンのパスにしか効かない。** `readerFor` が引かれるのは `openPath` の
先頭 (`core/app/open_dispatch.inc:2289`) だけで、`openRemote` は
`peerServesName` が false なら reader を提案せずに断る (`:1748`)。
`docs/input-adapters.md §4.13.1` (2026-08-03) は「adapter は peer 側で走る」と
**決めている**が、`core/serve.cpp` にその口は無い → **G11**。

**この表の読みどころ。** D1–D5 (直接の戸) はほぼ全部 ○ で、× は 動画の判断された
拒否だけ。**落穂は D6 / D7 の列に固まっている**——つまり **Browse という戸が、
他の戸が開けるファイルを開けない**。これは #111 と #148 が 2回続けて直した欠陥の
**3回目**で、直っていないのは 形式表に**行を持たない**形式 (F9/F10/F12) である。
述語 `viewerReadsName` / `peerServesName` はどちらも「表の行かどうか」で答えるので、
表に行が無い形式は**述語の外側にいる**。

---

## 3. 表B — 形式 × 操作 (直接の戸から入った doc, 91セル)

| | O1 見る | O2 stack | O3 フレーム毎統計 | O4 peer MEASURE | O5 Watch | O6 Reload | O7 export |
|---|---|---|---|---|---|---|---|
| **F1 .npy** | ○ `[T:remote]` | ○ `[T:scan]` | ○ `[T:framestats]` | — 構造上 `[C:od:408]` | ○ `[T:watch]` | ○ `[T:reload]` | ○ `[T:export]` |
| **F2 .npz** | ○ `[T:verify V21]` | ○ ファイル内フレーム軸 / **×落 G3** フォルダ連番 | ○ `[C:pt:527]` | — | ○ `[C:watch:135]` | ○ `[C:od:768-783]` | ○ |
| **F3 PNG** | ○ `[T:media]` | ○ `[T:scan S3]` | ○ | — | ○ `[C:watch:135]` 無試験 | ○ `[T:fmtreg F2]` | ○ |
| **F4 JPEG** | ○ `[T:media]` | ○ `[C:seq:392]` 無試験 | ○ | — | ○ 無試験 | ○ `[T:fmtreg F2]` | ○ |
| **F5 TIFF** | ○ `[T:media]` | ○ 複数ページ `[T:fmtreg F4]` | ○ | — | ○ 無試験 | ○ `[T:fmtreg F2/F4]` | ○ |
| **F6 OpenEXR** | ○ `[T:media]` | ○ 連番 / レイヤは documents `[T:fmtreg F5]` | ○ | — | ○ 無試験 | ○ `[T:fmtreg F5]` | ○ |
| **F7 y4m** | ○ `[T:media]` | ○ 1ファイル=1 stack `[T:fmtreg F4]` | ○ | — | ○ 無試験 | ○ `[T:fmtreg F2]` | ○ |
| **F8 ベンダ RAW** | ○ `[T:fmtreg]` | ○ `[C:seq:1383]` 無試験 | ○ | — | ○ 無試験 | ○ `[T:fmtreg F2]` | ○ |
| **F9 ヘッダ無し RAW** | ○ `[T:srcmap M3]` | ○ `[C:seq:1524]` 無試験 | ○ | — | ○ `[T:srcmap M3]` (baseline) | ○ `[C:od:690]` **無試験** | ○ |
| **F10 `.rggb`** | ○ 同 F9 | ○ 同 F9 | ○ | — | ○ | ○ **無試験** | ○ |
| **F11 動画** | — 入れない (表A) | — | — | — | — | — | — |
| **F12 `.vsession`** | — 画像ではない | — | — | — | — | — | — |
| **F13 container / reader 出力** | ○ `[T:fmtreg F6]` | ○ `stack` 層 → 2 doc `[T:fmtreg F6]` | ○ | — | ○ 起点ファイルを見る `[C:watch:32]` | ×決 `[T:fmtreg F6/F8]` `[C:od:610]` | ○ |

`O4` の列が丸ごと「—」なのは欠落ではなく**設計**である: 直接の戸から入った doc は
`remoteUrl` も `remoteFiles` も持たないので `serverComputes` が false を返し、
MEASURE は発射されない (`core/app/open_dispatch.inc:408-411`)。この列に意味が
生じるのは表C だけである。

`O7` が形式に依らず全部 ○ なのは、export が**デコード後の float32 バッファしか
読まない**からである (`exportDocRGBA` `core/app/export.inc:77-82`、
`renderDocRGBA` `core/app/compare.inc:799`、`buildRoiExport` `core/ui/panel_rois.inc:30`)。
形式・dtype・path は provenance 行に**印字されるだけ**で、分岐には使われない。
**ただし試験は `.npy` float32 しか通していない** (`selftest.export` / `export-tsv` は
`tools/testdata/multi`、`roi-export` はファイルを一切開かない)。

---

## 4. 表C — 形式 × 操作 (Browse 経由の doc, 91セル)

`local://` と `ssh://` で答えが割れるセルは `local:// | ssh://` と書く。

| | O1 見る | O2 stack | O3 フレーム毎統計 | O4 peer MEASURE | O5 Watch | O6 Reload | O7 export |
|---|---|---|---|---|---|---|---|
| **F1 .npy** | ○ `[T:fmtgate P1/P2]` | ○ `[T:scan]` `[T:browse]` | ○ 常駐分のみ `[C:pt:429]` | ○ `[T:rtemporal]` | ○ `[T:rwatch]` | ○ `[T:rwatch R]` `[C:od:616]` | ○ / **×落 G4** preview |
| **F2 .npz** | ○ ローカルの戸へ再ルート `[C:od:1734]` \| ×決 `[P]` | — 再ルート後は表B \| — | — \| — | — \| — | — \| — | — \| — | — \| — |
| **F3 PNG** | ○ `[T:fmtgate P1/P2]` | ○ `[T:fmtgate P7]` | ○ | ○ `[T:rtemporal-png]` | ○ `[T:rwatch R12]` | ○ `[T:rwatch]` | ○ / **×落 G4** |
| **F4 JPEG** | ○ `[T:fmtgate P1/P2]` | ○ `[C:serve:1129]` | ○ | ○ `[C:serve:558]` 無試験 | ○ `[C:watch:51]` | ○ | ○ / **×落 G4** |
| **F5 TIFF** | ○ `[T:fmtgate P1/P2]` | ○ 複数ページ `[T:fmtgate P2]` | ○ | ○ `[C:serve:558]` 無試験 | ○ | ○ | ○ / **×落 G4** |
| **F6 OpenEXR** | ○ `[T:fmtgate P1/P2]` | ○ | ○ | ○ `[C:serve:558]` 無試験 | ○ | ○ | ○ / **×落 G4** |
| **F7 y4m** | ○ `[T:fmtgate P1/P2]` | ○ フレーム軸 | ○ | ○ `[C:serve:558]` 無試験 | ○ | ○ | ○ / **×落 G4** |
| **F8 ベンダ RAW** | ○ 再ルート `[T:fmtgate F1]` \| ×決 `[T:fmtgate F3/P6]` | — \| — | — \| — | — \| — | — \| — | — \| — | — \| — |
| **F9 ヘッダ無し RAW** | **×落 G2** \| **×落 G1** | — 届かない | — | — | — | — | — |
| **F10 `.rggb`** | **×落 G2** \| **×落 G1** | — | — | — | — | — | — |
| **F11 動画** | ×決 `[T:fmtgate F3 local]` \| **×落 G7** | — | — | — | — | — | — |
| **F12 `.vsession`** | **×落 G2** \| — | — | — | — | — | — | — |
| **F13 container / reader 出力** | ○ 再ルート `[C:od:1734]` \| ×決 `[C:imagefile:422]` | — \| — | — \| — | — \| — | — \| — | ×決 `[T:fmtreg F8]` \| — | — \| — |

### 表C で気付いたこと

**`local://` の非 peer 形式は「戸を通ってローカルに落ちる」。** `.npz` と ベンダ RAW は
`openRemote` の #111 分岐 (`core/app/open_dispatch.inc:1733-1743`) で `openPath` に
渡され、`remoteUrl` を持たない doc になる。したがって表C の行としては O1 だけが
意味を持ち、以降は表B の行と同一になる。これは判断であって落穂ではない
(`[D:input-adapters §3.6.4]`)。

**stack 系の動詞がローカル行でも peer の問いのままなのは判断である**
(`[D:input-adapters §3.6.4]`「stack 系の動詞は peer の問いのまま……出して押させて
断るより出さない」、`core/browse/panel.cpp:444/1048/1136/1501`)。プレビューが付か
ないのも同じ節で決めてある。

---

## 5. セルの数え

| | 表A | 表B | 表C | 合計 |
|---|---|---|---|---|
| セル総数 | 117 | 91 | 91 | **299** |
| **○ 通る** | 91 | 64 | 36 | **191** |
| **×決 判断あり** | 7 | 1 | 4 | **12** |
| **×落 落穂** | **12** | **1** | **10** | **23** |
| **— 対象外** | 7 | 25 | 41 | **73** |

`local:// \| ssh://` で割れているセルは 1セルと数え、片方が `×落` ならそのセルは
`×落` に数えた (落穂を薄めないため)。表B F2 の O2 も同じ扱い。

`×落` 23セルは重複を除くと **11件の原因** (G1–G11) に落ちる。§7。
セルを最も多く占めているのは **G4 (6セル)**、**G3 (5セル)**、**G1 (4セル)**、
**G2 (3セル + G1 と共有 2セル)** で、残りの 7件は 1セルずつである。
**セル数と噛みやすさは別物**なので、§7 の順序はセル数ではなく被害で並べてある。

**selftest が直接証明しているセル: 80**
(表A 34、表B 28、表C 18)。残りの ○ はコードを読んだ結果か `[P]` である。
形式別の試験の厚みは極端に偏っている——`.npy` が 8試験、PNG が 6、TIFF/EXR/y4m/JPEG/
ベンダ RAW が 3–4、**ヘッダ無し RAW は 1 (`selftest.srcmap` M3 の 1ブロックだけ)**、
`.rggb` は 0。

---

## 6. どのセルを selftest が証明しているか

| selftest | 証明しているセル |
|---|---|
| `fmtgate` | 表A F1/F3–F8 × D6/D7 (一覧・開ける・断り文)、表C F1/F3–F7 × O1 (peer META/TILE が local デコードとビット一致)、F8 × D7 の**理由付き拒否**、F3 × O2 (peer SCAN が連番 PNG を1つに束ねる) |
| `media` | 表A F3–F7 × D1、表B F3–F7 × O1、TIFF の複数ページ = stack、EXR のレイヤ = documents |
| `fmtreg` | 表B F3–F8 × O6 (register / reload / 2度目の open が共有)、F13 × O6 の**判断された拒否** |
| `scan` | 表A F1/F3 × D5、表B F1/F3 × O2、表C F1 × O2 |
| `rtemporal` / `rtemporal-png` | 表C F1 × O4、**F3 × O4** (peer 側 σ_t が独立な f64 参照と一致し、同じ stack をローカルで測った値ともビット一致) |
| `rwatch` | 表C F1/F3 × O5/O6 (R12 が「watch 対象のリモートフォルダはもう `.npy` とは限らない」を担保) |
| `watch` / `reload` | 表B F1 × O5/O6 |
| `srcmap` | **表B F9 × O1/O5** (ヘッダ無し RAW が Watch の baseline を残す) ——ヘッダ無し RAW の唯一の試験 |
| `browse` / `browse-keys` / `localbrowse` | 表A F1 × D6、一覧の行・キー操作 |
| `export` / `export-tsv` / `roi-export` | O7 (ただし `.npy` float32 のみ) |
| `framestats` | O3 (`.npy` のみ) |
| `verify` | 表A F1/F2/F13 × D8/D9 |

---

## 7. 落穂 (fall-through) — 見つかったもの

**噛みやすい順。** どれもこの PR では**直していない**。各件について「決めて記録する」
と「道を開く」の両方を書く。

---

### G1. ヘッダ無し RAW が peer 越しに開けない ← 2026-08-11 の報告

**どこ。** `core/imagefile.cpp:213-283` の表に `.raw` の行が無い →
`imagefile::peerServes()` (`:379`) が false → `peerRefusal()` (`:402`) の最後の
`return` に落ちる。

**実測 `[P]`:**

```
a.raw   forPath=-  viewerReadsName=0  peerServes=0
        "the peer serves .npy, PNG, JPEG, TIFF, OpenEXR and y4m"
```

**何が問題か。** 断ること自体ではない。**断り方**である。この文は
(a) ファイルを名指ししていない、(b) 理由が「表に無いから」であって形式について
何も言っていない、(c) **逃げ道が無い** (§3.2 の三部構成のうち三つ目が欠ける)。
ベンダ RAW の断り (`vendor RAW is read on this machine, but the peer does not
serve it: LibRaw is CDDL-1.0 …` + `browse it locally, or copy it here first`) と
並べると差が分かる——あれは**決めてある**。

**判断が無いことの確認。** issue #166 (ベタ RAW を名前付きレシピのパネルにする) は
レシピの永続化の話で、リンクには触れていない。`docs/input-adapters.md:577` は
「`.raw` は LibRaw に**取らせない**」を決めているが、これは vendor library に渡すな
という話で、peer に渡すなとは書いていない。`docs/remote.md` にも記載無し。

**正直な選択肢。**

- **(a) 断ると決めて記録する。** ヘッダ無し RAW は**形と深度を人が宣言する**形式で、
  その宣言は client 側の `RawDialog` にしか無い。peer は「そのファイルが何か」を
  自力で言えない——だから渡さない、と決めれば筋は通る。やることは
  `peerRefusal()` に一節足すだけ:
  `"a headerless .raw states its shape in the dialog on this machine, and the peer
  has no way to be told it"` + 既存の `WAY_OUT`。**#166 の A 案 (名前付きレシピ) が
  入ると、この理由は「まだレシピが送れないから」に変わる**ので、そのとき再考する
  ことになる。
- **(b) 道を開く。** レシピを wire に載せる。前例がある——`#124` の declared reading
  (`npyRead`) は META と TILE の trailer として送られ、peer 側で
  `serveLayout` が適用している (`core/serve.cpp:162`)。同じ形で
  `(rawDtype, interp, W, H, offset, LE, cfaPattern)` を trailer にすれば、peer は
  `decodeRawFrame` 相当を実装するだけで済む (ヘッダを読まないので `openNpy` より
  簡単で、`readRegion` は素直な seek になる——**むしろ `.npy` に近い**)。
  protocol 番号が上がる。#166 の A 案が入っていれば「レシピを選んで peer に渡す」
  という動線がそのまま使える。

---

### G2. ヘッダ無し RAW と `.vsession` が「このマシンの」Browse でも淡色になり、偽の理由を言う

**どこ。** `viewerReadsName` (`core/ui/menus.inc:668`) は `.npy` / `.npz` と
**形式表の行**しか true にしない。`rbRowOpenable(host, name)` は host が空なら
これを引く (`core/browse/panel.cpp:118-120`)。`.raw` は表に行が無いので false。

**実測 `[P]`: `viewerReadsName("a.raw") == 0`、`viewerReadsName("a.rggb") == 0`。**
`.vsession` も同じ (表に無く、`.npy/.npz` でもない)。

**何が問題か。** 出る文は `viewerRefusalFor` (`core/ui/menus.inc:708`):

```
not a name this viewer reads (*.npy *.npz *.png *.jpg ... *.y4m)
  choose a reader to read it another way
```

**これは偽である。** 同じファイルを File ▸ Open で開けば RAW ダイアログが立ち、
`.vsession` なら `loadSession` が走る。**同じディスクの 2つの戸が同じファイルに
ついて逆のことを言っている**——#148 が 1段上で言っていた欠陥そのもので、
`docs/input-adapters.md:88` の「同じファイルがローカルとリモートで違う読まれ方を
するのは欠陥」がここにも掛かる。

しかも `.raw` は**フォルダ単位では Open Folder が束ねる** (`SEQ_EXTS` にある) ので、
「Browse で見えているのに開けない、File ▸ Open Folder なら stack になる」という
状態になっている。

**正直な選択肢。**

- **(a) 戸を開ける。** `viewerReadsName` に `SEQ_EXTS` と `.vsession` を足す。
  `openRemote` の #111 分岐 (`:1734`) は既に「host が空で viewerReadsName なら
  `openPath` に渡す」と書いてあるので、**この 1語で `openPath` に届き、RAW
  ダイアログが立つ**。副作用: peer 一覧では `peerServesName` が別述語なので何も
  変わらない (G1 とは独立に直せる)。
- **(b) 断ると決めて理由を直す。** 「Browse の 1クリックはプレビュー、ダブル
  クリックは開く」という動線にモーダルを挟むのが嫌なら、`viewerRefusalFor` に
  ヘッダ無し RAW 専用の一節を足す:
  `"a headerless .raw needs its shape stated - open it from File > Open"`。
  **今の文が偽であるという問題だけは、どちらにせよ消える。**

---

### G3. `.npz` のフォルダが RAW ダイアログに送られる

**どこ。** `core/app/sequence.inc:1454`:

```cpp
g.isRaw = ext != ".npy" && !imagefile::forPath(g.files[0]);
```

真上のコメントはこう書いてある——「`.npy` は自分で形を名乗るし、
`core/imagefile.h` の裏の絵の形式も全部そうなので、**どちらも RAW ダイアログに
送ってはならない**」。**`.npz` がその列挙から漏れている。** `.npz` は `SEQ_EXTS`
(`:1380`) に入っているので走査では拾われ、`forPath(".npz")` は nullptr なので
`isRaw` が true になる。

**その先。** `startNextQueuedGroup` (`:1509`) が `openRawDialogFor` を立て、
答えると `:1528` で `loadRaw` が走る——**zip のバイト列が画素として読まれる。**
ファイルサイズは割り切れれば候補も出るので、**もっともらしい絵が出る。**

**なぜ噛むか。** `.npz` は「変換結果の保存・キャッシュ」の既定形式
(`docs/npz-design.md:151`) なので、**`.npz` だけが入ったフォルダ**は普通に存在する。
File ▸ Open で 1本開けば正しく開き、フォルダごと開くと RAW ダイアログが立つ。
**viewer container (`__viewer` ツリー) も同じ穴に落ちる**——ディスク上は `.npz`
なので、reader が吐いたコンテナを集めたフォルダを開くと、ツリーが画素として
読まれる。

**正直な選択肢。**

- **(a) 1語足す。** `g.isRaw = ext != ".npy" && ext != ".npz" && !imagefile::forPath(...)`。
  すると `startNextQueuedGroup:1529` の分岐は `forPath` → `loadImageFile`、
  else → `loadNpy` なので、`.npz` は `loadNpy` に行って「not a .npy file」で落ちる
  ——**分岐にも `.npz` の枝が要る** (`loadNpz`)。実装は 2箇所。
- **(b) `SEQ_EXTS` から `.npz` を外す。** 「コンテナはフォルダでは束ねない、
  1本ずつ開いてメンバを選ぶもの」と決める。走査から消えるので `no loadable files`
  になり、**黙って garbage を出すよりは正しい**が、`.npz` の連番フォルダを stack に
  したい人には道が無くなる。

**どちらにせよ、`isRaw` の述語は「表に行があるか」ではなく「ファイルが自分で形を
名乗るか」を訊くべきである**——それが真上のコメントの言っていることなので。

---

### G4. 間引かれた preview がそのまま export される

**どこ。** `exportDocRGBA` (`core/app/export.inc:77-82`) に `remoteStep` の項が無い。

```cpp
static bool exportDocRGBA(ImageDoc* im, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!im || im->w < 1 || im->h < 1) return false;
    w = im->w; h = im->h;
    renderDocRGBA(*im, rgba);
    return true;
}
```

**何が問題か。** Browse から開いた最初の一枚は必ず間引かれている
(`openRemote:1788`、`step = ceil(max(w,h)/1600)`)。`Save image as PNG...` は
その 1/N の絵を**印も無しに**書き、toast は preview の寸法を画像の寸法として言う
(`:117`)。ROI 統計の export も同じで、`roiBasicStatsUncached` に `remoteStep` の
判定は無く、provenance 行は preview の `w x h` を印字する
(`core/ui/panel_rois.inc:455`)。

**周りは全部断っている。** `panel_temporal.inc:429` は
`if (d->px().empty() || d->src->remoteStep > 1) { res.skipped++; continue; } // previews lie`、
`panel_projection.inc:871` は `"frame not resident"`、`annotations.inc:69` は
`"preview: wait for full resolution before placing ROIs/pins"`、
`loader_npy_raw.inc:374` は crop を拒否。**canvas は画面に "PREVIEW 1/N" と
出している** (`core/ui/canvas.inc:1049`) ——つまり「preview は嘘をつく」は
このリポジトリの既知の規則で、**export だけがそれを守っていない。**

**正直な選択肢。**

- **(a) 断る。** 隣の 8箇所と同じ文で。`"still a preview - export after the full
  frame lands"`。
- **(b) 出すが名乗る。** PNG の tEXt チャンクと toast に `1/N preview` と書く。
  ROI/統計の export は provenance 行に `step N` を足す。
  **「測っていない絵を測ったことにしない」という点ではどちらでもよく、
  今の「黙って出す」だけが選べない。**

---

### G5. remote のファイル内フレーム軸が session 復元でフレーム 0 に戻る

**どこ。** `writeSessionTo` は `seqframe <seqIndex>` を書く
(`core/app/session.inc:192`)。復元側 (`:2451-2457`) は**同期的に**走るが、
remote の open (`openRemote`、`:2583` から frame 0 固定) は残りのフレームを
**非同期に**積む (`core/app/open_dispatch.inc:2241-2257`)。復元の瞬間
`framesOfSeq` には頭しか無いので `want` が一致せず、**誰も後から適用しない**
(`seqframe` は `session.inc` にしか現れない)。

**選択肢。** (a) remote の stack が揃ってから適用する遅延キュー (ローカルの
`app.seqRestore` と同じ形)。(b) remote では `seqframe` を書かないと決め、
「リンク越しの stack は頭から戻る」と文書に書く。

---

### G6. reader で開いた doc の session 復元が prefs.txt 頼み

**どこ。** session 行は path しか持たない (`session.inc:186`)。復元は
`readerFor(p)` (`:2591` → `:1178`) で `app.readerMemo` を引く——これは
**prefs.txt の 64件 LRU、完全一致キー**である (`:990`, `READER_MEMO_MAX :1161`)。

**噛み方。** memo が無い (別マシン / 64件から溢れた / ファイルを移した) と、
起点が本物の `.npz` や `.png` なら**native で開いてしまい**、`sessionDocAt` が
`__pixels_1` を見つけられず、**`err` が空のまま** 行の range / LUT / crop が
別の doc に当たる (`:2628-2637`)。何も報告されない。
副窓で選んだ reader はそもそも永続化されない (`rememberReader` → `savePrefs` は
`g_secondary` で早期 return, `:993`)。

**選択肢。** (a) session 行に reader spec を書く (`member` の隣に 1キー)。
(b) memo が無いときは**失敗として報告する** (`sessionDocAt` が -1 のとき
`err` を立てる) ——直すのは 1行で、少なくとも黙らなくなる。

---

### G7. `.mp4` が peer 一覧では「測った理由」を失う

**実測 `[P]`:**

```
a.mp4  peerRefusal  = "the peer serves .npy, PNG, JPEG, TIFF, OpenEXR and y4m"
       videoRefusal = "MP4 (H.264/HEVC) needs a video codec this build does not link.
                       Decoded 8-bit video is display-referred, not DN - a known
                       sigma_t of 40 DN16 comes back as 0.00 ... ffmpeg -i ..."
```

`rbRowWhyNot(host, name)` は host が空でなければ `peerRefusalFor` を引く
(`core/browse/panel.cpp:121-123`) ので、**peer のフォルダに置いてある `.mp4` は
測定に基づく理由も ffmpeg の逃げ道も失う。** `selftest.fmtgate` の F3 は
「local: an unreadable row names ITS reason, not the peer's」を assert して
いるが、**その鏡像 (remote 行が自分の理由を名乗る) は誰も見ていない。**

**選択肢。** (a) `peerRefusal` の頭で `videoRefusal` を先に返す (3行)。
(b) 「リンク越しの断りは link の限界だけを言う」と決めて記録する——ただしそれは
F3 の local 側の理屈と逆になる。

---

### G8. `.rggb` が File ▸ Open の「Images」フィルタに無い

**どこ。** `core/app/open_dispatch.inc:2369`:

```cpp
const std::string pat = "*.npy *.npz *.bin *.raw *.yuv *.dat " + media;
```

`SEQ_EXTS` (`core/app/sequence.inc:1380`) は
`{ ".npy", ".npz", ".bin", ".raw", ".yuv", ".dat", ".rggb" }` ——**`.rggb` だけ
リテラルが 2箇所に割れていて、片方に無い。** 「All files」に切り替えれば選べるし、
選べば `openPath` は普通に RAW ダイアログを立てる。D&D と Open Folder は効く。

**選択肢。** (a) 足す。(b) `SEQ_EXTS` から外す。**どちらでもよいが、リテラルが
2つある状態を残すのは駄目である**——この表の隣で 絵の形式は
`imagefile::dialogPattern()` から計算されていて、そちらは割れようがない。
ヘッダ無し RAW の拡張子も同じように 1箇所から出すのが筋。

---

### G9. generic な peer 拒否文に逃げ道が無い

**どこ。** `core/imagefile.cpp:425` — `return "the peer serves " + servedList();`
分岐の上 2つ (表の行 / `.npz`) は `WAY_OUT` を付けているのに、最後の return だけ
付いていない。`docs/input-adapters.md §3.2` の三部構成 (名指し・理由・逃げ道) の
三つ目が欠ける。G1・G7 はどちらもこの return に落ちてくる。

**選択肢。** (a) `WAY_OUT` を付ける。(b) `CHOOSE_A_READER` を付ける
(§4.13.1 で「adapter は peer 側で走る」と決めてあるので、逃げ道としては
そちらが本筋——ただし G11)。

---

### G10. 間引かれた remote doc の crop が session 復元で黙って落ちる

**どこ。** `session.inc:2653` は `cropInPlace(...)` の**戻り値を見ていない**。
`cropInPlace` は `remoteStep > 1` で false を返す (`loader_npy_raw.inc:374`、
理由は同所のコメント: 全解像度の着地が crop の帳簿を知らずに上書きするから)。
復元直後の remote doc はまさに間引かれた状態なので、**1600px を超える画像では
crop が黙って消える。**

**選択肢。** (a) 戻り値を見て、落ちたら失敗行として報告する。
(b) 全解像度が着地してから適用する (G5 と同じ遅延キューに載る)。

---

### G11. 「reader は peer 側で走る」という 2026-08-03 の決定が実装されていない

**どこ。** `docs/input-adapters.md §4.13.1` は明確に決めている——
「データが向こうにあるのに adapter を手元で走らせると生ファイルを転送してから
変換することになり、この道具の設計思想に反する。**したがって adapter は peer 側で
走る**」。`core/serve.cpp` に adapter/reader の口は無い (あるのは
`MOP_PLUGIN_ANALYZE` = ABI v3 の解析プラグインで別物)。`openRemote` は
`peerServesName` が false なら reader を提案せずに断る (`:1748`)。

**これは「決定はあるが実装が無く、拒否文もそれを言わない」状態**である。
判断された拒否でも、開いた道でもない。

**選択肢。** (a) v1 の範囲を「ローカルで走らせる (ファイルが流れます)」に狭めると
決め直して記録する——§4.13.1 自身がその逃げ道を「明示的に選ばせる」と書いている
ので、**その半分だけ先に実装する**のが一番安い。
(b) §4.13.1 のとおり protocol に adapter の口を足す。

---

## 8. 落穂ではないもの (=判断が記録されている拒否)

マトリクスを作った副産物として、**ちゃんと決めてある**ものを一覧にしておく。
次に同じ問いが出たときに探し直さないため。

| 拒否 | 記録場所 |
|---|---|
| ベンダ RAW は peer に渡らない | `Backend::overLink=0` + `core/imagefile.h:126-145` + `docs/input-adapters.md §3.6.4b` + issue #148 判断B。試験: `fmtgate` F3/P6 |
| `.npz` は peer に渡らない | `core/imagefile.cpp:422` の拒否文 + `docs/npz-design.md:91,187` (「リモートでは peer に zip の中身一覧を返す動詞が要る。ローカル先行」) |
| 動画コンテナは読まない | `imagefile::videoRefusal` + `docs/video-support.md §1` (実測: σ_t 40 DN16 → 0.00) + issue #54 |
| CFA TIFF は当てずに断る | `core/tiffread.cpp` + `core/imagefile.h:41-47` (規則3) + `docs/manual.md:270` |
| container / reader メンバは per-member reload しない | `reloadUnavailable` `core/app/open_dispatch.inc:505` + `reloadSource:610`。試験: `fmtreg` F6/F8 |
| Browse の stack 系の動詞は peer の問いのまま | `docs/input-adapters.md §3.6.4` |
| Browse のローカル行にプレビューは付かない | 同上 (「1クリックは選択・ダブルクリックで開く」) |
| peer が配れない行も一覧から落とさない | 同上 (#111 の裁定「見せて理由を言う」)。試験: `fmtgate` F2/F3 |
| 直接の戸から入った doc は MEASURE を peer に投げない | `serverComputes` `core/app/open_dispatch.inc:408` |
| 単独フレームは Watch しない (手動 Reload) | `docs/watch-design.md §9` + `core/app/watch.inc:66-68` |
| 自動 reload は peer の stack に対して走らない | `watchAutoRefusal` `core/app/watch.inc:680-688` + `docs/watch-design.md §16.6` |
| `.raw` を LibRaw に渡さない | `core/imagefile.cpp:230-234` + `docs/input-adapters.md:577` |

---

## 9. `[P]` — 手で確かめたもの

**何を。** 形式の門 (`imagefile::forPath` / `peerServes` / `peerRefusal` /
`videoRefusal` / `dialogPattern` / `backends`) が**実際に返す値**。ソースを読んだ
結果ではなく、**出荷されるオブジェクトが答えたもの**。

**どうやって。** `viewer-serve` がリンクしているのと同一の
`build-vm/CMakeFiles/viewer-serve.dir/core/imagefile.cpp.obj` (と
`tiffread` / `exrread` / `y4mread` / `rawread` / `miniz` / `stb` / OpenEXR) に、
19個の名前を上の関数群に通して印字するだけの `main()` を**リポジトリの外で**
リンクして走らせた。`viewerReadsName` は `core/main.cpp` の中にいてリンクでき
ないので、`core/ui/menus.inc:668` の 3行をそのまま写した。

**結果 (抜粋)。**

```
name         forPath      vRead  peer   | peerRefusal
a.npy        -            1      1      |
a.npz        -            1      0      | .npz is read on this machine, but the peer serves
                                        | one array per file, not a container | browse it
                                        | locally (File > Browse Folder), or copy it here first
a.png        PNG          1      1      |
a.dng        vendor RAW   1      0      | vendor RAW is read on this machine, but the peer does
                                        | not serve it: LibRaw is CDDL-1.0 and viewer-serve
                                        | installs itself onto another machine over ssh | ...
a.raw        -            0      0      | the peer serves .npy, PNG, JPEG, TIFF, OpenEXR and y4m
a.bin        -            0      0      | (同上)
a.rggb       -            0      0      | (同上)
A.RAW        -            0      0      | (同上、大文字でも同じ)
a.mp4        -            0      0      | (同上)   VIDEO: MP4 (H.264/HEVC) needs a video codec...
a.vsession   -            0      0      | (同上)

dialogPattern: *.png *.jpg *.jpeg *.jpe *.dng ... *.tif *.tiff *.exr *.y4m
decodableFormats: PNG, JPEG, TIFF, OpenEXR, y4m
```

`vRead=0` かつ `peer=0` の行 (`a.raw` / `a.bin` / `a.rggb` / `A.RAW` /
`a.vsession`) が **G1 と G2 の実測**である。`a.mp4` の 2つの列が **G7 の実測**——
同じファイルについて 2つの関数が別のことを言っている。

**この PR で走らせた試験。** `bash tools/run_selftests.sh build-vm` →
`ran 50, skipped 0` / `100% tests passed, 0 tests failed out of 50` /
`run_selftests: PASS`、exit 0。この文書は**製品の振る舞いを 1バイトも変えて
いない**ので、この結果は base と同じである (この PR は文書のみ)。

---

## 10. この表の保ち方

**この文書は手で保つものではない。** 上の表のうち、形式の門の列
(`forPath` / `viewerReadsName` / `peerServes`) は `selftest.fmtgate` の F4 が
**表の全行について不変条件として** assert している——「peer が配れるものは
必ずこの viewer も読む」「`peerServesName(x) == b.overLink`」。だから
**表に行を足せば F4 が勝手に見る。**

**見ていないのは「表に行が無い形式」である。** `SEQ_EXTS` の 7つと `.vsession` は
どの不変条件の中にもいない。G1・G2・G3・G8 はすべてそこから出ている。

次に同じことを起こさないための最小の一手は、形式ごとの回避ではなく
**`SEQ_EXTS` を `viewerReadsName` / `openFileDialog` / `isRaw` の 3箇所が
参照する 1つの表にすること**である——`imagefile::backends()` が絵の形式に対して
やっているのと同じことを、ヘッダ無し形式に対してやる。#166 の
「名前付きレシピ」はその表の自然な置き場所になる。
