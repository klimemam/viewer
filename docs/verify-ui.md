# UI 検証項目 (verify-ui)

対象: main @ e3883f5 (frame-reference refactor の stage 1 マージ直後)。
stage 1 は `FrameSource` を `ImageDoc` の下に敷いただけで **UI は無変更のはず** —
本表 A/B 節はその「無変更」を状態で証明するためのもの。

## この機械でできること・できないこと

この環境は **OpenGL の画面を撮れない** (CopyFromScreen / BitBlt / PrintWindow は
クライアント領域が白で返る)。したがって UI 検証とは:

- アプリ自身の selftest / automation フラグで **合成クリック・合成キー** を流し、
- 結果の **状態** (stderr のトレース、保存された session/prefs、selftest の assert)
  を突き合わせること。

ピクセル・色・破線そのものに関する項目は D節「実機確認のみ」に隔離してある。
**それらを「見た」と書いてはならない。**

## 自動化の口 (automation surface)

`core/main.cpp` の `parseCli()` が持つ selftest フラグは 22 個
(`--*-selftest`)。うち 21 個はオーケストレータのスイートが回している
(`--sweepfile-selftest` のみ未使用)。

UI を **実フレーム経由で** 駆動できるのは 1 本だけ:

| 口 | 実体 | 何ができるか |
|---|---|---|
| `--browse-keys-selftest <dir>` | main.cpp:27739 / 28632 / 29102 | 隠しウィンドウに実フレームを回し、**実入力キューに** マウス/キーを注入する。人間の操作と区別できない |
| `--browse-keys "<act,act,…>"` | main.cpp:19962 | 上の **既定アクション列を丸ごと差し替える**。product code を触らずに検証を追加/校正できる唯一のレバー |

他の selftest は `<dir>` 以外の引数を **一切パースしない** (副引数は存在しない)。
つまり新しい UI 検証を CLI から書けるのは `--browse-keys` の言語だけ。

### `--browse-keys` アクション言語 (抜粋・実測済み)

- キー: `down` `up` `left` `right` `enter` `home` `end` `back` `esc` `comma` `period`
- マウス: `click` (カーソル行の中心 +40px)、`dbl` (本物のダブルクリック)、
  `ctrlclick`、`chevclick` (chevron 座標を直接叩く)、`mback`/`mfwd` (ボタン 3/4)、
  `altleft`/`altright`、`fmenu`、`rctx`
- 状態ピン: `viewreset` (grouped+list+folded の**絶対**ピン)、`w<px>` (パネルを
  その幅で float)、`focus` / `blur`、`more` / `flat` / `tree`
- インスタンス: `target:N` `newpanel` `reconnect` `closep` `hidep` `showp` `filt:S` `sessrt`
- 待ち: `waitimg:N` `waitdir:LEAF` (各 60 秒)
- assert: `chkimg:N` `chkpv:N` `chkidx:K` `chkopen:S` `chkcurn:NAME` `chknames:SPEC`
  `chkdir:LEAF` `chkcursor:N` `chkatrow:NAME` `chkback:N` `chkfwd:N` `chkexp:N`
  `exparm`/`chkexpn:0` `chkfocus:0|1` `chkfilt:S` `chkpanels:N` `chkshown:N` `chksel:N`

### 自作スクリプトの定型 (C節はこれを前提に書いてある)

```
PROLOG  waitdir:<leaf>,viewreset,w400,home
EPILOG  <6個以上のnavキー>,blur,down,up,end,home,rawopen,popupcheck,seqask,popupcheck
```

- **PROLOG が要る理由**: selftest は **利用者の生の状態を継承する**。起動時に
  flat/tree/advanced は落とされる (main.cpp:27750) が、`viewreset` は列の途中で
  効かせられる唯一の絶対ピン。`w400` はパネルを固定幅で float させ、注入クリックが
  利用者の保存レイアウト次第の座標に落ちるのを防ぐ。
- **EPILOG が要る理由**: 終了コードは
  `keysOk = routeOk && popOk && keysCheckBad == 0` (main.cpp:29132)。
  `routeOk` / `popOk` は `blur` と `popupcheck` だけが立てるカウンタを読む。
  EPILOG を省くと **全 assert が通っても rc=1**
  (`FAILED (the action list did not finish)`) になる。実測済み。

