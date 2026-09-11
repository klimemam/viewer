# 2026-09-08 #230 phase④ model boundary 検証結果

- 対象実装コミット: `02e4695647c8cb75aecf479b7562572bea238c78`
- 比較基準コミット: `e09e018c2c386f01430dda31f0262d6f8f0981b4`
- ブランチ: `fix/model-boundaries-230-final`
- 対象: [PR #240](https://github.com/klimemam/viewer/pull/240) / #230 phase④
- carrier 裁定: Issue #242 の
  [ユーザー裁定 B](https://github.com/klimemam/viewer/issues/242#issuecomment-5584445947) と
  [exact-v3 解釈](https://github.com/klimemam/viewer/issues/242#issuecomment-5584561821)
- 実行機: `DESKTOP-DI1MT7O`
- 環境: Windows、MinGW-w64 UCRT / GNU C++ 16.1.0、CMake 4.3.2、
  Ninja 1.13.2、Release、Python 3.11.0、NumPy 1.26.4
- GPU / OpenGL probe: NVIDIA GeForce RTX 3090、OpenGL 4.6.0 NVIDIA 576.02
- ビルドディレクトリ: `build-mingw`
- 再実行仕様: [../functional.md](../functional.md) §4.1

この文書は上記1コミット、識別した1台、2026-09-08の1回の実施を固定する。
後日の実装状態に合わせて書き換えない。

---

## 1. 結論

phase④の実装を最新mainへ統合した対象コミットで、全target build、CTest **62/62**、
公式selftest driver **61/61**、Python adapter **48/48**、文書checkerをすべて通した。
公式driverが数えるskip、quarantine、run-set外はいずれも0だった。

carrierはwriter/readerともexact-v3である。v1/v2/v4以上はtree/pixels生成前に全体拒否する。
carrier generationとremote wire protocolは別の番号空間であり、typed axesを運ぶwireだけが
protocol 15を要求する。mixed SeriesはStackとstandalone Frameを同じmember語彙で扱う。

11個の契約を一つずつ意図的に壊すcontrolled mutationを実施した。各mutationで担当する
assertionがREDになり、隣接する対照は記載どおりGREENのままだった。各変更を直ちに復元し、
同じ対象suiteがGREENへ戻ることを確認した。最終ソースにmutationは残っていない。

## 2. REDの由来を偽らない

初期実装と回帰テストは同時に作成されたため、実装前コミットに対するhistorical REDは
存在しない。本書はfail-firstを主張しない。

§4のREDはすべて、最終製品treeへ契約を一つだけ破る変更を加えた
**post-implementation controlled mutation calibration**である。比較基準コミットに同じ欠陥が
存在した証拠ではなく、追加assertionが空虚にPASSしていないことの校正である。
RED出力は各実行直後に本表へ転記した。CTestの`LastTest.log`は復元後GREENを同じsuiteで
確認するたびに上書きされるため、mutation版のraw logは別artifactとして保存していない。

## 3. 実行コマンド

主要コマンドは次のとおり。CMake / CTestにはWinLibs内の実体を用いた。

```powershell
& 'C:\Users\hish\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\cmake.exe' --build build-mingw --target all -j 2
& 'C:\Users\hish\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\ctest.exe' --test-dir build-mingw -C Release --output-on-failure -j 1
& 'C:\Users\hish\AppData\Local\Programs\Python\Python311\python.exe' tools\import\test_adapters.py
& 'C:\Users\hish\AppData\Local\Programs\Python\Python311\python.exe' tools\check_doc_links.py --selftest
& 'C:\Users\hish\AppData\Local\Programs\Python\Python311\python.exe' tools\check_doc_links.py
git diff --check
```

公式driverはGit Bashから、同じMinGW / Pythonを`PATH`先頭に置いて実行した。

```bash
tools/run_selftests.sh build-mingw
```

mutation calibrationでは変更に応じて`viewer`または`viewer-serve`を再buildし、
`selftest.rreader`、`selftest.rnpz`、`selftest.seriespanel`を`-R`で個別実行した。

## 4. controlled mutation calibration

| ID | 一時変異 | REDと対照 | 復元後 |
|---|---|---|---|
| M1 exact carrier gate | 共通predicateを`version == CARRIER_VERSION`から`version <= CARRIER_VERSION`へ変更 | `R7f3 stream v1/v2 are refused identically before local/peer pixels`と`R19h valid i4 versions 1/2/4 reach the distinct exact-v3 gate on both doors`がFAIL。後段を汚した二次結果として`R7f4`もFAIL | `rreader` / `rnpz` 2/2 PASS |
| M2 remote scatter | `sameOrder`を`false`にしてbulk-copyを無効化 | `R26a0 HW/HWC/FHW/FHWC each retain one memcpy with exact bytes`がFAIL。FCHW loopの`R26a1`はPASS | `rnpz` PASS |
| M3 C上限 | `c > 4`判定を無効化 | `R7f2`とFrame/HWC、Frame/CHW、Stack/FHWC、Stack/FCHWの4件すべての`R7f6` C=5がFAIL。4件すべての`R7f5` C=4はPASS | `rreader` PASS |
| M4 session drain | typed Series復元前のqueue drainを除去 | `R21g drained remote session restores writer-owned Series facts and axes`だけがFAIL | `rreader` PASS |
| M5 materialisation run | session slot選択の`savedRun`比較を無効化 | `R21h4`、既存反復Readerの`R18b/c/e/g/i/j`、remote NPZの`R26d6`、local NPZの`R26e4`がFAIL | `rreader` / `rnpz` 2/2 PASS |
| M6 keyed source sharing | `srcShareable`の`remoteKey`除外を無効化 | `R23a large same-origin outputs of two Readers stay distinct after full landing`がFAIL。`R23b/c/d`と`rnpz`全体はPASS | `rreader` PASS |
| M7 materialiser issuer | Viewer-NPZ再利用時の`FromNpz`条件を除去 | `R26d7 Reader-kind docs with the same URL/member cannot dedupe an NPZ`だけがFAIL。`R26d6`はPASS | `rnpz` PASS |
| M8 current client → pre-15 peer | client側keyed protocol 15門を無効化 | `R21j`、再接続full fetchの`R23c`、MEASUREの`R23d`がFAIL。canonical v3の`R21k`と`R23a/b`はPASS | `rreader` PASS |
| M9 pre-15 client → current peer | peer側`requiresTypedAxes15`判定を無効化 | cached named layoutの`R21m`とfresh narrow Stackの`R21q`がFAIL。canonical cacheの`R21n`とtyped wire不要controlの`R21o`はPASS | `rreader` PASS |
| M10 mixed Series Move | standalone `frameUid` memberのMoveを無効化 | `A1b Move sends stack + standalone frame to one target batch`と`A1b audit after mixed Move`がFAIL。member identity / control / roleの対照はPASS | `seriespanel` PASS |
| M11 mixed Series Close | standalone `frameUid` memberのCloseを無効化 | `A1b Close removes every stack/frame member and the series`、`leaves the unrelated control open`、`unbinds removed roles and keeps the control binding`がFAIL。toastと構造auditはPASS | `seriespanel` PASS |

## 5. 最終GREEN

| 検査 | 実測 | 判定 |
|---|---|---|
| 全target build | `viewer`、`viewer-serve`、plugin、test executable、`mkexr`、EXR fixtureを含めexit 0 | PASS |
| 全CTest | **62/62 passed、0 failed、179.65 s** | PASS |
| 公式selftest driver | OpenGL probe成功。**61 ran、0 skipped、0 quarantined、0 in no run set**。179.51 s | PASS |
| Python adapter | **48/48 passed、0 failed** | PASS |
| 文書checker selftest | `doc-check selftest: lifecycle cases 1-4 and stub/fragment gates green` | PASS |
| 文書checker全体 | `doc-check: ok` | PASS |
| mutation残骸 | `CONTROLLED MUTATION` / 製品コードのmutation用`false &&`は0件。製品コードは対象コミットと内容差分0 | PASS |
| whitespace | `git diff --check` exit 0、whitespace error 0 | PASS |

buildは成功したが、このrunの合格条件に警告ゼロは含めていない。`canvas.inc`の
misleading-indentation、未使用の`npzExtractPrefix`、未使用の`autoRange`に関する既存警告が
出た。

### 5.1 prefs cleanup

公式driver後の隔離home `rreader` / `rnpz`で、両`prefs.txt`の`readerfor`は0行だった。
その状態から同じ2 suiteを再実行して2/2 PASSし、実行前後で両ファイルとも次のSHA-256から
変化せず、再実行後も`readerfor`は0行だった。

```text
334E4400B7341672595A61B5ED890D4871DA8F89C3A0826AA54B72FA8485E25F
```

この確認は今回追加した`rreader` / `rnpz`の対象URLに限定する。他suiteのprefs全般を
cleanup済みとは主張しない。

## 6. 未実施・skip

- 公式driverが数えるCTest単位ではskip 0、quarantine 0、run-set外 0だった。
- `rreader`内のsymlink小項目R19a〜R19oは、このWindows hostがfile/root symlink作成へ
  `Function not implemented`を返したため内部skipとなった。62/62をこの15小項目の実行済みとは
  読まない。
- Linux / macOS / Wayland実機およびGUI目視は、このWindowsローカルrunでは実施していない。
- PR #240の更新後CIは、このローカルrunの外側であり本結果のPASS数へ含めない。
