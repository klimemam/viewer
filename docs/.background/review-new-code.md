現行ドキュメント: [terminology.md](../terminology.md)（series 層の正典）の背景 — 実装直後のレビュー記録。

# Review: code landed after e38de8b

Scope: `e38de8b..HEAD` (series layer phases 1-5 + picker sweep, Browse deferred actions,
A/B statistics changes, arrow-key clipper fix; ~3000 added lines). Four review lenses,
every finding adversarially verified against the source; only what survived is here.
Everything below has a concrete trigger and was confirmed at the stated lines on `main`.

One refutation worth recording so nobody re-fixes it wrong: "rename two DIFFERENT-folder
batches to one name loses a series on round-trip" is false — `loadSession` starts with
`closeAll()` (main.cpp:3462) and `imgbatch` restores through `batchReuse` (3572), which
merges same-named batches into one, after which both series' member paths are distinct and
both restore intact. The rename gap is real (fix 8) but only loses data in the
same-folder-copies variant.

---

## 1. Ranked fixes, grouped by root cause

Ordered by the most dangerous symptom in each group.

### Cause G — a membership edit that skips fit invalidation  (1 symptom, HIGH)

**Fix 1. Move-to-batch on a series member leaves the computed fit standing.**
`moveStackToBatch` (core/main.cpp:2007-2020) removes the stack from its series
(removeFromSeries/pruneEmptySeries at 2012-2013) and toasts, but never calls
`linFitStale()`. The Linearity panel's only freshness keys are `L.seriesId == S->id`
(9401) and `L.fitValid` (9461); a move leaves the series alive, so both stay true. The fit
table, R²/K/read-noise, the "%d points" count and both plots (which iterate the `L.rows`
snapshot, 9525-9535) keep describing the old membership and still draw the departed
stack's point — labelled fresh. Every sibling membership edit invalidates (closeStack
1912, seriesJoinFromMenu 2111, pendingLeaveSeq 12480, seriesModalAccept 9165); this is the
one that doesn't, violating terminology.md:112-113 added in this very window.
*Trigger:* series of 3+, Compute, then Files → right-click a member → Move to batch.
*Fix:* inside the `S->batchId != batchId` branch, add `linFitStale();` after
`pruneEmptySeries()` (main.cpp:2013), mirroring pendingLeaveSeq. `moveSeriesToBatch`
(2027-2035) sets `S->batchId` first so its calls never enter this branch — the fix cannot
misfire there.

### Cause A — the pending-series name contract is not upheld by every stack producer  (2 symptoms, HIGH + LOW)

series-plan.md §5 asserts `PendingGroup::name == SeqInfo::name`. Only `startSequenceLoad`
(3930) and openRemoteStack's folder path (6276) establish that equality; three producers
don't, and the resolver has no other key.

