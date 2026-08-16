// core/browse/nav.cpp — Browse 実 TU その 1 (P7, docs/split-plan.md §3/§5)。担当: Browse
// instance 管理 (instances.inc) + worker / pump / navigation (この下)。
// viewer への呼び出しは g_browseHost (host.h) 経由のみ — S3 が置いた
// BrowseHost 注記 15 箇所はここで潰れた。GLFW への依存も一緒に消えている
// (rbWorker の UI 起こしは host の wakeUi になった)。
#include "../app/state.h"
#include "../remote_proto.h"         // rp::VERSION - the protocol both ends speak
#include "host.h"                    // the browse -> viewer seam (g_browseHost)
#include "browse.h"                  // ...and the declarations this TU defines

#include <algorithm>
#include <cmath>                     // ceil (browseWatchEvery's ratio)
#include <cstring>                   // strstr (rbIsBrowseWindowName), memcmp
#include <mutex>                     // the poll round asks the instance's queue
#include <string>
#include <vector>

#include "instances.inc"

// ---- the connect/browse worker: ONE per Browse instance ------------------------
// The instance's Session belongs to the instance's worker thread and to nobody
// else - the singleton's "one Session, one owning thread" rule (1ac211e), now
// held per place being viewed. N instances = N sessions x N owning threads;
// still nothing shared, still nothing to lock beyond each instance's own queue.
static void rbSetPhase(App::BrowseInstance& I, const std::string& p) {
    std::lock_guard<std::mutex> lk(I.mtx);
    I.phase = p;
}

static void rbWorker(App::BrowseInstance* ip) {
    App::BrowseInstance& I = *ip;
    while (!I.stop) {
        App::RbJob job;
        {
            std::unique_lock<std::mutex> lk(I.mtx);
            I.cv.wait(lk, [&I] { return !I.queue.empty() || I.stop; });
            // Stop wins over pending work - see the same line in rfWorker. It
            // matters more here: the extra job can be a connect to a host that
            // has no peer installed, which falls into deployPeer, which takes no
            // abort pointer and which the comment there calls minutes of ssh.
            // Quitting, or closing the panel, would freeze for that long with no
            // window drawn. Session::setAbort cannot help - it only gates the
            // read loop.
            if (I.stop) break;
            job = std::move(I.queue.front());
            I.queue.erase(I.queue.begin());
        }
        // A POLL is not something anybody is waiting for (watch-design §2's
        // second row), so it does not raise `busy`: rbAnyBusy is the main
        // loop's "keep this window animating" question and the panel's
        // spinner, and a background re-listing must answer neither. The skip
        // rule asks pollPending instead - see rbPollDue.
        const bool rbQuiet = job.kind == App::RbPoll;
        if (!rbQuiet) I.busy = true;
        App::RbResult r;
        r.kind = job.kind;
        r.host = job.host;
        r.port = job.port;
        r.dir = job.dir;
        r.gen = job.gen;              // the cancel token, for every kind that has one
        const std::string& exe = job.exe;   // snapshot taken at enqueue time
        if (job.kind == App::RbUpdatePeer) {
            rbSetPhase(I, "updating the peer on " + job.host + " (git)...");
            if (I.session) I.session->stop();
            std::string log;
            r.ok = deployPeer(job.host, job.port, true, log);
            r.info = log;
            r.err = r.ok ? "" : log;
        } else if (job.kind == App::RbDisconnect) {
            // stop() runs on the thread that owns the Session, never on the UI
            // thread that merely clicked the status bar
            if (I.session) I.session->stop();
            r.ok = true;
        } else {
            if (!I.session) {
                I.session.reset(new remote::Session());
                I.session->setAbort(&I.stop);   // Quit interrupts a blocked read
            }
            std::string err;
            bool alive = I.session->alive() && I.session->host() == job.host;
            if (!alive) {
                rbSetPhase(I, job.host.empty() ? "starting the local reader..."
                                               : "connecting to " + job.host + "...");
                alive = I.session->startOn(job.host, job.port, exe, err);
                if (!alive && !job.host.empty()) {
                    // not there: the server installs its own peer from the
                    // binaries branch. This is the multi-second step that used
                    // to freeze the window.
                    rbSetPhase(I, "installing the peer on " + job.host + "...");
                    std::string log;
                    bool got = deployPeer(job.host, job.port, false, log);
                    r.info = log;    // the UI thread owns g_bootstrapLog, not us
                    if (got) {
                        rbSetPhase(I, "connecting to " + job.host + "...");
                        alive = I.session->startOn(job.host, job.port, exe, err);
                    } else {
                        err = "could not install the peer on " + job.host + ":\n" + log;
                    }
                }
            }
            if (!alive) {
                r.err = err.empty() ? "connection failed" : err;
            } else if (job.kind == App::RbScan) {
                rbSetPhase(I, "scanning " + job.dir + " for stacks...");
                bool trunc = false;
                int skipped = 0;
                // The depth is scanDepthBelow()'s and not this call site's
                // (#204): the local walk in scanFolderGroups asks the same
                // function, so the two doors cannot answer the same folder
                // differently again. The 256 is still a literal here because
                // the peer's cap and the local one have never disagreed and
                // neither is a setting.
                if (I.session->scan(job.dir, scanDepthBelow(), 256, r.scanGroups,
                                    trunc, skipped, err)) {
                    r.ok = true;
                    r.truncated = trunc;
                    r.skippedDirs = skipped;
                } else {
                    r.err = err;
                }
            } else if (job.kind == App::RbGlob) {
                rbSetPhase(I, "searching " + job.pattern + " under " + job.dir + "...");
                bool trunc = false;
                int skipped = 0;
                if (I.session->glob(job.dir, job.pattern, 6, 2000, r.hits,
                                    trunc, skipped, err)) {
                    r.ok = true;
                    r.truncated = trunc;
                    r.skippedDirs = skipped;
                } else {
                    r.err = err;
                }
            } else {
                // ...and it says nothing either: the phase line is what the
                // panel prints while the user waits, and "listing ..." flashing
                // up every three seconds on its own is the visible half of a
                // poll that is supposed to be silent.
                if (!rbQuiet) rbSetPhase(I, "listing " + job.dir + "...");
                std::vector<remote::Entry> got;
                if (I.session->list(job.dir, got, err)) {
                    r.ok = true;
                    r.entries = std::move(got);
                } else {
                    r.err = err;
                }
            }
        }
        // Published from the owning thread with EVERY job kind: the UI reads
        // these plain values instead of ever naming the Session.
        if (I.session) {
            r.peerVersion = I.session->peerVersion();
            r.alive = I.session->alive();
            r.rx = I.session->bytesReceived();
        }
        if (!rbQuiet) rbSetPhase(I, "");
        {
            std::lock_guard<std::mutex> lk(I.mtx);
            I.done.push_back(std::move(r));
        }
        if (!rbQuiet) I.busy = false;
        // A poll DOES wake the UI: the reply has to be looked at (it may be a
        // new listing), and the pumps live inside the frame. What it must never
        // do is wake it to DECIDE to poll - that is the idle-skip term
        // rbAnyPollDue, asked once per idle timeout and false unless a panel is
        // both drawn and overdue.
        g_browseHost.wakeUi();
    }
}

