// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---- Watch: the source files, on disk (docs/features/watch/watch-design.md 項目20) -----------
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
//
// The url half is localDiskPathOfUrl (state.h), which is the same question
// asked through remote::parseUrl's "empty host means this machine". It used to
// be spelled out here; §6.2's identity key needs the identical answer, and two
// spellings of one predicate differ exactly once, in the place nobody reads.
static std::string watchLocalPathOf(const FrameSource& s) {
    if (!s.remoteUrl.empty()) return localDiskPathOfUrl(s.remoteUrl);
    return s.path;
}

// Is this stack the PEER's?
//
// It declares a remote origin (remoteFiles for a folder stack, remoteUrl for a
// frame-axis one) AND not one of its frames has a file on this disk behind it.
//
// The second half is what keeps a local:// stack out of the remote path, and it
// is reloadUnavailable's own line restated: a Browse-panel local open carries
// the on-disk path inside its url, so those frames really are stat-able from
// here. The LOCAL half then watches them PER FILE, which is strictly more than a
// group row can say - a stack must not lose that precision merely because it was
// opened through the browser. A real peer's frames answer "" to
// watchLocalPathOf, and a stack whose frames are not resident at all (the memory
// budget fetched four of three hundred) answers it for every one of them, which
// is the same answer for the same reason.
static bool watchIsPeerStack(const App::SeqInfo& si) {
    if (si.remoteFiles.empty() && si.remoteUrl.empty()) return false;
    for (int idx : framesOfSeq(si.id))
        if (!watchLocalPathOf(*app.images[idx]->src).empty()) return false;
    return true;
}

// (remoteDirOf - "which folder is this PEER path in" - moved to
// open_dispatch.inc: §16's membership rebuild needs it and that fragment is
// included first. Same function, one spelling.)

