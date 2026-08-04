# 参照設計 — frame の共有参照化 (単独所有 `seqId` の置き換え)

対象: todo 週末項目「参照設計 (seqId→共有参照。項目0のコピーを置き換え) + 項目20
Watch」の前半。本書が仕様で、実装はここに書いた形と理由に従う。Watch は
docs/watch-design.md が本書の上に組む。行番号はすべて `eb5609a` 時点の
`core/main.cpp`(別ファイルはその都度言う)。数値は grep で数えた実測値。

前提とする正典: docs/terminology.md (frame ⊂ stack ⊂ series ⊂ batch、厳密包含、
操作マトリクスの Close 行、σ_t は stack の属性)、docs/todo-open.md 項目0
(derive はコピーが暫定で、`materializeDerivedFrame` が交換用の継ぎ目)、項目20
(Watch の確定仕様)、項目28 (別プロセスが同じファイルを持ち得る)。

---

## 0. 何ができれば成功か

1. **1 frame が複数の stack に属せる。** 項目0 の derive が画素をコピーしている
   のはこれができないからで (`ImageDoc::seqId` は int 1個 — main.cpp:166)、
   コピーを参照に置き換えるのが本書の直接の目的。
2. **「元ファイルが変わった」が、それを参照する全 stack に一歩で伝わる。**
   Watch (項目20) がコピーの上に立てない理由そのもの。
3. **正典の Close マトリクスがそのまま成立する。** stack を閉じる=参照を放す、
   画素は最後の参照が消えたときに死ぬ、Ctrl+Alt+W は「この stack から」だけ。
4. **測定の不変条件を一度も壊さない。** σ_t は stack の属性のまま、CFA プレーン
   不混合、部分ロードの n/N。移行のどの段階でもテストが緑であること。

## 1. 消費者インベントリ (数えた)

置き換え対象が何に読まれているか。`seqId|seqIndex` を含む行は main.cpp に
**448 行、91 関数** (grep 実測。`seqId` 371 行 + `seqIndex` 90 行、重複行あり)。
内訳: main() より前の本体に 342 行、CLI/セルフテスト用グローバル領域 (19500..20657)
に 7 行、main() 内のセルフテスト群に 99 行。**serve.cpp / remote.cpp /
remote_proto.h は 0 行** — stack のメンバーシップは完全にクライアント側の概念で、
プロトコルと peer はこの再設計に一切関与しない。

関数別の主な消費者 (行数、awk で計数):