void rbEnqueue(App::BrowseInstance& I, App::RbJob job) {
    // how the peer is invoked, frozen NOW on the UI thread that owns the string
    if (job.exe.empty())
        job.exe = app.remoteExe.empty()
            ? (job.host.empty() ? app.exePath : std::string(REMOTE_HOME) + "/viewer-serve")
            : app.remoteExe;
    // A NAVIGATION RE-ARMS the poll timer (watch-design §2's second row). The
    // listing about to arrive is fresh, so the next round belongs one interval
    // after IT, not one interval after the last poll - otherwise walking into a
    // folder can be followed by a re-listing of it a tenth of a second later.
    // Here rather than in the six callers: every way of navigating funnels
    // through this queue, which is the same argument remoteBrowseTo makes for
    // owning the history.
    if (job.kind == App::RbConnect || job.kind == App::RbList) I.polledAt = 0;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        I.queue.push_back(std::move(job));
    }
    I.cv.notify_one();
    if (!I.thread.joinable()) I.thread = std::thread(rbWorker, &I);
}

// Stop ONE instance's worker (closing its window, or shutdown). After the join
// this thread is the Session's only toucher, so letting the unique_ptr destroy
// it later is single-threaded by construction.
static void rbStopWorkerOf(App::BrowseInstance& I) {
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        I.stop = true;
    }
    I.cv.notify_all();
    if (I.thread.joinable()) I.thread.join();
}
void stopRbWorker() {          // shutdown: every instance's worker
    for (auto& p : app.browsePanels) rbStopWorkerOf(*p);
}

