// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- derive a stack from a stack (docs/background/project/todo-open.md item 0) ---------------
    // Two synthesised stacks: A = f_000..f_007 (8 frames, pixel value = number),
    // B = f_000,001,002,004,006,007,009 (gaps at 3 and 5, one alien file, pixel
    // value = number + 100). B loads with CONTIGUOUS 0..6 frame numbers, which
    // is what makes today's follow-frame both silently mispair and diverge.
    // What has to hold:
    //   D1/D2  match counts are right in BOTH directions (both derive directions)
    //   D3     BEFORE deriving, follow-frame mispairs silently and then diverges
    //          - proven present first, or D6 would be vacuous
    //   D4     derived stack: frames in seqIndex order, numbers adopted from the
    //          other stack, pixels copied from the SOURCE even after the source's
    //          FILES ARE DELETED - the copy comes from RAM, never a re-read
    //   D5     the source stack is intact (copy-only, by user decision)
    //   D6     after deriving, follow-frame pairs the same FILE on both sides and
    //          never diverges - the reason the feature exists
    //   D7     membership is the FILE LIST: matched-but-not-resident frames are
    //          counted, reported and left out - never fetched
    //   D8     range + hand-picked rules; new batch on request; no series joined
    //   D11    a SeqInfo* held across an open or a close is invalidated - here,
    //          on this platform, not only where the allocator chose to grow
    if (!g_deriveSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "deriveselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto writeNpy = [](const std::string& path, int w, int h, float v) {
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }",
                     h, w);
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
        // The scratch area is a subdirectory this test CREATES, never the
        // directory it was handed. The first version did remove_all on the
        // argument itself, and the first time the suite pointed it at the
        // shared `multi` fixture it deleted it - every multi-based test then
        // failed with "picker did not open". A test may only destroy what it
        // made.
        // "derive-scratch" used to be a subdirectory of the handed folder,
        // which is tools/testdata - shared with every other selftest. A run that
        // did not reach its cleanup left a directory there for the next test to
        // walk. Counted here, compared at the bottom, fixtures moved to TMP.
        const SharedFixture deriveHanded(g_deriveSelftest);
        std::string root = selftestScratch("viewer_derive");
        std::string rootA = root + "/A", rootB = root + "/B";
        std::error_code ec;
        std::filesystem::create_directories(pathFromUtf8(rootA), ec);
        std::filesystem::create_directories(pathFromUtf8(rootB), ec);
        for (int i = 0; i < 8; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "f_%03d.npy", i);
            writeNpy(rootA + "/" + nm, 4, 4, (float)i);
        }
        const int bNums[] = { 0, 1, 2, 4, 6, 7, 9 };
        for (int n : bNums) {
            char nm[32];
            snprintf(nm, sizeof nm, "f_%03d.npy", n);
            writeNpy(rootB + "/" + nm, 4, 4, 100.0f + n);
        }
        app.seqLoadMode = 1;                    // always load numbered runs
        closeAll();
        openFolder(rootA);
        loadAll();
        int seqA = app.seqs.empty() ? 0 : app.seqs.front().id;
        openFolder(rootB);
        loadAll();
        int seqB = 0;
        for (const auto& s : app.seqs) if (s.id != seqA) seqB = s.id;
        {   // ---- D0: the fixture is what the comment above claims -----------
            std::vector<int> fb = framesOfSeq(seqB);
            bool contiguous = true;
            for (int k = 0; k < (int)fb.size(); k++)
                if (app.images[fb[k]]->seqIndex != k) contiguous = false;
            fprintf(stderr, "deriveselftest: D0 stacks=%zu A=%zu frames B=%zu frames "
                            "(B renumbered contiguously: %d)\n",
                    app.seqs.size(), framesOfSeq(seqA).size(), fb.size(),
                    contiguous ? 1 : 0);
            check(app.seqs.size() == 2 && framesOfSeq(seqA).size() == 8 &&
                  fb.size() == 7 && contiguous,
                  "D0 fixture: 8-frame A, 7-frame B with gaps renumbered 0..6");
        }
        {   // ---- D1/D2: match counts, in BOTH directions ---------------------
            DerivePlan P = buildDerivePlan(seqA, DR_NAME_IN, seqB, 0, 0, nullptr);
            fprintf(stderr, "deriveselftest: D1 A vs B by name: %d of %d here paired, "
                            "%d of B's %d without counterpart, %d to copy\n",
                    P.pairs, P.srcKnown, P.otherUnmatched, P.otherKnown,
                    (int)P.picks.size());
            check(P.pairs == 6 && P.srcKnown == 8 && P.srcMatched == 6 &&
                  P.otherKnown == 7 && P.otherUnmatched == 1 &&
                  (int)P.picks.size() == 6 && P.notResident == 0,
                  "D1 counts both ways: 6 of 8 match; 1 of B's 7 unmatched");
            DerivePlan R = buildDerivePlan(seqB, DR_NAME_IN, seqA, 0, 0, nullptr);
            check(R.pairs == 6 && R.srcKnown == 7 && R.otherKnown == 8 &&
                  R.otherUnmatched == 2 && (int)R.picks.size() == 6,
                  "D2 reverse: 6 of 7 match; 2 of A's 8 unmatched");
        }
        {   // ---- D3: the defect, present BEFORE the derive -------------------
            setCompareB(app.images[framesOfSeq(seqB).front()].get());
            app.compareMode = App::CmpWipe;
            selectImage(framesOfSeq(seqA)[3]);          // f_003
            ImageDoc* b3 = resolveB();
            bool mispair = b3 && !g_abFollowDiverged &&
                           baseName(b3->src->path) == "f_004.npy";
            fprintf(stderr, "deriveselftest: D3 A#3 (f_003) follows to %s, diverged=%d\n",
                    b3 ? baseName(b3->src->path).c_str() : "(null)",
                    g_abFollowDiverged ? 1 : 0);
            check(mispair, "D3 today: same-number follow pairs the WRONG file, silently");
            selectImage(framesOfSeq(seqA)[7]);          // f_007: B has no #7
            resolveB();
            check(g_abFollowDiverged, "D3 today: follow-frame diverges past B's numbers");
        }
        // the source's files go away NOW: what follows must never touch disk
        std::filesystem::remove_all(pathFromUtf8(rootA), ec);
        std::string nameA = seqInfo(seqA)->name;
        int imagesBefore = (int)app.images.size();
        const size_t bytesBeforeDerive = residentImageBytes();   // D9's baseline
        int derived = 0;
        {   // ---- D4: derive A' = A matched to B by name ----------------------
            DerivePlan P = buildDerivePlan(seqA, DR_NAME_IN, seqB, 0, 0, nullptr);
            derived = applyDerivePlan(P, false);
            std::vector<int> fd = framesOfSeq(derived);
            bool ascending = true, adopted = true;
            for (int k = 0; k < (int)fd.size(); k++) {
                if (k > 0 && app.images[fd[k]]->seqIndex <=
                             app.images[fd[k - 1]]->seqIndex) ascending = false;
                if (app.images[fd[k]]->seqIndex != k) adopted = false;
            }
            check(derived != 0 && fd.size() == 6, "D4 derived stack has the 6 matches");
            check(ascending, "D4 derived frames sit in seqIndex order");
            check(adopted, "D4 matched frames adopted B's numbers (0..5)");
            bool pixels = false, noteSame = true;
            std::string note0 = fd.empty() ? "" : app.images[fd[0]]->note;
            for (int idx : fd) {
                const ImageDoc& d = *app.images[idx];
                if (d.seqIndex == 3)
                    pixels = baseName(d.src->path) == "f_004.npy" &&
                             fabsf(d.px()[0] - 4.0f) < 1e-6f;
                if (d.note != note0 || d.note.empty()) noteSame = false;
            }
            check(pixels, "D4 pixels copied from the SOURCE, files already deleted");
            check(noteSame, "D4 every derived frame carries the same origin note");
            const App::SeqInfo* dsi = seqInfo(derived);
            bool named = dsi && dsi->name.find(nameA) == 0 &&
                         dsi->name.find("same names as") != std::string::npos &&
                         dsi->name.find("6 of 8") != std::string::npos;
            fprintf(stderr, "deriveselftest: D4 derived name: %s\n",
                    dsi ? dsi->name.c_str() : "(null)");
            check(named, "D4 the derived stack names its origin and its rule");
        }
        {   // ---- D5: the source is intact ------------------------------------
            check(framesOfSeq(seqA).size() == 8 && seqInfo(seqA) &&
                  seqInfo(seqA)->name == nameA &&
                  (int)app.images.size() == imagesBefore + 6,
                  "D5 source stack untouched; exactly 6 docs were added");
        }
        {   // ---- D6: follow-frame no longer diverges - the reason this exists
            bool follows = true, sameFile = true, paired = true;
            for (int idx : framesOfSeq(derived)) {
                selectImage(idx);
                ImageDoc* b = resolveB();
                if (!b || g_abFollowDiverged) { follows = false; continue; }
                if (baseName(b->src->path) != baseName(cur()->src->path)) sameFile = false;
                if (fabsf(b->px()[0] - (cur()->px()[0] + 100.0f)) > 1e-4f) paired = false;
            }
            check(follows, "D6 follow-frame never diverges across the derived stack");
            check(sameFile, "D6 A/B now show the SAME file on both sides");
            check(paired, "D6 the paired pixels really are the counterpart's");
        }
        {   // ---- D9: derive references, it does not copy (§6.1/§7) -----------
            const size_t bytesNow = residentImageBytes();
            fprintf(stderr, "deriveselftest: D9 resident bytes %zu before derive, "
                            "%zu after\n", bytesBeforeDerive, bytesNow);
            check(bytesNow == bytesBeforeDerive, "D9 derive adds no resident bytes");
        }
        {   // ---- D10: the stage-1 CoW fires for real - cropping the ORIGINAL
            // after a derive re-scopes only the original; the derived stack's
            // pixels, identity and shape do not move (§2.2)
            int srcIdx = -1;                 // A's f_004 (value 4) = derived #3
            for (int idx : framesOfSeq(seqA))
                if (baseName(app.images[idx]->src->path) == "f_004.npy") srcIdx = idx;
            int dIdx = -1;
            for (int idx : framesOfSeq(derived))
                if (app.images[idx]->seqIndex == 3) dIdx = idx;
            check(srcIdx >= 0 && dIdx >= 0, "D10 the original/derived pair exists");
            if (srcIdx >= 0 && dIdx >= 0) {
                ImageDoc& sdoc = *app.images[srcIdx];
                ImageDoc& ddoc = *app.images[dIdx];
                const uint64_t sharedId = ddoc.src->srcId;
                check(sdoc.src.get() == ddoc.src.get() && sdoc.src->srcId == sharedId,
                      "D10 derived member shares the original's source");
                cropInPlace(sdoc, 0, 0, 2, 2);
                fprintf(stderr, "deriveselftest: D10 cropped the original: srcId "
                                "%llu->%llu (derived keeps %llu), original %dx%d, "
                                "derived %dx%d px[0]=%g\n",
                        (unsigned long long)sharedId,
                        (unsigned long long)sdoc.src->srcId,
                        (unsigned long long)ddoc.src->srcId,
                        sdoc.w, sdoc.h, ddoc.w, ddoc.h, (double)ddoc.px()[0]);
                check(sdoc.src->srcId != sharedId,
                      "D10 cropping the shared original makes a NEW source (CoW)");
                check(ddoc.src->srcId == sharedId, "D10 the derived stack keeps its srcId");
                check(ddoc.w == 4 && ddoc.h == 4 && fabsf(ddoc.px()[0] - 4.0f) < 1e-6f,
                      "D10 the derived pixels did not move under the crop");
                check(sdoc.w == 2 && sdoc.h == 2, "D10 the original really was cropped");
            }
        }
        {   // ---- D7: membership is the FILE LIST, copies are the resident ----
            App::SeqInfo* sa = seqInfo(seqA);
            sa->remoteFiles.clear();
            for (int i = 0; i < 8; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "f_%03d.npy", i);
                sa->remoteFiles.push_back(rootA + "/" + nm);
            }
            sa->remoteFiles.push_back(rootA + "/f_009.npy");   // listed, never fetched
            sa->expectedFrames = 9;
            DerivePlan P = buildDerivePlan(seqA, DR_NAME_IN, seqB, 0, 0, nullptr);
            fprintf(stderr, "deriveselftest: D7 file-list match: %d of %d paired, "
                            "%d matched, %d not resident, %d to copy\n",
                    P.pairs, P.srcKnown, P.srcMatched, P.notResident,
                    (int)P.picks.size());
            check(P.srcKnown == 9 && P.pairs == 7 && P.srcMatched == 7 &&
                  P.notResident == 1 && (int)P.picks.size() == 6 &&
                  P.otherUnmatched == 0,
                  "D7 matching works on the file list, beyond resident frames");
            int nid = applyDerivePlan(P, false);
            const App::SeqInfo* dsi = seqInfo(nid);
            bool honest = dsi && framesOfSeq(nid).size() == 6 &&
                          dsi->expectedFrames == 6 &&
                          !framesOfSeq(nid).empty() &&
                          app.images[framesOfSeq(nid).front()]->note.find(
                              "not loaded") != std::string::npos;
            check(honest, "D7 the miss is reported in the note, not fetched");
            // applyDerivePlan() appended a stack, and app.seqs holds SeqInfo BY
            // VALUE - so that push_back can reallocate, and every SeqInfo* taken
            // before it (sa, twenty lines up) is dangling afterwards. Ask for it
            // again rather than reusing the old one.
            //
            // This is why the Windows CI job was the only one that failed the
            // day it first ran these: MSVC's vector grows 1.5x and reallocates
            // on exactly this push, so the two lines below wrote through freed
            // memory and the process died with no output. libstdc++ grows 2x and
            // had spare capacity, so the same write landed on memory that was
            // still mapped and nothing ever showed. Forcing cap==size here
            // reproduced it on this box: seqInfo(seqA) != sa.
            sa = seqInfo(seqA);
            sa->remoteFiles.clear();                 // leave the source honest
            sa->expectedFrames = 8;
        }
        {   // ---- D8: range, hand-picked, new batch, no series ----------------
            DerivePlan Pr = buildDerivePlan(seqB, DR_RANGE, 0, 2, 5, nullptr);
            bool rangeOk = (int)Pr.picks.size() == 4;
            for (const auto& pk : Pr.picks)
                if (pk.newIndex < 2 || pk.newIndex > 5) rangeOk = false;
            check(rangeOk, "D8 range rule keeps exactly the numbers 2..5");
            int serId = newSeries(batchOfStack(seqB), "derive-test sweep");
            if (App::Series* S = seriesById(serId)) {
                App::Series::Member m;
                m.seqId = seqB;
                S->members.push_back(m);
            }
            std::vector<char> mask(7, 0);
            mask[0] = mask[6] = 1;                   // f_000 and the alien f_009
            DerivePlan Pp = buildDerivePlan(seqB, DR_PICK, 0, 0, 0, &mask);
            int bBatch = app.images[framesOfSeq(seqB).front()]->batchId;
            int nid = applyDerivePlan(Pp, true);
            std::vector<int> fd = framesOfSeq(nid);
            bool picked = fd.size() == 2 &&
                          baseName(app.images[fd[0]]->src->path) == "f_000.npy" &&
                          baseName(app.images[fd[1]]->src->path) == "f_009.npy";
            check(picked, "D8 hand-picked rule copies exactly the ticked frames");
            check(!fd.empty() && app.images[fd[0]]->batchId != bBatch,
                  "D8 'into a new batch' really is a new batch");
            check(framesOfSeq(seqB).size() == 7, "D8 picking from B left B whole");
            check(seriesOfStack(seqB) != nullptr && seriesOfStack(nid) == nullptr,
                  "D8 the derived stack joins NO series (membership is explicit)");
        }
        {   // ---- D11: a SeqInfo* held across a mutation ALWAYS dangles --------
            // The rule written over seqInfo() ("never hold it across an open or
            // a close") is only a rule if breaking it fails HERE rather than at
            // a customer. It used to be a fact about the allocator: D7 above
            // records the day this exact push_back killed the Windows job while
            // libstdc++'s spare capacity kept the same pointer usable on this
            // box, so the defect was real for three weeks and visible nowhere.
            // App::seqs reseats on every mutation now, and these two asserts are
            // what says so on all three CI platforms.
            //
            // Nothing below DEREFERENCES the stale pointer - that is undefined
            // however the table is implemented. holds() is the defined question,
            // and asking it is the whole test.
            const App::SeqInfo* held = seqInfo(seqA);
            const unsigned long long rev0 = app.seqs.rev();
            check(held != nullptr && app.seqs.holds(held),
                  "D11 a pointer just returned by seqInfo() is live");
            DerivePlan Pg = buildDerivePlan(seqB, DR_RANGE, 0, 0, 1, nullptr);
            int gid = applyDerivePlan(Pg, false);
            fprintf(stderr, "deriveselftest: D11 open: seqs.rev %llu -> %llu, held "
                            "pointer %s, seqInfo(seqA) %s\n",
                    rev0, app.seqs.rev(),
                    app.seqs.holds(held) ? "STILL LIVE" : "invalidated",
                    seqInfo(seqA) == held ? "unchanged" : "moved");
            check(gid != 0 && app.seqs.rev() == rev0 + 1,
                  "D11 opening a stack is one mutation of the stack table");
            check(!app.seqs.holds(held) && seqInfo(seqA) != held,
                  "D11 an open invalidates a pointer held across it");
            const App::SeqInfo* held2 = seqInfo(seqA);
            const unsigned long long rev1 = app.seqs.rev();
            closeStack(gid);
            check(app.seqs.rev() > rev1 && !app.seqs.holds(held2),
                  "D11 a close invalidates one too - not just an open");
            check(seqInfo(gid) == nullptr && seqInfo(seqA) != nullptr,
                  "D11 the close took the right stack and left the others");
        }
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        // the folder this test was handed must be exactly as it was found
        if (!deriveHanded.intact("deriveselftest")) ok = false;
        fprintf(stderr, "deriveselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