| 消費者クラス | 代表 (定義行) | 行数 |
|---|---|---|
| stack 列挙・巡回 | `framesOfSeq` (5991) / `stacksOf` (6001) / `stacksCached` (6016) / `gotoFrame` (6070) / `gotoStack` (6085) / `selectImage` (6027) | `framesOfSeq(` 出現 103 (本体 24 + selftest 79、宣言/定義 4 を含む)、`seqInfo(` 出現 75 (本体 49 + selftest 26) |
| Close 一族 | `closeImages` (2742) / `closeStack` (2783) / `closeBatch` (2827) / `closeCurrent` = Ctrl+Alt+W (2870) | closeStack 単体で 13 行 |
| Files パネル | `buildFileGroups` (18058) / `drawFileList` (18101) | 15 行 |
| セッション | `writeSessionTo` (4156) 14 行 / `loadSession` (4900) 8 行 / `findSequenceSiblings` (5429) / キー `seqframe` `seqload` `seqname` `seqaxis*` `seqlevel`(旧) | 22 行超 |
| 測定キャッシュ | hist (9835) / proj (11026) / ROI 表 (15303) / 差分テクスチャ (1842) / A/B auto range (1817) — **キーは uid+dataRev**。temporal (7027) だけ **seqId+枚数+ROI+CFA で dataRev を含まない** | §3 で扱う |
| series | `Series::Member::seqId` / `seriesModalAccept` (6 行) / `addToSeries` / `removeFromSeries` / `seriesOfStack` / `batchOfStack` / `seqIdOfFirstFramePath` (5735、path で復元) | 30 行超 |
| compare | `followFrame` (1297) / `resolveB` (1314) / `resolveSlots` (1512) / `pumpCompareSlotRestore` (1580)。ピンは `compareBUid` (28 行) / `cmpExtra` (52 行) = **membership の uid** | |
| montage | `montageROI` (3349) — `framesOfSeq` で並べ、新規 doc に画素を合成 | 3 行 |
| derive | `stackMembers` (17617) / `buildDerivePlan` (17672) / `materializeDerivedFrame` (17745) / `applyDerivePlan` (17777) | 計 24 行 |
| remote | `openRemoteStack` (8318、prefetch job に seqId/seqIndex を刻印) / `promotePreview` (8382) / `rfWorker` (2194) / `pumpRemoteFetch` (2313、到着フレームの graft と「閉じた stack へは接がない」判定) / `requestServerTemporal` (7474) / `seqMosaicChanged` (7507) | 20 行超 |
| ローカルローダ | `startSequenceLoad`〜`pumpSequence` (5633)、`seqLoadingId` (17 行) / `seqRestore` (947) | |
| Temporal/Linearity/Export | `recomputeTemporalIfNeeded` (7011) / `drawTemporalAB` 11 行 / `lin.rows[].seqId` / `frameLinCollect` / `exportPerFrameBlock` / `buildTemporalExport` | 30 行超 |
| selftest | main() 内 99 行 + `--derive-selftest` 等。**これが回帰網** | 99 行 |

画素バッファ `data` へのメンバアクセスは **92 行** (`\(->\|\.\)data\b`、psFrame /
RFetchDone 受けを含む)。data への**書き込み**は本体 8 箇所に閉じている:
rfetch 新フレーム 2349 / rfetch 差し替え 2378 / montage 3374 / npz 3676 /
npy 3850 / raw 3991 / crop 4059 / remote 先頭 8205 (+ selftest の fixture 6 箇所)。
`w`/`h` の読みは各 240/244 行 — この数字が §2 の設計を決める。

## 2. 中心決定 — membership は今の ImageDoc のまま、画素だけを共有層へ下ろす

取れる形は2つあり、選ぶのは **B**。

- **A. stack を参照リストの実体にする**: `app.images` は一意な frame だけを持ち、
  `SeqInfo` が (frameRef, index) の列を持つ。「参照」を素直に描いた形だが、
  §1 の 448 行 (列挙・Close・Files・セッション・compare・selftest 99 行) を
  **全部書き換える**。`app.current` の index 意味論、`closeImages` の index 列、
  Files の行 walk、`seqframe` 復元 — 全消費者が同時に動く。
- **B. flyweight**: 今の `ImageDoc` を **membership (1 stack に 1 枚として
  見えているもの)** と再定義し、画素と出自だけを参照カウントされる
  **FrameSource** に下ろす。同じ画素を 2 つの stack が持つ = 2 つの membership
  doc が 1 つの source を共有する。

**B を選ぶ。** 理由:

1. **正典との一致。** terminology.md の frame は「画面に見えている測定の最小
   単位」で、木 (frame ⊂ stack ⊂ series ⊂ batch) の厳密包含は**見えるものの木**。
   B は木をそのまま残し、画素をその**下**の共有プールにする。frame が 2 つの
   stack に居るとき、木の上では 2 枚の frame (2 membership) であり、包含は
   1 枚も壊れない。共有されているのは「中身」で、それは木の主張ではない。
2. **seqIndex は定義から per-membership になる。** membership = doc なら、
   「同じ frame が stack ごとに別の位置を持つ」は doc が 2 つあるというだけの
   こと。448 行の消費者は 1 行も変わらない。
3. **blast radius が2桁違う。** B で書き換わるのは data の読み 92 行 + 書き 14
   箇所 + 会計 4 関数。A は 448 行 + `app.current`/index 意味論の全面書き換え。
