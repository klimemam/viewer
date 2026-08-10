// core/browse/browse_state.h — Browse の状態型。担当: Browse
// P7 (docs/split-plan.md §3): App::BrowseInstance ほか rb 系ネスト型の本体は
// ここに住む。App 側には using BrowseInstance = browse::Instance; などの alias
// が残るので、既存の App::BrowseInstance / App::RbJob / App::RbConnect 参照は
// 全て無傷で通る — 数千行の参照 churn なしで型を外へ出す方式(§1 の「移動のみ」
// を、意味変更が許される P7 でも参照面については守る)。
// 自己完結: 単独で include できる。客は browse の実 TU (nav.cpp / panel.cpp)
// と core/app/state.h の両方。App には依存しない(依存したら循環する)。
#pragma once

#include "imgui.h"                   // ImVec2 (toolbar geometry, cursor rects)
#include "../remote.h"               // remote::Session / Entry / ScanGroup / GlobHit

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Where the Browse toolbar's two width-critical controls ended up, in screen x,
// against the panel's own content edges. Recorded every frame so a selftest can
// assert the thing a human reads off a screenshot: "the filter is on screen".
// (Defined up here because every Browse INSTANCE carries one - see
// browse::Instance below.)
struct RbToolbarGeom {
    // menuR was moreR until the drawer went away: the row's LAST permanent item
    // is the "..." panel menu now, and the contract it stands for is unchanged -
    // whatever ends the toolbar row must still be inside the panel at any width.
    float x0 = 0, x1 = 0, filterL = 0, filterR = 0, menuR = 0;
    float dateCellW = 0, dateTextW = 0;    // the listing's "modified" column
    int   emptyLocalBtn = 0;               // the not-connected state's local entry
    float rowX = 0, rowY = 0;              // centre of the first row submitted
    // ---- the bottom status line, and the star that ends the path line -------
    // The line is built as one string and then elided to fit, so the selftest
    // reads the LITERAL text the user sees rather than re-deriving it.
    std::string statusText;                // after elision: what is on screen
    std::string statusFull;                // before elision: the whole sentence
    int   statusShown = 0, statusTotal = 0, statusSel = 0;
    // what the drawn line MEASURES against the room it had. The line elides
    // middle-out and must never wrap into a second row, so textW <= availW is
    // the whole contract, checked at every width the geometry sweep uses.
    float statusTextW = 0, statusAvailW = 0;
    float listTopY = 0;                    // where the listing starts: a band
                                           // above it would push this down
    ImVec2 starCentre = ImVec2(-1, -1);    // the path line's bookmark star
    int   starLit = 0;                     // 1 = this place is bookmarked
    // ---- the ancestors held at the top of a scrolled tree -------------------
    // What the reader can still see of where they are. Recorded rather than
    // inferred because the whole claim this feature makes is about the SCREEN:
    // "the folder you are inside is laid out, at the top", and a flag saying
    // the feature is enabled would not have caught a band that drew nothing.
    int   pinnedRows = 0;                  // how many levels are held
    std::string pinnedNames;               // which, outermost first, ";"-joined
    // ...and where the outermost one landed, so an injected click can be aimed
    // at it. A pinned row that cannot be clicked would be a picture of a row.
    ImVec2 pinCentre = ImVec2(-1, -1);
};

