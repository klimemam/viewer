# アーキテクチャ

コードを変える人向けの見取り図です。**何がどこにあり、何を壊してはいけないか**を書きます。
用語は [docs/terminology.md](docs/terminology.md) が正典で、本書はそれに従います。

---

## 1. 全体像

ビルドされる実行ファイルは3つです。

| バイナリ | 中身 | 役割 |
|---|---|---|
| `viewer` | `core/main.cpp` + `plugin_host` + `ui_theme` + `serve` + `remote` + `app_icon` + `window_frame` + miniz | GUI 本体。`--serve` を渡すと peer にもなる |
| `viewer-serve` | `core/serve_main.cpp` + `serve.cpp` + `plugin_host.cpp` + `version.cpp` + miniz | ssh 越しに動く peer。**GL/GLFW/X11 に一切リンクしない**。`version.cpp` は `MOP_SET_FOLD` の provenance のため — 組み込みの解析には dll が無いので、「どのビルドが畳んだか」は viewer 版そのものになる |
| `mkicon` | `tools/mkicon.cpp` | ビルド時にアイコンを生成するホストツール |

加えて `plugins/*.c` が7つの `MODULE` ライブラリになり、`build/plugins/` へ出ます
(MSVC のマルチコンフィグでも同じ場所に出るよう、出力先に `$<1:...>` を使っています)。

`viewer-serve` が GL を持たないのは意図的です。計算ノードには GL のランタイムが
無いことがあり、あれば動的リンカが `--serve` に到達する前に起動を拒みます。
`plugin_host` は GL を含まないことが前提で、だから peer 側でも同じ analyzer が走ります。

### アプリケーションが1ファイルであること

GUI のほぼ全体 (`core/main.cpp`、約 29,000 行) は1つの翻訳単位です。
これは事実として受け入れてください。ここでの結合は「パネルが `App` という1つの
状態を共有し、1フレームの中で順に描かれる」というもので、ヘッダに切り出しても
その結合は減らず、`static` な内部関数が公開シンボルに変わるだけです。
探すときは本書のファイルマップと、関数名の規約 (`drawPanel*` / `draw*Modal` /
`*Selftest`) を使ってください。

---

## 2. 層モデルと、それを守っている場所

`frame ⊂ stack ⊂ series ⊂ batch` は厳密な包含です。層の**省略**はできますが
(単発 frame が batch に直接ぶら下がる、どの series にも属さない stack がある)、
**順序を飛び越えることはしません**。

| 層 | 実体 | 同一性 |
|---|---|---|
| frame | `ImageDoc` | `ImageDoc::uid` (`uint64_t`) |
| stack | `App::SeqInfo` | `SeqInfo::id`、frame 側は `ImageDoc::seqId` |
| series | `App::Series` | `Series::id`。所属は **`Series::members` が唯一の真実** |
| batch | `App::Batch` | `Batch::id`、frame 側は `ImageDoc::batchId` |

守られている場所:

- **Close の粒度** — `closeCurrent()` は frame が stack の一員なら **stack ごと**
  閉じます。1枚だけ閉じる逃げ道は `Ctrl+Alt+W` です。
- **series の所属** — `SeqInfo` に `seriesId` は**ありません**。2箇所が同じ事実を
  持つと必ずずれるので、逆引きは `seriesOfStack` が数個の series を走査します。
- **series は自動で作らない** — フォルダ構造からの推測は、外れたときに黙って嘘の
  掃引を作ります。作る経路は picker / Files の「Group as series」/ Linearity パネルの
  3つだけで、自動生成は `--lin-selftest` の `selftestMakeSeries` にしかありません。
- **series は1つの batch に収まる** — またぐ必要が出たら、先にメンバを同じ batch へ
  移してから作ります。
- **単位と値は「未設定」を持つ** — `Series::unit` の空文字は「未設定」であって
  既定値ではなく、`Member::value` の NaN は 0 ではありません。0 は測定値
  (linearity では暗電流 stack) なので、読めない文字列が 0 に落ちてはいけません。
- **σ_t は stack の属性** — per-frame の表に `sigma_t` 列は作りません。