> **警告 (defect D-1 参照)**: `--browse-keys-selftest` は利用者の実
> `%APPDATA%/viewer/layout.ini` と `autosave.vsession` を **書き換える**。
> 検証は必ず `APPDATA` を捨てディレクトリに向けて回すこと。
> `tools/verify/*.sh` はそうしてある。

---

## A. 現行 main の UI 回帰 — 今すぐ自動実行できるもの

実行: `bash tools/verify/run_ui_matrix.sh <checkout>` (APPDATA 隔離済み)。
判定はすべて **本日 main @ e3883f5 で実測**。

| 項番 | 検証項目 | 操作手順 | 期待 (観測可能な状態) | 実施結果 | 判定 |
|---|---|---|---|---|---|
| A1 | Files/Browse: クリック=選択、ダブルクリック=確定 の分離 (利用者命名の原則) | `--browse-keys-selftest tools/testdata/rb` 既定列の `click,chkdir:rb,chkatrow:digitset` 区間 | フォルダ行を single click してもディレクトリは変わらず (`chkdir:rb`)、カーソルだけがその行に載る | `chkdir:rb -> … dir=tools/testdata/rb: ok` / `chkatrow:digitset -> … cursor row=digitset: ok` | PASS |
| A2 | フォルダ行のダブルクリックは降りる | 同上 `dbl,waitdir:digitset` | 降下先が `digitset` になる | `dbl` 後 `waitdir:digitset` 成立、後続 `chkback:5 -> back=5 fwd=0: ok` | PASS |
| A3 | ツリーの `..` と親への復帰 | 既定列末尾 `showp,chkdir:rb,focus,up,chkatrow:..` | 先頭行の名前が `..` | `chkatrow:.. -> … cursor row=..: ok` | PASS |
| A4 | **chevron ヒットゾーン**: ダブルクリック経路で展開が発火しない | `tree,home,down,chkatrow:digitset,exparm,dbl,waitdir:digitset,chkexpn:0,chkexp:0` | ジェスチャ中の **全フレーム** で当該パスが未展開。`exparm` は毎フレーム `rbHas(expanded,path)` を見張る (終状態だけ見る `chkexp` では一瞬の展開を見逃す) | `chkexpn:0 -> … tools/testdata/rb/digitset expanded on 0/25 watched frame(s): ok` | PASS |
| A5 | chevron は展開動詞: 1クリックで即トグル、2度目で畳む | `chevclick,chkexp:1,chevclick,chkexp:0` | 展開数 1 → 0。`chevclick` 自身がダブルクリック窓より前にトグルしたことを assert | `chkexp:1 … ok` / `chkexp:0 … ok` | PASS |
| A6 | 名前クリックは選択のみ (展開もしない・降りもしない) | `click,chkexp:0,chkdir:rb,chkatrow:digitset` | 展開 0、dir 不変、カーソルのみ移動 | 3 assert すべて `ok` | PASS |
| A7 | 複数選択 Enter は選択ごとに 1 スタック開く | `ctrlclick,down,ctrlclick,enter,waitimg:44,chkopen:4` | 開いたスタック数 4、名前一覧が `gain10_00#.npy*8 + gain20_00#.npy*8` を含む | `chkopen:4 -> imgs=44 seqs=4 … : ok` | PASS |
| A8 | ダブルクリックはスタックを **1回だけ** 開き、poster を残さない | `dbl,waitimg:24,chkopen:1,chkimg:24,chknames:frame_###.npy*24` | 画像 24 枚ちょうど、preview 由来の重複なし (`missing=[] surplus=[]`) | `chknames:frame_###.npy*24 -> … missing=[] surplus=[]: ok` | PASS |
| A9 | 戻る/進む (マウスボタン 4/5) と Alt+←/→ | `mback,waitdir:rb,chkfwd:1,mfwd,waitdir:digitset,chkfwd:0,altleft,…,altright,…` | back/fwd カウンタが対称に動き、新規ナビゲーションが forward 枝を切る | `chkfwd:1 -> … back=4 fwd=1: ok` / `chkfwd:0 -> … back=5 fwd=0: ok` | PASS |
| A10 | preview スロット (Files 下端) はスタック行 1枠のまま、`,`/`.` でスクラブ | `chkimg:1,chkpv:24,chkidx:0,period,chkidx:1,comma,chkidx:0` | preview 枠は常に 1、scrub 24 コマ、index が ±1 する | `chkpv:24 … pv=1 scrub=24 idx=0: ok`、`chkidx:1 … idx=1: ok` | PASS |
| A11 | server-temporal 要求がキーを奪わない | `svtemp,chkfocus:1,period,chkidx:1` | 要求後もパネルが focus を保持し、スクラブが生きている (defect 1 の再発防止) | `chkfocus:1 … : ok` | PASS |
| A12 | キー経路: パネル focus 中は主ビューが矢印を食わない | 既定列 `blur,down,up,end,home` | focus 中 主ビュー実行 0 / 譲り ≥6、blur 後 ≥4 | `key routing: … the main view ran 0 nav key(s) and stood down for 55; after blur it ran 4 more: ok` | PASS |
| A13 | compare A/B の席 (seat) 規則: 比較中は A/B/C が全濃度 | `--abstats-selftest tools/testdata/multi` S6 | badge probe が `…=A;…=B;…=C;` | `S6 badges (comparing): '00/frame_000‥004.npy=A;02/frame_000‥004.npy=B;rgb_u8.npy=C;'` | PASS |
| A14 | **ESC は一段ずつ外へ**、席は残る (ROI → compare-off → 何もなし) | S6 の `escapePressed()` 3連打 | 1回目 ROI 解除のみ / 2回目 compare 離脱のみ (B ピンとスロット文字は保持) / 3回目 取るものなし | `S6 first ESC deselects the ROI, and nothing else PASS` / `S6 second ESC exits compare, and only compare PASS` / `S6 …keeping the B pin and the slot letters (seats survive) PASS` / `S6 a third ESC has nothing left to take PASS` | PASS |
| A14b | ESC の **popup 段**: メニュー/コンテキストが先に閉じる | 既定列 `rctx,esc,fmenu,esc` | popup が開いている状態から esc 1回で閉じる。`escapePressed()` はそこでは走らない | `after rctx  a popup is open (cursor 90,623): ok` / `after esc   a popup is closed (cursor 90,623): ok` / `after fmenu a popup is open (cursor 41,16): ok` / `after esc   a popup is closed (cursor 41,16): ok` | PASS |
| A15 | compare off でも席は残り **薄く** 出る、A は消える | S6 後半 | `=B(dim);` `=C(dim);` が立ち、`=A` は消滅 | `S6 badges (compare off): '02/frame_000‥004.npy=B(dim);rgb_u8.npy=C(dim);'` / `S6 compare off: no A badge - the cursor is not a seat PASS` | PASS |
| A16 | compare 無しの右クリックに **灰色の Swap を出さない** | abstats A7 | 現在行のメニュー項目数 0、他行は "Set as compare B" のみ | `A7 compare off: A-row=1 other-row=0` / `A7 compare off: the current row offers no items PASS` | PASS |
| A17 | 片側が古い (stale) ときの保持フラグ | abstats P2b/P2c | 連続ステップ中 B は最後の結果を保持し stale と分かる | `P2b throttle: armed=1 held uid=6 vs B uid=8, released=1, after release uid=8` / `P2b B slot holds its last result while stepping (stale) PASS` | PASS |
| A18 | Temporal パネル: A/B 両方の曲線がパネル内に収まる | abstats T1/T1b/T2/T3 | `ran=1 plotted=1 slots=2`、plot 下端 ≤ window 下端、軸なし側は理由を出す | `T1 overlay 560x440: ran=1 plotted=1 slots=2 A=5 B=5 plot y 222.0..327.0 (window bottom 440.0)` / `T2 …and the panel says B has no time axis PASS` | PASS |
| A19 | Temporal x軸ペースト解析 (改行・CRLF 含む) | `--export-tsv-selftest …` E7 | カンマ/空白/タブ/改行/CRLF が全て区切りとして通り、非数値は **位置付きで** 拒否 (黙って飛ばさない) | `E7 newline and comma+newline (Excel column paste) parse PASS` / `E7 parse error: value 3 ('x') is not a number;  value 5 ('1e') is not a number` | PASS |
| A20 | frame-lin パネルの節とスタブ | `--frame-lin-selftest tools/testdata` F10/F11 | 軸未設定なら linearity 節を出さず `set the x axis first` と言う。両プロットがパネル内 | `F11 no axis: the section says 'set the x axis first' PASS` / `F11 both plots laid out INSIDE the panel PASS` | PASS |
| A21 | ROI montage の命名と per-frame-range 注記 | `--export-selftest … --stack always` E5/E6/E10/E11 | 名前が `… ROI 9x7 x5 (montage H)`。per-frame 版は `(montage H, per-frame range)` | `montage 45x7 from 5 frame(s), ROI 9x7 (cfa=0), name '00/frame_000‥004.npy  ROI 9x7 x5 (montage H)'` / `E11 per-frame montage is produced PASS` | PASS |
| A22 | タイル各ペインの同一性表示と狭幅での省略 | `--tile-selftest tools/testdata/multi` T11/T12 | ペインは letter+batch をファイル名より優先。狭くしても A バッジは残り、名前は前方省略/脱落で **溢れない** | `T12 narrow: the A badge itself survives PASS` / `T12 narrow: the name elides from the front or drops, never overflows PASS` | PASS |
| A23 | Browse インスタンス独立性 (item 17) | 既定列 `newpanel … target:N …` | 2枚目の Browse が別ディレクトリを同時に指し、filter/選択/履歴/focus が各々独立 | `chkpanels:2`、`chkfilt:-`、`chksel:0`、`chkback:0` すべて `ok` | PASS |
| A24 | 新規ウィンドウの argv 形 (配置自体は D節) | `--newwin-selftest tools/testdata` N7 | stack は HEAD file + `--stack always`、CFA は pattern が先、remote は ssh url | `N7 stack argv: HEAD file + --stack always PASS` / `newwinselftest: ALL PASS` | PASS |
| A25 | ルート popup 衝突: RAW ダイアログが競合モーダルで消えない | 既定列末尾 `rawopen,popupcheck,seqask,popupcheck` | 前後とも open=1、`forQueue` なら生存 | `root popup collision: RAW dialog open before the competing modal=1, after=1; forQueue implies a live dialog=1: ok` | PASS |

