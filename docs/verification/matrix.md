# 形式 × 入口 × 操作 の検証マトリクス

> **読み方:** 本書は**現況台帳**である。3軸の定義 (§0・§1) と、軸の交差から
> 落ちていた事実の現況 (§7・§8・§10) を置く。**2026-08-11 の測定そのもの**——
> 表A / 表B / 表C の 299セル、セルの数え、どのセルを selftest が証明しているか
> (旧 §2〜§6)、および `[P]` の実測 (旧 §9)——は
> [results/20260811-matrix.md](results/20260811-matrix.md) へ凍結した
> (2026-09-27 分離)。**凍結測定と現況台帳を1つのファイルに同居させない**のが現在の
> 規則で、割る境界は節番号ではなく**凍結された実測かどうか**である。番号は詰めずに
> 空けてある——§7 以降を指している既存の参照を動かさないため。

本書でいう「入口」は、ファイルやデータを製品へ取り込む操作経路を指す。たとえば
File > Open、Browse、フォルダ走査、リモート接続、リーダはそれぞれ別の入口である。

このリポジトリは「どの形式が読めるか」を `core/imagefile.h` の表で、「どの入口から
入れるか」を `openPath` / `openRemote` / `scanFolderGroups` / `serve.cpp` の述語で、
「入ったあと何ができるか」を各パネルで決めている。**その3つを一度に見る場所が
無かった。** 代償が 2026-08-11 に「remoteで、.rawが開けないね」として現れた経緯と、
そのとき数えた 299セルは [results/20260811-matrix.md](results/20260811-matrix.md)
にある。

**判断の服を着た落穂は、マトリクスにしないと見えない。** §7 がその一覧の現況で、
§10 が同じことを次に起こさないための保ち方である。

---

## 0. 読み方

セルは必ず次の4状態のいずれかを明示する。

| 記号 | 意味 |
|---|---|
| **○** | 通る。根拠キー付き (selftest 名 = 最強、無ければ symbol か手で確かめた記録) |
| **×決** | 断る。**判断が記録されている**——issue 番号 / 文書の節 / コードのコメント |
| **×落** | 断る。**何も決めていない。届かないだけ。**←これが見つけたいもの |
| **—** | 対象外。理由を添える |

根拠キーの体系:

- `[T:name]` — selftest。`viewer_selftest(name ...)` (CMakeLists.txt) で CI が走らせて
  いるもの。**セルの証拠としては最強**
- `[S:symbol]` — コードを読んだ。**関数名・配列名で指す**。ファイル名は添えてよいが
  **行番号は書かない**
- `[D:...]` — 文書の節 anchor / issue 番号
- `[P]` — **手で確かめた**。何をどう走らせたかは
  [results/20260811-matrix.md](results/20260811-matrix.md) §9

**現況側に `file:line` を書かない。** 行はコミットごとに動くので、生きた台帳に
書くと「読んだ時点では正しかった」しか意味しなくなる。行で指してよいのは対象
コミットを宣言した凍結結果だけで、そちらは
[results/20260811-matrix.md](results/20260811-matrix.md) が `[C:file:line]`
(行は `9d307b8` 時点) として持っている。

---

## 1. 軸

軸は3つ。どれも**今日のコードから数え直せる**定義として置く——2026-08-11 に
数えた 13 × 9 × 7 の内訳そのものは
[results/20260811-matrix.md](results/20260811-matrix.md) §1 にある。