// UI thread: apply what ONE instance's worker produced.
static void pumpRemoteBrowseOne(App::BrowseInstance& I) {
    std::vector<App::RbResult> batch;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        batch.swap(I.done);
    }
    for (auto& r : batch) {
        App::RemoteBrowse& B = I.b;
        // What the worker saw of ITS session, applied for every job kind. The
        // old code cleared B.connected only for RbConnect, so a LIST that died
        // with the peer left the status bar showing a green link over a corpse.
        if (r.kind != App::RbUpdatePeer && r.kind != App::RbDisconnect) {
            B.peerVersion = r.peerVersion;
            B.rxBytes = r.rx;
            if (B.connected && !r.alive) B.connected = false;
        }
        if (r.kind == App::RbDisconnect) continue;
        if (r.kind == App::RbUpdatePeer) {
            if (!r.info.empty()) g_bootstrapLog = r.info;
            g_browseHost.toast(r.ok ? "remote peer updated on " + r.host
                             : "remote update failed: " + r.err, !r.ok);
            // The update stopped the session; reconnect to where the user was,
            // so the listing comes back with the new peer's metadata without
            // another trip through Start Remote.
            if (r.ok && B.host == r.host) {
                App::RbJob j;
                j.kind = App::RbConnect;
                j.host = B.host; j.port = B.port; j.dir = B.dir;
                rbEnqueue(I, std::move(j));
            }
            continue;
        }
        if (r.kind == App::RbGlob) {
            App::RemoteSearch& S = I.search;
            if (r.gen != S.gen) continue;          // stopped or superseded: drop
            S.running = false;
            if (!r.ok) { g_browseHost.toast("remote search: " + r.err, true); continue; }
            S.hits = std::move(r.hits);
            S.truncated = r.truncated;
            S.skippedDirs = r.skippedDirs;
            S.active = true;
            continue;
        }
        if (r.kind == App::RbScan) {
            // Stopped or superseded: DROP it. openPickerWith clears
            // app.folderPick and resets the picker's root / filter / merge /
            // sweep fields, so a late reply from an abandoned scan does not
            // merely arrive - it destroys whatever the picker currently holds,
            // including a local Open Folder selection being filtered, and
            // re-raises the modal over it. RbGlob has had this guard since it
            // was written; RbScan carried no token at all.
            if (r.gen != B.scanGen) continue;
            // The scan you started has ANSWERED - recorded here, before the
            // answer is acted on, and for the EMPTY answer too. Zero groups
            // opens no picker, queues nothing and loads nothing, so a caller
            // watching for a stack to appear has nothing to watch for; this is
            // the only place the fact exists (the reply is consumed below and
            // dropped). See RemoteBrowse::scanDoneGen for what that cost.
            B.scanDoneGen = r.gen;
            B.scanGroups = r.ok ? (int)r.scanGroups.size() : -1;
            if (!r.ok) { g_browseHost.toast("remote scan: " + r.err, true); continue; }
            auto joinR = [](const std::string& a, const std::string& b) {
                if (b.empty()) return a;
                return a == "/" ? "/" + b : a + "/" + b;
            };
            std::vector<App::PendingGroup> groups;
            // batchId stays 0 here: batches are made at ACCEPT time in
            // pickerAccept (Cancel must not leave an empty batch behind)
            for (const auto& g : r.scanGroups) {
                App::PendingGroup pg;
                pg.name = g.dir.empty() ? g.entry.name : g.dir + "/" + g.entry.name;
                std::string base = joinR(r.dir, g.dir);
                for (const auto& m : g.entry.members) pg.files.push_back(joinR(base, m));
                if (g.entry.hasMeta && g.entry.ndim > 0) {   // v3 metadata -> the
                    for (int d = 0; d < g.entry.ndim; d++)   // picker's shape column
                        pg.shape += (d ? "x" : "") + std::to_string(g.entry.dims[d]);
                    if (!g.entry.dtype.empty()) pg.shape += " " + g.entry.dtype;
                }
                groups.push_back(std::move(pg));
            }
            if (r.truncated)
                g_browseHost.toast("scan stopped at 256 stacks - open a narrower folder", true);
            if (r.skippedDirs)
                g_browseHost.toast(std::to_string(r.skippedDirs) +
                                   " unreadable folder(s) skipped in the scan", true);
            if (groups.empty()) { g_browseHost.toast("no .npy stacks under " + r.dir, true); continue; }
            // A remote scan ALWAYS goes through the picker - one group included.
            // Every frame here is a transfer, the modal is where the filters
            // live, and the "1 group -> just open it" shortcut turned every
            // scan STARTED INSIDE a leaf folder into "it opened everything
            // without asking" (verbatim, reported three times).
            {
                // the picker "not appearing" has now been reported three times
                // with three different causes; the trail stays in
                fprintf(stderr, "remote scan: %d groups -> picker requested\n",
                        (int)groups.size());
                // The dialog's first line is READ, not pasted: for a local
                // browse it is a path on this disk, and prefixing it "local://"
                // told the user a scheme they never chose and a machine they
                // are not talking to. The url form is unchanged everywhere it
                // is stored (prefs, bookmarks, Copy path).
                g_browseHost.openPickerWith(std::move(groups),
                               r.host.empty() ? r.dir : makeRemoteUrl(r.host, r.dir, r.port),
                               r.dir, true, r.host, r.port);
            }
            continue;
        }
        if (r.kind == App::RbPoll) {
            // watch-design §2/§5. What this branch does NOT do is most of it,
            // and each omission is a rule:
            //
            //   a failed LIST is dropped, not shown. §13.4: an unreachable
            //   peer, a dropped ssh link and a refused path all arrive as "no
            //   reply", and a listing nobody asked for must not put an error
            //   band over a panel that is showing perfectly good rows. The
            //   next round comes.
            //   an answer about somewhere ELSE is dropped. The user navigated
            //   while it was in flight, and B.dir is not this reply's dir.
            //   an answer IDENTICAL to what is on screen is dropped, whole.
            //   Bumping `rev` for it would re-run the cursor and selection
            //   carry, re-key every row cache and cost a frame, three seconds
            //   at a time, to say that nothing happened (§3's idle promise is
            //   what that would spend).
            //   the recents are NOT touched. A poll is not a visit: reordering
            //   the places list and setting prefsDirty every three seconds
            //   would rewrite prefs.txt because a panel was left open.
            I.pollPending = false;
            if (!r.ok) continue;
            if (r.host != B.host || r.port != B.port || r.dir != B.dir) continue;
            if (rbSameListing(B.entries, r.entries)) continue;
            B.entries = std::move(r.entries);
            B.rev++;                  // ...and the cursor and ticks follow it
            I.pollsApplied++;
            continue;
        }
        if (r.kind == App::RbTreeList) {
            // A node's children. It must NOT touch B.dir / B.entries / recents:
            // expanding a folder in the tree is not a navigation.
            I.treePending.erase(std::remove(I.treePending.begin(),
                                            I.treePending.end(), r.dir),
                                I.treePending.end());
            if (r.ok) I.treeCache[r.dir] = std::move(r.entries);
            else {
                // an unreadable folder collapses again, with a reason
                I.expanded.erase(std::remove(I.expanded.begin(),
                                             I.expanded.end(), r.dir),
                                 I.expanded.end());
                g_browseHost.toast(r.dir + ": " + r.err, true);
            }
            continue;
        }
        if (!r.ok) {
            B.err = r.err;
            // the install log comes home in r.info and is assigned HERE, on the
            // UI thread that owns g_bootstrapLog - the two windows that print it
            // read it every frame
            if (!r.info.empty()) g_bootstrapLog = r.info;
            if (r.kind == App::RbConnect) {
                B.connected = false;
                // so the failure text has somewhere to live - inline, in THE
                // instance that failed, never a global dialog
                rbShowInstance(I);
                g_browseHost.toast("remote: " + r.err, true);
            }
            continue;
        }
        if (!r.info.empty()) g_bootstrapLog = r.info;   // installed, then connected
        B.err.clear();
        B.host = r.host;
        B.port = r.port;
        B.dir = r.dir;
        B.entries = std::move(r.entries);
        B.rev++;                      // the row caches key on this, not on a count
        {   // every directory actually listed becomes the newest "recent place"
            std::string u = placeUrl(r.host, r.port, r.dir);
            auto& v = app.rbRecents;
            v.erase(std::remove(v.begin(), v.end(), u), v.end());
            v.insert(v.begin(), u);
            if (v.size() > 10) v.resize(10);
            app.prefsDirty = true;
        }
        if (r.kind == App::RbConnect && !B.connected) {
            B.connected = true;
            rbTreeForget(I);              // another machine: other children entirely
            // a listing nobody can see is not a connection anyone believes in
            rbShowInstance(I);
            g_browseHost.toast(r.host.empty() ? "browsing " PEER_HERE
                                              : "connected to " + r.host, false);
        }
        // An outdated peer ANSWERS perfectly well, so the install-on-failure
        // path never runs for it - and every listing would show "-" for shape
        // and mtime until someone finds the update menu item. Update it now,
        // unasked, once per connect: this is the vscode-server model the whole
        // remote design follows. The reconnect rides on the RbUpdatePeer
        // result above; autoUpdateTried stops a loop if the update binary is
        // still old (e.g. the binaries branch has not rebuilt yet).
        if (r.kind == App::RbConnect && !r.host.empty() && r.peerVersion > 0 &&
            r.peerVersion < (int)rp::VERSION && !B.autoUpdateTried) {
            B.autoUpdateTried = true;
            g_browseHost.toast("peer on " + r.host + " is protocol " +
                               std::to_string(r.peerVersion) + " - updating it now...", false);
            App::RbJob j;
            j.kind = App::RbUpdatePeer;
            j.host = r.host; j.port = r.port;
            rbEnqueue(I, std::move(j));
        }
        if (!I.pendingOpen.empty()) {   // a url was pasted with the host
            std::string u = I.pendingOpen;
            I.pendingOpen.clear();
            g_browseHost.openRemote(u, false, 0, 0);
        }
    }
}
// ...and every instance, once per UI frame.
void pumpRemoteBrowse() {
    for (size_t i = 0; i < app.browsePanels.size(); i++)
        pumpRemoteBrowseOne(*app.browsePanels[i]);
}

