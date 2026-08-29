# 実装計画: series (系列) をコードに入れる

> **状態: phase 1〜5 実装済み。** 任意の phase 6（Files の multi-select）だけは
> 未実装である。§0 の「現状」と各 phase の予定形は、設計着手時の制約と実装順を
> 保存した履歴として読む。現行のデータモデルは §1 の mixed frame / stack member。

正典は [terminology.md](terminology.md)。順序付きデータ層は
`frame ≼ stack ≼ series` (`≼` は集合包含ではない)。各層は単独でも存在でき、
中間層の省略は可、逆転は不可。**series と各メンバは同じ1つの batch に
`managed-by` される**。
各 phase は単独で出荷できる。

## 0. 設計を縛る現状

- `linRecompute()` は `app.seqs` **全体**を走査（batch フィルタも系列も無い）。`LinState::unit` はアプリで1つ。
- **`seqlevel` はセッションに書かれていない**（読みだけで `writeSessionTo` に出力が無い）。level は現状セッションを跨がず、
  移行対象の実データは `linunit` だけ。
- **`seqname` はフォルダ stack で復元されない**: `seqload` は `seqRestore` に積むだけで SeqInfo は rescan 後にしか
  出来ず、直後の `seqname` 行は `cur()->seqId==0` に落ちる。→ メンバ参照は **stack 名でなく先頭フレームの path**（§3）。
- **Files に複数選択が無い**（行は `app.current` 単一の Selectable）。「multi-select → Group as series」は選択モデルの新設を
  伴うので最後の phase に回し、それまでは `seqctx` の「Series ▸」で同じ結果に到達させる。

## 1. データモデルと不変条件（UI 無し・phase 1）

```c++
struct Series {
    int id = 0, batchId = 0;                  // この series を管理する batch
    std::string name;                         // 初期値 "<batch名> 掃引"、リネーム可
    std::string paramName;                    // "illuminance" / "exposure" ...
    char unit[16] = "";                       // 空 = 未設定。既定で "lx" にしない
    int kind = KLinearity;                    // linearity / PTC / temperature / other
    struct Member {
        int seqId = 0;                        // stack メンバ
        double value = NaN;
        bool include = true;
        uint64_t frameUid = 0;                // standalone frame メンバ
        int fold = 0;                         // stack が frame として立つ集約
    };                                        // seqId/frameUid は必ず片方だけ
    std::vector<Member> members;              // 順序 = 表示順（値順ソートはボタン）
};
std::vector<Series> series; int nextSeriesId = 1; int curSeriesId = 0;
```

**メンバシップは `Series::members` が唯一の真実**。`SeqInfo::seriesId` は置かない（二重管理は必ずずれる）。逆引き
`seriesOfStack(seqId)` / `seriesOfFrame(frameUid)` は線形走査 — series は数個・
メンバは数十、Files は `imagesRev`/`seriesRev` でキャッシュを作り直す。

**`SeqInfo::level` は削除し、値は `Member::value` へ移す。** level は**単位とパラメータ名が無ければ意味を持たない量**で、それらは
series の持ち物だから。stack に残すのは「単位はアプリで1つ、値は stack 毎」＝*暗黙の単一 series* を残すこと。値・順序・include
を1構造体に置けば表の1行が `members[i]` に 1:1 対応し keepInc 保存も消える。値は seqId 紐づけでリネームにも並べ替えにも不変。

不変条件（phase 1 の selftest で全数検査）:

1. 各メンバは実在する stack または standalone frame のどちらか一方で、
   その `batchId == series.batchId` (`managed-by` の整合)。
2. 1 stack / standalone frame は**高々1つの** series のメンバになる
   （追加時に旧 series から外す）。
3. stack / standalone frame を閉じればメンバから外し、`closeBatch` はその batch
   が管理する series を消す。メンバ 0 は `pruneEmptySeries()` が削除。
4. メンバだけを別 batch へ移すと、**そのメンバを series から外す**（toast で
   明示）。series ごと動かすのは series 行の方。
5. メンバ 1 個以下の series は**合法**（作りかけ）だが `seriesCanFit()==false`。
6. `value` が NaN のメンバは「未設定」と表示し fit から除外。**0 として扱わない。**

## 2. リニアリティを series に向ける（phase 2）

- `linRecompute(int seriesId)`: `app.seqs` 走査をやめ `S.members` を順に回す。fit に入るのは
  `include && value 有限 && stats valid` の行のみ。level==0 の dark 実測則はそのまま。