| 軸 | 定義 | 今日の正典 |
|---|---|---|
| 形式 | 読める / 読めないを分ける単位。**表に行を持つ絵の形式**と、**行を持たない自己記述・ヘッダ無し・コンテナ・セッション**を別に数える | `imagefile::backends()` の表 (`Backend::overLink` を含む)、`SELFDESC_EXTS` / `HEADERLESS_EXTS` (`core/app/sequence.inc`)、`.vsession` |
| 入口 | データを製品へ取り込む操作経路。**同じ形式が入口によって違う答えを返すなら、それは別の行として数える** | `parseCli` / `dropCallback` / `openPath` / `openFolder` / `openFileDialog` / `openFolderDialog` / `scanFolderGroups` / `browseLocalFolder` / `startRemote` / `openRemote` / `loadSession` / `readerFor` → `openWithReader` |
| 操作 | 入ったあとにできること | `ImageDoc` が1つ立つ、`scanFolderGroups` / `startSequenceLoad` / peer `SCAN`、`copyPerFrameStats`、`handleMeasure`、`watchTargetsNow`、`reloadSource` / `planStackMembership`、`exportDocRGBA` |

**由来で操作の表を割る。** peer 側 MEASURE は `serverComputes` が
「`remoteUrl` も `remoteFiles` も空なら false」と決めているので、直接の入口から
入った doc には**構造上あり得ない**。ゆえに操作の表は必ず2枚になる——直接の入口
から入った doc (このディスクのファイル) と、Browse 経由で `remoteUrl` を持つ doc。

**形式軸を入口で割る根拠の実例:** ヘッダ無し RAW のうち `.rggb` だけが
`SEQ_EXTS` にあって File ▸ Open のフィルタに無かった (§7 G8)。同じ形式が入口に
よって違う答えを返したので、別の行として数える。

---

> **§2〜§6 は本書に無い。** 表A (形式 × 入口 117セル)、表B / 表C
> (形式 × 操作 各91セル)、セルの数え、selftest が証明しているセルの対応表は
> すべて [results/20260811-matrix.md](results/20260811-matrix.md) にある。§9 の
> `[P]` 実測も同じ理由でそちらにある。
> 再測定するときは新しい `results/<YYYYMMDD>-matrix.md` を作り、本書の §7・§8・§10 を
> 更新する——凍結した測定を書き換えない。

---

## 7. 落穂 (fall-through) — 見つかったもの

**噛みやすい順。** 各件について「決めて記録する」と「道を開く」の両方を書く。
**下の一覧が現況である。**[results/20260811-matrix.md](results/20260811-matrix.md)
のセルは測定の記録なので直した後も書き換えない——あちらは「2026-08-11 に何が
出荷されていたか」で、こちらは「今どうなっているか」である。

各件の「**どこ。**」以下は 2026-08-11 の調査そのままで、そこに出る行番号は
**当時の所見**である。生きた指し先は関数名・配列名のほうで、行は照合の助けに
すぎない (§0 の規則)。

| | 件 | 状態 |
|---|---|---|
| G1 | ヘッダ無し RAW が peer 越しに開けない | **済** PR #185/#186/#187/#188/#189/#278 — protocol 11 (メンバーサイズの検算は 16)。試験 F4d F4e F4f F4h P8 P8b P9 P9b。**2026-08-13 に「済」と書いたのは早すぎた**: レシピを訊く扉は Browse の行だけで、url 名指し・フォルダ stack・Reload・Watch・session 復元は断られ続け、段1 の拒否文も差し替え忘れていた (ユーザー報告 2026-09-29)。扉の表は remote-headerless-design.md §12.1 |
| G2 | ローカル Browse が偽の拒否理由を出す | **済** PR #174 — 試験 F4b |
| G3 | `.npz` のフォルダが幾何プロンプトへ | **済** PR #173 — 試験 UC8 |
| G4 | 間引かれた preview を export できてしまう | **済** PR #170 — 試験 E5/E6 |
| G5 | リモートのファイル内フレーム軸が復元で 0 に戻る | **済** PR #177 — 試験 browse restore-wait |
| G6 | reader で開いた doc の復元が memo 頼み | **一部** PR #181 — 黙らなくなった。試験 V25p。(a) session に spec を書くかは信頼の問い(#179) |
| G7 | `.mp4` が peer 一覧で理由と逃げ道を失う | **済** PR #176 — 試験 F3b |
| G8 | `.rggb` が File ▸ Open のフィルタに無い | **済** PR #175 — 試験 F4c |
| G9 | generic な peer 拒否文に逃げ道が無い | **済** PR #176 — 試験 F3b |
| G10 | 間引かれた doc の crop が復元で黙って落ちる | **済** PR #177 — 試験 browse restore-wait |
| G11 | §4.13.1「adapter は peer で走る」が未実装かつ未拒否 | **全段済** PR #211 / #218 / #219 / #221 / 本 PR — 拒否が判断を言い (F4d2)、reader が peer で走り (protocol 12)、木も渡り (13)、測定も向こうで走る (14)。試験 `--rreader-selftest` / `--rnpz-selftest` / `--rmeasure-selftest`。設計は `docs/features/remote/remote-reader-design.md` |