// ---- watch-design §2, second row: a DRAWN instance re-lists where it stands ---
//
// The stack half of Watch (core/app/watch.inc) polls to SAY something: a stack's
// pixels and its files disagree, and a line of amber says so and waits for a
// human. This half says nothing at all. §5 is explicit about the difference -
// "a listing is something you look at, not a measurement, so it may update
// silently" - and that is not a shortcut but the whole reason no notification
// belongs here: the panel is a view of a directory, and a view whose subject
// moved is simply out of date.
//
// What that buys, and what it therefore has to be worth: the listing is REPLACED
// under the reader's eyes. It is safe to do only because the cursor and the
// multi-selection follow the ROW BY NAME across a rev bump (rbCursorFollow /
// rbSelFollow, PR #155). Before #155 a second listing of the same folder wiped
// both, which was an intermittent defect when it took a duplicate reply to
// trigger it and would be a permanent one at one re-listing every three seconds.
// --browse-selftest B4 is that assertion and it is the reason this stage could
// be built at all.

// Two listings, as the panel would draw them. Compared POSITION BY POSITION,
// which is sound because both came off the same peer's LIST and core/serve.cpp
// sorts its reply - two readings of an unchanged directory are byte-identical,
// so a difference here is a difference on the disk and not a difference of
// order. Every field the panel puts on screen is in it: a row whose SIZE moved
// is a changed listing even though its name did not.
bool rbSameListing(const std::vector<remote::Entry>& a,
                   const std::vector<remote::Entry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const remote::Entry& x = a[i];
        const remote::Entry& y = b[i];
        if (x.name != y.name || x.dir != y.dir || x.size != y.size ||
            x.mtime != y.mtime || x.group != y.group || x.frames != y.frames ||
            x.members != y.members || x.hasMeta != y.hasMeta || x.dtype != y.dtype ||
            x.ndim != y.ndim || x.fortran != y.fortran ||
            memcmp(x.dims, y.dims, sizeof x.dims) != 0)
            return false;
    }
    return true;
}

