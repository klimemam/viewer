// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
    // Every A/B range combination, printed. See scratchpad/rangetest.py.
    if (!g_rangeSelftest.empty()) {
        std::string dir = g_rangeSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        app.seqLoadMode = 1;
        openFolder(dir);
        double t0 = nowSec();
        while (nowSec() - t0 < 300.0) {
            if (app.folderPickOpen && !app.folderPickRemote) {
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen && app.seqs.size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (app.seqs.size() < 2) {
            fprintf(stderr, "rangeselftest: need 2 stacks under %s\n", dir.c_str());
            return 1;
        }
        std::vector<int> fa = framesOfSeq(app.seqs[0].id), fb = framesOfSeq(app.seqs[1].id);
        if (fa.size() < 2 || fb.size() < 2) {
            fprintf(stderr, "rangeselftest: stacks need 2+ frames\n");
            return 1;
        }
        selectImage(fa[0]);
        setCompareB(app.images[fb[0]].get());
        app.compareMode = App::CmpSplit;
        app.compareFollowFrame = true;
        static const char* MODE[3] = { "auto-per-frame", "per-stack", "linked" };
        static const char* SHARE[3] = { "each-own", "B-uses-A", "union-auto" };
        int bad = 0;
        {   // The two stacks of a capture series hold identically NAMED frames
            // (00/frame_000.npy and 01/frame_000.npy are both frame_000.npy),
            // so this is also the check that the compare UI can tell them apart
            // at all: every label there goes through abDocLabel.
            std::string la = abDocLabel(cur()), lb = abDocLabel(cmpB());
            fprintf(stderr, "rangeselftest: labels | A = %s | B = %s | files %s / %s | %s\n",
                    la.c_str(), lb.c_str(), cur()->name.c_str(), cmpB()->name.c_str(),
                    la != lb ? "DISTINCT" : "AMBIGUOUS");
            if (la == lb) bad++;
        }
        {   // The out-of-the-box display-range mode. "B uses A's range" renders
            // a B captured at another level at >white 99.7% - one dashed line
            // jammed against the right edge, and an empty half in side by side.
            // Union bins both sides identically and clips neither.
            fprintf(stderr, "rangeselftest: default compareRangeMode at startup = %d (%s)%s\n",
                    g_abRangeDefault,
                    g_abRangeDefault >= 0 && g_abRangeDefault < 3 ? SHARE[g_abRangeDefault] : "?",
                    g_abRangeDefault == 2 ? "" : "  <- EXPECTED 2 (union-auto)");
            if (g_abRangeDefault != 2) bad++;
        }
        {   // The axis label has to name the range ACTUALLY in force. It read
            // "A's black-white range" in every mode, including the union - a
            // claim about the picture that the picture does not support.
            int save = app.compareRangeMode;
            bool saveLink = app.linkRange;
            // ...and "in force" includes the Value range scope. linkRange
            // short-circuits effBlack/effWhite before abRange is even consulted,
            // so with the scope set to everything the axis spans the linked pair
            // - widened by stacks that have nothing to do with the comparison -
            // whatever the A/B combo happens to say.
            for (int lk = 0; lk < 2; lk++) {
                app.linkRange = lk != 0;
                if (app.linkRange) {
                    app.linkBlack = app.images[fa[0]]->vmin;
                    app.linkWhite = app.images[fa[0]]->vmax;
                }
                for (int sh = 0; sh < 3; sh++) {
                    app.compareRangeMode = sh;
                    recomputeHistogramIfNeeded(cur(), app.hist[0]);
                    std::string lab = abHistXLabel(cur(), cmpB(), app.hist[0]);
                    bool named = app.linkRange
                                     ? lab.find("linked range") != std::string::npos
                                     : (sh == 2
                                            ? lab.find("A and B combined") != std::string::npos
                                            : lab.find("A\'s black-white") != std::string::npos);
                    // and the label has to match the NUMBERS, not just read well
                    bool spans = !app.linkRange || (effBlack(*cur()) == app.linkBlack &&
                                                    effWhite(*cur()) == app.linkWhite);
                    fprintf(stderr, "rangeselftest: hist x axis | %-6s %-11s | %s | %s\n",
                            app.linkRange ? "linked" : "own", SHARE[sh], lab.c_str(),
                            named && spans ? "names the range in force" : "WRONG");
                    if (!named || !spans) bad++;
                }
            }
            app.linkRange = saveLink;
            app.compareRangeMode = save;
        }
        for (int m = 0; m < 3; m++) {
            app.rangeScope = m == 0 ? 0 : 1;
            app.linkRange = m == 2;
            if (app.linkRange) { app.linkBlack = app.images[fa[0]]->vmin;
                                 app.linkWhite = app.images[fa[0]]->vmax; }
            for (int sh = 0; sh < 3; sh++) {
                app.compareRangeMode = sh;
                for (int step = 0; step < 2; step++) {
                    selectImage(fa[step]);
                    ImageDoc* A = cur();
                    ImageDoc* B = cmpB();
                    if (!A || !B) { fprintf(stderr, "rangeselftest: no A/B\n"); return 1; }
                    bool same = effBlack(*A) == effBlack(*B) && effWhite(*A) == effWhite(*B);
                    fprintf(stderr,
                            "rangeselftest: %-14s %-11s frame %d | A %.6g..%.6g  B %.6g..%.6g  "
                            "| B frame %d | %s\n",
                            MODE[m], SHARE[sh], step, effBlack(*A), effWhite(*A),
                            effBlack(*B), effWhite(*B), B->seqIndex,
                            same ? "MATCHED" : "differs");
                    // the contract: anything but "each-own" must produce one range
                    if (sh != 0 && !same) bad++;
                    // follow-frame must keep the sides on the same frame number
                    if (B->seqIndex != A->seqIndex) {
                        fprintf(stderr, "rangeselftest: FOLLOW BROKEN A#%d vs B#%d\n",
                                A->seqIndex, B->seqIndex);
                        bad++;
                    }
                }
            }
        }
        // Switching TO "auto per frame" must re-fit the frame on screen. The
        // policy is applied in selectImage, which only runs on a frame change,
        // so the mode used to look dead until the first step ("選んだ直後は
        // 画像がかわらず、コマ送りすると反映される").
        {
            app.linkRange = false;
            app.rangeScope = 1;                       // per stack
            selectImage(fa[1]);                       // a frame that inherited
            ImageDoc* d = cur();
            d->black = d->vmin - 1000; d->white = d->vmax + 1000;   // clearly not its own fit
            float wasB = d->black, wasW = d->white;
            applyRangeMode(0, d);              // exactly what the combo calls
            bool refit = d->black != wasB || d->white != wasW;
            fprintf(stderr, "rangeselftest: auto-per-frame applied on the spot: "
                            "%.6g..%.6g -> %.6g..%.6g (frame data %.6g..%.6g) %s\n",
                    wasB, wasW, d->black, d->white, d->vmin, d->vmax,
                    refit ? "ok" : "WRONG");
            if (!refit) bad++;
            app.rangeScope = 1;
        }

        // ==================================================================
        // #245 - the two Auto flavors, and what "auto per frame" re-fits to.
        // Fixtures are built in memory (the histhl discipline): every number
        // below is derived by hand from them, so no fixture on disk can drift
        // the expectations. NOTHING here is left in app.images or in a
        // persisted mode - the selftest home is shared with every other test.
        // ==================================================================
        auto mkFloat = [](const char* nm, int W, int H) {
            auto d = std::make_unique<ImageDoc>();
            d->name = nm;
            d->src->w = W; d->src->h = H; d->src->ch = 1;
            d->src->dtype = "f32";
            d->src->data.assign((size_t)W * H, 0.0f);
            d->syncMirrors();
            d->uid = app.nextUid++;
            return d;
        };
        // A ramp filling exactly [0.3, 0.6], with `hot` pixels of it replaced
        // by a far outlier. 128x128 = 16384 px, so 0.1% is 16.38 px and four
        // hot pixels are comfortably inside the tail that gets cut.
        auto mkRamp = [&](const char* nm, float lo, float hi, int hot, float hotV) {
            auto d = mkFloat(nm, 128, 128);
            const size_t N = d->px().size();
            for (size_t i = 0; i < N; i++)
                d->px()[i] = lo + (hi - lo) * (float)i / (float)(N - 1);
            for (int k = 0; k < hot; k++) d->px()[N - 1 - (size_t)k] = hotV;
            computeMinMax(*d);
            return d;
        };

        {   // ---- P1: the hot pixel decides Auto and does NOT decide Auto % --
            // 16380 ramp values over [0.3, 0.6] plus 4 px at 1000.
            //   Auto      -> 0.3 .. 1000: the frame is black except 4 pixels.
            //   Auto %    -> bins are (1000-0.3)/65536 = 0.0152542 wide.
            //     lo: the 0.1% cut (16.38 px) falls inside bin 0, whose lower
            //         edge is vmin, so lo = 0.3 exactly.
            //     hi: the 99.9% cut (16367.6 px) is ramp value 0.59978, bin
            //         floor(0.29978/0.0152542) = 19, upper edge
            //         0.3 + 20*0.0152542 = 0.605084.
            auto d = mkRamp("pct_hot", 0.3f, 0.6f, 4, 1000.0f);
            float lo = 0, hi = 0;
            bool got = percentileRange(*d, 0.001f, 0.999f, lo, hi);
            fprintf(stderr, "rangeselftest: P1 min/max %.6g..%.6g | 0.1-99.9%% %.6g..%.6g%s\n",
                    d->vmin, d->vmax, lo, hi, got ? "" : "  (FELL BACK)");
            if (!(d->vmin == 0.3f && d->vmax == 1000.0f)) {
                fprintf(stderr, "rangeselftest: P1 the fixture is not 0.3..1000  WRONG\n");
                bad++;
            }
            // the claim, both halves: the outlier is out, and the signal is in
            bool cut = got && hi < 1.0f;
            bool keeps = hi >= 0.5999f && fabsf(lo - 0.3f) < 1e-5f;
            bool edge = fabsf(hi - 0.605084f) < 1e-3f;      // the hand-derived edge
            fprintf(stderr, "rangeselftest: P1 hot pixel excluded by Auto %% (%s), "
                            "ramp still inside (%s), edge as derived (%s)\n",
                    cut ? "ok" : "WRONG", keeps ? "ok" : "WRONG", edge ? "ok" : "WRONG");
            if (!cut || !keeps || !edge) bad++;
        }

        {   // ---- P2: the cuts are the quantiles, checked in closed form ----
            // The same ramp with NO outlier: a uniform distribution, whose
            // 0.1 and 99.9 percentiles are known exactly - 0.1% of the span in
            // from each end, so 0.3003 and 0.5997. Bins here are only
            // 0.3/65536 = 4.6e-6 wide, far finer than the tolerance, so this
            // pins the arithmetic itself rather than the quantization. It also
            // says what Auto % is NOT: it does cut a clean image, by exactly
            // the 0.2% it is named after, and that is why "Auto" stays.
            auto d = mkRamp("pct_clean", 0.3f, 0.6f, 0, 0.0f);
            float lo = 0, hi = 0;
            bool got = percentileRange(*d, 0.001f, 0.999f, lo, hi);
            bool tight = got && fabsf(lo - 0.3003f) < 5e-4f && fabsf(hi - 0.5997f) < 5e-4f &&
                         lo > d->vmin && hi < d->vmax;
            fprintf(stderr, "rangeselftest: P2 clean ramp | min/max %.6g..%.6g | "
                            "0.1-99.9%% %.6g..%.6g (expect 0.3003..0.5997) | %s\n",
                    d->vmin, d->vmax, lo, hi, tight ? "ok" : "WRONG");
            if (!tight) bad++;
        }

        {   // ---- P3: degenerate input falls back to min..max ---------------
            // Nothing finite to count. percentileRange says so AND still
            // leaves lo/hi usable - callers set a display range from them.
            auto d = mkFloat("pct_nan", 8, 8);
            for (auto& v : d->px()) v = std::numeric_limits<float>::quiet_NaN();
            computeMinMax(*d);                     // no finite pixel -> 0..1
            float lo = -7, hi = -7;
            bool got = percentileRange(*d, 0.001f, 0.999f, lo, hi);
            bool okFall = !got && lo == d->vmin && hi == d->vmax && hi > lo;
            fprintf(stderr, "rangeselftest: P3 all-NaN | returned %d, lo/hi %.6g..%.6g | %s\n",
                    (int)got, lo, hi, okFall ? "falls back to min..max" : "WRONG");
            if (!okFall) bad++;
        }

        {   // ---- P4: linked Auto % pools the images onto ONE histogram -----
            // The counterpart of linked Auto ("min/max over all open images").
            // A quantile of a union is not the union of quantiles, so the bins
            // are accumulated, not the answers. Two ramps at different levels,
            // each with its own outlier; the pooled answer must span both
            // bulks and neither outlier.
            //   A = ramp 0.3..0.6 + 4 px at 1000, B = ramp 2..3 + 4 px at -500
            //   union min/max = -500 .. 1000 - which is exactly what linked
            //   "Auto" would hand you: two flat grey frames.
            //   pooled N = 32768, cuts at 32.77 px and 32735.2 px, so the 8
            //   outliers (4 at each end) are inside the cut tails and nothing
            //   else is.
            const size_t imgWas = app.images.size();
            auto A2 = mkRamp("pct_A", 0.3f, 0.6f, 4, 1000.0f);
            auto B2 = mkRamp("pct_B", 2.0f, 3.0f, 4, -500.0f);
            float rlo = std::min(A2->vmin, B2->vmin), rhi = std::max(A2->vmax, B2->vmax);
            std::vector<uint32_t> bins;
            uint64_t n = accumulatePercentileBins(*A2, rlo, rhi, bins) +
                         accumulatePercentileBins(*B2, rlo, rhi, bins);
            float xlo = 0, xhi = 0;
            bool got = percentileFromBins(bins, n, rlo, rhi, 0.001f, 0.999f, xlo, xhi);
            fprintf(stderr, "rangeselftest: P4 pooled union %.6g..%.6g | n=%llu | "
                            "0.1-99.9%% %.6g..%.6g%s\n",
                    rlo, rhi, (unsigned long long)n, xlo, xhi, got ? "" : "  (FELL BACK)");
            // Neither outlier survives, and both bulks (0.3..0.6 and 2..3) are
            // inside to within the 0.1% each tail is allowed to shave.
            bool spans = got && n == 32768 &&
                         xlo > -1.0f && xlo < 0.35f && xhi > 2.9f && xhi < 10.0f;
            fprintf(stderr, "rangeselftest: P4 both bulks inside, neither outlier: %s\n",
                    spans ? "ok" : "WRONG");
            if (!spans) bad++;

            // ...and that is exactly what the button computes in linked mode.
            // app.images is SWAPPED, not appended to: the loaded frames must
            // not join this pool, and the swap back restores every one of them.
            bool saveLink = app.linkRange;
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A2));
            pool.push_back(std::move(B2));
            std::swap(app.images, pool);
            app.linkRange = true;
            float blo = 0, bhi = 0;
            autoPercentileFit(*app.images[0], blo, bhi);
            std::swap(app.images, pool);
            app.linkRange = saveLink;
            bool same = blo == xlo && bhi == xhi;
            fprintf(stderr, "rangeselftest: P4 linked autoPercentileFit %.6g..%.6g | %s\n",
                    blo, bhi, same ? "same numbers as the pooled bins" : "WRONG");
            if (!same) bad++;
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P4 app.images %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size());
                bad++;
            }
        }

        {   // ---- P5: "auto per frame" re-fits to min..max, not to 0..1 -----
            // The mode's own tooltip says "every frame re-fits to its own
            // min..max", and it went through defaultRange instead - whose
            // float magnet swallows anything in [-0.001, 1.2]. An
            // arbitrary-unit frame running 0.3..0.6 therefore came back as
            // 0..1 on every step and the mode looked dead (#245).
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope;
            auto d = mkRamp("apf", 0.3f, 0.6f, 0, 0.0f);
            d->black = -5; d->white = 5;                  // clearly not its own fit
            app.linkRange = false;
            applyRangeMode(0, d.get());                   // what the combo calls
            const float gotB = d->black, gotW = d->white;
            bool own = fabsf(gotB - d->vmin) < 1e-6f && fabsf(gotW - d->vmax) < 1e-6f;
            // anti-vacuity: this fixture really does trip the load-time magnet,
            // so "0.3..0.6" above is a fix and not a fixture that never snapped
            defaultRange(*d);
            bool magnet = d->black == 0.0f && d->white == 1.0f;
            fprintf(stderr, "rangeselftest: P5 auto-per-frame %.6g..%.6g (data %.6g..%.6g) %s"
                            " | load-time default would say %.6g..%.6g %s\n",
                    gotB, gotW, d->vmin, d->vmax, own ? "ok" : "WRONG",
                    d->black, d->white, magnet ? "(the magnet, still there)" : "ANTI-VACUITY LOST");
            if (!own || !magnet) bad++;
            app.linkRange = saveLink;
            app.rangeScope = saveScope;
        }

        {   // ---- P6: and the frame-step path is wired to the same fit ------
            // selectImage is the other site that re-fits in mode 0. These
            // fixtures are floats in the 1000s, which no magnet touches, so
            // this is a wiring check (the regression is caught by P5) - it
            // asserts the frame stepped ONTO holds its OWN numbers.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope;
            app.linkRange = false;
            app.rangeScope = 0;
            selectImage(fa[0]);
            selectImage(fa[1]);
            ImageDoc* f = cur();
            bool own = f && f->black == f->vmin && f->white == f->vmax;
            fprintf(stderr, "rangeselftest: P6 stepped frame holds %.6g..%.6g "
                            "(data %.6g..%.6g) %s\n",
                    f ? f->black : 0.f, f ? f->white : 0.f,
                    f ? f->vmin : 0.f, f ? f->vmax : 0.f, own ? "ok" : "WRONG");
            if (!own) bad++;
            app.linkRange = saveLink;
            app.rangeScope = saveScope;
        }
        // the shared home gets the modes it was lent, back
        app.rangeScope = 1;
        app.linkRange = false;

        fprintf(stderr, "rangeselftest: %s\n", bad ? "FAILED" : "ok");
        if (app.seqThread.joinable()) app.seqThread.join();
        return bad ? 1 : 0;
    }
