# File Browse 100点化と持ち出し準備 — 設計 (#47)

対象: `core/browse/`（nav.cpp / panel.cpp / instances.inc / browse.h / host.h /
browse_state.h）と、viewer 側との統合境界（core/main.cpp の `g_browseHost` 表、
core/ui/menus.inc の述語群）。
状態: 提案。**本書だけではコードを変更しない。実装は §5 の段階ごとに、
個別の作業として行う。** 前提文書:
[browse-inventory.md](../../background/browse/browse-inventory.md)（裁定対象の全体）、
[browse-topbar-design.md](browse-topbar-design.md) §10（実装方針の記録）、
[split-plan.md](../../background/project/split-plan.md) §3 / §9（持ち出し単位と判断1）、
[input-adapters.md](../adapters/input-adapters.md) §3.6.4（述語の分離）、
[remote-headerless-design.md](../remote/remote-headerless-design.md) §5（統合境界を変更した直近の例）。

旧稿の比喩は、次の具体的な意味で読む。**継ぎ目**は統合境界、**戸**は操作の入口、
**召喚**は必要時だけ UI を表示すること、**降格**は常設 UI からメニューなどへ移すことを
指す。現行の説明では具体的な語を先に用い、履歴上必要な箇所だけ旧語を残す。

本書で導入する用語は2つである。**リンク時契約**は、統合境界を「ホストが定義する
関数宣言の集合」として表し、「表 + 名前による呼び出し」という二重構造を1つにする。
**linktest** は、スタブホストだけで browse をリンクし、持ち出し可能な境界であることを
ビルドの合否として検証するテスト対象を指す。

---

## 1. 残り10点 — 数え方と勘定

**数え方を先に書く。** 分母は設けない。「90点」はユーザーの体感を表すため、
100項目の表を新設すると、根拠のない定量評価になる。ここでは残件だけを数える。

- (a) browse-inventory.md の裁定(A表35行 + C表 + D表)と topbar-design §10 の
  決定を、panel.cpp / nav.cpp の現状と**1行ずつ突き合わせ**、
  「裁定済み・未実装」を各1点。
- (b) 2026-08-04 以降の**実地報告**を各1点。
- (c) 実装が裁定から**逸れている**(どちらが正しいか再裁定が要る)ものを各1点。
- 実装済みは点にしない(エラー帯→status line、grp|flat/list|tree の降格+チップ、
  filter/search の1箱化、ホスト→タイトル、選択動詞→右クリック、`<` `>` `up`
  `~` `refresh` の撤去、Watch、自動 peer 更新 — ここまでが「90点」の中身)。

数えた結果はちょうど 10 点になった。

