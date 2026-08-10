# Watch 設計 — 元ファイルの変化を検知し、通知し、再読込する (項目20)

docs/reference-design.md の上に組む (前提: FrameSource、reload の walk =
同書 §3.2、ステージ5 の手動 Reload)。意味論は確定済み (項目20、ユーザー決定
A6/A6b、2026-07-30) — 本書はその**機構**を決める。行番号は `eb5609a` 時点。

確定済みの意味論 (再掲、変更しない):

- 既定は**通知 → 手動 Reload**。自動更新はオプトイン。
- 部分更新された stack は **stack ごと作り直す** (frame 単位の差し替えはしない)。
- 判定は **mtime + サイズ**。「新しい」ではなく「**等しくない**」を変化とみなす
  (コピーで mtime が戻るケース)。
- 書き込み途中のファイルは掴まない: **サイズが2回連続で同じ**になってから。
- ポーリングは**数秒〜十数秒**、Browse は**パネルが見えているときだけ**。

---

## 1. 変化検知の元 — origin ごとに、ただし判定は同じ式

判定式は両 origin 共通: **(mtime, size) が基線と等しくない → 変化候補**。
基線は FrameSource が持つ (reference-design §2.1 の `mtime`/`fsize` —
ローカルはデコード時に stat 済み)。

- **ローカル**: `std::filesystem::last_write_time` / `file_size`。stack の
  メンバー列は開いたときの走査と同じ関数で取り直す (siblings =
  `findSequenceSiblings` 5429、フォルダ walk = `scanFolderGroups` 6111) ので、
  「新しいファイルが現れた / 消えた」も同じ一往復で分かる。in-file frame-axis
  stack (1つの .npy に F 枚) は stat 1回。
- **リモート**: **peer が既に返す LIST** (プロトコル v3: remote_proto.h:57-63、
  クライアント側パース remote.cpp:417)。**新しいプロトコルは作らない。**
  監視対象のディレクトリに LIST を1発投げると、グループ行が
  **bytes = メンバー合計サイズ、mtime = 最新メンバー、メンバー名の全列**を
  運んでくる (serve.cpp:452-454 で集計、546-551 で送出)。つまり
  **stack 1本の変化判定はグループ行1つの比較**で済む: (合計 bytes, 最新 mtime,
  名前集合) のどれかが基線と違えば変化。
  - 正直さの注意: グループ行は**メンバー個別の mtime を運ばない**。リモートの
    通知は「この stack のソースが変わった (N 枚中どれかは言えない)」になる。
    ローカルは per-file stat なので枚数を言える。**どちらも reload は
    stack ごと作り直し** (A6b) なので、個別特定は通知の精度の差でしかない —
    画面はそのとおりの粒度で言う。
  - 開いている remote stack の照合キーは `SeqInfo::remoteHost/remotePort/
    remoteFiles` (420-423)。remoteUrl 型 (frame-axis 1ファイル) は
    その1ファイルの行を見る。
- **基線が無いもの** (Watch 実装前から開いていた stack、remote は open 時に
  mtime を通していない): **最初のポーリングが基線を作る**。基線づくりは
  通知しない。変化は2周目から見える — 「開いた瞬間の状態」を偽造しない。

## 2. ポーリングの対象と間隔

対象は2種類。どちらも (host, port, dir) で**重複排除**する (同じフォルダの
stack が3本開いていても LIST は1発)。

| 対象 | いつ回すか | 間隔 (初期値) |
|---|---|---|
| 開いている stack のソース dir | アプリが最小化されていない間 (28410 の ICONIFIED 判定と同じ線) | ローカル 5 s / リモート 15 s |
| 見えている Browse インスタンスの現在 dir | そのインスタンスが**描かれている**間 (drawPanelRemote 16191 の頭で lastDrawnAt を刻む。窓が閉じ/折りたたまれれば止まる) | ローカル 3 s / リモート 10 s |

- 間隔は定数から始める。prefs 化はユーザーが欲しがってから (§9)。
- リモートのコスト上限: 監視 dir 数 × 15 秒に1 LIST。数百ファイルの dir でも
  LIST 1発はグループ集計込みで peer 側 stat の和 — 項目20 の「毎秒 LIST は
  論外」の線の内側。
- Browse インスタンスのポーリングは**そのインスタンス自身のワーカー**に
  `RbList` (567) として積む。listing の置換は既存の refresh と同じ経路で、
  `RemoteBrowse::rev` (552) が上がり選択/カーソルの正直さも既存の仕組みが守る。
  ワーカーが busy ならその周は**飛ばす** (溜めない — 次の周が来る)。