4. **identity の連続性。** compare のピン、キャッシュキー、セッション復元は
   すべて membership 単位の事実で、B ではそのまま真。

### 2.1 フィールドの三分割 (これが設計の心臓)

今日の `ImageDoc` (140-178) の全フィールドを **per-FRAME (= FrameSource、共有)** /
**per-MEMBERSHIP (= ImageDoc に残る)** / **per-VIEW (= ImageDoc に残る、表示の都合)**
に割る。

**FrameSource (新設、`std::shared_ptr` で共有):**

| フィールド | 理由 |
|---|---|
| `data`, `w`, `h`, `ch` | 画素そのものと形 |
| `dtype`, `vmin`, `vmax` | データの性質 (vmin/vmax は data から計算される値) |
| `path`, `npzMember`, `remoteUrl`, `remoteFrame` | 出自。Watch の照合キー |
| `rawDtype/rawInterp/rawOffset/rawLE` | 再デコードのレシピ (reload に使う) |
| `srcW/srcH/cropX/cropY` | crop 簿記 = 画素状態の一部 |
| `remoteStep`, `remoteErr` | 「まだプレビュー画素である」「取得に失敗した」は画素の状態 |
| **新規** `srcId` (uint64) | frame identity。会計と Files の共有表示に使う |
| **新規** `rev` (int) | 画素が中身ごと入れ替わるたび +1 (reload / preview→full) |
| **新規** `mtime`, `fsize` | Watch の基線 (ローカルはデコード時に stat、リモートは §watch) |

**ImageDoc に残る — per-MEMBERSHIP:**