`--series-selftest` は変更のたびに `seriesAudit` を回してこれらを検査します。

---

## 3. データモデル

### 3.1 `FrameSource` と `ImageDoc`

```
App::images : vector<unique_ptr<ImageDoc>>
                    |
                    +-- ImageDoc  ... 1つの「stack から見た frame」= メンバシップ
                          |          uid / seqId / seqIndex / batchId
                          |          black / white / tex / cfa / displayLut / dataRev
                          |
                          +-- shared_ptr<FrameSource> src ... 画素と素性
                                     data / w / h / ch / dtype / vmin / vmax
                                     path / npzMember / remoteUrl / rawDtype...
                                     srcId / rev / mtime / fsize
```

`FrameSource` は**画素と素性 (どこから来たか)** を持ち、`ImageDoc` は
**そのメンバシップ固有の状態** (同一性、位置、表示レンジ、解釈、テクスチャ) を持ちます。

現状 **1つの `FrameSource` の所有者はちょうど1つの `ImageDoc`** です
(`use_count() == 1`)。`shared_ptr` なのはこの分割のためで、frame の共有は
まだ行われていません。`ImageDoc` をコピーすると**ポインタがコピーされて同じ
source を指す**ので、画素を own すべきコピーは `cloneSource()` を通します
(新しい `srcId`、`rev = 0`)。

`w / h / ch / dtype / vmin / vmax` は `ImageDoc` 側にも**ミラー**として置いてあります
(240行以上から読まれるため)。source を作る・書き換えるコードだけが両側を書いてよく、
通常は source を埋めてから `syncMirrors()` を呼びます。

### 3.2 同一性とキャッシュキー

3種類の識別子があり、役割が違います。

| 識別子 | 何を指すか | なぜポインタでないか |
|---|---|---|
| `ImageDoc::uid` | frame のメンバシップ | 閉じて開き直すとポインタは ABA する。compare スロットもセッションもこれで指す |
| `FrameSource::srcId` | 画素の実体 | 会計上の重複排除 |
| `ImageDoc::dataRev` | その場での画素の書き換え (crop) | 中身が変わったのに uid が同じ、をキャッシュに教える |

統計パネル (Histogram / Projection / Temporal / ROI / Analysis) のキャッシュキーは
**その数値が依存するものを全部**含みます — `uid`、`dataRev`、`black` / `white`、
`cfa` / `cfaPattern`、注釈の版 (`annRev`)、プラグイン索引。
数値が古い入力を黙って説明することがないようにするためで、
Analysis パネルは stale を検出したら計算済みの値を残したまま
「inputs changed - Run (M)」を出します。

`FrameSource::rev` はプレビュー→フル解像度の差し替えのように**画素を丸ごと
置き換えた**ときに上がります。

### 3.3 `SeqInfo` (stack) が持つもの

`id` / `name` / `lastImageIdx` に加えて、リモート由来なら `remoteUrl` `remoteHost`
`remotePort` `remoteFiles` `expectedFrames`、CFA の既定、そして
**フレーム軸** (`axisName` / `axisUnit` / `axisVals`) を持ちます。

フレーム軸は正当に stack の属性です (この stack のフレーム番号だけを写像するため)。
名前・単位・値の3つが揃って初めて軸になるので、3つとも必須で、単位に既定値は
与えません。一方、**stack が撮られたパラメータ値は `SeqInfo` にありません** —
名前と単位が無い値は意味を持たず、名前と単位は series のものだからです
(`Series::Member::value`)。

---

## 4. フレームループと描画

`main()` は `--serve` を GLFW 初期化の**前**に横取りし、peer モードならそのまま
`rp::runServeMode()` を返します。GUI の場合は prefs → ウィンドウ → ImGui →
プラグイン読み込み → `parseCli` の順です (プラグインは `parseCli` より前に読むので、
CLI から analyzer を指定できます)。

1フレームの流れ:

1. イベント待ち (`glfwWaitEventsTimeout`) — **常時再描画はしません**。
   入力・ワーカの完了・アニメーション (blink、スピナ) が起きたときだけ描きます。
   `app.wakeFrames` と `g_inputSeq` がその調停役です。