**A節まとめ: 26項目 / 実行 26 / PASS 26 / FAIL 0。**
スイート全体 (21 test): `SUITE TOTAL /c/Users/hish/Desktop/viewer-wt-verifyui pass=21 fail=0`

> **A節から外した項目 (追跡用)**
> - **フッタの zoom% 行の存在**: 状態として観測できないため **E3 (要probe)** へ移した。
>   `dl->AddText()` で直接描くだけで変数にも stderr にも残らない (main.cpp:9873-9877)。
>   「見た」とは書けないので PASS にしていない。
> - **ESC のテキスト編集段**: 同様に **E7** へ。A14/A14b が押さえるのは
>   ROI/compare 段と popup 段のみ。
> - **stale side の "見え方"**: 状態フラグは A17 で PASS。濃さの品質は **D3**。

---

## B. 設計正典 (docs/) のうち UI 状態で表現できるもの

| 項番 | 検証項目 | 操作手順 | 期待 (観測可能な状態) | 実施結果 | 判定 |
|---|---|---|---|---|---|
| B1 | **読み順の規則**: クリックは選ぶ、ダブルクリックは確定する (利用者命名の原則。breadcrumb-as-buttons が名指しの失敗例) | A1/A2/A6 と同じ列 | 単クリックで遷移が起きない、という否定形が状態で立つ | A1/A2/A6 の証拠行に同じ | PASS |
| B2 | 軸ラベルは量と単位を持ち、整数量は `[DN]` を負う | `--export-tsv-selftest …` E1/E7 | ヘッダに `mean [DN]` `sigma_t [DN]` `sigma_frame [%]` が並び、単位は既定で埋められない | `side ch n N sigma_t [DN] sigma_fpn [DN] sigma_tot [DN] source` / `E7 the unit is never defaulted PASS` | PASS |
| B3 | **CFA プレーンを UI の表で混ぜない** | 同上 (`--cfa bayer --bayer-pattern RGGB`) | プレーンごとに 1 行/1 列群。`mean_R / mean_Gr / mean_Gb / mean_B` が独立に並ぶ | `frame file mean_R [DN] sigma_R [DN] … mean_Gr [DN] … mean_Gb [DN] … mean_B [DN] …` | PASS |
| B4 | 部分ロードは n/N で出す (完全に見せない) | E1/E3 | `resident n of N`、部分スタックは `n of n` と言わない | `E3 partial stack says n of N, not n of n PASS` / `# -- side A: … resident 5 of 5 frame(s) --` | PASS |
| B5 | ImGui テーブルの宣言列数と `TableSetupColumn` 数の一致 | `python tools/check_table_columns.py` | 不一致は実行時に `TableSetupColumn() called too many times` になり表が壊れる。ループ生成分は手読み | 16 テーブル中、数値宣言の 10 件はすべて一致 (`quickstats 4/4` `lintab 7/7` `abtemporal 5/5` `framelin 7/7` `roitable 8/8` `rblist 4/4` `derivelist 5/5` ほか)。手読み対象はループ生成 5 件と条件式 1 件 (`px  declared=b ? 5 : 3  setups=5`) のみで、**新規の不一致なし** | PASS |