// The open stacks, as watched sets. UI thread only: it reads app.seqs and
// app.images.
//
// What is NOT here, and why: loose frames (§9 - "single files are covered by
// manual Reload"), and any stack whose members have neither a local file nor a
// peer to ask. A stack that cannot be watched gets no line rather than an empty
// one - saying nothing is the honest output for "not looked at".
static std::vector<App::WatchTarget> watchTargetsNow() {
    std::vector<App::WatchTarget> out;
    for (const App::SeqInfo& si : app.seqs) {
        if (watchIsPeerStack(si)) {
            // ---- §1's remote bullet: ONE LIST decides this whole stack -------
            App::WatchTarget t;
            t.seqId = si.id;
            t.peer = true;
            t.host = si.remoteHost;
            t.port = si.remotePort;
            // What the SENTENCE calls it, frozen here: watch.h builds that
            // sentence and the worker carries it, and neither may reach into the
            // app to ask what an empty host is called on screen.
            t.hostLabel = peerLabel(si.remoteHost);
            // ...and HOW the peer is invoked, frozen here too, for rfEnqueue's
            // reason: the UI thread edits app.remoteExe freely and the worker
            // must not read it mid-round.
            t.exe = app.remoteExe.empty()
                        ? (si.remoteHost.empty() ? app.exePath
                                                 : std::string(REMOTE_HOME) + "/viewer-serve")
                        : app.remoteExe;
            std::vector<std::string> files = si.remoteFiles;
            if (files.empty() && !si.remoteUrl.empty()) {
                // A frame-axis stack: ONE file on the peer holding F frames.
                // §1 says to look at that one file's row, and that is what the
                // member list of one does.
                std::string h, rpath;
                int prt = 0;
                if (remote::parseUrl(si.remoteUrl, h, rpath, &prt)) files.push_back(rpath);
            }
            if (files.empty()) continue;
            t.dir = remoteDirOf(files[0]);
            if (t.dir.empty()) continue;
            // ONE directory, or this stack is not watched - W14c's rule, and it
            // bites harder here: one LIST speaks about one directory, so members
            // living elsewhere would be absent from every observation and §4
            // would confirm them, correctly by its own lights, as VANISHED.
            bool oneDir = true;
            for (const std::string& f : files) {
                if (remoteDirOf(f) != t.dir) { oneDir = false; break; }
                t.members.push_back(baseName(f));
            }
            if (!oneDir) continue;
            std::sort(t.members.begin(), t.members.end());
            t.members.erase(std::unique(t.members.begin(), t.members.end()),
                            t.members.end());
            // §1's "no baseline": NO SEED, ever. A remote open records no
            // (mtime, size) at all - an ssh:// url stays 0/0 on purpose, because
            // a peer's disk cannot be statted from here (statLocalUrl says so) -
            // so the FIRST poll makes the baseline and announces nothing. That
            // is not a gap to fill later: forging "the state at the moment it
            // was opened" is exactly what §1 forbids.
            out.push_back(std::move(t));
            continue;
        }
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
            // ONE directory, or this stack is not watched. The whole mechanism
            // is "re-run the sibling rule over this folder's listing" (§1), and
            // that rule can only speak about one folder: members living
            // elsewhere would simply be absent from every observation, which §4
            // would confirm - correctly, by its own lights - as VANISHED. A
            // false sentence about a stack whose files are all present is worse
            // than no sentence, so the stack is skipped and says nothing.
            bool oneDir = true;
            for (size_t k = 1; k < paths.size() && oneDir; k++)
                oneDir = pathFromUtf8(paths[k]).parent_path().u8string() == t.dir;
            if (!oneDir) continue;
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

// The worker's memory of one watched set: §4's state machine, plus the one
// thing that is decided ONCE and never revisited.
struct WatchSet {
    watch::SetState st;
    // Is this stack its folder's WHOLE group?
    //
    // A derived stack (Files > "Derive stack (a subset, by reference)") is a
    // chosen subset of one, and it shares its source stack's directory. Telling
    // it that the frames its own rule deliberately left out have "appeared"
    // would be a false statement about that stack for as long as it is open -
    // and §6 is explicit that a derive's membership is a decision made at derive
    // time and is never re-applied. So a stack that is not the whole group is
    // watched over its OWN members: what changed and what is gone are still true
    // of it, and what its rule excluded is not its business.
    //
    // Decided at the moment the baseline is taken, and never again: a folder
    // that GAINS a file must not thereby stop being the group it was, which is
    // what re-deciding every poll would conclude. A file that appears in the gap
    // between the stack opening and the first poll therefore makes this stack
    // look like a subset and costs it appearance reports - a bounded miss that
    // fails towards saying nothing, never towards saying something untrue.
    bool wholeGroup = true;
    bool decided = false;
    // What a PEER's stack last SAID (watch::remoteFinding's output). The raw
    // finding is not the statement here: the conversion drops `changed`'s count,
    // so two raw findings differing only in a number the sentence never prints
    // would otherwise cost the UI a wake for a line that did not move (§3).
    watch::Finding saidRemote;
};

// One reading of a peer's directory, and whether there WAS one. `ok == false`
// is not an empty directory: see WatchLister.
struct WatchListing {
    bool ok = false;
    std::vector<remote::Entry> rows;
};

// HOW a peer's directory is listed, as a function the round is HANDED (§1's
// remote bullet: no new protocol, one LIST answers a whole stack).
//
// A callback and not a Session inside watchPollRound, for the reason
// watchPollRound itself exists: the round must stay drivable by hand. The worker
// passes a lambda over ITS OWN Session (§3's "one Session, one owning thread");
// --rwatch-selftest passes one over a Session of its own and counts the calls,
// which is how §2's "one LIST per (host, port, dir)" becomes an assertion rather
// than a claim; and a lister that ANSWERS FALSE is how "the link was down" is
// tested without unplugging anything.
//
// Returning false means NO READING WAS TAKEN - not "the directory is empty".
// The distinction is the whole reason this returns a bool at all: an
// unreachable peer whose silence was folded into an empty listing would have
// §4 confirm, correctly by its own lights, that every file of the stack is
// gone.
using WatchLister = std::function<bool(const App::WatchTarget& t,
                                       std::vector<remote::Entry>& rows)>;

// §2's two intervals as ONE decision. The worker's timer is the LOCAL interval
// (5 s), so a peer is asked every Nth round - and N is computed from the two
// constants rather than typed, so moving either one cannot silently leave the
// peer polled at the local rate. 5 s and 15 s give 3.
static int watchRemoteEvery() {
    const double a = app.watchIntervalSec, b = app.watchRemoteIntervalSec;
    if (a <= 0.0 || b <= a) return 1;
    return (int)std::ceil(b / a - 1e-9);
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
                           std::map<int, WatchSet>& states,
                           std::vector<App::WatchDone>& out,
                           const WatchLister& lister = WatchLister(),
                           bool doRemote = true) {
    // §2's dedupe: one listing per DISTINCT directory, not one per stack.
    std::map<std::string, watch::Obs> listed;
    // ...and the same rule over the link, keyed on (host, port, dir): three
    // stacks open from one folder of one peer is ONE request. This is what makes
    // §2's cost ceiling - "watched dirs x one LIST per interval" - a ceiling.
    std::map<std::string, WatchListing> peerListed;
    bool changed = false;
    for (const App::WatchTarget& t : targets) {
        // §2: a peer is asked on ITS interval (15 s), not the worker's timer
        // (5 s). The worker decides whose round this is; the round itself reads
        // no clock. BEFORE `states[...]`, so a round that is not the peer's
        // costs nothing at all - not even a default-constructed set - and its
        // baseline, which lives in `states` from an earlier round, is untouched.
        if (t.peer && !doRemote) continue;
        WatchSet& ws = states[t.seqId];
        watch::SetState& st = ws.st;
        if (t.peer) {
            const std::string key = t.host + "\n" + std::to_string(t.port) + "\n" + t.dir;
            auto pit = peerListed.find(key);
            if (pit == peerListed.end()) {
                WatchListing L;
                L.ok = lister && lister(t, L.rows);
                pit = peerListed.emplace(key, std::move(L)).first;
            }
            // A LIST THAT DID NOT ANSWER IS NOT A READING. An unreachable peer,
            // a dropped ssh link and a refused path all arrive as "no reply";
            // folded into an empty listing they would have §4 confirm - quite
            // correctly by its own lights - that every file of the stack is
            // GONE, and the screen would announce that a capture was deleted
            // because a network cable was. The round is skipped for this stack,
            // its baseline and its candidate untouched, so the poll after the
            // link returns reads today's disk against the same baseline.
            if (!pit->second.ok) continue;
            // The rows that SPEAK FOR this stack's members. A group row speaks
            // for every member it lists and speaks for them TOGETHER (one pair
            // of numbers for the set); a plain row speaks for itself, and that
            // one is per-member truth - so a frame-axis file, or a folder with a
            // single capture in it, is watched exactly as precisely as a local
            // stat watches it. Hedging a reading that IS exact would be its own
            // dishonesty, in the other direction.
            uint64_t bytes = 0;
            int64_t mtime = 0;
            bool aggregated = false;
            std::vector<std::string> names;
            for (const remote::Entry& e : pit->second.rows) {
                if (e.dir) continue;
                bool mine = false;
                if (e.group) {
                    for (const std::string& m : e.members)
                        if (std::binary_search(t.members.begin(), t.members.end(), m)) {
                            mine = true;
                            break;
                        }
                } else {
                    mine = std::binary_search(t.members.begin(), t.members.end(), e.name);
                }
                if (!mine) continue;
                bytes += e.size;
                mtime = std::max(mtime, e.mtime);
                if (e.group) {
                    aggregated = true;
                    for (const std::string& m : e.members) names.push_back(m);
                } else {
                    names.push_back(e.name);
                }
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            // An EMPTY reading is not a decision (R15). A folder that holds none
            // of this stack's members at the instant of the baseline - not yet
            // filled, or cleared between the open and the first poll - carries
            // no names to compare, and answering "not the whole group" from
            // that would put the stack permanently on mute: the question below
            // is settled ONCE, so the files arriving a second later would never
            // be spoken about again. Deferring costs the stack nothing: the
            // baseline is taken from the empty reading either way, and the
            // files that then arrive are an ARRIVAL, which is what they are.
            if (!ws.decided && !names.empty()) {
                // §11.4 from the other side of the link, and it is STRICTER
                // here. The local half narrows its observation to the stack's
                // own members, because a local scan stats every file
                // separately; a group row cannot be narrowed at all - its bytes
                // and its mtime cover files this stack does not contain, and
                // there is no per-member state to subtract them with. So a
                // stack that is not its group's whole membership (a derived
                // subset, a hand-picked open, one frame-axis file among
                // numbered siblings) says NOTHING. Same failure direction as
                // the local half takes, and the only honest one an aggregate
                // leaves available.
                //
                // Decided ONCE, at the baseline, for the local half's reason: a
                // folder that GAINS a file must not thereby stop being the
                // group it was, which is what re-deciding every poll concludes.
                ws.wholeGroup = (names == t.members);
                ws.decided = true;
            }
            if (!ws.wholeGroup) continue;
            watch::Obs obs = aggregated
                                 ? watch::groupObs(bytes, mtime, std::move(names))
                                 : watch::Obs();
            if (!aggregated)
                for (const remote::Entry& e : pit->second.rows) {
                    if (e.dir || e.group) continue;
                    if (std::binary_search(t.members.begin(), t.members.end(), e.name))
                        obs.push_back({ e.name, e.mtime, e.size });
                }
            watch::observe(st, std::move(obs));
            // The STATEMENT, not the raw finding: what the panel prints is the
            // converted one, and the wake belongs to the sentence changing (§3).
            watch::Finding said;
            if (aggregated) {
                said = watch::remoteFinding(st.found, t.hostLabel, (int)st.conf.size());
            } else {
                // Every row that spoke was a PLAIN row, so every number here is
                // the file's own: the finding is the local one, counts and names
                // included, and the only thing to add is WHERE.
                said = st.found;
                if (said.any()) said.host = t.hostLabel;
            }
            if (said != ws.saidRemote) {
                ws.saidRemote = said;
                out.push_back({ t.seqId, said });
                changed = true;
            }
            continue;
        }
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
            if (!ws.decided && st.haveBase) {   // see WatchSet::wholeGroup
                ws.wholeGroup = true;
                for (const std::string& n : mem)
                    if (!watch::find(st.base, n)) { ws.wholeGroup = false; break; }
                ws.decided = true;
            }
            for (const std::string& n : mem) {
                // a subset stack sees its own members and nothing else
                if (!ws.wholeGroup && st.haveBase && !watch::find(st.base, n)) continue;
                if (const watch::Ent* e = watch::find(it->second, n)) obs.push_back(*e);
            }
            // A member of a SUBSET stack whose file is gone is not in `mem` at
            // all (the rule works off the listing), and its absence from obs is
            // exactly what §4 confirms as vanished - so nothing extra is needed
            // for it. The same is true of the whole-group case.
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
    std::map<int, WatchSet> states;
    // §3's rule, kept the shape rfWorker (remote_client.inc) and mWorker keep:
    // ONE Session, owned by the thread that uses it. It is emphatically NOT the
    // Browse instance's - two threads writing one pipe interleave two requests'
    // frames, and the reply either one then reads is a decode of the other's
    // bytes. Connections are held across rounds and reused, which is what makes
    // "one LIST every 15 s" cost one LIST every 15 s rather than an ssh
    // handshake every 15 s.
    remote::Session ses;
    ses.setAbort(&app.watchStop);      // a blocked read gives up when Quit asks
    std::string sesHost = "\n";        // impossible: force the first connect
    int sesPort = -1;
    long long round = 0;
    while (!app.watchStop) {
        std::vector<App::WatchTarget> targets;
        if (!app.watchPaused) {        // §2: not while the window is minimised
            std::lock_guard<std::mutex> lk(app.watchMtx);
            targets = app.watchTargets;
        }
        std::vector<App::WatchDone> found;
        // §2's second interval. The timer below is the LOCAL one, so a peer
        // gets every Nth round and N comes out of the two constants.
        const bool doRemote = (round % watchRemoteEvery()) == 0;
        bool anyPeer = false;
        for (const App::WatchTarget& t : targets) anyPeer |= t.peer;
        WatchLister lister = [&](const App::WatchTarget& t,
                                 std::vector<remote::Entry>& rows) {
            std::string err;
            ensureSession(ses, sesHost, sesPort, t.host, t.port, t.exe, err);
            if (!err.empty()) return false;
            return ses.list(t.dir, rows, err);
        };
        // An empty target list is a no-op round and NOT a re-baselining one:
        // watchPollRound would forget every state it holds. Minimised must cost
        // the baselines nothing - a window restored after an hour has to say
        // what changed during it, not start over.
        const bool any = !targets.empty() &&
                         watchPollRound(targets, states, found, lister, doRemote);
        round++;
        // Nothing on the peer is being watched any more (the last remote stack
        // was closed, or Watch was paused): let the link go. An idle ssh process
        // per stack somebody closed an hour ago is a real cost on the machine at
        // the other end, and the next remote target reconnects in one round.
        if (!anyPeer && ses.alive()) {
            ses.stop();
            sesHost = "\n";
            sesPort = -1;
        }
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

// May the watch line OFFER [Reload] for this stack? One predicate, asked by the
// panel and by --rwatch-selftest, and it is the same question the stack's
// context menu already asks about the same members (reloadUnavailable) - a
// second transcription of that list is how the menu and this button would come
// to disagree about one stack.
//
// §16 REMOVED a term from it. The peer's origin used to be refused here on top
// of reloadUnavailable's own answer, because §6's rebuild was local-only and an
// offered button would have refused every member and latched #56's mark onto a
// stack where nothing failed. The rebuild reaches the peer now, so the extra
// term is gone and this is once again a question with exactly one source of
// truth: can the walk serve at least one of these members?
static bool watchReloadOffered(int seqId) {
    for (int idx : framesOfSeq(seqId))
        if (reloadUnavailable(*app.images[idx]->src).empty()) return true;
    return false;
}

// WHAT TO DO ABOUT IT, and it is now one sentence for both origins.
//
// The line used to fork on the ORIGIN, because a peer's stack had no [Reload] to
// point at. It forks on the BUTTON instead - the same predicate that draws it -
// so the sentence and the control cannot disagree, which was the point of the
// fork in the first place. A stack the walk cannot serve at all is a
// container/reader member (`__pixels_*`), and reloadUnavailable's own words for
// that one are "the file rebuilds as a unit - re-open it to reload".
static const char* watchActionText(int seqId) {
    return watchReloadOffered(seqId) ? " - Reload in Files to re-read them"
                                     : " - re-open the file to re-read it";
}

// ============================================================================
//  §9: AUTO-RELOAD - the first thing in this feature that MOVES A NUMBER
//  ============================================================================
//
// Everything above only ever SAYS something. This acts, and with nobody's click
// behind it, so the whole of it is arranged around three rules:
//
//   1. DEFAULT OFF, and off means off. The settled semantics are "notify, then a
//      manual Reload" (ruling A6); this is the opt-in and nothing else changes.
//      --watch-selftest W39 drives a confirmed finding past two frames of the
//      drain with the switch untouched and asserts that not a pixel moved.
//
//   2. ONE RELOAD PATH. What runs is reloadStackFromDisk - the very function
//      the [Reload] button calls, with every guard §12 and §13 put in it: the
//      departures are taken before reloadSource so a gone file is not a refusal
//      (§12.1), the rule is the one recorded at the open so a subset stack is
//      not re-grown (§12.2), a member the rule un-names but whose file is there
//      STAYS (§12.3), a named file that will not decode is n/N and not a failure
//      (§12.4), and a rebuild NEVER empties a stack (§12.5). A second reload path
//      for the automatic case would be a second set of those decisions, and the
//      one that is wrong is always the one nobody is looking at.
//
//   3. LOUDER, NEVER QUIETER. The user did not ask, so the evidence has to be
//      MORE than a manual reload leaves, not less: the same toast (which lands
//      in Messages), the same #56 mark, the same n-of-N on the Files row, AND a
//      note latched on the stack that does not expire (SeqInfo::autoReloadNote).
//
// What it deliberately does NOT do is decide anything about WHICH findings are
// worth acting on. "Only reload when files changed, not when they vanished"
// would be a second policy sitting beside §6's, and the two would drift.

// Is this stack eligible for an AUTOMATIC reload at all? nullptr = yes; a
// reason, in the words the screen uses, when not.
//
// Three refusals, and none of them is about the switch:
//
//   THE PEER - and §16 changed the REASON while keeping the answer. The old one
//   was that §6's rebuild could not reach a peer's stack at all, so an automatic
//   reload would have refused every member and latched #56 onto a stack where
//   nothing failed. That is spent: the rebuild reaches it, watchReloadOffered
//   says yes, and the button below the amber line works.
//
//   What has NOT changed is what the act COSTS, and that is the whole of the
//   argument now. A local reload is N file reads on a disk that answers or
//   errors in milliseconds. A remote one is META + TILE per member on a link
//   that can drop, read to completion on the UI thread with nothing repainting
//   (openRemote's UiThreadStall says so in as many words) - so a stack of forty
//   resident frames is forty round trips and a folder's worth of pixels, with
//   nobody's click behind it, repeatable every time the peer's folder settles.
//   Waiting it out is not available either: watchAutoBusy waits for work the
//   frame loop is already keeping frames coming for, and a blocked ssh read is
//   the opposite of that.
//
//   So: the gesture reaches the peer, the AUTOMATON does not. That is one term
//   in this predicate rather than a second copy of §12/§13's rules - what runs
//   when it does run is still the one reloadStackFromDisk - and it is asked
//   through watchIsPeerStack, the same predicate the watcher already splits the
//   origins with. The Files line says it (watchAutoNote), and it says the
//   gesture that does work: Reload it by hand.
//
//   A RELOAD THAT ALREADY REFUSED. Without this the retry loop is real and it
//   never stops: the reload refuses a member, watchReloadNow clears the finding,
//   the worker re-reports the same difference on its next round (the refused
//   member's baseline never advanced - that is exactly right, and file_list
//   says so), and the automatic reload fires again. Every five seconds. #56's
//   latch already records precisely the state that must stop it and is cleared
//   by precisely the event that should restart it - a reload of the whole stack
//   in which every member succeeds - so it is the latch that is asked rather
//   than a second counter kept beside it. One attempt, then the mark, then the
//   person.
static const char* watchAutoRefusal(int seqId) {
    if (!watchReloadOffered(seqId))
        return "nothing here re-reads these pixels - re-open the file";
    if (const App::SeqInfo* si = seqInfo(seqId); si && watchIsPeerStack(*si))
        return "these frames come back over the link - Reload it by hand";
    if (const App::SeqInfo* si = seqInfo(seqId); si && si->reloadFailed > 0)
        return "the last reload of this stack refused a member - Reload it by hand";
    return nullptr;
}

// Is something IN FLIGHT that this reload must not run underneath? nullptr = no.
//
// THE DECISION (§15): an automatic reload WAITS. It does not cancel the work and
// it does not run beside it - the finding stays on the row, the amber line stays
// up, and the reload happens on a later frame. Silently swapping the pixels
// under a running measurement is the one failure this stage could introduce that
// no later reading of the screen would ever reveal, because the number would
// look perfectly ordinary.
//
// Waiting is safe to do here for a reason worth writing down: every condition
// below is one the main loop ALREADY keeps frames coming for (seqRunning,
// mPending, rfPending are all terms of its `busy`), so the frame that ends the
// work is a frame, and watchAutoDue is asked again in it. Nothing has to be
// remembered and nothing can be forgotten.
//
// A COMPARE PIN IS NOT ON THIS LIST, and that is a decision and not an omission.
// A pin is a uid - a membership identity - so it survives a renumbering (§12.5),
// and the compare readout is recomputed from the current pixels every frame, so
// there is no accumulating number to leave a seam in. The one thing a pin CAN
// lose is its frame, when that frame's file is gone; that is not prevented (the
// alternative is to keep measuring a file that does not exist) but it is SAID,
// in autoReloadText's own clause.
static const char* watchAutoBusy(int seqId) {
    // ---- this stack in particular ------------------------------------------
    if (app.srvTemporal.pending && app.srvTemporal.seqId == seqId)
        return "a server-side sigma_t of this stack is running";
    if (app.srvTemporalB.pending && app.srvTemporalB.seqId == seqId)
        return "a server-side sigma_t of this stack is running (compare B)";
    for (const App::PendingAvg& p : app.pendingAvg)
        if (p.seqId == seqId) return "a frame average of this stack is queued";
    if (app.seqLoadingId == seqId && (app.seqRunning || seqReadyPending()))
        return "frames are still loading into this stack";
    // ---- ...and the workers whose jobs do not carry a stack ------------------
    // A measure job names FILES, not a seqId (App::MJob), so "is this one about
    // that stack?" has no answer here - and the answer that matters is about the
    // RESULT, which is filed against a stack and a frame count. Same for a
    // full-resolution fetch, which replaces pixels in place. Both are seconds
    // long and both keep the frame loop awake, so waiting them out costs one
    // round of the poll interval at most.
    if (app.mPending > 0)  return "a measurement is in flight";
    if (app.rfPending > 0) return "a frame fetch is in flight";
    if (app.seqRunning || seqReadyPending() || !app.seqQueue.empty())
        return "frames are still loading";
    // A drag is a measurement being AIMED. temporal_model.inc already holds off
    // its own recompute for exactly this (`app.annBusy && T.seqId == ...`), and
    // moving the pixels under the rectangle somebody is dragging is worse.
    if (app.annBusy) return "an ROI or annotation drag is in progress";
    return nullptr;
}

// The stacks a standing finding has ARMED. Not a queue: "this stack's files have
// moved and it has not been reloaded since" is a STATE, and it is already
// written down (SeqInfo::watchFound), so it is asked of the stacks rather than
// remembered beside them. A queue would have to be kept in step with every
// close, every manual Reload, every switch and every finding that stopped being
// true - four ways to hold a stale seqId, and the symptom of holding one is a
// reload of something the user never saw a line about.
//
// It also means flipping the switch ON arms whatever is already on the board:
// the worker only speaks when a statement CHANGES (§3), so a queue filled at
// detection time would leave every stack that was already amber unreloaded
// until its files happened to move a second time.
static std::vector<int> watchAutoArmed() {
    std::vector<int> out;
    // watchPaused with the other two, and rbPollDue's line is the precedent:
    // §2's "not while the window is minimised" is about not doing background
    // work behind a window nobody can see, and ACTING behind one is more of
    // that, not less. (The frame loop already returns before pumpWatch while
    // iconified, so this is a statement rather than a guard - but it is also
    // what a test that pauses Watch to stop other writers is entitled to
    // assume, which is now a real caller: a9b7973.)
    if (!app.watchEnabled || !app.watchAutoReload || app.watchPaused) return out;
    for (const App::SeqInfo& si : app.seqs)
        if (si.watchFound.any() && !watchAutoRefusal(si.id)) out.push_back(si.id);
    return out;
}

// The idle-skip chain's term, in rbAnyPollDue's shape (§14.2): it asks whether a
// reload is DUE, never whether one is armed. A stack that is armed but waiting
// on a measurement answers false, so a long measurement cannot turn the 0 fps
// idle into a spin - which is §11.6's lesson, and the one thing this feature
// could break that nobody would report as a bug.
static bool watchAutoDue() {
    for (int s : watchAutoArmed())
        if (!watchAutoBusy(s)) return true;
    return false;
}

// THE reload, and there is one of it: the [Reload] button calls this with
// automatic=false and §9's drain calls it with true. What differs is only what is
// SAID afterwards - the operation is byte for byte the same one, which is the
// point (rule 2 above).
static void watchReloadNow(int seqId, bool automatic) {
    const std::string name = seqInfo(seqId) ? seqInfo(seqId)->name : std::string();
    // §12.5's other half: a pin on a frame that VANISHES is dropped by
    // closeImages, exactly as closing that frame drops it. Sampled across the
    // call, because the summary reloadStackFromDisk returns is about files and
    // this is about a comparison that has just ended.
    const uint64_t pinWas = app.compareBUid;
    int failed = 0;
    std::string msg = reloadStackFromDisk(seqId, &failed);
    const bool pinDropped = pinWas != 0 && app.compareBUid != pinWas;
    if (automatic) {
        char ts[32] = "";
        time_t now = time(nullptr);
        if (struct tm* lt = localtime(&now)) strftime(ts, sizeof ts, "%H:%M:%S", lt);
        msg = watch::autoReloadText(ts, msg, pinDropped);
    }
    if (App::SeqInfo* si = seqInfo(seqId)) {
        // The record, latched (rule 3). A manual reload CLEARS it: the user has
        // taken the wheel, and a note about what the program did behind them
        // stops being the thing they need to read.
        si->autoReloadNote = automatic ? msg : std::string();
        // The Watch line (§5) said "the files behind these pixels have moved",
        // and the operation it offered has just run: drop it HERE rather than
        // waiting for the worker to notice, so the answer to a click is not two
        // polls late. The worker re-seeds from the fresh stat on its own round
        // and agrees; a member the reload REFUSED still differs from the
        // baseline, so its finding comes straight back - which is correct, and
        // is the difference between clearing a fact and clearing a statement
        // that has stopped being true. (It is also why an automatic reload that
        // refused must disarm the stack - see watchAutoRefusal.)
        si->watchFound = watch::Finding{};
        si->watchToasted = false;
    }
    // Named by STACK when it was automatic. A manual reload was a click on one
    // row and needs no label; this one arrives out of nowhere, and a toast that
    // does not say whose numbers just moved is not a notice.
    toast(automatic ? name + ": " + msg : msg, failed > 0);
}

// The reload the LAST frame armed. Called at the head of pumpWatch and therefore
// BEFORE watchIntegrate, which is §9's "put the line up, then run on the next
// frame" spelled as an order of two calls: a finding integrated in frame N is on
// screen for frame N's draw, and this runs it at the head of N + 1. What
// happened is then readable in the same place it was announced.
static void watchAutoDrain() {
    for (int seqId : watchAutoArmed()) {
        // Re-asked per stack: the reload above this one in the list may have
        // closed frames, and seqInfo()'s rule is that a pointer is never held
        // across anything.
        const App::SeqInfo* si = seqInfo(seqId);
        if (!si || !si->watchFound.any()) continue;
        if (watchAutoRefusal(seqId) || watchAutoBusy(seqId)) continue;
        watchReloadNow(seqId, true);
    }
}

// What the Files line adds when this stack is armed but cannot run yet. "" when
// there is nothing to add. It is a separate string from the finding on purpose:
// seqWatchNote states what the DISK did, and this states what the program is
// about to do about it, which are two facts and not one.
static std::string watchAutoNote(int seqId) {
    // The same three terms watchAutoArmed opens with, and they have to be the
    // same three: this sentence promises "reloading automatically", and a
    // promise made under conditions the drain does not share is a false one.
    if (!app.watchEnabled || !app.watchAutoReload || app.watchPaused) return {};
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si || !si->watchFound.any()) return {};
    // A stack the walk cannot serve at all has already been told what to do by
    // watchActionText ("re-open the file to re-read it"), so repeating it under
    // a second heading would print one instruction twice in one sentence.
    //
    // The PEER is no longer that case (§16): its line now offers [Reload], so
    // the fact that the AUTOMATIC one is holding off has to be spoken here or a
    // peer's stack reads as one the feature simply forgot about.
    if (!watchReloadOffered(seqId)) return {};
    if (const char* why = watchAutoRefusal(seqId))
        return std::string(" - auto-reload cannot: ") + why;
    if (const char* why = watchAutoBusy(seqId))
        return std::string(" - auto-reload is waiting: ") + why;
    return " - reloading automatically";
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
        // changed on disk" must not (seqNote's precedent). What it tells the
        // reader to DO is watchActionText's, asked of the STACK rather than of
        // the finding's origin (§16: the sentence forks on whether there is a
        // button, which is the thing it is about) - unless §9 has something to
        // say, in which case what to do is not the reader's business and the
        // toast says what is about to happen, or why it is not. Telling somebody
        // to press a button the program is already pressing for them is how a
        // setting gets forgotten.
        const std::string an = watchAutoNote(d.seqId);
        toast(si->name + ": " + watch::findingText(d.found) +
              (an.empty() ? watchActionText(d.seqId) : an));
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
        // ...and whatever it found before it was told to stop is INTEGRATED,
        // not dropped. Two reasons, and the second is the one that bites:
        // those files really did change (switching the watcher off does not
        // un-change them), and an undrained queue leaves
        // watchFindingsPending() true forever - which puts `working` in the
        // idle-skip chain permanently and turns the 0 fps idle into a
        // continuous 12 fps for the rest of the session.
        watchIntegrate();
        // ...but NOTHING IS RELOADED on the way out. The findings are facts and
        // are kept; an automatic reload is an ACTION, and the switch that was
        // just thrown is the one that governs acting. watchAutoArmed answers
        // empty while watchEnabled is false, so this is a statement about what
        // the next frame will not do rather than a line of code.
        return;
    }
    // §9: the reload the LAST frame armed, before this frame integrates anything
    // new. See watchAutoDrain - the order of these two calls IS the rule.
    watchAutoDrain();
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
