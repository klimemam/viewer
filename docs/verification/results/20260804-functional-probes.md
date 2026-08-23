# 2026-08-04 機能 probe（観測点）検証結果

- 対象コミット: `67e626cd073dfa66ece118690695a447c263afa2`
- ブランチ: `verify-probes`
- 実行環境: Windows / MinGW（実行機の識別情報は記録されていない）
- 種別: 実施記録。後日の実装状態に合わせて書き換えない。
- 再実行仕様: [../functional.md](../functional.md)
- 記録原文: `67e626cd:docs/verify-functional.md`（blob `c1feb565232df15417864b3e477b1d1b908367c0`）

---

### D節の更新 (2026-08-04) — probe を実装した (branch `verify-probes`)

`srcMapDump()` と、それを回す `--srcmap-selftest <dir>` を追加した。開いている
membership 1件につき1行を stderr へ出す:

```
srcmap: <tag> uid=… seq=… idx=… src=#K refs=N rev=… datarev=… WxHxC bytes=…
        mtime=… fsize=… name='…' [member='…'] [path='…']
srcmap: <tag> 15 doc(s), 15 source(s), 307200 resident byte(s)
```

`src=#K` は **srcId そのものではなく密な通し番号**。srcId は大域カウンタで、その値は
「この frame の前に何枚復号したか」に依存して実行ごとに変わり、表明に使えない。
**共有とは2つの doc が同じ #K を出すこと**で、これは実行間で安定する。

**D-1/D-2/D-7 の「観測手段が無い」は解消した。** ただし本行の提案文にあった
「1本の probe で stage2/3/5 の受入行が**全部**解錠される」は、C節を1行ずつ読んだ
結果**そのままでは正しくない** — C-1/C-2/C-3 の15行のうち、この probe が
決め手になるのは**5行**である。

| 受入行 | 解錠 | 理由 |
|---|---|---|
| C1-1 同じフォルダ2回で source 共有 | **する** | 共有の有無は srcId でしか言えない。2行の `src=#K` が一致するかがそのまま判定になる |
| C1-2 共有時バイトは1倍 | しない (既に観測可能) | `--verify-selftest` V14 が resident バイトを印字済み。probe は根拠を1行足すだけ |
| C1-3 片方 close 後も画素が残る | 部分的 | 「残る」は統計が出るかで判る。probe は refs が 2→1 に落ちる証跡を足す |
| C1-4 Ctrl+Alt+W が membership だけ消す | **する** | 「source は生きている」は use_count でしか見えない |
| C1-5 compare の same-pixels 文 | しない | chip 文字列の probe が要る (verify-ui.md C4) |
| C2-1 既存 derive 表明が無傷 | しない | 既存の表明そのもの |
| C2-2 派生がバイトを増やさない | しない (既に観測可能) | resident バイト |
| C2-3 crop CoW、派生側は不変 | **する。しかも今日実行した** | M4 が手で共有を作り (stage2 相当)、M5 が `cropInPlace` の `use_count()>1` 分岐を発火させて、他方の画素が1バイトも動かないことを表明する。stage1 では製品経路から到達不能な分岐で、規律だけが先に検査された |
| C3-1 画素差し替えで uid 不変 | **する** | uid は probe 行が持つ |
| C3-2 全キャッシュが再計算される | しない | 鍵の中身は出ない |
| C3-3 σ_t が変わる | しない | |
| C3-4 fit が落ちる | しない | |
| C3-5 共有先 stack も同じ一歩で更新される | **する** | 全 membership の src と rev を並べれば walk の抜けがそのまま見える |
| C3-6 temporalExtra も忘れる | しない | |
| C3-7 手動 Reload (GUI) | しない | verify-ui.md 側の probe |

**D節の外で解錠したもの**

- **A-12 / D-2 は「コード読みでしか言えない」ではなくなった。** `--srcmap-selftest` の
  M2 が npy 経路、M3 が raw 経路を、**ディスクの `last_write_time`/`file_size` と
  突き合わせて**表明する (0 では通らない)。npz 経路は `--verify-selftest` V21 に
  同じ形の1件を足した (container のサイズと全メンバーの `fsize` の一致)。
  実測 15/15 frame・raw 1件・npz 5メンバー。
  **未実行はコンテナ経路 (`.vnz`, `main.cpp:4929`) の1つだけ** — A-12 が数えた3経路は
  すべて実行で確認済み。
- **D-7**: `rev` は probe 行に出る。ただし +1 させるには remote の preview→full 差し替えが
  要るので、**インクリメントの実行確認は未了**。M6 は `cloneSource` が rev を 0 に戻し
  新しい srcId を取ることだけを表明する。
- **A-3 (CLI 表面)**: フラグが 52 → 53 に増えた (`--srcmap-selftest`)。selftest フラグは
  22 → 23、ctest の本数は 22 → 23。

**新しい表明 (すべて実測 PASS)**

| 表明 | 何が今日まで言えなかったか |
|---|---|
| M1 「N docs, N sources, 各1 holder」 | stage1 の基線。stage2 が破る対象で、破れたら**目立つ**ように書いてある |
| M1 mirror 一致 | §2.1 の per-doc ミラーが source とずれていないこと (印字経路が無かった) |
| M2 npy の Watch 基線 = ディスクの実値 | A-12 は目視だった |
| M3 raw の Watch 基線 = ディスクの実値 | 同上 |
| M4 共有が **見える** (同じ #K・refs=2・resident が1枚分減る) | probe 自身の校正。refs=1 しか出せない probe は何も証明しない |
| M5 crop CoW が分離し、他方の画素が不変 | **C2-3 そのもの**。stage1 では製品経路から起こせない |
| M6 `cloneSource` は内容同じ・identity 別・rev=0 | D-7 の半分 |
| V21 npz メンバーが container の Watch 基線を持つ | A-12 の3経路目 |

### E-6 の訂正と修正 (2026-08-04, branch `verify-probes`)

**訂正**: 「無限に続く」は言い過ぎだった。action 段階で `hold` を立てるのは
`waitdir` / `waitimg` の2つだけで、どちらも**個別に60秒**で諦める
(`main.cpp` の `waitD0` / `waitT0`)。実測: 不可能な待ちを3つ並べた列は
**3分03秒**で終わる (60秒×3 + 実行)。したがって停止は無限ではなく、
**行の長さに比例して伸びる** — 上限が run に無く、待ちの個数にしか無い。
既定の列は待ちを15個持つので、A-16 の形 (レイアウトが古く、注入クリックが
別の場所に落ち、待ちが全部空振りする) では**15分級**になり、そこで
ctest の TIMEOUT 900 に殺される。ctest は "Timeout" としか言わないので、
**どのアクションで止まったかはどこにも残らない**。

**修正**: action 段階に run 全体の壁時計 (300秒) を入れた。listing 段階の60秒と
同じ形で、時計は**listing が揃った時点から**回る。切れたときは
`browsekeys: action phase gave up after 300 s at action 12/275 'waitimg:44'
(phase 0), dir=…, imgs=…: FAILED` と**止まったアクションを名指しして** rc=1 で
終わる。健康な run (約35秒) の8倍、ctest の 900 秒の内側。**待つスイートは
誰も読まないスイートである。**
