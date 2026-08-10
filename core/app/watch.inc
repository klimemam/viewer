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

// The peer's directory holding `p`, with no trailing separator - and "" when it
// has none. A peer path is the SERVER's, so it is split as text: this machine's
// std::filesystem would apply this machine's rules to a name that never touches
// this disk (a Windows client reading a Linux peer's "/data/cap/f_000.npy").
// Both separators, because the peer may be either.
static std::string remoteDirOf(const std::string& p) {
    const size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : p.substr(0, cut);
}

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

// WHAT TO DO ABOUT IT, which is not one sentence for both origins.
//
// The local line ends "- Reload in Files to re-read them" and puts a [Reload]
// beside it. A stack whose pixels are the PEER's has no such button: reloadSource
// refuses remote-backed sources by name ("these pixels come from the peer, not
// from a local file" - reloadUnavailable), and §6's membership rebuild has not
// reached the remote origin. Printing the local instruction there would be
// telling the user to press a control this program greys out, so the remote line
// names the gesture that does work.
static const char* watchActionText(bool peer) {
    return peer ? " - re-open the folder from Browse to re-read it"
                : " - Reload in Files to re-read them";
}

// May the watch line OFFER [Reload] for this stack? One predicate, asked by the
// panel and by --rwatch-selftest, and it is the same question the stack's
// context menu already asks about the same members (reloadUnavailable) - a
// second transcription of that list is how the menu and this button would come
// to disagree about one stack.
static bool watchReloadOffered(int seqId) {
    const App::SeqInfo* si = seqInfo(seqId);
    if (si && watchIsPeerStack(*si)) return false;
    for (int idx : framesOfSeq(seqId))
        if (reloadUnavailable(*app.images[idx]->src).empty()) return true;
    return false;
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
        // reader to DO is watchActionText's, because a peer's stack has no
        // [Reload] to press.
        toast(si->name + ": " + watch::findingText(d.found) +
              watchActionText(!d.found.host.empty()));
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
