# 未着手・中断中の課題 (2026-07-29 時点)

実機で報告された不具合と、クレジット切れで中断した調査の引き継ぎ。
各項目に「どこまで分かっているか」を書いてあるので、そこから再開する。

Fable エージェント2本が月間上限で死亡。**やり直しではなく、途中から**再開できるよう
に、両方の「死ぬ直前に掴んでいた手がかり」をここに残す。両方とも worktree は作成済み。

---

## 1. Preview バグ2件 — worktree `c:/Users/hish/Desktop/viewer-preview-wt` (branch `preview-fixes`)

ユーザー報告 (逐語):
- 「Previewで連続フレームが見れない．Group解除ができないから画像が見れないことになってしまう．」
- 「Previewで連続フレーム(Stack)をダブルクリックすると，一枚目とStackが2つFilesに登録される．」

### 死ぬ直前の最後の一言 (これが再開の起点)
> Toasts draw into the foreground list — not the focus thief.
> The suspect is the **server-temporal result focusing the Temporal panel**. Let me check.

つまり Fable は「Preview で画像が見れない」の原因を **フォーカス泥棒** の線で追っていた:
Preview を出した直後に何かが別パネル(Temporal)にフォーカスを奪い、Image View が
Preview を映さない/映しても即座に取り替えられる、という筋。トースト描画は容疑から外れた。
**次に見るべきは server-temporal の結果到着ハンドラが Temporal パネルへ
SetWindowFocus/FocusWindow している箇所。**

### 期待仕様 (設計はこれで確定済み)
- グループ化された stack 行の Preview は **ungroup 不要で画素が出る**。ポスターフレーム +
  既存の preview スクラブ(`stepPreviewFrame`, main.cpp ~6141、UI は ~11931-11953)で送れる。
  永続登録は一切しない。
- ダブルクリック = その stack が **ちょうど1回** 開く。ポスター/preview は兄弟として残らず
  drop される。Files に残るのは stack エントリのみ。
- preview スロットは1個のまま。measure したら promote (~10537) も維持。

### コード上のアンカー (main.cpp)
`openRemote(url, asPreview)` 6156 / `dropPreview` 6117 / `promotePreview` 6317 /
`app.previewUid` / `batchReuse("preview")` 6198 / preview 疑似バッチの描画 12129 /
ダブルクリック経路 11270 (`dropPreview(); // the poster frame did its job`) と 11277。
Browse パネルは `drawPanelRemote` 11010、行の遅延操作は `rbDefer`/`RbDeferredActions`。

---

## 2. X11 で窓が動かせない・最大化できない — worktree `c:/Users/hish/Desktop/viewer-x11-wt` (branch `x11-frame`)

ユーザー報告: 「LinuxでX WindowsでLocal 実行すると，Windowを動かせない．」「最大化も効かない．」

### 死ぬ直前の最後の一言 (実機で計測済みの貴重な結果)
> **Metacity: drag1 works, drag2 dead, maximize works.**
> The **eaten-release pattern is confirmed across WMs**. Try xfwm4 and a software-GL mutter retry.

これは大きい。**Linux 実機で実際に動かして** 取った観測:
- 1回目のドラッグは動く、**2回目以降が死ぬ** → 「押しっぱなし」状態がアプリ側に残る。
- 原因は **release イベントが食われる (eaten release)**。`_NET_WM_MOVERESIZE` を投げると
  以後のポインタは WM がグラブするので、**ButtonRelease がアプリに来ない**。アプリ側の
  「ドラッグ中」フラグ(または ImGui の MouseDown)が立ちっぱなしになり、次のドラッグ開始
  条件(押下エッジ)が二度と成立しない。
- Metacity では maximize は効いた → `glfwMaximizeWindow` が全 WM でダメなのではなく
  **WM 依存**。`_NET_WM_STATE` ClientMessage 版のフォールバックは依然として要る。
- 次にやる予定だった検証: **xfwm4** と、**software-GL での mutter 再試行**。

