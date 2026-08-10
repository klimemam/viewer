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

        std::map<int, watch::SetState> states;
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
            std::map<int, watch::SetState> st2;
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