namespace browse {

// A connected server, browsable in the Files panel. Connect first, then look
// around - which is why nobody has to know the path shape up front.
struct RemoteBrowse {
    bool connected = false;
    bool autoUpdateTried = false; // one shot per connect: no update loops
    std::string host, dir = "~", err;
    int port = 0;
    std::vector<remote::Entry> entries;
    // Published by the worker with every result (RbResult), so no draw-path
    // code ever names the Session. peerVersion gates the metadata columns
    // and the MEASURE button; rxBytes feeds the status-bar tooltip.
    int peerVersion = 0;
    uint64_t rxBytes = 0;
    // "Search under here" stores an ABSOLUTE path on THIS machine. It was a
    // function-local static of drawPanelRemote, and every state-replacing
    // path (the disconnect button, goToPlace) assigns `App::RemoteBrowse{}`
    // - so entries, dir, host and the tree cache were forgotten on a host
    // change and this string was not: Search on machine B stayed rooted at
    // machine A's folder. Living here makes "reset on host change" true by
    // construction, for this field and for the next one.
    std::string searchRoot;
    // Bumped on every listing REPLACEMENT. The row-selection and cursor
    // caches keyed on host|dir|entry-COUNT, so a refresh that added one
    // file and removed another kept row-indexed selection flags pointing
    // at different files - and "Open N selected as stack" then acted on
    // files the user never picked.
    uint64_t rev = 1;
    // The cancel token for SCAN, as rbSearch.gen is for GLOB. A cancelled
    // scan's late reply used to reach openPickerWith, which does not merely
    // raise a modal - it CLEARS app.folderPick and resets the root, filter,
    // merge and sweep fields, destroying a local Open Folder selection the
    // user was in the middle of filtering.
    uint32_t scanGen = 0;
    // ...and what the scan ANSWERED (#148). scanDoneGen is the token of the
    // last reply that was accepted, so `scanDoneGen == scanGen` means "the scan
    // you started has come back"; scanGroups is how many stacks it carried, or
    // -1 when the peer refused. ZERO IS AN ANSWER: nav.cpp toasts it and opens
    // no picker, queues no stack and loads no frame, so anything waiting for a
    // stack to appear would wait forever. The UI never did - it draws the next
    // frame - but --scan-selftest sat in that wait for its whole budget, which
    // made a fixture that lost its .npy files HANG the suite instead of failing
    // it. There is nowhere else this can be read off: the reply is consumed and
    // dropped on the UI thread.
    uint32_t scanDoneGen = 0;
    int scanGroups = -1;
};
// The connect / install / list sequence runs on the INSTANCE's worker, never
// on the UI thread: a git clone on the far side takes seconds, and "Connect
// froze the app" is precisely the bug class this tool exists to avoid.
// RbTreeList is an ordinary LIST whose answer lands in the tree cache
// instead of replacing the listing: expanding a node must not navigate.
// RbDisconnect stops the session on the thread that OWNS it: the status bar
// must never call stop() on the worker's Session from the UI thread.
// RbPoll is an ordinary LIST of the directory the panel is ALREADY standing
// in, issued by nobody's click (watch-design §2's second row). It is a
// separate kind and not an RbList because what the UI does with the answer
// differs in every particular: it is dropped when it is the listing already
// on screen, it never touches the recents, it never raises an error band,
// and it never makes the worker look busy. A nav's answer is something the
// user asked for; this one is something they did not.
enum RbKind { RbConnect = 0, RbList = 1, RbUpdatePeer = 2, RbScan = 3, RbGlob = 4,
              RbTreeList = 5, RbDisconnect = 6, RbPoll = 7 };
struct RbJob {
    int kind = RbConnect;
    std::string host, dir;
    int port = 0;
    std::string pattern;          // RbGlob only
    uint32_t gen = 0;             // RbGlob: Stop bumps it, stale results drop
    std::string exe;              // frozen at enqueue: see MJob
};
struct RbResult {
    int kind = RbConnect;
    bool ok = false;
    std::string err, host, dir, info;
    int port = 0;
    std::vector<remote::Entry> entries;
    // RbScan payload: every stack under dir, plus how the walk ended
    std::vector<remote::ScanGroup> scanGroups;
    bool truncated = false;
    int skippedDirs = 0;
    // RbGlob payload
    std::vector<remote::GlobHit> hits;
    uint32_t gen = 0;
    // Everything the UI would otherwise have to ask the Session for, read
    // by the thread that OWNS it and carried home as plain values.
    int peerVersion = 0;              // what the peer answered in HELLO
    bool alive = false;               // is the session still up AFTER this job?
    uint64_t rx = 0;                  // bytes received so far
};
// Server-side recursive find (GLOB). gen is the cancel token: Stop bumps
// it and the worker's result is dropped on arrival (the walk itself is
// bounded by depth and result caps, so there is nothing to interrupt).
struct RemoteSearch {
    bool active = false;          // results view instead of the listing
    bool running = false;
    uint32_t gen = 0;
    std::string root, pattern;
    std::vector<remote::GlobHit> hits;
    bool truncated = false;
    int skippedDirs = 0;
};
// ==== ONE Browse instance: a view onto ONE place =========================
// Decision record: docs/todo-open.md item 17 ("instance-able views").
// The Browse panel stopped being a singleton: every instance holds a place
// (RemoteBrowse), the listing state that used to be ~21 function-local
// statics of drawPanelRemote and five g_rb* globals, and its OWN worker
// thread with its OWN Session. Local vs remote is where an instance
// STANDS, never a panel type - the place vocabulary of
// docs/browse-as-file-manager.md survives, only the panel count changed.
struct Instance {
    // identity. num 1 is the primordial instance: it wears the original
    // ImGui id "Browse###Remote" (existing layouts and the dock builder
    // keep working) and is never destroyed, only hidden via app.showRemote
    // - the last Browse closing behaves like the panel being hidden, so
    // there is always a Browse to reopen. num >= 2 wear "Browse N###BrowseN"
    // (stable across sessions: the number is saved) and are destroyed when
    // their window closes.
    int num = 1;
    std::string wtitle;           // the ImGui window name, fixed at creation
    bool open = true;             // window shown (num 1 mirrors app.showRemote)
    bool focusReq = false;        // bring the window forward next frame
    // Where "+" wants this panel to appear: the dock node of the panel whose
    // "+" was clicked, so a new Browse is a TAB beside the one you were
    // looking at rather than a floating window somewhere else. Applied once,
    // then cleared - after that the panel is wherever the user put it.
    unsigned dockInto = 0;
    // the place, and everything listed there
    RemoteBrowse b;
    RemoteSearch search;
    std::string pendingOpen;      // a pasted url, opened once the session is up
    // ---- the worker: one thread, one Session, per instance --------------
    // The Session is touched by THIS thread only; the UI reads the plain
    // values the worker publishes (RbResult). Destroying an instance joins
    // the thread first, so the Session's destruction is single-threaded too.
    std::unique_ptr<remote::Session> session;   // the worker's, exclusively
    std::thread thread;
    // guards queue / done / phase. `mutable` so a const view of an instance can
    // still ask whether its queue is empty - rbPollDue decides nothing and
    // changes nothing, and taking a non-const reference to say so would let it.
    mutable std::mutex mtx;
    std::condition_variable cv;
    std::vector<RbJob> queue;
    std::vector<RbResult> done;
    std::string phase;            // what the worker is doing, for the UI
    std::atomic<bool> busy{ false };
    std::atomic<bool> stop{ false };
    // ---- watch-design §2, second row: this instance re-lists its own dir ----
    // WHILE IT IS BEING DRAWN, and that is the whole condition. drawPanelRemote
    // writes `drawnFrame` at its head; a window that is closed, collapsed or on
    // an unselected dock tab never reaches that line (ImGui::Begin answers
    // false and the spine does not call the panel at all), so the mark simply
    // stops advancing and the poll stops with it. Nothing has to be told.
    uint64_t drawnFrame = 0;      // App::uiFrame at the head of the last draw
    // nowSec() of the last round ISSUED, and 0 = the timer is not running. The
    // first round after a navigation or a first draw ARMS it and lists nothing:
    // the listing on screen arrived a moment ago, and re-reading it because a
    // panel opened would be a round trip for a fact already on the glass.
    double polledAt = 0;
    // A poll is out. §2 says a round is SKIPPED when the worker is busy rather
    // than queued behind it, and a poll deliberately does NOT set `busy` (it is
    // not something anybody is waiting for, and rbAnyBusy drives the window's
    // "keep animating" logic), so this is what the skip is asked about.
    std::atomic<bool> pollPending{ false };
    int pollsIssued = 0;          // selftest probe: rounds that went out
    int pollsApplied = 0;         // ...and answers that replaced the listing
    // ---- tree mode: this instance's expanded nodes and their children ---
    std::map<std::string, std::vector<remote::Entry>> treeCache;
    std::vector<std::string> expanded;     // absolute dirs currently open
    std::vector<std::string> treePending;  // ...and those still being listed
    int treeLists = 0;                     // node LISTs issued (selftest)
    // ---- navigation history (mouse back/forward, Alt+Left / Alt+Right) --
    std::vector<std::string> histBack, histFwd;
    std::string histKey;          // host:port the history belongs to
    bool histNav = false;         // set while back/forward itself navigates
    // ---- the shape of the listing. Per instance; app.rbFlat / rbTree /
    // rbNatural remain the persisted DEFAULTS a new instance starts from,
    // and a toggle writes through to them (last toggle wins next start).
    // (`advanced` - was the "more" drawer open - went with the drawer.)
    bool flat = false, tree = false;
    // ...and the order the NAME column puts rows in. Natural by default
    // (rp::naturalLess): see rbNameCmp for why the listing gets a choice
    // here and a stack never does.
    bool nameNatural = true;
    // ---- panel state, formerly function-local statics of drawPanelRemote.
    // keyboard cursor (row index into the built view, -1 = none)
    int cursor = -1;
    bool cursorScroll = false;    // bring it into view this frame
    // A click GESTURE that navigated: how many clicks of it have landed so
    // far (0 = no such gesture in flight). Everything after that click
    // belongs to the navigation and not to the listing that replaced the
    // one it was aimed at - see rbNavGesture in drawPanelRemote.
    int navChain = 0;
    std::string curSig;           // host|dir|rev the cursor was built for
    // The NAME under the cursor, so the cursor can be found again after a
    // re-listing replaces every entry. A path, not an index: indices move when
    // a file appears or goes away, which is exactly when a refresh happens.
    std::string cursorKey;
    bool curFlat = false, curTree = false;
    // sort spec, stashed from the table one frame late (see RB_COL_NAME)
    int sortCol = 0;              // RB_COL_NAME
    bool sortDesc = false;
    // multi-select, by row index; sig says which listing it was built for
    std::vector<char> sel;
    // the NAMES of the ticked rows, for the same reason cursorKey exists: a
    // re-listing replaces every entry, so nothing that is a pointer or an
    // index survives it
    std::vector<std::string> selKeys;
    int selAnchor = -1;
    bool selFlat = false, selTree = false;
    std::string selSig;
    // filter / search / path editing. ONE buffer: the filter box is also the
    // search box, and Enter is the difference between "narrow these rows"
    // and "walk below this folder". searchBuf/searchFocus fed a second,
    // identical-looking box in a popup and went with it.
    char filter[256] = "";
    bool searchOpen = false;      // put the caret in the filter box next frame
    char pathEdit[1024] = "";
    bool pathEditing = false, pathFocus = false;
    // Properties popup: a snapshot, because the row may scroll out of the
    // clipper (or the listing may refresh) while the popup is up
    remote::Entry propsEntry;
    std::string propsPath;
    bool propsOpen = false;
    bool propsNoSize = false;     // an expanded frame: no size/mtime of its own
    // deferred panel actions - see rbDefer. Per instance: the queue fills
    // only while THIS instance's rows are alive and drains when they die.
    std::vector<std::function<void()>> deferred;
    // selftest probes: where the toolbar and the cursor row landed on screen
    RbToolbarGeom toolbar;
    ImVec2 cursorRect[2] = { ImVec2(0, 0), ImVec2(0, 0) };
    std::string cursorName;
    // ...and its FULL path, which is the key the tree's `expanded` set is
    // written in: "is the cursor row expanded" cannot be asked with a bare
    // name, and counting the whole set answers a different question.
    std::string cursorFull;
    // ...and the centre of the cursor row's chevron hit zone (tree dir
    // rows only; x < 0 = the row has no chevron), for "chevclick"
    ImVec2 cursorChev = ImVec2(-1, -1);
    // The listing's row PITCH, measured from the rows the clipper actually
    // laid out. The pinned-ancestor band has to know which row is at the
    // top of the viewport BEFORE it submits anything (ImGui wants the
    // frozen-row count before the first row), so it divides the scroll
    // offset by this instead of asking the clipper - which has not run yet.
    // Measured, not derived: a row is a Selectable inside a table cell, and
    // reconstructing that height from font size and two paddings is a
    // second copy of ImGui's arithmetic that would drift from it.
    float listRowH = 0;
};

} // namespace browse