### 修正の方向 (上の観測から確定的に言えること)
`_NET_WM_MOVERESIZE` を送った直後に、**アプリ側のドラッグ状態を自分で降ろす**
(ImGui へ MouseUp を注入する / window_frame 側の startZone をクリアする)。
WM にグラブを渡した時点でそのドラッグはアプリのものではなくなるので、
「release を待つ」設計自体が誤り。

### 不変条件 (これは死守)
Integrated が move+resize+maximize を提供できない環境では、**必ず装飾付き System へ
大声でフォールバック** (`unavailableReason()` + Messages パネルに1行)。
動かせない窓を絶対に作らない。

### 触ってはいけない場所
`window_frame.cpp` の Win32 ブロック — user-bugs (`ec33428`) が
WM_NCPAINT/UAH/WM_NCACTIVATE を潰す修正を入れている。統合後の main を基点にすること。

---

## 3. A と B のサイズが違うと画面全体 Fit になる (ユーザー報告 2026-07-29 夜)

ユーザー報告 (逐語): 「A, Bが異なるサイズの時に画面全体Fitになってしまう．」

### 構造 (調査済み — ここから始めれば早い)

**ビューは1つしかない。** `app.view` (zoom + center) を A と B が共有していて、
`mapIn`/`unmapIn` (main.cpp ~7563) はどちらのペインでも同じ `app.view` を使う。
これは意図的で、コメントもそう言っている:

    // "fit" must fit the pane the image is drawn in, not the whole canvas. A is
    // the reference, so fit A's pane; B is the same view by construction.
    ImVec2 fitSize = split ? ImVec2(paneAsz.x, canvasSize.y) : canvasSize;
    if (im && app.fitRequested) { fitToCanvas(fitSize); app.fitRequested = false; }

`fitToCanvas` (~7480) は `cur()` = **A の寸法だけ**で zoom を決める:

    app.view.zoom = std::min(canvasSize.x / im->w, canvasSize.y / im->h) * 0.97f;
    app.view.center = ImVec2(im->w * 0.5f, im->h * 0.5f);

`fitRequested` を立てる場所: 2140 / 2252 (画像を閉じたとき) / 2700
(`images.size()==1 || fitOnSwitch`) / 3352 / 3400 / 7035 / 13587 / 13643
(stack 切替時の `fitOnSwitch`)。**B を設定した経路がどれかを踏んでいないか**を
まず確認すること。

### ここは設計判断が要る (だから Fable 案件)

サイズが違う2枚に「同じビュー」を課すと、fit は本質的に定義できない
(A に合わせれば B がはみ出す/縮む)。取れる道は3つ:

1. **共有ビューのまま、大きいほうに fit** — 両方必ず画面内。ただし A が小さいと
   A が小さく表示される。
2. **共有ビューのまま A に fit** (現状の建前) — B のはみ出しを許容し、はみ出して
   いることを画面で言う。
3. **ペインごとに独立 fit** — 見た目は一番きれいだが、**画質評価では罠**。
   異なる倍率で並べた2枚の解像感・ノイズ感を比べると誤った結論に直行する。
   やるなら「倍率が違う」ことを両ペインに常時明示しないと危険。

**推奨は 1 か 2。** 3 を選ぶ場合はラベル必須。どれを選ぶにせよ、
**倍率が共有されているのか否かは画面から常に読めること** が要件。
(既に `b->w != im->w` のときは Compare 設定に "size differs: B is %dx%d" と
橙で出している — main.cpp ~8805。表示の置き場所としてはこの近くが自然。)

### 回帰テスト

`--abstats-selftest` に、サイズの違う2枚を A/B にして
「fit 後に A も B も可視で、zoom が共有されている(または共有されていないと
明示されている)」を assert する check を足す。fixture はサイズ違いが要るので
`tools/gen_testdata.py` に1つ足すことになるはず。

---

## 4. 非階層(常に遷移)モードには `..` 行を戻す (ユーザー報告 2026-07-29 夜)

ユーザー報告 (逐語): 「階層ビューワの再は..は不要だけど，今の非階層(常に遷移)だと，
..は欲しくなるね．」
(= tree 表示では `..` は要らないが、フォルダに入って遷移していく今の平坦表示では要る)

