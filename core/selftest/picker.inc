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
        fprintf(stderr, "pickerselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