**Fix 2. "Open as a sweep" loses every single-file member — a one-npy-per-level sweep
resolves to zero members and the confirmed values/parameter/unit are discarded.**
`resolveOnePendingSeries` matches only `si.name == e.first` or the exact `" [remote x"`
suffix (4239-4243). But: a local single-file group (created at 4533; or a multi-file group
the picker's live filter cut to one file, 4816-4817) loads via `loadNpy`, whose frame-axis
stack is named `"<base>  (N frames)"` (2563/2493), never the group name — `g.name` is
applied only for `files.size() >= 2` (4573). A local single-frame file gets no SeqInfo at
all (2559) and the resolver walks only `app.seqs` (4236). A remote frame-axis file keeps
openRemote's `"<base> [remote]"` name (6226) because `openRemoteStack` early-returns at
6270 before its rename. Each takes `lost++` (4245); if all members are single-file, no
series is created and the typed parameter, unit, and per-row previewed values (painted at
5052) are thrown away — toast: `sweep: 0 stack(s) in series "(none)"; 7 could not be
matched`. One multi-frame capture per level folder is a standard IQ layout; the
--series-selftest fixture (7 folders × 24 numbered frames) is structurally unable to see
this. (A remote single 2-D file does resolve — it becomes `"<group> [remote x1]"`.)
*Fix (uphold the contract at the producers, resolver unchanged):* in
`startNextQueuedGroup`, after a successful 1-file load that produced a stack, rename that
SeqInfo to `g.name`; in `openRemoteStack`, before the 6270 early return, rename the
frame-axis SeqInfo to `name + " [remote x1]"`; in the picker, disable the sweep row (with
a note) when a selected group is a single 2-D file, which can never be a stack and hence
never a member.

**Fix 3. Mid-load rename misassigns a sweep value to the wrong stack.**
The Files panel renames `si->name` unconditionally as soon as the head frame lands
(12258-12267), while resolution is deferred to full drain (4307-4314) and matches on the
*current* name, first match in creation order (4236-4243). Rename `10lx/f` → `20lx/f`
mid-load: entry `10lx/f` is counted lost, but entry `20lx/f` binds to the renamed ex-10lx
stack (which precedes the real one in creation order) and assigns it value 20 — a silently
misplaced point on the fit axis, worse than the counted loss.
*Fix (and the durable fix for #2 as well):* stamp identity at creation instead of
resolving display names at drain — give PendingGroup/RemoteOpen and each SeriesPending
entry a token; when the loader creates the stack for a group, record token→seqId;
`resolveOnePendingSeries` consumes the recorded seqId, name-matching only tokenless
entries (hand-written sessions).

### Cause C — lazily-resolved series are invisible to the save path  (1 symptom, HIGH-leaning MEDIUM)

**Fix 4. Saving while series await resolution silently drops them — via Ctrl+S, the
autosave, the quit autosave, and the 0.4 s crash snapshot.**
`loadSession` parses series into `app.seriesRestore` (3597-3627) and picker sweeps sit in
`app.seriesPending` (4894); both become `app.series` only after *every* queue drains
(4307-4314) — minutes for a remote sweep. In that window `app.series` is empty (closeAll,
2139-2143) and `writeSessionTo` (3224-3253) serializes neither pending container nor
counts them into `lostSeries`/`lostMembers`. Ctrl+S is live from the first image (16758,
6433-6438); `refreshCrashSnapshot` runs every 0.4 s of change (17359-17362); the debounced
autosave fires on any 3 s lull and unconditionally on quit (17363-17368, 17467). A file
written in the window has no series block and `saveSession` toasts a clean "session saved"
(3277-3282). This is data loss routed around the very loss counters this window added.
*Fix:* in `writeSessionTo`, re-emit each `app.seriesRestore` entry verbatim as a series
block (it already holds name/batch/param/unit/kind and members by path — exactly the file
format), and count each `app.seriesPending` into lostSeries/lostMembers (its members are
group names, not yet paths, so they cannot be emitted) so the toast warns.

### Cause D — one legacy branch still parses "unset" as 0  (1 symptom, MEDIUM)

**Fix 5. The `seqlevel` reader turns unreadable text into a hard 0.0, and migration
builds a fittable linearity series anchored on it.**
`double lv = 0; ls >> lv;` (3588-3591) — a failed istream extraction stores 0.0 (C++11),
so `seqlevel notanumber`, `seqlevel 1e`, `seqlevel -`, and bare `seqlevel` all pass the
`std::isfinite` guard. `migrateLegacyLevels` (4181-4221) then creates a `KLinearity`
series with unit = the `app.lin.unit` prefill (default `"lx"`, line 545 — fittable out of
the box) and value 0.0 per garbage line, and `linRecompute` (9011-9016) treats
`|value| <= 1e-9` as THE dark stack: offset anchored there, its σT reported as read noise
— a fake dark point in a lit stack. This is exactly the atof class 1f33bfa closed:
`parseSeriesValue` strictly guards the `seriesmember` branch 30 lines below (3624) but not
this one — and the seqlevel path exists *only* for hand-written/third-party files,
precisely the input class that produces unparseable text.
*Fix:* parse the branch with the strict parser —
`double lv = parseSeriesValue(restOfLine(ls).c_str());` keeping the isfinite guard. An
unreadable legacy level is no legacy level; a batch of them migrates to nothing, which is
the canon's stated behavior.

### Cause B — deferred series targets outlive what they point at  (2 symptoms, MEDIUM + LOW)

**Fix 6. `seriesModalAccept` with a dead `editId` strips stacks out of live series and
discards them, silently.**
`E.editId` is validated only when the modal opens (9072), never at Save.
`pumpSequenceAndQueue` runs every frame regardless of the modal (17295; 17270 even forces
frames while it is open), so `resolveSeriesRestore` (removeFromSeries, 4148-4149) or
`resolveOnePendingSeries` (addToSeries moves stacks, 1710) can empty the edited series
mid-modal and `pruneEmptySeries` deletes it. Save then keeps the dead id (9143), the rip
at 9146-9149 removes every checked stack from whichever live series now holds it,
`seriesById(id)` fails at 9150 so the rebuilt list goes nowhere, `pruneEmptySeries` (9160)
deletes any series the rip emptied, and the `return 0` is discarded at 9302 — no toast.
(`migrateLegacyLevels` cannot trigger this: 4185 skips stacks already in a series.)
*Trigger:* sweep over a slow folder; while stacks arrive, hand-create a series, open
Edit… on it, let the drain move its members into the sweep series, press Save.
*Fix:* revalidate at 9143 —
`int id = seriesById(E.editId) ? E.editId : newSeries(E.batchId, E.name);`. A dead edit
becomes a create: the rip is followed by a real assignment, preserving membership and
typed values.

**Fix 7. `closeBatch` leaves the batch's pending sweeps in `app.seriesPending`.**
closeBatch purges `rbOpenQueue` (1941-1943) and `seqQueue` (1944-1946) by batchId but not
the third container; the only `seriesPending` purge anywhere is closeAll's (2143). Batch
ids are never reused (961), so at drain every member fails the batch test (4237) and the
user gets a red "sweep: 0 stack(s) … could not be matched" toast about an open they
deliberately discarded; a reopened folder gets no series from the stale note.
*Fix:* next to the seqQueue purge, erase `seriesPending` entries with
`it->batchId == batchId`.

### Cause E — restore matches members by non-unique first-frame path, and losses on that path are uncounted  (2 symptoms, MEDIUM combined)

The function's own header (4126-4127) states the contract: "silently dropping points out
of a measurement is the one thing this must not do." Three sub-paths violate it, all in
`resolveSeriesRestore` or just upstream.

**Fix 8a. Duplicate-resolving members are dropped without `lost++`.**
Two members legitimately share a first-frame path: (i) canon-blessed flow — open the same
folder twice, Move one stack across (moveStackToBatch has no same-path rejection;
terminology.md recommends move-then-group), add both to one series (addToSeries keys only
on seqId); (ii) two 3-D arrays in one `.npz` share the archive path (2540/2543),
distinguished only by `npzMember`, which the `seriesmember` line does not carry
(3237-3247). On restore `seqIdOfFirstFramePath` (4114-4122, no exclusion set) returns the
same first match for both; the second hits `if (dup) continue;` at 4142 — unlike the
`!sid` branch at 4139, no `lost++`. Toast reports full success; the member and its
hand-typed value are gone, permanently at the next autosave.

**Fix 8b. A later series robs an earlier one, after `made++`.**
With one same-path stack per series, the later series resolves to the stack the earlier
already holds; `removeFromSeries` (4149) shrinks the earlier series uncounted; if that
empties it, `pruneEmptySeries` (4160) deletes it while the toast still says "restored 2
series" because `made++` (4156) preceded the theft.

**Fix 8c. A truncated `seriesmember` line vanishes uncounted.**
The path is the LAST field (3245-3246), so truncation cuts it first; an empty
`restOfLine` fails the guard at 3623 and the line is dropped before becoming a member —
`badValues` counts only unreadable values on lines that still carry a path, and a member
never pushed is invisible to `lost`. Truncated files are a real input: `saveSession`
writes with no temp+rename (3272) and the crash handler's write loop `break`s on a short
write (3427-3438). The comment at 3616-3619 names the half-written line as the case to
handle.

*Fix for the group:* (1) keep a used-seqId set across all members and series in
`resolveSeriesRestore` and pass it to `seqIdOfFirstFramePath` as an exclusion list — the
second same-path member then binds to the second same-path stack, which also eliminates
the 8b steal; (2) `if (dup) { lost++; continue; }` at 4142 for whatever still collides;
(3) count truncated lines: `else if (curSeries >= 0) app.seriesRestore[curSeries].truncated++;`
at 3623's else, folded into `lost`. The full identity fix — persisting `npzMember` in the
seriesmember line — is format-affecting and can wait; the counting cannot.

### Cause F — batch rename skips uniqueBatchName  (1 symptom, LOW severity, data-loss consequence)

**Fix 9. Rename permits duplicate batch names; with same-folder copies, the collision
silently deletes one series on the round-trip.**
Every creation path uses `uniqueBatchName` (4873, 4880, 12049); the rename handler
assigns verbatim (12128-12130, `b.name = nameBuf`), violating terminology.md:120 ("batch
名は一意 … 衝突には (2) を付す"). Two same-named batches merge at restore via `batchReuse`
(3572); when they were two opens of the SAME folder, the merge nullifies `preferBatch` —
`seqIdOfFirstFramePath` hands both series the same stacks, the at-most-one loop
(4147-4149) strips the first restored series, `pruneEmptySeries` deletes it, and the toast
counts it as restored. The rename UI predates the window; the new series persistence is
what turned a cosmetic merge into measurement loss.
*Fix:* pass renames through the uniquifier, excluding the batch being renamed: if any
OTHER batch bears `nameBuf`, assign `uniqueBatchName(nameBuf)` and toast the adjustment.

### Cause I — the raw recipe stayed process-global after the queue became multi-Open  (1 symptom, MEDIUM)

**Fix 10. One raw recipe silently decodes the queued stacks of two different Opens, and
the dialog's Cancel discards both Opens.**
`enqueueGroups` now APPENDs (4588-4589 — deliberately, the old replacement silently
cancelled the first Open) but `app.folderRecipeValid`/the latched recipe have exactly
three touch points (reset 4578, test 4546, set 6583) and `PendingGroup` carries no
per-Open key. Opening raw folder B while raw folder A drains resets the flag; the next A
group re-raises the dialog titled with an A file to a user who just opened B; the answered
geometry is then applied unconditionally to every remaining raw group of BOTH folders
(4560-4564) — the mismatched folder decodes as garbage (silently, if its files are large
enough) or fails file-by-file. Cancel executes `app.seqQueue.clear()` (6572) with no
toast, discarding the second Open's stacks along with the first's remainder.
*Fix:* stamp each PendingGroup with an openId in `enqueueGroups`; store the openId the
recipe was answered for beside `folderRecipeValid` and re-raise the dialog when
`g.openId` differs; make Cancel erase only the asking openId's groups. (The same openId
slots into the token plan of Fix 3.)

### Cause H — A/B consumers don't consult the range actually in force  (2 symptoms, MEDIUM)

**Fix 11. Histogram/projection labels ignore `linkRange`: axis says "A and B combined
range" while spanning the global linked range.**
`abRangeSaid` (8123-8126) keys purely on `compareRangeMode`, but `effBlack`/`effWhite`
(1286-1297) — which set both the bin range (8147/8154) and the plot limits
(8338/8353/8361, projection 8540) — short-circuit on `app.linkRange`. With Value range
scope = everything and a B present, the axis spans linkBlack..linkWhite (seeded over ALL
open images, 8041/8068) under a label naming an A/B range. All three linked-scope
combinations mislabel; same for the projection y label (8692-8694). This defeats
86046e7's stated purpose. The Inspector even hides the combo under linkRange (7859) — the
code knows linked overrides A/B — but the View menu (12673-12678) still sets it and the
default is 2 regardless.
*Fix:* first line of `abRangeSaid`:
`if (app.linkRange) return "linked range (all open images)";` — both label call sites go
through it.

**Fix 12. Union default re-bins A per step, but B's [stale] flag keys on uid only — a
pinned B's curve is drawn mis-binned with no stale tag.**
In union mode (default since 09b9e41) the bin range is min/max of the two CURRENT frames
(1276-1284), so stepping A moves the range every frame and hist[0] rebins (black/white
are cache keys, 7311-7317). B's recompute is skipped while `abStepBusy()` (8153), and
`bStale = HB.uid != Bim->uid` (8157) detects only an image change — never that
`HB.black/HB.white` no longer match the range in force. `plotSeries` places bins by index
across the current axis (8264-8284), so B's old-union bins draw horizontally mis-scaled on
the new axis with no amber [stale]. A stack B changes uid and gets tagged; the unflagged
case is exactly the pinned single-frame B (Shift+B, an advertised workflow). Under the old
mode-1 default the bin range didn't move during stepping, so uid-only staleness was
sufficient then; the union default broke that assumption. Contradicts the contracts at
8149-8152 and 4367-4371 ("hold their last result and say so").
*Fix:* extend the flag —
`const bool bStale = Bim && (HB.uid != Bim->uid || HB.black != effBlack(*im) || HB.white != effWhite(*im));`
Once stepping stops, the recompute runs and both comparisons return to equal.

### Cause J — lossy display text becomes authoritative  (1 symptom, LOW)

**Fix 13. Touching then reverting a value box in Edit series rounds the stored value to
6 significant digits.**
The box is prefilled at `"%.6g"` (9101); any accepted keystroke sets `touched` (9262);
Save then parses the display text (9053-9056). Click into 1234567.89 (shown
"1.23457e+06"), type a digit, delete it, Save → 1234570, persisted via fmtExact. The
comment at 573-576 names exactly this rounding as what the orig/touched split exists to
prevent — but the protection covers only the never-edited case.
*Fix:* prefill losslessly at 9101 with `fmtExact(value)` (1749-1757; max %.17g ≈ 24 chars
fits the 32-char buffer) — then even the touched path re-parses to the exact double.

---

## 2. Patterns, not instances

**P1 — Resolution by display name instead of identity stamped at creation.**
Instances: fixes 2, 3 (pending sweeps), 9 (batch restore by name), 8a (members by
first-frame path). The pending layer defers binding until drain, but the only keys it
carries are strings that (a) some producers never set to the expected value and (b) the
user or a merge can change in the window. *Invariant to adopt:* anything queued for later
resolution carries an identity token minted when the queue entry is created; the loader
records token→seqId at stack creation; names are a fallback reserved for entities that
predate the token (hand-written session files). One `int token` on
PendingGroup/RemoteOpen/SeriesPending plus a small token→seqId map kills the whole class,
including the rename race no name-matching patch can fix.

**P2 — Deferred actions whose target can die before they run.**
Instances: fixes 6 (modal editId), 7 (seriesPending vs closeBatch); the rbDefer machinery
is the in-window *cure* for the same class and shows the shape of the rule. *Invariant:*
every deferred consumer revalidates its target id at execution time (ids are never
reused, so `seriesById`/`batchOfStack` checks are exact), and every destroyer of a target
sweeps every queue that can reference it — closeBatch already sweeps two of three;
grep for containers keyed by batchId/seriesId whenever one is added.

**P3 — Loss that routes around the loss counters.**
Instances: fixes 4 (save-side), 8a/8b/8c (restore-side). The window's stated principle —
count and report every dropped point — is implemented on the main paths and missed on the
side paths (`dup continue`, post-`made++` theft, pre-parse truncation, pending containers
at save). *Invariant:* every `continue`/early-return that discards a member or series in
the session reader, resolver, or writer must increment a counter that reaches the toast.
This is cheap to audit: grep `continue;` in
writeSessionTo/loadSession-series-block/resolveSeriesRestore/resolveOnePendingSeries and
demand each either pushes data or counts.

**P4 — One value-parsing discipline, N parsers.**
Instance: fix 5; the class was already fixed twice (modal, seriesmember) via
`parseSeriesValue`, and the third numeric-from-session-text site regressed to
`ls >> lv`. *Invariant:* session text → measurement value goes through `parseSeriesValue`
and nothing else; grep the reader for `>> ` into a double is the review rule.

**P5 — Displayed state derived from a mode flag rather than from the value in force.**
Instances: fixes 11 (label vs effBlack/effWhite), 12 (stale flag vs bin range). The
range machinery has one authority (`effBlack`/`effWhite`) but consumers re-derive what it
"should" be from `compareRangeMode`/uid. *Invariant:* labels and staleness compare
against the authority's output, never against the inputs they believe determine it. Any
future `compareRangeMode ==` in UI code is a smell; ask effBlack/effWhite instead.

**P6 — Process-global answer applied to a queue that now spans questions.**
Instance: fix 10. When the queue semantics changed (replace → append), the coincidence
that made a global valid ("one queue = one Open") vanished silently. *Invariant:* any
global latched from a dialog gets stamped with the scope it was answered for, and every
consumer checks the stamp.

---

## 3. What the 15 selftests structurally cannot see, and the cheapest cover

The selftests are single-threaded scripts that drive model functions back-to-back with a
homogeneous fixture and a writer-produced session file. That shape has four blind spots,
and every confirmed defect sits in one of them:

**(a) Interleaving with the pump.** Every selftest calls
openSeriesModal → seriesModalAccept adjacently (15032-15127); none closes a batch or lets
a queue drain between arm and fire. Fixes 6, 7, 10, and the save-window of fix 4 live
here. *Cheapest shape:* a `pumpN(n)` helper that runs n iterations of
pumpSequenceAndQueue between two scripted actions; one test = open modal → drain →
Save, asserting the sweep series' membership survived; one test = closeBatch mid-queue,
asserting seriesPending is empty.

**(b) Fixture homogeneity.** The series fixture is 7 folders × 24 numbered frames —
every group multi-file, so only startSequenceLoad's naming path is ever exercised (fix
2's three failing producers are untouched code paths in every test). Same-path stacks
inside ONE batch never occur (fixes 8a/8b; the seriesdup test at 15208-15247 covers only
the cross-batch case preferBatch handles). *Cheapest shape:* add one fixture folder with
one multi-frame .npy per level and run the existing --series-selftest sweep over it; add
one npz with two 3-D arrays to the round-trip test.

**(c) Writer-produced inputs only.** Sessions in tests are written by writeSessionTo,
which never emits garbage seqlevel lines, truncated members, or duplicate batch names —
fixes 5, 8c, 9 are unreachable. *Cheapest shape:* hand-authored session strings written
to a temp file and fed to loadSession, asserting the toast's loss counts. Three ~10-line
fixtures cover all three.

**(d) Value assertions without label/flag assertions.** --range-selftest checks label
strings only before its linkRange loop (16570-16581) and compares only
effBlack/effWhite in the nine-combination loop (16583-16611) — fix 11 is invisible.
--abstats-selftest P2b (16321-16356) exercises staleness solely via uid change — fix 12
invisible. The move test (14896-14915) checks member count, toast, and audit but never
`app.lin.fitValid` — fix 1 invisible. *Cheapest shape:* assert `abHistXLabel` while
linkRange is on; assert bStale (or its inputs HB.black/white vs effBlack/effWhite) after
a range move under abStepBusy; add one line `assert(!app.lin.fitValid)` after the move
test's moveStackToBatch. These are one-to-three-line additions to existing tests.

The general lesson matches the last audit's null-pointer find: the selftests verify the
model's happy-path algebra perfectly and cannot see (1) time (interleaving, staleness),
(2) hostile input, (3) UI-layer state the model functions don't return. (1) and (2) are
cheap to add as above; (3) — e.g. fix 1's stale panel — needs the flag assertions, since
no headless test will ever see the drawn plot.

---

## 4. Does the series layer deliver what the canon promised?

Mostly yes, and the hard parts genuinely hold:

- **Membership only in Series::members** — verified: no SeqInfo::seriesId mirror exists
  anywhere; every consumer walks the series. No drift bug was found in four lenses. The
  decision is vindicated.
- **Unit UNSET means no fit** — holds on every UI path; the one leak is migration's use
  of the `"lx"` prefill (fix 5's second half), which is a bug against the canon, not a
  canon problem.
- **Never inferred from folder structure** — holds; even migrateLegacyLevels requires
  explicit seqlevel lines and skips stacks already in a series (4185).
- **Unset value excluded, never 0** — holds in the modal, the seriesmember reader, and
  linRecompute; violated only by the seqlevel branch (fix 5).

Where the implementation falls short of the canon's own text: the "count every dropped
point" contract (4126-4127, series-plan.md §3) is violated on four side paths (fixes 4,
8a-c); "batch 名は一意" (terminology.md:120) is asserted but unenforced on rename (fix 9);
series-plan.md §5's name-equality assumption is upheld by two of five stack producers
(fix 2).

Two places the CANON should move, not the code alone:

1. **series-plan.md §5 (name-based pending resolution).** The name contract is the root
   cause of the worst sweep bugs (fixes 2, 3), and no patch to name-matching can fix the
   rename race — the name is mutable in exactly the window resolution waits out. The
   canon should be amended to: *pending members are bound by an identity token stamped
   when the pending entry is created; display-name matching is the fallback for entities
   that predate tokens (session files list members by path, which stays).* The current
   sentence "PendingGroup::name＝SeqInfo::name だから解決できる" documents a coincidence,
   not a contract.

2. **Member identity is "path of frame 0" while the canon blesses same-path stacks.**
   terminology.md:21-23/115-116 explicitly permits the same path in two stacks and
   recommends move-then-group; the session format cannot represent the result (fixes
   8a/8b, npz variant). Either the canon gains one sentence — "同一 batch 内の同一パス
   stack は、セッションの往復で先勝ちに解決される" — making the limitation stated
   behavior with the loss *counted* (fix 8's counting is still mandatory), or the format
   grows a disambiguator (npzMember + an ordinal for same-path duplicates). I'd state the
   limitation now and grow the format when it first hurts a real workflow; the counting
   fix makes the failure loud, which is the canon's real requirement.

---

## 5. What I would NOT change

- **The rbDefer / RbDeferredActions machinery (003651b).** The invariant was attacked
  directly: the deferred-actions object is declared before the row vector, reverse
  destruction order guarantees rows die first, and every queued action was checked for
  container-invalidating effects. Nothing that replaces browse state still runs inline.
  It survived all four lenses — don't "simplify" it, and use it as the template for P2.
- **enqueueGroups' APPEND (4579-4585).** The append is the correct fix for a real silent
  cancellation; fix 10 is about scoping the recipe, not reverting the append.
- **The at-most-one-series-per-stack rip in seriesModalAccept (9144-9149) and in
  resolveSeriesRestore (4147-4149).** Correct policy; the defects are the dead-id
  revalidation (fix 6) and the accounting (fix 8b), not the rule.
- **abStepBusy's hold-and-label-stale contract (4367-4377, 8149-8152).** Right design for
  interactive stepping; fix 12 completes its stale predicate, nothing more.
- **The four canonical series decisions** (membership location, UNSET unit, no folder
  inference, unset-excluded values) — all confirmed load-bearing; every related defect
  found is a failure to *implement* them, never a reason to revisit them.
- **parseSeriesValue and the value-as-text session format (fmtExact writer).** This
  discipline is what made fixes 5 and 13 small and local; extend it (P4), don't dilute it.
- **migrateLegacyLevels' "an explicit series wins" guard (4185).** It provably blocks the
  modal-clobber path from the migration side; keep it exactly as is.
- **The clipper fix (e89dd51) and the sort-outside-the-table change.** Correct and
  minimal; IncludeItemByIndex before Begin() was the whole bug.
- **A/B naming by stack (39ce89f) and the legend row (baffa17).** Both do what they say;
  no defect found and the stack-name change is what makes two series distinguishable.