2. ワーカからの結果を回収し、`App` に反映する (UI スレッドのみが `App` を書く)
3. `##root` ウィンドウと `MainDock` ドックスペースを出す
4. 各パネルを `drawPanel*` で描く。パネルの表示状態は `app.show*` で、
   配置は Dear ImGui が `layout.ini` に持ちます
5. ステータスバー、トースト、モーダル
6. テクスチャの更新 (`texDirty` のものだけ) と描画

パネルの描画関数は表のとおりです。

| パネル | 関数 |
|---|---|
| Files | `drawFileList` |
| Browse (インスタンス毎) | `drawPanelRemote(App::BrowseInstance&)` |
| Image View | `drawCanvas` |
| Inspector | `drawInspector` |
| Histogram | `drawPanelHistogram` |
| Projection (H/V) | `drawPanelProjection` |
| Temporal | `drawPanelTemporal` (+ `drawTemporalAB`、`drawFrameLinSection`、`drawBrowseTemporal`) |
| Linearity | `drawPanelLinearity` (+ `drawSeriesModal`) |
| ROIs | `drawPanelRois` |
| Analysis | `drawPanelAnalysis` (+ `drawAnalysisPlots`) |
| Messages | `drawMessagesPanel` |

**プラグインは決して描きません。** 曲線は `emit_series` で出され、
`drawAnalysisPlots` が全部を同じ流儀で描きます。

`core/window_frame.cpp` は「メニューバーに描く独自タイトルバー」を実装します
(`--frame integrated`)。ドラッグ・リサイズは Win32 と X11 (`_NET_WM_MOVERESIZE`)
に渡します。Wayland ではシステムのフレームを使います。

---

## 5. ローダとシーケンスワーカ

`openPath` が拡張子で分岐します。`.npy` → `loadNpy`、`.npz` → `loadNpz`
(miniz による最小の zip リーダ)、`.vsession` → セッション復元、
`ssh://` / `local://` → リモート、それ以外で raw のパラメータが揃っていれば
`loadRaw`、揃っていなければ RAW モーダル。

**連番の検出**はファイル名を文字と数字の交互のセグメントに分解し、
そのフォルダの中で**実際に変化する数字の run** をフレーム番号とみなします。
変化しない数字は数字のまま残ります (`gain10_000‥007.npy`)。
2本以上の run が変化する場合は条件混在なので stack を分割します。
表示名は範囲付き (`10lx/frame_000‥023.npy`、区切りは U+2025) で、
**クライアントと peer が同じ関数 `rp::patternWithExtent` を使います** — 両端が
一字一句一致しないと、同じフォルダが2つの違う名前で出ます。

**シーケンスワーカ** (`app.seqThread`) は1本で、デコードだけを行います。
`App` にも GL にも触りません。ジョブは値でキャプチャして渡し、結果はキューを介して
UI スレッドが取り込みます。メモリ予算 (既定は物理 RAM の 60%、`--mem-budget` と
View メニューで変更可) を超えないよう、次の stack はローダが空いてから始まります。

リモート側には別のワーカがあります: Browse インスタンス毎の `rbWorker`
(一覧・検索・スキャン)、`rfWorker` (フレーム軸ファイルの残りフレームの先読み)、
`mWorker` (サーバ側 MEASURE)。

**スレッド所有の規則:** `remote::Session` はスレッドセーフではないので、
**1つの Session につき触るスレッドはちょうど1つ**です。
`BrowseInstance::session` はそのインスタンスのワーカ専用、`App::uiSession` は
UI スレッド専用、`rfWorker` / `mWorker` は関数ローカルの Session を持ちます。
ワーカは結果 (`RbResult`) と一緒に `peerVersion` などを**値で公開**し、
UI はその平文の値を読みます。競合する2者の片方だけが取る mutex は何も守りません。

---

## 6. テクスチャ

テクスチャは必要になったときに作られ、`texDirty` か、作ったときの
`texBlack`/`texWhite` が現在の表示レンジと違えば作り直されます。

