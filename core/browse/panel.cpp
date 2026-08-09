// core/browse/panel.cpp — Browse 実 TU その 2 (P7, docs/split-plan.md §3/§5)。担当: Browse
// drawPanelRemote 一式。viewer への呼び出しは g_browseHost (host.h) 経由のみ —
// S3 が置いた BrowseHost 注記 40 箇所はここで潰れた。RbRow と view builder 群
// は NOGL selftest が直接叩くので browse.h に出ている。
#include "imgui.h"
#include "../app/state.h"
#include "../remote_proto.h"         // rp::naturalLess / rp::VERSION
#include "host.h"                    // the browse -> viewer seam (g_browseHost)
#include "browse.h"                  // ...and the declarations this TU defines

#include <algorithm>
#include <cfloat>                    // FLT_MIN (the places combo width)
#include <cstring>                   // memcmp (the stack-shape gate)
#include <ctime>                     // localtime / strftime (the modified column)
#include <string>
#include <vector>

// The connected server, in its own panel. It used to hang off the bottom of
// Files, but the two do different jobs: Files lists what is already OPEN (and
// closes it), this one BROWSES a machine and decides what to open next. Sharing
// a window made both harder to read once a server had more than a few folders.
// ---- remote listing row formatting ----
std::string fmtBytesHuman(uint64_t n) {
    char b[32];
    if (n >= (1ull << 30))      snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else if (n >= 1024)         snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else                        snprintf(b, sizeof b, "%llu B", (unsigned long long)n);
    return b;
}
static std::string fmtUnixTime(int64_t t) {
    if (t <= 0) return "-";                    // v2 peer / unreadable: unknown
    time_t tt = (time_t)t;
    struct tm tmv {};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}
// The same instant in five fewer characters, for a column too narrow to hold
// the full stamp. The rule is `ls -l`'s, and for the same reason: a capture
// dump is browsed on the day it was taken, where the time of day is the whole
// question, and an archive is browsed by year, where it is noise.
static std::string fmtUnixTimeShort(int64_t t) {
    if (t <= 0) return "-";
    time_t tt = (time_t)t;
    struct tm tmv {};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[32];
    const int64_t SIX_MONTHS = 182LL * 24 * 3600;
    strftime(b, sizeof b, (int64_t)time(nullptr) - t < SIX_MONTHS ? "%m-%d %H:%M"
                                                                 : "%Y-%m-%d", &tmv);
    return b;
}
// "(3000,4000) u16" - the file's declared shape, so what the browser promises is
// what META will later confirm.
std::string fmtEntryShape(const remote::Entry& e) {
    if (!e.hasMeta) return "-";
    std::string s = "(";
    for (int i = 0; i < e.ndim; i++)
        s += (i ? "," : "") + std::to_string(e.dims[i]);
    s += ") " + e.dtype;
    if (e.fortran) s += " F";                  // Fortran order: rare enough to flag
    return s;
}

// (RbRow and the RB_COL_* column ids live in browse.h now: the headless (NOGL)
// selftests build and sort views through the same declarations the panel uses.)

// RB_COL_NAME's comparator, and the ONE place both listing shapes ask for it.
// There are two sorts - the flat listing sorts row indices in rbSortShown, the
// tree sorts each LEVEL inside rbAddRows - and a comparator written out twice
// is how a listing comes to disagree with itself. It is written once here.
//
// NATURAL is the default (rp::naturalLess, remote_proto.h - the same function
// the peer sorts a group's members with, so a local listing and a remote one
// of the same folder read identically). Digit runs compare by value, so
// frame_2 comes before frame_10; letters compare case-insensitively.
//
// Plain lexicographic is what std::string::compare gives, what this listing
// did unconditionally until 2026-08-05, and what the panel menu can still ask
// for - per panel. It was the wrong default because the stack you open FROM
// the listing is built with naturalLess (sortFramesNumerically), so the order
// on screen was not the order sigma_t was computed over, and a tooltip in the
// same panel already told the user frames stack in numeric name order.
//
// Only NAMES. RB_COL_SIZE and RB_COL_MTIME are numbers and have exactly one
// order; this cannot reach them. And it cannot reach a stack either - see the
// comment on sortFramesNumerically, which is the half of this that has no
// setting on purpose.
//
// Returns <0 / 0 / >0, not a bool, because the callers negate it for a
// descending sort. Two naturalLess calls rather than one: names that are equal
// BY VALUE but different as text ("img01" vs "img1") must come out 0 here, so
// that the callers' stable_sort leaves them in the order the peer sent them -
// which is deterministic, where picking either one arbitrarily would not be.
static int rbNameCmp(const std::string& a, const std::string& b, bool natural) {
    if (!natural) return a.compare(b);
    if (rp::naturalLess(a, b)) return -1;
    if (rp::naturalLess(b, a)) return 1;
    return 0;
}
// (The keyboard cursor and the stashed sort spec were file-scope singletons
// here - g_rbCursor / g_rbSortCol / g_rbSortDesc. They live per instance now:
// BrowseInstance::cursor / sortCol / sortDesc.)

static void rbAddRows(const App::BrowseInstance& I,
                      const std::string* dir, const std::vector<remote::Entry>& ents,
                      bool flat, bool tree, int depth, std::vector<RbRow>& out);

// Grouped or flat, listing or tree, from the same entries. Free function so the
// headless selftest drives exactly what the panel draws. The instance supplies
// the tree's expanded set / cache and the per-level sort.
std::vector<RbRow> rbBuildView(const App::BrowseInstance& I,
                               const std::string* dir,
                               const std::vector<remote::Entry>& entries,
                               bool flat, bool tree) {
    std::vector<RbRow> v;
    v.reserve(entries.size() + 1);
    // "..", in BOTH shapes. The first version of this excluded the tree on the
    // grounds that a tree already shows the parent - it does not. The tree is
    // rooted at the folder being browsed and only ever DESCENDS: it shows
    // children and grandchildren, never an ancestor. So in the tree, exactly as
    // in the listing, the only ways out are the toolbar's "up" and Backspace,
    // both of which live outside the list the eye is following.
    //
    // It used to exist and was removed because it appeared only outside the
    // home directory and so shifted every row by one on the way in and out.
    // The fix for that is to always be row 0 - at the root it is present and
    // dead rather than absent - not to have no row at all.
    {
        static remote::Entry upEnt;        // read-only, stable: rows hold pointers
        upEnt.name = "..";
        upEnt.dir = true;
        RbRow r;
        r.e = &upEnt;
        r.dir = dir;
        r.up = true;
        v.push_back(r);
    }
    rbAddRows(I, dir, entries, flat, tree, 0, v);
    return v;
}
static void rbAddRows(const App::BrowseInstance& I,
                      const std::string* dir, const std::vector<remote::Entry>& ents,
                      bool flat, bool tree, int depth, std::vector<RbRow>& out) {
    if (depth > 24) return;                    // a symlink loop is not a tree
    std::vector<int> order(ents.size());
    for (int i = 0; i < (int)ents.size(); i++) order[i] = i;
    if (tree) {
        // Per LEVEL: a global sort over a flattened tree would tear children
        // away from their parents. Directories first, as in the flat listing.
        // Same name comparator as the flat listing (rbNameCmp), or the two
        // shapes of the same panel would put the same folder in two orders.
        std::stable_sort(order.begin(), order.end(), [&](int ia, int ib) {
            const remote::Entry& a = ents[ia];
            const remote::Entry& b = ents[ib];
            if (a.dir != b.dir) return a.dir;
            int cmp = 0;
            switch (I.sortCol) {
                case RB_COL_SIZE:  cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                case RB_COL_MTIME: cmp = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
                default:           cmp = rbNameCmp(a.name, b.name, I.nameNatural); break;
            }
            return I.sortDesc ? cmp > 0 : cmp < 0;
        });
    }
    for (int oi : order) {
        const remote::Entry& e = ents[oi];
        if (flat && e.group && !e.members.empty()) {
            for (int m = 0; m < (int)e.members.size(); m++)
                { RbRow r; r.e = &e; r.dir = dir; r.member = m; r.depth = depth;
                  out.push_back(r); }
            continue;
        }
        { RbRow r; r.e = &e; r.dir = dir; r.depth = depth; out.push_back(r); }
        if (!tree || !e.dir) continue;
        std::string sub = *dir == "/" ? "/" + e.name : *dir + "/" + e.name;
        if (!rbHas(I.expanded, sub)) continue;
        auto it = I.treeCache.find(sub);
        if (it != I.treeCache.end())
            rbAddRows(I, &it->first, it->second, flat, tree, depth + 1, out);
        else {
            // the LIST is still in flight: say so where the children will be
            static const remote::Entry busy = [] {
                remote::Entry b; b.name = "(listing...)"; return b;
            }();
            { RbRow r; r.e = &busy; r.dir = dir; r.depth = depth + 1; r.ph = true;
              out.push_back(r); }
        }
    }
}

// The listing's own sort: `shown` holds row indices into `view` and comes back
// in SCREEN order. A free function for the same reason rbBuildView is one -
// the headless selftest sorts exactly what the panel sorts, rather than a
// second copy of the rules that can drift from it.
//
// TREE mode never comes here. Its levels were sorted inside the builder, one
// frame earlier (the sort spec is stashed in I.sortCol because
// TableGetSortSpecs only exists between Begin/EndTable), and sorting the
// flattened tree would tear children away from their parents.
//
// Directories sort before files under every key: this is a browser, not a
// table of numbers.
void rbSortShown(const App::BrowseInstance& I, const std::vector<RbRow>& view,
                        std::vector<int>& shown) {
    std::stable_sort(shown.begin(), shown.end(), [&](int ia, int ib) {
        const RbRow& a = view[ia];
        const RbRow& b = view[ib];
        if (a.up != b.up) return a.up;      // ".." is row 0 under every sort
        if (a.isDir() != b.isDir()) return a.isDir();
        // an expanded frame has no size / mtime of its own: it sorts as
        // unknown (0) rather than borrowing the group's totals
        uint64_t sa = a.ownFile() ? a.e->size : 0, sb = b.ownFile() ? b.e->size : 0;
        int64_t ma = a.ownFile() ? a.e->mtime : 0, mb = b.ownFile() ? b.e->mtime : 0;
        int cmp = 0;
        switch (I.sortCol) {
            case RB_COL_SIZE:  cmp = sa < sb ? -1 : sa > sb ? 1 : 0; break;
            case RB_COL_MTIME: cmp = ma < mb ? -1 : ma > mb ? 1 : 0; break;
            default:           cmp = rbNameCmp(a.name(), b.name(), I.nameNatural); break;
        }
        return I.sortDesc ? cmp > 0 : cmp < 0;
    });
}

// ---- the ancestors of the first visible row ---------------------------------
// Scrolling a tree used to carry its own context off the top of the panel: six
// rows down inside scanroot/40lx there was nothing on screen saying you were in
// scanroot, or in 40lx, and the only way to find out was to scroll back. The
// levels above the reader stay put now - every one of them, no depth limit -
// and this function is which rows those are.
//
// It takes the position ON SCREEN of the first row the clipper will submit and
// gives back the screen positions of that row's ancestors, outermost first.
//
// Why it is written against `shown` and not against the drawn rows: the listing
// is virtualised. Everything above DisplayStart is never submitted, so there is
// no row object to ask "who is your parent" - and there is no parent POINTER on
// an RbRow either, because rbAddRows flattens the tree into a depth-tagged run.
// That flattening is what makes this cheap: in a run where a child always
// follows its parent, the ancestors of row f are exactly the rows you meet
// walking BACKWARDS from f while taking each one whose depth is lower than the
// last one taken. It reads only the rows it takes plus the ones it steps over,
// touches no ImGui state, and needs no row to have been drawn - which is what
// lets the whole rule be tested without a GL context.
//
// ".." is excluded by construction, not by a special case: it sits at depth 0
// as row 0 and the walk stops the moment it takes a depth-0 row, which for any
// row inside a tree is that row's real top-level folder. The `up` test is there
// for the one listing where ".." is the ONLY depth-0 row - a flat listing has
// no depth at all, and then this correctly returns nothing.
//
// The returned values index `shown`, i.e. they are ROW POSITIONS, not view
// indices: a pinned row is drawn by the same code, from the same index, as the
// row it is a copy of, so there is nothing about it that can behave differently.
void rbAncestorRows(const std::vector<RbRow>& view, const std::vector<int>& shown,
                           int firstRow, std::vector<int>& out) {
    out.clear();
    if (firstRow <= 0 || firstRow >= (int)shown.size()) return;
    int want = view[shown[firstRow]].depth;      // an ancestor is strictly shallower
    for (int r = firstRow - 1; r >= 0 && want > 0; r--) {
        const RbRow& c = view[shown[r]];
        if (c.up || c.depth >= want) continue;
        out.push_back(r);
        want = c.depth;
    }
    std::reverse(out.begin(), out.end());        // outermost first, as drawn
}

// ---- #81: the ticked rows, partitioned into the stacks they belong to --------
// See browse.h for the ruling this implements. The whole of the fix is here:
// the caller no longer gets ONE file list to take ONE mean over, it gets one
// entry per stack, and it opens each of them. Nothing about a mean, a frame or
// a pixel is decided in this file - the seam still carries file lists and
// names, only now it carries the right number of them.
//
// The partition key is the ENTRY, not the directory and not the name. That is
// what makes the two halves fall out of one rule: expanded frames of a stack
// all point at their group's entry, so ticking eight of them gives one part;
// three group rows are three entries, so they give three. In tree mode the
// three may sit in three different folders whose members are all called
// frame_000.npy - the entry pointer tells them apart where the name cannot.
std::vector<RbAvgStack> rbSelectionStacks(const std::vector<RbRow>& view,
                                          const std::vector<char>& sel) {
    struct Part {
        const RbRow* row = nullptr;         // any row of this stack: joins paths
        bool whole = false;                 // the GROUP row itself was ticked
        std::vector<std::string> names;     // the frame / file names ticked
    };
    std::vector<Part> parts;
    for (size_t i = 0; i < view.size() && i < sel.size(); i++) {
        if (!sel[i]) continue;
        const RbRow& r = view[i];
        // "(listing...)", "..", folders and non-.npy can never be a stack. The
        // panel's gate has already refused a selection containing one; dropping
        // them here as well keeps this function total, so a selftest may call it
        // on any view without first reproducing the gate.
        if (r.ph || r.up || r.isDir() || !isNpyName(r.name())) continue;
        Part* p = nullptr;
        for (auto& q : parts)
            if (q.row->e == r.e && q.row->dir == r.dir) { p = &q; break; }
        if (!p) { parts.push_back(Part{ &r, false, {} }); p = &parts.back(); }
        if (r.isGroup()) p->whole = true;   // the group row means all its frames
        else p->names.push_back(r.name());
    }
    std::vector<RbAvgStack> out;
    for (const Part& p : parts) {
        const remote::Entry& e = *p.row->e;
        const std::vector<std::string>& names = p.whole ? e.members : p.names;
        if (names.empty()) continue;
        RbAvgStack s;
        for (const auto& n : names) s.files.push_back(p.row->join(n));
        sortFramesNumerically(s.files);     // frame order is a fact, not a sort
        // The name, by the same rule the row's own menu item uses - which is
        // what stops three stacks from coming out under one borrowed name.
        // A subset of a stack's frames is named for what was picked, because
        // that is what the mean is over; taking all of them (either way of
        // saying so) is the stack itself, so it keeps the stack's name.
        if (e.group)
            s.name = p.whole || names.size() == e.members.size()
                   ? stackNameFor(*p.row->dir, e.name)
                   : stackNameFor(*p.row->dir, patternOfNames(names));
        else
            s.name = stackNameFor(*p.row->dir, names.front());   // a lone .npy row
        out.push_back(std::move(s));
    }
    return out;
}

