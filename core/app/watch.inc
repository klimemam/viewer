// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---- Watch: the source files, on disk (docs/watch-design.md 項目20) -----------
// core/watch.h holds the DECISION (what "changed" means, §1/§4). This file holds
// the three things that decision needs and it does not: where to look, when to
// look, and who is told.
//
// The division of labour across threads is the same one rfWorker / mWorker keep:
//
//   UI thread     builds the target list out of the open stacks (watchTargetsNow),
//                 integrates findings (pumpWatch), draws the line (§5).
//   watchWorker   sleeps on its own timer, lists directories, calls watch::observe,
//                 and posts ONE empty event when - and only when - a finding
//                 changed (§3). It touches no app state except the two vectors
//                 under app.watchMtx, and it never holds a SeqInfo*.
//
// §3 is a hard requirement and not a preference: this program idles at 0 fps,
// and a poller that woke the UI to decide whether to poll would turn that into a
// 0.2 fps spin forever. Hence: the worker's timer is its own, the wake is
// conditional on a finding, and watchFindingsPending() joins the idle-skip chain
// so a finding that HAS landed is never left sitting in the queue until the
// mouse happens to move (the remote scan's picker, twice).

// The on-disk file behind a source, or "" when there is none to watch. local://
// counts as local for the reason reloadUnavailable gives: a Browse-panel local
// open carries the path inside its url, and those pixels really are on this
// disk. A peer's pixels, a montage and a plugin result have no file here.
static std::string watchLocalPathOf(const FrameSource& s) {
    if (!s.remoteUrl.empty())
        return s.remoteUrl.rfind("local://", 0) == 0 ? s.remoteUrl.substr(8) : std::string();
    return s.path;
}

// The open stacks, as watched sets. UI thread only: it reads app.seqs and
// app.images.
//
// What is NOT here, and why: loose frames (§9 - "single files are covered by
// manual Reload"), remote stacks (§1's remote half needs a LIST on the worker's
// own Session and is the next stage), and any stack whose members have no local
// file at all. A stack that cannot be watched gets no line rather than an empty
// one - saying nothing is the honest output for "not looked at".
static std::vector<App::WatchTarget> watchTargetsNow() {
    std::vector<App::WatchTarget> out;
    for (const App::SeqInfo& si : app.seqs) {
        if (!si.remoteHost.empty() || !si.remoteUrl.empty()) continue;   // stage 6
        App::WatchTarget t;
        t.seqId = si.id;
        // The members' files. A frame-axis stack has one path repeated F times;
        // a folder stack has one file per frame. Both are answered by collecting
        // the DISTINCT paths and looking at how many there are.
        std::vector<std::string> paths;
        std::unordered_set<std::string> seen;   // a 300-frame stack makes the
                                                // linear scan this replaces
                                                // 45,000 string compares
        watch::Obs seed;
        bool seedComplete = true;
        for (int idx : framesOfSeq(si.id)) {
            const FrameSource& s = *app.images[idx]->src;
            std::string p = watchLocalPathOf(s);
            if (p.empty()) { seedComplete = false; continue; }
            if (!seen.insert(p).second) continue;
            paths.push_back(p);
            // §1: the baseline the loader already recorded. 0/0 means the stat
            // never happened, and a set with ONE unknown member has no baseline
            // at all - a half-seeded baseline would report the seeded half's
            // changes and stay blind to the rest, which is worse than waiting a
            // poll.
            if (s.mtime == 0 && s.fsize == 0) seedComplete = false;
            seed.push_back({ baseName(p), s.mtime, s.fsize });
        }
        if (paths.empty()) continue;
        if (seedComplete) { watch::normalize(seed); t.seed = std::move(seed); }
        if (paths.size() == 1) {
            // One file. Either a frame-axis stack (F frames in one .npy) or a
            // folder stack that has loaded exactly one member so far; both are
            // watched as that file, and if the folder later grows the stack's
            // own membership is what would have to change, not this set.
            t.file = paths[0];
        } else {
            std::filesystem::path p0 = pathFromUtf8(paths[0]);
            t.dir = p0.parent_path().u8string();
            t.headName = p0.filename().u8string();
            if (t.dir.empty()) continue;
        }
        out.push_back(std::move(t));
    }
    return out;
}

// One directory, listed once (§2: however many stacks live in it). Names only -
// the members' (mtime,size) come from the same directory entry, which is where
// the operating system already has them.
static watch::Obs watchScanDir(const std::string& dir) {
    watch::Obs o;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        watch::Ent en;
        en.name = e.path().filename().u8string();
        std::error_code ec3;
        auto t = std::filesystem::last_write_time(e.path(), ec3);
        en.mtime = ec3 ? 0 : (int64_t)t.time_since_epoch().count();
        auto sz = std::filesystem::file_size(e.path(), ec3);
        en.size = ec3 ? 0 : (uint64_t)sz;
        o.push_back(std::move(en));
    }
    watch::normalize(o);
    return o;
}