**B節まとめ: 5項目 / 実行 5 / PASS 5 / FAIL 0。**

---

## C. 進行中の stage 2 / 3 / 5 の受け入れ行 — 未実施 (未マージ)

現 main には該当文字列が **存在しない** ことを確認済み
(`grep -n 'share the same pixels\|no pixel copy\|Reload from disk' core/main.cpp` → 0 hit)。
マージ後、下の手順をそのまま流せば機械的に判定できる。

判定はすべて現時点で **未実施(未マージ)**。

| 項番 | 検証項目 | 操作手順 (マージ後そのまま実行) | 期待 (観測可能な状態) | 実施結果 | 判定 |
|---|---|---|---|---|---|
| C1 | stage2: Files の **⧉ 共有バッジ行** | `--browse-keys-selftest tools/testdata/rb --browse-keys "waitdir:rb,viewreset,w400,home,down,down,down,down,down,down,down,down,dbl,waitimg:24,<共有を作る操作>,chkbadge:⧉"` ※ `chkbadge` 相当が無ければ E1 の probe が要る | 共有元と共有先の **両方** の行に ⧉ が立つ。`g_filesBadgeProbe` 方式なら `name=⧉;` が 2 件 | — | 未実施(未マージ) |
| C2 | stage2: ⧉ の tooltip が **相手の名前** を言う | 同上 + tooltip 文字列 probe | tooltip に相手側スタック名が含まれる (「誰と共有しているか」が読める) | — | 未実施(未マージ) |
| C3 | stage2: `closeStack` の生存通知が **双方向の件数** を言う | `--close-selftest tools/testdata/multi` を拡張、または abstats 流に直接 `closeStack()` を呼んで通知文字列を stderr へ | 「N frame(s) は他 M スタックから参照されているため残す」相当が、**両方向の数** を持つ | — | 未実施(未マージ) |
| C4 | stage2: compare の "A and B share the same pixels" チップ | `--abstats-selftest` の chip 系 (`abStatusChipText()`) に倣う。A と B を同一 source に向けて `abStatusChipText()` を読む | chip が `A and B share the same pixels` を含む (現行 S5 の `A = B` / `paused` と併存すること) | — | 未実施(未マージ) |
| C5 | stage3: derive ダイアログが "will reference … (no pixel copy)" と言う | `--derive-selftest tools/testdata`。現行 D1 が `D1 A vs B by name: … %d to copy` を出しているので、同じ行に参照件数を足す形が自然 | ダイアログ文字列に `will reference` と `(no pixel copy)` が入り、コピー件数と参照件数が別々に出る | — | 未実施(未マージ) |
| C6 | stage5: Files 右クリックの "Reload from disk" が **frame 行** に出る | `--browse-keys` の `rctx` で右クリック → メニュー項目 probe | frame 行のコンテキストメニューに `Reload from disk` が存在し、活性 | — | 未実施(未マージ) |
| C7 | stage5: 同項目が **stack ヘッダ行** にも出る | 同上、カーソルを stack ヘッダに置いて `rctx` | stack ヘッダでも同項目が存在し、活性 | — | 未実施(未マージ) |
| C8 | stage5: remote / origin 無しでは **理由付きで灰色** | remote fixture (`--remote-selftest` の ssh url 系) と derived image (montage) で `rctx` | 項目は **消えず** 非活性、tooltip/サブテキストに理由 (例: リモート、元ファイル無し)。A16 の「灰色の Swap を出さない」とは別ルールなので取り違えないこと | — | 未実施(未マージ) |
| C9 | stage5: 結果の文言 "reloaded N frame(s), M failed" | 上記実行後 toast / Messages 行を stderr probe で読む | 成功数と失敗数の **両方** を必ず言う (0 でも省略しない) | — | 未実施(未マージ) |

