# core/main.cpp 分割計画 — 並列作業が衝突しない構造へ (#47 包含)

基準: main @ 2c3c3d3(main.cpp 38,939 行)。状態: 提案。§9 の判断 4 件が返れば着手できる。
**この文書はコードを 1 行も動かさない。動かすのは §5 の増分で、それぞれ別の作業。**

## 0. 目的と成功基準

主目的は **マージ衝突面**。今週、並列に走らせたエージェントは全員 core/main.cpp で衝突した。
成功基準: 無関係な 2 課題を並列に走らせたとき、触るファイルが交わらない。

副目的は #47。Browse を「別リポジトリで管理できる形」に近づける。
ファイル境界をここで確定し、以後の手術を不要にする。

非目的: 動作変更、リネーム、API の再設計。挙動を変える提案はこの文書には無い。

## 1. 方式 — 2 段階

### S 段階: テキスト分割(1 TU のまま)

main.cpp を節ごとに切り出し、切り口に `#include "xxx.inc"` を残す。
翻訳単位は 1 個のまま。コンパイラから見えるものは分割前と同一で、それを機械的に証明できる(§5 の検証)。

なぜ最初から実 TU にしないか。main.cpp には file-scope の `static` 関数が 694 個、`static` グローバルが 82 個ある。
実 TU 化はその連鎖を de-static してヘッダに起こす作業で、増分十数個・数週間の規模になる。
その間、並列展開は止まるか、動く構造の上で走ることになる。どちらも本末転倒。
S 段階は純粋なテキスト移動なので 2〜3 日で終わり、衝突面はその時点で消える。

S 段階の絶対規則:

- **順序を一切変えない。** 断片は元の位置に元の順で `#include` される。住所が変な関数(例: `selftestMakeSeries` が series モーダルの隣にいる)も S 段階では動かさず、`// → P 段階で core/selftest/util.inc へ` と注記だけ置く。
- 切るのは連続した塊だけ。塊の途中に別件の関数が挟まっていたら、それは残して注記する。
- 断片の先頭には 1 行書く: `// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: <板テーマ>`。
- CMakeLists.txt は無変更。`#include` された断片はコンパイラの depfile が追うので、再ビルドは正しく走る。

拡張子は `.inc`。`.cpp` にすると「独立にコンパイルされる」と誤読される。

### P 段階: 実 TU 化(払う所だけ)

実 TU 化が買うものはコンパイル時間・IDE・持ち出し可能性で、衝突面ではない(それは S 段階で済んでいる)。
だから全部はやらない。予定に入れるのは 2 つだけ:

