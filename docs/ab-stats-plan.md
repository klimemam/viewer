# A/B 統計パネル — compare 中に B の統計も出す

「重ねる」と「画像と同じ並びで横に並べる」の**両方**を持つ、統計パネルの A/B 仕様。

## 1. キャッシュ — 2 スロット化
- `App::HistState hist[2]` / `ProjState proj[2]` / `TemporalState temporal[2]`。0=A、1=B。**struct
  自体は変えない**(最小差分)。`recomputeHistogramIfNeeded(ImageDoc*)` → `(ImageDoc*, HistState&)`、
  projection も同様、`recomputeTemporalIfNeeded()` → `(const ImageDoc*, TemporalState&)`。
  中身は `app.hist` 参照を引数に置換するだけ。
- 参照は少ない(hist 1057/1543/6204/6799、proj 1058/6879/6998、temporal 1059/1449/1545/3376/
  4212/7801)。`forgetImage`/`closeAll` は**両スロット**を潰す。
- **slot 1 は `cmpB() != nullptr` のときだけ埋める**。compare off の 1 フレーム目で
  `hist[1].uid=0; temporal[1].seqId=-1;` と 1 回無効化し以後触らない → compare off の
  コストは現状と完全に同じ。パネルは `ImGui::Begin` が true のときしか計算しない(既存の
  性質)ので閉じたパネルの B 側コストはゼロ。
- **frame step**: `compareFollowFrame` on だと B の uid も毎回変わり hist/proj は A/B 両方が再計算
  = **ステップ当たりのコストがほぼ倍**。temporal は `seqId`+ROI がキーなので再計算されない。
- 12 Mpx での実コストは**未測定。推測を書かない**。測り方: 12 Mpx の 2 stack を開き
  Histogram/Projection を出した状態で `--bench 300` を (a) compare off、(b) CmpSplit+follow on
  で走らせ median frame time の差を取る。ただし `--bench` はフレームを進めないので **bench に
  フレーム送りを足す必要がある**。B 込みの draw が 8 ms を超えるなら `app.annBusy` と同型の
  ガード(キーリピート中は slot 1 を再計算せず B 側見出しに `stale`)を足す。先に測る。

## 2. 重ねる (overlay) の描き分け
色相は CFA plane に割当済(`CFA_COLS`/`RGB_COLS`)なので **B に別色相は使えない**。
- **A は現行のまま**。**B は同色相・同線幅・破線**、塗りなし。alpha を落とすと暗い plane が消える
  ので据え置き、区別は破線と塗りの有無で付ける。L2 に `addDashedPolyline()` を追加(ImDrawList
  に破線がない)。
- **凡例**: `beginPlot` 直後に `drawABLegend(pr, aName, bName)` —— 実線見本 + `A: <name>`、
  破線見本 + `B: <name>`(テキストだけの凡例にしない)。
- **Histogram**: A は塗り、B は**階段状アウトラインのみ**を上に重ねる。`nSeries >= 3`
  (CFA 4 系列 / RGB 3 系列)では **A も塗りをやめ両方アウトライン** —— 塗り 8 面は読めない。
  あわせて ROI パネルと同じ **plane セレクタ**(`all/R/Gr/Gb/B`)を追加する(CFA で重ねを
  実用にするには事実上必須)。y 軸は compare 中のみ **sampled px に対する割合 [%] に正規化**
  (`H.sampled` が A/B で違えば bar 高は比較不能)。軸ラベルに明記しフッタに実 px 数を残す。
  `bins` は触らない = A の数値は不変。
- **Projection**: B は破線、min-max バーは **A だけ**、y レンジは A∪B で共有。

## 3. 横並び (Side by Side) — 画像の並びを写す
- 設定は**グローバルに 1 つ**: `app.abStatsLayout` = `Auto / Overlay / Side by side`。置き場所は
  `View > Compare A/B` + ステータスバーの `A/B split 50%` 隣の小コンボ。**パネル毎にしない**
  理由: 「画像と同じ並び」は比較全体の性質でパネルの性質ではない。パネル毎だと状態が 5 倍に
  なり、崩れた瞬間にどのパネルが何を見せているか分からなくなる。
- `Auto`: `CmpSplit`(画像が左右)→ **横並び**。`CmpWipe`/`CmpFlip`/`CmpDiff`(画像領域が 1 つ)
  → **重ね**。どちらのモードでも明示選択で他方に切り替えられる(両方残す)。
- **横並びは常に 50/50**、`splitFrac` に追従しない。形を比べるには両者のプロット幅が同一で
  ある必要があるため。写すのは**順序と向き(A が左)**であって分割比ではない。
