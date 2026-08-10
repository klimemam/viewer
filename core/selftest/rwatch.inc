// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- Watch, the REMOTE half (docs/watch-design.md §1's remote bullet) ------
    //
    // --watch-selftest is the same rule over a local disk. This is the same rule
    // again with a PEER on the other side of it, and it is a separate test for
    // the reason selftest.rtemporal is separate from selftest.abstats: it needs
    // the standalone `viewer-serve` binary (--remote-exe), and a client talking
    // to itself cannot tell "the peer grouped that folder" from "I did".
    //
    // THE CLAIM UNDER TEST, in one line: no new protocol. The peer's existing
    // LIST already answers a whole stack, because for a numbered sequence it
    // returns a synthetic GROUP row carrying the set's total bytes, its newest
    // member's mtime and the full member name list (core/serve.cpp
    // putGroupEntryV3). R1 is that claim, asserted against the real peer before
    // anything is built on it.
    //
    // ...and the HONESTY CONSTRAINT that follows from it, which is what R7
    // exists for: the row carries no PER-MEMBER mtime. A remote finding can say
    // that this stack's source moved; it cannot say which of N files moved, and
    // §4's "two readings that agree" therefore settles the stack as ONE unit
    // rather than per file. The local half releases a settled frame while its
    // neighbour is still being written (--watch-selftest W9); this half cannot,
    // and R7 asserts that it does not pretend to.
    //
    // NOTHING HERE SLEEPS, for --watch-selftest's reason doubled: the remote
    // interval is 15 s (§2). watchPollRound takes the lister as an argument, so
    // this test decides when a poll happens AND what the link answered.
    if (!g_rwatchSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "rwatchselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };

        // ---- the fixture: a folder of .npy on the "peer", which is this disk --
        // A local:// peer is a REAL peer - the same protocol over a pipe instead
        // of ssh (remote.h) - and with --remote-exe it is the standalone
        // viewer-serve binary, which is the only way the grouping under test is
        // the peer's own and not this process's.
        std::string root = g_rwatchSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/rwatch-scratch";
        const std::string stk = root + "/stk";      // 5 numbered frames: a group
        const std::string one = root + "/one";      // ONE file: a plain row
        const std::string pic = root + "/pic";      // a PNG stack (#148/#159)
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(stk), ec);
        std::filesystem::create_directories(pathFromUtf8(one), ec);
        std::filesystem::create_directories(pathFromUtf8(pic), ec);
        // One 2-D f32 .npy, w x h. Its size on disk is arithmetic and R1 asserts
        // the arithmetic: 8 (magic+version) + 2 (header length) + a header
        // padded so the whole region is a multiple of 64 -> 128 bytes for either
        // shape used here + w*h*4 bytes of float32. So 4x4 = 128 + 64 = 192 and
        // 6x6 = 128 + 144 = 272.
        auto writeNpy2 = [](const std::string& path, int w, int h, float v) {
            std::vector<float> px((size_t)w * h, v);
            std::vector<uint8_t> b = selftestNpyBytes("<f4", { h, w }, px.data(),
                                                      px.size() * sizeof(float));
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            f.write((const char*)b.data(), (std::streamsize)b.size());
        };
        // A file of an ARBITRARY byte count, for the cases whose subject is the
        // group row's SUM rather than anything decodable.
        auto writeBlob = [](const std::string& path, size_t n, char fill) {
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            std::string s(n, fill);
            f.write(s.data(), (std::streamsize)s.size());
        };
        auto fsizeOf = [](const std::string& p) {
            std::error_code e;
            auto n = std::filesystem::file_size(pathFromUtf8(p), e);
            return e ? (uintmax_t)0 : n;
        };
        auto rm = [](const std::string& p) {
            std::error_code e;
            std::filesystem::remove(pathFromUtf8(p), e);
        };
        auto nm = [](int i) {
            char b[32];
            snprintf(b, sizeof b, "w_%03d.npy", i);
            return std::string(b);
        };
        for (int i = 0; i < 5; i++) writeNpy2(stk + "/" + nm(i), 4, 4, (float)i);
        writeNpy2(one + "/solo.npy", 4, 4, 1.0f);

        // ---- the link ---------------------------------------------------------
        // The worker's discipline, in the test: ONE Session, owned by the thread
        // that uses it (§3). The lister is the seam watchPollRound takes, so the
        // test can count the calls (§2's dedupe) and can answer FALSE (R9).
        const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
        remote::Session ses;
        std::string serr;
        if (!ses.startOn("", 0, exe, serr)) {
            fprintf(stderr, "rwatchselftest: cannot start the peer (%s): %s\n",
                    exe.c_str(), serr.c_str());
            return 1;
        }
        int lists = 0;                       // how many LIST round trips a round made
        bool linkUp = true;                  // R9 pulls this down
        auto lister = [&](const App::WatchTarget& t,
                          std::vector<remote::Entry>& rows) {
            if (!linkUp) return false;
            lists++;
            std::string err;
            return ses.list(t.dir, rows, err);
        };

        {   // ---- R1: the LIST group row still carries what §1 assumes ----------
            // Asserted against the REAL peer, first, because everything below is
            // built on it: if the peer stopped folding a numbered sequence into
            // one row, or stopped summing the bytes, or stopped sending the
            // member names, the rest of this file would be testing a different
            // mechanism than the one the design describes.
            std::vector<remote::Entry> rows;
            std::string err;
            const bool got = ses.list(stk, rows, err);
            const remote::Entry* g = nullptr;
            int groups = 0;
            for (const auto& e : rows)
                if (e.group) { groups++; if (!g) g = &e; }
            // 5 files x 192 bytes (128-byte header region + 4*4*4 of float32).
            check(got && groups == 1 && g && g->members.size() == 5 &&
                  g->size == 5 * 192 && g->frames == 5,
                  "R1 the peer folds the 5 files into ONE row: bytes = 5*192 = 960");
            check(g && g->members[0] == "w_000.npy" && g->members[4] == "w_004.npy",
                  "R1b ...and the row carries the full member NAME list");
            // mtime is the NEWEST member's, in unix seconds. Asserted as "a real
            // clock reading" rather than against a literal: what matters is that
            // the field is populated at all, since it is half the change test.
            check(g && g->mtime > 1600000000LL,
                  "R1c ...and a newest-member mtime, which is half the rule");
            // ANTI-VACUITY: the row is SYNTHETIC. If the peer had sent five
            // plain rows instead, `groups` above would be 0 and this whole file
            // would be watching something else.
            int plain = 0;
            for (const auto& e : rows) if (!e.dir && !e.group) plain++;
            check(plain == 0,
                  "R1d ...and the 5 members do NOT also appear as plain rows");
        }

        // ---- the stack, as the UI thread publishes it -------------------------
        // Built by hand rather than by opening the folder over the link, for
        // --derive-selftest D7's reason: what watchTargetsNow reads is
        // SeqInfo::remoteHost / remotePort / remoteFiles, and those are the
        // thing under test. Opening the stack would also fetch five frames'
        // pixels, which this test never looks at.
        auto makeRemoteSeq = [&](const std::string& host, int port,
                                 const std::vector<std::string>& files) {
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = "remote stack " + std::to_string(si.id);
            si.remoteHost = host;
            si.remotePort = port;
            si.remoteFiles = files;
            si.expectedFrames = (int)files.size();
            app.seqs.push_back(si);
            return si.id;
        };
        // SeqTable has no remove-by-id: erase takes an iterator, and the table
        // RESEATS its storage on every mutation, so the loop must not hold one
        // across the call (App::SeqTable's whole reason for existing).
        auto dropSeq = [&](int id) {
            for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                if (it->id == id) { app.seqs.erase(it); return; }
        };
        std::vector<std::string> stkFiles;
        for (int i = 0; i < 5; i++) stkFiles.push_back(stk + "/" + nm(i));

        {   // ---- R2: a peer's stack becomes ONE watched set --------------------
            const int sid = makeRemoteSeq("kepler", 2222, stkFiles);
            std::vector<App::WatchTarget> tg = watchTargetsNow();
            const App::WatchTarget* t = nullptr;
            for (const auto& x : tg) if (x.seqId == sid) t = &x;
            check(t && t->peer && t->host == "kepler" && t->port == 2222 &&
                  t->dir == stk && t->members.size() == 5 &&
                  t->members.front() == "w_000.npy" && t->members.back() == "w_004.npy",
                  "R2 a remote stack is one watched set: host, port, dir, members");
            // §1: no seed. An ssh:// url is never statted from here (statLocalUrl
            // says so in as many words), so the FIRST poll makes the baseline -
            // which is exactly the case §1 names, and the reason it says nothing.
            check(t && t->seed.empty() && t->file.empty() && t->headName.empty(),
                  "R2b ...with NO seed: the first poll is its baseline (§1)");
            check(t && t->hostLabel == "kepler",
                  "R2c ...and the name the sentence will use is the peer's");
            // ANTI-VACUITY: a stack spanning two of the peer's directories is
            // not watched at all, for the local half's reason (W14c) - one LIST
            // speaks about one directory, and members living elsewhere would be
            // absent from every observation, which §4 would confirm as VANISHED.
            std::vector<std::string> split = stkFiles;
            split.back() = root + "/elsewhere/w_004.npy";
            const int sid2 = makeRemoteSeq("kepler", 2222, split);
            bool watched2 = false;
            for (const auto& x : watchTargetsNow()) if (x.seqId == sid2) watched2 = true;
            check(!watched2,
                  "R2d a stack spanning two of the peer's dirs is not watched");
            dropSeq(sid2);
            dropSeq(sid);
        }

        // The state the rounds below share, and the two calls that drive one.
        // No thread is started: a worker polling on its own 15 s timer would
        // race this test for the queue, exactly as --watch-selftest says.
        std::map<int, WatchSet> states;
        std::vector<App::WatchDone> out;
        std::vector<App::WatchTarget> targets;
        auto poll = [&](bool doRemote = true) {
            out.clear();
            return watchPollRound(targets, states, out, lister, doRemote);
        };
        auto integrate = [&]() {
            {
                std::lock_guard<std::mutex> lk(app.watchMtx);
                app.watchDone = out;
            }
            watchIntegrate();
        };

        const int seq = makeRemoteSeq("", 0, stkFiles);      // the peer runs here
        for (const auto& x : watchTargetsNow()) if (x.seqId == seq) targets.push_back(x);

        {   // ---- R3: §2's dedupe - one LIST per (host, port, dir) --------------
            // Three stacks over ONE folder of the peer is ONE request. The cost
            // ceiling §2 states is "watched dirs x one LIST per interval", and
            // it is a ceiling only if this holds.
            const int a = makeRemoteSeq("", 0, stkFiles);
            const int b = makeRemoteSeq("", 0, stkFiles);
            std::vector<App::WatchTarget> three;
            for (const auto& x : watchTargetsNow())
                if (x.seqId == seq || x.seqId == a || x.seqId == b) three.push_back(x);
            check(three.size() == 3, "R3 (setup) three stacks over one peer folder");
            std::map<int, WatchSet> st3;
            std::vector<App::WatchDone> o3;
            lists = 0;
            watchPollRound(three, st3, o3, lister, true);
            check(lists == 1, "R3b three stacks in one peer folder cost ONE LIST");
            // ANTI-VACUITY: the one call really answered, and all three stacks
            // really took a baseline from it. A lister that was never called
            // would also report `lists == 1` if the counter were wrong, and a
            // round that skipped every stack would too.
            check(st3.size() == 3 && st3[seq].st.haveBase &&
                  st3[a].st.haveBase && st3[b].st.haveBase &&
                  st3[seq].st.base.size() == 5,
                  "R3c ...and all three took a 5-member baseline from that one row");
            dropSeq(a);
            dropSeq(b);
        }
        {   // ---- R4: §2's interval - a peer is asked every 15 s, not every 5 ---
            // The worker's timer is the LOCAL interval, so this is the ratio.
            check(watchRemoteEvery() == 3 && app.watchIntervalSec == 5.0 &&
                  app.watchRemoteIntervalSec == 15.0,
                  "R4 5 s local / 15 s remote: a peer is asked every 3rd round");
            std::map<int, WatchSet> st4;
            std::vector<App::WatchDone> o4;
            lists = 0;
            const bool spoke = watchPollRound(targets, st4, o4, lister, false);
            check(lists == 0 && !spoke && o4.empty() && st4.empty(),
                  "R4b a round that is not the peer's costs ZERO LIST requests");
            // ANTI-VACUITY: the same round WITH the peer's turn does list.
            watchPollRound(targets, st4, o4, lister, true);
            check(lists == 1 && st4.size() == 1,
                  "R4c ...and the peer's own round makes exactly one");
        }
        {   // ---- R5: §1's baseline - the first poll takes one and says nothing -
            const bool spoke = poll();
            check(!spoke && out.empty() && seqWatchNote(seq).empty(),
                  "R5 the first poll of a peer's stack announces nothing");
            // ANTI-VACUITY, and it is what makes R5 mean something: the baseline
            // was TAKEN, and it is the group row - five members, every one of
            // them carrying the SET's total of 960 bytes, because that is the
            // only (mtime, size) the row has to give.
            check(states[seq].st.haveBase && states[seq].st.base.size() == 5 &&
                  states[seq].st.base[0].size == 5 * 192 &&
                  states[seq].st.base[4].size == 5 * 192 &&
                  states[seq].st.base[0].mtime == states[seq].st.base[4].mtime,
                  "R5b ...and the baseline IS the row: 5 names, one shared 960/mtime");
        }
        {   // ---- R6: a frame rewritten on the peer -----------------------------
            // 4x4 -> 6x6 is 192 -> 272 bytes, so the group row's sum moves from
            // 5*192 = 960 to 4*192 + 272 = 1040. A real difference in the summed
            // bytes, not a timestamp race.
            writeNpy2(stk + "/w_002.npy", 6, 6, 42.0f);
            check(fsizeOf(stk + "/w_002.npy") == 272,
                  "R6 (setup) the rewritten frame is 272 bytes (128 + 6*6*4)");
            const bool s1 = poll();
            check(!s1 && out.empty() && seqWatchNote(seq).empty(),
                  "R6b the first poll after the write says nothing (§4: read once)");
            const bool s2 = poll();
            integrate();
            // THE SENTENCE, and the honesty constraint in it: the peer answered
            // with one row for five files, so the line says the source moved and
            // says outright that which file cannot be known - it does not
            // borrow the local half's count.
            check(s2 && out.size() == 1 && out[0].seqId == seq &&
                  seqWatchNote(seq) == "source changed on this machine "
                                       "(5 file(s); a peer listing cannot say which)",
                  "R6c the second poll confirms it, and names no file it cannot name");
            // ANTI-VACUITY: the local half's sentence must not appear here. A
            // remote finding that said "1 file(s) changed on disk (w_002.npy)"
            // would be a claim the group row never made.
            check(seqWatchNote(seq).find("changed on disk") == std::string::npos,
                  "R6d ...and it is NOT the local sentence, which counts files");
            const bool s3 = poll();
            check(!s3 && seqWatchNote(seq) ==
                    "source changed on this machine "
                    "(5 file(s); a peer listing cannot say which)",
                  "R6e a repeat of a known finding does not wake the UI again (§3)");
        }
        {   // ---- R7: THE per-member question, decided and asserted -------------
            // §11.2's two-readings rule is symmetric and applied PER MEMBER. When
            // the reading is ONE aggregated row, "per member" collapses to "per
            // stack": every member carries the same (bytes, mtime), so the set
            // settles as one unit.
            //
            // The consequence, stated as a test rather than left in a comment:
            // one file still being WRITTEN on the peer holds back the whole
            // stack's finding. The local half does not - W9 announces the
            // settled member while its neighbour grows - and this is the exact
            // place the two origins differ. It is a limit of one reading of N
            // files, not a policy, and the alternative (announcing an aggregate
            // that includes a half-written file) would be announcing a number
            // that is not the file's size.
            //
            // Put the folder back to a known state first, so the only thing
            // moving is what this case is about.
            writeNpy2(stk + "/w_002.npy", 4, 4, 2.0f);
            std::map<int, WatchSet> st7;
            std::vector<App::WatchDone> o7;
            auto poll7 = [&]() {
                o7.clear();
                return watchPollRound(targets, st7, o7, lister, true);
            };
            poll7();                                    // baseline: 5*192 = 960
            check(st7[seq].st.base.size() == 5 && st7[seq].st.base[0].size == 5 * 192,
                  "R7 (setup) the baseline is the quiet folder: 960 bytes");
            // w_001 SETTLES at a new size (192 -> 272) while w_003 GROWS by 10
            // bytes a round. Locally that is W9 and the finding about w_001 is
            // announced immediately.
            writeNpy2(stk + "/w_001.npy", 6, 6, 11.0f);      // settled from now on
            bool spokeWhileGrowing = false;
            for (int k = 0; k < 4; k++) {
                writeBlob(stk + "/w_003.npy", 300 + (size_t)k * 10, 'x');
                spokeWhileGrowing |= poll7();
            }
            check(!spokeWhileGrowing && o7.empty(),
                  "R7b a settled frame is NOT released while a neighbour grows");
            // ANTI-VACUITY, and the half that matters: the instant the writer
            // stops, the same watcher speaks. Without this pair, "a remote stack
            // never says anything" passes R7b.
            //
            // The arithmetic, so the next poll is not a surprise: the last round
            // above wrote w_003 at 330 bytes and then read the folder, so the
            // row summed 192 + 272 + 192 + 330 + 192 = 1178 ONCE. Nothing has
            // been written since, so THIS poll reads 1178 again - two readings
            // that agree, which is §4 satisfied and the finding released.
            const bool a1 = poll7();
            check(a1 && o7.size() == 1 &&
                  watch::findingText(o7[0].found) ==
                      "source changed on this machine "
                      "(5 file(s); a peer listing cannot say which)",
                  "R7c ...and it speaks the instant the peer's folder stops moving");
            const bool a2 = poll7();
            check(!a2 && o7.empty(),
                  "R7d ...once: a repeat of that finding does not wake the UI (§3)");
            writeNpy2(stk + "/w_001.npy", 4, 4, 1.0f);       // put the folder back
            writeNpy2(stk + "/w_003.npy", 4, 4, 3.0f);
        }
        {   // ---- R8: the NAMES are per-member truth, and that is the half the
            // group row DOES carry. A file that appeared and a file that is gone
            // are named as precisely as a local scan names them - only the
            // (mtime, size) is an aggregate.
            //
            // The case is deliberately the one a COUNT cannot see (W29's remote
            // twin): one member deleted and one added, both 192 bytes, so the
            // row's summed bytes are the same 960 before and after. If the names
            // were not read, this poll would have nothing at all to report.
            std::map<int, WatchSet> st8;
            std::vector<App::WatchDone> o8;
            auto poll8 = [&]() {
                o8.clear();
                return watchPollRound(targets, st8, o8, lister, true);
            };
            poll8();
            const uint64_t base8 = st8[seq].st.base.empty() ? 0 : st8[seq].st.base[0].size;
            check(base8 == 5 * 192, "R8 (setup) the baseline row sums to 960 bytes");
            rm(stk + "/w_004.npy");
            writeNpy2(stk + "/w_005.npy", 4, 4, 5.0f);       // same 192 bytes
            const bool s1 = poll8();
            check(!s1 && o8.empty(),
                  "R8b one poll after an add+delete says nothing (§4 both ways)");
            const bool s2 = poll8();
            check(s2 && o8.size() == 1 &&
                  watch::findingText(o8[0].found) ==
                      "1 file(s) appeared in the folder (w_005.npy) on this machine, "
                      "1 file(s) no longer exist (w_004.npy)",
                  "R8c the second names BOTH, from the row's member list");
            // ANTI-VACUITY: the summed bytes really did NOT move, so the names
            // are the only thing that could have produced that sentence.
            check(o8.size() == 1 &&
                  watch::findingText(o8[0].found).find("source changed") ==
                      std::string::npos,
                  "R8d ...and the summed bytes did not move: 960 before and after");
            rm(stk + "/w_005.npy");
            writeNpy2(stk + "/w_004.npy", 4, 4, 4.0f);       // put the folder back
        }
        {   // ---- R9: a LIST that did not answer is NOT "everything vanished" ---
            // An unreachable peer, a dropped ssh link and a refused path all
            // arrive as "no reply". Folded into an empty listing they would have
            // §4 confirm - correctly by its own lights - that every file of the
            // stack is gone, and the screen would say a capture was deleted
            // because a network cable was. The round is SKIPPED for that stack.
            std::map<int, WatchSet> st9;
            std::vector<App::WatchDone> o9;
            auto poll9 = [&]() {
                o9.clear();
                return watchPollRound(targets, st9, o9, lister, true);
            };
            poll9();                                     // baseline over a live link
            check(st9[seq].st.haveBase && st9[seq].st.base.size() == 5,
                  "R9 (setup) a 5-member baseline, taken while the link was up");
            linkUp = false;
            bool spokeDown = false;
            lists = 0;
            for (int k = 0; k < 4; k++) spokeDown |= poll9();
            check(!spokeDown && o9.empty() && lists == 0,
                  "R9b four polls with the link down say nothing at all");
            linkUp = true;
            bool spokeBack = false;
            for (int k = 0; k < 2; k++) spokeBack |= poll9();
            check(!spokeBack && o9.empty() && st9[seq].st.base.size() == 5,
                  "R9c ...and the link coming back finds the baseline intact");
            // ANTI-VACUITY: the watcher is not simply mute now.
            writeNpy2(stk + "/w_002.npy", 6, 6, 99.0f);
            poll9();
            const bool spoke = poll9();
            check(spoke && o9.size() == 1 &&
                  watch::findingText(o9[0].found) ==
                      "source changed on this machine "
                      "(5 file(s); a peer listing cannot say which)",
                  "R9d ...and a real change after that is seen, normally");
            writeNpy2(stk + "/w_002.npy", 4, 4, 2.0f);
        }
        {   // ---- R10: a SUBSET of a peer's group is not watched at all ---------
            // §11.4 from the other side of the link, and it is STRICTER here.
            // The local half narrows its observation to the stack's own members
            // (W23), because a local scan stats every file separately. A group
            // row cannot be narrowed: its bytes and its mtime cover files this
            // stack does not contain, and there is no per-member state to
            // subtract them with. So a subset stack says NOTHING - the same
            // failure direction the local half takes, and the only honest one
            // available from an aggregate.
            std::vector<std::string> subset(stkFiles.begin(), stkFiles.begin() + 3);
            const int sub = makeRemoteSeq("", 0, subset);
            std::vector<App::WatchTarget> tg;
            for (const auto& x : watchTargetsNow()) if (x.seqId == sub) tg.push_back(x);
            check(tg.size() == 1 && tg[0].members.size() == 3,
                  "R10 (setup) a 3-of-5 subset of the peer's folder");
            std::map<int, WatchSet> st10;
            std::vector<App::WatchDone> o10;
            auto poll10 = [&]() {
                o10.clear();
                return watchPollRound(tg, st10, o10, lister, true);
            };
            // The baseline FIRST and the change after it - §1's own rule (W12):
            // a file rewritten before the first poll is invisible on purpose,
            // and a case that changed the folder before it would prove nothing.
            poll10();
            writeNpy2(stk + "/w_000.npy", 6, 6, 70.0f);     // one of ITS OWN three
            bool spokeEver = false;
            for (int k = 0; k < 3; k++) spokeEver |= poll10();
            check(!spokeEver && o10.empty() && !st10[sub].wholeGroup,
                  "R10b a subset of a peer's group says nothing, even about its own");
            // ANTI-VACUITY: the WHOLE-group stack over the SAME folder, told the
            // same kind of thing, does speak. Without this, R10b is "watch
            // nothing" wearing a reason.
            std::map<int, WatchSet> st10b;
            std::vector<App::WatchDone> o10b;
            auto poll10b = [&]() {
                o10b.clear();
                return watchPollRound(targets, st10b, o10b, lister, true);
            };
            poll10b();                                      // its own baseline
            writeNpy2(stk + "/w_000.npy", 4, 4, 0.0f);      // ...and now it moves
            bool spokeWhole = false;
            for (int k = 0; k < 3; k++) spokeWhole |= poll10b();
            check(spokeWhole && st10b[seq].wholeGroup,
                  "R10c ...while the whole-group stack over that folder does");
            dropSeq(sub);
        }
        {   // ---- R11: a PLAIN row is per-member truth, and says so -------------
            // A file the peer did NOT fold into a group - a frame-axis stack of
            // one .npy holding F frames, a folder with a single capture in it -
            // comes back as its own row with its OWN bytes and its OWN mtime.
            // Nothing is aggregated, so nothing has to be hedged: the sentence
            // is the local one, and it names the file.
            //
            // Getting this right is the reason the observation is built PER ROW
            // rather than "remote means aggregate": hedging a reading that is
            // exact would be its own dishonesty, in the opposite direction.
            const int solo = makeRemoteSeq("", 0, { one + "/solo.npy" });
            std::vector<App::WatchTarget> tg;
            for (const auto& x : watchTargetsNow()) if (x.seqId == solo) tg.push_back(x);
            check(tg.size() == 1 && tg[0].members.size() == 1 && tg[0].dir == one,
                  "R11 (setup) a one-file remote stack: the peer sends a plain row");
            std::map<int, WatchSet> st11;
            std::vector<App::WatchDone> o11;
            auto poll11 = [&]() {
                o11.clear();
                return watchPollRound(tg, st11, o11, lister, true);
            };
            poll11();
            check(st11[solo].st.base.size() == 1 && st11[solo].st.base[0].size == 192,
                  "R11b ...and its baseline is that file's OWN 192 bytes");
            writeNpy2(one + "/solo.npy", 6, 6, 2.0f);       // 192 -> 272
            poll11();
            const bool spoke = poll11();
            check(spoke && o11.size() == 1 &&
                  watch::findingText(o11[0].found) ==
                      "1 file(s) changed on disk (solo.npy) on this machine",
                  "R11c a plain row DOES name the file: no hedge it has not earned");
            dropSeq(solo);
        }
        {   // ---- R12: the peer serves the PICTURE formats now (#148 / PR #159) -
            // A watched remote folder is no longer necessarily .npy, and the
            // grouping that this whole file rests on is keyed on
            // isServedSuffix - so a folder of PNGs folds into a group row with
            // summed bytes exactly as a folder of .npy does. If it did not, a
            // watched PNG stack would have no row that speaks for it and would
            // silently report nothing forever.
            const char* src[4] = { "frame_001.png", "frame_002.png",
                                   "frame_003.png", "frame_004.png" };
            std::string media = g_rwatchSelftest;
            std::replace(media.begin(), media.end(), '\\', '/');
            while (!media.empty() && media.back() == '/') media.pop_back();
            media += "/media/pngstack";
            uint64_t sum = 0;
            std::vector<std::string> pfiles;
            for (const char* s : src) {
                std::filesystem::copy_file(pathFromUtf8(media + "/" + s),
                                           pathFromUtf8(pic + "/" + s),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                sum += (uint64_t)fsizeOf(pic + "/" + s);
                pfiles.push_back(pic + "/" + s);
            }
            std::vector<remote::Entry> rows;
            std::string err;
            ses.list(pic, rows, err);
            const remote::Entry* g = nullptr;
            for (const auto& e : rows) if (e.group) g = &e;
            check(g && g->members.size() == 4 && g->size == sum && sum > 0,
                  "R12 a folder of PNGs folds into a group row too, bytes summed");
            const int ps = makeRemoteSeq("", 0, pfiles);
            std::vector<App::WatchTarget> tg;
            for (const auto& x : watchTargetsNow()) if (x.seqId == ps) tg.push_back(x);
            std::map<int, WatchSet> st12;
            std::vector<App::WatchDone> o12;
            auto poll12 = [&]() {
                o12.clear();
                return watchPollRound(tg, st12, o12, lister, true);
            };
            poll12();
            // frame_001.png is 799 bytes and frame_002.png is 1006, so copying
            // the second over the first moves the group's sum by 207 bytes.
            const uint64_t was = fsizeOf(pic + "/frame_001.png");
            std::filesystem::copy_file(pathFromUtf8(media + "/frame_002.png"),
                                       pathFromUtf8(pic + "/frame_001.png"),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            check(fsizeOf(pic + "/frame_001.png") != was,
                  "R12b (setup) the rewritten PNG is a different size");
            poll12();
            const bool spoke = poll12();
            check(spoke && o12.size() == 1 &&
                  watch::findingText(o12[0].found) ==
                      "source changed on this machine "
                      "(4 file(s); a peer listing cannot say which)",
                  "R12c ...and the same rule sees it move");
            dropSeq(ps);
        }
        {   // ---- R15: an EMPTY reading is not a decision about the stack -------
            // The whole-group question (R10) is settled once, at the baseline,
            // from the names the peer's rows carried. A folder that happens to
            // hold none of them at that instant - the capture directory has not
            // been filled yet, or was cleared between the open and the first
            // poll - carries no names at all, and answering "not the whole
            // group" from that would put the stack PERMANENTLY on mute: the
            // decision is never revisited, so the files arriving a second later
            // would never be spoken about again.
            //
            // So an empty reading defers the decision. The baseline is still
            // taken from it, which is §1 unchanged - and the files arriving
            // afterwards are then an ARRIVAL, which is exactly what they are.
            const std::string late = root + "/late";
            std::filesystem::create_directories(pathFromUtf8(late), ec);
            std::vector<std::string> lateFiles;
            for (int i = 0; i < 3; i++) {
                char b[32];
                snprintf(b, sizeof b, "l_%03d.npy", i);
                lateFiles.push_back(late + "/" + b);
            }
            const int ls = makeRemoteSeq("", 0, lateFiles);
            std::vector<App::WatchTarget> tg;
            for (const auto& x : watchTargetsNow()) if (x.seqId == ls) tg.push_back(x);
            check(tg.size() == 1 && tg[0].members.size() == 3 && fsizeOf(lateFiles[0]) == 0,
                  "R15 (setup) a 3-member peer stack whose folder is empty right now");
            std::map<int, WatchSet> st15;
            std::vector<App::WatchDone> o15;
            auto poll15 = [&]() {
                o15.clear();
                return watchPollRound(tg, st15, o15, lister, true);
            };
            const bool s0 = poll15();                   // baseline: nothing there
            check(!s0 && o15.empty() && st15[ls].st.haveBase && st15[ls].st.base.empty(),
                  "R15b the baseline over an empty folder is empty, and says nothing");
            for (size_t i = 0; i < lateFiles.size(); i++)
                writeNpy2(lateFiles[i], 4, 4, (float)i);
            poll15();
            const bool spoke = poll15();
            check(spoke && o15.size() == 1 &&
                  watch::findingText(o15[0].found) ==
                      "3 file(s) appeared in the folder on this machine",
                  "R15c ...and the files arriving are named, not silently ignored");
            dropSeq(ls);
        }
        {   // ---- R14: both halves of a remote finding in ONE sentence ----------
            // The ordinary case, and the one that shows what the two halves of
            // a group row are worth beside each other: a file APPEARS, so the
            // member list names it exactly, and the summed bytes move, so the
            // aggregate says the source changed without claiming which member
            // did. Both are true and both are said - suppressing the second
            // because the first "probably explains it" would be a guess, since
            // a member could have been rewritten in the same round.
            //
            // The arithmetic: the folder is back to 5 x 192 = 960, and the new
            // file is 6x6 = 272 bytes, so the row sums 1232 over 6 names.
            std::map<int, WatchSet> st14;
            std::vector<App::WatchDone> o14;
            auto poll14 = [&]() {
                o14.clear();
                return watchPollRound(targets, st14, o14, lister, true);
            };
            poll14();
            check(st14[seq].st.base.size() == 5 && st14[seq].st.base[0].size == 5 * 192,
                  "R14 (setup) the baseline is the quiet folder again: 960 bytes");
            writeNpy2(stk + "/w_005.npy", 6, 6, 5.0f);
            poll14();
            const bool spoke = poll14();
            check(spoke && o14.size() == 1 &&
                  watch::findingText(o14[0].found) ==
                      "source changed on this machine "
                      "(6 file(s); a peer listing cannot say which), "
                      "1 file(s) appeared in the folder (w_005.npy)",
                  "R14b the name it CAN give and the count it cannot, in one line");
            // ANTI-VACUITY: the peer is named ONCE, on the first clause. A
            // sentence that named it after each would read as two machines.
            const std::string txt = o14.size() == 1
                                        ? watch::findingText(o14[0].found)
                                        : std::string();
            check(!txt.empty() &&
                  txt.find("on this machine") == txt.rfind("on this machine"),
                  "R14c ...and the peer is named once, not once per clause");
            rm(stk + "/w_005.npy");
        }
        // =====================================================================
        //  R13, R16-R21: §16 - the Reload that REACHES the peer.
        //
        //  Everything above only ever SAYS something about a peer's folder.
        //  These act on it, through the one reloadStackFromDisk the [Reload]
        //  button calls, with every guard §12 put in it (the departures before
        //  reloadSource, the rule recorded at the mint, a rebuild that never
        //  empties a stack). What §16 adds is WHOSE listing answers "which files
        //  are in that folder now" and WHERE the pixels are re-read from.
        //
        //  THE FIXTURE, and why it is built by hand a second time: R2-R15 need
        //  no frames at all (what they read is SeqInfo::remoteFiles), but a
        //  reload is about the frames, and a stack with none of them passes
        //  every assertion below vacuously - watchReloadOffered walks members and
        //  answers false for a stack that has none. So these frames are FETCHED,
        //  through the same peer, and their pixels are the peer's own bytes.
        //
        //  The urls are the scp spelling "kepler:<path>" (remote::parseUrl's
        //  third form). That is deliberate and it is the only fiction here: the
        //  local peer is reached with an EMPTY host, and an empty host spells
        //  local://, which is LOCAL by §13.7 and would reload off this disk
        //  through the branch this file is not testing. A named host makes these
        //  sources peer-backed everywhere in the program - watchIsPeerStack,
        //  reloadUnavailable, planStackMembership - and the ReloadPeer handed in
        //  points the round trip at the real viewer-serve on the other end of
        //  `ses`. It is exactly the fiction the lister above already runs on
        //  (R2 watches "kepler" over a local pipe).
        // =====================================================================
        for (int i = 0; i < 5; i++) writeNpy2(stk + "/" + nm(i), 4, 4, 100.0f + (float)i);
        // WHICH session the reload talks to, and the one that never answers.
        auto peerOf = [&](const std::string&, int, std::string&) { return &ses; };
        auto peerDown = [&](const std::string&, int, std::string& e) -> remote::Session* {
            e = "the peer did not answer";
            return nullptr;
        };
        // One frame of a peer stack, fetched the way pumpRemoteFetch mints one.
        auto fetchFrame = [&](const std::string& file, int sid, int idx) -> ImageDoc* {
            remote::Meta m;
            std::string err;
            if (!ses.meta(file, m, err)) return nullptr;
            std::vector<float> px;
            int w = 0, h = 0, c = 0;
            std::string dt;
            if (!ses.tile(file, 0, 0, 0, m.w, m.h, 1, px, w, h, c, dt, err)) return nullptr;
            auto d = std::make_unique<ImageDoc>();
            d->name = baseName(file);
            FrameSource& S = *d->src;
            S.path = "kepler:" + file;
            S.remoteUrl = S.path;
            S.remoteFrame = 0;
            S.remoteStep = 1;
            S.npyShape = m.shape;
            S.dtype = dt;
            S.w = w; S.h = h; S.ch = c;
            S.data = std::move(px);
            d->syncMirrors();
            computeMinMax(*d);
            defaultRange(*d);
            shareOrRegisterSource(*d);
            d->seqId = sid;
            d->seqIndex = idx;
            d->uid = app.nextUid++;
            app.images.push_back(std::move(d));
            app.imagesRev++;
            return app.images.back().get();
        };
        // A peer stack of `files`, with `resident` of them actually fetched.
        auto makePeerStack = [&](const std::vector<std::string>& files, int resident) {
            const int sid = makeRemoteSeq("kepler", 0, files);
            ImageDoc* head = nullptr;
            for (int i = 0; i < resident && i < (int)files.size(); i++) {
                ImageDoc* d = fetchFrame(files[i], sid, i);
                if (!head) head = d;
            }
            if (App::SeqInfo* si = seqInfo(sid)) {
                si->expectedFrames = (int)files.size();
                if (head) {
                    si->refW = head->w; si->refH = head->h; si->refCh = head->ch;
                    si->refName = head->name;
                }
            }
            return sid;
        };
        // THE membership, as ONE string (--watch-selftest W25's discipline): a
        // count is not a membership, so every assertion states the whole list and
        // the number each member carries.
        auto members = [&](int sid) {
            std::string s;
            for (int idx : framesOfSeq(sid))
                s += (s.empty() ? "" : " ") + std::to_string(app.images[idx]->seqIndex) +
                     ":" + baseName(app.images[idx]->src->path);
            return s;
        };
        auto memberDoc = [&](int sid, int index) -> ImageDoc* {
            for (int idx : framesOfSeq(sid))
                if (app.images[idx]->seqIndex == index) return app.images[idx].get();
            return nullptr;
        };
        const int pseq = makePeerStack(stkFiles, 5);

        {   // ---- R22: an EMPTY host is THIS DISK, and the rule is run here -----
            // §16.5's cost, asserted rather than claimed. Every Browse open of a
            // folder goes through recordRemoteRule, and the overwhelming
            // majority of them are LOCAL (`local://`, an empty host, the peer
            // serving this filesystem). Those frames stay on the local half by
            // §13.7, so the rebuild will re-apply siblingNamesAmong over a
            // directory_iterator - and recording the rule with the PEER's
            // grouper instead would ask a link for a grouping this machine is
            // about to compute, on every single open.
            //
            // The assertion IS the absence of that round trip: the resolver
            // handed in is peerDown, which answers null to everything. A rule
            // recorded through it cannot have asked the peer anything.
            App::SeqInfo local;
            local.remoteHost.clear();
            local.remotePort = 0;
            const bool got = recordRemoteRule(local, stkFiles, 0, peerDown);
            check(got && local.ruleHead == "w_000.npy" && local.ruleDir == stk,
                  "R22 an empty host records the rule from THIS disk, no request");
            // ANTI-VACUITY: the same files with a NAMED host DO go to the link,
            // so the same dead resolver records nothing. Without this pair, R22
            // passes on a function that ignores its resolver entirely.
            App::SeqInfo named = local;
            named.ruleDir.clear();
            named.ruleHead.clear();
            named.remoteHost = "kepler";
            const bool got2 = recordRemoteRule(named, stkFiles, 0, peerDown);
            check(!got2 && named.ruleHead.empty(),
                  "R22b ...while a named host asks the peer: a dead link records none");
        }
        {   // ---- R13: what the screen may OFFER for a remote finding -----------
            // §13.6 said the remote line could promise no [Reload], because
            // reloadSource refused a peer's pixels BY NAME and an offered button
            // would refuse every member and latch #56's mark onto a stack where
            // nothing failed. §16 gave reloadSource the peer's equivalent of
            // "stat it and read it again", so the refusal is gone and the line
            // says the thing that now works.
            FrameSource peerSrc;
            peerSrc.remoteUrl = "ssh://kepler/data/cap/w_000.npy";
            peerSrc.path = peerSrc.remoteUrl;
            check(reloadUnavailable(peerSrc).empty(),
                  "R13 [Reload] no longer refuses a peer's pixels by name");
            check(watchReloadOffered(pseq),
                  "R13b so the watch line DOES offer [Reload] for a peer's stack");
            check(std::string(watchActionText(pseq)) ==
                      " - Reload in Files to re-read them",
                  "R13c ...and names that button rather than sending them to Browse");
            // ANTI-VACUITY, both ways. The two refusals reloadSource still makes
            // are still made - dropping the remote row must not have dropped the
            // list - and a stack with no member the walk can serve still gets the
            // other sentence.
            FrameSource unit;                     // a container/reader member
            unit.path = stk + "/tree.npz";
            unit.member = "__pixels_1";
            FrameSource none;                     // a montage / plugin result
            check(!reloadUnavailable(unit).empty() && !reloadUnavailable(none).empty(),
                  "R13d ...while the other two refusals are untouched");
            check(!watchReloadOffered(seq) &&
                  std::string(watchActionText(seq)).find("Reload in Files") ==
                      std::string::npos,
                  "R13e ...and a stack with no reloadable member keeps the other line");
        }
        {   // ---- R16: §9's Auto-reload does NOT follow it there -----------------
            // §15.7 refused a peer's stack because the REBUILD could not reach
            // it. That reason is spent. The refusal stays, and §16.6 states the
            // new one: the act is a blocking round trip per member over a link
            // that can drop, repeatable every fifteen seconds, with nobody's
            // click behind it. A manual Reload pays that price knowingly.
            //
            // What makes this an assertion rather than a comment is that
            // watchReloadOffered now answers TRUE here: the peer clause is
            // demonstrably the thing doing the refusing, where before it could
            // have been the missing button.
            const bool wasAuto = app.watchAutoReload;
            app.watchAutoReload = true;
            if (App::SeqInfo* si = seqInfo(pseq)) {
                si->watchFound = watch::Finding{};
                si->watchFound.srcMoved = true;
                si->watchFound.host = "kepler";
                si->watchFound.members = 5;
            }
            check(seqInfo(pseq) && seqInfo(pseq)->watchFound.any() && app.watchAutoReload &&
                      watchReloadOffered(pseq) && !watchAutoBusy(pseq) &&
                      seqInfo(pseq)->reloadFailed == 0,
                  "R16 (setup) armed, auto ON, reloadable, and nothing else holds it");
            const char* why = watchAutoRefusal(pseq);
            check(why && std::string(why).find("link") != std::string::npos,
                  "R16b auto still refuses a peer, and the reason is now the ACT");
            check(watchAutoArmed().empty() && !watchAutoDue(),
                  "R16c ...so a standing finding never arms it");
            // ...and the Files line SAYS so. §15.7 suppressed this clause because
            // watchActionText already carried the peer refusal; now that line
            // offers the button, so the refusal has to be spoken here or the
            // stack looks like one the feature simply forgot.
            check(watchAutoNote(pseq).find("auto-reload cannot") != std::string::npos,
                  "R16d ...and the Files line says so, where it used to say nothing");
            if (App::SeqInfo* si = seqInfo(pseq)) si->watchFound = watch::Finding{};
            app.watchAutoReload = wasAuto;
        }
        {   // ---- R17: the LISTING is the peer's, and a gone file LEAVES --------
            // §13's own finding, spent: the group row aggregates (mtime, size)
            // and cannot say which member moved, but it carries the full member
            // NAME list - and names are exactly what a membership rebuild is made
            // of. planRebuild wants three name lists and the peer can answer two
            // of them, so it needs no new protocol either.
            rm(stk + "/" + nm(4));
            watch::Rebuild plan = planStackMembership(pseq, peerOf);
            check(plan.dropped.size() == 1 && plan.dropped[0] == "w_004.npy" &&
                      plan.added.empty() && plan.order.size() == 4,
                  "R17 the peer's rows answer the rebuild: one departure, named");
            int failed = -1;
            const std::string msg = reloadStackFromDisk(pseq, &failed, peerOf);
            fprintf(stderr, "rwatchselftest: R17 summary: %s\n", msg.c_str());
            check(members(pseq) == "0:w_000.npy 1:w_001.npy 2:w_002.npy 3:w_003.npy",
                  "R17b the vanished member is out and the survivors renumber 0..3");
            check(seqInfo(pseq) && seqInfo(pseq)->expectedFrames == 4 &&
                      seqInfo(pseq)->remoteFiles.size() == 4,
                  "R17c ...and the stack's OWN list of the peer's files moved with it");
            check(msg.find("1 file(s) no longer exist (w_004.npy)") != std::string::npos &&
                      msg.find("4 of 4 frame(s), was 5") != std::string::npos,
                  "R17d the summary states the NAME and BOTH counts (§12)");
            check(failed == 0 && seqInfo(pseq) && seqInfo(pseq)->reloadFailed == 0,
                  "R17e a gone file is not a refusal: no (reload failed) mark");
        }
        {   // ---- R18: the pixels really come back FROM THE PEER ----------------
            // The other half of a Reload, and the half §13.6 said was missing:
            // reloadSource re-reads the file. For a peer's source that is META +
            // TILE on the same connection the open used, not a local re-read of a
            // path that does not exist here.
            ImageDoc* d2 = memberDoc(pseq, 2);
            check(d2 && d2->px()[0] == 102.0f,
                  "R18 (setup) frame 2 holds the 102 the fixture wrote");
            const int rev0 = d2 ? d2->src->rev : -1;
            const int drev0 = d2 ? d2->dataRev : -1;
            writeNpy2(stk + "/" + nm(2), 4, 4, 902.0f);
            int failed = -1;
            reloadStackFromDisk(pseq, &failed, peerOf);
            ImageDoc* n2 = memberDoc(pseq, 2);
            check(failed == 0 && n2 && n2->px()[0] == 902.0f,
                  "R18b the peer's new bytes are in the frame after a Reload");
            check(n2 && n2->src->rev > rev0 && n2->dataRev > drev0,
                  "R18c ...through reloadSource: the source and doc revisions moved");
        }
        {   // ---- R19: NO ANSWER REBUILDS NOTHING -------------------------------
            // §13.4 made DETECTION fail towards silence. A rebuild has to fail
            // the same way and harder: an unreachable peer folded into an empty
            // listing would take every member out of the stack, and the sentence
            // it printed on the way ("4 file(s) no longer exist") would be a lie
            // about a capture that is sitting there.
            //
            // Asserted on planStackMembership DIRECTLY, and that is the whole
            // point: §12.5's "a rebuild never empties a stack" would zero the
            // plan afterwards, so a summary-only assertion passes on exactly the
            // implementation this case exists to forbid.
            rm(stk + "/" + nm(3));                       // a REAL deletion...
            watch::Rebuild silent = planStackMembership(pseq, peerDown);
            check(!silent.any() && silent.dropped.empty() && silent.added.empty(),
                  "R19 a peer that did not answer produces no plan at all");
            int failed = -1;
            const std::string msg = reloadStackFromDisk(pseq, &failed, peerDown);
            fprintf(stderr, "rwatchselftest: R19 summary: %s\n", msg.c_str());
            check(msg.find("no longer exist") == std::string::npos,
                  "R19b ...and the summary never says the files stopped existing");
            check(members(pseq) == "0:w_000.npy 1:w_001.npy 2:w_002.npy 3:w_003.npy",
                  "R19c ...and the membership is exactly what it was");
            check(failed == 4 && seqInfo(pseq) && seqInfo(pseq)->reloadOk == 0,
                  "R19d what failed is the RE-READ, and #56 says nothing was re-read");
            // ANTI-VACUITY: the same folder, the same deletion, over a live link.
            // Without this pair R19 is "the remote rebuild never does anything".
            watch::Rebuild live = planStackMembership(pseq, peerOf);
            check(live.dropped.size() == 1 && live.dropped[0] == "w_003.npy",
                  "R19e ...while a live link sees exactly that deletion");
            writeNpy2(stk + "/" + nm(3), 4, 4, 103.0f);  // put it back
            int f2 = -1;
            reloadStackFromDisk(pseq, &f2, peerOf);      // ...and clear #56's latch
            check(f2 == 0 && seqInfo(pseq) && seqInfo(pseq)->reloadFailed == 0,
                  "R19f a clean run over a live link clears the mark again");
        }
        {   // ---- R20: §12.2 over the link - the RULE is recorded at the mint ---
            // A stack may only re-admit a file that APPEARED when its membership
            // is its folder's whole group, and at Reload time that is not
            // decidable: "the folder gained one" and "this stack always excluded
            // two" look identical. Over a link it is worse, because a group row
            // cannot be narrowed at all (§13.5). So the peer's own grouper is run
            // ONCE, at the mint, and compared against the files handed in.
            App::SeqInfo* psi = seqInfo(pseq);
            const bool got = psi && recordRemoteRule(*psi, psi->remoteFiles, 0, peerOf);
            check(got && seqInfo(pseq) && seqInfo(pseq)->ruleHead == "w_000.npy" &&
                      seqInfo(pseq)->ruleDir == stk,
                  "R20 the peer's own grouper is recorded as this stack's rule");
            writeNpy2(stk + "/" + nm(5), 4, 4, 905.0f);
            int failed = -1;
            const std::string msg = reloadStackFromDisk(pseq, &failed, peerOf);
            fprintf(stderr, "rwatchselftest: R20 summary: %s\n", msg.c_str());
            check(members(pseq) == "0:w_000.npy 1:w_001.npy 2:w_002.npy "
                                   "3:w_003.npy 4:w_005.npy" &&
                      msg.find("1 file(s) appeared in the folder (w_005.npy)") !=
                          std::string::npos,
                  "R20b a file that appeared JOINS, in the open's own order");
            ImageDoc* n4 = memberDoc(pseq, 4);
            check(failed == 0 && n4 && n4->px()[0] == 905.0f,
                  "R20c ...and it was FETCHED from the peer, not invented");
            // The other half, and the one that costs a user their own choice if
            // it is wrong: a stack that is 3 of the folder's 5 records no rule
            // and can never be handed back the frames it excluded.
            std::vector<std::string> three(stkFiles.begin(), stkFiles.begin() + 3);
            const int sub = makePeerStack(three, 3);
            App::SeqInfo* ssi = seqInfo(sub);
            const bool got2 = ssi && recordRemoteRule(*ssi, three, 0, peerOf);
            check(!got2 && seqInfo(sub) && seqInfo(sub)->ruleHead.empty(),
                  "R20d a 3-of-5 subset of the peer's group records no rule");
            watch::Rebuild subPlan = planStackMembership(sub, peerOf);
            check(subPlan.added.empty() && subPlan.dropped.empty(),
                  "R20e ...so its rebuild admits nothing the peer's group holds");
            // ANTI-VACUITY: hand that same stack a rule and those same files DO
            // join. Without it R20e is "a subset stack is never rebuilt".
            if (App::SeqInfo* s2 = seqInfo(sub)) {
                s2->ruleDir = stk;
                s2->ruleHead = "w_000.npy";
            }
            watch::Rebuild subWith = planStackMembership(sub, peerOf);
            check(subWith.added.size() == 2 &&
                      subWith.added.front() == "w_003.npy" &&
                      subWith.added.back() == "w_005.npy",
                  "R20f ...while WITH a rule the other two members join it");
            if (App::SeqInfo* s2 = seqInfo(sub)) {
                s2->ruleDir.clear();
                s2->ruleHead.clear();
            }
            dropSeq(sub);
        }
        {   // ---- R21: RESIDENCY IS NOT MEMBERSHIP ------------------------------
            // A remote stack is allowed to be short of its own file list - the
            // memory budget fetches four of three hundred and the Files row says
            // "n of N". `have` is built from the frames that are HERE, so a
            // rebuild taken from it would re-issue expectedFrames and remoteFiles
            // as the four and delete two hundred and ninety-six files from the
            // stack without a word. A stack whose list it is not is not rebuilt.
            const std::string part = root + "/part";
            std::filesystem::create_directories(pathFromUtf8(part), ec);
            std::vector<std::string> pf;
            for (int i = 0; i < 4; i++) {
                char b[32];
                snprintf(b, sizeof b, "p_%03d.npy", i);
                pf.push_back(part + "/" + b);
                writeNpy2(pf.back(), 4, 4, 200.0f + (float)i);
            }
            const int ps = makePeerStack(pf, 3);         // 3 of 4 resident
            rm(pf[2]);                                   // ...and one of THOSE is gone
            check((int)framesOfSeq(ps).size() == 3 && seqInfo(ps) &&
                      seqInfo(ps)->remoteFiles.size() == 4,
                  "R21 (setup) a 3-of-4 resident peer stack, one resident file gone");
            watch::Rebuild partial = planStackMembership(ps, peerOf);
            check(!partial.any(),
                  "R21b a stack short of its own file list is not rebuilt at all");
            // ANTI-VACUITY: the very same stack, once its list IS its residency,
            // sees that deletion immediately.
            if (App::SeqInfo* si = seqInfo(ps))
                si->remoteFiles = { pf[0], pf[1], pf[2] };
            watch::Rebuild whole = planStackMembership(ps, peerOf);
            check(whole.dropped.size() == 1 && whole.dropped[0] == "p_002.npy",
                  "R21c ...while the same stack whose list it IS drops the gone one");
            dropSeq(ps);
        }

        dropSeq(pseq);
        dropSeq(seq);
        closeAll();
        ses.stop();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "rwatchselftest: %s\n", ok ? "ok" : "FAILED");
        stopWatchWorker();
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