## 3. Watch ワーカーとアイドルループ

- **stack 監視は専用ワーカー1本** (`watchWorker`)。ローカルは stat、リモートは
  **自分の Session** で LIST — 「one Session, one owning thread」の規律は
  rfWorker (2194) / mWorker (2415) と同じ形で守る。ホストごとに接続を張り、
  使い回す。Browse の監視は §2 のとおり各インスタンスのワーカーで、
  watchWorker は触らない。
- **ワーカーは自前のタイマーで回る**。UI スレッドは発行に関与しない。
  発見があったときだけ `glfwPostEmptyEvent()` で UI を起こす (ローダの
  5612 と同じ作法)。**発見が無ければアイドルの 0 fps を乱さない** —
  `wakeFrames` (1132) の節約設計をポーリングが壊さないための要点。
- 結果の統合は UI スレッドの `pumpWatch()`。既存のポンプ列 (28414 からの
  pumpSequenceAndQueue / pumpRemoteFetch / pumpMeasure / ...) に1つ並ぶ。
  アイドルスキップの `working |=` 群 (28376-28407) に
  `working |= watchFindingsPending()` を1行足す — 統合待ちの発見がある間は
  フレームを飛ばさない。ポーリングの**発行**のために UI を起こすことは無い。

## 4. サイズ2回一致 (書き込み途中を掴まない)

変化候補は即通知しない。ワーカー内の2段階:

    poll k   : (mtime,size) が基線と違う → candidate に記録 (その値ごと)
    poll k+1 : candidate と同じ (mtime,size) がもう一度読めた → 確定、通知へ
               違う値が読めた → candidate を更新してもう一周

書き込み中のファイルはサイズが伸び続けるので candidate に留まり続ける。
確定まで画面には何も出さない (「書き込み中です」の中間通知は出さない —
確定していない事実を言わない)。リモートはグループ行の合計 bytes で同じ
2段階を踏む。

## 5. 通知面 — どこで言うか

- **開いている stack**: Files のその stack のヘッダ行直下に琥珀の1行 +
  [Reload] ボタン。文はローカル「3 file(s) changed on disk」/ リモート
  「source changed on <host> (N files → M files / bytes / mtime のうち
  言える事実)」/ 消えたものがあれば「2 file(s) no longer exist」。
  この行は**消えない** (トーストは消える — `seqNote` の先例 5657-5659:
  「A toast expires; "60 of 300" must not」)。加えて初回検知時にトースト1発。
  状態は `SeqInfo` に持つ (`WatchState`: 基線、candidate、確定した変化の要約)。
- **Browse インスタンス**: 通知ではなく**listing がそのまま新しくなる**
  (ポーリング = refresh。listing は測定ではなく眺めなので、黙って
  更新してよい — rev 機構が選択の誤爆を防いでいる)。
- **watch folder (項目20-(a)、新しい連番を自動で開く)**: インスタンスの
  トグル「Watch: open new stacks」(既定 OFF)。ON の間、監視 dir に**新しく
  現れたグループ**を既存の `rbOpenQueue` 経路で開く。既に開いた名前は
  インスタンスが覚える (同じものを2回開かない)。撮影しながら見る、の答え。

## 6. Reload — 参照モデルを通る作り直し (この結合の眼目)

[Reload] は reference-design §3.2 の `reloadSource` + membership 再構築。
**seqId と SeqInfo は保つ** (名前・per-frame X軸・series 所属・compare の
ピンが生き残る)。「stack ごと作り直す」(A6b) はメンバーシップ列の話であって、
identity の張り替えではない。

    reloadStack(seqId):
        1. ソース列を取り直す (ローカル: siblings 再走査 / リモート: LIST の
           グループ行のメンバー名列)
        2. 生き残る名前  → その FrameSource を reloadSource() で in-place 更新
                           (mtime/size が基線と同じなら読み直さない)
           消えた名前    → その membership doc を closeImages で外す
                           (他 stack が共有していれば source は生きる)
           新しい名前    → 新 source をデコードし、新 membership を足す
        3. seqIndex を振り直し、expectedFrames を更新、n/N を言い直す
        4. reloadSource の walk が全キャッシュを落とす (下表)

**共有が効く場所**: 手順 2 の `reloadSource` は source を持つ**全 stack の
membership** を一歩で更新する。元 stack を Reload すれば、そこから derive
した stack も、同じフォルダをもう一度開いた stack も、**同じ瞬間に新しい
画素になる** — コピー設計なら必要だった「どのコピーが古いか」台帳が
存在しない。これが参照設計と Watch を同じ週末にやる理由の実体。

