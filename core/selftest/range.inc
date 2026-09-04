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
        // ==================================================================
        // #248 - "Per stack" as a policy that is HELD, not a load-time
        // accident, and the Range row saying which preset is in force.
        // Same discipline as #245 above: in-memory fixtures, hand-derived
        // numbers, every borrowed mode returned to the shared home.
        // ==================================================================
        {   // ---- P7: in "Per stack", setRange writes the whole stack -------
            // Every write of a display range - the number field and all five
            // preset buttons - arrives at setRange, and setRange did not read
            // the scope. Pressing Auto in per-stack mode therefore fitted the
            // frame on screen and left its siblings on their old numbers
            // (#248 (b)); the mode only ever looked right because frames
            // INHERIT at landing and nothing had disturbed them yet.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope;
            const size_t imgWas = app.images.size();
            auto A3 = mkRamp("ps_A", 0.3f, 0.6f, 0, 0.0f);   // data 0.3..0.6
            auto B3 = mkRamp("ps_B", 2.0f, 3.0f, 0, 0.0f);   // data 2..3
            A3->seqId = 4242; A3->seqIndex = 0;
            B3->seqId = 4242; B3->seqIndex = 1;
            B3->black = -1; B3->white = 7;                   // plainly not A's
            B3->texDirty = false;                            // so "was redrawn" means something
            ImageDoc* pA = A3.get(); ImageDoc* pB = B3.get();
            // app.images is SWAPPED, not appended to (the P4 discipline): the
            // loaded frames must not be caught by a stack-wide write, and the
            // swap back restores every one of them.
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A3));
            pool.push_back(std::move(B3));
            std::swap(app.images, pool);
            app.linkRange = false;
            app.rangeScope = 1;                              // per stack
            setRange(*pA, 0.3f, 0.6f);                       // what "Auto" on A presses
            bool shared = pB->black == 0.3f && pB->white == 0.6f && pB->texDirty;
            fprintf(stderr, "rangeselftest: P7 per-stack setRange | A %.6g..%.6g -> "
                            "sibling %.6g..%.6g (expect 0.3..0.6, redrawn) %s\n",
                    pA->black, pA->white, pB->black, pB->white, shared ? "ok" : "WRONG");
            if (!shared) bad++;

            // ...and "auto per frame" must NOT spread: the sibling keeps what
            // it had, and is not even marked for redraw.
            app.rangeScope = 0;
            pB->texDirty = false;
            setRange(*pA, 0.0f, 255.0f);
            bool alone = pB->black == 0.3f && pB->white == 0.6f && !pB->texDirty &&
                         pA->black == 0.0f && pA->white == 255.0f;
            fprintf(stderr, "rangeselftest: P7 scope 0 touches A only | A %.6g..%.6g "
                            "sibling %.6g..%.6g (expect 0.3..0.6, untouched) %s\n",
                    pA->black, pA->white, pB->black, pB->white, alone ? "ok" : "WRONG");
            if (!alone) bad++;

            // ...and a standalone image is not a stack. Only a POSITIVE seqId
            // names one ("0 = standalone, >0 = frame of that sequence"), and
            // seqId 0 is the whole rest of the program: two unrelated singles
            // must not fuse just because the scope says "per stack". The
            // negative pass is the same claim for an id that should never
            // exist - a doc carrying one must not be treated as a stack of all
            // the others.
            app.rangeScope = 1;
            bool single = true;
            for (int id : { 0, -1 }) {
                pA->seqId = id; pB->seqId = id;
                pB->texDirty = false;
                setRange(*pA, 1.0f, 2.0f);
                bool ok = pB->black == 0.3f && pB->white == 0.6f && !pB->texDirty;
                fprintf(stderr, "rangeselftest: P7 seqId %d shares with nobody | other single "
                                "%.6g..%.6g (expect 0.3..0.6) %s\n",
                        id, pB->black, pB->white, ok ? "ok" : "WRONG");
                single = single && ok;
            }
            if (!single) bad++;

            std::swap(app.images, pool);
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P7 app.images %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size());
                bad++;
            }
            app.linkRange = saveLink;
            app.rangeScope = saveScope;
        }

        {   // ---- P8: switching TO "Per stack" re-shares --------------------
            // This is the reported bug itself (#248 (a)). "Auto per frame"
            // REWRITES each frame's stored black/white as you step - main.cpp
            // 1393 records that as known and deliberate - so a stack that has
            // been stepped in mode 0 holds per-frame values. applyRangeMode
            // used to only raise the scope flag, and those stored per-frame
            // values then reappeared on every step: 「Per stack にしても一枚毎
            // に Auto されてしまう」. The frame on screen is the reference.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope;
            const size_t imgWas = app.images.size();
            auto A4 = mkRamp("ms_A", 0.3f, 0.6f, 0, 0.0f);
            auto B4 = mkRamp("ms_B", 2.0f, 3.0f, 0, 0.0f);
            A4->seqId = 4243; B4->seqId = 4243;
            // exactly what stepping in mode 0 leaves behind: each frame on its
            // own min..max (autoRange), which is what these fixtures measure to
            A4->black = 0.3f; A4->white = 0.6f;
            B4->black = 2.0f; B4->white = 3.0f;
            B4->texDirty = false;
            ImageDoc* pA = A4.get(); ImageDoc* pB = B4.get();
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A4));
            pool.push_back(std::move(B4));
            std::swap(app.images, pool);
            app.linkRange = false;
            app.rangeScope = 0;                        // coming FROM auto-per-frame
            applyRangeMode(1, pA);                     // exactly what the combo calls
            bool reshared = app.rangeScope == 1 && !app.linkRange &&
                            pB->black == 0.3f && pB->white == 0.6f && pB->texDirty &&
                            pA->black == 0.3f && pA->white == 0.6f;
            fprintf(stderr, "rangeselftest: P8 mode 0 -> 1 re-shares | A %.6g..%.6g "
                            "sibling %.6g..%.6g (expect A's 0.3..0.6, was 2..3) %s\n",
                    pA->black, pA->white, pB->black, pB->white, reshared ? "ok" : "WRONG");
            if (!reshared) bad++;
            std::swap(app.images, pool);
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P8 app.images %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size());
                bad++;
            }
            app.linkRange = saveLink;
            app.rangeScope = saveScope;
        }

        {   // ---- P9: the first batch of a new stack has no seam ------------
            // This runs the REAL landing path: pumpSequence is the one place a
            // decoded frame becomes a document, and it is driven here by
            // filling app.seqReady exactly as the decode worker does, with no
            // worker and no files. `ref` (the first landed frame of the stack
            // being loaded) is looked up once before the loop, so in the FIRST
            // batch there is nothing to find and up to MAX_PER_FRAME = 4
            // frames each took their own defaultRange (#248 (c)).
            //   Fixtures are arbitrary-unit floats - 100..200, 300..400,
            //   500..600 - chosen so defaultRange has no magnet to apply and
            //   answers each frame's own min..max. Frame 1 therefore lands on
            //   100..200, and frames 2 and 3 must hold 100..200 too, not
            //   300..400 and 500..600.
            const size_t imgWas = app.images.size();
            const int saveLoadingId = app.seqLoadingId;
            const bool saveCancel = app.seqCancel;
            app.seqCancel = false;    // a standing cancel empties seqReady, and
                                      // an empty batch is not a passing test
            auto mkBand = [&](const char* nm, float lo, float hi) {
                auto d = mkRamp(nm, lo, hi, 0, 0.0f);
                d->black = -12345; d->white = 12345;   // no answer smuggled in
                return d;
            };
            auto f1 = mkBand("seam_1", 100.0f, 200.0f);
            auto f2 = mkBand("seam_2", 300.0f, 400.0f);
            auto f3 = mkBand("seam_3", 500.0f, 600.0f);
            f1->batchId = 77; f1->displayLut = 3;      // the block that rides along
            f2->batchId = 0;  f2->displayLut = -1;
            f3->batchId = 0;  f3->displayLut = -1;
            // app.images is emptied for the duration: that is what makes this
            // the FIRST batch (ref == nullptr), and it keeps the landed frames
            // out of the loaded stacks - the swap back drops them.
            std::vector<std::unique_ptr<ImageDoc>> pool;
            std::swap(app.images, pool);
            app.seqLoadingId = 4244;
            {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(0, std::move(f1));
                app.seqReady.emplace_back(1, std::move(f2));
                app.seqReady.emplace_back(2, std::move(f3));
            }
            pumpSequence();                            // the real integration
            bool landed = app.images.size() == 3;
            bool seam = landed &&
                        app.images[0]->black == 100.0f && app.images[0]->white == 200.0f &&
                        app.images[1]->black == 100.0f && app.images[1]->white == 200.0f &&
                        app.images[2]->black == 100.0f && app.images[2]->white == 200.0f;
            bool block = landed &&
                         app.images[1]->batchId == 77 && app.images[2]->batchId == 77 &&
                         app.images[1]->displayLut == 3 && app.images[2]->displayLut == 3;
            fprintf(stderr, "rangeselftest: P9 first batch | %zu landed | "
                            "%.6g..%.6g / %.6g..%.6g / %.6g..%.6g (expect 100..200 three times) %s"
                            " | batch+colormap carried %s\n",
                    app.images.size(),
                    landed ? app.images[0]->black : 0.f, landed ? app.images[0]->white : 0.f,
                    landed ? app.images[1]->black : 0.f, landed ? app.images[1]->white : 0.f,
                    landed ? app.images[2]->black : 0.f, landed ? app.images[2]->white : 0.f,
                    seam ? "ok" : "WRONG", block ? "ok" : "WRONG");
            if (!seam || !block) bad++;
            // anti-vacuity: the fixtures really do measure to three different
            // bands, so "100..200 three times" is the fix and not an artifact
            bool distinct = landed && app.images[1]->vmin == 300.0f && app.images[2]->vmin == 500.0f;
            if (!distinct) {
                fprintf(stderr, "rangeselftest: P9 fixtures are not three distinct bands"
                                "  ANTI-VACUITY LOST\n");
                bad++;
            }
            std::swap(app.images, pool);
            app.seqLoadingId = saveLoadingId;
            app.seqCancel = saveCancel;
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P9 app.images %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size());
                bad++;
            }
        }

        {   // ---- P10: which Range button is the range in force -------------
            // The row's whole claim, asserted on the predicate itself rather
            // than through a draw: rangeButtonMatches is where the rule lives
            // and the five buttons are a thin wrapper over it (#248 (2)).
            // Fixture: the clean ramp of P2 - data exactly 0.3..0.6, whose
            // 0.1/99.9 percentiles are the hand-derived 0.3003 / 0.5997.
            const bool saveLink = app.linkRange;
            app.linkRange = false;
            auto d = mkRamp("btn", 0.3f, 0.6f, 0, 0.0f);
            struct Case { const char* what; float b, w; int expect; };
            // expect is a bitmask over RB_Auto..RB_0_65535
            const int mAuto = 1 << RB_Auto, mPct = 1 << RB_AutoPct,
                      m01 = 1 << RB_0_1, m0255 = 1 << RB_0_255, m0655 = 1 << RB_0_65535;
            // the percentile answer has to be asked for once to be quoted here
            float plo = 0, phi = 0;
            percentileRange(*d, 0.001f, 0.999f, plo, phi);
            const Case cases[] = {
                { "min..max",      d->vmin, d->vmax, mAuto },
                { "0.1-99.9%",     plo,     phi,     mPct  },
                { "0-1",           0.0f,    1.0f,    m01   },
                { "0-255",         0.0f,    255.0f,  m0255 },
                { "0-65535",       0.0f,    65535.0f,m0655 },
                { "hand-typed",    0.31f,   0.59f,   0     },
            };
            for (const Case& c : cases) {
                int got = 0;
                for (int b = RB_Auto; b <= RB_0_65535; b++)
                    if (rangeButtonMatches(*d, b, c.b, c.w)) got |= 1 << b;
                fprintf(stderr, "rangeselftest: P10 %-10s %.6g..%.6g | lit 0x%02x "
                                "(expect 0x%02x) %s\n",
                        c.what, c.b, c.w, got, c.expect, got == c.expect ? "ok" : "WRONG");
                if (got != c.expect) bad++;
            }
            // The row is NOT a radio group: data running exactly 0..1 makes
            // Auto and 0-1 the same answer and BOTH light. Hiding one of two
            // true facts to keep the look of a selector would misdescribe the
            // range that is actually in force.
            auto u = mkRamp("btn01", 0.0f, 1.0f, 0, 0.0f);
            int both = 0;
            for (int b = RB_Auto; b <= RB_0_65535; b++)
                if (rangeButtonMatches(*u, b, 0.0f, 1.0f)) both |= 1 << b;
            fprintf(stderr, "rangeselftest: P10 data 0..1 | lit 0x%02x (expect Auto+0-1 = 0x%02x) %s\n",
                    both, mAuto | m01, both == (mAuto | m01) ? "ok" : "WRONG");
            if (both != (mAuto | m01)) bad++;
            // ...and the percentile answer is cached, and the cache is dropped
            // when the pixels are re-measured. Never a 65536-bin pass per UI
            // frame; never a stale answer after a reload/crop/detrend.
            bool cached = d->pctValid && d->pctLo == plo && d->pctHi == phi;
            computeMinMax(*d);
            bool dropped = !d->pctValid;
            fprintf(stderr, "rangeselftest: P10 pct cache | held after use %s | "
                            "dropped by computeMinMax %s\n",
                    cached ? "ok" : "WRONG", dropped ? "ok" : "WRONG");
            if (!cached || !dropped) bad++;
            // Linked mode: the fixed presets and Auto still answer, Auto %
            // deliberately does not (its linked answer reads every open pixel;
            // see rangeButtonMatches).
            app.linkRange = true;
            bool quiet = !rangeButtonMatches(*d, RB_AutoPct, plo, phi) &&
                         rangeButtonMatches(*d, RB_0_255, 0.0f, 255.0f);
            fprintf(stderr, "rangeselftest: P10 linked | Auto %% stays dark, presets answer %s\n",
                    quiet ? "ok" : "WRONG");
            if (!quiet) bad++;
            app.linkRange = saveLink;
        }

        // ==================================================================
        // #252 - the third fit, "Med": put the median at `target`. Same
        // discipline again: in-memory fixtures, every number below derived by
        // hand from them, and every borrowed mode handed back to the home the
        // whole selftest suite shares.
        //
        // The closed form all of P11-P14 lean on: a uniform ramp over
        // [0.3, 0.6] with 128x128 = 16384 pixels has median 0.45, so
        //     white = black + (0.45 - black) / target
        // The histogram answers 0.449991 rather than 0.45 exactly - 65536 bins
        // over a 0.3-wide span, and the cut opens outward to the bin's LOWER
        // edge - i.e. it is short by less than one bin (4.6e-6), which is far
        // finer than the 1e-3 these check to and finer than the black/white
        // field ever prints. The white points inherit that shortfall divided
        // by the target: 0.599982 for 0.5, 0.899963 for 0.25.
        // ==================================================================
        {   // ---- P11: median -> target, in closed form ---------------------
            const bool saveLink = app.linkRange;
            const int  saveFlav = app.autoFlavor, saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            auto d = mkRamp("med", 0.3f, 0.6f, 0, 0.0f);
            float med = 0;
            bool gotMed = medianValue(*d, med);
            bool medOk = gotMed && fabsf(med - 0.45f) < 1e-3f;
            fprintf(stderr, "rangeselftest: P11 median of the 0.3..0.6 ramp = %.6g "
                            "(closed form 0.45) %s\n", med, medOk ? "ok" : "WRONG");
            if (!medOk) bad++;

            struct MCase { const char* what; int bmode; float bval, target, wantLo, wantHi; };
            const MCase mcases[] = {
                // black = min = 0.3, target 0.5 -> 0.3 + 0.15/0.5  = 0.6
                { "min, t=0.5",   MB_Min,   0.0f, 0.50f, 0.3f, 0.6f },
                // black = min = 0.3, target 0.25 -> 0.3 + 0.15/0.25 = 0.9
                { "min, t=0.25",  MB_Min,   0.0f, 0.25f, 0.3f, 0.9f },
                // black = 0 (typed), target 0.5 -> 0 + 0.45/0.5    = 0.9
                { "value 0, t=0.5", MB_Value, 0.0f, 0.50f, 0.0f, 0.9f },
            };
            for (const MCase& c : mcases) {
                app.medBlackMode = c.bmode;
                app.medBlackValue = c.bval;
                app.medTarget = c.target;
                float lo = 0, hi = 0;
                medianFit(*d, lo, hi);
                bool ok = fabsf(lo - c.wantLo) < 1e-3f && fabsf(hi - c.wantHi) < 1e-3f;
                fprintf(stderr, "rangeselftest: P11 %-15s -> %.6g..%.6g "
                                "(expect %.6g..%.6g) %s\n",
                        c.what, lo, hi, c.wantLo, c.wantHi, ok ? "ok" : "WRONG");
                if (!ok) bad++;
            }
            // ...and the degenerate case takes the Auto % convention: a black
            // point at or above the median leaves no positive white to derive,
            // so the fit hands back min..max rather than an inverted range.
            app.medBlackMode = MB_Value;
            app.medBlackValue = 0.9f;              // above the median, and above vmax
            app.medTarget = 0.5f;
            float dlo = -7, dhi = -7;
            medianFit(*d, dlo, dhi);
            bool fell = dlo == d->vmin && dhi == d->vmax && dhi > dlo;
            fprintf(stderr, "rangeselftest: P11 black 0.9 >= median | %.6g..%.6g | %s\n",
                    dlo, dhi, fell ? "falls back to min..max" : "WRONG");
            if (!fell) bad++;
            // ...and so does a target outside the open interval (0, 1).
            app.medBlackValue = 0.0f;
            bool badTarget = true;
            for (float t : { 0.0f, 1.0f, -0.5f, 2.0f }) {
                app.medTarget = t;
                float a = -7, b = -7;
                medianFit(*d, a, b);
                if (!(a == d->vmin && b == d->vmax)) badTarget = false;
            }
            fprintf(stderr, "rangeselftest: P11 target outside (0,1) falls back: %s\n",
                    badTarget ? "ok" : "WRONG");
            if (!badTarget) bad++;
            app.linkRange = saveLink; app.autoFlavor = saveFlav;
            app.medBlackMode = saveBM; app.medTarget = saveTgt; app.medBlackValue = saveBV;
        }

        {   // ---- P12: black = "min 0.1%" IS the Auto % lower cut -----------
            // The reason the third black mode exists: one cold pixel or one
            // dead column owns vmin, and a fit measured up from THAT starts
            // 500 DN below anything real. The claim is an identity - the black
            // point the Med fit uses is the very number Auto % puts at its
            // lower end - so it is asserted as one, exactly.
            //   Fixture: the 0.3..0.6 ramp with its top 4 pixels replaced by
            //   -500, so vmin = -500 and vmax = 0.599927 (the 5th value down).
            const bool saveLink = app.linkRange;
            const int  saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            app.medTarget = 0.5f;
            auto d = mkRamp("medcold", 0.3f, 0.6f, 4, -500.0f);
            float plo = 0, phi = 0;
            percentileRange(*d, 0.001f, 0.999f, plo, phi);
            app.medBlackMode = MB_MinPct;
            float clo = 0, chi = 0;
            medianFit(*d, clo, chi);
            app.medBlackMode = MB_Min;
            float mlo = 0, mhi = 0;
            medianFit(*d, mlo, mhi);
            bool isPct = clo == plo;
            bool isMin = mlo == d->vmin && d->vmin == -500.0f;
            // anti-vacuity: the outlier really does own vmin, so the two black
            // modes are genuinely different answers here
            bool differ = clo != mlo && plo > d->vmin;
            fprintf(stderr, "rangeselftest: P12 black min-0.1%% %.6g (Auto %% lo %.6g) %s | "
                            "black min %.6g (vmin %.6g) %s | different answers %s\n",
                    clo, plo, isPct ? "ok" : "WRONG", mlo, d->vmin, isMin ? "ok" : "WRONG",
                    differ ? "ok" : "ANTI-VACUITY LOST");
            if (!isPct || !isMin || !differ) bad++;
            // ...and the white point is derived from THAT black, not from vmin
            float med = 0;
            medianValue(*d, med);
            float want = clo + (med - clo) / 0.5f;
            bool derived = fabsf(chi - want) < 1e-3f;
            fprintf(stderr, "rangeselftest: P12 white %.6g = black + (median %.6g - black)/0.5 "
                            "= %.6g %s\n", chi, med, want, derived ? "ok" : "WRONG");
            if (!derived) bad++;
            app.linkRange = saveLink; app.medBlackMode = saveBM;
            app.medTarget = saveTgt; app.medBlackValue = saveBV;
        }

        {   // ---- P13: "Auto per frame" now has a flavor, and obeys it ------
            // P5's shape, one level up: mode 0 used to mean min..max and only
            // min..max. The claim is that both re-fit sites read app.autoFlavor
            // - and the anti-vacuity half is that the flavors give DIFFERENT
            // answers on this fixture, so P13 would fail outright if the
            // flavour were ignored and minmax applied.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope, saveFlav = app.autoFlavor;
            const int  saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            app.medBlackMode = MB_Value; app.medBlackValue = 0.0f; app.medTarget = 0.5f;
            auto d = mkRamp("flav", 0.3f, 0.6f, 0, 0.0f);
            // black = 0 typed, median 0.45, target 0.5 -> 0 .. 0.9
            d->black = -5; d->white = 5;                   // clearly not any fit
            applyRangeMode(0, d.get(), AF_Median);         // what the combo calls
            bool medFit = app.autoFlavor == AF_Median &&
                          d->black == 0.0f && fabsf(d->white - 0.9f) < 1e-3f;
            fprintf(stderr, "rangeselftest: P13 flavor median %.6g..%.6g (expect 0..0.9) %s\n",
                    d->black, d->white, medFit ? "ok" : "WRONG");
            if (!medFit) bad++;
            // ...and the same call with the minmax flavor is the old behaviour,
            // unchanged. These two must not be the same numbers, or the test
            // above proves nothing.
            d->black = -5; d->white = 5;
            applyRangeMode(0, d.get(), AF_MinMax);
            bool mmFit = app.autoFlavor == AF_MinMax &&
                         d->black == d->vmin && d->white == d->vmax;
            bool distinct = !(d->vmin == 0.0f && fabsf(d->vmax - 0.9f) < 1e-3f);
            fprintf(stderr, "rangeselftest: P13 flavor minmax %.6g..%.6g (expect %.6g..%.6g) %s"
                            " | the two flavors differ %s\n",
                    d->black, d->white, d->vmin, d->vmax, mmFit ? "ok" : "WRONG",
                    distinct ? "ok" : "ANTI-VACUITY LOST");
            if (!mmFit || !distinct) bad++;
            // ...and the pct flavor is wired too, to its own cached answer
            d->black = -5; d->white = 5;
            applyRangeMode(0, d.get(), AF_Pct);
            float qlo = 0, qhi = 0;
            percentileRange(*d, 0.001f, 0.999f, qlo, qhi);
            bool pctFit = d->black == qlo && d->white == qhi && qlo > d->vmin;
            fprintf(stderr, "rangeselftest: P13 flavor pct %.6g..%.6g (expect %.6g..%.6g) %s\n",
                    d->black, d->white, qlo, qhi, pctFit ? "ok" : "WRONG");
            if (!pctFit) bad++;

            // ...and the OTHER re-fit site: stepping a frame (P6's shape).
            // selectImage is where the mode is applied while you work, and it
            // has to read the same flavor the combo just set.
            app.rangeScope = 0;
            app.autoFlavor = AF_Median;
            selectImage(fa[0]);
            selectImage(fa[1]);
            ImageDoc* f = cur();
            float wlo = 0, whi = 0;
            if (f) medianFit(*f, wlo, whi);
            bool stepped = f && f->black == wlo && f->white == whi;
            bool notMinMax = f && !(wlo == f->vmin && whi == f->vmax);
            fprintf(stderr, "rangeselftest: P13 stepped frame holds %.6g..%.6g "
                            "(its own median fit %.6g..%.6g, data %.6g..%.6g) %s | "
                            "not its min..max %s\n",
                    f ? f->black : 0.f, f ? f->white : 0.f, wlo, whi,
                    f ? f->vmin : 0.f, f ? f->vmax : 0.f,
                    stepped ? "ok" : "WRONG", notMinMax ? "ok" : "ANTI-VACUITY LOST");
            if (!stepped || !notMinMax) bad++;
            app.linkRange = saveLink; app.rangeScope = saveScope; app.autoFlavor = saveFlav;
            app.medBlackMode = saveBM; app.medTarget = saveTgt; app.medBlackValue = saveBV;
        }

        {   // ---- P14: the Med lamp, and the session round trip -------------
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope, saveFlav = app.autoFlavor;
            const int  saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            app.rangeScope = 0;                    // a lone fixture is not a stack
            app.medBlackMode = MB_Min; app.medTarget = 0.5f; app.medBlackValue = 0.0f;
            auto d = mkRamp("medbtn", 0.3f, 0.6f, 0, 0.0f);
            float lo = 0, hi = 0;
            medianFit(*d, lo, hi);                 // 0.3 .. 0.6 (see P11)
            bool onExact = rangeButtonMatches(*d, RB_Median, lo, hi);
            bool offTyped = !rangeButtonMatches(*d, RB_Median, 0.3f, 0.55f);
            fprintf(stderr, "rangeselftest: P14 Med lamp | fit %.6g..%.6g lights %s | "
                            "hand-typed 0.3..0.55 dark %s\n",
                    lo, hi, onExact ? "ok" : "WRONG", offTyped ? "ok" : "WRONG");
            if (!onExact || !offTyped) bad++;
            // The lamp answers by COMPARISON, so moving the target moves the
            // answer and the old range stops being it - no state to stale.
            app.medTarget = 0.25f;
            bool wentOut = !rangeButtonMatches(*d, RB_Median, lo, hi);
            float lo2 = 0, hi2 = 0;
            medianFit(*d, lo2, hi2);               // 0.3 .. 0.9
            bool newOne = rangeButtonMatches(*d, RB_Median, lo2, hi2) &&
                          fabsf(hi2 - 0.9f) < 1e-3f;
            fprintf(stderr, "rangeselftest: P14 target 0.5 -> 0.25 | old range dark %s | "
                            "new fit %.6g..%.6g lights %s\n",
                    wentOut ? "ok" : "WRONG", lo2, hi2, newOne ? "ok" : "WRONG");
            if (!wentOut || !newOne) bad++;
            // Linked pools every open pixel to answer, exactly as Auto % does,
            // so Med is dark there for exactly the same reason.
            app.linkRange = true;
            bool quiet = !rangeButtonMatches(*d, RB_Median, lo2, hi2);
            app.linkRange = false;
            fprintf(stderr, "rangeselftest: P14 linked | Med stays dark %s\n",
                    quiet ? "ok" : "WRONG");
            if (!quiet) bad++;
            // ...and the median is cached and dropped where the pct answer is
            app.medTarget = 0.5f;
            bool cached = d->medValid && fabsf(d->medVal - 0.45f) < 1e-3f;
            computeMinMax(*d);
            bool dropped = !d->medValid;
            fprintf(stderr, "rangeselftest: P14 median cache | held after use %s | "
                            "dropped by computeMinMax %s\n",
                    cached ? "ok" : "WRONG", dropped ? "ok" : "WRONG");
            if (!cached || !dropped) bad++;

            // ---- the session round trip -------------------------------------
            // app.images is SWAPPED OUT for this (the P4 discipline, and then
            // some): loadSession closes every open, and the frames this test
            // still needs must not be in the list when it does.
            const size_t imgWas = app.images.size();
            std::vector<std::unique_ptr<ImageDoc>> pool;
            std::swap(app.images, pool);
            std::error_code tec;
            std::string sess = (std::filesystem::temp_directory_path(tec) /
                                "viewer_rangeselftest_flavor.vsession").u8string();
            app.autoFlavor = AF_Median;
            app.medTarget = 0.25f;                 // exact in the stream's 9 digits
            app.medBlackMode = MB_MinPct;
            app.medBlackValue = 0.125f;            // ...and so is this
            saveSession(sess, true);
            app.autoFlavor = AF_MinMax;            // scrambled, so a no-op load shows
            app.medTarget = 0.5f;
            app.medBlackMode = MB_Value;
            app.medBlackValue = 0.0f;
            std::string serr = loadSession(sess);
            bool trip = serr.empty() && app.autoFlavor == AF_Median &&
                        app.medTarget == 0.25f && app.medBlackMode == MB_MinPct &&
                        app.medBlackValue == 0.125f;
            fprintf(stderr, "rangeselftest: P14 session round trip err=\"%s\" | "
                            "flavor %d target %.6g black %d value %.6g "
                            "(expect 2 / 0.25 / 2 / 0.125) %s\n",
                    serr.c_str(), app.autoFlavor, app.medTarget, app.medBlackMode,
                    app.medBlackValue, trip ? "ok" : "WRONG");
            if (!trip) bad++;
            // ...and a value the program must never take: a target of 0 divides.
            {
                std::ofstream bf(pathFromUtf8(sess), std::ios::binary | std::ios::app);
                bf << "autoflavor 9 0 7 1\n";
            }
            std::string berr = loadSession(sess);
            bool refused = berr.empty() && app.autoFlavor == AF_Median &&
                           app.medTarget == 0.25f && app.medBlackMode == MB_MinPct &&
                           app.medBlackValue == 1.0f;   // the only field in range
            fprintf(stderr, "rangeselftest: P14 out-of-range line refused | "
                            "flavor %d target %.6g black %d value %.6g %s\n",
                    app.autoFlavor, app.medTarget, app.medBlackMode, app.medBlackValue,
                    refused ? "ok" : "WRONG");
            if (!refused) bad++;
            std::error_code rec;
            std::filesystem::remove(pathFromUtf8(sess), rec);
            std::swap(app.images, pool);
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P14 app.images %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size());
                bad++;
            }
            app.linkRange = saveLink; app.rangeScope = saveScope; app.autoFlavor = saveFlav;
            app.medBlackMode = saveBM; app.medTarget = saveTgt; app.medBlackValue = saveBV;
        }

        // ==================================================================
        // #254 - in "Per stack", the lamp answers for the REFERENCE frame.
        // ==================================================================
        {   // ---- P15: the shared range keeps its lamp on every frame -------
            // The report: 「Stack で Auto の設定が外れる」. Press Auto in
            // per-stack mode and the fit is spread over the whole stack
            // (#248) - but the row compared the shared numbers against the
            // frame ON SCREEN, so on the next frame they were nobody's fit and
            // the lamp went out. The range was never lost; the display was.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeScope;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            auto A5 = mkRamp("ref_A", 0.3f, 0.6f, 0, 0.0f);   // data 0.3..0.6
            auto B5 = mkRamp("ref_B", 2.0f, 3.0f, 0, 0.0f);   // data 2..3
            A5->seqId = 4245; A5->seqIndex = 0;
            B5->seqId = 4245; B5->seqIndex = 1;
            ImageDoc* pA = A5.get(); ImageDoc* pB = B5.get();
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A5));
            pool.push_back(std::move(B5));
            std::swap(app.images, pool);
            App::SeqInfo si;                  // the stack these two are frames of:
            si.id = 4245; si.name = "ref";    // seqInfo is where the reference is kept
            app.seqs.push_back(si);
            app.linkRange = false;
            app.rangeScope = 1;                        // per stack
            setRange(*pA, pA->vmin, pA->vmax);         // what "Auto" on A presses
            bool spread = pB->black == 0.3f && pB->white == 0.6f;
            bool litA = rangeButtonMatches(*pA, RB_Auto, pA->black, pA->white);
            bool litB = rangeButtonMatches(*pB, RB_Auto, pB->black, pB->white);
            // anti-vacuity: B's own min..max is 2..3, so the shared 0.3..0.6 is
            // emphatically NOT B's own fit - which is why it used to go dark
            bool notBs = !(pB->black == pB->vmin && pB->white == pB->vmax);
            fprintf(stderr, "rangeselftest: P15 per-stack Auto | spread %s | lamp on A %s, "
                            "on sibling %s (sibling data %.6g..%.6g, range %.6g..%.6g) %s\n",
                    spread ? "ok" : "WRONG", litA ? "ok" : "WRONG", litB ? "ok" : "WRONG",
                    pB->vmin, pB->vmax, pB->black, pB->white,
                    notBs ? "ok" : "ANTI-VACUITY LOST");
            if (!spread || !litA || !litB || !notBs) bad++;
            // "Auto per frame" is unchanged: there the frame on screen IS the
            // subject, and the sibling's inherited range is not its own fit.
            app.rangeScope = 0;
            bool perFrame = !rangeButtonMatches(*pB, RB_Auto, pB->black, pB->white) &&
                            rangeButtonMatches(*pB, RB_Auto, pB->vmin, pB->vmax);
            fprintf(stderr, "rangeselftest: P15 auto-per-frame asks the frame itself: %s\n",
                    perFrame ? "ok" : "WRONG");
            if (!perFrame) bad++;
            // ...and a reference that is gone is not silently replaced by the
            // frame on screen: no comparison to make, no claim to publish.
            app.rangeScope = 1;
            if (App::SeqInfo* s = seqInfo(4245)) s->rangeRefUid = app.nextUid + 999;
            bool darkGone = !rangeButtonMatches(*pB, RB_Auto, pB->black, pB->white) &&
                            !rangeButtonMatches(*pB, RB_Median, pB->black, pB->white);
            // the fixed presets are nobody's measurement and still answer
            bool presets = rangeButtonMatches(*pB, RB_0_255, 0.0f, 255.0f);
            fprintf(stderr, "rangeselftest: P15 reference frame gone | fits dark %s | "
                            "fixed presets still answer %s\n",
                    darkGone ? "ok" : "WRONG", presets ? "ok" : "WRONG");
            if (!darkGone || !presets) bad++;
            for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                if (it->id == 4245) { app.seqs.erase(it); break; }
            std::swap(app.images, pool);
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: P15 app.images %zu -> %zu, seqs %zu -> %zu"
                                "  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
            app.linkRange = saveLink;
            app.rangeScope = saveScope;
        }

        // the shared home gets the modes it was lent, back
        app.rangeScope = 1;
        app.linkRange = false;
        app.autoFlavor = AF_MinMax;
        app.medTarget = 0.5f;
        app.medBlackMode = MB_Value;
        app.medBlackValue = 0;

        fprintf(stderr, "rangeselftest: %s\n", bad ? "FAILED" : "ok");
        if (app.seqThread.joinable()) app.seqThread.join();
        return bad ? 1 : 0;
    }
