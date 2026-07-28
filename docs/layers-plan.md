# 実装計画: 正典 (terminology.md) にコードを合わせる

順序は「痛み × 実装リスク」。正典を満たす*最小の変更*のみ。検証は headless selftest + GUI 撮影の 2 本立て。

## 1. Close は stack ごと (最優先・報告済みバグ)

`closeCurrent()` (main.cpp:1311) が現在フレーム 1 枚しか消さない。正典の Close 行に反する。

- 新ヘルパを closeCurrent の隣に。`closeImages(idx群)`= 降順に forgetImage → glDeleteTextures →
  erase、current 再選択、`imagesRev++`、`resolveB()` が空なら `compareB/compareBSeq` をクリア。
  `closeStack(seqId)`= framesOfSeq→closeImages、`app.seqs` から SeqInfo 削除、`lin.rows` の該当行削除+
  `lin.rev++`、`temporal.seqId=-1`、`srvTemporal.seqId==seqId` なら `srvTemporal={}`。
  `closeBatch(batchId)`= 含む stack と単発 frame 全部。
- `closeCurrent(bool frameOnly=false)`: `seqId!=0 && !frameOnly` なら closeStack。
- メニュー: Close Stack/Image `Ctrl+W`、Close Frame `Ctrl+Alt+W`(stack 内のみ)、Close Batch
  `Ctrl+Shift+W`、Close All は現状維持。Files の seqctx に「Close stack」、batchctx に「Close batch」。
- 取りこぼし対策 (本命の罠):
  - `rfQueue`: `rfMtx` 下で `job.seqId==seqId` を除去し `rfPending` を同数減。**加えて** pumpRemoteFetch
    の uid==0 分岐 (:1133) で `seqInfo(d.seqId)==nullptr` なら破棄。両方要る (worker 実行中の 1 件は
    キュー掃除では消えない)。
  - `rbOpenQueue` は seqId をまだ持たないので stack では照合不能。**closeBatch では batchId 一致を
    除去**する(でないと batch が復活する)。stack 単位で後から開き直るのは許容しコメントに残す。
  - `seqLoadingId==seqId` なら `stopSequenceLoader()`+`seqLoadingId=0`(pumpSequence が残骸に
    seqLoadingId を刻印して復活させる)。空 batch は項目 3 の prune で除去。
- 検証: `--close-selftest`(fixture=testdata/multi: 3 フォルダ×5 枚)。中央 stack の中間フレームで
  closeCurrent → images 15→10、seqs 3→2、当該 seqId のフレーム 0、`temporal.seqId==-1`、lin.rows に
  残骸なし。続けて `local://…/rb/scanroot` で fetch 中に closeStack → rfQueue に該当 job なし、2 秒
  pump しても当該 seqId のフレームが生えないこと。GUI: multi を開いて Ctrl+W → stack 行 3→2、
  残った行の枚数が「5f」のまま(「4f」でない)こと。

## 2. Open 時の batch 割り当て (picker フッター)

- `App::pickBatchMode`(0=1 batch、1=トップフォルダ毎) を `pickMerge` の隣に。`openPickerWith` で 0 に
  リセット。UI はモード行の下に 2 行目 `batch: ( ) one batch  ( ) N batches (one per top folder)`。
  `pickMerge==1` では BeginDisabled(排他ではなく merge が勝つ)。トップフォルダ = `g.name` の最初の `/` まで。
- batch 生成を**受理時に移す**: `openFolder`(:3588) と RbScan(:4460) は `batchId=0` のままにし
  `pickerAccept()` で採番。`pickerSelection()` は副作用なしのまま(既存 selftest が通る)。Cancel で空
  batch が残る現バグも消える。命名は mode0=ルート名 / mode1=`ルート/00`、`uniqueBatchName()` で衝突は
  ` (2)`。セッションは batch を**名前で**復元する(`imgbatch`→`batchReuse`)ので一意でないと再読込で合体。
