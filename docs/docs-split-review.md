# docs 分割原案のレビュー — issue #55 (branch `docs-split-draft`)

レビュー: Fable(設計)。原案: Opus(fdeccd7 + d425dd8 + 53ec607)。実施日 2026-08-13。
分割の規準(ユーザー指示): **直下 = 今それを理解・変更するのに要るもの(現在形)、
`.background` = なぜ他の形でないか。削除ではなく移動。**

原案ブランチには一切触れていない。本書は main の作業ツリーに置くレビュー文書 1 本のみ。

---

## 0. 結論(先に書く)

**3択の答え: 直してから入れる。** ただし「直す」の実体はリンク修正ではなく、
**rebase + 分割線の引き直し**である。機械的には入らない:

- 原案の merge-base は 578f5c7 (2026-08-03 15:44)。以後 main には **564 コミット**入った
  (`git log docs-split-draft..main --oneline | wc -l`)。
- 今 main に merge すると **15 ファイルが textual conflict**
  (`git merge-tree --write-tree main docs-split-draft` の実測。一覧は §4-E)。
- さらに危険なのは conflict に**ならない** 3 ファイル: 全体移動した
  flat-field-stats.md(+599 行)/ media-support.md(+53 行)/ layers-plan.md(+9 行)は
  rename 追跡が main の新規内容を**黙って `.background` へ運ぶ**。このうち前2者は
  main 側でこの10日間に「確定 (2026-08-09, #57 / #53)」へ昇格した**現行仕様の根拠**で、
  警告なしに背景へ沈む。

一方で**骨格は正しい**。背景ファイル冒頭の「現行ドキュメント: X の背景 — 要旨」と
現行末尾の「経緯と検討: [.background/…]」という規約、「なぜ他の形でないか」を抜く線、
検証マトリクスの列分割(手順・期待=現行 / 実施結果・判定=背景)は、規準の忠実な実装で、
24 の移動塊のうち **17 は今の main でもそのまま正しい**(全件判定は §2)。捨てるのは惜しい。

**削除は本文レベルでゼロ**(数え方と例外 2 点は §3)。壊れる参照は **C++ 60 箇所 +
docs 34 箇所 + tasks.csv 6 行 + README 1 行 + 原案自身の内部リンク切れ 1 件**(§4)。

ユーザー裁定を仰ぐ問いは**残った** — 4 件、§7。

---

## 1. 原案の形(事実の確認)

`git diff main...docs-split-draft --stat`: 44 ファイル、**+1793 / −1175**。内訳:

| 型 | 本数 | 対象 |
|---|---|---|
| 分割(現行を刈り、背景を新設) | 16 | ab-stats-plan / browse-as-file-manager / browse-topbar-design / compare-n / export-design / imgui_modern_design / manual / measure-ux / reference-design / remote / series-plan / startup / terminology / verify-functional / verify-ui / watch-design |
| 全体移動(rename) | 6 | browse-display-candidates / browse-inventory / flat-field-stats / layers-plan / media-support / review-new-code |
| 見送り(背景側だけ残る孤児) | 2 | npz-design / import-adapters(現 input-adapters — d425dd8 が「main 側で書き直されたので分割せず、後で同じ処置をやり直す」と明記) |
| 参照修正のみ | 4 | todo-open.md(flat-field-stats のパス 3 箇所)/ tasks.csv(2 行追加)/ tools/run_selftests.sh / tools/verify/run_ui_matrix.sh |

`.background/` は 24 ファイル(分割の片割れ 16 + rename 6 + 孤児 2)。全ファイルが
「現行ドキュメント: … の背景 — …」の 1 行で始まる。

---

## 2. 問い1 — 分割線は正しいか(全件判定)

判定は 2 列: **原案時**(merge-base 2026-08-03 の main に対して)と **今**(2026-08-13 の
main に対して)。○ = 規準に合う(経緯・却下案・実施済み計画=背景、現在形=現行)。

| # | 文書 | 背景へ移した塊 | 原案時 | 今 | 根拠 |
|---|---|---|---|---|---|
| 1 | ab-stats-plan | §7 フェーズ P0-P5 + リスク 4 点 | ○ | ○ | フェーズは実施済みの計画=経緯。退避規則(不一致時)は現行 §5 に残っており、リスク(4)の「揃ったふりはしない」の現在形は失われていない |
| 2 | browse-as-file-manager | 決定記録(2026-07-29)・診断・実装スケッチ・順序 | ○ | ○ | 現行には「あるべき形」「やらないこと」+ 決着の現在形要約が残る。模範的 |
| 3 | browse-display-candidates(全体) | 判定なしの素材表 | ○ | ○ | 外部参照 0 件。判定前の素材=経緯そのもの |
| 4 | browse-inventory(全体) | 再設計前の棚卸し(コード行番号は当時のもの) | ○ | ○ | ただし tasks.csv 参照列 2 行(行15, 67)の直しが要る(§4-C) |
| 5 | browse-topbar-design | §1 棚卸し/§2.1 サーベイ/§3 案A-C/§4 推奨/§5 未決/§10.1・10.5・10.7・10.8 | 大半○ | **§5・§10.8 は×** | §1-§4 は「なぜこの形でないか」の教科書。**§5 未決事項(要ユーザー判断 6 件)は未決=現在の仕事** — §10.7 が答えたのは 2 件だけ。**§10.8 command palette は main の現行文書が名指しで引く**: preferences-panel-design.md:19「command palette 本体 (browse-topbar-design.md §10.8)」、core/app/settings.inc:1317 |
| 6 | compare-n | §3 画像レイアウト/§4 数値側 N 化/§7 セッション・上限/§8 移行/§9 推奨形と増分 I1-I5 | ○(当時は議論用文書) | **×** | main は §10-12(+314 行)を足し、I1..I5 を前提に I6/I7 を規定、§3・§4・§8-1/8-3/8-4 を**現行参照**している(compare-n.md 103, 261, 335-340, 405, 425, 490 行 — 10 箇所超)。auto-merge すると現行文書の内部参照が宙に浮く |
| 7 | export-design | §0 動機/§1 読者の根拠/§2 却下した 2 つの畳み方 | ○ | ○ | 「カテゴリエラーだから畳まない」=なぜ他の形でないか。模範的 |
| 8 | flat-field-stats(全体) | 全部(「叩き台」として) | ○(凍結叩き台、参照は todo-open 3 箇所のみ) | **×(最重要)** | main で **確定 (2026-08-09, #57)** に昇格、+599 行(判断record・適用ノート・実装の進め方)。**core/ 55 箇所(22 ファイル)+ docs/ 36 箇所(7 ファイル)+ README 1 = 92 箇所**から引かれる現行仕様(数え方: `grep -rn` から build-*/.background を除外)。tasks.csv 行201「分離フィットの判断2件(ユーザー裁定待ち)」の参照先でもある |
| 9 | imgui_modern_design | Python デモ節・フォント節(実験場は 2026-08-03 削除済み) | ○ | ○ | 現行ヘッダに core/ui_theme.cpp への導線が残る。損失なし |
| 10 | import-adapters(孤児) | 全面改訂の経緯・決定 11 項の記録 | △ | **×** | 現行文書は input-adapters.md に**改名済み**。背景ヘッダの自己リンク `../import-adapters.md` は**存在しないファイルを指す**(原案唯一の即時リンク切れ)。内容も改名前のスナップショット。d425dd8 の宣言どおり作り直し |
| 11 | layers-plan(全体) | 実施済み実装計画 | ○ | ○ | main 側 +9 行(撤回追記)も歴史なので、rename に付いて行って害なし。series-plan↔layers-plan の相互参照は両方 .background 内で解決する(偶然だが成立) |
| 12 | manual | §1b 旧レイアウト図・スライダ移設理由 | ○ | ○ | 図の下の説明文のうち現在を指す部分は現行 §1 に残す、という取り分けまでしてある。§1b への外部参照なし |
| 13 | measure-ux | 発端のユーザー申告 1 文 | ○ | ○ | |
| 14 | npz-design(孤児) | v1 前の挙動・決めてほしいこと・レビュー欠陥 V21d-g | △ | △ | 移動でなく**コピー**(現行は main 文のまま、§1・§3 が両方に居る)。背景側が「対応前の挙動。現在は §4 を見ること」と自己申告しており実害は小。やり直し時に再抽出 |
| 15 | reference-design | §1 消費者インベントリ/案A と選定理由/§3.2・§5.1 欠陥記録/§9 判断record+(原文) | ○ | ○(要 rebase) | 判断record のうち判断3(Files のバイト列)は現行 §7 に現在形で畳み直してある — 丁寧。判断2(CFA 現状維持)の**再訪条件**(平坦統計レビューと一緒に再議)だけ △ — 再訪条件は現在の縛り。main 側 §10 追補(2026-08-06, #82)と同じ領域で conflict |
| 16 | remote | §1 なぜ ssh -X が遅いか/§3 利点表/§5 WebSocket との関係/§6 他の選択肢 | ○ | ○ | 規準の模範例。現行 §8 の §5 参照も背景リンクへ張り替え済み |
| 17 | review-new-code(全体) | 実装直後のレビュー記録 | ○ | ○ | tasks.csv 行125 の参照列の直しが要る |
| 18 | series-plan | §0 当時の現状/phase 注記/§7 正典への修正提案/追記 8-17 | ○だが §5 が痩せ過ぎ | ○(main 文を正に) | 原案の現行 §5 は「resolvePendingSeries() が解決する」とだけ言い、**どうやって**(token)を背景に落とした — 現在の機構が現行から消える誤り。ただし main がその後 §5 に token 機構を書き直した(series-plan.md:106-107)ので、**rebase で main 文を残せば解消**。追記 9-13 の不変条件は正典側に反映済みかを rebase 時に照合すること |
| 19 | startup | §2-2 廃止した入口/前の姿 | ○ | ○ | 現行に「File メニューに残るのは 2 つ」という現在形を残す取り分けが正確 |
| 20 | terminology | なぜ正典が要ったか/なぜ series が要るか/廃止した入口 | ○ | ○(1 行だけ△) | △: **series/batch の判定 1 問**(「メンバにパラメータ値が付いていて、その並びでフィットするか?」)は歴史でなく**分類の現行規則**。この 1 行だけ現行へ残す価値がある |
| 21 | verify-functional | 実施記録(2026-08-03 ラン): 結論・実施結果/判定列・A-16・証跡・E 節詳細 | ○ | ○(要 rebase) | **列分割**(検証項目・手順・期待=現行 / 実施結果・判定=背景)は規準の良い実装 — 表が「再実行できる手順」として現行に残る。main 側が D 節更新(2026-08-04, srcmap probe で D-1/D-2/D-7 解消)等 +85 行を足しており conflict。新しい行も同じ型で分ける |
| 22 | verify-ui | 同上 + D-1 調査記録(md5 実測・再現手順) | ○ | ○(要 rebase) | 現行に APPDATA 警告箱 + 背景ポインタを残している — 丁寧。main.cpp:928 のコメントが D-1 の実測を指している点は §4-A |
| 23 | watch-design | §9 判断record(2026-08-02 確定)+「決めずに残す(ユーザー判断待ち)」4 項 | 判断record は○ | **未決 4 項は×** | 判断record の中身は現行 §9 の現在形記述でカバーされる。しかし**未決リストは背景ではなく現在の仕事**。なお main 側で項2(ポーリング間隔の prefs 化)は #50 の P11 裁定で決着済み — rebase 時に 4 項を再判定して残りを board へ |
| 24 | media-support(全体) | 全部(「未実装形式の調査」として) | ○ | **×** | main で §1 に「**状態: 実装済み (2026-08-09、#53)**」ヘッダが付き、実測ビルドコスト・生きている判断(channel マッピング等)を運ぶ**現行判断の根拠**になった。video-support.md が §2 を正面から引く(3 箇所、§「media-support.md §2 からの変更点」を含む)。core/exrread.h:26・core/selftest/media.inc:542,906・tools/mkexr.cpp:9・input-adapters.md:331 からも参照 |

**間違っている塊の名指し(今の main 基準)**: flat-field-stats(全体・最重要)/
media-support(全体)/ compare-n §3・§4・§8・§9 / browse-topbar-design §5・§10.8 /
watch-design 未決 4 項 / import-adapters(孤児・リンク切れ)。
このうち flat-field-stats・media-support・compare-n は「原案時は正しく、main が動いた」もの。
原案自身の判定ミスと言えるのは **§5/§10.8・watch-design 未決 4 項・series-plan §5 の痩せ過ぎ**
(=「未決」と「機構のどうやって」を背景に落とした)の 3 系統。

---

## 3. 問い2 — 「削除ではなく移動」は守られているか

**数え方**:

1. `git diff main...docs-split-draft`(merge-base 578f5c7 起点)= **削除 1175 行**。
2. 削除行の各行を空白正規化し、**同 diff の追加行 1793 行の集合**と照合 →
   一致する行が無い非空行 **138**。
3. 138 行を全件目視で分類(スクリプト出力を 1 行ずつ確認):

| 分類 | 行数 | 中身 |
|---|---|---|
| 検証表の**列分割** | 86 | verify-functional 43 + verify-ui 43。6 列の表を現行 4 列 + 背景 3 列に割ったため行全体では一致しないが、**全セルがどちらかに現存**することを項番単位で確認(A-1..A-16 / B-1a..B-5 / C 全行 / まとめ行 / 集計表 / 「A 節から外した項目」/ D-1 / 校正) |
| 見出し・表題の言い換え | 22 | 元の表題は背景に「元の表題は…」として記録(ab-stats-plan / browse-topbar / series-plan / compare-n ほか)。phase 番号付き見出し → 番号なし(phase 分けは背景に一覧化) |
| 文の分割 | 18 | 前半=現行、後半=背景に現存(export-design / manual / terminology / startup / browse-topbar §10.3 ほか) |
| リンクパスの機械的修正 | 12 | `../`→`../../`(media-support / imgui の画像)、素の `docs/x.md` → 相対リンク化、todo-open の `.background/` パス 3 箇所 |
| tools コメントの参照更新 | 2 | run_selftests.sh(行番号参照→節名)、run_ui_matrix.sh(D-1 の在処) |

**判定: 本文が消えた行は 0。** 1 行単位の削除は見つからなかった。ただし注意 2 点:

- (a) npz-design / import-adapters の 2 本は移動でなく**コピー**(現行から削っていない —
  d425dd8 が意図して main 文を残した)。規準どおりの「移動」になっていないのはこの 2 本だけで、
  原案自身が「後でやり直す」と宣言済み。
- (b) 列分割は「行の移動」であって現行側から実施結果が読めなくなる — これは規準の意図
  (実施記録=経緯)どおりであり削除ではない、と判定した。

---

## 4. 問い3 — 壊れる参照の一覧(直しはしない。リストのみ)

「今の main にこのまま入れた場合」。**切** = パスが存在しなくなる。**⚠** = ファイルは在るが
参照先の節・内容が `.background` へ移り、参照が空を指す。

### A. C++ 側(約束どおり触っていない — 修正リストのみ)

flat-field-stats.md(→ .background へ rename)で**切**になるもの、計 **55 箇所 / 22 ファイル**:

| ファイル | 箇所数 | 代表行 |
|---|---|---|
| core/app/setanalysis.inc | 15 | 4, 36, 61, 100, 300, 315, 317 ほか |
| core/ui/panel_temporal.inc | 5 | |
| core/app/preprocess.inc | 4 | 26, 44, 162, 183(**文字列リテラル内**=UI 文言に出るパスを含む) |
| core/app/state.h / core/app/temporal_model.inc | 各 4 | |
| core/ui/panel_rois.inc / core/ui/panel_setanalysis.inc | 各 3 | |
| core/app/detrend.inc / core/selftest/setanalysis.inc | 各 2 | detrend.inc:4, 34 |
| core/main.cpp:126 / core/app/cli.inc:1454 / core/remote_proto.h / core/serve.cpp / core/setfold.h / core/shading_probe.h / core/ui/canvas.inc / core/ui/file_list.inc / core/ui/panel_histogram.inc / core/selftest/{detrend,roi-export,rtemporal,verify}.inc | 各 1 | |

media-support.md で**切**: core/exrread.h:26(§1)、core/selftest/media.inc:542(§1)・906(§2.4)、
tools/mkexr.cpp:9(§1)。

節が背景へ移って**⚠**: core/app/settings.inc:1317(browse-topbar-design.md §10.8)、
core/main.cpp:928(verify-ui.md の D-1 md5 実測 — 内容は .background/verify-ui.md へ)。

無事を確認したもの: main.cpp:774(ab-stats-plan §1 — 現行に残る)、2757(browse-topbar §10.3 —
残る)、2945(verify-ui E7 — 残る)、137(watch-design)、1264(terminology)、28(input-adapters §4)。

### B. docs 側(main の現行文書)

- flat-field-stats **切**: analysis-layers.md ×11、settings-inventory.md ×7、
  stats-taxonomy.md ×6、todo-open.md ×3(原案が直すが行位置がずれ conflict)、
  export-design.md ×2、manual.md ×2。
- media-support **切**: video-support.md:3, 472(見出し「media-support.md §2 からの変更点」), 484、
  input-adapters.md:331。
- browse-topbar-design §10.8 **⚠**: preferences-panel-design.md:19。
- compare-n **⚠(内部)**: 現行に残る §10-12 から、背景へ移る §3/§4/§8-1/§8-3/§8-4/I1-I5 への
  参照 10 箇所超(103, 261, 318-321, 335-340, 405, 425, 490 行)。
- README.md:302 **切**(flat-field-stats の表リンク)。README は合図があるまで触らない運用
  なので、リストに載せるのみ。

### C. tasks.csv 参照列

| 行 | 分類 | 参照 | 何が起きるか |
|---|---|---|---|
| 15 | 残課題【Fable】 | docs/browse-inventory.md | 切 |
| 67 | 対応済み | docs/browse-inventory.md | 切 |
| 111 / 197 | 対応済み | flat-field-stats.md | 切 |
| 125 | 残課題【Fable】 | docs/review-new-code.md 5節 | 切 |
| 201 | 対応済み(**ユーザー裁定待ち 2 件**を含む) | docs/flat-field-stats.md | 切 — 裁定待ち項目の根拠文書 |

さらに原案は自分の 2 行(「Browse 下端ステータス行」「update.cmd/update.sh」)を持ち込むが、
main では**同じ 2 行が既に「対応済み」**(行 2, 3)。merge すると状態の矛盾した重複行になる。
原案側 2 行は捨てるのが正。

### D. 原案自身の内部リンク

- **docs/.background/import-adapters.md ヘッダ → `../import-adapters.md`: 存在しない**
  (現 input-adapters.md)。原案唯一の即時リンク切れ。
- .background/startup.md 内の `[browse-topbar-design.md](browse-topbar-design.md)` は
  .background 側の同名に解決する(切れないが、意図が現行側なら誤誘導。再抽出時に確認)。
- それ以外の背景内相互リンク(`../terminology.md` 等)と現行→背景リンクは全て解決することを
  確認した。

### E. merge の機械的結果(`git merge-tree --write-tree main docs-split-draft` 実測)

- **textual conflict 15 ファイル**: browse-topbar-design / compare-n / export-design /
  imgui_modern_design / measure-ux / reference-design / remote / series-plan / startup /
  tasks.csv / terminology / verify-functional / verify-ui / watch-design /
  tools/run_selftests.sh。
- **無警告で auto-merge される危険 3 ファイル**: flat-field-stats(+599 行の確定 spec が
  背景へ)/ media-support(+53 行の実装済みヘッダが背景へ)/ layers-plan(+9 行、これは無害)。
  conflict にならないものが一番危ない。
- なお tools/run_selftests.sh の原案修正(行番号→節名)は main が既に同内容を取り込み済み。

---

## 5. 問い4 — 「今は要らない」は本当か(現在形が背景に落ちた例の名指し)

1. **flat-field-stats.md 全体**(§2 #8)。確定仕様 + 判断record + 92 箇所の被参照。
2. **media-support.md §1**(§2 #24)。実装済み判断の根拠と実測コスト。
   ※ 1・2 は原案時点では正しかった — 落とした線ではなく、動いた地面。
3. **compare-n.md §3・§4・§8・§9**。「本来あるべき姿【実装しない】」が §10-12 の着地で
   実装記録+現行仕様の参照先に変わった。特に §3 の「wipe/diff は厳密に A 対 B」は §12 が
   「触らないもの」として現行参照している。
4. **browse-topbar-design.md §5 未決事項**(6 件中 4 件が未回答)と **§10.8**(command
   palette — preferences-panel-design が「本体はここ」と名指す現役の設計母体)。
5. **watch-design.md「決めずに残す(ユーザー判断待ち)」4 項**。未決は現在の仕事。
6. **series-plan.md §5 の解決機構(token)**。原案は「どうやって」を背景に落とした。
   main が §5 を書き直したため rebase で main 文を正とすれば解消。
7. **terminology.md の series/batch 判定 1 問**(△)。分類の現行規則なので 1 行だけ現行へ。
8. **reference-design.md 判断record の判断2**(CFA 現状維持、△)。**再訪条件**
   (平坦統計レビューと一緒に再議 — その平坦統計は #57 で確定した)が現在の縛り。

構造的な教訓を 3 行で:
- **未決リスト・判断record の再訪条件は背景に落とさない**(未決=現在の仕事、再訪条件=現在の縛り)。
- **他文書・コードから §番号で引かれている節は、移すなら参照ごと**。被参照の全数を先に数える。
- **「叩き台」「実装しない」の但し書きは賞味期限を持つ** — 分割線は merge 直前の main で引き直す。

---

## 6. 問い5 — 3択の答えと、直し方

**直してから入れる。** 「入れる」は不可能(conflict 15 + 無警告誤マージ 3)、
「やめる」は過剰(24 塊中 17 は今も正しく、規約・列分割・背景ヘッダの設計は再利用すべき資産)。

文書ごとの処置(rebase 後の再判定込み):

| 処置 | 対象 |
|---|---|
| ほぼそのまま(conflict 解消と参照修正のみ) | ab-stats-plan, browse-as-file-manager, browse-display-candidates, browse-inventory, export-design, imgui_modern_design, layers-plan, manual, measure-ux, remote, review-new-code, startup |
| 小修正して入れる | terminology(判定 1 問を現行へ残す), reference-design(main §10 と rebase・判断2 の再訪条件を現行へ), series-plan(main の §5 文を正とする), verify-functional / verify-ui(main の追記行を同じ列分割の型で処理), watch-design(未決 4 項を現行へ戻し、うち決着済みを board と突合), browse-topbar-design(§5 残 4 件と §10.8 を現行へ戻す) |
| 分割線を引き直す | compare-n(§10-12 を正とし、何が「済んだ経緯」で何が「現行仕様」かを再判定) |
| 移動を取り消す | flat-field-stats, media-support(今は現行仕様の根拠。背景化するなら「確定後の姿」に対する第二段の分割として別途) |
| 作り直し | .background/import-adapters.md(→ input-adapters 現行文から再抽出), .background/npz-design.md(重複解消) |
| 捨てる | 原案の tasks.csv 2 行(main で対応済み) |

同時に出す修正リスト(C++ は触らない約束なので、**分割を入れる PR とは別に** Opus へ):
§4-A の 55+4+2 箇所のパス/節参照。ただし flat-field-stats / media-support の移動を
取り消すなら大半は不要になり、残るのは §10.8(settings.inc:1317)と D-1(main.cpp:928)の
2 箇所だけ — **移動を取り消す判断が C++ 側の修正 59 箇所を消す**。これも取り消しを推す理由。

---

## 7. ユーザーに裁定を仰ぐ問い(残った)

1. **flat-field-stats / media-support の第二段分割をやるか。** 移動取り消しだけなら安全で
   C++ 修正もほぼ不要。ただし両文書とも「確定した現在形 + 長大な経緯」を 1 本に抱えたまま
   になる。経緯部分(却下した推定量、tinyexr 比較等)を確定後の姿で改めて背景へ抜くか —
   #57/#53 の帳簿(判断record の所在)を動かすので、指示なしにはやらない。
2. **「実装から返ってきたもの」節(watch-design §11-16、compare-n §11 型)の扱い。**
   原案はこの型の節を知らない(全て原案後の main 産)。分割の対象に含めるなら規準の追記が
   要る。私見: 本文に勝っている間は現在形 — 現行に置く。ただし本文へ吸収された時点で背景へ。
3. **未決の吸い上げ先。** browse-topbar §5 の残 4 件・watch-design の残項を、現行文書に
   戻すだけでなく board(tasks.csv)にも載せるか。私見: 載せる(未決が文書の奥で眠るのが
   今回の×の型)。
4. **`.background` の可視性。** `grep -n "^## " docs/*.md` 型の一覧・レビュー動線から背景が
   消えるのは意図どおりか。README の docs 表(行 302 ほか)の扱いは README 運用(合図待ち)に
   従い、ここではリストに留めた。

---

## 付録 — 数えの再現手順

- 全体像: `git show docs-split-draft --stat` / `git diff main...docs-split-draft --stat`
- コミット差: `git log docs-split-draft..main --oneline | wc -l` → 564
- 削除行の照合: diff の −行 1175 を空白正規化し +行集合と照合 → 不一致 138 → 全件目視(§3 の表)
- 被参照の全数: `grep -rn "<doc名>"`(*.md *.csv *.sh *.py *.cpp *.inc *.h、build-*/.background 除外)
  — flat-field-stats 92(core 55 / docs 36 / README 1)、media-support 9、browse-inventory 3(うち
  tasks.csv 2)、review-new-code 1、layers-plan 1(background 内で自己解決)、browse-display-candidates 0
- merge の実測: `git merge-tree --write-tree --name-only main docs-split-draft`(conflict 15 +
  無警告 auto-merge 3)
- main.cpp の docs 引用: `grep -n "docs/" core/main.cpp` は今日 **17 行**(課題文の 35 箇所は
  TU 分割(split-plan)前の数と思われる — 引用は core/app/・core/ui/・core/selftest/ の
  .inc 群へ分散しており、影響箇所は上の §4-A で .inc 込みで数えた)