### なぜ消えたか (コードに理由が書いてある — main.cpp ~12877)

    // The "[..]" row that used to sit here is gone: the toolbar's "up" button
    // and Backspace both do it, and a row that exists only outside the home
    // directory shifted every listing row by one line on the way in and out.

**この理由は tree モードでしか成り立たない。** tree なら親は画面に見えているので
`..` は重複だが、平坦表示 (`app.rbTree == false`、~698) では現在地しか出ておらず、
上に戻る手段がツールバーの `up` ボタン (~12487 `rbGoParent()`) と Backspace だけになる。
どちらもリスト**の外**にあるので、行を追っているカーソル/マウスの流れから外れる。

### 要件

- `app.rbTree == false` のときだけ `..` 行を先頭に出す (tree のときは出さない —
  ユーザーが明示的にそう言っている)。
- 消した理由だった「行が1つずれる」問題は残る。**ホームより上でも下でも常に
  1行目にある**ようにすれば、入る/出るで行位置がずれない (かつての実装は
  「ホーム外でだけ出る」から動いていた)。ルート直下で押せない場合も、
  行は出したまま無効表示にすればずれない。
- `rbCursor` (~12887) のキーボード操作、`rbSel`、クリッパ (`IncludeItemByIndex` は
  **必ず `clipper.Begin()` の後** — 前に呼ぶと null TempData 書き込みで即 SIGSEGV)
  のインデックス計算が1行ぶんずれるので、そこを全部見ること。
- 既存の `up` ボタンと Backspace はそのまま残す (増やすのであって置き換えではない)。

### 回帰テスト

`--browse-keys-selftest` は既に実入力で行を叩いているので、そこに
「平坦表示では 1 行目が `..` で、入って出ても行番号がずれない」
「tree 表示では `..` 行が無い」を足す。

---

## 5. `sequence` と `stack` が混在している — 用語を定義しなおす

ユーザー指摘 (2026-07-29): 「sequencesとstackという言葉が混在している．terminologyを
改めて定義しなおしましょう．」

正典 (`docs/terminology.md`) は **stack** を層の名前として定義している
(`frame ⊂ stack ⊂ series ⊂ batch`)。にもかかわらず UI は今も `sequence` と言う。
**同じ物を2つの名前で呼んでいる**のが今の状態。

### 実測した混在 (UI 文字列)

    "Select sequences"            "Load sequence"        "Load sequence?"
    "Sequence loading"            "which sequences do you want?"
    "%d sequence(s), %d files"    "selected: %d sequence(s), %d files"
    "Open folder (all sequences below it)"   "Open the whole sequence (%u frames)"
    "each checked sequence becomes its own stack"      ← 1文に両方
    "folder: loads every numbered sequence below it, one stack per group"  ← 同上
    "step the previewed sequence"  "loading sequence: "  "frame number (index in sequence)"
    "needs a stack: load a numbered sequence first"    ← 同上

最後の3つが症状を一番よく表している: **1つの文の中で両方使っている。**

### まず決めるべきこと (これが設計判断で、だから Fable)

`sequence` は消すべき同義語なのか、それとも**別の物を指しているのか**。
後者に見える根拠がある:

- **ディスク上の連番ファイル群** (`frame_000.npy … frame_023.npy`) と、
- **読み込まれて時間軸を持つ計測対象** (`SeqInfo`、σ_t が意味を持つ単位)

は同じものではない。前者はまだ何も開いていない状態でも存在する
(ブラウザのグループ行、picker の候補)。正典が名前を与えているのは後者 = **stack**。

**推奨**: ディスク側は「連番ファイル (numbered files)」と呼び、`sequence` という
語を UI から**引退**させる。読み込まれた物は常に **stack**。
"each checked sequence becomes its own stack" は
"each checked group of numbered files becomes its own stack" になる。
ただし最終判断はユーザーに確認すること — 日本語の画面表記も併せて決める。

### 触ってよい範囲と、触ってはいけない範囲

