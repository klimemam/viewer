# UI 検証項目 (verify-ui)

UI 検証の手順・自動化の口・各項目の合格条件。

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
> D-1 の調査記録は [.background/verify-ui.md](.background/verify-ui.md) にある。

---

## A. 現行 main の UI 回帰 — 今すぐ自動実行できるもの

実行: `bash tools/verify/run_ui_matrix.sh <checkout>` (APPDATA 隔離済み)。

| 項番 | 検証項目 | 操作手順 | 期待 (観測可能な状態) |
|---|---|---|---|
| A1 | Files/Browse: クリック=選択、ダブルクリック=確定 の分離 (利用者命名の原則) | `--browse-keys-selftest tools/testdata/rb` 既定列の `click,chkdir:rb,chkatrow:digitset` 区間 | フォルダ行を single click してもディレクトリは変わらず (`chkdir:rb`)、カーソルだけがその行に載る |
| A2 | フォルダ行のダブルクリックは降りる | 同上 `dbl,waitdir:digitset` | 降下先が `digitset` になる |
| A3 | ツリーの `..` と親への復帰 | 既定列末尾 `showp,chkdir:rb,focus,up,chkatrow:..` | 先頭行の名前が `..` |
| A4 | **chevron ヒットゾーン**: ダブルクリック経路で展開が発火しない | `tree,home,down,chkatrow:digitset,exparm,dbl,waitdir:digitset,chkexpn:0,chkexp:0` | ジェスチャ中の **全フレーム** で当該パスが未展開。`exparm` は毎フレーム `rbHas(expanded,path)` を見張る (終状態だけ見る `chkexp` では一瞬の展開を見逃す) |
| A5 | chevron は展開動詞: 1クリックで即トグル、2度目で畳む | `chevclick,chkexp:1,chevclick,chkexp:0` | 展開数 1 → 0。`chevclick` 自身がダブルクリック窓より前にトグルしたことを assert |
| A6 | 名前クリックは選択のみ (展開もしない・降りもしない) | `click,chkexp:0,chkdir:rb,chkatrow:digitset` | 展開 0、dir 不変、カーソルのみ移動 |
| A7 | 複数選択 Enter は選択ごとに 1 スタック開く | `ctrlclick,down,ctrlclick,enter,waitimg:44,chkopen:4` | 開いたスタック数 4、名前一覧が `gain10_00#.npy*8 + gain20_00#.npy*8` を含む |
| A8 | ダブルクリックはスタックを **1回だけ** 開き、poster を残さない | `dbl,waitimg:24,chkopen:1,chkimg:24,chknames:frame_###.npy*24` | 画像 24 枚ちょうど、preview 由来の重複なし (`missing=[] surplus=[]`) |
| A9 | 戻る/進む (マウスボタン 4/5) と Alt+←/→ | `mback,waitdir:rb,chkfwd:1,mfwd,waitdir:digitset,chkfwd:0,altleft,…,altright,…` | back/fwd カウンタが対称に動き、新規ナビゲーションが forward 枝を切る |
| A10 | preview スロット (Files 下端) はスタック行 1枠のまま、`,`/`.` でスクラブ | `chkimg:1,chkpv:24,chkidx:0,period,chkidx:1,comma,chkidx:0` | preview 枠は常に 1、scrub 24 コマ、index が ±1 する |
| A11 | server-temporal 要求がキーを奪わない | `svtemp,chkfocus:1,period,chkidx:1` | 要求後もパネルが focus を保持し、スクラブが生きている (defect 1 の再発防止) |
| A12 | キー経路: パネル focus 中は主ビューが矢印を食わない | 既定列 `blur,down,up,end,home` | focus 中 主ビュー実行 0 / 譲り ≥6、blur 後 ≥4 |
| A13 | compare A/B の席 (seat) 規則: 比較中は A/B/C が全濃度 | `--abstats-selftest tools/testdata/multi` S6 | badge probe が `…=A;…=B;…=C;` |
| A14 | **ESC は一段ずつ外へ**、席は残る (ROI → compare-off → 何もなし) | S6 の `escapePressed()` 3連打 | 1回目 ROI 解除のみ / 2回目 compare 離脱のみ (B ピンとスロット文字は保持) / 3回目 取るものなし |
| A14b | ESC の **popup 段**: メニュー/コンテキストが先に閉じる | 既定列 `rctx,esc,fmenu,esc` | popup が開いている状態から esc 1回で閉じる。`escapePressed()` はそこでは走らない |
| A15 | compare off でも席は残り **薄く** 出る、A は消える | S6 後半 | `=B(dim);` `=C(dim);` が立ち、`=A` は消滅 |
| A16 | compare 無しの右クリックに **灰色の Swap を出さない** | abstats A7 | 現在行のメニュー項目数 0、他行は "Set as compare B" のみ |
| A17 | 片側が古い (stale) ときの保持フラグ | abstats P2b/P2c | 連続ステップ中 B は最後の結果を保持し stale と分かる |
| A18 | Temporal パネル: A/B 両方の曲線がパネル内に収まる | abstats T1/T1b/T2/T3 | `ran=1 plotted=1 slots=2`、plot 下端 ≤ window 下端、軸なし側は理由を出す |
| A19 | Temporal x軸ペースト解析 (改行・CRLF 含む) | `--export-tsv-selftest …` E7 | カンマ/空白/タブ/改行/CRLF が全て区切りとして通り、非数値は **位置付きで** 拒否 (黙って飛ばさない) |
| A20 | frame-lin パネルの節とスタブ | `--frame-lin-selftest tools/testdata` F10/F11 | 軸未設定なら linearity 節を出さず `set the x axis first` と言う。両プロットがパネル内 |
| A21 | ROI montage の命名と per-frame-range 注記 | `--export-selftest … --sequence always` E5/E6/E10/E11 | 名前が `… ROI 9x7 x5 (montage H)`。per-frame 版は `(montage H, per-frame range)` |
| A22 | タイル各ペインの同一性表示と狭幅での省略 | `--tile-selftest tools/testdata/multi` T11/T12 | ペインは letter+batch をファイル名より優先。狭くしても A バッジは残り、名前は前方省略/脱落で **溢れない** |
| A23 | Browse インスタンス独立性 (item 17) | 既定列 `newpanel … target:N …` | 2枚目の Browse が別ディレクトリを同時に指し、filter/選択/履歴/focus が各々独立 |
| A24 | 新規ウィンドウの argv 形 (配置自体は D節) | `--newwin-selftest tools/testdata` N7 | stack は HEAD file + `--sequence always`、CFA は pattern が先、remote は ssh url |
| A25 | ルート popup 衝突: RAW ダイアログが競合モーダルで消えない | 既定列末尾 `rawopen,popupcheck,seqask,popupcheck` | 前後とも open=1、`forQueue` なら生存 |