// §2: "while that instance is BEING DRAWN". drawPanelRemote writes the mark at
// its head, and the spine calls it only when ImGui::Begin answered true - so a
// window that is closed, collapsed or sitting on an unselected dock tab stops
// advancing the mark and stops being polled, with nothing having to be told.
//
// A FRAME NUMBER and not a timestamp, and this is the correction §14.1 records:
// this program idles at 0 fps, so a `lastDrawnAt` on a panel that is plainly on
// screen goes stale the moment the user stops touching anything - which is
// precisely the case this feature is for (watching a capture folder fill while
// doing nothing). The last frame that HAPPENED is what is on the glass, and a
// frame counter says that where a clock says the opposite.
//
// "This frame or the one before": the pump runs at the head of a frame, before
// this frame's draw, so an instance drawn in the frame that just finished
// carries uiFrame - 1. drawnFrame 0 is "never drawn" and is never a match, which
// is why uiFrame starts at 1.
static bool rbInstanceDrawn(const App::BrowseInstance& I, uint64_t uiFrame) {
    return I.drawnFrame != 0 && I.drawnFrame + 1 >= uiFrame;
}

// §2's two Browse intervals as ONE decision, in watchRemoteEvery's shape (§13.7):
// the LOCAL interval is the tick, and a peer is asked every Nth one with N
// computed from the two constants rather than typed. The point of computing it
// is what happens when one of them moves: nothing can then leave the peer polled
// at the local rate, because N is never less than 1. 3 s and 10 s give
// ceil(10/3) = 4, so a peer's folder is re-listed every 4 x 3 = 12 s - twelve
// and not ten, which is the price of the ratio being a whole number of rounds
// and is recorded as such in §14.3.
int browseWatchEvery() {
    const double a = app.browseWatchIntervalSec, b = app.browseWatchRemoteIntervalSec;
    if (a <= 0.0 || b <= a) return 1;
    return (int)std::ceil(b / a - 1e-9);
}
double browseWatchInterval(bool peer) {
    const double a = app.browseWatchIntervalSec;
    return peer ? a * browseWatchEvery() : a;
}

// Does this instance owe a round right now? Nothing in here reads a clock or
// touches the network: `now` is handed in, for the reason watchPollRound is
// handed its lister - a test that had to live through a three-second interval
// would be a slow test that fails on a loaded machine.
bool rbPollDue(const App::BrowseInstance& I, double now, uint64_t uiFrame) {
    // ONE switch for the feature (§9: "start with one global switch"). Watch
    // turned off stops the Browse half with the stack half; a listing that
    // refreshes itself is Watch whichever panel it lands in.
    if (!app.watchEnabled || app.watchPaused) return false;
    if (!I.b.connected) return false;
    // §2 says the instance's CURRENT DIR. The search results view stands in for
    // the listing (remoteBrowseTo turns it off for that very reason), so while
    // it is up the listing is not on screen and re-reading it would be a round
    // trip for rows nobody can see.
    if (I.search.active) return false;
    if (!rbInstanceDrawn(I, uiFrame)) return false;
    // §2: SKIP, NEVER QUEUE. A queue of stale listings is worse than a missed
    // round - each one replaces the listing when it lands, so a slow link would
    // redraw the panel N times with N answers to the same question, the last
    // N-1 already out of date on arrival. Three ways to be occupied and all
    // three count: a job running, a poll already out (a poll deliberately does
    // not set `busy`), and a navigation waiting to go out.
    if (I.busy || I.pollPending) return false;
    {
        std::lock_guard<std::mutex> lk(I.mtx);
        if (!I.queue.empty()) return false;
    }
    if (I.polledAt <= 0) return false;          // the timer is not running yet
    return now - I.polledAt >= browseWatchInterval(!I.b.host.empty());
}