---

### G1. ヘッダ無し RAW が peer 越しに開けない ← 2026-08-11 の報告 —— **修正済 (protocol 11)**

**どこ。** `core/imagefile.cpp:213-283` の表に `.raw` の行が無い →
`imagefile::peerServes()` (`:379`) が false → `peerRefusal()` (`:402`) の最後の
`return` に落ちる。

**実測 `[P]`:**

```
a.raw   forPath=-  viewerReadsName=0  peerServes=0
        "the peer serves .npy, PNG, JPEG, TIFF, OpenEXR and y4m"
```

**何が問題か。** 断ること自体ではない。**断り方**である。名指しは呼び出し側が
やっている (`open_dispatch.inc:1748` が `baseName(rpath) + ": "` を前置する) が、
残りの2つが無い —— (a) **理由が「表に無いから」であって、この形式について
何も言っていない**、(b) **逃げ道が無い** (§3.2 の三部構成のうち三つ目)。
ベンダ RAW の断り (`vendor RAW is read on this machine, but the peer does not
serve it: LibRaw is CDDL-1.0 …` + `browse it locally, or copy it here first`) と
並べると差が分かる——あれは**決めてある**。

**判断が無いことの確認。** issue #166 (ベタ RAW を名前付きレシピのパネルにする) は
レシピの永続化の話で、リンクには触れていない。`docs/features/adapters/input-adapters.md:577` は
「`.raw` は LibRaw に**取らせない**」を決めているが、これは vendor library に渡すな
という話で、peer に渡すなとは書いていない。`docs/features/remote/remote.md` にも記載無し。

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

### G2. ヘッダ無し RAW と `.vsession` が「このマシンの」Browse でも淡色になり、偽の理由を言う —— **修正済 (PR #174)**

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
`.vsession` なら `loadSession` が走る。**同じディスクの2つの入口が同じファイルに
ついて逆のことを言っている**——#148 が 1段上で言っていた欠陥そのもので、
`docs/features/adapters/input-adapters.md:88` の「同じファイルがローカルとリモートで違う読まれ方を
するのは欠陥」がここにも掛かる。

しかも `.raw` は**フォルダ単位では Open Folder が束ねる** (`SEQ_EXTS` にある) ので、
「Browse で見えているのに開けない、File ▸ Open Folder なら stack になる」という
状態になっている。

**正直な選択肢。**

- **(a) 入口を対応させる。** `viewerReadsName` に `SEQ_EXTS` と `.vsession` を足す。
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

### G3. `.npz` のフォルダが RAW ダイアログに送られる —— **修正済 (PR #173)**

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
(`docs/features/adapters/npz-design.md:151`) なので、**`.npz` だけが入ったフォルダ**は普通に存在する。
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

**どちらにせよ、`isRaw` の述語は「表に行があるか」ではなく「ファイルが自身の形状を
記述するか」を判定すべきである**——それが上の引用でいう「形を名乗る」の意味である。

---