| # | 何 | 出所 | 現状(コード) | 種別 | 行き先 |
|---|---|---|---|---|---|
| 1 | **`…` メニューが見つからない。** 撤去した5常設ボタン(`<` `>` `up` `~` `refresh`)と grp\|flat・list\|tree の行き先が、無ラベルの `…` **1箇所**。tooltip はあるが、tooltip は探しに来た人にしか出ない | 実地 2026-08-04(「切り替えができない」) | panel.cpp `"...##rbmenu"` | (b) | 段1 |
| 2 | **複数選択→実行の動線に受け皿が無い。** 動詞は行右クリック(選択行の上でのみ選択に作用)と Enter に生きているが、「選択した、次はどうする」を画面が言わない。右クリックだけでは「複数選択してから実行する」動線が読めない | 棚卸し #26/#27 + D4(選択バー裁定) | panel.cpp: アクション行撤去済み、バー未実装 | (a) | 段2 |
| 3 | **空状態が「接続」語彙のまま。** `Browse Local Folder...` / `Start Remote (ssh)...` の2ボタン + places コンボ。Cyberduck 型「場所の一覧」裁定は未着手 | 棚卸し #32-35 + D5 | panel.cpp `!B.connected` 分岐 | (a) | 段3 |
| 4 | **Home の行き先が無い。** #11 は「places の1項目に統合」で `~` ボタンを消したが、places に Home 項目が**無い**。今 home へ戻る手段はパス編集で `~` と打つことだけ | 棚卸し #11 | panel.cpp `drawRemotePlacesItems` | (a) | 段3 |
| 5 | **places に hosts が無い。** #1 の裁定は「bookmarks / recent / hosts を1つの一覧に」。hosts 節が欠け、ホストへの跳躍は空状態の ssh ダイアログ経由だけ | 棚卸し #1 | 同上 | (a) | 段3 |
| 6 | **Ctrl+L が無い。** パス編集の入口は不可視クリック域と右クリック `Edit path...` のみ。点1と同族(入口が不可視) | 棚卸し #3 | panel.cpp パス編集 | (a) | 段1 |
| 7 | **`+` の身の上。** 裁定は降格(`…` へ)、実装は常設のまま。ただし実装は裁定の後に別の(正しい)理を得た: タブの隣の `+` は「ここにもう1枚」という OS 共通の約束で、メニューの中の言葉より強い(rbPlusButton の注記) | 棚卸し #5 ↔ 実装 | panel.cpp `rbPlusButton` | (c) | **本書で裁定改定: 残す**(作業なし)。再訪条件: クロームが重いという指摘が再び `+` を名指しした日 |
| 8 | **クラムが折り返す。** 裁定は「1行・中央省略(ホストと葉は必ず残す)」、実装は折返し — 狭い×深いで**行数が呼吸し**、下の全行が動く。出没帯を消した設計の自己違反。status line は同じ問題を `rbElideMiddle` で既に解いている | 棚卸し #2 ↔ 実装 | panel.cpp クラム描画 `NewLine()` | (c) | 段1 |
| 9 | **`(listing...)` がテキスト出没。** 裁定は右端固定の小スピナー。実装は編集域が縮んでテキストが出る形(行は動かないが、パス行に第2の可読テキストが出る) | 棚卸し #4 ↔ 実装 | panel.cpp `(listing...)` | (c) | **v1 では断る**。行が動かない実装になっており実害の報告が無い。再訪条件: パス行が読みにくいという報告 |
| 10 | **Watch の状態を表示しない。** #12 の裁定は `…` に「最終確認 hh:mm」を併記するが、現状は表示がない。ただし watch-design §5 は「一覧は黙って更新してよい」を原則に置き、裁定同士が衝突している | 棚卸し #12/D6 ↔ watch-design §5 | 表示なし | (a) | **v1 では追加しない**（沈黙の原則を採用）。再訪条件: 「更新されているか分からない」という報告 |

**点数の外(構造)**: §10.8 のメニューのテーブル化と command palette。点1の
恒久解の半分だが、Browse 単独の課題ではない(メニューバー全体)。
残課題【Fable】として板に置く。段1はテーブル化を**待たない**（入口を増やすだけで
テーブルと矛盾しない)。

---

## 2. 切り出しの境界 — host.h で足りるか

### 2.1 今の継ぎ目は1本ではなく3層ある

| 層 | 中身 | 本数 | 検査 |
|---|---|---|---|
| 1 | `BrowseHost` 表(host.h 上半分): viewer の**動詞** | 関数ポインタ 15 | 型のみ。**位置初期化**(§3.1 の事故面) |
| 2 | 名前呼びの自由関数(host.h 下半分): path/名前 8、述語 6、remote 基盤 4(deployPeer / peerLabel / g_bootstrapLog / REMOTE_HOME)、文言・色 3 | ~21 | リンカが検査する |
| 3 | **`extern App app` の直接読み書き** | 32 フィールド、~120 箇所(grep 実測: rbBookmarks 23、browsePanels 22、prefsDirty 10、images 9 …) | **無し。宣言も表も無い** |

これに逆向き(viewer→browse)の browse.h ~45 シンボルが加わるが、これは
パッケージの**公開ヘッダ**であって漏れではない(NOGL selftest が描画物と同じ
関数を運転するための公開を含む — 削らない)。

**答え: 足りない。** host.h は第1・第2層しか語らず、第3層がある限り browse は
viewer 以外にリンクできない。「切り出せる形」の作業の実体は第3層の解体である
(§5 段4)。

### 2.2 viewer 固有として残るもの = ホスト契約の中身