// ---- deferred panel actions -------------------------------------------------
// Every row the listing draws is an RbRow: RAW POINTERS into that instance's
// b.entries and into its tree cache, held in a `view` vector that is rebuilt
// each frame and read from the moment it is built until the table ends. An
// action that REPLACES either container therefore cannot run where it is
// clicked - the rest of the frame would read freed memory.
//
// The tree cache knew this ("a mid-frame clear would dangle every row below the
// refresh button") and hand-rolled a one-flag deferral. The Places combo did
// not: picking a bookmark on another host runs goToPlace, which assigns a fresh
// App::RemoteBrowse over the live one, and the table below then dereferenced
// nine destroyed entries - reproduced as a SIGSEGV inside strlen, one click
// after connecting.
//
// So it is a rule now instead of three careful call sites: navigation, the tree
// cache and the connection state are QUEUED here and run once `view` is dead.
// A new button inherits the rule instead of having to remember it.
//
// PER INSTANCE, and why that is exactly as safe as the old global: the queue
// fills only while ITS instance's rows are alive (rbDefer targets the instance
// being drawn - g_rbDrawInst below) and drains in the same drawPanelRemote
// invocation, after that instance's `view` has been destroyed. Panel draws
// never nest, so when a queue runs NO instance has a live row vector: another
// instance's rows exist only inside its own draw, which has either not begun
// or already finished. An action from panel A that touched panel B's entries
// would still run row-free - the guarantee never rested on which state is
// replaced, only on when.
static App::BrowseInstance* g_rbDrawInst = nullptr;   // the instance being drawn
void rbDefer(std::function<void()> f) {
    App::BrowseInstance& I = g_rbDrawInst ? *g_rbDrawInst : rbMain();
    I.deferred.push_back(std::move(f));
}
size_t rbDeferredPending() {
    return (g_rbDrawInst ? *g_rbDrawInst : rbMain()).deferred.size();
}
// RAII, so the panel's early returns run the queue too. Declared BEFORE `view`
// in drawPanelRemote: reverse destruction order is what guarantees the rows are
// already gone when the queue runs. (The struct is declared in browse.h - the
// selftests hold one across rbBuildView - and the bodies live here because
// they touch g_rbDrawInst, which stays this TU's private state.)
RbDeferredActions::RbDeferredActions(App::BrowseInstance& i) : I(i), prev(g_rbDrawInst) {
    g_rbDrawInst = &i;
}
RbDeferredActions::~RbDeferredActions() {
    std::vector<std::function<void()>> q;
    q.swap(I.deferred);            // an action may queue another...
    for (auto& f : q) f();         // (...and it lands in I.deferred again:
    g_rbDrawInst = prev;           // g_rbDrawInst still points here, so a
}                                  // re-queued action waits for the next draw)
// Every navigation the panel offers. remoteBrowseTo only enqueues a job today,
// so this changes nothing that can be seen - it makes "the panel never
// navigates mid-draw" true by construction rather than by inspection.
static void rbGoTo(App::BrowseInstance& I, const std::string& dir) {
    rbDefer([&I, dir] {
        // Going somewhere leaves no cursor behind. The rows this index named are
        // about to be replaced, and row 1 of the old place is a different row -
        // or no row - in the new one. This used to be inferred one frame later,
        // from the listing signature changing, which is true but late: anything
        // that writes the cursor between the click and that frame wins. Two
        // attempts to guard the individual writers both missed one. The rule
        // belongs to the navigation, which is the thing that makes it true.
        I.cursor = -1;
        remoteBrowseTo(I, dir);
    });
}

// Bookmarks + recents. The ITEMS are their own function because they live in
// two places: the disconnected state's full-width combo, and - once connected
// - a small dropdown docked at the left edge of the path bar, where a file
// manager keeps its address-bar chevron.
static void drawRemotePlacesItems(App::BrowseInstance& I) {
    // display only: a local place reads better as "[local] path" than as the
    // url scheme (the stored prefs string stays the url)
    auto placeLabel = [](const std::string& u) {
        return u.rfind("local://", 0) == 0 ? "[local] " + u.substr(8) : u;
    };
    int rm = -1;
    if (!app.rbBookmarks.empty()) ImGui::TextDisabled("bookmarks");
    for (int i = 0; i < (int)app.rbBookmarks.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) rm = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("remove this bookmark");
        ImGui::SameLine();
        if (ImGui::Selectable(placeLabel(app.rbBookmarks[i]).c_str()))
            rbDefer([&I, u = app.rbBookmarks[i]] { goToPlace(I, u); });   // see rbDefer
        ImGui::PopID();
    }
    if (rm >= 0) {
        app.rbBookmarks.erase(app.rbBookmarks.begin() + rm);
        app.prefsDirty = true;
        g_browseHost.savePrefs();
    }
    if (!app.rbRecents.empty()) ImGui::TextDisabled("recent");
    for (int i = 0; i < (int)app.rbRecents.size(); i++) {
        ImGui::PushID(1000 + i);
        if (ImGui::Selectable(placeLabel(app.rbRecents[i]).c_str()))
            rbDefer([&I, u = app.rbRecents[i]] { goToPlace(I, u); });
        ImGui::PopID();
    }
    if (app.rbBookmarks.empty() && app.rbRecents.empty())
        ImGui::TextDisabled("nothing yet - the * button bookmarks the open folder");
}
static void drawRemotePlacesCombo(App::BrowseInstance& I) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##places", "places  (bookmarks + recent)",
                           ImGuiComboFlags_HeightLarge))
        return;
    drawRemotePlacesItems(I);
    ImGui::EndCombo();
}

// (RbToolbarGeom is defined in browse_state.h now - each instance carries one.)
float g_rbForceW = 0;      // >0: selftest floats instance 1 at this width
// ...and its HEIGHT, which the pinned-ancestor checks need and the width sweep
// does not: a band only exists once the listing is too long for the panel, and
// a test that waits for a fixture big enough to overflow whatever height the
// screen happens to give is a test that passes for the wrong reason on a big
// monitor. 0 = the old behaviour (80% of the work area).
float g_rbForceH = 0;
// "marklist" / "starmark": the baselines the drawer-removal checks compare
// against. The list's top y proves an error does not open a band above the rows
// (it changes the status line and nothing else), and the star's baseline makes
// the bookmark check a FLIP rather than an absolute - a scripted run inherits
// the user's real bookmark list and must not care what is already in it.
float g_rbListTopY0 = 0;
int   g_rbStar0 = -1;
int   g_rbPeerV0 = -1;     // "setpv" saves the real one; "pvback" restores
// --browse-keys-selftest's injected cursor. It has to be re-asserted INSIDE the
// frame, after the GLFW backend has had its say: the backend overwrites the
// mouse position from the OS cursor whenever the window counts as focused, and
// a HIDDEN window still counts (Win32 GetActiveWindow answers per thread). An
// event queued between frames therefore lost the race about one run in six.
ImVec2 g_injMouse(-1, -1);   // <0: not injecting
int    g_injMouseBtn = -1;   // button held, -1 = none
// (The cursor-row rectangle and name the keys selftest aims at moved into the
// instance: BrowseInstance::cursorRect / cursorName. The selftest reads them
// from its TARGET instance - g_rbKeysTarget, actions "target:N".)
int g_rbKeysTarget = 1;      // instance NUM the keys selftest drives
App::BrowseInstance& rbKeysT() {
    if (App::BrowseInstance* p = rbFindNum(g_rbKeysTarget)) return *p;
    return rbMain();
}

// The "+" affordance: one more Browse, as a TAB beside this one. It used to
// make a panel docked nowhere in particular, which is a different promise from
// the one a "+" makes anywhere else on a computer - next to tabs it means "one
// more of these, here", and a window appearing somewhere else reads as a bug.
// Docking is on (ImGuiConfigFlags_DockingEnable), and ImGui already draws
// co-docked windows as a tab bar, so the tab strip is not something to build:
// it is what happens once the new panel lands in the same node.
//
// Deferred: creating an instance grows app.browsePanels, and the window loop is
// iterating it. The dock id has to be read HERE, inside the panel's own window.
static void rbPlusButton() {
    if (ImGui::SmallButton("+##rbnew")) {
        ImGuiID here = ImGui::GetWindowDockID();
        rbDefer([here] { rbNewInstance().dockInto = (unsigned)here; });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("one more Browse, as a tab beside this one\n"
                          "(View > New Browse Panel)");
}

// The protocol-mismatch sentence, in BOTH directions, for the bottom status
// line. It used to be a conditional orange row above the listing; the fact is
// worth keeping and the row was not (a warning that shoves every file down one
// line the moment an old peer answers). Empty = the versions agree, and
// agreement is silent.
std::string rbProtocolNote(int pv) {
    char t[192];
    if (pv > 0 && pv < 3)
        snprintf(t, sizeof t, "peer speaks protocol %d, this viewer speaks %d - "
                              "shape and date need an update (File > Update remote peer)",
                 pv, (int)rp::VERSION);
    else if (pv > 0 && pv < (int)rp::VERSION)
        snprintf(t, sizeof t, "peer speaks protocol %d, this viewer speaks %d - "
                              "File > Update remote peer", pv, (int)rp::VERSION);
    else if (pv > (int)rp::VERSION)
        snprintf(t, sizeof t, "peer speaks protocol %d, this viewer speaks %d - "
                              "listings may group differently from a local open; "
                              "update the viewer", pv, (int)rp::VERSION);
    else return std::string();
    return t;
}

// Middle-out elision to a PIXEL width, for the status line. Middle-out and not
// front or back because both ends of that line carry a fact: the machine is at
// the head and the counts are at the tail, and dropping either answers a
// question the reader did not ask. Binary search on how many bytes survive -
// width is monotone in that - so a long failure message costs a dozen
// CalcTextSize calls, not one per character.
static std::string rbElideMiddle(const std::string& s, float maxW) {
    if (s.empty()) return s;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    // Not even the marker fits: print NOTHING. Returning "..." here would draw
    // wider than the room it was given, which is the one thing this function
    // exists to prevent - and three dots that overflow say less than nothing.
    if (ImGui::CalcTextSize("...").x > maxW) return std::string();
    auto onBoundary = [&s](size_t i) {         // never cut a UTF-8 sequence
        return i == 0 || i >= s.size() || (s[i] & 0xC0) != 0x80;
    };
    auto build = [&](size_t keep) {
        size_t head = (keep + 1) / 2;
        while (head > 0 && !onBoundary(head)) head--;
        size_t tstart = s.size() - (keep - (keep + 1) / 2);
        while (tstart < s.size() && !onBoundary(tstart)) tstart++;
        if (tstart < head) tstart = head;
        return s.substr(0, head) + "..." + s.substr(tstart);
    };
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (ImGui::CalcTextSize(build(mid).c_str()).x <= maxW) lo = mid;
        else hi = mid - 1;
    }
    return build(lo);
}