常駐数は `touchTex()` が **`TEX_KEEP = 12`** に抑えます。順序は**生成順ではなく
使用順**です — A/B compare の B はしばしば古い doc でありながら毎フレーム必要で、
挿入順の LRU では追い出しと再アップロードを繰り返してしまいます。
画面に出ているもの (`cur()`、`cmpB()`、今 touch したもの) は決して追い出しません。

画素そのもの (`FrameSource::data`、常に `float`) はメモリ予算の対象で、
テクスチャとは別の勘定です。

---

## 7. リモートの層構造

```
UI (main.cpp)
  |  openRemote / startRemote / rbWorker / rfWorker / mWorker
  v
remote::Session            core/remote.h, remote.cpp
  |  parseUrl / startOn / list / meta / tile / measure / glob / scan
  |  spawn: ssh -o BatchMode=yes ... <exe> --serve      (host あり)
  |         <exe> --serve                               (host なし = local://)
  v
--- stdin/stdout パイプ、フレーミングは rp::Header ---
  ^
  |  handleRequest -> handleList / handleMeta / handleTile /
  v                   handleMeasure / handleGlob / handleScan
rp:: (core/remote_proto.h, core/serve.cpp)
```

**フレーミング** — 12バイトのヘッダ (`magic 'VRP1'` / `type` / `len`) + ペイロード。
スカラはリトルエンディアンのパック、文字列は `[u32 len][bytes]`、64bit 値は
lo/hi の u32 対。クライアントは 512MB 超の返信を、サーバは 64MB 超の要求を拒みます。