**互換性のある表面** — 変えると既存のセッション/スクリプトが壊れる。改名しない
(するなら読み込み側で旧キーも受ける移行が要る):

- セッションのキー: `seqload` `seqname` `seqframe` `seqlevel`
- CLI: `--sequence ask|always|never`
- selftest 名 (`--lin-selftest` 等の内部名は自由だが、既存スクリプトが叩いている)

**自由に変えてよい**: UI 文字列、`docs/*.md`、トースト、ログ。

**コード識別子** (`SeqInfo` `seqId` `seqQueue` `seqLoadingId` … 約40個) は
機械的な一括改名になるので**別コミット**にすること。混ぜると意味のある変更が
レビューで埋もれる。急ぎではない — 画面の言葉が先。

### 成果物

1. `docs/terminology.md` に「`sequence` は使わない。ディスク上の連番ファイル群は
   〜、読み込まれた物は stack」と**明記**する (今は stack の定義はあるが、
   `sequence` を禁じる記述がない。だから混ざった)。
2. UI 文字列の置換。
3. 回帰: 文字列を assert しているテスト (`--browse-selftest` の row 文言など) の追随。

---

## 6. Search 結果に複数選択が無い — Search の価値がここで頭打ちになっている

ユーザー指摘 (2026-07-29): 「File BrowseでShift選択とかCtrl選択ができて，それを
Stackとして読むもしくはフレームとしてそれぞれ読むができると，Searchの意義が
ぐっと高まる．」

### 確認した事実

**通常のリストには既にある。** `rbSel` (Ctrl/Shift クリック) と、選択2件以上で
出るアクション行 `"Open %d selected as stack"` (main.cpp ~12771) と
`"Temporal stats (server)"` (~12804)。

**Search 結果には無い。** 検索結果ビュー (`app.rbSearch.active`、~12858) は
素の `ImGui::Selectable` を1行ずつ描いているだけで、`rbSel` に一切参加していない
(~12882)。だから「散らばった場所から条件に合うファイルを集める」という
**Search が本来一番得意なこと**が、集めた後に何もできずに終わっている。

### やること

1. 検索結果ビューを `rbSel` に載せる。行の実体が `remote::GlobHit` で
   `remote::Entry` ではないので、`RbRow` に載せるか、選択を**パス文字列**で
   持つ形に一般化するかの判断が要る (後者のほうが、リストと検索で選択が
   生き残るので筋が良い可能性がある)。
2. アクションを2つにする。ユーザーの言葉どおり:
   - **Stack として読む** (既存の "Open selected as stack" と同じ) — 選んだ
     ファイル群が1つの時間軸になる。
   - **それぞれ frame として読む** — N 個の独立したフレームとして開く。
     現状これは**リストにも無い**ので、両方のビューに足すことになる。
3. 順序の定義: Stack として読むなら並び順が時間軸そのものになる。
   検索結果はパス順なので、`rp::naturalLess` で並べ直すのか、結果の並びを
   そのまま使うのかを決めて**画面で言う**こと (σ_t が並び順に依存する)。

---

## 7. Search 中に「まだ探している」のか「見つからなかった」のか分からない

ユーザー指摘 (2026-07-29): 「File BrowseでSearch中に，見つかってないのか探している
途中かがわかりにくい．」

現状 (main.cpp ~12862): 実行中は `"searching <pattern> under <root> ..."` の
1行だけ。**途中経過が無い** — 何件見つかったか、どこを見ているか、いつ終わるか。
終わると `"%d result(s)"` に切り替わる。0 件で終わったのか、まだ走っているのかは、
この2つの文字列を注意して読み分けないと分からない。

やること: 実行中も **今の件数**と**今どこを見ているか**を出す。プロトコルは GLOB
(`rp::` の `MSG_GLOB`)。今は完了時に一括で返っているはずなので、**途中結果を
流す**なら peer 側も変える必要がある — そこが設計判断。最低限、ヒット数の
インクリメンタル表示と「探索中のフォルダ」だけでも、体感はまったく違う。
`skippedDirs` (読めなかったフォルダ数) も実行中から出してよい。