void drawPanelRemote(App::BrowseInstance& I) {
    App::RemoteBrowse& B = I.b;
    // FIRST, so it is destroyed LAST - after `view` and after every row that
    // points into the browse state. Anything that replaces that state is queued
    // through rbDefer and runs here. (This replaces a one-flag "forget the tree
    // next frame" deferral that covered the tree cache and nothing else.)
    RbDeferredActions rbActions(I);
    // ---- A GESTURE THAT NAVIGATES OWNS THE WHOLE GESTURE --------------------
    // A folder row is entered on ONE click in a list now, and the pointer does
    // not move afterwards. Two decades of "a folder takes two clicks" say the
    // second click is coming anyway; it arrives one or two frames later, by
    // which time the listing it was aimed at has been replaced under it, and
    // without this it lands on whatever row now occupies that pixel - opening
    // it, previewing it, or dragging the keyboard cursor onto it.
    //
    // That is not a hypothesis. browse-keys' instance segment double-clicks
    // scanroot/10lx in panel 2, and on the ubuntu runner (where a local listing
    // of eight files lands inside the ~8 ms frame budget the selftest runs at,
    // which it does not on Windows) the log reads
    //     185 dbl        dir=.../rb/scanroot rows=3 imgs=44
    //     186 waitdir:10lx dir=.../rb/scanroot/10lx rows=1 imgs=52
    // - eight images and a fifth stack that nobody asked for, because click two
    // landed on 10lx's own frame_000..007 group, plus cursor=1 written by that
    // click's release. Three earlier fixes all guarded the row that navigated;
    // the write to stop belongs to a legitimate click on a DIFFERENT listing,
    // so none of them could see it.
    //
    // The chain is ImGui's own: MouseClickedLastCount counts the clicks of one
    // gesture and resets to 1 when the next press is too late or too far to
    // chain. So "still the same gesture" is exactly "the count went up", and
    // the first press that does not is the listing becoming live again.
    // (".." is deliberately NOT latched: it is row 0 of every listing, so a
    // repeat click there is a repeat of the same control - the toolbar's "up"
    // button case - not a click that landed on something else.)
    {
        const ImGuiIO& nio = ImGui::GetIO();
        if (I.navChain > 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int n = nio.MouseClickedLastCount[ImGuiMouseButton_Left];
            I.navChain = n > I.navChain ? n : 0;
        }
    }
    const bool rbNavGesture = I.navChain > 0;
    // the focused panel is the one the File menu's remote commands aim at
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        g_rbActiveNum = I.num;
    I.toolbar = RbToolbarGeom{};
    I.toolbar.x0 = ImGui::GetCursorScreenPos().x;
    I.toolbar.x1 = I.toolbar.x0 + ImGui::GetContentRegionAvail().x;
    ImGui::PushID("remotetree");
    if (!B.connected) {
        // The empty state used to offer ssh and nothing else, and then say
        // "Connect to a machine, then pick the file here" - so View > Panels >
        // Browse opened a panel with no way out unless you already knew the
        // File menu had a second door into it. The panel browses THIS disk too;
        // that has to be one of the two buttons, and it is the cheaper one, so
        // it goes first.
        const float need = ImGui::CalcTextSize("Start Remote (ssh)...").x +
                           ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::Button("Browse Local Folder...")) {
            g_rbActiveNum = I.num;     // the dialog's result lands HERE
            g_browseHost.browseFolderDialog();
        }
        I.toolbar.emptyLocalBtn = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("pick a folder on this machine and list it here -\n"
                              "same rows, same grouping, same server-side stats.\n"
                              "(File > Browse Folder (Local)... is the same command)");
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
        if (ImGui::Button("Start Remote (ssh)...")) {
            g_rbActiveNum = I.num;     // the dialog connects THIS panel
            app.remoteDlgOpen = true;
        }
        ImGui::SameLine();
        rbPlusButton();
        if (I.busy) { ImGui::SameLine(); ImGui::TextDisabled("connecting..."); }
        drawRemotePlacesCombo(I);
        if (!B.err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.4f, 1));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(B.err.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("details / copy")) app.showRemoteError = true;
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Pick a folder on this machine, or connect to another one, "
                            "and the files land here.");
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        return;
    }
    // The panel used to stack FIVE things above the listing: a host row with
    // three buttons, the breadcrumb bar, the filter, the server-search row, and
    // (when a preview was alive) the scrub bar - plus an error band and a
    // protocol warning that appeared and vanished, moving every file row under
    // the cursor. What is above the listing now is TWO rows that never change
    // count: where am I (the path, with the bookmark star at its end) and
    // narrow it down (the filter, the count, the "..." menu). Everything that
    // is state rather than navigation reports on the bottom status line, and
    // the "more" drawer is gone - see docs/browse-topbar-design.md 10.2/10.3.
    // Server-side search: a different thing from the filter (which only narrows
    // what is already listed). Referenced up here because the path bar's context
    // menu can aim it at a folder.
    // set by "Search under here"; empty = this folder. On App::RemoteBrowse, so
    // it dies with the connection - see the field.
    std::string& rbSearchRoot = B.searchRoot;
    // where we are, and how to leave
    const std::string rbRoot = pathRootOf(B.dir);
    bool atRoot = B.dir == rbRoot || B.dir == "~" || B.dir == "/";
    auto rbGoParent = [&]() {
        if (atRoot) return;
        std::string d = B.dir;
        size_t s = d.find_last_of('/');
        std::string up = s == std::string::npos || s == 0 ? rbRoot : d.substr(0, s);
        // never step above the volume: the parent of "C:/data" is "C:/", not
        // "C:" (a different place on Windows), and a UNC share has no parent
        if (up.size() < rbRoot.size()) up = rbRoot;
        rbGoTo(I, up);
    };
    // Leaving the place. One verb, two entrances - the bottom status line (next
    // to the host it acts on) and the root crumb's menu (the crumb that NAMES
    // the host). It was in the "more" drawer, where nobody could see the thing
    // it disconnects from either. Deferred: it replaces the state every row on
    // screen is pointing into, so it runs after the panel is done drawing.
    // There is nothing to disconnect FROM when the peer runs on this machine,
    // so the local wording is "close browse" - the panel never claims a network
    // connection it does not have.
    const bool rbLocalPeer = B.host.empty();
    auto rbDisconnect = [&I] {
        rbDefer([&I] {                   // another machine, other children
            app.uiSession.stop();        // ours to stop; the worker's is a job
            App::RbJob j;
            j.kind = App::RbDisconnect;
            rbEnqueue(I, std::move(j));
            I.b = App::RemoteBrowse{};
            rbTreeForget(I);
        });
    };
    // ---- row 1: path bar - breadcrumbs, or one text field while editing ----
    char* rbPathEdit = I.pathEdit;
    bool& rbPathEditing = I.pathEditing;
    bool& rbPathFocus = I.pathFocus;
    if (!rbPathEditing) {
        std::vector<PathSeg> segs = pathSegments(B.dir);
        bool editReq = false;
        // The places dropdown sits at the LEFT EDGE of the path bar - a file
        // manager's address-bar chevron - instead of being a band of its own.
        if (ImGui::SmallButton("v##placesbtn")) ImGui::OpenPopup("placespop");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("places (bookmarks + recent)");
        if (ImGui::BeginPopup("placespop")) {
            drawRemotePlacesItems(I);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 6);
        // The path reads as a PATH: flat text segments with separators, click
        // to go there. It was a chain of SmallButtons - boxes in a row - which
        // the user named as exactly the kind of implementation-shaped UI this
        // panel had accumulated (「パスを四角の連続で表示するとか」). The link
        // wears the ordinary text color; hover underlines it.
        ImGui::PushStyleColor(ImGuiCol_TextLink, ImGui::GetStyle().Colors[ImGuiCol_Text]);
        for (size_t k = 0; k < segs.size(); k++) {
            ImGui::PushID((int)k);
            if (k) {
                // the root "/" IS the separator; printing another would say "//"
                if (k > 1 || segs[0].label != "/") {
                    ImGui::SameLine(0, 0);
                    ImGui::TextDisabled("/");
                }
                ImGui::SameLine(0, 0);
                // capture paths run long: wrap instead of clipping the tail off
                if (ImGui::GetContentRegionAvail().x <
                    ImGui::CalcTextSize(segs[k].label.c_str()).x + ImGui::GetFontSize())
                    ImGui::NewLine();
            }
            if (ImGui::TextLink(segs[k].label.c_str()))
                rbGoTo(I, segs[k].path);
            // Right-click used to jump straight into path editing. It is a menu
            // now: the actions that take a FOLDER as their subject all belong on
            // the bar that names the folder.
            if (ImGui::BeginPopupContextItem("crumbctx")) {
                const std::string& target = segs[k].path;
                // the same item a folder ROW carries, aimed at a folder that is
                // not in any listing - the one you are inside, or an ancestor
                if (ImGui::MenuItem("Open folder (all stacks below)"))
                    remoteScanFolder(I, target);
                if (ImGui::MenuItem("Search under here")) {
                    rbSearchRoot = target;
                    I.searchOpen = true;        // aim the root, then focus the box
                }
                if (ImGui::MenuItem("Bookmark")) {
                    std::string u = placeUrl(B.host, B.port, target);
                    if (std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(), u) ==
                        app.rbBookmarks.end()) {
                        app.rbBookmarks.push_back(u);
                        app.prefsDirty = true;
                        g_browseHost.savePrefs();
                    }
                    g_browseHost.toast("bookmarked " + u, false);
                }
                // The ROOT crumb is the one that stands for the machine, so it
                // is the one that can leave it. Deeper crumbs are folders and
                // have no opinion about the connection.
                if (k == 0) {
                    ImGui::Separator();
                    if (ImGui::MenuItem(rbLocalPeer ? "Close this browse"
                                                    : "Disconnect")) rbDisconnect();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(rbLocalPeer
                            ? "stop listing this machine and empty the panel"
                            : "drop the ssh session to %s and empty the panel",
                            peerLabel(B.host).c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy path")) {
                    ImGui::SetClipboardText(target.c_str());
                    g_browseHost.toast("copied " + target, false);
                }
                if (ImGui::MenuItem("Edit path...")) editReq = true;
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(right-click for what can be done to this folder)",
                                  segs[k].path.c_str());
            ImGui::PopID();
        }
        ImGui::PopStyleColor();
        // the empty run after the last segment enters edit mode - the bar
        // itself is the affordance, not another button in the row
        if (!segs.empty()) ImGui::SameLine(0, 4);
        {
            // the star and the "+" (one more Browse) hold the bar's right edge -
            // the click-to-edit run pays for both
            const float oneBtnW = ImGui::CalcTextSize("+").x +
                                  ImGui::GetStyle().FramePadding.x * 2 + 6;
            float editW = std::max(ImGui::GetContentRegionAvail().x - oneBtnW * 2 -
                                   (I.busy ? ImGui::CalcTextSize("(listing...)").x + 8 : 0),
                                   ImGui::GetFontSize() * 1.5f);
            if (ImGui::InvisibleButton("##pathedit", ImVec2(editW, ImGui::GetTextLineHeight())))
                editReq = true;
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                ImGui::SetTooltip("click to type or paste a path\n"
                                  "(right-clicking a folder name works too)");
            }
        }
        if (I.busy) { ImGui::SameLine(); ImGui::TextDisabled("(listing...)"); }
        // The star ends the PATH line because it is about the path: lit means
        // this place is bookmarked, so it reads as a state before it is used as
        // a verb. In the drawer it could be neither - a bookmark indicator that
        // is only visible after you open a fold indicates nothing.
        ImGui::SameLine(0, 4);
        {
            std::string curUrl = placeUrl(B.host, B.port, B.dir);
            bool starred = std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(),
                                     curUrl) != app.rbBookmarks.end();
            I.toolbar.starLit = starred ? 1 : 0;
            // lit = gold, unlit = the dimmed text colour. The star is always
            // THERE (an affordance that appears only once used cannot be found
            // the first time); what changes is whether it is on.
            ImGui::PushStyleColor(ImGuiCol_Text, starred
                ? ImVec4(0.98f, 0.83f, 0.35f, 1)
                : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::SmallButton("*##rbstar")) {
                if (starred)
                    app.rbBookmarks.erase(std::remove(app.rbBookmarks.begin(),
                                                      app.rbBookmarks.end(), curUrl),
                                          app.rbBookmarks.end());
                else
                    app.rbBookmarks.push_back(curUrl);
                app.prefsDirty = true;
                g_browseHost.savePrefs();
            }
            ImGui::PopStyleColor();
            I.toolbar.starCentre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                          (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(starred ? "bookmarked - click to forget:\n%s"
                                          : "bookmark this place:\n%s", curUrl.c_str());
        }
        ImGui::SameLine(0, 4);
        rbPlusButton();
        if (editReq) {
            snprintf(rbPathEdit, sizeof I.pathEdit, "%s", B.dir.c_str());
            rbPathEditing = true;
            rbPathFocus = true;
        }
    } else {
        if (rbPathFocus) { ImGui::SetKeyboardFocusHere(); rbPathFocus = false; }
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
        bool entered = ImGui::InputText("##rbpath", rbPathEdit, sizeof I.pathEdit,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered) {
            std::string p = rbPathEdit;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(0, 1);
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            rbPathEditing = false;
            rbGoTo(I, p.empty() ? "~" : p);
        } else if (ImGui::IsItemDeactivated()) {
            rbPathEditing = false;    // Esc (ImGui reverts the text) or focus loss
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel##path")) rbPathEditing = false;
    }
    // (The full-width orange error band used to be here. A wrapped band above
    // the listing took one to three ROWS and shoved every file down by them -
    // the appearing-and-vanishing rows this redesign is against - and it said
    // the failure in the one place the failure did not happen. The text lives
    // on the bottom status line now, at the end of this function.)
    // The listing as ROWS: grouped (one row per sequence) or flat (one row per
    // frame), listing or tree. Rebuilt every frame - see rbBuildView.
    std::vector<RbRow> view = rbBuildView(I, &B.dir, B.entries, I.flat, I.tree);
    // Multi-select (Ctrl / Shift + click on file rows), indexed by ROW. A
    // navigation or a refresh invalidates the indices, so it resets rather
    // than pointing at different rows. A grouped/flat TOGGLE does not: the
    // selection carries across by owning entry, so expanding a selected
    // sequence selects its frames and collapsing them selects the sequence.
    std::vector<char>& rbSel = I.sel;
    int& rbSelAnchor = I.selAnchor;            // row index of the last click
    bool& rbSelFlat = I.selFlat;               // which view rbSel was built for
    bool& rbSelTree = I.selTree;
    {
        std::string& selSig = I.selSig;
        std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.rev);
        if (sig != selSig) {
            selSig = sig;
            rbSel.assign(view.size(), 0);
            rbSelAnchor = -1;
        } else if (rbSelFlat != I.flat || rbSelTree != I.tree) {
            std::vector<RbRow> old = rbBuildView(I, &B.dir, B.entries, rbSelFlat, rbSelTree);
            std::vector<const remote::Entry*> sel;
            for (size_t i = 0; i < old.size() && i < rbSel.size(); i++)
                if (rbSel[i]) sel.push_back(old[i].e);
            rbSel.assign(view.size(), 0);
            for (size_t i = 0; i < view.size(); i++)
                if (std::find(sel.begin(), sel.end(), view[i].e) != sel.end()) rbSel[i] = 1;
            rbSelAnchor = -1;
        }
        rbSelFlat = I.flat;
        rbSelTree = I.tree;
        // an expand or a collapse moved every row below it: start clean
        // Going somewhere ends the selection too, and this used to be inferred
        // from the row COUNT changing - so walking into a different folder with
        // the same number of rows carried the ticks across, pointing at files
        // that were no longer listed. The listing's identity is the signature,
        // not its length.
        {
            // I.selSig, not a function-local static: this file already learned
            // that lesson once - one search box was shared by every panel - and
            // a shared signature would clear panel 2's selection whenever panel
            // 1 navigated.
            std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.rev);
            if (sig != I.selSig) { I.selSig = sig; rbSel.assign(view.size(), 0); }
        }
        if (rbSel.size() != view.size()) rbSel.assign(view.size(), 0);
    }
    // What a plain click does: show a throwaway PREVIEW of a file / of a
    // sequence's poster frame - nothing is registered - or, on a folder,
    // SELECT it (cursor + anchor, set by the caller). Entering a folder is the
    // double-click (or Enter): click and double-click were indistinguishable
    // on folder rows, which is what made the pair unusable ("もったいない").
    // Factored out because the keyboard (arrow keys) does the same thing.
    auto rbActivateRow = [&](const RbRow& r) {
        if (r.ph) return;
        if (r.up) { rbGoParent(); return; }     // dead at the root, by rbGoParent
        if (r.isDir()) {
            // Nothing, and the MOUSE no longer arrives here for a folder: the
            // click handler does the folder's verb itself (enter in the list,
            // expand in the tree) because it needs to know whether the chevron
            // was hit, which this cannot see. What still reaches this branch is
            // the keyboard walking the listing, and moving a cursor over a
            // folder must not open it.
            return;
        }
        if (!isNpyName(r.name())) return;
        // Everything this needs is read out of the row BEFORE the open, as
        // VALUES: an RbRow is a pair of raw pointers into B.entries / the tree
        // cache, and `r` is a reference into a vector rebuilt every frame.
        // Nothing on the open path mutates either container today, but the row
        // list is one refresh away from being replaced underneath us, and the
        // tree work already had to defer a mid-frame clear for exactly this.
        // stepping context for the scrub bar / , and . : the whole sequence
        // when the row belongs to one, so a flat row still steps its siblings
        std::vector<std::string> seq;
        if ((r.isGroup() || r.member >= 0) && !r.e->members.empty())
            for (const auto& m : r.e->members) seq.push_back(r.join(m));
        int seqAt = r.member >= 0 ? r.member : 0;
        std::string seqLabel = r.e->name;
        std::string target = r.isGroup()
            ? r.join(r.e->members.empty() ? r.e->name : r.e->members[0])
            : r.full();
        // idempotent: clicking the same file again re-shows the preview
        // instead of re-fetching it
        ImageDoc* pv = nullptr;
        for (const auto& di : app.images)
            if (di->uid == app.previewUid && di->preview) pv = di.get();
        std::string u = makeRemoteUrl(B.host, target, B.port);
        bool live = true;
        if (pv && pv->src->remoteUrl == u) g_browseHost.selectImage(app.current);
        else live = g_browseHost.openRemote(u, true, 0);
        // only when there IS a preview: a scrub bar over nothing re-runs the
        // failing open on every press of its buttons and of , / .
        if (!live) return;
        app.previewFiles = std::move(seq);
        app.previewHost = B.host;      // the global slot must know its machine:
        app.previewPort = B.port;      // N instances, N possible hosts
        if (!app.previewFiles.empty()) {
            app.previewIndex = seqAt;
            app.previewLabel = std::move(seqLabel);
        }
    };
    // What a double-click (and Enter) does: a REGISTERED open. A sequence row
    // opens the whole stack; a folder is entered; a frame promotes the preview
    // it just made. A folder's first click was selection only (above), so
    // entering has nothing to take back - no expand happened, none is undone.
    auto rbOpenRow = [&](const RbRow& r) {
        if (r.ph) return;
        if (r.up) { rbGoParent(); return; }
        if (r.isDir()) {
            rbGoTo(I, r.full());
            return;
        }
        if (!isNpyName(r.name())) return;
        if (r.isGroup()) {
            g_browseHost.dropPreview();      // the poster frame did its job
            std::vector<std::string> files;
            for (const auto& m : r.e->members) files.push_back(r.join(m));
            // the canon's `folder/pattern`, built from the SAME text the peer
            // put in the group row (rp::patternWithExtent, shared verbatim)
            g_browseHost.openRemoteStack(B.host, files,
                                         stackNameFor(*r.dir, r.e->name), B.port, 0);
            return;
        }
        // A single file: promote ITS preview when that is what the slot holds.
        // The old form promoted whatever preview happened to be live - a stale
        // slot made a double-click register a file nobody pointed at - and
        // when none was live (a failed open, a slot emptied by a promote-on-
        // measure) it did NOTHING at all, which is the other way a double-
        // click "opened a frame or nothing" instead of what it was aimed at.
        std::string u = makeRemoteUrl(B.host, r.full(), B.port);
        ImageDoc* pv = nullptr;
        for (const auto& di : app.images)
            if (di->uid == app.previewUid && di->preview) pv = di.get();
        if (pv && pv->src->remoteUrl == u) { g_browseHost.promotePreview(pv); return; }
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->src->remoteUrl == u && !app.images[i]->preview) {
                g_browseHost.selectImage(i); // already registered: show it
                return;
            }
        g_browseHost.dropPreview();          // a stale preview is not this row's
        g_browseHost.openRemote(u, false, 0);
    };
    // ---- row 2: the toolbar. Narrow the listing down, and say so when the
    // listing's shape is not the default. Everything else is in the "..." menu
    // at the end of the row - there is no drawer any more.
    //
    // The row FLOWS. It used to be one fixed SameLine chain, and at the panel's
    // own default docked width (0.17 of the window - 271 px on a 1600 px screen)
    // that chain ran off the right edge somewhere after "list": the filter box
    // and the "more" button were submitted outside the clip rect, so the panel's
    // most-used control was not on screen at all and read as missing. Now every
    // item asks for room before it joins the line and starts a new one when
    // there is none, exactly as the breadcrumb bar above already does - the row
    // grows downwards instead of disappearing sideways, and nothing in it can
    // become unreachable at any width.
    const ImGuiStyle& rbStyle = ImGui::GetStyle();
    auto rbBtnW = [&](const char* lb) {           // what a SmallButton will take
        return ImGui::CalcTextSize(lb, nullptr, true).x + rbStyle.FramePadding.x * 2;
    };
    auto rbFlow = [&](float need) {               // join the line, or break first
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
    };
    // Measured, not guessed: what has to survive to the right of the filter.
    // That used to be the fold button, measured at the WIDER of its two labels
    // so a click could not reflow its own row; the fold is gone and the fixed
    // label "..." cannot change width at all.
    const float rbMenuW = rbBtnW("...##rbmenu");
    auto rbFilterTailW = [&](bool counted) {
        float w = rbMenuW + rbStyle.ItemSpacing.x;
        if (counted) w += ImGui::CalcTextSize("9999/9999").x + rbStyle.ItemSpacing.x;
        // Stop and the search root sit between the box and the count, and both
        // appear WHILE the box is in use. Not reserving them is how a control
        // that only exists during a search ends up off the right edge exactly
        // when it is the one control that matters.
        if (I.search.running) w += rbBtnW("Stop##rbsearch") + rbStyle.ItemSpacing.x;
        if (!B.searchRoot.empty()) w += rbBtnW("under: x") + rbStyle.ItemSpacing.x;
        return w;
    };
    char* rbFilter = I.filter;
    // Refresh lost its button to F5 and the "..." menu; both call this, so it
    // is declared before either of them.
    auto rbRefresh = [&I, &B] {
        rbDefer([&I] { rbTreeForget(I); });        // in order: forget, then list
        rbGoTo(I, B.dir);
    };
    {
        // One toolbar row: the chips that name a non-default listing shape,
        // then the filter, then the count, then the "..." menu.
        // Navigation has no buttons: back / forward are mouse 4 / 5 and
        // Alt+Left / Alt+Right, the parent is the ".." row and Backspace, home
        // is a place in the places popup. Five buttons that answered no
        // question were charging every glance (docs/browse-topbar-design.md
        // 10.2). Refresh and the two view modes moved into the menu below, and
        // a mode that is NOT the default says so with a chip instead - a
        // setting that changes what you see must name itself when it is on.
        auto rbModeChip = [&](const char* text, const char* why) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            ImGui::SmallButton(text);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", why);
        };
        if (I.flat) {
            rbModeChip("flat##rbchip", "every frame is its own row - the default is "
                                       "grouped (one row per numbered stack).\n"
                                       "change it in the panel menu");
            rbFlow(rbBtnW("tree##rbchip"));
        }
        if (I.tree) {
            rbModeChip("tree##rbchip", "folders open in place - the default is a "
                                       "list, one folder at a time.\n"
                                       "change it in the panel menu");
            rbFlow(rbBtnW("a-z##rbchip"));
        }
        if (!I.nameNatural) {
            // The order the rows are IN is the least visible setting on this
            // panel - nothing on screen says which of two plausible orders you
            // are reading - so the chip names it rather than merely marking it
            // non-default. It also says what did NOT change, because that is
            // the question this toggle raises.
            rbModeChip("a-z##rbchip", "names sort as text: frame_10 before frame_2.\n"
                                      "the default is natural order (frame_2 first, "
                                      "digits by value).\n"
                                      "frames still STACK in natural order - that one "
                                      "is not a setting.\n"
                                      "change it in the panel menu");
            rbFlow(rbBtnW("filter"));
        }
        // F5 is the refresh that lost its button; the panel must be the one
        // holding focus, or a second browser would reload on the first one's key
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_F5, false))
            rbRefresh();
        // Filter what is already listed - no round trip. Substring by default,
        // glob when * or ? appears (globListMatch's contract), because a capture
        // dump directory holds hundreds of entries and one condition matters.
        //
        // The filter is the row's payload, so it is the one item with a floor:
        // it takes a whole line of its own rather than shrink below what a glob
        // needs to be readable in.
        const float tailW = rbFilterTailW(rbFilter[0] != 0);
        const float filterMin = ImGui::GetFontSize() * 8;
        rbFlow(filterMin + tailW);
        // Ctrl+F, the "..." menu's "Search below...", and a crumb or folder's
        // "Search under here" all land in the same place: this box, with the
        // caret in it. They differ only in whether they also aim the root.
        if (I.searchOpen ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
             ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))) {
            ImGui::SetKeyboardFocusHere();
            I.searchOpen = false;
        }
        // below the floor the tail wraps instead (rbFlow, further down), so a
        // panel too narrow for both still shows a usable filter
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - tailW,
                                         ImGui::GetFontSize() * 4));
        // ONE box, two depths. Typing narrows the rows that are already here,
        // instantly and without asking anyone. Enter commits the SAME text as a
        // recursive walk below this folder.
        //
        // There used to be a second text box, identical in appearance, in a
        // popup off the "..." menu - so the panel asked the user to choose an
        // engine before they were allowed to state the question, and the two
        // doors were told apart only by which menu you came through. The
        // question is the same one either way; only how far it reaches differs,
        // and "how far" is a key, not a place.
        //
        // Local panels get the same Enter. A local Browse is served by a peer
        // session exactly as a remote one is (an empty host means the local://
        // peer, not a different code path), so glob works there too - and a rule
        // that means something different depending on where you are is a rule
        // nobody remembers.
        bool goSearch = ImGui::InputTextWithHint(
            "##rbfilter", "filter (Ctrl+F); Enter searches below",
            rbFilter, sizeof I.filter, ImGuiInputTextFlags_EnterReturnsTrue);
        I.toolbar.filterL = ImGui::GetItemRectMin().x;
        I.toolbar.filterR = ImGui::GetItemRectMax().x;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("typing narrows the rows below, without asking anyone;\n"
                              "bare text matches anywhere; * and ? make it a glob;\n"
                              "comma separates alternatives\n"
                              "\n"
                              "Enter walks BELOW %s instead (depth 6, first 2000\n"
                              "hits) - * and ? cross '/' there",
                              rbSearchRoot.empty() ? B.dir.c_str() : rbSearchRoot.c_str());
        // Stop, beside the box that started it. A recursive walk is the one verb
        // in this panel that costs a round trip, and Enter is now an easy way to
        // start one: it must stay as easy to call off.
        if (I.search.running) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop##rbsearch")) {
                I.search.gen++;               // the in-flight result becomes stale
                I.search.running = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("stop walking; what has arrived stays listed");
        }
        // The root, and the way out of it. "Search under here" aims the next
        // Enter at a folder that is not the one on screen; without somewhere to
        // say so AND somewhere to undo it, that is a one-way door whose effect
        // is invisible until the answers are wrong.
        if (!rbSearchRoot.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string("under: ") + baseName(rbSearchRoot) +
                                    " x##sroot").c_str()))
                rbSearchRoot.clear();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Enter searches under\n%s\nclick to search from "
                                  "the folder being browsed instead",
                                  rbSearchRoot.c_str());
        }
        if (goSearch && rbFilter[0])
            remoteStartSearch(I, rbSearchRoot.empty() ? B.dir : rbSearchRoot, rbFilter);
    }
    // filtered view, by row index (the clipper needs random access)
    std::vector<int> shown;
    shown.reserve(view.size());
    if (!I.tree || !rbFilter[0]) {
        for (int i = 0; i < (int)view.size(); i++)
            if (view[i].up || !rbFilter[0] || globListMatch(rbFilter, view[i].name()))
                shown.push_back(i);      // a filter narrows the listing, not the exit
    } else {
        // In a tree, dropping a folder because its own NAME does not match
        // would orphan the matching files inside it. Rows are in pre-order, so
        // one backward pass keeps every match and every ancestor of a match.
        std::vector<char> keep(view.size(), 0);
        int need = -1;                 // an ancestor shallower than this is wanted
        for (int i = (int)view.size() - 1; i >= 0; i--) {
            if (view[i].up) { keep[i] = 1; continue; }   // a filter never hides the exit
            bool m = !view[i].ph && globListMatch(rbFilter, view[i].name());
            if (m || (need >= 0 && view[i].depth < need)) {
                keep[i] = 1;
                need = view[i].depth;
            }
        }
        for (int i = 0; i < (int)view.size(); i++) if (keep[i]) shown.push_back(i);
    }
    // ...and in SCREEN order, here, before anything indexes it. The sort used to
    // happen inside the table, after the keyboard block had already turned the
    // cursor's row index into a screen position: with any sort but the default
    // the two disagreed, so the clipper was told to keep a different row alive
    // than the one the cursor was on and SetScrollHereY never fired. The spec
    // itself is stashed from the table one frame earlier (I.sortCol), exactly
    // as the tree builder needs it - see RB_COL_NAME.
    if (!I.tree) rbSortShown(I, view, shown);
    if (rbFilter[0]) {
        rbFlow(ImGui::CalcTextSize("9999/9999").x);
        ImGui::TextDisabled("%d/%d", (int)shown.size(), (int)view.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("rows shown of rows listed");
    }
    // The panel's own menu ENDS the toolbar row: the verbs that are real but
    // rare. They used to be buttons charging every glance - and behind them sat
    // the "more" drawer, whose contents have all gone to the place each one
    // belongs (docs/browse-topbar-design.md 10.2): the host to the title,
    // disconnect to the status line and the root crumb, the star to the path
    // line, open folder to File, and the server search to the popup below.
    rbFlow(rbMenuW);
    if (ImGui::SmallButton("...##rbmenu")) ImGui::OpenPopup("rbpanelmenu");
    I.toolbar.menuR = ImGui::GetItemRectMax().x;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("this panel: refresh, how it lists, search the server");
    if (ImGui::BeginPopup("rbpanelmenu")) {
        if (ImGui::MenuItem("Refresh", "F5")) rbRefresh();
        ImGui::Separator();
        // Radio pairs, not toggles: the state is visible without clicking.
        if (ImGui::MenuItem("Grouped (a numbered stack is one row)", nullptr, !I.flat)
            && I.flat) {
            I.flat = app.rbFlat = false;   // this panel now; the pref = the default
            app.prefsDirty = true;
            g_browseHost.savePrefs();
        }
        if (ImGui::MenuItem("Flat (every frame is its own row)", nullptr, I.flat)
            && !I.flat) {
            I.flat = app.rbFlat = true;
            app.prefsDirty = true;
            g_browseHost.savePrefs();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("List (one folder at a time)", nullptr, !I.tree) && I.tree) {
            // Leaving the tree with something under the cursor: the listing
            // opens on THAT folder. Walking down a tree is how you got to a
            // folder five levels deep; dropping back to the root on the way
            // out throws away the only thing the trip was for. A file under
            // the cursor means the folder holding it.
            if (I.cursor >= 0 && I.cursor < (int)view.size()) {
                const RbRow& r = view[I.cursor];
                std::string want = r.ph || r.up ? std::string()
                                 : r.isDir()    ? r.full() : *r.dir;
                if (!want.empty() && want != B.dir) rbGoTo(I, want);   // deferred
            }
            I.tree = app.rbTree = false;
            app.prefsDirty = true;
            g_browseHost.savePrefs();
        }
        if (ImGui::MenuItem("Tree (folders open in place)", nullptr, I.tree) && !I.tree) {
            I.tree = app.rbTree = true;
            app.prefsDirty = true;
            g_browseHost.savePrefs();
        }
        ImGui::Separator();
        // The third thing that changes the LISTING's shape, so it sits with
        // the other two - one place for everything that changes what the rows
        // look like (there is no Preferences panel yet, issue #50).
        //
        // Each item names the order it IS, with the example that distinguishes
        // them, because a listing whose order you cannot name is how the
        // listing and the stack came to disagree without anyone noticing. And
        // it says NAMES: the size and modified columns are numbers with one
        // order each, and this cannot touch them.
        auto rbOrderItem = [&](const char* label, bool wantNatural) {
            if (ImGui::MenuItem(label, nullptr, I.nameNatural == wantNatural) &&
                I.nameNatural != wantNatural) {
                I.nameNatural = app.rbNatural = wantNatural;   // this panel; the pref = the default
                app.prefsDirty = true;
                g_browseHost.savePrefs();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("the NAME column only - size and modified are numbers.\n"
                                  "frames always STACK in natural order whatever this "
                                  "says: for a stack the order is part of the "
                                  "measurement, not a way of looking at it");
        };
        rbOrderItem("Names in natural order (frame_2 before frame_10)", true);
        rbOrderItem("Names in text order (frame_10 before frame_2)", false);
        ImGui::Separator();
        // The two doors are one door now. This entry does not open anything -
        // it puts the caret in the box that is already on the toolbar, which is
        // the whole point: there is one place to state the question and the only
        // choice left is how far Enter reaches.
        if (ImGui::MenuItem("Search below...", "Ctrl+F")) I.searchOpen = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("type in the filter box, then press Enter:\n"
                              "it walks below %s (depth 6, first 2000 hits)",
                              B.dir.c_str());
        if (ImGui::MenuItem("Open folder (all stacks below)")) remoteScanFolder(I, B.dir);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("scan THIS folder and everything below it, then pick\n"
                              "which stacks to open:\n%s", B.dir.c_str());
        ImGui::EndPopup();
    }
    // (The server-search popup used to be here: a second text box, identical in
    // appearance to the filter, reached from the menu above or from a crumb's
    // "Search under here". It is gone - the filter IS the search box now, and
    // Enter is how far it reaches. Everything it carried survived: Stop and the
    // root chip sit beside the box, and the depth and hit limits moved into the
    // box's own tooltip.)
    // How many rows are selected: counted once, here, because two places need
    // it - the action row below and the bottom status line at the end.
    int rbNSel = 0;
    for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) if (rbSel[i]) rbNSel++;
    // (The selection action row lived here: "Open N selected as stack",
    // "Temporal stats (server)", "Copy paths" and "clear". All four moved into
    // the row right-click menu, which now acts on the SELECTION when the row
    // under the pointer is part of it - the way a file manager does. Four
    // controls charging every glance, to say what a right-click already says.
    //
    // The move is only safe because of that rule: right-click used to target
    // the one row it hit, so deleting the buttons without it would have deleted
    // the only path for "pick several, then act".)
    // Everything the selection verbs need, computed once: the files behind the
    // ticked rows (a group row means the frames it stands for), and whether
    // those files can actually stack - answered from the v3 metadata BEFORE any
    // pixel is transferred.
    auto rbSelFiles = [&]() {
        std::vector<std::string> files;
        for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
            if (!rbSel[i]) continue;
            if (view[i].isGroup())
                for (const auto& m : view[i].e->members) files.push_back(view[i].join(m));
            else files.push_back(view[i].full());
        }
        return files;
    };
    std::string rbSelStackWhyNot;        // empty = the selection can stack
    // ...and the frame average's OWN gate, which is not the same question (#81).
    // "Open N selected as stack" MERGES the ticked rows into one stack, so every
    // one of them has to have the same shape or the stack is ragged. The average
    // no longer merges anything: it is one mean per stack, and two stacks of
    // different shapes each average perfectly well. Sharing one gate meant a
    // selection of a 32x32 stack and a 64x64 stack was refused for a reason that
    // had stopped applying to this verb.
    std::string rbSelAvgWhyNot;
    bool rbSelTemporalOk = B.peerVersion >= 2;
    {
        const remote::Entry* first = nullptr;
        for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
            if (!rbSel[i]) continue;
            const remote::Entry& e = *view[i].e;
            if (!isNpyName(view[i].name())) {
                rbSelStackWhyNot = "only .npy files can form a stack";
                rbSelAvgWhyNot = "only .npy files can be averaged";
                rbSelTemporalOk = false;     // MEASURE is npy-only too
                continue;
            }
            if (!e.hasMeta) {
                rbSelStackWhyNot = rbSelAvgWhyNot =
                    "shape unknown - the peer is protocol 2 "
                    "(File > Update remote peer)";
                continue;
            }
            if (!first) { first = &e; continue; }
            if (e.ndim != first->ndim || e.dtype != first->dtype ||
                memcmp(e.dims, first->dims, sizeof e.dims) != 0)
                rbSelStackWhyNot = "selected files differ: " + fmtEntryShape(*first) +
                                   " vs " + fmtEntryShape(e);
        }
        if (rbNSel < 2)
            rbSelStackWhyNot = rbSelAvgWhyNot =
                "select two or more files (Ctrl / Shift + click)";
    }

    // (The protocol-mismatch warning was a conditional full-width orange row
    // here. It is a FACT about the connection that is rarely true and never
    // urgent - exactly the kind of thing the bottom status line was added to
    // carry - and as a row it moved the whole listing the moment a peer of the
    // wrong version answered. rbProtocolNote() below builds the same sentence,
    // in both directions, for the status line.)
    ImGui::Separator();
    // Where the listing begins. A selftest reads this to prove that a failure
    // does NOT open a band above the rows: the error changes the status line's
    // text and leaves this number alone.
    I.toolbar.listTopY = ImGui::GetCursorScreenPos().y;
    if (I.search.active) {         // search results stand in for the listing
        App::RemoteSearch& S = I.search;
        auto joinS = [&S](const std::string& rel) {
            return S.root == "/" ? "/" + rel : S.root + "/" + rel;
        };
        // The way back comes FIRST. It used to sit after the result line, and
        // that line carries a pattern and a full path - longer than the panel
        // at its docked width, so the only exit was pushed off the right edge.
        if (ImGui::SmallButton("back to listing")) S.active = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("back to %s\n(navigating anywhere also leaves the results)",
                              B.dir.c_str());
        ImGui::SameLine();
        if (S.running) {
            ImGui::TextDisabled("searching %s under %s ...", S.pattern.c_str(), S.root.c_str());
        } else {
            ImGui::Text("%d result(s) for %s under %s%s", (int)S.hits.size(),
                        S.pattern.c_str(), S.root.c_str(),
                        S.truncated ? "  (first 2000 - narrow the pattern)" : "");
            if (S.skippedDirs)
                ImGui::TextDisabled("%d unreadable folder(s) skipped", S.skippedDirs);
        }
        if (ImGui::BeginChild("searchhits", ImVec2(0, 0), ImGuiChildFlags_None)) {
            ImGuiListClipper clip;
            clip.Begin((int)S.hits.size());
            while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const remote::GlobHit& h = S.hits[row];
                ImGui::PushID(row);
                std::string lb = h.dir ? h.rel + "/" : h.rel;   // trailing / marks dirs
                if (ImGui::Selectable(lb.c_str())) {
                    std::string full = joinS(h.rel);
                    if (h.dir) {
                        rbGoTo(I, full);
                    } else if (isNpyName(h.rel)) {
                        g_browseHost.openRemote(makeRemoteUrl(B.host, full, B.port),
                                                false, 0);
                    } else {
                        // not servable: at least go where it lives
                        size_t sl = full.find_last_of('/');
                        rbGoTo(I, sl == std::string::npos || sl == 0 ? "/"
                                                                 : full.substr(0, sl));
                    }
                }
                if (ImGui::BeginPopupContextItem("sctx")) {
                    std::string full = joinS(h.rel);
                    if (!h.dir && ImGui::MenuItem("Go to containing folder")) {
                        size_t sl = full.find_last_of('/');
                        rbGoTo(I, sl == std::string::npos || sl == 0 ? "/"
                                                                 : full.substr(0, sl));
                    }
                    if (ImGui::MenuItem("Copy path")) {
                        ImGui::SetClipboardText(full.c_str());
                        g_browseHost.toast("copied " + full, false);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }
    // The "[..]" row that used to sit here is gone: the toolbar's "up" button
    // and Backspace both do it, and a row that exists only outside the home
    // directory shifted every listing row by one line on the way in and out.
    // ---- keyboard navigation of the listing --------------------------------
    // Up / Down walk the rows and preview as they go (what a plain click does),
    // Enter opens for real (what a double-click does), Backspace leaves for the
    // parent. Gated on IsAnyItemActive so the filter box, the path field and
    // the search field keep every key they type; disjoint from the , / . frame
    // stepping under the listing, which owns different keys entirely.
    int& rbCursor = I.cursor;          // row index, or -1 = no cursor yet
    bool& rbCursorScroll = I.cursorScroll;  // bring it into view this frame
    {
        std::string& curSig = I.curSig;
        bool& curFlat = I.curFlat;
        bool& curTree = I.curTree;
        std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.rev);
        if (sig != curSig) { curSig = sig; rbCursor = -1; }
        else if (curFlat != I.flat || curTree != I.tree) {
            // follow the row across a grouped/flat or list/tree toggle
            std::vector<RbRow> old = rbBuildView(I, &B.dir, B.entries, curFlat, curTree);
            const remote::Entry* was = rbCursor >= 0 && rbCursor < (int)old.size()
                                     ? old[rbCursor].e : nullptr;
            rbCursor = -1;
            if (was)
                for (size_t i = 0; i < view.size(); i++)
                    if (view[i].e == was) { rbCursor = (int)i; break; }
            rbCursorScroll = rbCursor >= 0;
        }
        curFlat = I.flat;
        curTree = I.tree;
        if (rbCursor >= (int)view.size()) rbCursor = -1;
    }
    int rbCursorPos = -1;                // ...where it sits on SCREEN
    for (int k = 0; k < (int)shown.size(); k++) if (shown[k] == rbCursor) rbCursorPos = k;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        int want = rbCursorPos;
        int last = (int)shown.size() - 1;
        if (!shown.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                want = rbCursorPos < 0 ? 0 : std::min(rbCursorPos + 1, last);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                want = rbCursorPos < 0 ? last : std::max(rbCursorPos - 1, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) want = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_End, false))  want = last;
        }
        if (want != rbCursorPos && want >= 0) {
            rbCursorPos = want;
            rbCursor = shown[want];
            rbCursorScroll = true;
            // moving onto a FOLDER must not enter it - walking a list of
            // folders would then dive into the first one and never come back
            if (!view[rbCursor].isDir()) rbActivateRow(view[rbCursor]);
        }
        // Cmd/Ctrl+O rides with Enter (2026-08-03, user): on macOS that chord
        // means "open what is selected", which is this, and never "show me a
        // file dialog". Same code path, so the two cannot come to differ.
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyChordPressed(MODK | ImGuiKey_O)) {
            int nSel = 0;
            for (size_t i = 0; i < view.size() && i < rbSel.size(); i++)
                if (rbSel[i]) nSel++;
            if (nSel >= 2) {
                // Enter on a multi-selection opens EVERY selected row - each
                // group as its own stack, each file as itself. This is the
                // OTHER thing from the action row's "Open N selected as
                // stack", which MERGES the selection into one stack; opening
                // three checked groups used to open only the cursor's one.
                g_browseHost.dropPreview();  // the posters did their job
                int skipped = 0;
                for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
                    if (!rbSel[i]) continue;
                    const RbRow& r = view[i];
                    if (r.ph || r.up || r.isDir() || !isNpyName(r.name())) {
                        skipped++;           // named below, never silent
                        continue;
                    }
                    if (r.isGroup()) {
                        std::vector<std::string> files;
                        for (const auto& m : r.e->members) files.push_back(r.join(m));
                        g_browseHost.openRemoteStack(B.host, files,
                                        stackNameFor(*r.dir, r.e->name), B.port, 0);
                    } else {
                        g_browseHost.openRemote(makeRemoteUrl(B.host, r.full(), B.port),
                                                false, 0);
                    }
                }
                if (skipped)
                    g_browseHost.toast(std::to_string(skipped) +
                                       " selected item(s) skipped (folders / not .npy)", true);
                rbSel.assign(view.size(), 0);          // the selection is consumed
            } else if (rbCursor >= 0 && rbCursor < (int)view.size()) {
                rbOpenRow(view[rbCursor]);
            }
        }
        // Right / Left expand and collapse the folder under the cursor. Only in
        // tree mode: in the flat listing there is nothing to open in place.
        // (Alt+arrow is history navigation, below - not a tree toggle.)
        if (I.tree && !ImGui::GetIO().KeyAlt &&
            rbCursor >= 0 && rbCursor < (int)view.size() &&
            view[rbCursor].isDir()) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                rbTreeExpand(I, view[rbCursor].full());
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                rbTreeCollapse(I, view[rbCursor].full());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) rbGoParent();
        // Alt+Left / Alt+Right: the browser's own back / forward, the keyboard
        // mirror of mouse buttons 4 / 5 (below, which also work when the panel
        // is merely hovered). Deferred like every other navigation.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow))
            rbDefer([&I] { rbHistGo(I, true); });
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_RightArrow))
            rbDefer([&I] { rbHistGo(I, false); });
    }
    // Mouse back / forward, scoped to this panel (hovered or focused) so a
    // future second browser can own its own history. The vendored backend
    // forwards GLFW buttons 4 / 5 as ImGui buttons 3 / 4 - verified by the
    // keys selftest's mback / mfwd, which inject exactly those.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
        if (ImGui::IsMouseClicked(3)) rbDefer([&I] { rbHistGo(I, true); });
        if (ImGui::IsMouseClicked(4)) rbDefer([&I] { rbHistGo(I, false); });
    }
    // The listing scrolls on its own so the header above never leaves the view.
    // Properties target: a snapshot, because the row may scroll out of the
    // clipper (or the listing may refresh) while the popup is up.
    remote::Entry& rbPropsEntry = I.propsEntry;
    std::string& rbPropsPath = I.propsPath;
    bool& rbPropsOpen = I.propsOpen;
    bool& rbPropsNoSize = I.propsNoSize; // an expanded frame: no size/mtime of its own
    // A row used to be reserved here for the preview scrub bar, held even when
    // no preview was alive so that starting one never shifted the rows under
    // the cursor (a bar that appeared above the list moved every row
    // mid-double-click). The scrub bar moved to the Image View footer on
    // 2026-08-04, so the reservation goes with it: an empty row kept against
    // the return of something that is no longer here is just a lost row, and
    // this panel works down to 300 px.
    //
    // What stays is the bottom status line, which is permanent: its separator
    // and its one text row come out of the table's height so the listing stops
    // above it instead of scrolling underneath it.
    float rbFootH = ImGui::GetTextLineHeightWithSpacing() +
                    ImGui::GetStyle().ItemSpacing.y * 2 + 1;
    // Column widths, measured from the widest thing each column actually
    // prints. They were all TableSetupColumn(..., 0.0f) - "auto-fit" - and a
    // SCROLLING table auto-fits over its first frames, which for this table are
    // the frames before the listing has arrived. "modified" was therefore sized
    // against an empty column and kept that width for good: a 16-character
    // timestamp clipped to "202" even with the panel dragged out to 1150 px.
    // ImGuiTableFlags_Resizable then wrote the number into the layout file, so
    // it survived restarts too.
    // A metadata column is WidthFixed | NoResize with an explicit width, which
    // is the one combination ImGui re-applies on EVERY frame (TableUpdateLayout:
    // a non-resizable fixed column takes InitStretchWeightOrWidth as its width).
    // With Resizable the width was latched on the initialising frame instead and
    // then written to the layout file, which is exactly how a stale number
    // outlived every resize. The panel is what the user drags now, not the
    // column edges - and the columns follow it.
    const ImGuiStyle& tSt = ImGui::GetStyle();
    const float tCell  = tSt.CellPadding.x * 2 + 1;              // + inner border
    const float wShape = ImGui::CalcTextSize("(3000,4000) u16").x;
    const float wSize  = ImGui::CalcTextSize("1023.9 MB").x;
    const float wDate  = ImGui::CalcTextSize("2026-07-27 14:03").x;
    // the wider of fmtUnixTimeShort's two forms, so the column does not clip on
    // the day a file crosses the six-month line
    const float wDateS = std::max(ImGui::CalcTextSize("2026-07-27").x,
                                  ImGui::CalcTextSize("07-27 14:03").x);
    // The name IS the row, so it keeps a readable minimum and the metadata is
    // paid for out of what is left, dropping from the right. A column that
    // cannot hold its own value is worth less than the name it is taking the
    // width from - three characters of a timestamp tell nobody anything.
    float tBudget = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 9 - tCell;
    const bool colShape = tBudget >= wShape + tCell;
    if (colShape) tBudget -= wShape + tCell;
    const bool colSize = colShape && tBudget >= wSize + tCell;
    if (colSize) tBudget -= wSize + tCell;
    const bool dateFull  = colSize && tBudget >= wDate + tCell;
    const bool dateShort = colSize && !dateFull && tBudget >= wDateS + tCell;
    const bool colDate = dateFull || dateShort;
    auto tHide = [](bool show) {
        return show ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_Disabled;
    };
    const float rbTableH = ImGui::GetContentRegionAvail().y - rbFootH;
    if (ImGui::BeginTable("rblist", 4, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SortTristate,
                                       ImVec2(0, -rbFootH))) {
        // ---- the levels above the reader, held at the top ---------------------
        // A tree that scrolls loses the folder you are in: it is above
        // DisplayStart, so it is never submitted, and the panel goes on showing
        // eight files with nothing on screen saying which of six folders they
        // came from. Every ancestor stays instead - all of them, at any depth.
        //
        // FROZEN ROWS, not an overlay. ImGui's own ScrollFreeze puts the rows it
        // is given ABOVE the scrolling region and shortens that region by their
        // height, which is the difference between a band that occupies space and
        // one that covers the first real row. It is normally used for a fixed
        // count (this table already froze the header with it); what is new is
        // that the count is recomputed every frame from where the scroll is.
        //
        // The columns, the row background, the selection fill, the cursor
        // outline and the horizontal position of every cell therefore come out
        // right for free: a pinned row is the same table's row, drawn by the
        // same code from the same index. It is not a picture of a row.
        //
        // Vendored ImGui is NOT patched for this. It arrives through
        // FetchContent and a local patch is a cost at every version bump, and
        // nothing here needed one: TableSetupScrollFreeze takes its row count as
        // an ordinary per-frame argument, and ImGui already stops freezing
        // entirely at scroll 0 (imgui_tables.cpp: FreezeRowsCount is zeroed when
        // Scroll.y == 0), which is exactly the "no band at the top of the list"
        // behaviour this wants.
        //
        // Why the scroll offset and not clipper.DisplayStart: the freeze count
        // has to be set before the first row, and the clipper does not run until
        // after it. Dividing the scroll by the measured row pitch is the same
        // arithmetic the clipper itself does, one step earlier - so the band and
        // the clipper agree on which row is at the top, in the same frame, with
        // no state carried over from the last one.
        std::vector<int> rbPins;
        {
            const float sy = ImGui::GetScrollY();   // the inner window's, this frame
            const float rh = I.listRowH;
            if (sy > 0 && rh > 0) {
                int first = std::min((int)(sy / rh), (int)shown.size() - 1);
                rbAncestorRows(view, shown, first, rbPins);
                // A guard on the WIDGET, not on the depth. There is no depth
                // limit here on purpose - Browse moves its current directory
                // with a double-click, so a listing is rarely more than a few
                // levels below where it stands - and this only ever bites when
                // the panel is dragged so short that the ancestors would leave
                // NO scrollable row at all, which is a table that cannot be
                // used rather than a policy about hierarchies. One row is
                // enough for it to be a listing again; the deepest levels are
                // what give way, because the outermost is the one that says
                // most about where you are.
                int room = (int)(rbTableH / rh) - 1;
                if (room < 0) room = 0;
                if ((int)rbPins.size() > room) rbPins.resize((size_t)room);
            }
        }
        ImGui::TableSetupScrollFreeze(0, 1 + (int)rbPins.size());
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_DefaultSort, 0.0f, RB_COL_NAME);
        ImGui::TableSetupColumn("shape / dtype", ImGuiTableColumnFlags_WidthFixed |
                                                 ImGuiTableColumnFlags_NoResize |
                                                 ImGuiTableColumnFlags_NoSort |
                                                 tHide(colShape), wShape, RB_COL_SHAPE);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoResize |
                                        ImGuiTableColumnFlags_PreferSortDescending |
                                        tHide(colSize), wSize, RB_COL_SIZE);
        ImGui::TableSetupColumn("modified", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_NoResize |
                                            ImGuiTableColumnFlags_PreferSortDescending |
                                            tHide(colDate), dateFull ? wDate : wDateS,
                                            RB_COL_MTIME);
        ImGui::TableHeadersRow();
        // The spec is STASHED, not applied: `shown` was already sorted with it,
        // above, where the keyboard and the clipper can agree with the screen.
        // A sort change therefore lands on the next frame - invisible, and the
        // same one-frame contract the tree builder has always had.
        if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsCount > 0) {
                I.sortCol = (int)sp->Specs[0].ColumnUserID;
                I.sortDesc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            } else {
                I.sortCol = RB_COL_NAME;
                I.sortDesc = false;
            }
        }
        // ONE row, drawn one way. `pinned` says only that this copy is being
        // held at the top of the listing by the frozen-row band above; it
        // changes nothing about what the row is or what clicking it does, and
        // the three things it does suppress are all recordings of "where is
        // this on screen" that belong to the row in its own place:
        //   - SetScrollHereY, which would scroll the listing to the band;
        //   - toolbar.rowX/rowY, the first LISTED row a selftest right-clicks;
        //   - cursorRect/cursorChev, which aim injected clicks.
        // A pinned copy shares its index with the row it copies, so it also
        // shares its ImGui ID - and the cursor row is force-submitted by the
        // clipper even when scrolled out, so the two really can meet. The band
        // is pushed into its own ID scope below to keep them apart.
        auto rbDrawRow = [&](int row, bool pinned) {
            const RbRow& r = view[shown[row]];
            const remote::Entry& e = *r.e;
            const std::string& rname = r.name();
            ImGui::PushID(shown[row]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // vscode-quiet rows: a dim chevron marks a folder, a stack gets
            // three hairlines, a file gets nothing - the name is the row.
            // (First cut had drawn folder/page pictograms; they collided with
            // the text and were, verbatim, "くどい".)
            // the tree's indent is drawn as leading spaces, so the Selectable
            // still spans the whole row and the hit target never narrows
            std::string lb(2 + (size_t)r.depth * 3, ' ');
            lb += rname;
            if (r.isGroup()) lb += "   [" + std::to_string(e.frames) + " frames]";
            bool servable = !r.ph && (r.isDir() || isNpyName(rname));   // (ph draws dimmed)
            if (!servable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            int ei = shown[row];
            bool isSel = ei < (int)rbSel.size() && rbSel[ei] != 0;
            bool rowClicked = ImGui::Selectable(lb.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns);
            // The glyph gutter's geometry, shared by the drawing below and by
            // the tree's chevron HIT ZONE - the zone must split exactly where
            // the pixels say it does. All inside the leading spaces the label
            // reserves, so rows with and without a chevron keep their names
            // aligned and nothing about the row indices moves.
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const float rowH   = ImGui::GetTextLineHeight();
            const float rowGut = ImGui::CalcTextSize("  ").x;   // never touch the name
            const float rowInd = ImGui::CalcTextSize("   ").x * (float)r.depth;
            // TREE dir rows own a chevron: expansion is ITS verb, not the
            // name's. ".." is navigation, not a listing node - no chevron.
            // Groups never expand in place (grouped<->flat is a view toggle),
            // so they get none either.
            const bool chevRow = I.tree && r.isDir() && !r.up;
            // the zone: everything left of the name (indent + gutter)
            const bool chevHit = chevRow && ImGui::IsItemHovered() &&
                                 ImGui::GetIO().MousePos.x < rowMin.x + rowInd + rowGut;
            // where a selftest aims a right-click: the first row that HAS a
            // context menu (a placeholder "(listing...)" row has none, the ".."
            // row has none, and a tree still fetching a node can put one at the
            // top)
            if (pinned && I.toolbar.pinCentre.x < 0)      // the outermost, for a click
                I.toolbar.pinCentre =
                    ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            if (!r.ph && !r.up && !pinned && I.toolbar.rowY <= 0) {
                I.toolbar.rowX = (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f;
                I.toolbar.rowY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
            }
            if (shown[row] == rbCursor) {
                // the keyboard cursor: an outline, not a fill - the fill means
                // "selected for a multi-file action" and the two are not the same
                //
                // Drawn on the pinned copy too. Wheeling a tree can leave the
                // cursor on a folder that is now being held at the top, and a
                // cursor that vanishes because its row moved is a cursor the
                // user has to go and find.
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
                                                    ImGui::GetItemRectMax(),
                                                    IM_COL32(150, 180, 215, 190), 0.0f, 0, 1.0f);
                // WHERE it is, though, is recorded off the row in its own place
                // only: this drives SetScrollHereY and the coordinates injected
                // clicks are aimed at, and both mean the row's real position.
                if (!pinned) {
                    if (rbCursorScroll) { ImGui::SetScrollHereY(0.5f); rbCursorScroll = false; }
                    I.cursorRect[0] = ImGui::GetItemRectMin();
                    I.cursorRect[1] = ImGui::GetItemRectMax();
                    I.cursorName = rname;
                    I.cursorFull = r.full();   // the key `expanded` is written in
                    I.cursorChev = chevRow
                        ? ImVec2(rowMin.x + rowInd + rowGut * 0.45f,
                                 (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f)
                        : ImVec2(-1.0f, -1.0f);
                }
            }
            if (r.isDir() || r.isGroup()) {   // inside the two-space gutter the label reserves
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                float y = rowMin.y + (ImGui::GetItemRectSize().y - rowH) * 0.5f;
                float cxm = rowMin.x + rowInd + rowGut * 0.45f, cym = y + rowH * 0.5f;
                if (chevRow) {
                    // the tree's chevron is a CONTROL now, so it says so: a
                    // small filled triangle, > closed / v open, brightening
                    // under the mouse. Same anchor as the hint glyph below -
                    // the name column does not know the difference.
                    ImU32 c = chevHit ? IM_COL32(212, 218, 224, 255)
                                      : IM_COL32(150, 158, 166, 190);
                    float a = std::min(rowH * 0.22f, rowGut * 0.50f);
                    if (rbHas(I.expanded, r.full()))
                        rdl->AddTriangleFilled(ImVec2(cxm - a, cym - a * 0.5f),
                                               ImVec2(cxm + a, cym - a * 0.5f),
                                               ImVec2(cxm, cym + a * 0.7f), c);
                    else
                        rdl->AddTriangleFilled(ImVec2(cxm - a * 0.5f, cym - a),
                                               ImVec2(cxm - a * 0.5f, cym + a),
                                               ImVec2(cxm + a * 0.7f, cym), c);
                } else if (r.isDir()) {   // › chevron, the way a row hints "enter me"
                    // (list-view dirs and the ".." row: a hint, not a control)
                    ImU32 c = IM_COL32(150, 158, 166, 170);
                    float a = std::min(rowH * 0.16f, rowGut * 0.30f);
                    rdl->AddLine(ImVec2(cxm - a * 0.5f, cym - a), ImVec2(cxm + a * 0.5f, cym), c, 1.4f);
                    rdl->AddLine(ImVec2(cxm + a * 0.5f, cym), ImVec2(cxm - a * 0.5f, cym + a), c, 1.4f);
                } else {              // stack: three hairlines, barely there
                    ImU32 c = IM_COL32(130, 165, 200, 150);
                    float w = std::min(rowH * 0.36f, rowGut * 0.8f);
                    for (int k = -1; k <= 1; k++)
                        rdl->AddLine(ImVec2(cxm - w * 0.5f, cym + k * rowH * 0.18f),
                                     ImVec2(cxm + w * 0.5f, cym + k * rowH * 0.18f), c, 1.0f);
                }
            }
            // The chevron's verb, on the PRESS: one click, one toggle, right
            // now. There is no double-click meaning on this zone (a fast pair
            // is two toggles), so nothing here is ever optimistic and nothing
            // ever needs cancelling - the anti-flash guarantee is structural.
            if (chevHit && !r.ph && !rbNavGesture &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                std::string full = r.full();
                if (rbHas(I.expanded, full)) rbTreeCollapse(I, full);
                else rbTreeExpand(I, full);
                rbCursor = ei;          // the mouse also places the keyboard
            }
            if (rowClicked && servable && !rbNavGesture) {
                ImGuiIO& sio = ImGui::GetIO();
                bool rbLeaving = false;      // this click navigates away
                bool canSel = !r.up && ei < (int)rbSel.size();
                if (sio.KeyCtrl && canSel) {
                    rbSel[ei] ^= 1;                    // toggle, plain click still opens
                    rbSelAnchor = ei;
                } else if (sio.KeyShift && canSel) {
                    // range in the order on SCREEN (sorted + filtered), anchor kept
                    int a = -1;
                    for (int k = 0; k < (int)shown.size(); k++)
                        if (shown[k] == rbSelAnchor) a = k;
                    if (a < 0) a = row;
                    for (int k = std::min(a, row); k <= std::max(a, row); k++)
                        if (!view[shown[k]].up && (size_t)shown[k] < rbSel.size())
                            rbSel[shown[k]] = 1;
                } else if (!sio.KeyCtrl && !sio.KeyShift &&
                           sio.MouseClickedLastCount[ImGuiMouseButton_Left] < 2) {
                    // A plain click ENDS the selection. Everywhere else a list
                    // works this way - Ctrl and Shift build a selection, a bare
                    // click replaces it - and here it did not, so ticks made
                    // three folders ago were still armed and still feeding
                    // "Open N selected", with nothing on screen tying them to
                    // what the user was now looking at. Clearing rather than
                    // selecting this row: in this panel a bare click PREVIEWS,
                    // and a preview is not a selection.
                    if (rbNSel > 0) rbSel.assign(view.size(), 0);
                    // (Ctrl / Shift on a row that cannot join the selection -
                    // ".." - must not fall through and act as a plain click:
                    // Ctrl+clicking the exit walked up a directory.)
                    // < 2: the RELEASE half of a double-click is not a new
                    // click. Selectable returns true on release, so the second
                    // click of a double-click landed here too - one frame
                    // after rbOpenRow had already consumed the gesture - and
                    // re-previewed the poster frame NEXT TO the freshly opened
                    // stack ("一枚目とStackが2つFilesに登録される"), or a
                    // second copy of the file just promoted. Only a first
                    // click previews.
                    // A FOLDER is navigation, not a document. One click goes in,
                    // in whichever way "in" is drawn in this mode: the tree
                    // expands it in place, the list moves to it. Files keep the
                    // old contract - click selects, double-click commits -
                    // because a file is a thing you compare and open on purpose,
                    // and opening one by brushing past it is expensive.
                    //
                    // This is safe now only because folders lose their
                    // double-click verb below. Its predecessor toggled here and
                    // CANCELLED on the second click of a double-click: correct
                    // at rest, but the expand was rendered between the clicks,
                    // and a flash the eye sees is a flash however it ends. With
                    // one gesture owning the verb there is nothing to cancel.
                    // !chevHit: the chevron already toggled on the PRESS, and
                    // this runs on the release - both firing would toggle twice.
                    if (r.isDir() && !r.up && !chevHit) {
                        std::string full = r.full();
                        if (!I.tree) { rbGoTo(I, full); rbLeaving = true; }
                        else if (rbHas(I.expanded, full)) rbTreeCollapse(I, full);
                        else rbTreeExpand(I, full);
                    } else {
                        rbActivateRow(r);
                    }
                    rbSelAnchor = ei;
                }
                // A click that LEAVES this listing leaves no cursor behind: the
                // rows it indexed are about to be replaced, and index 1 of the
                // old place is a different row - or no row - in the new one.
                // Said here rather than left to the sig-change reset at the top
                // of the next frame, so that nothing can write it back in
                // between. (The thing that used to write it back was the second
                // half of the same double-click; that is handled one level up
                // now - see rbNavGesture - because it was never only the cursor
                // it wrote.)
                rbCursor = rbLeaving ? -1 : ei;
                // ...and the rest of THIS gesture belongs to the navigation.
                if (rbLeaving) I.navChain = sio.MouseClickedLastCount[ImGuiMouseButton_Left];
            }
            // Double-click = a registered open (the VSCode pinning gesture):
            // a stack row opens the whole stack, a frame promotes the preview
            // its first click just made.
            //
            // For a FOLDER the two gestures split by mode, because the modes
            // have different numbers of verbs to give away:
            //
            //   tree - a folder has TWO things you can do to it. Open it where
            //          it is (single click, and the chevron) or go to it and
            //          make it the listing (double click). Both are useful and
            //          they are not the same, so both gestures are spoken for.
            //   list - a folder has ONE. There is no "in place" in a list, so
            //          the single click is the whole vocabulary and a double
            //          click is just a click that arrived twice.
            //
            // ".." is single-click in both: it is the exit, not a folder row.
            if (servable && !r.up && !chevHit && (!r.isDir() || I.tree) &&
                !rbNavGesture && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                rbOpenRow(r);
                // the tree's double-click navigates on the PRESS, so the same
                // rule applies to it: the release is one frame behind, and one
                // frame is enough for the new listing to be under the pointer
                if (r.isDir())
                    I.navChain = ImGui::GetIO().MouseClickedLastCount[ImGuiMouseButton_Left];
            }
            // §4.13.0's first entrance: double-clicking something the viewer
            // cannot read opens the Reader panel on it, instead of the row being
            // simply inert. A dimmed row that does nothing when you double-click
            // it is the tool saying "no" with no way forward, which is the exact
            // dead end §3.2 set out to remove.
            // Local only: running the adapter on the peer is §4.13.1, and
            // pretending a remote path is a local one would hand the reader a
            // path that does not exist on this machine.
            if (!servable && !r.ph && !r.up && !r.isDir() && !rbNavGesture &&
                ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (B.host.empty())
                    g_browseHost.openReaderPicker(r.full(),
                        "the viewer has no native reading for this file");
                else
                    g_browseHost.toast("readers run on this machine only for now - "
                                       "copy the file over, or open it from a local folder",
                                       true);
            }
            if (!servable) ImGui::PopStyleColor();   // before the popup, or it tints the menu
            if (!r.ph && !r.up && ImGui::BeginPopupContextItem("ctx")) {
                std::string full = r.full();
                // Right-clicking a row that is PART of a multi-row selection acts
                // on the whole selection, the way every file manager does. This
                // is what replaced the four buttons that used to sit above the
                // listing: the verbs did not go away, they stopped charging rent.
                // Right-clicking a row that is NOT selected still means that row
                // alone - the pointer beats the ticks, or a menu would act on
                // something the user is not pointing at.
                if (isSel && rbNSel >= 2) {
                    char lb[96];   // the average item names its stack count too
                    snprintf(lb, sizeof lb, "Open %d selected as stack", rbNSel);
                    if (!rbSelStackWhyNot.empty()) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(lb)) {
                        std::vector<std::string> files = rbSelFiles();
                        sortFramesNumerically(files);
                        std::vector<std::string> bases;
                        for (const auto& f : files) bases.push_back(baseName(f));
                        g_browseHost.openRemoteStack(B.host, files,
                                        stackNameFor(B.dir, patternOfNames(bases)), B.port, 0);
                        rbSel.assign(view.size(), 0);
                    }
                    if (!rbSelStackWhyNot.empty()) ImGui::EndDisabled();
                    // This line has always claimed numeric name order, and
                    // until the listing was natural too it was the only place
                    // saying so while the rows above it said otherwise. Now it
                    // can say which of the two it is - and when the panel is
                    // in text order it names the disagreement instead of
                    // leaving the user to find it in the frame numbers.
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", !rbSelStackWhyNot.empty()
                            ? rbSelStackWhyNot.c_str()
                            : I.nameNatural
                            ? "frames stack in natural name order - the order this "
                              "listing is showing them in"
                            : "frames stack in natural name order (frame_2 before "
                              "frame_10), NOT the text order this listing is showing");

                    // ...and the same selection AVERAGED, which is where it stops
                    // being the same operation (#81). Above merges the ticked
                    // rows into one stack; a mean cannot follow it across, so
                    // this one asks rbSelectionStacks how many stacks are really
                    // ticked and opens one average per stack. The COUNT is in the
                    // label, before the click, because "3 stacks -> 3 averages"
                    // is the whole ruling and it used to be invisible: the item
                    // said "average" and delivered one picture whether you had
                    // picked one stack or five.
                    std::vector<RbAvgStack> avgStacks = rbSelectionStacks(view, rbSel);
                    const int nAvg = (int)avgStacks.size();
                    if (nAvg > 1)
                        snprintf(lb, sizeof lb, "Open %d selected as %d frame averages"
                                                " (one per stack)", rbNSel, nAvg);
                    else
                        snprintf(lb, sizeof lb, "Open %d selected as frame average", rbNSel);
                    if (!rbSelAvgWhyNot.empty()) ImGui::BeginDisabled();
                    if (ImGui::MenuItem(lb)) {
                        for (const auto& s : avgStacks)
                            g_browseHost.openStackForAverage(B.host, s.files, s.name, B.port);
                        // Said again AFTER the fact, because the opens are
                        // asynchronous and land one at a time: without this the
                        // only evidence of how the selection was read is however
                        // many pictures eventually appear.
                        if (nAvg > 1)
                            g_browseHost.toast(std::to_string(nAvg) + " stacks selected -> " +
                                               std::to_string(nAvg) +
                                               " frame averages, one per stack", false);
                        rbSel.assign(view.size(), 0);
                    }
                    if (!rbSelAvgWhyNot.empty()) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (!rbSelAvgWhyNot.empty())
                            ImGui::SetTooltip("%s", rbSelAvgWhyNot.c_str());
                        else if (nAvg > 1)
                            ImGui::SetTooltip(
                                "ONE AVERAGE PER STACK - %d of them here, opened side\n"
                                "by side. Frames from different stacks are not on the\n"
                                "same time axis, so they are never folded into one\n"
                                "mean. To average files that are NOT one stack, use\n"
                                "\"Open %d selected as stack\" above and average that.\n\n%s",
                                nAvg, rbNSel, AVG_TIP);
                        else
                            ImGui::SetTooltip("%s", AVG_TIP);
                    }

                    snprintf(lb, sizeof lb, "Temporal stats (server) for %d", rbNSel);
                    ImGui::BeginDisabled(!rbSelTemporalOk);
                    if (ImGui::MenuItem(lb)) {
                        std::vector<std::string> files = rbSelFiles();
                        // baseName("/") is empty; the folder at the root of a
                        // filesystem is still called "/" on screen
                        std::string leaf = baseName(B.dir);
                        if (leaf.empty()) leaf = B.dir;
                        g_browseHost.requestBrowseTemporal(B.host, files,
                                              leaf + " (" + std::to_string(files.size()) +
                                              " selected)", B.port);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(rbSelTemporalOk
                            ? "sigma_t / sigma_fpn computed on the server over the\n"
                              "selected files, shown in the Temporal panel - nothing\n"
                              "opens and no pixel transfers"
                            : "needs a protocol 2+ peer and .npy files only");

                    snprintf(lb, sizeof lb, "Copy %d path(s)", rbNSel);
                    if (ImGui::MenuItem(lb)) {
                        std::vector<std::string> files = rbSelFiles();
                        std::string all;
                        for (const auto& f : files) { all += f; all += "\n"; }
                        ImGui::SetClipboardText(all.c_str());
                        g_browseHost.toast("copied " + std::to_string(files.size()) + " path(s)", false);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("one absolute path per line; a numbered group\n"
                                          "expands to the frames it stands for");
                    if (ImGui::MenuItem("Clear selection")) rbSel.assign(view.size(), 0);
                    ImGui::Separator();
                }
                if (r.isDir()) {
                    if (ImGui::MenuItem("Open folder (all stacks below)"))
                        remoteScanFolder(I, full);
                    if (ImGui::MenuItem("Search under here")) {
                        rbSearchRoot = full;   // the chip beside the box shows and clears it
                        I.searchOpen = true;   // ...and the caret goes in the box
                    }
                    if (ImGui::MenuItem("Bookmark")) {
                        std::string u = placeUrl(B.host, B.port, full);
                        if (std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(), u) ==
                            app.rbBookmarks.end()) {
                            app.rbBookmarks.push_back(u);
                            app.prefsDirty = true;
                            g_browseHost.savePrefs();
                        }
                        g_browseHost.toast("bookmarked " + u, false);
                    }
                    ImGui::Separator();
                } else if (r.isGroup()) {
                    if (ImGui::MenuItem("Open as stack")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(r.join(m));
                        g_browseHost.openRemoteStack(B.host, files,
                                                     stackNameFor(*r.dir, e.name), B.port, 0);
                    }
                    // ...and the same stack as ONE frame: its per-pixel mean
                    // across the frame axis. Beside "Open as stack" because it
                    // is the same subject and the same click, answering the
                    // other question you open a dark or flat set to ask.
                    if (ImGui::MenuItem("Open as frame average")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(r.join(m));
                        g_browseHost.openStackForAverage(B.host, files,
                                                         stackNameFor(*r.dir, e.name), B.port);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", AVG_TIP);
                    // the server aggregates WITHOUT opening: "is this set even
                    // worth transferring?" costs zero pixels this way. Group
                    // rows only exist from protocol 3 on, so no version gate.
                    if (ImGui::MenuItem("Temporal stats (server)")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(r.join(m));
                        std::string leaf = B.dir;
                        size_t sl2 = leaf.find_last_of('/');
                        if (sl2 != std::string::npos && sl2 + 1 < leaf.size())
                            leaf = leaf.substr(sl2 + 1);
                        g_browseHost.requestBrowseTemporal(B.host, files,
                                                           leaf + "/" + e.name, B.port);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("sigma_t / sigma_fpn computed on the server,\n"
                                          "shown in the Temporal panel - nothing opens,\n"
                                          "no pixel transfers. plane=all (no CFA split).");
                    ImGui::Separator();
                } else if (isNpyName(rname)) {
                    if (ImGui::MenuItem("Open"))
                        g_browseHost.openRemote(makeRemoteUrl(B.host, full, B.port),
                                                false, 0);
                    // one file as a stack: a frame-axis file becomes its frames
                    if (ImGui::MenuItem("Open as stack"))
                        g_browseHost.openRemoteStack(B.host, { full },
                                                     stackNameFor(*r.dir, rname), B.port, 0);
                    if (ImGui::MenuItem("Open as frame average"))
                        g_browseHost.openStackForAverage(B.host, { full },
                                                         stackNameFor(*r.dir, rname), B.port);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", AVG_TIP);
                    // an expanded frame still knows the sequence it came from
                    if (r.member >= 0) {
                        char sl[64];
                        snprintf(sl, sizeof sl, "Open the whole stack (%u frames)", e.frames);
                        if (ImGui::MenuItem(sl)) {
                            std::vector<std::string> files;
                            for (const auto& m : e.members) files.push_back(r.join(m));
                            g_browseHost.openRemoteStack(B.host, files,
                                                         stackNameFor(*r.dir, e.name), B.port, 0);
                        }
                        snprintf(sl, sizeof sl, "Open the whole stack as frame average (%u)",
                                 e.frames);
                        if (ImGui::MenuItem(sl)) {
                            std::vector<std::string> files;
                            for (const auto& m : e.members) files.push_back(r.join(m));
                            g_browseHost.openStackForAverage(B.host, files,
                                                             stackNameFor(*r.dir, e.name), B.port);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", AVG_TIP);
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Copy path")) {
                    ImGui::SetClipboardText(full.c_str());
                    g_browseHost.toast("copied " + full, false);
                }
                if (!r.isDir() && ImGui::MenuItem("Properties...")) {
                    rbPropsEntry = e;
                    if (r.member >= 0) {          // an expanded frame: the group's
                        rbPropsEntry.name = rname;   // meta, but none of its totals
                        rbPropsEntry.group = false;
                        rbPropsEntry.frames = 0;
                        rbPropsEntry.members.clear();
                    }
                    rbPropsNoSize = r.member >= 0;
                    rbPropsPath = full;
                    rbPropsOpen = true;
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (!r.ph && !r.isDir() && isNpyName(rname))
                ImGui::TextDisabled("%s", fmtEntryShape(e).c_str());
            ImGui::TableNextColumn();
            // blank, not zero: the group reply has no per-frame size or mtime
            if (!r.ph && !r.isDir() && r.ownFile())
                ImGui::TextDisabled("%s", fmtBytesHuman(e.size).c_str());
            ImGui::TableNextColumn();
            if (!r.ph && r.ownFile() && e.mtime > 0) {
                float cellW = ImGui::GetContentRegionAvail().x;
                std::string ts = dateShort ? fmtUnixTimeShort(e.mtime)
                                           : fmtUnixTime(e.mtime);
                ImGui::TextDisabled("%s", ts.c_str());
                // widest stamp actually printed against the room it had: the
                // one thing --browse-keys-selftest asserts about this column
                // (nothing is printed at all when the column is dropped)
                if (colDate) {
                    I.toolbar.dateCellW = cellW;
                    I.toolbar.dateTextW = std::max(I.toolbar.dateTextW,
                                                     ImGui::CalcTextSize(ts.c_str()).x);
                }
                // the short form drops a field, so the full stamp stays one
                // hover away rather than only in Properties
                if (dateShort && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", fmtUnixTime(e.mtime).c_str());
            }
            else if (!r.ph && !r.isDir() && r.ownFile()) ImGui::TextDisabled("-");
            ImGui::PopID();
        };
        // The band. Submitted FIRST and nowhere else: ImGui hands the frozen
        // rows to whatever comes immediately after the header, so these rows
        // and TableSetupScrollFreeze's count above have to agree exactly or the
        // listing's first ordinary row is frozen instead.
        I.toolbar.pinnedRows = (int)rbPins.size();
        I.toolbar.pinnedNames.clear();
        I.toolbar.pinCentre = ImVec2(-1, -1);
        if (!rbPins.empty()) {
            ImGui::PushID("pinned");        // see rbDrawRow: IDs must not collide
            for (int p : rbPins) {
                I.toolbar.pinnedNames += view[shown[p]].name();
                I.toolbar.pinnedNames += ";";
                rbDrawRow(p, true);
            }
            ImGui::PopID();
        }
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        // The cursor row must be SUBMITTED even when it is scrolled out, or
        // there is no item for SetScrollHereY to scroll to. AFTER Begin(): the
        // clipper allocates its range list there, and IncludeItemByIndex writes
        // straight through the null TempData pointer otherwise - the two
        // IM_ASSERTs that say so are compiled out of a release build, so a
        // single Down arrow segfaulted the process before any handler ran.
        if (rbCursorScroll && rbCursorPos >= 0 && rbCursorPos < (int)shown.size())
            clip.IncludeItemByIndex(rbCursorPos);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++)
                rbDrawRow(row, false);
            // what the band divides the scroll offset by NEXT frame - see the
            // freeze block above. ImGui measures it off the first row it laid
            // out, which is the same pitch every row here has.
            if (clip.ItemsHeight > 0) I.listRowH = clip.ItemsHeight;
        }
        ImGui::EndTable();
    }
    // The preview's scrub WIDGETS are gone from here (2026-08-04): they now sit
    // in the Image View footer, under the picture they step, which is where the
    // stack scrub bar already was. Two frame-stepping rows on screen at once
    // was the whole of the "スクラブ2重" complaint.
    //
    // The KEYS stay. They cost no space, and reaching them without moving your
    // hand off the list is the reason to have them at all - the duplication
    // that mattered was two widgets, not two ways to press a key. They only
    // fire while this panel has focus, so they never race the Image View's.
    int pvN = app.previewFiles.size() >= 2 ? (int)app.previewFiles.size()
                                           : app.previewFrames;
    if (pvN >= 2 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Comma, true))  g_browseHost.stepPreviewFrame(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) g_browseHost.stepPreviewFrame(+1);
    }
    // ---- the bottom status line (docs/browse-topbar-design.md 10.3) --------
    // One permanent thin row under the listing, carrying the facts that are
    // rarely true and had no home: which machine this panel stands on, how much
    // is in front of you, how much of it you have picked - and, only when they
    // are true, what the connection is doing, what a scan or a search is doing,
    // which protocol the peer speaks, and what failed.
    //
    // PERMANENT, not conditional (10.7). Every one of the rows this replaces
    // appeared and vanished, and each time it did it moved every file row under
    // the reader's eye. A line that is always there costs one line and moves
    // nothing.
    //
    // It is NOT an action bar. The single verb on it is disconnect, and it is
    // allowed because it acts on the host named at the other end of the same
    // line. Everything else the drawer used to hold is a menu item now.
    {
        ImGui::Separator();
        // counted over REAL rows: ".." is the way out, not an item in the
        // folder, and a tree's placeholder rows are not files either
        int total = 0, shownN = 0;
        for (const RbRow& r : view) if (!r.up && !r.ph) total++;
        for (int i : shown) if (!view[i].up && !view[i].ph) shownN++;
        I.toolbar.statusTotal = total;
        I.toolbar.statusShown = shownN;
        I.toolbar.statusSel   = rbNSel;

        std::string line = "";
        const char* DOT = " \xC2\xB7 ";              // U+00B7, in the font's Latin-1
        char cnt[96];
        // Counts BOTH directions. While a filter is narrowing, "34 items" alone
        // would be a false statement about the folder, so the line says what is
        // in front of you and what it was narrowed from. (This absorbs the
        // toolbar's old shown/total, which stays on the toolbar next to the
        // filter that causes it - the two agree by construction, both being
        // this same pair of numbers.)
        if (shownN != total)
            snprintf(cnt, sizeof cnt, "%d of %d items", shownN, total);
        else
            snprintf(cnt, sizeof cnt, "%d item%s", total, total == 1 ? "" : "s");
        line += cnt;
        // Selection, and only when there IS one: "0 selected" is noise on every
        // glance to report a state whose absence is already obvious.
        if (rbNSel > 0) {
            snprintf(cnt, sizeof cnt, "%d of %d selected", rbNSel, total);
            line += DOT;
            line += cnt;
            // ...and what those rows STAND FOR. A grouped row is one row and
            // four hundred frames, and "2 selected" just before pressing "Open
            // selected as stack" is the row count answering a question nobody
            // asked. Said only when the two numbers differ - otherwise it would
            // print "3 selected, 3 frames" on every ordinary file selection.
            int frames = 0;
            for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
                if (!rbSel[i]) continue;
                frames += view[i].isGroup() && !view[i].e->members.empty()
                        ? (int)view[i].e->members.size() : 1;
            }
            if (frames != rbNSel) {
                snprintf(cnt, sizeof cnt, "%d frames", frames);
                line += DOT;
                line += cnt;
            }
        }
        // ---- and now the things that are usually not true. Connection OK is
        // SILENT: the host's name above is the whole report. Only work in
        // progress, a version disagreement and a failure get words.
        bool warn = false;
        if (I.busy) {
            // I.phase belongs to the worker thread (BrowseInstance::mtx guards
            // queue / done / phase): copy it, never read it in place.
            std::string phase;
            { std::lock_guard<std::mutex> lk(I.mtx); phase = I.phase; }
            line += DOT;
            line += phase.empty() ? std::string("listing...") : phase;
        }
        if (I.search.running) {
            line += DOT;
            line += "searching " + std::string(I.search.pattern) + " under " + I.search.root;
        } else if (!B.searchRoot.empty()) {
            line += DOT;
            line += "search aimed at " + B.searchRoot;
        }
        if (std::string pn = rbProtocolNote(B.peerVersion); !pn.empty()) {
            line += DOT;
            line += pn;
            warn = true;
        }
        // The error band's content, in the line rather than in a band. First
        // line only: the rest is in the details dialog, one click away, which
        // is where a stack trace belonged all along.
        if (!B.err.empty()) {
            std::string first = B.err.substr(0, B.err.find('\n'));
            line += DOT;
            line += "failed: " + first;
            warn = true;
        }
        I.toolbar.statusFull = line;
        // One row, always - the line elides, it does not wrap (300 px is a width
        // this panel has to work at, and a status line that becomes two lines is
        // another moving row).
        const float avail = ImGui::GetContentRegionAvail().x;
        float textW = avail;
        std::string shownLine = rbElideMiddle(line, textW);
        I.toolbar.statusText  = shownLine;
        I.toolbar.statusAvailW = textW;
        I.toolbar.statusTextW  = ImGui::CalcTextSize(shownLine.c_str()).x;
        if (warn) ImGui::PushStyleColor(ImGuiCol_Text, AB_AMBER);
        else      ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(shownLine.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            // the untruncated sentence, plus the way to the full failure text -
            // the old band carried a "copy" button, and a second verb is
            // exactly what this line must not grow
            ImGui::SetTooltip("%s%s", line.c_str(),
                              B.err.empty() ? "" : "\n\nclick for the full failure text");
            if (!B.err.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                app.showRemoteError = true;
        }
    }
    if (rbPropsOpen) { ImGui::OpenPopup("Remote properties"); rbPropsOpen = false; }
    if (ImGui::BeginPopup("Remote properties")) {
        const remote::Entry& e = rbPropsEntry;
        // the path is the thing people need to paste into scripts: selectable
        char pathBuf[1024];
        snprintf(pathBuf, sizeof pathBuf, "%s", rbPropsPath.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        ImGui::InputText("##proppath", pathBuf, sizeof pathBuf, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) {
            ImGui::SetClipboardText(rbPropsPath.c_str());
            g_browseHost.toast("copied " + rbPropsPath, false);
        }
        if (e.group)
            ImGui::Text("stack: %u frames (%s)", e.frames, e.name.c_str());
        if (rbPropsNoSize) {
            // Never invent one: the listing reply carries the group's total
            // bytes and its newest mtime, and no per-member breakdown.
            ImGui::TextDisabled("size      -   (not in the stack listing)");
            ImGui::TextDisabled("modified  -   (not in the stack listing)");
        } else {
        ImGui::Text("size      %s (%llu bytes)%s", fmtBytesHuman(e.size).c_str(),
                    (unsigned long long)e.size, e.group ? "  - all frames" : "");
        ImGui::Text("modified  %s (this machine's timezone)", fmtUnixTime(e.mtime).c_str());
        }
        ImGui::Text("shape     %s%s", fmtEntryShape(e).c_str(),
                    e.hasMeta && e.group ? "  - per frame" : "");
        if (!e.hasMeta)
            ImGui::TextDisabled("shape/dtype need a protocol 3 peer (File > Update remote peer)");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
