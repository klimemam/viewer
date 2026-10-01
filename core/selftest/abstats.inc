// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
    // ---- A/B statistics caches (docs/features/compare/ab-stats-plan.md, section 6) -------------
    // The five statistics panels keep TWO cache slots now: 0 = A (the current
    // frame), 1 = B (the compare side). Headless, what has to hold:
    //   A1  both slots fill, each keyed to its OWN document
    //   A2  B's numbers really are B's - recomputed here from B's pixels by a
    //       second implementation, not by calling the panel's code
    //   A3  A's numbers are byte-identical to the compare-off run: B's existence
    //       must not move a single digit on the A side
    //   A4  a frame step follows on both sides, and temporal (keyed on the STACK,
    //       not the frame) does NOT re-run for it
    //   A6  compare off invalidates slot 1 exactly once, and closing B leaves no
    //       dangling ImageDoc* behind in it
    if (!g_abstatsSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 300.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "abstatsselftest: %-54s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto relEq = [](double x, double y, double tol) {
            double m = std::max(std::max(fabs(x), fabs(y)), 1e-300);
            return fabs(x - y) <= tol * m;
        };

        openFolder(g_abstatsSelftest);
        loadAll();
        if (app.seqs.size() < 2) {
            fprintf(stderr, "abstatsselftest: need 2 stacks under %s\n",
                    g_abstatsSelftest.c_str());
            stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
            return 1;
        }

        // ---- the reference implementations, written from the rule, not shared
        // with the panel: 256 bins on the SNAPPED grid (docs/background/project/todo-open.md 9 -
        // an integer number of DN per bin, or a power of two for float), one
        // sample every `step`-th pixel of the resolved rect, CFA planes never
        // mixed.
        struct RefHist {
            uint32_t bins[4][256] = {};
            double mean[4] = {}, sd[4] = {};
            size_t sampled = 0;
            int nSeries = 0;
        };
        auto refHistogram = [](const ImageDoc& im) {
            RefHist R;
            const int rw = im.w, rh = im.h;             // whole image: no ROI here
            bool cfa = im.ch == 1 && im.cfa != 0;
            R.nSeries = cfa ? 4 : std::min(im.ch, 3);
            const float black = effBlack(im), white = effWhite(im);
            // The grid, from the RULE and by a different construction than the
            // panel's: the power of two is found by doubling/halving (exact in
            // binary, and not frexp), the integer width by ceil. Sharing
            // histBinGrid here would make this reference agree with the panel by
            // definition, which is the one thing it must not do.
            float span = white - black;
            if (!(span > 0)) span = 1.0f;
            const float q = span / 256.0f;
            const bool isInt = !im.dtype.empty() &&
                               (im.dtype[0] == 'u' || im.dtype[0] == 'i' ||
                                im.dtype == "bool");
            float bw;
            if (isInt) {
                bw = std::ceil(q);
                if (!(bw >= 1.0f)) bw = 1.0f;
            } else {
                bw = 1.0f;
                while (bw < q) bw *= 2.0f;          // up to the first >= q ...
                while (bw * 0.5f >= q) bw *= 0.5f;  // ... and down to the least
            }
            const float origin = (float)(std::floor((double)black / (double)bw) *
                                         (double)bw);
            const size_t total = (size_t)rw * rh;
            const size_t step = std::max<size_t>(1, total / 1000000);
            double s1[4] = {}, s2[4] = {};
            size_t n[4] = {};
            // row-major walk, keeping every step-th linear index: the same
            // sample SET as the panel, reached a different way
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    size_t p = (size_t)y * rw + x;
                    if (p % step) continue;
                    const float* src = &im.px()[((size_t)y * im.w + x) * im.ch];
                    if (cfa) {
                        float v = src[0];
                        if (std::isfinite(v)) {
                            int s = cfaChannelAt(im, x, y);
                            double t = ((double)v - origin) / bw;
                            int b = t < 0 ? 0 : (t >= 256 ? 255 : (int)t);
                            R.bins[s][b]++;
                            s1[s] += v; s2[s] += (double)v * v; n[s]++;
                        }
                    } else {
                        for (int c = 0; c < R.nSeries; c++) {
                            float v = src[c];
                            if (!std::isfinite(v)) continue;
                            double t = ((double)v - origin) / bw;
                            int b = t < 0 ? 0 : (t >= 256 ? 255 : (int)t);
                            R.bins[c][b]++;
                            s1[c] += v; s2[c] += (double)v * v; n[c]++;
                        }
                    }
                    R.sampled++;
                }
            for (int s = 0; s < R.nSeries; s++) {
                if (!n[s]) continue;
                double m = s1[s] / n[s], var = s2[s] / n[s] - m * m;
                R.mean[s] = m;
                R.sd[s] = sqrt(var > 0 ? var : 0);
            }
            return R;
        };
        // Independent temporal noise over a stack: per-sample variance across
        // the resident frames, on the same 40000-sample grid - and PER CFA
        // PLANE, from the same rule refHistogram above is written from. "CFA
        // planes are NEVER mixed in a statistic" (docs/terminology.md) is a
        // rule about every statistic on a mosaiced frame, not about the
        // histogram alone: on a mosaic sigma_t is four numbers, and pooling the
        // four planes into one produces a quantity nothing on screen claims.
        struct RefSigT {
            int nPl = 1;
            double sigT[4] = {};
        };
        auto refSigmaT = [](const ImageDoc& im) {
            RefSigT R;
            std::vector<int> f = framesOfSeq(im.seqId);
            // a mosaic needs one whole cell to be a mosaic - the same condition
            // the panel resolves nPl from, reached from the rule, not shared
            const bool cfa = im.ch == 1 && im.cfa != 0;
            const int cell = im.cfa == 2 ? 4 : 2;        // Quad Bayer repeats over 4x4
            R.nPl = cfa && im.w >= cell && im.h >= cell ? 4 : 1;
            const size_t total = (size_t)im.w * im.h;
            const size_t step = std::max<size_t>(1, total / 40000);
            std::vector<size_t> offs;
            std::vector<uint8_t> plane;                  // which plane each sample is
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) {
                    size_t p = (size_t)y * im.w + x;
                    if (p % step) continue;
                    offs.push_back(((size_t)y * im.w + x) * im.ch);
                    plane.push_back((uint8_t)(R.nPl == 1 ? 0 : cfaChannelAt(im, x, y)));
                }
            std::vector<double> s1(offs.size(), 0), s2(offs.size(), 0);
            int used = 0;
            for (int fi : f) {
                const ImageDoc& fr = *app.images[fi];
                if (fr.w != im.w || fr.h != im.h || fr.ch != im.ch) continue;
                bool any = false;
                for (size_t k = 0; k < offs.size(); k++) {
                    float v = fr.px()[offs[k]];
                    if (!std::isfinite(v)) continue;
                    s1[k] += v; s2[k] += (double)v * v; any = true;
                }
                if (any) used++;
            }
            if (used < 2) return R;
            double tvar[4] = {};
            size_t n[4] = {};
            for (size_t k = 0; k < offs.size(); k++) {
                double m = s1[k] / used;
                // ddof=1 (#57 item 3). This reference divided by `used` while
                // serve.cpp divided by used-1, so it agreed with the panel only
                // because the PANEL was the side that was wrong - two copies of
                // one mistake reading as a confirmation. The number this
                // reference exists to check is defined once, in
                // docs/features/analysis/stats-taxonomy.md, and the definition is the unbiased
                // one; the whole point of computing it here rather than calling
                // the panel's code is that it is written from the DEFINITION.
                double v = std::max(0.0, s2[k] / used - m * m) *
                           ((double)used / (double)(used - 1));
                tvar[plane[k]] += v;
                n[plane[k]]++;
            }
            for (int p = 0; p < R.nPl; p++)
                if (n[p]) R.sigT[p] = sqrt(tvar[p] / n[p]);
            return R;
        };
        // "byte-identical" as the plan means it: every number the panel puts on
        // screen, compared bit for bit. (Not memcmp over the whole struct - its
        // padding bytes are unspecified and would make the test lie either way.)
        auto histSame = [](const App::HistState& a, const App::HistState& b) {
            return a.nSeries == b.nSeries && a.maxBin == b.maxBin &&
                   a.sampled == b.sampled && a.rx == b.rx && a.ry == b.ry &&
                   a.rw == b.rw && a.rh == b.rh && a.roiUsed == b.roiUsed &&
                   memcmp(&a.black, &b.black, sizeof a.black) == 0 &&
                   memcmp(&a.white, &b.white, sizeof a.white) == 0 &&
                   memcmp(&a.binOrigin, &b.binOrigin, sizeof a.binOrigin) == 0 &&
                   memcmp(&a.binW, &b.binW, sizeof a.binW) == 0 &&
                   memcmp(a.bins, b.bins, sizeof a.bins) == 0 &&
                   memcmp(a.mean, b.mean, sizeof a.mean) == 0 &&
                   memcmp(a.sd, b.sd, sizeof a.sd) == 0 &&
                   memcmp(&a.clipLo, &b.clipLo, sizeof a.clipLo) == 0 &&
                   memcmp(&a.clipHi, &b.clipHi, sizeof a.clipHi) == 0;
        };
        auto vecSame = [](const std::vector<float>& a, const std::vector<float>& b) {
            return a.size() == b.size() &&
                   (a.empty() || memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
        };
        auto projSame = [&](const App::ProjState& a, const App::ProjState& b) {
            if (a.nSeries != b.nSeries || a.rw != b.rw || a.rh != b.rh) return false;
            if (memcmp(&a.hMin, &b.hMin, sizeof a.hMin) || memcmp(&a.hMax, &b.hMax, sizeof a.hMax) ||
                memcmp(&a.vMin, &b.vMin, sizeof a.vMin) || memcmp(&a.vMax, &b.vMax, sizeof a.vMax))
                return false;
            for (int s = 0; s < 4; s++) {
                if (!vecSame(a.h[s], b.h[s]) || !vecSame(a.v[s], b.v[s])) return false;
                const App::ProjState::Stats& x = a.hStat[s]; const App::ProjState::Stats& y = b.hStat[s];
                const App::ProjState::Stats& u = a.vStat[s]; const App::ProjState::Stats& w2 = b.vStat[s];
                if (memcmp(&x, &y, sizeof(double) * 6) || x.valid != y.valid) return false;
                if (memcmp(&u, &w2, sizeof(double) * 6) || u.valid != w2.valid) return false;
            }
            return true;
        };
        auto tempSame = [&](const App::TemporalState& a, const App::TemporalState& b) {
            return a.seqId == b.seqId && a.frames == b.frames && a.valid == b.valid &&
                   a.rx == b.rx && a.ry == b.ry && a.rw == b.rw && a.rh == b.rh &&
                   vecSame(a.idx, b.idx) && vecSame(a.frameMean, b.frameMean) &&
                   vecSame(a.frameStd, b.frameStd) &&
                   memcmp(&a.tempNoise, &b.tempNoise, sizeof a.tempNoise) == 0 &&
                   memcmp(&a.fixedPattern, &b.fixedPattern, sizeof a.fixedPattern) == 0 &&
                   memcmp(&a.totalNoise, &b.totalNoise, sizeof a.totalNoise) == 0;
        };

        int sidA = app.seqs[0].id, sidB = app.seqs[1].id;
        std::vector<int> frA = framesOfSeq(sidA), frB = framesOfSeq(sidB);
        selectImage(frA.front());

        // ---- A3 baseline: everything A, with compare OFF ----
        app.compareMode = App::CmpOff;
        abStatsFrame();
        recomputeHistogramIfNeeded(cur(), app.hist[0]);
        recomputeProjectionIfNeeded(cur(), app.proj[0]);
        recomputeTemporalIfNeeded(cur(), app.temporal[0]);
        App::HistState hOff = app.hist[0];
        App::ProjState pOff = app.proj[0];
        App::TemporalState tOff = app.temporal[0];
        fprintf(stderr, "abstatsselftest: compare OFF, A='%s' (stack %d): "
                        "hist sampled=%zu maxBin=%u mean0=%.9g, sigma_t=%.9g\n",
                cur()->name.c_str(), sidA, hOff.sampled, hOff.maxBin, hOff.mean[0],
                tOff.tempNoise[0]);
        check(app.hist[1].uid == 0 && app.proj[1].uid == 0 &&
              app.temporal[1].seqId == -1,
              "A0 compare off leaves slot 1 empty");

        // ---- A1: compare on, both slots fill ----
        // Explicitly on "B uses A's range". A3 below asserts that adding B does
        // not move A, and that is only the contract in this mode: under the
        // union default A's bin axis is SUPPOSED to widen to cover B - that is
        // the whole mode - and the union's own contract is checked in A3b.
        app.compareRangeMode = 1;
        setCompareB(app.images[frB.front()].get());
        app.compareMode = App::CmpSplit;
        app.compareFollowFrame = true;
        abStatsFrame();
        ImageDoc* B = cmpB();
        if (!B) { fprintf(stderr, "abstatsselftest: no B after setCompareB\n"); return 1; }
        recomputeHistogramIfNeeded(cur(), app.hist[0]);
        recomputeProjectionIfNeeded(cur(), app.proj[0]);
        recomputeTemporalIfNeeded(cur(), app.temporal[0]);
        recomputeHistogramIfNeeded(B, app.hist[1]);
        recomputeProjectionIfNeeded(B, app.proj[1]);
        recomputeTemporalIfNeeded(B, app.temporal[1]);
        fprintf(stderr, "abstatsselftest: compare SPLIT+follow, B='%s' (stack %d): "
                        "hist[0].uid=%llu (A=%llu)  hist[1].uid=%llu (B=%llu)  "
                        "proj[1].uid=%llu  temporal[1].seqId=%d frames=%d\n",
                B->name.c_str(), sidB,
                (unsigned long long)app.hist[0].uid, (unsigned long long)cur()->uid,
                (unsigned long long)app.hist[1].uid, (unsigned long long)B->uid,
                (unsigned long long)app.proj[1].uid,
                app.temporal[1].seqId, app.temporal[1].frames);
        check(app.hist[0].uid == cur()->uid && app.hist[1].uid == B->uid &&
              app.hist[0].uid != app.hist[1].uid, "A1 histogram: both slots, own uid");
        check(app.proj[0].uid == cur()->uid && app.proj[1].uid == B->uid,
              "A1 projection: both slots, own uid");
        check(app.temporal[0].seqId == sidA && app.temporal[1].seqId == sidB &&
              app.temporal[0].valid && app.temporal[1].valid,
              "A1 temporal: both slots, own stack");
        check(app.hist[1].img == B && app.proj[1].img == B,
              "A1 slot 1 names B, not A");

        // ---- A1p: a POI is a region of ONE PIXEL, and Temporal follows it ----
        // The user report (2026-08-12): "ROI で P をやった場合に Temporal が
        // ROI 選択のように切り替わらない。一画素の時間遷移を見たいときに困る。"
        // recomputeTemporalIfNeeded read `a->type == 0` and nothing else, so a
        // selected POI left the region at the whole frame: pressing P changed
        // no number on the panel and the trace of one pixel could not be seen
        // at all.
        //
        // The expected values are derived HERE, from the same frames, and the
        // derivation is the point: for a one-pixel region the per-frame mean IS
        // the pixel, with no averaging to hide an off-by-one - so an
        // implementation that quietly kept the whole frame produces a visibly
        // different series rather than a slightly different one.
        {
            const int PX = 5, PY = 3;             // inside every fixture frame
            const int savedSel = app.selectedAnn;
            addAnn(1, PX, PY, 0, 0);              // a POI, selected by addAnn
            App::TemporalState TP;
            recomputeTemporalIfNeeded(cur(), TP);
            // what the panel must be plotting: this pixel, frame by frame
            std::vector<double> want;
            for (int idx : framesOfSeq(sidA)) {
                const ImageDoc& fr = *app.images[idx];
                if (fr.px().empty()) continue;
                want.push_back(fr.px()[((size_t)PY * fr.w + PX) * fr.ch]);
            }
            // sigma_t of one pixel, ddof=1 - the same estimator the model uses
            // (#57 item 3), written out so this test does not inherit the bug
            // it is checking for.
            double mean = 0;
            for (double v : want) mean += v;
            mean /= (double)std::max<size_t>(1, want.size());
            double var = 0;
            for (double v : want) var += (v - mean) * (v - mean);
            var = want.size() > 1 ? var / (double)(want.size() - 1) : 0.0;
            const double wantSigT = sqrt(var);
            bool traceEq = TP.frameMean.size() == want.size();
            if (traceEq)
                for (size_t i = 0; i < want.size(); i++)
                    if (!relEq(TP.frameMean[i], want[i], 1e-6)) traceEq = false;
            fprintf(stderr, "abstatsselftest: A1p POI (%d,%d): region %dx%d at (%d,%d) "
                            "poi=%d roiUsed=%d, %zu point(s) (want %zu), trace equal=%d, "
                            "sigma_t=%.9g (want %.9g), splitValid=%d sigma_fpn=%.9g\n",
                    PX, PY, TP.rw, TP.rh, TP.rx, TP.ry, TP.poi ? 1 : 0,
                    TP.roiUsed ? 1 : 0, TP.frameMean.size(), want.size(),
                    traceEq ? 1 : 0, TP.tempNoise[0], wantSigT,
                    TP.splitValid ? 1 : 0, TP.fixedPattern[0]);
            check(TP.valid && TP.poi && TP.roiUsed && TP.rw == 1 && TP.rh == 1 &&
                  TP.rx == PX && TP.ry == PY,
                  "A1p a selected POI makes Temporal's region that one pixel");
            check(traceEq, "A1p ...and the series IS that pixel, frame by frame");
            check(relEq(TP.tempNoise[0], wantSigT, 1e-6),
                  "A1p ...with the pixel's own sigma_t (ddof=1)");
            // The other half, and the reason this is not just "accept type 1":
            // sigma_fpn is the SPATIAL spread of the time-averaged frame, so
            // one pixel has nothing for it to be a spread over. The arithmetic
            // would hand back a clamped 0, which reads as a finding about the
            // sensor. It is refused instead.
            check(!TP.splitValid && TP.fixedPattern[0] == 0 && TP.totalNoise[0] == 0,
                  "A1p one pixel has no fixed-pattern split, and does not fake one");
            // anti-vacuity: splitValid is not simply always false. The same
            // stack with a real rectangle still produces the split.
            deleteAnn(app.selectedAnn);
            addAnn(0, 0, 0, 8, 8);
            App::TemporalState TR;
            recomputeTemporalIfNeeded(cur(), TR);
            fprintf(stderr, "abstatsselftest: A1p 8x8 ROI: poi=%d splitValid=%d "
                            "region %dx%d\n",
                    TR.poi ? 1 : 0, TR.splitValid ? 1 : 0, TR.rw, TR.rh);
            check(TR.valid && !TR.poi && TR.splitValid && TR.rw == 8 && TR.rh == 8,
                  "A1p a rectangle still gets the split (the flag means something)");
            deleteAnn(app.selectedAnn);
            app.selectedAnn = savedSel;
        }

        // ---- A2: B's numbers recomputed here, from B's pixels ----
        {
            RefHist R = refHistogram(*B);
            const App::HistState& HB = app.hist[1];
            bool binsEq = R.nSeries == HB.nSeries &&
                          memcmp(R.bins, HB.bins, sizeof R.bins) == 0 &&
                          R.sampled == HB.sampled;
            bool statEq = true;
            for (int s = 0; s < R.nSeries; s++)
                if (!relEq(R.mean[s], HB.mean[s], 1e-6) || !relEq(R.sd[s], HB.sd[s], 1e-6))
                    statEq = false;
            RefSigT refSig = refSigmaT(*B);
            const App::TemporalState& TB = app.temporal[1];
            // every plane, not plane 0: on a mosaic the panel publishes four
            // sigma_t and a check that reads only the first leaves three
            // unmeasured
            bool sigEq = refSig.nPl == TB.nPl;
            for (int p = 0; p < refSig.nPl; p++)
                if (!relEq(refSig.sigT[p], TB.tempNoise[p], 1e-6)) sigEq = false;
            fprintf(stderr, "abstatsselftest: B independent recompute: series=%d "
                            "sampled=%zu/%zu  mean0 %.12g vs %.12g  sd0 %.12g vs %.12g  "
                            "planes %d vs %d  sigma_t %.12g vs %.12g",
                    R.nSeries, R.sampled, HB.sampled, R.mean[0], HB.mean[0],
                    R.sd[0], HB.sd[0], refSig.nPl, TB.nPl,
                    refSig.sigT[0], TB.tempNoise[0]);
            for (int p = 1; p < refSig.nPl; p++)
                fprintf(stderr, " | %.12g vs %.12g", refSig.sigT[p], TB.tempNoise[p]);
            fprintf(stderr, "\n");
            check(binsEq, "A2 B histogram bins match an independent pass exactly");
            check(statEq, "A2 B mean/sd match within 1e-6 relative");
            check(sigEq, "A2 B sigma_t matches within 1e-6 relative, every plane");
            // the two sides really are different data, or A2/A3 prove nothing
            check(memcmp(app.hist[0].bins, app.hist[1].bins, sizeof app.hist[0].bins) != 0,
                  "A2 fixture: A and B histograms actually differ");
        }

        // ---- A3: A did not move (on "B uses A's range") ----
        check(histSame(hOff, app.hist[0]), "A3 A histogram byte-identical with B present");
        check(projSame(pOff, app.proj[0]), "A3 A projection byte-identical with B present");
        check(tempSame(tOff, app.temporal[0]), "A3 A temporal byte-identical with B present");

        // ---- A3b: the union default bins BOTH sides on A and B together ----
        {
            app.compareRangeMode = 2;
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            recomputeHistogramIfNeeded(B, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            float uLo = std::min(cur()->vmin, B->vmin), uHi = std::max(cur()->vmax, B->vmax);
            bool axis = app.hist[0].black == uLo && app.hist[0].white == uHi;
            bool shared = app.hist[0].black == app.hist[1].black &&
                          app.hist[0].white == app.hist[1].white;
            fprintf(stderr, "abstatsselftest: union: A bins %.9g..%.9g, B bins %.9g..%.9g "
                            "| content A %.9g..%.9g, B %.9g..%.9g | A alone %.9g..%.9g\n",
                    app.hist[0].black, app.hist[0].white, app.hist[1].black, app.hist[1].white,
                    cur()->vmin, cur()->vmax, B->vmin, B->vmax, hOff.black, hOff.white);
            check(axis, "A3b union: A is binned on the A-and-B union, not on A alone");
            check(shared, "A3b union: both sides land on the same bin axis");
            // ...and it is a DISPLAY overlay: going back restores A exactly
            app.compareRangeMode = 1;
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            check(histSame(hOff, app.hist[0]),
                  "A3b back on A's range: A is byte-identical to compare-off again");
        }

        // ---- A4: five frame steps, both keys follow; temporal does not re-run ----
        {
            App::TemporalState tB0 = app.temporal[1];
            uint64_t bUid0 = B->uid;
            bool keysFollow = true, temporalMoved = false, bMoved = false;
            for (int k = 0; k < 5; k++) {
                gotoFrame(1);
                abStatsFrame();
                ImageDoc* b2 = cmpB();
                if (!b2) { keysFollow = false; break; }
                recomputeHistogramIfNeeded(cur(), app.hist[0]);
                recomputeHistogramIfNeeded(b2, app.hist[1]);
                recomputeTemporalIfNeeded(cur(), app.temporal[0]);
                recomputeTemporalIfNeeded(b2, app.temporal[1]);
                if (app.hist[0].uid != cur()->uid || app.hist[1].uid != b2->uid)
                    keysFollow = false;
                if (b2->uid != bUid0) bMoved = true;
                if (!tempSame(tB0, app.temporal[1])) temporalMoved = true;
            }
            fprintf(stderr, "abstatsselftest: 5x gotoFrame(1): A frame=%d, B frame=%d, "
                            "B uid %llu->%llu, temporal[1].seqId=%d frames=%d\n",
                    cur()->seqIndex, cmpB() ? cmpB()->seqIndex : -1,
                    (unsigned long long)bUid0,
                    (unsigned long long)(cmpB() ? cmpB()->uid : 0),
                    app.temporal[1].seqId, app.temporal[1].frames);
            check(keysFollow, "A4 both hist slots follow the frame step");
            check(bMoved && cmpB() && cmpB()->seqIndex == cur()->seqIndex,
                  "A4 follow-frame put B on A's frame number");
            check(!temporalMoved && app.temporal[1].seqId == sidB,
                  "A4 temporal[1] unchanged by a frame step (keyed on the stack)");
        }

        // ---- P1: the histogram's own rules ----
        {
            ImageDoc* b4 = cmpB();
            // B gets a deliberately DIFFERENT display range; the bins must still
            // be A's, or the two curves share an x axis they were not binned on
            float bBlack0 = b4->black, bWhite0 = b4->white;
            b4->black = bBlack0 - 137.5f; b4->white = bWhite0 + 211.25f;
            recomputeHistogramIfNeeded(b4, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            fprintf(stderr, "abstatsselftest: P1 bin axis: A black/white %.6g/%.6g, "
                            "B's own %.6g/%.6g, hist[1] binned on %.6g/%.6g\n",
                    effBlack(*cur()), effWhite(*cur()), effBlack(*b4), effWhite(*b4),
                    app.hist[1].black, app.hist[1].white);
            // b4->black/white, NOT effBlack: since the A/B range modes landed,
            // effBlack(B) deliberately returns A's range, so asking it whether
            // B "differs" answers a different question and always says no.
            check(b4->black != cur()->black || b4->white != cur()->white,
                  "P1 fixture: B's own range really differs");
            check(app.hist[1].black == effBlack(*cur()) &&
                  app.hist[1].white == effWhite(*cur()),
                  "P1 B is binned on A's black/white, not its own");
            b4->black = bBlack0; b4->white = bWhite0;
            recomputeHistogramIfNeeded(b4, app.hist[1], effBlack(*cur()), effWhite(*cur()));
        }
        {   // the plane selector is presentation only: it must not touch a bin
            App::HistState hA = app.hist[0], hB = app.hist[1];
            int keep = app.histPlane;
            bool same = true;
            for (int p = -1; p < 4; p++) {
                app.histPlane = p;
                recomputeHistogramIfNeeded(cur(), app.hist[0]);
                recomputeHistogramIfNeeded(cmpB(), app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
                if (!histSame(hA, app.hist[0]) || !histSame(hB, app.hist[1])) same = false;
            }
            app.histPlane = keep;
            check(same, "P1 plane selector changes nothing that was measured");
        }
        {   // Auto mirrors the image; an explicit choice overrides it both ways
            int keep = app.abStatsLayout;
            app.abStatsLayout = App::AbAuto;
            app.compareMode = App::CmpSplit; bool autoSplit = abSideBySide();
            app.compareMode = App::CmpWipe;  bool autoWipe = abSideBySide();
            app.abStatsLayout = App::AbSide; bool forcedSide = abSideBySide();
            app.abStatsLayout = App::AbOverlay;
            app.compareMode = App::CmpSplit; bool forcedOver = abSideBySide();
            fprintf(stderr, "abstatsselftest: P1 layout: auto/split=%d auto/wipe=%d "
                            "forced-side/wipe=%d forced-overlay/split=%d\n",
                    autoSplit, autoWipe, forcedSide, forcedOver);
            check(autoSplit && !autoWipe && forcedSide && !forcedOver,
                  "P1 Auto follows the image layout, explicit overrides it");
            app.abStatsLayout = keep;
            app.compareMode = App::CmpSplit;
        }

        // ---- P2: a B that does not share A's axis must never be overlaid ----
        {
            check(abProjOverlayable(app.proj[0], app.proj[1]),
                  "P2 same-size B shares A's profile axis");
            std::string root = g_abstatsSelftest;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            size_t sl = root.find_last_of('/');
            root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
            size_t before = app.images.size();
            openPath(root + "/grad_u16.npy");           // 640x480 against A's 80x64
            loadAll();
            if (app.images.size() > before) {
                ImageDoc* odd = app.images.back().get();
                selectImage(frA.front());
                setCompareB(odd);
                app.compareMode = App::CmpSplit;
                abStatsFrame();
                ImageDoc* b5 = cmpB();
                recomputeProjectionIfNeeded(cur(), app.proj[0]);
                recomputeProjectionIfNeeded(b5, app.proj[1]);
                bool over = abProjOverlayable(app.proj[0], app.proj[1]);
                fprintf(stderr, "abstatsselftest: P2 odd B '%s' %dx%d vs A %dx%d: "
                                "profile A x %d..%d / B x %d..%d, overlayable=%d\n",
                        b5->name.c_str(), b5->w, b5->h, cur()->w, cur()->h,
                        app.proj[0].rx, app.proj[0].rx + app.proj[0].rw - 1,
                        app.proj[1].rx, app.proj[1].rx + app.proj[1].rw - 1, over ? 1 : 0);
                check(b5->w != cur()->w || b5->h != cur()->h,
                      "P2 fixture: the odd B really is a different size");
                check(!over, "P2 size-mismatched B is refused the overlay");
                // and the channel counts differ too, which P3's delta columns use
                check(app.proj[0].nSeries != app.proj[1].nSeries ||
                      cur()->ch == b5->ch, "P2 series count reflects the channel count");
                selectImage((int)app.images.size() - 1);  // the extra image ITSELF
                closeCurrent(true);                       // ...and only that one
                selectImage(frA.front());
                setCompareB(app.images[framesOfSeq(sidB).front()].get());
                abStatsFrame();
            } else {
                fprintf(stderr, "abstatsselftest: P2 skipped (%s/grad_u16.npy not there)\n",
                        root.c_str());
            }
        }

        // ---- P2b: held-down stepping holds B's slots instead of recomputing ----
        {
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            app.abStepBusyUntil = 0;
            abStatsFrame();
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            recomputeHistogramIfNeeded(cmpB(), app.hist[1],
                                       effBlack(*cur()), effWhite(*cur()));
            uint64_t held = app.hist[1].uid;
            gotoFrame(1); gotoFrame(1);            // two switches inside 300 ms
            bool armed = abStepBusy();
            ImageDoc* b6 = cmpB();
            // exactly what the panel does
            if (b6 && (!abStepBusy() || app.hist[1].uid == 0))
                recomputeHistogramIfNeeded(b6, app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
            bool heldStale = b6 && app.hist[1].uid == held && app.hist[1].uid != b6->uid;
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            bool released = !abStepBusy();
            if (b6 && (!abStepBusy() || app.hist[1].uid == 0))
                recomputeHistogramIfNeeded(b6, app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
            fprintf(stderr, "abstatsselftest: P2b throttle: armed=%d held uid=%llu vs B "
                            "uid=%llu, released=%d, after release uid=%llu\n",
                    armed ? 1 : 0, (unsigned long long)held,
                    (unsigned long long)(b6 ? b6->uid : 0), released ? 1 : 0,
                    (unsigned long long)app.hist[1].uid);
            check(armed, "P2b rapid stepping arms the B throttle");
            check(heldStale, "P2b B slot holds its last result while stepping (stale)");
            check(released && b6 && app.hist[1].uid == b6->uid,
                  "P2b B refreshes once the stepping stops");
            selectImage(frA.front());
            app.abStepBusyUntil = 0;
        }

        // ---- P2c: a PINNED B goes stale when the RANGE moves, not only when
        // B does ----
        // The union default re-fits the bin range to the two current frames, so
        // stepping A moves it every frame. B's recompute is skipped while the
        // throttle is armed and its bins are drawn positionally on the new axis;
        // keyed on uid alone the [stale] tag could not see that, and a pinned B
        // (Shift+B) never changes uid at all.
        {
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            bool saveFollow = app.compareFollowFrame;
            int saveShare = app.compareRangeMode;
            app.compareFollowFrame = false;        // PINNED: B's uid never moves
            app.compareRangeMode = 2;              // the union default
            app.abStepBusyUntil = 0;
            abStatsFrame();
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            recomputeHistogramIfNeeded(cmpB(), app.hist[1], effBlack(*cur()), effWhite(*cur()));
            bool freshFirst = !abHistSideStale(cur(), cmpB(), app.hist[1]);
            float binned0 = app.hist[1].black, binned1 = app.hist[1].white;
            gotoFrame(1); gotoFrame(1);            // two switches inside 300 ms
            ImageDoc* Bp = cmpB();
            if (Bp && (!abStepBusy() || app.hist[1].uid == 0))   // exactly the panel
                recomputeHistogramIfNeeded(Bp, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            bool uidUnchanged = Bp && app.hist[1].uid == Bp->uid;
            bool rangeMoved = app.hist[1].black != effBlack(*cur()) ||
                              app.hist[1].white != effWhite(*cur());
            bool flagged = abHistSideStale(cur(), Bp, app.hist[1]);
            fprintf(stderr, "abstatsselftest: P2c pinned B: binned on %.6g..%.6g, axis now "
                            "%.6g..%.6g, B uid unchanged=%d, stale=%d\n",
                    binned0, binned1, effBlack(*cur()), effWhite(*cur()),
                    uidUnchanged ? 1 : 0, flagged ? 1 : 0);
            check(freshFirst, "P2c a freshly binned B is not stale");
            check(rangeMoved && uidUnchanged,
                  "P2c fixture: stepping A moves the union while B's uid does not");
            check(flagged, "P2c a pinned B binned on the OLD union reads as stale");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            if (Bp && (!abStepBusy() || app.hist[1].uid == 0))
                recomputeHistogramIfNeeded(Bp, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            check(!abHistSideStale(cur(), Bp, app.hist[1]),
                  "P2c ...and is fresh again once the stepping stops");
            app.compareFollowFrame = saveFollow;
            app.compareRangeMode = saveShare;
            selectImage(frA.front());
            app.abStepBusyUntil = 0;
        }

        // ---- P3: the A|B|delta|delta% table's inputs ----
        {
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            abStatsFrame();
            ImageDoc* b7 = cmpB();
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            recomputeTemporalIfNeeded(b7, app.temporal[1]);
            AbTemporal TA = abTemporalOf(cur(), app.temporal[0], app.srvTemporal);
            AbTemporal TB = abTemporalOf(b7, app.temporal[1], app.srvTemporalB);
            double dAbs = TA.sigT[0] - TB.sigT[0];
            double dPct = TA.sigT[0] != 0 ? (TA.sigT[0] - TB.sigT[0]) / fabs(TA.sigT[0]) * 100.0 : 0.0;
            fprintf(stderr, "abstatsselftest: P3 table inputs: A sigma_t=%.9g (n=%d/%d, "
                            "server=%d), B sigma_t=%.9g (n=%d/%d, server=%d), "
                            "delta A-B=%.9g, delta=%.6g %%, unit=[%s]\n",
                    TA.sigT[0], TA.frames, TA.expected, TA.fromServer ? 1 : 0,
                    TB.sigT[0], TB.frames, TB.expected, TB.fromServer ? 1 : 0,
                    dAbs, dPct, abValueUnit(cur()->dtype).c_str());
            check(TA.valid && TB.valid && TA.isStack && TB.isStack,
                  "P3 both sides resolve to stack temporal numbers");
            check(!TA.fromServer && !TB.fromServer,
                  "P3 local stacks come from the local computation");
            check(dAbs == TA.sigT[0] - TB.sigT[0] && !TA.fromServer,
                  "P3 delta is A - B (the difference image's sign)");
            {   // REGION. The server aggregate is unconditionally whole-frame
                // (requestServerTemporal puts no rect on the wire, and mWorker
                // only sends one when rw > 0), while the local side honours the
                // selected ROI. The panel used to label BOTH with A's local
                // roiUsed flag and then subtract them. Each side must now carry
                // the region it actually measured, and a mismatch must blank
                // the delta the same way a channel-count mismatch does.
                App::ServerTemporal fake;      // as requestServerTemporal fills it
                fake.valid = true;
                fake.seqId = cur()->seqId;
                fake.frames = TA.frames;
                fake.nPl = app.temporal[0].nPl;
                fake.rw = fake.rh = 0;
                fake.roiUsed = false;          // whole frame, always
                AbTemporal SA = abTemporalOf(cur(), app.temporal[0], fake);
                App::TemporalState roiT = app.temporal[0];
                roiT.roiUsed = true;           // a local side restricted to an ROI
                roiT.rx = 1; roiT.ry = 1; roiT.rw = 8; roiT.rh = 8;
                AbTemporal RB = abTemporalOf(b7, roiT, app.srvTemporalB);
                roiT.seqId = b7->seqId;
                RB = abTemporalOf(b7, roiT, app.srvTemporalB);
                bool mixed = SA.fromServer && !SA.roiUsed && RB.roiUsed;
                fprintf(stderr, "abstatsselftest: P3b region: server A roiUsed=%d "
                                "(rw=%d), local B roiUsed=%d (rw=%d), same region=%d\n",
                        SA.roiUsed ? 1 : 0, SA.rw, RB.roiUsed ? 1 : 0, RB.rw,
                        (SA.roiUsed == RB.roiUsed && SA.rw == RB.rw) ? 1 : 0);
                check(mixed && !(SA.roiUsed == RB.roiUsed && SA.rw == RB.rw),
                      "P3b a server whole-frame A and an ROI B are not one region");
                check(!TA.roiUsed && TA.rw == 0 && !TB.roiUsed,
                      "P3b with no ROI both sides say whole image");
            }
            check(abDeltaMeaningful(cur(), b7), "P3 equal channel counts allow a delta");
            // B's server aggregate must never have been fired on its own
            check(app.srvTemporalB.token == 0 && !app.srvTemporalB.pending &&
                  app.srvTemporalB.seqId == -1,
                  "P3 B's server temporal is never fired automatically");
        }
        {   // a B that is not a stack, and a B with a different channel count
            std::string root = g_abstatsSelftest;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            size_t sl = root.find_last_of('/');
            root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
            size_t before = app.images.size();
            openPath(root + "/rgb_u8.npy");            // 3 channels, single frame
            loadAll();
            if (app.images.size() > before) {
                ImageDoc* lone = app.images.back().get();
                selectImage(frA.front());
                setCompareB(lone);
                abStatsFrame();
                ImageDoc* b8 = cmpB();
                recomputeTemporalIfNeeded(b8, app.temporal[1]);
                AbTemporal TB2 = abTemporalOf(b8, app.temporal[1], app.srvTemporalB);
                fprintf(stderr, "abstatsselftest: P3 lone B '%s' %dch: isStack=%d valid=%d "
                                "note='%s', delta meaningful=%d (A %dch)\n",
                        b8->name.c_str(), b8->ch, TB2.isStack ? 1 : 0, TB2.valid ? 1 : 0,
                        TB2.note, abDeltaMeaningful(cur(), b8) ? 1 : 0, cur()->ch);
                check(!TB2.isStack && !TB2.valid && std::string(TB2.note) == "not a stack",
                      "P3 a B that is not a stack says so instead of a number");
                check(cur()->ch != b8->ch && !abDeltaMeaningful(cur(), b8),
                      "P3 different channel counts suppress the delta columns");
                selectImage((int)app.images.size() - 1);  // the extra image ITSELF
                closeCurrent(true);                       // ...and only that one
                selectImage(frA.front());
                setCompareB(app.images[framesOfSeq(sidB).front()].get());
                abStatsFrame();
            } else {
                fprintf(stderr, "abstatsselftest: P3 lone-B checks skipped (%s/rgb_u8.npy "
                                "not there)\n", root.c_str());
            }
        }

        // ---- T: the Temporal panel's CHART, laid out for real ----
        // The report this group exists for is "with A and B on, Temporal shows
        // the numbers and no chart". Nothing that only calls the recompute
        // functions can see it: the numbers were right, the curve data was
        // right, and the chart was drawn - past the bottom edge of the panel,
        // because ImGuiTableFlags_ScrollX puts the A|B table in a child window
        // and a child-window table with the default outer_size takes ALL the
        // height the panel has left. So this group drives REAL ImGui frames and
        // asserts on the layout the panel records (g_tchart). It has to be
        // state, not pixels: this machine's screen capture returns a white
        // client area for OpenGL content, and glReadPixels of a 1600x1000
        // window is not a regression test anyone will keep running.
        // ...and it is one of THREE groups here that do - this one, S4-S6 and N -
        // against seven that are CPU, so selftest.abstats-nogl runs those seven
        // on every runner (板 304). A1/A2/A3/A1p are in them, and A2's per-plane
        // sigma_t against refSigmaT is the only parity that kernel has.
        if (!glGroup("--abstats-selftest (the Temporal chart group)")) return 1;
        if (g_haveWindow) {
            auto temporalFrame = [&](float w, float h) {
                // two passes: the first settles the window, the second is the
                // one whose layout is measured
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                    ImGui::Begin("TemporalProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    drawPanelTemporal();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            auto ptsOf = [&](char letter) {
                for (int i = 0; i < g_tchart.nSlots; i++)
                    if (g_tchart.slot[i] == letter) return g_tchart.pts[i];
                return -1;
            };
            auto saidNoAxis = [&](char letter) {
                for (int i = 0; i < g_tchart.nNoAxis; i++)
                    if (g_tchart.noAxisSlot[i] == letter) return true;
                return false;
            };
            const int saveLayout = app.abStatsLayout, saveMode = app.compareMode;

            // T1: two stacks, overlaid. Both curves on one axis, and the plot
            // INSIDE the panel - the check the reported symptom needed.
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            app.abStatsLayout = App::AbOverlay;
            abStatsFrame();
            temporalFrame(560.0f, 440.0f);
            fprintf(stderr, "abstatsselftest: T1 overlay 560x440: ran=%d plotted=%d "
                            "slots=%d A=%d B=%d plot y %.1f..%.1f (window bottom %.1f) "
                            "avail before chart=%.1f\n",
                    g_tchart.ran ? 1 : 0, g_tchart.plotted ? 1 : 0, g_tchart.nSlots,
                    ptsOf('A'), ptsOf('B'), g_tchart.plotTop, g_tchart.plotBot,
                    g_tchart.winBot, g_tchart.availBefore);
            check(g_tchart.ran && g_tchart.plotted,
                  "T1 A and B both stacks: the chart is drawn");
            check(ptsOf('A') >= 2 && ptsOf('B') >= 2,
                  "T1 ...with data for BOTH sides on it");
            check(temporalChartVisible(),
                  "T1 ...and it lands inside the panel, not below it");
            check(g_tchart.nNoAxis == 0, "T1 ...and neither side is reported axis-less");

            // T1b: the same pair side by side. Two panels, both with a curve.
            // The width is derived from AB_MIN_SIDE and the display scale, not
            // typed in: below that threshold the layout is SUPPOSED to fall
            // back to an overlay, and a hard-coded width would make this check
            // a test of the machine's DPI setting.
            app.abStatsLayout = App::AbSide;
            const float sideW = AB_MIN_SIDE * app.uiScale * 2 + 120.0f;
            temporalFrame(sideW, 440.0f);
            fprintf(stderr, "abstatsselftest: T1b side-by-side %.0fx440 (uiScale %.2f): "
                            "side=%d A=%d B=%d plot bottom=%.1f window bottom=%.1f\n",
                    sideW, app.uiScale, g_tchart.sideBySide ? 1 : 0, ptsOf('A'),
                    ptsOf('B'), g_tchart.plotBot, g_tchart.winBot);
            check(g_tchart.sideBySide && ptsOf('A') >= 2 && ptsOf('B') >= 2,
                  "T1b side by side: each side gets its own curve");
            check(temporalChartVisible(), "T1b ...and both plots fit in the panel");
            app.abStatsLayout = App::AbOverlay;

            // T2: B is a lone frame. A's chart must survive, and the panel must
            // SAY that B has no time axis - drawing nothing was the old answer
            // to half of this, and saying nothing to the other half.
            {
                std::string root = g_abstatsSelftest;
                std::replace(root.begin(), root.end(), '\\', '/');
                while (!root.empty() && root.back() == '/') root.pop_back();
                size_t sl = root.find_last_of('/');
                root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
                size_t before = app.images.size();
                openPath(root + "/rgb_u8.npy");
                loadAll();
                if (app.images.size() > before) {
                    ImageDoc* lone = app.images.back().get();
                    selectImage(frA.front());
                    setCompareB(lone);
                    abStatsFrame();
                    temporalFrame(560.0f, 440.0f);
                    fprintf(stderr, "abstatsselftest: T2 lone-frame B: plotted=%d A=%d "
                                    "B=%d noAxis=%d ('%c')\n",
                            g_tchart.plotted ? 1 : 0, ptsOf('A'), ptsOf('B'),
                            g_tchart.nNoAxis,
                            g_tchart.nNoAxis ? g_tchart.noAxisSlot[0] : '-');
                    check(g_tchart.plotted && ptsOf('A') >= 2,
                          "T2 B a lone frame: A's chart is still drawn");
                    check(ptsOf('B') == 0 && saidNoAxis('B'),
                          "T2 ...and the panel says B has no time axis");
                    check(temporalChartVisible(), "T2 ...and A's plot fits in the panel");

                    // T2b: the mirror image - A is the lone frame, B the stack.
                    // This used to leave the panel completely empty: the AB
                    // branch was gated on A being a stack, and the fallback
                    // wanted a stack too.
                    int loneIdx = -1;
                    for (int i = 0; i < (int)app.images.size(); i++)
                        if (app.images[i].get() == lone) loneIdx = i;
                    if (loneIdx >= 0) {
                        selectImage(loneIdx);
                        setCompareB(app.images[framesOfSeq(sidB).front()].get());
                        abStatsFrame();
                        temporalFrame(560.0f, 440.0f);
                        fprintf(stderr, "abstatsselftest: T2b lone-frame A: plotted=%d "
                                        "A=%d B=%d noAxis=%d\n",
                                g_tchart.plotted ? 1 : 0, ptsOf('A'), ptsOf('B'),
                                g_tchart.nNoAxis);
                        check(g_tchart.plotted && ptsOf('B') >= 2,
                              "T2b A a lone frame: B's chart is drawn anyway");
                        check(ptsOf('A') == 0 && saidNoAxis('A'),
                              "T2b ...and the panel says A has no time axis");
                    }
                    // T6: NEITHER side has a time axis. There is no chart to
                    // draw - and the panel has to SAY that, for both sides.
                    // Leaving the space a chart used to occupy blank is the
                    // defect, not the absence of the chart.
                    size_t before2 = app.images.size();
                    openPath(root + "/grad_u16.npy");
                    loadAll();
                    if (app.images.size() > before2) {
                        ImageDoc* lone2 = app.images.back().get();
                        int li = -1;
                        for (int i = 0; i < (int)app.images.size(); i++)
                            if (app.images[i].get() == lone) li = i;
                        if (li >= 0) {
                            selectImage(li);
                            setCompareB(lone2);
                            abStatsFrame();
                            temporalFrame(560.0f, 440.0f);
                            fprintf(stderr, "abstatsselftest: T6 two lone frames: ran=%d "
                                            "plotted=%d noAxis=%d\n", g_tchart.ran ? 1 : 0,
                                    g_tchart.plotted ? 1 : 0, g_tchart.nNoAxis);
                            check(g_tchart.ran && !g_tchart.plotted &&
                                  g_tchart.nNoAxis == 2 && saidNoAxis('A') && saidNoAxis('B'),
                                  "T6 no time axis on either side: both say why, no chart");
                        }
                        selectImage((int)app.images.size() - 1);   // lone2, and only it
                        closeCurrent(true);
                    }
                    selectImage((int)app.images.size() - 1);   // the lone frame ITSELF
                    closeCurrent(true);                        // ...and only that one
                } else {
                    fprintf(stderr, "abstatsselftest: T2 skipped (%s/rgb_u8.npy not "
                                    "there)\n", root.c_str());
                }
            }

            // T3: a third stack in compare slot C gets a curve of its own.
            if (app.seqs.size() >= 3) {
                std::vector<int> frC = framesOfSeq(app.seqs[2].id);
                selectImage(frA.front());
                setCompareB(app.images[framesOfSeq(sidB).front()].get());
                app.cmpExtra.clear();
                if (!frC.empty()) addCompareSlot(app.images[frC.front()].get());
                abStatsFrame();
                temporalFrame(560.0f, 440.0f);
                fprintf(stderr, "abstatsselftest: T3 slot C: slots=%d A=%d B=%d C=%d\n",
                        g_tchart.nSlots, ptsOf('A'), ptsOf('B'), ptsOf('C'));
                check(g_tchart.nSlots == 3 && ptsOf('C') >= 2,
                      "T3 an extra compare slot gets its own curve");
                check(ptsOf('A') >= 2 && ptsOf('B') >= 2,
                      "T3 ...without costing A or B theirs");
                app.cmpExtra.clear();
            }

            // T5: a SHORT panel. The table may not have room for all its rows
            // now, and the rule is that the chart keeps its floor and the table
            // scrolls - not the other way round, which is how the chart got
            // pushed off the panel in the first place.
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            app.abStatsLayout = App::AbOverlay;
            abStatsFrame();
            temporalFrame(560.0f, 300.0f);
            fprintf(stderr, "abstatsselftest: T5 short panel 560x300: plotted=%d A=%d "
                            "B=%d plot y %.1f..%.1f window bottom=%.1f\n",
                    g_tchart.plotted ? 1 : 0, ptsOf('A'), ptsOf('B'),
                    g_tchart.plotTop, g_tchart.plotBot, g_tchart.winBot);
            check(g_tchart.plotted && ptsOf('A') >= 2 && ptsOf('B') >= 2,
                  "T5 a short panel keeps both curves");
            check(temporalChartVisible(),
                  "T5 ...and the chart, not the table, keeps the room");

            // T4: compare off - the one-sided panel still charts, and still
            // fits. The A/B fix must not have moved the plain case.
            app.compareMode = App::CmpOff;
            abStatsFrame();
            temporalFrame(560.0f, 440.0f);
            fprintf(stderr, "abstatsselftest: T4 compare off: plotted=%d A=%d "
                            "plot bottom=%.1f window bottom=%.1f\n",
                    g_tchart.plotted ? 1 : 0, ptsOf('A'), g_tchart.plotBot,
                    g_tchart.winBot);
            check(g_tchart.plotted && ptsOf('A') >= 2 && temporalChartVisible(),
                  "T4 compare off: the one-sided chart is drawn and fits");

            app.abStatsLayout = saveLayout;
            app.compareMode = saveMode;
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            abStatsFrame();
        }

        // ---- A6: compare off invalidates slot 1 once; closing B leaves nothing ----
        app.compareMode = App::CmpOff;
        abStatsFrame();
        fprintf(stderr, "abstatsselftest: CmpOff: hist[1].uid=%llu img=%p, "
                        "proj[1].uid=%llu img=%p, temporal[1].seqId=%d, slot1Live=%d\n",
                (unsigned long long)app.hist[1].uid, (const void*)app.hist[1].img,
                (unsigned long long)app.proj[1].uid, (const void*)app.proj[1].img,
                app.temporal[1].seqId, app.abSlot1Live ? 1 : 0);
        check(app.hist[1].uid == 0 && app.hist[1].img == nullptr &&
              app.proj[1].uid == 0 && app.proj[1].img == nullptr &&
              app.temporal[1].seqId == -1 && !app.abSlot1Live,
              "A6 CmpOff invalidates slot 1");
        {   // and once only: a second frame with compare off must not touch it
            app.hist[1].uid = 12345;              // white-box tripwire
            abStatsFrame();
            check(app.hist[1].uid == 12345, "A6 compare-off frames after the first do nothing");
            app.hist[1] = App::HistState{};
        }
        {   // B closed while slot 1 held it: no dangling ImageDoc* may survive
            app.compareMode = App::CmpSplit;
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            abStatsFrame();
            ImageDoc* b3 = cmpB();
            recomputeHistogramIfNeeded(b3, app.hist[1]);
            recomputeProjectionIfNeeded(b3, app.proj[1]);
            const void* held = app.hist[1].img;
            closeStack(sidB);
            fprintf(stderr, "abstatsselftest: closeStack(B=%d): slot 1 held %p, now "
                            "hist[1].img=%p uid=%llu, proj[1].img=%p, cmpB()=%p\n",
                    sidB, held, (const void*)app.hist[1].img,
                    (unsigned long long)app.hist[1].uid,
                    (const void*)app.proj[1].img, (const void*)cmpB());
            check(held != nullptr, "A6 fixture: slot 1 really held B");
            check(app.hist[1].img == nullptr && app.hist[1].uid == 0 &&
                  app.proj[1].img == nullptr && app.proj[1].uid == 0 && !cmpB(),
                  "A6 closing B leaves no dangling pointer in slot 1");
        }

        // ---- A7: the Files row's A/B item is never a dead grey line ----------
        // Reported from the shipped build: "Set as compare B" is greyed out.
        // It was greyed on exactly one row - the CURRENT one, which is the row
        // you right-click first - and nothing said so. The rule now: EVERY row
        // offers "Set as compare B" and the slot item, the current one
        // included. That last row was still withheld until 2026-08-04, on the
        // reasoning that it IS A; the user threw that reasoning out (「比較の際
        // に同じものを選んだ時に，同じことを確認できる方がよい」), so setting
        // this row as B is a supported move and the menu has to offer it.
        // (The "Swap A and B" line that used to sit here is still gone - a
        // different ruling, 2026-07-30, and not the one being reversed.)
        {   // A6 closed one stack; multi/ has three, so two are still open
            check(app.seqs.size() >= 2, "A7 fixture: two stacks still open");
            std::vector<int> fa, fb;
            if (app.seqs.size() >= 2) {
                fa = framesOfSeq(app.seqs[0].id);
                fb = framesOfSeq(app.seqs[1].id);
            }
            check(!fa.empty() && !fb.empty(), "A7 fixture: two stacks with frames");
            if (!fa.empty() && !fb.empty()) {
                selectImage(fa.front());
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                fprintf(stderr, "abstatsselftest: A7 compare off: A-row=%d other-row=%d\n",
                        (int)abRowItem(app.images[fa.front()].get()),
                        (int)abRowItem(app.images[fb.front()].get()));
                check(abRowItem(app.images[fb.front()].get()) == AbSetB,
                      "A7 another row offers Set as compare B");
                // The current row offers it too, in every mode: putting the
                // document you are looking at on the B side is how you confirm
                // two sides agree, and it used to be the one row you could not
                // do it from.
                check(abRowItem(app.images[fa.front()].get()) == AbSetB,
                      "A7 compare off: the current row offers Set as compare B");
                // ...and turning compare on changes NOTHING about another row:
                // the user read the greying as "compare has to be armed first".
                app.compareMode = App::CmpWipe;
                ensureCompareB();
                check(abRowItem(app.images[fb.front()].get()) == AbSetB,
                      "A7 compare on: another row still offers Set as compare B");
                check(abRowItem(app.images[fa.front()].get()) == AbSetB,
                      "A7 compare on: the current row offers it here too");
                // every other frame of A's own stack is still a legal B
                if (fa.size() > 1)
                    check(abRowItem(app.images[fa[1]].get()) == AbSetB,
                          "A7 a different frame of A's stack can still be B");
                // and the swap really moves A, which is what makes the row
                // right-clickable as B a moment later. B is armed EXPLICITLY
                // for it: since board 270 a default B with nothing remembered
                // is A ITSELF, and swapping a document with itself moves
                // nothing - correct arithmetic, and no longer a fixture for
                // "the swap moves A". WHICH document is B was never this
                // check's subject, and leaving it to the ladder made the check
                // depend on what earlier groups had left in lastCompareBUid.
                setCompareB(app.images[fb.front()].get());
                ImageDoc* wasA = cur();
                swapCompare();
                check(cur() != wasA && abRowItem(wasA) == AbSetB,
                      "A7 after the swap the old A is a Set-as-B row again");
                // A8: B is a PIN. Walking A onto it used to swap the two, so
                // looking at B stopped it from being B and rewrote the pin to
                // whatever A had been. The pin holding is still the subject
                // here - but the comparison no longer goes QUIET while A stands
                // on it. That silence was the "kindness" the user removed on
                // 2026-08-04: B resolves to the document A is on, both sides
                // draw, and the panels show them coinciding. The assertion
                // below therefore reads the opposite of what it used to.
                {
                    uint64_t pin = app.compareBUid;
                    int bIdx = -1;
                    for (int i = 0; i < (int)app.images.size(); i++)
                        if (app.images[i]->uid == pin) bIdx = i;
                    int away = -1;
                    for (int i = 0; i < (int)app.images.size(); i++)
                        if (app.images[i]->uid != pin && app.images[i]->seqId != 0) { away = i; break; }
                    if (bIdx >= 0 && away >= 0) {
                        selectImage(bIdx);                 // stand on B
                        bool kept = app.compareBUid == pin;
                        bool live = cmpB() == cur() && abStatsB() == cur();
                        selectImage(away);                 // and step off it
                        bool back = app.compareBUid == pin && cmpB() &&
                                    cmpB()->uid == pin;
                        fprintf(stderr, "abstatsselftest: A8 stand on B: pin kept=%d "
                                        "B resolves to A=%d; step away: B is back=%d\n",
                                kept ? 1 : 0, live ? 1 : 0, back ? 1 : 0);
                        check(kept, "A8 selecting B does not rewrite the pin");
                        check(live, "A8 ...and A standing on it leaves a real B, not silence");
                        check(back, "A8 ...and B is still B when A steps away");
                    }
                }
                // A9: turning compare off and on again returns to the SAME
                // pair. ensureCompareB used to reach for the previously VIEWED
                // image, which after a bit of navigating is rarely the B you
                // chose - so the comparison you were working with quietly
                // became a different one.
                {
                    uint64_t chosen = app.compareBUid;
                    if (chosen) {
                        app.compareMode = App::CmpOff;
                        setCompareB(nullptr);                 // as switching off does
                        selectImage(app.current);             // ...and navigate a little
                        ensureCompareB();
                        // ...and read it with compare ON: cmpB() answers null
                        // whenever the mode is off, whatever B holds
                        app.compareMode = App::CmpWipe;
                        ImageDoc* back2 = cmpB();
                        fprintf(stderr, "abstatsselftest: A9 off then on: chose #%llu, came back #%llu\n",
                                (unsigned long long)chosen,
                                (unsigned long long)(back2 ? back2->uid : 0));
                        check(back2 && back2->uid == chosen,
                              "A9 compare off and on returns to the same B");
                    }
                }

                // A9b: the FIRST compare, with no remembered B at all (board
                // 270). The user's rule has two clauses - 「Bが前のやつあったら
                // それ，無かったら選択のやつ」- and only the first was
                // implemented. "Otherwise" meant the image NEXT to A in the
                // list: a partner nobody picked, and with a single image open,
                // no partner at all and an empty compare pane.
                {
                    const uint64_t keepLast = app.lastCompareBUid;
                    const uint64_t keepPrev = app.prevImageUid;
                    const std::vector<uint64_t> keepExtra = app.cmpExtra;
                    app.compareMode = App::CmpOff;
                    setCompareB(nullptr);
                    app.cmpExtra.clear();
                    app.lastCompareBUid = 0;     // no history: a first compare
                    app.prevImageUid = 0;
                    selectImage(fa.front());
                    ImageDoc* firstA = cur();
                    ensureCompareB();
                    app.compareMode = App::CmpWipe;   // cmpB() answers null while off
                    ImageDoc* firstB = cmpB();
                    fprintf(stderr, "abstatsselftest: A9b first compare, no history: "
                                    "A=%s B=%s\n",
                            firstA ? firstA->name.c_str() : "(none)",
                            firstB ? firstB->name.c_str() : "(none)");
                    check(firstB && firstB == firstA,
                          "A9b no remembered B: the default is the image you are on");
                    check(abStatsB() == firstA,
                          "A9b ...and it has pixels, so the first compare is never empty");
                    // ...and with a history the last B still wins. The chosen B
                    // is a document OTHER than A, so this cannot pass by
                    // accidentally agreeing with the clause above.
                    ImageDoc* pick = app.images[fb.front()].get();
                    check(pick != firstA, "A9b fixture: the chosen B is not A");
                    setCompareB(pick);
                    app.compareMode = App::CmpOff;
                    setCompareB(nullptr);             // as switching off does
                    ensureCompareB();
                    app.compareMode = App::CmpWipe;
                    ImageDoc* again = cmpB();
                    fprintf(stderr, "abstatsselftest: A9b re-enter after choosing B: "
                                    "B=%s (A=%s)\n",
                            again ? again->name.c_str() : "(none)",
                            cur() ? cur()->name.c_str() : "(none)");
                    check(again == pick,
                          "A9b a remembered B outranks the image you are on");
                    app.lastCompareBUid = keepLast;
                    app.prevImageUid = keepPrev;
                    app.cmpExtra = keepExtra;
                }
            }
        }

        // ---- S: the slots past B, held to B's own rules -----------------------
        // (defects, 2026-07-30: 「Cに選んだやつが，Stackだとフレーム数更新され
        // ない」, 「FilesにC,D,,,,が表示されない」, and A standing on the pinned
        // B reading as 「比較を抜けてしまう」.)
        // do/while(0) and not a plain block: S1-S3 are CPU and S4-S6 drive real
        // frames, so the NOGL sibling has to stop in the MIDDLE of this group
        // (板 304). `break` is the only exit a block has, and re-indenting 200
        // lines to gain an `if` would bury the change it is making.
        do {
            check(app.seqs.size() >= 2, "S fixture: two stacks are open");
            int sid0 = app.seqs[0].id, sid1 = app.seqs[1].id;
            std::vector<int> f0 = framesOfSeq(sid0), f1 = framesOfSeq(sid1);
            check(f0.size() >= 5 && f1.size() >= 5, "S fixture: five frames each");
            app.compareFollowFrame = true;
            app.compareMode = App::CmpOff;
            setCompareB(nullptr);
            app.lastCompareBUid = 0;
            app.cmpExtra.clear();
            selectImage(f0[0]);
            addCompareSlot(app.images[f1[0]].get());   // slot C pins stack 1, at frame 0

            // S1: step A to frame 2. The slot pins the STACK (through a member
            // uid); follow-frame must hand back ITS frame 2, exactly as
            // resolveB does for B - not the frame the slot was clicked on.
            selectImage(f0[2]);
            std::vector<ImageDoc*> ex = compareExtras();
            fprintf(stderr, "abstatsselftest: S1 A at frame 2: slot C -> %s seq=%d idx=%d\n",
                    ex.empty() ? "(gone)" : ex[0]->name.c_str(),
                    ex.empty() ? -1 : ex[0]->seqId, ex.empty() ? -1 : ex[0]->seqIndex);
            check(ex.size() == 1 && ex[0]->seqId == sid1 && ex[0]->seqIndex == 2,
                  "S1 slot C follows A's frame number, like B");

            // S2: ...and keeps its LETTER while doing so - the letter belongs
            // to the slot, the uid only pins the stack.
            app.compareMode = App::CmpSplit;
            std::vector<TileSlot> ts = tileSlots();
            fprintf(stderr, "abstatsselftest: S2 tiles:");
            for (const TileSlot& t : ts)
                fprintf(stderr, "  %s:%s(f%d)", t.tag.c_str(), t.doc->name.c_str(),
                        t.doc->seqIndex);
            fputc('\n', stderr);
            check(ts.size() == 2 && ts[1].tag == "C" && ts[1].doc->seqIndex == 2,
                  "S2 the tile badge stays C on the followed frame");

            // S3: a stack with no frame at A's number DIVERGES, audibly, the
            // way g_abFollowDiverged says it for B - never a stale frame
            // passed off as a match.
            selectImage(f1[4]);
            closeCurrent(true);            // stack 1 now ends at frame 3
            f0 = framesOfSeq(sid0);
            selectImage(f0[4]);            // A at frame 4: stack 1 has no partner
            ex = compareExtras();
            fprintf(stderr, "abstatsselftest: S3 A at frame 4: slot C idx=%d diverged=%d\n",
                    ex.empty() ? -1 : ex[0]->seqIndex, slotFollowDiverged(0) ? 1 : 0);
            check(ex.size() == 1 && slotFollowDiverged(0),
                  "S3 a slot past its stack's frames says diverged");
            check(ex.size() == 1 && ex[0]->seqIndex == 0,
                  "S3 ...showing the pinned frame, flagged, not a fake match");

            // S4: the Files rows carry the letters. A stack holding B and slot
            // C reads "BC"; a standalone frame in slot D reads "D". Real ImGui
            // frames, asserted through g_filesBadgeProbe - same reasoning as
            // the T group above.
            if (!glGroup("--abstats-selftest (S4-S6, the Files list group)")) return 1;
            if (!g_haveWindow) break;        // S1-S3 above are the sibling's share
            f1 = framesOfSeq(sid1);
            selectImage(f0[0]);
            setCompareB(app.images[f1[0]].get());       // B = stack 1, frame 0
            app.compareMode = App::CmpWipe;
            app.cmpExtra.clear();
            addCompareSlot(app.images[f1[1]].get());    // slot C = stack 1, frame 1
            std::string root = g_abstatsSelftest;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            size_t sl2 = root.find_last_of('/');
            root = sl2 == std::string::npos ? std::string(".") : root.substr(0, sl2);
            size_t nBefore = app.images.size();
            openPath(root + "/rgb_u8.npy");             // a standalone frame
            loadAll();
            ImageDoc* lone = app.images.size() > nBefore ? app.images.back().get() : nullptr;
            selectImage(f0[0]);      // openPath moved A onto the new image; step off,
            if (lone) addCompareSlot(lone);             // or A's row could not take slot D
            auto filesFrame = [&]() {
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(420.0f, 600.0f), ImGuiCond_Always);
                    ImGui::Begin("FilesProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    drawFileList();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            filesFrame();
            const App::SeqInfo* si1 = seqInfo(sid1);
            std::string wantStack = (si1 ? si1->name : std::string("?")) + "=BC;";
            std::string wantLone = lone ? lone->name + "=D;" : std::string();
            fprintf(stderr, "abstatsselftest: S4 badges '%s'  want '%s'%s%s\n",
                    g_filesBadgeProbe.c_str(), wantStack.c_str(),
                    lone ? " + " : "", wantLone.c_str());
            check(g_filesBadgeProbe.find(wantStack) != std::string::npos,
                  "S4 the stack holding B and slot C reads BC");
            if (lone)
                check(g_filesBadgeProbe.find(wantLone) != std::string::npos,
                      "S4 a standalone frame in slot D reads D");
            else
                fprintf(stderr, "abstatsselftest: S4 lone-frame check skipped "
                                "(%s/rgb_u8.npy not there)\n", root.c_str());

            // S4b: the Files panel's own pinned-ancestor band
            // (「上層の階層は常に表示」). The walk above recorded, in the
            // list's content coordinates, where every heading starts and where
            // the rows under it end. Scroll to the middle of a heading's span -
            // i.e. past the heading itself, but still inside its rows - and
            // that heading has to be drawn ABOVE the list, by name.
            //
            // The scroll is set rather than wheeled because the assertion is
            // about which levels the band holds at a given offset, and a
            // wheeled one would only ever reach whatever offset this fixture
            // and this window size happen to allow. The SPANS are real: they
            // come from the frame that just drew the panel, in the panel's own
            // units, and nothing here computes them a second way.
            //
            // Then the other direction, which is the half a "does it draw"
            // check would miss: back at the top there is no band at all. A
            // heading that is on screen anyway must not also be held above
            // itself.
            {
                // ONE pass per offset, deliberately: the list writes its own
                // scroll back into g_filesScrollY on its way out, so a second
                // pass would draw the band at the offset the probe window
                // really is at (zero) rather than the one being asked about.
                auto bandAt = [&](float y) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(420.0f, 600.0f), ImGuiCond_Always);
                    ImGui::Begin("FilesProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    g_filesScrollY = y;
                    drawFileList();
                    ImGui::End();
                    ImGui::EndFrame();
                    return g_filesPinProbe;
                };
                const FilesSpan* deepest = nullptr;
                for (const auto& s : g_filesSpans)
                    if (s.y1 - s.hy > 1.0f && (!deepest || s.y0 > deepest->y0)) deepest = &s;
                std::string want, got;
                float at = 0;
                if (deepest) {
                    for (const auto& b : app.batches)
                        if (b.id == deepest->batch && deepest->series == 0) want = b.name + ";";
                    for (const auto& s : app.series)
                        if (s.id == deepest->series) want = s.name + ";";
                    at = (deepest->hy + deepest->y1) * 0.5f;
                    got = bandAt(at);
                }
                fprintf(stderr, "abstatsselftest: S4b files band at scroll %.0f "
                                "holds '%s'  want '%s'\n", at, got.c_str(), want.c_str());
                check(deepest && !want.empty() && got.find(want) != std::string::npos,
                      "S4b a heading the list has scrolled past is drawn above it");
                std::string top = bandAt(0.0f);
                fprintf(stderr, "abstatsselftest: S4b files band at the top holds '%s'\n",
                        top.c_str());
                check(top.empty(),
                      "S4b nothing is pinned while the list is at the top");
            }

            // S5: A standing on the pinned B is an ORDINARY comparison, and
            // the chip names the pair like any other. It used to be a PAUSE,
            // announced as "A = B (paused)" - the sentence that existed only to
            // explain why every panel had gone quiet. Nothing goes quiet now
            // (2026-08-04: 「こういうの全般不要です」), so there is nothing to
            // explain, and the chip must not have a second vocabulary for it:
            // the reading BEFORE and AFTER stepping off is the same sentence.
            app.cmpExtra.clear();
            app.compareMode = App::CmpWipe;
            selectImage(f0[0]);
            setCompareB(cur());                  // Shift+B's path: pin THIS frame
            std::string chip = abStatusChipText();
            fprintf(stderr, "abstatsselftest: S5 chip while A==B: '%s'\n", chip.c_str());
            check(chip.find("B:") != std::string::npos,
                  "S5 A standing on the pinned B still names B in the chip");
            check(chip.find("paused") == std::string::npos &&
                  chip.find("no B image") == std::string::npos,
                  "S5 ...with no 'paused' or 'no B image' vocabulary left");
            check(cmpB() == cur() && abStatsB() == cur(),
                  "S5 ...and the panels get a B: the same document A is on");
            selectImage(f0[1]);                  // step off: the pair is unchanged
            chip = abStatusChipText();
            fprintf(stderr, "abstatsselftest: S5 chip after stepping off: '%s'\n", chip.c_str());
            check(chip.find("B:") != std::string::npos,
                  "S5 ...and stepping off changes nothing about the chip's shape");

            // S6: ESC steps outward one claim per press, and the Files letters
            // are SEATS: full ink while a comparison runs, visible but dimmed
            // while compare is off, with A - the cursor, not a seat - on the
            // current row exactly while a comparison is active. (User report
            // 2026-07-30: ESCで抜ける + 「FilesパネルのB,Cは薄く表示」.)
            {
                f0 = framesOfSeq(sid0);
                f1 = framesOfSeq(sid1);
                selectImage(f0[0]);
                setCompareB(app.images[f1[0]].get());
                app.compareMode = App::CmpWipe;
                app.cmpExtra.clear();
                if (lone) addCompareSlot(lone);       // slot C = the lone frame
                app.selectedAnn = 0;
                const App::SeqInfo* s6a = seqInfo(sid0);
                const App::SeqInfo* s6b = seqInfo(sid1);
                std::string row0 = s6a ? s6a->name : "?";
                std::string row1 = s6b ? s6b->name : "?";
                filesFrame();
                fprintf(stderr, "abstatsselftest: S6 badges (comparing): '%s'\n",
                        g_filesBadgeProbe.c_str());
                check(g_filesBadgeProbe.find(row0 + "=A;") != std::string::npos,
                      "S6 comparing: the current row wears the A badge, full ink");
                check(g_filesBadgeProbe.find(row1 + "=B;") != std::string::npos,
                      "S6 comparing: the B seat reads B, full ink");
                if (lone)
                    check(g_filesBadgeProbe.find(lone->name + "=C;") != std::string::npos,
                          "S6 comparing: slot C reads C, full ink");
                // ESC with an ROI selected AND compare on: the first press
                // releases the ROI and ONLY that - one press, one step.
                app.selectedAnn = 4242;               // white-box: a selected ROI
                g_escProbe.clear(); g_escProbeN = 0;  // the chain starts here
                EscTook took = escapePressed();
                check(took == EscTook::RoiDeselected && app.selectedAnn == 0 &&
                      app.compareMode == App::CmpWipe,
                      "S6 first ESC deselects the ROI, and nothing else");
                uint64_t pinBefore = app.compareBUid;
                size_t slotsBefore = app.cmpExtra.size();
                took = escapePressed();
                check(took == EscTook::CompareOff && app.compareMode == App::CmpOff,
                      "S6 second ESC exits compare, and only compare");
                check(app.compareBUid == pinBefore && app.cmpExtra.size() == slotsBefore,
                      "S6 ...keeping the B pin and the slot letters (seats survive)");
                check(escapePressed() == EscTook::Nothing,
                      "S6 a third ESC has nothing left to take");
                // ...and the three presses as ONE record, which is the form the
                // rule is written in: each press took exactly one step and they
                // came in that order. Three separate return values cannot say
                // "and nothing else happened in between" (E7).
                fprintf(stderr, "abstatsselftest: S6 esc chain: '%s'\n", g_escProbe.c_str());
                check(g_escProbe == "roi;compare;nothing;" && g_escProbeN == 3,
                      "S6 the three presses read as one chain, one step each");
                // compare OFF: the seats stay visible, dimmed ("armed, not
                // active"); A is not a seat and is gone entirely
                filesFrame();
                fprintf(stderr, "abstatsselftest: S6 badges (compare off): '%s'\n",
                        g_filesBadgeProbe.c_str());
                check(g_filesBadgeProbe.find(row1 + "=B(dim);") != std::string::npos,
                      "S6 compare off: the B seat stays visible, dimmed");
                if (lone)
                    check(g_filesBadgeProbe.find(lone->name + "=C(dim);") != std::string::npos,
                          "S6 compare off: slot C stays visible, dimmed");
                check(g_filesBadgeProbe.find("=A") == std::string::npos,
                      "S6 compare off: no A badge - the cursor is not a seat");
                // C resumes: the SAME letters come back at full ink - the
                // round trip kept the seats (A9's pair, now visible)
                cycleCompare();                        // Off -> wipe
                check(app.compareMode == App::CmpWipe, "S6 C resumes the comparison");
                filesFrame();
                fprintf(stderr, "abstatsselftest: S6 badges (resumed): '%s'\n",
                        g_filesBadgeProbe.c_str());
                check(cmpB() && cmpB()->uid == pinBefore,
                      "S6 ...with the same B (A9: the pair returns)");
                check(g_filesBadgeProbe.find(row0 + "=A;") != std::string::npos &&
                      g_filesBadgeProbe.find(row1 + "=B;") != std::string::npos,
                      "S6 ...and the letters are back at full ink");
            }
        } while (0);

        // ---- N: the panel says something about EVERY slot it holds -----------
        // The defect (issue #60): with C, D, E armed the Statistics/Histogram
        // panel drew A and B and said nothing whatever about the rest. The
        // assertion therefore has to be about what the panel SAYS, not about
        // what the cache holds - a cache full of correct numbers nobody prints
        // is precisely the bug. g_histSideProbe records the letters the panel
        // put on screen: a row in the table, a curve on the plot, or a named
        // omission under it. The invariant every case below re-checks is that
        // those last two ACCOUNT FOR the first. A letter with a row and no
        // curve and no mention is the defect, restated as an equation.
        //
        // Every assert in this group reads a probe a real frame wrote, so all of
        // it is the GL run's (板 304). The W group after it is CPU and runs in
        // both, which is why the two are separate blocks.
        if (!glGroup("--abstats-selftest (N, the N-slot panel group)")) return 1;
        if (g_haveWindow) {
            check(app.seqs.size() >= 2, "N fixture: two stacks are open");
            int sid0 = app.seqs[0].id, sid1 = app.seqs[1].id;
            std::vector<int> f0 = framesOfSeq(sid0), f1 = framesOfSeq(sid1);
            check(f0.size() >= 4 && f1.size() >= 4, "N fixture: four frames each");
            // follow OFF: a pin then stays on the frame it was put on, so the
            // slots are N DISTINCT documents and the count under test is the
            // slot count. (Following is S1's subject, not this group's.)
            app.compareFollowFrame = false;
            app.compareMode = App::CmpWipe;
            app.abStepBusyUntil = 0;
            app.cmpExtra.clear();
            app.histPlane = 0;          // ONE plane on the axis: the overlay case
            selectImage(f0[0]);
            setCompareB(app.images[f1[0]].get());
            addCompareSlot(app.images[f0[1]].get());     // C
            addCompareSlot(app.images[f0[2]].get());     // D
            check(cmpB() != nullptr && app.cmpExtra.size() == 2,
                  "N fixture: B plus slots C and D are armed");

            // The panel width is an INPUT, not a constant: side by side keeps
            // one pane per side and retreats to the overlay when a pane would
            // fall under AB_MIN_SIDE, so "how many panes did it draw" and "did
            // it retreat" are two different fixtures and differ only in width.
            // 560 is what every test here used before N-wide panes existed and
            // is now the narrow one; the wide one has to hold nSides * 320.
            auto panelFrame = [&](void (*draw)(), const char* name, float w) {
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, 720.0f), ImGuiCond_Always);
                    ImGui::Begin(name, nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    draw();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            auto histFrameW = [&](float w) { panelFrame(drawPanelHistogram, "HistProbe", w); };
            auto projFrameW = [&](float w) { panelFrame(drawPanelProjection, "ProjProbe", w); };
            auto histFrame = [&]() { histFrameW(560.0f); };
            auto projFrame = [&]() { projFrameW(560.0f); };
            // "rows=", "curves=" and "said-no-curve=" as letter sets, read back
            // out of the probe the panel wrote
            auto field = [](const std::string& probe, const char* key) {
                size_t k = probe.find(key);
                if (k == std::string::npos) return std::string();
                k += strlen(key);
                size_t e = probe.find(';', k);
                return probe.substr(k, e == std::string::npos ? e : e - k);
            };
            auto rowsOf = [](const std::string& probe) {
                std::string out;
                for (size_t k = probe.find("row"); k != std::string::npos;
                     k = probe.find("row", k + 1))
                    if (k + 3 < probe.size() && probe[k + 4] == ';') out += probe[k + 3];
                return out;
            };
            // the one sentence this group exists for, asked the same way of
            // every layout below: is there a letter the panel gave numbers to
            // and then neither drew nor mentioned?
            auto accountedFor = [&](const std::string& probe, std::string& unsaidOut) {
                std::string rows = rowsOf(probe);
                std::string said = field(probe, "curves=") + field(probe, "said-no-curve=");
                unsaidOut.clear();
                for (char c : rows)
                    if (said.find(c) == std::string::npos) unsaidOut += c;
                return unsaidOut.empty();
            };

            // N1: four sides armed, four sides named. This is the assert that
            // fails against every build before this change: the table stopped
            // at B because hist[] did.
            app.abStatsLayout = App::AbOverlay;
            histFrame();
            std::string pOver = g_histSideProbe;
            fprintf(stderr, "abstatsselftest: N1 overlay probe '%s'\n", pOver.c_str());
            check(rowsOf(pOver) == "ABCD",
                  "N1 the statistics table gives A, B, C and D a row each");
            check(field(pOver, "curves=") == "ABCD",
                  "N1 ...and one plane on the axis puts all four on the plot");
            std::string unsaid;
            check(accountedFor(pOver, unsaid),
                  "N1 every slot with numbers is drawn or named");

            // N2: the plot cannot take four when four PLANES are on the axis
            // (four planes times four slots is sixteen curves). It must then
            // NAME the ones it left off - the table keeps all four either way.
            app.histPlane = -1;
            bool multiPlane = app.hist[0].nSeries > 1;
            histFrame();
            std::string pAll = g_histSideProbe;
            fprintf(stderr, "abstatsselftest: N2 all-planes probe '%s' (nSeries=%d)\n",
                    pAll.c_str(), app.hist[0].nSeries);
            check(rowsOf(pAll) == "ABCD",
                  "N2 the table still holds all four with every plane on the axis");
            check(accountedFor(pAll, unsaid),
                  "N2 ...and nothing is dropped in silence");
            if (multiPlane)
                check(field(pAll, "said-no-curve=") == "CD",
                      "N2 ...the slots come off the plot BY NAME, not quietly");
            else
                fprintf(stderr, "abstatsselftest: N2 single-plane fixture - the "
                                "plane limit is not reachable here, invariant only\n");
            app.histPlane = 0;

            // N3: side by side holds ONE PANE PER SIDE, at any N. The ruling on
            // #60 (2026-08-14): 「splitの場合は横並び(A, B, Cなら3つならぶ)」 -
            // the plots are arranged the way the images are, and split tiles
            // every armed slot. Two halves were the two-image spelling of that,
            // and while they were nailed in this assert read curves=AB with C
            // and D named off the plot - which is half of the reported
            // "histogram とか projection が3個以上にならん".
            //
            // Width is part of the fixture: four panes need 4 * AB_MIN_SIDE.
            app.abStatsLayout = App::AbSide;
            // AB_MIN_SIDE is LOGICAL px and the floor is compared against the
            // DPI-scaled one, so the fixture has to scale too - on a 150%
            // display four 320 px panes need 1920, not 1280.
            const float wide4 = (AB_MIN_SIDE * 4 + 140.0f) * app.uiScale;
            histFrameW(wide4);
            std::string pSide = g_histSideProbe;
            fprintf(stderr, "abstatsselftest: N3 side-by-side probe '%s' "
                            "(width %.0f, uiScale %.2f)\n",
                    pSide.c_str(), wide4, app.uiScale);
            check(rowsOf(pSide) == "ABCD",
                  "N3 side by side keeps every slot in the table");
            check(field(pSide, "layout=") == "side",
                  "N3 ...and a panel this wide really is laid out, not fallen back");
            check(field(pSide, "curves=") == "ABCD" &&
                  field(pSide, "said-no-curve=").empty(),
                  "N3 four sides get four panes - nothing is left off");
            check(accountedFor(pSide, unsaid),
                  "N3 every slot with numbers is drawn or named");
            // ...and the Projection panel answers the same question the same
            // way, off the same abSideBySide(): one setting, every panel, so a
            // comparison laid out on the image is laid out in both of them.
            projFrameW(wide4);
            std::string pSideP = g_projSideProbe;
            fprintf(stderr, "abstatsselftest: N3 projection side-by-side probe '%s'\n",
                    pSideP.c_str());
            check(field(pSideP, "layout=") == "side" &&
                  field(pSideP, "curves=") == "ABCD",
                  "N3 the projection lays out the same four sides in four panes");
            check(accountedFor(pSideP, unsaid),
                  "N3 ...and accounts for every one of them");

            // N3b: ...and the same four sides in a 560 px panel. A pane would be
            // 140 px, under the 320 px floor, so the panel SHRINKS to the point
            // it cannot and then says so and retreats to the overlay (ruling 3:
            // 「狭くなる,縮むでよい」 + the standing rule that a limit is spoken).
            histFrameW(560.0f);
            std::string pNarrow = g_histSideProbe;
            fprintf(stderr, "abstatsselftest: N3b narrow probe '%s'\n", pNarrow.c_str());
            check(field(pNarrow, "layout=") == "overlay",
                  "N3b four panes do not fit in 560 px: it falls back to the overlay");
            check(rowsOf(pNarrow) == "ABCD" && accountedFor(pNarrow, unsaid),
                  "N3b ...and loses nobody on the way");
            app.abStatsLayout = App::AbOverlay;

            // N4: past the palette. slotInk tells slotInkCount() slots apart
            // and then its modulo hands slot H back A's green - two slots in
            // one colour is the same omission wearing a different hat, so the
            // plot stops at the palette and says which letters it stopped at.
            {
                std::vector<ImU32> ink;
                for (size_t i = 0; i < slotInkCount(); i++) ink.push_back(slotInk(i));
                std::sort(ink.begin(), ink.end());
                check(std::adjacent_find(ink.begin(), ink.end()) == ink.end(),
                      "N4 the palette's own colours are all different");
            }
            size_t want = slotInkCount() + 2;     // two more sides than inks
            for (size_t k = 3; k < f0.size() && app.cmpExtra.size() + 2 < want; k++)
                addCompareSlot(app.images[f0[k]].get());
            for (size_t k = 1; k < f1.size() && app.cmpExtra.size() + 2 < want; k++)
                addCompareSlot(app.images[f1[k]].get());
            size_t nSides = app.cmpExtra.size() + 2;
            if (nSides > slotInkCount()) {
                histFrame();
                std::string pMany = g_histSideProbe;
                fprintf(stderr, "abstatsselftest: N4 %zu sides, %zu inks: '%s'\n",
                        nSides, slotInkCount(), pMany.c_str());
                check(rowsOf(pMany).size() == nSides,
                      "N4 every armed slot has a row, however many there are");
                check(field(pMany, "curves=").size() == slotInkCount(),
                      "N4 the plot draws exactly as many as it has colours for");
                check(field(pMany, "said-no-curve=").size() == nSides - slotInkCount(),
                      "N4 ...and names every one it could not colour");
                check(accountedFor(pMany, unsaid),
                      "N4 no slot is left with numbers and no word about it");
            } else {
                fprintf(stderr, "abstatsselftest: N4 skipped - only %zu images to "
                                "arm, need more than %zu\n", nSides, slotInkCount());
            }
            if (!unsaid.empty())
                fprintf(stderr, "abstatsselftest: N slots with a row and no word: '%s'\n",
                        unsaid.c_str());

            // N5: the ARMING path - the missing precondition of everything
            // above. N1-N4 arm their slots by calling addCompareSlot directly,
            // so they prove the panels can HANDLE N sides and prove nothing
            // about whether a user can PRODUCE them: the equation
            // rows ⊆ curves ∪ said-no-curve holds over the empty set, which is
            // exactly how "histogram とか projection が3個以上にならん" (issue
            // #60) shipped under a green suite. So walk the path a user walks:
            // the Files row menu offers what slotRowItem answers (the menu
            // builds its items from it and nothing else), and clicking the item
            // fires addCompareSlot. A menu item cannot be scripted (the
            // browse-keys precedent), so the offer is asserted at the gate and
            // the click's action is fired directly - the same one line the
            // MenuItem runs.
            {
                app.cmpExtra.clear();
                selectImage(f0[0]);
                setCompareB(app.images[f1[0]].get());
                app.abStepBusyUntil = 0;   // selectImage arms the step throttle
                std::string L;
                check(slotRowItem(app.images[f0[1]].get(), L) == SlotRowAdd && L == "C",
                      "N5 a free row is offered 'Add as compare slot C'");
                addCompareSlot(app.images[f0[1]].get());        // the item's click
                check(slotRowItem(app.images[f0[1]].get(), L) == SlotRowRemove && L == "C",
                      "N5 ...and afterwards the same row offers the remove, by letter");
                check(slotRowItem(app.images[f1[0]].get(), L) == SlotRowNone,
                      "N5 the pinned B's row is offered no slot: one image, one letter");
                check(slotRowItem(app.images[f0[2]].get(), L) == SlotRowAdd && L == "D",
                      "N5 the next free row is offered D, not C again");
                addCompareSlot(app.images[f0[2]].get());
                check(slotRowItem(app.images[f1[1]].get(), L) == SlotRowAdd && L == "E",
                      "N5 ...and the one after that E");
                addCompareSlot(app.images[f1[1]].get());
                check(app.cmpExtra.size() == 3,
                      "N5 three slots armed through the entry path");
                histFrame();
                std::string pArm = g_histSideProbe;
                fprintf(stderr, "abstatsselftest: N5 armed-path probe '%s'\n",
                        pArm.c_str());
                check(rowsOf(pArm) == "ABCDE",
                      "N5 all five sides appear as rows - the third slot exists on screen");
                check(field(pArm, "curves=") == "ABCDE",
                      "N5 ...and one plane on the axis draws every one of them");
                check(accountedFor(pArm, unsaid),
                      "N5 every armed slot is drawn or named");
            }

            // N6: the same equation over the PROJECTION panel, which had no
            // probe at all - the panel issue #60 actually caught. Its table
            // went N wide (75e5369) while its plots drew only A and B, and
            // nothing on screen said so: rows had letters that were in neither
            // curves nor any sentence. The first fix was to make it SAY so, and
            // this assert pinned "AB" as the spoken limit of the day.
            //
            // The limit is now gone (docs/features/compare/compare-n-design.md D5, unblocked by
            // ruling 2 of 2026-08-14): with one plane on the axis - which the
            // shared plane selector is what makes reachable - every side takes
            // the shared axis in its own slot ink. Derivation of the expected
            // value: A, B and the three slots N5 armed = 5 sides, histPlane is
            // 0 so one plane is drawn, 5 <= slotInkCount() = 7, so five curves
            // and nothing named off.
            {
                check(app.cmpExtra.size() == 3, "N6 fixture: N5 left C, D, E armed");
                check(app.histPlane == 0, "N6 fixture: one plane on the axis");
                projFrame();
                std::string pProj = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N6 projection probe '%s'\n",
                        pProj.c_str());
                check(rowsOf(pProj) == "ABCDE",
                      "N6 the projection table gives every armed side a row");
                check(field(pProj, "curves=") == "ABCDE",
                      "N6 ...and one plane on the axis puts all five on the profile plots");
                check(field(pProj, "said-no-curve=").empty(),
                      "N6 ...with no letter left off to name");
                check(accountedFor(pProj, unsaid),
                      "N6 no slot has numbers and no word about it");
                // ...and every plane on the axis is still the spoken limit:
                // four planes times five slots times two axes is forty curves.
                if (app.proj[0].nSeries > 1) {
                    const int keepPlane = app.histPlane;
                    app.histPlane = -1;
                    projFrame();
                    std::string pAllP = g_projSideProbe;
                    fprintf(stderr, "abstatsselftest: N6 all-planes probe '%s'\n",
                            pAllP.c_str());
                    check(field(pAllP, "curves=") == "AB" &&
                          field(pAllP, "said-no-curve=") == "CDE",
                          "N6 every plane on the axis: the slots come off BY NAME");
                    check(accountedFor(pAllP, unsaid),
                          "N6 ...and nothing is dropped in silence");
                    app.histPlane = keepPlane;
                } else {
                    fprintf(stderr, "abstatsselftest: N6 single-plane fixture - the "
                                    "plane limit is not reachable here\n");
                }
            }

            // N7: the OTHER half of ruling 1 - 「重なって表示される場合(wipe/
            // blink/diff)は重ねる。ただし表示中のスロットが分かる状態にする
            // (太線等)」. Overlaid, every side is on the axis at once, so the
            // layout cannot say which one the image is showing; the drawing
            // has to. This is the chart-side twin of the ROI table following
            // the blink (#76), so it is asserted the same way: as a LETTER the
            // panel wrote down, never as a stroke width in pixels.
            //
            // Before this change there was no `emph` field at all, so every
            // check below failed on an empty string.
            {
                const int keepMode = app.compareMode;
                const bool keepFlip = app.flipShowB;
                app.abStatsLayout = App::AbAuto;   // let the IMAGE decide, as shipped
                app.compareMode = App::CmpFlip;    // one image area, showing one side

                app.flipShowB = false;             // ...and the side it shows is A
                histFrame();  std::string pFlipA = g_histSideProbe;
                projFrame();  std::string qFlipA = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N7 blink=A hist '%s' proj '%s'\n",
                        pFlipA.c_str(), qFlipA.c_str());
                check(field(pFlipA, "layout=") == "overlay",
                      "N7 blink is one image area, so Auto overlays the plots");
                check(field(pFlipA, "emph=") == "A",
                      "N7 the histogram draws the side the image is showing heavy");
                check(field(qFlipA, "emph=") == "A",
                      "N7 ...and so does the projection, off the same answer");

                app.flipShowB = true;              // the blink turns over
                histFrame();  std::string pFlipB = g_histSideProbe;
                projFrame();  std::string qFlipB = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N7 blink=B hist '%s' proj '%s'\n",
                        pFlipB.c_str(), qFlipB.c_str());
                check(field(pFlipB, "emph=") == "B",
                      "N7 ...and the weight moves with it, to B");
                check(field(qFlipB, "emph=") == "B",
                      "N7 ...on both panels");
                // the emphasis is the ONLY thing that moved: every side still
                // has its row and its curve, which is what "overlay" means
                check(rowsOf(pFlipB) == rowsOf(pFlipA) &&
                      field(pFlipB, "curves=") == field(pFlipA, "curves="),
                      "N7 nobody enters or leaves the plot when the blink turns over");

                // Wipe and difference put BOTH sides on screen in the same
                // instant, so no side is "the one you are looking at" and
                // inventing a lead would be a claim the image does not make.
                app.compareMode = App::CmpWipe;
                histFrame();  std::string pWipe = g_histSideProbe;
                projFrame();  std::string qWipe = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N7 wipe hist '%s'\n", pWipe.c_str());
                check(field(pWipe, "emph=").empty() && field(qWipe, "emph=").empty(),
                      "N7 wipe shows both at once: no side is emphasised");
                check(field(pWipe, "curves=") == field(pFlipB, "curves="),
                      "N7 ...and the curves are the same ones either way");

                app.compareMode = keepMode;
                app.flipShowB = keepFlip;
                app.abStatsLayout = App::AbOverlay;
            }

            // N8: ruling 2 - the Projection panel's plane selector IS the
            // histogram's (`app.histPlane`), one setting read by both. Narrowing
            // to one plane is a statement about what is being compared, not
            // about which panel is being read.
            //
            // A menu combo cannot be scripted (the browse-keys precedent), so
            // the sharing is asserted through the ONE thing a shared variable
            // does that two variables cannot: each panel clamps it, and the
            // other panel then reports what its neighbour wrote. That is the
            // "both directions" check - projection -> histogram and back - and
            // it fails against any build where the projection has a plane of
            // its own or no plane at all.
            {
                const int keepPlane = app.histPlane;

                app.histPlane = 99;             // out of range for either panel
                projFrame();                    // the PROJECTION clamps it...
                const int afterProj = app.histPlane;
                histFrame();                    // ...and the histogram reads that
                std::string pH = g_histSideProbe, pP = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N8 after proj clamp histPlane=%d "
                                "hist '%s' proj '%s'\n", afterProj, pH.c_str(), pP.c_str());
                check(afterProj >= -1 && afterProj < std::max(app.proj[0].nSeries, 1),
                      "N8 the projection panel clamps the shared plane");
                check(!field(pP, "plane=").empty() &&
                      field(pH, "plane=") == field(pP, "plane="),
                      "N8 ...and the histogram is showing the plane the projection wrote");

                app.histPlane = -1;             // "all", set from outside both
                histFrame();  pH = g_histSideProbe;
                projFrame();  pP = g_projSideProbe;
                check(field(pH, "plane=") == "all" && field(pP, "plane=") == "all",
                      "N8 both panels follow the shared plane to 'all'");

                app.histPlane = 0;              // ...and back to one plane
                histFrame();  pH = g_histSideProbe;
                projFrame();  pP = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N8 plane=0 hist '%s' proj '%s'\n",
                        pH.c_str(), pP.c_str());
                check(!field(pH, "plane=").empty() && field(pH, "plane=") != "all" &&
                      field(pH, "plane=") == field(pP, "plane="),
                      "N8 ...and to the same single plane, named the same way");

                app.histPlane = keepPlane;
            }

            // ---- N9: "ch, then side" pairs planes BY NAME (board 265) --------
            // A is a mosaic - R, Gr, Gb, B - and B is a plain 1-channel image
            // with one plane called ch0. The walk ran `for i < maxSeries` and
            // printed whatever each side held at INDEX i, so the line directly
            // under R:A was ch0:B: two different planes offered as a pair, in
            // the one row order whose entire purpose is comparing ONE plane
            // across sides. ch0 is not R, and the canon's iron rule is that CFA
            // planes are never mixed.
            //
            // CFA interpretation is per IMAGE (Inspector > Interpret), so this
            // pair is made by setting it on A and clearing it on B - the same
            // white-box move P2 makes for the size mismatch, and the only way to
            // get the fixture under BOTH runners: --cfa bayer arms every image.
            {
                const int keepOrder = app.projStatOrder;
                const int keepPlane = app.histPlane;
                const int keepLayout = app.projStatLayout;
                const bool keepAll = app.projAllRow;
                app.cmpExtra.clear();            // A and B: two sides, the smallest
                app.projAllRow = false;          // ...and no pooled row, which is
                                                 // not a plane and never pairs
                app.compareMode = App::CmpWipe;
                selectImage(f0[0]);
                setCompareB(app.images[f1[0]].get());
                abStatsFrame();
                // The step throttle skips B's recompute (P2b's subject), and
                // selecting a frame arms it - so a B whose CFA setting has just
                // changed would keep the cache from before the change and this
                // group would be reading a stale plane count. Cleared for every
                // frame below, which is the only reason they are all taken
                // through one lambda.
                auto projFrameNow = [&]() { app.abStepBusyUntil = 0; projFrame(); };
                ImageDoc* nA = cur();
                ImageDoc* nB = cmpB();
                check(nA && nB && nA != nB, "N9 fixture: A and B are two documents");
                if (nA && nB) {
                    const int keepCfaA = nA->cfa, keepPatA = nA->cfaPattern;
                    const int keepCfaB = nB->cfa;
                    nA->cfa = 1; nA->cfaPattern = 0;   // A is told it is a mosaic
                    nB->cfa = 0;                       // B is told it is not
                    app.projStatOrder = 1;             // "ch, then side"
                    app.histPlane = 0;
                    app.projStatLayout = 1;            // wide
                    projFrameNow();
                    const std::string wideRows = field(g_projSideProbe, "planerows=");
                    app.projStatLayout = 2;            // ...and per axis
                    projFrameNow();
                    const std::string axisRows = field(g_projSideProbe, "planerows=");
                    fprintf(stderr, "abstatsselftest: N9 A %d plane(s) / B %d plane(s), "
                                    "ch-then-side rows: '%s'\n",
                            app.proj[0].nSeries, app.proj[1].nSeries, wideRows.c_str());
                    check(app.proj[0].nSeries == 4 && app.proj[1].nSeries == 1,
                          "N9 fixture: A really has four planes and B one");
                    check(wideRows == "R@A,R@B-,Gr@A,Gr@B-,Gb@A,Gb@B-,B@A,B@B-,"
                                      "ch0@A-,ch0@B",
                          "N9 every row is ONE plane name, and the missing side says -");
                    // ANTI-VACUOUS: the index-matched order this replaced put
                    // ch0:B under R:A. That exact adjacency must be gone.
                    check(wideRows.find("R@A,ch0@B") == std::string::npos,
                          "N9 ...so R:A is no longer followed by ch0:B");
                    check(wideRows == axisRows,
                          "N9 both layouts walk the same rows in the same order");
                    // ...and with the mosaic off again the two sides pair 1:1
                    // with no "-" at all: the "-" is a statement about a missing
                    // plane, not decoration the walk always adds.
                    nA->cfa = 0;
                    projFrameNow();
                    const std::string same = field(g_projSideProbe, "planerows=");
                    fprintf(stderr, "abstatsselftest: N9 same planes both sides: '%s'\n",
                            same.c_str());
                    check(same == "ch0@A,ch0@B",
                          "N9 equal plane sets pair one for one, with no - row");
                    // side major is untouched by any of this: it never asked the
                    // pairing question, and its order is A's planes then B's
                    app.projStatOrder = 0;
                    nA->cfa = 1;
                    projFrameNow();
                    const std::string sideMajor = field(g_projSideProbe, "planerows=");
                    fprintf(stderr, "abstatsselftest: N9 side-major rows: '%s'\n",
                            sideMajor.c_str());
                    check(sideMajor == "R@A,Gr@A,Gb@A,B@A,ch0@B",
                          "N9 'side, then ch' still lists each side's own planes");
                    nA->cfa = keepCfaA; nA->cfaPattern = keepPatA;
                    nB->cfa = keepCfaB;
                }
                app.projStatOrder = keepOrder;
                app.projStatLayout = keepLayout;
                app.projAllRow = keepAll;
                app.histPlane = keepPlane;
                projFrameNow();                     // leave the cache on the real CFA
            }

            app.cmpExtra.clear();
            app.compareFollowFrame = true;
            app.abStatsLayout = App::AbAuto;
            app.histPlane = -1;
        }

        // ---- W: the bin GRID - integer / power-of-two bin widths ------------
        // (docs/background/project/todo-open.md 9, user report 2026-07-29.) 256 bins spread evenly
        // over [black, white] gives a FRACTIONAL bin width whenever the range is
        // not a multiple of 256: at 64..1023 that is 3.746 DN, so one bin
        // swallows 4 integer DN values and the next swallows 3, and the panel
        // prints a periodic comb that is not in the data. Reading the shape of a
        // noise distribution is what this panel is FOR, so that is fatal.
        //
        // Every fixture below is a RAMP holding each DN of its range exactly
        // once, so "how many DN values landed in this bin" is read straight off
        // the bin counts - the comb is measured, not inferred. Deterministic and
        // windowless: synthetic documents, recomputeHistogramIfNeeded called
        // directly, no timing anywhere.
        {
            auto saveCmp = app.compareMode;
            bool saveLink = app.linkRange;
            auto saveAnn = app.selectedAnn;
            app.compareMode = App::CmpOff;   // these docs are nobody's compare part
            app.linkRange = false;           // ...and effBlack must be the doc's own
            app.selectedAnn = 0;             // whole image, no ROI

            auto mkRamp = [](const char* dt, int W2, int H2, double v0, double dv,
                             float black, float white) {
                auto d = std::make_unique<ImageDoc>();
                d->name = "wgrid"; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
                d->src->dtype = dt;
                d->src->data.resize((size_t)W2 * H2);
                for (size_t i = 0; i < d->src->data.size(); i++)
                    d->src->data[i] = (float)(v0 + dv * (double)i);
                d->src->vmin = (float)v0;
                d->src->vmax = (float)(v0 + dv * (double)(d->src->data.size() - 1));
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->black = black; d->white = white;
                return d;
            };
            // How many DN values landed in each bin, and is it the SAME number
            // everywhere? Only the bins the data REACHES are asked: the grid
            // widens outward on purpose, so bins past the data are legitimately
            // empty and an empty bin is not a comb tooth.
            auto binDnSpread = [](const App::HistState& HS, uint32_t& lo, uint32_t& hi,
                                  int& nonEmpty) {
                lo = 0xFFFFFFFFu; hi = 0; nonEmpty = 0;
                for (int b = 0; b < 256; b++) {
                    uint32_t c = HS.bins[0][b];
                    if (!c) continue;
                    lo = std::min(lo, c); hi = std::max(hi, c); nonEmpty++;
                }
                if (!nonEmpty) lo = 0;       // ...and "uniform" must not pass on nothing
            };
            auto isPow2 = [](float w) {
                if (!(w > 0) || !std::isfinite(w)) return false;
                int e; return frexpf(w, &e) == 0.5f;   // exact: no log2 rounding
            };
            // the snapped grid always CONTAINS the requested range - it widens
            // outward, never inward, or the panel would hide real pixels
            auto contains = [](const App::HistState& HS) {
                return HS.binOrigin <= HS.black &&
                       (double)HS.binOrigin + 256.0 * (double)HS.binW >= (double)HS.white;
            };
            auto grid = [&](const App::HistState& HS, const char* tag) {
                uint32_t lo, hi; int ne;
                binDnSpread(HS, lo, hi, ne);
                fprintf(stderr, "abstatsselftest: %s binW=%.9g origin=%.9g bins %.9g..%.9g"
                                " | range %.9g..%.9g | DN/bin %u..%u over %d bins\n",
                        tag, HS.binW, HS.binOrigin, HS.binOrigin,
                        (double)HS.binOrigin + 256.0 * (double)HS.binW,
                        HS.black, HS.white, lo, hi, ne);
            };

            {   // W1  u16 0..4095, a 12-bit sensor at full scale.
                // span = 4095 - 0 = 4095;  4095/256 = 15.99609375;  ceil -> 16.
                // origin = floor(0/16)*16 = 0, so the bins cover 0..4096, which
                // contains 0..4095. 4096 DN values / 256 bins = 16 in EVERY bin.
                auto d = mkRamp("u16", 64, 64, 0, 1, 0.0f, 4095.0f);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                uint32_t lo, hi; int ne;
                binDnSpread(T, lo, hi, ne);
                grid(T, "W1 u16 0..4095:");
                check(ne == 256 && hi > 0, "W1 fixture: all 256 bins carry data");
                check(T.binW == 16.0f, "W1 u16 0..4095 is exactly 16 DN/bin");
                check(T.binOrigin == 0.0f, "W1 u16 0..4095 bin origin is 0 DN");
                check(contains(T), "W1 the grid contains the requested range");
                check(lo == 16 && hi == 16, "W1 no comb: every bin holds 16 DN values");
            }
            {   // W2  u8 0..255.  span = 255;  255/256 = 0.99609375;  ceil -> 1
                // (and the floor of 1 would give the same). origin = 0, bins
                // cover 0..256: exactly one DN per bin, which is the case the
                // user named first.
                auto d = mkRamp("u8", 16, 16, 0, 1, 0.0f, 255.0f);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                uint32_t lo, hi; int ne;
                binDnSpread(T, lo, hi, ne);
                grid(T, "W2 u8 0..255:  ");
                check(ne == 256 && hi > 0, "W2 fixture: all 256 bins carry data");
                check(T.binW == 1.0f, "W2 u8 0..255 is exactly 1 DN/bin");
                check(T.binOrigin == 0.0f, "W2 u8 0..255 bin origin is 0 DN");
                check(contains(T), "W2 the grid contains the requested range");
                check(lo == 1 && hi == 1, "W2 every bin holds exactly one DN value");
            }
            {   // W3  THE COMB, at the report's own example: u16 64..1023.
                // Unsnapped the width is 959/256 = 3.74609375 DN, so bin 0 takes
                // DN 64..67 (four values) and bin 1 takes 68..70 (three) - a
                // periodic structure that is not in the data.
                // Snapped: ceil(3.74609375) = 4 DN/bin, origin =
                // floor(64/4)*4 = 64, bins cover 64..1088 (contains 64..1023).
                // The 960 DN of the range fill bins 0..239 with 4 each; bins
                // 240..255 lie past the data and stay empty.
                auto d = mkRamp("u16", 32, 30, 64, 1, 64.0f, 1023.0f);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                uint32_t lo, hi; int ne;
                binDnSpread(T, lo, hi, ne);
                grid(T, "W3 u16 64..1023:");
                check(ne >= 200 && hi > 0, "W3 fixture: the data reaches 200+ bins");
                check(T.binW == 4.0f, "W3 u16 64..1023 is exactly 4 DN/bin");
                check(T.binOrigin == 64.0f, "W3 u16 64..1023 bin origin is 64 DN");
                check(contains(T), "W3 the grid contains the requested range");
                check(lo == hi, "W3 no comb: every filled bin holds one DN count");
                check(lo == 4, "W3 ...and that count is 4 DN values per bin");
            }
            {   // W4  float, an arbitrary range: -3.7 .. 11.3.
                // span = 15 exactly in float; 15/256 = 0.05859375; the next
                // power of two at or above it is 2^-4 = 0.0625.
                // origin = floor(-3.7/0.0625)*0.0625 = floor(-59.2)*0.0625
                //        = -60*0.0625 = -3.75, so the bins cover -3.75..12.25.
                // Powers of two are the point: the edges are exact in binary and
                // re-binning accumulates no error.
                auto d = mkRamp("f32", 40, 40, -3.7, 0.009, -3.7f, 11.3f);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                grid(T, "W4 f32 -3.7..11.3:");
                check(T.sampled > 0, "W4 fixture: the histogram has samples");
                check(isPow2(T.binW), "W4 f32 bin width is a power of two");
                check(T.binW == 0.0625f, "W4 f32 -3.7..11.3 is 2^-4 DN/bin");
                check(T.binOrigin == -3.75f, "W4 f32 bin origin is -3.75 DN");
                check(contains(T), "W4 the grid contains the requested range");
            }
            {   // W4b  f32 0..1: 1/256 = 0.00390625 is ALREADY 2^-8, so the rule
                // must leave it exactly there - the next power of two at or
                // above an exact power of two is itself, and widening it would
                // halve the resolution of every normalised float image there is.
                auto d = mkRamp("f32", 32, 32, 0.0, 1.0 / 2000.0, 0.0f, 1.0f);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                grid(T, "W4b f32 0..1: ");
                check(isPow2(T.binW), "W4b f32 bin width is a power of two");
                check(T.binW == 0.00390625f, "W4b f32 0..1 stays at 2^-8 DN/bin");
                check(T.binOrigin == 0.0f, "W4b f32 0..1 bin origin is 0 DN");
                check(contains(T), "W4b the grid contains the requested range");
            }
            {   // W5  THE TRAP the design names. u16, display range 101..1000.
                // span = 899;  899/256 = 3.51171875;  ceil -> 4 DN/bin;
                // origin = floor(101/4)*4 = 25*4 = 100. The bins run 100..1124,
                // so the grid is wider than the display range at BOTH ends and
                // DN 100 / DN 1050 are outside what the user SEES but inside the
                // bins. clipLo/clipHi are "blown out or crushed at the DISPLAY
                // range" - the footer prints them as <black / >white - so they
                // have to be counted against 101..1000. Count them off the bins,
                // as the code did before this change, and the saturated-pixel
                // count silently drops the moment the grid widens.
                //
                // 1000 px:  10 @ 50   and 10 @ 100   are < 101   -> clipLo 2.00%
                //           20 @ 1050 and 10 @ 1100  are >= 1000 -> clipHi 3.00%
                //           950 in 101..999 are in range.
                auto d = mkRamp("u16", 25, 40, 0, 0, 101.0f, 1000.0f);
                for (size_t i = 0; i < d->src->data.size(); i++)
                    d->src->data[i] = i < 10   ? 50.0f
                                    : i < 20   ? 100.0f
                                    : i < 40   ? 1050.0f
                                    : i < 50   ? 1100.0f
                                               : 101.0f + (float)((i - 50) % 899);
                d->src->vmin = 50.0f; d->src->vmax = 1100.0f;
                d->syncMirrors();
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                const double topDN = (double)T.binOrigin + 256.0 * (double)T.binW;
                grid(T, "W5 u16 101..1000:");
                fprintf(stderr, "abstatsselftest: W5 clipLo=%.6f%% clipHi=%.6f%% "
                                "(want 2.000000%% / 3.000000%%) of %zu px\n",
                        T.clipLo * 100.0, T.clipHi * 100.0, T.sampled);
                check(T.binW == 4.0f && T.binOrigin == 100.0f,
                      "W5 fixture: 101..1000 snaps to 4 DN/bin from 100");
                check(T.binOrigin < T.black && topDN > (double)T.white,
                      "W5 fixture: the grid is wider than the range, both ends");
                check(T.clipLo > 0 && T.clipHi > 0,
                      "W5 fixture: it really does hold clipped pixels");
                check(fabs(T.clipLo - 0.02) < 1e-12,
                      "W5 clipLo 2.00%: counted at the DISPLAY range");
                check(fabs(T.clipHi - 0.03) < 1e-12,
                      "W5 clipHi 3.00%: the wider bins did not eat it");
                check(T.black == 101.0f && T.white == 1000.0f,
                      "W5 black/white stay the REQUESTED range");
                // ...and that is what keeping them requested BUYS: the cache
                // still hits. canvas.inc keys on H.black == wantBlack and
                // panel_histogram.inc asks H.black != effBlack(A) every frame;
                // store the snapped value in either and the histogram re-scans a
                // million pixels forever. Overwrite an OUTPUT and call again - a
                // re-scan would clear it.
                T.maxBin = 0xABCDEF;
                recomputeHistogramIfNeeded(d.get(), T);
                check(T.maxBin == 0xABCDEF, "W6 an identical second call is a cache HIT");
                check(T.black == effBlack(*d) && T.white == effWhite(*d),
                      "W6 ...because black/white still equal effBlack/White");
            }
            app.compareMode = saveCmp;
            app.linkRange = saveLink;
            app.selectedAnn = saveAnn;
        }

        // ==================================================================
        // ---- C: THE PROJECTION PANEL'S CURVES ---------------------------
        // User report 2026-09-29: 「Bayer で開いた場合に Projection がでないね」, and,
        // asked what exactly was missing: 「パネルの枠と数表は出ていて、曲線だけが無い」.
        // A frame, two axes, a full statistics table - and not one line on either plot.
        //
        // THE CAUSE, and it is why this group measures what it measures. A CFA profile
        // puts a NaN wherever the plane has no sample, so an RGGB R profile is a
        // sample, a hole, a sample. The stroke decimated by walking `stride`
        // consecutive entries and breaking the line at any bucket that held nothing
        // finite - correct for a real gap in the data, and fatal here the moment
        // `stride` came out 1, because then every other bucket was one hole and the
        // run was flushed before it ever held the two points a polyline needs. Not one
        // segment was drawn. `stride` is 1 whenever the plot is at least as wide in
        // pixels as the region is in columns, which is a small frame, a widened panel,
        // and above all AN ROI - the situation the report was in.
        //
        // So the assertions here are about the PAINT: g_projCurveProbe records what the
        // panel handed the draw list (points, separate strokes, and the column numbers
        // the curve spans), the shape --verify-selftest V29 uses for the Files rows.
        // A table of correct numbers over an empty plot is the defect, so a test that
        // reads the cache cannot see it.
        //
        // THE DISTINCTION every group below is really about: a hole in the mosaic and a
        // hole in the DATA are different facts. The first carries no information and
        // must be walked past; the second is a column no finite pixel was found in and
        // the curve must BREAK there, or the plot silently bridges missing data. C4 and
        // C5 are that pair, asserted on one fixture each, and they are the reason the
        // fix is a per-plane sample lattice rather than "skip every NaN".
        //
        // Windowless, on the --histhl-selftest pattern and for its reasons: the
        // fixtures are analytic and built in memory, and every assertion is a count, a
        // column number or a string, so it runs under the NOGL registrations of this
        // test as well as the GL ones - on every runner in the matrix rather than on
        // the one with a context. It opens no file and cannot be made to pass by a
        // fixture drifting, which is also why it is the LAST group in this file: it
        // closes the session to stand its own documents up, and every group above it
        // needs the folder that was opened at the top.
        //
        // ONE SELFTEST HOME IS SHARED BY EVERY TEST. Everything this group borrows -
        // projMode, projAllRow, projYMode, showProjH/V, histPlane, compareMode,
        // selectedAnn, the annotations, the projection caches - is put back at the
        // single exit below. Nothing here returns early for exactly that reason: a
        // group that bailed out would leave a preference behind for every later test
        // in the home, and for every later RUN of the suite.
        {
            // ---- everything borrowed, noted here and restored at the bottom --------
            const int keepCurrent = app.current;
            const size_t keepImages = app.images.size();
            const int keepMode = app.projMode, keepYMode = app.projYMode;
            const bool keepH = app.showProjH, keepV = app.showProjV;
            const bool keepAllRow = app.projAllRow;
            const int keepPlane = app.histPlane;
            // C14b and C15 put two sides on the panel, and whether the plots
            // overlay or go side by side is app.abStatsLayout's answer - it
            // decides whether the plane legend is built over one side or both,
            // so a group that reads the legend depends on it. Pinned below and
            // restored here: one selftest home is shared by every test.
            const auto keepAbLayout = app.abStatsLayout;
            const int keepStatLayout = app.projStatLayout, keepStatOrder = app.projStatOrder;
            const int keepDecimals = app.projDecimals;
            const auto keepCompare = app.compareMode;
            const int keepSel = app.selectedAnn;
            const bool keepLink = app.linkRange;
            const bool keepPrefsDirty = app.prefsDirty;

            closeAll();
            app.anns.clear(); app.nextAnnId = 1; app.selectedAnn = 0;   // whole image
            app.cmpExtra.clear();
            app.compareMode = App::CmpOff;          // one side: A, and nothing to tint
            app.abStepBusyUntil = 0;
            app.linkRange = false;
            app.projMode = 0;                       // mean
            app.projYMode = 0;                      // auto: the axis fits the data
            app.showProjH = app.showProjV = true;
            app.projAllRow = false;
            app.projStatLayout = 1;                 // wide: the table's height is then
            app.projStatOrder = 0;                  // the same whatever the panel width
            app.projDecimals = -1;
            app.histPlane = -1;                     // every plane on the axis
            app.abStatsLayout = App::AbOverlay;     // one axis, both sides on it
            app.proj[0] = App::ProjState{};
            app.proj[1] = App::ProjState{};
            app.projExtra.clear();

            // A synthetic frame. Every plane gets its own level AND its own slope along
            // x, so a curve that is drawn is also recognisable - four flat lines at
            // four levels would still be four curves, but nothing about them would say
            // which sample landed where.
            auto mkFrame = [&](const char* nm, int W, int H, int cfaKind, int chn) {
                auto d = std::make_unique<ImageDoc>();
                d->name = nm;
                d->src->w = W; d->src->h = H; d->src->ch = chn;
                d->src->dtype = "u16";
                d->src->data.assign((size_t)W * H * chn, 0.0f);
                d->uid = app.nextUid++;
                d->cfa = cfaKind; d->cfaPattern = 0;      // RGGB (and quad RGGB at cfa 2)
                d->syncMirrors();
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        for (int c = 0; c < chn; c++) {
                            const int pl = cfaKind ? cfaChannelAt(*d, x, y) : c;
                            d->px()[((size_t)y * W + x) * chn + c] =
                                (float)(500 + 400 * pl + (x % 89) + (y % 7));
                        }
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                return d;
            };
            // ...installed as the one open document, with the projection caches cleared
            // so nothing below can read a profile of the frame before it.
            auto useOnly = [&](std::unique_ptr<ImageDoc> d) {
                app.images.clear();
                app.images.push_back(std::move(d));
                app.current = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                return app.images[0].get();
            };

            // The Projection panel, drawn for real. histHlSelftest's frame shape:
            // the GL backends when there is a window, io.DisplaySize and a
            // built font atlas when there is not, two passes so the first one
            // sizes the window. It is NOT this file's panelFrame - that one is
            // GL-only and takes no mouse - and it does not clear the step
            // throttle the way projFrameNow does in N9, because every fixture
            // here installs its documents and its caches by hand.
            ImGuiIO& io = ImGui::GetIO();
            auto projFrame = [&](float w, float h, ImVec2 mouse) {
                for (int pass = 0; pass < 2; pass++) {   // the first frame sizes the window
                    if (g_haveWindow) {
                        ImGui_ImplOpenGL3_NewFrame();
                        ImGui_ImplGlfw_NewFrame();
                    } else {
                        io.DisplaySize = ImVec2(1600, 1000);
                        io.DisplayFramebufferScale = ImVec2(1, 1);
                        io.DeltaTime = 1.0f / 60.0f;
                        if (!io.Fonts->IsBuilt()) io.Fonts->Build();
                    }
                    // after the backend, before NewFrame - the GLFW backend rewrites
                    // MousePos from the OS cursor every frame (verify.inc V19). Off the
                    // plot by default, or the operator's own cursor would decide
                    // whether the hover readout ran.
                    io.AddMousePosEvent(mouse.x, mouse.y);
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                    ImGui::Begin("ProjCurveProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
                    drawPanelProjection();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            const ImVec2 NO_MOUSE(-FLT_MAX, -FLT_MAX);
            auto draw = [&]() { projFrame(900.0f, 900.0f, NO_MOUSE); };

            // The probe entry for one plane on one axis, or nullptr when the panel drew
            // nothing about that plane at all - which is itself a distinct failure from
            // "drew it with no points", and the checks below keep the two apart.
            auto curve = [&](const char* plane, bool horiz) -> const ProjCurveProbe* {
                for (const auto& c : g_projCurveProbe)
                    if (c.plane == plane && c.horizontal == horiz) return &c;
                return nullptr;
            };
            auto dump = [&](const char* tag) {
                std::string s;
                for (const auto& c : g_projCurveProbe) {
                    char b[192];
                    snprintf(b, sizeof b, "%s%s/%s pts=%d strokes=%d lattice=%d stride=%d x=%d..%d",
                             s.empty() ? "" : " | ", c.plane.c_str(), c.horizontal ? "H" : "V",
                             c.points, c.strokes, c.lattice, c.stride, c.xFirst, c.xLast);
                    s += b;
                }
                fprintf(stderr, "abstatsselftest: %s %s\n", tag,
                        s.empty() ? "(no curve entries at all)" : s.c_str());
                fprintf(stderr, "abstatsselftest: %s sides '%s'\n", tag, g_projSideProbe.c_str());
            };
            auto field = [](const std::string& probe, const char* key) {
                size_t k = probe.find(key);
                if (k == std::string::npos) return std::string();
                k += strlen(key);
                size_t e = probe.find(';', k);
                return probe.substr(k, e == std::string::npos ? e : e - k);
            };
            // Every plane of a CFA frame, on both axes, said the same thing about.
            static const char* CFA4[4] = { "R", "Gr", "Gb", "B" };
            auto eachPlane = [&](bool horiz, const std::function<void(const char*,
                                                                      const ProjCurveProbe*)>& f) {
                for (int i = 0; i < 4; i++) f(CFA4[i], curve(CFA4[i], horiz));
            };

            // ======================================================================
            // C0 - A DENSE PROFILE IS UNCHANGED. First, and green from the start
            // rather than red then green: every group after this one changes how the
            // stroke WALKS, and a mono image has one sample per column, so it is the
            // regression guard the rest of the file leans on. 16 columns, 16 points,
            // one unbroken stroke, and the decimation off.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_mono", 16, 16, 0, 1));
                draw();
                dump("C0 mono 16x16");
                const ProjCurveProbe* hh = curve("ch0", true);
                const ProjCurveProbe* vv = curve("ch0", false);
                check(d->cfa == 0 && app.proj[0].nSeries == 1,
                      "C0 fixture: one plane, no mosaic");
                check(hh && vv, "C0 the panel drew both profiles of it");
                if (hh && vv) {
                    check(hh->lattice == 16 && vv->lattice == 16,
                          "C0 every column and every row is a sample position");
                    check(hh->stride == 1 && vv->stride == 1,
                          "C0 fixture: 16 samples into ~800 px, so no decimation");
                    check(hh->points == 16 && hh->strokes == 1,
                          "C0 the H profile is one unbroken 16-point curve");
                    check(vv->points == 16 && vv->strokes == 1,
                          "C0 ...and so is the V profile");
                    check(hh->xFirst == 0 && hh->xLast == 15,
                          "C0 ...spanning columns 0..15");
                }
            }

            // ======================================================================
            // C1 - THE REPORT. 8x8 RGGB, whole image, every plane on the axis. The
            // plot is ~800 px wide and the image is 8 columns, so stride is 1 and this
            // is exactly the shape that drew nothing: each of the four planes owns
            // four of the eight positions, and before the fix NOT ONE of them got a
            // polyline. rw / cell = 8 / 2 = 4 points per plane, per axis.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_rggb8", 8, 8, 1, 1));
                draw();
                dump("C1 RGGB 8x8");
                check(d->cfa == 1 && app.proj[0].nSeries == 4 && app.proj[0].cell == 2,
                      "C1 fixture: four planes on a 2x2 mosaic");
                for (int axis = 0; axis < 2; axis++) {
                    const bool horiz = axis == 0;
                    eachPlane(horiz, [&](const char* nm, const ProjCurveProbe* c) {
                        char what[128];
                        snprintf(what, sizeof what, "C1 plane %s has a %s curve of 4 points",
                                 nm, horiz ? "H" : "V");
                        check(c && c->points == 4 && c->strokes == 1, what);
                        snprintf(what, sizeof what, "C1 plane %s %s: 4 sample positions, stride 1",
                                 nm, horiz ? "H" : "V");
                        check(c && c->lattice == 4 && c->stride == 1, what);
                    });
                }
                // ...and THE X AXIS IS NOT PACKED. Halving each plane into a dense
                // array of its own would have drawn four curves too - and would have
                // put Gr's first sample at column 0, on top of R's, so the four could
                // no longer be read against each other and the hover's "column x" and
                // the table's x would mean two different things.
                const ProjCurveProbe* r = curve("R", true);
                const ProjCurveProbe* gr = curve("Gr", true);
                check(r && r->xFirst == 0 && r->xLast == 6,
                      "C1 R's H curve spans columns 0..6 - the even columns, in place");
                check(gr && gr->xFirst == 1 && gr->xLast == 7,
                      "C1 Gr's spans 1..7: the two planes keep the IMAGE's x axis");
                const ProjCurveProbe* gbv = curve("Gb", false);
                check(gbv && gbv->xFirst == 1 && gbv->xLast == 7,
                      "C1 ...and Gb's V curve spans rows 1..7, its own rows");
            }

            // ======================================================================
            // C2 - DECIMATION IS STILL ON. The stroke runs every frame, and the whole
            // reason it decimates is that 4000 samples into 400 px is 4000 line
            // segments per series per frame. Walking each plane's own samples must not
            // undo that: 4096 columns is 2048 samples per Bayer plane, and the stride
            // is now taken over those 2048 rather than over the 4096 entries - which
            // is the point, because a mosaic plane used to be decimated `cell` times
            // harder than it needed to be and threw away half its resolution.
            //
            // The BOUND is stated the way the code computes it - nOwn / stride, with
            // the stride an integer floor - and not as "one point per pixel column":
            // 2048 samples over an ~880 px plot is a stride of 2 and therefore 1024
            // points, which is more than the pixels. That overshoot of up to 2x is the
            // floor division and is unchanged by this commit; the claim under test is
            // that the count is a FRACTION OF THE SAMPLES and not all of them.
            // ======================================================================
            {
                useOnly(mkFrame("pc_rggb_wide", 4096, 8, 1, 1));
                draw();
                dump("C2 RGGB 4096x8");
                const ProjCurveProbe* r = curve("R", true);
                check(r != nullptr, "C2 the wide frame's R plane has an H curve");
                if (r) {
                    check(r->lattice == 2048, "C2 fixture: 2048 samples in the R plane");
                    check(r->stride >= 2, "C2 the decimation is ON: stride is 2 or more");
                    check(r->points == r->lattice / r->stride,
                          "C2 the curve holds exactly lattice/stride points");
                    check(r->points <= r->lattice / 2,
                          "C2 ...so at most half the samples reach the draw list");
                    check(r->points <= 2 * (int)(r->px1 - r->px0),
                          "C2 ...and the integer stride overshoots the pixels by under 2x");
                    check(r->strokes == 1, "C2 ...in one unbroken stroke");
                    check(r->xFirst == 0 && r->xLast >= 4090,
                          "C2 ...spanning the frame, in the image's own columns");
                }
            }

            // ======================================================================
            // C3 - QUAD BAYER, where a second spelling of the phase would be caught.
            // At cfa == 2 the cell is 4 and a plane is TWO columns wide: cfaChannelAt
            // reads (x >> 1) & 1, so RGGB's R owns columns 0, 1, 4, 5, 8, 9, ... and
            // Gr owns 2, 3, 6, 7, ... An "even columns are R" formula - the obvious
            // one, and the wrong one - would put R on 0, 2, 4 and lose half its
            // samples while inventing the other half. 16 columns, 8 per plane.
            // ======================================================================
            {
                useOnly(mkFrame("pc_quad16", 16, 16, 2, 1));
                draw();
                dump("C3 quad Bayer 16x16");
                check(app.proj[0].cell == 4 && app.proj[0].nSeries == 4,
                      "C3 fixture: four planes on a 4x4 quad cell");
                for (int axis = 0; axis < 2; axis++) {
                    const bool horiz = axis == 0;
                    eachPlane(horiz, [&](const char* nm, const ProjCurveProbe* c) {
                        char what[128];
                        snprintf(what, sizeof what,
                                 "C3 quad plane %s has an %s curve of 8 points", nm,
                                 horiz ? "H" : "V");
                        check(c && c->points == 8 && c->strokes == 1 && c->lattice == 8, what);
                    });
                }
                const ProjCurveProbe* r = curve("R", true);
                const ProjCurveProbe* gr = curve("Gr", true);
                check(r && r->xFirst == 0 && r->xLast == 13,
                      "C3 R's columns are 0,1,4,5,...,12,13 - a plane two columns wide");
                check(gr && gr->xFirst == 2 && gr->xLast == 15,
                      "C3 Gr's start at 2, which an even/odd phase would get backwards");
            }

            // ======================================================================
            // C4 - A REAL GAP STILL BREAKS THE LINE (mono). Column 8 holds no finite
            // pixel at all, so the H profile has a NaN there that is a fact about the
            // DATA. The curve must break: two strokes, 15 points. The V profile of the
            // same frame must NOT break - every row still has 15 finite columns - so
            // the break is where the data is missing and nowhere else.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_gap_mono", 16, 16, 0, 1));
                for (int y = 0; y < 16; y++)
                    d->px()[(size_t)y * 16 + 8] = std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C4 mono with column 8 missing");
                const ProjCurveProbe* hh = curve("ch0", true);
                const ProjCurveProbe* vv = curve("ch0", false);
                check(hh && hh->strokes == 2 && hh->points == 15,
                      "C4 the H curve BREAKS at the missing column: 2 strokes, 15 points");
                check(hh && hh->lattice == 16,
                      "C4 ...and column 8 is still a sample POSITION - the data is what is gone");
                check(vv && vv->strokes == 1 && vv->points == 16,
                      "C4 the V curve is whole: no row lost its samples");
            }

            // ======================================================================
            // C5 - ...AND ON A MOSAIC PLANE, which is the proof that the two kinds of
            // hole are told apart. 16x16 RGGB with column 4 missing. Column 4 is even,
            // so it is on the lattice of R and of Gb and of neither Gr nor B: R and Gb
            // must break there (2 strokes, 7 of their 8 points) and Gr and B must not
            // (1 stroke, 8 points). An implementation that skipped every NaN would
            // draw all four whole and bridge the missing column in silence; the one
            // that broke at every NaN drew none of them at all.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_gap_rggb", 16, 16, 1, 1));
                for (int y = 0; y < 16; y++)
                    d->px()[(size_t)y * 16 + 4] = std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C5 RGGB with column 4 missing");
                for (const char* nm : { "R", "Gb" }) {
                    const ProjCurveProbe* c = curve(nm, true);
                    char what[128];
                    snprintf(what, sizeof what,
                             "C5 plane %s owns column 4 and BREAKS there: 2 strokes, 7 points", nm);
                    check(c && c->strokes == 2 && c->points == 7 && c->lattice == 8, what);
                }
                for (const char* nm : { "Gr", "B" }) {
                    const ProjCurveProbe* c = curve(nm, true);
                    char what[128];
                    snprintf(what, sizeof what,
                             "C5 plane %s is not on column 4 and stays whole: 8 points", nm);
                    check(c && c->strokes == 1 && c->points == 8 && c->lattice == 8, what);
                }
                eachPlane(false, [&](const char* nm, const ProjCurveProbe* c) {
                    char what[128];
                    snprintf(what, sizeof what, "C5 plane %s's V curve is whole: no row is gone", nm);
                    check(c && c->strokes == 1 && c->points == 8, what);
                });
            }

            // ======================================================================
            // C6 - THE ROI, which is the report's own situation: an ROI shrinks rw, so
            // stride falls to 1 on any frame however large. Eight columns of a 4096
            // wide mosaic, taken at an EVEN origin and then at an ODD one - because the
            // phase is a property of the absolute column, and a lattice spelled as
            // "even indices" would be right at 1000 and wrong at 1001.
            // ======================================================================
            {
                useOnly(mkFrame("pc_roi", 4096, 16, 1, 1));
                auto roi = [&](int x0) {
                    app.anns.clear();
                    App::Ann a;
                    a.id = app.nextAnnId++;
                    a.type = 0;
                    a.x = x0; a.y = 0; a.w = 8; a.h = 8;
                    a.label = "pcroi";
                    app.anns.push_back(a);
                    app.selectedAnn = a.id;
                    app.annRev++;
                    app.proj[0] = App::ProjState{};
                    draw();
                };
                roi(1000);
                dump("C6 ROI at x=1000 (even)");
                check(app.proj[0].roiUsed && app.proj[0].rx == 1000 && app.proj[0].rw == 8,
                      "C6 fixture: the ROI really is 8 columns from x=1000");
                eachPlane(true, [&](const char* nm, const ProjCurveProbe* c) {
                    char what[128];
                    snprintf(what, sizeof what, "C6 plane %s has an H curve inside the ROI", nm);
                    check(c && c->points == 4 && c->strokes == 1 && c->stride == 1, what);
                });
                const ProjCurveProbe* r = curve("R", true);
                check(r && r->xFirst == 1000 && r->xLast == 1006,
                      "C6 R's curve is at columns 1000..1006 of the IMAGE, not 0..6");
                // ...and the spread line under the plot, which is measured over the
                // VISIBLE x range. That range is column numbers while the profile is
                // indexed from 0, so at an ROI origin of 1000 every sample fell outside
                // the window and the line said nothing at all.
                check(field(g_projSideProbe, "spreadH=") == "4",
                      "C6 all four planes get a spread line under the H plot");
                check(field(g_projSideProbe, "spreadV=") == "4",
                      "C6 ...and under the V plot");

                roi(1001);
                dump("C6b ROI at x=1001 (odd)");
                const ProjCurveProbe* r2 = curve("R", true);
                const ProjCurveProbe* gr2 = curve("Gr", true);
                check(r2 && r2->points == 4 && r2->xFirst == 1002 && r2->xLast == 1008,
                      "C6b at an ODD origin R starts at column 1002: the phase follows the image");
                check(gr2 && gr2->points == 4 && gr2->xFirst == 1001,
                      "C6b ...and Gr starts at 1001, the first column of the ROI");
                app.anns.clear();
                app.selectedAnn = 0;
                app.annRev++;
            }

            // ======================================================================
            // C7 - THE HOVER READOUT, ABOUT THE CELL. "There is a spike - which
            // column is it?" is what it is for, and on a mosaic two of the four
            // planes have no sample at any given column. Naming one column and
            // then saying nothing about half the planes is the worst of the
            // three available answers, on the panel whose purpose with a mosaic
            // is comparing the planes at one PLACE. So with a mosaic on the plot
            // the reading is about the cell: the heading is "column x 2-3" and
            // all four planes get a line, each with the column its own sample
            // came from.
            //
            // Aimed at column 3 of an 8x8 RGGB - an odd column, so Gr and B
            // have their samples there and R and Gb have theirs at column 2.
            // ======================================================================
            {
                useOnly(mkFrame("pc_hover", 8, 8, 1, 1));
                draw();
                const ProjCurveProbe* r = curve("R", true);
                check(r != nullptr, "C7 fixture: the H plot was laid out");
                if (r) {
                    // the plot's own mapping, from the rect and the axis it recorded
                    const float t = (3.0f - r->xmin) / std::max(r->xmax - r->xmin, 1e-6f);
                    const ImVec2 aim(r->px0 + t * (r->px1 - r->px0),
                                     (r->py0 + r->py1) * 0.5f);
                    projFrame(900.0f, 900.0f, aim);
                    fprintf(stderr, "abstatsselftest: C7 hover '%s'\n",
                            g_projHoverProbe.c_str());
                    const std::string t2 = g_projHoverProbe;
                    auto has = [&](const char* sub) { return t2.find(sub) != std::string::npos; };
                    check(has("column x 2-3") && has("one mosaic cell"),
                          "C7 the heading names the CELL, and says that is what it is");
                    check(has("\nR: ") && has("\nGr: ") && has("\nGb: ") && has("\nB: "),
                          "C7 ...and all FOUR planes get a line, not the two on column 3");
                    check(has("at column 2") && has("at column 3"),
                          "C7 ...each saying which column its own sample came from");
                    check(!has("no sample") && !has("no data"),
                          "C7 ...and none of them has to refuse: every plane is in the cell");
                }
            }

            // ======================================================================
            // C7b - quad Bayer, where a cell is FOUR columns and a plane owns
            // two of them. The line quoted is the one nearest the cursor, so
            // the answer is still "the sample under your pointer" and the
            // heading still names what the four values have in common.
            // ======================================================================
            {
                useOnly(mkFrame("pc_hover_quad", 16, 16, 2, 1));
                draw();
                const ProjCurveProbe* r = curve("R", true);
                check(r != nullptr, "C7b fixture: the quad H plot was laid out");
                if (r) {
                    const float t = (5.0f - r->xmin) / std::max(r->xmax - r->xmin, 1e-6f);
                    projFrame(900.0f, 900.0f,
                              ImVec2(r->px0 + t * (r->px1 - r->px0),
                                     (r->py0 + r->py1) * 0.5f));
                    fprintf(stderr, "abstatsselftest: C7b hover '%s'\n",
                            g_projHoverProbe.c_str());
                    const std::string t3 = g_projHoverProbe;
                    check(t3.find("column x 4-7") != std::string::npos,
                          "C7b a quad cell is four columns wide, and the heading says so");
                    check(t3.find("\nR: ") != std::string::npos &&
                          t3.find("\nGr: ") != std::string::npos &&
                          t3.find("\nGb: ") != std::string::npos &&
                          t3.find("\nB: ") != std::string::npos,
                          "C7b ...and all four planes are in it");
                    check(t3.find("at column 5") != std::string::npos,
                          "C7b ...R quoting column 5, the one under the cursor of its two");
                    check(t3.find("no sample") == std::string::npos,
                          "C7b ...and nothing refuses");
                }
            }

            // ======================================================================
            // C8 - ...and on a plain image the heading is still ONE column, and
            // a hole in the data is called "no data". Two different facts about
            // a missing value - the mosaic's lattice and a column no finite
            // pixel was found in - and two different words for them.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_hover_gap", 16, 16, 0, 1));
                for (int y = 0; y < 16; y++)
                    d->px()[(size_t)y * 16 + 8] = std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                const ProjCurveProbe* c = curve("ch0", true);
                check(c != nullptr, "C8 fixture: the H plot was laid out");
                if (c) {
                    const float t = (8.0f - c->xmin) / std::max(c->xmax - c->xmin, 1e-6f);
                    projFrame(900.0f, 900.0f,
                              ImVec2(c->px0 + t * (c->px1 - c->px0), (c->py0 + c->py1) * 0.5f));
                    fprintf(stderr, "abstatsselftest: C8 hover '%s'\n",
                            g_projHoverProbe.c_str());
                    check(g_projHoverProbe.find("column x 8") != std::string::npos &&
                          g_projHoverProbe.find("mosaic cell") == std::string::npos,
                          "C8 a plain image's heading is one column, with no cell in it");
                    check(g_projHoverProbe.find("no data (no finite pixel in column 8)") !=
                              std::string::npos,
                          "C8 ...and a missing MEASUREMENT is 'no data', naming the column");
                    check(g_projHoverProbe.find("no sample") == std::string::npos,
                          "C8 ...never 'no sample', which would blame a mosaic it has not got");
                }
            }

            // ======================================================================
            // C9 - the plane selector still narrows the plot to one plane, and the one
            // it names is the one that gets the curve. The filter runs before the walk,
            // so this is the assert that the walk did not start ignoring it.
            // ======================================================================
            {
                useOnly(mkFrame("pc_one_plane", 8, 8, 1, 1));
                app.histPlane = 3;                       // B
                draw();
                dump("C9 plane=B only");
                check(g_projCurveProbe.size() == 2,
                      "C9 one plane on the axis is two curves - H and V - and no more");
                const ProjCurveProbe* b = curve("B", true);
                check(b && b->points == 4, "C9 ...and the one drawn is B, with its four points");
                check(curve("R", true) == nullptr, "C9 R is filtered out, not drawn empty");
                app.histPlane = -1;
            }

            // ======================================================================
            // C13 - THE AUTO VALUE AXIS FOLLOWS THE PLANE SELECTOR. A colour
            // filter array is what puts the planes at DIFFERENT LEVELS, so an
            // axis taken over all four is three levels taller than the plane
            // being read - and a lens-shading tilt of a few percent inside one
            // plane is then a flat line. The fixture's levels are 500 + 400*p
            // plus a 0..87 ramp along x, so all four planes span ~500..1787 and
            // R alone spans ~500..590 (the +y term adds 0..6 to a column mean).
            // ======================================================================
            {
                useOnly(mkFrame("pc_yaxis", 64, 8, 1, 1));
                app.histPlane = -1;
                draw();
                const std::string yAll = field(g_projSideProbe, "y=");
                app.histPlane = 0;                    // R only
                draw();
                const std::string yR = field(g_projSideProbe, "y=");
                fprintf(stderr, "abstatsselftest: C13 axis all planes '%s' / R only '%s'\n",
                        yAll.c_str(), yR.c_str());
                auto span = [](const std::string& f) {
                    const size_t d = f.find("..");
                    if (d == std::string::npos) return 0.0;
                    return atof(f.c_str() + d + 2) - atof(f.c_str());
                };
                check(span(yAll) > 1000.0,
                      "C13 fixture: the four planes really are a thousand DN apart");
                check(span(yR) > 0.0 && span(yR) < 200.0,
                      "C13 one plane on the axis gets an axis that fits ONE plane");
                check(span(yR) < span(yAll) / 4.0,
                      "C13 ...which is the whole point: the shape is no longer flattened");
                app.histPlane = -1;
            }

            // ======================================================================
            // C14 - THE TABLE'S HEIGHT BUDGET IS THE ROW WALK. A is a mosaic
            // (R, Gr, Gb, B) and B was never told it is one (ch0), so in "ch,
            // then side" the row axis is five plane NAMES times two sides = ten
            // rows. The budget summed the sides' plane counts instead - 4 + 1 -
            // and reserved five, so half the table hung below the visible panel
            // on the very pair the "-" rows exist to make readable.
            // ======================================================================
            {
                app.images.clear();
                app.images.push_back(mkFrame("pc_rows_A", 16, 16, 1, 1));
                app.images.push_back(mkFrame("pc_rows_B", 16, 16, 0, 1));
                app.current = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                const int keepOrder2 = app.projStatOrder;
                app.projStatOrder = 1;                 // "ch, then side"
                draw();
                const std::string rows = field(g_projSideProbe, "planerows=");
                const std::string budget = field(g_projSideProbe, "statrows=");
                fprintf(stderr, "abstatsselftest: C14 rows '%s' budget '%s'\n",
                        rows.c_str(), budget.c_str());
                check(rows == "R@A,R@B-,Gr@A,Gr@B-,Gb@A,Gb@B-,B@A,B@B-,ch0@A-,ch0@B",
                      "C14 fixture: five plane names across two sides is ten rows");
                check(budget == "10",
                      "C14 ...and the height budget is those ten, not the 4+1 the sum gave");
                app.projStatOrder = keepOrder2;
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
            }

            // ======================================================================
            // C14b - A SERIES ONLY ONE SIDE HAS IS STILL FOUR DIFFERENT CURVES.
            // A plane hue means "A's plane of this name", so it cannot be lent
            // to a plane A has not got - but the one neutral ink that rule left
            // behind put B's R, Gr, Gb and B on the plot in a single grey,
            // overlaid and impossible to tell apart. Four neutral tones,
            // separated by luminance so they still read as "not a plane
            // colour", and each one NAMED with the letter it belongs to.
            // ======================================================================
            {
                app.images.clear();
                app.images.push_back(mkFrame("pc_odd_A", 16, 16, 0, 1));   // mono
                app.images.push_back(mkFrame("pc_odd_B", 16, 16, 1, 1));   // mosaic
                app.current = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                draw();
                dump("C14b A mono against a mosaic B");
                check(field(g_projSideProbe, "planelegH=") ==
                          "ch0,R (B only),Gr (B only),Gb (B only),B (B only)",
                      "C14b every series B alone has is named, with the letter");
                // ANTI-VACUOUS: they are four DIFFERENT curves on the plot, so
                // the legend is naming four things that can be told apart.
                check(curve("R", true) && curve("Gr", true) &&
                      curve("Gb", true) && curve("B", true) &&
                      curve("R", true)->points == 8,
                      "C14b ...and all four are really drawn, 8 points each");
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
            }

            // ======================================================================
            // C11 - A PLANE WITH NO CURVE IS NAMED. This panel keeps that
            // discipline for the LETTERS - a slot the plots cannot carry is
            // named under them in amber, which is the whole of the #60 fix -
            // and kept none of it for the planes. Two columns of a mosaic give
            // every plane ONE sample, a line needs two, and before this the
            // plots were empty, nothing said so, and the table below still
            // gave all four planes a full row of numbers. The same silence one
            // level down.
            // ======================================================================
            {
                useOnly(mkFrame("pc_silent", 4096, 16, 1, 1));
                app.anns.clear();
                App::Ann a2;
                a2.id = app.nextAnnId++;
                a2.type = 0;
                a2.x = 100; a2.y = 0; a2.w = 2; a2.h = 16;
                a2.label = "pcthin";
                app.anns.push_back(a2);
                app.selectedAnn = a2.id;
                app.annRev++;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C11 a 2-column ROI on a mosaic");
                check(app.proj[0].rw == 2, "C11 fixture: the ROI is two columns wide");
                eachPlane(true, [&](const char* nm, const ProjCurveProbe* c) {
                    char what[128];
                    snprintf(what, sizeof what,
                             "C11 plane %s really has one sample and no curve", nm);
                    check(c && c->lattice == 1 && c->points == 0, what);
                });
                check(field(g_projSideProbe, "drewH=").empty(),
                      "C11 the panel drew no H curve at all");
                check(field(g_projSideProbe, "said-no-curve-planeH=") ==
                          "R@A,Gr@A,Gb@A,B@A",
                      "C11 ...and names every plane it left off, BY PLANE and side");
                // ANTI-VACUOUS: the V axis of the same frame is 16 rows, so
                // every plane has 8 of them and is drawn. A probe that said
                // "no curve" unconditionally would pass the check above.
                check(field(g_projSideProbe, "drewV=") == "R@A,Gr@A,Gb@A,B@A" &&
                      field(g_projSideProbe, "said-no-curve-planeV=").empty(),
                      "C11 the V axis of the same ROI has all four, and says so");
                app.anns.clear();
                app.selectedAnn = 0;
                app.annRev++;
            }

            // ======================================================================
            // C12 - WHICH HUE IS WHICH PLANE. Four planes were overlaid in four
            // hues - Gr and Gb being two greens - with nothing on screen naming
            // them, on the panel whose purpose with a mosaic is comparing the
            // planes. One name is not a legend, so a mono image gets none: the
            // plane selector already says which plane a single curve is.
            // ======================================================================
            {
                useOnly(mkFrame("pc_legend", 8, 8, 1, 1));
                draw();
                fprintf(stderr, "abstatsselftest: C12 sides '%s'\n",
                        g_projSideProbe.c_str());
                check(field(g_projSideProbe, "planelegH=") == "R,Gr,Gb,B",
                      "C12 the plane legend names all four mosaic planes, in order");
                check(field(g_projSideProbe, "planelegV=") == "R,Gr,Gb,B",
                      "C12 ...under both plots");
                app.histPlane = 3;
                draw();
                check(field(g_projSideProbe, "planelegH=").empty(),
                      "C12 one plane on the axis needs no legend - the selector names it");
                app.histPlane = -1;
                useOnly(mkFrame("pc_legend_mono", 8, 8, 0, 1));
                draw();
                check(field(g_projSideProbe, "planelegH=").empty(),
                      "C12 ...and a mono image has no planes to tell apart");
            }

            // ======================================================================
            // C10 - THE POOLED ROW IS SLOT 4. The spread lines walked `k < nSeries +
            // allRow`, which on a 3-channel image with "all" ticked is 0..3 - and slot
            // 3 is not the pooled row, it is "ch3", a vector the pass never filled and
            // that a previously open mosaic may have left data in. So the "all" line
            // never appeared and a stale plane could appear under its name. Four lines:
            // ch0, ch1, ch2, all. (The same count/slot confusion the projection pass
            // carries a paragraph about; it survived here because on a 4-plane mosaic
            // the count is accidentally right.)
            // ======================================================================
            {
                useOnly(mkFrame("pc_rgb", 16, 16, 0, 3));
                app.projAllRow = true;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C10 RGB 16x16 with the pooled row on");
                check(app.proj[0].nSeries == 3 && app.proj[0].allRow,
                      "C10 fixture: three channels plus the pooled row");
                check(field(g_projSideProbe, "spreadH=") == "4",
                      "C10 four spread lines: the three channels AND the pooled row");
                check(g_projCurveProbe.size() == 6,
                      "C10 ...while the pooled row is still not a CURVE: three planes, two axes");
                app.projAllRow = false;
            }

            // ======================================================================
            // C15 - A MOSAIC PREVIEW HAS NO PROFILE, AND SAYS SO - PER SIDE.
            // A decimated remote preview carries every Nth row and column of
            // the sensor, so the mosaic phase is not preserved: cfaChannelAt
            // over preview coordinates names a plane the pixel is not from, and
            // every plane's curve was another plane's values under this plane's
            // name. Four wrong curves and four wrong columns of numbers, with
            // nothing on screen distinguishing them from the real thing.
            //
            // The refusal belongs to THE SIDE, and the panel goes on drawing the
            // others. Stopping the whole panel because A is a preview took B's
            // and C's curves and the entire statistics table with them, without
            // a word - a slot measured perfectly well, blanked because a
            // different slot is a preview.
            // ======================================================================
            {
                app.images.clear();
                app.images.push_back(mkFrame("pc_preview_A", 16, 16, 1, 1));
                app.images.push_back(mkFrame("pc_preview_B", 16, 16, 1, 1));
                app.current = 0;
                app.images[0]->src->remoteStep = 2;   // A is a preview, as the loader marks one
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                draw();
                dump("C15 A is a mosaic preview, B is full resolution");
                fprintf(stderr, "abstatsselftest: C15 notices '%s'\n",
                        g_projUnavailProbe.c_str());
                check(!app.proj[0].unavail.empty() && app.proj[1].unavail.empty(),
                      "C15 A's profile is refused and B's is not");
                check(app.proj[0].unavail.find("full-resolution remote pixels required") == 0,
                      "C15 ...in computeProfileNoise's words for the same cause");
                check(field(g_projSideProbe, "unavail=") == "A",
                      "C15 ...and the panel names A, and only A");
                check(g_projUnavailProbe.find("A: no profile - full-resolution") == 0,
                      "C15 ...with A's OWN reason in the notice, not an empty string");
                check(app.proj[0].h[0].empty() && !app.proj[0].hStat[0].valid,
                      "C15 A has no profile and no numbers - the half that was wrong");
                // A's planes are named as having no curve, rather than silently
                // absent; B's four are drawn.
                check(field(g_projSideProbe, "said-no-curve-planeH=") ==
                          "R@A,Gr@A,Gb@A,B@A",
                      "C15 A's four planes are NAMED as having no curve");
                for (const char* nm : { "R", "Gr", "Gb", "B" }) {
                    char what[128];
                    snprintf(what, sizeof what,
                             "C15 B's plane %s keeps its curve: A's refusal is not B's", nm);
                    const ProjCurveProbe* c = nullptr;
                    for (const auto& e : g_projCurveProbe)
                        if (e.plane == nm && e.horizontal && e.side == "B") c = &e;
                    check(c && c->points == 8 && c->strokes == 1, what);
                }
                check(field(g_projSideProbe, "drewH=") == "R@B,Gr@B,Gb@B,B@B",
                      "C15 ...and the panel says B is what it drew");
                // ...and it does NOT send the reader to A's rows, which are all
                // dashes: every silent plane here belongs to the refused side.
                fprintf(stderr, "abstatsselftest: C15 no-curve '%s'\n",
                        g_projNoCurveProbe.c_str());
                check(g_projNoCurveProbe.find("no profile on this side") !=
                          std::string::npos,
                      "C15 A's planes say the SIDE has no profile");
                check(g_projNoCurveProbe.find("no sample position in this region") ==
                          std::string::npos,
                      "C15 ...and never that the REGION holds no position: it holds them");
                check(g_projNoCurveProbe.find("table below") == std::string::npos,
                      "C15 ...and the reader is not pointed at a table of dashes");
                // ...and the HOVER says the same thing about the same side. This
                // is the sentence that was still making the region claim three
                // lines from the one that had stopped.
                {
                    const ProjCurveProbe* bc = nullptr;
                    for (const auto& e : g_projCurveProbe)
                        if (e.side == "B" && e.horizontal && e.plane == "R") bc = &e;
                    check(bc != nullptr, "C15 fixture: B's R curve was laid out");
                    if (bc) {
                        const float t = (6.0f - bc->xmin) /
                                        std::max(bc->xmax - bc->xmin, 1e-6f);
                        projFrame(900.0f, 900.0f,
                                  ImVec2(bc->px0 + t * (bc->px1 - bc->px0),
                                         (bc->py0 + bc->py1) * 0.5f));
                        fprintf(stderr, "abstatsselftest: C15 hover '%s'\n",
                                g_projHoverProbe.c_str());
                        check(g_projHoverProbe.find("A R: no profile on this side") !=
                                  std::string::npos,
                              "C15 hover says A has no PROFILE, side by side with B's values");
                        check(g_projHoverProbe.find("no position in this region") ==
                                  std::string::npos,
                              "C15 ...and makes no claim about the region's positions");
                        check(g_projHoverProbe.find("B R: ") != std::string::npos,
                              "C15 ...while B's own value is still read out");
                    }
                }
                // ...and the reason comes from THE SIDE IT IS ABOUT. With the
                // refused side past B, reading sides[1] printed nothing at all.
                addCompareSlot(app.images[0].get());       // C, the same preview doc
                selectImage(1);                            // A is now the full-res frame
                setCompareB(app.images[1].get());
                app.abStepBusyUntil = 0;
                app.proj[0] = App::ProjState{};
                draw();
                fprintf(stderr, "abstatsselftest: C15b unavail='%s' notices '%s'\n",
                        field(g_projSideProbe, "unavail=").c_str(),
                        g_projUnavailProbe.c_str());
                check(field(g_projSideProbe, "unavail=") == "C",
                      "C15b the refused side is C now, and the panel says C");
                check(g_projUnavailProbe.find("C: no profile - full-resolution") == 0,
                      "C15b ...with C's own reason, which sides[1] would have left empty");
                // ...and the OFF-PLOT notice, which carries the same two clauses
                // the per-plot one does and had a gate on neither: C is off the
                // shared axis, and it is off it because it has no profile - so
                // "pick ONE plane above" is a remedy that cannot work and its
                // numbers are a row of dashes.
                fprintf(stderr, "abstatsselftest: C15b off-plot '%s'\n",
                        g_projOffProbe.c_str());
                check(field(g_projSideProbe, "said-no-curve=") == "C",
                      "C15b fixture: the plot really did leave C off");
                check(g_projOffProbe.find("slot C: no curve - no profile at all") !=
                          std::string::npos,
                      "C15b the off-plot notice names C's own reason");
                check(g_projOffProbe.find("pick ONE plane") == std::string::npos,
                      "C15b ...and does not offer a remedy that cannot work");
                check(g_projOffProbe.find("in the table below") == std::string::npos,
                      "C15b ...nor promise numbers C does not have");
                app.cmpExtra.clear();
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
                app.projExtra.clear();
                app.images[0]->src->remoteStep = 1;
                // ANTI-VACUOUS: the same frame at full resolution has all four.
                useOnly(mkFrame("pc_preview_full", 16, 16, 1, 1));
                draw();
                check(app.proj[0].unavail.empty() && curve("R", true) &&
                      curve("R", true)->points == 8,
                      "C15 the full-resolution frame draws its four planes as before");
            }

            // ======================================================================
            // C16 - ON A MOSAIC THE POOLED ROW HAS NO PROFILE AT ALL. Pooling
            // the planes makes a DENSE column mean out of their level
            // difference: every column carries the same mix of planes, so the
            // profile is perfectly plottable and means the inter-plane offset.
            // On this 16x16 RGGB fixture its p-p is 415 DN, of which 400 is the
            // step between the planes and 15 is all the column structure there
            // is - and it was appearing under the plots as
            // "all in view [0 .. 15]: p-p 415 (37.38% of mean 1110.19)" three
            // lines above a footnote saying p-p was unavailable.
            //
            // So: no profile, no p-p line, no axis-direction statistics. The
            // pooled REGION moments - what the row was added for - stand.
            // ======================================================================
            {
                useOnly(mkFrame("pc_pooled", 16, 16, 1, 1));
                app.projAllRow = true;
                app.proj[0] = App::ProjState{};
                draw();
                const App::ProjState& PP = app.proj[0];
                fprintf(stderr, "abstatsselftest: C16 pooled: profile %zu long, fStat %d, "
                                "hStat %d, vStat %d, nFill %d\n",
                        PP.h[4].size(), PP.fStat[4].valid ? 1 : 0,
                        PP.hStat[4].valid ? 1 : 0, PP.vStat[4].valid ? 1 : 0, PP.nFill);
                fprintf(stderr, "abstatsselftest: C16 sides '%s'\n",
                        g_projSideProbe.c_str());
                check(PP.allRow, "C16 fixture: the pooled row is switched on");
                check(PP.h[4].empty() && PP.v[4].empty(),
                      "C16 the pooled PROFILE is not built on a mosaic");
                check(PP.nFill == 4,
                      "C16 ...so the walk that draws p-p lines never reaches it");
                check(field(g_projSideProbe, "spreadH=") == "4" &&
                      field(g_projSideProbe, "spreadV=") == "4",
                      "C16 four p-p lines under each plot - the planes, and not 'all'");
                check(!PP.hStat[4].valid && !PP.vStat[4].valid,
                      "C16 no axis-direction statistics either: unavailable: pooled planes");
                check(PP.fStat[4].valid && PP.fStat[4].mean > 0 && PP.fStat[4].n > 0,
                      "C16 ...while the pooled REGION mean and sigma are over pixels and stand");
                // ANTI-VACUOUS: on a 3-channel image the pooled row is still a
                // pooled row, the planes are not a mosaic's, there is no period
                // in the pooling - so it keeps its profile, its p-p line and its
                // profile sigma, and spreadH is four for a DIFFERENT reason
                // (three channels plus the pooled row).
                useOnly(mkFrame("pc_pooled_rgb", 16, 16, 0, 3));
                app.proj[0] = App::ProjState{};
                draw();
                fprintf(stderr, "abstatsselftest: C16 RGB sides '%s'\n",
                        g_projSideProbe.c_str());
                check(app.proj[0].h[4].size() == 16 && app.proj[0].nFill == 4,
                      "C16 a non-mosaic pooled row DOES get a profile");
                check(app.proj[0].hStat[4].valid && app.proj[0].vStat[4].valid,
                      "C16 ...and keeps its profile sigma");
                check(field(g_projSideProbe, "spreadH=") == "4",
                      "C16 ...and its p-p line: three channels plus 'all'");
                app.projAllRow = false;
            }

            // ======================================================================
            // C17 - ONE SAMPLE IS NOT A SIGMA OF ZERO. A 2-column ROI gives a
            // Bayer plane ONE column mean, the ddof=1 variance of one sample is
            // 0 by construction, and `valid` published that exact zero - which
            // reads as a perfectly uniform sensor rather than as "there is
            // nothing here to measure".
            // ======================================================================
            {
                useOnly(mkFrame("pc_one_sample", 4096, 16, 1, 1));
                auto roiW = [&](int w) {
                    app.anns.clear();
                    App::Ann a3;
                    a3.id = app.nextAnnId++;
                    a3.type = 0;
                    a3.x = 200; a3.y = 0; a3.w = w; a3.h = 16;
                    a3.label = "pcone";
                    app.anns.push_back(a3);
                    app.selectedAnn = a3.id;
                    app.annRev++;
                    app.proj[0] = App::ProjState{};
                    draw();
                };
                roiW(2);
                fprintf(stderr, "abstatsselftest: C17 2-wide ROI: hStat valid=%d sd=%.17g\n",
                        app.proj[0].hStat[0].valid ? 1 : 0, app.proj[0].hStat[0].sd);
                check(app.proj[0].h[0].size() == 2,
                      "C17 fixture: the R plane has one column mean in a 2-column ROI");
                check(!app.proj[0].hStat[0].valid,
                      "C17 one sample publishes no sigma_col at all");
                // ANTI-VACUOUS: four columns is two samples, which IS a spread.
                roiW(4);
                fprintf(stderr, "abstatsselftest: C17 4-wide ROI: hStat valid=%d sd=%.17g\n",
                        app.proj[0].hStat[0].valid ? 1 : 0, app.proj[0].hStat[0].sd);
                check(app.proj[0].hStat[0].valid && app.proj[0].hStat[0].sd > 0,
                      "C17 two samples do, and it is not zero");
                app.anns.clear();
                app.selectedAnn = 0;
                app.annRev++;
            }

            // ======================================================================
            // C18 - THE LATTICE IS WHERE THE PASS LOOKED, NOT WHERE IT FOUND
            // SOMETHING. The phase bits were being set inside put(), which runs
            // only for FINITE values - so an offset whose every sample is
            // non-finite looked like an offset the plane does not have, the
            // stroke walked straight past it, and it JOINED THE TWO SIDES OF A
            // REAL GAP. That is the one distinction this whole change exists to
            // draw, lost in the case where it matters most.
            //
            // The fixture is quad Bayer, where a plane owns TWO offsets of the
            // four - R owns x mod 4 in {0, 1} - and every pixel at offset 0 is
            // NaN. 32 columns, so R has 16 sample positions, 8 of them dead.
            //   before: the offset-0 bit never set, lattice 8, one unbroken
            //           8-point curve from column 1 to column 29 - bridging
            //           seven dead columns in silence
            //   after:  lattice 16, and no two live samples are adjacent on it,
            //           so there is NO LINE TO DRAW - and the panel says which
            //           plane it left off instead of drawing a lie
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_dead_phase", 32, 32, 2, 1));
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 32; x++)
                        if ((x % 4) == 0 && cfaChannelAt(*d, x, y) == 0)
                            d->px()[(size_t)y * 32 + x] =
                                std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C18 quad Bayer with R's offset 0 dead");
                const App::ProjState& PQ = app.proj[0];
                check(PQ.cell == 4, "C18 fixture: a quad cell, so a plane owns two offsets");
                check(std::isnan(PQ.h[0][0]) && std::isnan(PQ.h[0][4]) &&
                      std::isfinite(PQ.h[0][1]) && std::isfinite(PQ.h[0][5]),
                      "C18 fixture: R's offset-0 columns really hold no finite pixel");
                const ProjCurveProbe* r = curve("R", true);
                check(r && r->lattice == 16,
                      "C18 the dead offset is still a POSITION R has: lattice 16, not 8");
                check(r && r->points == 0,
                      "C18 ...so no two live samples are adjacent and no line is drawn");
                check(field(g_projSideProbe, "said-no-curve-planeH=") == "R@A",
                      "C18 ...and the panel names R rather than bridging the gap");
                // ANTI-VACUOUS: Gr owns offsets 2 and 3, both alive, and is a
                // whole 16-point curve on the same frame.
                const ProjCurveProbe* g = curve("Gr", true);
                check(g && g->lattice == 16 && g->points == 16 && g->strokes == 1,
                      "C18 Gr, whose offsets are both alive, is one whole curve");
                check(field(g_projSideProbe, "drewH=") == "Gr@A,Gb@A,B@A",
                      "C18 ...and the other three planes are what was drawn");
            }

            // ======================================================================
            // C19 - THE "all" CHECKBOX TAKES EFFECT AT ONCE. app.projAllRow was
            // not in the cache key, so ticking it hit the cache and did nothing
            // until some OTHER condition changed - move the ROI, step a frame -
            // and the row then appeared for a reason the reader did not do.
            // Asserted on a non-mosaic image, where the pooled row has a
            // profile, so the change is visible in the walk itself.
            // ======================================================================
            {
                useOnly(mkFrame("pc_allrow_key", 16, 16, 0, 3));
                app.projAllRow = false;
                draw();
                const int fillOff = app.proj[0].nFill;
                const bool rowOff = app.proj[0].allRow;
                app.projAllRow = true;          // ...and NOTHING else changes
                draw();
                const int fillOn = app.proj[0].nFill;
                fprintf(stderr, "abstatsselftest: C19 nFill %d -> %d, allRow %d -> %d\n",
                        fillOff, fillOn, rowOff ? 1 : 0, app.proj[0].allRow ? 1 : 0);
                check(fillOff == 3 && !rowOff,
                      "C19 fixture: three channels and no pooled row");
                check(fillOn == 4 && app.proj[0].allRow && app.proj[0].h[4].size() == 16,
                      "C19 ticking 'all' recomputes on the very next frame");
                app.projAllRow = false;
                draw();
                check(app.proj[0].nFill == 3 && !app.proj[0].allRow,
                      "C19 ...and un-ticking it does too, with nothing else touched");
            }

            // ======================================================================
            // C20 - THE p-p LINES ARE RESERVED FOR. One "in view" line per
            // series is drawn under each plot, and the height budget allowed
            // ONE line per plot - so a mosaic's four (five with the pooled row)
            // pushed the statistics table below the visible panel, exactly where
            // the numbers are read.
            //
            // Asserted as the relation between the two numbers rather than as a
            // plot height: `each` has a 60 px floor, so on a panel with little
            // room both budgets clamp to the same height and a height
            // comparison passes or fails on how much space the test window
            // happened to have. What has to be true is that the reservation is
            // the number of lines the spread loop will DRAW - which is the
            // 1-against-4 the defect was.
            // ======================================================================
            {
                useOnly(mkFrame("pc_reserve", 16, 16, 1, 1));       // a mosaic: four
                app.projAllRow = false;
                draw();
                fprintf(stderr, "abstatsselftest: C20 mosaic pprows=%s spreadH=%s "
                                "(plot %.1f px)\n",
                        field(g_projSideProbe, "pprows=").c_str(),
                        field(g_projSideProbe, "spreadH=").c_str(),
                        curve("R", true) ? curve("R", true)->py1 - curve("R", true)->py0 : 0.0f);
                check(field(g_projSideProbe, "pprows=") == "4" &&
                      field(g_projSideProbe, "spreadH=") == "4",
                      "C20 a mosaic reserves four p-p rows and draws four");
                useOnly(mkFrame("pc_reserve_rgb", 16, 16, 0, 3));   // three + pooled
                app.projAllRow = true;
                draw();
                fprintf(stderr, "abstatsselftest: C20 RGB+all pprows=%s spreadH=%s\n",
                        field(g_projSideProbe, "pprows=").c_str(),
                        field(g_projSideProbe, "spreadH=").c_str());
                check(field(g_projSideProbe, "pprows=") == "4" &&
                      field(g_projSideProbe, "spreadH=") == "4",
                      "C20 ...and three channels plus the pooled row reserve four too");
                // ANTI-VACUOUS: a single plane reserves ONE, so "4" above is not
                // a constant the panel always prints.
                app.projAllRow = false;
                useOnly(mkFrame("pc_reserve_mono", 16, 16, 0, 1));
                draw();
                fprintf(stderr, "abstatsselftest: C20 mono pprows=%s spreadH=%s\n",
                        field(g_projSideProbe, "pprows=").c_str(),
                        field(g_projSideProbe, "spreadH=").c_str());
                check(field(g_projSideProbe, "pprows=") == "1" &&
                      field(g_projSideProbe, "spreadH=") == "1",
                      "C20 one plane reserves one - the budget follows the series count");
            }

            // ======================================================================
            // C21 - A PLANE THE PASS NEVER LANDED ON HAS NO SAMPLE POSITIONS,
            // and that is a THIRD answer, different from "its positions hold
            // nothing finite".
            //
            // A 1 px high ROI is a supported gesture (a band), and the H pass
            // walks `for (y = 0; y < rh; y += step) for (dy = 0; dy < cell &&
            // y + dy < rh; dy++)`, so at rh == 1 only dy == 0 runs and only
            // even rows are seen. cfaChannelAt returns R and Gr there, so Gb
            // and B never enter the plane loop at all and their phase word
            // stays 0.
            //
            // Reading that 0 as "no constraint - walk every position" made a
            // plane with NOTHING claim every column of the region: "Gb@A: 16 of
            // column 16" in the same sentence as "fewer samples than two", and
            // a hover that said "no data (no finite pixel in column 6)" - a
            // claim about the sensor that is not true. The right answer is that
            // this region holds no Gb position, and the words for it were
            // already three lines away and unreachable.
            // ======================================================================
            {
                useOnly(mkFrame("pc_band", 4096, 16, 1, 1));
                app.anns.clear();
                App::Ann a4;
                a4.id = app.nextAnnId++;
                a4.type = 0;
                a4.x = 200; a4.y = 0; a4.w = 16; a4.h = 1;    // a 1 px high band
                a4.label = "pcband";
                app.anns.push_back(a4);
                app.selectedAnn = a4.id;
                app.annRev++;
                app.proj[0] = App::ProjState{};
                draw();
                dump("C21 a 1 px high ROI on a mosaic");
                fprintf(stderr, "abstatsselftest: C21 no-curve '%s'\n",
                        g_projNoCurveProbe.c_str());
                check(app.proj[0].rh == 1 && app.proj[0].rw == 16,
                      "C21 fixture: the ROI is 16 columns and ONE row");
                check(app.proj[0].hPhase[2] == 0 && app.proj[0].hPhase[3] == 0,
                      "C21 fixture: the pass never landed on Gb or B at all");
                check(app.proj[0].hPhase[0] != 0 && app.proj[0].hPhase[1] != 0,
                      "C21 fixture: ...and it did land on R and Gr");
                // R and Gr have eight positions each and are drawn.
                const ProjCurveProbe* r = curve("R", true);
                const ProjCurveProbe* g = curve("Gr", true);
                check(r && r->lattice == 8 && r->points == 8,
                      "C21 R has its eight columns of the band and a curve");
                check(g && g->lattice == 8 && g->points == 8,
                      "C21 ...and so does Gr");
                // Gb and B have NO positions - not sixteen.
                for (const char* nm : { "Gb", "B" }) {
                    const ProjCurveProbe* c = curve(nm, true);
                    char what[144];
                    snprintf(what, sizeof what,
                             "C21 plane %s has ZERO sample positions here, not the "
                             "region's 16", nm);
                    check(c && c->lattice == 0 && c->points == 0, what);
                }
                check(g_projNoCurveProbe.find("16 of column 16") == std::string::npos,
                      "C21 the sentence does not claim 16 positions for a plane with none");
                check(g_projNoCurveProbe.find("no sample position in this region") !=
                          std::string::npos,
                      "C21 ...it says the region holds no position of that plane");
                check(g_projNoCurveProbe.find("fewer samples than") == std::string::npos,
                      "C21 ...and does not also call it 'fewer than two'");
                // ...AND IT DOES NOT SEND THE READER TO THEIR ROWS. Gb and B
                // are on a MEASURED side, so a per-side gate let this through -
                // but the pass never landed on them, so fStat is invalid (zero
                // samples), the profile is all NaN and hStat/vStat are invalid
                // at n < 2: twelve dashes. The H sentence must therefore end at
                // the reason.
                {
                    const size_t bar = g_projNoCurveProbe.find('|');
                    const std::string hLine = g_projNoCurveProbe.substr(0, bar);
                    const std::string vLine = bar == std::string::npos
                                            ? std::string() : g_projNoCurveProbe.substr(bar);
                    fprintf(stderr, "abstatsselftest: C21 H line '%s'\n", hLine.c_str());
                    fprintf(stderr, "abstatsselftest: C21 V line '%s'\n", vLine.c_str());
                    check(!app.proj[0].fStat[2].valid && !app.proj[0].hStat[2].valid &&
                          !app.proj[0].vStat[2].valid,
                          "C21 fixture: Gb's table row really is all dashes");
                    check(app.proj[0].fStat[0].valid,
                          "C21 fixture: ...while R's region statistics stand");
                    check(hLine.find("in the table below") == std::string::npos,
                          "C21 the H sentence does not point at Gb's and B's dashes");
                    // THE MIXED CASE, which is the one a bool cannot carry. The
                    // V axis leaves all four planes silent - R and Gr with one
                    // sample, Gb and B with no position - and only R and Gr have
                    // numbers. A flag set by any of them put "Their numbers are
                    // in the table below" under a sentence naming four. The
                    // clause has to NAME its subject.
                    check(vLine.find("  R@A, Gr@A have numbers in the table below.") !=
                              std::string::npos,
                          "C21 the V sentence names the two planes that DO have numbers");
                    check(vLine.find("Gb@A have") == std::string::npos &&
                          vLine.find("Gb@A, B@A have") == std::string::npos &&
                          vLine.find("B@A have numbers") == std::string::npos,
                          "C21 ...and never promises Gb's or B's twelve dashes");
                }
                // ...and the hover says the same thing, about the MOSAIC and not
                // about the sensor.
                if (r) {
                    const float t = (206.0f - r->xmin) / std::max(r->xmax - r->xmin, 1e-6f);
                    projFrame(900.0f, 900.0f,
                              ImVec2(r->px0 + t * (r->px1 - r->px0),
                                     (r->py0 + r->py1) * 0.5f));
                    fprintf(stderr, "abstatsselftest: C21 hover '%s'\n",
                            g_projHoverProbe.c_str());
                    check(g_projHoverProbe.find("no finite pixel") == std::string::npos,
                          "C21 hover makes no claim about the sensor's pixels");
                    check(g_projHoverProbe.find("Gb: no sample") != std::string::npos &&
                          g_projHoverProbe.find("B: no sample") != std::string::npos,
                          "C21 ...it says Gb and B have no sample in this cell");
                }
                app.anns.clear();
                app.selectedAnn = 0;
                app.annRev++;
            }

            // ======================================================================
            // C22 - DEAD PHASE AND UNVISITED PLANE ARE DIFFERENT SENTENCES.
            // C18's fixture has positions and no finite value at half of them;
            // C21's has no positions at all. Both end with no curve, and the
            // reason a reader can act on is not the same one - so the panel must
            // not print one sentence for both.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_deadphase", 32, 32, 2, 1));
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 32; x++)
                        if ((x % 4) == 0 && cfaChannelAt(*d, x, y) == 0)
                            d->px()[(size_t)y * 32 + x] =
                                std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                fprintf(stderr, "abstatsselftest: C22 dead-phase no-curve '%s'\n",
                        g_projNoCurveProbe.c_str());
                const ProjCurveProbe* r = curve("R", true);
                check(r && r->lattice == 16 && r->points == 0,
                      "C22 fixture: R has 16 positions and no drawable pair");
                check(g_projNoCurveProbe.find("no sample position in this region") ==
                          std::string::npos,
                      "C22 a dead PHASE is not 'no sample position' - the positions exist");
                check(g_projNoCurveProbe.find("8 of 16") != std::string::npos,
                      "C22 ...the sentence counts the finite samples of the positions");
                check(g_projNoCurveProbe.find("no two of them next to each other") !=
                          std::string::npos,
                      "C22 ...and names the condition that actually failed");
                // ANTI-VACUOUS for C21's clause, taken where the whole quiet set
                // HAS numbers: R is the only silent plane here and its region
                // statistics stand, so the clause appears, names R, and takes
                // the singular verb. The gate is not simply off.
                check(app.proj[0].fStat[0].valid &&
                      field(g_projSideProbe, "said-no-curve-planeH=") == "R@A",
                      "C22 fixture: R is the only silent plane, and it has numbers");
                check(g_projNoCurveProbe.find("  R@A has numbers in the table below.") !=
                          std::string::npos,
                      "C22 ...so the clause does appear, naming R, in the singular");
            }

            // ======================================================================
            // C23 - THE "all" FOOTNOTE IS ABOUT EVERY SIDE, NOT ABOUT A. The
            // notice that explains why the pooled row's axis numbers are dashes
            // was raised on A's own mosaic flag, so a B (or a C) that is the
            // mosaic put dashes in its own rows and no sentence appeared
            // anywhere on the panel - the silence the notice exists to stop.
            // ======================================================================
            {
                app.images.clear();
                app.images.push_back(mkFrame("pc_foot_A", 16, 16, 0, 1));   // mono
                app.images.push_back(mkFrame("pc_foot_B", 16, 16, 1, 1));   // mosaic
                app.current = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.projAllRow = true;
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                draw();
                fprintf(stderr, "abstatsselftest: C23 A mono / B mosaic foot '%s'\n",
                        g_projFootProbe.c_str());
                check(app.proj[0].cell == 1 && app.proj[1].cell == 2 &&
                      app.proj[1].allRow,
                      "C23 fixture: A is not a mosaic and B is, both with 'all' on");
                check(g_projFootProbe == "pooled-mosaic=B",
                      "C23 the footnote is raised, and it names B");
                check(app.proj[1].h[4].empty() && !app.proj[1].hStat[4].valid,
                      "C23 ...which is exactly the side whose pooled axis rows are dashes");
                // ANTI-VACUOUS: with neither side a mosaic there is nothing to
                // explain and the sentence must not appear at all.
                app.images[1]->cfa = 0;
                app.proj[1] = App::ProjState{};
                app.abStepBusyUntil = 0;
                draw();
                fprintf(stderr, "abstatsselftest: C23 both mono foot '%s'\n",
                        g_projFootProbe.c_str());
                check(g_projFootProbe.empty(),
                      "C23 two non-mosaic sides raise no footnote at all");
                // ...and with BOTH sides a mosaic it names both.
                app.images[0]->cfa = 1; app.images[0]->cfaPattern = 0;
                app.images[1]->cfa = 1; app.images[1]->cfaPattern = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.abStepBusyUntil = 0;
                draw();
                fprintf(stderr, "abstatsselftest: C23 both mosaic foot '%s'\n",
                        g_projFootProbe.c_str());
                check(g_projFootProbe == "pooled-mosaic=A, B",
                      "C23 ...and both sides when both are mosaics");
                app.projAllRow = false;
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
            }

            // ======================================================================
            // C24 - THE PREVIEW NOTE ON THE x AXIS BELONGS TO A SIDE, AND SAYS
            // WHICH. A decimated preview's columns are every Nth column of the
            // sensor, so "column x (px)" is a sensor coordinate its samples do
            // not have - but the note was raised by ANY side on the plot,
            // including a refused mosaic preview that draws no curve at all and
            // is therefore not on this axis, and it never said whose grid it
            // meant.
            // ======================================================================
            {
                // (a) a MONO preview: it really is drawn, on its own grid
                ImageDoc* d = useOnly(mkFrame("pc_xaxis_mono", 16, 16, 0, 1));
                d->src->remoteStep = 2;
                app.proj[0] = App::ProjState{};
                draw();
                fprintf(stderr, "abstatsselftest: C24 mono preview x '%s'\n",
                        g_projXAxisProbe.c_str());
                check(g_projXAxisProbe.find("A: PREVIEW grid") != std::string::npos,
                      "C24 a mono preview's axis names the SIDE and its grid");
                check(curve("ch0", true) && curve("ch0", true)->points == 16,
                      "C24 ...and that side really does have a curve on that axis");
                // (b) a MOSAIC preview is refused, draws nothing, and must not
                // annotate an axis it is not on.
                ImageDoc* m = useOnly(mkFrame("pc_xaxis_cfa", 16, 16, 1, 1));
                m->src->remoteStep = 2;
                app.proj[0] = App::ProjState{};
                draw();
                fprintf(stderr, "abstatsselftest: C24 mosaic preview x '%s' unavail '%s'\n",
                        g_projXAxisProbe.c_str(),
                        field(g_projSideProbe, "unavail=").c_str());
                check(field(g_projSideProbe, "unavail=") == "A",
                      "C24 fixture: the mosaic preview is the refused side");
                check(g_projXAxisProbe.find("PREVIEW") == std::string::npos,
                      "C24 a side with no curve does not get to annotate the axis");
                // (c) ...and a full-resolution frame says nothing at all.
                useOnly(mkFrame("pc_xaxis_full", 16, 16, 0, 1));
                draw();
                check(g_projXAxisProbe.find("PREVIEW") == std::string::npos &&
                      g_projXAxisProbe.find("column x (px)") != std::string::npos,
                      "C24 a full-resolution frame keeps the plain axis title");
                // (d) THE PLANE SELECTOR CLOSES IT TOO. A is a full-resolution
                // mosaic and B is a mono preview; with one of A's planes on the
                // axis, B's ch0 is not drawn, so B puts nothing on this axis and
                // must not annotate it. A gate that asked "does this side have a
                // profile" answered yes and annotated an axis B is not on.
                app.images.clear();
                app.images.push_back(mkFrame("pc_xf_A", 16, 16, 1, 1));   // mosaic, full
                app.images.push_back(mkFrame("pc_xf_B", 16, 16, 0, 1));   // mono preview
                app.images[1]->src->remoteStep = 2;
                app.current = 0;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                app.histPlane = -1;                 // every plane: B's ch0 is drawn
                draw();
                fprintf(stderr, "abstatsselftest: C24 all planes x '%s'\n",
                        g_projXAxisProbe.c_str());
                check(g_projXAxisProbe.find("B: PREVIEW grid") != std::string::npos,
                      "C24 with ch0 drawn, B's preview grid is on the axis and says so");
                app.histPlane = 0;                  // R only: B has no R
                app.abStepBusyUntil = 0;
                draw();
                fprintf(stderr, "abstatsselftest: C24 plane=R x '%s'\n",
                        g_projXAxisProbe.c_str());
                check(g_projXAxisProbe.find("PREVIEW") == std::string::npos,
                      "C24 ...and with R only, B is not on the axis and does not annotate it");
                app.histPlane = -1;
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
            }

            // ======================================================================
            // C25 - WITH THE PLOT DECIMATING, ADJACENCY IS BETWEEN BUCKETS. A
            // run breaks at a bucket holding nothing finite, and a bucket is
            // `stride` positions - so "no two POSITIONS next to each other" is
            // a claim about the data that the data does not make: here every
            // finite position has a finite neighbour, and the curve is still
            // undrawable because the pairs land in alternating buckets.
            //
            // 4096 columns gives R 2048 positions and, over an ~800 px plot, a
            // stride of 2. R is made finite at positions 4m and 4m+1 and
            // non-finite at 4m+2 and 4m+3, so bucket 2m is finite, bucket 2m+1
            // is dead, and no two neighbouring buckets both carry a point.
            // ======================================================================
            {
                ImageDoc* d = useOnly(mkFrame("pc_bucket", 4096, 8, 1, 1));
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 4096; x++)
                        if (cfaChannelAt(*d, x, y) == 0 && ((x / 2) % 4) >= 2)
                            d->px()[(size_t)y * 4096 + x] =
                                std::numeric_limits<float>::quiet_NaN();
                computeMinMax(*d);
                d->black = 0; d->white = 4095;
                app.proj[0] = App::ProjState{};
                draw();
                const ProjCurveProbe* r = curve("R", true);
                fprintf(stderr, "abstatsselftest: C25 R lattice=%d finite=%d stride=%d "
                                "pts=%d\n", r ? r->lattice : -1, r ? r->finite : -1,
                        r ? r->stride : -1, r ? r->points : -1);
                fprintf(stderr, "abstatsselftest: C25 no-curve '%s'\n",
                        g_projNoCurveProbe.c_str());
                check(r && r->stride > 1 && r->lattice == 2048 && r->finite == 1024,
                      "C25 fixture: 2048 positions, half of them finite, and decimation on");
                check(r && r->points == 0,
                      "C25 ...and no curve, because the live pairs alternate by bucket");
                // ANTI-VACUOUS: every finite position HAS a finite neighbour
                // here, so the stride-1 sentence would be false. Checked on the
                // profile itself, not on the wording.
                check(std::isfinite(app.proj[0].h[0][0]) &&
                      std::isfinite(app.proj[0].h[0][2]) &&
                      !std::isfinite(app.proj[0].h[0][4]),
                      "C25 fixture: columns 0 and 2 are both finite - they ARE neighbours");
                check(g_projNoCurveProbe.find("no two of them next to each other") ==
                          std::string::npos,
                      "C25 so the sentence does not say 'no two positions next to each other'");
                check(g_projNoCurveProbe.find("decimating 2 positions to a point") !=
                          std::string::npos &&
                      g_projNoCurveProbe.find("no two neighbouring groups both do") !=
                          std::string::npos,
                      "C25 ...it names the decimation, which is what actually refused");
            }

            // ======================================================================
            // C26 - "ROI" IS PER SIDE. recomputeProjectionIfNeeded clamps the
            // selected rectangle into each side's own frame and DROPS it when
            // the clamp leaves nothing (`cw > 0 && chh > 0`), so a side the ROI
            // falls outside of is measured over its whole image while A is
            // measured over the ROI. Two sentences printed A's answer for every
            // side's row: the dimensions line called those sides' full frames an
            // ROI - right next to the sizes a reader would check it against -
            // and the TSV's region header labelled their rows with a region they
            // were never measured over.
            //
            // This was mis-classified as "the same for every side because the
            // rectangle comes from one annotation". The rectangle does; the
            // ANSWER does not.
            // ======================================================================
            {
                app.images.clear();
                app.images.push_back(mkFrame("pc_roi_A", 4096, 32, 0, 1));  // ROI fits
                app.images.push_back(mkFrame("pc_roi_B", 8, 8, 0, 1));      // ROI misses
                app.current = 0;
                app.anns.clear();
                App::Ann a5;
                a5.id = app.nextAnnId++;
                a5.type = 0;
                a5.x = 200; a5.y = 0; a5.w = 16; a5.h = 8;
                a5.label = "pcroiside";
                app.anns.push_back(a5);
                app.selectedAnn = a5.id;
                app.annRev++;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.compareMode = App::CmpWipe;
                app.abStepBusyUntil = 0;
                setCompareB(app.images[1].get());
                draw();
                fprintf(stderr, "abstatsselftest: C26 dims '%s'\n", g_projDimsProbe.c_str());
                check(app.proj[0].roiUsed && !app.proj[1].roiUsed,
                      "C26 fixture: the ROI lands on A and falls outside B entirely");
                check(app.proj[0].rw == 16 && app.proj[1].rw == 8,
                      "C26 ...so A is 16 columns and B is its whole 8");
                check(g_projDimsProbe == "A 16x8 ROI  B 8x8 whole",
                      "C26 the dimensions line says which side is an ROI, per side");
                // ...and the TSV's region header, which is the same answer in
                // the file that carries both sides' rows.
                std::vector<ProjSide> two;
                two.push_back({ &app.proj[0], "A", "a", false, 0 });
                two.push_back({ &app.proj[1], "B", "b", false, 0 });
                const std::string reg = projRegionSaid(two);
                fprintf(stderr, "abstatsselftest: C26 tsv region '%s'\n", reg.c_str());
                check(reg == "; A ROI (200,0) 16x8; B whole image 8x8",
                      "C26 ...and so does the TSV header, for every side in the file");
                // ANTI-VACUOUS: with no ROI at all both sides say "whole", so
                // the word tracks the answer rather than being decoration.
                app.anns.clear();
                app.selectedAnn = 0;
                app.annRev++;
                app.proj[0] = App::ProjState{};
                app.proj[1] = App::ProjState{};
                app.abStepBusyUntil = 0;
                draw();
                fprintf(stderr, "abstatsselftest: C26 no-ROI dims '%s'\n",
                        g_projDimsProbe.c_str());
                check(g_projDimsProbe == "A 4096x32 whole  B 8x8 whole",
                      "C26 with no ROI selected, neither side is called one");
                app.compareMode = App::CmpOff;
                setCompareB(nullptr);
                app.proj[1] = App::ProjState{};
            }

            // ---- everything borrowed, put back ------------------------------------
            app.anns.clear();
            app.nextAnnId = 1;
            app.selectedAnn = keepSel;
            app.annRev++;
            app.images.clear();
            app.current = -1;
            // The caches are NOT put back as they were found: a ProjState holds an
            // ImageDoc*, closeAll() destroyed the documents it pointed at, and handing
            // a dangling pointer back would be worse than the empty cache an empty
            // session is entitled to. Cleared, which is what closeAll leaves anyway.
            app.proj[0] = App::ProjState{};
            app.proj[1] = App::ProjState{};
            app.projExtra.clear();
            app.projMode = keepMode;
            app.projYMode = keepYMode;
            app.showProjH = keepH;
            app.showProjV = keepV;
            app.projAllRow = keepAllRow;
            app.projStatLayout = keepStatLayout;
            app.projStatOrder = keepStatOrder;
            app.projDecimals = keepDecimals;
            app.histPlane = keepPlane;
            app.abStatsLayout = keepAbLayout;
            app.compareMode = keepCompare;
            app.linkRange = keepLink;
            app.prefsDirty = keepPrefsDirty;
            // The document list this test emptied cannot be handed back - closeAll()
            // took it - so what is restored is the INDEX being out of range in the same
            // way an empty session leaves it, and the count is reported rather than
            // faked. Nothing runs after this in the process.
            if (keepImages || keepCurrent >= 0)
                fprintf(stderr, "abstatsselftest: entered with %zu document(s), current %d; "
                                "the session was closed for the fixtures and is not restored\n",
                        keepImages, keepCurrent);
            g_projCurveProbe.clear();
            g_projHoverProbe.clear();

        }
        fprintf(stderr, "abstatsselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
