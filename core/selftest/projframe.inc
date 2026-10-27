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

    const int W = 4096, H = 3000;
    // the row pattern, period 6: row y%6 == 3 is the hot one
    static const float FPN[6] = { 0.0f, 2.0f, -3.0f, 12.0f, 1.0f, -2.0f };
    auto mk = [&](const char* nm, int cfa) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = H; d->src->ch = 1;
        d->src->dtype = "f32";
        d->src->data.resize((size_t)W * H);
        uint32_t st = 0x2468ace1u;                 // fixed: the numbers repeat
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                st = st * 1664525u + 1013904223u;
                float n = 16.0f * ((float)(st >> 8) * 0x1p-24f - 0.5f);   // +-8 DN
                // a mosaic gets a level per plane, so a phase error is a WRONG
                // LEVEL and cannot pass by reading a neighbour
                float lvl = cfa ? 200.0f * (float)(((y & 1) << 1) | (x & 1)) : 0.0f;
                d->src->data[(size_t)y * W + x] = 1000.0f + lvl + FPN[y % 6] + n;
            }
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
    auto scan = [&](const ImageDoc& d, int rx, int ry, int rw, int rh, int slot,
                    const std::function<bool(int)>& rowOk) {
        const bool cfa = d.cfa != 0;
        auto in = [&](int x, int y) {
            if (slot == 4 || !cfa) return true;
            return cfaChannelAt(d, x, y) == slot;
        };
        Ref r;
        double sum = 0;
        r.mn = DBL_MAX; r.mx = -DBL_MAX;
        for (int y = ry; y < ry + rh; y++) {
            if (!rowOk(y - ry)) continue;
            for (int x = rx; x < rx + rw; x++) {
                if (!in(x, y)) continue;
                const double v = d.px()[(size_t)y * d.w + x];
                sum += v; r.n++;
                r.mn = std::min(r.mn, v); r.mx = std::max(r.mx, v);
            }
        }
        if (!r.n) return r;
        r.mean = sum / r.n;
        double ss = 0;
        for (int y = ry; y < ry + rh; y++) {
            if (!rowOk(y - ry)) continue;
            for (int x = rx; x < rx + rw; x++) {
                if (!in(x, y)) continue;
                const double e = d.px()[(size_t)y * d.w + x] - r.mean;
                ss += e * e;
            }
        }
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