**C節まとめ: 9項目 / すべて 未実施(未マージ)。**

> C1/C2/C6/C7/C8 は現状 **probe が足りない** (E節 E1/E2 参照)。
> stage 2/5 側が `g_filesBadgeProbe` と同じ流儀で
> 「バッジ文字列」「コンテキストメニュー項目名+活性」を stderr に出す probe を
> 足してくれれば、上の手順はそのまま機械実行になる。

---

## D. 実機確認のみ (利用者の目が要る)

この機械では **原理的に** 撮れない/測れないもの。数分で歩ける順に並べた。

| 項番 | 見るもの | 見方 | 合格の目安 |
|---|---|---|---|
| D1 | 破線 (dashed line) の描画 | Temporal / linearity の基準線を出す | 破線が破線に見える。実線に潰れていない、点が粗すぎない |
| D2 | マーカーと **アンバー** 色 | compare 中の警告系表示 | アンバーが他の系列色と弁別でき、赤とも黄とも取り違えない |
| D3 | dimming の見え方 | compare を ESC で抜け、Files の B/C を見る (状態は A15 で PASS 済み。**見え方** だけが残件) | 「armed, not active」に読める。消えたと誤解されない、かつ有効と誤解されない濃さ |
| D4 | チャートの box-zoom の操作感 | Temporal でドラッグ矩形ズーム | 掴んだ範囲に素直に追従。取り消しが直感的 |
| D5 | montage の見た目 | ROI montage を H と V で作る | コマ境界が判別でき、per-frame range 版で各コマが飽和/黒潰れしていない |
| D6 | 新規ウィンドウの実配置 | 新規ウィンドウを開く (argv は A24 で PASS 済み) | 実際に画面内へ、親と重ならない位置に出る。マルチモニタでも画面外に飛ばない |
| D7 | 狭幅パネルの実見え | Browse を極端に狭める (幾何は A22/既定列の `w180`/`w1150` で PASS 済み) | 省略記号が意味を殺していない。ボタンが押せる |

