// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --projframe-selftest: the Projection frame statistics are over EVERY ---
// ---- pixel of the region, and never show the previous region's number -------
//
// Issue #283. fStat (the panel's mean / sigma (frame) / p-p frame, the TSV's
// *_frame columns) was gathered inside the H pass, which above PROJ_MAX_SAMPLES
// visits one row in `step` - 1/6 of a 4096x3000 frame - while the panel said
// "the whole region". The ruling (option B): a full-pixel pass of its own, on a
// worker when the region is that big, "computing..." until it lands, the result
// keyed so a late answer for an old region is dropped.
//
//   F1  mono, step 6, synchronous door: fStat == a direct scan of every pixel
//       (mean, sigma, p-p, min, max, n), and NOT what the H rows alone give
//   F2  Bayer, step 12, "all" on: every plane and the pooled row == direct
//   F3  the worker door: pending with every fStat cleared, the TSV row says
//       "computing..." in all four frame cells, then the wait lands == direct
//   F4  a new region: pending at once, the previous region's mean appears
//       nowhere in the TSV row, and the landed answer is the NEW region's
//   F5  two regions in quick succession: only the last one's answer lands
//   F6  a crop while the pass runs: the fence stops it, the crop happens in
//       place (no copy forced by the stopped job), the answer is the crop's
//   F7  a recompute that does not move the key (reduce mode) keeps the pass
//   F8  the worker door on the mosaic: every plane == direct
//   F9  an ROI being DRAGGED (app.annBusy): the profile waits for the drag, the
//       frame cells go pending at once - never the previous region's numbers
//   F10 the same ProjState switched to another document of the same size:
//       the first document's late answer is not installed as the second's
//   F11 the pixels change in place under the same buffer (dataRev only): the
//       finished-but-unlanded answer for the old values is not installed
//   F12 a reload swap, F13 a remote full-resolution swap, F14 a frame-average
//       recompute, each WHILE a pass runs: the fence stopped and released the
//       job before the buffer was replaced, and what lands is the new pixels
//   F15 a region the H pass covers whole (step == cell): fStat is BIT-identical
//       to the pre-#283 accumulation, copied verbatim below
//   F16 RGB (each channel and the pooled row), F17 quad Bayer, F18 a region
//       with NaN / +inf / -inf: every value == a direct scan of the finite pixels
//
// The fixture has a row-periodic pattern with period == the stride, the case
// the issue names: the H pass at step 6 sees rows y%6 == 0 only and the hot row
// is y%6 == 3, so the old sample set never contained it. Every F prints the
// H-rows-only numbers beside the full ones; those lines are the PR's
// before/after measurement.
//
// NOGL and fixture-free: the documents are built in memory, every expected
// value is a scan written out below. The home is shared, so every app.* field
// borrowed here is put back at the end.
if (g_projFrameSelftest) {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "projframeselftest: %-74s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    const int saveMode = app.projMode;
    const bool saveAllRow = app.projAllRow, saveBusy = app.annBusy;
    const int saveAnn = app.selectedAnn;
    const std::vector<App::Ann> saveAnns = app.anns;
    const bool saveForce = g_forceAsyncProjFrame;
    app.projMode = 0;
    app.projAllRow = false;
    app.annBusy = false;
    app.anns.clear();
    app.selectedAnn = 0;

    constexpr int W = 4096, H = 3000;
    // the row pattern, period 6: row y%6 == 3 is the hot one
    static const float FPN[6] = { 0.0f, 2.0f, -3.0f, 12.0f, 1.0f, -2.0f };
    // `off` shifts every value, so two documents of one shape have different
    // answers; `ch` > 1 gives each channel its own level, as cfa gives each plane
    auto fill = [&](std::vector<float>& px, int w, int h, int ch, int cfa, float off) {
        px.resize((size_t)w * h * ch);
        uint32_t st = 0x2468ace1u;                 // fixed: the numbers repeat
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < ch; c++) {
                    st = st * 1664525u + 1013904223u;
                    float n = 16.0f * ((float)(st >> 8) * 0x1p-24f - 0.5f);   // +-8 DN
                    // a mosaic gets a level per plane (and a multi-channel frame
                    // per channel), so a phase or channel error is a WRONG LEVEL
                    // and cannot pass by reading a neighbour
                    float lvl = cfa ? 200.0f * (float)cfaChannelAtRaw(cfa, 0, x, y)
                                    : 300.0f * (float)c;
                    px[((size_t)y * w + x) * ch + c] = 1000.0f + off + lvl + FPN[y % 6] + n;
                }
    };
    auto mk = [&](const char* nm, int cfa, int w = 4096, int h = 3000, int ch = 1,
                  float off = 0.0f) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = w; d->src->h = h; d->src->ch = ch;
        d->src->dtype = "f32";
        fill(d->src->data, w, h, ch, cfa, off);
        d->cfa = cfa;
        d->cfaPattern = 0;                          // RGGB
        d->syncMirrors();
        computeMinMax(*d);
        d->uid = app.nextUid++;
        app.images.push_back(std::move(d));
        app.imagesRev++;
        return app.images.back().get();
    };
    // ---- the reference: an independent scan, two-pass, per slot -------------
    // `rowOk` picks the rows: every row (the truth), or the H pass's rows (what
    // the code used to report - printed as "before", and the anti-vacuity).
    struct Ref { double mean = 0, sd = 0, mn = 0, mx = 0, pp = 0; size_t n = 0; };
    // Slot s of a multi-channel frame is channel s; the pooled slot 4 is every
    // channel the panel shows (the first three). Non-finite samples are not
    // samples: skipped, and not counted.
    auto scan = [&](const ImageDoc& d, int rx, int ry, int rw, int rh, int slot,
                    const std::function<bool(int)>& rowOk) {
        const bool cfa = d.cfa != 0 && d.ch == 1;
        const int nCh = cfa ? 1 : std::min(d.ch, 3);
        auto each = [&](const std::function<void(double)>& f) {
            for (int y = ry; y < ry + rh; y++) {
                if (!rowOk(y - ry)) continue;
                for (int x = rx; x < rx + rw; x++) {
                    if (cfa && slot != 4 && cfaChannelAt(d, x, y) != slot) continue;
                    for (int c = 0; c < nCh; c++) {
                        if (!cfa && slot != 4 && c != slot) continue;
                        const double v = d.px()[((size_t)y * d.w + x) * d.ch + c];
                        if (std::isfinite(v)) f(v);
                    }
                }
            }
        };
        Ref r;
        double sum = 0;
        r.mn = DBL_MAX; r.mx = -DBL_MAX;
        each([&](double v) {
            sum += v; r.n++;
            r.mn = std::min(r.mn, v); r.mx = std::max(r.mx, v);
        });
        if (!r.n) return r;
        r.mean = sum / r.n;
        double ss = 0;
        each([&](double v) { ss += (v - r.mean) * (v - r.mean); });
        r.sd = std::sqrt(ss / r.n);                 // ddof=0, as sigma (frame)
        r.pp = r.mx - r.mn;
        return r;
    };
    auto allRows = [](int) { return true; };
    // the H pass's rows, from the recompute's own stride rule
    auto hRowsOf = [](int rw, int rh, int cell) {
        int step = (int)std::max<size_t>(1, ((size_t)rw * rh * cell) / 2000000);
        if (cell > 1) step = ((step + cell - 1) / cell) * cell;
        return std::function<bool(int)>([step, cell](int y) { return y % step < cell; });
    };
    // n, min, max and p-p are compared EXACTLY - they are what a subset gets
    // wrong. The mean to 1e-12 (the same additions in the same order). Sigma to
    // 1e-7: fStat's sigma is the single-pass E[x^2] - m^2 it has always been
    // (#283 did not touch the formula), and over 12 M samples at ~1000 DN that
    // form sits ~1e-8 relative away from this two-pass reference (measured
    // 7.5e-9) - while the H-rows sigma it replaces is 30 % away.
    auto same = [](const App::ProjState::Stats& f, const Ref& r) {
        auto rel = [](double a, double b, double t) { return std::fabs(a - b) <= t * std::fabs(b); };
        const bool eq = f.valid && f.n == r.n && f.mn == r.mn && f.mx == r.mx &&
                        f.pp == r.pp && rel(f.mean, r.mean, 1e-12) && rel(f.sd, r.sd, 1e-7);
        if (!eq)
            fprintf(stderr, "projframeselftest:   differs: n %zu/%zu mn %.17g/%.17g mx %.17g/%.17g "
                            "mean %.17g/%.17g sd %.17g/%.17g\n", f.n, r.n, f.mn, r.mn, f.mx, r.mx,
                    f.mean, r.mean, f.sd, r.sd);
        return eq;
    };
    auto say = [&](const char* tag, const char* plane, const App::ProjState::Stats& f,
                   const Ref& full, const Ref& old) {
        fprintf(stderr, "projframeselftest: %s %-3s now  n=%zu mean=%.6f sigma=%.6f p-p=%.4f "
                        "min=%.4f max=%.4f\n", tag, plane, f.n, f.mean, f.sd, f.pp, f.mn, f.mx);
        fprintf(stderr, "projframeselftest: %s %-3s full n=%zu mean=%.6f sigma=%.6f p-p=%.4f\n",
                tag, plane, full.n, full.mean, full.sd, full.pp);
        fprintf(stderr, "projframeselftest: %s %-3s Hrow n=%zu mean=%.6f sigma=%.6f p-p=%.4f "
                        "(the pre-#283 sample set)\n",
                tag, plane, old.n, old.mean, old.sd, old.pp);
    };
    auto setRoi = [&](int x, int y, int w, int h) {
        app.anns.clear();
        App::Ann a;
        a.id = app.nextAnnId++;
        a.type = 0; a.x = x; a.y = y; a.w = w; a.h = h;
        a.label = "pf";
        app.anns.push_back(a);
        app.selectedAnn = a.id;
    };
    auto clearRoi = [&]() { app.anns.clear(); app.selectedAnn = 0; };
    // the TSV cells by index: side, ch, mean, sf, sv, sh, qf, qv, qh, pf, pv, ph, n
    auto cells = [](const std::string& row) {
        std::vector<std::string> c;
        for (size_t i = 0, j; i <= row.size(); i = j + 1) {
            j = row.find_first_of("\t\n", i);
            if (j == std::string::npos) j = row.size();
            c.push_back(row.substr(i, j - i));
            if (j == row.size() || row[j] == '\n') break;
        }
        return c;
    };
    auto pendingSaid = [&](const App::ProjState& P, int i) {
        const std::vector<std::string> c = cells(projTsvRow(P, "A", false, i));
        return c.size() > 12 && c[2] == PROJ_FRAME_PENDING && c[3] == PROJ_FRAME_PENDING &&
               c[6] == PROJ_FRAME_PENDING && c[9] == PROJ_FRAME_PENDING &&
               c[12] == PROJ_FRAME_PENDING;
    };
    auto noneValid = [](const App::ProjState& P) {
        for (int s = 0; s < 5; s++) if (P.fStat[s].valid) return false;
        return true;
    };

    ImageDoc* M = mk("pf_mono", 0);

    // ---- F1: mono, synchronous door ------------------------------------------
    {
        g_forceAsyncProjFrame = false;
        App::ProjState P;
        recomputeProjectionIfNeeded(M, P);
        const Ref full = scan(*M, 0, 0, W, H, 0, allRows);
        const Ref old = scan(*M, 0, 0, W, H, 0, hRowsOf(W, H, 1));
        say("F1", "ch0", P.fStat[0], full, old);
        check(!P.fPending && same(P.fStat[0], full),
              "F1 mono 4096x3000: fStat == a scan of every pixel (mean/sd/p-p/min/max/n)");
        check(P.fStat[0].n == (size_t)W * H, "F1 ...n is every pixel of the frame");
        check(old.n * 6 == full.n && old.pp < full.pp && P.fStat[0].pp > old.pp,
              "F1 ...and NOT the H rows' answer: 1/6 of the rows, a smaller p-p");
        // ...and the TSV publishes the count: n_frame is the 13th cell
        const std::vector<std::string> c = cells(projTsvRow(P, "A", false, 0));
        check(c.size() > 12 && c[12] == std::to_string((size_t)W * H),
              "F1 the TSV's n_frame column carries the full pixel count");
        // what the pass costs - the reason it is not on the UI thread. Printed,
        // not asserted: a time is a property of the machine.
        App::ProjState::Stats out[5];
        const auto t0 = std::chrono::steady_clock::now();
        projFrameMoments(M->px().data(), W, 1, 0, 0, 1, false, 0, 0, W, H, out, nullptr);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "projframeselftest: F1 one every-pixel pass over %d x %d: %.1f ms\n",
                W, H, ms);
    }

    ImageDoc* B = mk("pf_bayer", 1);
    static const char* PL[5] = { "R", "Gr", "Gb", "B", "all" };

    // ---- F2: Bayer, synchronous door, every plane and the pooled row ---------
    {
        g_forceAsyncProjFrame = false;
        app.projAllRow = true;
        App::ProjState P;
        recomputeProjectionIfNeeded(B, P);
        bool all = !P.fPending, fewer = true;
        for (int s = 0; s < 5; s++) {
            const Ref full = scan(*B, 0, 0, W, H, s, allRows);
            const Ref old = scan(*B, 0, 0, W, H, s, hRowsOf(W, H, 2));
            say("F2", PL[s], P.fStat[s], full, old);
            all = all && same(P.fStat[s], full);
            fewer = fewer && old.n * 6 == full.n;
        }
        check(all, "F2 Bayer: R/Gr/Gb/B and the pooled row == a scan of every pixel");
        check(fewer && P.fStat[0].n == (size_t)W * H / 4 && P.fStat[4].n == (size_t)W * H,
              "F2 ...each plane over a quarter of the frame, \"all\" over all of it");
        app.projAllRow = false;
    }

    // ---- F3: the worker door ---------------------------------------------------
    g_forceAsyncProjFrame = true;
    App::ProjState Q;                 // ONE state from here on, as the panel has
    {
        const int starts = g_projFrameJobStarts;
        recomputeProjectionIfNeeded(M, Q);
        check(Q.fPending && noneValid(Q) && g_projFrameJobStarts == starts + 1,
              "F3 step > 1 on the worker door: pending, and every fStat cleared");
        check(pendingSaid(Q, 0),
              "F3 ...the TSV row says computing... in mean/sigma/%/p-p/n_frame");
        check(Q.hStat[0].valid && Q.vStat[0].valid,
              "F3 ...while the profile statistics are there at once");
        projFrameWait(M, Q);
        const Ref full = scan(*M, 0, 0, W, H, 0, allRows);
        check(!Q.fPending && same(Q.fStat[0], full),
              "F3 the wait lands it: == a scan of every pixel");
    }
    const App::ProjState::Stats wholeF = Q.fStat[0];

    // ---- F4: a new region ------------------------------------------------------
    {
        setRoi(64, 32, 3000, 2000);            // 6 Mpx: step 3, the worker again
        recomputeProjectionIfNeeded(M, Q);
        char oldMean[40];
        snprintf(oldMean, sizeof oldMean, "%.9g", wholeF.mean);
        const std::string row = projTsvRow(Q, "A", false, 0);
        check(Q.fPending && noneValid(Q) && pendingSaid(Q, 0) &&
              row.find(oldMean) == std::string::npos,
              "F4 new ROI: pending at once - the whole frame's mean is nowhere in the row");
        projFrameWait(M, Q);
        const Ref roi = scan(*M, 64, 32, 3000, 2000, 0, allRows);
        const Ref old = scan(*M, 64, 32, 3000, 2000, 0, hRowsOf(3000, 2000, 1));
        say("F4", "ch0", Q.fStat[0], roi, old);
        check(!Q.fPending && same(Q.fStat[0], roi) && Q.fStat[0].n == (size_t)3000 * 2000 &&
              Q.fStat[0].mean != wholeF.mean,
              "F4 ...and what lands is the ROI's every pixel, not the frame's");
    }

    // ---- F5: two regions in quick succession -------------------------------------
    {
        setRoi(0, 0, 2500, 2400);
        recomputeProjectionIfNeeded(M, Q);
        check(Q.fPending, "F5 first ROI: pending");
        setRoi(700, 400, 3200, 2500);
        recomputeProjectionIfNeeded(M, Q);
        check(Q.fPending && noneValid(Q), "F5 second ROI before the first landed: pending");
        projFrameWait(M, Q);
        const Ref last = scan(*M, 700, 400, 3200, 2500, 0, allRows);
        check(!Q.fPending && same(Q.fStat[0], last) && Q.rx == 700 && Q.rw == 3200,
              "F5 only the LAST region's answer lands");
    }

    // ---- F7: a recompute that does not move the key keeps the pass --------------
    {
        clearRoi();
        recomputeProjectionIfNeeded(M, Q);
        const int starts = g_projFrameJobStarts;
        app.projMode = 1;                       // max: the profiles recompute...
        recomputeProjectionIfNeeded(M, Q);
        check(Q.fPending && g_projFrameJobStarts == starts && Q.fJob != nullptr,
              "F7 reduce mode changed: the frame pass is kept, not restarted");
        projFrameWait(M, Q);
        check(!Q.fPending && same(Q.fStat[0], scan(*M, 0, 0, W, H, 0, allRows)),
              "F7 ...and lands the same every-pixel answer");
        app.projMode = 0;
        recomputeProjectionIfNeeded(M, Q);
        check(!Q.fPending && Q.fStat[0].valid,
              "F7 ...which then survives a mode change back (nothing to recompute)");
    }

    // ---- F8: the worker door on the mosaic ---------------------------------------
    {
        App::ProjState P;
        recomputeProjectionIfNeeded(B, P);
        check(P.fPending && noneValid(P), "F8 Bayer on the worker door: pending");
        projFrameWait(B, P);
        bool all = !P.fPending;
        for (int s = 0; s < 4; s++) all = all && same(P.fStat[s], scan(*B, 0, 0, W, H, s, allRows));
        check(all, "F8 ...every plane lands == a scan of every pixel");
    }

    // ---- F9: an ROI being dragged ------------------------------------------------
    {
        App::ProjState P;
        setRoi(0, 0, 3000, 2000);
        recomputeProjectionIfNeeded(M, P);
        projFrameWait(M, P);
        check(!P.fPending && same(P.fStat[0], scan(*M, 0, 0, 3000, 2000, 0, allRows)),
              "F9 before the drag: the ROI's every-pixel answer");
        app.annBusy = true;
        setRoi(100, 100, 2800, 1900);
        recomputeProjectionIfNeeded(M, P);
        check(P.rx == 0 && P.rw == 3000,
              "F9 mid-drag: the profile keeps the last region (unchanged rule)");
        check(P.fPending && noneValid(P) && pendingSaid(P, 0),
              "F9 ...but the frame cells say computing..., not the last region's numbers");
        // ...and a small target region (the synchronous size) is no different
        setRoi(10, 10, 600, 400);
        recomputeProjectionIfNeeded(M, P);
        check(P.fPending && noneValid(P), "F9 mid-drag to a small region: pending too");
        app.annBusy = false;
        recomputeProjectionIfNeeded(M, P);
        check(!P.fPending && same(P.fStat[0], scan(*M, 10, 10, 600, 400, 0, allRows)),
              "F9 the drag ends on a small region: its answer at once (synchronous size)");
        app.annBusy = true;
        setRoi(100, 100, 2800, 1900);
        recomputeProjectionIfNeeded(M, P);
        app.annBusy = false;
        recomputeProjectionIfNeeded(M, P);
        projFrameWait(M, P);
        check(!P.fPending && same(P.fStat[0], scan(*M, 100, 100, 2800, 1900, 0, allRows)),
              "F9 the drag ends on a large region: the pass runs and lands ITS answer");
        clearRoi();
    }

    ImageDoc* M2 = mk("pf_mono2", 0, W, H, 1, 50.0f);

    // ---- F10: another document of the same size, same slot ------------------------
    {
        App::ProjState P;
        recomputeProjectionIfNeeded(M, P);                 // M's pass starts...
        check(P.fPending && P.fJob != nullptr, "F10 M's pass running");
        recomputeProjectionIfNeeded(M2, P);                // ...and the slot moves on
        projFrameWait(M2, P);
        const Ref r2 = scan(*M2, 0, 0, W, H, 0, allRows);
        check(!P.fPending && same(P.fStat[0], r2) && P.fStat[0].mean > wholeF.mean + 40.0,
              "F10 same rect, other document: M2's own answer, not M's late one");
    }

    // ---- F11: the same buffer, new values (dataRev only) ---------------------------
    {
        App::ProjState P;
        recomputeProjectionIfNeeded(M2, P);
        check(P.fPending && P.fJob != nullptr, "F11 pass running");
        P.fJob->th.join();                                  // finished, NOT landed
        const float* buf = M2->px().data();
        for (float& v : M2->src->data) v += 7.0f;           // in place: no realloc
        M2->dataRev++;
        check(M2->px().data() == buf, "F11 the buffer is the same one");
        recomputeProjectionIfNeeded(M2, P);
        projFrameWait(M2, P);
        check(!P.fPending && same(P.fStat[0], scan(*M2, 0, 0, W, H, 0, allRows)),
              "F11 the answer for the OLD values is dropped: the new values' lands");
    }

    // the fence fired: the job the writer found was stopped and let go of the
    // source BEFORE the buffer was replaced. Asked of the job the slot still
    // holds, right after the writer returns and before anything pumps it.
    auto fenced = [](const App::ProjState& P) {
        return P.fJob != nullptr && P.fJob->cancel.load() && P.fJob->src == nullptr;
    };
    const int W2 = 2048, H2 = 2048;                         // 4.2 Mpx: step 2

    // ---- F12: a reload swap while the pass runs -------------------------------------
    {
        const std::string path = (std::filesystem::temp_directory_path() /
                                  "viewer_projframe_reload.npy").u8string();
        std::vector<float> a, b;
        fill(a, W2, H2, 1, 0, 0.0f);
        fill(b, W2, H2, 1, 0, 100.0f);
        npyWriteFile(path, "<f4", { H2, W2 }, a.data(), a.size() * sizeof(float));
        const size_t before = app.images.size();
        const std::string lerr = loadNpy(path);
        ImageDoc* R = app.images.size() > before ? app.images.back().get() : nullptr;
        check(lerr.empty() && R && R->w == W2, "F12 the .npy opened");
        if (R) {
            App::ProjState P;
            recomputeProjectionIfNeeded(R, P);
            check(P.fPending && P.fJob != nullptr, "F12 pass running");
            npyWriteFile(path, "<f4", { H2, W2 }, b.data(), b.size() * sizeof(float));
            std::string rerr;
            const bool rok = reloadSource(R->src, rerr);
            check(rok && fenced(P), "F12 the reload swap stopped and released the pass first");
            recomputeProjectionIfNeeded(R, P);
            projFrameWait(R, P);
            const Ref r = scan(*R, 0, 0, W2, H2, 0, allRows);
            check(!P.fPending && same(P.fStat[0], r) && r.mean > 1090.0,
                  "F12 ...and what lands is the RELOADED pixels' every-pixel answer");
        }
        std::error_code ec;
        std::filesystem::remove(pathFromUtf8(path), ec);
    }

    // ---- F13: a remote full-resolution swap while the pass runs ------------------------
    {
        ImageDoc* Q = mk("pf_remote", 0, W2, H2);
        Q->src->remoteStep = 2;                             // "a preview"
        App::ProjState P;
        recomputeProjectionIfNeeded(Q, P);
        check(P.fPending && P.fJob != nullptr, "F13 pass running");
        App::RFetchDone d;
        d.uid = Q->uid;
        d.w = W2; d.h = H2; d.ch = 1;
        d.dtype = "f32";
        fill(d.data, W2, H2, 1, 0, 200.0f);
        d.vmin = 0; d.vmax = 4000;
        d.gen = app.rfGen.load();
        app.rfPending++;
        {
            std::lock_guard<std::mutex> lk(app.rfMtx);
            app.rfDone.push_back(std::move(d));
        }
        pumpRemoteFetch();
        check(Q->src->remoteStep == 1 && fenced(P),
              "F13 the remote swap stopped and released the pass first");
        recomputeProjectionIfNeeded(Q, P);
        projFrameWait(Q, P);
        const Ref r = scan(*Q, 0, 0, W2, H2, 0, allRows);
        check(!P.fPending && same(P.fStat[0], r) && r.mean > 1190.0,
              "F13 ...and what lands is the FULL-RESOLUTION pixels' answer");
    }

    // ---- F14: a frame-average recompute while the pass runs ----------------------------
    {
        ImageDoc* f0 = mk("pf_avg0", 0, W2, H2, 1, 0.0f);
        ImageDoc* f1 = mk("pf_avg1", 0, W2, H2, 1, 40.0f);
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = "pf_avg";
        app.seqs.push_back(si);
        f0->seqId = si.id; f0->seqIndex = 0;
        f1->seqId = si.id; f1->seqIndex = 1;
        ImageDoc* X = mk("pf_avg_into", 0, W2, H2, 1, 500.0f);
        App::ProjState P;
        recomputeProjectionIfNeeded(X, P);
        check(P.fPending && P.fJob != nullptr, "F14 pass running");
        std::string aerr;
        const bool aok = stackReduceFrame(si.id, FOLD_MEAN, aerr, nullptr, X);
        check(aok && fenced(P), "F14 the frame-average recompute stopped and released the pass first");
        recomputeProjectionIfNeeded(X, P);
        projFrameWait(X, P);
        const Ref r = scan(*X, 0, 0, W2, H2, 0, allRows);
        check(!P.fPending && same(P.fStat[0], r) && r.mean > 1010.0 && r.mean < 1030.0,
              "F14 ...and what lands is the AVERAGE's every-pixel answer");
    }

    // ---- F15: where the H pass covers every pixel, not one bit moves -------------------
    // THE PRE-#283 ACCUMULATION, copied verbatim from panel_histogram.inc as it
    // stood on main (the H pass with step == cell, the region moments gathered
    // in its `toH` branch, the same formulas after). Copying it is the point:
    // the claim is "the same additions in the same order", and only the old
    // code itself can say what that order was.
    {
        auto oldMoments = [](const ImageDoc& im, int rx, int ry, int rw, int rh, bool allRow,
                             int nSeries, App::ProjState::Stats out[5]) {
            const bool cfa = im.ch == 1 && im.cfa != 0;
            double fSum[5] = {}, fSum2[5] = {};
            size_t fN[5] = {};
            double fMn[5] = { DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX };
            double fMx[5] = { -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
            const int cell = im.cfa == 2 ? 4 : im.cfa ? 2 : 1;
            int step = (int)std::max<size_t>(1, ((size_t)rw * rh * cell) / 2000000);
            if (cell > 1) step = ((step + cell - 1) / cell) * cell;
            auto accumulate = [&](int x, int y) {
                const float* src = &im.px()[((size_t)(ry + y) * im.w + (rx + x)) * im.ch];
                int lo = 0, hi = nSeries;
                if (cfa) { lo = cfaChannelAt(im, rx + x, ry + y); hi = lo + 1; }
                auto put = [&](int slot, float val) {
                    fSum[slot] += val; fSum2[slot] += (double)val * val; fN[slot]++;
                    fMn[slot] = std::min(fMn[slot], (double)val);
                    fMx[slot] = std::max(fMx[slot], (double)val);
                };
                for (int s = lo; s < hi; s++) {
                    float val = cfa ? src[0] : src[std::min(s, im.ch - 1)];
                    if (!std::isfinite(val)) continue;
                    put(s, val);
                    if (allRow) put(4, val);
                }
            };
            for (int y = 0; y < rh; y += step)
                for (int dy = 0; dy < cell && y + dy < rh; dy++)
                    for (int x = 0; x < rw; x++) accumulate(x, y + dy);
            for (int s = 0; s < 5; s++) {
                App::ProjState::Stats& f = out[s];
                f = {};
                if (!fN[s]) continue;
                f.n = fN[s];
                f.mean = fSum[s] / fN[s];
                double var = fSum2[s] / fN[s] - f.mean * f.mean;
                f.sd = sqrt(var > 0 ? var : 0);
                f.mn = fMn[s]; f.mx = fMx[s]; f.pp = f.mx - f.mn;
                f.pct = f.mean != 0 ? f.sd / fabs(f.mean) * 100.0 : 0.0;
                f.valid = true;
            }
            return step;
        };
        auto bits = [](const App::ProjState::Stats& a, const App::ProjState::Stats& b) {
            return a.valid == b.valid && a.n == b.n &&
                   memcmp(&a.mean, &b.mean, sizeof(double)) == 0 &&
                   memcmp(&a.sd, &b.sd, sizeof(double)) == 0 &&
                   memcmp(&a.mn, &b.mn, sizeof(double)) == 0 &&
                   memcmp(&a.mx, &b.mx, sizeof(double)) == 0 &&
                   memcmp(&a.pp, &b.pp, sizeof(double)) == 0 &&
                   memcmp(&a.pct, &b.pct, sizeof(double)) == 0;
        };
        ImageDoc* S1 = mk("pf_small_mono", 0, 1536, 1100, 1, 0.37f);
        ImageDoc* S2 = mk("pf_small_bayer", 1, 1536, 1100, 1, 0.37f);
        ImageDoc* S3 = mk("pf_small_rgb", 0, 900, 700, 3, 0.37f);
        struct Case { ImageDoc* d; int rx, ry, rw, rh; bool all; const char* what; };
        const Case cases[] = {
            { S1, 0, 0, 1536, 1100, false, "F15 mono 1536x1100: bit-identical to the old accumulation" },
            { S2, 3, 1, 1501, 1077, true,  "F15 Bayer, odd ROI origin, with all: bit-identical" },
            { S3, 0, 0, 900, 700, true,    "F15 RGB with all: bit-identical" },
        };
        for (const Case& c : cases) {
            app.projAllRow = c.all;
            if (c.rx || c.ry) setRoi(c.rx, c.ry, c.rw, c.rh); else clearRoi();
            App::ProjState P;
            recomputeProjectionIfNeeded(c.d, P);
            App::ProjState::Stats old[5];
            const int step = oldMoments(*c.d, c.rx, c.ry, c.rw, c.rh, P.allRow, P.nSeries, old);
            bool eq = !P.fPending && step == P.cell;
            for (int s = 0; s < 5; s++) eq = eq && bits(P.fStat[s], old[s]);
            check(eq, c.what);
        }
        app.projAllRow = false;
        clearRoi();
    }

    // ---- F16: RGB, each channel and the pooled row ------------------------------------
    {
        ImageDoc* C3 = mk("pf_rgb", 0, 2560, 2000, 3);
        app.projAllRow = true;
        App::ProjState P;
        recomputeProjectionIfNeeded(C3, P);
        projFrameWait(C3, P);
        bool all = !P.fPending && P.nSeries == 3;
        for (int s : { 0, 1, 2, 4 }) {
            const Ref r = scan(*C3, 0, 0, 2560, 2000, s, allRows);
            all = all && same(P.fStat[s], r);
        }
        check(all && P.fStat[2].mean > P.fStat[0].mean + 500.0,
              "F16 RGB 2560x2000: ch0/ch1/ch2 and \"all\" == a scan of every pixel");
        app.projAllRow = false;
    }

    // ---- F17: quad Bayer --------------------------------------------------------------
    {
        ImageDoc* QB = mk("pf_quad", 2, W2, H2);
        App::ProjState P;
        recomputeProjectionIfNeeded(QB, P);
        projFrameWait(QB, P);
        bool all = !P.fPending && P.cell == 4;
        for (int s = 0; s < 4; s++) all = all && same(P.fStat[s], scan(*QB, 0, 0, W2, H2, s, allRows));
        check(all && P.fStat[3].mean > P.fStat[0].mean + 500.0,
              "F17 quad Bayer 2048x2048: every plane == a scan of every pixel");
    }

    // ---- F18: NaN and infinities are not samples ---------------------------------------
    {
        ImageDoc* NF = mk("pf_nonfinite", 0, W2, H2);
        const float inf = std::numeric_limits<float>::infinity();
        NF->src->data[(size_t)7 * W2 + 5] = std::numeric_limits<float>::quiet_NaN();
        NF->src->data[(size_t)3 * W2 + 100] = inf;            // an odd row: not an H row
        NF->src->data[(size_t)2046 * W2 + 2000] = -inf;       // an even row: an H row
        NF->src->data[(size_t)1001 * W2 + 9] = std::numeric_limits<float>::quiet_NaN();
        NF->dataRev++;
        App::ProjState P;
        recomputeProjectionIfNeeded(NF, P);
        projFrameWait(NF, P);
        const Ref r = scan(*NF, 0, 0, W2, H2, 0, allRows);
        check(!P.fPending && same(P.fStat[0], r) && P.fStat[0].n == (size_t)W2 * H2 - 4 &&
              std::isfinite(P.fStat[0].mean) && std::isfinite(P.fStat[0].pp),
              "F18 NaN/+inf/-inf excluded: n = every pixel - 4, values == the finite scan");
    }

    // ---- F6: a crop while the pass runs (last: it changes M) --------------------
    {
        clearRoi();
        App::ProjState P;
        recomputeProjectionIfNeeded(M, P);
        check(P.fPending && P.fJob != nullptr, "F6 pass started");
        const uint64_t idBefore = M->src->srcId;
        const bool cropped = cropInPlace(*M, 0, 0, 3000, 2400);
        // a clone would carry a fresh srcId (cloneSource): the same id is the
        // in-place branch, which the job's reference would have ruled out
        check(cropped && M->w == 3000 && M->h == 2400 && M->src->srcId == idBefore,
              "F6 the crop happened IN PLACE: the stopped pass forced no copy");
        recomputeProjectionIfNeeded(M, P);
        check(P.fPending && noneValid(P), "F6 ...the frame statistics are pending again");
        projFrameWait(M, P);
        const Ref c = scan(*M, 0, 0, 3000, 2400, 0, allRows);
        check(!P.fPending && same(P.fStat[0], c) && P.fStat[0].n == (size_t)3000 * 2400,
              "F6 ...and land over the CROPPED pixels");
    }

    // The home is shared: put back everything borrowed.
    Q = App::ProjState{};
    closeAll();
    app.seqs.clear();
    app.projMode = saveMode;
    app.projAllRow = saveAllRow;
    app.annBusy = saveBusy;
    app.anns = saveAnns;
    app.selectedAnn = saveAnn;
    g_forceAsyncProjFrame = saveForce;
    fprintf(stderr, "projframeselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
