// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---------------------------------------------------------------- CLI
static bool g_linSelftest = false;      // --lin-selftest: fit, print, exit
static bool g_fstatSelftest = false;    // --framestats-selftest: TSV to stdout, exit
static std::string g_rangeSelftest;      // --range-selftest <dir with 2 stacks>
static std::string g_scanSelftest;      // --scan-selftest <dir>: remote Open Folder, print, exit
static std::string g_pickerSelftest;    // --picker-selftest <dir>: filter cut + merge, print, exit
static std::string g_closeSelftest;     // --close-selftest <dir>: close per stack, print, exit
static std::string g_batchSelftest;     // --batch-selftest <dir>: move-to-batch + session, exit
static std::string g_rtemporalSelftest; // --rtemporal-selftest <dir>: browser temporal, exit
static std::string g_localbrowseSelftest; // --localbrowse-selftest <dir>: Browse (local), exit
static std::string g_fmtgateSelftest;   // --fmtgate-selftest <dir>: which listing asks
                                        // which question (#111), print, exit
static std::string g_browseSelftest;    // --browse-selftest <dir>: Browse panel behaviour, exit
static std::string g_verifySelftest;    // --verify-selftest <dir>: close/batch corners, exit
static std::string g_deriveSelftest;    // --derive-selftest <dir>: derive stack from stack, exit
static std::string g_stackAvgSelftest;  // --stackavg-selftest <dir>: stack mean as a frame, exit
static std::string g_abstatsSelftest;   // --abstats-selftest <dir>: A/B stats caches, exit
static std::string g_tileSelftest;      // --tile-selftest <dir>: side-by-side pane geometry, exit
static std::string g_seriesSelftest;    // --series-selftest <dir>: series invariants, exit
static std::string g_seriesPanelSelftest; // --seriespanel-selftest <dir>: Series Analysis
                                          // panel (mixed members, folds, fit window), exit
static std::string g_exportSelftest;    // --export-selftest <dir>: PNG + montage, exit
static std::string g_exportTsvSelftest; // --export-tsv-selftest <dir>: Temporal export document, exit
static std::string g_sweepFileSelftest; // --sweepfile-selftest <dir>: one .npy per level, exit
static std::string g_newwinSelftest;    // --newwin-selftest <dir>: instance slots + spawn line, exit
static std::string g_reloadSelftest;    // --reload-selftest <dir>: reload walk + temporal key, exit
static std::string g_srcmapSelftest;    // --srcmap-selftest <dir>: who holds which pixels, exit
static std::string g_mediaSelftest;     // --media-selftest <dir>: PNG/JPEG/TIFF through the seam, exit
static std::string g_fmtRegSelftest;    // --fmtreg-selftest <dir>: every format's place in the
                                        // source registry and the reload walk (§6.2 / §3.2), exit
static std::string g_asetSelftest;      // --aset-selftest <dir>: AnalysisSet transport/binding/session, exit
static std::string g_setAnalysisSelftest; // --setanalysis-selftest <dir>: direct DSNU / PRNU
                                          // over a bound set (#57 判断4) + the famous-name
                                          // scan of the shipping sources, exit
static std::string g_rsetSelftest;        // --rset-selftest <dir>: the same set analysis
                                          // folded on a peer and folded here, compared
                                          // field by field (MOP_SET_FOLD), exit
static std::string g_detrendSelftest;     // --detrend-selftest <dir>: the detrend
                                          // AnalysisPreProcessor (#57 判断6/7) - the raw
                                          // default, the shading figure, the product, exit
// --roistats-selftest: the ROI table's NUMBERS, through the real panel, with no
// window. It takes no directory because it needs no file: every fixture it
// measures is analytic and built in memory (see roiStatsSelftest()).
static bool g_roiStatsSelftest = false;

// --abeq-selftest: A and B (and a letter past B) on ONE document - the
// comparison people set up on purpose, to confirm the two sides agree. Takes no
// directory for the same reason as --roistats-selftest, and is windowless for
// the same reason: see abEqSelftest().
static bool g_abEqSelftest = false;

// --anaprov-selftest: analyzer results name the dll they came from (#46 stage
// 1, the host's ledger) - and builtin statistics wear no plugin tag. Takes no
// directory and no window for the --roistats-selftest reasons: the fixture is
// analytic, the assertions are strings. The analyzers are the REAL dlls
// startup loaded from exeDir()/plugins.
static bool g_anaProvSelftest = false;

// --stackana-selftest: the STACK mouth of ABI v3 (§5) and the partial-load
// contract (§6) - a real fixture dll pulling a real stack, the min_frames gate
// refusing before the call, and n/N crossing as facts. Takes no directory and
// no window for the --anaprov-selftest reasons: the stack is analytic and
// built in memory, and every assertion is a number or a string.
static bool g_stackAnaSelftest = false;

// --bundled-selftest: the five SHIPPED analyzers on the v3 mouth (docs/abi-v3.md
// §1's migration) - each declares a version, that version reaches the status
// line and the Copy-table document, and nothing else about the results moved.
// The second half is the reason it prints its whole result set to stdout: the
// evidence for "no number changed" is a diff of that output across the
// fail-first commit and the migration commit, not an assertion about a value
// this test would have had to learn from the code it is checking. Takes no
// directory and no window for the --anaprov-selftest reasons.
static bool g_bundledSelftest = false;