**D節まとめ: 7項目 / すべて 実機確認のみ。**

---

## E. 要 probe (自動観測できない UI 事実)

「まだ状態がどこにも印字されていない」もの。**実装はしない**、必要な probe を名指すだけ。

| 項番 | 観測したい UI 事実 | なぜ今は無理か | 必要な probe (名指し) |
|---|---|---|---|
| E1 | Files のコンテキストメニューの **項目名と活性/非活性と理由** | 現状 abstats A7 が数えるのは **項目数** だけ (`A-row=1 other-row=0`)。名前も灰色理由も文字列として出ない | `g_filesBadgeProbe` と同じ流儀の `g_filesCtxProbe`: 1 行に `item=Reload from disk;enabled=0;why=remote source;` を連結 |
| E2 | Files 行の **バッジ集合** の拡張 (⧉ 共有など) | `g_filesBadgeProbe` は A/B/C の席文字と `(dim)` しか積まない (main.cpp:18445) | 同 probe に席以外のバッジも積む (`name=⧉;` を追加)、あるいは `g_filesBadgeProbe2` |
| E3 | **フッタの zoom% 行** の内容 | `dl->AddText()` で直接描いているだけ (main.cpp:9873-9877)。変数にも stderr にも残らない | `g_footerProbe`: コンテキスト行 (`batch > series`) と zoom 文字列を組で保持 |
| E4 | toast / Messages ログの **本文** | toast はフェードする描画物。`toast()` の引数は保持されない | `g_toastProbe` (最後の N 件の本文と重大度) — C9 の "reloaded N frame(s), M failed" の判定にそのまま要る |
| E5 | tooltip の本文 | `ImGui::SetTooltip()` は即描画で残らない | `g_tooltipProbe`: 直近に出た tooltip 文字列。C2 (⧉ の相手名) に必須 |
| E6 | 破線/色/線幅などの **描画属性** | draw list に入って消える | (自動化の対象外と割り切るのが妥当。D節で回す) |
| E7 | ESC の **テキスト編集段** (入力中の ESC が編集だけを抜ける) | `escapePressed()` は `EscTook{Nothing, RoiDeselected, CompareOff}` の 3 段しか持たない (main.cpp:1715)。テキスト段は ImGui 内部で処理され、こちらの状態には残らない。A14/A14b は ROI/compare 段と popup 段しか押さえていない | `g_escProbe`: 1 回の ESC がどの層に食われたかを層名で 1 行出す (`popup` / `textedit` / `roi` / `compare` / `nothing`)。これがあれば「一段ずつ外へ」を **鎖として** 通しで検証できる |