- `LinState` に `seriesId` を持たせ、`unit[16]` は**新規 series のプリフィル**へ格下げ（prefs キー `linunit` は据え置きで
  動く）。表とグラフの `[%s]` は series の unit を読み、未設定なら `[unit 未設定]`（勝手に lx にしない）。
- パネル頭に series セレクタ `"<batch名> / <series名>"`。0 個なら空状態:「series がありません。掃引は自動では作りません」＋
  **「この batch の stack から series を作る…」**（現在の frame の batch を初期値に modal を開く）。
- `drawSeriesModal()`（作成と編集で同一）: 名前 / パラメータ名 / 単位 / 種類 と、対象 batch の stack 一覧
  `チェック | stack 名 | 値(InputDouble) | 抽出元`。値は `extractLevelFromName(si.name)` を**提案として最初から表示**し、
  見た上で Create を押させる（＝確認）。未編集の提案は淡色＋「(推定)」、抽出不能は空欄のまま。編集時も同じ modal: チェックの
  増減＝メンバ追加/削除、値の打ち直し、`↑↓` で並べ替え、「値でソート」。`Auto levels` ボタンはこの提案列に吸収して廃止。

## 3. 永続化（phase 3）

セッションの画像行の**後ろ**に平坦なブロックを追記する（既存パーサのまま、キーは一意）:

```
series <name>            # 以降 seriesend まで直前の series に付く
seriesbatch <batch名>    # 復元は batch も名前で（imgbatch と同じ規約）
seriesparam <param名>
seriesunit <unit>        # 空 = 未設定
serieskind <n>
seriesmember <value|-> <0|1> <先頭フレームの path>   # 値 - = 未設定、順序どおり
refmember <member>       # 直前の1行への限定。path が container の中を指すとき
                         # だけ書く（.npz のメンバ名 / .exr のレイヤ名）
seriesend
```

- **メンバは path で参照**（§0 の `seqname` バグを踏まない）。ただし path だけでは
  **1つの .npz の2つの image メンバを区別できない** —— 区別するのは §6.2 の identity tuple の
  もう半分 `FrameSource::member` だけなので、container の中を指すメンバ行には `refmember` が続く
  （reference-design.md §5.2 の「直前の行への限定」と同じ形。無ければ今日どおり path だけで解決し、
  旧セッションはそのまま開く）。解決は遅延: パース時は `app.seriesRestore` に積み、
  `pumpSequenceAndQueue()` で `seqRestore`/`seqQueue`/`seqReady` が空・`seqRunning` 偽になった時点で path→image→`seqId` を
  引いて構築。解決できなかったメンバは黙って捨てず件数を toast する。
- 後方互換は両方向成立: 旧ビューアは未知キーを読み飛ばし、新ビューアは `series` ブロック無しのセッションを series 0 個で開く。
  prefs は無変更（`linunit` は「新規 series の既定単位」として生きる）。
- **旧セッションの移行**: `seqlevel` を読んだ場合に限り、level 付き stack が 2 個以上ある batch ごとに series を1つ作る
  （名前 `<batch名> 掃引`、unit=`linunit`、kind=linearity）。皆無なら**何も作らない**（フォルダ構造からの推測は禁止）。
  §0 のとおり書き出しが無いので実際はほぼ空振りする — それが正直な挙動。

## 4. Files パネルの見え方（phase 4）

- batch 見出しの直下に **series のサブノード**を先に並べ、その後に非メンバ stack を従来どおり**インデント無し**で置く。
  非メンバが隠れたり下がったりしてはならない。
- series 行 `▸ 25℃ 照度掃引   [lx] (5)`。メンバ行は1段インデントし、**ラベル先頭に値**を出す
  `  100 lx · 100lx/frame_???.npy`。未設定は `  値未設定 · …` とラベル側に（右端の dim メタに混ぜると見落とす）。順は `members`。
- `serctx`: rename / 編集…(modal) / Move to batch(**全メンバが動く**) / **解散 (ungroup)**（series だけ消し stack は残す）
  / **Close series**（正典どおり中身ごと破棄）。後者2つは別項目・別文言で並べる。
- `seqctx` に「Series ▸」: **同じ batch の** series 一覧（他 batch のものは無効化＋tooltip「別 batch の series。先に Move to
  batch」）、区切り、「新しい series を作る…」。メンバの Move to batch は不変条件 4 の警告付き。

> **現行実装 gap / phase④。** `moveSeriesToBatch()` / `closeSeries()` は現在
> `seqId` だけを収集し、standalone frame (`frameUid`) を移動・破棄しない。
> 上の「全メンバ」が正典であり、mixed series の Move / Close 回帰と
> Files / toast の `member(s)` 語彙を同じ修正で固定する。

