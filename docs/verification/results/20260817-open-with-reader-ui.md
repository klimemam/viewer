# 2026-08-17 Browse「Open with reader...」UI 検証結果

- 対象コミット: `08e6fb79cb995d5c64101e30b8e2deb230598279`
- 実行環境: Windows / MinGW（実行機の識別情報は記録されていない）
- 種別: Browse 行メニューと Reader panel への操作経路を追加した際の実施記録
- 再実行仕様: [../ui.md](../ui.md) A26
- 記録原文: 対象コミットの commit message と `08e6fb79:docs/verify-ui.md`（blob `e9f6b4e0e3c490aed87118425785e17485d0ab97`）。今回の文書再構成では再実行していない

---

## Commit message 原文

Browse 行の右クリックに「Open with reader...」—— 紐付けの明示的な入口 (#222)

* browse-keys: the row menu, read back as text and clicked by name (fail-first)

A right-click on a Browse row could be asserted on in exactly one way -
"a popup is open" - which is equally true of a menu that has lost half
its verbs. ImGui keeps no record of what a popup contained, so every
item of the listing's context menu now goes through rbCtxItem on its way
in: the labels in the order they were submitted, and the rectangle each
one landed on, so a selftest can read the menu as text AND aim a real
click at one item by name.

Four actions on top of it: rctxcur (the right-click of "rctx", aimed at
the CURSOR's row, so a folder, a group and a file row can each be asked
what they offer), chkctx (the count, or one label present or absent),
ctxclick:LABEL, and rdrshut / chkrdr for the Reader panel.

The six new assertions FAIL: docs/input-adapters.md §4.13 lists three
entrances to a reader and none of them is on a row, so "Open with
reader..." is on no menu, a .npy cannot be handed to a reader at all,
and nothing lands on the Reader panel when the item is clicked.

  chkctx:6      -> 5 item(s): [Open;Open as stack;Open as frame average;
                   Copy path;Properties...;]: FAIL
  chkctx:+Open with reader...                                     : FAIL
  chkctx:4      -> 3 item(s): [Open;Copy path;Properties...;]     : FAIL
  chkctx:+Open with reader...                                     : FAIL
  ctxclick:Open with reader... -> no such item                    : FAIL
  chkrdr:dark.npy -> reader panel CLOSED, path ""                 : FAIL

Every existing assertion of the run is unchanged and green, including
the two "-Open with reader..." rows and the item counts of the folder
and group menus - the shape that says the door is missing, not that the
menu is different.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

* browse: "Open with reader..." on a row - the door that was only ever an exit

The Reader panel had three entrances (File > Open With a Reader..., the
Inspector's reader field, and a double-click on a row this viewer cannot
read), and all three assume the viewer has ALREADY FAILED. That left two
doors missing:

  * a file already bound to a reader had no way back. The memo runs the
    old choice from then on (§4.12), so the file opens - and a file that
    opens has no failure to click through to reach the panel again;
  * a file the viewer reads natively (.npy, .png) could not be handed to
    a reader at all. The row is not dim, so the one row-level entrance
    does not exist on it, and "read this .npy MY way" was unsayable.

Both are now one item on the row's right-click menu. It leads to the
same panel by the same call as the double-click: rbReaderDoor is one
function, and the dim row's gesture goes through it too.

On a FILE row only - a reader is handed one path and returns one thing
(§4.1), so a folder and a group row do not offer it. On a remote row as
well as a local one: since #180 stages 1-2 a reader runs on the peer, so
the panel is handed the row's URL and the peer's own consent
(--serve-readers) is answered where it has always been answered, by the
Reader panel at Load. The Browse side invents no second refusal - and
the dim row's double-click stops inventing one too: its bespoke "readers
run on this machine only for now" is gone, and its "why" is now the
row's own tooltip sentence, the same one openPath puts on the panel.

Green, and the counts say nothing else moved:

  chkctx:6 -> [Open;Open as stack;Open as frame average;
               Open with reader...;Copy path;Properties...;]: ok
  chkctx:4 -> [Open;Open with reader...;Copy path;Properties...;]: ok
  chkctx:5 (group) / chkctx:4 (folder), both -Open with reader...: ok
  ctxclick:Open with reader... then
  chkrdr:dark.npy -> reader panel open on "tools/testdata/rb/dark.npy": ok

  54/54 selftests pass.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

* docs: the panel says four entrances, in both places that count them

§4.13.0's summary line and App's own comment both said "three
entrances", written when the row menu did not have one. The count is
part of the claim - a panel that lists its doors and misses one is how
the missing door stayed invisible in the first place.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

---------

Co-authored-by: Claude Opus 5 <noreply@anthropic.com>

## `docs/verify-ui.md` A26 原文

| A26 | Browse 行の右クリックが**何を出すか** —— 「Open with reader...」の戸 (§4.13) | 既定列 `rctxcur,chkctx:N,chkctx:±LABEL,ctxclick:LABEL,chkrdr:NAME` | ファイル行に項目が出る (native で読める `.npy` にも、読めない `.txt` にも)。フォルダ行・グループ行には**出ない**。既存項目は1つも消えていない (件数)。**実際に押すと** Reader パネルがその path で開く | `chkctx:6 -> 6 item(s): [Open;Open as stack;Open as frame average;Open with reader...;Copy path;Properties...;]: ok` / `chkctx:4 -> 4 item(s): [Open folder (all stacks below);Search under here;Bookmark;Copy path;]: ok` / `chkrdr:dark.npy -> reader panel open on "tools/testdata/rb/dark.npy": ok` | PASS |

**A節まとめ: 27項目 / 実行 27 / PASS 27 / FAIL 0。**
スイート全体 (21 test): `SUITE TOTAL /c/Users/hish/Desktop/viewer-wt-verifyui pass=21 fail=0`