- 以降の配線は既存のまま(`enqueueGroups`→`loadBatchId` / `rbOpenQueue{…batchId}`→`pumpRemoteOpenQueue`)。
- ②の average.npy に新機構は不要: 各フォルダの単独ファイルは既に `00/average.npy` という別グループ
  なので、フィルタ `average` + mode1 で別 batch になる(= Open 操作を 2 回に分ける)。
- 検証: `--picker-selftest` に UC5 追加(fixture を gen_testdata.py へ:
  `batchset/{00,01,02}/frame_###.npy + average.npy`)。mode1 受理 → 相異 batchId 数 == トップフォルダ数、
  1 stack の全 frame が同一 batchId、batch 名が全て相異なる。GUI: mode1 で Load → Files に
  `batchset/00` `…/01` `…/02` の 3 見出し、各配下に `frame_???.npy` stack 行と `average.npy` 行。

## 3. stack の「Move to batch」

- `seqctx` に `BeginMenu("Move to batch")`: 既存 batch 一覧(自分と "preview" を除く)+区切り+「New
  batch...」(rename と同じ InputText → `newBatch(uniqueBatchName())`)。単発 frame 行にはコンテキスト
  メニューが無いので `imgctx` を新設し同じ項目を置く。
- 変更は `framesOfSeq(seqId)` 全部の `batchId` 書き換え + `imagesRev++`。**stack は丸ごと移る**(frame
  単位の移動は UI に出さない = 正典の包含関係)。永続化の追加は不要(`imgbatch <name>` が既に往復)。
- `pruneEmptyBatches()`(move 後と close 後に呼ぶ): image / `loadBatchId` / `seqQueue[*].batchId` /
  `rbOpenQueue[*].batchId` のどれからも参照されない batch を削除。preview 疑似 batch は除外。
- 検証: `--batch-selftest`: multi を読み `moveStackToBatch(seq, newBatch("moved"))` → 全 frame の batchId
  一致、旧 batch が prune された。続けて saveSession→closeAll→loadSession で "moved" 見出しに復元。GUI:
  サブメニューの画と、移動後に別見出し配下へ移った Files。

## 4. ブラウザから Temporal (開かずにサーバ集計)

- group 行の「Open as stack」直後に「Temporal stats (server)」。複数選択(≥2 npy)時は「Open N selected
  as stack」の隣にボタン 1 個。実行は **measure worker (`mEnqueue`)** — browse worker は LIST/SCAN で
  直列に詰まり、ブラウズ用 ssh セッションを共有しているので使わない。
- `MJob{op=MOP_TEMPORAL_STATS, url=files[0], files=files}`、ROI なし(画面に絵が無い)、`cfaType=0`
  (未オープンのファイルにモザイクを推測すると plane 別の数値が黙って狂う。タグに `plane=all` と明記)。
- 表示先は Temporal パネル。`ServerTemporal` に `label`/`detached` を足し `seqId=-2` を番兵に。タグは
  `[server <host>, N frames — not opened: <label>]`、隣に「Open as stack」(測定→本オープンが 1 クリック)
  と「x」。ポップアップは不採用(ドッキングも比較もできない)。発火時に `showTemporal/focusTemporal=true`。
- protocol 2 の相手: LE_GROUP は v3 のみなので group 行自体が存在しない(到達不能)。複数選択経路は
  MEASURE が v2 からあるので実行可、ただし `hasMeta` が無く事前検証できないため失敗は既存の
  `[server failed]` 表示に落とす。`peerVersion()<2` は無効化。
- 検証: `--rtemporal-selftest`: `local://…/rb/scanroot/10lx` に接続しメニューと同じ
  `requestBrowseTemporal()` を叩き `!pending` まで pump → `valid && frames==8`、σ_t を `--remote-selftest`
  の TEMPORAL 参照計算と rel<1e-9 で比較。GUI: 画像を 1 枚も開いていない状態で
  `[server local peer, 8 frames — not opened: 10lx/frame_???.npy]` が出た Temporal パネル。

## 5. ローカルブラウズの入口

