現行ドキュメント: [series-plan.md](../series-plan.md) の背景 — 設計を縛っていた当時の現状、正典への修正提案、実装後に判った分。

# series (系列) 実装計画 — 経緯と検討

元の表題は「実装計画: series (系列) — 第4の層をコードに入れる」。各 phase は単独で出荷できる、という前提で組んだ。

## 0. 設計を縛る現状

> 設計当時 (phase 1 着手前) のコード状態のスナップショット。

- `linRecompute()` は `app.seqs` **全体**を走査（batch フィルタも系列も無い）。`LinState::unit` はアプリで1つ。
- **`seqlevel` はセッションに書かれていない**（読みだけで `writeSessionTo` に出力が無い）。level は現状セッションを跨がず、
  移行対象の実データは `linunit` だけ。
- **`seqname` はフォルダ stack で復元されない**: `seqload` は `seqRestore` に積むだけで SeqInfo は rescan 後にしか
  出来ず、直後の `seqname` 行は `cur()->seqId==0` に落ちる。→ メンバ参照は **stack 名でなく先頭フレームの path**（§3）。
- **Files に複数選択が無い**（行は `app.current` 単一の Selectable）。「multi-select → Group as series」は選択モデルの新設を
  伴うので最後の phase に回し、それまでは `seqctx` の「Series ▸」で同じ結果に到達させる。

§3 の「メンバは path で参照」は、この **§0 の `seqname` バグを踏まない**ためのもの。
旧セッションの移行についても、§0 のとおり書き出しが無いので実際はほぼ空振りする — それが正直な挙動。

## phase 分け

- phase 1: データモデルと不変条件（UI 無し）。
- phase 2: リニアリティを series に向ける。`Auto levels` ボタンはこの提案列に吸収して廃止。
- phase 3: 永続化。
- phase 4: Files パネルの見え方。
- phase 5: picker の「掃引として開く」。phase 6（任意）: Files 複数選択。

## 7. 正典 (terminology.md) への修正提案

1. 表「実体」欄 `App::Series (SeqInfo::seriesId)` → `App::Series (Series::members)`。値と順序は stack の属性ではない（§1）。
2. 操作マトリクス series/Close の欄に **解散 (ungroup)** を併記。「中身を捨てる Close」と「まとめを解く ungroup」は別操作。
3. 不変条件に追加: **1 stack は高々1つの series に属する**（当面の制限。表示と操作が一意に決まる）。
4. 不変条件に追加: **メンバを単独で別 batch へ移すと series から外れる**（厳密包含の帰結）。禁じるのではなく画面で告げる。
5. 「series が持つもの」に追記: **単位の既定は未設定（空）**。既定 `lx` は「単位を仮定しない」に反する（現 `LinState::unit` はプリフィルへ格下げ）。
6. 命名の規則に追記: **series の既定名は `<batch名> 掃引`**（人が付けるまでの初期値。リネーム可）。
7. [layers-plan.md](layers-plan.md) の修正提案 6（「リニアリティ/batch 欄は現状不可」）は phase 2 で解消するので**撤回**可。
   → 撤回済み（当該項目に取り消し線と理由を入れた）。

### 検証後の追記（実装を出してから判った分）

8. 命名の規則の「リニアリティの Auto levels」は phase 2 で**廃止済み**の名前だった。正典を現存する面
   （作成/編集モーダルの推定値列・picker の掃引プレビュー）に書き換えた。規則そのもの（フォルダ部を読む /
   フォルダ部が無ければ範囲 run を除く）は不変。
9. 不変条件に追加: **値は「読めなければ未設定」**。`atof` は読めない文字列に 0 を返し、linearity の 0 は
   暗電流 stack という**最も効く点**なので、テキスト欄もセッションの欄も全体消費を要求する strict parse に
   する（`parseSeriesValue`）。
10. 不変条件に追加: **種類が fit の式を決める**。`seriesCanFit` は kind が linearity / PTC のときだけ真。
    temperature を「DN/℃ の感度」として印字しない。
11. 不変条件に追加: **series を編集したら計算済み fit を捨てる**。ラベルは series から生で読むので、捨てないと
    lx で測った傾きが `DN/ms` の見出しの下に出る。
12. 不変条件に追加: 同じフォルダの 2 回開き（正典が明示的に認めている）では、復元時のメンバ探索は
    **その series の batch 側の stack** を選ぶ。
13. §2 の「値は提案として最初から表示」は**新規作成のときだけ**。既存メンバの「値未設定」に推定値を入れ直すと、
    何も触らない Save が値を捏造する。編集時は空欄のまま、推定値は隣の列に出すだけにした。

### 新コードレビューの後（掃引の同定まわり）

14. §5 は当初こう書いていた:
    「remote は `rbOpenQueue` 消化後に、§3 と同じ pump 位置で `resolvePendingSeries()` が stack 名（＝`PendingGroup::name`
    ＝`SeqInfo::name`）で解決。`rbOpenQueue` は seqId を持たず名前解決以外の手が無い（`RemoteOpen::name` が保持済み）。」
    この「`PendingGroup::name＝SeqInfo::name` で解決」を**撤回**する。名前の一致自体は 15 で成立させたが、
    **表示名は同定ではない**: Files の F2 は先頭フレームが着いた瞬間から効くので、ドレイン待ちの間に人が名前を
    変えられる。別グループの名前に変えると、そのグループの**レベルが別の stack に付く**（数えられた欠落より悪い）。
    解決は **作成時に刻んだ token**（`PendingGroup`/`RemoteOpen::token` → seqId、`noteGroupStack`）で行い、
    名前一致は token を持たない項目のフォールバックとして残す。
15. 命名の規則は**単一ファイルの stack にも効く**。`startSequenceLoad`（2ファイル以上）だけがフォルダ/パターン名を
    付けていたので、1レベル1ファイル（`lv000/capture.npy`）の掃引は同名の stack が N 個できた。推定値はそこから
    フレーム数（`(8 frames)` の 8）を読んでいた。名前は**生成した場所**で付ける: `startNextQueuedGroup` の
    単一ファイル経路と、`openRemoteStack` のフレーム軸の早期 return（`<group> [remote xN]`）。
16. §3 の「黙って捨てない」は**保存側にも**要る。`app.seriesRestore` が解決待ちの間（リモート掃引なら数分）
    `app.series` は空で、その間の Ctrl+S / autosave / クラッシュスナップショットは series を消していた。
    `writeSessionTo` は seriesRestore をそのまま書き戻し、まだ path を持たない picker の掃引は
    lost に数える。復元側の探索は **file 全体で1つの取得済みリスト**を持つ（同じ path の stack が同じ batch に
    2つ居るのは、正典が勧める「同じ batch へ移してから series を作る」の結果として正常）。
17. 掃引として開けない行は **picker で言う**。1ファイル・2次元の行は stack にならない＝メンバになれないので、
    ドレイン後の「N could not be matched」ではなく、チェックを外せるうちに出す。
    **ローカル限定**: リモートは openRemoteStack が単発フレームにも SeqInfo を鋳るので
    (`[remote x1]`)、その行は stack になり掃引に**入る**。リモートで警告するのは嘘になる。