- **P6**: App を自己完結ヘッダ `core/app/state.h` に(新規コードが実 TU で生まれられるように。#84 の実装が最初の客)。
- **P7**: Browse の実 TU 化 + 継ぎ目(#47 の本体。§3)。

パネル群・ローダ群の実 TU 化は「必要になったら」であり、この計画では予定しない(§9 判断 3)。

### App の扱い(この仕様が決める)

**グローバル `static App app` は残す。分割は TU 単位のみ。**

- エリア別インターフェース化(Browse 以外)は却下。ほぼ全関数が引数なしで `app` を読む設計を作り直す作業で、費用は分割全体より大きく、衝突面には効かない。
- 費用の明示: App の定義を持つヘッダ(P6 の state.h)は全域から include され、フィールド追加はそこに集中する。ただしフィールド追加は 1 行の挿入で、エリアごとに節が分かれていれば git の 3-way マージが機械的に併合する。今週の衝突は関数本体の重なりであって、フィールドの隣接ではない。
- state.h 内はエリア別のバナーコメント(`// ---- browse ----` など)で節を分ける。並べ替えはしない(既にほぼエリア順に固まっている)。
- `App::BrowseInstance` などのネスト型のリネームはしない。数千行の参照 churn は「移動のみ」の原則に反する。P7 だけ例外で、alias 方式(§3)により churn ゼロで型を外へ出す。

## 2. 目標ファイル地図

行数は 2c3c3d3 時点の節境界からの概算(±)。「節名」は main.cpp 内の `// ---- xxx` 区切りコメントで、行番号より長生きするのでそちらを一次キーにする。

### core/main.cpp(残る背骨、~2.6k 行)

include 群と GL define、ウィンドウ/GLFW/ImGui backend の起動列、dock builder の初期レイアウト、
crash handler の設置、フレームループ(idle throttle: g_wakeUntil / wakeUi / redrawNow)、
browse-keys のループ内ドライバ(ループに 18 箇所編み込まれている)、teardown の停止順。
動かさない理由は §6。

### core/app/ — モデル・ローダ・基盤

| ファイル | 中身(節名) | 行数 | 板テーマ |
|---|---|---|---|
| state.inc → P6 で state.h | image model: FrameSource / ImageDoc / ViewState / App 本体と `app` | ~1,300 | 全テーマ共有 |
| util.inc | utilities: path/font/nowSec/readFileBytes | ~140 | ハーネス基盤 |
| compare.inc | AB/compare: slot 群、follow、cycleCompare、escProbe | ~800 | ImageView/compare |
| export.inc | export: PNG encode、clipboard、dot-by-dot | ~240 | ImageView/compare |
| remote_client.inc | remote 前方宣言群 + rfWorker/mWorker(fetch・measure) | ~1,120 | remote client |
| annotations.inc | annotations | ~75 | ImageView |
| plugin_glue.inc | plugin glue | ~250 | ローダ |
| loader_npz.inc | .npz (zip) | ~1,670 | ローダ |
| loader_npy_raw.inc | npy loader + raw loader + dynamic crop | ~410 | ローダ |
| session.inc | session save/load + prefs | ~1,720 | session/prefs |
| sequence.inc | sequences (連番) + folder pick / reader / npz pick モーダル | ~2,390 | ローダ |
| temporal_model.inc | temporal analysis(計算側) | ~125 | パネル/解析 |
| open_dispatch.inc | open dispatch(rb 系を除く。§3) | ~1,010 | ローダ |
| cli.inc | CLI: parseCli / printUsage / remoteSelfTest | ~1,400 | ハーネス基盤 |

### core/ui/ — パネル 1 枚 = ファイル 1 個

| ファイル | 中身(節名 / 関数) | 行数 | 板テーマ |
|---|---|---|---|
| canvas.inc | UI 節冒頭: raw モーダル、tiles 幾何、drawCanvas | ~1,680 | ImageView |
| inspector.inc | AB legend/band、drawAnalysisPlots、drawInspector | ~830 | ImageView/compare |
| panel_histogram.inc | drawPanelHistogram | ~590 | パネル/解析 |
| panel_projection.inc | drawPanelProjection | ~1,440 | パネル/解析 |
| modal_series.inc | drawSeriesModal(+ selftestMakeSeries は注記付きで同居) | ~150 | パネル/解析 |
| panel_linearity.inc | drawPanelLinearity | ~460 | パネル/解析 |
| panel_temporal.inc | server/browse temporal、temporal AB、frame-lin 節、drawPanelTemporal | ~2,380 | パネル/解析 |
| panel_rois.inc | drawPanelRois | ~250 | パネル/解析 |
| panel_analysis.inc | drawPanelAnalysis | ~480 | パネル/解析 |
| modal_derive.inc | drawDeriveModal | ~350 | パネル/解析 |
| file_list.inc | drawFileList + sequence モーダル | ~820 | ImageView |
| menus.inc | menu bar / dialogs 節: title bar、menu、remote open/error、messages、help | ~770 | 横断(編集は行単位) |

### core/browse/ — #47 の持ち出し単位(§3)

| ファイル | 中身 | 行数 | 板テーマ |
|---|---|---|---|
| instances.inc | rb 前方宣言 + instance 管理(rbMain/rbActive/rbNewInstance/破棄) | ~150 | Browse |
| nav.inc | rbWorker、rbEnqueue、tree、history、rbParseSpec(open dispatch 節の中の連続塊) | ~660 | Browse |
| panel.inc | rbNameCmp/rbAddRows/rbSortShown … drawPanelRemote 一式 | ~2,490 | Browse |

### core/analysis/ — #84 の生誕地(分割時点では空)

#48/#84 の実装(4 種のアナライザ、AnalysisSet、レジストリ)はここに **実 TU として生まれる**。
モノリスに足してから移す往復をしない。前提は P6(state.h が自己完結していること)。
Series Analysis パネルは core/ui/panel_series.inc として新規に生まれる。

### core/selftest/ — 1 テスト 1 ファイル(§4)

| ファイル | 中身 | 行数 |
|---|---|---|
| util.inc | needWindow、fixture 生成、roiStatsSelftest、abEqSelftest 等(main 直前の補助群) | ~530 |
| picker.inc / browse.inc / export.inc / …(~25 個) | main() 内の各 `if (!g_xxxSelftest.empty())` ブロック、逐語 | 計 ~9,800 |

## 3. Browse の境界 (#47)

**持ち出す単位** = `core/browse/`(§2)+ 既に独立している `core/remote.h/.cpp` + `core/remote_proto.h` + `core/serve.cpp` + `core/serve_main.cpp`。
プロトコルと peer は最初から別ファイルなので、S3 で core/browse/ が確定した時点でファイル境界の手術は終わる。

**viewer 側に残るもの**: openRemote / openRemoteStack / promotePreview(開いた結果を viewer に登録する側)、
session の browse place 復元、メニューの入口。「一覧して選ぶ」までが Browse、「開いて何をするか」が viewer。

**P7 の継ぎ目**: panel.inc の中に viewer 呼び出しが約 36 箇所ある(openRemote / openStack 系 / toast / selectImage / prefs)。
P7 でこれを `BrowseHost` 構造体(コールバック集)に束ね、browse 側は BrowseHost と remote_proto だけを見る。
`App::BrowseInstance` ほか rb 系ネスト型は core/browse/browse_state.h へ移し、App 側には
`using BrowseInstance = browse::Instance;` の alias を置く。既存の `App::BrowseInstance` 参照は全て無傷で通り、churn はゼロ。

S3 の時点で 36 箇所に `// BrowseHost 候補` の注記を置く。P7 はその注記を潰していく作業になる。

## 4. selftest と NOGL 契約

selftest 本体は main() の**中**にあり(起動列と prefs/plugins/CLI の後、フレームループの前)、
file-scope の static 群と main() のローカルの両方を見る。関数に括り出すと引数設計が要り、逐語移動でなくなる。

置き場: `core/selftest/<テスト名>.inc`、**main() の本文中の元の位置で `#include`**。
関数本文中の `#include` は合法で、ローカルも見えるので、移動は逐語のまま。
新しい selftest を足す作業は「新ファイル 1 個 + main() に 1 行 + CMakeLists に 1 行」になり、他人と衝突しない。

- browse-keys だけはフレームループに 18 箇所編み込まれているので、ループ内部分は背骨に残す。ループ前の準備ブロックだけ切り出す。
- --remote-selftest はウィンドウ生成前に分岐する独立関数(cli.inc に同居)。
- **NOGL 契約は無傷で生き残る。** ラベルの宣言場所は CMakeLists.txt の viewer_selftest() 行のまま(唯一の場所)。
  ラベルは実測で決めた値であり、S 段階は挙動同一性を証明して移動するので実測は失効しない。
  再実測が要るのは「テストの描画経路を変えた」ときだけで、それは分割ではなく機能変更の義務。

## 5. 移動の順序

各増分はエージェント 1 人・1 増分。検証は毎回同じ 3 点:

1. **プリプロセス同一性**: `g++ -E -P <ビルドと同じフラグ> core/main.cpp | sha256sum` が移動前後で一致。
   フラグは `ninja -t commands viewer` の main.cpp 行から取る。一致すればコンパイラへの入力が同一という証明で、
   これが通る増分は挙動を変えようがない(main.cpp に `__FILE__`/`__LINE__` は 0 箇所であることを確認済み)。
2. **ビルド緑**(viewer / viewer-serve / plugins)。
3. **スイート 30+1 緑 + stderr バイト一致**(移動前バイナリと突き合わせ。挙動保存移動の家の基準)。
   レビューは `git diff --color-moved=zebra` で「移動と #include 行以外の差分ゼロ」を見る。

| 増分 | 中身 | 出る行数 | main.cpp 残 |
|---|---|---|---|
| S0 | ゲート: ref-integrate が main に入り CI 緑(§7) | — | 38.9k |
| S1 | selftest ~26 ファイルへ(main() 内ブロック + 直前の補助群) | ~10.3k | 28.6k |
| S2 | core/ui/ 12 ファイルへ | ~10.2k | 18.4k |
| S3 | core/browse/ 3 ファイルへ + BrowseHost 候補 36 箇所に注記 | ~3.3k | 15.1k |
| S4 | loaders + sequence + session + open_dispatch | ~7.3k | 7.8k |
| S5 | state / util / compare / export / remote_client / annotations / plugin_glue / cli | ~5.2k | ~2.6k |
| P6 | state.inc → 自己完結の state.h、`extern App app`(定義は背骨に残る) | — | — |
| P7 | Browse 実 TU 化: browse_state.h(alias 方式)+ BrowseHost + CMake に 2 ファイル追加 | — | — |

- **S1〜S5 + P6 は凍結ウィンドウ**: この間 main.cpp を触る並列作業を出さない。各増分は機械的な切り貼りで、直列に詰めて 2〜3 日。終わった時点で並列展開を開始する。
- P6 の検証はハッシュではなくスイート(include の並びが変わるため)。P7 の検証は 2〜3 に加え、browse / localbrowse / browse-keys / browse-dbl の出力バイト一致を明記する。
- P7 は凍結の外。並列展開と同時期に 1 エージェントで走らせてよい(触るのは core/browse/ と state.h だけ)。

## 6. 動かさないもの

- **フレームループと起動・停止列**(GLFW/GL/ImGui backend の初期化、dock builder、crash handler、teardown の join 順)。
  順序それ自体が仕様であり、機能追加でここを編集することはほぼ無い。動かしても衝突面が減らない。
- **idle throttle 一式**(g_wakeUntil / wakeUi / redrawNow)。ループの一部。コールバックが main のローカルを参照で掴んでいる。
- **browse-keys のループ内ドライバ**(§4)。
- **`App app` の定義**(宣言は P6 で state.h へ、定義は背骨)。
- 既に独立しているもの(serve / remote / imagefile / ui_theme / adapter / window_frame / plugins)はそのまま。

## 7. 他ブランチとの順序

- **ref-integrate が先。** あのブランチは main.cpp に ~1,966 行の差分を持つ。分割を先にやると、
  1 ファイルのマージが 40 ファイルの手作業マージに化ける。S0 はこれのゲート。
- **#84(spec-analysis-layers)は docs のみ**で干渉しない。ただし #48 の実装開始は P6 の後にする。
  そうすれば core/analysis/ に実 TU として生まれ、モノリスへの追記→引越しの往復が消える。
- **待機中のブランチ群**(media-openexr / media-video / 形式戦略ほか、main.cpp に差分を持つもの)は、
  S 段階の後の rebase が手作業になる(git は「関数がどのファイルへ行ったか」を追わない)。
  復元の手順は機械的: `git diff main...branch -- core/main.cpp` の各 hunk を、節名で対応する断片ファイルに当て直す。
  それでも、**入れられるものは S0 までに入れるのが安い**(§9 判断 4)。

## 8. 引用の作法(分割後)

行番号引用は腐る(今週の監査で 10 件)。分割後の正しい引用は **ファイル + 節見出し/関数名**:
`core/browse/panel.inc drawPanelRemote` のように。board(docs/tasks.csv)の既存の行番号引用は、
その行を次に触るときにファイル引用へ書き換える。一斉置換はしない。

## 9. 判断(ユーザーに訊く 4 件)

**判断 1 — Browse は今すぐ別リポジトリにするか。**
- 推奨: しない。今回は「持ち出せる形」(core/browse/ + remote + serve、P7 の継ぎ目)まで。
- 理由: 利用者が viewer 1 つの間、別 repo は submodule の往復を並列作業に足すだけ。第二の利用者が現れた日に、この単位をそのまま持ち出せる。

**判断 2 — selftest は main() 内 #include のままでよいか。**
- 推奨: よい。1 テスト 1 ファイル、位置は main() 内の元の場所(§4)。
- 理由: 逐語移動なので挙動も NOGL 実測も無傷。関数化・TU 化は引数設計を要求し、買うものが今は無い。

**判断 3 — P 段階はどこまでやるか。**
- 推奨: P6(state.h)と P7(Browse)だけ予定に入れる。パネル・ローダの実 TU 化は必要が生じた時に個別に。
- 理由: 衝突面は S 段階で消えている。残る便益はコンパイル時間だけで、痛くなってから測って払えばよい。

**判断 4 — 待機中の media 一括(EXR / 形式戦略 / y4m)は分割の前か後か。**
- 推奨: 準備ができているなら S0 の前に main へ。間に合わないなら分割後に §7 の手順で rebase。
- 理由: 分割前なら通常のマージ。分割後は hunk の当て直しが手作業になる。2026-08-04 の「揃えて一度に検証」の決定はどちらでも守れる。