---

## B. 設計正典 (docs/) のうち UI 状態で表現できるもの

| 項番 | 検証項目 | 操作手順 | 期待 (観測可能な状態) |
|---|---|---|---|
| B1 | **読み順の規則**: クリックは選ぶ、ダブルクリックは確定する (利用者命名の原則。breadcrumb-as-buttons が名指しの失敗例) | A1/A2/A6 と同じ列 | 単クリックで遷移が起きない、という否定形が状態で立つ |
| B2 | 軸ラベルは量と単位を持ち、整数量は `[DN]` を負う | `--export-tsv-selftest …` E1/E7 | ヘッダに `mean [DN]` `sigma_t [DN]` `sigma_frame [%]` が並び、単位は既定で埋められない |
| B3 | **CFA プレーンを UI の表で混ぜない** | 同上 (`--cfa bayer --bayer-pattern RGGB`) | プレーンごとに 1 行/1 列群。`mean_R / mean_Gr / mean_Gb / mean_B` が独立に並ぶ |
| B4 | 部分ロードは n/N で出す (完全に見せない) | E1/E3 | `resident n of N`、部分スタックは `n of n` と言わない |
| B5 | ImGui テーブルの宣言列数と `TableSetupColumn` 数の一致 | `python tools/check_table_columns.py` | 不一致は実行時に `TableSetupColumn() called too many times` になり表が壊れる。ループ生成分は手読み |