// ONE ROUND for one instance. Returns true exactly when a LIST went out.
bool rbPollRound(App::BrowseInstance& I, double now, uint64_t uiFrame) {
    // ARMING is not polling. A panel that has just been drawn, or that has just
    // navigated, is looking at a listing that arrived a moment ago; re-reading
    // it because a timer had never been started would be a round trip for a
    // fact already on the glass. So the first round after either event starts
    // the clock and lists nothing - the same shape as §1's first observation,
    // which takes the baseline and announces nothing.
    if (I.polledAt <= 0) {
        if (app.watchEnabled && !app.watchPaused && I.b.connected &&
            rbInstanceDrawn(I, uiFrame))
            I.polledAt = now;
        return false;
    }
    if (!rbPollDue(I, now, uiFrame)) return false;
    I.polledAt = now;
    I.pollsIssued++;
    I.pollPending = true;
    App::RbJob j;
    j.kind = App::RbPoll;
    j.host = I.b.host;
    j.port = I.b.port;
    j.dir  = I.b.dir;
    rbEnqueue(I, std::move(j));
    return true;
}

// ...and every instance, once per UI frame, beside pumpRemoteBrowse.
void pumpBrowseWatch(double now) {
    for (size_t i = 0; i < app.browsePanels.size(); i++)
        rbPollRound(*app.browsePanels[i], now, app.uiFrame);
}
// The idle-skip chain's term (§3). It asks "is a round DUE", never "is a Browse
// panel open": with no panel drawn, none connected or Watch switched off it is
// false and the 0 fps idle is exactly what it was.
bool rbAnyPollDue(double now) {
    for (auto& p : app.browsePanels)
        if (rbPollDue(*p, now, app.uiFrame)) return true;
    return false;
}

