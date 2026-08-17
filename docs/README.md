# docs — 文書ポータル

この repo の文書の入口。**正典は一つ**を守るために、ここが「何がどこにあり、
衝突したらどちらが勝つか」を宣言する。

- 構造の設計: issue #55。材料は [docs-restructure-handoff.md](docs-restructure-handoff.md)
  (codex 案) と [docs-split-review.md](docs-split-review.md) (旧分割案の全件レビュー)。
- 課題の正典は [tasks.csv](tasks.csv) (板)。`python tools/board.py` が読む。
- **リポジトリ直下の README.md は文書表を持つが、その張り替えは合図待ち。**
  今この瞬間の入口は本書である。

> **この波 (#55 波0) ではファイルを1本も動かしていない。** 下の分類は
> **読み方の宣言**であって、ディレクトリの現状ではない。行き先の列がある文書は
> 波1以降でそこへ移る。移動が起きるたびに §9 の対応表に行が増える。
> 例外は §11 の第二段分割で新設した `background/` の2本だけで、これは
> 移動ではなく**節の切り出し**である。

---

## 0. 読む順番 (初見)

1. [terminology.md](terminology.md) — frame / stack / series / batch。**語彙の正典**。
2. [analysis-layers.md](analysis-layers.md) — 4層と4種のアナライザ。**住所の正典**。
3. [manual.md](manual.md) — 実際の操作。
4. [tasks.csv](tasks.csv) — いま何が開いているか。**課題の正典**。
5. 触る機能の節 (§2) を、その feature の行から。

---

## 1. 製品全体 (docs/ 直下 = project 層)

**判断規則**: 書き換えたときの再検証が **1 機能に閉じないもの** — 機能間の契約・
語彙・データモデル — が直下に住む。閉じるものは機能側 (§2)。迷ったら機能側に置き、
直下から引く。

| 文書 | 何を決めるか | 状態 |
|---|---|---|
| [terminology.md](terminology.md) | frame / stack / series / batch の定義と不変条件 | 正典・据え置き |
| [analysis-layers.md](analysis-layers.md) | Frame/Stack/Series/AnalysisSet の 4 層と 4 種のアナライザ (#48) | 正典・据え置き |
| [reference-design.md](reference-design.md) | frame の共有参照化 (単独所有 `seqId` の置き換え) | 確定・据え置き |
| [series-plan.md](series-plan.md) | series を第4の層としてコードへ入れる計画 | 実装済み。波3で機構と経緯に分ける |
| [split-plan.md](split-plan.md) | core/main.cpp の TU 分割 (#47 包含) | 実施済み。完了確認後 background へ |
| [tasks.csv](tasks.csv) | 課題台帳 (5分類: 対応済み / 進行中 / 残課題 / 暫定対応したが後で直す / レビュー必要項目) | 正典・据え置き (`tools/board.py` が直読み) |

---

## 2. 機能 (行き先 = `features/<name>/`。今はまだ直下)

各機能の「正典」列が、**衝突したらどれが勝つか**である。

### compare — 波1で移動予定

| 文書 | 役割 |
|---|---|
| [compare-n-design.md](compare-n-design.md) | **仕様の正典** (同文書冒頭が「衝突したら本書が勝つ」と宣言)。#60 |
| [compare-n.md](compare-n.md) | 指針 + §11 着地台帳 + §12 開いている判断 1–7。C++ 8 箇所が §番号で引く |
| [ab-stats-plan.md](ab-stats-plan.md) | 数値側 A/B 統計パネルの計画 |

検証: selftest **N 群** (`--abstats-selftest`) / **R 群** (`--roistats-selftest`) /
**T 群** (tile)。一覧の正典は CMakeLists (`tools/run_selftests.sh` 冒頭が
「there is no second copy」と明記)。

### browse

| 文書 | 役割 |
|---|---|
| [browse-extract-design.md](browse-extract-design.md) | **現行設計** — 100点化と持ち出し (#47) |
| [browse-as-file-manager.md](browse-as-file-manager.md) | 「接続するもの」ではなく「場所を見るもの」— あるべき形 |
| [browse-topbar-design.md](browse-topbar-design.md) | 上部領域の設計。**§5 に未決、§10.8 に command palette の母体** (板 66 / 251) |
| [browse-topbar-mockup.html](browse-topbar-mockup.html) | 上の案の絵 |
| [picker-ux.md](picker-ux.md) | Select stacks ダイアログ |

### analysis

| 文書 | 役割 |
|---|---|
| [flat-field-stats.md](flat-field-stats.md) | **σ_fpn / DSNU / PRNU の推定量と detrend の正典** (確定 2026-08-09, #57)。C++ 49 箇所が引く |
| [stats-taxonomy.md](stats-taxonomy.md) | 「基本 stats」と「追加 measurement」の境界 (ddof 規約を含む) |
| [measure-ux.md](measure-ux.md) | 測定体験の理想形 (出力はそれ単体で証拠になる) |

経緯: [background/analysis/flat-field-stats.md](background/analysis/flat-field-stats.md)

### histogram

| 文書 | 役割 |
|---|---|
| [histogram-select-design.md](histogram-select-design.md) | 値域ハイライト v1 の仕様 (#68) |

### adapters

| 文書 | 役割 |
|---|---|
| [input-adapters.md](input-adapters.md) | **native の範囲とリーダ契約**。C++ 44 箇所が §番号で引く (§4 = リーダ契約) |
| [npz-design.md](npz-design.md) | `.npz` に key が入っているときの扱い |
| [reader-analysisset.md](reader-analysisset.md) | Reader 発の AnalysisSet — 返り値・参照・部分束縛・セッション |

### remote

| 文書 | 役割 |
|---|---|
| [remote.md](remote.md) | `ssh://` でリモートのデータを手元から見る |
| [remote-headerless-design.md](remote-headerless-design.md) | ヘッダ無し RAW のレシピがリンクを渡る設計 (G1) |

`remote-reader-design.md` (#180 裁定 B、板 240) は**未着地**。生まれるときは
`features/remote/` に直接生む — それが remote 波の引き金になる (§10)。

### export / watch / settings / media / theme

| 文書 | 役割 |
|---|---|
| [export-design.md](export-design.md) | Temporal + H/V profile を1回で持ち出す |
| [watch-design.md](watch-design.md) | 変化検知・通知・再読込 (項目20)。§11–16 が実装から返ってきたもの |
| [settings-inventory.md](settings-inventory.md) | 何が設定になりうるか、どう分けるか |
| [preferences-panel-design.md](preferences-panel-design.md) | Preferences パネル (#50 段2) |
| [media-support.md](media-support.md) | OpenEXR (実装済 #53) / 動画 §2 / click-to-open UX |
| [video-support.md](video-support.md) | **動画の現行結論** — v1 は y4m 輝度のみ。lossy は名指しで拒否 |
| [imgui_modern_design.md](imgui_modern_design.md) | "Aurora" テーマ |

経緯: [background/media/media-support.md](background/media/media-support.md)

---

## 3. 検証 (行き先 = `verification/`)

| 文書 | 役割 | 行き先 |
|---|---|---|
| [verify-matrix.md](verify-matrix.md) | 形式 13 × 戸 9 × 操作 7 の**生きた表**。セルは根拠キー ([T:]/[C:]/[P]) を運び、門の列は selftest fmtgate F4 が保つ | `verification/matrix.md` — **割らない** |
| [verify-functional.md](verify-functional.md) | 機能検証。手順・期待 (現行) と 2026-08-03 / 08-04 の実施記録が同居 | `verification/functional.md` + `results/20260803-functional.md` |
| [verify-ui.md](verify-ui.md) | UI 検証。同上 | `verification/ui.md` + `results/20260803-ui.md` |

**Results の単位は「検証 run」** = 1 コミット × 1 機械 × 1 マトリクス実施。
`results/<YYYYMMDD>-<何を>.md` の冒頭に commit / 機械 / 走らせた物を必ず書く。
過去の実行結果は書き換えない。もう1つの形態が**機能内着地台帳**
(compare-n §11 型 — 本文に勝っている間は現行文書内、本文へ吸収された時点で背景へ)。

テスト一覧の正典は CMakeLists であって文書ではない。

---

## 4. 手引き (行き先 = `guides/`)

| 文書 | 役割 |
|---|---|
| [manual.md](manual.md) | 操作マニュアル |
| [startup.md](startup.md) | 起動手順 |

## 5. 参照契約 (行き先 = `reference/`)

**基準は「消費者がこの repo の外にいる契約」**。実装との一致を必須にする。

| 文書 | 読者 |
|---|---|
| [abi-v3.md](abi-v3.md) | プラグイン作者 |
| [analyzers.md](analyzers.md) | 出荷プラグインの目録 |

`input-adapters.md §4` (リーダ契約) の抽出は第一波ではやらない — C++ 44 箇所が
§番号で引き、§4.13.1 は進行中の設計 (#180) の根拠だから。再訪条件は §10。

## 6. 背景 (`background/` — 非隠しディレクトリ)

「なぜ他の形でないか」「もう役目を終えた記録」。**現行仕様はここに置かない。**
背景ファイルは冒頭 1 行が `現行ドキュメント: X の背景 — 要旨`、
現行側の末尾が `経緯と検討: [...]`。

| 文書 | 中身 | 行き先 |
|---|---|---|
| [background/analysis/flat-field-stats.md](background/analysis/flat-field-stats.md) | #57 の判断record (7項 + 分離フィットの追加2項) と実施済みの実装計画 | **済 (2026-08-17)** |
| [background/media/media-support.md](background/media/media-support.md) | EXR リーダの候補比較と、覆された tinyexr 推奨 | **済 (2026-08-17)** |
| [browse-inventory.md](browse-inventory.md) | 再設計前の棚卸し (行番号は当時のもの) | `background/browse/` |
| [browse-display-candidates.md](browse-display-candidates.md) | 判定なしの素材表 | `background/browse/` |
| [layers-plan.md](layers-plan.md) | 実施済みの実装計画 | `background/project/` |
| [review-new-code.md](review-new-code.md) | e38de8b 以降のコードレビュー記録 | `background/reviews/` |
| [adapter-transport-review.md](adapter-transport-review.md) | #44 / #45 合同レビュー (#44 は 2026-08-14 に closed) | `background/reviews/` |
| [flat-field-stats-review.md](flat-field-stats-review.md) | #57 の締めの記録 | `background/reviews/` |
| [review-lenses/session-20260806.md](review-lenses/session-20260806.md) | session / persistence lens | `background/reviews/` |
| [todo-open.md](todo-open.md) | 2026-07-30 時点の未着手一覧。**板に無い項目の統合監査が先** (板 参照) | `background/project/` (凍結 → 監査 → 移送) |
| [docs-split-review.md](docs-split-review.md) | 旧分割案の全件レビュー | `background/documentation/` |
| [docs-restructure-handoff.md](docs-restructure-handoff.md) | codex の構造案 | `background/documentation/` |

## 7. 素材と生成物 (据え置き)

| パス | 中身 |
|---|---|
| `img/` | スクリーンショット (imgui_modern_design が引く) |
| `diagnostics/` | 証跡の置き場。**動かさない** — `.github/workflows/build.yml` が `diagnostics/auto/` へ書く CI の契約 |

---

## 8. 文書ポリシー — 移行中も正典が一意であることを守る 8 条

1. **move は `git mv` + stub を同一コミット。** stub は 3 行固定
   (`# (moved) <旧名> → <新パス>` / 移動日 / 「本文はここに無い」)。本文複製ゼロ。
2. **分割は節単位の移動であって要約の複製ではない。** 旧位置には**節名と新所在だけ**
   残す。背景側は §6 の規約ヘッダを継承する。
3. **勝敗宣言。** 1 機能に仕様文書が 2 本ある間は、どちらが勝つかを両方の冒頭と
   §2 の表に書く。
4. **板の規律。** 対応済み行の参照列は書き換えない (歴史)。live 行 (残課題 / 進行中 /
   暫定 / レビュー必要項目) は移動と同一 PR で更新する。live 行が名指す**未作成**の
   成果物は、作成時点の正典配置で生む。
5. **移動 PR は move-only。** 内容編集を混ぜない。混ぜると rename 追跡が新規内容を
   黙って運ぶ。
6. **C++ 参照は別 PR。** ただし文書 PR と C++ PR の**両方**が main に入るまで
   stub を消さない。
7. **リポジトリ直下の README.md は合図まで触らない。** 従って README が引く 11 本の
   stub は合図より前に消せない。README への変更は合図待ちキューに積む。
8. **各波の完了条件** = リンク検査 (§12) が緑 + 板の live 行の参照が全て新パス +
   §9 の対応表に当該波の全行が載っている。

**未決の扱い** (これが旧分割案の最大の×だった): 未決リストと判断record の
**再訪条件**は背景に落とさない。未決は現在の仕事、再訪条件は現在の縛りである。
機能の未決節は「板の row を指す」か「板未登載」を名乗る。

---

## 9. 旧 → 新 対応表 (恒久)

**stub が消えてもこの表は消さない。** 板の対応済み行・background の記録・
issue 本文・git 履歴は旧パスで引き続けるので、1 ホップで解決できる必要がある。

| 旧 | 新 | 波 | 日付 |
|---|---|---|---|
| flat-field-stats.md の「実装の進め方」「判断record」 | [background/analysis/flat-field-stats.md](background/analysis/flat-field-stats.md) | 0 (第二段分割) | 2026-08-17 |
| media-support.md の「候補比較」 | [background/media/media-support.md](background/media/media-support.md) | 0 (第二段分割) | 2026-08-17 |

(以降の波でファイルが動くたびに行を足す。)

## 10. 例外表 — 存在しないのに引かれている docs パス

§12 の宣言2 はこの表を見る。**例外はスクリプトの中ではなくここに置く** —
例外そのものを文書化するため。

| パス | 実体 | 予約先 |
|---|---|---|
| media-formats.md | branch 上 (未マージ)。C++ 3 箇所 (`core/rawread.h` ほか) + 板の参照列 3 | 上陸は現行パスで受け、media 波で `features/media/` へ |
| python-plugins.md | branch `plugin-python-study` (未マージ)。板の参照列 2 | 上陸後 `features/plugins/` (このとき features/plugins を新設) |
| remote-reader-design.md | **未着地の成果物** (板 240、#180 裁定 B) | `features/remote/` に直接生む |

`docs-split-review.md` のコードフェンス内にある `docs/x.md` と
`.background/import-adapters.md` は**引用例文**であって参照ではない。
検査を script 化するときはコードフェンスを除外する (今は grep なので出る)。

## 11. この波でやったこと (#55 波0 + B1 + B2)

- 本書 (docs/README.md) の新設。**ファイルは1本も動かしていない。**
- **第二段分割** (裁定 B1): `flat-field-stats.md` と `media-support.md` を、
  確定した現在形と経緯に分けた。両文書は**今の場所のまま**で、経緯だけが
  `background/` へ出た (`flat-field-stats.md` のファイル移動は波3 でペンディング)。
  切って移しただけで、本文は1文字も変えていない。
- **未決の板吸い上げ** (裁定 B2): 文書の奥で眠っていた未決を `tasks.csv` の行にした。
- 「`project/` を作らず docs/ 直下を project 層とする」を採用 (裁定 B3)。

Compare 試験移行 (波1) は #60 の実装が `compare-n-design.md` を触っている間はやらない。

---

## 12. リンク検査 — 6 つの宣言と、その場で走る形

selftest 文化に合わせる。まずは**読む物** (gate しない)。わざと1本壊して赤を
確認してから `tools/run_selftests.sh` 末尾または CI の独立ステップで gate 化する。
実装を1本にまとめるなら `tools/check_doc_links.py`。

**宣言1 (到達)** — docs 配下の全 `.md` は本書から 1 ホップで到達できる。
孤児 = この表に無いファイル。

```bash
cd docs && for f in $(find . -name '*.md' | sed 's|^\./||'); do
  [ "$f" = "README.md" ] || grep -qF "$f" README.md || echo "ORPHAN $f"
done
```

**宣言2 (実在)** — repo が引く `docs/...` パスは、存在するか §10 の例外表に
載っているかのどちらか。

```bash
grep -rIhoE 'docs/[A-Za-z0-9._/-]+\.(md|csv|html)' core tools docs README.md .github \
  | sort -u | while read p; do [ -e "$p" ] || echo "MISSING $p"; done
```

**宣言3 (相対リンク)** — docs 内の相対 Markdown リンクは各ファイルの親ディレクトリ
基準で解決する。**2026-08-17 の基線: 221 本、切れ 0。**

```bash
python - <<'PY'
import os, re, io
tot = 0; bad = []
for root, _, files in os.walk('docs'):
    for f in files:
        if not f.endswith('.md'): continue
        p = os.path.join(root, f)
        for m in re.finditer(r'\]\(([^)\s]+)\)', io.open(p, encoding='utf-8').read()):
            t = m.group(1)
            if t.startswith(('http://', 'https://', '#', 'mailto:')): continue
            t = t.split('#')[0]
            if not t: continue
            tot += 1
            if not os.path.exists(os.path.normpath(os.path.join(root, t))):
                bad.append((p, t))
print('links', tot, 'broken', len(bad), bad)
PY
```

**宣言4 (stub の純潔)** — §9 の対応表に載る旧パスのファイルは stub 形式
(1行目が `# (moved)`、本文見出しなし) を守る。stub に本文が生えたら正典が割れた合図。

```bash
grep -rl '^# (moved)' docs --include='*.md' | while read f; do
  n=$(grep -c '^## ' "$f"); [ "$n" = "0" ] || echo "STUB GREW BODY $f ($n headings)"
done
```

(今日は stub が 0 本なので出力なし。ファイルを動かす波1以降で効き始める。)

**宣言5 (§参照)** — `docs/x.md §N.M` 型の参照は、対象ファイルに `## N.` /
`### N.M` の見出しが実在する。C++ からの引用は §番号が主なので、パスの実在だけでは
「ファイルは在るが節が空を指す」を検出できない。板の素引用 (`docs/` 前置きの無い
`compare-n.md` の形) の正規化もここに含める。

```bash
PYTHONIOENCODING=utf-8 python - <<'PY'
import re, io, os, subprocess
out = subprocess.run(['grep', '-rIhoE',
    r'docs/[A-Za-z0-9._/-]+\.md (§|#)[0-9]+(\.[0-9]+)*', 'core', 'tools', 'docs'],
    capture_output=True).stdout.decode('utf-8')
refs = sorted(set(l.strip() for l in out.splitlines() if l.strip()))
bad = []
for r in refs:
    p, s = r.split(' ', 1); n = s.lstrip('§#')
    if not os.path.exists(p): bad.append((r, 'FILE MISSING')); continue
    if not re.search(r'(?m)^#{1,6} +%s(\.|\s|$)' % re.escape(n),
                     io.open(p, encoding='utf-8').read()):
        bad.append((r, 'NO HEADING'))
print('refs', len(refs), 'unresolved', len(bad))
for b in bad: print(' ', b)
PY
```

**2026-08-17 の実行結果: § 参照 116 本、未解決 7 本。** うち 6 本は §10 の例外表の
3 ファイル (まだ在らない) 由来。残る 1 本は `docs/media-support.md §2.4` —
`core/selftest/media.inc:906` が引いているが §2 に 2.4 は無い。**この検査の初の実収穫**で、
直しは C++ 側なので §8 規則 6 のとおり別 PR が持つ。

**宣言6 (板の live 行)** — 参照列の docs パス検査は **live 行のみ**
(残課題 / 進行中 / 暫定対応したが後で直す / レビュー必要項目)。対応済み行は
§9 の対応表で解決できればよい (歴史は書き換えない)。

```bash
PYTHONIOENCODING=utf-8 python - <<'PY'
import csv, io, os, re
for r in list(csv.reader(io.open('docs/tasks.csv', encoding='utf-8-sig')))[1:]:
    if r[0] == '対応済み': continue
    for m in re.findall(r'docs/[A-Za-z0-9._/-]+\.(?:md|csv)', r[3]):
        if not os.path.exists(m): print('LIVE ROW MISSING', r[0], m, '|', r[1][:40])
PY
```