---

## 8. zoom % は Image View の中にあるべき

ユーザー指摘 (2026-07-29): 「zoom ?%の表示はImage Viewに会った方が良い気が．
デザイン的なむずかしさがあるのでこれもFable TODOでも．」

現状 zoom は**ステータスバー**に出ている (main.cpp ~20418)。画面の一番下、
Image View から目を離した先。拡大率は**今見ている画素の性質**なので、
画素の隣にあるべき、という指摘。

デザイン上の難しさ (ユーザー自身が挙げている):

- Image View には既に下辺に2行のフッター (バッチ / ファイル名 + スクラブバー +
  フレーム番号) があり、上辺には何も置きたくない (画素を隠す)。
- A/B 分割時はペインが2つあるが、ビューは共有なので zoom は1つ。どちらのペインに
  出すのか、あるいは中央か。
- ステータスバーから**消す**のか、両方に出すのか。両方は重複だが、
  ステータスバーは「アプリ全体の状態」の場所でもある。

判断してから実装すること。ついでに: 現状の表示は `%.0f%%` と `%.3g%%` の
出し分け (10% 未満で桁を増やす) なので、置き場所が変わっても**単位 (%) は必ず
付ける**こと (グラフ軸と同じ規則)。

---

## 9. Histogram の bin は整数 / 2のべき乗で切る

ユーザー指摘 (2026-07-29): 「HistgramのBinだけど基本的には整数もしくは2のべき乗
Floatでbin切りたいなぁ．」

### なぜ効くか

今は **256 bin を [black, white] に等分**しているだけ (main.cpp ~8270,
`inv = 256.0f / (H.white - H.black)`)。レンジが 256 の整数倍でないと、
bin 幅が半端になる (例: 64..1023 なら 3.746 DN/bin)。整数センサ値をその幅で
刻むと、**ある bin には 4 個の DN 値が、隣の bin には 3 個が入る**。
結果として実在しない周期構造 = 櫛状のアーティファクトが出る。
ノイズ分布の形を読む用途では致命的で、ユーザーの指摘はここを突いている。

### 実装の勘所 (調べた結果)

**snap する場所は1箇所でよい**: `recomputeHistogramIfNeeded` の頭で
`wantBlack`/`wantWhite` を求めた直後 (~8241)。ここで丸めれば、キャッシュキー・
ビニング・x軸ラベル・A/B の bin 共有が**全部同じ値を見る**ので追随する。
**キャッシュ比較 (`H.black == wantBlack`, ~8254) より前**に丸めること —
後で丸めると毎フレーム キャッシュミスして再スキャンする。

規則:
- 整数 dtype (u8/u16/i16/u32...): `w = ceil((white-black)/256)` を整数に切り上げ、
  最低 1。origin は `floor(black/w)*w`。u12 の 0..4095 なら w=16 でちょうど、
  0..255 なら w=1 でちょうど 1 DN/bin になる。
- float: `w = 2^ceil(log2((white-black)/256))`。origin は `floor(black/w)*w`。
  bin 端が2進で厳密に表現でき、再ビニングで誤差が積まれない。
- 丸めたレンジは元のレンジを**必ず含む**(外側に広げる)。

### 見落としやすい罠 (これを踏むと測定値が変わる)

**クリップ数を bin レンジで数えてはいけない。** 今は `t < 0` / `t >= 256`
(= bin の外) で `below`/`above` を数えている (~8283)。bin レンジを外側に広げると
**飽和画素の数が減って見える**。`clipLo`/`clipHi` は「表示レンジ = ユーザーが
見ている白飛び/黒潰れ」の話なので、**元の black/white と比較して数える**ように
分離すること。bin の外か否かとは別の量。

x 軸ラベルにも、丸めた実効レンジと bin 幅 (DN/bin) を出すこと
(グラフは常に量と単位を言う、という既存の規則)。

### 検証

