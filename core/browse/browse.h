// core/browse/browse.h — viewer → browse の入口宣言。担当: Browse
// P7 (docs/split-plan.md §3): S3 まで remote_client.inc の static 前方宣言が
// 担っていた役をヘッダに移す。背骨(フレームループ・teardown・browse-keys
// ドライバ)、menus / session / open_dispatch / cli、そして selftest 群が呼ぶ
// browse 側シンボルは全部ここ。browse の 2 TU (nav.cpp / panel.cpp) も互いを
// この宣言越しに見る。ここに無いものは TU 内部 (static) であり、外から呼ばない。
#pragma once

#include "browse_state.h"
#include "../app/state.h"            // App::BrowseInstance (= browse::Instance)

// ---- instance management (instances.inc, compiled into nav.cpp) --------------
std::string rbPanelTitle(int num, const std::string& host);
App::BrowseInstance& rbMain();
App::BrowseInstance* rbFindNum(int num);
// The instance that last had keyboard focus: what the File menu's remote
// commands and the local-folder dialog aim at when no panel is naming itself.
extern int g_rbActiveNum;
App::BrowseInstance& rbActive();
App::BrowseInstance& rbNewInstance(int wantNum = 0);
void rbShowInstance(App::BrowseInstance& I);
bool rbInstanceShown(const App::BrowseInstance& I);
bool rbIsBrowseWindowName(const char* n);
void rbDestroyInstance(int num);
// Aggregations for the main loop's wake logic: "is ANY browse worker doing
// something the window must keep animating for".
bool rbAnyBusy();
bool rbAnyDonePending();
bool rbAnyTreePending();

// ---- worker, pumps, navigation (nav.cpp) --------------------------------------
void rbEnqueue(App::BrowseInstance& I, App::RbJob job);
void stopRbWorker();                 // shutdown: every instance's worker
void pumpRemoteBrowse();             // UI thread, once per frame
void pumpRemoteOpenQueue();          // folder-scan stacks, opened one at a time
bool rbHas(const std::vector<std::string>& v, const std::string& s);
void rbTreeExpand(App::BrowseInstance& I, const std::string& dir);
void rbTreeCollapse(App::BrowseInstance& I, const std::string& dir);
void rbTreeForget(App::BrowseInstance& I);
void rbHistGo(App::BrowseInstance& I, bool back);
void remoteBrowseTo(App::BrowseInstance& I, const std::string& dir);
std::string placeUrl(const std::string& host, int port, const std::string& path);
void goToPlace(App::BrowseInstance& I, const std::string& url);
// the viewer → browse restore entry (§3: session の browse place 復元は viewer
// 側の仕事で、BrowseHost のコールバックではない - S3 の判断のまま)
void sessionRestoreBrowsePlace(int num, const std::string& url);
void remoteStartSearch(App::BrowseInstance& I,
                       const std::string& root, const std::string& pattern);
void remoteScanFolder(App::BrowseInstance& I, const std::string& root);
App::BrowseInstance& rbInstanceFor(const std::string& hostSpec);
void startRemote(App::BrowseInstance& I, const std::string& hostSpec);

// ---- the listing view, and the panel (panel.cpp) ------------------------------
// row formatting the CLI's remote selftest reuses (cli.inc)
std::string fmtBytesHuman(uint64_t n);
std::string fmtEntryShape(const remote::Entry& e);

