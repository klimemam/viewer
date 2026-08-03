現行ドキュメント: [verify-ui.md](../verify-ui.md) の背景 — stage 1 直後に回した UI 検証の実施記録と、そこで見つけた検証衛生の欠陥。

# UI 検証マトリクスの実施記録

## 対象としたラン

対象: main @ e3883f5 (frame-reference refactor の stage 1 マージ直後)。
stage 1 は `FrameSource` を `ImageDoc` の下に敷いただけで **UI は無変更のはず** —
本表 A/B 節はその「無変更」を状態で証明するためのもの。

## A. 現行 main の UI 回帰 — 実施結果

判定はすべて **本日 main @ e3883f5 で実測**。

| 項番 | 実施結果 | 判定 |
|---|---|---|
| A1 | `chkdir:rb -> … dir=tools/testdata/rb: ok` / `chkatrow:digitset -> … cursor row=digitset: ok` | PASS |
| A2 | `dbl` 後 `waitdir:digitset` 成立、後続 `chkback:5 -> back=5 fwd=0: ok` | PASS |
| A3 | `chkatrow:.. -> … cursor row=..: ok` | PASS |
| A4 | `chkexpn:0 -> … tools/testdata/rb/digitset expanded on 0/25 watched frame(s): ok` | PASS |
| A5 | `chkexp:1 … ok` / `chkexp:0 … ok` | PASS |
| A6 | 3 assert すべて `ok` | PASS |
| A7 | `chkopen:4 -> imgs=44 seqs=4 … : ok` | PASS |
| A8 | `chknames:frame_###.npy*24 -> … missing=[] surplus=[]: ok` | PASS |
| A9 | `chkfwd:1 -> … back=4 fwd=1: ok` / `chkfwd:0 -> … back=5 fwd=0: ok` | PASS |
| A10 | `chkpv:24 … pv=1 scrub=24 idx=0: ok`、`chkidx:1 … idx=1: ok` | PASS |
| A11 | `chkfocus:1 … : ok` | PASS |
| A12 | `key routing: … the main view ran 0 nav key(s) and stood down for 55; after blur it ran 4 more: ok` | PASS |
| A13 | `S6 badges (comparing): '00/frame_000‥004.npy=A;02/frame_000‥004.npy=B;rgb_u8.npy=C;'` | PASS |
| A14 | `S6 first ESC deselects the ROI, and nothing else PASS` / `S6 second ESC exits compare, and only compare PASS` / `S6 …keeping the B pin and the slot letters (seats survive) PASS` / `S6 a third ESC has nothing left to take PASS` | PASS |
| A14b | `after rctx  a popup is open (cursor 90,623): ok` / `after esc   a popup is closed (cursor 90,623): ok` / `after fmenu a popup is open (cursor 41,16): ok` / `after esc   a popup is closed (cursor 41,16): ok` | PASS |
| A15 | `S6 badges (compare off): '02/frame_000‥004.npy=B(dim);rgb_u8.npy=C(dim);'` / `S6 compare off: no A badge - the cursor is not a seat PASS` | PASS |
| A16 | `A7 compare off: A-row=1 other-row=0` / `A7 compare off: the current row offers no items PASS` | PASS |
| A17 | `P2b throttle: armed=1 held uid=6 vs B uid=8, released=1, after release uid=8` / `P2b B slot holds its last result while stepping (stale) PASS` | PASS |
| A18 | `T1 overlay 560x440: ran=1 plotted=1 slots=2 A=5 B=5 plot y 222.0..327.0 (window bottom 440.0)` / `T2 …and the panel says B has no time axis PASS` | PASS |
| A19 | `E7 newline and comma+newline (Excel column paste) parse PASS` / `E7 parse error: value 3 ('x') is not a number;  value 5 ('1e') is not a number` | PASS |
| A20 | `F11 no axis: the section says 'set the x axis first' PASS` / `F11 both plots laid out INSIDE the panel PASS` | PASS |
| A21 | `montage 45x7 from 5 frame(s), ROI 9x7 (cfa=0), name '00/frame_000‥004.npy  ROI 9x7 x5 (montage H)'` / `E11 per-frame montage is produced PASS` | PASS |
| A22 | `T12 narrow: the A badge itself survives PASS` / `T12 narrow: the name elides from the front or drops, never overflows PASS` | PASS |
| A23 | `chkpanels:2`、`chkfilt:-`、`chksel:0`、`chkback:0` すべて `ok` | PASS |
| A24 | `N7 stack argv: HEAD file + --sequence always PASS` / `newwinselftest: ALL PASS` | PASS |
| A25 | `root popup collision: RAW dialog open before the competing modal=1, after=1; forQueue implies a live dialog=1: ok` | PASS |

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

## B. 設計正典 (docs/) のうち UI 状態で表現できるもの — 実施結果

| 項番 | 実施結果 | 判定 |
|---|---|---|
| B1 | A1/A2/A6 の証拠行に同じ | PASS |
| B2 | `side ch n N sigma_t [DN] sigma_fpn [DN] sigma_tot [DN] source` / `E7 the unit is never defaulted PASS` | PASS |
| B3 | `frame file mean_R [DN] sigma_R [DN] … mean_Gr [DN] … mean_Gb [DN] … mean_B [DN] …` | PASS |
| B4 | `E3 partial stack says n of N, not n of n PASS` / `# -- side A: … resident 5 of 5 frame(s) --` | PASS |
| B5 | 16 テーブル中、数値宣言の 10 件はすべて一致 (`quickstats 4/4` `lintab 7/7` `abtemporal 5/5` `framelin 7/7` `roitable 8/8` `rblist 4/4` `derivelist 5/5` ほか)。手読み対象はループ生成 5 件と条件式 1 件 (`px  declared=b ? 5 : 3  setups=5`) のみで、**新規の不一致なし** | PASS |

**B節まとめ: 5項目 / 実行 5 / PASS 5 / FAIL 0。**

---

## C. 進行中の stage 2 / 3 / 5 の受け入れ行 — 未実施 (未マージ)

現 main には該当文字列が **存在しない** ことを確認済み
(`grep -n 'share the same pixels\|no pixel copy\|Reload from disk' core/main.cpp` → 0 hit)。

判定はすべて現時点で **未実施(未マージ)**。

| 項番 | 実施結果 | 判定 |
|---|---|---|
| C1 | — | 未実施(未マージ) |
| C2 | — | 未実施(未マージ) |
| C3 | — | 未実施(未マージ) |
| C4 | — | 未実施(未マージ) |
| C5 | — | 未実施(未マージ) |
| C6 | — | 未実施(未マージ) |
| C7 | — | 未実施(未マージ) |
| C8 | — | 未実施(未マージ) |
| C9 | — | 未実施(未マージ) |

**C節まとめ: 9項目 / すべて 未実施(未マージ)。**

---

## D / E 節の item 数 (実施時点)

**D節まとめ: 7項目 / すべて 実機確認のみ。**

**E節まとめ: 7項目 / すべて 要probe。**

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

## 校正の実測

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