- **派生 stack の規則は再適用しない。** derive のメンバーシップは derive 時の
  決定 (note が as-of を言っている)。元 stack が Reload で枚数を変えても、
  派生側は画素だけ新しくなる。派生側の通知行はそう言う:
  「source stack was reloaded — pixels updated; rule not re-applied」。
- **寸法が変わったら**: 鏡の更新と fit 要求、ROI が画像外に出たら既存の
  クランプ規約 (montage/crop と同じ) に従い、変わった事実を通知行で言う。

## 7. Reload で何が死ぬか (5092c4b の規律: 数値は画素と運命を共にする)

| もの | 機構 | 状態 |
|---|---|---|
| Histogram / Projection / ROI 表 / 差分テクスチャ / A/B auto range | (uid, dataRev) の鍵が外れる (9835 / 11026 / 15303 / 1842 / 1817) | walk が dataRev++ するだけで既存機構が働く |
| Temporal (σ_t) | 鍵に `SeqInfo::stackRev` を足す (reference-design §3.2 — 今日の鍵 7027 は dataRev を含まない) | ステージ5 の修正に含む |
| temporalExtra (スロットの σ_t) | forgetImage が今日触っていない (2183 の k<2) — walk で落とす | 同上 |
| server temporal (σ_t サーバ集計) | その stack を指していれば再発火 (seqMosaicChanged 7519-7520 と同型)。**自動再発火は A 側 (`srvTemporal`) のみ** — B の「Measure B は押した人のもの」(835 の struct 注記) はそのまま、B 側は結果を破棄して「measure again」を出す | 新規1行ずつ |
| series の fit / linearity 行 | `linFitStale()` + 係る series の fit 破棄 (5092c4b と同じ規律) | walk に含む |
| テクスチャ | texDirty + texLru はそのまま | 既存 |
| compare のピン / follow | uid が生き残るので無傷。divergence は次の resolve で再計算 | 既存 |
| ROI・注釈・per-frame X軸・series の値 | **死なない** (画素の記述ではなく問いの記述)。ただし軸の値の個数が枚数と合わなくなったら既存の規約どおり「なぜ使わないか」つきでフレーム番号に落ちる (export-design §8) | 既存 |

## 8. 複数ウィンドウ (項目28 との整合)

2つのプロセスが同じ dir を監視する: **両方が通知し、Reload はプロセスごと**。
ポーリングは読むだけなので衝突しない。プロセス1が Reload してもプロセス2の
RAM は変わらない — プロセス2は自分の通知行を持ち続け、自分の [Reload] で
追いつく。**プロセス間の協調は作らない** (autosave は既にインスタンス別 +
PID 生存判定 — 4464 / 20726 — で分離済み。Watch が足すものは無い)。
これは仕様であり欠陥ではない: 測定中の窓の画素を、隣の窓の操作が
動かさないことの方が正しい。

## 9. 自動更新 — 判断record (2026-08-02 確定)

- **グローバル1スイッチで開始** (File > Watch > Auto-reload、既定 OFF)。
  per-stack トグルは運用を見てから。
- **ポーリング間隔は提案初期値で開始** (stack 5s/非表示15s、Browse 3s/10s)。
  prefs 化は後。
- **watch 対象は初版範囲で開始**: 自動オープンは連番グループのみ、監視は
  stack + Browse dir のみ (単発は手動 Reload)。

## (原文) 自動更新 (オプトイン) と決めずに残していたもの

- **自動 Reload はグローバルのオプトイン1つ** (File > Watch > Auto-reload、
  既定 OFF、prefs 保存)。ON でも§4 の2回一致は踏む。自動 Reload の実行は
  「通知行を出してから次のフレームで実行」— 何が起きたかは同じ場所に残る。
- 決めずに残す (ユーザー判断待ち):
  1. per-stack の自動更新 (「この stack だけ自動」) — グローバルで足りるか
     運用を見てから。
  2. ポーリング間隔の prefs 化と値そのもの (§2 の表は初期値)。
  3. watch folder の自動オープン対象 (グループのみか、単発ファイルもか) —
     初版はグループ (連番) のみ。
  4. リモート単発 frame (stack でない開き方) の監視 — 初版は stack と
     Browse dir のみ。単発は手動 Reload (ステージ5) で拾える。

## 10. 検証 — `--watch-selftest <dir>`

文字列と状態に対して assert する (この機体は GL を screenshot できない):

