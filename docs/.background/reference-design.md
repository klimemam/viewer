現行ドキュメント: [reference-design.md](../reference-design.md) の背景 — 却下した設計、消費者の実測、既存欠陥の記録、判断record。

# 参照設計 — 背景 (経緯と検討)

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

## 2. 中心決定 — 却下した A と、B を選んだ理由

取れる形は2つあり、選ぶのは **B**。

- **A. stack を参照リストの実体にする**: `app.images` は一意な frame だけを持ち、
  `SeqInfo` が (frameRef, index) の列を持つ。「参照」を素直に描いた形だが、
  §1 の 448 行 (列挙・Close・Files・セッション・compare・selftest 99 行) を
  **全部書き換える**。`app.current` の index 意味論、`closeImages` の index 列、
  Files の行 walk、`seqframe` 復元 — 全消費者が同時に動く。
- **B. flyweight**: 採った形。定義は現行ドキュメント §2 にある。

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

## 3.2 reload の walk が同時に塞ぐ既存欠陥 (コードで確認)

**直すべき既存の穴を2つ、ここで一緒に塞ぐ** (どちらもコードで確認済み):

1. `TemporalState` の鍵は (seqId, 枚数, ROI, CFA) だけで **dataRev を含まない**
   (7027-7029)。今日は差し替えのたび `forgetImage` が temporal[0..1] を
   リセットするから隠れているが、reload で枚数も ROI も変わらない場合に
   古い σ_t を返す構造。`SeqInfo::stackRev` を鍵に足す。
2. `forgetImage` (2179) は temporal[0..1] しか触らず **`temporalExtra` を
   忘れている** (1065)。スロットの stack の preview→full 差し替えで既に
   踏み得る。walk はスロット側も落とす。

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
   (項目5) は別件のまま。