### G4. 間引かれた preview がそのまま export される —— **修正済 (PR #170)**

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
出している** (`core/ui/canvas.inc:1049`) ——つまり「preview を全解像度画像として扱わない」は
このリポジトリの既知の規則で、**export だけがそれを守っていない。**

**正直な選択肢。**

- **(a) 断る。** 隣の 8箇所と同じ文で。`"still a preview - export after the full
  frame lands"`。
- **(b) preview であることを明示して出す。** PNG の tEXt チャンクと toast に `1/N preview` と書く。
  ROI/統計の export は provenance 行に `step N` を足す。
  **「測っていない絵を測ったことにしない」という点ではどちらでもよく、
  今の「黙って出す」だけが選べない。**

---

### G5. remote のファイル内フレーム軸が session 復元でフレーム 0 に戻る —— **修正済 (PR #177)**

**どこ。** `writeSessionTo` は `seqframe <seqIndex>` を書く
(`core/app/session.inc:192`)。復元側 (`:2451-2457`) は**同期的に**走るが、
remote の open (`openRemote`、`:2583` から frame 0 固定) は残りのフレームを
**非同期に**積む (`core/app/open_dispatch.inc:2241-2257`)。復元の瞬間
`framesOfSeq` には頭しか無いので `want` が一致せず、**誰も後から適用しない**
(`seqframe` は `session.inc` にしか現れない)。

**選択肢。** (a) remote の stack が揃ってから適用する遅延キュー (ローカルの
`app.seqRestore` と同じ形)。(b) remote では `seqframe` を書かないと決め、
「リンク越しの stack は頭から戻る」と文書に書く。

**採ったのは (a)、G10 と共通の 1つの park で。** 2件は同じ形の欠陥である ——
「session の行が、doc がまだなっていない状態について書かれている」。
`App::RestoreWait` に uid で park し、`pumpRestoreWaits()` が frame loop で
drain する。**諦めることも出来事にした**: fetch が失敗した / stack がその
フレームを含まないまま伸びるのをやめた場合は名指しで言う。黙って session より
少なくやることが、この2件の欠陥そのものだったので。

---

### G6. reader で開いた doc の session 復元が prefs.txt 頼み —— **半分修正 (PR #181)**

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