// ---- listing view: one row of the Browse table --------------------------------
// A numbered sequence arrives as ONE synthetic entry carrying `.members`, so
// "show me the individual frames" is a view over the reply we already have -
// no LIST, no round trip, nothing to invalidate. `member` picks which face the
// row wears: -1 = the entry itself (folder, plain file, or the collapsed group
// row), >= 0 = the n-th frame of a group.
//
// What an expanded frame does NOT have is its own size and mtime: the group
// reply carries the SUM of the members' bytes and the NEWEST member's time,
// and there is no per-file breakdown in it. Those cells stay blank rather than
// repeating the group's numbers on 24 rows, which would be a lie 24 times.
// shape/dtype are shared by construction (the peer only groups files that
// agree), so they are shown.
// In TREE mode rows no longer all come from one directory, so a row carries the
// directory it was listed from. `dir` points at a string that outlives the
// frame: either App::RemoteBrowse::dir or a key of App::rbTreeCache (std::map
// nodes do not move).
struct RbRow {
    const remote::Entry* e = nullptr;
    const std::string* dir = nullptr;
    int member = -1;
    bool up = false;               // the ".." row (listing only - see rbBuildView)
    int depth = 0;                 // tree indent level; 0 = the listed folder
    bool ph = false;               // "(listing...)": a node whose LIST is in flight
    const std::string& name() const { return member < 0 ? e->name : e->members[member]; }
    bool isDir()   const { return !ph && member < 0 && e->dir; }
    bool isGroup() const { return !ph && member < 0 && e->group; }
    bool ownFile() const { return member < 0; }   // has its own size / mtime
    // absolute path of this row, and of one of its members
    std::string join(const std::string& n) const {
        return *dir == "/" ? "/" + n : *dir + "/" + n;
    }
    std::string full() const { return join(name()); }
};
// The listing table's columns, and the sort the tree builder has to honour.
// Stashed from the table (TableGetSortSpecs only exists between Begin/EndTable,
// and a tree has to be sorted per LEVEL while it is being built, one frame
// earlier). A sort change therefore lands on the next frame - invisible, and
// far cheaper than building the view twice.
enum { RB_COL_NAME = 0, RB_COL_SHAPE, RB_COL_SIZE, RB_COL_MTIME };

// Free functions so the headless (NOGL) selftests drive exactly what the panel
// draws - the reason they are declared here and not kept TU-internal.
std::vector<RbRow> rbBuildView(const App::BrowseInstance& I,
                               const std::string* dir,
                               const std::vector<remote::Entry>& entries,
                               bool flat, bool tree);
void rbSortShown(const App::BrowseInstance& I, const std::vector<RbRow>& view,
                 std::vector<int>& shown);
void rbAncestorRows(const std::vector<RbRow>& view, const std::vector<int>& shown,
                    int firstRow, std::vector<int>& out);

// deferred panel actions - see panel.cpp rbDefer for the ownership story. The
// selftests queue through the same door the panel does, so the door is here.
void rbDefer(std::function<void()> f);
size_t rbDeferredPending();
// RAII, so the panel's early returns run the queue too. Declared BEFORE `view`
// in drawPanelRemote: reverse destruction order is what guarantees the rows are
// already gone when the queue runs. (Bodies live in panel.cpp: they touch the
// TU-internal "instance being drawn" pointer, which nothing outside may see.)
struct RbDeferredActions {
    App::BrowseInstance& I;
    App::BrowseInstance* prev;        // draws never nest today; stay honest anyway
    explicit RbDeferredActions(App::BrowseInstance& i);
    ~RbDeferredActions();
};

std::string rbProtocolNote(int pv);  // the status line's version sentence
void drawPanelRemote(App::BrowseInstance& I);

// ---- selftest probes (panel.cpp; read/written by the browse-keys driver and
// the harness ops in the spine) -------------------------------------------------
extern float g_rbForceW;    // >0: selftest floats instance 1 at this width
extern float g_rbForceH;    // ...and this height (0 = 80% of the work area)
extern float g_rbListTopY0; // "marklist": the listing-top baseline
extern int   g_rbStar0;     // "starmark": the bookmark star's baseline
extern int   g_rbPeerV0;    // "setpv" saves the real peer version; "pvback" restores
extern ImVec2 g_injMouse;   // --browse-keys-selftest's injected cursor (<0: off)
extern int    g_injMouseBtn;
extern int    g_rbKeysTarget;        // instance NUM the keys selftest drives
App::BrowseInstance& rbKeysT();