// --browse-keys-selftest <dir>: the Browse panel's KEYBOARD, driven through real
// frames. The other browse selftests call the panel's helpers directly, which
// is enough for what the rows contain but blind to everything that only exists
// inside a frame - and a single Down arrow segfaulted the process on an ImGui
// clipper call that a frameless test can never reach. So this one connects a
// LOCAL peer to <dir> in a hidden window and replays real UI actions into the
// real input queue, one per script slot: the panel cannot tell them from a
// human. Actions (--browse-keys overrides the canned list): focus, down, up,
// left, right, enter, home, end, back, flat, tree, disc, fmenu, rctx,
// esc, w<px>; comma / period (the preview scrub); altleft / altright (history);
// img0 (select the first image); natorder (flip the NAME column between
// natural and text order - relative, like flat / tree, and pinned back by
// viewreset); click / ctrlclick / dbl (real mouse clicks on
// the cursor's row - a double-click only exists as clicks); clickoff:N /
// dbloff:N (the same two, N rows BELOW the cursor - the only aim the keyboard
// has not already previewed, and so the only one where click one pays for the
// fetch, as a human's first click on a row always does); idle:N (hold N frames
// - a delay knob, so "two clicks a second apart" can be said); chevclick (a click
// on the cursor row's tree chevron, asserting the toggle landed at once);
// starclick (a click on the bookmark star at the end of the path line);
// pinclick (a click on the OUTERMOST row the pinned-ancestor band is holding,
// with chkpin:SPEC reading that band back as text - ";"-terminated names,
// outermost first, "-" for nothing pinned - and h<px> forcing the panel's
// height so the listing is short enough to have scrolled at all);
// marklist / starmark / seterr / clrerr (baselines and a fake failure for the
// drawer-removal checks below); mback / mfwd
// (mouse buttons 4 / 5); svtemp (the group row's server-temporal request);
// viewreset (absolute grouped+list+folded+natural-name state - the toggles are
// relative);
// waitimg:N / waitdir:LEAF (hold until N images are open / the browsed dir's
// leaf is LEAF); and the assertions chkimg:N (count + NAMES), chkpv:N,
// chkidx:K, chkopen:S, chkcur, chkcurn:NAME, chknames:SPEC, chkdir:LEAF,
// chkcursor:N (view index), chkatrow:NAME (the cursor row's name), chkback:N,
// chkfwd:N, chkexp:N, exparm / chkexpn:0 (a per-frame watch: the armed path
// was never expanded on ANY frame in between), chkfocus:0|1 - any FAIL fails
// the run.
//
// The "more" drawer's removal (docs/browse-topbar-design.md 10.2/10.3) is
// asserted by chktitle (the panel is named after its machine, both directions,
// with the ### id untouched), chkstat:N (the bottom status line's item and
// selected counts, both directions, and one line only), chkstar:0|1 (the star
// flipped from starmark and agrees with the bookmark list), chkerr:0|1 (a
// failure is IN the status line and the listing did not move - marklist is the
// baseline) and setpv:N / pvback / chkproto:0|1 (the same two claims for the
// protocol-mismatch notice, the other orange row that is gone). "more" is gone
// as an action along with the drawer it folded.
//
// INSTANCES (item 17): every stateful action and check drives the TARGET
// instance (target:N switches it; 1 = the primordial panel). newpanel creates
// a second Browse aimed at <dir>/scanroot and targets it; reconnect
// re-connects the target to <dir> post-"disc"; closep closes the target's
// window (extras are destroyed, the last one hides); hidep / showp drive
// app.showRemote (the View menu's flag); filt:S types S into the target's
// filter; chkfilt:S ("-" = empty), chkpanels:N (instance count), chkshown:N
// (windows visible), chksel:N (rows selected in the target).
//
// w<px> floats the panel at an exact width and then asserts what a human would
// otherwise have to read off a screenshot: that the filter box and the item
// that ENDS the toolbar row are still INSIDE the panel, and that the filter is
// wide enough to type a glob into. 271 is the panel's own default docked width
// on a 1600 px screen, which is where the fixed SameLine chain used to push
// both of them off-screen. (That last item was the "more" fold button and is
// the "..." panel menu now - same contract, different button.)
static int g_abRangeDefault = -1;      // App's compareRangeMode before loadPrefs
static std::string g_browseKeys;        // <dir>, empty = not running
// Key-routing evidence for --browse-keys-selftest: how many times the MAIN
// view's arrow/Home/End handler ran, and how many times it stood down because
// the Browse panel owned focus. The rule is one line of arithmetic - while the
// panel has focus the first must not move and the second must.
static int g_navKeyGlobal = 0;
static int g_navKeyYielded = 0;
static int g_navKeyAtBlur = -1, g_navKeyYieldAtBlur = -1;   // snapshot at "blur"
// Root-level popup collision evidence: "popupcheck" records whether the RAW
// dialog is still on the popup stack, once before and once after a competing
// root-level modal asks to open.
static int g_popupCheck[2] = { -1, -1 };
static int g_popupChecks = 0;
// Tree double-click purity evidence: "exparm" arms a PER-FRAME watch on the
// cursor row's path; until "chkexpn:0" every frame probes
// rbHas(expanded, path). An end-state check (chkexp) cannot see a flash -
// an expand executed on click one and cancelled on click two is state-
// correct at the end but was RENDERED between the clicks, and the eye sees
// every frame. hits counts the frames the path stood expanded.
static std::string g_expNeverPath;      // armed path; empty = not watching
static int g_expNeverHits = 0;          // frames rbHas(expanded, path) was true
static int g_expNeverFrames = 0;        // frames watched
static int g_chevPreExp = -1;           // "chevclick": expanded count pre-press
static bool g_browseKeysBlur = false;   // "blur" action: drop panel focus
// "imgmark" / "chkimgmark": how many documents were open before a gesture, and
// the assertion that the gesture opened none. An absolute chkimg:N says the
// same thing only as long as every count before it holds, which is a
// maintenance tax on a 258-action list; this pair says "this gesture opened
// nothing" in the two actions it takes to say it.
static int g_rbImgMark = -1;
static std::string g_browseKeysActs =
    "down,down,down,enter,flat,down,down,down,flat,down,tree,down,right,down,"
    "left,end,home,up,down,down,back,"
    // ---- what PREVIEW means for a stack row (docs/todo-open.md items 1/26).
    // Floated first (w400): the click actions aim real mouse events at the
    // cursor row's rectangle, and the user's saved layout must not decide
    // where that is. A grouped row previews as a poster + a 24-frame scrub,
    // ONE slot throughout; , / . step it; the server-temporal request may
    // surface the Temporal panel but must NOT steal the keys (chkfocus - the
    // scrub going dead right after asking for stats was defect 1).
    // (waitdir first: the prefix ends on "back", and keys fired before that
    // listing lands would walk the OLD rows and be reset mid-sequence.)
    "waitdir:rb,viewreset,w400,home,down,down,down,down,down,down,down,down,down,"
    "chkimg:1,chkpv:24,chkidx:0,period,chkidx:1,chknames:frame_001.npy[pv],"
    "comma,chkidx:0,svtemp,chkfocus:1,period,chkidx:1,comma,chkidx:0,"
    // Double-click = the stack opens exactly ONCE, poster dropped, nothing
    // else registered. The release half of the double-click used to re-run
    // the single-click path and park the poster NEXT TO the stack
    // (「一枚目とStackが2つFilesに登録される」) - the NAMES prove it stays gone.
    "dbl,waitimg:24,chkopen:1,chkimg:24,chknames:frame_###.npy*24,"
    // ...same overlap on a single-file row: promote once, no duplicate
    // preview, and Enter afterwards re-shows the open file instead of doing
    // nothing (the old final branch) or opening a second copy.
    "home,down,down,down,down,down,down,down,chkpv:0,dbl,chkimg:25,"
    "chknames:frame_###.npy*24+dark.npy,enter,chkimg:25,chkcurn:dark.npy,"
    // ...and a group whose numbering starts at 9 (padset): grouping, preview
    // scrub and the double-click open all work by POSITION, not by the
    // parsed number (the user's "連番の開始が0じゃないから?" hypothesis).
    "home,down,down,down,down,enter,waitdir:padset,down,down,chkpv:3,chkidx:0,"
    "period,chkidx:1,dbl,waitimg:28,chkopen:2,chkimg:28,"
    "chknames:frame_###.npy*24+dark.npy+f_9.npy+f_10.npy+f_11.npy,"
    "back,waitdir:rb,"
    // ---- folder rows in a LIST: one click ENTERS. A list has no "open it
    // where it is", so the single click is the folder's whole vocabulary and
    // there is no double-click meaning left to give it. Mouse back/forward and
    // Alt+Left/Right walk the history; a fresh navigation truncates forward.
    "home,down,chkatrow:digitset,click,waitdir:digitset,chkback:5,"
    "chkfwd:0,mback,waitdir:rb,chkfwd:1,mfwd,waitdir:digitset,chkfwd:0,"
    "altleft,waitdir:rb,chkfwd:1,altright,waitdir:digitset,altleft,waitdir:rb,"
    // ---- THE ORDER THE ROWS ARE IN (user decision 2026-08-05: natural).
    // rb/unpadded is the one fixture whose names are not zero-padded, so it is
    // the only one where the two candidate orders differ at all: read as TEXT,
    // lv10 falls between lv1 and lv2. Walking down with the arrow keys asserts
    // the order the panel actually laid out, through the real sort - and the
    // group row below the folders makes the same claim about the file names,
    // because the peer's extent spans frame_1 to frame_10 only if the run was
    // read by value.
    //
    // Placed after the history checks and before the next chkfwd: entering a
    // folder and backing out costs two history entries (chkback above counts
    // them) and truncates the forward stack (the double-click below sets it
    // back to 0 itself).
    //
    // This is the on-screen half. --browse-selftest owns the other half: that
    // this is also the order the stack is BUILT in.
    "home,down,down,down,down,down,down,enter,waitdir:unpadded,"
    "home,down,chkatrow:lv1,down,chkatrow:lv2,down,chkatrow:lv10,"
    "down,chkatrow:frame_1\xE2\x80\xA5" "10.npy,"
    "back,waitdir:rb,"
    // ...and a folder that is DOUBLE-clicked is entered ONCE, and opens
    // NOTHING. The habit is older than this panel, so the second click will
    // arrive whatever the list mode means by one click; it lands a frame or
    // two after rbGoTo, by which time the row it was aimed at has been
    // replaced under a pointer that never moved. This is the assertion the
    // one-click design lives or dies on, and it is the one CI produced the
    // counter-example for: imgs 44 -> 52 between "dbl" and the listing
    // landing - eight frames of the folder just entered, opened by a click
    // aimed at the folder. Checked twice, because an open is asynchronous and
    // one check right after the navigation could beat it.
    "home,down,chkatrow:digitset,imgmark,dbl,waitdir:digitset,chkimgmark,"
    "chkfwd:0,back,waitdir:rb,chkimgmark,"
    "home,down,down,click,waitdir:expset,chkfwd:0,back,waitdir:rb,"
    // ...in a TREE a folder has TWO verbs, so both gestures are spoken for:
    // the name toggles it in place, a double-click goes to it. That is not the
    // ce02f12 latch this file used to forbid - THAT expanded on click one and
    // CANCELLED on click two, so the expand was rendered and then undone. Here
    // click one expands and it STAYS expanded; the double-click adds a
    // navigation on top. Nothing is undone, so there is nothing to flash.
    //
    // The toggles are asserted on THIS ROW now (chkrowexp), which is what the
    // gap named here used to be: chkexp counts every expanded folder in the
    // panel, so "the name-click toggled this row" was only expressible when
    // the panel started fully collapsed - and the keyboard section far above
    // leaves one open with Right, so it was not expressible at all. cursorFull
    // is the exact key `expanded` is written in, so the question is asked
    // directly.
    //
    // The two toggles ALTERNATE name / chevron on purpose: two clicks at the
    // same pixel inside ImGui's double-click window are one gesture with a
    // count of two, not two gestures, and the second would be read as a
    // double-click. The name (row centre + 40 px) and the chevron (the left
    // gutter) are far enough apart that each press starts a fresh chain, so
    // this reads the same at any frame rate.
    "tree,home,down,chkatrow:digitset,chkrowexp:0,"
    "click,chkrowexp:1,chevclick,chkrowexp:0,"
    "click,chkrowexp:1,chevclick,chkrowexp:0,"
    // ...and the OTHER verb, on the same row: a double-click navigates, once,
    // and opens nothing on the way (the tree's navigation happens on the
    // second PRESS, so its release is one frame behind - the same window the
    // list's is two frames behind).
    "imgmark,dbl,waitdir:digitset,chkimgmark,back,waitdir:rb,tree,"
    // ---- Enter on a multi-selection opens EVERY selected row, each group
    // as its own stack (the action-row button is the MERGE; this is the other
    // one - only the cursor's row used to open).
    "home,down,down,down,enter,waitdir:gainset,down,down,ctrlclick,down,ctrlclick,"
    "enter,waitimg:44,chkopen:4,"
    "chknames:frame_###.npy*24+dark.npy+f_9.npy+f_10.npy+f_11.npy"
    "+gain10_00#.npy*8+gain20_00#.npy*8,"
    "back,waitdir:rb,"
    // ...then hand focus back to nobody and repeat four of the same keys: the
    // main view must own them again, or the gate would be "the arrows are dead"
    // rather than "the panel owns them while it has focus"
    "blur,down,up,end,home,"
    // ...then the panel geometry sweep: the toolbar must stay inside the panel
    // at every docked width, and Escape must close a menu and a context popup.
    // Swept with EVERY chip lit (viewreset pins the absolute state first, then
    // one toggle each): a chip only appears when its setting is off the
    // default, so the widest the toolbar can ever be is the case no default
    // run would ever draw - which is exactly the one where the filter box gets
    // pushed off the right edge.
    "viewreset,flat,tree,natorder,"
    "w271,w200,w180,w420,w700,w1150,w271,w700,w180,w0,"
    "viewreset,rctx,esc,fmenu,esc,disc,"
    // ---- INSTANCES (docs/todo-open.md item 17: the panel stopped being a
    // singleton). Reconnect the primordial panel, then open a SECOND Browse
    // standing in a DIFFERENT place at the same moment - the one sentence a
    // singleton can never make true (one App::rbrowse.dir = one place, ever).
    "reconnect,waitdir:rb,chkpanels:1,"
    "newpanel,chkpanels:2,waitdir:scanroot,chkdir:scanroot,"
    "target:1,chkdir:rb,"
    // independence: a filter typed into one panel does not narrow the other
    "filt:frame,chkfilt:frame,target:2,chkfilt:-,"
    // the keyboard belongs to the FOCUSED instance: panel 2 holds focus (it
    // was just created), so Down moves ITS cursor and leaves panel 1's alone
    "down,down,chkatrow:10lx,chkfocus:1,target:1,chkcursor:-1,target:2,"
    // selection is per instance too
    "ctrlclick,chksel:1,target:1,chksel:0,target:2,"
    // ...and so is the history: navigating panel 2 records nothing in panel 1
    "imgmark,dbl,waitdir:10lx,chkimgmark,chkback:1,target:1,chkback:0,"
    // focus back on panel 1: the same keys now move ITS cursor, and panel 2's
    // (reset to -1 by its own navigation) stays put
    "focus,chkfocus:1,down,chkcursor:0,target:2,chkcursor:-1,"
    // a session round-trip (item 10, decision A5): both places are written as
    // rbplace lines, every instance is torn down, and the restore reconnects
    // BOTH - each through its own worker - to where they stood
    "sessrt,chkpanels:2,target:1,waitdir:rb,target:2,waitdir:10lx,"
    // closing the second leaves the first fully working...
    "closep,chkpanels:1,target:1,chkdir:rb,focus,down,down,"
    "chkatrow:frame_000\xE2\x80\xA5" "023.npy,"
    // ...and the LAST one closing only hides: View > Panels > Browse reopens
    // it with its place, listing and cursor intact
    "hidep,chkshown:0,showp,chkshown:1,chkdir:rb,focus,up,chkatrow:..,"
    // ---- THE DRAWER IS GONE (docs/browse-topbar-design.md 10.2/10.3). Each
    // of these asserts one of its contents in the place it moved to, which is
    // the only way to tell "rehomed" from "deleted". Floated first so the path
    // line has room for the star and the click can be aimed at it.
    // The title names the machine; the status line counts what is in front of
    // you in both directions; the star is lit iff the place is bookmarked and
    // survives a real click at the end of the path line; and a failure speaks
    // ON the status line without opening a band that moves the listing.
    // (the filter from the instance segment is still on here, so the first
    // chkstat reads the narrowed form "N of M items"; clearing it reads the
    // plain "M items" - the count in both directions)
    "w400,chktitle,chkstat:0,"
    // ...a GROUPED row: one row, twenty-four frames, and the line says both
    // (the filter from the instance segment leaves exactly that row showing)
    "home,down,chkatrow:frame_000\xE2\x80\xA5" "023.npy,"
    "ctrlclick,chkstat:1,chkframes:24,ctrlclick,chkstat:0,"
    "filt:,marklist,chkstat:0,"
    "starmark,starclick,chkstar:1,starclick,chkstar:0,"
    "seterr,chkerr:1,clrerr,chkerr:0,"
    "setpv:2,chkproto:1,setpv:99,chkproto:1,pvback,chkproto:0,"
    // ...and a plain row: one row, one thing, no second count. Placed from
    // "home" because clearing the filter above rebuilt the shown set and reset
    // the cursor - a bare "down" would land on ".." , which selects nothing.
    "home,down,chkatrow:digitset,ctrlclick,chkstat:1,chkframes:0,w0,"
    // ---- the levels above the reader do not scroll away -------------------
    // 「階層表示で下にスクロールすると上層の階層が上に行って消える」. Every
    // ancestor of the row at the top of the viewport is held above it now, at
    // any depth, with no setting - so this asserts the two things a setting
    // would otherwise have to be trusted for.
    //
    // FIRST, that they are on screen. chkpin reads the band back as text,
    // outermost first, which is the same order and the same words a human
    // reads off the panel - not a flag saying the feature is on.
    // rb/expset/zdeep exists for exactly this shape of question: its last
    // sixty rows are all three levels down, so scrolling to the end is
    // guaranteed to leave the reader inside a/b/c. The panel is floated SHORT
    // (h360) as well, because "the fixture was longer than this screen" is not
    // a thing a gate should rest on - on a taller monitor the same script
    // would pass by never scrolling at all.
    //
    // SECOND, that a pinned row is the row and not a picture of it. pinclick
    // aims a real mouse click at the outermost pinned rectangle; what answers
    // is the verb a folder row has always had in a tree - collapse it - and
    // the proof is that one action later its whole subtree, and the band with
    // it, is gone. imgmark/chkimgmark says the same click opened nothing,
    // which is also what clicking a folder has always not done.
    "viewreset,w400,h360,waitdir:rb,home,"
    "down,down,chkatrow:expset,enter,waitdir:expset,"
    "home,down,chkatrow:zdeep,enter,waitdir:zdeep,"
    "tree,home,down,chkatrow:a,chkpin:-,right,idle:10,"
    "down,chkatrow:b,right,idle:10,"
    "down,chkatrow:c,right,idle:20,"
    "end,chkpin:a;b;c;,"
    "imgmark,pinclick,chkimgmark,chkpin:-,"
    "h0,w0,viewreset,back,waitdir:expset,back,waitdir:rb,"
    // ...and LAST the root-level popup collision: open the RAW dialog for a
    // QUEUE, then ask for the sequence prompt, which is the other root-level
    // modal. The RAW dialog must survive - which is why this runs last: it
    // deliberately ENDS with a modal up, and Escape leaves modals alone
    // (theirs is a decision), so anything checking Escape after it would be
    // reading the surviving modal rather than its own popup.
    "rawopen,popupcheck,seqask,popupcheck";