- **左が必ず A**。各半分の上に見出し帯 `A  <name>` / `B  <name>`(中立色)、x/y 軸レンジは強制的に
  同一。パネル幅が `320 * uiScale` 未満なら重ねに退避し `幅不足のため重ね表示` と明記する。

## 4. 数値表 — 横並びは「列の対」
- **Temporal**: 行 = 量、列 = `A | B | Δ (A−B) | Δ [%]`。符号は画像側の `A-B` に合わせる。
  ヘッダには必ず単位(`sigma_t [DN]` 等)。Δ[%] = `(A−B)/|A|×100`、A=0 または量が既に % なら
  絶対差のみ(単位 `pt`)。**絶対と相対の両方**(σ の 0.3 DN が大きいかは相対でしか判らない)。
  常駐フレーム数の差は頻出なので列見出しに `A (n=12/12)` `B (n=8/300)` を併記。B が stack で
  なければ B 列は `—(stack ではない)`、Δ は空。`app.srvTemporal` は **B 側を自動発火しない**
  (remote job が倍になる)。`Measure B` ボタンで明示実行し同じ 4 列表に流す。
- **ROI stats 表**: 行 = ROI、列 = 量なので A/B を列対にできない。**1 ROI = 2 行**(A 行 / B 行、
  B 行はインデント+行頭 `B`)。Δ 行は既定オフのトグル(行数 3 倍)。annotation は全画像共有なので
  B のサイズが違えば矩形を clamp し `(clipped)` を付す。
- **Analysis グリッドは A のみのまま**。列軸が既に ROI で埋まり、実行コストが実測で存在し
  (`ana.runMs`、e-SFR は重い)、provenance が run 単位に紐づくため。代わりに `Run on B` を追加し、
  押されたときだけ `B: <name>` 列群と **B 専用 provenance 行**を足す(`anaAuto` では走らせない)。

## 5. やってはいけないこと / 不一致時の退避
- **A と B を平均・合成した系列を作らない**(差は Δ 列と Diff モードの仕事)。**B の数値を「B」と
  書かずに出さない**(凡例・見出し帯・列見出しのどれかが必ず言う)。**B のキャッシュが A を
  無効化しない**(別オブジェクトで構造的に保証、潰すのは slot 1 のみ)。
- **サイズ違い**: histogram は A の black/white を bin 軸に固定して B を投げ込む(画素数差は
  正規化 y で吸収済み)。projection は**長さか原点が違えば重ねない** —— 自動で横並びに落とし
  `profile 長が違う (A=1600, B=1200) ため重ねない` と表示。**伸縮して合わせるのは禁止**。
- **dtype 違い**: 軸ラベルに両 dtype を書き `dtype 不一致` を明示、Δ は注記付きで出す。**CFA
  違い**: 系列は**名前で対応付ける**(R↔R)。片側だけの系列は片側だけ描いて印を付け、`ch0` を
  `R` に対応させない(canon: plane は混ぜない)。ch 数が違えば Δ 列は出さない。

## 6. 検証
`--abstats-selftest <dir>`(既存 `--verify-selftest` と同じ `check()` 形式、GUI なし):
(1) 2 stack を load、A=stack0/frame0、B=stack1 を `setCompareB`、`CmpSplit`+follow on →
`hist[1].uid == B->uid` ほか両スロットが埋まる。(2) B の `bins` は selftest 内で B の画素から
素朴に再計算した値と**完全一致**、mean/sd/σ_t は相対 1e-6 以内。(3) compare off で取った
`hist[0]`/`proj[0]` と on 後の値が**バイト一致**(A は B の存在で変わらない)。
(4) `gotoFrame(1)` ×5 で両スロットのキーが追従し、`temporal[1].seqId` はステップで変化しない。
(5) サイズ違いの B で projection が「重ね不可」フラグを立て、ch 数違いで Δ 列が出ない。
(6) `CmpOff` で slot 1 が無効化され、B を close 後 slot 1 の `img` が dangling でない。

GUI(パネル×モードで各 1 枚): Histogram 重ね = 塗り(A)+破線階段(B)+凡例に 2 ファイル名 + y 軸が
`% of sampled px`。Histogram 横並び = 左 `A <name>` 帯 / 右 `B <name>` 帯、x 軸目盛り同一。
Projection 重ね = 破線が A の実線に沿い min-max バーは A のみ。Temporal = 4 列表、全ヘッダに
`[DN]` で Δ の符号が A−B。ROI = 各 ROI が 2 行で B 行に `B`。

**再測定**: (a) follow-frame ステップの draw 時間(bench にフレーム送りが必要)、(b) ROI 表の
`1.35 ms/frame @ 400 ROI` は行数 2 倍で測り直し、(c) CFA 4 系列 × 2 side の描画。

経緯と検討: [.background/ab-stats-plan.md](.background/ab-stats-plan.md)
