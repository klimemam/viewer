// core/browse/nav.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
// P7 で背骨の TU から Browse の TU へ移った。ここの関数は背骨・メニュー・selftest が呼ぶので
// extern (宣言は browse.h)。static は rbStopWorkerOf の前方宣言だけ残る。
// ---- Browse instances (docs/background/project/todo-open.md item 17: instance-able views) -----
// What a Browse panel is CALLED. The host is the most important fact about a
// panel and it used to live inside the "more" drawer, which meant that with two
// panels open the two were pixel-identical until you unfolded one
// (docs/features/browse/browse-topbar-design.md 10.3). It belongs in the title: a title is read
// once, changes rarely, and costs no row.
//
//   Browse                    - this machine (local:// is LOCAL, not a remote:
//                               the panel never says "ssh" about a pipe)
//   Browse [ssh: trc2]        - a machine reached over ssh
//   Browse 2 [ssh: trc2]      - ...and which panel it is
//
// The ### id is NOT touched by any of this: layouts, the dock builder and the
// session file key off "###Remote" / "###BrowseN", so a panel that renames
// itself on connect still docks where the user left it.
std::string rbPanelTitle(int num, const std::string& host) {
    std::string t = "Browse";
    if (num > 1) t += " " + std::to_string(num);
    if (!host.empty()) t += " [ssh: " + host + "]";   // cf. ICON_SSH on the status line
    t += num > 1 ? "###Browse" + std::to_string(num) : std::string("###Remote");
    return t;
}
// The primordial instance. Lazy so it exists whenever anything asks - the
// selftests, the menus and the first frame all funnel through here.
App::BrowseInstance& rbMain() {
    if (app.browsePanels.empty()) {
        auto p = std::make_unique<App::BrowseInstance>();
        p->num = 1;
        p->wtitle = rbPanelTitle(1, "");    // the singleton's id: layouts survive
        p->flat = app.rbFlat;
        p->tree = app.rbTree;
        p->nameNatural = app.rbNatural;
        app.browsePanels.push_back(std::move(p));
    }
    return *app.browsePanels[0];
}
App::BrowseInstance* rbFindNum(int num) {
    for (auto& p : app.browsePanels)
        if (p->num == num) return p.get();
    return nullptr;
}
// The instance that last had keyboard focus: what the File menu's remote
// commands and the local-folder dialog aim at when no panel is naming itself.
int g_rbActiveNum = 1;
App::BrowseInstance& rbActive() {
    if (App::BrowseInstance* p = rbFindNum(g_rbActiveNum)) return *p;
    return rbMain();
}
// A new instance: the "+" in the panel and View > New Browse Panel. The number
// is the smallest unused >= 2, so window ids stay stable across sessions.
App::BrowseInstance& rbNewInstance(int wantNum) {
    rbMain();                               // [0] is always the primordial one
    int num = wantNum >= 2 ? wantNum : 2;
    while (rbFindNum(num)) num++;
    auto p = std::make_unique<App::BrowseInstance>();
    p->num = num;
    p->wtitle = rbPanelTitle(num, "");
    p->flat = app.rbFlat;                   // a new view starts from the prefs
    p->tree = app.rbTree;
    p->nameNatural = app.rbNatural;
    p->open = true;
    p->focusReq = true;
    app.browsePanels.push_back(std::move(p));
    return *app.browsePanels.back();
}
// Make an instance's window visible and bring it forward (what showRemote +
// focusRemote did for the singleton). num 1 stays on app.showRemote: that is
// the flag the session's "panels" line and View > Panels > Browse speak.
void rbShowInstance(App::BrowseInstance& I) {
    if (I.num == 1) app.showRemote = true;
    else I.open = true;
    I.focusReq = true;
}
bool rbInstanceShown(const App::BrowseInstance& I) {
    return I.num == 1 ? app.showRemote : I.open;
}
// Is this window name a Browse instance? The main loop's shortcut table stands
// down for the FOCUSED instance, whichever one that is.
bool rbIsBrowseWindowName(const char* n) {
    return n && (strstr(n, "###Remote") != nullptr || strstr(n, "###Browse") != nullptr);
}
static void rbStopWorkerOf(App::BrowseInstance& I);   // defined with rbWorker
// Destroy a closed instance (num >= 2 only): its worker stops cleanly first.
// After the join the Session has exactly one toucher left - this thread - so
// destroying it here keeps the ownership rule intact.
void rbDestroyInstance(int num) {
    if (num == 1) return;                   // the primordial one only hides
    for (size_t i = 0; i < app.browsePanels.size(); i++)
        if (app.browsePanels[i]->num == num) {
            rbStopWorkerOf(*app.browsePanels[i]);
            if (g_rbActiveNum == num) g_rbActiveNum = 1;
            app.browsePanels.erase(app.browsePanels.begin() + i);
            return;
        }
}
// Aggregations for the main loop's wake logic: "is ANY browse worker doing
// something the window must keep animating for".
bool rbAnyBusy() {
    for (auto& p : app.browsePanels) if (p->busy) return true;
    return false;
}
bool rbAnyDonePending() {
    for (auto& p : app.browsePanels) {
        std::lock_guard<std::mutex> lk(p->mtx);
        if (!p->done.empty()) return true;
    }
    return false;
}
bool rbAnyTreePending() {
    for (auto& p : app.browsePanels) if (!p->treePending.empty()) return true;
    return false;
}
