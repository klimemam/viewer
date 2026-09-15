# 2026-09-12 #259 typed channel 検証結果

- 対象実装コミット: `c7aab8f8e51a81a9728de4799ae9d697a7ab2242`
- 比較基準: `5d46998f0962bac725286e59d4bca5deeec170a5`（PR #240 を含む main）
- 対象: [Issue #259](https://github.com/klimemam/viewer/issues/259)
- 実行機: `DESKTOP-DI1MT7O`、Windows 11 Pro `10.0.26120`
- 環境: MinGW-w64 UCRT / GNU C++ 16.1.0、CMake 4.3.2、Ninja 1.13.2、Release、
  Python 3.11.0、NumPy 1.26.4
- GPU / OpenGL: NVIDIA GeForce RTX 3090、OpenGL 4.6.0 NVIDIA 576.02
- worktree: `viewer_work/codex-typed-channels-259`、build: `build-mingw`
- 再実行仕様: [../functional.md](../functional.md) §2・§4.1

この記録は上記コミットのコードに対する2026-09-12の実施を固定する。

## 1. 修正前の再現と修正後

比較基準に新しいテストを追加し、製品のコピー判定を変更する前に
`selftest.rreader` と `selftest.rnpz` を逐次実行した。失敗した表明は次の2件だけだった。

```text
R26a0b C=1 CHW uses one memcpy with exact bytes and guards               FAIL
R26a0c C=1 FCHW uses one memcpy at the correct frame offset              FAIL
```

同じ実行で既存の連続コピー `R26a0` と複数channelの転置 `R26a1` はPASS。
新しいC=4の `R7f5a–e` ×4形状、計20表明もすべてPASSし、rreader全体が成功した。
CTestは1/2失敗、終了コード8、40.09秒。これは実際の修正前REDである。

channel数1を連続コピーへ通す修正後、同じ対象テストは **2/2 PASS、37.08秒**。
この修正後実行は、対象実装コミットと同じ製品・テストコードで行った。

C=1テストは実際に使ったコピー経路、全bytes、前方guard、FCHWの2枚目への書き込み位置を
確認する。C=4テストはFrame/HWC・Frame/CHW・Stack/FHWC・Stack/FCHWの手書きv3 carrierを
localと実peerのRUN経由で開き、kind・shape・発行keyと全サンプルを独立したliteral期待値で
照合する。Frameは24サンプル、Stackは2枚合計48サンプルで、第4channelまで検査する。

## 2. 最終検証

cleanな対象実装コミットから標準driverで再ビルドし、全selftestを逐次実行した。

| 検査 | 結果 |
|---|---|
| 全target build | PASS。既存のcanvas indentation、unused npzExtractPrefix / autoRange警告あり |
| OpenGL probe | PASS |
| 標準driver | **61 ran、0 skipped、0 quarantined、0 in no run set、197.48秒、PASS** |
| Python adapter | **48/48 PASS** |
| 文書checker selftest / full | PASS |
| `git diff --check` | PASS |
| rreader / rnpzの隔離prefs | 両方とも `readerfor` 行は0件 |

CTest単位のskipとは別に、Windows上でsymlink作成が `Function not implemented` となるため、
rreader内部の既存 `R19a–R19o` はSKIP。今回のC=1／C=4追加テストに未実行項目はない。
Linux/macOSの実行結果はこのWindows記録に含めない。

## 3. 実行コマンドとログ

worktreeをカレントディレクトリとし、PATH上の上記MinGW / Pythonを使用した。
依存ソースは主checkoutの `build-mingw/_deps/*-src` をCMakeの
`FETCHCONTENT_SOURCE_DIR_*` で参照し、生成物はこのworktree内に置いた。

```powershell
cmake --build build-mingw --target all -j 4
python tools/gen_testdata.py
ctest --test-dir build-mingw -C Release -R '^selftest\.(rreader|rnpz)$' -V -j 1
python tools/import/test_adapters.py
$env:VIEWER_TEST_JOBS='4'
& 'C:/Program Files/Git/bin/bash.exe' tools/run_selftests.sh build-mingw
python tools/check_doc_links.py --selftest
python tools/check_doc_links.py
git diff --check
```

実行直後のログはローカルの `build-mingw/channels-red.log`、`channels-green.log`、
`full-suite.log`、`adapters.log` に保存した。これらのbuild生成物はGit管理外であり、
恒久記録は本書の表明・結果とする。
