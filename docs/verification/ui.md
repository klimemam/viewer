# UI 検証仕様

本書は、現在の `main` に対して繰り返し実行する UI 検証の仕様である。特定のコミットに
対する結果、PASS/FAIL の集計、実行環境固有の測定値は [results/](results/) に置き、本書には
手順、期待する状態、観測方法だけを置く。

UI 検証も [機能検証仕様](functional.md) と同じ CMake の合否判定（gate）に含まれる。
登録名、引数、テストデータ（fixture）、OpenGL の要否の正典は
[`CMakeLists.txt`](../../CMakeLists.txt) である。

## 1. 検証の境界

自動検証は、次のいずれかを製品の実経路から観測して判定する。

- 実際の ImGui フレームにマウス・キーボード操作を注入した後の状態
- ImGui が実際に配置した項目やパネルの矩形と、描画した文字列
- メニュー、バッジ、フッター、ESC の処理先などを記録する専用 probe（観測点）
- UI と同じ整形・構築処理が作った表、ツールチップ、状態表示、export の文字列

画面を撮れない環境で、色、線種、濃さ、重なりの見た目を「確認した」としてはならない。
状態が正しくても見た目の品質は別の事実なので、§6 の実機確認に分ける。

## 2. 実行方法

全 UI 項目を含む標準の実行入口は、次のとおりである。

```bash
tools/run_selftests.sh build
```

OpenGL コンテキストがない実行機では、CMake で `NOGL` と宣言された項目だけが走る。
標準入口が出す skip の名前と理由を記録し、skip を PASS に数えない。全 UI の受入れは、
GUI テストを含めて `skipped=0` となる検証実施（run）で判定する。

単独調査は、標準入口でビルドとテストデータを更新した後、次の形で行う。

```bash
ctest --test-dir build -C Release -R '^selftest\.<name>$' -V
```

`tools/verify/run_ui_matrix.sh` は、本書の A/B 項目を項番別に再生して失敗箇所を
絞り込む補助ドライバーである。CMake の全登録を列挙するものではないため、
`tools/run_selftests.sh` による現行の合否判定の代用にはしない。

## 3. UI 自動操作の契約

| 観測面 | 契約 |
|---|---|
| `--browse-keys-selftest` | 非表示ウィンドウの実フレームと実入力キューを使う。既定の action 列は `core/app/cli.inc` が正典で、`--browse-keys` は列全体を置換する |
| Browse 操作の状態表明 | `chk*` は対象の実状態を読み、1件でも不一致なら run を非0で終える。行番号（index）だけに依存せず、`chkatrow:<name>` などで名前と位置を確かめてから操作する |
| `chkctx` / `ctxclick` | 開いている実メニューから項目名・件数を読み、項目自身の矩形を実際にクリックする。背後の関数を直接呼ぶ検査で代用しない |
| `g_filesBadgeProbe` | Files が最後に描いた A/B/追加スロットの文字と淡色状態を記録する。色を再計算せず、描画されたバッジの状態を読む |
| `g_footerProbe` | canvas のフッターが実際に描いた `ctx / zoom / name / count / previewcount` を1フレーム分記録する。値を別実装で再計算しない |
| `g_escProbe` | 1回の ESC を消費した層を `popup / textedit / roi / compare / nothing` のいずれかで、押下順に1件だけ記録する |
| layout probe（配置観測） | item / plot / pane の矩形を ImGui から読み返し、親ウィンドウの範囲、重なり、必要なバッジが残っていることを数値で判定する |

### `--browse-keys` action language

UI を実フレーム経由で駆動できる入口は `--browse-keys-selftest <dir>` である。
`--browse-keys "<act,act,…>"` は既定の action 列全体を置き換える。完全な語彙と
意味の正典は [core/app/cli.inc](../../core/app/cli.inc) とし、ここには再実行時に必要な
系統を列挙する。

- キー: `down` `up` `left` `right` `enter` `home` `end` `back` `esc` `comma` `period`
- マウス: `click` `ctrlclick` `dbl` `clickoff:N` `dbloff:N` `idle:N` `chevclick`
  `mback` `mfwd` `altleft` `altright` `fmenu` `rctx` `rctxcur` `ctxclick:LABEL`
- 状態: `viewreset` `w<px>` `h<px>` `focus` `blur` `flat` `tree` `disc`。
  撤去済みの drawer と同じく `more` action も存在しない
- instance: `target:N` `newpanel` `reconnect` `closep` `hidep` `showp` `filt:S` `sessrt`
- 待機: `waitimg:N` `waitdir:LEAF`
- 主な assert: `chkimg:N` `chkpv:N` `chkidx:K` `chkopen:S` `chkcur`
  `chkcurn:NAME` `chknames:SPEC` `chkdir:LEAF` `chkcursor:N` `chkatrow:NAME`
  `chkback:N` `chkfwd:N` `chkexp:N` `exparm` `chkexpn:0` `chkfocus:0|1`
  `chkfilt:S` `chkpanels:N` `chkshown:N` `chksel:N` `chkctx:N`
  `chkctx:+LABEL` `chkctx:-LABEL` `rdrshut` `chkrdr:NAME`