**E節まとめ: 7項目 / すべて 要probe。**

### E節の更新 (2026-08-04, branch `verify-probes`) — E3 と E7 に probe が入った

どちらも既存の `g_tilePanesDrawn` / `g_filesBadgeProbe` と同じ流儀:
**描いた文字列をそのまま積む**。計算し直した値ではない。

**E3 → `g_footerProbe`** (`drawCanvas` のフッタ帯、`main.cpp` の footer strip)。
1フレームぶんを `ctx=<batch  >  series>;zoom=<zoom NN%>;name=<file>;count=<n/N>;`
で保持し、画像が無いフレームは空 (= 帯が無かった、という事実)。
`--tile-selftest` の **T13** が実フレーム経由で4件表明する:

| 表明 | 何が言えるようになったか |
|---|---|
| ctx 行が batch を名乗る | 2行アイデンティティの規則が**フッタでも**成り立つこと。ペインのバッジ (T12) だけが検査されていた |
| zoom 1 → `zoom 100%` | 読み値そのもの |
| zoom 0.005 → `zoom 0.5%` で、`zoom 0%` を**含まない** | **読み値に2書式ある理由そのもの**。`%.0f` だけなら 0.5% は "zoom 0%" になり、0% は「小さい」ではなく「無い」という別の主張になる。値を再計算する検査では絶対に見えない |
| 部分ロードの counter が `1/4 of 8` | docs/terminology.md の n of N を**フッタで**。この counter には読み手が1人も居らず、規則は隣のコメントだけが担っていた |

**E7 → `g_escProbe`** (`escapePressed()` の隣、`main.cpp`)。ESC 1回を**どの層が
食ったか**を層名で1件ずつ積む: `popup;textedit;roi;compare;nothing;`。
4段は4箇所で決まり、うち2箇所 (popup を閉じる末尾の一手と、ImGui 内の text edit) は
`EscTook` に現れないので、「1回の押下が2段進んでいない」は**表明のしようが無かった**。

- `--abstats-selftest` **S6**: 3回の押下が `roi;compare;nothing;` という**1本の鎖**に
  なることを表明。戻り値3つでは「その間に他に何も起きていない」が言えない。
- `--browse-keys-selftest`: `esc` アクションごとに、**消費した層がちょうど1つで、
  それが `popup` である**ことを表明。既存の A14b は「popup が消えた」しか言えず、
  同じ押下が下の層 (ROI / compare) にも届いていないことは見えなかった。
- **校正 (この検査は失敗し得る)**: popup を開かずに `esc` を撃つと、既存の行は
  `after esc a popup is closed: ok` と**通る**のに、新しい行は
  `esc consumed by 1 layer(s), chain ...'nothing;': FAIL` になる。実測。

(同じ branch で selftest フラグが1本増えている: `--srcmap-selftest`。冒頭
「自動化の口」の 22 個は 23 個、スイートは 22 本から 23 本になった。)

**残り**: E1 / E2 / E4 / E5 は未着手、E6 は対象外のまま。
E7 の `textedit` 段は**記録はされるが実行されていない** — `--browse-keys` に
テキスト入力を起こすアクションが無く、この機械で ESC をテキスト編集に食わせる
経路が今日は無い。層名だけが用意されている状態である。

---

## 新規に見つかった不具合

### D-1. selftest が利用者の実 `%APPDATA%/viewer` を書き換える

**分類: 新規。検証衛生の欠陥** (product code の意図と実挙動の食い違い)。

main.cpp:29169-29176 は終了時に、はっきりこう書いて `autosaveSession()` と
`savePrefs()` を止めている:

> a selftest must not leave its scripted clicks in the user's session or their preferences

ところが実際には **2 つ穴がある**:

1. **`layout.ini`**: `io.IniFilename` は main.cpp:20910-20911 で利用者の実
   `%APPDATA%/viewer/layout.ini` に **無条件で** 向けられ、`g_browseKeys` のガードが無い。
   ImGui が終了時に自動保存するため、スクリプトが作ったパネル幾何がそのまま残る。
2. **`autosave.vsession`**: フレームループ内の周期オートセーブ (main.cpp:28609) は
   `!benchFrames` としか見ておらず `g_browseKeys` を見ていない。
   `--browse-keys-selftest` は約 2000 フレーム走り 45 枚開くので、これが発火する。