- 基線: 初回ポーリングが通知を出さないこと。
- 2回一致: 1回だけ違う値を見せた (書き換え続けた) ファイルが通知に
  ならないこと / 2回同じで通知になること。mtime だけ戻したコピー
  (サイズ同一・mtime 相違) が「変化」になること (等しくない、の規約)。
- Reload: uid が生き残ること、dataRev が上がること、σ_t・fit・hist が
  再計算されること (stackRev が鍵に効いている証明)、消えたファイルの
  membership が外れて n/N と通知文が両方の数を言うこと。
- 共有伝播: derive した stack が**自分の Reload なしに**新しい画素を
  見ること — この機能の存在理由なので必ず assert する。
- Browse: 監視中の listing が新ファイルで rev を上げること、
  「open new stacks」ON で新グループが1回だけ開くこと。
- リモート経路はローカル peer (`--localbrowse-selftest` の流儀) で
  同じ script を回す — 判定式が origin で違わないことの証明。

---

## 11. 実装から返ってきたもの (2026-08-10、PR #156 — 検知+通知)

本書の §1/§4/§5 が着地した。**実装が設計を5箇所で訂正している**ので、以後は
この節が優先する。

### 11.1 §5 の「`WatchState` を `SeqInfo` に持つ」は `SeqTable` の規律と衝突する

ワーカーが `SeqInfo*` を握るのは、**reseat がまさに防いでいる欠陥そのもの**
(`cc1ee8b`)。分割した:

- **確定したサマリ**は `SeqInfo` に (画面が読むのはこれ)。
- **基線と candidate** はワーカーが持つ (ポインタではなく、stack を識別する
  値で引く)。

### 11.2 §4 は「いつ言い始めるか」しか書いていない

2回一致の規則は**開始条件としてしか書かれておらず、終了条件が無い**。文字どおり
実装すると、in-place で書き換わり続けるファイルで**琥珀の行がちらつく**。
規則は**対称**にする (`SetState::conf`): 言い始めるのに2回一致が要るなら、
言うのをやめるのにも2回一致が要る。

### 11.3 §2 の重複排除は `findSequenceSiblings` を割らないと届かない

§1 は「stack のメンバー列は開いたときの走査と**同じ関数**で取り直す」と書き、
§2 は「(host, port, dir) で重複排除して LIST は1発」と書くが、**この2つは
今の関数の形では両立しない** —— `findSequenceSiblings` が走査と判定を1つに
持っているため。`siblingNamesAmong(headName, names, pat)` を切り出し、
`findSequenceSiblings` = それ + `directory_iterator` 1回、という形にすると
両方が成り立つ (バイト同一を `--scan-selftest` の 58 stack で確認済み)。

### 11.4 §1 のフォルダ走査は**部分集合 stack** に対して偽になる

本書は暗黙に **stack == フォルダのグループ**と仮定している。picker で一部だけ
選んだ **派生 stack** では、自分の規則で除外したフレームが毎回「現れた」と
報告され続ける (実装前にこれを踏んだ)。2つのディレクトリにまたがる stack も
同様に、他方のメンバーを「消えた」と確定する。**監視の対象はフォルダではなく
stack のメンバー列**である。

### 11.5 (mtime, size) の粒度には原理的な穴がある — 選択は正しい、限界は書く

**サイズの変わらない書き換えが、クロック粒度 (Windows で約 15.6 ms) の内側で
起きると見えない。** 実測で踏んでいる: 192→272 バイトの書き換えが mtime を
動かさず、素朴な watcher が「変化なし」と答えた。(mtime, size) を選んだこと
自体は正しい —— 中身のハッシュを取るのは監視のコストではない —— が、
**限界として書いていなかった**のは本書の落ち度である。

### 11.6 アイドルの主張は、実装中に一度壊れた

「発見が無ければアイドルの 0 fps を乱さない」(§3) は、**Watch を切ったときに
発見がキューに残ると永久に破れる** (`watchFindingsPending()` が true のまま =
session 中ずっと 12 fps)。切るときにキューを drain する。実測 22 秒で
**0.078 s → 0.156 s** (ポーリング分のみ、UI は起きない)、bench 2562 fps 不変。

### 11.7 未着手 (PR #156 は検知と通知だけ)

§6 の membership 再構築を伴う Reload / §1 のリモート LIST / §2・§5 の Browse 側
と「open new stacks」/ §9 の Auto-reload。板に4行として立てている。今の
[Reload] は既出荷の `reloadStackFromDisk` を呼ぶだけで、**メンバーシップは
変わらない** —— tooltip がそう明言し、増減したファイルはフォルダを開き直す
必要があると言う。