static void printUsage() {
    // The picture extensions come from the format table, not from a literal
    // here: parseCli already dispatches on imagefile::forPath, so a literal
    // list is a second statement of the same fact that can only ever go stale
    // (it said ".png, .jpg" for the whole time TIFF was readable).
    printf(
        "usage: viewer [options] [files or folders...]\n"
        "  files:  .npy, .npz, .vsession (saved session), %s,\n"
        "          or raw binaries (.bin/.raw/...)\n", imagefile::dialogPattern().c_str());
    printf(
        "  folder: loads the numbered files below it, one stack per group\n"
        "options:\n"
        "  --session <file.vsession>   restore a saved session\n"
        "  --raw-dtype <t>             storage of one sample: u8|u16|f32|f64\n"
        "  --raw-interp <i>            meaning of samples: gray|rgb|bgr|rgba|bgra|bayer|quad-bayer\n"
        "  --raw-format <fmt>          legacy combined names (gray8|...|rgbf32|bayer8|bayer16)\n"
        "  --raw-size <WxH>            raw dimensions, e.g. 1920x1080\n"
        "  --raw-crop <x,y,WxH>        decode only a window, e.g. 100,200,640x480\n"
        "  --raw-offset <bytes>        raw header offset (default 0)\n"
        "  --big-endian                raw byte order (default little endian)\n"
        "  --bayer-pattern <p>         RGGB|BGGR|GRBG|GBRG (default RGGB)\n"
        "  --quad-bayer                treat the CFA as Quad Bayer\n"
        "  --cfa <none|bayer|quad>     1ch files (.npy included) arrive mosaiced\n"
        "  --stack <mode>              numbered siblings: ask (default) | always | never\n"
        "  --mem-budget <GB>           what the stack loader may hold (default: auto,\n"
        "                              60%% of physical RAM)\n"
        // %s, not a copy: this line said "(H,W,3|4)" while the refusal it is
        // describing said something else, which is exactly the drift issue #71
        // was. There is now one spelling and it lives in one place.
        "  .npy shapes read natively:  %s\n"
        "                              any other shape is refused by name; to read one\n"
        "                              differently, use \"re-read as...\" in the Inspector\n"
        "  ssh://user@host/path.npy    view a file on another machine: the UI stays\n"
        "                              here, only the visible region is fetched\n"
        "  ssh://user@host/~/dir       connect and browse there instead (a host on\n"
        "                              its own works too) - what a desktop shortcut\n"
        "                              made by tools/install_shortcut.* passes\n"
        "  --remote-exe <path>         how to start the peer there (default:\n"
        "                              ~/.viewer/viewer-serve, self-installed on the\n"
        "                              first connection)\n"
        "  --remote-policy <p>         where a measurement runs: auto (default, on the\n"
        "                              server) | local-fetch (pull tiles, measure here)\n"
        "  --serve                     BE that peer: answer requests on stdin/stdout\n"
        "  --compare <off|wipe|split|diff>  A/B compare the first two images given\n"
        "  --bench <frames>            render N frames offscreen, print frame-time stats, exit\n"
        "  --bench-tiles               ...with A, B and two extra slots tiled side by\n"
        "                              side, and report the panes the canvas drew\n"
        "  --bench-step                ...and step A one frame per bench frame (A/B\n"
        "                              follow-frame cost: both sides recompute)\n"
        "  --bench-panels              ...with Projection, Histogram and Temporal side\n"
        "                              by side rather than tabbed, so all three draw\n"
        "  --no-ab-throttle            do NOT hold the B statistics while stepping\n"
        "  --gl-probe                  can this machine make the GL context the viewer\n"
        "                              needs? name it and exit (0 yes, 3 no + why)\n"
        "  --no-window                 start WITHOUT a window or a GL context, for the\n"
        "                              selftests that never draw (nothing else runs)\n"
        "  --frame <system|integrated> title bar: the desktop's, or the one drawn\n"
        "                              in the menu bar (default: last used)\n"
        "  --window-offset <dx,dy>     shift the window off its default position (how\n"
        "                              \"Open in new window\" cascades the child ~40 px)\n"
        "  --secondary                 a spawned extra window: prefs are read-only\n"
        "  --zoom <z>                  initial zoom (1 = 100%%)\n"
        "  --center <x,y>              initial view center in image pixels\n"
        "  -h, --help                  show this help\n"
        // All of them, always. docs/manual.md 9 promises this list is the canon,
        // and it said 8 of 22 for long enough that --verify-selftest could be
        // believed not to exist. Adding a flag means adding its line here.
        //
        // It happened again, quietly, and was found by an audit rather than by
        // anyone missing it: --reload-selftest had a parser, a ctest target and
        // no line here, so the canon was one short for as long as that test has
        // existed. The COUNT is gone with it - "all 22" was written when there
        // were 22 and was never true again. A number here is a second copy of a
        // fact that lives in CMakeLists.txt, and this is the second time that
        // copy has gone stale; tools/run_selftests.sh prints the real one, per
        // run, computed from ctest.
        "self-tests (load, check, print, exit; 0 = pass). ctest runs every one:\n"
        "  --lin-selftest              linearity fit over the stacks given\n"
        "  --sweepfile-selftest <dir>  a sweep of ONE .npy per level: names + fit\n"
        "  --series-selftest <dir>     series (系列) model + invariants\n"
        "  --seriespanel-selftest <dir> Series Analysis panel: frame|stack mixed\n"
        "                              members, folds, fit window/method, session\n"
        "  --range-selftest <dir>      every A/B display-range combination (2 stacks)\n"
        "  --framestats-selftest       per-frame statistics, TSV to stdout\n"
        "  --abstats-selftest <dir>    A/B statistics caches: two slots\n"
        "  --tile-selftest <dir>       side-by-side compare panes: geometry\n"
        "  --export-selftest <dir>     PNG export + montage\n"
        "  --export-tsv-selftest <dir> the Temporal export document\n"
        "  --picker-selftest <dir>     stack picker: filter cut + merge\n"
        "  --scan-selftest <dir>       Open Folder: every stack below a root\n"
        "  --close-selftest <dir>      closing, per stack\n"
        "  --batch-selftest <dir>      move-to-batch + session round trip\n"
        "  --verify-selftest <dir>     the corners the others miss (V1-V18)\n"
        "  --aset-selftest <dir>       AnalysisSet: transport, Ref binding, session\n"
        "  --roistats-selftest         the ROI table's numbers and its std/mean column,\n"
        "                              through the real panel, on analytic fixtures\n"
        "  --abeq-selftest             A and B on ONE document: both sides drawn,\n"
        "                              coinciding, through the real panel\n"
        "  --anaprov-selftest          analyzer results name the plugin dll that\n"
        "                              computed them (the host's ledger, #46)\n"
        "  --bundled-selftest          the five shipped analyzers on ABI v3: each\n"
        "                              declares a version, and no result moved\n"
        "  --derive-selftest <dir>     derive a stack from a stack: counts, copy, follow\n"
        "  --stackavg-selftest <dir>   a stack's per-pixel mean opened as one frame:\n"
        "                              value, NaN exclusion, CFA planes, provenance\n"
        "  --detrend-selftest <dir>    the detrend preprocessor stage (#57 judgment 6/7):\n"
        "                              the raw default, the shading figure it always\n"
        "                              prints, the product, and the product's recipe\n"
        "  --srcmap-selftest <dir>     which document holds which pixels: shared sources,\n"
        "                              refcounts, Watch baselines\n"
        "  --media-selftest <dir>      PNG/JPEG/TIFF bit depth, channels and notes;\n"
        "                              a multi-page TIFF as a stack, and the refusals\n"
        "  --reload-selftest <dir>     a frame's FILE rewritten under a live stack:\n"
        "                              the invalidation walk, the temporal key, the\n"
        "                              registry re-key, the recorded npy reading\n"
        "  --fmtreg-selftest <dir>     which formats reach the registry and that walk:\n"
        "                              one row per backend plus container members -\n"
        "                              registers / reload reaches / a second open shares\n"
        "  --newwin-selftest <dir>     instance autosave slots + spawn line\n"
        "  --browse-selftest <dir>     Browse panel behaviour\n"
        "  --localbrowse-selftest <dir>  Browse against the local filesystem\n"
        "  --fmtgate-selftest <dir>    which Browse listing asks which question:\n"
        "                              what this build reads vs what the peer serves\n"
        "  --browse-keys-selftest <dir>  the Browse panel's KEYBOARD, replayed as real\n"
        "                              input into real frames (--browse-keys <script>\n"
        "                              overrides the canned action list)\n"
        "  --rtemporal-selftest <dir>  temporal analysis driven from the browser\n"
        "  --remote-selftest <file>    remote round trip against a peer started here;\n"
        "                              pass --remote-exe to test viewer-serve itself\n"
        "  --rplugin-selftest <file>   plugin analysis on the peer (ABI v3 §10):\n"
        "                              name+version parity, its refusals, and the\n"
        "                              stack mouth pulled over the link\n"
        "  --rset-selftest <dir>       set analysis folded on the peer (MOP_SET_FOLD):\n"
        "                              the same rows folded there and here, compared\n"
        "                              field by field, plus the fold-parity and\n"
        "                              too-old-peer refusals\n",
        NPY_NATIVE_FORMS);
}

static void parseCli(int argc, char** argv) {
    RawDialog d;                       // accumulates --raw-* options for positional raw files
    bool rawReady = false, cliQuad = false;
    bool haveZoom = false, haveCenter = false;
    float zoom = 1, cx = 0, cy = 0;
    int cliCompare = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--session") {
            std::string p = next();
            if (!p.empty()) openPath(p);
        } else if (a == "--raw-format") {          // legacy combined names
            std::string v = next();
            bool found = false;
            for (int f = 0; f < 12; f++)
                if (v == LEGACY_RAW_NAMES[f]) {
                    d.dtype = LEGACY_RAW_MAP[f][0];
                    d.interp = LEGACY_RAW_MAP[f][1];
                    rawReady = found = true;
                }
            if (!found) fprintf(stderr, "unknown raw format: %s (see --help)\n", v.c_str());
        } else if (a == "--raw-dtype") {
            std::string v = next();
            for (int f = 0; f < RD_COUNT; f++)
                if (v == RAW_DTYPE_NAMES[f]) { d.dtype = f; rawReady = true; }
        } else if (a == "--raw-interp") {
            std::string v = next();
            for (int f = 0; f < RI_COUNT; f++)
                if (v == RAW_INTERP_CLI[f]) { d.interp = f; rawReady = true; }
        } else if (a == "--raw-size") {
            std::string v = next();
            size_t x = v.find_first_of("xX*");
            if (x != std::string::npos) {
                d.w = std::clamp(atoi(v.substr(0, x).c_str()), 1, 32768);
                d.h = std::clamp(atoi(v.substr(x + 1).c_str()), 1, 32768);
            }
        } else if (a == "--raw-crop") {            // x,y,WxH
            std::string v = next();
            int x = 0, y = 0, w = 0, h = 0;
            if (sscanf(v.c_str(), "%d,%d,%dx%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
                d.cropOn = true;
                d.cropX = std::max(0, x); d.cropY = std::max(0, y);
                d.cropW = w; d.cropH = h;
            } else {
                fprintf(stderr, "bad --raw-crop (expected x,y,WxH)\n");
            }
        } else if (a == "--raw-offset") {
            d.offset = std::max(0, atoi(next().c_str()));
        } else if (a == "--big-endian") {
            d.littleEndian = false;
        } else if (a == "--bayer-pattern") {
            std::string v = next();
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            for (int p = 0; p < 4; p++)
                if (v == CFA_PATTERNS[p]) d.cfaPattern = p;
            // ...and forward it to --cfa NOW, so the two can be given in either
            // order. Only --cfa used to publish the pattern, which made
            // "--cfa bayer --bayer-pattern GRBG" demosaic as RGGB without a word
            // - the CMake tests write exactly that order and only escape it
            // because RGGB is index 0. Nothing reads forceCfaPattern unless
            // --cfa was given (addImage tests forceCfa >= 0 first).
            app.forceCfaPattern = d.cfaPattern;
        } else if (a == "--quad-bayer") {
            cliQuad = true;                        // applied at load; order-independent
            rawReady = true;
        } else if (a == "--npy-axis") {
            // docs/input-adapters.md §3.4. One global flag decided the meaning
            // of every 3-D array in a run, so a session holding one file to read
            // as a stack and another to read as colour could not be expressed at
            // all. Say so and carry on - silence would leave the user believing
            // the old meaning still applied.
            next();                                // swallow its value, not a path
            fprintf(stderr, "--npy-axis is gone: (F,H,W) is F frames and (H,W,C<=4) is one "
                            "frame of C channels, always. To read one file differently, open "
                            "it and use \"re-read as...\" in the Inspector - it is per file "
                            "and it is remembered.\n");
        } else if (a == "--stack") {               // ask | always | never
            std::string v = next();
            if (v == "always") app.seqLoadMode = 1;
            else if (v == "never") app.seqLoadMode = 2;
            else if (v == "ask") app.seqLoadMode = 0;
            else fprintf(stderr, "--stack expects ask|always|never\n");
        } else if (a == "--bench" || a == "--crash-test" || a == "--frame" ||
                   a == "--window-offset") {
            next();                                // consumed in main(), not an error
        } else if (a == "--bench-step" || a == "--bench-tiles" || a == "--bench-panels" ||
                   a == "--secondary" || a == "--no-window") {
            /* consumed in main(): no value */
        } else if (a == "--no-ab-throttle") {
            g_abNoThrottle = true;     // measure what the B-slot throttle saves
        } else if (a == "--cfa") {                 // none | bayer | quad
            std::string v = next();
            app.forceCfa = v == "bayer" ? 1 : v == "quad" || v == "quad-bayer" ? 2
                         : v == "none" ? 0 : -1;
            if (app.forceCfa < 0) fprintf(stderr, "--cfa expects none|bayer|quad\n");
            app.forceCfaPattern = d.cfaPattern;    // --bayer-pattern seen so far, if any
        } else if (a == "--lin-selftest") {
            g_linSelftest = true;                  // handled in main() after loading
        } else if (a == "--range-selftest") {
            g_rangeSelftest = next();
        } else if (a == "--framestats-selftest") {
            g_fstatSelftest = true;                // handled in main() after loading
        } else if (a == "--export-tsv-selftest") {
            g_exportTsvSelftest = next();          // handled in main()
        } else if (a == "--roistats-selftest") {
            g_roiStatsSelftest = true;             // handled in main()
        } else if (a == "--abeq-selftest") {
            g_abEqSelftest = true;                 // handled in main()
        } else if (a == "--anaprov-selftest") {
            g_anaProvSelftest = true;              // handled in main()
        } else if (a == "--stackana-selftest") {
            g_stackAnaSelftest = true;             // handled in main()
        } else if (a == "--bundled-selftest") {
            g_bundledSelftest = true;              // handled in main()
        } else if (a == "--scan-selftest") {
            g_scanSelftest = next();               // handled in main()
        } else if (a == "--picker-selftest") {
            g_pickerSelftest = next();             // handled in main()
        } else if (a == "--close-selftest") {
            g_closeSelftest = next();              // handled in main()
        } else if (a == "--batch-selftest") {
            g_batchSelftest = next();              // handled in main()
        } else if (a == "--rtemporal-selftest") {
            g_rtemporalSelftest = next();          // handled in main()
        } else if (a == "--localbrowse-selftest") {
            g_localbrowseSelftest = next();        // handled in main()
        } else if (a == "--fmtgate-selftest") {
            g_fmtgateSelftest = next();            // handled in main()
        } else if (a == "--browse-selftest") {
            g_browseSelftest = next();             // handled in main()
        } else if (a == "--browse-keys-selftest") {
            g_browseKeys = next();                 // handled in main()'s frame loop
        } else if (a == "--browse-keys") {
            g_browseKeysActs = next();             // override the canned action list
        } else if (a == "--verify-selftest") {
            g_verifySelftest = next();             // handled in main()
        } else if (a == "--aset-selftest") {
            g_asetSelftest = next();               // handled in main()
        } else if (a == "--setanalysis-selftest") {
            g_setAnalysisSelftest = next();        // handled in main()
        } else if (a == "--rset-selftest") {
            g_rsetSelftest = next();               // handled in main()
        } else if (a == "--detrend-selftest") {
            g_detrendSelftest = next();            // handled in main()
        } else if (a == "--derive-selftest") {
            g_deriveSelftest = next();             // handled in main()
        } else if (a == "--stackavg-selftest") {
            g_stackAvgSelftest = next();           // handled in main()
        } else if (a == "--abstats-selftest") {
            g_abstatsSelftest = next();            // handled in main()
        } else if (a == "--tile-selftest") {
            g_tileSelftest = next();               // handled in main()
        } else if (a == "--export-selftest") {
            g_exportSelftest = next();             // handled in main()
        } else if (a == "--series-selftest") {
            g_seriesSelftest = next();             // handled in main()
        } else if (a == "--seriespanel-selftest") {
            g_seriesPanelSelftest = next();        // handled in main()
        } else if (a == "--sweepfile-selftest") {
            g_sweepFileSelftest = next();          // handled in main()
        } else if (a == "--newwin-selftest") {
            g_newwinSelftest = next();             // handled in main()
        } else if (a == "--reload-selftest") {
            g_reloadSelftest = next();             // handled in main()
        } else if (a == "--srcmap-selftest") {
            g_srcmapSelftest = next();             // handled in main()
        } else if (a == "--media-selftest") {
            g_mediaSelftest = next();              // handled in main()
        } else if (a == "--fmtreg-selftest") {
            g_fmtRegSelftest = next();             // handled in main()
        } else if (a == "--remote-selftest") {
            next();
        } else if (a == "--rplugin-selftest") {
            next();                                // handled before startup, in main()
        } else if (a == "--remote-policy") {        // auto | local-fetch
            std::string v = next();
            if (v == "auto") app.procPolicy = App::PolAuto;
            else if (v == "server") {          // accepted, so scripts keep working
                app.procPolicy = App::PolAuto;
                fprintf(stderr, "--remote-policy server is retired (it was identical to "
                                "auto): using auto\n");
            }
            else if (v == "local-fetch" || v == "local") app.procPolicy = App::PolLocalFetch;
            else fprintf(stderr, "--remote-policy expects auto|local-fetch\n");
        } else if (a == "--remote-exe") {          // how to invoke the peer over ssh
            app.remoteExe = next();
        } else if (a == "--mem-budget") {          // GB the stack loader may use
            app.memBudgetGB = std::clamp((float)atof(next().c_str()), 0.5f, 4096.0f);
        } else if (a == "--compare") {             // off | wipe | split
            std::string v = next();
            if (v == "wipe") cliCompare = App::CmpWipe;
            else if (v == "split" || v == "side") cliCompare = App::CmpSplit;
            else if (v == "diff") cliCompare = App::CmpDiff;
            else if (v == "off") cliCompare = App::CmpOff;
            else fprintf(stderr, "--compare expects off|wipe|split|diff\n");
        } else if (a == "--zoom") {
            zoom = (float)atof(next().c_str()); haveZoom = true;
        } else if (a == "--center") {
            std::string v = next();
            size_t c = v.find(',');
            if (c != std::string::npos) {
                cx = (float)atof(v.substr(0, c).c_str());
                cy = (float)atof(v.substr(c + 1).c_str());
                haveCenter = true;
            }
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "unknown option: %s (see --help)\n", a.c_str());
        } else if (std::filesystem::is_directory(pathFromUtf8(a))) {
            openFolder(a);                     // a folder = every stack below it
        } else {
            std::string low = a;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            bool special = (low.size() > 4 && low.compare(low.size() - 4, 4, ".npy") == 0) ||
                           (low.size() > 4 && low.compare(low.size() - 4, 4, ".npz") == 0) ||
                           (low.size() > 9 && low.compare(low.size() - 9, 9, ".vsession") == 0) ||
                           // ...and a picture format, which carries its own
                           // dimensions and dtype: --raw-* on the same command
                           // line is for the files that carry none
                           imagefile::forPath(a) != nullptr;
            if (!special && rawReady) {   // raw params given: load directly, no dialog
                if (cliQuad && RAW_INTERP_CH[d.interp] == 1) d.interp = RI_QUAD;
                d.path = a;
                std::string err = loadRaw(d);
                if (!err.empty()) toast(baseName(a) + ": " + err, true);
                else maybeOfferSequence(app.current);
            } else {
                openPath(a);
            }
        }
    }
    // Applied later, not here: with --stack always the sibling frames are still
    // arriving on the loader thread, so at this point there may be only one image.
    if (cliCompare >= 0) app.pendingCompare = cliCompare;
    if (haveZoom || haveCenter) {
        if (haveZoom) app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        if (haveCenter) app.view.center = ImVec2(cx, cy);
        else if (cur()) app.view.center = ImVec2(cur()->w * 0.5f, cur()->h * 0.5f);
        app.fitRequested = false;      // explicit view from CLI wins over fit-on-load
    }
}

