// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- Watch: source files that moved on disk (docs/watch-design.md §10) -----
    // Two halves, and the split is the point.
    //
    //   W1-W12  the RULE, with no filesystem and no clock: watch::observe fed
    //           readings by hand. Every expected number is written out beside
    //           its assert, because a watcher that lies lies about arithmetic
    //           this small - "is 50 different from 100?" is the whole of the
    //           not-equal rule, and it is the one a "newer?" test gets wrong.
    //   W13-W22 the same rule through the real path: real files, a real stack,
    //           the real directory listing, watchPollRound - the function the
    //           worker calls, called here without a thread.
    //
    // NOTHING HERE SLEEPS. The polling interval is 5 s (§2) and a test that
    // lived through two of them would take longer than the rest of the suite and
    // would fail on a loaded machine; watchPollRound was extracted from the
    // worker for exactly that reason (rbCursorFollow's precedent). The test
    // decides WHEN a poll happens, which is the only thing a wall clock was
    // ever deciding.
    if (!g_watchSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "watchselftest: %-66s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto E = [](const char* n, int64_t m, uint64_t s) {
            watch::Ent e; e.name = n; e.mtime = m; e.size = s; return e;
        };

        // ---- the baseline every W1-W12 case starts from ------------------------
        // Three members, three distinct sizes, one shared mtime. Chosen so that
        // every later reading differs from it in exactly ONE stated way, and the
        // way is named in the check.
        const watch::Obs BASE = { E("a", 100, 10), E("b", 100, 20), E("c", 100, 30) };

        {   // ---- W1: §1, the first poll IS the baseline and says nothing -------
            watch::SetState st;
            const bool spoke = watch::observe(st, BASE);
            check(!spoke && !st.found.any() && watch::findingText(st.found).empty(),
                  "W1 the first poll announces nothing (it IS the baseline)");
            // ANTI-VACUITY: a state machine that silently did nothing at all
            // would also pass the line above. It has to have TAKEN the baseline.
            check(st.haveBase && st.base.size() == 3 && st.conf.size() == 3 &&
                  st.base[0].name == "a" && st.base[0].size == 10 &&
                  st.base[2].name == "c" && st.base[2].size == 30,
                  "W1b ...and it recorded all 3 members as the baseline");
        }
        {   // ---- W2/W3: §4, a differing reading is a CANDIDATE, not a fact -----
            watch::SetState st;
            watch::observe(st, BASE);
            // "a" is rewritten: size 10 -> 11. ONE reading of it.
            const watch::Obs A1 = { E("a", 200, 11), E("b", 100, 20), E("c", 100, 30) };
            const bool spoke1 = watch::observe(st, A1);
            check(!spoke1 && !st.found.any(),
                  "W2 one differing reading says nothing (size read once)");
            // ...and the SAME reading again. 11 == 11 and 200 == 200, so the
            // member has now read identically twice: §4 is satisfied.
            const bool spoke2 = watch::observe(st, A1);
            check(spoke2 && st.found.changed == 1 && st.found.appeared == 0 &&
                  st.found.vanished == 0 && st.found.firstChanged == "a",
                  "W3 the same reading twice IS a fact: 1 changed, and it is \"a\"");
            check(watch::findingText(st.found) == "1 file(s) changed on disk (a)",
                  "W3b ...and the sentence the panel draws says exactly that");
            // A second poll with nothing new must NOT re-wake the UI (§3): the
            // statement did not change, so there is nothing to tell anyone.
            check(!watch::observe(st, A1) && st.found.changed == 1,
                  "W3c a repeat of a known finding does not wake the UI again");
        }
        {   // ---- W4: a file being WRITTEN is never grabbed (§4, the headline) --
            // 30 -> 31 -> 32 -> 33 -> 34: five readings, no two alike, which is
            // what a file growing under the poller looks like. Not one of them
            // may be announced.
            watch::SetState st;
            watch::observe(st, BASE);
            bool spokeEver = false;
            for (uint64_t sz = 31; sz <= 34; sz++)
                spokeEver |= watch::observe(st, { E("a", 100, 10), E("b", 100, 20),
                                                  E("c", 100 + (int64_t)sz, sz) });
            check(!spokeEver && !st.found.any() &&
                  watch::findingText(st.found).empty(),
                  "W4 a file still being written is NOT announced (4 growing reads)");
            // ANTI-VACUITY, and it is the important half: the SAME state machine,
            // one poll later, must speak the moment the writer stops. Without
            // this pair, a watcher that never says anything passes W4.
            const watch::Obs SETTLED = { E("a", 100, 10), E("b", 100, 20), E("c", 135, 35) };
            const bool s1 = watch::observe(st, SETTLED);   // 34 -> 35: still moving
            const bool s2 = watch::observe(st, SETTLED);   // 35 == 35: settled
            check(!s1 && s2 && st.found.changed == 1 && st.found.firstChanged == "c" &&
                  watch::findingText(st.found) == "1 file(s) changed on disk (c)",
                  "W4b ...and the same watcher speaks the instant the size repeats");
        }
        {   // ---- W5: NOT EQUAL, never NEWER (§1) -------------------------------
            // mtime 100 -> 50 (BACKWARDS by 50), size 10 -> 10 (IDENTICAL). A
            // watcher asking "is it newer?" computes 50 > 100 = false and reports
            // nothing; a watcher asking "is it equal?" computes (50,10) != (100,10)
            // = not equal and reports one change. This is a copy, an rsync or a
            // restore from backup - the commonest way a capture folder is refilled.
            watch::SetState st;
            watch::observe(st, BASE);
            const watch::Obs BACK = { E("a", 50, 10), E("b", 100, 20), E("c", 100, 30) };
            watch::observe(st, BACK);
            const bool spoke = watch::observe(st, BACK);
            check(spoke && st.found.changed == 1 && st.found.firstChanged == "a",
                  "W5 mtime moved BACKWARDS with the size unchanged is a change");
            // ANTI-VACUITY: "everything is a change" would also pass W5. The
            // identical reading, twice, must stay silent.
            watch::SetState st2;
            watch::observe(st2, BASE);
            watch::observe(st2, BASE);
            const bool spoke2 = watch::observe(st2, BASE);
            check(!spoke2 && !st2.found.any(),
                  "W5b ...and a reading EQUAL to the baseline is not a change");
        }
        {   // ---- W6: a file that disappeared -----------------------------------
            watch::SetState st;
            watch::observe(st, BASE);
            const watch::Obs GONE = { E("a", 100, 10), E("b", 100, 20) };
            const bool s1 = watch::observe(st, GONE);
            check(!s1 && !st.found.any(),
                  "W6 absent for ONE poll is not gone (a delete-then-write)");
            const bool s2 = watch::observe(st, GONE);
            check(s2 && st.found.vanished == 1 && st.found.changed == 0 &&
                  st.found.firstVanished == "c" &&
                  watch::findingText(st.found) == "1 file(s) no longer exist (c)",
                  "W6b absent TWICE is gone, and the sentence names it");
        }
        {   // ---- W7: a file that appeared ---------------------------------------
            watch::SetState st;
            watch::observe(st, BASE);
            watch::Obs PLUS = BASE;
            PLUS.push_back(E("d", 400, 40));
            const bool s1 = watch::observe(st, PLUS);
            check(!s1 && !st.found.any(),
                  "W7 a file seen ONCE is not yet a member (it may still be growing)");
            const bool s2 = watch::observe(st, PLUS);
            check(s2 && st.found.appeared == 1 && st.found.changed == 0 &&
                  st.found.firstAppeared == "d" &&
                  watch::findingText(st.found) == "1 file(s) appeared in the folder (d)",
                  "W7b seen twice at the same size, it appeared");
        }
        {   // ---- W8: a finding that stops being true goes away ------------------
            // ...and it takes two readings to go away, exactly as it took two to
            // arrive. The asymmetric version of this rule is what makes a line
            // FLICKER on a file that is rewritten in place.
            watch::SetState st;
            watch::observe(st, BASE);
            const watch::Obs A1 = { E("a", 200, 11), E("b", 100, 20), E("c", 100, 30) };
            watch::observe(st, A1);
            watch::observe(st, A1);
            check(st.found.changed == 1, "W8 (setup) the change is on the board");
            // the writer truncates and starts again: one reading that agrees with
            // nothing. The finding it already earned must NOT blink out.
            const bool s1 = watch::observe(st, { E("a", 300, 0), E("b", 100, 20),
                                                 E("c", 100, 30) });
            check(!s1 && st.found.changed == 1,
                  "W8b a mid-rewrite reading does not un-say a confirmed finding");
            // ...and now the file is restored to exactly what was loaded.
            const bool s2 = watch::observe(st, BASE);
            check(!s2 && st.found.changed == 1,
                  "W8c one reading equal to the baseline does not clear it either");
            const bool s3 = watch::observe(st, BASE);
            check(s3 && !st.found.any() && watch::findingText(st.found).empty(),
                  "W8d TWO such readings do: the statement stopped being true");
        }
        {   // ---- W9: per MEMBER, not per set -------------------------------------
            // "b" settles at a new size while "c" is still growing. The finding
            // about b must not wait for c to stop - and c must not be counted.
            watch::SetState st;
            watch::observe(st, BASE);
            watch::observe(st, { E("a", 100, 10), E("b", 150, 21), E("c", 101, 31) });
            const bool spoke = watch::observe(st,
                                { E("a", 100, 10), E("b", 150, 21), E("c", 102, 32) });
            check(spoke && st.found.changed == 1 && st.found.firstChanged == "b",
                  "W9 a settled member is announced while its neighbour still grows");
        }
        {   // ---- W10: three kinds at once, and the sentence lists them ----------
            watch::SetState st;
            watch::observe(st, BASE);
            // a changed (10 -> 12), c gone, d appeared. b untouched.
            const watch::Obs MIX = { E("a", 200, 12), E("b", 100, 20), E("d", 400, 40) };
            watch::observe(st, MIX);
            watch::observe(st, MIX);
            check(st.found.changed == 1 && st.found.appeared == 1 && st.found.vanished == 1,
                  "W10 changed / appeared / vanished are counted separately");
            check(watch::findingText(st.found) ==
                    "1 file(s) changed on disk (a), 1 file(s) appeared in the folder (d), "
                    "1 file(s) no longer exist (c)",
                  "W10b ...and the one sentence both the panel and this test read");
        }
        {   // ---- W11: the counts are counts ---------------------------------------
            // All three of a/b/c rewritten: 10->11, 20->21, 30->31. Three, not one,
            // and the name shown is the FIRST by name rather than whichever the
            // filesystem happened to hand over first.
            watch::SetState st;
            watch::observe(st, BASE);
            const watch::Obs ALL = { E("c", 200, 31), E("a", 200, 11), E("b", 200, 21) };
            watch::observe(st, ALL);
            watch::observe(st, ALL);
            check(st.found.changed == 3 && st.found.firstChanged == "a" &&
                  watch::findingText(st.found) == "3 file(s) changed on disk",
                  "W11 3 rewritten files count 3, and >1 names none of them");
        }
        {   // ---- W12: §1's other baseline - a stack open before Watch existed ---
            // No seed: the FIRST poll makes the baseline. A change that happened
            // BEFORE that poll is therefore invisible forever, and that is the
            // rule, not a defect - the alternative is to forge a claim about the
            // moment the stack was opened, which nobody observed.
            watch::SetState st;
            const watch::Obs ALREADY = { E("a", 999, 99), E("b", 100, 20), E("c", 100, 30) };
            check(!watch::observe(st, ALREADY) && !st.found.any(),
                  "W12 no baseline: the first poll takes one and says nothing");
            check(!watch::observe(st, ALREADY) && !st.found.any(),
                  "W12b ...and a change from BEFORE it stays invisible, on purpose");
            // ANTI-VACUITY: the same set, changed AFTER the baseline was taken,
            // is seen normally. Without this, "never says anything" passes W12.
            const watch::Obs AFTER = { E("a", 999, 99), E("b", 100, 20), E("c", 700, 77) };
            watch::observe(st, AFTER);
            check(watch::observe(st, AFTER) && st.found.changed == 1 &&
                  st.found.firstChanged == "c",
                  "W12c ...while a change AFTER that first poll is seen normally");
        }

        // ======================================================================
        //  W13-W22: the same rule through real files, a real stack and the real
        //  directory listing - watchPollRound, the function the worker calls.
        // ======================================================================
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        // One 2-D f32 .npy, w x h, every pixel = v. Its size on disk is
        // arithmetic and W14 asserts the arithmetic:
        //   8 (magic+version) + 2 (header length) + header, padded so the whole
        //   header region is a multiple of 64 -> 128 bytes for either shape here
        //   (the dict differs only inside "(4, 4)" / "(6, 6)", same length)
        //   + w*h*4 bytes of float32.
        // So 4x4 = 128 + 64 = 192 bytes and 6x6 = 128 + 144 = 272 bytes.
        auto writeNpy2 = [](const std::string& path, int w, int h, float v) {
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }", h, w);
            std::string hdr = dict;
            size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
            hdr.append(pad, ' ');
            hdr += '\n';
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            uint16_t hl = (uint16_t)hdr.size();
            f.write("\x93NUMPY\x01\x00", 8);
            f.write((const char*)&hl, 2);
            f.write(hdr.data(), (std::streamsize)hdr.size());
            std::vector<float> px((size_t)w * h, v);
            f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        };
        auto fsizeOf = [](const std::string& p) {
            std::error_code ec;
            auto n = std::filesystem::file_size(pathFromUtf8(p), ec);
            return ec ? (uintmax_t)0 : n;
        };
        // Move a file's mtime by a whole number of hours. Used instead of
        // waiting for the clock: the readings this test needs must differ (or
        // not) by an amount it chose, not by whatever the filesystem's timestamp
        // resolution happens to be this decade.
        auto bumpMtime = [](const std::string& p, int hours) {
            std::error_code ec;
            auto q = pathFromUtf8(p);
            auto t = std::filesystem::last_write_time(q, ec);
            if (ec) return;
            std::filesystem::last_write_time(q, t + std::chrono::hours(hours), ec);
        };

        // the scratch area is a subdirectory this test CREATES, never the one it
        // was handed (the derive test's remove_all lesson)
        std::string root = g_watchSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/watch-scratch";
        const std::string stk = root + "/stk";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(stk), ec);
        for (int i = 0; i < 5; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "w_%03d.npy", i);
            writeNpy2(stk + "/" + nm, 4, 4, (float)i);
        }
        app.seqLoadMode = 1;
        closeAll();
        openFolder(stk);
        loadAll();

        int seq = 0;
        for (const auto& s : app.seqs) if (!s.name.empty()) { seq = s.id; break; }
        check(seq != 0 && (int)framesOfSeq(seq).size() == 5,
              "W13 (setup) a 5-frame local stack is open");

        std::map<int, WatchSet> states;
        std::vector<App::WatchDone> out;
        // One poll, exactly as watchWorker performs it - and with the target
        // list built exactly as pumpWatch builds it.
        auto poll = [&]() {
            out.clear();
            return watchPollRound(watchTargetsNow(), states, out);
        };
        // ...and the UI thread's half: hand the findings to the queue the worker
        // would have filled, then run the integration the frame runs. No thread
        // is started - pumpWatch's other half (publish targets, spawn the
        // worker) is what --watch-selftest deliberately does not do, because a
        // worker polling on its own 5 s timer would race this test for the queue.
        auto integrate = [&]() {
            {
                std::lock_guard<std::mutex> lk(app.watchMtx);
                app.watchDone = out;
            }
            const bool pending = watchFindingsPending();
            watchIntegrate();
            return pending;
        };

        {   // ---- W14: the target, and the baseline the LOADER recorded ---------
            std::vector<App::WatchTarget> tg = watchTargetsNow();
            const App::WatchTarget* t = nullptr;
            for (const auto& x : tg) if (x.seqId == seq) t = &x;
            check(t && t->seed.size() == 5 && t->file.empty() && !t->dir.empty() &&
                  t->headName == "w_000.npy",
                  "W14 the stack is one watched set: a folder, 5 seeded members");
            // The seed is FrameSource's own stat, so it is the size on disk. 192
            // is derived above: 128-byte header region + 4*4*4 bytes of float32.
            check(t && t->seed[0].name == "w_000.npy" && t->seed[0].size == 192 &&
                  fsizeOf(stk + "/w_000.npy") == 192,
                  "W14b ...and that seed is the file's real size (128 + 4*4*4 = 192)");
            // A stack spanning TWO directories is not watched at all. The whole
            // mechanism is the sibling rule over ONE folder's listing (§1), and
            // members living elsewhere would be absent from every observation -
            // which §4 would confirm, correctly by its own lights, as VANISHED.
            // A false sentence about a stack whose files are all present is
            // worse than no sentence.
            //
            // Built by pointing one member's source at a copy of itself in
            // another folder, which is what the guard actually reads, and put
            // back immediately.
            std::error_code ec2;
            std::filesystem::create_directories(pathFromUtf8(root + "/elsewhere"), ec2);
            writeNpy2(root + "/elsewhere/w_004.npy", 4, 4, 4.0f);
            const std::vector<int> fr = framesOfSeq(seq);
            const std::string was = app.images[fr.back()]->src->path;
            app.images[fr.back()]->src->path = root + "/elsewhere/w_004.npy";
            bool stillWatched = false;
            for (const auto& x : watchTargetsNow()) if (x.seqId == seq) stillWatched = true;
            app.images[fr.back()]->src->path = was;
            bool backAgain = false;
            for (const auto& x : watchTargetsNow()) if (x.seqId == seq) backAgain = true;
            // ...and the pair: the SAME stack with every member in one folder IS
            // watched, or the line above is only "watch nothing".
            check(!stillWatched && backAgain,
                  "W14c a stack spanning two directories is not watched at all");
        }
        {   // ---- W15: nothing has moved, so nothing is said, ever ---------------
            // ...which is also the IDLE COST assertion (§3). The worker's return
            // value is the ONLY thing that makes it call glfwPostEmptyEvent, and
            // watchFindingsPending() is the only term this feature adds to the
            // idle-skip chain: both false here means four polls over a quiet
            // folder cost the UI thread not one frame.
            bool spokeEver = false;
            for (int k = 0; k < 4; k++) spokeEver |= poll();
            check(!spokeEver && out.empty() && seqWatchNote(seq).empty() &&
                  !watchFindingsPending(),
                  "W15 four polls over an untouched folder wake the UI zero times");
        }
        {   // ---- W16/W17: a frame is rewritten under the live stack -------------
            // 4x4 -> 6x6 is 192 -> 272 bytes on disk (the arithmetic above), so
            // this is a real (mtime, size) difference and not a timestamp race.
            writeNpy2(stk + "/w_002.npy", 6, 6, 42.0f);
            check(fsizeOf(stk + "/w_002.npy") == 272,
                  "W16 (setup) the rewritten frame is 272 bytes (128 + 6*6*4)");
            const bool s1 = poll();
            check(!s1 && out.empty() && seqWatchNote(seq).empty(),
                  "W16b the first poll after the write says nothing (§4: read once)");
            const bool s2 = poll();
            check(s2 && out.size() == 1 && out[0].seqId == seq &&
                  out[0].found.changed == 1 && out[0].found.firstChanged == "w_002.npy",
                  "W17 the second poll confirms it: 1 changed, named");
        }
        {   // ---- W18: the finding reaches SeqInfo and the panel's sentence ------
            // pumpWatch's half, without a frame: it is the UI thread copying the
            // worker's summary onto the stack and firing the one toast (§5).
            check(integrate(),
                  "W18 a queued finding makes watchFindingsPending() true (§3's line)");
            check(seqWatchNote(seq) == "1 file(s) changed on disk (w_002.npy)",
                  "W18b the Files panel's amber line says exactly that");
            App::SeqInfo* si = seqInfo(seq);
            check(si && si->watchToasted && si->watchFound.changed == 1,
                  "W18c ...and the one toast (§5) has fired, once");
        }
        {   // ---- W19: Reload clears it, and the worker agrees on its own --------
            // The pixels are re-read, so reloadSource re-stats the file and
            // FrameSource's baseline MOVES - which is the whole mechanism by
            // which the line goes away with no message from the UI to the worker.
            int failed = 0;
            reloadStackFromDisk(seq, &failed);
            if (App::SeqInfo* si = seqInfo(seq)) {
                si->watchFound = watch::Finding{};
                si->watchToasted = false;
            }
            check(failed == 0 && seqWatchNote(seq).empty(),
                  "W19 after Reload the line is gone (the pixels are today's)");
            bool spokeEver = false;
            for (int k = 0; k < 3; k++) spokeEver |= poll();
            check(!spokeEver && seqWatchNote(seq).empty(),
                  "W19b ...and three more polls re-seed silently, no second finding");
        }
        {   // ---- W20: a file that appears, and one that disappears --------------
            writeNpy2(stk + "/w_005.npy", 4, 4, 5.0f);
            std::filesystem::remove(pathFromUtf8(stk + "/w_004.npy"), ec);
            const bool s1 = poll();
            check(!s1 && seqWatchNote(seq).empty(),
                  "W20 one poll after an add+delete says nothing (§4 both ways)");
            const bool s2 = poll();
            integrate();
            check(s2 && seqWatchNote(seq) ==
                    "1 file(s) appeared in the folder (w_005.npy), "
                    "1 file(s) no longer exist (w_004.npy)",
                  "W20b the second poll names both, in one line");
        }
        {   // ---- W21: mtime BACKWARDS on the real filesystem ---------------------
            // The same bytes with an older timestamp: a restore from backup. The
            // size does not move at all, so only the not-equal rule can see it.
            const uintmax_t before = fsizeOf(stk + "/w_001.npy");
            bumpMtime(stk + "/w_001.npy", -48);          // two days into the past
            check(fsizeOf(stk + "/w_001.npy") == before && before == 192,
                  "W21 (setup) the size did not move: 192 bytes before and after");
            poll();
            const bool s2 = poll();
            integrate();
            check(s2 && seqInfo(seq) && seqInfo(seq)->watchFound.changed == 1 &&
                  seqInfo(seq)->watchFound.firstChanged == "w_001.npy",
                  "W21b an OLDER file with the same size is still a change");
        }
        {   // ---- W22: §1's fallback, end to end ---------------------------------
            // A stack whose sources carry no recorded stat (opened before Watch
            // existed) has no seed, so its baseline is its FIRST poll - and a
            // file already rewritten by then is invisible, on purpose.
            std::vector<App::WatchTarget> tg = watchTargetsNow();
            std::vector<App::WatchTarget> noSeed;
            for (auto x : tg) if (x.seqId == seq) { x.seed.clear(); noSeed.push_back(x); }
            check(noSeed.size() == 1, "W22 (setup) one target, with its seed removed");
            writeNpy2(stk + "/w_003.npy", 6, 6, 9.0f);   // changed BEFORE poll 1
            std::map<int, WatchSet> st2;
            std::vector<App::WatchDone> o2;
            bool spokeEver = false;
            for (int k = 0; k < 3; k++) {
                o2.clear();
                spokeEver |= watchPollRound(noSeed, st2, o2);
            }
            check(!spokeEver && o2.empty(),
                  "W22b no seed: the first poll is the baseline, and it says nothing");
            // ANTI-VACUITY: the same three-poll loop on a file changed AFTER that
            // first poll must speak. Without it, "never says anything" passes W22b.
            writeNpy2(stk + "/w_000.npy", 6, 6, 8.0f);
            bool spokeAfter = false;
            for (int k = 0; k < 2; k++) {
                o2.clear();
                spokeAfter |= watchPollRound(noSeed, st2, o2);
            }
            check(spokeAfter && o2.size() == 1 && o2[0].found.changed == 1 &&
                  o2[0].found.firstChanged == "w_000.npy",
                  "W22c ...and a change AFTER that baseline is seen, normally");
        }
        {   // ---- W23: a SUBSET stack is not told about the frames it excluded ---
            // A derived stack (Files > "Derive stack (a subset, by reference)")
            // shares its source's directory, and its membership was a decision
            // made at derive time that §6 says is never re-applied. Watching it
            // over the whole folder group would tell it, for as long as it is
            // open, that the frames its own rule left out have "appeared".
            //
            // The situation is built directly rather than through the modal: a
            // target whose seed names 3 of the folder's files is exactly what a
            // 3-of-5 derive produces, and it is the seed that the rule reads.
            //
            // A reload first, so the baseline this case is about is the ONLY
            // difference on the table: W20-W22 left three of these files
            // rewritten on disk, and a subset that starts out already changed
            // would answer W23b with W17's finding.
            //
            // That reload now REBUILDS this stack's membership (§6), and this
            // folder is exactly the interesting case: w_004.npy was deleted in
            // W20 (so it leaves, and refuses nothing - a gone frame is not a
            // failed read) while w_005.npy appeared (so the rule names it) but
            // is 4x4 where W22 left this stack's reference frame at 6x6 - so it
            // is refused BY SHAPE, precisely as the loader refuses a sibling of
            // another shape at the open. The stack is therefore 4 of 5: four
            // members, five names, and the Files row says so.
            int reFailed = -1;
            const std::string reMsg = reloadStackFromDisk(seq, &reFailed);
            fprintf(stderr, "watchselftest: W23 rebuild: %s\n", reMsg.c_str());
            check(reFailed == 0 && (int)framesOfSeq(seq).size() == 4 &&
                  seqInfo(seq) && seqInfo(seq)->expectedFrames == 5 &&
                  reMsg.find("1 file(s) no longer exist (w_004.npy)") != std::string::npos &&
                  reMsg.find("1 file(s) the rule names did not read (w_005.npy") !=
                      std::string::npos,
                  "W23 (setup) the rebuild drops the deleted file and says 4 of 5");
            check(!*seqReloadMark(seq),
                  "W23a ...and neither event is a (reload failed): no seam was made");
            std::vector<App::WatchTarget> tg = watchTargetsNow();
            App::WatchTarget sub;
            for (const auto& x : tg) if (x.seqId == seq) sub = x;
            watch::Obs keep;
            for (const watch::Ent& e : sub.seed)
                if (e.name == "w_000.npy" || e.name == "w_001.npy" || e.name == "w_002.npy")
                    keep.push_back(e);
            sub.seed = keep;
            check(sub.seed.size() == 3 && !sub.dir.empty(),
                  "W23 (setup) a 3-of-5 subset of the same folder");
            std::map<int, WatchSet> st3;
            std::vector<App::WatchDone> o3;
            bool spokeEver = false;
            for (int k = 0; k < 4; k++) {
                o3.clear();
                spokeEver |= watchPollRound({ sub }, st3, o3);
            }
            check(!spokeEver && o3.empty() && !st3[seq].wholeGroup,
                  "W23b the other 2 files in the folder are NOT \"appeared\" for it");
            // ANTI-VACUITY: one of ITS OWN three members changing must still be
            // reported, or the rule above is just "say nothing about subsets".
            writeNpy2(stk + "/w_001.npy", 6, 6, 3.0f);
            bool spokeAfter = false;
            for (int k = 0; k < 2; k++) {
                o3.clear();
                spokeAfter |= watchPollRound({ sub }, st3, o3);
            }
            check(spokeAfter && o3.size() == 1 && o3[0].found.changed == 1 &&
                  o3[0].found.appeared == 0 && o3[0].found.firstChanged == "w_001.npy",
                  "W23c ...while a change to one of its OWN 3 is reported normally");
        }
        {   // ---- W24: switching Watch OFF drains the queue -----------------------
            // The one way this feature could cost the idle loop everything it
            // promised (§3): a finding queued just before the switch is thrown
            // would sit there for the rest of the session, and
            // watchFindingsPending() is IN the idle-skip chain - so `working`
            // would be permanently true and the 0 fps idle would become a
            // continuous 12 fps. Turning the watcher off does not un-change the
            // files, so the finding is integrated rather than dropped.
            {
                std::lock_guard<std::mutex> lk(app.watchMtx);
                app.watchDone.clear();
                App::WatchDone d;
                d.seqId = seq;
                d.found.changed = 1;
                d.found.firstChanged = "w_002.npy";
                app.watchDone.push_back(d);
            }
            check(watchFindingsPending(), "W24 (setup) a finding is sitting in the queue");
            app.watchEnabled = false;
            pumpWatch();
            check(!watchFindingsPending() && !app.watchThread.joinable(),
                  "W24b Watch switched OFF drains the queue (else the idle loop spins)");
            check(seqWatchNote(seq) == "1 file(s) changed on disk (w_002.npy)",
                  "W24c ...by INTEGRATING it: the files changed either way");
            app.watchEnabled = true;
        }

        // ======================================================================
        //  W25-W33: §6 - the Reload that rebuilds MEMBERSHIP.
        //
        //  W1-W24 are about SAYING that the folder moved. These are about acting
        //  on it, and the sentence under test is §6's own: Reload is
        //  reloadSource PLUS a membership rebuild, and seqId and SeqInfo survive
        //  it. "Rebuilt as a stack" (ruling A6b) is a statement about the member
        //  LIST - the name, the per-frame X axis, the series membership and the
        //  compare pins belong to the same stack before and after.
        //
        //  Its own fixture folder, for R25's reason: W16-W23 spent 150 lines
        //  rewriting w_* into 6x6 and deleting one of them, and a membership
        //  test that has to be read against that history is a test nobody can
        //  check by eye.
        // ======================================================================
        closeAll();
        const std::string reb = root + "/reb";
        std::filesystem::create_directories(pathFromUtf8(reb), ec);
        auto rebName = [](int i) {
            char nm[32];
            snprintf(nm, sizeof nm, "r_%03d.npy", i);
            return std::string(nm);
        };
        auto rebWrite = [&](int i, float v) { writeNpy2(reb + "/" + rebName(i), 4, 4, v); };
        auto rebKill = [&](int i) {
            std::error_code e2;
            std::filesystem::remove(pathFromUtf8(reb + "/" + rebName(i)), e2);
        };
        for (int i = 0; i < 5; i++) rebWrite(i, 100.0f + (float)i);
        openFolder(reb);
        loadAll();
        int rs = 0;
        for (const auto& s : app.seqs) if (!s.name.empty()) { rs = s.id; break; }
        // THE membership, as ONE string: "0:r_000.npy 1:r_001.npy ...". A count
        // is not a membership - a stack that swapped one member for another has
        // the same count and is not the same stack - so every assert below
        // states the whole list and the number each member carries.
        auto members = [&](int sid) {
            std::string s;
            for (int idx : framesOfSeq(sid))          // framesOfSeq sorts by seqIndex
                s += (s.empty() ? "" : " ") + std::to_string(app.images[idx]->seqIndex) +
                     ":" + baseName(app.images[idx]->src->path);
            return s;
        };
        auto memberDoc = [&](int sid, int index) -> ImageDoc* {
            for (int idx : framesOfSeq(sid))
                if (app.images[idx]->seqIndex == index) return app.images[idx].get();
            return nullptr;
        };
        double sig5 = 0;
        {   // ---- W25 (setup): five frames, and the sigma_t they answer ---------
            check(rs != 0 && members(rs) == "0:r_000.npy 1:r_001.npy 2:r_002.npy "
                                            "3:r_003.npy 4:r_004.npy" &&
                  seqInfo(rs) && seqInfo(rs)->expectedFrames == 5,
                  "W25 (setup) a 5-frame folder stack, members named and numbered");
            App::TemporalState T{};
            if (ImageDoc* d0 = memberDoc(rs, 0)) recomputeTemporalIfNeeded(d0, T);
            sig5 = T.tempNoise[0];
            // Five flat frames, 100..104. Deviations from the mean 102 are
            // -2,-1,0,1,2, so the sum of squares is 4+1+0+1+4 = 10 and ddof=1
            // divides by 4: sqrt(10/4) = 1.5811388.
            check(fabs(sig5 - 1.5811388) < 1e-5,
                  "W25b (setup) sigma_t over the five frames is sqrt(10/4)");
        }
        {   // ---- W26: a member whose FILE IS GONE leaves the membership --------
            rebKill(3);
            int failed = -1;
            const std::string msg = reloadStackFromDisk(rs, &failed);
            fprintf(stderr, "watchselftest: W26 summary: %s\n", msg.c_str());
            check(members(rs) == "0:r_000.npy 1:r_001.npy 2:r_002.npy 3:r_004.npy",
                  "W26 the vanished member is out and the survivors renumber 0..3");
            check(seqInfo(rs) && seqInfo(rs)->expectedFrames == 4,
                  "W26b n of N states the new N (the Files row reads 4f, not 4/5f)");
            check(msg.find("1 file(s) no longer exist (r_003.npy)") != std::string::npos &&
                  msg.find("4 of 4 frame(s), was 5") != std::string::npos,
                  "W26c the summary states the NAME and BOTH counts");
            // A frame that is GONE is not a frame that FAILED TO READ (#56): what
            // remains is a stack of one moment, so it carries no seam mark.
            check(failed == 0 && seqReloadNote(rs).empty() && !*seqReloadMark(rs),
                  "W26d a gone file is not a refusal: no (reload failed) mark");
            // ANTI-VACUITY: the doc is really gone from app.images, not merely
            // missing from one listing.
            int stillThere = 0;
            for (const auto& d : app.images)
                if (baseName(d->src->path) == "r_003.npy") stillThere++;
            check(stillThere == 0, "W26e ...and no doc anywhere still holds r_003.npy");
        }
        {   // ---- W27: what is keyed on the stack follows the membership --------
            App::TemporalState T{};
            if (ImageDoc* d0 = memberDoc(rs, 0)) recomputeTemporalIfNeeded(d0, T);
            // The four survivors are 100,101,102,104: mean 101.75, deviations
            // -1.75,-0.75,0.25,2.25, sum of squares 3.0625+0.5625+0.0625+5.0625
            // = 8.75, ddof=1 divides by 3: sqrt(8.75/3) = 1.7078251.
            check(fabs(T.tempNoise[0] - 1.7078251) < 1e-5 &&
                  fabs(T.tempNoise[0] - sig5) > 1e-3,
                  "W27 sigma_t is the FOUR-frame answer, not the cached five");
        }
        {   // ---- W28: a member that APPEARS joins in the stack's own order -----
            // ANTI-VACUITY first: this case is only about an ARRIVAL if the
            // stack does not already hold r_003 - on a Reload that never changed
            // membership, W28 below would be asserting that a member which never
            // left is still there.
            check((int)framesOfSeq(rs).size() == 4 && !memberDoc(rs, 4),
                  "W28 (setup) the stack is down to four members, none of them r_003");
            rebWrite(3, 903.0f);                  // r_003 is back, with new pixels
            int failed = -1;
            const std::string msg = reloadStackFromDisk(rs, &failed);
            fprintf(stderr, "watchselftest: W28 summary: %s\n", msg.c_str());
            // If it were appended, this would read "...3:r_004.npy 4:r_003.npy".
            // sortFramesNumerically is the open's own order and the frame number
            // is a measurement property, so r_003 is frame 3 and r_004 moves back
            // to 4.
            check(members(rs) == "0:r_000.npy 1:r_001.npy 2:r_002.npy "
                                 "3:r_003.npy 4:r_004.npy",
                  "W28 the new file is frame 3, not frame 4 (the open's order)");
            ImageDoc* n3 = memberDoc(rs, 3);
            check(n3 && n3->px()[0] == 903.0f && n3->w == 4 && n3->h == 4,
                  "W28b ...and it was really decoded: the file's new pixels");
            check(failed == 0 && seqInfo(rs) && seqInfo(rs)->expectedFrames == 5 &&
                  msg.find("1 file(s) appeared in the folder (r_003.npy)") !=
                      std::string::npos &&
                  msg.find("5 of 5 frame(s), was 4") != std::string::npos,
                  "W28c the summary names it and states both counts again");
        }
        {   // ---- W29: one gone and one arrived - the COUNT does not move -------
            // The case a count-only test cannot see, and the one that proves the
            // stack's key is not the frame count: five members before, five
            // after, and NOT the same five.
            const int rev0 = seqInfo(rs) ? seqInfo(rs)->stackRev : -1;
            App::TemporalState T0{};
            if (ImageDoc* d0 = memberDoc(rs, 0)) recomputeTemporalIfNeeded(d0, T0);
            // 100,101,102,903,104: mean 262, squares 26244+25921+25600+410881+
            // 24964 = 513610, /4 = 128402.5, sqrt = 358.33295.
            check(fabs(T0.tempNoise[0] - 358.33295) < 1e-3,
                  "W29 (setup) sigma_t of the five members now on the board");
            rebKill(0);
            rebWrite(5, 5000.0f);
            int failed = -1;
            const std::string msg = reloadStackFromDisk(rs, &failed);
            fprintf(stderr, "watchselftest: W29 summary: %s\n", msg.c_str());
            check(members(rs) == "0:r_001.npy 1:r_002.npy 2:r_003.npy "
                                 "3:r_004.npy 4:r_005.npy",
                  "W29b five before and five after, and not the same five");
            check(msg.find("1 file(s) appeared in the folder (r_005.npy)") !=
                      std::string::npos &&
                  msg.find("1 file(s) no longer exist (r_000.npy)") != std::string::npos &&
                  msg.find("5 of 5 frame(s), was 5") != std::string::npos,
                  "W29c the summary names BOTH directions - the count says nothing");
            check(seqInfo(rs) && seqInfo(rs)->stackRev > rev0,
                  "W29d the stack's data revision moved on a same-size rebuild");
            App::TemporalState T1{};
            if (ImageDoc* d0 = memberDoc(rs, 0)) recomputeTemporalIfNeeded(d0, T1);
            // 101,102,903,104,5000: mean 1242, squares 1301881+1299600+114921+
            // 1295044+14122564 = 18134010, /4 = 4533502.5, sqrt = 2129.2023.
            check(fabs(T1.tempNoise[0] - 2129.2023) < 1e-3 && T1.frames == 5,
                  "W29e ...and sigma_t is the new set's, at the same frame count");
        }
        int ds = 0;
        {   // ---- W30: a DERIVED stack does not re-admit what its rule left out -
            // §6 and §11.4: a derive's membership is a decision made at derive
            // time and is never re-applied. It lives in the same folder as its
            // source, so a rebuild that asked the FOLDER instead of the STACK
            // would hand back the two frames the user deliberately excluded.
            DerivePlan P = buildDerivePlan(rs, DR_RANGE, 0, 0, 2, nullptr);
            ds = applyDerivePlan(P, false);
            check(ds != 0 && members(ds) == "0:r_001.npy 1:r_002.npy 2:r_003.npy",
                  "W30 (setup) a 3-of-5 derived stack over the same folder");
            int failed = -1;
            const std::string msg = reloadStackFromDisk(ds, &failed);
            fprintf(stderr, "watchselftest: W30 summary: %s\n", msg.c_str());
            check(failed == 0 && members(ds) == "0:r_001.npy 1:r_002.npy 2:r_003.npy" &&
                  msg.find("appeared in the folder") == std::string::npos,
                  "W30b Reload does not re-apply the folder rule to a subset stack");
            // ANTI-VACUITY: "never change a derived stack" would pass W30b. Its
            // OWN members are re-read like anybody's.
            rebWrite(2, 777.0f);
            int f2 = -1;
            reloadStackFromDisk(ds, &f2);
            ImageDoc* dm = memberDoc(ds, 1);
            check(f2 == 0 && dm && dm->px()[0] == 777.0f,
                  "W30c ...while its own members are re-read normally");
        }
        {   // ---- W31: a derived stack sees the new pixels with NO Reload of its
            // own. §10 calls this the reason the feature exists: one reload of
            // the source stack, and every stack sharing those frames is current
            // in the same step - there is no ledger of which copy is stale,
            // because there is no copy.
            requestStackAverage(ds);
            loadAll();
            ImageDoc* avg = app.images.empty() ? nullptr : app.images.back().get();
            check(avg && !avg->avgOfPath.empty() && !avgIsStale(avg),
                  "W31 (setup) a frame average of the derived stack, fresh");
            const int dsRev = seqInfo(ds) ? seqInfo(ds)->stackRev : -1;
            rebWrite(2, 888.0f);
            int f = -1;
            reloadStackFromDisk(rs, &f);          // the SOURCE stack, not this one
            ImageDoc* dm = memberDoc(ds, 1);
            check(f == 0 && dm && dm->px()[0] == 888.0f,
                  "W31b the derived stack has the new pixels, unreloaded");
            check(seqInfo(ds) && seqInfo(ds)->stackRev > dsRev,
                  "W31c ...and ITS stack key moved, so its numbers unlatch too");
            check(avg && avgIsStale(avg),
                  "W31d ...and the reduced frame of it reads (stale)");
        }
        {   // ---- W32: the compare pins and the selection across a rebuild ------
            // The decision: a pin is a pin on THIS FRAME (a membership uid),
            // never on "whatever is at index 2 now". So a surviving pin is
            // untouched even when its frame NUMBER moves, and a pin on the
            // member that vanished is dropped exactly as closing that frame
            // drops it. Promoting the frame that inherited the number would
            // silently compare against a different picture.
            ImageDoc* keep = memberDoc(rs, 2);            // r_003.npy, survives
            const uint64_t keepUid = keep ? keep->uid : 0;
            setCompareB(keep);
            for (size_t k = 0; k < app.images.size(); k++)
                if (app.images[k].get() == keep) selectImage((int)k);
            rebKill(1);                                   // r_002.npy: NOT the pin
            int f = -1;
            reloadStackFromDisk(rs, &f);
            ImageDoc* after = nullptr;
            for (const auto& d : app.images) if (d->uid == keepUid) after = d.get();
            check(app.compareBUid == keepUid && after && after->seqIndex == 1 &&
                  baseName(after->src->path) == "r_003.npy",
                  "W32 a pin survives a rebuild that renumbered the frame it holds");
            check(app.current >= 0 && app.current < (int)app.images.size() &&
                  app.images[app.current]->uid == keepUid,
                  "W32b ...and so does the selection: the same frame, new number");
            // ...and now the pinned frame is the one that goes.
            rebKill(3);                                   // r_003.npy IS the pin
            int f2 = -1;
            reloadStackFromDisk(rs, &f2);
            check(app.compareBUid == 0 && app.compareB.empty(),
                  "W32c a pin on the frame that vanished is dropped, not moved");
            check(app.current >= 0 && app.current < (int)app.images.size() &&
                  app.images[app.current]->seqId == rs,
                  "W32d ...and the selection lands on a live frame of the stack");
        }
        {   // ---- W33: the two things a rebuild must NOT do ---------------------
            // (a) a file that EXISTS but cannot be read is still a REFUSAL - the
            //     #56 mark keeps its subject, which is frames of two moments
            //     under one name; and
            // (b) a rebuild never empties a stack. "Every file is gone" is not a
            //     thing a click on Reload should silently do to a measurement;
            //     the stack keeps its frames and says the reload refused.
            const std::string b2 = root + "/reb2";
            std::filesystem::create_directories(pathFromUtf8(b2), ec);
            for (int i = 0; i < 3; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "s_%03d.npy", i);
                writeNpy2(b2 + "/" + nm, 4, 4, 10.0f + (float)i);
            }
            std::vector<int> had;
            for (const auto& s : app.seqs) had.push_back(s.id);
            openFolder(b2);
            loadAll();
            int s2 = 0;
            for (const auto& s : app.seqs)
                if (std::find(had.begin(), had.end(), s.id) == had.end()) s2 = s.id;
            check(s2 != 0 && members(s2) == "0:s_000.npy 1:s_001.npy 2:s_002.npy",
                  "W33 (setup) a second 3-frame stack of its own");
            {   // (a) present, unreadable
                std::ofstream g(pathFromUtf8(b2 + "/s_001.npy"), std::ios::binary);
                g << "not a npy file at all";
                g.close();
                int f = -1;
                const std::string msg = reloadStackFromDisk(s2, &f);
                fprintf(stderr, "watchselftest: W33 unreadable: %s\n", msg.c_str());
                check(f == 1 && members(s2) == "0:s_000.npy 1:s_001.npy 2:s_002.npy" &&
                      seqInfo(s2) && seqInfo(s2)->reloadFailed == 1 &&
                      seqInfo(s2)->reloadOk == 2,
                      "W33b a member that exists but will not read stays, and REFUSES");
                check(!seqReloadNote(s2).empty() && *seqReloadMark(s2),
                      "W33c ...and the #56 mark is still exactly about that");
            }
            {   // (b) every file gone
                for (int i = 0; i < 3; i++) {
                    char nm[32];
                    snprintf(nm, sizeof nm, "s_%03d.npy", i);
                    std::error_code e3;
                    std::filesystem::remove(pathFromUtf8(b2 + "/" + nm), e3);
                }
                int f = -1;
                const std::string msg = reloadStackFromDisk(s2, &f);
                fprintf(stderr, "watchselftest: W33 all gone: %s\n", msg.c_str());
                check(f == 3 && members(s2) == "0:s_000.npy 1:s_001.npy 2:s_002.npy",
                      "W33d a rebuild never empties a stack: the frames stay, it refuses");
            }
        }
        {   // ---- W34-W38: the same rule at the SEAM, with no filesystem -------
            // watch::planRebuild is to §6 what watch::observe is to §1/§4: the
            // decision, as a function of the name lists it is handed. W26-W33
            // above drove every one of these through real files (that is where
            // the red was captured); these state them where a case that cannot
            // conveniently be built out of a folder can be stated too - W35 is
            // the one that matters and there is no comfortable way to write it
            // with a directory.
            auto V = [](std::initializer_list<const char*> xs) {
                std::vector<std::string> v;
                for (const char* x : xs) v.push_back(x);
                return v;
            };
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
                return s;
            };
            const std::vector<std::string> HAVE = V({ "a_000.npy", "a_001.npy", "a_002.npy" });
            {   // ---- W34: gone leaves, named joins, and the ORDER is the open's
                const std::vector<std::string> listed =
                    V({ "a_000.npy", "a_001.npy", "a_003.npy" });      // 002 gone, 003 new
                watch::Rebuild r = watch::planRebuild(HAVE, listed, listed, rp::naturalLess);
                check(join(r.dropped) == "a_002.npy" && join(r.added) == "a_003.npy" &&
                      join(r.order) == "a_000.npy,a_001.npy,a_003.npy" && r.any(),
                      "W34 gone leaves, named joins, and the order is by name");
                // ...and an arrival that belongs in the MIDDLE is put there. The
                // stack was opened with a_002 missing, so its members number
                // 0,1,2 over 000/001/003; when a_002 arrives it is frame 2 and
                // a_003 goes back to being frame 3. Appended, a_002 would be
                // frame 3 and a_003 would keep a number that is now a lie.
                const std::vector<std::string> gap = V({ "a_000.npy", "a_001.npy",
                                                         "a_003.npy" });
                const std::vector<std::string> full = V({ "a_000.npy", "a_001.npy",
                                                          "a_002.npy", "a_003.npy" });
                watch::Rebuild m = watch::planRebuild(gap, full, full, rp::naturalLess);
                check(join(m.added) == "a_002.npy" &&
                      join(m.order) == "a_000.npy,a_001.npy,a_002.npy,a_003.npy",
                      "W34b an arrival belonging in the middle is numbered in the middle");
            }
            {   // ---- W35: the rule may un-name a member; only the DISK removes it
                // siblingNamesAmong picks the frame axis as the last digit field
                // that VARIES, so one new file can move that choice and stop
                // naming members whose files are sitting right there. Dropping
                // those would delete live frames from a measurement on the
                // strength of a naming heuristic.
                const std::vector<std::string> listed =
                    V({ "a_000.npy", "a_001.npy", "a_002.npy" });      // all present
                const std::vector<std::string> want = V({ "a_000.npy" });   // rule moved
                watch::Rebuild r = watch::planRebuild(HAVE, listed, want, rp::naturalLess);
                check(r.dropped.empty() && r.added.empty() && !r.any() &&
                      join(r.order) == "a_000.npy,a_001.npy,a_002.npy",
                      "W35 a member the rule stopped naming STAYS while its file does");
            }
            {   // ---- W36: no rule (a derived subset) can lose, never gain ------
                const std::vector<std::string> listed =
                    V({ "a_000.npy", "a_002.npy", "a_003.npy", "a_004.npy" });
                watch::Rebuild r = watch::planRebuild(HAVE, listed, {}, rp::naturalLess);
                check(join(r.dropped) == "a_001.npy" && r.added.empty() &&
                      join(r.order) == "a_000.npy,a_002.npy",
                      "W36 an empty rule loses the gone member and admits nothing");
                // ANTI-VACUITY: the SAME lists with the folder's rule supplied do
                // admit them, or W36 is only "planRebuild adds nothing".
                watch::Rebuild g = watch::planRebuild(HAVE, listed, listed, rp::naturalLess);
                check(join(g.added) == "a_003.npy,a_004.npy",
                      "W36b ...and with a rule, those same two files DO join");
            }
            {   // ---- W37: the sentence, both directions and both counts --------
                watch::Rebuild r;
                r.added = V({ "a_005.npy" });
                r.dropped = V({ "a_000.npy" });
                check(watch::rebuildText(r, 5, 5, 5) ==
                        "membership rebuilt: 1 file(s) appeared in the folder (a_005.npy), "
                        "1 file(s) no longer exist (a_000.npy) - 5 of 5 frame(s), was 5",
                      "W37 the sentence names both and states n of N and the before");
                r.added = V({ "a_005.npy", "a_006.npy" });
                r.dropped.clear();
                check(watch::rebuildText(r, 3, 4, 5) ==
                        "membership rebuilt: 2 file(s) appeared in the folder - "
                        "4 of 5 frame(s), was 3",
                      "W37b >1 names none of them, and 4 of 5 is a partial arrival");
            }
            {   // ---- W38: nothing moved, nothing said --------------------------
                const std::vector<std::string> same = HAVE;
                watch::Rebuild r = watch::planRebuild(HAVE, same, same, rp::naturalLess);
                check(!r.any() && watch::rebuildText(r, 3, 3, 3).empty() &&
                      join(r.order) == "a_000.npy,a_001.npy,a_002.npy",
                      "W38 an untouched folder rebuilds to itself and says nothing");
            }
        }

        // ======================================================================
        //  W39-W45: §9 - AUTO-RELOAD.
        //
        //  Every stage of Watch before this one only ever SAID something. This
        //  one changes a number with no click, so the assertions are shaped
        //  around that and not around the happy path:
        //
        //    W39  the default is OFF, and it is PROVABLY off - a confirmed
        //         finding, two frames of the drain, and not one pixel moves.
        //    W40  ...and the same standing finding with the switch ON does
        //         reload it. Without this pair W39 is "the drain does nothing".
        //    W41  the frame that SAYS is not the frame that DOES (§9's "put the
        //         line up, then run on the next frame") - which is exactly the
        //         order pumpWatch calls watchAutoDrain and watchIntegrate in.
        //    W42  the record it LEAVES: #56's mark, the n-of-N line and a note
        //         on the stack that does not expire. An automatic reload has to
        //         be LOUDER than a manual one, because nobody asked for it.
        //    W43  work in flight: a measurement of this stack defers it.
        //    W44  a refused automatic reload DISARMS auto for that stack, and
        //         only a manual Reload that succeeds re-arms it.
        //    W45  the sentence, with no filesystem at all.
        //
        //  Its own fixture folder, for W25's reason: the folders above have
        //  been rewritten, emptied and refilled by 400 lines of other cases.
        //  The stack stays 4x4 throughout, so the size never carries a change
        //  and mtime is the only thing that can - see autWrite for how it is
        //  kept out of §11.5's blind spot.
        // ======================================================================
        closeAll();
        const std::string aut = root + "/auto";
        std::filesystem::create_directories(pathFromUtf8(aut), ec);
        auto autName = [](int i) {
            char b[32];
            snprintf(b, sizeof b, "a_%03d.npy", i);
            return std::string(b);
        };
        // Write member i with value v, and give it an mtime NO OTHER WRITE IN
        // THIS SECTION HAS. Two days into the past plus one hour per write: the
        // size never moves (4x4 f32 throughout), so mtime is the only thing that
        // can carry the change, and §11.5's hole is exactly that two rewrites
        // inside the clock's 15.6 ms granularity are indistinguishable - which
        // two of these are, since the whole section runs in a few milliseconds.
        // Counting hours is not a workaround for the limit; it is how a test
        // about auto-reload avoids being a test about timestamp resolution.
        int autBump = 0;
        auto autWrite = [&](int i, float v) {
            writeNpy2(aut + "/" + autName(i), 4, 4, v);
            bumpMtime(aut + "/" + autName(i), -48 - (++autBump));
        };
        for (int i = 0; i < 5; i++) writeNpy2(aut + "/" + autName(i), 4, 4, 200.0f + (float)i);
        openFolder(aut);
        loadAll();
        int as = 0;
        for (const auto& s : app.seqs) if (!s.name.empty()) { as = s.id; break; }
        std::map<int, WatchSet> ast;
        std::vector<App::WatchDone> aout;
        auto apoll = [&]() {
            aout.clear();
            return watchPollRound(watchTargetsNow(), ast, aout);
        };
        auto aintegrate = [&]() {
            {
                std::lock_guard<std::mutex> lk(app.watchMtx);
                app.watchDone = aout;
            }
            watchIntegrate();
        };
        // "the disk has settled on a change" in one call: §4 needs two readings
        // that agree, so two polls, and then the UI thread's integration.
        auto aSettle = [&]() { apoll(); apoll(); aintegrate(); };
        auto pxOf = [&](int index) {
            ImageDoc* d = memberDoc(as, index);
            return d ? d->px()[0] : -1.0f;
        };
        {   // ---- W39: the default is OFF, and OFF means NOTHING happens -------
            check(as != 0 && (int)framesOfSeq(as).size() == 5 && !app.watchAutoReload,
                  "W39 (setup) a 5-frame stack, and auto-reload is OFF by default");
            autWrite(2, 42.0f);                    // 202 -> 42 on disk, same size
            aSettle();
            check(seqWatchNote(as) == "1 file(s) changed on disk (a_002.npy)",
                  "W39b (setup) the amber line says the file moved, as it always did");
            const int rev0 = seqInfo(as) ? seqInfo(as)->stackRev : -1;
            // BEFORE the drains, or this asserts nothing: with the switch on, the
            // drains would have reloaded the stack and cleared the finding, and
            // an empty armed list afterwards would then mean "already done".
            check(watchAutoArmed().empty() && !watchAutoDue(),
                  "W39c OFF: a CONFIRMED finding arms nothing - no work, not a veto");
            watchAutoDrain();
            watchAutoDrain();                      // two frames, not one
            // The pixels in memory are STILL 202: what is on screen is what was
            // measured, which is the whole of ruling A6 and is what "opt-in"
            // has to mean when the thing being opted into is this.
            check(pxOf(2) == 202.0f && seqInfo(as) && seqInfo(as)->stackRev == rev0 &&
                  seqInfo(as)->autoReloadNote.empty() &&
                  seqWatchNote(as) == "1 file(s) changed on disk (a_002.npy)",
                  "W39d ...and two frames of the drain later, not a pixel has moved");
        }
        {   // ---- W40: ON, and the SAME finding reloads --------------------------
            // The anti-vacuity half of W39. Nothing about the disk or the finding
            // changes here - only the switch - so anything W40 sees is the switch.
            const int rev0 = seqInfo(as) ? seqInfo(as)->stackRev : -1;
            app.watchAutoReload = true;
            check(watchAutoArmed().size() == 1 && watchAutoArmed()[0] == as &&
                  !watchAutoRefusal(as) && !watchAutoBusy(as) && watchAutoDue(),
                  "W40 ON: the standing finding arms this stack and a round is due");
            watchAutoDrain();
            check(pxOf(2) == 42.0f && seqInfo(as) && seqInfo(as)->stackRev > rev0,
                  "W40b ...and one drain re-read it: the frame holds the disk's pixels");
            check(seqWatchNote(as).empty() && !watchAutoDue(),
                  "W40c ...and the line is gone, because it has stopped being true");
        }
        {   // ---- W41/W42: SAY on one frame, ACT on the next - and say what it did
            // The membership moves as well as the pixels, which is the case that
            // matters: a stack quietly measuring a different set of frames than
            // it did a second ago is the whole failure mode of this stage.
            std::error_code e2;
            std::filesystem::remove(pathFromUtf8(aut + "/" + autName(3)), e2);
            autWrite(5, 205.0f);
            aSettle();
            // ...the frame that SAYS. pumpWatch calls watchAutoDrain BEFORE
            // watchIntegrate for exactly this: the line is on screen for a frame
            // before anything acts on it, so what happened is readable where it
            // was announced.
            check(seqWatchNote(as) == "1 file(s) appeared in the folder (a_005.npy), "
                                      "1 file(s) no longer exist (a_003.npy)" &&
                  members(as) == "0:a_000.npy 1:a_001.npy 2:a_002.npy "
                                 "3:a_003.npy 4:a_004.npy",
                  "W41 the frame that SAYS it has not also done it");
            const size_t logWas = app.msgLog.size();
            watchAutoDrain();
            check(members(as) == "0:a_000.npy 1:a_001.npy 2:a_002.npy "
                                 "3:a_004.npy 4:a_005.npy" &&
                  seqInfo(as) && seqInfo(as)->expectedFrames == 5,
                  "W41b ...and the next frame's drain rebuilt the membership");
            // ---- W42: the RECORD. A toast expires and nobody asked for this
            // reload, so the note is latched on the stack and states the whole
            // summary - both names, both counts, and n of N.
            const std::string note = seqInfo(as) ? seqInfo(as)->autoReloadNote
                                                 : std::string();
            fprintf(stderr, "watchselftest: W42 note: %s\n", note.c_str());
            check(note.compare(0, 14, "auto-reloaded ") == 0 &&
                  note.find("1 file(s) appeared in the folder (a_005.npy)") !=
                      std::string::npos &&
                  note.find("1 file(s) no longer exist (a_003.npy)") != std::string::npos &&
                  note.find("5 of 5 frame(s), was 5") != std::string::npos,
                  "W42 the note names the time, both directions and n of N");
            check(app.msgLog.size() > logWas && !app.msgLog.empty() &&
                  app.msgLog.back().text.find(note) != std::string::npos &&
                  app.msgLog.back().text.find(seqInfo(as)->name) == 0,
                  "W42b ...and the same sentence is in Messages, named by stack");
            check(seqWatchNote(as).empty() && !seqInfo(as)->autoReloadNote.empty(),
                  "W42c the amber finding goes; the record of what was done stays");
        }
        {   // ---- W43: work in flight - a measurement is not swapped underneath --
            // The decision (§15): an automatic reload WAITS. It does not cancel
            // the measurement and it does not run beside it; the finding stays on
            // the row, and the reload happens on a later frame.
            autWrite(1, 55.0f);
            aSettle();
            check(seqWatchNote(as) == "1 file(s) changed on disk (a_001.npy)" &&
                  pxOf(1) == 201.0f,
                  "W43 (setup) a confirmed change, and the old pixels still resident");
            const bool wasPending = app.srvTemporal.pending;
            const int wasSeq = app.srvTemporal.seqId;
            app.srvTemporal.pending = true;
            app.srvTemporal.seqId = as;
            check(watchAutoBusy(as) != nullptr && !watchAutoDue() &&
                  watchAutoArmed().size() == 1,
                  "W43b a sigma_t of THIS stack is in flight: armed, but not due");
            watchAutoDrain();
            watchAutoDrain();
            check(pxOf(1) == 201.0f &&
                  seqWatchNote(as) == "1 file(s) changed on disk (a_001.npy)",
                  "W43c two frames pass and nothing moves - the line stays up");
            // ANTI-VACUITY: the same stack, the same finding, the measurement
            // done. Without this, W43c is "auto-reload stopped working".
            app.srvTemporal.pending = wasPending;
            app.srvTemporal.seqId = wasSeq;
            check(!watchAutoBusy(as) && watchAutoDue(),
                  "W43d ...and the moment it finishes the stack is due again");
            watchAutoDrain();
            check(pxOf(1) == 55.0f && seqWatchNote(as).empty(),
                  "W43e ...and the very next frame re-reads it");
        }
        {   // ---- W44: a REFUSED automatic reload disarms this stack --------------
            // Without this the loop is real and it is hourly: the reload refuses,
            // watchReloadNow clears the finding, the worker re-reports the same
            // difference on its next round, and auto reloads again - every five
            // seconds, for the rest of the session. #56's latch already records
            // exactly the state that must stop it, so it is the latch that is
            // asked rather than a second counter beside it.
            {   // a_004 exists and will not read
                std::ofstream g(pathFromUtf8(aut + "/" + autName(4)), std::ios::binary);
                g << "not a npy file at all";
            }
            bumpMtime(aut + "/" + autName(4), -48 - (++autBump));   // its own hour
            aSettle();
            check(seqWatchNote(as) == "1 file(s) changed on disk (a_004.npy)" &&
                  !watchAutoRefusal(as),
                  "W44 (setup) a member that will not read, and auto is still armed");
            watchAutoDrain();
            const std::string note = seqInfo(as) ? seqInfo(as)->autoReloadNote
                                                 : std::string();
            fprintf(stderr, "watchselftest: W44 note: %s\n", note.c_str());
            check(seqInfo(as) && seqInfo(as)->reloadFailed == 1 && *seqReloadMark(as) &&
                  note.find(SEQ_RELOAD_FAIL_TOKEN) != std::string::npos,
                  "W44b the automatic reload leaves #56's mark, and the note says so");
            check(watchAutoRefusal(as) != nullptr,
                  "W44c ...and this stack is DISARMED: one attempt, never a retry loop");
            // ...and it stays disarmed through a further, unrelated change. The
            // finding IS standing and nothing else is in the way, so the empty
            // armed list below is the disarming and not an idle assertion:
            // a_000 moved, and a_004 still differs from a baseline the refused
            // reload never advanced, which is two.
            autWrite(0, 77.0f);
            aSettle();
            watchAutoDrain();
            watchAutoDrain();
            check(pxOf(0) == 200.0f && seqInfo(as)->autoReloadNote == note &&
                  seqWatchNote(as) == "2 file(s) changed on disk" &&
                  watchAutoArmed().empty() && !watchAutoBusy(as),
                  "W44d a later change on a disarmed stack is NOT auto-reloaded");
            // ANTI-VACUITY: a manual Reload that succeeds is what re-arms it, and
            // the note goes because the user has taken the wheel. 444 and not 204
            // on purpose - a value the frame could already be holding would let a
            // reload that did nothing pass this line.
            writeNpy2(aut + "/" + autName(4), 4, 4, 444.0f);
            watchReloadNow(as, false);
            check(seqInfo(as) && seqInfo(as)->reloadFailed == 0 && !*seqReloadMark(as) &&
                  pxOf(3) == 444.0f && pxOf(0) == 77.0f &&   // frame 3 IS a_004
                                                             // (a_003 left in W41)
                  seqInfo(as)->autoReloadNote.empty() && !watchAutoRefusal(as),
                  "W44e a manual Reload that succeeds clears the mark and re-arms");
            autWrite(1, 88.0f);
            aSettle();
            watchAutoDrain();
            check(pxOf(1) == 88.0f && !seqInfo(as)->autoReloadNote.empty(),
                  "W44f ...and the next change is automatic again");
            app.watchAutoReload = false;
        }
        {   // ---- W45: the sentence, with no filesystem and no stack -------------
            check(watch::autoReloadText("14:32:07", "reloaded 5 frame(s), 0 failed",
                                        false) ==
                    "auto-reloaded 14:32:07 - reloaded 5 frame(s), 0 failed",
                  "W45 the sentence an automatic reload latches on the stack");
            // The compare pin is uid-keyed, so a rebuild that renumbers frames
            // leaves it alone (§12.5) - but a pin on a frame whose FILE IS GONE
            // is dropped, and that is a number moving with nobody's click. It is
            // therefore said out loud rather than left to be noticed.
            check(watch::autoReloadText("09:00:01", "reloaded 4 frame(s), 0 failed",
                                        true) ==
                    "auto-reloaded 09:00:01 - reloaded 4 frame(s), 0 failed"
                    " - the compare pin was on a frame that is gone",
                  "W45b ...and it says when the compare pin went with a gone frame");
        }

        closeAll();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "watchselftest: %s\n", ok ? "ok" : "FAILED");
        stopWatchWorker();
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