**バージョン** — `rp::VERSION = 9`。HELLO で双方向に交換し、サーバはクライアントの
版に合わせて LIST の形を選び、クライアントは機能を版で gate します。
番号が動く理由は2種類あり、**どちらもワイヤ形式とは限りません**。v4/v5/v6 は
形式を一切変えず**意味**だけを縛りました (連番のグループ化規則、表示パターンの
範囲表記、`sigma_fpn` が補正済みの量になったこと) — 古い peer が黙って違う答えを
返すのを防ぐためです。v7/v8/v9 は枠が増えた側で、それでも番号を上げる理由は
**拒否が読めること**: 古い peer の「unknown measure op」は打ち間違いの拒否と
区別がつかないので、client が**送る前に**数から断って「古い」と「不一致」を
言い分けます。v9 だけは逆向きで、古い peer は**拒否しません** — TILE 末尾の
宣言された読み方を読まないまま自分の読み方で成功を返すので、番号が無ければ
「間違った絵が正しいラベルで出る」ことになります (issue #124)。
理由は `core/remote_proto.h` の `VERSION` の上に版ごとに書いてあります。

| opcode | 何をするか |
|---|---|
| `MSG_HELLO` | 版の交換 |
| `MSG_LIST` | 1ディレクトリの一覧。v3 以降は mtime、`.npy` ヘッダの覗き見、連番グループ行を含む |
| `MSG_META` | `w/h/ch/dtype/frames` — 画素が動く前にレイアウトを決めるための24バイト |
| `MSG_TILE` | 領域の画素。**元の dtype のまま**(値が変わらない)、必要なら deflate |
| `MSG_MEASURE` | サーバ側の測定。analyzer / temporal stats / frame ROI stats |
| `MSG_GLOB` | 再帰検索 |
| `MSG_SCAN` | サブツリーの stack 発見 (リモートの Open Folder) |
| `MSG_OK` / `MSG_ERR` | 成功 / エラー文字列 |

**peer は「読み手」であって「ローダ」ではありません。** `.npy` のヘッダを解析して
seek し、要求された行だけを読みます。フレーム全体をメモリに置く箇所はありません。
ビッグエンディアンはサーバ側で byteswap します。

**サーバ側で走るもの:** 一覧、メタ、領域読み出し、検索、そして**測定**
(per-pixel の temporal stats、per-frame ROI stats、および `dlopen` した
**ローカルと同じ analyzer プラグイン**)。返るのは結果のグリッドと系列だけで、
画素は返りません。描画・表示変換・取得済みタイル上の ROI 統計はすべて手元です。

**`local://` はトランスポートです。** ホストが空のとき `startOn` は ssh を挟まず
`<exe> --serve` を素の子プロセスとして起動します。この `<exe>` は既定で
**動いている `viewer` 自身** (`app.exePath`) で、`viewer` が `serve.cpp` を
リンクしているから成立します。つまり `local://` は ssh 以外の全経路 — フレーミング、
ハンドラ、コーデック — をそのまま通ります。リモートのコードを2台目のマシン無しに
テストできるのはこのためで、Browse 系のセルフテストはすべてこれを使います。
`--remote-exe` に `viewer-serve` を渡せば、本物の peer バイナリを手元で検証できます。

**帯域** — 最初の一枚は画面の画素数に合わせて間引いて取り (`step`)、その後
バックグラウンドでフル解像度を取り直します。`remoteStep > 1` が「まだプレビュー」の
印です。TILE のみ deflate を掛け、**縮まなかったら生で送ります**
(圧縮できないセンサデータが損をしないように)。一覧はインスタンス毎に
`treeCache` に載りますが、画素のキャッシュはありません — 届いた frame は
普通の `ImageDoc` になり、以後は0バイトで測れます。

---

## 8. プラグイン ABI の境界

契約は [include/ps/ps_plugin.h](include/ps/ps_plugin.h) の1ファイルです。
**壊さず、新しい V2 構造体と `reserved` フィールドでのみ拡張します。**

- 公開シンボルは `psRegisterPlugins(const psHostApi*)` ひとつ (0 = 成功)
- `PS_ABI_VERSION` は **2**。ホストの版は `psHostApi::abi_version`、
  各記述子は**自分の版**を持ち、ホストは `>=` ではなく**完全一致**で照合します。
  だから V1 の analyzer は `abi_version = 1` のまま v2 ホストで動き続けます
- 種別は「どの `register_*` を呼んだか」で決まります: display (値→色、画素は不変) /
  analyzer (数値、画素は不変) / processor (Frame → 新しい Frame)
- 記述子はホストが**値でコピー**します。中の `const char*` はプロセス寿命が必要です
- **画素メモリはホストが確保しホストが解放します** (`frame_alloc` / `frame_free`)。
  Windows で CRT が食い違ってもヒープが壊れないようにするためです
- display の `fill_lut` は**登録時に1度だけ**呼ばれ、256段の表がキャッシュされます
- 結果は戻り値ではなく**コールバックで押し出します**: `emit_number` /
  `emit_text` / `emit_series` (V2)。ホストは戻る前に全部コピーします
- processor の出力フレームは**使う前に契約違反を検査します** (非 NULL、F32、CPU、
  `1 ≤ ch ≤ 4`、`1 ≤ w,h ≤ 32768`、`pitch_bytes` が足りているか)。
  行儀の悪いプラグインがホストを落としてはいけません
- ホットリロードはありません。`unloadAll()` は終了時に1度だけ呼ばれます

`plugin_host.cpp` は GL を含みません。だから `viewer-serve` にリンクでき、
サーバ側 MEASURE がローカルと同じ analyzer を走らせられます。

analyzer 名は `category/name` で、ホストが分割して2段のコンボと Measure メニューの
グループ分けに使います。Measure メニューは**プラグインのファイル単位ではなく問い単位**で
まとめます (`MEASURE_GROUPS`: Noise / SNR、Resolution / Focus、
Uniformity / Flat field、Statistics / Sanity)。知らないカテゴリは自分の名前のまま出ます。

---

## 9. セッションと設定ファイル

置き場所は Windows が `%APPDATA%\viewer\`、それ以外が `$HOME/.config/viewer/`
(`viewerConfigDir`)。環境変数が無ければアプリは動きますが何も永続化しません。

### `prefs.txt` — 「アプリがどう振る舞うか」

行志向のテキスト。1行目が `viewer-prefs 1`、以降は `key value`。
テーマ、compact、dragpans、wheelzoom、fitonswitch、seqload、showfps、
lowbandwidth、membudget、procpolicy、linunit、gamma、grid、frame、
Browse の表示形 (`rbflat`/`rbtree`/`rbnatural`)、`remoteexe`、`remoteurl`、
そして繰り返しの `rbookmark` / `rbrecent` (履歴は10件まで)。
パスを含む値は行の残り全部を値として読みます。

**`--secondary` のウィンドウは prefs を読みますが書きません。** 1つのファイルに
last-writer-wins で書くと、派生ウィンドウが本体の設定を上書きするためです。

### `*.vsession` — 「今なにをどう開いていたか」

行志向のテキスト、1行目が `viewer-session 1`。
ビュー状態 (`zoom` / `center` / `current` / `rangescope` / `compare` / `cmpslot` /
`panels` …)、Browse インスタンスの場所 (`rbplace`、復元時に自動再接続)、
`layout_begin`〜`layout_end` に挟んだ ImGui の dock ini (各行に `|` を前置)、
そして画像ごとの**再読込レシピ** (パス、raw のパラメータ、`.npz` メンバ、LUT、
batch は**名前で**、stack 名、series ブロック、フレーム軸)。

保存されるのは**手配**であって画素ではありません。だから montage のような
派生画像は元ファイルが無いので飛ばし、**飛ばした数を報告します** (黙って落としません)。
batch を名前で復元する以上、名前は一意でなければならず、衝突には ` (2)` を付けます。

### 自動保存とインスタンス

起動時に `autosave-N.lock` を `O_EXCL` で取って枠を確保します。
1番目は歴史的な名前 `autosave.vsession`、以降は `autosave-2.vsession`, … です。
lock には所有者の PID が入っていて、**PID が死んでいる lock は stale = 回収可能**
です。クラッシュがわざと lock を残すのはこのためで、復旧の提案
(`recoveryOfferIn`) は「生きているインスタンスが持っていない、いちばん新しい
autosave」を選びます。

---

## 10. セルフテスト

GUI を出さず、stderr に `<name>selftest: ...` を出して 0/1 で終了します。
**22個**あり、`--help` に載っているのは8個です。
`--browse-keys-selftest` だけが実際に ImGui のフレームを回します
(隠しウィンドウ)。

| フラグ | 何を守っているか |
|---|---|
| `--verify-selftest <dir>` | 他が取りこぼす角: 非連続 stack、compare-B の dangling、prune と `closeBatch`、batch 名の衝突、preview をセッションに書かないこと、4面モザイク統計、DN 表記、n=N/N の正直さ、NaN の除外、メモリ予算の勘定、CFA プレーン別のヒストグラム/ROI 抽出 (V1-V18) |
| `--lin-selftest` | series のリニアリティ/PTC が、仕込んだ感度・ゲインを取り戻せること |
| `--frame-lin-selftest` | フレーム毎リニアリティ。厳密な平均を持つ合成 stack で R²=1、+2% の折れの検出、両方のフィット手法、n-of-N とエクスポート、軸未設定の空状態 |
| `--series-selftest <dir>` | series モデルの不変条件を、変更のたびに `seriesAudit` で全数検査 |
| `--sweepfile-selftest <dir>` | 1レベル1ファイル (フレーム軸つき `.npy`) の掃引。名前がローカルとリモートで一致すること、1 series になること、フィットが仕込み値を返すこと |
| `--abstats-selftest <dir>` | 5つの統計パネルの2スロット化。B の数値を第2実装と 1e-6 で照合、A が compare-off と**バイト一致**すること、stack 属性の temporal が frame 送りで再計算されないこと |
| `--tile-selftest <dir>` | side-by-side のペイン幾何を算術として (この機械では GL が白を返すため)。スロット順、重なり無し、全ペインで1つのズーム |
| `--derive-selftest <dir>` | 派生 stack の件数・コピー・追従。元 stack を壊さないこと、非常駐の一致フレームを取りに行かないこと |
| `--newwin-selftest <dir>` | autosave の枠取り、stale lock の回収、復旧の提示、secondary が prefs を書かないこと、`newWindowArgv` が組む argv の完全一致 |
| `--export-selftest <dir>` | 画を外に出す経路をバイト列で。PNG の IHDR、montage の寸法と CFA 位相、タイル0が frame0 の ROI と画素一致 |
| `--export-tsv-selftest <dir>` | Temporal 統一エクスポートの文字列 — 見出し、素性行、単位、n-of-N、領域の申告、CSV 方言、フレーム軸の往復 |
| `--framestats-selftest` | per-frame TSV を stdout に出す (numpy で再現するための golden 出力。内部アサート無し) |
| `--range-selftest <dir>` | A/B の表示レンジの全組み合わせ、軸の命名、追従、auto の再フィット |
| `--picker-selftest <dir>` | picker の5ユースケース (merge、フィルタ、トップフォルダ毎の batch、単一フレーム) |
| `--close-selftest <dir>` | Ctrl+W の層ごとの意味。取得中に閉じた stack が先読みキューから蘇らないこと |
| `--batch-selftest <dir>` | batch 移動が stack を丸ごと動かし、Files の並びを乱さず、空 batch を刈り、名前が往復すること |
| `--scan-selftest <dir>` | リモート Open Folder を実経路 (local peer → rb worker → LIST → グループ化 → picker) で |
| `--browse-selftest <dir>` | Browse パネルの挙動: grouped↔flat が1つの返信上の純粋なビューであること、ツリーの遅延展開、キャンセル世代、壊れた `.npy` も一覧に出ること |
| `--browse-keys-selftest <dir>` | キー配送とプレビュー。パネルに focus がある間は主ビューのハンドラが引くこと、ダブルクリックがちょうど1回開くこと |
| `--localbrowse-selftest <dir>` | `Browse Folder (Local)` がローカル peer で着地し、**そう正直に名乗る**こと (タイトルに ssh を出さない、`local://` を漏らさない) |
| `--rtemporal-selftest <dir>` | 誰も開いていない stack のサーバ集計が detached で返り、σ_t / σ_fpn / mean が独立なローカル float64 集計と 1e-9 で一致すること |
| `--remote-selftest <path>` | ssh 無しで peer を起こし、META とローカルデコーダを照合、LIST v3 の形を検査 |

`tools/verify/` には、実際に取得した出力に対して不変条件を当てるシェルスクリプトが
あります (`invariants.sh` が §11 の B-1〜B-5)。

CI (`.github/workflows/build.yml`) は **windows / ubuntu / macos の3面マトリクス**で
ビルドし、Linux でのみ `xvfb-run ./build/viewer --bench 120 ...` を回して
フレーム時間の中央値が 50ms を超えないことだけを見ます (ランナーはソフトウェア GL
なので、これは破滅的退行の門であってベンチマークではありません)。あわせて
`--bench` 自身が出す **パネル被覆行** (`benchcov: ... PASS`) も見ます —— 開いた
パネルのうち実際に描かれたものを数えて名乗る行で、選択されていないドックタブは
`ImGui::Begin` が false を返して何も描かないため、これが無いと「フレーム時間」が
自分の名乗るより小さい仕事の数字になり得ます (ba40ba8 から 815173c までが実際にそれ)。
同じ表明は `selftest.benchcov` として3面マトリクスの中にもあります。

**セルフテストは3面すべてで走ります** (`tools/run_selftests.sh` が CI と手元の
共通入口)。22本のうち実 ImGui フレームを描く5本 (`browse-keys` / `abstats` /
`verify` / `tile` / `frame-lin`) だけが GL コンテキストを要り、Linux では
xvfb + ソフトウェア GL で走ります。残り17本は `--no-window` 起動経路
(ウィンドウも GL も作らない) を通るので、GL の無い windows / macos ランナーでも
走ります。どちらに属するかは `CMakeLists.txt` の `viewer_selftest(...)` 行の
`NOGL` の一語が唯一の宣言で、この語がラベルと `--no-window` の両方を出します。
走れなかったものは**名指しで** skip と報告され、pass には決してなりません。

---

## 11. 壊してはいけない不変条件

測定ツールなので、次は仕様ではなく**正しさ**です。
`tools/verify/invariants.sh` が実出力に対して検査します。

- **B-1 σ_t は stack の属性であって frame の属性ではない。**
  per-frame の表に `sigma_t` 列を作らない。エクスポートの temporal 要約は
  「stack statistics」と名乗る。
- **B-2 CFA プレーンを統計で混ぜない。** モザイクなら R/Gr/Gb/B は常に別系列で、
  プールした列を出さない。ヒストグラムも ROI 統計もアナライザも同じです。
- **B-3 部分ロードは「何枚中何枚か」を必ず併記する。** 裸の件数は不足を隠します。
  `resident n of N frame(s)`、表には `n` と `N` の列。
- **B-4 ラベルに乗った量は単位を連れる。** `sigma_t [DN]`、`sigma_frame [%]` —
  絶対量と相対量が同じ見た目で並ばないこと。
- **B-5 画素値は [DN]。**

加えて:

- **1回の Open は batch を1つだけ作る** (picker で「トップフォルダ毎」を指定した
  ときのみ複数)。同じフォルダを開き直せば新しい batch です。
- **表示に出るパターン範囲は、クライアントと peer で一字一句一致する**
  (`rp::patternWithExtent`)。
- **リニアリティに EMVA 1288 準拠を主張しない。** フィット窓は EMVA スタイル、
  手法は `3.1-style` / `rev4-style` で、画面のラベルもエクスポートも "-style" と書き、
  「NOT an EMVA 1288 compliance claim」の一文を出します
  (この一文の存在自体をセルフテストが検査しています)。
- **プラグインは描かない。** 曲線は `emit_series` で出し、描くのはホストです。
- **`App` を書くのは UI スレッドだけ。** ワーカはキューに積みます。
- **1つの `remote::Session` に触るスレッドは1つ。**

---

## 12. ファイルマップ

```
core/
  main.cpp          GUI 本体。App、全パネル、ローダ、セッション、CLI、セルフテスト
  remote.h/.cpp     remote::Session — URL 解析、プロセス起動、要求/返信
  remote_proto.h    ワイヤ形式。両端が共有する定数と、一致必須の関数
                    (rp::naturalLess / rp::patternWithExtent)
  serve.cpp         peer 側のハンドラ (list/meta/tile/measure/glob/scan)
  serve_main.cpp    viewer-serve の main
  setfold.h         set 解析のうち「画素の居る側で走る」半分。両端が呼ぶ1つの
                    実装 (畳み込みと平面別の集約、パリティ用の form 宣言)
  shading_probe.h   シェーディング量の測定 (2次面) — 同じ理由で両端が呼ぶ
  plugin_host.h/.cpp  プラグインの探索・読み込み・検証・登録簿 (GL 非依存)
  ui_theme.h/.cpp   ダーク/ライトとアクセント色
  window_frame.h/.cpp  メニューバーに描くタイトルバー (Win32 / X11)
  app_icon.h/.cpp   実行時のウィンドウアイコン

include/ps/ps_plugin.h   プラグイン ABI。このファイルが契約

plugins/          同梱プラグイン (すべて純粋な C)
  analyzer_stats.c      stats/moments
  analyzer_noise.c      noise/floor
  analyzer_prnu.c       uniformity/prnu-fpn
  analyzer_sharpness.c  sharpness/gradient
  analyzer_esfr.c       iso12233/e-sfr
  display_falsecolor.c  viridis
  proc_demosaic.c       demosaic (bilinear)

tools/
  gen_testdata.py       セルフテスト用フィクスチャの生成
  mkicon.cpp            アイコン生成 (ビルド時)
  install_shortcut.*    デスクトップショートカット (cmd / ps1 / sh)
  verify/               実出力に不変条件を当てるスクリプト

docs/             用語の正典、マニュアル、設計文書 (README のドキュメント表を参照)
CMakeLists.txt    ターゲット、FetchContent、プラグインの追加規則
.github/workflows/build.yml   3 OS のビルドと Linux での bench ゲート
```

`main.cpp` の中を探すときの手掛かり:

| 探しもの | 手掛かり |
|---|---|
| パネル | `drawPanel<名前>` |
| モーダル | `draw<名前>Modal` |
| データモデル | ファイル先頭 (`FrameSource` / `ImageDoc` / `App`) |
| CLI | `parseCli`、および `main()` の中の早期 `strcmp` 走査 |
| セルフテスト | `<名前>Selftest`、フラグは `--<名前>-selftest` |
| リモートの UI 側 | `openRemote` / `startRemote` / `rbWorker` / `rfWorker` / `mWorker` |
</content>