`--abstats-selftest` か新しい単体テストで:
- u16 の 0..4095 → bin 幅がちょうど 16 DN、櫛が出ない (各 bin の DN 個数が均一)
- 0..255 → 1 DN/bin
- float の任意レンジ → bin 幅が 2 のべき乗
- **飽和画素の数が、bin レンジを広げても変わらない**(上の罠)

---

## 10. セッション復元で remote (ssh) が戻らない

ユーザー指摘 (2026-07-29): 「restore sessionで，remote sshが復元されない．」

### 原因は確認済み — セッションは remote browse の状態を1行も書いていない

`writeSessionTo` (main.cpp ~3421-3658) に `rb*` / `remote*` の書き出しは
**0 行**。ホスト・ポート・現在のディレクトリはどこにも保存されていない。

保存されているのは **prefs のほう** (`savePrefs`, ~3731):
`rbflat` / `rbadv` / `rbtree` (表示の形)、`remoteexe`、
`remoteurl` (= `app.lastRemoteUrl`、ただし **Start Remote ダイアログの
入力欄の初期値としてしか使われていない** — ~14522)、`rbookmark` / `rbrecent`。

つまり「次に開くときダイアログに前回のホストが入っている」までは実装済みで、
**セッションを開いたら勝手に繋ぎ直す**は未実装。画像のパスは
`ssh://host/path` 形式で保存されているので、**フレームは復元されるのに
ブラウザだけ空**という今の状態になる。

### 設計判断 (だから Fable)

1. **勝手に繋ぐのか、聞くのか。** ssh 接続は数秒かかり、鍵のパスフレーズや
   VPN 未接続で失敗しうる。セッションを開くたびに固まるのは避けたい。
   案: 復元はするが**遅延接続** — Browse パネルに「trc2:/data (click to
   reconnect)」を出しておき、押した時に繋ぐ。`docs/browse-as-file-manager.md`
   の「場所 (place)」設計と素直に噛み合う (復元された場所が1つ増えるだけ)。
2. **何を保存するか。** host / port / dir。`placeUrl()` が既に
   「このホストのこのディレクトリ」の正準 url を作るので、**1行 `rbplace <url>`
   で足りる** (ポートも含む — `makeRemoteUrl` はポートを落とさない)。
3. **失敗の見せ方。** 復元時に繋がらないのは平常運転。エラーはその場所の
   行にインラインで出す (グローバルなエラー枠ではなく)。これも
   browse-as-file-manager.md と同じ方針。

### 注意

- セッション形式は**キーを増やすだけ**なら後方互換 (未知キーは読み飛ばす)。
  古いビューアで開いても壊れない。
- 画像側の復元と**順序を競合させない**こと。`seqRestore` の隊列と
  `rbOpenQueue` は別物で、ブラウザの復元はどちらにも入れてはいけない
  (ブラウザは「見ている場所」であって、開いているデータではない)。

---

## 11. 環境の重要事実 (両エージェントが独立に確認)

**この Windows 機は現在 OpenGL のクライアント領域を一切キャプチャできない。**
`CopyFromScreen` / `BitBlt+CAPTUREBLT` / `BitBlt(GetWindowDC)` / `PrintWindow` の
すべてが**真っ白**を返す。一方で同じ画像内のタイトルバー(GDI)は正しく写り、デスクトップ
全体のキャプチャも正常(VS Code は読める)。GL 自体は動いている
(`glReadPixels` はマゼンタを返す、`--bench` は 0.32ms/frame)。
→ **スクリーンショットによる検証は今この機械では不可能。** 白い画像を「モニタが寝てる」
と誤診して時間を溶かさないこと。代わりに使えた観測手段:
`GetWindowText` によるウィンドウタイトル、stderr、Win32 メッセージトレース、
そして「視覚的事実を selftest の測定可能な assertion に変換する」。

入力注入の罠2つ:
- `keybd_event` に scancode 0 を渡すと `GLFW_KEY_UNKNOWN` になり矢印キーが無反応。
  `MapVirtualKey` + `KEYEVENTF_EXTENDEDKEY` が要る。
- タイトルバーを続けて2回クリックするとダブルクリック扱いで最大化し、全座標がずれる。