## 5. picker の「掃引として開く」（phase 5）と Files 複数選択（phase 6・任意）

- footer 3行目、`pickMerge==0 && selGroups>=2` の時だけ出す: `掃引として開く (series を作る)` ＋ パラメータ名・単位・各
  グループの推定値プレビュー列。`pickBatchMode==1` とは**排他**（series が batch をまたぐため）— チェック時は 0 に固定し disable。
- `pickerAccept()` は `app.seriesPending{name,param,unit,kind,{stack名→値}}` を積むだけ。local は `enqueueGroups` 後、
  remote は `rbOpenQueue` 消化後に、§3 と同じ pump 位置で `resolveOnePendingSeries()` が解決する。解決は**名前ではなく
  token** で行う: picker が受理した時点で group ごとに `PendingGroup::token` を刻み（`core/main.cpp:8770`）、producer 側が
  `app.groupStacks`（`token -> seqId`、`core/main.cpp:7857`）に記録する。名前で引く経路は残っているが、それは token を持たない古いセッション
  （`" [remote xN]"` を含む C2 以前）のための後方互換であって、token がある項目の一次経路ではない。
- phase 6: `app.seriesPick`(seqId 集合) を Ctrl+click で溜め、フッタに「N stacks selected → Group as series…」（modal は §2 と同一物）。

**やってはいけない（受け入れ条件）**: 自動生成しない / batch をまたがせない / 単位を仮定しない（既定は空文字）/
値未設定を 0 扱いせず fit に入れず、画面上で「未設定」と読めること。

## 6. 検証

`--series-selftest <dir>`（fixture = `<scratchpad>/linset`、7 光量 × 24 frame）:

1. 生成: `pickerAccept` で 1 batch・7 stack を読み、全 stack と `extractLevelFromName` の値で series を作る → メンバ 7、
   値すべて有限、`series.batchId` と全 frame の `batchId` が一致。
2. 不変条件: 別 batch の stack の追加が**失敗する** / メンバを別 batch へ move するとメンバから外れる / `closeStack` で
   メンバが減る / メンバ 1 個で `seriesCanFit()==false` / `closeBatch` で series が消える。
3. fit: `linRecompute(id)` の sens/offs/R²/LEmax/K/read が現行 `--lin-selftest` の出力と**完全一致**（同じ行フォーマットで印字
   して差分を取る）。`--lin-selftest` 自体は「全 stack を暗黙の1 series に入れて `linRecompute(id)`」へ**付け替える**
   （値は `extractLevelFromName`、単位は既定 unit）。数値回帰はこれで保つ。
4. 往復: `saveSession`→`loadSession`→resolve まで pump → 名前/param/unit/kind/メンバ数/値/順序/include と所属 batch が一致。
5. GUI 撮影: (a) series サブノード＋値付きメンバ行と非メンバ stack が同居する Files、(b) 推定値の並んだ作成 modal、
   (c) セレクタ付き Linearity と 0 個のときの空状態、(d) picker の新 footer 行。

## 7. 正典 (terminology.md) への修正提案

1. 表「実体」欄 `App::Series (SeqInfo::seriesId)` → `App::Series (Series::members)`。値と順序は stack の属性ではない（§1）。
2. 操作マトリクス series/Close の欄に **解散 (ungroup)** を併記。「中身を捨てる Close」と「まとめを解く ungroup」は別操作。
3. 不変条件に追加: **1 stack / standalone frame は高々1つの series のメンバになる**
   （当面の制限。表示と操作が一意に決まる）。
4. 不変条件に追加: **メンバを単独で別 batch へ移すと series から外れる**
   （series とメンバの `managed-by` を揃えるため）。禁じるのではなく画面で告げる。
5. 「series が持つもの」に追記: **単位の既定は未設定（空）**。既定 `lx` は「単位を仮定しない」に反する（現 `LinState::unit` はプリフィルへ格下げ）。
6. 命名の規則に追記: **series の既定名は `<batch名> 掃引`**（人が付けるまでの初期値。リネーム可）。
7. [layers-plan.md](background/project/layers-plan.md) の修正提案 6（「リニアリティ/batch 欄は現状不可」）は phase 2 で解消するので**撤回**可。
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

14. §5 の「`PendingGroup::name＝SeqInfo::name` で解決」を**撤回**する。名前の一致自体は 15 で成立させたが、
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
