// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- --stackavg-selftest: the frame average's NUMBERS ---------------------
    // The VALUE, never the menu item. A menu item that opens the wrong image is
    // a passing test and a wrong measurement, and this image is the one that
    // ends up in a report as if it had been captured. Every fixture is written
    // here so CI has seen exactly these numbers: a stack whose mean is known
    // exactly, a NaN that must be excluded and counted rather than folded into
    // the divisor, a sample with no finite frame at all, a sum that float32
    // cannot carry, a mosaic whose four planes must survive per pixel, a partly
    // loaded stack that must name its count, and a session that must bring the
    // provenance back rather than a capture wearing its name. The B group at
    // the end is the stack reduction ruling (docs/analysis-layers.md §3.2):
    // the fold as a mean/sum choice, NaN as a surfaced warning that carries
    // the exclusion count and the valid frame count, and the sum standing on
    // the mean's own frame path.
    if (!g_stackAvgSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "stackavgselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
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
        // f4 .npy, pixels given: the NaN and the precision fixtures need
        // per-pixel control, which a constant-fill writer cannot give.
        auto writeNpy = [](const std::string& path, int w, int h,
                           const std::vector<float>& px) {
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
            f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        };
        auto flat = [&](int w, int h, float v) { return std::vector<float>((size_t)w * h, v); };
        // A scratch subdirectory this test CREATES - never the directory it was
        // handed, which is a shared fixture root (the derive selftest's comment
        // records what happened the one time that rule was broken).
        std::string root = g_stackAvgSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/stackavg-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        const std::string dA = root + "/A", dN = root + "/N", dC = root + "/C",
                          dP = root + "/P";
        for (const std::string& d : { dA, dN, dC, dP })
            std::filesystem::create_directories(pathFromUtf8(d), ec);
        const int W = 4, H = 4;
        const float NA = std::numeric_limits<float>::quiet_NaN();
        // A: 100, 200, 300 -> exactly 200, everywhere.
        writeNpy(dA + "/f_000.npy", W, H, flat(W, H, 100.0f));
        writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 200.0f));
        writeNpy(dA + "/f_002.npy", W, H, flat(W, H, 300.0f));
        // N: same three levels, but pixel 0 is NaN in the MIDDLE frame (mean of
        // the other two = 200 - the same answer, which is the point: excluding
        // must not shift it) and pixel 1 is NaN in ALL THREE (no mean exists).
        for (int k = 0; k < 3; k++) {
            std::vector<float> px = flat(W, H, 100.0f * (k + 1));
            if (k == 1) px[0] = NA;
            px[1] = NA;
            char nm[32]; snprintf(nm, sizeof nm, "/f_%03d.npy", k);
            writeNpy(dN + nm, W, H, px);
        }
        // C: a mosaic. Each Bayer plane gets its own level, so mixing planes
        // anywhere shows up immediately as a wrong per-plane number.
        const float BASE[4] = { 1000.0f, 2000.0f, 3000.0f, 4000.0f };   // R Gr Gb B
        for (int k = 0; k < 3; k++) {
            std::vector<float> px((size_t)W * H);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int p = CFA_MAP[0][(y & 1) * 2 + (x & 1)];          // RGGB
                    px[(size_t)y * W + x] = BASE[p] + 10.0f * k;
                }
            char nm[32]; snprintf(nm, sizeof nm, "/f_%03d.npy", k);
            writeNpy(dC + nm, W, H, px);
        }
        // P: a sum float32 cannot carry. 2^24 + 1 + 1 is 2^24 in float32 (both
        // ones fall off the end of the mantissa) and 16777218 in double, and the
        // two means differ by more than a float32 ULP at that magnitude - so
        // this fixture fails if the accumulator is ever narrowed back to float.
        writeNpy(dP + "/f_000.npy", W, H, flat(W, H, 16777216.0f));
        writeNpy(dP + "/f_001.npy", W, H, flat(W, H, 1.0f));
        writeNpy(dP + "/f_002.npy", W, H, flat(W, H, 1.0f));

        app.seqLoadMode = 1;                    // always load numbered runs
        // Opens the average of the ONE stack the given folder holds, and hands
        // back the frame it made. Goes through requestStackAverage, the same
        // door every menu item uses - a test that called stackReduceFrame
        // directly would not be testing what the menu does.
        auto averageOf = [&](const std::string& dir, int* seqOut) -> ImageDoc* {
            closeAll();
            openFolder(dir);
            loadAll();
            int seq = app.seqs.empty() ? 0 : app.seqs.front().id;
            if (seqOut) *seqOut = seq;
            if (!seq) return nullptr;
            size_t before = app.images.size();
            requestStackAverage(seq);
            loadAll();                          // settled already, but drive the pump
            if (app.images.size() != before + 1) return nullptr;
            return app.images.back().get();
        };

        {   // ---- A1: the mean is the mean, and the source is untouched --------
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "A1 the average opened as exactly one new image");
            if (m) {
                bool allTwoHundred = true;
                for (float v : m->px()) if (v != 200.0f) allTwoHundred = false;
                fprintf(stderr, "stackavgselftest: A1 100/200/300 -> %.9g (%dx%d %dch), "
                                "stack still %zu frames\n",
                        (double)m->px()[0], m->w, m->h, m->ch, framesOfSeq(seq).size());
                check(allTwoHundred, "A1 frames at 100, 200, 300 average to exactly 200");
                check(m->w == W && m->h == H && m->ch == 1,
                      "A1 ...at the source's size, one frame's worth of pixels");
                // A frame, per docs/terminology.md: the layer below the stack,
                // hanging off the same batch. Nothing about it needs a special
                // case for Histogram / Projection / ROIs to work (issue #48).
                check(m->seqId == 0, "A2 the result is a FRAME, not a stack");
                check(m->batchId == app.images[framesOfSeq(seq).front()]->batchId,
                      "A2 ...in the source stack's batch");
                check(framesOfSeq(seq).size() == 3,
                      "A2 ...and averaging did NOT close or consume the source stack");
            }
            if (m) {   // ---- A3: provenance, in all three places ---------------
                const std::string fp = app.images[framesOfSeq(seq).front()]->src->path;
                fprintf(stderr, "stackavgselftest: A3 name='%s'\n  note='%s'\n",
                        m->name.c_str(), m->note.c_str());
                check(m->name.find("frame average") != std::string::npos,
                      "A3 the NAME says it is a frame average");
                check(m->note.find("frame average of") != std::string::npos &&
                      m->note.find("3 frame(s)") != std::string::npos,
                      "A3 the NOTE names the stack and how many frames");
                check(m->note.find("double") != std::string::npos,
                      "A3 ...and says the accumulation was in double");
                // The load-bearing one: an empty path is what keeps the session
                // from writing an `image` line that would reload frame 0 from
                // disk and hand it back under this name as a capture.
                check(m->src->path.empty(),
                      "A3 no file path: a computed frame is not a file");
                check(m->avgOfPath == fp && !fp.empty(),
                      "A3 ...but it records the stack it averaged, for the session");
            }
            if (m) {   // ---- A4: ONE averaging, shared with the stack statistic -
                StackStats st = computeStackStats(seq, 0, 0, W, H);
                double pic = 0;
                for (float v : m->px()) pic += v;
                pic /= (double)m->px().size();
                fprintf(stderr, "stackavgselftest: A4 picture mean %.9g, "
                                "computeStackStats mean %.9g\n", pic, st.mean[0]);
                check(st.valid && fabs(pic - st.mean[0]) < 1e-9,
                      "A4 the picture and the Temporal table are the same number");
            }
        }

        {   // ---- A5: a NaN is EXCLUDED and COUNTED, never folded into N -------
            int seq = 0;
            ImageDoc* m = averageOf(dN, &seq);
            check(m != nullptr, "A5 the NaN stack averages");
            if (m) {
                // pixel 0: NaN in the middle frame only -> mean(100, 300) = 200.
                // Folding the NaN in as 0 would give 133.33; keeping N = 3 as
                // the divisor would give the same 133.33 - both plausible, both
                // wrong, and both invisible without this assert.
                // pixel 1: NaN in every frame -> no mean exists, so NaN out.
                fprintf(stderr, "stackavgselftest: A5 px0=%.9g (want 200), px1=%.9g "
                                "(want NaN), note='%s'\n",
                        (double)m->px()[0], (double)m->px()[1], m->note.c_str());
                check(m->px()[0] == 200.0f,
                      "A5 a pixel's NaN frame leaves the OTHER frames' mean intact");
                check(std::isnan(m->px()[1]),
                      "A6 a pixel with no finite frame at all is NaN, not 0");
                check(m->note.find("4 non-finite sample(s) EXCLUDED") != std::string::npos,
                      "A6 ...and the note reports how many samples were excluded");
                check(m->note.find("1 sample(s) had no finite frame") != std::string::npos,
                      "A6 ...and how many have no mean at all");
                // the stack statistic counts the same four, off the same pass
                StackStats st = computeStackStats(seq, 0, 0, W, H);
                check(st.valid && st.nonFinite == 4 && st.unknown == 1,
                      "A6 computeStackStats reports the identical exclusion counts");
            }
        }

        {   // ---- A7: double accumulation, which float32 cannot do -------------
            int seq = 0;
            ImageDoc* m = averageOf(dP, &seq);
            check(m != nullptr, "A7 the precision stack averages");
            if (m) {
                const float want = (float)((16777216.0 + 1.0 + 1.0) / 3.0);
                float f32 = 0;                       // what a float32 accumulator
                f32 += 16777216.0f; f32 += 1.0f; f32 += 1.0f;   // would have got
                const float got32 = f32 / 3.0f;
                fprintf(stderr, "stackavgselftest: A7 double %.9g, float32 %.9g, "
                                "got %.9g\n", (double)want, (double)got32,
                        (double)m->px()[0]);
                check(want != got32,
                      "A7 the fixture really does separate double from float32");
                check(m->px()[0] == want, "A7 the mean is the DOUBLE sum's mean");
            }
        }

        {   // ---- A8: a mosaic stays a mosaic, per plane -----------------------
            app.forceCfa = 1; app.forceCfaPattern = 0;      // --cfa bayer, RGGB
            int seq = 0;
            ImageDoc* m = averageOf(dC, &seq);
            check(m != nullptr, "A8 the CFA stack averages");
            if (m) {
                check(m->cfa == 1 && m->cfaPattern == 0,
                      "A8 the CFA declaration travels to the result");
                // The mean is per pixel, so every pixel keeps its Bayer phase
                // and each plane's mean is that plane's own level + 10.
                bool planesOk = true;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        int p = cfaChannelAt(*m, x, y);
                        if (m->px()[(size_t)y * W + x] != BASE[p] + 10.0f) planesOk = false;
                    }
                StackStats st = computeStackStats(seq, 0, 0, W, H);
                fprintf(stderr, "stackavgselftest: A8 per-plane stack means "
                                "%.9g %.9g %.9g %.9g (want %.9g %.9g %.9g %.9g), "
                                "nPl=%d\n", st.mean[0], st.mean[1], st.mean[2], st.mean[3],
                        BASE[0] + 10.0, BASE[1] + 10.0, BASE[2] + 10.0, BASE[3] + 10.0,
                        st.nPl);
                check(planesOk, "A8 every pixel is its OWN plane's mean - no plane mixing");
                bool statOk = st.valid && st.nPl == 4;
                for (int p = 0; p < 4 && statOk; p++)
                    if (fabs(st.mean[p] - (BASE[p] + 10.0)) > 1e-6) statOk = false;
                check(statOk, "A8 ...and the stack's per-plane statistics agree with it");
            }
            app.forceCfa = -1;
        }

        {   // ---- A9: a partly loaded stack NAMES its count --------------------
            // What the memory budget leaves behind, reproduced exactly as every
            // other consumer sees it: more frames expected than are resident.
            // docs/terminology.md requires the ratio to be STATED, not the
            // aggregate refused - 8 of 24 frames is a real measurement of 8.
            int seq = 0;
            closeAll();
            openFolder(dA);
            loadAll();
            seq = app.seqs.empty() ? 0 : app.seqs.front().id;
            if (App::SeqInfo* si = seqInfo(seq)) si->expectedFrames = 5;
            size_t before = app.images.size();
            requestStackAverage(seq);
            loadAll();
            check(app.images.size() == before + 1, "A9 the partial stack still averages");
            if (app.images.size() == before + 1) {
                ImageDoc* m = app.images.back().get();
                fprintf(stderr, "stackavgselftest: A9 name='%s'\n  note='%s'\n",
                        m->name.c_str(), m->note.c_str());
                check(m->name.find("3 of 5") != std::string::npos,
                      "A9 the NAME says 3 of 5, so it cannot read as 'the average'");
                check(m->note.find("PARTLY LOADED") != std::string::npos &&
                      m->note.find("not of all 5") != std::string::npos,
                      "A9 ...and the note says which 3 and out of how many");
                check(m->px()[0] == 200.0f,
                      "A9 ...over the frames that ARE here, not a different denominator");
            }
        }

        {   // ---- A10: through a session, as a RECIPE -------------------------
            // The pixels cannot be saved (the format is a list of files) and the
            // note is not a session key, so what has to survive is the
            // derivation. What must NOT happen is the frame coming back as an
            // ordinary image line pointing at the source file - a single capture
            // wearing the name "frame average".
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "A10 an average to round-trip");
            std::string sess = root + "/stackavg.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            check(txt.find("stackavg ") != std::string::npos,
                  "A10 the session writes the recipe, not the pixels");
            std::string lerr = loadSession(sess);      // closeAll happens inside
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int avgs = 0, captures = 0;
            ImageDoc* back = nullptr;
            for (auto& d : app.images) {
                if (!d->avgOfPath.empty()) { avgs++; back = d.get(); }
                else if (d->seqId == 0) captures++;   // a lone frame that is not one
            }
            fprintf(stderr, "stackavgselftest: A10 load='%s', %d average(s), %d loose "
                            "frame(s), %zu image(s)\n", lerr.c_str(), avgs, captures,
                    app.images.size());
            check(lerr.empty() && avgs == 1,
                  "A10 exactly one frame average comes back");
            check(captures == 0,
                  "A10 ...and nothing came back as a loose capture in its place");
            if (back) {
                bool same = back->w == W && back->h == H;
                for (float v : back->px()) if (v != 200.0f) same = false;
                fprintf(stderr, "stackavgselftest: A10 restored note='%s'\n",
                        back->note.c_str());
                check(same, "A10 ...recomputed to the same 200, from the reopened stack");
                check(back->note.find("frame average of") != std::string::npos,
                      "A10 ...and it is saying what it came from again");
                check(back->src->path.empty(),
                      "A10 ...and it still has no file behind it");
            }
        }

        // ---- B: the stack reduction ruling (docs/analysis-layers.md §3.2, ----
        // 判断record 3). The fold is a CHOICE of mean / sum; NaN present is a
        // WARNING, carrying the exclusion count and the valid frame count
        // together; and the fold's result stands on the same frame path the
        // mean already stands on (判断record 7's premise). Written before the
        // behavior existed, red against the unimplemented fold on purpose.
        //
        // Opens the one stack in `dir`, folded as asked, through the same door
        // the menu items use - the fold sibling of averageOf above.
        auto reduceOf = [&](const std::string& dir, int fold, int* seqOut) -> ImageDoc* {
            closeAll();
            openFolder(dir);
            loadAll();
            int seq = app.seqs.empty() ? 0 : app.seqs.front().id;
            if (seqOut) *seqOut = seq;
            if (!seq) return nullptr;
            size_t before = app.images.size();
            requestStackAverage(seq, fold);
            loadAll();
            if (app.images.size() != before + 1) return nullptr;
            return app.images.back().get();
        };
        // The warning is a toast, and toasts are kept: every one lands in
        // app.msgLog (compare.inc), so "a warning was SURFACED" is an assert on
        // state, exactly like the FrameLinProbe. `since` bounds the scan to
        // what THIS reduction said.
        auto warningSince = [&](size_t since) -> std::string {
            for (size_t i = since; i < app.msgLog.size(); i++)
                if (app.msgLog[i].err &&
                    app.msgLog[i].text.find("non-finite") != std::string::npos)
                    return app.msgLog[i].text;
            return std::string();
        };

        {   // ---- B1: NaN in the fold is a WARNING, not only a note ------------
            // §3.2: "NaN 混入時は Warning を出す". The mean path already counted
            // its exclusions in the note (A6); what the ruling adds is that the
            // count is SURFACED - a warning at reduction time, stating the
            // exclusion count and the valid frame count together. A note is
            // where you look when you already suspect; a warning is how you
            // come to suspect.
            closeAll();
            openFolder(dN);
            loadAll();
            int seq = app.seqs.empty() ? 0 : app.seqs.front().id;
            size_t before = app.images.size(), msgs = app.msgLog.size();
            requestStackAverage(seq);
            loadAll();
            check(app.images.size() == before + 1,
                  "B1 NaN warns and still reduces - never refuses");
            std::string w = warningSince(msgs);
            fprintf(stderr, "stackavgselftest: B1 warning='%s'\n", w.c_str());
            check(!w.empty(),
                  "B1 NaN in the reduction surfaces a WARNING, not only a note");
            check(w.find("4 non-finite sample(s)") != std::string::npos,
                  "B1 ...the warning carries the exclusion count");
            check(w.find("3 frame") != std::string::npos,
                  "B1 ...and the valid frame count, stated together");
        }

        {   // ---- B2/B3: sum is SELECTABLE, and its value is the sum ------------
            int seq = 0;
            ImageDoc* m = reduceOf(dA, FOLD_SUM, &seq);
            check(m != nullptr, "B2 'sum' is a selectable fold and opens one new image");
            if (m) {
                bool all600 = true;
                for (float v : m->px()) if (v != 600.0f) all600 = false;
                fprintf(stderr, "stackavgselftest: B2 name='%s'\n  note='%s'\n",
                        m->name.c_str(), m->note.c_str());
                check(all600, "B2 frames at 100, 200, 300 sum to exactly 600");
                check(m->name.find("frame sum") != std::string::npos,
                      "B2 the NAME carries the fold - a sum never reads as 'the average'");
                check(m->note.find("per-pixel sum over") != std::string::npos &&
                      m->note.find("3 frame(s)") != std::string::npos,
                      "B2 the NOTE says sum, and over how many frames");
                // 判断record 7's premise: mean OR sum stands as a frame, on the
                // very path the mean established - nothing special-cased.
                check(m->seqId == 0, "B3 the sum is a FRAME, the same path as the mean");
                check(m->batchId == app.images[framesOfSeq(seq).front()]->batchId,
                      "B3 ...in the source stack's batch");
                check(m->src->path.empty() && !m->avgOfPath.empty(),
                      "B3 ...computed (no file), carrying its recipe for the session");
            }
        }

        {   // ---- B4: sum under NaN - terms excluded, counted, and DECLARED -----
            // §3.2's one warning about sum: with per-pixel exclusion the number
            // of terms differs pixel to pixel, so two sums are two different
            // quantities. The count map's vessel is #49's business; what this
            // build must already do is SAY the counts vary, with the range.
            closeAll();
            openFolder(dN);
            loadAll();
            int seq = app.seqs.empty() ? 0 : app.seqs.front().id;
            size_t before = app.images.size(), msgs = app.msgLog.size();
            requestStackAverage(seq, FOLD_SUM);
            loadAll();
            check(app.images.size() == before + 1, "B4 the NaN stack still sums");
            if (app.images.size() == before + 1) {
                ImageDoc* m = app.images.back().get();
                fprintf(stderr, "stackavgselftest: B4 px0=%.9g (want 400), px1=%.9g "
                                "(want NaN)\n  note='%s'\n",
                        (double)m->px()[0], (double)m->px()[1], m->note.c_str());
                check(m->px()[0] == 400.0f,
                      "B4 a NaN term is EXCLUDED: 100+300 = 400, never rescaled");
                check(std::isnan(m->px()[1]),
                      "B4 a pixel with no finite frame at all is NaN, not 0");
                check(m->note.find("4 non-finite sample(s) EXCLUDED") != std::string::npos,
                      "B4 the sum's note carries the exclusion count");
                check(m->note.find("valid frame counts vary (0..3 of 3)") != std::string::npos,
                      "B4 ...and DECLARES the per-pixel count range");
                check(!warningSince(msgs).empty(),
                      "B4 ...and the sum warns on NaN exactly as the mean does");
            }
        }

        {   // ---- B5: the sum's precision is the mean's - double all the way ----
            int seq = 0;
            ImageDoc* m = reduceOf(dP, FOLD_SUM, &seq);
            check(m != nullptr, "B5 the precision stack sums");
            if (m) {
                fprintf(stderr, "stackavgselftest: B5 got %.9g (want 16777218)\n",
                        (double)m->px()[0]);
                check(m->px()[0] == 16777218.0f,
                      "B5 2^24+1+1 sums to 16777218 - float32 would have said 2^24");
            }
        }

        {   // ---- B6: the sum through a session, as a recipe WITH its fold ------
            // A10 proved the recipe round-trips; what the fold adds is that the
            // recipe must say WHICH fold, or a saved sum comes back as a mean -
            // a number 3x off under the same provenance-looking note.
            int seq = 0;
            ImageDoc* m = reduceOf(dA, FOLD_SUM, &seq);
            check(m != nullptr, "B6 a sum to round-trip");
            std::string sess = root + "/stacksum.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            check(txt.find("stacksum ") != std::string::npos,
                  "B6 the session writes the sum's own recipe key");
            std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int sums = 0;
            ImageDoc* back = nullptr;
            for (auto& d : app.images)
                if (!d->avgOfPath.empty()) { sums++; back = d.get(); }
            check(lerr.empty() && sums == 1, "B6 exactly one computed frame comes back");
            if (back) {
                bool same = true;
                for (float v : back->px()) if (v != 600.0f) same = false;
                fprintf(stderr, "stackavgselftest: B6 restored note='%s'\n",
                        back->note.c_str());
                check(same && back->note.find("frame sum of") != std::string::npos,
                      "B6 ...recomputed as a SUM (600), not folded back to a mean");
            }
        }

        // ---- C: generation & staleness (issue #82, reference-design.md §10) --
        // A reduced frame is a computation whose inputs can move on without
        // it, and it ends up in reports as if it had been captured. Within a
        // session its key is (seqId, stackRev): the reload walk moves the key
        // and the picture must go VISIBLY stale - never silently recompute.
        // Across sessions the vocabulary is §6.2's identity tuple (path +
        // mtime + fsize), recorded beside the recipe, compared at restore.
        // The group also pins #82's four questions as behavior: two averages
        // are two docs (1), two session lines stay two docs (2), the recipe
        // follows a batch move (3), and WHEN/which generation is on screen (4).

        {   // ---- C1/C2: stale on reload - the key moves, the pixels do not ----
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "C1 an average to make stale");
            if (m) {
                check(m->avgSeqId == seq && seqInfo(seq) &&
                      m->avgStackRev == seqInfo(seq)->stackRev,
                      "C1 the doc records (seqId, stackRev) at compute time");
                check(m->avgSrcDigest != 0 && m->avgSrcCount == 3,
                      "C1 ...and the input tuple digest of the 3 folded frames");
                check(!m->avgWhen.empty() &&
                      m->note.find("; computed ") != std::string::npos,
                      "C1 ...and WHEN, in the doc and in the note");
                check(!avgIsStale(m), "C1 fresh from its stack, it is not stale");
                const std::string noteBefore = m->note;
                // the disk moves on: one member rewritten, stack reloaded
                writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 500.0f));
                int failed = 0;
                std::string msg = reloadStackFromDisk(seq, &failed);
                fprintf(stderr, "stackavgselftest: C1 reload said: %s\n", msg.c_str());
                // the walk runs once per member SOURCE, so a 3-frame stack
                // reload moves the key three times - what matters is that it
                // moved at all, past the recorded value
                check(failed == 0 && seqInfo(seq) &&
                      seqInfo(seq)->stackRev > m->avgStackRev,
                      "C1 the reload walk bumped stackRev");
                check(avgIsStale(m), "C1 the average is STALE the moment its key moves");
                bool still200 = true;
                for (float v : m->px()) if (v != 200.0f) still200 = false;
                check(still200 && m->note == noteBefore,
                      "C2 ...and its pixels and note did NOT move - no silent recompute");
                check(msg.find("stale") != std::string::npos,
                      "C2 ...and the reload summary SAYS a computed frame went stale");
                for (size_t k = 0; k < app.images.size(); k++)
                    if (app.images[k].get() == m) { selectImage((int)k); break; }
                check(windowTitleText().find("(stale)") != std::string::npos,
                      "C2 ...and the title - where reports copy the name from - says so");

                // ---- C3: Recompute is the USER'S one move ---------------------
                std::string rerr;
                bool rok = recomputeStackAverage(m, rerr);
                fprintf(stderr, "stackavgselftest: C3 recompute err='%s'\n  note='%s'\n",
                        rerr.c_str(), m->note.c_str());
                check(rok, "C3 Recompute from stack succeeds while the stack is open");
                bool now300 = true;                       // (100+500+300)/3
                for (float v : m->px()) if (v != 300.0f) now300 = false;
                check(now300, "C3 ...the pixels are the CURRENT stack's mean");
                check(!avgIsStale(m) && seqInfo(seq) &&
                      m->avgStackRev == seqInfo(seq)->stackRev,
                      "C3 ...and the mark clears because the record is the new key");

                // ---- C4: the latch survives the stack closing -----------------
                writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 200.0f));
                reloadStackFromDisk(seq, &failed);
                check(avgIsStale(m), "C4 stale again after a second reload");
                closeStack(seq);
                check(seqInfo(seq) == nullptr && avgIsStale(m),
                      "C4 ...and closing the stack cannot take the fact with it");
            }
        }

        {   // ---- C5: the generation record rides the session ------------------
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "C5 an average to save with its record");
            std::string sess = root + "/stackgen.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            check(txt.find("stackavggen 3 ") != std::string::npos,
                  "C5 the recipe line travels with its input tuple record");
            std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ImageDoc* back = nullptr;
            for (auto& d : app.images) if (!d->avgOfPath.empty()) back = d.get();
            fprintf(stderr, "stackavgselftest: C5 restored note='%s'\n",
                    back ? back->note.c_str() : "(none)");
            check(back != nullptr &&
                  back->note.find("recomputed from newer files") == std::string::npos &&
                  !back->avgGenChanged,
                  "C5 unchanged files restore with NO generation claim");
        }

        {   // ---- C6: #82's core - the stack was rewritten between sessions ----
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "C6 an average to betray");
            std::string sess = root + "/stackgen2.vsession";
            saveSession(sess, true);
            // let a coarse-mtime filesystem move before the rewrite: the tuple
            // comparison is mtime+fsize, and the pixels here keep the size
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 800.0f));
            size_t msgs = app.msgLog.size();
            std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ImageDoc* back = nullptr;
            for (auto& d : app.images) if (!d->avgOfPath.empty()) back = d.get();
            check(back != nullptr, "C6 the average still comes back");
            if (back) {
                bool now400 = true;                       // (100+800+300)/3
                for (float v : back->px()) if (v != 400.0f) now400 = false;
                fprintf(stderr, "stackavgselftest: C6 restored note='%s'\n",
                        back->note.c_str());
                check(now400,
                      "C6 ...recomputed from what the disk holds TODAY (as always)");
                check(back->note.find("recomputed from newer files (source changed "
                                      "since ") != std::string::npos,
                      "C6 ...and the note SAYS so, dating the saved picture");
                check(back->avgGenChanged,
                      "C6 ...and carries the mark the Files row shows");
                bool warned = false;
                for (size_t k = msgs; k < app.msgLog.size(); k++)
                    if (app.msgLog[k].err && app.msgLog[k].text.find(
                            "recomputed from newer files") != std::string::npos)
                        warned = true;
                check(warned, "C6 ...and a WARNING is surfaced at restore, not only stored");
            }
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 200.0f));   // fixture back
        }

        {   // ---- C7: #82 (1) - a second average is a SECOND doc ----------------
            int seq = 0;
            ImageDoc* m1 = averageOf(dA, &seq);
            check(m1 != nullptr, "C7 a first average");
            size_t before = app.images.size();
            requestStackAverage(seq);
            loadAll();
            check(app.images.size() == before + 1,
                  "C7 averaging twice makes a SECOND doc - results are never deduped");
            ImageDoc* m2 = app.images.back().get();
            check(m1 && m2 && m1 != m2 && m1->avgOfPath == m2->avgOfPath,
                  "C7 ...both naming the same stack");
            check(avgSamePicture(m1, m2),
                  "C7 same recorded input tuples -> the program can SAY 'same picture'");
            // the stack moves on: a third average records a DIFFERENT tuple,
            // and the reload marks both earlier results stale in one step.
            // Sleep first, like C6: mtime through _wstat is 1 s coarse on
            // Windows, and the rewrite keeps the byte size - a same-second
            // rewrite is genuinely the same tuple, which is not this fixture.
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 900.0f));
            int failed = 0;
            reloadStackFromDisk(seq, &failed);
            check(avgIsStale(m1) && avgIsStale(m2),
                  "C7 one reload marks BOTH earlier results stale");
            requestStackAverage(seq);
            loadAll();
            ImageDoc* m3 = app.images.back().get();
            fprintf(stderr, "stackavgselftest: C7 digests m1=%016llx m3=%016llx "
                            "(n %d/%d), m3 stale=%d\n",
                    (unsigned long long)m1->avgSrcDigest,
                    (unsigned long long)m3->avgSrcDigest,
                    m1->avgSrcCount, m3->avgSrcCount, (int)avgIsStale(m3));
            check(m3 != m2 && !avgIsStale(m3) &&
                  m3->avgSrcDigest != m1->avgSrcDigest && !avgSamePicture(m1, m3),
                  "C7 a new generation records a new tuple - and says NOT the same picture");
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 200.0f));   // fixture back
        }

        {   // ---- C8: #82 (2) - two session lines restore as two docs ----------
            int seq = 0;
            ImageDoc* m1 = averageOf(dA, &seq);
            check(m1 != nullptr, "C8 a first average to save");
            requestStackAverage(seq);
            loadAll();
            std::string sess = root + "/stacktwo.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            int lines = 0;
            for (size_t p = txt.find("stackavg "); p != std::string::npos;
                 p = txt.find("stackavg ", p + 1)) lines++;
            check(lines == 2, "C8 the session writes TWO recipe lines, not one key");
            std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int avgs = 0;
            bool all200 = true, anyClaim = false;
            for (auto& d : app.images)
                if (!d->avgOfPath.empty()) {
                    avgs++;
                    for (float v : d->px()) if (v != 200.0f) all200 = false;
                    if (d->note.find("recomputed from newer files") != std::string::npos)
                        anyClaim = true;
                }
            check(lerr.empty() && avgs == 2,
                  "C8 ...and two docs come back - same recipe, no collapse");
            check(all200 && !anyClaim,
                  "C8 ...each recomputed clean: same files, no generation claim");
        }

        {   // ---- C9: #82 (3) - the recipe follows the stack across batches ----
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "C9 an average whose stack will move");
            int nb = newBatch("moved-here");
            moveStackToBatch(seq, nb);
            std::string sess = root + "/stackmove.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            check(txt.find("stackavg ") != std::string::npos,
                  "C9 a batch move does not orphan the recipe (the path is the key)");
            std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.avgRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ImageDoc* back = nullptr;
            for (auto& d : app.images) if (!d->avgOfPath.empty()) back = d.get();
            check(back != nullptr &&
                  back->note.find("recomputed from newer files") == std::string::npos,
                  "C9 ...and it comes back clean from the moved stack");
            check(back && batchNameOf(back->batchId) == "moved-here",
                  "C9 ...in the batch the stack lives in now");
        }

        {   // ---- C10: #82 (4) - the generation is visible where reports are made
            int seq = 0;
            ImageDoc* m = averageOf(dA, &seq);
            check(m != nullptr, "C10 an average to export");
            for (size_t k = 0; k < app.images.size(); k++)
                if (app.images[k].get() == m) { selectImage((int)k); break; }
            std::string tsv = buildTemporalExport('\t');
            check(tsv.find("frame average of") != std::string::npos,
                  "C10 the export cites the full provenance note (recipe, count, when)");
            check(tsv.find("(stale)") == std::string::npos,
                  "C10 fresh: no stale token anywhere in the export");
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 700.0f));
            int failed = 0;
            reloadStackFromDisk(seq, &failed);
            tsv = buildTemporalExport('\t');
            fputs(tsv.c_str(), stdout);          // the stale document, for eyeballs
            check(tsv.find("(stale)") != std::string::npos,
                  "C10 stale reaches the TSV export's provenance lines");
            writeNpy(dA + "/f_001.npy", W, H, flat(W, H, 200.0f));   // fixture back
        }

        closeAll();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "stackavgselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
