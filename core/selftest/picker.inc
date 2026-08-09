// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // Remote Open Folder, verifiable without a human or a second machine: connect
    // to the local peer, scan a folder-of-folders, and count what opened. Goes
    // through the REAL path - rb worker, LIST protocol, grouping, stack open.
    // The picker's two new promises, verifiable without a human. UC3: a live
    // filter cuts each group's FILE list, and accepting loads exactly the cut.
    // UC2: merge mode folds the whole selection into ONE stack, union of files
    // in natural order. Runs against an abset-style folder (A/B variants of
    // the same numbering in one directory).
    if (!g_pickerSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto stackFiles = [&](int seqId) {
            std::vector<std::string> out;
            for (int idx : framesOfSeq(seqId)) out.push_back(app.images[idx]->src->path);
            return out;
        };
        // a shared source keeps the FIRST opener's path spelling (the registry
        // canonicalizes identity, not provenance), so compare files as
        // identities, not byte-for-byte strings
        auto sameFilesInOrder = [&](const std::vector<std::string>& a,
                                    const std::vector<std::string>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++)
                if (srcKeyPath(a[i]) != srcKeyPath(b[i])) return false;
            return true;
        };
        auto joinBase = [&](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& f : v) { s += s.empty() ? "" : ","; s += baseName(f); }
            return s;
        };
        bool ok = true;

        // ---- UC3: filter "_A", one stack per group -> only the A files load
        // Scanned through a NATIVE path, separators and all - which is what a
        // folder dialog hands over on Windows, and the input the yen-sign bug
        // needs: the CJK font draws U+005C as the yen sign, so a path shown
        // raw reads "C:(yen)Users(yen)hish". What the picker DISPLAYS is
        // checked below.
        {
            std::string root = g_pickerSelftest;
#if defined(_WIN32)
            std::replace(root.begin(), root.end(), '/', '\\');
#endif
            openFolder(root);
        }
        if (!app.folderPickOpen) {
            fprintf(stderr, "pickerselftest: picker did not open\n");
            return 1;
        }
        {   // no separator survives to the screen as the codepoint the font
            // draws as a yen sign
            std::string bad;
            auto noBS = [&](const std::string& what, const std::string& shown) {
                if (shown.find('\\') != std::string::npos && bad.empty())
                    bad = what + ": " + shown;
            };
            noBS("picker header", dispPath(app.folderPickRoot));
            for (const auto& e : app.folderPick) {
                noBS("group name", e.g.name);
                for (const auto& r : e.rel) noBS("file row", r);
            }
            fprintf(stderr, "pickerselftest: scanned \"%s\" -> header reads \"%s\": %s%s\n",
                    app.folderPickRoot.c_str(), dispPath(app.folderPickRoot).c_str(),
                    bad.empty() ? "ok" : "FAIL ", bad.c_str());
            if (!bad.empty()) ok = false;
        }
        int total = 0;
        for (const auto& e : app.folderPick) total += (int)e.g.files.size();
        snprintf(app.pickFilter, sizeof app.pickFilter, "_A");
        applyPickFilter();
        app.pickMerge = 0;
        std::vector<App::PendingGroup> want = pickerSelection();
        std::vector<std::string> wantFiles;
        for (const auto& g : want)
            wantFiles.insert(wantFiles.end(), g.files.begin(), g.files.end());
        pickerAccept();
        loadAll();
        std::vector<std::string> got =
            app.seqs.empty() ? std::vector<std::string>() : stackFiles(app.seqs[0].id);
        fprintf(stderr, "pickerselftest: UC3 filter \"_A\" kept %d of %d files -> "
                        "%d stack(s), loaded [%s]\n",
                (int)wantFiles.size(), total, (int)app.seqs.size(), joinBase(got).c_str());
        if ((int)wantFiles.size() >= total || wantFiles.empty()) {
            fprintf(stderr, "pickerselftest: UC3 FAILED - filter cut nothing\n");
            ok = false;
        }
        if (app.seqs.size() != want.size() || got != wantFiles) {
            fprintf(stderr, "pickerselftest: UC3 FAILED - loaded files != filtered selection\n");
            ok = false;
        }

        // ---- UC6: a group the filter cut to ONE 2-D file cannot join a sweep --
        // "Open as a sweep" resolves STACKS. A lone frame never becomes one
        // (loadNpy calls addImage with no SeqInfo) and resolveOnePendingSeries
        // walks app.seqs, so it is structurally unmatchable - which the user used
        // to discover at the far end of the load as "N could not be matched".
        {
            openFolder(g_pickerSelftest);
            snprintf(app.pickFilter, sizeof app.pickFilter, "00_A");
            applyPickFilter();
            int cut = 0, lone = pickLoneImageGroups();
            std::string shp;
            for (const auto& e : app.folderPick)
                if (e.selected && e.nMatch == 1) { cut++; shp = e.g.shape; }
            fprintf(stderr, "pickerselftest: UC6 filter \"00_A\" -> %d group(s) cut to one "
                            "file (shape \"%s\"), %d cannot join a sweep\n",
                    cut, shp.c_str(), lone);
            if (cut != 1 || lone != 1) {
                fprintf(stderr, "pickerselftest: UC6 FAILED - a group cut to one 2-D frame "
                                "must be reported as unable to join a sweep\n");
                ok = false;
            }
            app.pickFilter[0] = '\0';
            applyPickFilter();
            app.folderPick.clear();
            app.folderPickOpen = false;
        }

        // ---- UC2: no filter, merge mode -> ONE stack, union, natural order
        size_t seqsBefore = app.seqs.size();
        openFolder(g_pickerSelftest);          // reopens the picker, filter cleared
        app.pickMerge = 1;
        std::vector<App::PendingGroup> mwant = pickerSelection();
        pickerAccept();
        loadAll();
        std::vector<std::string> mgot = app.seqs.size() == seqsBefore + 1
            ? stackFiles(app.seqs.back().id) : std::vector<std::string>();
        fprintf(stderr, "pickerselftest: UC2 merge -> +%d stack(s) '%s', order [%s]\n",
                (int)(app.seqs.size() - seqsBefore),
                app.seqs.empty() ? "" : app.seqs.back().name.c_str(), joinBase(mgot).c_str());
        if (mwant.size() != 1 || app.seqs.size() != seqsBefore + 1) {
            fprintf(stderr, "pickerselftest: UC2 FAILED - merge did not make ONE stack\n");
            ok = false;
        } else if (!sameFilesInOrder(mgot, mwant[0].files) || (int)mgot.size() != total) {
            fprintf(stderr, "pickerselftest: UC2 FAILED - merged stack != union in natural order\n");
            ok = false;
        }
        // ---- UC5: batch mode "one per top folder" -> N batches, unique names
        {
            std::string bsdir;
            {   // fixture: batchset/ next to the given folder, else testdata
                std::string d2 = g_pickerSelftest;
                std::replace(d2.begin(), d2.end(), '\\', '/');
                size_t sl = d2.find_last_of('/');
                bsdir = (sl == std::string::npos ? std::string(".") : d2.substr(0, sl))
                      + "/batchset";
                std::error_code ec;
                if (!std::filesystem::is_directory(pathFromUtf8(bsdir), ec))
                    bsdir = "tools/testdata/batchset";
            }
            size_t imagesBefore = app.images.size();
            openFolder(bsdir);
            if (!app.folderPickOpen) {
                fprintf(stderr, "pickerselftest: UC5 FAILED - picker did not open\n");
                ok = false;
            } else {
                app.pickBatchMode = 1;             // the new footer radio
                pickerAccept();
                loadAll();
                std::vector<int> bids;             // distinct batches actually used
                for (size_t i = imagesBefore; i < app.images.size(); i++) {
                    int b = app.images[i]->batchId;
                    if (std::find(bids.begin(), bids.end(), b) == bids.end())
                        bids.push_back(b);
                }
                bool stacksOneBatch = true;        // a stack never straddles batches
                for (const auto& si : app.seqs) {
                    int b0 = -1;
                    for (size_t i = imagesBefore; i < app.images.size(); i++) {
                        if (app.images[i]->seqId != si.id) continue;
                        if (b0 < 0) b0 = app.images[i]->batchId;
                        else if (app.images[i]->batchId != b0) stacksOneBatch = false;
                    }
                }
                bool namesUnique = true;           // sessions restore by NAME
                for (size_t a2 = 0; a2 < app.batches.size(); a2++)
                    for (size_t b2 = a2 + 1; b2 < app.batches.size(); b2++)
                        if (app.batches[a2].name == app.batches[b2].name)
                            namesUnique = false;
                std::string names;
                for (int b : bids)
                    for (const auto& bb : app.batches)
                        if (bb.id == b) names += (names.empty() ? "" : ",") + bb.name;
                fprintf(stderr, "pickerselftest: UC5 batch-per-top-folder -> %d batch(es) "
                                "[%s], stacks one-batch=%d, names unique=%d\n",
                        (int)bids.size(), names.c_str(), stacksOneBatch ? 1 : 0,
                        namesUnique ? 1 : 0);
                if (bids.size() != 3 || !stacksOneBatch || !namesUnique) {
                    fprintf(stderr, "pickerselftest: UC5 FAILED - batches wrong\n");
                    ok = false;
                }
            }
        }
        // ---- UC7: a numbered family whose SHAPES disagree is not one stack ----
        // The grouper reads NAMES, and names are all it can read: .raw and .bin
        // declare no geometry at all, and peeking the header of every file in a
        // 300-frame run would read the folder twice to answer a question the
        // decode answers for free. So four files called shape_1..shape_4 are one
        // group whatever is inside them - that part is right and is asserted
        // here so it stays right. What was wrong is what happened NEXT: every
        // sibling was stacked whatever it turned out to hold. docs/terminology.md
        // gives a stack a TIME axis - the same picture measured again - so
        // sigma_t, FPN separation and the per-frame table are quantities only if
        // the frames are one shape; four different pictures under one seqId make
        // all three into numbers with no meaning, which is the failure the whole
        // terminology exists to prevent. The disagreeing sibling is refused BY
        // NAME instead, in the words core/tiffread.cpp already refuses a
        // disagreeing PAGE in.
        //
        // The shapes are (8,8), (8,8,3) and (4,4) ON PURPOSE. Issue #71 - whether
        // a trailing axis of 1 or 2 counts as channels - is open, and every
        // reading in it agrees about these three, so this test neither answers
        // #71 nor depends on the answer. What it pins is that siblings which
        // disagree do not silently stack, which has to hold either way.
        //
        // Fixtures are written here rather than read from tools/testdata, for the
        // reasons the --remote-selftest channel sweep gives at length: that tree
        // is gitignored and generated, so a machine without it would skip the
        // check in silence, and a fixture living apart from the rule it pins
        // drifts away from it. The per-test TMP directory CMake hands this test
        // is also the one place no other selftest scans.
        {
            closeAll();
            std::error_code sec;
            std::filesystem::path sdir =
                std::filesystem::temp_directory_path(sec) / "viewer_pickershape";
            std::filesystem::remove_all(sdir, sec);
            std::filesystem::create_directories(sdir, sec);
            std::vector<uint16_t> ramp(8 * 8 * 3);
            for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (uint16_t)(i * 13 + 7);
            // shape_1 and shape_4 are the stack. shape_2 disagrees in CHANNELS
            // and shape_3 in GRID, because those are two different ways for a
            // capture to go wrong and a check on one is not a check on the other.
            npyWriteFile((sdir / "shape_1.npy").u8string(), "<u2",
                         { 8, 8 }, ramp.data(), 64 * 2);
            const std::string s2 = npyWriteFile((sdir / "shape_2.npy").u8string(), "<u2",
                                                { 8, 8, 3 }, ramp.data(), 64 * 3 * 2);
            npyWriteFile((sdir / "shape_3.npy").u8string(), "<u2",
                         { 4, 4 }, ramp.data(), 16 * 2);
            npyWriteFile((sdir / "shape_4.npy").u8string(), "<u2",
                         { 8, 8 }, ramp.data(), 64 * 2);

            auto grp = scanFolderGroups(sdir.u8string());
            fprintf(stderr, "pickerselftest: UC7 scan -> %zu group(s), %zu file(s)\n",
                    grp.size(), grp.empty() ? (size_t)0 : grp[0].files.size());
            if (grp.size() != 1 || grp[0].files.size() != 4) {
                fprintf(stderr, "pickerselftest: UC7 FAILED - four numbered files must "
                                "still scan as ONE group: the fix is not a name rule\n");
                ok = false;
            }

            // app.seqNote carries the loader's word to the Files panel and is
            // REPLACED per pump, so it is collected as the load proceeds rather
            // than read once at the end - two refusals need not arrive together.
            std::string notes;
            auto loadCollect = [&]() {
                double t0 = nowSec();
                while (nowSec() - t0 < 120.0) {
                    if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                    pumpSequenceAndQueue();
                    if (!app.seqNote.empty() && notes.find(app.seqNote) == std::string::npos)
                        notes += app.seqNote;
                    if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                        !app.folderPickOpen) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            app.seqNote.clear();
            app.pickMerge = 0;              // UC2/UC5 left the footer radios where
            app.pickBatchMode = 0;          // they were: this case answers for none
            openFolder(sdir.u8string());
            loadCollect();
            std::vector<std::string> got =
                app.seqs.size() == 1 ? stackFiles(app.seqs[0].id) : std::vector<std::string>();
            fprintf(stderr, "pickerselftest: UC7 loaded %d doc(s), %d stack(s) [%s]\n",
                    (int)app.images.size(), (int)app.seqs.size(), joinBase(got).c_str());
            if (app.images.size() != 2 || got.size() != 2 ||
                baseName(got[0]) != "shape_1.npy" || baseName(got[1]) != "shape_4.npy") {
                fprintf(stderr, "pickerselftest: UC7 FAILED - the stack must hold the two "
                                "8x8 1ch frames and NOTHING else\n");
                ok = false;
            }
            // ...and it must SAY which files it left out, and why. Both sides of
            // the comparison by name, the rule in the canon's words: a refusal
            // nobody can read is the silent stacking with extra steps.
            const char* WANT[] = {
                "shape_2.npy: 8x8 3ch and shape_1.npy is 8x8 1ch: a run of numbered files "
                "is a stack, and a stack's frames are one shape - open it on its own",
                "shape_3.npy: 4x4 1ch and shape_1.npy is 8x8 1ch: a run of numbered files "
                "is a stack, and a stack's frames are one shape - open it on its own",
            };
            for (const char* w : WANT)
                if (notes.find(w) == std::string::npos) {
                    fprintf(stderr, "pickerselftest: UC7 FAILED - the refusal never said\n"
                                    "  want: %s\n  said: %s\n", w, notes.c_str());
                    ok = false;
                }

            // The way out the refusal names has to exist. Opening the odd file
            // ITSELF makes it the reference frame, so the sibling offer stays on
            // ("always", the setting under which the stack above was built) and
            // the file still opens as what it is.
            closeAll();
            const int mode = app.seqLoadMode;
            app.seqLoadMode = 1;
            openPath(s2);
            loadCollect();
            app.seqLoadMode = mode;
            const ImageDoc* alone = app.images.empty() ? nullptr : app.images[0].get();
            fprintf(stderr, "pickerselftest: UC7 \"open it on its own\" -> %d doc(s) %dx%d %dch\n",
                    (int)app.images.size(), alone ? alone->w : 0, alone ? alone->h : 0,
                    alone ? alone->ch : 0);
            if (app.images.size() != 1 || !alone ||
                alone->w != 8 || alone->h != 8 || alone->ch != 3) {
                fprintf(stderr, "pickerselftest: UC7 FAILED - the refused file must still "
                                "open ALONE, as its own shape\n");
                ok = false;
            }
            closeAll();
            std::filesystem::remove_all(sdir, sec);      // ours: ours to remove
        }
        fprintf(stderr, "pickerselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