**(b) を実装した (PR #181)。** 「開いたが名指しされたメンバは無かった」枝は
コメントに「never worse than today」と書いてあったが、**worse である** ——
以降の range / LUT / モザイク読み / crop はすべて `cur()` に当たるので、
その行の設定が**別の document に適用される**。既に報告している隣の枝
(already-here dedupe) と揃えた。メンバ名が `__pixels_` で始まるときは理由も
明示する —— 「そのメンバが無い」だけだと、問題ではないファイルの中を探させる
ことになるので。

**(a) は実装していない。信頼の問いだから。** `docs/features/adapters/input-adapters.md §4.12`
の信頼モデルは「viewer は adapter を探しに行かないし、言われていない adapter を
適用しない —— ユーザが選んだ spec か、以前選んだ記録から」である。memo は
**このマシンのこのユーザ自身の選択**の記録だが、**session は旅をする文書**で
ある。session 行に spec を書くと「.vsession が、開いた時に走る Python を
名指しできる」ことになる。これは私の判断ではないので **#179 で裁定を仰ぐ**。

**裁定 C (2026-08-14) で (a) は「名指しはする、実行はしない」に決まり
(`readerhint`、PR #209、試験 V25p2)、その線は link を越えても 1 文字も動かない
—— #180 stage 4 (PR、2026-08-17、試験 `--rreader-selftest` V25p-r1/r2)。**
peer 側で reader が作った doc の復元も、走らせるのは **memo だけ**である:
memo が引ければ RUN を再依頼して開き直す。peer のキャッシュが答えるため、viewer
側では Python を起動せず、peer 側も reader / harness は再実行しない。ただし peer は
環境同一性を確認する provenance probe を 1 回起動する。memo が引けなければ hint を
名乗って失敗する —— **peer には要求すら出さない** (`bytesReceived()` 不変で観測)。
「どの reader か」は client の
memo/picker、「走ってよいか」は peer 起動者の `--serve-readers`、という
`docs/features/remote/remote-reader-design.md` §2.2 の2段階の許可判定のうち、client 側の段である。

---

### G7. `.mp4` が peer 一覧では「測った理由」を失う —— **修正済 (PR #176)**

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
いるが、**その逆向きの条件 (remote 行が自身の理由を表示する) は誰も検査していない。**

**選択肢。** (a) `peerRefusal` の頭で `videoRefusal` を先に返す (3行)。
(b) 「リンク越しの断りは link の限界だけを言う」と決めて記録する——ただしそれは
F3 の local 側の理屈と逆になる。

**採ったのは (a)。** 置いたのは頭ではなく **fall-through の直前**——表の行と
`.npz` は「ここでは読めるが link が運ばない」という link の話で、そちらが先に
答えるべきだから。動画コンテナは表の行ではないので順序は実際には競合しない。

**試験 F3b** が F3 の鏡像を据える: 同じ `capture.mp4` を `rbRowWhyNot("trc2", ...)`
で訊き、`codec` と `ffmpeg` を含むことを主張する。**F3 は local 側だけを見ていた**
——だから link 側でこの文が消えても誰も気付かなかった。

---

### G8. `.rggb` が File ▸ Open の「Images」フィルタに無い —— **修正済 (PR #175)**

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

**採ったのは (a) の一般形。** 数えたらリテラルは 2つではなく **4つ**あった
(`SEQ_EXTS` / `HEADERLESS_EXTS`(関数内) / ダイアログのフィルタ / `viewerReadsName`)
——さらに拒否文と RAW パネルの説明文が同じ5形式を日本語で列挙していて **6箇所**。
`.rggb` が落ちていたのは 1箇所だけなので、足すだけでは「次に足す拡張子が
どこかで落ちる」状態が残る。

配置は**入口ごとに2配列**、`core/app/sequence.inc` の先頭:

```cpp
static const char* SELFDESC_EXTS[]   = { ".npy", ".npz" };                        // 形を自分で述べる
static const char* HEADERLESS_EXTS[] = { ".bin", ".raw", ".yuv", ".dat", ".rggb" }; // 述べない
static std::string ownDoorPattern();      // "*.npy *.npz *.bin ..." —— dialogPattern() と同じ作り方
static std::string headerlessExtList();   // ".bin .raw ..." —— 散文用
static bool extIsHeaderless(const std::string&);
static bool isLoadableName(const std::string&);   // 拡張子ではなく名前で訊く版
```

これで 6箇所すべてが計算になる。`viewerReadsName` は `isLoadableName` を**呼ぶ**
(F4b の等式が構造的に真になる —— それが F4b の目的だった)。`.vsession` だけは
その2配列に**入れない**: Open Folder がセッションを stack に束ねてはならず、
ダイアログでも別エントリだから、「この名前で viewer は何かできるか」を訊く
1箇所 (`viewerReadsName`) にだけ足す。

**試験 F4c** (`core/selftest/fmtgate.inc`): 2配列と表の全行を歩き、**各拡張子が
`openImagesFilter()` に出ること**を主張する。この試験の中に拡張子のリストは無い
(明日どちらかに足した拡張子は誰も試験を編集せずに検査される)。トークン一致で
見る —— 部分一致だと短い拡張子が長い方の陰に隠れる。拒否文が同じ集合を列挙する
ことも同時に主張する。反空虚は「12個以上を検査し、フィルタが空でない」。

赤: `missing [.rggb]` / 緑: `missing [-]`。

---

### G9. generic な peer 拒否文に逃げ道が無い —— **修正済 (PR #176)**

**どこ。** `core/imagefile.cpp:425` — `return "the peer serves " + servedList();`
分岐の上 2つ (表の行 / `.npz`) は `WAY_OUT` を付けているのに、最後の return だけ
付いていない。`docs/features/adapters/input-adapters.md §3.2` の三部構成 (名指し・理由・逃げ道) の
三つ目が欠ける。G1・G7 はどちらもこの return に落ちてくる。

**選択肢。** (a) `WAY_OUT` を付ける。(b) `CHOOSE_A_READER` を付ける
(§4.13.1 で「adapter は peer 側で走る」と決めてあるので、逃げ道としては
そちらが本筋——ただし G11)。

**採ったのは (a)。ただし `WAY_OUT` そのものではない。** 上2つの `WAY_OUT` は
「browse it locally」と言っていて、それは**この分岐に落ちてくる名前について真とは
限らない**——ヘッダ無しの `.raw` はローカルなら開くが、正体不明の名前は開かない。
そして `core/imagefile.cpp` は **peer 側のバイナリにもリンクされる**ので、
`core/main.cpp` 自身の入口 (`SELFDESC_EXTS` / `HEADERLESS_EXTS`) を見て区別する
ことができない。だから逃げ道は**リンクの限界**を述べる:「この link はその形式
しか運ばない —— このマシンにコピーすれば、この viewer が読む残り全部で開ける」。
どちらの名前についても真で、しかも実際にファイルを動かす事実である。

(b) は G11 が決まってから。

---

### G10. 間引かれた remote doc の crop が session 復元で黙って落ちる —— **修正済 (PR #177)**

**どこ。** `session.inc:2653` は `cropInPlace(...)` の**戻り値を見ていない**。
`cropInPlace` は `remoteStep > 1` で false を返す (`loader_npy_raw.inc:374`、
理由は同所のコメント: 全解像度の読込み完了時に crop 状態を参照せず上書きするから)。
復元直後の remote doc はまさに間引かれた状態なので、**1600px を超える画像では
crop が黙って消える。**

**選択肢。** (a) 戻り値を見て、落ちたら失敗行として報告する。
(b) 全解像度の読込み完了後に適用する (G5 と同じ遅延キューに載る)。

**採ったのは (b)。** (a) は黙らなくなるだけで crop は失われたままで、しかも
**対話側は既に (b) の答えを返している** (`cropCurrentToSelectedRoi` の
「still fetching the full frame - crop after it lands」)。同じアプリの中で
同じ状況に 2つの答えがあってはならない。待つ条件は G5 と違って `rfPending`
ではなく **`remoteStep <= 1`**: crop が拒否される理由はそれ 1つで、
fetcher の暇を待つと混んだキューの中で「もう少しで着く doc」を諦めてしまう。

---

### G11. 「reader は peer 側で走る」という 2026-08-03 の決定が実装されていない —— **全段実装済み (reader 経路と peer 測定に対応)**

**どこ。** `docs/features/adapters/input-adapters.md §4.13.1` は明確に決めている——
「データが向こうにあるのに adapter を手元で走らせると生ファイルを転送してから
変換することになり、この道具の設計思想に反する。**したがって adapter は peer 側で
走る**」。`core/serve.cpp` に adapter/reader API は無い (あるのは
`MOP_PLUGIN_ANALYZE` = ABI v3 の解析プラグインで別物)。`openRemote` は
`peerServesName` が false なら reader を提案せずに断る (`:1748`)。

**これは「決定はあるが実装が無く、拒否文もそれを言わない」状態**である。
判断された拒否でも、開いた道でもない。

**選択肢。** (a) v1 の範囲を「ローカルで走らせる (ファイルが流れます)」に狭めると
決め直して記録する——§4.13.1 自身がその逃げ道を「明示的に選ばせる」と書いている
ので、**その半分だけ先に実装する**のが一番安い。
(b) §4.13.1 のとおり protocol に adapter API を追加する。

**裁定 (#180) は B。** 設計は `docs/features/remote/remote-reader-design.md` (2026-08-17, Fable)——
protocol 12・`MSG_READER_RUN`・起動引数 `--serve-readers`・reader は client から
運ぶ、を stage 0〜5 に分けた。

**stage 0 済 (PR #211)。** 二文のうち後ろの半分——「拒否文もそれを言わない」——だけ
先に消した。`imagefile::peerRefusal` の fall-through が、決定 (§4.13.1) を名指しし、
**reader 経路が未実装である**と示し、現時点で利用できる方法 (手元に copy して reader) を出す。
無い機構は仄めかさない (同設計 §7: peer 上に reader を名前で探す道は無い)。
試験は `fmtgate` **F4d2**——ヘッダレス拒否文を歩く F4d の隣。
**この文は stage 2 で `choose a reader` に差し替わる**(reader 経路が実装された日にこの検査が
落ちるように書いてある)。

**stage 1-2 済 (2026-08-17)。** 前の半分「実装が無い」も消えた。protocol 12 /
`MSG_READER_RUN` / 起動引数 `--serve-readers` (既定 閉) / reader は 3 テキストを
client から運ぶ / peer 一時 dir で実行し実行後に削除 / 鍵は peer 発行の 16 進 /
META・TILE の末尾 `[u32 flags][str key][u32 node]` で materialisation の画素だけが
戻る。単根の frame / stack がリンク越しに doc になり、**同じ reader を手元で
走らせた doc と画素ビット一致**する。F4d2 は予告どおり裏返した——拒否文は
`choose a reader` と `--serve-readers` を言い、「not built yet」を言わない。
試験は `--rreader-selftest` (`core/selftest/rreader.inc`, R0-R11、`--remote-exe` で
本物の `viewer-serve` を立てる)。

**stage 3 済 (2026-08-17)。** 木がリンクを渡る。`[u32 node]` は series の
stack を個別に指し、conditions・名前・単位がローカル実行と同値で立つ
(`--rreader-selftest` R12)。範囲外のノードは名指しで断る (R13)。zoom による精細化
も**この回で繋いだ**——`FrameSource::remoteKey/remoteNode` を `RFetchJob` まで
通し、tree の構築後に `remoteTreeRefine` が node ごとに `requestFullRemote` を
呼ぶ。

**同じ回に issue #217 (remote `.npz`) が乗った。** protocol **13** /
`MSG_NPZ_SCAN` / peer は列挙と member 単位の遅延 materialise だけ / 分類・picker・
木は client の既存 1 経路。`.npz` は `peerServesDeclared` に入り (行が生きる)、
`peerServes` には入らない (1クリックプレビューの可否判定は据え置き)。試験は
`--rnpz-selftest` (R14〜R17) と `fmtgate` **F4g** (R18)。

**stage 4 済 (2026-08-17)。** memo / session 復元の remote 版。memo (url 鍵) が
引ければ復元が peer 側で reader を走らせ直す——ただし RUN キャッシュが答えるので
viewer 側では Python を起動せず、peer 側も reader / harness は再実行しない。
peer は環境同一性を確認する provenance probe だけを 1 回起動する。memo が無ければ
`readerhint` を**名指すだけ**で、要求すら線に出ない (#179 裁定 C はリンクを越えても同じ)。試験は
`--rreader-selftest` V25p-r0〜r2d。

**stage 5 済 (2026-08-17)。** protocol **14**。MEASURE がファイルしか名指せなかった
最後の穴を塞ぐ——`MeasureReqHead::flags` の `MRF_KEYED` + rois の後ろの
`[str key][u32 node]` で、**reader の node も .npz の member も**「データのある側」で
集計される (`nPaths` は 0 = パスは一切送らない)。実装上の変更は
`FrameSource::init` が `openKeyed` を通るだけで、σ_t/σ_fpn の算術は 1 文字も
動いていない。σ_t は独立 f64 参照と一致し、同じ stack をローカルで開いて測った値と
**ビット一致**する。v13 peer へは送信前に断る (`rp::measureKeyedTooOldText`)——
v13 の実際の答えは "bad MEASURE header" で、それは請求が壊れているという文であって
peer が古いという文ではない。試験は `--rmeasure-selftest`
(`core/selftest/rmeasure.inc`, M1〜M5f、`--remote-exe` で本物の `viewer-serve`)。

**これで G11 は閉じる**——決定 (§4.13.1) は実装され、拒否文はどれも今日の事実を
言う。#180 のクローズ自体は親の判断。

---

## 8. 落穂ではないもの (=判断が記録されている拒否)

マトリクスを作った副産物として、**ちゃんと決めてある**ものを一覧にしておく。
次に同じ問いが出たときに探し直さないため。**記録場所は symbol と節 anchor で
指す** (§0 の規則)。

| 拒否 | 記録場所 |
|---|---|
| ベンダ RAW は peer に渡らない | `Backend::overLink=0` の列とその隣のコメント (`core/imagefile.h`) + `docs/features/adapters/input-adapters.md §3.6.4b` + issue #148 判断B。試験: `fmtgate` F3/P6 |
| ~~`.npz` は peer に渡らない~~ **渡る (#217, protocol 13)** | `MSG_NPZ_SCAN` + `docs/features/remote/remote-reader-design.md §10`。`docs/features/adapters/npz-design.md §2.3` が名指しした「zip の中身一覧を返す動詞」がこれ。残る拒否は「`.npz` 丸ごとを 1 配列として指した要求」だけで、文面は `core/imagefile.cpp` の `isNpz` 分岐。試験: `fmtgate` F4g / `--rnpz-selftest` |
| 動画コンテナは読まない | `imagefile::videoRefusal` + `docs/features/media/video-support.md §1` (実測: σ_t 40 DN16 → 0.00) + issue #54 |
| CFA TIFF は当てずに断る | `core/tiffread.cpp` + `core/imagefile.h` 冒頭の規則3「CFA IS READ OR ABSENT, NEVER GUESSED」 + `docs/guides/manual.md §2.3b` |
| container / reader メンバは per-member reload しない | `reloadUnavailable` と `reloadSource` (`core/app/open_dispatch.inc`)。試験: `fmtreg` F6/F8 |
| Browse の stack 系の動詞は peer の問いのまま | `docs/features/adapters/input-adapters.md §3.6.4` |
| Browse のローカル行にプレビューは付かない | 同上 (「1クリックは選択・ダブルクリックで開く」) |
| peer が配れない行も一覧から落とさない | 同上 (#111 の裁定「見せて理由を言う」)。試験: `fmtgate` F2/F3 |
| 直接の入口から入った doc は MEASURE を peer に投げない | `serverComputes` (`core/app/open_dispatch.inc`) |
| 単独フレームは Watch しない (手動 Reload) | `docs/features/watch/watch-design.md §9` + `watchTargetsNow` 冒頭の「何がここに無いか」コメント (`core/app/watch.inc`) |
| 自動 reload は peer の stack に対して走らない | `watchAutoRefusal` (`core/app/watch.inc`) + `docs/features/watch/watch-design.md §16.6` |
| `.raw` を LibRaw に渡さない | `core/imagefile.cpp` の vendor RAW 行のコメント (「".raw" is deliberately NOT among these extensions」) + `docs/features/adapters/input-adapters.md §3.6.5` |

---

## 9. `[P]` — 手で確かめたもの

実施記録なので [results/20260811-matrix.md](results/20260811-matrix.md) §9 へ移した。

---

## 10. この表の保ち方

**この文書は手作業だけで保守するものではない。** 形式の可否判定の列
(`forPath` / `viewerReadsName` / `peerServes` —
[results/20260811-matrix.md](results/20260811-matrix.md) §9 が実測したもの) は
`selftest.fmtgate` の F4 が
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