- **形式の述語**(viewerReadsName ほか6関数): 残る、ホスト提供。述語は形式名を
  持たない(§3.6.4)ので契約の形は今のまま。
- **画像モデルと preview slot**: 残る。ただし今は panel.cpp が `app.images` /
  `app.previewUid` を**直接走査**している(rbActivateRow / rbOpenRow の
  preview 一致判定)。これを url 語りの動詞に置換する:
  `previewShow(url, seq...)` / `showExisting(url|path)->bool` /
  `promotePreview(url)->bool` / `previewFrameCount()`。`ImageDoc*` と
  `App::PendingGroup` が継ぎ目から消え、**継ぎ目はファイルリストと名前だけを
  運ぶ**(browse.h #81 の既存原則を継ぎ目全体の家訓に昇格)。
  `PendingGroup` は browse 側 POD(`browse::PickGroup`)になり App が alias を持つ
  (P7 の alias 方式そのまま)。
- **toast / prefs 永続化 / ダイアログ / picker / session 復元 / メニュー入口**:
  残る。全て既に動詞であり形は正しい。
- **browse 帰属なのに App に居るもの**は browse 側へ移す(下表)。

32 フィールドの帰属(段4の作業一覧の骨格):

| 帰属 | フィールド | 行き先 |
|---|---|---|
| browse 所有 | browsePanels, rbBookmarks, rbRecents, rbFlat, rbTree, rbNatural, rbOpenQueue(型 `RemoteOpen` ごと), browseWatchIntervalSec, browseWatchRemoteIntervalSec, showRemote | `browse::Shared`(browse_state.h、実体 `g_browseShared` は browse TU)。viewer 側の prefs/session/menus の読み書き(~57 行 / 7 ファイル、実測)は機械置換 |
| host 動詞化 | images, previewUid, current, previewFiles/Host/Port/Index/Label/Frames(preview 一式)、uiSession(stop)、showRemoteError → `showErrorDetails()`、remoteDlgOpen → `openConnectDialog()`、rfPending+seqRunning+loadBatchId → `openPipelineIdle()` 述語 + openRemoteStack に batchId 引数 | `browse::host`(§3) |
| host が書く設定値 | exePath, remoteExe, uiFrame(毎フレーム), watchEnabled+watchPaused → `watchOn()` 述語でも可(viewer のスイッチは viewer に残す) | `browse::Shared` の config 節 / host 述語 |

`prefsDirty` は `savePrefs()` 動詞に**畳む**(呼び側は常に対で書いている。
「prefs が変わった、あなたの方針で永続化せよ」が動詞の意味になる)。

### 2.3 別リポジトリになったとき何をパッケージするか

- **形態: ソース。** ヘッダ+実装の組を add_subdirectory / FetchContent で
  取り込む形(この repo が ImGui を取り込むのと同じ形)。
  静的ライブラリ配布は**しない**: C++ の ABI・コンパイラ・ImGui の版がホストと
  結合するため、安定したバイナリ境界を提供できない。ヘッダオンリーにも**しない**。worker
  スレッドと 2 TU が実体で、include されるたびに増える種類のコードではない。
- **単位**(split-plan §3 を踏襲、#148 で1点補正):
  `core/browse/` + `remote.h/.cpp` + `remote_proto.h` + `serve.cpp` /
  `serve_main.cpp`。補正: #148 判断 B 以降 serve は `core/imagefile.h`
  (形式表)をリンクする。形式表はパッケージに**入れない** — パネルの述語と
  同じ「ホスト提供部品」と宣言する(npy/npz とプロトコルはパッケージ側、
  表の overLink 列はホスト側。一覧が2つあればずれる、の同型)。
- **ImGui は同梱しない。** パネルは widget 集であり、ImGui の版はホストのもの
  (VS Code Remote-SSH が VS Code の版を持たないのと同じ)。パッケージは
  ホストの imgui ターゲットに依存する。
- **repo 化の時期: split-plan §9 判断1を維持 — 今はしない。**
  「準備の完了」の定義は §4 の linktest が CI で緑(スタブホストだけでリンクし
  smoke が走る事実)。実施の再訪条件: 第二の利用者が現れる、またはユーザーが
  指示する。その日の作業は `git subtree split`(§5 段6 でディレクトリを
  1つに集約しておくのはこのため)。**repo 名と取り込み方式(subtree か
  FetchContent か)はその日のユーザー裁定** — 今決めても利用時までに陳腐化し得る2択。

---

## 3. 継ぎ目の形 — 表 + 名前呼びの2本立てをどうするか

### 3.1 診断

2本立ての**区別**自体は正しい(host.h 自身が理由を書いている): 表は「viewer が
決める動詞」、名前呼びは「どのホストでも同じ純関数 + remote 基盤」。
問題は区別ではなく**表の工作精度**にある:

- **位置初期化の事故面。** `g_browseHost` は集成体の位置初期化で、同型
  `void(*)()` のスロットが**4本**ある(savePrefs / wakeUi / dropPreview /
  browseFolderDialog)。真ん中に1本挿して以後がずれても、同型同士なら
  **コンパイルが通り**、実行時に「prefs を保存したら preview が消える」型の
  誤配線になる。protocol 11 の `openRemoteRaw` 追加は実際に表の真ん中への
  挿入だった(remote-headerless §5 の実例) — たまたま隣が同型でなかっただけ。
- **「足すたび host.h と main.cpp の2箇所」自体は消えない**(宣言と定義は
  継ぎ目の宿命)。消すべきは**順序依存**と **null で走る可能性**。
- 第3層(§2.1)は表でも名前呼びでもなく、無宣言。これが最大の欠陥で、
  形の議論はこれを畳んでから初めて意味を持つ。

### 3.2 決定 — 最終形は**リンク時契約1本**、表は解消する

`browse::host` 名前空間の**自由関数宣言**に統一する。現表の15本は関数宣言に、
現名前呼び群(述語・path 系)も同じ名前空間に入る。viewer は main.cpp で定義する
— 中身は今の static 関数を1行呼ぶだけで、**de-static は不要**(表の初期化
15 行が 15 個の1行定義になる。行数は同じ、場所も同じ)。

理由:

1. **欠けはリンクエラーが名指しする。** null のまま走る可能性が型から消える。
   順序も消える(名前で結ぶものに順序は無い)。
2. **足す作業が「宣言1 + 定義1」になり、他のあらゆる関数と同じ作法になる。**
   2箇所を触るのは変わらないが、2箇所目を忘れても黙って通ることが無くなる。
3. **wakeUi の worker スレッド呼び出しは自由関数でも同一。** host.h が
   std::function を退けた理由(capturing wrapper)は関数には当たらない。
   constant-init の論点も消滅する(初期化するものが無い)。
4. **パッケージ後の「ホストが実装すべきもの」が1ヘッダで完結する。**
   linktest のスタブはこのヘッダを実装するだけ(§4)。

却下した形と理由:
- std::function 束: host.h 既存の理由のまま(worker から呼ぶ、初期化順)。
- 仮想 interface: 差し替える客が居ない(selftest は実 viewer を運転する。
  スタブホストはリンク単位の差し替えで足りる)。vtable と初期化の議論を
  買って得るものが無い。
- 「全部を表に」: 純関数 ~50 呼び出し箇所を継ぎ目の衣装に着せ替える —
  host.h 自身が既に退けた形。

**経過措置**（段5より前に新しい入口を追加する場合のみ）: 表の初期化を constexpr
named-assignment builder(`constexpr BrowseHost makeHost(){ BrowseHost h;
h.toast=&toast; …; return h; }`)に変え、フィールド位置の取り違えをコンパイル時に検出する。次の入口追加が
段5より遅いなら省略してよい — 二度作るものではない。

### 3.3 逆向き(browse.h)はそのまま

viewer→browse の ~45 シンボルは入口であり契約の一部。パッケージ時は
そのまま公開ヘッダになる。selftest が直接叩く関数群(rbBuildView /
rbSelectionStacks / rbCursorFollow …)は「NOGL が描画物と同じ関数を運転する」
ための公開であり、**削らない**。

---

## 4. linktest — 「持ち出せる形」の赤緑

新 CMake ターゲット `browse_linktest`: core/browse の 2 TU + remote.cpp +
**スタブホスト1ファイル**(`browse::host` 契約の全関数の最小実装 — toast は
stderr、述語は npy のみ true、preview 系は no-op)+ NOGL smoke(合成 entries で
rbBuildView / rbSortShown / rbSelectionStacks / rbSameListing を回し、
既知の入出力を assert)。

- **赤の定義**: 今日これを作ると**リンクしない**。未定義参照は `app` と
  viewer シンボルの列であり、**その一覧が §5 段4の作業一覧そのもの**になる
  (数える道具を先に置く、という fail-first のこの家の形)。
- **緑の定義**: 段4・段5 が終わるとリンクして smoke が走る。
- それまでは `EXCLUDE_FROM_ALL`(壊れたターゲットを CI に置かない —
  各段単独 CI 緑の規律)。CI へ入れるのは段6。
- 較正: 緑になった段で、スタブから関数を1本消して**リンクエラーがその名前を
  言う**ことを一度確かめる(「わざと戻して落ちることで較正した」の家例:
  tasks.csv 先頭行の browse-keys 較正)。

---

## 5. 段階分け

各段は単独で main に入り、CI 緑、単独で意味を持つ。UI 3段(段1-3)と
構造3段(段4-6)は継ぎ目を挟んで反対側なので**独立に進められる**。
順序制約は2つだけ: 段1→段2(panel.cpp の同じ節を触る)、段4→段5→段6。

試験の作法(全段共通): fail-first — op と assert を先に入れて**落ちる run を
見てから**実装する。期待値は手で導出し、導出を assert の隣に書く。
selftest は共有 home を継ぐ(bookmarks / recents の絶対数を assert しない —
starmark と同じ**差分・フリップ**の形にする)。

### 段1 — 開けられるメニュー(点1・6・8)

中身:
- **一覧の背景**(行の無い領域)**右クリック = `…` と同じ panel menu。**
  ファイルマネージャで最も探される場所にも同じ入口を置く。入口を**追加**するのであって
  `…` は動かさない(覚えた人の場所を変えない。ラベル付けもしない —
  ラベルは点1の原因ではなく、利用者が探した場所に入口がなかったのが原因）。
- **非既定チップ(flat / tree / a-z)のクリック = panel menu を開く。**
  チップは現在の設定を表示しているため、押すと設定メニューを開く。現在は SmallButton の見た目で
  押しても何も起きない — 押せる見た目で押せないものを直すのが最安の1手。
- **Ctrl+L = パス編集**(#3 裁定どおり。Ctrl+F と同じ focused-window gating)。
- **クラムの中央省略**(#2 裁定どおり): 収まらないとき、中間セグメント群を
  **1つの `…` クラム**に畳む(先頭のルートクラム=ホストと、末尾の葉は必ず残す)。
  `…` クラムのクリックは畳んだ祖先の一覧ポップアップ(各行クリック可 —
  祖先への到達性を落とさない)。折返し(`NewLine`)は死ぬ。

試験(browse-keys に op / probe を足す):
- `bgrclick` + `chkpmenu:1`: 背景点に右クリック注入 → panel menu が開く。
  probe: `RbToolbarGeom.panelMenuOpen`(`IsPopupOpen("rbpanelmenu")` を毎フレーム
  記録)。背景点の導出を assert の隣に書く: rb フィクスチャの root は
  entries 10(dark.npy + flat.npy + notes.txt + frame_000..023 の group 1行 +
  dir 6: digitset/expset/gainset/padset/scanroot/unpadded)+ `..` = **view 11 行**。
  `g_rbForceH` で 11×listRowH より十分高くし、y = listTopY + 11.5×listRowH。
  赤: 現状は popup が開かず probe 0。
- `chipclick` + `chkpmenu:1`(前段でメニューから flat に): probe
  `RbToolbarGeom.chipCentre` へ click 注入。赤: 現状チップは無反応。
- `ctrlL` + `chkedit:1`: probe = `I.pathEditing`。赤: 未配線で 0。
- クラム: `waitdir` で `rb/expset/zdeep/a` へ降り、`g_rbForceW=300` で
  (i) パス行の行数 1(probe: パス行 y 範囲。現状は折返しで listTopY が
  行分下がる — その差で赤が出る)、(ii) `…` クラムが在ること、
  (iii) ポップアップ行数 = `pathSegments().size() − 表示中セグメント数`
  (パスは temp 依存なので、導出は同関数から取り実行時に突き合わせる —
  NOGL が「描画物と同じ関数を運転する」既存の形)。

### 段2 — 選択バー(点2)

中身: **nSel ≥ 2 のときだけ**、一覧と下端 status line の間に1行:

```
[Open 2] [Open 2 as one stack] [2 frame averages] [Copy paths] [x]
```

- 動詞は行右クリックメニューと**同一コード**(共通関数に括る。二重登録は
  必ず不整合になる — §10.8 がメニューについて言うことの縮小版)。ラベルも同文。
- **数はバーに書かない**(ラベル内の対象名指しを除く): 「2 of 10 selected ·
  16 frames」は status line が既に言う。下端行の家規「他のどこも言っていない
  ことだけを言う」を、バーは動詞・status は事実、で守る。
- **Temporal は載せない**(#107: sigma_t は stack の属性。多選択の temporal
  動詞は存在しない — バーが再輸入しない)。
- 出没するが**下端**なので上の行は動かない(出没帯の禁は「一覧の上」の話。
  棚卸し #26 の裁定どおり)。1行ぶん一覧が縮むのは選択という自分の行為の
  直後だけで、因果が画面上で読める。
- Esc の段は不変: 選択解除でバーも消える。`[x]` は同じ操作を可視化した入口である。

試験:
- `gainset` へ降りる(view = `..` + gain10 群 + gain20 群 = 3行。導出:
  gainset は gain10_000..007 と gain20_000..007 の 8+8)。`ctrlclick` ×2 で
  両群を選択 → `chkbar:1`(probe: バーの y、非表示は <0)。1つ外す →
  `chkbar:0`。赤: probe 不在/0。
- 等価性: バーの [Open 2 as one stack] click → `chkimg` = 開始時 +1
  (16 frames の1 stack。gain10 / gain20 は同形 — フィクスチャ生成側で確認し
  数値を assert の隣に)。[2 frame averages] → `chkimg` = 開始時 +2。
  それぞれ右クリック経由の既存経路と同じ枚数になること。
- `chkstat`: バー表示中も status line は "2 of 2 selected · 16 frames" を
  言い続ける(事実の置き場は動いていない)。

### 段3 — 空状態は場所の一覧(点3・4・5)

中身: 空状態を「**場所の一覧 + 2つの入口**」に置き換える:

```
Open a folder on this machine...
Another machine (ssh)...
--------------------------------
~  home (this machine)
[local] C:/capt/run42
trc2: /data/run42
trc2: (host)
```

- 「connect」という語はパネルの主要表示から外す（D5）。ssh 入口の文言は
  **場所の語彙**(`Another machine (ssh)...`)。押した先のダイアログは不変。
- places は**直接描画**(コンボの中に隠さない — 空状態の仕事は場所を
  選ばせることだけなので、一覧を主表示にする）。
- 接続後の `v` ポップアップにも **Home** 項目(今のホストの `~` へ)と
  **hosts 節**を足す。hosts は bookmarks / recents の url から host を
  **蒸留**して作る(新しい保存は作らない。保存が2つあればずれる)。
- これで #11(Home)と #1(hosts)が同時に閉じる。

試験:
- 新 op `disconnect`(rbDisconnect 経路)で空状態へ → probe
  `emptyPlacesRows` = 実行時の bookmarks+recents+蒸留 hosts の行数 + Home 1
  (絶対数を書かない — 共有 home を継ぐため。導出式を assert の隣に)。
- 語彙: 空状態のボタンラベルに `Connect` / `Start Remote` が**含まれない**
  ことを文字列で assert(chkstat と同族の文言試験)。赤: 現状は
  `Start Remote (ssh)...` が在る。
- places の Home click → `waitdir:~`(local peer では home 展開先)。
  bookmark 行 click → `waitdir` 相当。既存 `goToPlace` 経路なので開通のみ。

### 段4 — 状態の帰属(linktest を赤で導入)

- `browse_linktest` を `EXCLUDE_FROM_ALL` で追加(§4)。**未定義参照の一覧を
  取り、それを本段の作業一覧として板に貼る。**
- §2.2 の帰属表どおりに移す: browse 所有 → `browse::Shared`(App に alias は
  残さない — 型と違いフィールドの alias は無償でない。browse TU 内 ~120 箇所 +
  viewer 側 ~57 行 / 7 ファイルの機械置換)。preview 一式は url 語りの
  host 動詞に置換し、`ImageDoc*` / `PendingGroup` / `app.images` 走査を
  継ぎ目から消す。
- 検証: **挙動保存**。fail-first ではなく同一性検証(split-plan §5 の家基準):
  スイート緑 + browse / localbrowse / browse-keys / browse-dbl の
  **stderr バイト一致**。加えて linktest の未定義参照が state / preview の分
  だけ**減っている**こと(赤が縮む数を段の成果として記録する)。

### 段5 — 継ぎ目1本化

- `BrowseHost` 表を `browse::host` 自由関数宣言へ(§3.2)。main.cpp の表初期化
  15 行は同数の1行定義に。名前呼び群も同じ名前空間へ(browse TU 内の呼び出し
  置換は機械的。`g_browseHost.toast(…)` → `browse::host::toast(…)`)。
- host.h は「ホスト契約」1枚になる: 上に契約(ホストが定義する)、下に
  browse_state.h(パッケージが定義する)。`#include "../app/state.h"` が
  **core/browse から消える** — grep 0 がこの段の完了条件。
- 検証: 同一性(段4と同じ4本のバイト一致)+ **linktest がリンクして smoke 緑**
  (ここが切り出しの赤→緑)+ §4 の較正(1本消してリンクエラーが名指し)。

### 段6 — パッケージ形

- ディレクトリ集約: remote.h/.cpp / remote_proto.h / serve.cpp / serve_main.cpp
  を `core/browse/` へ(将来の subtree 単位 = 1ディレクトリ)。include 修正のみ。
- `core/browse/CMakeLists.txt`(object library `browse_core` + `viewer-serve` +
  `browse_linktest`)、親は add_subdirectory。形式表(imagefile)と imgui は
  親から**渡される**依存として書く(§2.3)。
- linktest を CI へ(viewer_selftest 1行)。
- 検証: 同一性 + 3 platform CI 緑。**この段の完了が issue #47 後半
  「別レポジトリで管理できる形」の達成条件。** repo 化そのものはしない(§2.3)。

---

## 6. 決めたこと・決めなかったこと(まとめ)

決めたこと(理由は各節):
- `+` は**残す**(裁定改定。タブ文法が棚卸し #5 より後に立った)。
- クラムは**中央省略**、折返しは死ぬ(#2 の裁定側。行数の呼吸は自己違反)。
- 選択バーは**下端・nSel≥2 のみ・右クリックと同一コード・Temporal 無し**。
- 空状態は**場所の一覧**。hosts は保存せず**蒸留**。
- 統合境界の最終形は**リンク時契約1本**（表は段5で解消。それまで新しい入口が
  来たら constexpr builder を先に)。
- パッケージは**ソース**(subtree 単位を段6で1ディレクトリに)。形式表と
  ImGui は**ホスト提供**。
- repo 化は**しない**(split-plan 判断1維持)。「準備完了」= linktest CI 緑。

決めなかったこと(再訪条件つき):
- **repo 化の実施・repo 名・取り込み方式**(subtree / FetchContent):
  第二の利用者かユーザー指示の日に。→ **ユーザー裁定待ち**(急がない)。
- §10.8 **palette / メニューのテーブル化**: 残課題【Fable】。段1は待たない。
- 点9（スピナー）・点10（Watch の状態表示）: §1 の再訪条件。
- serve の形式拡張(#148 の残り半分)と、リモート行の Reader 実行
  (§4.13.1): 本書の範囲外のまま。
- 段2で status line の `selected` 句を削るか(バーとの重複と見るか):
  削らない側で始める。バーは動詞・status は事実、が崩れたら再訪。