- File > Open Folder... の直後に「Browse Folder (Local)...」。`folderDlgMode`(0=open,1=browse) を足し同じ
  `pfd::select_folder` を使い、`pollFileDialog` の mode1 で `startRemote("local://"+p)` +
  `showRemote/focusRemote=true`(`--scan-selftest` と同じ経路)。
- パネル名は**「Browse」**。ただし文字列 "Remote" は (a)`Begin` (b)`DockBuilderDockWindow` (c)Ctrl+F 判定の
  `strcmp(…Name,"Remote")` (d)保存済み layout.ini とセッションの `layout_begin` で load-bearing。よって
  **`Begin("Browse###Remote")`** と ID 据え置きで改名し DockBuilder も同じ文字列、(c) は
  `nav->RootWindow->ID == ImHashStr("Remote")` に置換。`panels` 行は位置固定の 9 個なので
  `app.showRemote` の名前・順序は触らない。places は `placeUrl()` が host 空で `local://<path>` を返し
  `goToPlace`→`startRemote` も対応済み。表示だけ `[local] <path>` に整形(prefs の文字列は url のまま)。
- 検証: `--localbrowse-selftest`: メニューが呼ぶ関数を叩き `connected && host.empty() && entries 非空`。
  GUI: File メニューの新項目と、`local peer` + ローカルパスのパンくずが出た「Browse」パネル。

## 6. パターンの `?` は変化する桁だけ (peer 側)

`serve.cpp:358 npySegKey` が全桁を潰すので `gain??_???.npy` になる。二段構えにする。

- 1 段目(バケット化)は現状維持 (key = 全桁 run を `\x01` に潰した文字列)。2 段目を
  `groupNumberedNpy` に追加: バケット内で run 毎の相異値を数え、変化する run のうち相異値が
  最多のもの(同数なら**後ろ**。client の `findSequenceSiblings` と同じ規則)をフレーム軸に。変化する
  run が 2 本以上なら**フレーム軸以外の値でサブバケットに分割**する(条件混在 stack は σ_t が無意味で、
  client 側も分割するため)。要素 1 のサブバケットは singles へ。
- pattern は各グループ先頭メンバから再構築: フレーム軸 run のみ `'?'×桁数`、他の桁は数字のまま。ゼロ詰め
  不揃い(`frame_9`/`frame_10`)は先頭メンバの桁数を採用(表示だけの問題。メンバ名は `putGroupEntryV3` が
  実名で送るので復元性は pattern に依存しない)。key は変えないので LIST の枠組みも client も無変更。
- 検証: `--remote-selftest` の LIST grouping 節を拡張。fixture 追加 `rb/gainset/{gain10_000..007,
  gain20_000..007}.npy` → group 2 本、pattern が `gain10_???.npy`/`gain20_???.npy`(`gain??_???.npy` で
  ないこと)。`rb/expset/{g00_e00..03, g01_e00..03}` でサブバケット分割(2 本、`g00_e??.npy`)も assert。
  GUI: `local://…/rb/gainset` の Browse パネルに `gain10_???.npy [8 frames]` 等 2 行。

## 正典 (terminology.md) への修正提案 — 正典は決定記録であって不可侵ではない

1. 不変条件「1 回の Open = batch 1 つ」→「既定は 1 つ。picker の指定時のみ複数」(項目 2)。
2. 不変条件に追加「**batch 名は一意**。セッションは名前で復元するため、衝突は ` (2)` を付す」。
3. マトリクス Temporal/stack 欄に追記「未オープンの stack(ブラウザ上のグループ)にもサーバ集計を許す。
   結果は `not opened` タグ付き」(項目 4)。
4. Close/frame 欄に例外を明記「`Ctrl+Alt+W` でその 1 枚だけ閉じる逃げ道。既定は stack ごと」。
5. 命名の規則に追記「桁 run が 2 本以上変化する場合、peer は `?` を 2 本並べず stack を分割する」(項目 6)。
6. マトリクス リニアリティ/batch 欄「fit の対象集合」は**現状不可**(`linRecompute` は `app.seqs` 全体を
   走査し batch フィルタを持たない)。「将来(現在は全 stack が対象)」への格下げを提案。
