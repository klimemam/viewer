// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- reload from disk: the §3.2 walk, headless -----------------------------
    // What has to hold when a frame's FILE is rewritten under a live stack:
    //   R3/R4  identity survives the reload (uid, srcId), and EVERY membership
    //          sharing the source gets new mirrors/dataRev in the same step
    //   R7/R8  sigma_t is recomputed even when frame count, ROI and CFA are all
    //          unchanged - SeqInfo::stackRev in the temporal key is what lets a
    //          latched TemporalState notice the reload at all
    //   R9     forgetImage forgets the extra compare-slot states too
    //   R24    a crop is a change to the STACK's data, so it moves the same
    //          revision the temporal key reads - a member that no longer has
    //          the stack's shape leaves its numbers, and they say so
    //   R12    frame-axis provenance (fileFrame) reloads the RIGHT frame, and
    //          refuses one the file no longer has
    if (!g_reloadSelftest.empty()) {
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
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "reloadselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        // one 2-D f32 .npy: value v + ramp*k over the pixels (ramp 0 = flat)
        auto writeNpy2 = [](const std::string& path, int w, int h, float v, float ramp) {
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
            std::vector<float> px((size_t)w * h);
            for (size_t k = 0; k < px.size(); k++) px[k] = v + ramp * (float)k;
            f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        };
        // one frame-axis (F,h,w) f32 .npy: frame f is flat at base + 10*f
        auto writeNpy3 = [](const std::string& path, int F, int w, int h, float base) {
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d, %d), }",
                     F, h, w);
            std::string hdr = dict;
            size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
            hdr.append(pad, ' ');
            hdr += '\n';
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            uint16_t hl = (uint16_t)hdr.size();
            f.write("\x93NUMPY\x01\x00", 8);
            f.write((const char*)&hl, 2);
            f.write(hdr.data(), (std::streamsize)hdr.size());
            for (int fr = 0; fr < F; fr++) {
                std::vector<float> px((size_t)w * h, base + 10.0f * fr);
                f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
            }
        };
        // the scratch area is a subdirectory this test CREATES, never the
        // directory it was handed (the derive test's remove_all lesson)
        std::string root = g_reloadSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/reload-scratch";
        std::string stk = root + "/stk";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(stk), ec);
        for (int i = 0; i < 5; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "a_%03d.npy", i);
            writeNpy2(stk + "/" + nm, 4, 4, (float)i, 0);
            snprintf(nm, sizeof nm, "b_%03d.npy", i);
            writeNpy2(stk + "/" + nm, 4, 4, 100.0f + i, 0);
        }
        writeNpy2(root + "/loose.npy", 4, 4, 7.0f, 0);
        writeNpy3(root + "/c.npy", 5, 8, 8, 50.0f);
        app.seqLoadMode = 1;
        // c.npy is (5,8,8): §3.1's native rule reads it as frames both before
        // and after R12 shrinks it to (2,8,8) - the last axis (8) is never a
        // channel count - so fileFrame keeps its meaning across the shrink.
        // The RECORDED-reading questions (--npy-axis's successor, §3.3) are
        // R18's, on a file whose declared reading differs from native.
        closeAll();
        openFolder(stk);
        loadAll();
        int seqA = 0, seqB = 0;
        for (const auto& s : app.seqs) {
            if (s.name.rfind("a_", 0) == 0) seqA = s.id;
            if (s.name.rfind("b_", 0) == 0) seqB = s.id;
        }
        std::string lErr = loadNpy(root + "/loose.npy");
        ImageDoc* loose = lErr.empty() ? app.images.back().get() : nullptr;
        std::string cErr = loadNpy(root + "/c.npy");
        int seqC = 0;
        for (const auto& s : app.seqs)
            if (s.id != seqA && s.id != seqB) seqC = s.id;
        auto docOf = [&](int seqId, int seqIndex) -> ImageDoc* {
            for (int idx : framesOfSeq(seqId))
                if (app.images[idx]->seqIndex == seqIndex) return app.images[idx].get();
            return nullptr;
        };
        ImageDoc* a2 = docOf(seqA, 2);
        ImageDoc* c2 = docOf(seqC, 2);
        {   // ---- R0: fixture + sharing, manufactured by hand ------------------
            // app-level sharing arrives with the source registry (stage 2); the
            // WALK must already handle it, so the test wires it up directly
            if (a2 && loose) { loose->src = a2->src; loose->syncMirrors(); }
            check(seqA && seqB && seqC && a2 && loose && c2 && cErr.empty() &&
                  framesOfSeq(seqA).size() == 5 && framesOfSeq(seqB).size() == 5 &&
                  framesOfSeq(seqC).size() == 5 && loose->seqId == 0 &&
                  a2->src.use_count() == 2 && loose->px()[0] == 2.0f,
                  "R0 fixture: 3 stacks + loose frame; sharing manufactured");
            if (!a2 || !loose || !c2) {
                fprintf(stderr, "reloadselftest: FAILED (fixture)\n");
                stopRbWorker();
                stopSequenceLoader();
                stopRemoteFetcher();
                stopMeasureWorker();
                return 1;
            }
        }
        {   // series over the stk batch (stacks A and B), values set by hand
            int sid = selftestMakeSeries(a2->batchId, "lx");
            if (App::Series* S = seriesById(sid))
                for (auto& m : S->members) m.value = m.seqId == seqA ? 10.0 : 20.0;
            linRecompute(sid);
        }
        float sig0 = 0, sigX0 = 0;
        double hMean0 = 0;
        {   // ---- R1: measurements over the OLD pixels -------------------------
            recomputeHistogramIfNeeded(a2, app.hist[0]);
            hMean0 = app.hist[0].mean[0];
            recomputeProjectionIfNeeded(a2, app.proj[0]);
            recomputeTemporalIfNeeded(a2, app.temporal[0]);
            sig0 = (float)app.temporal[0].tempNoise[0];
            // a compare slot's cache: the TemporalState forgetImage used to miss
            app.temporalExtra.resize(1);
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);
            sigX0 = (float)app.temporalExtra[0].tempNoise[0];
            fprintf(stderr, "reloadselftest: R1 sigma_t=%.4f slot=%.4f hist mean=%.2f "
                            "fit=%s rows=%d\n", sig0, sigX0, hMean0,
                    app.lin.fitValid ? "ok" : "none", (int)app.lin.rows.size());
            // 1.58114, not the 1.41421 that stood here until #57 item 3. Stack
            // a is five FLAT frames whose pixels are 0,1,2,3,4, so every pixel's
            // population is exactly that: mean 2, sum of squared deviations
            // 4+1+0+1+4 = 10. sqrt(10/5) = 1.414214 was ddof=0; sqrt(10/4) =
            // 1.581139 is the ddof=1 the peer and computeStackStats always used.
            // Ratio sqrt(5/4) = 1.118034, as it must be for a pure ddof move.
            check(fabsf(sig0 - 1.58114f) < 0.01f && sigX0 == sig0 &&
                  fabs(hMean0 - 2.0) < 1e-4 && app.proj[0].uid == a2->uid &&
                  app.lin.fitValid && app.lin.rows.size() == 2,
                  "R1 pre: sigma_t / hist / proj / fit over the old pixels");
        }
        // ---- the event: one frame's FILE changes on disk -----------------------
        uint64_t uid0 = a2->uid, srcId0 = a2->src->srcId;
        int rev0 = a2->src->rev, dr0 = a2->dataRev, ldr0 = loose->dataRev;
        int64_t mt0 = a2->src->mtime;
        int srBefore = seqInfo(seqA)->stackRev;
        a2->texDirty = false;
        loose->texDirty = false;
        // long enough to cross a write-time bucket on ANY filesystem this can
        // run on: this stdlib reports last_write_time at seconds granularity,
        // and FAT/exFAT scratch volumes round write times to 2-SECOND buckets -
        // a 1.1 s pause landed in the same bucket ~45% of the time there
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        writeNpy2(stk + "/a_002.npy", 4, 4, 200.0f, 0);
        {
            std::string err, note;
            bool r = reloadSource(a2->src, err, &note);
            check(r && note.empty(), "R2 reloadSource succeeds; same shape, no dims note");
        }
        fprintf(stderr, "reloadselftest: R3 rev %d->%d mtime %lld->%lld file_clock ticks uid %d srcId %d\n",
                rev0, a2->src->rev, (long long)mt0, (long long)a2->src->mtime,
                a2->uid == uid0 ? 1 : 0, a2->src->srcId == srcId0 ? 1 : 0);
        check(a2->uid == uid0 && a2->src->srcId == srcId0 && a2->src->rev == rev0 + 1 &&
              a2->src->mtime != 0 && a2->src->mtime != mt0,
              "R3 identity survives: uid/srcId kept, rev+1, mtime fresh");
        check(a2->dataRev == dr0 + 1 && loose->dataRev == ldr0 + 1 &&
              a2->px()[0] == 200.0f && loose->px()[0] == 200.0f &&
              a2->vmin == 200.0f && loose->vmin == 200.0f && a2->w == 4,
              "R4 walk: BOTH sharing docs' dataRev+mirrors, one step");
        check(a2->texDirty && loose->texDirty,
              "R5 walk: texDirty set on every sharing membership");
        check(seqInfo(seqA)->stackRev == srBefore + 1 && seqInfo(seqB)->stackRev == 0,
              "R10 stackRev bumped for the reloaded stack only");
        {   // ---- R6: keyed caches died and recompute over the NEW pixels ------
            bool dropped = app.hist[0].img == nullptr && app.proj[0].img == nullptr;
            recomputeHistogramIfNeeded(a2, app.hist[0]);
            recomputeProjectionIfNeeded(a2, app.proj[0]);
            check(dropped && fabs(app.hist[0].mean[0] - 200.0) < 1e-4 &&
                  app.proj[0].uid == a2->uid && app.proj[0].dataRev == a2->dataRev,
                  "R6 hist/proj forgotten, recomputed over new pixels");
        }
        {   // ---- R7: sigma_t follows the pixels --------------------------------
            recomputeTemporalIfNeeded(a2, app.temporal[0]);
            float sig1 = (float)app.temporal[0].tempNoise[0];
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);
            float sigX1 = (float)app.temporalExtra[0].tempNoise[0];
            fprintf(stderr, "reloadselftest: R7 sigma_t pre=%.4f post=%.4f slot post=%.4f "
                            "(frame count and ROI unchanged)\n", sig0, sig1, sigX1);
            // 88.5624, not the 79.2126 that stood here until #57 item 3. a_002
            // was rewritten from 2 to 200, so each pixel's five values are
            // 0,1,200,3,4: mean 41.6, sum of squared deviations 31373.2.
            // sqrt(31373.2/5) = 79.212625 was ddof=0; sqrt(31373.2/4) = 88.562407
            // is ddof=1. Ratio sqrt(5/4) = 1.118034 again - the same factor as
            // R1, which is the evidence that the reload walk itself did not move.
            check(fabsf(sig1 - 88.5624f) < 0.5f,
                  "R7a A-slot sigma_t recomputed after the reload");
            // the hole (§3.2): same seqId, same frame count, same ROI, same CFA -
            // only SeqInfo::stackRev in the key can tell this cache its numbers
            // describe pixels that no longer exist
            check(fabsf(sigX1 - 88.5624f) < 0.5f,
                  "R7b slot sigma_t is NOT the stale pre-reload value");
        }
        {   // ---- R8: stackRev ALONE unlatches the key (permanent mechanism
            // proof - forgetImage also resets these states on today's paths, so
            // R7b alone could pass for the wrong reason with both fixes in)
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);       // latch
            app.temporalExtra[0].tempNoise[0] = -12345.0;              // poison
            // negative control first: WITHOUT a bump the key must match and
            // the cache must HOLD - otherwise a broken latch (always-recompute)
            // would pass the bump half of this test vacuously
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);
            check(app.temporalExtra[0].tempNoise[0] == -12345.0,
                  "R8a the latched cache HOLDS while stackRev is unchanged");
            seqInfo(seqA)->stackRev++;             // what a second reload does
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);
            check(fabs(app.temporalExtra[0].tempNoise[0] + 12345.0) > 1.0,
                  "R8b stackRev alone unlatches the temporal key");
        }
        {   // ---- R9: forgetImage covers the slot states (§3.2 hole 2) ----------
            recomputeTemporalIfNeeded(a2, app.temporalExtra[0]);       // latch
            forgetImage(a2);
            check(app.temporalExtra[0].seqId == -1,
                  "R9 forgetImage forgets the extra slot states too");
        }
        check(app.lin.rows.size() == 1 && app.lin.rows[0].seqId == seqB &&
              !app.lin.fitValid,
              "R11 lin: reloaded stack's row dropped, fit stale, B's kept");
        {   // ---- R12: frame-axis provenance ------------------------------------
            check(c2->src->fileFrame == 2 && c2->px()[0] == 70.0f,
                  "R12a fileFrame provenance recorded at decode time");
            writeNpy3(root + "/c.npy", 5, 8, 8, 500.0f);
            std::string e2;
            bool r2 = reloadSource(c2->src, e2);
            check(r2 && c2->px()[0] == 520.0f,
                  "R12b reload decodes THIS frame of the frame-axis file");
            writeNpy3(root + "/c.npy", 2, 8, 8, 900.0f);
            ImageDoc* c4 = docOf(seqC, 4);
            int c4rev = c4->src->rev, c4dr = c4->dataRev;
            std::string e3;
            bool r3 = reloadSource(c4->src, e3);
            fprintf(stderr, "reloadselftest: R12 vanished frame: %s\n", e3.c_str());
            check(!r3 && e3.find("no longer exists") != std::string::npos &&
                  c4->px()[0] == 90.0f && c4->src->rev == c4rev && c4->dataRev == c4dr,
                  "R12c a frame the file no longer has is refused, pixels stay");
            ImageDoc* c1 = docOf(seqC, 1);
            std::string e4;
            check(reloadSource(c1->src, e4) && c1->px()[0] == 910.0f,
                  "R12d frames the shrunken file still has keep reloading");
        }
        {   // ---- R13: dimensions are facts, not errors --------------------------
            writeNpy2(stk + "/a_004.npy", 6, 6, 300.0f, 0);
            ImageDoc* a4 = docOf(seqA, 4);
            int dr = a4->dataRev;
            std::string e5, n5;
            bool r5 = reloadSource(a4->src, e5, &n5);
            fprintf(stderr, "reloadselftest: R13 dims note: %s\n", n5.c_str());
            check(r5 && a4->w == 6 && a4->h == 6 && a4->dataRev == dr + 1 &&
                  n5.find("->") != std::string::npos,
                  "R13 dims change is a fact: reload proceeds and says so");
        }
        {   // ---- R14: failure is refused per frame and counted both ways --------
            std::filesystem::remove(pathFromUtf8(stk + "/a_003.npy"), ec);
            ImageDoc* a3 = docOf(seqA, 3);
            int rv = a3->src->rev, dr = a3->dataRev;
            std::string e6;
            bool r6 = reloadSource(a3->src, e6);
            check(!r6 && a3->px()[0] == 3.0f && a3->src->rev == rv && a3->dataRev == dr,
                  "R14a a vanished file refuses; the old pixels stay");
            int failed = 0;
            std::string msg = reloadStackFromDisk(seqA, &failed);
            fprintf(stderr, "reloadselftest: R14 summary: %s\n", msg.c_str());
            check(failed == 1 && msg.rfind("reloaded 4 frame(s), 1 failed", 0) == 0 &&
                  msg.find("a_003.npy") != std::string::npos,
                  "R14b stack reload counts both directions, names the error");
            // the walk also refreshed the loose frame sharing a_002's source -
            // pixels OUTSIDE this stack changed, and the summary must say so
            check(msg.find("also refreshed 1 shared frame(s) in \"loose.npy\"")
                      != std::string::npos,
                  "R14c cross-stack refresh is named, never silent");
        }
        {   // ---- R15: what reload is NOT offered for ----------------------------
            auto rdoc = std::make_unique<ImageDoc>();
            rdoc->src->remoteUrl = "ssh://host/x.npy";
            rdoc->src->path = "x.npy";
            std::string e7, e8;
            bool r7 = reloadSource(rdoc->src, e7);
            auto odoc = std::make_unique<ImageDoc>();       // montage/plugin: no path
            bool r8 = reloadSource(odoc->src, e8);
            check(!r7 && !r8 && e7 == "remote-backed" &&
                  e8 == "no file behind these pixels",
                  "R15 remote-backed and origin-less sources are refused");
        }
        {   // ---- R16: reload RE-KEYS the registry ------------------------------
            // the reloaded source carries a NEW tuple (fresh mtime/fsize); a
            // re-open of the changed file must find it and share, not decode a
            // resident duplicate of identical bytes
            // reuse the source's OWN path spelling: the registry key holds the
            // folder worker's spelling, and this test pins re-keying, not
            // spelling canonicalization (R21 owns that)
            size_t n0 = app.images.size();
            std::string e16 = loadNpy(a2->src->path);
            check(e16.empty() && app.images.size() == n0 + 1 &&
                  app.images.back()->src == a2->src,
                  "R16 re-open after reload adopts the SAME source (re-keyed)");
        }
        {   // ---- R17: an in-place crop leaves the registry; the slot reclaims --
            ImageDoc* b1 = docOf(seqB, 1);
            // capture the worker's path spelling BEFORE the crop, so the
            // re-opens below hit the occupied slot, not a differently-spelled
            // empty one
            std::string bpath = b1->src->path;
            cropInPlace(*b1, 1, 1, 2, 2);
            check(b1 && b1->w == 2 && b1->src->srcW == 4,
                  "R17a crop in place (sole owner keeps its source)");
            size_t n0 = app.images.size();
            std::string e17 = loadNpy(bpath);
            ImageDoc* r1 = app.images.back().get();
            check(e17.empty() && app.images.size() == n0 + 1 &&
                  r1->src != b1->src && r1->w == 4,
                  "R17b re-open after crop decodes the FULL frame fresh");
            std::string e17b = loadNpy(bpath);
            ImageDoc* r2 = app.images.back().get();
            check(e17b.empty() && r2->src == r1->src,
                  "R17c the tuple's slot was reclaimed: next open shares again");
        }
        {   // ---- R18: reload replays the RECORDED reading (npyRead, §3.3) ------
            // d.npy is (5,8,3): native reads it as ONE 5x8 3-channel image
            // (§3.1: a last axis of 3 is channels). Open it DECLARED as a
            // stack - (F,H,W): 5 frames of 3x8. The declaration is provenance
            // (it is what --npy-axis stopped being): reload must replay it,
            // and must count the file's frames under it, not under native.
            writeNpy3(root + "/d.npy", 5, 3, 8, 70.0f);    // shape (5, 8, 3)
            std::string dErr = loadNpy(root + "/d.npy", NR_STACK);
            int seqD = app.seqs.empty() ? 0 : app.seqs.back().id;
            ImageDoc* d0 = docOf(seqD, 0);
            ImageDoc* d1 = docOf(seqD, 1);
            ImageDoc* d4 = docOf(seqD, 4);
            check(dErr.empty() && seqD && d0 && d1 && d4 &&
                  framesOfSeq(seqD).size() == 5 && d0->w == 3 && d0->h == 8 &&
                  d0->ch == 1 && d1->px()[0] == 80.0f,
                  "R18a a declared (F,H,W) reading opens 5 frames, not 1 image");
            {   // ---- R21b: the key embeds the reading - a NATIVE open of the
                // SAME file (same path, same mtime/fsize, fileFrame 0) is a
                // DIFFERENT tuple from the declared stack's frame 0: two
                // readings are two sets of pixels and must never share
                size_t n0 = app.images.size();
                std::string nErr = loadNpy(root + "/d.npy");
                ImageDoc* nat = app.images.empty() ? nullptr : app.images.back().get();
                fprintf(stderr, "reloadselftest: R21b native beside declared: "
                                "src %s, %dx%d %dch\n",
                        (nat && d0 && nat->src == d0->src) ? "ADOPTED" : "own",
                        nat ? nat->w : -1, nat ? nat->h : -1, nat ? nat->ch : -1);
                check(nErr.empty() && app.images.size() == n0 + 1 && nat &&
                      nat->src != d0->src && nat->w == 8 && nat->h == 5 &&
                      nat->ch == 3,
                      "R21b two readings of one file are two tuples");
                closeImages({ (int)app.images.size() - 1 });
            }
            writeNpy3(root + "/d.npy", 2, 3, 8, 900.0f);   // shape (2, 8, 3)
            std::string e18;
            bool r18 = reloadSource(d1->src, e18);
            check(r18 && d1->px()[0] == 910.0f && d1->w == 3 && d1->h == 8,
                  "R18b reload keeps the recorded reading (5->2 frames, still 3x8)");
            std::string e18b;
            bool r18b = reloadSource(d4->src, e18b);
            fprintf(stderr, "reloadselftest: R18 refusal: %s\n", e18b.c_str());
            check(!r18b && e18b.find("file now has 2 frame(s)") != std::string::npos,
                  "R18c refusal counts frames under the RECORDED reading");
            closeStack(seqD);
        }
        {   // ---- R19: local:// is LOCAL - the url embeds a path on this disk ---
            writeNpy2(root + "/lref.npy", 4, 4, 1.0f, 0);
            auto ldoc = std::make_unique<ImageDoc>();
            ldoc->src->path = ldoc->src->remoteUrl = "local://" + root + "/lref.npy";
            ldoc->src->w = 4; ldoc->src->h = 4; ldoc->src->ch = 1;
            ldoc->src->dtype = "f32";
            ldoc->src->data.assign(16, -1.0f);
            ldoc->syncMirrors();
            std::string e19;
            bool r19 = reloadSource(ldoc->src, e19);
            check(r19 && ldoc->src->data[0] == 1.0f,
                  "R19 local:// reloads from the embedded local path");
        }
        {   // ---- R19b: a local:// tuple carries the DISK baseline ---------------
            // The url embeds a path on this disk, so an overwrite must move the
            // tuple. Without the baseline (mtime/fsize 0) the key never moves,
            // and a re-open of the same url ADOPTS the stale resident: old
            // pixels wearing a fresh open's name.
            writeNpy2(root + "/lb.npy", 4, 4, 5.0f, 0);
            auto mkLocal = [&]() {
                auto d = std::make_unique<ImageDoc>();
                d->src->path = d->src->remoteUrl = "local://" + root + "/lb.npy";
                d->src->w = 4; d->src->h = 4; d->src->ch = 1;
                d->src->dtype = "f32";
                d->src->data.assign(16, -2.0f);
                statLocalUrl(*d->src);       // what every local:// open path does
                d->syncMirrors();
                computeMinMax(*d);
                d->uid = app.nextUid++;
                return d;
            };
            auto la = mkLocal();
            shareOrRegisterSource(*la);                   // fresh tuple, registered
            // dims change too, so the check does not hinge on mtime granularity
            writeNpy2(root + "/lb.npy", 6, 6, 9.0f, 0);
            auto lb2 = mkLocal();
            bool adopted = shareOrRegisterSource(*lb2);
            fprintf(stderr, "reloadselftest: R19b after overwrite: %s\n",
                    lb2->src == la->src ? "ADOPTED the stale resident" : "own tuple");
            check(!adopted && lb2->src != la->src,
                  "R19b an overwritten local:// file is a NEW tuple, never adopted");
            auto lc = mkLocal();
            bool ad2 = shareOrRegisterSource(*lc);
            check(ad2 && lc->src == lb2->src,
                  "R19c an unchanged local:// url still adopts (sharing kept)");
        }
        {   // ---- R20: the stack summary states a member's dims change ----------
            writeNpy2(stk + "/b_002.npy", 6, 6, 400.0f, 0);
            int failed20 = 0;
            std::string msg = reloadStackFromDisk(seqB, &failed20);
            fprintf(stderr, "reloadselftest: R20 summary: %s\n", msg.c_str());
            check(failed20 == 0 && msg.find("changed dimensions") != std::string::npos &&
                  msg.find("b_002") != std::string::npos,
                  "R20 stack summary names the dims change");
        }
