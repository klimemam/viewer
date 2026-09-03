// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    if (!g_scanSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "scanselftest: %-74s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        // "every open stack holds all the frames it promised", as a reusable
        // claim: it is asked once after the .npy scan and again after the
        // picture scan, and the two must not be able to drift into different
        // questions.
        auto checkStacksWhole = [&](auto&& chk, const char* what) {
            std::string say;
            int shortfall = 0;
            for (const auto& si : app.seqs) {
                const int have = (int)framesOfSeq(si.id).size();
                if (have >= si.expectedFrames) continue;
                shortfall++;
                say += (say.empty() ? "" : ", ") + si.name + " " +
                       std::to_string(have) + "/" + std::to_string(si.expectedFrames);
            }
            if (shortfall)
                fprintf(stderr, "scanselftest: short stack(s): %s\n", say.c_str());
            chk(shortfall == 0, what);
        };
        std::string dir = g_scanSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        startRemote(rbMain(), "local://" + dir);
        double t0 = nowSec();
        while (nowSec() - t0 < 120.0 && !rbMain().b.connected &&
               rbMain().b.err.empty()) {
            pumpRemoteBrowse();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // ---- ONE scan wait, used by every scan below ---------------------------
        // Was inline in this test and is a lambda now because #148 needed the
        // SAME wait run against a folder that groups into nothing: a budget that
        // is only ever exercised by a folder full of .npy is a budget nobody has
        // ever seen expire.
        //
        // timedOut says which way the loop left: `false` means something ANSWERED,
        // `true` means the budget ran out - and those are different events that
        // used to look identical from outside (the test printed "0 stack(s)"
        // either way, after two minutes of nothing).
        bool timedOut = false;
        auto runScan = [&](const std::string& d, double budget) -> double {
            App::RbJob j; j.kind = App::RbScan;
            j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = d;
            j.gen = ++rbMain().b.scanGen;      // the cancel token nav.cpp matches on
            rbEnqueue(rbMain(), std::move(j));
            const App::RemoteBrowse& B = rbMain().b;
            double ts = nowSec();
            timedOut = true;
            while (nowSec() - ts < budget) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                // headless "Load selected": accept the picker with everything
                // checked, through the SAME function the Load button calls
                if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
                if (!rbMain().busy && !app.seqs.empty() &&
                    app.rbOpenQueue.empty() && app.rfPending == 0) {
                    timedOut = false; break;
                }
                // #148: ZERO GROUPS IS AN ANSWER. nav.cpp toasts it and carries
                // on - no picker, no open queue, no stack - so the wait above
                // is a wait for something that will never be queued. This test
                // used to sit here for its whole budget and then print the same
                // "0 stack(s)" line it prints now, which made a fixture that had
                // lost its .npy files HANG the suite instead of failing it.
                if (B.scanGen && B.scanDoneGen == B.scanGen && B.scanGroups <= 0) {
                    timedOut = false; break;
                }
                if (!B.err.empty()) { timedOut = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return nowSec() - ts;
        };
        double el1 = runScan(dir, 120.0);
        for (const auto& si : app.seqs)
            fprintf(stderr, "scanselftest: stack '%s' expected=%d\n",
                    si.name.c_str(), si.expectedFrames);
        {
            const bool opened = !app.seqs.empty() && rbMain().b.err.empty();
            fprintf(stderr, "scanselftest: %d stack(s) in %.1fs: %s\n",
                    (int)app.seqs.size(), el1,
                    opened ? "ok" : ("FAILED " + rbMain().b.err).c_str());
            if (!opened) ok = false;
        }
        // ...and the FRAMES, which this test did not look at. A stack whose head
        // opened and whose siblings were never fetched - a memory budget that
        // computed fit == 0, a fetch generation that dropped every arrival -
        // still leaves app.seqs non-empty and rbOpenQueue empty and rfPending
        // 0, so every condition above is satisfied by a stack that is one frame
        // of forty. That is issue #244's exact shape ("remote stacks stopped
        // being read out"), and it is why this test could not have caught it.
        //
        // Asked as "does every stack hold the frames it says it expects", not
        // as a fixture count: expectedFrames is what the SCAN promised, so the
        // claim stays true if the fixture grows and false the moment a promise
        // is not kept.
        checkStacksWhole(check, "S-frames the remote scan's stacks hold every frame they promise");

        // ================= issue #148 ==========================================
        // The same folder used to return two answers depending on which door
        // opened it. `viewer <dir>`, a dropped folder and the file dialog reach
        // scanFolderGroups, which asks core/imagefile.h and therefore groups
        // png/tif/exr/RAW. File ▸ Open Folder (all stacks below)… and every
        // Browse "Open folder (all stacks below)" reached remoteScanFolder ->
        // core/serve.cpp, whose walk collects isNpySuffix ONLY - so a folder of
        // .png answered "no .npy stacks under …" and opened nothing, while the
        // Browse listing beside it showed the very files (LIST has no extension
        // filter; only the meta peek is npy-gated).
        //
        // core/serve.cpp:533-538 is this repository's own rule against exactly
        // that: the same folder must not read one way over ssh and another way
        // opened from disk. The REMOTE half stays npy-only on purpose (a
        // declared refusal, #148's parked question); the local half is what is
        // asserted here.
        std::string nonpy;
        {   // sibling of the scanned root - tools/testdata/linset ->
            // tools/testdata/openfolder - with the same literal fallback
            // --picker-selftest uses when the sibling is not where it should be.
            size_t s = dir.find_last_of('/');
            nonpy = (s == std::string::npos ? std::string() : dir.substr(0, s + 1)) +
                    "openfolder";
            std::error_code ec;
            if (!std::filesystem::is_directory(pathFromUtf8(nonpy), ec))
                nonpy = "tools/testdata/openfolder";
        }
        // The two stack names, hand-derived from the six file names on disk
        // (tools/gen_testdata.py writes them and nothing else into openfolder/):
        //
        //   dark_001.png dark_002.png dark_003.png
        //     -> segmentName splits the stem into ["dark_"]["001"]: ONE digit
        //        run, so it is the frame axis; findSequenceSiblings writes one
        //        '?' per digit -> "dark_???.png"; patternWithExtent then names
        //        the extent by VALUE, lowest‥highest, keeping the padding on
        //        disk -> "dark_001‥003.png"  (‥ is U+2025, "\xE2\x80\xA5").
        //   暗電流_001.png .. 暗電流_003.png
        //     -> identical derivation; the stem's bytes are not ASCII and
        //        nothing in the rule looks at them -> "暗電流_001‥003.png".
        //
        // The scan root IS this folder, so `rel` is empty and the group name is
        // the bare pattern (no "folder/" prefix). Order: scanFolderGroups sorts
        // the file list as UTF-8 bytes and 'd' (0x64) precedes 暗 (0xE6 9A 97),
        // so dark_ is group 0.
        const std::string DD = "\xE2\x80\xA5";                    // U+2025 ‥
        const std::string JP = "\xE6\x9A\x97\xE9\x9B\xBB\xE6\xB5\x81";  // 暗電流
        const std::string G0 = "dark_001" + DD + "003.png";
        const std::string G1 = JP + "_001" + DD + "003.png";

        // ---- S0: the fixture is what the rest of this section claims ----------
        // ANTI-VACUITY. Every assertion below is a count, and "0 groups" and
        // "2 groups" are both things an ABSENT or emptied folder could produce
        // for reasons that have nothing to do with #148. So the folder is
        // counted first, by name, and a fixture that drifted fails HERE.
        {
            std::vector<std::string> names;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(pathFromUtf8(nonpy), ec), e;
                 it != e; it.increment(ec)) {
                if (ec) break;
                std::error_code fec;
                if (it->is_regular_file(fec)) names.push_back(it->path().filename().u8string());
            }
            std::sort(names.begin(), names.end());
            std::vector<std::string> want = {
                "dark_001.png", "dark_002.png", "dark_003.png",
                JP + "_001.png", JP + "_002.png", JP + "_003.png",
            };
            std::string got;
            for (const auto& n : names) got += (got.empty() ? "" : " ") + n;
            fprintf(stderr, "scanselftest: fixture %s holds [%s]\n",
                    nonpy.c_str(), got.c_str());
            check(names == want,
                  "S0 the fixture is six .png in one folder and not one .npy");
        }

        // ---- S1: the LISTING shows them -------------------------------------
        // The half of the report that made the bug so confusing: the panel lists
        // the .png files (the peer's LIST has no extension filter at all), and
        // then Open Folder said there was nothing there. If this ever stops
        // being true, S2's "0 groups" would be a statement about an unreadable
        // folder rather than about the grouping rule.
        {
            // As their own rows, or as the MEMBERS of a stack-group row. The
            // second case arrived with #148 judgment B: the peer folds a
            // numbered family into one row now that it can read the format, so
            // dark_001..003.png is one row holding three names rather than
            // three rows - which is exactly what it has always done to
            // frame_001..003.npy. What S1 is FOR is unchanged: all six files
            // are SHOWN, so S2's answer below is about the grouping rule and
            // not about a folder the peer could not read.
            auto countPng = [] {
                int n = 0;
                auto isPng = [](const std::string& nm) {
                    return nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".png") == 0;
                };
                for (const auto& e : rbMain().b.entries) {
                    if (e.dir) continue;
                    if (e.members.empty()) { if (isPng(e.name)) n++; continue; }
                    for (const auto& m : e.members) if (isPng(m)) n++;
                }
                return n;
            };
            remoteBrowseTo(rbMain(), nonpy);
            double ts = nowSec();
            int png = 0;
            while (nowSec() - ts < 30.0) {
                pumpRemoteBrowse();
                png = countPng();
                if (png >= 6) break;
                if (!rbMain().b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            fprintf(stderr, "scanselftest: the peer LISTS %d entr(ies) of %s, "
                            "%d of them .png\n",
                    (int)rbMain().b.entries.size(), rbMain().b.dir.c_str(), png);
            check(png == 6, "S1 the Browse listing shows all six .png (LIST has no filter)");
        }

        // ---- S2: the peer scan opens them, the same way the local door does --
        // This assertion is the REVERSE of what it was, and the reversal is
        // issue #148's judgment B. It used to read "the peer scan of a folder
        // with no .npy opens nothing" - true, correct under judgment A, and the
        // whole of the complaint: the Browse list showed six .png (S1) and Open
        // Folder answered "no .npy stacks under ...". viewer-serve links
        // core/imagefile.h now, so the answer is two stacks - the SAME two the
        // local door produces in S3 below, which is the property the issue
        // asked for and the reason both halves live in one test.
        //
        // S2b survives untouched and is about something else: zero groups must
        // be an ANSWER and not a wait. It was written when a folder that grouped
        // into nothing made this entry point sit out its whole budget, and it
        // still guards exactly that - what changed is which folder produces zero.
        closeAll();
        {
            const double budget = 30.0;
            double el = runScan(nonpy, budget);
            int frames = 0;
            std::string names;
            for (const auto& sq : app.seqs) names += (names.empty() ? "" : " | ") + sq.name;
            for (const auto& im : app.images) if (im->seqId >= 0) frames++;
            fprintf(stderr, "scanselftest: peer scan of %s: %d stack(s) [%s], %d frame(s), "
                            "%.1fs of a %.0fs budget, timedOut=%d, toast=\"%s\"\n",
                    nonpy.c_str(), (int)app.seqs.size(), names.c_str(), frames, el, budget,
                    timedOut ? 1 : 0, app.toast.c_str());
            check(app.seqs.size() == 2 && frames == 6,
                  "S2a the peer scan of a folder of .png opens two stacks, six frames");
            check(names.find(G0) != std::string::npos && names.find(G1) != std::string::npos,
                  "S2a ...under the same two patterns the local door produces (S3d)");
            check(!timedOut,
                  "S2b ...and REPORTS it: zero groups is an answer, not a wait");
            checkStacksWhole(check,
                  "S2c ...and both stacks hold every frame they promised");
        }

        // ---- S3: the local door groups them ----------------------------------
        // remoteScanFolder is what File ▸ Open Folder (all stacks below)… and
        // all three Browse "Open folder (all stacks below)" items call. With an
        // EMPTY host the subject is this machine's own disk, so it must reach
        // the extension-aware scan - the same one a dropped folder takes - and
        // land in the SAME picker. One code path for "scan a folder into groups,
        // then show the picker"; the picker is not forked.
        closeAll();
        app.toast.clear();
        {
            remoteScanFolder(rbMain(), nonpy);      // EXACTLY what the menu calls
            // The local branch answers before this line is reached (the walk is
            // synchronous, exactly as a drop's is). The loop is here so that a
            // regression which routes this back through the worker fails on the
            // ASSERTIONS below rather than on "the picker had not arrived yet".
            double ts = nowSec();
            while (nowSec() - ts < 30.0 && !app.folderPickOpen) {
                pumpRemoteBrowse();
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::string got;
            for (const auto& e : app.folderPick)
                got += (got.empty() ? "" : " | ") + e.g.name + " x" +
                       std::to_string(e.g.files.size());
            fprintf(stderr, "scanselftest: Open Folder on host='' -> picker=%d "
                            "remoteMode=%d groups=%d [%s] toast=\"%s\"\n",
                    app.folderPickOpen ? 1 : 0, app.folderPickRemote ? 1 : 0,
                    (int)app.folderPick.size(), got.c_str(), app.toast.c_str());
            check(app.folderPickOpen,
                  "S3a Open Folder (all stacks below) on THIS machine raises the picker");
            check(!app.folderPickRemote,
                  "S3b ...through the local door: the picker is not in remote mode");
            check(app.folderPick.size() == 2,
                  "S3c ...holding two stacks, one per digit-run pattern");
            bool named = app.folderPick.size() == 2 &&
                         app.folderPick[0].g.name == G0 &&
                         app.folderPick[1].g.name == G1 &&
                         app.folderPick[0].g.files.size() == 3 &&
                         app.folderPick[1].g.files.size() == 3;
            check(named, "S3d ...named dark_001..003.png and 暗電流_001..003.png, 3 frames each");
            bool onDisk = !app.folderPick.empty();
            for (const auto& e : app.folderPick)
                for (const auto& f : e.g.files)
                    if (f.find("://") != std::string::npos) onDisk = false;
            check(onDisk,
                  "S3e ...over on-disk paths: no url form reaches the local loader");
        }

        // ---- S4: identity (§6.2) ---------------------------------------------
        // ONE FILE ON THIS DISK HAS ONE IDENTITY, WHICHEVER DOOR OPENED IT.
        //
        // This used to be two fprintf lines and no assertion, and the two lines
        // did not match: a local:// open sets FrameSource::path to the URL, and
        // srcKeyPath passed anything holding "://" through untouched - no
        // weakly_canonical, no Windows case fold - while the local loader stores
        // the on-disk path, which srcKeyPath canonicalises. Same file, same
        // mtime/fsize, two §6.2 registry keys:
        //
        //   identity via the local scan   c:\users\...\openfolder\dark_001.png
        //   identity via a local:// open  local://tools/testdata/openfolder\dark_001.png
        //
        // Two keys is two residents that can be reloaded independently and can
        // disagree about the same bytes - and since 項目20 it is also two Watch
        // subjects and two recordRemoteRule records. The print is an assertion
        // now, and S5 below carries it through to the registry, because two
        // keys that MATCH would still be two rows if nothing shared.
        {
            pickerAccept();
            double ts = nowSec();
            while (nowSec() - ts < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            bool local = !app.images.empty();
            for (const auto& d : app.images)
                if (!d->src || !d->src->remoteUrl.empty() ||
                    d->src->path.find("://") != std::string::npos) local = false;
            fprintf(stderr, "scanselftest: opened %d stack(s) / %d frame(s)\n",
                    (int)app.seqs.size(), (int)app.images.size());
            check(app.seqs.size() == 2 && app.images.size() == 6,
                  "S4a the picker's Load opens both stacks, six frames");
            check(local,
                  "S4b ...as LOCAL sources: path on disk, no remoteUrl (§6.2 identity)");
            if (!app.images.empty() && app.images[0]->src) {
                const FrameSource& S = *app.images[0]->src;
                // WHICH file this is, hand-derived: S3d fixed the group order
                // (dark_ before 暗電流, UTF-8 bytes) and the frames within a
                // group are file-number order, so images[0] is dark_001.png.
                // Asserted rather than assumed - every string below is built
                // from that name, and a different frame here would compare two
                // different files and pass for the wrong reason.
                const std::string base = baseName(S.path);
                check(base == "dark_001.png",
                      "S4c the frame these keys are about is dark_001.png");

                // The SAME file through the local:// door, spelled the way that
                // door really spells it - relative, as the Browse panel's own
                // url is (the measured line above), because that is the case
                // weakly_canonical has to resolve. A SESSION WRITTEN BEFORE THIS
                // CHANGE HOLDS EXACTLY THIS STRING: writeSessionTo records
                // FrameSource::path verbatim and the restore hands it back to
                // openRemote, so this doubles as the old-session line and S4d
                // is the statement that restoring one does not mint a second
                // identity for a file the new code keys from the local door.
                FrameSource asUrl = S;
                asUrl.path = "local://" + nonpy + "/" + base;
                asUrl.remoteUrl = asUrl.path;
                // ...and the same fields with a REAL peer's url, which must NOT
                // collapse: "/data/cap/dark_001.png on trc2" is not a path on
                // this disk and weakly_canonical would answer about the wrong
                // machine. This is the anti-vacuity half of S4d - a comparison
                // that cannot tell a peer from this disk would pass by
                // canonicalising everything, which is the wrong fix.
                FrameSource asPeer = S;
                asPeer.path = "ssh://trc2/data/cap/" + base;
                asPeer.remoteUrl = asPeer.path;
                const std::string kLocal = srcIdentityKey(S);
                const std::string kUrl   = srcIdentityKey(asUrl);
                const std::string kPeer  = srcIdentityKey(asPeer);
                fprintf(stderr, "scanselftest: identity via the local scan   : %s\n"
                                "scanselftest: identity via a local:// open  : %s\n"
                                "scanselftest: identity via a PEER's url     : %s\n",
                        kLocal.c_str(), kUrl.c_str(), kPeer.c_str());
                // Anti-vacuity: neither side may be empty, and both must still
                // name the file - a srcKeyPath that returned "" or "/" for
                // everything would make the equality below trivially true.
                check(!kLocal.empty() && !kUrl.empty() &&
                      kLocal.find(base) != std::string::npos &&
                      kUrl.find(base) != std::string::npos,
                      "S4d both keys are non-empty and name dark_001.png");
                check(kUrl.find("local://") == std::string::npos,
                      "S4d ...the url spelling was RESOLVED, not compared verbatim");
                check(kLocal == kUrl,
                      "S4d ...and the two doors write ONE §6.2 key for one file");
                check(kPeer != kLocal && kPeer.find("ssh://trc2/data/cap/") == 0,
                      "S4d ...while a PEER's url stays verbatim and stays different");
                // srcKeyPath itself, on strings alone: the reason the "://"
                // test exists, stated as a test. A peer's path is never
                // resolved against this machine's filesystem.
                check(srcKeyPath("ssh://trc2/data/cap/x.npy") ==
                          "ssh://trc2/data/cap/x.npy" &&
                      srcKeyPath("ssh://trc2:2222/data/x.npy") ==
                          "ssh://trc2:2222/data/x.npy",
                      "S4e srcKeyPath leaves a named host's path exactly as it found it");

                // WHICH FRAME OF IT - the same defect one field over, and the
                // reason S4d alone is not the whole claim. A multi-frame .npy
                // counts its frames in fileFrame through the local door and in
                // remoteFrame through the local:// one, so frame 0 agreed
                // (both slots 0) and every other frame did not. Hand-derived
                // from srcIdentityKey's format string: the local door's frame 3
                // is "...\n3|0\nnr0\n<mtime>,<fsize>" and the local:// door's
                // was "...\n0|3\n..." - one file, two keys, off a field the two
                // doors do not disagree about the MEANING of.
                FrameSource lf3 = S, uf3 = asUrl, pf3 = asPeer;
                lf3.fileFrame = 3;
                uf3.remoteFrame = 3;
                pf3.remoteFrame = 3;
                check(srcIdentityKey(lf3) == srcIdentityKey(uf3),
                      "S4f ...frame 3 of that file is one key through both doors too");
                // ...and the fold did not flatten the field it folded: frame 3
                // is not frame 0, and the peer's frame 3 is still the peer's.
                check(srcIdentityKey(lf3) != kLocal &&
                      srcIdentityKey(pf3) != srcIdentityKey(lf3),
                      "S4f ...and frame 3 is still neither frame 0 nor the peer's");
            }
        }

        // ---- S5: two doors, ONE resident (§6.2's consequence) -----------------
        // S4d compares STRINGS, and a key that reads the same would still leave
        // two rows in the registry if nothing consulted it. So the same file is
        // opened for real through both doors and the registry is counted:
        // shareOrRegisterSource has to hand the second door the FIRST door's
        // source, which is one source with two holders, not two sources.
        //
        // .npy and not one of the .png above, on purpose: peerServesName is true
        // for .npy, so openRemote really crosses the protocol and mints a
        // local:// source. A .png url is routed straight back to openPath by
        // #111 and would test the routing, not the identity.
        //
        // The fixture is a COPY under this test's own TMP, with a name holding
        // no digit run (no siblings, so openPath opens exactly one document and
        // the count below is arithmetic). Not a subdirectory of the shared
        // fixture folder: a subdirectory there is a ROW there, and
        // --browse-keys navigates by position (ad2d4fc).
        {
            closeAll();
            std::error_code sec;
            std::string scratch =
                (std::filesystem::temp_directory_path(sec) / "viewer_ident").u8string();
            std::replace(scratch.begin(), scratch.end(), '\\', '/');
            std::filesystem::remove_all(pathFromUtf8(scratch), sec);
            std::filesystem::create_directories(pathFromUtf8(scratch), sec);
            const std::string solo = scratch + "/solo.npy";
            std::filesystem::copy_file(pathFromUtf8(dir + "/80lx/frame_000.npy"),
                                       pathFromUtf8(solo),
                                       std::filesystem::copy_options::overwrite_existing, sec);
            check(!sec && std::filesystem::exists(pathFromUtf8(solo), sec),
                  "S5a the fixture: one .npy under this test's own TMP");

            // Door 1: File > Open, which stores the path on disk.
            openPath(solo);
            check(app.images.size() == 1 && !app.images.empty() &&
                  app.images[0]->src && app.images[0]->src->remoteUrl.empty(),
                  "S5b the local door opens it: one document, no url");
            // Door 2: the same bytes through the local peer, which stores the
            // url. Registered (not a preview) - a preview never registers or
            // adopts, and asserting on one would prove nothing about §6.2.
            const bool opened = openRemote("local://" + solo);
            fprintf(stderr, "scanselftest: S5 local://%s opened=%d docs=%d "
                            "sources=%d toast=\"%s\"\n",
                    solo.c_str(), opened ? 1 : 0, (int)app.images.size(),
                    srcMapSources(), app.toast.c_str());
            check(opened && app.images.size() == 2,
                  "S5c ...and the local:// door opens it too: two documents");
            srcMapDump("S5 two doors, one file");
            std::vector<SrcMapRow> rows = srcMapRows();
            bool one = rows.size() == 2 && rows[0].src == rows[1].src;
            bool two = rows.size() == 2 && rows[0].refs == 2 && rows[1].refs == 2;
            check(srcMapSources() == 1 && one,
                  "S5d TWO documents over ONE source: the second door SHARED");
            // refs is src.use_count(): the two documents and nothing else (the
            // registry holds a weak_ptr on purpose). A "2" that came from one
            // document holding its own source twice is excluded by S5c's count.
            check(two, "S5e ...and both say two holders (refcount 2, §6.2)");

            // ...and the other way round, because a one-sided fix passes the
            // test above: whichever door is FIRST owns the resident, and the
            // second adopts it (§6.2, "first resident wins").
            closeAll();
            const bool opened2 = openRemote("local://" + solo);
            openPath(solo);
            rows = srcMapRows();
            bool rone = rows.size() == 2 && rows[0].src == rows[1].src &&
                        rows[0].refs == 2 && rows[1].refs == 2;
            fprintf(stderr, "scanselftest: S5 reversed: opened=%d docs=%d sources=%d\n",
                    opened2 ? 1 : 0, (int)app.images.size(), srcMapSources());
            check(opened2 && app.images.size() == 2 && srcMapSources() == 1 && rone,
                  "S5f local:// first, then the local door: still one source, two holders");

            // ANTI-VACUITY for S5d/S5f: a registry that collapsed everything
            // onto one row would pass both. A DIFFERENT file must still be a
            // different source.
            closeAll();
            const std::string other = scratch + "/other.npy";
            std::filesystem::copy_file(pathFromUtf8(dir + "/160lx/frame_000.npy"),
                                       pathFromUtf8(other),
                                       std::filesystem::copy_options::overwrite_existing, sec);
            openPath(solo);
            openPath(other);
            check(app.images.size() == 2 && srcMapSources() == 2,
                  "S5g two DIFFERENT files are still two sources");
            closeAll();
            std::filesystem::remove_all(pathFromUtf8(scratch), sec);
        }

        fprintf(stderr, "scanselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        // opening stacks spun up the fetch and measure workers too; an unjoined
        // std::thread at exit is std::terminate, i.e. a "passing" test that dies
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