| フィールド | 理由 |
|---|---|
| `seqId`, `seqIndex` | メンバーシップそのもの。**共有 frame は stack ごとに別の位置を持つ** |
| `batchId` | 正典の包含は membership の木。共有 frame が 2 つの batch に見えてよい (それぞれの membership が自分の batch を持つ) |
| `uid` | membership identity (§3) |
| `dataRev` | キャッシュキーの片割れ。**doc 単位の単調カウンタのまま** (§3.2) |
| `name`, `note` | 表示名と素性文。derive は独自の note を書く (17762-17763) |
| `black`, `white` | 表示レンジ。stack の既定「基準フレームを継承」(pumpSequence / pumpRemoteFetch 2358-2364) は stack ごとの規約なので、共有 frame は **stack ごとに別のレンジで映ってよい** — レンジは画素の性質ではなく見方 |
| `cfa`, `cfaPattern`, `cfaColorize` | 解釈。項目24-(2') の「doc の属性か宣言か」は本書では**動かさない** (現状 = doc の属性、stack へは `seqMosaicChanged` 7507 が書き通す)。共有 source を 2 つの stack が別解釈で見るのは合法で、既存の `projSayPlaneMismatch` (10243 系) が言う |
| `displayLut`, `preview`, `remoteFrames` | 表示選択・登録状態 |

**per-VIEW (ImageDoc に残る):**

| フィールド | 理由 |
|---|---|
| `tex`, `texDirty`, `texBlack/texWhite`, `texNearest` | テクスチャは (source の画素 × membership のレンジ) の関数。membership 側に置く。共有 frame をタイルで 2 回映すと VRAM は 2 枚分だが、`texLru` の TEX_KEEP=12 (1789) が上限を握っている。レンジが等しいときの共有は**やらない** (最適化は必要が示されてから) |
| `pendingViewScale` | ビューの都合 (2378 の差し替え時) |

`w/h/ch/dtype/vmin/vmax` は読みが 240+ 行あるためアクセサ化しない。**ImageDoc に
鏡 (mirror) として残し、書き手を source 変異の walk (§3.2) と CoW (§2.2) の
2 箇所に限定する**。「同じ事実を2箇所に持つと乖離する」(461 のコメント) への
答えは書き手を1系統にすること — reload の walk が全 membership の鏡を必ず更新する。
`data` の読み 92 行は `px()` アクセサへの機械置換 (ステージ1)。

### 2.2 変異の規律 — CoW と in-place の線引き

- **crop (`cropInPlace` 4045) は CoW。** source の use_count > 1 なら先に複製
  してから切る。crop は「この stack を別の測定対象にする」操作で、隣の stack の
  画素が足元で変わってはならない。doc の `dataRev` は従来どおり +1。
- **in-place の画素差し替えは「reload」だけ。** preview→full の swap (2378) と
  Watch の再読込。source の `rev` +1 → その source を持つ**全 membership** を
  walk して `dataRev` +1・`forgetImage`・`texDirty` (§3.2 の一箇所に集約)。
- **プラグインは安全。** processor の入力は ABI 上 const (`ps_plugin.h:113`
  `process(const psFrame* in, ...)`)。analyzer は読むだけ。`makeFrame` (3196) の
  ポインタ渡しはそのままでよい。
- **montage (3349) はコピーのまま。** 合成された新しい画素であって共有ではない。

## 3. Identity — uid は membership、srcId は frame

### 3.1 何が何を指すか

- **`uid` は今後も membership の identity。** compare のピン (`compareBUid`,
  `cmpExtra`) が uid を指すのは「**この stack のこの位置** を留めた」という
  ことで、共有化後も意味が変わらない。`followFrame` (1297) は seqId/seqIndex を
  読むので無傷。閉じたピンの剪定 (resolveSlots 1519-1523) も無傷。
- **`srcId` が frame (画素) の identity。** 使い道は3つ: 会計の重複排除 (§7)、
  Files の共有表示 (§4)、compare の正直さ — A と B が同じ source を映して
  いるとき (`a->src == b->src`)、compare チップは A=B の「paused」語彙 (1719 系)
  と同族の一文「A and B share the same pixels」を出す。差分が全ゼロなのは
  嘘ではないが、なぜかを画面が言うこと。
- **A==B の禁則は doc 同一性のまま** (`resolveB` の `b == cur()` 1322)。
  同じ source の別 membership の比較は合法 (禁じる理由がない — フレーム番号を
  介した並べ直しの確認に実際使える)。

### 3.2 何が無効化されるか — reload の walk (一箇所)

共有 frame の reload で死ぬべきものが全部死ぬ流れ。`5092c4b` の規律
(数値は自分が記述した画素と運命を共にする) の一般形:

    reloadSource(src):
        src->data 差し替え, src->rev++, src->mtime/fsize 更新
        for each doc in app.images where doc->src == src:
            doc->dataRev++            → hist(9835)/proj(11026)/ROI(15303)/
                                        diff(1842)/autoRange(1817) が鍵で外れる
            doc の鏡 (w/h/ch/dtype/vmin/vmax) を更新
            forgetImage(doc)          → ana/hist/proj ポインタ掃除 + texLru
            doc->texDirty = true
        for each SeqInfo si that has a member doc above:
            si->stackRev++            → temporal の鍵に入れる (下記)
            srvTemporal / srvTemporalB が si を指すなら再発火
                                        (seqMosaicChanged 7519-7520 と同型)
            linFitStale() + series fit 破棄 (係る series のみ)

**直すべき既存の穴を2つ、ここで一緒に塞ぐ** (どちらもコードで確認済み):

1. `TemporalState` の鍵は (seqId, 枚数, ROI, CFA) だけで **dataRev を含まない**
   (7027-7029)。今日は差し替えのたび `forgetImage` が temporal[0..1] を
   リセットするから隠れているが、reload で枚数も ROI も変わらない場合に
   古い σ_t を返す構造。`SeqInfo::stackRev` を鍵に足す。
2. `forgetImage` (2179) は temporal[0..1] しか触らず **`temporalExtra` を
   忘れている** (1065)。スロットの stack の preview→full 差し替えで既に
   踏み得る。walk はスロット側も落とす。

## 4. Close の意味論 — 正典マトリクスの各セル

- **Close stack** (`closeStack` 2783): membership doc を全部消す (コードは
  `closeImages(framesOfSeq(seqId))` のまま)。source は shared_ptr の参照が
  尽きたものだけ死ぬ — **「画素は最後の参照が消えたときに死ぬ」が言語機構で
  成立し、専用の解放コードを書かない**。SeqInfo・series 除名・lin 行・
  temporal リセット (2818-2820) は無傷。
  **1つ足す**: 画素が生き残ったとき沈黙しない。「closed "10lx" — 18 frame(s)
  freed, 6 still referenced by "10lx (same names as B)"」。derive の
  「両方向で数を言う」約束 (項目0) と同じ形。
- **Ctrl+Alt+W** (`closeCurrent(frameOnly=true)` 2870): **この stack から
  この1枚だけ**。doc が消え、他 stack の membership と source は無傷 — 今日の
  コードが既にこの形をしており、共有化で初めて「他所で生きている」が起き得る
  ようになるだけ。空になった stack の掃除 (2890) も無傷。
- **Close batch / Close all**: batchId walk のまま。共有 source が batch を
  またいで参照されていれば生き残る — 上と同じ一文で言う。
- **Ungroup**: stack の ungroup は今日存在しない (`ungroupSeries` 2972 は
  series 専用)。本書でも作らない — 正典の表で ungroup は series の操作。
- **Files パネルの見え方**: 共有 frame は**参照する stack ごとに1行** (木は
  membership の木のまま)。行に共有印を付け、tooltip で相手を言う:
  「⧉ shared — also frame 7 of "10lx (same names as B)"」。stack ヘッダの
  枚数は今までどおり membership の数。**バイト数は Files には今日も出ていない**
  (`fmtBytesHuman` の呼び出しは Browse 列 17501/17577 とプロパティ/状態バー
  だけ) — 会計の真実面は §7 の予算関数であり、Files への列追加はしない。

## 5. セッション形式

キーの**追加のみ**。旧ビューアは未知キーを読み飛ばして開ける (形式の常道、
項目10 で再確認済み)。

### 5.1 まず、記録すべき既存欠陥 (コードで確認)

derive でコピーされた stack は、今日**セッションを正しく往復しない**。
`writeSessionTo` にメンバーシップを書く行が無く、stack は代表 frame 1 行 +
`seqload 1` で保存される (4289-4307)。復元側の `seqload` は
`findSequenceSiblings` (5429) で**フォルダ全体**を拾い直すので、派生 stack は
「**全 siblings を持つ stack が派生の名前を名乗る**」形で戻る — 中身と名前が
食い違う。stderr で数を言う対象 (4254-4262) は path の無い montage だけで、
path を持つ派生コピーはこの防衛線の外。つまり「既存セッションに保存された
コピー stack の移行」は**存在しない** — 保存された時点で膜だけになっている。
新形式はこれを直すものでもある。

### 5.2 新キー

明示メンバーシップを持つ stack (派生 stack が最初の該当者) の image 行ブロックに:

    stackmember <seqIndex> <path>     ... メンバー1枚につき1行 (path 末尾、空白可 —
                                          image 行と同じ規約)
    stackrule <text>                  ... deriveRuleText (17728) の出力。復元時の
                                          note/名前の素性

- 書く側: `seqload 1` も**今までどおり書く**。旧ビューアの復元は今日と同じ
  (フォルダ全体、劣化だが開く — 現状より悪化しない)。
- 読む側 (新): 直前の image 行に `stackmember` が続いたら、sibling スキャンの
  `App::SeqRestore` (947) を**明示ファイル列**に差し替える。SeqRestore は既に
  files のリストを運べる構造をしている。
- 復元で同じ path が複数の stack に現れたら (共有)、ローダは §6.2 の registry
  で source を共有する。復元順に依存しない (registry は identity tuple で引く)。

compare のピンは今日 path+frame で復元され (`cmpslot` 4205 /
`pumpCompareSlotRestore` 1580)、同じ path+frame が2つの stack に居ると
どちらに留まるか不定になる。追加行で優先を運ぶ (無ければ今日の解決のまま):

    cmpslotstack <stack name>         ... 直前の cmpslot 行への限定
    comparebstack <stack name>        ... compareb 行への限定

`rbplace` とは**交わらない** (ブラウザは「見ている場所」でありデータではない —
項目10 の裁定)。`seqname`/`seqframe`/`seqaxis*` は membership/stack の事実
なので無傷。

## 6. derive の置き換えと、開くときの共有

### 6.1 `materializeDerivedFrame` (17745) がどうなるか

コメント (17602-17605) が予告したとおり、**この1関数だけが変わる**:
`d->data = s.data` (画素のコピー) が `d->src = s.src` (参照の共有) になり、
出自フィールドのコピーは source ごと共有されるので消える。membership 側
(seqId/seqIndex/batchId/note/black/white/cfa) の設定はそのまま。
`buildDerivePlan` / ダイアログ / 両方向の数の約束 / selftest はすべて無傷。
ダイアログの「will copy %d frame(s) (%.1f MB)」(17949-17951) は
「will reference %d frame(s) (no pixel copy)」になる。

### 6.2 開くときの共有 — source registry

source の identity tuple = **(path または url, npzMember, remoteFrame,
raw レシピ, mtime, fsize)**。ローダ (npy 3850 / npz 3676 / raw 3991 /
remote 8205・2349) はデコード前に registry を引き、**同じ tuple の source が
既に居ればデコードせず共有する**。

- **mtime+fsize が tuple に入っているのが要点。** ファイルがディスク上で
  変わっていれば tuple が変わり、別 source として読む — 「再オープンは
  ディスクを読み直す」という今日の意味論を壊さない。変わっていなければ
  RAM の画素と同一なので共有してよい (同じフォルダを2回開けば、2つの batch の
  2つの stack が同じ source 群を指し、メモリは1倍になる)。
- registry は弱参照 (source が全 membership を失えば消える)。

### 6.3 「not resident」のミスはミスのまま (V1 の裁定)

derive の規則が選んだが常駐していない frame (`notResident` 17663-17664) は、
今後も**報告して除外**する。「まだ読まれていない frame への参照」は作らない。

理由: 画素の無い source を許すと「resident / empty」の第3状態が全消費者に
入る (`sample()` 177 と全 recompute は data の実在を仮定している)。remote の
「まだ来ていない」は今日 membership 層に既に答えがある (prefetch 隊列 +
expectedFrames の n/N) — 空 source で二重に表現しない。
**V2 (本書の範囲外、ユーザー判断待ち)**: SeqInfo に「pending membership」を
持たせ、`pumpRemoteFetch` の graft (2331-2367) が着地時に複数 stack へ
membership を配る形。remote の derive がミスなく効くのはここから。

## 7. メモリ会計 — 積は数えない、和を数える

- `residentImageBytes` (5338) は doc ごとに `data.size()` を足している —
  共有後は**同じ source を2回数える**。srcId で重複排除した和に変える。
  消費者は `claimedImageBytes` (5346) と budget 判定 3 箇所 (8289 / 8359 / 8423)、
  ローダの逐次加算 (5566・5604) — 後者は「その load が新たに積む分」なので、
  共有ヒットは 0 バイトとして数える。
- derive ダイアログの見積り (17949) は §6.1 のとおり文言ごと変わる。
- Files パネルは §4 のとおり今日バイトを出していない。出すことになったら
  (将来)、**数えるのは unique source の和**であって stack×frame の積ではない —
  これがこの節の一行の教訓。
- VRAM は無傷 (texLru が membership 単位で上限 12 のまま)。

## 8. 移行計画 — 全ステージで suite 緑、測定不変条件を跨がない

各ステージは単独で main に入れられる。σ_t per stack / プレーン不混合 / n-of-N は
どのステージでも触らない (列挙・集計・expectedFrames のコードが不変のため)。

| # | 内容 | blast radius (実測ベースの見積り) | 検証 |
|---|---|---|---|
| 1 | **FrameSource を敷く。共有はまだ無い** (全 source の use_count == 1)。data 読み 92 行 → `px()` 機械置換、書き 8+6 箇所が source を作る形に、CoW コード (crop) を入れる (まだ発火しない)、`residentImageBytes` を srcId 和に (結果は不変)、`mtime/fsize` をデコード時に stat | ~120 行、意味変化ゼロ | 既存 suite 全部が無変更で緑 = このステージの合格基準そのもの |
| 2 | **共有が起き得るようにする**: §6.2 registry + Files の共有印 + closeStack の生存通知 + compare の same-pixels 文。共有の初出は「同じフォルダを2回開く」 | registry ~40 行 + ローダ呼び出し 5 箇所 + Files/文言 ~40 行 | `--verify-selftest` 拡張: 2回開いて source 共有・バイトが1倍・片方の batch を閉じても画素が残る・Ctrl+Alt+W が membership だけ消す |
| 3 | **derive をコピーから参照へ** (§6.1)。1関数 + 文言 | ~15 行 | `--derive-selftest` 既存の assert (両方向の数、seqIndex 順、元 stack 無傷、follow 収束) + 新規「派生がバイトを増やさない」「派生後に元を crop しても派生は変わらない (CoW)」 |
| 4 | **セッションの明示メンバー** (§5.2): stackmember/stackrule/cmpslotstack の書き読み | writer ~25 行 / loader ~40 行 | 派生 stack の保存→全撤去→復元でメンバーが一致 (§5.1 の欠陥の回帰テスト)。旧キーのみのファイルが今日と同じに開く |
| 5 | **reload の walk** (§3.2) + temporal 鍵の stackRev + temporalExtra の forget 修正 + Files 右クリック「Reload from disk」(手動再読込 — Watch の前に単独で有用) | ~90 行 | 新 `--reload-selftest`: 画素差し替えで uid 不変・全キャッシュ再計算・σ_t が変わる・fit が落ちる・共有先 stack も同じ一歩で更新される |
| 6 | **Watch** (docs/watch-design.md) | 同書 §7 | 同書 §8 |

ステージ 2 と 3 は入れ替え可能。4 は 3 の後 (書くものができてから)。
5 は 1 だけに依存する — Watch を急ぐなら 5 を 2/3/4 と並行にできる。

## 9. 判断record (2026-08-02 確定 — もう「待ち」ではない)

1. **V2 (未ロード frame への参照): V1 で開始、V2 は後続。** 欠けは欠けのまま
   参照化を完成させ、pending membership は安定後に別段として積む。
2. **CFA 解釈: 現状維持 (doc 毎)。** stack 単位の宣言は dark/flat 宣言と同じ
   語彙なので、平坦統計の仕様レビューと一緒に再議する。
3. **Files のバイト列: 追加する。unique source の和で** (§7 の一行の教訓どおり)。
4. **テクスチャ共有: 見送り。** 必要が示されてから。
5. **用語 (項目5)**: 別件のまま (ユーザーが分類概念の再議論を希望)。

---
## (原文) 明示的に決めずに残していたもの

1. **V2: 未ロード frame への参照** (§6.3)。remote derive の完全版。
2. **CFA 解釈の持ち主** (項目24-(2') の宣言 vs doc 属性)。本書は現状維持を
   選んだが、sharing はどちらの答えとも両立する。
3. **Files のバイト列** — 出すか、出すなら unique 和で (§7)。
4. **レンジが等しい共有 frame のテクスチャ共有** — 最適化。必要が示されてから。
5. **用語** — 本書は正典に従い stack と書いた。UI 文字列の sequence 引退
   (項目5) も済んだ (issue #58)。