**実測 (利用者の実ディレクトリ)** — `--browse-keys-selftest` 前後の md5:

```
layout.ini        f739f7b04adc65ebb16d1a457646fee9  ->  dd0b54d8a0f3f9bae51d4b219e2c94e9   (変化)
prefs.txt         5af40934d83e9a27bb27582d201a9509  ->  5af40934d83e9a27bb27582d201a9509   (不変)
autosave.vsession d7b57ce0ccf4e0a432208426efb9bade  ->  d7b57ce0ccf4e0a432208426efb9bade   (内容は不変, mtime は更新)
```

**再現 (空の config ディレクトリで 2 回とも同じ)**:

```bash
rm -rf /tmp/fresh; mkdir -p /tmp/fresh/viewer
APPDATA=/tmp/fresh ./build-mingw/viewer.exe --browse-keys-selftest tools/testdata/rb
ls /tmp/fresh/viewer/          # -> autosave.vsession  layout.ini
grep -c Browse2 /tmp/fresh/viewer/layout.ini   # -> 1
```

残留物の中身は紛れもなくスクリプトの産物:

```
[Window][###Browse2]
Pos=60,60
Size=630,780
```

`###Browse2` は既定アクション列の `newpanel` が作る **2 枚目の Browse パネル**。
つまり利用者がスイートを回すと、自分では開いたことのないパネルの幾何が
自分の layout に紛れ込む。対照実験としてフレームループに入らない
`--abstats-selftest` を空 config で回すと `autosave.vsession` は作られない。

**影響**: 大きくはないが、この案件を過去に焼いた「selftest は利用者の生状態を
継承する」問題の **逆向き** で、しかも意図はコメントで明文化済み。
`prefs.txt` は守られているので、穴は上の 2 箇所だけ。

**修正案 (実装はしていない)**: `g_browseKeys` が非空なら
(a) `io.IniFilename = nullptr` にする、(b) main.cpp:28609 の条件に
`&& g_browseKeys.empty()` を足す。

**回避策 (本 PR に同梱)**: `tools/verify/*.sh` は `APPDATA`/`HOME` を
捨てディレクトリへ向けてから起動する。手で回すときも同じにすること。

---

## 校正 (この表が空振りでない証拠)

UI テストは「落ちない」なら何も証明しない。**product code を一切編集せず**、
コマンドラインの期待値だけを間違いにして、assert が実際に落ちることを示す:

```bash
bash tools/verify/calibrate_browse_keys.sh <checkout>
```

同一のジェスチャ列で、期待する行名だけを差し替えた実測:

```
### 1. DELIBERATELY WRONG expectation (chkatrow:expset) - must FAIL
browsekeys: chkatrow:expset    -> … cur=-  cursor row=digitset: FAIL
browsekeys: 20 action(s) through real frames, no crash, 1 panel check(s) failed: FAILED
rc=1

### 2. CORRECT expectation (chkatrow:digitset) - must PASS
browsekeys: chkatrow:digitset  -> … cur=-  cursor row=digitset: ok
browsekeys: 20 action(s) through real frames, no crash, 0 panel check(s) failed: ok
rc=0
```

差分は `--browse-keys` の 1 トークン (`expset` / `digitset`) のみ。
再ビルドなし、product code の変更なし。

---

## 集計

| 節 | 項目数 | 実行 | PASS | FAIL | 未実施 | 実機確認のみ | 要probe |
|---|---|---|---|---|---|---|---|
| A. main の UI 回帰 | 26 | 26 | 26 | 0 | 0 | 0 | 0 |
| B. 設計正典 | 5 | 5 | 5 | 0 | 0 | 0 | 0 |
| C. stage 2/3/5 受け入れ | 9 | 0 | 0 | 0 | 9 | 0 | 0 |
| D. 実機確認のみ | 7 | — | — | — | — | 7 | 0 |
| E. 要probe | 7 | — | — | — | — | — | 7 |
| **計** | **54** | **31** | **31** | **0** | **9** | **7** | **7** |

- スイート: `SUITE TOTAL /c/Users/hish/Desktop/viewer-wt-verifyui pass=21 fail=0`
- 新規不具合: **D-1 のみ** (検証衛生。product UI の回帰は 0 件)
- stage 1 は UI を変えていない、という主張は A/B **31 項目**で支持される。
- 校正: 同一ジェスチャで期待値 1 トークンだけ違えて rc=1 / rc=0 を実測済み。
