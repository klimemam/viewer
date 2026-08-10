// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
    // ---- A/B statistics caches (docs/ab-stats-plan.md, section 6) -------------
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
        // with the panel: 256 bins on the SNAPPED grid (docs/todo-open.md 9 -
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
                // docs/stats-taxonomy.md, and the definition is the unbiased
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
        {
            if (!needWindow("--abstats-selftest (the Temporal chart group)")) return 1;
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
                // right-clickable as B a moment later
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
            }
        }

        // ---- S: the slots past B, held to B's own rules -----------------------
        // (defects, 2026-07-30: 「Cに選んだやつが，Stackだとフレーム数更新され
        // ない」, 「FilesにC,D,,,,が表示されない」, and A standing on the pinned
        // B reading as 「比較を抜けてしまう」.)
        {
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
        }

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
        {
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

            auto histFrame = [&]() {
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(560.0f, 720.0f), ImGuiCond_Always);
                    ImGui::Begin("HistProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    drawPanelHistogram();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
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

            // N3: side by side has two halves, so it holds two sides. The other
            // two are not on any plot and the panel has to say so.
            app.abStatsLayout = App::AbSide;
            histFrame();
            std::string pSide = g_histSideProbe;
            fprintf(stderr, "abstatsselftest: N3 side-by-side probe '%s'\n", pSide.c_str());
            check(rowsOf(pSide) == "ABCD",
                  "N3 side by side keeps every slot in the table");
            check(field(pSide, "curves=") == "AB" &&
                  field(pSide, "said-no-curve=") == "CD",
                  "N3 ...and names the two it has no half for");
            check(accountedFor(pSide, unsaid),
                  "N3 every slot with numbers is drawn or named");
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
            // went N wide (75e5369) while its plots still draw only A and B,
            // and nothing on screen said so: rows had letters that were in
            // neither curves nor any sentence. The plots keeping to A|B is the
            // pinned CURRENT limit, spoken on screen since this change; when
            // the N overlay lands (compare-n.md §12) the curves assert below
            // moves to ABCDE and said-no-curve empties.
            {
                auto projFrame = [&]() {
                    for (int pass = 0; pass < 2; pass++) {
                        ImGui_ImplOpenGL3_NewFrame();
                        ImGui_ImplGlfw_NewFrame();
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                        ImGui::SetNextWindowSize(ImVec2(560.0f, 720.0f), ImGuiCond_Always);
                        ImGui::Begin("ProjProbe", nullptr,
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoDocking);
                        drawPanelProjection();
                        ImGui::End();
                        ImGui::EndFrame();
                    }
                };
                check(app.cmpExtra.size() == 3, "N6 fixture: N5 left C, D, E armed");
                projFrame();
                std::string pProj = g_projSideProbe;
                fprintf(stderr, "abstatsselftest: N6 projection probe '%s'\n",
                        pProj.c_str());
                check(rowsOf(pProj) == "ABCDE",
                      "N6 the projection table gives every armed side a row");
                check(field(pProj, "curves=") == "AB",
                      "N6 the profile plots hold A and B - today's limit, pinned");
                check(field(pProj, "said-no-curve=") == "CDE",
                      "N6 ...and the letters off the plots are named on screen");
                check(accountedFor(pProj, unsaid),
                      "N6 no slot has numbers and no word about it");
            }

            app.cmpExtra.clear();
            app.compareFollowFrame = true;
            app.abStatsLayout = App::AbAuto;
            app.histPlane = -1;
        }

        // ---- W: the bin GRID - integer / power-of-two bin widths ------------
        // (docs/todo-open.md 9, user report 2026-07-29.) 256 bins spread evenly
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

        fprintf(stderr, "abstatsselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