// ONE POLL ROUND, as a function of what it is handed.
//
// This is the extraction §10 needs and the reason --watch-selftest is fast: the
// worker calls this on its own timer, the selftest calls it four times in a row,
// and neither of them can disagree about what a poll DOES because there is one
// of it. Nothing in here reads a clock or a thread.
//
// `states` is the worker's memory across rounds, keyed by seqId. Stacks that
// have left `targets` are forgotten here - a stack closed and a stack whose
// files are gone are different things, and only the first should lose its
// baseline.
//
// Returns true when at least one finding CHANGED, which is exactly the
// condition under which the UI is woken.
static bool watchPollRound(const std::vector<App::WatchTarget>& targets,
                           std::map<int, watch::SetState>& states,
                           std::vector<App::WatchDone>& out) {
    // §2's dedupe: one listing per DISTINCT directory, not one per stack.
    std::map<std::string, watch::Obs> listed;
    bool changed = false;
    for (const App::WatchTarget& t : targets) {
        watch::SetState& st = states[t.seqId];
        // §1: the baseline is FrameSource's own stat, and it MOVES when a
        // Reload re-reads the files (reloadSource re-stats and rewrites
        // mtime/fsize). That is the whole mechanism by which the amber line
        // goes away after a Reload, and why the worker needs no message from
        // the UI to make it go: the answer to "does the disk differ from these
        // pixels?" became no, so the set has nothing to say again.
        //
        // A seed that has NOT moved is not re-adopted, so a file changing on
        // disk never disturbs the baseline - which is the point of having one.
        if (!t.seed.empty() && (!st.haveBase || !watch::sameSet(st.base, t.seed)))
            watch::seedBaseline(st, t.seed);   // §4 starts over against it
        watch::Obs obs;
        if (!t.dir.empty()) {
            auto it = listed.find(t.dir);
            if (it == listed.end())
                it = listed.emplace(t.dir, watchScanDir(t.dir)).first;
            // The stack's membership, re-derived with the SAME rule the open
            // used (§1), over the listing already in hand.
            std::vector<std::string> names;
            names.reserve(it->second.size());
            for (const watch::Ent& e : it->second) names.push_back(e.name);
            std::string pat;
            std::vector<std::string> mem = siblingNamesAmong(t.headName, names, pat);
            // The head itself always belongs to its own stack. siblingNamesAmong
            // wants at least two names that share a structure, so a folder whose
            // other members have just been deleted answers "" - and the set
            // would silently become empty, which reads as "nothing to say"
            // rather than "they are gone".
            if (mem.empty()) mem.push_back(t.headName);
            for (const std::string& n : mem)
                if (const watch::Ent* e = watch::find(it->second, n)) obs.push_back(*e);
        } else {
            std::error_code ec;
            auto p = pathFromUtf8(t.file);
            if (std::filesystem::is_regular_file(p, ec)) {
                watch::Ent en;
                en.name = p.filename().u8string();
                std::error_code ec2;
                auto tm = std::filesystem::last_write_time(p, ec2);
                en.mtime = ec2 ? 0 : (int64_t)tm.time_since_epoch().count();
                auto sz = std::filesystem::file_size(p, ec2);
                en.size = ec2 ? 0 : (uint64_t)sz;
                obs.push_back(std::move(en));
            }
        }
        if (watch::observe(st, std::move(obs))) {
            out.push_back({ t.seqId, st.found });
            changed = true;
        }
    }
    // forget the sets nobody is watching any more
    for (auto it = states.begin(); it != states.end();) {
        bool live = false;
        for (const App::WatchTarget& t : targets) if (t.seqId == it->first) { live = true; break; }
        it = live ? std::next(it) : states.erase(it);
    }
    return changed;
}

static void watchWorker() {
    std::map<int, watch::SetState> states;
    while (!app.watchStop) {
        std::vector<App::WatchTarget> targets;
        if (!app.watchPaused) {        // §2: not while the window is minimised
            std::lock_guard<std::mutex> lk(app.watchMtx);
            targets = app.watchTargets;
        }
        std::vector<App::WatchDone> found;
        // An empty target list is a no-op round and NOT a re-baselining one:
        // watchPollRound would forget every state it holds. Minimised must cost
        // the baselines nothing - a window restored after an hour has to say
        // what changed during it, not start over.
        const bool any = !targets.empty() && watchPollRound(targets, states, found);
        if (any) {
            {
                std::lock_guard<std::mutex> lk(app.watchMtx);
                for (auto& d : found) {
                    // one row per stack: the latest statement replaces the last
                    bool merged = false;
                    for (auto& e : app.watchDone)
                        if (e.seqId == d.seqId) { e = d; merged = true; break; }
                    if (!merged) app.watchDone.push_back(std::move(d));
                }
            }
            // §3: the ONLY wake this feature performs, and it happens because
            // something is TRUE, never because a poll came round. The loader's
            // own wake (glfwPostEmptyEvent on a decoded frame) is the precedent.
            glfwPostEmptyEvent();
        }
        std::unique_lock<std::mutex> lk(app.watchMtx);
        app.watchCv.wait_for(lk, std::chrono::duration<double>(app.watchIntervalSec),
                             [] { return app.watchStop.load(); });
    }
}