#ifdef _WIN32
        {   // ---- R21: key canonicalization - spelling does not split the tuple -
            std::string alt = stk + "/b_003.npy";
            for (auto& c21 : alt) if (c21 == '/') c21 = '\\';
            if (alt.size() > 1 && alt[1] == ':')
                alt[0] = (char)toupper((unsigned char)alt[0]);
            ImageDoc* b3 = docOf(seqB, 3);
            size_t n0 = app.images.size();
            std::string e21 = loadNpy(alt);
            check(e21.empty() && app.images.size() == n0 + 1 &&
                  app.images.back()->src == b3->src,
                  "R21 a different path spelling adopts the same tuple");
        }
#endif
        {   // ---- R22: add-on-collision hands back the resident -----------------
            // the worker's post-decode state when the UI thread registered the
            // same tuple between its probe miss and its add: the add must
            // return the live occupant so the caller adopts it - both stacks
            // of one tuple end up sharing, never twin residents
            ImageDoc* b4 = docOf(seqB, 4);
            auto dup = cloneSource(*b4->src);      // same tuple, private twin
            std::shared_ptr<FrameSource> live = srcRegistryAdd(dup);
            check(b4 && live == b4->src,
                  "R22 srcRegistryAdd returns the live occupant to adopt");
        }
        {   // ---- R23: a frame still queued in seqReady lands with the source's
            // CURRENT geometry - the reload walk covers only app.images, so the
            // landing itself must resync what a reload moved under the queue
            ImageDoc* b0 = docOf(seqB, 0);
            auto qd = std::make_unique<ImageDoc>();
            qd->src = b0->src;
            qd->name = "queued";
            qd->syncMirrors();
            qd->w = 1; qd->h = 1;      // decode-time mirrors, made stale by hand
            {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(5, std::move(qd));
            }
            app.seqLoadingId = seqB;
            pumpSequence();
            ImageDoc* landed = app.images.back().get();
            check(landed->name == "queued" && landed->w == b0->src->w &&
                  landed->h == b0->src->h,
                  "R23 pumpSequence lands queued docs with fresh mirrors");
        }
        {   // ---- R24: a crop is a change to the STACK's data -------------------
            // cropInPlace re-scopes ONE membership's pixels. A stack's temporal
            // numbers are computed over every member, so a member that no longer
            // has the stack's shape drops out of them - but the key is
            // (seqId, frames, stackRev, ROI, CFA) and a crop of a member that is
            // not the reference doc moves none of those: same stack, same frame
            // count, same ROI (the REFERENCE doc's dims), same CFA. stackRev is
            // the stack's DATA revision, and a crop is a change to the stack's
            // data - so the crop bumps it, rather than this cache growing a
            // second answer to "have these pixels changed".
            std::string cstk = root + "/cropstk";
            std::filesystem::create_directories(pathFromUtf8(cstk), ec);
            for (int i = 0; i < 5; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "k_%03d.npy", i);
                writeNpy2(cstk + "/" + nm, 4, 4, 100.0f + (float)i, 0);
            }
            std::vector<int> had;
            for (const auto& s : app.seqs) had.push_back(s.id);
            openFolder(cstk);
            loadAll();
            int seqK = 0;
            for (const auto& s : app.seqs)
                if (std::find(had.begin(), had.end(), s.id) == had.end()) seqK = s.id;
            ImageDoc* k0 = docOf(seqK, 0);
            ImageDoc* k2 = docOf(seqK, 2);
            App::TemporalState T{};             // a slot keyed on k0, not on k2
            if (k0) recomputeTemporalIfNeeded(k0, T);
            double sigAll = T.tempNoise[0];     // five flat frames, 100..104
            int srK = seqK ? seqInfo(seqK)->stackRev : -1;
            bool cropped = k2 && cropInPlace(*k2, 0, 0, 2, 2);
            if (k0) recomputeTemporalIfNeeded(k0, T);
            fprintf(stderr, "reloadselftest: R24 crop of one member: stackRev %d->%d, "
                            "sigma_t %.5f -> %.5f (5 frames -> the 4 that still "
                            "have the stack's shape)\n",
                    srK, seqK ? seqInfo(seqK)->stackRev : -1, sigAll, T.tempNoise[0]);
            // Both numbers moved with #57 item 3, and they moved by DIFFERENT
            // factors because the frame count is what changed here:
            //   pre-crop  5 flat frames 100..104: deviations 4+1+0+1+4 = 10.
            //             sqrt(10/5) = 1.414214 (ddof=0) -> sqrt(10/4) = 1.581139
            //   post-crop the 4 that still have the stack's shape, 100,101,103,104:
            //             deviations 4+1+1+4 = 10. sqrt(10/4) = 1.581139 (ddof=0)
            //             -> sqrt(10/3) = 1.825742
            // BEWARE the coincidence this creates: 1.581139 was the post-crop
            // answer under ddof=0 and is the pre-crop answer under ddof=1. That
            // is arithmetic, not a copied number - the two populations really do
            // have the same sum of squared deviations and differ only in their
            // divisor. R24c still says what it says: 1.825742 != 1.581139, so a
            // cache that handed back the pre-crop value would still fail here.
            check(k0 && k2 && cropped && fabs(sigAll - 1.58114) < 0.001,
                  "R24a fixture: five flat frames, one member cropped");
            check(seqK && seqInfo(seqK)->stackRev == srK + 1,
                  "R24b a crop bumps the stack's data revision");
            check(fabs(T.tempNoise[0] - 1.82574) < 0.001,
                  "R24c sigma_t is NOT the stale pre-crop five-frame value");
        }
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "reloadselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
