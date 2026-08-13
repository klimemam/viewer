# Preferences パネル — 設定を変える場所と、値が誰のものかの申告 (issue #50 段2)

> **状態: 提案 (2026-08-13, #50 段2)。**
> [settings-inventory.md](settings-inventory.md) §10.7「まだやっていないこと」の
> 1つ目がこの設計である。前提は2つとも満たされている: 段1 の読む層 (PR #167、
> `core/app/settings.inc`、優先順位 defaults < prefs.txt < settings.jsonc < CLI)
> と、M1「申告を伴う」の先行実装 (PR #182 — ヘッダ無しの読みが note に全部を
> 名乗る)。
>
> 判断が要るものは **§10 の判断リスト** (P1〜P13) にまとめた。番号で答えられる。
> **裁定を仰ぐのは P11 の1件だけ**で、残りは既裁定 (inventory §9 の 20件) の
> 適用である。

## 1. 非目標

- **キーバインドと既定の表示レンジ。** §3.8 の「無いもの」2つは機構そのものが
  無く、パネルを作っても変えられない。判断14 / F6 が入口の別設計。パネルは
  `input.keyBindings` の行を「まだ」と名指しして場所だけ見せる (§3 の規律)。
- **command palette 本体** (browse-topbar-design.md §10.8)。本書が決めるのは
  パネルと palette の**表の関係**だけ (§7)。
- **state.json と履歴5種の移動** (判断3 の「アプリが書く側」)。書き手の変更で
  失うと戻せないので単独の変更のまま (inventory §10.7)。
- **settings.jsonc を書く機構。** 存在しないし、作らない (判断3、§4 で再論)。
- **`loading.raw` の既定 10 キー。** measuring 級で M3 に抵触する (§6)。

## 2. パネルが答える2つの問い

#50 の症状は2つある: **変える場所が無い**、そして段1 が層を増やしたことで
**変えても効かない理由が見えない**が構造的になった (settings.jsonc に書いた
キーは粘る — settings.inc 冒頭コメントが名指ししている帰結)。パネルは両方に
1画面で答える:

1. **変える場所** — application / preference 級の設定を1箇所で見て変えられる。
2. **申告** — 各行が「今の値はどの層が決めているか」を名乗る。段1 が
   file:line で断る規律 (`SettingsReport`) の画面化であり、#50 の
   「設定したのに効かない」を情報に変える。

## 3. 何を見せるか — 行の一覧と数え方 (問い1)

### 3.1 規準と数え方

規準は**判断19 そのまま**:「まっさらな起動のときどうあってほしいか、に答える
ものだけ」。そして数える対象は頭の中の一覧ではなく、**`core/app/settings.inc`
の `SETTING_KEYS` 表**である。段1 がこの表を「applier・綴り提案・テンプレート
生成の3者が同じ行を歩く1つの表」として作り、`--settings-selftest` S1 が
「表に足したのにどれかに足し忘れる」を止めている。パネルは判断19 を**もう一度
適用する場ではなく、適用済みの結果 (= この表) を表示する場**である。

数え方 (inventory §2 / 判断16 の流儀 — 数が合わなくなったら本書ではなく表を
数え直す):

| 何を | どう数えたか | 数 |
|---|---|---|
| 編集できる行 | `SETTING_KEYS` の `SS_Read` 行 | **23** |
| 「まだ」の行 | 同、`SS_Later` 行 | **6** (+ `panels` 節1つ) |
| 「ここではない」の行 | 同、`SS_NotHere` 行 | **2** |

23 の導出 (照合勘定): prefs.txt が書く 27 キー − 履歴 5 (`remoteurl`
`rbookmark` `rbrecent` `readerfor` `window`) = 設定 22。うち `membudget`
`watchfiles` `watchauto` の 3 つは段1 が読んでいない (§10.4) ので 19。
`theme` は theme + accent の 2 行に割れて 20 行。これに settings.jsonc に
しか無い 3 行 (`remote.repoUrl` / `loading.rawRecipes` / `readers.editor`) を
足して **23**。この勘定が S1 の表と合わなくなったら、直すのは本書の数である。

**v1 (段2b) 時点のパネルは 31 行** (23 編集可 + 6 まだ + 2 ここではない)、
セクション見出し 9 + `panels` 節の注 1 行。**段2d/2e 完了後は 33 行**
(26 編集可 + 5 まだ + 2 ここではない)。

### 3.2 行の一覧

ウィジェットは applier (`sjApplyKey`) が受ける型から機械的に決まる:
bool → チェックボックス、列挙 → コンボ、数値 → 入力、文字列 → テキスト、
配列 → **リンク行** (P7、編集しない)。ラベルの下に **キーの path をそのまま**
灰色で出す — パネルの語彙とファイルの語彙が同じ1つであることを、行自身に
言わせる (settings.inc「"grouped" is the word on screen」と同じ判断の逆向き)。

| 行 (キー) | ウィジェット | 既定 | prefs キー | 帰結 |
|---|---|---|---|---|
| appearance.theme | 列挙 dark/light | dark | theme | presentation |
| appearance.accent | 数値 0..accentCount−1 | 0 | theme (2値目) | presentation |
| appearance.compact | bool | **true** | compact | presentation |
| appearance.titleBar | 列挙 system/integrated | integrated | frame | presentation |
| appearance.showFps | bool | false | showfps | presentation |
| appearance.pixelGrid | bool | false | grid | presentation |
| appearance.displayGamma | 列挙 1.0/2.2 | 1.0 | gamma | presentation |
| input.dragPans | bool | false | dragpans | presentation |
| input.wheelZoomPlain | bool | false | wheelzoom | presentation |
| input.fitOnSwitch | bool | false | fitonswitch | presentation |
| *input.keyBindings* | **まだ** (判断14) | — | — | — |
| browse.grouped | bool | true | rbflat (負論理) | presentation |
| browse.tree | bool | false | rbtree | presentation |
| browse.naturalSort | bool | **true** | rbnatural | presentation (F2 の注を行に) |
| *browse.folderActivate* | **まだ** (板 row 136) | — | — | — |
| *browse.sortColumn / sortDescending* | **まだ** ×2 (§3.7) | — | — | — |
| loading.stackPrompt | 列挙 ask/always/never | **ask** | seqload | ★ 間接 measuring (N) |
| loading.rawRecipes | **リンク行** → RAW recipes パネル | [] | — | 不活性 (#166: 選ぶまで効かない) |
| *loading.raw* | **まだ** (判断10、§6) | — | — | measuring |
| remote.peerExecutable | 文字列 | "" | remoteexe | plumbing |
| remote.policy | 列挙 auto/local-fetch | auto | procpolicy | plumbing (同数値が約束) |
| remote.lowBandwidth | bool | false | lowbandwidth | presentation |
| remote.repoUrl | 文字列 | "" (ビルド値) | — | plumbing |
| *remote.pluginPath / pinBudgetMB* | **ここではない** ×2 (判断18: peer の中で効く) | — | — | — |
| readers.pythonExecutable | 文字列 | "" | pythonexe | plumbing |
| readers.searchPath | **リンク行** → Readers パネル | [] | readerplace | plumbing (F7 の注: どのリーダが勝つか = 画素) |
| readers.editor | 文字列 | "" | — | plumbing ($EDITOR との順を行の注に — 判断18) |
| series.defaultUnit | 文字列 | "lx" | linunit | presentation (prefill のみ) |
| *measuring.memoryBudgetGB* | 段2d で編集可に | 0 = 自動 | membudget | **measuring** (§6) |
| (panels 節) | 注1行:「layout.ini との決着待ち」 | — | — | — |
| loading.watchFiles (段2e) | bool | true | watchfiles | plumbing |
| loading.watchAutoReload (段2e) | bool | false | watchauto | plumbing (毎回1行名乗る — File メニューの約束のまま) |

### 3.3 43 個のパネル内トグルからの昇格は 0

判断19 を §3.7 の表のグループごとに当てた結果:

- **パネル開閉 13** → `panels` 節として登録済みだが「まだ」(layout.ini との
  決着が先、settings.inc `sectionIsLater`)。
- **Browse のソート列/向き** → `SS_Later` 登録済み (保存する機構が無い)。
- **残り (Projection の畳み方、Series のフィット法、Compare の diff 系、
  rangeScope …)** → すべて「今この1枚/この比較をどう見るか」= view state。
  設定に置くと次に開いたものにも掛かる、が判断19 の却下理由そのもの。
  measuring 級を含むが、それらはセッションが運ぶ (判断5/11、M2)。

つまり**新顔は無い**。43 のうち設定に立つべきものは、段1 までに全部
`SETTING_KEYS` に (Read か Later として) 立ち終わっている。この確認自体が
棚卸しの検算になっている。

### 3.4 出さないもの

- **履歴5種と window。** preference ではない (軸2)。state.json の段の仕事。
- **CLI 20 のうち表に無いもの** (`--zoom` `--center` `--window-offset`
  `--no-ab-throttle` `--big-endian` ほか raw 一式)。§11 の「決めなかったこと」。
- **環境変数。** `EDITOR` は `readers.editor` の行の注として現れる
  (「未設定のときだけ $EDITOR → code -g → OS」)。残りは判断18 のとおり
  設定ではない。

## 4. 書き込み先 (問い2) — この設計の芯

**決定 (P2): パネルは prefs.txt にだけ書く。** 実装としてはメニューのトグルと
完全に同じ経路 — `app.*` フィールドを書き、副作用ヘルパを呼び、
`app.prefsDirty = true` — であり、settings.jsonc には**1バイトも書かない**。

理由。判断3 が「アプリは settings.jsonc を書かない」を、コメント保全という
機械的理由で既に確定している。パネルは GUI であり、GUI が書く先が
メニューとパネルで違ったら、それこそが「自分の設定はどこにあるか分からない」
の製造になる。答えは3行で言えなければならない:

> 1. **GUI (メニューもパネルも) が書くのは prefs.txt だけ。**
> 2. **settings.jsonc は人だけが書く。書いたキーは粘る (次の起動で勝つ)。**
> 3. **どちらが今この値を決めているかは、パネルの各行が名乗る (§5)。**

**断った代替案2つ** (再訪条件つき):

- **パネルが settings.jsonc を外科的に書き換える** (VS Code 方式)。断る。
  コメントを保ったまま JSONC を書き換える編集器は段1 の読むだけのパーサ
  (判断2) より一桁大きい機械で、バグ1つが利用者のコメントを食う — 判断3 が
  避けたその事故を、別の口から呼び戻す。再訪条件: 利用者が「パネルの変更を
  ファイルに残したい」と繰り返し言い、かつ往復編集器を選ぶ費用を払うと
  決めたとき。
- **パネルは書かず、貼るための JSONC を出すだけ。** 断る。settings.jsonc を
  作らない利用者 (大多数) にとって #50 の「変える場所」が果たされない。
  prefs.txt の道は、ファイルを1つも作らずに今日も動いている道である。
  ただしこの案の良い半分は**採る**: 段2c の「Copy as JSONC」(P3)。

**Copy as JSONC (段2c)。** 各行の右クリックに「Copy as JSONC」を置き、その
キーだけを含む**完全な JSONC 文書**をクリップボードに出す
(例: `{ "appearance": { "theme": "light" } }`)。前例は2つ既にある:
RAW recipes パネルの「Copy as JSONC」(`rawRecipeJsonc`) と
`viewer --settings-template` (どちらも「アプリは書かない、出すだけ、
リダイレクトするのは利用者」)。パネル上部には「Copy template (現在値、
既定と違うものだけ)」— 実体は `settingsTemplateText()` をクリップボードへ —
と「Open settings.jsonc」(readers.editor > $EDITOR > `code -g` > OS の
既存連鎖、`adapter::` 経由) を置く。**prefs → settings.jsonc への移行の路が
パネルから一鍵になる**が、書く手は最後まで利用者の手である。

## 5. 出所の申告 (問い3)

### 5.1 4値のバッジ

各行が右端に1つ、**出所バッジ**を出す (新語はこれ1つ: 段1 の
`SettingsReport` が file:line で名乗る「出所」の画面語。既存語「名指し」は
断りの動詞なので、状態の名詞が要る):

| バッジ | 意味 | 根拠 |
|---|---|---|
| **既定のまま** | どの層も発言せず、値も組み込み既定と同じ | 判断9 の表示形 |
| **このマシン** | prefs.txt / GUI 由来 (値が既定と違う) | — |
| **settings.jsonc:NN** | ファイルが決めている。**粘る** | 段1 §10.2 の帰結 |
| **コマンドライン (--flag)** | この起動だけ | 判断13 |

**File / Cli は発言ベース、Default / このマシン は値ベース (P4)。** 非対称
なのは形式の事実による: settings.jsonc は「書いたキーだけが発言」(判断9) だが、
prefs.txt は `writePrefsTo` が**毎回全キーを書く**ので、キーの存在から
「利用者が選んだ」を読み取れない。だから prefs 層では値比較しか言えることが
無く、それを正直にバッジにする。値比較の基準 (組み込み既定) は新しい表を
作らず **`settingsCurrentValue()` を再利用する** — あれは既に「既定と同じなら
空を返す」形で全既定を1つの switch に持っており、2つ目の既定表は判断16 の
言う「古くなる写し」になる。

付記 (本書が記録する既知の歪み): prefs.txt が全キーを書く形式は、既定が
将来変わったとき既存利用者を旧値に**留める** (板 row 157 の区別が prefs 層では
できない)。これはパネルの欠陥ではなく prefs.txt の欠陥で、直す場所は
state.json の段である。

### 5.2 出所台帳 (段2a の機構)

`SKey` ごとに1エントリの小さな台帳:

```
enum class SetOrigin { None, File, Cli };        // 発言ベースの2層だけ持つ
struct OriginEntry { SetOrigin who; int line; const char* cliFlag; std::string fileVal; };
```

- `loadSettings()` が適用したキーに `File` + 行番号 + 適用値を記す
  (`SjVal` が位置を運んでいるので追加費用はほぼ無い)。
- `parseCli()` の既定を決める 20 フラグのうち表と 1:1 のもの (`--frame`
  `--stack` `--mem-budget` `--remote-exe` `--remote-policy`) が `Cli` を記す。
- Default / このマシン は台帳に**無い**: 描画時に
  `settingsCurrentValue(id).empty()` で判定する (§5.1)。

段1 の `overrode` (prefs と**違った**ときだけの一覧) は残す。台帳と答える
問いが違う —「何が変わったか」と「誰が決めているか」。

### 5.3 粘るキーの、その場の警告

台帳が `File` を持つキーを GUI で変えた瞬間 (パネルでもメニューでも)、
一度だけ名指しで言う:

> settings.jsonc:12 が appearance.compact を持っています — この変更は今回
> 限りで、次の起動ではファイルが勝ちます。恒久にするならファイルのキーを
> 書き換えるか消してください。

段1 は起動時に「上書きした事実を1行」言うが、それは**次の起動**の話で、
トグルが戻る理由を学べるのは再起動後だった。台帳で**戻る前に**言える。
これが段2a 単独の存在価値である (§9)。

### 5.4 既定に戻す

行の右クリックに「Reset to default」。パネルにできるのは**既定値を書く**こと
だけ (prefs.txt は wholesale なので「未設定に戻す」は表現できない — 判断9 の
区別は settings.jsonc だけの財産)。台帳が `File` のキーでは Reset は次の起動で
ファイルに負けるので、その行の Reset は無効化せず、押したら §5.3 と同じ文で
「ファイルのキーを消すのが本当の戻し方」と言う。黙って効かないより、
効かない理由を言うほうがこのリポの流儀である (判断6 と同型)。

## 6. measuring 級 (問い4)

**v1 でパネルに出す measuring 級は `measuring.memoryBudgetGB` の1つだけ (P9)。**

- **M1 (申告)**: membudget の帰結は「上限に当たるとフレームが減り N が変わる」
  であり、その申告は **n of N** が担う — analysis-layers §8 の stack 行が
  「n of N を運ばない結果は報告書に引用できない」と既に定め、判断record 5 で
  裁定済み。つまり membudget は**申告の仕組みが既にある** measuring 設定で、
  「設定ファイルにあるだけで結果に現れない measuring 級を作らない」(M1) に
  抵触しない。段2d の赤テストがこの前提を実測で確かめる (§9 — もし σ_t の
  出る場所で n of N が欠けていたら、それを先に塞ぐのが段2d の第0歩)。
- **M3 (自己記述的既定)**: `0 = 自動` は inventory が ★ を付けたとおり自己
  記述的でない。パネルは**解決後の実効値を行に出す** —「自動 (今この機械では
  19.2 GB) / 使用中 3.1 GB」— File メニューが既にやっている表示の踏襲で、
  既定を少しだけ自己記述的に近づける。
- **M2 (セッションが勝つ)**: membudget はセッションに載らない (載るのは
  結果側の n of N)。パネルは何もしなくてよいが、measuring 節の見出しに
  settings.jsonc の measuring 節と**同じ文言**の3行 (M1/M2/M3) を出す。

**`loading.raw` の 10 キーは出さない。** M3 に真っ向から当たる — 画素の解釈を
黙って決める既定であり、ダイアログが聞く現状 (`ask` の形) のほうが正しい。
`loading.rawRecipes` は対照的に**選ぶまで不活性** (#166 の判断) なので行には
出すが、編集は RAW recipes パネルの仕事 (P7)。

`loading.stackPrompt` は間接 measuring (★) として行に注を出す:
「always/never は聞かずに N を決めます」。

## 7. command palette との関係 (問い5)

§10.8 の前提は「メニューを1テーブルから生成し、palette は同じテーブルを引く。
手で二重登録すると必ず片方が腐る」。これに従い、かつ**同じ表には入れない**:

**決定 (P10): 表は「種類ごとに1つ」。設定は `SETTING_KEYS`、動詞は (将来の)
コマンド表。二重登録の禁止はそれぞれの種の中で効く。**

- 設定の行は「path・型・範囲・適用先・既定」を持つ。動詞の行は「名前・
  有効条件・実行・ショートカット」を持つ。**行の形が違う2種を1つの万能表に
  畳むと、全行が両方の欄を持たされ、表そのものが腐る** — 二重登録を避けよう
  として、腐る場所を表の内側に移すだけである。
- **Preferences パネルは `SETTING_KEYS` の4番目の消費者になる (P1)**:
  applier / 綴り提案 / テンプレート生成 / パネル。S1 型の selftest が
  「表に足したのにパネルに出ない」を止める (§9 段2b)。
- palette が来たら: Ctrl+Shift+P の動詞はコマンド表から、そして
  「Preferences: appearance.compact …」型の行は **`SETTING_KEYS` を直接
  引いて**出す。写しは作らない。
- コマンド表の動詞が設定をトグルするとき (View メニューの8トグルが将来
  コマンド表から描かれるとき) は、**値と適用を `SKey` で参照する** — 動詞表は
  値を持たない。join キーは `SKey`。
- メニューとパネルが同じ設定を2箇所に**見せる**のは表示の二重であって登録の
  二重ではない。腐るのは登録 (真実が2つあること) で、真実は表1つ +
  `app.*` フィールド1つのまま動かない。

## 8. パネルの形

- **1ウィンドウ「Preferences」、非モーダル。** 変えた結果を後ろの画面で見ながら
  触れること (gamma、compact) が settings ダイアログをモーダルにしない理由。
- **開き方は File > Preferences... のみ (P12)。** File が application 級の
  動詞の家 (watch のトグルも既にそこ)。**ショートカットは v1 で付けない** —
  キーの割り当て表が無い (§3.8) のに直書きショートカットを増やすのは、
  判断14 が塞ごうとしている穴を深くする。palette が来ればそこから引ける。
- **上からフィルタ箱、次にセクション見出し (appearance … measuring の9つ、
  `SETTING_SECTIONS` の順)、行。** フィルタは path とラベルと注に当たる。
  読み順は「探す → 見つける → 変える → 出所を確かめる」で、行の中も
  ラベル → ウィジェット → バッジ の順。
- **行の3点セット**: ウィジェット (現在値)、キー path (灰色)、出所バッジ。
  「まだ」「ここではない」の行はウィジェットの代わりに `SKeyDef::note` の
  文をそのまま出す — テンプレートが出す文と同じ1つの真実。
- **副作用はヘルパ経由で。** theme/accent/compact → `ui_theme::apply`、
  displayGamma → `markAllTexDirty`、titleBar → `window_frame::setMode`。
  メニューが呼んでいるのと同じヘルパを呼ぶ (ヘルパが共有物。メニューの
  inline コードの整理は本書の外)。
- **パネルの開閉はセッションに載せない (P13)。** これは workspace の一部では
  なく設定を触るための chrome で、`panels` 行に混ぜると「まっさらな起動」の
  議論 (§10.7) に無関係な住人が増える。

## 9. 段階分け (問い6) — 各段が単独で main に入り、単独で意味を持つ

すべての selftest は L5 の前例に従い**子プロセス + 使い捨て config dir**
(HOME/APPDATA を差し替え) で走る。selftest の home は全テスト共有なので、
prefs.txt を書くテストが共有 home を汚してはならない。

### 段2a — 出所台帳と、その場の警告

- 入るもの: `OriginEntry g_settingsOrigin[]` (§5.2)、`loadSettings` /
  `parseCli` の記帳、GUI トグル時の §5.3 警告 (メニューにも効く)。UI 新設なし。
- **赤**: `--settings-selftest` に P 系列を足す。子プロセスに
  prefs.txt `compact 0`、settings.jsonc `{"appearance":{"showFps":true}}`、
  CLI `--frame system` を与え、台帳を吐かせて assert:
  - compact → バッジ「このマシン」。導出: 既定 true (§3.1)、prefs が false、
    file/CLI は沈黙 → 発言層なし + 値≠既定。
  - showFps → File, 行番号はファイル内で書いた行。導出: file が名指しで
    書いた唯一のキー。
  - titleBar → Cli (`--frame`)。導出: 判断13、右端が勝つ。
  - dragPans → 既定のまま。導出: どの層も発言せず
    `settingsCurrentValue` が空。
- **単独の意味**: 粘るキーの警告が「次の起動後」から「変えた瞬間」に前倒しに
  なる。パネルが無くてもメニューで既に効く。

### 段2b — パネル本体

- 入るもの: `core/ui/panel_preferences.inc` (描画) と、settings.inc 側の
  **行モデル builder** (純関数: `SETTING_KEYS` を歩いて
  行 {def, widget種, 現在値文字列, バッジ} を返す。ImGui に触れないので
  headless で試験できる)。File > Preferences... のメニュー項目。
- **赤**: 新設 `--prefspanel-selftest` (ctest 登録)。
  - 行数 == `SETTING_KEY_COUNT` + panels 注 1。**構造 assert** (表の長さと
    比較し、リテラル 31 を焼き込まない — 判断16)。傍らに手導出のスポット
    チェック: `SS_Read` の行数 == 23 (導出は §3.1 の照合勘定を assert の
    隣に書き写す)。
  - 全 `SS_Read` 行に widget 種がある (S1 と同型:「表に足してパネルに
    足し忘れ」で赤になる)。
  - スポット: displayGamma の widget は2値列挙 (導出: applier が
    1.0/2.2 以外を名指しで断る、範囲ではない)。searchPath / rawRecipes は
    リンク行 (P7)。
  - 台帳 File のキーの行がバッジ文字列に「settings.jsonc:」を含む。
- **単独の意味**: #50 の「1箇所に集める」がここで果たされる (inventory §6 の 3)。

### 段2c — Copy as JSONC / Copy template / Open settings.jsonc

- 入るもの: 行の右クリック「Copy as JSONC」「Reset to default」、パネル上部の
  「Copy template」(= `settingsTemplateText()` → clipboard) と
  「Open settings.jsonc」(adapter の editor 連鎖)。
- **赤**: `--prefspanel-selftest` に往復を足す。theme を light にした状態の
  行コピー文字列を `sjParseDocument` → `sjApplyDocument` に通し、
  applied == 1 かつ themeVariant == Light。導出: コピーは段1 パーサの逆写像
  として書くので、合成は恒等になるはず — 文字列比較ではなく往復で assert
  する (`--rawrecipe-selftest` の前例)。
- **単独の意味**: prefs → settings.jsonc の移行が、CLI (`--settings-template`)
  を知らない利用者にも開く。

### 段2d — measuring 節を開く (memoryBudgetGB)

- 第0歩 (前提の実測): σ_t / σ_fpn が印字される場所 (Temporal、Set Analysis)
  で、budget により切り詰められた stack が **n of N** を運ぶことを確かめる
  赤テスト。導出: membudget の帰結はフレーム数、申告は判断record 5 の
  n of N。**欠けていたらそれを塞ぐまでこの段は進まない** (M1 を初日に
  破らないため — §10.7 が M1 を先にした理由と同じ)。
- 入るもの: `measuring.memoryBudgetGB` を `SS_Later` → `SS_Read`
  (値域は 0 または 0.5..4096 — メニューのクランプと同じ数字。範囲外は
  黙ってクランプせず名指しで断る: gamma の前例)。パネルの measuring 節に
  行 + 実効値表示 + M1/M2/M3 の3行見出し。
- **赤**: settings 系列に追加 — jsonc `{"measuring":{"memoryBudgetGB": 8}}` で
  `app.memBudgetGB == 8` (導出: 層の順 §10.2、prefs より file が勝つ)。
  `-1` は rejected 1 / 値不変 (導出: 判断8、キー単位で断る)。
- **単独の意味**: measuring 節が初めて読まれ、判断10/11 の形が実物になる。

### 段2e — watch の2キー (要裁定 P11)

- 入るもの: `loading.watchFiles` / `loading.watchAutoReload` を
  `SETTING_KEYS` に `SS_Read` で追加し、パネルの loading 節に行を足す。
- **赤**: jsonc `{"loading":{"watchFiles": false}}` で
  `app.watchEnabled == false`、台帳 File、未知キーの注が**出ない**こと
  (導出: 表に載った瞬間、判断6 の「知らないキー」経路から外れる)。
- **単独の意味**: prefs 22 設定のうちパネル/ファイルに届かない最後の2つが
  届く (§3.1 の照合勘定が 26 で閉じる)。

依存: 2a → 2b → 2c。2d と 2e は 2b の後なら順不同。各段とも既存の
`--settings-selftest` 77 本は素通りすること (段1 の挙動を変えない)。

## 10. 判断リスト

1. **P1 — パネルは `SETTING_KEYS` から生成する (4番目の消費者)。** 別の一覧を
   持てば S1 が守ってきた「1つの表」が破れ、足し忘れの口が開く。
2. **P2 — パネルの書き込み先は prefs.txt のみ。settings.jsonc には書かない。**
   判断3 の継承 + GUI の書き先が2つあれば「自分の設定はどこ」が壊れる (§4)。
3. **P3 — Copy as JSONC はクリップボードのみ。** `rawRecipeJsonc` と
   `--settings-template` の前例どおり、アプリは出すだけ、書くのは利用者。
4. **P4 — 出所は4バッジ。File/Cli は発言ベース、Default/このマシン は
   値ベース。** prefs.txt が全キーを書く形式である以上、prefs 層に発言の
   概念が無い。既定判定は `settingsCurrentValue()` を再利用し、既定表を
   2つにしない。
5. **P5 — File 持ちのキーも編集は許し、その場で「今回限り」と名指しする。**
   無効化は「設定したのに変えられない」という別の症状を作る。
6. **P6 — パネルの Reset は「既定値を書く」。** prefs.txt は未設定を表現
   できない。File 持ちの行では本当の戻し方 (キーを消す) を文で言う。
7. **P7 — 配列 (searchPath / rawRecipes) はリンク行。二重編集器を作らない。**
   同じリストに編集器が2つあれば、それが登録の二重である。
8. **P8 — `SS_Later` / `SS_NotHere` も行として出す。** 「まだ」と
   「ここではない」は違う答えで、どちらも黙って省かない — テンプレートと
   同じ文 (`SKeyDef::note`) を同じ表から出す。
9. **P9 — v1 の measuring 級は memoryBudgetGB のみ。申告は n of N が担う。
   `loading.raw` は出さない (M3)。** §6。
10. **P10 — 設定表とコマンド表は別。join は `SKey`。万能表にしない。** §7。
11. **P11 — `watchfiles` / `watchauto` を `loading.watchFiles` /
    `loading.watchAutoReload` として昇格する。【要裁定】** 棚卸しが
    数え漏らし裁定を受けていない (§10.4) ので、ここで裁定を仰ぐ。推奨は昇格:
    どちらも「まっさらな起動のときどうあってほしいか」に答え (判断19)、
    auto-reload は動くたび1行名乗る (申告あり) ので measuring 扱いは不要。
    節は loading (「いつ読み直すか」は読み込みの一部)。
12. **P12 — v1 のパネルにショートカットを付けない。File > Preferences...
    のみ。** 割り当て表の設計 (判断14) を待つ。直書きショートカットの追加は
    §3.8 の穴を深くする。
13. **P13 — パネルの開閉はセッションに載せない。** chrome であって
    workspace ではない (§8)。

## 11. 決めなかったこと (v1 で断る + 再訪条件)

- **`--zoom` `--center` `--window-offset` `--no-ab-throttle` と raw 系 CLI の
  設定キー化。** 誰も「起動のたびに毎回渡している」と言っていない。再訪条件:
  その声が出たとき、1キーずつ判断19 に掛ける。
- **`panels` 節を読む。** layout.ini との決着 (まっさらな起動のとき、開閉の
  真実はどちらか) が先 (§10.7)。パネルには注1行だけ出す。
- **Blink の起動時指定** (§3.7 の名指し1つ目)。要望の記録が無い。再訪条件:
  要望が出たら `--compare blink` の1語追加が先で、設定キーはその後。
- **settings.jsonc を書く編集器。** §4 で断った。再訪条件も §4。
- **履歴 (bookmarks / recent / readerfor / window) の表示。** state.json の
  段の入口で「見せるか」ごと再訪。
- **メニューのトグルをコマンド表から生成する再編。** §10.8 の順序
  (テーブル化 → メニュー → palette) の仕事で、パネルはそれを待たない。
  join キー (`SKey`) だけ本書が予約した (P10)。

## 12. 数の扱い

本書の数 (23 / 31 / 33 / 27−5=22 …) はすべて §3.1 の数え方とセットである。
合わなくなったら直すのは数ではなく、`SETTING_KEYS` を数え直すこと (判断16)。
パネル自体が表から生成される (P1) ので、実装後は「パネルの行数」がこの数の
生きた写しになり、`--prefspanel-selftest` の構造 assert が写しの腐りを
検出する。