static void startWatchWorker() {
    if (app.watchThread.joinable()) return;
    app.watchStop = false;
    app.watchThread = std::thread(watchWorker);
}
static void stopWatchWorker() {
    {
        std::lock_guard<std::mutex> lk(app.watchMtx);
        app.watchStop = true;
    }
    app.watchCv.notify_all();
    if (app.watchThread.joinable()) app.watchThread.join();
    app.watchStop = false;
}

// Findings a poll has produced that no frame has integrated yet. This is the
// `working |=` term in the idle-skip chain (§3): the pumps live inside the
// frame, so a finding that landed while the app was idle would otherwise sit in
// the queue until the user happened to move the mouse.
static bool watchFindingsPending() {
    std::lock_guard<std::mutex> lk(app.watchMtx);
    return !app.watchDone.empty();
}

// The SENTENCE, and it is THE guard: the Files panel draws this string and
// --watch-selftest asserts this string, so there is no second opinion to drift
// (seqReloadNote's discipline). "" = this stack has nothing to say.
static std::string seqWatchNote(int seqId) {
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) return {};
    return watch::findingText(si->watchFound);
}

// What a finding DOES once it reaches the UI thread: the summary lands on the
// stack, and the one toast fires. Split out of pumpWatch so --watch-selftest can
// drive the poll itself and still go through the one function that puts a
// finding on a stack - the alternative was a test that starts the worker thread
// and then races it for the queue.
static void watchIntegrate() {
    std::vector<App::WatchDone> batch;
    {
        std::lock_guard<std::mutex> lk(app.watchMtx);
        batch.swap(app.watchDone);
    }
    for (const App::WatchDone& d : batch) {
        App::SeqInfo* si = seqInfo(d.seqId);
        if (!si) continue;                       // closed while the poll ran
        si->watchFound = d.found;
        if (!d.found.any()) {
            // The files were put back: the statement is false again, and the
            // next change deserves its own toast.
            si->watchToasted = false;
            continue;
        }
        if (si->watchToasted) continue;
        si->watchToasted = true;
        // §5: one toast, and the line stays. A toast expires; "3 file(s)
        // changed on disk" must not (seqNote's precedent).
        toast(si->name + ": " + watch::findingText(d.found) +
              " - Reload in Files to re-read them");
    }
}

// The pump chain's member (pumpSequenceAndQueue / pumpRemoteFetch / pumpMeasure
// / ...): republish what to watch, make sure the worker exists, integrate.
static void pumpWatch() {
    if (!app.watchEnabled) {
        // Turned off while it was running: the worker goes away rather than
        // polling for a line nobody will draw. notify_all wakes it out of its
        // interval at once, so the join is not a stall.
        if (app.watchThread.joinable()) stopWatchWorker();
        return;
    }
    {   // What to watch, refreshed from the open stacks. It walks the stacks and
        // not the pixels, but pumpWatch runs once per FRAME and a frame can be
        // 120 Hz - so it is rebuilt on the three events that can change it and
        // never merely because a frame happened.
        //
        // The third event is a RELOAD, and it is not covered by either of the
        // first two: FrameSource's mtime/fsize move, the stack table's revision
        // and the image count do not, and re-publishing that seed is the only
        // thing that tells the worker its baseline moved. A time floor would not
        // do instead - the app idles at 0 fps, so "in two seconds" can mean
        // never, and the line the user just cleared with Reload would come back
        // at the next poll. reloadSource sets the flag; here it is spent.
        static unsigned long long lastRev = ~0ull;
        static size_t lastImgs = (size_t)-1;
        if (app.watchTargetsDirty || app.seqs.rev() != lastRev ||
            app.images.size() != lastImgs) {
            app.watchTargetsDirty = false;
            lastRev = app.seqs.rev();
            lastImgs = app.images.size();
            std::vector<App::WatchTarget> t = watchTargetsNow();
            std::lock_guard<std::mutex> lk(app.watchMtx);
            app.watchTargets = std::move(t);
        }
    }
    if (!app.watchThread.joinable()) startWatchWorker();
    watchIntegrate();
}