---

## C. 進行中の stage 2 / 3 / 5 の受け入れ行

マージ後、下の手順をそのまま流せば機械的に判定できる。

| 項番 | 検証項目 | 操作手順 (マージ後そのまま実行) | 期待 (観測可能な状態) |
|---|---|---|---|
| C1 | stage2: Files の **⧉ 共有バッジ行** | `--browse-keys-selftest tools/testdata/rb --browse-keys "waitdir:rb,viewreset,w400,home,down,down,down,down,down,down,down,down,dbl,waitimg:24,<共有を作る操作>,chkbadge:⧉"` ※ `chkbadge` 相当が無ければ E1 の probe が要る | 共有元と共有先の **両方** の行に ⧉ が立つ。`g_filesBadgeProbe` 方式なら `name=⧉;` が 2 件 |
| C2 | stage2: ⧉ の tooltip が **相手の名前** を言う | 同上 + tooltip 文字列 probe | tooltip に相手側スタック名が含まれる (「誰と共有しているか」が読める) |
| C3 | stage2: `closeStack` の生存通知が **双方向の件数** を言う | `--close-selftest tools/testdata/multi` を拡張、または abstats 流に直接 `closeStack()` を呼んで通知文字列を stderr へ | 「N frame(s) は他 M スタックから参照されているため残す」相当が、**両方向の数** を持つ |
| C4 | stage2: compare の "A and B share the same pixels" チップ | `--abstats-selftest` の chip 系 (`abStatusChipText()`) に倣う。A と B を同一 source に向けて `abStatusChipText()` を読む | chip が `A and B share the same pixels` を含む (現行 S5 の `A = B` / `paused` と併存すること) |
| C5 | stage3: derive ダイアログが "will reference … (no pixel copy)" と言う | `--derive-selftest tools/testdata`。現行 D1 が `D1 A vs B by name: … %d to copy` を出しているので、同じ行に参照件数を足す形が自然 | ダイアログ文字列に `will reference` と `(no pixel copy)` が入り、コピー件数と参照件数が別々に出る |
| C6 | stage5: Files 右クリックの "Reload from disk" が **frame 行** に出る | `--browse-keys` の `rctx` で右クリック → メニュー項目 probe | frame 行のコンテキストメニューに `Reload from disk` が存在し、活性 |
| C7 | stage5: 同項目が **stack ヘッダ行** にも出る | 同上、カーソルを stack ヘッダに置いて `rctx` | stack ヘッダでも同項目が存在し、活性 |
| C8 | stage5: remote / origin 無しでは **理由付きで灰色** | remote fixture (`--remote-selftest` の ssh url 系) と derived image (montage) で `rctx` | 項目は **消えず** 非活性、tooltip/サブテキストに理由 (例: リモート、元ファイル無し)。A16 の「灰色の Swap を出さない」とは別ルールなので取り違えないこと |
| C9 | stage5: 結果の文言 "reloaded N frame(s), M failed" | 上記実行後 toast / Messages 行を stderr probe で読む | 成功数と失敗数の **両方** を必ず言う (0 でも省略しない) |

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

---

## 校正 (この表が空振りでない証拠)

UI テストは「落ちない」なら何も証明しない。**product code を一切編集せず**、
コマンドラインの期待値だけを間違いにして、assert が実際に落ちることを示す:

```bash
bash tools/verify/calibrate_browse_keys.sh <checkout>
```

経緯と検討: [.background/verify-ui.md](.background/verify-ui.md)
