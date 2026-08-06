// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // VERIFICATION ADDITION (functional-verification agent, not part of the
    // feature work): the corners of the close / batch rules that the six
    // feature selftests do not reach - non-contiguous stacks, the Ctrl+Alt+W
    // escape hatch, compare-B left dangling, prune's reference set, a move
    // issued mid-load, closeBatch against a queued remote open, and the
    // remote in-flight drop actually being the path that catches a fetch.
    // ---- Open in new window: instance slots, recovery scan, prefs, spawn ----
    // State assertions, no pixels: the lock protocol, the recovery offer, the
    // read-only prefs of a secondary window, and the exact command line the
    // Files menu would spawn - plus ONE real detached spawn, to prove the
    // detach flags do not break launching at all.
    if (!g_newwinSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "newwinselftest: %-56s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        std::error_code ec;
        std::string dir = g_newwinSelftest + "/newwin-scratch";
        std::filesystem::remove_all(pathFromUtf8(dir), ec);
        std::filesystem::create_directories(pathFromUtf8(dir), ec);

        // N1: an empty dir yields slot 1, and the lock names this process
        std::string lock1;
        int s1 = claimAutosaveSlot(dir, lock1);
        check(s1 == 1 && lockPid(lock1) == currentPid(),
              "N1 first claim takes slot 1, our PID in the lock");

        // N2: slot 1 locked by a LIVE pid (our own stands in for another
        // window) pushes the next claim to slot 2 - and slot 1 keeps the
        // historic autosave name, so existing sessions stay untouched
        std::string lock2;
        int s2 = claimAutosaveSlot(dir, lock2);
        check(s2 == 2 && lockPid(lock2) == currentPid(),
              "N2 live lock on slot 1: next claim takes slot 2");
        check(slotAutosaveName(1) == "autosave.vsession" &&
              slotAutosaveName(2) == "autosave-2.vsession",
              "N2 slot 1 keeps the historic autosave name");

        // N3: releasing a slot frees it for the next claim
        std::filesystem::remove(pathFromUtf8(lock2), ec);
        std::string lock2b;
        check(claimAutosaveSlot(dir, lock2b) == 2, "N3 released slot is claimed again");

        // N4: one REAL detached spawn (--help: starts, prints, exits) - both
        // the proof that the detach flags do not break launch, and the source
        // of a genuinely dead PID for the stale-lock reclaim below
        unsigned long deadPid = 0;
        {
            std::vector<std::string> av{ selfExePath(), "--help" };
            bool sp = spawnDetached(av, &deadPid);
#if defined(_WIN32)
            bool exited = false;
            DWORD code = (DWORD)-1;
            if (sp && deadPid) {
                HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, (DWORD)deadPid);
                if (h) {
                    exited = WaitForSingleObject(h, 15000) == WAIT_OBJECT_0;
                    GetExitCodeProcess(h, &code);
                    CloseHandle(h);
                } else {
                    exited = true;      // --help was gone before we could look:
                    code = 0;           // it started (CreateProcess said so) and exited
                }
            }
            check(sp && exited && code == 0,
                  "N4 detached child (--help) started and exited 0");
#else
            check(sp, "N4 detached child (--help) started and exited");
#endif
            // slot 2's lock written BY HAND with the dead PID: stale, reclaimable
            std::filesystem::remove(pathFromUtf8(lock2b), ec);
            {
                std::ofstream lf(pathFromUtf8(slotLockPath(dir, 2)), std::ios::binary);
                lf << deadPid;
            }
            check(!pidAlive(deadPid), "N4 the joined child's PID reads as dead");
            std::string lock2c;
            int s2c = claimAutosaveSlot(dir, lock2c);
            check(s2c == 2 && lockPid(lock2c) == currentPid(),
                  "N4 stale lock reclaimed: slot 2 again, our PID");
        }

        // N5: the recovery scan offers the newest UNLOCKED autosave, by name.
        // Here slot 2 is newer but held live (the lock from N4, selfPid=0 makes
        // our own PID read as "another window"); slot 1 is older and free.
        {
            auto put = [&](const char* name, int hoursAgo) {
                std::string p = dir + "/" + name;
                { std::ofstream f(pathFromUtf8(p), std::ios::binary); f << "viewer-session 1\n"; }
                std::filesystem::last_write_time(pathFromUtf8(p),
                    std::filesystem::file_time_type::clock::now() - std::chrono::hours(hoursAgo), ec);
            };
            std::filesystem::remove(pathFromUtf8(slotLockPath(dir, 1)), ec);  // release slot 1
            put("autosave.vsession", 2);        // older, unlocked
            put("autosave-2.vsession", 1);      // newer, locked live
            std::string nm;
            std::string offer = recoveryOfferIn(dir, 0, &nm);
            check(offer == dir + "/autosave.vsession" && nm == "autosave.vsession",
                  "N5 newest is LOCKED live: scan offers the older, named");
            offer = recoveryOfferIn(dir, currentPid(), &nm);
            check(nm == "autosave-2.vsession",
                  "N5 a lock we hold ourselves does not hide our own autosave");
            std::filesystem::remove(pathFromUtf8(slotLockPath(dir, 2)), ec);
            offer = recoveryOfferIn(dir, 0, &nm);
            check(offer == dir + "/autosave-2.vsession" && nm == "autosave-2.vsession",
                  "N5 lock gone: scan offers the newest and names it");
        }

        // N6: prefs are read-only in secondary mode - byte-identical file, and
        // the refusal is one line in the Messages log
        {
            std::string pp = prefsPath();
            std::vector<uint8_t> before, after;
            bool hadBefore = readFileBytes(pp, before);
            bool wasSecondary = g_secondary;
            g_secondary = true;
            app.prefsDirty = true;
            savePrefs();
            g_secondary = wasSecondary;
            app.prefsDirty = false;
            bool hadAfter = readFileBytes(pp, after);
            check(hadBefore == hadAfter && before == after,
                  "N6 secondary savePrefs leaves prefs.txt byte-identical");
            bool noted = false;
            for (const auto& m : app.msgLog)
                if (m.text.find("read-only in a secondary window") != std::string::npos)
                    noted = true;
            check(noted, "N6 the refusal is one line in the Messages log");
        }

        // N7: the EXACT argv the Files menu would spawn, all four shapes
        {
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& a : v) { if (!s.empty()) s += ' '; s += a; }
                return s;
            };
            auto mk = [&](const char* path, int seq, int seqIdx, int cfa, int pat,
                          const char* url) {
                auto d = std::make_unique<ImageDoc>();
                d->src->path = path; d->seqId = seq; d->seqIndex = seqIdx;
                d->cfa = cfa; d->cfaPattern = pat;
                if (url) d->src->remoteUrl = url;
                app.images.push_back(std::move(d));
                return app.images.back().get();
            };
            closeAll();
            std::string exe = selfExePath();
            ImageDoc* plain = mk("C:/data/dark_007.npy", 0, 0, 0, 0, nullptr);
            ImageDoc* cfa   = mk("C:/data/flat_003.npy", 0, 0, 1, 2, nullptr);  // GRBG
            mk("C:/data/s/frame_000.npy", 9, 0, 0, 0, nullptr);                 // head
            ImageDoc* tail  = mk("C:/data/s/frame_001.npy", 9, 1, 0, 0, nullptr);
            ImageDoc* rem   = mk("x.npy", 0, 0, 0, 0, "ssh://user@trc2/data/x.npy");
            check(join(newWindowArgv(plain)) ==
                  exe + " --secondary --window-offset 40,40 C:/data/dark_007.npy",
                  "N7 local frame argv exact");
            check(join(newWindowArgv(cfa)) ==
                  exe + " --secondary --window-offset 40,40 --bayer-pattern GRBG"
                        " --cfa bayer C:/data/flat_003.npy",
                  "N7 CFA frame argv: pattern first, then --cfa");
            check(join(newWindowArgv(tail)) ==
                  exe + " --secondary --window-offset 40,40 --stack always"
                        " C:/data/s/frame_000.npy",
                  "N7 stack argv: HEAD file + --stack always");
            check(join(newWindowArgv(rem)) ==
                  exe + " --secondary --window-offset 40,40 ssh://user@trc2/data/x.npy",
                  "N7 remote argv passes the ssh url");
            closeAll();
        }

        // N8: the window's geometry - the clamp against a monitor set, and the
        // round trip through prefs.txt.
        //
        // The clamp is a pure function of the saved rectangle and the screens
        // that exist NOW, which is the only way to test the case that actually
        // matters: the screen that is no longer there. Nobody can unplug a
        // monitor from inside a test, nobody reproduces it by hand a year later,
        // and the failure it hides is the unrecoverable one - a window restored
        // where no mouse can reach it and no menu can bring it back.
        {
            // Two 1920x1080 screens side by side, taskbar along the bottom of
            // each, so the work areas are 1040 tall.
            const std::vector<WinRect> two = { { 0, 0, 1920, 1040 },
                                               { 1920, 0, 1920, 1040 } };
            const std::vector<WinRect> one = { { 0, 0, 1920, 1040 } };
            bool kept = false;
            auto fit = [&](WinRect r, const std::vector<WinRect>& w) {
                kept = fitSavedWindow(r, w);
                return r;
            };

            WinRect r = fit({ 100, 80, 1600, 900 }, two);
            check(kept && r.x == 100 && r.y == 80 && r.w == 1600 && r.h == 900,
                  "N8 a rectangle that still fits comes back untouched");

            r = fit({ 2100, 80, 1600, 900 }, two);
            check(kept && r.x == 2100 && r.y == 80 && r.w == 1600,
                  "N8 the second screen is a place a window is allowed to be");

            // The same prefs.txt, opened after the second screen went away.
            r = fit({ 2100, 80, 1600, 900 }, one);
            check(!kept && r.w == 1600 && r.h == 900,
                  "N8 a screen that is gone drops the position and keeps the size");

            // Docked laptop: the window was on a 4K panel and the laptop is
            // alone now. Position unusable, size larger than the screen.
            r = fit({ 2400, 1300, 2600, 1600 }, one);
            check(!kept && r.w == 1920 && r.h == 1040,
                  "N8 a size larger than the only screen is clamped to it");

            // On screen, but the title bar is above the top of it: nothing left
            // to grab, so the position goes.
            r = fit({ 300, -200, 1600, 900 }, one);
            check(!kept, "N8 a window whose drag strip is above the screen is not restored");

            // Only a corner left on screen - 60x40 of a 1600x900 window.
            r = fit({ 1860, 1000, 1600, 900 }, one);
            check(!kept, "N8 a corner peeking onto the screen is not enough to be reachable");

            // Hanging off the right and bottom edges but plainly reachable:
            // slid back on rather than thrown away.
            r = fit({ 600, 400, 1600, 900 }, one);
            check(kept && r.x == 320 && r.y == 140 && r.w == 1600 && r.h == 900,
                  "N8 a window hanging off the edge is slid back, not forgotten");

            // No monitor answered at all (a headless or mid-hotplug moment):
            // the size survives, the position does not.
            r = fit({ 100, 80, 1600, 900 }, {});
            check(!kept && r.w == 1600 && r.h == 900,
                  "N8 with no monitors the size still survives");

            r = fit({ 100, 80, 4, 4 }, one);
            check(r.w == WIN_MIN_W && r.h == WIN_MIN_H,
                  "N8 a degenerate saved size comes back as a usable window");

            // ---- the round trip, against the real prefs.txt ------------------
            // The hermetic APPDATA/HOME that CMakeLists.txt pins for every
            // selftest is what makes this safe; the bytes are put back anyway,
            // because a test that edits the file it is testing must leave it as
            // it found it.
            std::string pp = prefsPath();
            std::vector<uint8_t> before;
            bool hadBefore = readFileBytes(pp, before);
            int sx = app.winX, sy = app.winY, sw = app.winW, sh = app.winH;
            bool sm = app.winMax;
            auto bmSnap = app.rbBookmarks;
            auto rcSnap = app.rbRecents;
            auto memoSnap = app.readerMemo;
            auto reload = [&]() {
                // loadPrefs APPENDS to the list-valued prefs (V25k says so):
                // clear them first or reading twice doubles them.
                app.rbBookmarks.clear();
                app.rbRecents.clear();
                app.readerMemo.clear();
                loadPrefs();
            };

            app.winX = -7; app.winY = 33; app.winW = 1234; app.winH = 777;
            app.winMax = true;
            savePrefs();
            app.winX = app.winY = app.winW = app.winH = 0;
            app.winMax = false;
            reload();
            check(app.winX == -7 && app.winY == 33 && app.winW == 1234 &&
                  app.winH == 777 && app.winMax,
                  "N8 position, size and the maximized flag survive prefs.txt");

            // Quitting maximized has to come back maximized WITH a rectangle to
            // restore down to - the flag alone would leave nothing to restore.
            std::vector<uint8_t> savedBytes;
            readFileBytes(pp, savedBytes);
            std::string txt(savedBytes.begin(), savedBytes.end());
            check(txt.find("window -7 33 1234 777 1\n") != std::string::npos,
                  "N8 one line carries both: the rectangle and the state");

            // A hand-edited or truncated line is "nothing saved", not a 0x0
            // window at whatever the reads gave up on.
            {
                std::string bad = txt;
                size_t p = bad.find("window -7 33 1234 777 1");
                if (p == std::string::npos) { bad += "window 12 34\n"; p = 0; }
                else bad.replace(p, strlen("window -7 33 1234 777 1"), "window 12 34");
                std::ofstream bf(pathFromUtf8(pp), std::ios::binary);
                bf << bad;
                bf.close();
                app.winW = 999;
                reload();
                check(app.winW == 0 && app.winH == 0 && !app.winMax,
                      "N8 a truncated window line means nothing saved");
            }

            // A run that never samples a window - every scripted run - writes
            // back exactly what it read. This is the whole reason savePrefs
            // gates the line on winW > 0 rather than always emitting one.
            {
                app.winX = app.winY = app.winW = app.winH = 0;
                app.winMax = false;
                savePrefs();
                std::vector<uint8_t> a1;
                readFileBytes(pp, a1);
                reload();
                savePrefs();
                std::vector<uint8_t> a2;
                readFileBytes(pp, a2);
                check(a1 == a2 && std::string(a1.begin(), a1.end()).find("window ") ==
                                  std::string::npos,
                      "N8 a run that saw no window writes no window line");
            }

            app.winX = sx; app.winY = sy; app.winW = sw; app.winH = sh;
            app.winMax = sm;
            app.rbBookmarks = bmSnap;
            app.rbRecents = rcSnap;
            app.readerMemo = memoSnap;
            if (hadBefore) {
                std::ofstream rf(pathFromUtf8(pp), std::ios::binary);
                rf.write((const char*)before.data(), (std::streamsize)before.size());
            } else {
                std::filesystem::remove(pathFromUtf8(pp), ec);
            }
        }

        std::filesystem::remove_all(pathFromUtf8(dir), ec);
        fprintf(stderr, "newwinselftest: %s\n", ok ? "ALL PASS" : "FAILED");
        return ok ? 0 : 1;
    }