// MEASURE selftest glue: capture what an analyzer emits, formatted the same way
// on both sides so the comparison is string-exact.
static std::vector<std::pair<std::string, std::string>>* g_mstRows;
static void mstNum(void*, const char* k, double v) {
    char b[64];
    snprintf(b, 64, "%.9g", v);
    g_mstRows->emplace_back(k ? k : "", b);
}
static void mstTxt(void*, const char* k, const char* v) {
    g_mstRows->emplace_back(k ? k : "", v ? v : "");
}
static void mstSer(void*, const char*, const char*, const char*,
                   const float*, const float*, uint32_t) {}

// Round-trip check for the remote path, without needing an ssh host: start a
// local peer, ask for the same pixels two ways, and compare. A viewer that shows
// subtly wrong pixels over a link would be worse than one that shows none.
static int remoteSelfTest(const char* exe, const char* path) {
    remote::Session s;
    std::string err;
    if (!s.start("", exe, err)) { fprintf(stderr, "selftest: %s\n", err.c_str()); return 1; }
    remote::Meta m;
    if (!s.meta(path, m, err)) { fprintf(stderr, "selftest META: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "selftest: meta %dx%d %dch %s frames=%d\n", m.w, m.h, m.ch,
            m.dtype.c_str(), m.frames);

    std::string localErr = loadNpy(path);          // the local decoder, for reference
    if (!localErr.empty() || !cur()) {
        fprintf(stderr, "selftest: local load failed: %s\n", localErr.c_str());
        return 1;
    }
    const ImageDoc& ref = *cur();
    int bad = 0;
    std::string dir = path, base = path;       // the test file's folder and name
    {
        size_t sl = dir.find_last_of("/\\");
        if (sl == std::string::npos) dir = ".";
        else { dir = dir.substr(0, sl); base = base.substr(sl + 1); }
    }
    {   // LIST v3: the listing's metadata must agree with the local loader
        std::vector<remote::Entry> ents;
        if (!s.list(dir, ents, err)) {
            fprintf(stderr, "selftest LIST: %s\n", err.c_str());
            return 1;
        }
        const remote::Entry* me = nullptr;
        for (const auto& e : ents) if (e.name == base) me = &e;
        for (const auto& e : ents)                 // grouping may have folded it
            if (!me && e.group)
                for (const auto& mm : e.members) if (mm == base) me = &e;
        size_t bad0 = 0;
        if (!me) {
            fprintf(stderr, "selftest LIST: %s not in the listing of %s\n",
                    base.c_str(), dir.c_str());
            bad0++;
        } else {
            uint64_t px = 1;
            for (int i = 0; i < me->ndim; i++) px *= me->dims[i];
            uint64_t refPx = (uint64_t)m.frames * ref.w * ref.h * ref.ch;
            if (!me->hasMeta || me->dtype != m.dtype || px != refPx) {
                fprintf(stderr, "selftest LIST: meta mismatch (dtype %s vs %s, "
                                "%llu vs %llu samples)\n",
                        me->dtype.c_str(), m.dtype.c_str(),
                        (unsigned long long)px, (unsigned long long)refPx);
                bad0++;
            }
            if (me->mtime <= 0) {
                fprintf(stderr, "selftest LIST: mtime missing (%lld unix s)\n",
                        (long long)me->mtime);
                bad0++;
            }
        }
        fprintf(stderr, "selftest: LIST v3 %s: %s  %s  mtime %lld unix s : %s\n",
                base.c_str(), me ? fmtEntryShape(*me).c_str() : "?",
                me ? fmtBytesHuman(me->size).c_str() : "?",
                me ? (long long)me->mtime : 0, bad0 ? "FAIL" : "ok");
        bad += bad0 ? 1 : 0;
    }
    {   // A v2 peer's LIST reply must parse into "unknown" metadata, not into an
        // error - this is the compatibility contract, tested without a v2 binary.
        std::vector<uint8_t> pl;
        auto pu32 = [&](uint32_t v) { pl.insert(pl.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        auto pstr = [&](const std::string& t) { pu32((uint32_t)t.size());
                                                pl.insert(pl.end(), t.begin(), t.end()); };
        pu32(2);
        pstr("a.npy"); pu32(0); pu32(5); pu32(0);      // file, 5 bytes
        pstr("sub");   pu32(1); pu32(0); pu32(0);      // directory
        std::vector<remote::Entry> v2;
        std::string perr;
        bool ok = remote::parseListPayload(pl, 2, v2, perr) && v2.size() == 2 &&
                  !v2[0].dir && v2[0].size == 5 && !v2[0].hasMeta &&
                  v2[0].mtime == 0 && !v2[0].group && v2[1].dir;
        fprintf(stderr, "selftest: LIST v2-compat parse (missing fields -> unknown): %s\n",
                ok ? "ok" : "FAIL");
        bad += ok ? 0 : 1;
    }
    {   // Grouping: 24 numbered frames fold into ONE synthetic row; the loose
        // .npy and the .txt stay plain. Fixture from tools/gen_testdata.py.
        std::string rb = dir + "/rb";
        std::vector<remote::Entry> ents;
        if (!s.list(rb, ents, err)) {
            fprintf(stderr, "selftest: LIST grouping: skipped (%s: %s)\n",
                    rb.c_str(), err.c_str());
        } else {
            int nGroups = 0, nPlain = 0;
            const remote::Entry* g = nullptr;
            for (const auto& e : ents) {
                if (e.dir) continue;                   // scanroot/ lives here too
                if (e.group) { nGroups++; g = &e; }
                else nPlain++;
            }
            // the pattern SHOWS ITS EXTENT: no bare '?' run survives into the
            // name a user reads (rp::patternWithExtent)
            bool ok = nGroups == 1 && nPlain == 3 && g && g->frames == 24 &&
                      g->members.size() == 24 && g->hasMeta && g->dtype == "f32" &&
                      g->name == "frame_000\xE2\x80\xA5" "023.npy";
            // the member names must lead back to real files, or the row is a lie
            remote::Meta gm;
            if (ok && !s.meta(rb + "/" + g->members[0], gm, err)) {
                fprintf(stderr, "selftest LIST grouping: member META: %s\n", err.c_str());
                ok = false;
            }
            fprintf(stderr, "selftest: LIST grouping %s: %d group(s), %d single(s), "
                            "%u frames %s: %s\n",
                    g ? g->name.c_str() : "?", nGroups, nPlain,
                    g ? g->frames : 0, g ? fmtEntryShape(*g).c_str() : "?",
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Grouping stage 2: the extent covers ONLY the varying digit run.
        // gainset has a constant gain digit next to the frame counter - the
        // pattern must keep it LITERAL (gain10_000..007.npy, never
        // gain10..20_...), or two gains read as one stack.
        std::string gd = dir + "/rb/gainset";
        std::vector<remote::Entry> ents;
        if (!s.list(gd, ents, err)) {
            fprintf(stderr, "selftest: LIST gainset: skipped (%s: %s)\n",
                    gd.c_str(), err.c_str());
        } else {
            std::vector<std::string> pats;
            bool frames8 = true;
            int nPlain = 0;
            for (const auto& e : ents) {
                if (e.dir) continue;
                if (e.group) { pats.push_back(e.name); frames8 &= e.frames == 8; }
                else nPlain++;
            }
            std::sort(pats.begin(), pats.end());
            bool ok = pats.size() == 2 && nPlain == 0 && frames8 &&
                      pats[0] == "gain10_000\xE2\x80\xA5" "007.npy" &&
                      pats[1] == "gain20_000\xE2\x80\xA5" "007.npy";
            fprintf(stderr, "selftest: LIST grouping gainset -> %d group(s) [%s], "
                            "8 frames each=%d: %s\n",
                    (int)pats.size(),
                    (pats.empty() ? std::string("?")
                                  : pats.size() < 2 ? pats[0] : pats[0] + "," + pats[1]).c_str(),
                    frames8 ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Grouping stage 2, split: expset varies TWO digit runs. One stack per
        // condition (g00_e??.npy / g01_e??.npy), never a second '?' run - a
        // condition-mixed stack has a meaningless sigma_t.
        std::string ed = dir + "/rb/expset";
        std::vector<remote::Entry> ents;
        if (!s.list(ed, ents, err)) {
            fprintf(stderr, "selftest: LIST expset: skipped (%s: %s)\n",
                    ed.c_str(), err.c_str());
        } else {
            std::vector<std::string> pats;
            bool frames4 = true;
            int nPlain = 0;
            for (const auto& e : ents) {
                if (e.dir) continue;
                if (e.group) { pats.push_back(e.name); frames4 &= e.frames == 4; }
                else nPlain++;
            }
            std::sort(pats.begin(), pats.end());
            bool ok = pats.size() == 2 && nPlain == 0 && frames4 &&
                      pats[0] == "g00_e00\xE2\x80\xA5" "03.npy" &&
                      pats[1] == "g01_e00\xE2\x80\xA5" "03.npy";
            fprintf(stderr, "selftest: LIST grouping expset (2 varying runs) -> "
                            "%d group(s) [%s], 4 frames each=%d: %s\n",
                    (int)pats.size(),
                    (pats.empty() ? std::string("?")
                                  : pats.size() < 2 ? pats[0] : pats[0] + "," + pats[1]).c_str(),
                    frames4 ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Pattern extent, and the agreement between the two ends. digitset is
        // all digits ("????.npy" by the rule, which says nothing); padset has
        // uneven padding, so the extent has to be read by VALUE. For each, the
        // peer's LIST pattern and the pattern the CLIENT builds for the same
        // folder (findSequenceSiblings) must be the identical string - a
        // capture opened locally and listed over ssh names one stack, not two.
        struct PatCase { const char* sub; const char* first; const char* want; };
        const PatCase pcs[] = {
            { "digitset", "0000.npy", "0000\xE2\x80\xA5" "0003.npy" },
            { "padset",   "f_9.npy",  "f_9\xE2\x80\xA5" "11.npy" },
            { "gainset",  "gain10_000.npy", "gain10_000\xE2\x80\xA5" "007.npy" },
        };
        for (const PatCase& pc : pcs) {
            std::string pd = dir + "/rb/" + pc.sub;
            std::vector<remote::Entry> ents;
            if (!s.list(pd, ents, err)) {
                fprintf(stderr, "selftest: pattern %s: skipped (%s)\n", pc.sub, err.c_str());
                continue;
            }
            std::string peerPat;
            for (const auto& e : ents) if (e.group && peerPat.empty()) peerPat = e.name;
            std::string clientPat;
            findSequenceSiblings(pd + "/" + pc.first, clientPat);
            bool ok = peerPat == pc.want && clientPat == pc.want;
            fprintf(stderr, "selftest: pattern %s -> peer '%s', client '%s' "
                            "(want '%s'): %s\n",
                    pc.sub, peerPat.c_str(), clientPat.c_str(), pc.want,
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
        {   // The pattern is ALSO the stack name, and linearity's Auto levels
            // parses stack names. It reads the FOLDER part - verified here, not
            // assumed - so a changed file part is safe; and where there is no
            // folder part the extent run is cut out first, so an all-digit
            // stack still reports "no level" exactly as "????.npy" did.
            double folder = extractLevelFromName("10lx/0000\xE2\x80\xA5" "0003.npy");
            double bare   = extractLevelFromName("0000\xE2\x80\xA5" "0003.npy");
            double gain   = extractLevelFromName("gain10_000\xE2\x80\xA5" "007.npy");
            bool ok = folder == 10.0 && !std::isfinite(bare) && gain == 10.0;
            fprintf(stderr, "selftest: auto-level from '10lx/...' -> %g, from a bare "
                            "extent -> %s, from 'gain10_...' -> %g: %s\n",
                    folder, std::isfinite(bare) ? "a number (WRONG)" : "none", gain,
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // SCAN (the remote openFolder): 3 illumination folders -> 3 stacks
        std::string sroot = dir + "/rb/scanroot";
        std::vector<remote::ScanGroup> gs;
        bool trunc = false;
        int skipped = 0;
        if (!s.scan(sroot, 6, 256, gs, trunc, skipped, err)) {
            fprintf(stderr, "selftest: SCAN: skipped (%s: %s)\n", sroot.c_str(), err.c_str());
        } else {
            bool ok = gs.size() == 3 && !trunc;
            std::string dirsSeen;
            for (const auto& g : gs) {
                dirsSeen += (dirsSeen.empty() ? "" : ",") + g.dir;
                if (g.entry.frames != 8 || g.entry.members.size() != 8 ||
                    !g.entry.hasMeta || g.dir.empty())
                    ok = false;
            }
            fprintf(stderr, "selftest: SCAN %s -> %d stack(s) [%s], 8 frames each, "
                            "truncated=%d, %d dir(s) skipped: %s\n",
                    sroot.c_str(), (int)gs.size(), dirsSeen.c_str(),
                    trunc ? 1 : 0, skipped, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // GLOB: recursive find - full count, cap-5 truncation, bare substring
        std::string groot = dir + "/rb";
        std::vector<remote::GlobHit> hits;
        bool trunc = false;
        int skipped = 0;
        if (!s.glob(groot, "**/frame_*.npy", 6, 2000, hits, trunc, skipped, err)) {
            fprintf(stderr, "selftest: GLOB: skipped (%s: %s)\n", groot.c_str(), err.c_str());
        } else {
            // 24 in rb/ itself + 3 x 8 under scanroot/ + 3 under unpadded/
            // (frame_1, frame_2, frame_10 - the unpadded names, which is the
            //  whole of what that fixture contributes here)
            bool ok = hits.size() == 51 && !trunc;
            for (const auto& h : hits) if (h.dir) ok = false;
            fprintf(stderr, "selftest: GLOB **/frame_*.npy under %s -> %d hit(s), "
                            "truncated=%d: %s\n",
                    groot.c_str(), (int)hits.size(), trunc ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;

            if (!s.glob(groot, "**/frame_*.npy", 6, 5, hits, trunc, skipped, err)) {
                fprintf(stderr, "selftest GLOB cap: %s\n", err.c_str());
                bad++;
            } else {
                bool ok2 = hits.size() == 5 && trunc;
                fprintf(stderr, "selftest: GLOB cap 5 -> %d hit(s), truncated=%d: %s\n",
                        (int)hits.size(), trunc ? 1 : 0, ok2 ? "ok" : "FAIL");
                bad += ok2 ? 0 : 1;
            }
            if (!s.glob(groot, "dark", 6, 2000, hits, trunc, skipped, err)) {
                fprintf(stderr, "selftest GLOB substring: %s\n", err.c_str());
                bad++;
            } else {
                bool ok3 = hits.size() == 1 && hits[0].rel == "dark.npy";
                fprintf(stderr, "selftest: GLOB substring 'dark' -> %d hit(s) (%s): %s\n",
                        (int)hits.size(), hits.empty() ? "?" : hits[0].rel.c_str(),
                        ok3 ? "ok" : "FAIL");
                bad += ok3 ? 0 : 1;
            }
        }
    }
    struct Case { int x, y, w, h, step; };
    const Case cases[] = {
        { 0, 0, ref.w, ref.h, 1 },                              // whole frame
        { 0, 0, ref.w, ref.h, 3 },                              // decimated overview
        { ref.w / 4, ref.h / 4, ref.w / 2, ref.h / 2, 1 },      // zoomed-in crop
        { 1, 1, 17, 13, 2 },                                    // odd rect, odd stride
    };
    for (const Case& c : cases) {
        std::vector<float> got;
        int gw = 0, gh = 0, gch = 0;
        std::string dt;
        if (!s.tile(path, 0, c.x, c.y, c.w, c.h, c.step, got, gw, gh, gch, dt, err)) {
            fprintf(stderr, "selftest TILE: %s\n", err.c_str());
            return 1;
        }
        size_t mismatch = 0;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                for (int ci = 0; ci < gch; ci++) {
                    float a = got[((size_t)y * gw + x) * gch + ci];
                    float b = ref.sample(c.x + x * c.step, c.y + y * c.step, ci);
                    if (a != b && !(std::isnan(a) && std::isnan(b))) mismatch++;
                }
        static uint64_t prevRx = 0;
        uint64_t wire = s.bytesReceived() - prevRx;
        prevRx = s.bytesReceived();
        fprintf(stderr, "selftest: rect %dx%d@%d,%d step %d -> %dx%d %dch %s : %s "
                        "(%zu mismatches, %.2f MB on the wire for %.2f MB of samples)\n",
                c.w, c.h, c.x, c.y, c.step, gw, gh, gch, dt.c_str(),
                mismatch ? "FAIL" : "ok", mismatch,
                wire / 1048576.0, (double)got.size() * 4 / 1048576.0);
        bad += mismatch ? 1 : 0;
    }
    {   // A blocked protocol read must be interruptible, or Quit waits it out
        // with a dead window on screen: the workers poll their stop flags only
        // BETWEEN queue items, so one parked inside recv() never saw them.
        std::atomic<bool> stopFlag{ false };
        remote::Session s2;
        s2.setAbort(&stopFlag);
        std::string e2;
        std::vector<remote::Entry> ents2;
        bool started = s2.startOn("", 0, exe, e2);
        bool listedOk = started && s2.list(dir, ents2, e2);
        stopFlag = true;                       // exactly what stop*() sets
        double at0 = nowSec();
        bool listedAfter = s2.list(dir, ents2, e2);
        double asecs = nowSec() - at0;
        bool abortOk = started && listedOk && !listedAfter && asecs < 1.0;
        fprintf(stderr, "selftest: abortable read: LIST before the flag %s, after it %s "
                        "in %.3f s: %s\n",
                listedOk ? "ok" : "FAILED", listedAfter ? "STILL SUCCEEDED" : "gave up",
                asecs, abortOk ? "ok" : "FAIL");
        bad += abortOk ? 0 : 1;
    }
    {   // A TILE reply sizes an allocation, so it is validated against the
        // REQUEST and not only against itself. Both hostile shapes below pass
        // every self-consistency test the client used to apply.
        struct C { const char* what; uint32_t rw, rh, st, w, h, ch, dt, raw; bool want; };
        const C cases[] = {
            // 64-bit overflow: 2^30 x 2^30 x 4 x 4 bytes is exactly 2^64, which
            // wraps to 0 and matches rawBytes = 0. toFloat then resized to 2^62
            // floats and std::length_error killed the process.
            { "overflow to 0", 512, 512, 1, 0x40000000u, 0x40000000u, 4, rp::DT_F32, 0, false },
            // no overflow at all: fully self-consistent, ~4 MB of deflate on the
            // wire (under recv's 512 MB cap), 4 GB of allocation on arrival
            { "deflate bomb", 512, 512, 1, 32768, 32767, 1, rp::DT_U32, 4294836224u, false },
            { "more rows than asked", 512, 512, 1, 512, 513, 1, rp::DT_U16, 512u*513*2, false },
            { "bad dtype", 512, 512, 1, 8, 8, 1, rp::DT_COUNT, 64, false },
            { "byte count disagrees", 512, 512, 1, 8, 8, 1, rp::DT_U16, 64, false },
            { "legitimate full tile", 512, 512, 1, 512, 512, 1, rp::DT_U16, 512u*512*2, true },
            { "legitimate decimated", 513, 513, 3, 171, 171, 3, rp::DT_F32, 171u*171*3*4, true },
        };
        int badT = 0;
        for (const C& c : cases) {
            bool got = remote::tileReplySane(c.rw, c.rh, c.st, c.w, c.h, c.ch, c.dt, c.raw);
            if (got != c.want) {
                fprintf(stderr, "selftest TILE guard: %s -> %s, wanted %s\n",
                        c.what, got ? "accepted" : "rejected", c.want ? "accepted" : "rejected");
                badT++;
            }
        }
        fprintf(stderr, "selftest: TILE reply guard over %d shape(s): %s\n",
                (int)(sizeof cases / sizeof cases[0]), badT ? "FAIL" : "ok");
        bad += badT ? 1 : 0;
    }
    // MEASURE: the same plugin must produce the same numbers on both sides.
    // Same code, same f32 input - the comparison is string-exact on %.9g.
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins" },
                         [](const std::string&, bool) {});
    const auto& anas = plugin_host::analyzers();
    int pick = -1;
    for (int i = 0; i < (int)anas.size(); i++)
        if (anas[i].name.find("stats") != std::string::npos) { pick = i; break; }
    if (pick < 0 && !anas.empty()) pick = 0;
    if (pick >= 0) {
        const AnalyzerPluginInfo& a = anas[pick];
        std::vector<std::pair<std::string, std::string>> local;
        g_mstRows = &local;
        psFrame fr = makeFrame(ref);
        psRect roi{ (uint32_t)(ref.w / 4), (uint32_t)(ref.h / 4),
                    (uint32_t)(ref.w / 2), (uint32_t)(ref.h / 2) };
        char perr[256];
        perr[0] = 0;
        int rc;
        if (a.abi == 3) {
            psAnalyzeSink3 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
            rc = a.v3.analyze(&fr, &roi, &sk, perr, sizeof perr);
        } else if (a.abi == 2) {
            psAnalyzeSink2 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
            rc = a.v2.analyze(&fr, &roi, &sk, perr, sizeof perr);
        } else {
            psAnalyzeSink sk{ nullptr, mstNum, mstTxt };
            rc = a.v1.analyze(&fr, &roi, &sk, perr, sizeof perr);
        }
        if (rc != 0) {
            fprintf(stderr, "selftest MEASURE: local run failed: %s\n", perr);
            return 1;
        }
        remote::MeasureReq q;
        q.op = rp::MOP_ANALYZER;
        q.paths = { path };
        q.analyzer = a.name;
        q.cfaType = ref.cfa;
        q.cfaPattern = ref.cfaPattern;
        q.black = effBlack(ref);
        q.white = effWhite(ref);
        q.rois.push_back({ ref.w / 4, ref.h / 4, ref.w / 2, ref.h / 2 });
        remote::MeasureResult mr;
        if (!s.measure(q, mr, err)) {
            fprintf(stderr, "selftest MEASURE: %s\n", err.c_str());
            return 1;
        }
        size_t bad2 = 0;
        if (mr.cols.size() != 1 || mr.cols[0].size() != local.size()) {
            fprintf(stderr, "selftest MEASURE: shape mismatch (%zu cols, %zu vs %zu items)\n",
                    mr.cols.size(), mr.cols.empty() ? 0 : mr.cols[0].size(), local.size());
            bad2++;
        } else {
            for (size_t i = 0; i < local.size(); i++) {
                const remote::MeasureItem& it = mr.cols[0][i];
                char b[64];
                std::string got = it.kind == 0
                    ? (snprintf(b, 64, "%.9g", it.num), std::string(b)) : it.text;
                if (it.key != local[i].first || got != local[i].second) {
                    fprintf(stderr, "selftest MEASURE mismatch: %s: '%s' vs local '%s'\n",
                            it.key.c_str(), got.c_str(), local[i].second.c_str());
                    bad2++;
                }
            }
        }
        fprintf(stderr, "selftest: MEASURE %s on the peer [loc=%s]: %s (%zu keys)\n",
                a.name.c_str(), mr.serverLoc ? "gpu" : "cpu",
                bad2 ? "FAIL" : "ok", local.size());
        bad += bad2 ? 1 : 0;
        // ...and the same again with the frame declared MOSAICED. CFA planes
        // are never mixed (docs/stats-taxonomy.md), so stats/moments must
        // bucket by plane (R/Gr/Gb/B) instead of pooling into one "ch0" - and
        // both sides must do it identically, or a remote measurement and a
        // local one of the same pixels disagree.
        if (ref.ch == 1) {
            ImageDoc mosaic = ref;                  // same pixels, declared Bayer
            // the implicit copy ALIASES ref's source (one shared_ptr, two docs)
            // - stage 1's invariant is one owner per source, and makeFrame's
            // const-cast pointer must aim at a throwaway, not the live doc
            mosaic.src = cloneSource(*mosaic.src);
            mosaic.cfa = 1;
            mosaic.cfaPattern = 0;                  // RGGB
            std::vector<std::pair<std::string, std::string>> local2;
            g_mstRows = &local2;
            psFrame fr2 = makeFrame(mosaic);
            psRect roi2{ (uint32_t)(ref.w / 4), (uint32_t)(ref.h / 4),
                         (uint32_t)(ref.w / 2), (uint32_t)(ref.h / 2) };
            perr[0] = 0;
            if (a.abi == 3) {
                psAnalyzeSink3 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
                rc = a.v3.analyze(&fr2, &roi2, &sk, perr, sizeof perr);
            } else if (a.abi == 2) {
                psAnalyzeSink2 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
                rc = a.v2.analyze(&fr2, &roi2, &sk, perr, sizeof perr);
            } else {
                psAnalyzeSink sk{ nullptr, mstNum, mstTxt };
                rc = a.v1.analyze(&fr2, &roi2, &sk, perr, sizeof perr);
            }
            int planeKeys = 0;
            for (const auto& kv : local2)
                if (kv.first.rfind("R.", 0) == 0 || kv.first.rfind("Gr.", 0) == 0 ||
                    kv.first.rfind("Gb.", 0) == 0 || kv.first.rfind("B.", 0) == 0)
                    planeKeys++;
            int pooledKeys = 0;
            for (const auto& kv : local2) if (kv.first.rfind("ch0.", 0) == 0) pooledKeys++;
            remote::MeasureReq q2 = q;
            q2.cfaType = 1; q2.cfaPattern = 0;
            remote::MeasureResult mr2;
            size_t bad3 = 0;
            if (rc != 0 || !s.measure(q2, mr2, err)) {
                fprintf(stderr, "selftest MEASURE cfa: %s\n", rc ? perr : err.c_str());
                bad3++;
            } else if (mr2.cols.size() != 1 || mr2.cols[0].size() != local2.size()) {
                fprintf(stderr, "selftest MEASURE cfa: shape mismatch (%zu vs %zu items)\n",
                        mr2.cols.empty() ? 0 : mr2.cols[0].size(), local2.size());
                bad3++;
            } else {
                for (size_t i = 0; i < local2.size(); i++) {
                    const remote::MeasureItem& it = mr2.cols[0][i];
                    char b2[64];
                    std::string got = it.kind == 0
                        ? (snprintf(b2, 64, "%.9g", it.num), std::string(b2)) : it.text;
                    if (it.key != local2[i].first || got != local2[i].second) {
                        fprintf(stderr, "selftest MEASURE cfa mismatch: %s: '%s' vs "
                                        "local '%s'\n", it.key.c_str(), got.c_str(),
                                local2[i].second.c_str());
                        bad3++;
                    }
                }
            }
            bool split = planeKeys >= 4 * 8 && pooledKeys == 0;
            fprintf(stderr, "selftest: MEASURE %s on a Bayer frame: %d per-plane key(s), "
                            "%d pooled ch0 key(s), local==peer: %s\n",
                    a.name.c_str(), planeKeys, pooledKeys,
                    (split && !bad3) ? "ok" : "FAIL");
            bad += (split && !bad3) ? 0 : 1;
            g_mstRows = nullptr;
        }
    }
    // Aggregates: recompute the same statistics client-side from tiles the tile
    // path already proved correct, and compare. Frame-axis files only.
    if (m.frames >= 2) {
        remote::MeasureReq q;
        q.op = rp::MOP_TEMPORAL_STATS;
        q.paths = { path };
        remote::MeasureResult mr;
        if (!s.measure(q, mr, err)) {
            fprintf(stderr, "selftest TEMPORAL: %s\n", err.c_str());
            return 1;
        }
        // reference: per-pixel sums in f64 over all frames
        size_t samples = (size_t)m.w * m.h * m.ch;
        std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
        for (int f = 0; f < m.frames; f++) {
            std::vector<float> px;
            int tw, th, tch;
            std::string dt2;
            if (!s.tile(path, f, 0, 0, m.w, m.h, 1, px, tw, th, tch, dt2, err)) {
                fprintf(stderr, "selftest TEMPORAL tile: %s\n", err.c_str());
                return 1;
            }
            for (size_t i = 0; i < samples; i++) {
                double v = px[i];
                sum[i] += v; sum2[i] += v * v;
            }
        }
        double N = m.frames, aM = 0, aM2 = 0, aV = 0, aC = 0;
        for (size_t i = 0; i < samples; i++) {
            double mm = sum[i] / N;
            double v = std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
            aM += mm; aM2 += mm * mm;
            aV += v;
            aC += v / N;                       // s_t,i^2 / n_i; no NaN in this fixture
        }
        double ns = (double)samples;
        double refMean = aM / ns;
        double refSt = sqrt(aV / ns);
        // The CORRECTED sigma_fpn (#57 judgment 2, docs/flat-field-stats.md (b)):
        // var_spatial(M) at ddof=1 minus C = mean_i(s_t,i^2/n_i), clamped at 0.
        // This reference exists to disagree with serve.cpp if serve.cpp drifts,
        // so it is written from the settled formula rather than copied from it.
        double refCorr = aC / ns;
        double refVar = std::max(0.0, aM2 / ns - refMean * refMean) * (ns / (ns - 1.0));
        double refFpn = sqrt(std::max(0.0, refVar - refCorr));
        double refCorrDn = sqrt(refCorr);
        double refClamp = refVar - refCorr < 0.0 ? 1.0 : 0.0;
        auto findNum = [&](const char* k) {
            for (const auto& it : mr.cols[0])
                if (it.kind == 0 && it.key == k) return it.num;
            return -1.0;
        };
        auto rel = [](double a, double b) {
            return fabs(a - b) / std::max({ fabs(a), fabs(b), 1e-12 });
        };
        size_t bad3 = 0;
        struct { const char* k; double ref; } checks[] = {
            { "mean [DN]", refMean }, { "sigma_t [DN]", refSt }, { "sigma_fpn [DN]", refFpn },
            // the correction and the clamp are part of the answer, so they are
            // part of what the peer has to get right
            { "fpn_corr [DN]", refCorrDn }, { "fpn_clamped", refClamp },
        };
        for (const auto& c : checks)
            if (rel(findNum(c.k), c.ref) > 1e-9) {
                fprintf(stderr, "selftest TEMPORAL mismatch: %s server %.12g vs ref %.12g\n",
                        c.k, findNum(c.k), c.ref);
                bad3++;
            }
        bool seriesOk = mr.series.size() == 2 &&
                        (int)mr.series[0].ys.size() == m.frames &&
                        (int)mr.series[1].ys.size() == m.frames;
        if (!seriesOk) { fprintf(stderr, "selftest TEMPORAL: bad series shape\n"); bad3++; }
        fprintf(stderr, "selftest: TEMPORAL_STATS over %d frames: %s\n",
                mr.framesUsed, bad3 ? "FAIL" : "ok");
        bad += bad3 ? 1 : 0;

        // FRAME_ROI_STATS on an off-center ROI
        remote::MeasureReq q2;
        q2.op = rp::MOP_FRAME_ROI_STATS;
        q2.paths = { path };
        q2.rois.push_back({ 3, 5, m.w / 2, m.h / 2 });
        remote::MeasureResult r2;
        if (!s.measure(q2, r2, err)) {
            fprintf(stderr, "selftest FRAME_ROI: %s\n", err.c_str());
            return 1;
        }
        size_t bad4 = 0;
        const remote::MeasureSeries* mrs = nullptr;
        for (const auto& se : r2.series) if (se.name == "roi mean") mrs = &se;
        if (!mrs || (int)mrs->ys.size() != m.frames) {
            fprintf(stderr, "selftest FRAME_ROI: bad series\n");
            bad4++;
        } else {
            for (int f = 0; f < m.frames; f++) {          // reference per frame
                std::vector<float> px;
                int tw, th, tch;
                std::string dt2;
                if (!s.tile(path, f, 3, 5, m.w / 2, m.h / 2, 1, px, tw, th, tch, dt2, err))
                    { bad4++; break; }
                double s1 = 0;
                for (float v : px) s1 += v;
                double refM = s1 / (double)px.size();
                if (rel((double)mrs->ys[f], refM) > 1e-6) {   // series is f32
                    fprintf(stderr, "selftest FRAME_ROI mismatch at frame %d: %.9g vs %.9g\n",
                            f, mrs->ys[f], refM);
                    bad4++;
                }
            }
        }
        fprintf(stderr, "selftest: FRAME_ROI_STATS over %d frames: %s\n",
                r2.framesUsed, bad4 ? "FAIL" : "ok");
        bad += bad4 ? 1 : 0;
    }
    {   // ---- the channel rule, through BOTH doors (issue #71) ---------------
        //
        // This whole harness exists to compare what the LOCAL decoder makes of
        // a file against what the PEER makes of it, so it was always the thing
        // that would catch core/main.cpp and core/serve.cpp disagreeing. It had
        // simply never been aimed at a shape where they did. main.cpp read the
        // last axis as channels only when it was exactly 3 or 4; serve.cpp read
        // it as channels whenever it was 4 or fewer. So (H,W,1) - what
        // img[:, :, None] produces, and an ordinary way to save one mono
        // capture - opened here as H frames of 1xW and there as one WxH frame.
        // Pointed at a (48,640,1) fixture before the rule was unified, this
        // printed exactly that:
        //
        //   selftest: meta 640x48 1ch u16 frames=1        <- the peer: 1 frame
        //   npy stack: ... 48 of 48 frames (1x640 1ch)    <- here: 48 frames
        //   selftest: rect 1x640@0,0 step 1 -> 1x48 1ch u16 : FAIL (47 mismatches)
        //
        // Aiming an existing test is worth more than writing a new one: this
        // one was built to catch this class of fault and had been sitting
        // unused against it.
        //
        // The sweep walks the last axis ACROSS the ceiling - 1,2,3,4 are one
        // frame of C channels, 5 is a stack - because the fault was never
        // "channels or not", it was the two sides putting the boundary in
        // different places. A rule that agrees at 3 and 4 and nowhere else is
        // the bug that was here.
        //
        // Fixtures are written here rather than read from tools/testdata for
        // two reasons: that tree is gitignored and generated, so a machine
        // without it would silently skip the check; and a fixture that lives
        // apart from the rule it pins can drift away from it.
        std::error_code cec;
        std::filesystem::path cdir =
            std::filesystem::temp_directory_path(cec) / "viewer_chanrule";
        std::filesystem::remove_all(cdir, cec);
        std::filesystem::create_directories(cdir, cec);
        std::vector<uint16_t> ramp(48 * 40 * 5);
        for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (uint16_t)(i * 11 + 5);
        struct Want { int64_t lastAxis; int frames, w, h, ch; const char* why; };
        const Want SWEEP[] = {
            { 1, 1, 40, 48, 1, "(H,W,1) is ONE mono frame" },
            { 2, 1, 40, 48, 2, "(H,W,2) is ONE two-channel frame" },
            { 3, 1, 40, 48, 3, "(H,W,3) is ONE colour frame" },
            { 4, 1, 40, 48, 4, "(H,W,4) is ONE RGBA frame" },
            { 5, 48, 5, 40, 1, "(H,W,5) is past the ceiling: a stack" },
        };
        size_t badC = 0;
        for (const Want& t : SWEEP) {
            const std::vector<int64_t> shape = { 48, 40, t.lastAxis };
            std::string p = npyWriteFile(
                (cdir / ("chan_48x40x" + std::to_string(t.lastAxis) + ".npy")).u8string(),
                "<u2", shape, ramp.data(), (size_t)48 * 40 * t.lastAxis * 2);
            remote::Meta cm;
            std::string cerr;
            if (!s.meta(p, cm, cerr)) {
                fprintf(stderr, "selftest CHANNEL META %s: %s\n",
                        npyShapeText(shape).c_str(), cerr.c_str());
                badC++;
                continue;
            }
            closeAll();
            std::string lerr = loadNpy(p);
            const ImageDoc* lref = cur();
            if (!lerr.empty() || !lref) {
                fprintf(stderr, "selftest CHANNEL local %s: %s\n",
                        npyShapeText(shape).c_str(),
                        lerr.empty() ? "nothing opened" : lerr.c_str());
                badC++;
                continue;
            }
            int lframes = (int)app.images.size();
            // The geometry first: the two doors must MAKE the same thing of the
            // file before comparing pixels is even a meaningful question.
            bool geom = lframes == cm.frames && lref->w == cm.w &&
                        lref->h == cm.h && lref->ch == cm.ch;
            bool wanted = lframes == t.frames && lref->w == t.w &&
                          lref->h == t.h && lref->ch == t.ch;
            // ...then the pixels, which is where a geometry agreement that is
            // only a coincidence would still come apart.
            size_t mism = 0;
            std::vector<float> got;
            int gw = 0, gh = 0, gch = 0;
            std::string gdt;
            if (!s.tile(p, 0, 0, 0, lref->w, lref->h, 1, got, gw, gh, gch, gdt, cerr)) {
                fprintf(stderr, "selftest CHANNEL TILE %s: %s\n",
                        npyShapeText(shape).c_str(), cerr.c_str());
                badC++;
                continue;
            }
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++)
                    for (int ci = 0; ci < gch; ci++)
                        if (got[((size_t)y * gw + x) * gch + ci] != lref->sample(x, y, ci))
                            mism++;
            bool ok = geom && wanted && mism == 0;
            fprintf(stderr, "selftest: channel rule %-13s local %d frame(s) %dx%d %dch"
                            " == peer %d frame(s) %dx%d %dch, %zu pixel mismatch(es)"
                            " : %s  (%s)\n",
                    npyShapeText(shape).c_str(), lframes, lref->w, lref->h, lref->ch,
                    cm.frames, cm.w, cm.h, cm.ch, mism, ok ? "ok" : "FAIL", t.why);
            badC += ok ? 0 : 1;
        }
        closeAll();
        std::filesystem::remove_all(cdir, cec);          // ours: ours to remove
        fprintf(stderr, "selftest: channel rule over %d shape(s), both doors: %s\n",
                (int)(sizeof SWEEP / sizeof SWEEP[0]), badC ? "FAIL" : "ok");
        bad += badC ? 1 : 0;
    }
    {   // ---- the REFUSAL, through both doors (issue #71 D4/D5) -------------
        //
        // The block above pins what the two doors ACCEPT. This one pins what
        // they REJECT, which had drifted apart in its own way: the local door
        // refuses a non-native shape by naming it and naming the four forms
        // that would have worked (§3.2, asserted locally by V22c), while the
        // peer said "unsupported .npy shape" and stopped. The same bad file was
        // therefore a refusal with a destination through File > Open and a dead
        // end through a peer - the half of #71 recorded as D4.
        //
        // The assert is deliberately not "the peer's message contains some
        // words". It is that the LOCAL message STARTS WITH the peer's, which is
        // the strongest form the two can be in while remaining honest: the same
        // sentence about the same file, and the only thing the local door adds
        // is the line about the reader picker - the way out that exists on this
        // side and not yet on the other (D3). If someone edits one spelling
        // without the other, this fails.
        //
        // D5 is the listing half. LIST carries FOUR dims, so a rank above four
        // has no representation in them; the client used to clamp the rank to 4
        // and Browse then showed a five-axis file as a four-axis shape that the
        // loader would refuse by name. It now claims no shape at all, which is
        // what fmtEntryShape renders as "-". The peer cannot currently send
        // such an entry (it only sets LE_META after a parse that accepts rank
        // 2, 3 and 4 only), so what this pins is the OTHER half: a file the
        // loader refuses must not arrive in the listing wearing a shape.
        std::error_code rec;
        std::filesystem::path rdir =
            std::filesystem::temp_directory_path(rec) / "viewer_refusal";
        std::filesystem::remove_all(rdir, rec);
        std::filesystem::create_directories(rdir, rec);
        std::vector<uint16_t> rp16(2 * 3 * 4 * 5 * 6);
        for (size_t i = 0; i < rp16.size(); i++) rp16[i] = (uint16_t)(i * 3 + 1);
        struct Ref { const char* file; std::vector<int64_t> shape; bool ceiling;
                     const char* why; };
        // No DIGITS in these names, for the reason V22's ACCEPT table gives at
        // length: "r_1d" and "r_5d" are numbered siblings, and the listing
        // folds those into one synthetic stack entry - so neither would appear
        // under its own name in the LIST check below, and the check would pass
        // by finding nothing. (It did, until the names lost their digits.)
        const Ref REFUSE[] = {
            { "r_fiveaxes.npy",  { 2, 3, 4, 5, 6 }, false, "five axes: no native reading" },
            { "r_oneaxis.npy",   { 12 },            false, "1-D is an axis, not a picture" },
            { "r_chceiling.npy", { 2, 8, 8, 5 },    true,  "(F,H,W,C): C past the ceiling" },
        };
        size_t badR = 0;
        for (const Ref& t : REFUSE) {
            size_t n = 1;
            for (int64_t d : t.shape) n *= (size_t)d;
            std::string p = npyWriteFile((rdir / t.file).u8string(), "<u2", t.shape,
                                         rp16.data(), n * 2);
            // the peer's answer...
            remote::Meta rm;
            std::string perr;
            bool popened = s.meta(p, rm, perr);
            // ...and this door's, on the same bytes
            closeAll();
            std::string lerr = loadNpy(p);
            bool lopened = lerr.empty() && cur() != nullptr;
            // Neither door may open it, and the peer's sentence must be the
            // local one's opening - identical for the ceiling refusal, which
            // has no "choose a reader" line on either side.
            bool bothRefuse = !popened && !lopened;
            bool sameSentence = !perr.empty() && lerr.rfind(perr, 0) == 0;
            bool namesShape = perr.find(npyShapeText(t.shape)) != std::string::npos;
            bool namesForms = t.ceiling ||
                              (perr.find("(H,W)") != std::string::npos &&
                               perr.find("(H,W,C<=4)") != std::string::npos &&
                               perr.find("(F,H,W)") != std::string::npos &&
                               perr.find("(F,H,W,C<=4)") != std::string::npos);
            bool ok = bothRefuse && sameSentence && namesShape && namesForms;
            fprintf(stderr, "selftest: refusal %-15s peer=\"%s\"  (%s)\n",
                    npyShapeText(t.shape).c_str(), perr.c_str(), t.why);
            if (!ok)
                fprintf(stderr, "selftest:   local=\"%s\"  bothRefuse=%d same=%d"
                                " namesShape=%d namesForms=%d\n",
                        lerr.c_str(), (int)bothRefuse, (int)sameSentence,
                        (int)namesShape, (int)namesForms);
            badR += ok ? 0 : 1;
        }
        // D5: none of those may appear in the listing wearing a shape.
        {
            std::vector<remote::Entry> ents;
            std::string lerr2;
            if (!s.list(rdir.u8string(), ents, lerr2)) {
                fprintf(stderr, "selftest refusal LIST: %s\n", lerr2.c_str());
                badR++;
            } else {
                for (const Ref& t : REFUSE) {
                    const remote::Entry* me = nullptr;
                    for (const auto& e : ents) if (e.name == t.file) me = &e;
                    // The ceiling case IS a native rank, so the peer may well
                    // describe it; what must never happen is a shape whose rank
                    // is not the rank the file declares.
                    bool listedHonestly =
                        me && (!me->hasMeta ||
                               me->ndim == (int)t.shape.size());
                    fprintf(stderr, "selftest: listing %-15s shape column \"%s\"\n",
                            t.file, me ? fmtEntryShape(*me).c_str() : "<missing>");
                    badR += listedHonestly ? 0 : 1;
                }
            }
        }
        closeAll();
        std::filesystem::remove_all(rdir, rec);
        fprintf(stderr, "selftest: refusals name the shape, both doors: %s\n",
                badR ? "FAIL" : "ok");
        bad += badR ? 1 : 0;
    }
    {   // ---- §3.3's escape hatch, through a peer (issue #124) ---------------
        //
        // The two blocks above pin what the doors accept and what they refuse.
        // This one pins the third case, which had no remote half at all: the
        // file OPENED, and it opened the wrong way round. docs/input-adapters.md
        // §3.3 built the way out of that - the Inspector says how the array was
        // read and offers the other readings the shape permits - and §4.13.1
        // decided remote is in from v1, but FrameSource::npyShape was only ever
        // set by the local decoder, so through a peer there was no line and no
        // menu.
        //
        // (H,W,1) is the shape to aim it at, and it is the same fixture #71
        // used, for the same reason: it is what img[:, :, None] writes, it is
        // exactly one mono capture, and the rule now reads it as one 40x48
        // frame - correctly, but a stack of 48 one-pixel-wide lines IS what
        // some scripts mean by it, and §3.3 is how that is said per file. So it
        // is a shape with two honest readings and a real reason to choose.
        //
        // THREE asserts, and the order is the argument of the issue:
        //
        //   RA1  the peer says what the FILE declares, not what it made of it.
        //        Without a shape on the wire nothing below is possible, and
        //        this is the whole of the META change.
        //   RA2  the menu computed from the peer's shape is the SAME menu as
        //        the one computed from the local decoder's. Not "a menu" - the
        //        same one, entry for entry, in order. A remote document that
        //        offered a different set of readings than the same file opened
        //        from File > Open would be a second rule, which is #71 again.
        //   RA3  choosing one CHANGES WHAT TILE RETURNS, and returns what the
        //        local decoder returns for that same declaration - geometry
        //        and every pixel. This is the assert that fails against a peer
        //        that reads the trailer and ignores it, which is the state a
        //        version bump exists to make impossible: the fault it catches
        //        is not a refusal but a plausible wrong picture.
        //
        // Fixtures written here rather than read from tools/testdata, for the
        // reasons the channel-rule block states: that tree is generated and
        // gitignored, and a fixture living apart from the rule it pins drifts.
        std::error_code aec;
        std::filesystem::path adir =
            std::filesystem::temp_directory_path(aec) / "viewer_readas";
        std::filesystem::remove_all(adir, aec);
        std::filesystem::create_directories(adir, aec);
        std::vector<uint16_t> aramp(48 * 40);
        for (size_t i = 0; i < aramp.size(); i++) aramp[i] = (uint16_t)(i * 7 + 3);
        const std::vector<int64_t> ashape = { 48, 40, 1 };
        std::string ap = npyWriteFile((adir / "readas_hw1.npy").u8string(), "<u2",
                                      ashape, aramp.data(), aramp.size() * 2);
        size_t badA = 0;
        remote::Meta am;
        std::string aerr;
        if (!s.meta(ap, am, aerr)) {
            fprintf(stderr, "selftest READAS META: %s\n", aerr.c_str());
            badA++;
        } else {
            // RA1: the declared shape crossed the wire.
            bool shapeOk = am.shape == ashape;
            fprintf(stderr, "selftest: read-as RA1 peer META declares %s (wanted %s)"
                            " : %s\n",
                    am.shape.empty() ? "no shape" : npyShapeText(am.shape).c_str(),
                    npyShapeText(ashape).c_str(), shapeOk ? "ok" : "FAIL");
            badA += shapeOk ? 0 : 1;

            // RA2: the same menu on both sides. The local side's shape comes
            // from the decoder that actually read the file, not from the
            // fixture's declaration, so this compares two ANSWERS rather than
            // one answer against the question.
            closeAll();
            std::string alerr = loadNpy(ap);
            std::vector<int64_t> lshape;
            if (alerr.empty() && cur()) lshape = cur()->src->npyShape;
            std::vector<int> popts = npyReadOptions(am.shape), lopts = npyReadOptions(lshape);
            bool menuOk = !popts.empty() && popts == lopts;
            std::string pl, ll;
            for (int o : popts) pl += (pl.empty() ? "" : " | ") + npyReadLabel(am.shape, o);
            for (int o : lopts) ll += (ll.empty() ? "" : " | ") + npyReadLabel(lshape, o);
            fprintf(stderr, "selftest: read-as RA2 peer offers [%s]\n"
                            "selftest: read-as RA2 local offers [%s]  : %s\n",
                    pl.c_str(), ll.c_str(), menuOk ? "ok" : "FAIL");
            badA += menuOk ? 0 : 1;

            // RA3: every reading the menu offers, served by the peer and
            // compared with the local decoder under the same declaration. The
            // NATIVE entry is in the sweep too - it is the control: if the peer
            // ignored the declaration entirely, native would still agree and
            // only the declared entry would fail, which is precisely the shape
            // of the "silent wrong answer" the version bump is about.
            for (int r : lopts) {
                closeAll();
                std::string rerr = loadNpy(ap, r);
                const ImageDoc* lref2 = cur();
                if (!rerr.empty() || !lref2) {
                    fprintf(stderr, "selftest READAS local %s: %s\n",
                            npyReadLabel(lshape, r).c_str(),
                            rerr.empty() ? "nothing opened" : rerr.c_str());
                    badA++;
                    continue;
                }
                const int lframes2 = (int)app.images.size();
                remote::Meta rm2;
                std::string merr;
                if (!s.meta(ap, rm2, merr, r)) {
                    fprintf(stderr, "selftest READAS peer META %s: %s\n",
                            npyReadLabel(lshape, r).c_str(), merr.c_str());
                    badA++;
                    continue;
                }
                std::vector<float> got2;
                int gw2 = 0, gh2 = 0, gc2 = 0;
                std::string gd2;
                if (!s.tile(ap, 0, 0, 0, rm2.w, rm2.h, 1, got2, gw2, gh2, gc2, gd2, merr, r)) {
                    fprintf(stderr, "selftest READAS peer TILE %s: %s\n",
                            npyReadLabel(lshape, r).c_str(), merr.c_str());
                    badA++;
                    continue;
                }
                bool geom2 = lframes2 == rm2.frames && lref2->w == rm2.w &&
                             lref2->h == rm2.h && lref2->ch == rm2.ch;
                size_t mism2 = 0;
                for (int y = 0; y < gh2; y++)
                    for (int x = 0; x < gw2; x++)
                        for (int ci = 0; ci < gc2; ci++)
                            if (got2[((size_t)y * gw2 + x) * gc2 + ci] != lref2->sample(x, y, ci))
                                mism2++;
                bool ok2 = geom2 && mism2 == 0;
                fprintf(stderr, "selftest: read-as RA3 %-22s local %d frame(s) %dx%d %dch"
                                " == peer %d frame(s) %dx%d %dch, %zu pixel mismatch(es)"
                                " : %s\n",
                        npyReadLabel(lshape, r).c_str(), lframes2, lref2->w, lref2->h,
                        lref2->ch, rm2.frames, rm2.w, rm2.h, rm2.ch, mism2,
                        ok2 ? "ok" : "FAIL");
                badA += ok2 ? 0 : 1;
            }
            // ...and the two readings really are two different pictures. An
            // agreement between two identical answers proves nothing, and this
            // is the line that says the sweep above was not vacuous.
            if (lopts.size() > 1) {
                remote::Meta m0, m1;
                std::string e0, e1;
                bool two = s.meta(ap, m0, e0, lopts[0]) && s.meta(ap, m1, e1, lopts[1]) &&
                           (m0.frames != m1.frames || m0.w != m1.w || m0.h != m1.h ||
                            m0.ch != m1.ch);
                fprintf(stderr, "selftest: read-as RA3 the choice CHANGES the answer:"
                                " %d frame(s) %dx%d %dch vs %d frame(s) %dx%d %dch : %s\n",
                        m0.frames, m0.w, m0.h, m0.ch, m1.frames, m1.w, m1.h, m1.ch,
                        two ? "ok" : "FAIL");
                badA += two ? 0 : 1;
            }
        }
        // ---- RA6: the two ways a DECLARATION can be wrong ------------------
        // Neither is reachable from the menu - npyReadOptions only offers what
        // the shape permits - so what reaches them is a SESSION line pointing
        // at a file that was rewritten under it. Both doors have to answer the
        // same, and they answer differently from each other:
        //
        //   does not FIT the rank   ->  read natively, and say so. Not a
        //                               refusal: the local decoder opens it,
        //                               so a peer that refused would make the
        //                               same session restore succeed here and
        //                               fail there.
        //   fits but breaks the ceiling -> refused, in the ceiling's own words
        //                               ((48,40,1) as (C,H,W) is 48 channels).
        {
            // (H,W) has no reading to declare at all, so NR_STACK cannot fit.
            std::vector<int64_t> f2 = { 8, 8 };
            std::vector<uint16_t> b2(64);
            for (size_t i = 0; i < b2.size(); i++) b2[i] = (uint16_t)(i * 5 + 1);
            std::string p2 = npyWriteFile((adir / "readas_hw.npy").u8string(), "<u2",
                                          f2, b2.data(), b2.size() * 2);
            remote::Meta fm;
            std::string fe;
            bool pOk = s.meta(p2, fm, fe, NR_STACK);
            closeAll();
            std::string le2 = loadNpy(p2, NR_STACK);
            const ImageDoc* ld2 = cur();
            const std::string want = "re-read choice does not fit (8, 8); read natively";
            bool fbOk = pOk && fm.readFellBack && fm.frames == 1 && fm.w == 8 && fm.h == 8 &&
                        le2.empty() && ld2 && ld2->w == 8 && ld2->h == 8 &&
                        (int)app.images.size() == 1 && ld2->note.find(want) != std::string::npos;
            fprintf(stderr, "selftest: read-as RA6 (8,8) declared (F,H,W): peer %d frame(s)"
                            " %dx%d fellBack=%d, local note \"%s\" : %s\n",
                    fm.frames, fm.w, fm.h, (int)fm.readFellBack,
                    ld2 ? ld2->note.c_str() : "<no doc>", fbOk ? "ok" : "FAIL");
            badA += fbOk ? 0 : 1;

            // ...and the ceiling, which IS a refusal, in the same words on both
            // sides - the sentence PR #121 put in remote_proto.h, now said
            // about a declared reading and not only about a file's own shape.
            remote::Meta cm2;
            std::string ce2;
            bool pRef = !s.meta(ap, cm2, ce2, NR_CHW);
            closeAll();
            std::string lce = loadNpy(ap, NR_CHW);
            bool lRef = !lce.empty() && !cur();
            bool ceilOk = pRef && lRef && ce2 == lce &&
                          ce2.find("48 channels") != std::string::npos;
            fprintf(stderr, "selftest: read-as RA6 (48,40,1) declared (C,H,W):\n"
                            "  peer  \"%s\"\n  local \"%s\"  : %s\n",
                    ce2.c_str(), lce.c_str(), ceilOk ? "ok" : "FAIL");
            badA += ceilOk ? 0 : 1;
            closeAll();
        }
        // ---- RA5: the same thing as a DOCUMENT -----------------------------
        // RA1-RA3 are about the wire and the peer. This is the sentence the
        // issue actually makes - "a doc opened through a peer has no read as
        // line and no re-read as" - so it opens one through a peer and asks the
        // document, not the session: the shape openRemote recorded, the menu
        // the Inspector would compute from it, and then npyReReadAs, which is
        // the button. It has to go through openRemote/npyReReadAs rather than
        // assert on the pieces, because "the peer can serve it" and "the
        // Inspector can reach it" were two different halves of the gap and
        // either one alone leaves a menu that cannot act.
        {
            const std::string savedExe = app.remoteExe;
            app.remoteExe = exe;                 // the standalone peer, as everywhere
            closeAll();
            const std::string url = "local://" + ap;
            size_t badD = 0;
            if (!openRemote(url)) {
                fprintf(stderr, "selftest READAS openRemote failed\n");
                badD++;
            } else {
                ImageDoc* rd = cur();
                std::vector<int> dopts = rd ? npyReadOptions(rd->src->npyShape)
                                            : std::vector<int>();
                bool haveLine = rd && rd->src->npyShape == ashape &&
                                rd->src->remoteProto == (int)rp::VERSION &&
                                dopts.size() == 2;
                fprintf(stderr, "selftest: read-as RA5 remote doc %dx%d %dch declares %s,"
                                " peer v%d, %d reading(s) : %s\n",
                        rd ? rd->w : 0, rd ? rd->h : 0, rd ? rd->ch : 0,
                        rd && !rd->src->npyShape.empty()
                            ? npyShapeText(rd->src->npyShape).c_str() : "no shape",
                        rd ? rd->src->remoteProto : 0, (int)dopts.size(),
                        haveLine ? "ok" : "FAIL");
                badD += haveLine ? 0 : 1;
                // ...and the button WORKS: same file, same url, read the other
                // way, and the document that comes back is the one the local
                // decoder produces for that declaration.
                std::string re = rd ? npyReReadAs(rd, NR_STACK) : "no doc";
                ImageDoc* rd2 = cur();
                bool reOk = re.empty() && rd2 && rd2->src->npyRead == NR_STACK &&
                            rd2->w == 1 && rd2->h == 40 && rd2->ch == 1 &&
                            rd2->remoteFrames == 48 &&
                            rd2->src->remoteUrl == url;   // still the peer's, not fetched here
                fprintf(stderr, "selftest: read-as RA5 re-read as (F,H,W) -> %dx%d %dch,"
                                " %d frame(s), npyRead=%d%s : %s\n",
                        rd2 ? rd2->w : 0, rd2 ? rd2->h : 0, rd2 ? rd2->ch : 0,
                        rd2 ? rd2->remoteFrames : 0, rd2 ? rd2->src->npyRead : -1,
                        re.empty() ? "" : ("  err=\"" + re + "\"").c_str(),
                        reOk ? "ok" : "FAIL");
                badD += reOk ? 0 : 1;
                // and back, because a declaration must be unmakeable: §3.3's
                // menu is a way to say it again, not a one-way door.
                std::string back = rd2 ? npyReReadAs(rd2, NR_HWC) : "no doc";
                ImageDoc* rd3 = cur();
                bool backOk = back.empty() && rd3 && rd3->w == 40 && rd3->h == 48 &&
                              rd3->src->npyRead == NR_HWC;
                fprintf(stderr, "selftest: read-as RA5 ...and back to (H,W,C) ->"
                                " %dx%d %dch : %s\n",
                        rd3 ? rd3->w : 0, rd3 ? rd3->h : 0, rd3 ? rd3->ch : 0,
                        backOk ? "ok" : "FAIL");
                badD += backOk ? 0 : 1;
            }
            // The (F,H,W) reading made this a 48-frame stack, so it enqueued 47
            // sibling fetches AND fired the server-side temporal measurement
            // openRemote fires for any frame axis. Both own a thread; a
            // joinable std::thread reaching its destructor is std::terminate,
            // so a test that opens a remote stack has to end the workers it
            // started, exactly as --rset-selftest ends the sequence loader.
            stopRemoteFetcher();
            stopMeasureWorker();
            closeAll();
            app.uiSession.stop();
            app.remoteExe = savedExe;
            badA += badD;
        }
        // ---- RA4: a peer that predates it says so -------------------------
        // Not a thought experiment about an old peer: an old peer, on this
        // disk, refusing - and refused BEFORE the request is sent, from the
        // number it announced, because an old peer would NOT refuse it. It
        // would answer with its own reading of the array under the name of the
        // one that was asked for, and there is no error string anywhere in that
        // exchange for the client to notice afterwards.
        {
            selftestSetEnv("VIEWER_SERVE_PROTOCOL", "8");
            remote::Session old;
            std::string eo;
            if (!old.start("", exe, eo)) {
                fprintf(stderr, "selftest READAS old peer: %s\n", eo.c_str());
                badA++;
            } else {
                remote::Meta om;
                std::string e8;
                bool refused = !old.meta(ap, om, e8, NR_STACK);
                // ...and it is still a usable peer for everything it CAN do:
                // the file opens, and the "read as" line has nothing to say
                // because a v8 META carries no shape.
                std::string e8b;
                bool stillWorks = old.meta(ap, om, e8b) && om.w == 40 && om.h == 48 &&
                                  om.shape.empty();
                bool ok8 = old.peerVersion() == 8 && refused && stillWorks &&
                           e8.find("too old") != std::string::npos &&
                           e8.find("protocol 8") != std::string::npos &&
                           e8.find("needs 9") != std::string::npos;
                fprintf(stderr, "selftest: read-as RA4 old peer (v%d) refusal:\n  %s\n"
                                "selftest: read-as RA4 ...and it still opens natively"
                                " (%dx%d, no shape declared): %s\n",
                        old.peerVersion(), e8.c_str(), om.w, om.h, ok8 ? "ok" : "FAIL");
                badA += ok8 ? 0 : 1;
                old.stop();
                // ...and as a DOCUMENT the sentence is the same one. A doc
                // opened through a v8 peer has an empty npyShape, which is also
                // what a .png has - so the branch order matters, and this is
                // what pins it: the answer must be "your peer is too old", not
                // "not a .npy", which would blame the file for the link's
                // limit. The Inspector reads its line off the same call.
                const std::string savedExe8 = app.remoteExe;
                app.remoteExe = exe;
                closeAll();
                app.uiSession.stop();
                selftestSetEnv("VIEWER_SERVE_PROTOCOL", "8");
                bool opened8 = openRemote("local://" + ap);
                ImageDoc* d8 = cur();
                std::string say8 = d8 ? npyReReadAs(d8, NR_STACK) : "no doc";
                bool doc8 = opened8 && d8 && d8->src->remoteProto == 8 &&
                            d8->src->npyShape.empty() && d8->w == 40 && d8->h == 48 &&
                            say8.find("too old") != std::string::npos &&
                            say8.find("protocol 8") != std::string::npos &&
                            say8.find("not a .npy") == std::string::npos &&
                            (int)app.images.size() == 1;   // nothing was closed
                fprintf(stderr, "selftest: read-as RA4 doc through a v8 peer says:\n"
                                "  \"%s\"  : %s\n", say8.c_str(), doc8 ? "ok" : "FAIL");
                badA += doc8 ? 0 : 1;
                closeAll();
                app.uiSession.stop();
                app.remoteExe = savedExe8;
            }
            selftestSetEnv("VIEWER_SERVE_PROTOCOL", nullptr);
        }
        closeAll();
        std::filesystem::remove_all(adir, aec);
        fprintf(stderr, "selftest: read as / re-read as, through a peer: %s\n",
                badA ? "FAIL" : "ok");
        bad += badA ? 1 : 0;
    }
    fprintf(stderr, "selftest: %llu bytes received from the peer\n",
            (unsigned long long)s.bytesReceived());
    return bad ? 1 : 0;
}