### Script template and calibration

自作スクリプトは次の定型を前後に置く。

```text
PROLOG  waitdir:<leaf>,viewreset,w400,home
EPILOG  <6個以上のnavキー>,blur,down,up,end,home,rawopen,popupcheck,seqask,popupcheck
```

- **PROLOG が要る理由**: `viewreset` は grouped + list + folded + natural-name の
  絶対ピンである。`w400` はパネルを固定幅で float させ、注入クリックが
  利用者の保存レイアウト次第の座標に落ちるのを防ぐ。**重なり順も `w400` が固定する**
  (#206): 矩形を決めても、そこに描かれているのが別の窓なら注入クリックはそちらに
  落ちる —— layout.ini の無い config では Browse は docked で始まり、`w400` の
  undock だけでは既定で開いている ROIs / Analysis の下のままだった。
- **EPILOG が要る理由**: 終了判定は
  `keysOk = routeOk && popOk && watchOff && keysCheckBad == 0`
  ([core/main.cpp](../../core/main.cpp)) である。`routeOk` / `popOk` / `watchOff` を
  観測する action を省くと、個々の assert が通っても run は非0で終わる。

校正は、製品コードを変更・再ビルドせず、同じ20 actionの期待行名だけを変えて行う。

```bash
bash tools/verify/calibrate_browse_keys.sh <checkout>
```

```text
### 1. DELIBERATELY WRONG expectation (chkatrow:expset) - must FAIL
browsekeys: chkatrow:expset    -> … cur=-  cursor row=digitset: FAIL
browsekeys: 20 action(s) through real frames, no crash, 1 panel check(s) failed: FAILED
rc=1

### 2. CORRECT expectation (chkatrow:digitset) - must PASS
browsekeys: chkatrow:digitset  -> … cur=-  cursor row=digitset: ok
browsekeys: 20 action(s) through real frames, no crash, 0 panel check(s) failed: ok
rc=0
```

スクリプト実行（scripted run）は、CMake がテストごとの設定用・一時用ディレクトリに
隔離する。さらに製品側も、scripted run では利用者の `layout.ini` と autosave を
読み書きしない。この二重の隔離を外した直接実行は、標準の判定に使わない。

[2026-08-03 の結果](results/20260803-ui.md) にある「利用者の `layout.ini` と
`autosave.vsession` を書く」は当時の欠陥記録であり、現在の挙動ではない。後続実装では、
設定パスを決める前に `scriptedRun` を判定してレイアウトの読み書きを止め、周期保存と
終了時保存も `g_browseKeys` の実行中は止めた。CMake によるテストごとの隔離は、
製品側の防止策とは独立した検証環境の保護として残す。

## 4. 現行 UI 回帰項目

「手順」にある selftest 名は `selftest.<name>` を表す。本書は再実行用の仕様なので、
実施結果の欄は意図的に持たない。

| 項番 | 検証項目 | 手順 | 期待する観測状態 |
|---|---|---|---|
| A1 | クリックで選択し、ダブルクリックで確定する | `browse-keys` のフォルダ行 `click` / `dbl` | 1回のクリックでは表示ディレクトリが変わらず、カーソルだけが移る。ダブルクリックすると、そのフォルダへ1回だけ移動する |
| A2 | フォルダをダブルクリックする | `browse-keys` の `dbl,waitdir:<leaf>` | 対象ディレクトリへ移動し、履歴が1段だけ増える |
| A3 | `..` で親へ戻る | `browse-keys` の `home` / `up` / `chkatrow:..` | ルート以外の一覧では先頭に `..` があり、選択・確定の規則は通常行と同じである |
| A4 | シェブロンの判定領域と名前領域を分ける | `browse-keys` の `exparm,dbl,chkexpn:0` | 名前をダブルクリックしている間、ツリーの展開を一瞬も発火させない |
| A5 | シェブロンで展開・折り畳みを切り替える | `browse-keys` の `chevclick,chkexp:1,chevclick,chkexp:0` | クリックするたびに、展開と折り畳みが直ちに反転する |
| A6 | 名前のクリックでは選択だけを行う | `browse-keys` の `click,chkexp:0,chkdir:<leaf>` | 展開もディレクトリ移動も起こさず、カーソルだけを移す |
| A7 | 複数選択した対象を開く | `browse-keys` の `ctrlclick` 群と `enter` | 選択した group ごとに stack を一つ開き、member の欠落や重複を作らない |
| A8 | stack のダブルクリックで一度だけ開く | `browse-keys` の `dbl,waitimg,chkopen,chknames` | stack を一度だけ開き、一時プレビューを本来の stack と重複して残さない |
| A9 | 戻る・進む | `browse-keys` のマウスの戻る・進むボタンと Alt+Left/Right | どちらの入力も同じ履歴を動かし、新しく移動した場合は「進む」側の分岐を切る |
| A10 | プレビュー内を前後に送る | `browse-keys` の `chkpv,comma,period,chkidx` | プレビューは1スロットのまま、`,` / `.` で member index だけが移る |
| A11 | server-temporal 実行中もフォーカスを保つ | `browse-keys` の `svtemp,chkfocus,period` | 要求後も Browse がキーボードフォーカスを保持し、プレビューの前後送りが働く |
| A12 | キーボード入力の送り先を切り替える | `browse-keys` の focus / blur と矢印キー | パネルにフォーカスがある間は main view が移動キーを受け取らず、フォーカスを外すと main view に戻る |
| A13 | 比較スロットの表示位置（seat） | `abstats` / `abeq` | 比較中は A/B/追加スロットをそれぞれ描く。同じ document を複数スロットに置いても、片側を隠さない |
| A14 | ESC で一層ずつ抜ける | `abstats` の ESC chain | ROI → compare → nothing の順に、1回の押下で1層だけ抜ける。B pin とスロットの seat は残る |
| A14b | popup が ESC を最初に受け取る | `browse-keys` の `rctx,esc,fmenu,esc` | popup が1回で閉じ、同じ押下を ROI / compare に渡さない |
| A15 | 比較を切った後も seat を残す | `abstats` の Files badge probe | 比較を切った後も B/追加スロットを淡色で残し、カーソルを A の seat として残さない |
| A16 | 文脈のないメニュー項目を出さない | `abstats` の row menu 表明 | 比較相手がない現在行では、無効な Swap を灰色表示せず、実行可能な動作だけを出す |
| A17 | 更新待ち側の stale 状態 | `abstats` の step / throttle 表明 | 更新待ちの側は直前の結果を保持し、stale 状態を明示する。新しい結果が届いたら同じスロットを更新する |
| A18 | Temporal の配置 | `abstats` | A/B/追加スロットの曲線と表がパネル内に収まり、軸を持たない側は理由を表示する |
| A19 | x 軸の値を貼り付ける | `export-tsv` | コンマ、空白、タブ、改行、CRLF を区切りとして扱い、数値でない値は位置を示して拒否する |
| A20 | リニアリティを Series Analysis に配置する | `seriespanel` | frame / stack member と x 軸、fit の範囲・方法、plane ごとの曲線を Series Analysis 層に表示し、旧 frame-lin panel を復活させない |
| A21 | ROI montage の表示名 | `export` | H/V と per-frame range の有無を名前に含め、元の stack と結果を区別できる |
| A22 | tile ごとの同一性を表示する | `tile` | 各表示領域（pane）は、スロット文字と batch / stack の identity（識別情報）を優先する。幅が狭くてもバッジを残し、名前は領域内で省略する。フッターには実際の zoom と `n/N` を表示する |
| A23 | Browse instance を独立させる | `browse-keys` の `newpanel` / `target` | 複数パネルのディレクトリ、絞り込み、選択、履歴、フォーカスが互いに独立する |
| A24 | 新しいウィンドウへ引数を渡す | `newwin` | stack、CFA、remote、secondary window の引数列（argv）を、意味を失わない順で組み立てる。実際の配置は §6 で確認する |
| A25 | root popup の競合を防ぐ | `browse-keys` の `rawopen,popupcheck,seqask,popupcheck` | RAW dialog が別の root modal 要求で消えず、所有中の queue と結び付いたまま残る |
| A26 | Browse の「Open with reader...」 | `browse-keys` の `rctxcur,chkctx,ctxclick,chkrdr` | native file 行は6項目で、この項目を含む。group 行は5項目、folder 行は4項目で、この項目を含まない。native では読めない file 行は4項目で、この項目を含む。項目をクリックすると、そのパスを持つ Reader panel が開く |

## 5. 追加の現行 UI 契約

| 項番 | 検証項目 | 手順 | 期待する観測状態 |
|---|---|---|---|
| B1 | 遅い peer 上でのダブルクリック | `browse-dbl` | 1回目のクリックによるプレビュー取得がダブルクリックの判定時間より長くても、ファイルのダブルクリックでは1回だけ開く。時間を空けた二つのクリックをダブルクリックと誤認せず、フォルダは1回だけ移動する |
| B2 | 同じ画素を共有していることの説明 | `verify` / `abeq` | A と他のスロットが同じ `source` を描くときだけ、表示チップが `share the same pixels` と説明する。同じ document 同士の比較でも、通常どおり両側を描く |
| B3 | Files で共有関係を説明する | `verify` | frame 行は共有相手をすべて示し、stack 行は相手の stack ごとの共有枚数を示す。最初の保持者に行全体の枚数を誤って帰属させない |
| B4 | histogram で値域を選択する | `histhl` | x 軸で選んだ値域と、色を付けた pixel 数が bin の合計と一致する。端の bin の折り畳みとグリッド外を取り違えない |
| B5 | ROI 表を文書として出力する | `roistats` / `roi-export` | パネルの数値と Copy / Save で得る文字列が、同じ値・単位・矩形を持つ。比較の反対側の値を混ぜない |
| B6 | reload / stale / failure を区別して表示する | `reload` / `stackavg` | `(stale)` と `(reload failed)` を、対象と解消方法が異なる状態として表示する。成功条件を満たす操作が行われるまで、黙って消さない |
| B7 | 設定 UI と保存値を一致させる | `settings` | default < prefs < settings.jsonc < CLI の優先順を保つ。不正な file / key は理由付きで拒否し、画面上の値と保存値を取り違えない |
| B8 | Browse の行モデルと picker | `browse` / `localbrowse` / `picker` | grouped / flat、local / peer、file / group / folder という各分類の行 identity と選択対象を混同せず、picker の操作を実際の行に結び付ける |
| B9 | 表示範囲と false color | `range` / `test_display_falsecolor` | 表示範囲の状態遷移と false-color plugin の出力が、定義済みの値と境界を保つ |
| B10 | benchmark 中の描画対象を確認する | `benchcov` | 開いた対象パネルが計測フレーム中に実際に描画される。隠れたタブを測定したことにしない |

## 6. 実機確認

次の項目は、状態を記録する probe だけでは品質を判定できない。実機で確認し、結果ファイルに
OS、ディスプレイ、表示倍率、操作、観測結果を記録する。

| 項番 | 見るもの | 操作 | 合格の目安 |
|---|---|---|---|
| M1 | 破線 | Temporal / リニアリティの基準線を表示する | 実線のように潰れず、点の粗さが読み取りを妨げない |
| M2 | マーカーと琥珀色 | compare の警告を表示する | 他の系列色や赤・黄と区別できる |
| M3 | 淡色表示 | ESC で compare を抜け、Files の B/追加 seat を見る | 「準備済みだが非アクティブ」と読め、消失や有効状態と誤認しない |
| M4 | グラフの box-zoom | Temporal / Histogram で範囲をドラッグする | 選んだ範囲に追従し、解除方法が一貫する |
| M5 | montage | ROI montage を H/V、共通 range / per-frame range で作る | frame の境界が分かり、意図しない飽和や黒潰れがない |
| M6 | 新しいウィンドウの配置 | 通常環境、複数モニター、異なる表示倍率で新しいウィンドウを開く | 画面内に現れ、親ウィンドウと完全には重ならず、操作できる |
| M7 | 狭いパネル | Browse / Files / analysis の各パネルを狭める | 省略後も行とボタンの意味が残り、押下領域が失われない |
| M8 | 共有マークとツールチップ | 同じ `source` を持つ複数の stack に Files でポインターを合わせる | グリフが欠落文字にならず、ツールチップが共有相手と枚数を読みやすく示す |
| M9 | テキスト編集中の ESC | フィルター、名前、数値入力を編集中に ESC を押す | 最初の1回はテキスト編集だけを抜け、popup / ROI / compare まで同時に進まない |

## 7. 要 probe として扱う場合

次の状態を新しい受入条件にするときは、描画関数の存在やコード経路を目で追っただけで
PASS にしない。製品が実際に描画または発行した値を保持する probe を先に追加する。

- 新しいコンテキストメニューの項目名、有効・無効の状態、その理由
- toast / Messages の本文と重大度（severity）
- ツールチップが実際の項目に結び付いたこと
- 新しい ESC の層（layer）が入力を消費する順序
- 色、線幅、線種など、状態値だけでは見た目を保証できない属性

probe がない場合は `要 probe`、見た目そのものを判定する場合は `実機確認` と記録する。

> **C4 の現行注記:** [2026-08-03 の結果](results/20260803-ui.md) にある
> `A = B` / `paused` との併存は、当時の未実施期待であり、2026-08-04 に撤去済みである。
> 現行契約は A13 / B2。凍結した過去結果そのものは書き換えない。

## 8. 保存済みの実施記録

- [2026-08-03 ステージ1 UI 検証](results/20260803-ui.md)
- [2026-08-04 UI probe 検証](results/20260804-ui-probes.md)
- [2026-08-17 Browse「Open with reader...」UI 検証](results/20260817-open-with-reader-ui.md)
