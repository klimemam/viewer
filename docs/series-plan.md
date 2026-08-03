# series (系列) — 第4の層の仕様

正典は [terminology.md](terminology.md)。`frame ⊂ stack ⊂ series ⊂ batch` は**厳密**、層の省略は可・飛び越しは不可、
**series は1つの batch に収まる**。

## 1. データモデルと不変条件

```c++
struct Series {
    int id = 0, batchId = 0;                  // 属する batch (厳密包含)
    std::string name;                         // 初期値 "<batch名> 掃引"、リネーム可
    std::string paramName;                    // "illuminance" / "exposure" ...
    char unit[16] = "";                       // 空 = 未設定。既定で "lx" にしない
    int kind = KLinearity;                    // linearity / PTC / temperature / other
    struct Member { int seqId; double value = NaN; bool include = true; };
    std::vector<Member> members;              // 順序 = 表示順（値順ソートはボタン）
};
std::vector<Series> series; int nextSeriesId = 1; int curSeriesId = 0;
```

**メンバシップは `Series::members` が唯一の真実**。`SeqInfo::seriesId` は置かない（二重管理は必ずずれる）。逆引き
`seriesOfStack(seqId)` は線形走査 — series は数個・メンバは数十、Files は `imagesRev`/`seriesRev` でキャッシュを作り直す。

**`SeqInfo::level` は削除し、値は `Member::value` へ移す。** level は**単位とパラメータ名が無ければ意味を持たない量**で、それらは
series の持ち物だから。stack に残すのは「単位はアプリで1つ、値は stack 毎」＝*暗黙の単一 series* を残すこと。値・順序・include
を1構造体に置けば表の1行が `members[i]` に 1:1 対応し keepInc 保存も消える。値は seqId 紐づけでリネームにも並べ替えにも不変。

不変条件（selftest で全数検査）:

1. 全メンバの stack は実在し、その frame の `batchId == series.batchId`。
2. 1 stack は**高々1つの** series に属する（追加時に旧 series から外す）。
3. `closeStack` はメンバから外し、`closeBatch` は自分の series を消す。メンバ 0 は `pruneEmptySeries()` が削除。
4. `moveStackToBatch(member)` は**その stack を series から外す**（toast で明示）。series ごと動かすのは series 行の方。
5. メンバ 1 個以下の series は**合法**（作りかけ）だが `seriesCanFit()==false`。
6. `value` が NaN のメンバは「未設定」と表示し fit から除外。**0 として扱わない。**

## 2. リニアリティは series に向く

- `linRecompute(int seriesId)`: `app.seqs` 走査をやめ `S.members` を順に回す。fit に入るのは
  `include && value 有限 && stats valid` の行のみ。level==0 の dark 実測則はそのまま。
- `LinState` に `seriesId` を持たせ、`unit[16]` は**新規 series のプリフィル**へ格下げ（prefs キー `linunit` は据え置きで
  動く）。表とグラフの `[%s]` は series の unit を読み、未設定なら `[unit 未設定]`（勝手に lx にしない）。
- パネル頭に series セレクタ `"<batch名> / <series名>"`。0 個なら空状態:「series がありません。掃引は自動では作りません」＋
  **「この batch の stack から series を作る…」**（現在の frame の batch を初期値に modal を開く）。
- `drawSeriesModal()`（作成と編集で同一）: 名前 / パラメータ名 / 単位 / 種類 と、対象 batch の stack 一覧
  `チェック | stack 名 | 値(InputDouble) | 抽出元`。値は `extractLevelFromName(si.name)` を**提案として最初から表示**し、
  見た上で Create を押させる（＝確認）。未編集の提案は淡色＋「(推定)」、抽出不能は空欄のまま。編集時も同じ modal: チェックの
  増減＝メンバ追加/削除、値の打ち直し、`↑↓` で並べ替え、「値でソート」。

## 3. 永続化

セッションの画像行の**後ろ**に平坦なブロックを追記する（既存パーサのまま、キーは一意）:

```
series <name>            # 以降 seriesend まで直前の series に付く
seriesbatch <batch名>    # 復元は batch も名前で（imgbatch と同じ規約）
seriesparam <param名>
seriesunit <unit>        # 空 = 未設定
serieskind <n>
seriesmember <value|-> <0|1> <先頭フレームの path>   # 値 - = 未設定、順序どおり
seriesend
```

- **メンバは path で参照**。解決は遅延: パース時は `app.seriesRestore` に積み、
  `pumpSequenceAndQueue()` で `seqRestore`/`seqQueue`/`seqReady` が空・`seqRunning` 偽になった時点で path→image→`seqId` を
  引いて構築。解決できなかったメンバは黙って捨てず件数を toast する。
- 後方互換は両方向成立: 旧ビューアは未知キーを読み飛ばし、新ビューアは `series` ブロック無しのセッションを series 0 個で開く。
  prefs は無変更（`linunit` は「新規 series の既定単位」として生きる）。
- **旧セッションの移行**: `seqlevel` を読んだ場合に限り、level 付き stack が 2 個以上ある batch ごとに series を1つ作る
  （名前 `<batch名> 掃引`、unit=`linunit`、kind=linearity）。皆無なら**何も作らない**（フォルダ構造からの推測は禁止）。

## 4. Files パネルの見え方

- batch 見出しの直下に **series のサブノード**を先に並べ、その後に非メンバ stack を従来どおり**インデント無し**で置く。
  非メンバが隠れたり下がったりしてはならない。
- series 行 `▸ 25℃ 照度掃引   [lx] (5)`。メンバ行は1段インデントし、**ラベル先頭に値**を出す
  `  100 lx · 100lx/frame_???.npy`。未設定は `  値未設定 · …` とラベル側に（右端の dim メタに混ぜると見落とす）。順は `members`。
- `serctx`: rename / 編集…(modal) / Move to batch(**全メンバが動く**) / **解散 (ungroup)**（series だけ消し stack は残す）
  / **Close series**（正典どおり中身ごと破棄）。後者2つは別項目・別文言で並べる。
- `seqctx` に「Series ▸」: **同じ batch の** series 一覧（他 batch のものは無効化＋tooltip「別 batch の series。先に Move to
  batch」）、区切り、「新しい series を作る…」。メンバの Move to batch は不変条件 4 の警告付き。

## 5. picker の「掃引として開く」と Files 複数選択

- footer 3行目、`pickMerge==0 && selGroups>=2` の時だけ出す: `掃引として開く (series を作る)` ＋ パラメータ名・単位・各
  グループの推定値プレビュー列。`pickBatchMode==1` とは**排他**（series が batch をまたぐため）— チェック時は 0 に固定し disable。
- `pickerAccept()` は `app.seriesPending{name,param,unit,kind,{stack名→値}}` を積むだけ。local は `enqueueGroups` 後、
  remote は `rbOpenQueue` 消化後に、§3 と同じ pump 位置で `resolvePendingSeries()` が解決する。
- `app.seriesPick`(seqId 集合) を Ctrl+click で溜め、フッタに「N stacks selected → Group as series…」（modal は §2 と同一物）。

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

経緯と検討: [.background/series-plan.md](.background/series-plan.md)