// ---- tree mode: expand / collapse one node ------------------------------------
// Expanding is LAZY and asynchronous: the LIST goes to the browse worker, and
// the node shows "(listing...)" until the answer lands in the cache. Collapsing
// keeps the cache, so opening the same node again costs nothing at all - which
// is the whole reason a tree is usable over ssh.
bool rbHas(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
void rbTreeExpand(App::BrowseInstance& I, const std::string& dir) {
    if (!rbHas(I.expanded, dir)) I.expanded.push_back(dir);
    if (I.treeCache.count(dir) || rbHas(I.treePending, dir)) return;
    I.treePending.push_back(dir);
    I.treeLists++;
    App::RbJob j;
    j.kind = App::RbTreeList;
    j.host = I.b.host;
    j.port = I.b.port;
    j.dir = dir;
    rbEnqueue(I, std::move(j));
}
void rbTreeCollapse(App::BrowseInstance& I, const std::string& dir) {
    I.expanded.erase(std::remove(I.expanded.begin(), I.expanded.end(), dir),
                     I.expanded.end());
}
// A different machine, or "list this again": the cached children are stale.
void rbTreeForget(App::BrowseInstance& I) {
    I.treeCache.clear();
    I.expanded.clear();
    I.treePending.clear();
}

// ---- browse navigation history (mouse back/forward, Alt+Left / Alt+Right) ----
// Only B.dir changes recorded through remoteBrowseTo are entries: tree
// expansions and the search-results view are not places. Back/forward restore
// the LOCATION only - no preview, no selection replay. Per instance
// (BrowseInstance::histBack / histFwd), so each panel walks its own past.
static void rbHistOnNavigate(App::BrowseInstance& I, const std::string& toDir) {
    if (!I.b.connected) return;
    // a different machine's directories are not this history's past
    std::string key = I.b.host + ":" + std::to_string(I.b.port);
    if (key != I.histKey) { I.histBack.clear(); I.histFwd.clear(); I.histKey = key; }
    if (I.histNav || toDir == I.b.dir) return;
    I.histBack.push_back(I.b.dir);
    if (I.histBack.size() > 64) I.histBack.erase(I.histBack.begin());
    I.histFwd.clear();               // a fresh navigation truncates the forward branch
}
void rbHistGo(App::BrowseInstance& I, bool back) {
    std::vector<std::string>& from = back ? I.histBack : I.histFwd;
    std::vector<std::string>& to   = back ? I.histFwd  : I.histBack;
    if (from.empty() || !I.b.connected) return;
    std::string dest = from.back();
    from.pop_back();
    to.push_back(I.b.dir);
    I.histNav = true;
    remoteBrowseTo(I, dest);
    I.histNav = false;
}

// Browse one directory on the connected server (async: a dead link hangs the
// worker, never the window).
void remoteBrowseTo(App::BrowseInstance& I, const std::string& dir) {
    // Going anywhere leaves the search results: they stand in for the listing,
    // so a listing that arrives underneath them cannot be seen. Every way of
    // navigating funnels through here, which is why the rule lives here and
    // not on each of the six things that move.
    I.search.active = false;
    // ...and every way of navigating is one funnel, which is also why the
    // back/forward history is recorded here and nowhere else. Only a change
    // of B.dir is an entry - tree expansion and the search view are not.
    rbHistOnNavigate(I, dir);
    App::RbJob j;
    j.kind = App::RbList;
    j.host = I.b.host;
    j.port = I.b.port;
    j.dir = dir;
    rbEnqueue(I, std::move(j));
}

// One canonical url for "this directory on this host", used by the places list
// (bookmarks / recents). It IS makeRemoteUrl now: the two differed only in
// whether they kept the port, and "the one that keeps it" is the only correct
// answer, so there is one function and no way to pick the lossy one by mistake.
std::string placeUrl(const std::string& host, int port, const std::string& path) {
    return makeRemoteUrl(host, path, port);
}

// Navigate to a places url. Same host and a live session: just browse there.
// Anything else goes through the normal async connect - never a handshake on
// the UI thread.
void goToPlace(App::BrowseInstance& I, const std::string& url) {
    std::string host, path;
    int port = 0;
    if (!remote::parseUrl(url, host, path, &port)) {
        g_browseHost.toast("cannot parse place: " + url, true);
        return;
    }
    if (I.b.connected && I.b.host == host && I.b.port == port) {
        remoteBrowseTo(I, path);
        return;
    }
    I.b = App::RemoteBrowse{};
    I.b.host = host;
    I.b.port = port;
    App::RbJob j;
    j.kind = App::RbConnect;
    j.host = host;
    j.port = port;
    j.dir = path;
    rbEnqueue(I, std::move(j));
}

// → P7 の継ぎ目: §3「session の browse place 復元」— viewer→browse の入口。
// One session "rbplace <num> <url>" line, applied: put that instance back on
// its place and reconnect it through ITS OWN worker (item 10, decision A5 -
// auto-reconnect, asynchronously; the UI never blocks on ssh, and a failure
// lands inline in that instance's error band). Shared by the session reader
// and the keys selftest's round-trip op, so the two cannot drift apart.
void sessionRestoreBrowsePlace(int num, const std::string& url) {
    if (url.empty()) return;
    App::BrowseInstance* I = num <= 1 ? &rbMain() : rbFindNum(num);
    if (!I) {
        I = &rbNewInstance(num);
        I->focusReq = false;          // a restore is not a focus request
    }
    goToPlace(*I, url);
}

// Kick a server-side recursive find; the result lands in I.search through
// the instance's worker. Any previous search is superseded by the gen bump.
void remoteStartSearch(App::BrowseInstance& I,
                              const std::string& root, const std::string& pattern) {
    App::RemoteSearch& S = I.search;
    S.gen++;
    S.running = true;
    S.active = true;
    S.root = root;
    S.pattern = pattern;
    S.hits.clear();
    S.truncated = false;
    S.skippedDirs = 0;
    App::RbJob j;
    j.kind = App::RbGlob;
    j.host = I.b.host;
    j.port = I.b.port;
    j.dir = root;
    j.pattern = pattern;
    j.gen = S.gen;
    rbEnqueue(I, std::move(j));
}

// The remote openFolder(): the worker asks the server to walk the subtree and
// report every stack below it; the result feeds the picker / open queue on
// the UI thread (pumpRemoteBrowse).
//
// #148 - EXCEPT when the folder is on this machine. An empty host means the
// files are on this disk, and this process reads png/tif/exr/RAW; the peer
// reads .npy and nothing else (core/serve.cpp's walk collects isNpySuffix, and
// npySegKey refuses the rest on its first line). Sending a local folder round
// through it made the SAME folder answer differently depending on which door
// opened it: `viewer <dir>`, a dropped folder and the file dialog grouped the
// pictures, and File ▸ Open Folder (all stacks below)… / every Browse "Open
// folder (all stacks below)" said "no .npy stacks under …" and opened nothing,
// with the listing right beside it showing those very files (LIST has no
// extension filter - only the meta peek is npy-gated). serve.cpp:533-538 is our
// own rule against precisely that: the same folder must not read one way over
// ssh and another way opened from disk.
//
// So the local case goes to openFolder(), which is the ONE path a drop already
// takes - scanFolderGroups (it asks core/imagefile.h, so a format the table
// gains is gained here with it) into the SAME picker. The picker is not forked;
// only the scan differs, and only in which extensions it can see.
//
// The REMOTE case is unchanged and stays npy-only on purpose: over ssh that is
// a declared refusal the panel already explains (peerRefusalFor), not an
// oversight. Whether the peer should serve more than .npy is #148's open
// question and is not answered here.
//
// HOW FAR DOWN both walks reach is one number, and it is a setting (#204,
// ruled 2026-08-17). #148 handed this file a discrepancy it could not settle
// alone: the peer was asked for depth 6 and scanFolderGroups walked 3, so a
// tree four levels deep read one way over ssh and another way off this disk -
// the very thing the paragraph above says must not happen, in the one dimension
// the extension fix did not cover. The answer is not a literal agreed between
// two call sites (that is what drifted): both doors ask scanDepthBelow()
// (core/app/state.h), which answers loading.folderScanDepth, default 6. 6
// because it is the depth that has already shipped through this door, so the
// ruling narrows nobody's reach; the operator with a deeper capture tree - or a
// shallower one they want scanned faster - now says so in settings.jsonc or in
// Preferences instead of asking for a code change. Both walks still cap at 256
// groups and both still report the cap.
void remoteScanFolder(App::BrowseInstance& I, const std::string& root) {
    if (I.b.host.empty()) {
        // Bump the token first: an earlier peer scan still in flight would
        // otherwise land on top of the picker this is about to raise, and
        // openPickerWith does not merely re-raise the modal - it CLEARS the
        // selection (the reason scanGen exists at all).
        ++I.b.scanGen;
        g_browseHost.openFolder(root);
        return;
    }
    App::RbJob j;
    j.kind = App::RbScan;
    j.host = I.b.host;
    j.port = I.b.port;
    j.dir = root;
    j.gen = ++I.b.scanGen;            // cancel token, exactly as RbGlob has
    rbEnqueue(I, std::move(j));
}

// Chained opens from a folder scan: the next stack starts only when the remote
// fetcher is idle, so the memory budget openRemoteStack applies reflects what
// the previous stack actually loaded.
void pumpRemoteOpenQueue() {
    if (app.rbOpenQueue.empty() || app.rfPending > 0 || app.seqRunning) return;
    App::RemoteOpen ro = std::move(app.rbOpenQueue.front());
    app.rbOpenQueue.erase(app.rbOpenQueue.begin());
    sortFramesNumerically(ro.files);
    app.loadBatchId = ro.batchId;
    g_browseHost.openRemoteStack(ro.host, ro.files, ro.name, ro.port, ro.token);
    app.loadBatchId = 0;
}

// File > Start Remote: connect (installing the peer if needed), then browse -
// in the given instance. Everything slow happens on the instance's worker;
// this returns immediately.
// Where a spec points, without connecting to it. startRemote used to be the only
// thing that knew, which is why the caller could not tell whether it was about to
// replace the panel the user was reading.
static void rbParseSpec(const std::string& hostSpec, std::string& host, int& port,
                        std::string& dir) {
    host = hostSpec; dir = "~"; port = 0;
    // accept a full url here too, so a pasted path still works
    if (hostSpec.find("://") != std::string::npos || hostSpec.find(':') != std::string::npos) {
        std::string h, p;
        if (remote::parseUrl(hostSpec, h, p, &port)) { host = h; dir = p; }
    }
    while (host.size() > 1 && host.back() == '/') host.pop_back();
}

// Which panel a new place should land in. Connecting somewhere else must NOT
// take over the panel already showing something: Browse is instanced exactly so
// a local listing and a machine can sit side by side, and startRemote clears the
// instance it is given, so reusing the active one made the local view disappear
// - which reads as the panel having closed (user report, 2026-08-04).
//
// Reuse when there is nothing to lose (never connected) or when it is the same
// place (reconnect, not a second window). Otherwise: a panel already on that
// place if there is one, else a new one.
App::BrowseInstance& rbInstanceFor(const std::string& hostSpec) {
    std::string host, dir;
    int port = 0;
    rbParseSpec(hostSpec, host, port, dir);
    App::BrowseInstance& cur = rbActive();
    if (!cur.b.connected && cur.b.host.empty() && cur.b.dir == "~") return cur;
    if (cur.b.host == host && cur.b.port == port) return cur;
    for (auto& p : app.browsePanels)
        if (p->b.connected && p->b.host == host && p->b.port == port) return *p;
    return rbNewInstance();
}

void startRemote(App::BrowseInstance& I, const std::string& hostSpec) {
    std::string host, dir;
    int port = 0;
    rbParseSpec(hostSpec, host, port, dir);
    // A pasted FILE path opens; a pasted folder is browsed. #111: the question
    // is "does this text name a file this viewer can open", which is the format
    // table's - a pasted shot.exr used to be taken for a directory and browsed,
    // and the listing of a file is empty.
    if (dir.size() > 4 && viewerReadsName(dir)) {
        size_t s = dir.find_last_of('/');
        std::string parent = s == std::string::npos ? "~" : dir.substr(0, s);
        I.b = App::RemoteBrowse{};
        I.b.host = host;
        I.b.port = port;
        App::RbJob j;
        j.kind = App::RbConnect; j.host = host; j.port = port; j.dir = parent;
        rbEnqueue(I, std::move(j));
        // opened once the worker has the session; opening it here would put the
        // ssh handshake back on the UI thread
        I.pendingOpen = makeRemoteUrl(host, dir, port);
        return;
    }
    I.b = App::RemoteBrowse{};
    I.b.host = host;
    I.b.port = port;
    App::RbJob j;
    j.kind = App::RbConnect;
    j.host = host;
    j.port = port;
    j.dir = dir;
    rbEnqueue(I, std::move(j));
}
