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
        static const char* SHARE[4] = { "each-own", "B-uses-A", "union-auto", "each-fits" };
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
                    g_abRangeDefault >= 0 && g_abRangeDefault < 4 ? SHARE[g_abRangeDefault] : "?",
                    g_abRangeDefault == 2 ? "" : "  <- EXPECTED 2 (union-auto)");
            if (g_abRangeDefault != 2) bad++;
        }
        {   // The axis label has to name the range ACTUALLY in force. It read
            // "A's black-white range" in every mode, including the union - a
            // claim about the picture that the picture does not support.
            int save = app.compareRangeMode;
            bool saveLink = app.linkRange;
            // ...and "in force" includes the Value range scope. linkRange
            // short-circuits displayRangeFor before the A/B mode is consulted,
            // so with the scope set to everything the axis spans the linked pair
            // - widened by stacks that have nothing to do with the comparison -
            // whatever the A/B combo happens to say.
            for (int lk = 0; lk < 2; lk++) {
                app.linkRange = lk != 0;
                if (app.linkRange) {
                    app.linkBlack = app.images[fa[0]]->vmin;
                    app.linkWhite = app.images[fa[0]]->vmax;
                }
                for (int sh = 0; sh < 4; sh++) {
                    app.compareRangeMode = sh;
                    recomputeHistogramIfNeeded(cur(), app.hist[0]);
                    std::string lab = abHistXLabel(cur(), cmpB(), app.hist[0]);
                    // the words of the Inspector's `in force` line (#321 I4)
                    bool named = app.linkRange
                                     ? lab.find("one range over all open images") != std::string::npos
                                     : sh == 2
                                         ? lab.find("union over A, B") != std::string::npos
                                     : sh == 3
                                         ? lab.find("each side fits itself (") != std::string::npos
                                     : sh == 1
                                         ? lab.find("A's range for every side") != std::string::npos
                                         : lab.find("one range for the stack") != std::string::npos;
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
            app.rangeRule = m == 0 ? 0 : 1;
            app.linkRange = m == 2;
            if (app.linkRange) { app.linkBlack = app.images[fa[0]]->vmin;
                                 app.linkWhite = app.images[fa[0]]->vmax; }
            for (int sh = 0; sh < 4; sh++) {
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
                    // the contract: the shared modes must produce one range
                    // ("each-own" and "each-fits" are per side by definition;
                    // under Link range every mode is one range)
                    if ((app.linkRange || (sh != 0 && sh != 3)) && !same) bad++;
                    // follow-frame must keep the sides on the same frame number
                    if (B->seqIndex != A->seqIndex) {
                        fprintf(stderr, "rangeselftest: FOLLOW BROKEN A#%d vs B#%d\n",
                                A->seqIndex, B->seqIndex);
                        bad++;
                    }
                }
            }
        }
        // Switching TO "each frame fits itself" must show on the frame on
        // screen at once (it used to look dead until the first step: "選んだ
        // 直後は画像がかわらず、コマ送りすると反映される") - and, since #321,
        // without writing the frame's stored numbers (I3).
        {
            const int saveCmpR0 = app.compareRangeMode;
            app.compareRangeMode = 0;                 // each keeps its own: A is its own
            app.linkRange = false;
            app.rangeRule = RR_OneRange;              // per stack
            selectImage(fa[1]);                       // a frame that inherited
            ImageDoc* d = cur();
            d->black = d->vmin - 1000; d->white = d->vmax + 1000;   // clearly not its own fit
            float wasB = d->black, wasW = d->white;
            setRangeRule(RR_EachFrame, d);     // exactly what the rule row calls
            bool shown = effBlack(*d) == d->vmin && effWhite(*d) == d->vmax;
            bool kept = d->black == wasB && d->white == wasW;
            fprintf(stderr, "rangeselftest: each-frame applied on the spot: drawn %.6g..%.6g "
                            "(frame data %.6g..%.6g) %s | stored %.6g..%.6g kept %s\n",
                    effBlack(*d), effWhite(*d), d->vmin, d->vmax, shown ? "ok" : "WRONG",
                    d->black, d->white, kept ? "ok" : "WRONG");
            if (!shown || !kept) bad++;
            app.rangeRule = RR_OneRange;
            app.compareRangeMode = saveCmpR0;
        }

        // ---- P17: "A/B: each side fits itself" (#301) ----------------------
        // Three stacks at three levels (multi/00, 01, 02): A, B and slot C on
        // three different exposures. Every compared side must be DRAWN at
        // exactly the fit that side would get on its own - in each of the three
        // flavours - while its own black/white stay untouched; mode 2 must go
        // back to the union; and stepping with follow-frame must re-fit every
        // side to its NEW frame.
        {
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    app.seqs.size() >= 3) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (app.seqs.size() < 3) {
                fprintf(stderr, "rangeselftest: P17 needs 3 stacks, have %d  WRONG\n",
                        (int)app.seqs.size());
                bad++;
            } else {
                auto ok = [&](bool c, const char* what) {
                    fprintf(stderr, "rangeselftest: P17 %s %s\n", what, c ? "ok" : "WRONG");
                    if (!c) bad++;
                };
                const int saveCmpR = app.compareRangeMode, saveFlavor = app.autoFlavor;
                const auto saveCmpMode = app.compareMode;
                const bool saveFollow = app.compareFollowFrame;
                std::vector<int> g0 = framesOfSeq(app.seqs[0].id), g1 = framesOfSeq(app.seqs[1].id),
                                 g2 = framesOfSeq(app.seqs[2].id);
                app.linkRange = false;
                app.rangeRule = 1;                    // per stack: the default
                app.compareFollowFrame = true;
                selectImage(g0[0]);
                app.cmpExtra.clear();
                setCompareB(app.images[g1[0]].get());
                addCompareSlot(app.images[g2[0]].get());
                app.compareMode = App::CmpSplit;
                std::vector<ImageDoc*> ex = compareExtras();
                ImageDoc* A = cur();
                ImageDoc* B = cmpB();
                ImageDoc* C = ex.size() == 1 ? ex[0] : nullptr;
                ok(A && B && C && A != B && B != C && A != C, "fixture: A, B and slot C on three stacks");
                // A stored range no fit can land on, on every frame of all three
                // stacks: a side showing its doc's own range is caught at once.
                // (Saved and put back after: the blocks below read these stacks.)
                std::vector<std::pair<ImageDoc*, std::pair<float, float>>> stored;
                for (const auto* g : { &g0, &g1, &g2 })
                    for (int i : *g) {
                        ImageDoc* d = app.images[i].get();
                        stored.push_back({ d, { d->black, d->white } });
                        d->black = d->vmin - 1000.0f; d->white = d->vmax + 1000.0f;
                    }
                // "that doc alone's fit": what each frame fits itself draws it at
                auto aloneFit = [](ImageDoc* d, float& lo, float& hi) { fitByFlavor(*d, lo, hi); };
                auto untouched = [](const ImageDoc* d) {
                    return d->black == d->vmin - 1000.0f && d->white == d->vmax + 1000.0f;
                };
                static const char* FL[3] = { "min-max", "0.1-99.9%", "median->target" };
                static const int FL_BTN[3] = { RB_Auto, RB_AutoPct, RB_Median };
                static const char SIDE[3] = { 'A', 'B', 'C' };
                const float saveTarget = app.medTarget, saveBlackValue = app.medBlackValue;
                const int saveBlackMode = app.medBlackMode;
                if (A && B && C) {
                    // The flavour is picked through the mode's own selector
                    // (setFlavor - what the radio row under the A/B combo
                    // and the View menu items call), and every side must move
                    // with it: the selector exists so "each side at Auto %" can
                    // be chosen at all under Per stack.
                    app.compareRangeMode = 3;
                    setFlavor(AF_Median);           // start away from min-max
                    float prevLo[3] = {}, prevHi[3] = {};
                    {   int k = 0;
                        for (ImageDoc* d : { A, B, C }) { prevLo[k] = effBlack(*d); prevHi[k] = effWhite(*d); k++; } }
                    for (int fl = AF_MinMax; fl <= AF_Median; fl++) {
                        setFlavor(fl);
                        bool each = true, lamps = true, moved = true;
                        float w[3] = {};
                        int k = 0;
                        for (ImageDoc* d : { A, B, C }) {
                            float lo = 0, hi = 0;
                            aloneFit(d, lo, hi);
                            const float eb = effBlack(*d), ew = effWhite(*d);
                            fprintf(stderr, "rangeselftest: P17 %-14s side %c  shown %.6g..%.6g  "
                                            "alone %.6g..%.6g\n", FL[fl], SIDE[k], eb, ew, lo, hi);
                            each = each && app.autoFlavor == fl && eb == lo && ew == hi && untouched(d);
                            lamps = lamps && rangeButtonMatches(*d, FL_BTN[fl], eb, ew);
                            moved = moved && (eb != prevLo[k] || ew != prevHi[k]);
                            prevLo[k] = eb; prevHi[k] = ew;
                            w[k++] = ew;
                        }
                        char what[112];
                        snprintf(what, sizeof what, "%s: every side shown at its own fit, docs untouched", FL[fl]);
                        ok(each, what);
                        snprintf(what, sizeof what, "%s: the three fits really differ", FL[fl]);
                        ok(w[0] != w[1] && w[1] != w[2] && w[0] != w[2], what);
                        snprintf(what, sizeof what, "%s: the Range row lights its button on every side", FL[fl]);
                        ok(lamps, what);
                        snprintf(what, sizeof what, "selector -> %s: every side's fit moved", FL[fl]);
                        ok(moved, what);
                    }
                    // Same flavour, different Med parameters: the per-side cache
                    // is keyed on them, so every side must re-fit.
                    {
                        bool tgt = true, blk = true;
                        float was[3];
                        int k = 0;
                        for (ImageDoc* d : { A, B, C }) was[k++] = effWhite(*d);
                        app.medTarget = 0.4f;
                        k = 0;
                        for (ImageDoc* d : { A, B, C }) {
                            float lo = 0, hi = 0;
                            aloneFit(d, lo, hi);
                            tgt = tgt && effBlack(*d) == lo && effWhite(*d) == hi && effWhite(*d) != was[k++];
                        }
                        ok(tgt, "median->target: a new target re-fits every side");
                        k = 0;
                        for (ImageDoc* d : { A, B, C }) was[k++] = effBlack(*d);
                        app.medBlackMode = MB_Min;          // black: the value 0 -> each side's min
                        k = 0;
                        for (ImageDoc* d : { A, B, C }) {
                            float lo = 0, hi = 0;
                            aloneFit(d, lo, hi);
                            blk = blk && effBlack(*d) == lo && effWhite(*d) == hi && effBlack(*d) != was[k++];
                        }
                        ok(blk, "median->target: a new black-point mode re-fits every side");
                        app.medTarget = saveTarget;
                        app.medBlackMode = saveBlackMode;
                        app.medBlackValue = saveBlackValue;
                    }
                    // New pixels on one side (computeMinMax is where reload, crop
                    // and detrend arrive): that side re-fits, from the new pixels.
                    {
                        setFlavor(AF_Pct);
                        const float wasLo = effBlack(*B);
                        const std::vector<float> keep = B->px();
                        for (float& v : B->px()) v += 100.0f;
                        B->dataRev++;
                        computeMinMax(*B);
                        float lo = 0, hi = 0;
                        aloneFit(B, lo, hi);
                        const bool fresh = effBlack(*B) == lo && effWhite(*B) == hi && effBlack(*B) != wasLo;
                        B->px() = keep;
                        B->dataRev++;
                        computeMinMax(*B);
                        ok(fresh, "new pixels + computeMinMax: that side re-fits from them");
                    }
                    app.compareRangeMode = 2;              // back to the union
                    const float ulo = std::min({ A->vmin, B->vmin, C->vmin });
                    const float uhi = std::max({ A->vmax, B->vmax, C->vmax });
                    bool uni = true;
                    for (ImageDoc* d : { A, B, C })
                        uni = uni && effBlack(*d) == ulo && effWhite(*d) == uhi && untouched(d);
                    ok(uni, "mode 2 again: every side back on the union");
                    // step: follow-frame moves B and C onto A's new frame number
                    app.compareRangeMode = 3;
                    setFlavor(AF_Pct);
                    const float wasB = effWhite(*B), wasC = effWhite(*C);
                    selectImage(g0[1]);
                    std::vector<ImageDoc*> ex2 = compareExtras();
                    ImageDoc* B2 = cmpB();
                    ImageDoc* C2 = ex2.size() == 1 ? ex2[0] : nullptr;
                    ok(B2 && C2 && B2 != B && C2 != C && B2->seqIndex == cur()->seqIndex &&
                       C2->seqIndex == cur()->seqIndex, "step: B and C followed A to its new frame");
                    if (B2 && C2) {
                        bool refit = true;
                        for (ImageDoc* d : { cur(), B2, C2 }) {
                            float lo = 0, hi = 0;
                            aloneFit(d, lo, hi);
                            refit = refit && effBlack(*d) == lo && effWhite(*d) == hi;
                        }
                        refit = refit && untouched(B2) && untouched(C2);
                        fprintf(stderr, "rangeselftest: P17 step  B white %.6g -> %.6g  C white %.6g -> %.6g\n",
                                wasB, effWhite(*B2), wasC, effWhite(*C2));
                        ok(refit && effWhite(*B2) != wasB && effWhite(*C2) != wasC,
                           "step: every side re-fitted to its own new frame");
                        // Per stack, on frames that are NOT the frame whose range
                        // the stack shares: the lamp must still compare each side
                        // against its own pixels, because that is the range in force.
                        bool lamps = app.rangeRule == 1;
                        for (ImageDoc* d : { B2, C2 })
                            lamps = lamps && rangeButtonMatches(*d, RB_AutoPct, effBlack(*d), effWhite(*d));
                        ok(lamps, "step, Per stack: Auto % lit on B and C at their own new fit");
                    }
                }
                for (auto& s : stored) { s.first->black = s.second.first; s.first->white = s.second.second; }
                app.cmpExtra.clear();           // A ends on g0[1], the frame it arrived on
                app.compareMode = saveCmpMode;
                app.compareFollowFrame = saveFollow;
                app.compareRangeMode = saveCmpR;
                app.autoFlavor = saveFlavor;
            }
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
            const int  saveScope = app.rangeRule;
            auto d = mkRamp("apf", 0.3f, 0.6f, 0, 0.0f);
            d->black = -5; d->white = 5;                  // clearly not its own fit
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            setRangeRule(RR_EachFrame, d.get());          // what the rule row calls
            // DRAWN at it (#321 I3: the stored -5..5 is not written)
            const float gotB = effBlack(*d), gotW = effWhite(*d);
            bool own = fabsf(gotB - d->vmin) < 1e-6f && fabsf(gotW - d->vmax) < 1e-6f &&
                       d->black == -5.0f && d->white == 5.0f;
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
            app.rangeRule = saveScope;
        }

        {   // ---- P6: and the frame-step path is wired to the same fit ------
            // selectImage is the other site that re-fits in mode 0. These
            // fixtures are floats in the 1000s, which no magnet touches, so
            // this is a wiring check (the regression is caught by P5) - it
            // asserts the frame stepped ONTO holds its OWN numbers.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeRule;
            app.linkRange = false;
            app.rangeRule = 0;
            selectImage(fa[0]);
            ImageDoc* f = app.images[fa[1]].get();
            const float sb = f->black, sw = f->white;
            selectImage(fa[1]);
            bool own = f == cur() && effBlack(*f) == f->vmin && effWhite(*f) == f->vmax &&
                       f->black == sb && f->white == sw;
            fprintf(stderr, "rangeselftest: P6 stepped frame drawn at %.6g..%.6g "
                            "(data %.6g..%.6g), stored %.6g..%.6g untouched %s\n",
                    effBlack(*f), effWhite(*f), f->vmin, f->vmax, f->black, f->white,
                    own ? "ok" : "WRONG");
            if (!own) bad++;
            app.linkRange = saveLink;
            app.rangeRule = saveScope;
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
            const int  saveScope = app.rangeRule;
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
            app.rangeRule = 1;                              // per stack
            setRange(*pA, 0.3f, 0.6f);                       // what "Auto" on A presses
            bool shared = pB->black == 0.3f && pB->white == 0.6f && pB->texDirty;
            fprintf(stderr, "rangeselftest: P7 per-stack setRange | A %.6g..%.6g -> "
                            "sibling %.6g..%.6g (expect 0.3..0.6, redrawn) %s\n",
                    pA->black, pA->white, pB->black, pB->white, shared ? "ok" : "WRONG");
            if (!shared) bad++;

            // ...and under "each frame fits itself" a written range is a
            // declaration of ONE range (#321 I2): the rule moves to one range
            // and the number reaches the whole stack. (It used to touch A
            // alone, and the next step drew the fit over it.)
            app.rangeRule = RR_EachFrame;
            pB->texDirty = false;
            setRange(*pA, 0.0f, 255.0f);
            bool declared = app.rangeRule == RR_OneRange && pB->black == 0.0f &&
                            pB->white == 255.0f && pB->texDirty &&
                            pA->black == 0.0f && pA->white == 255.0f;
            fprintf(stderr, "rangeselftest: P7 each-frame + a typed range -> one range for the stack | "
                            "A %.6g..%.6g sibling %.6g..%.6g rule %d (expect 0..255 both, rule 1) %s\n",
                    pA->black, pA->white, pB->black, pB->white, app.rangeRule,
                    declared ? "ok" : "WRONG");
            if (!declared) bad++;
            pB->black = 0.3f; pB->white = 0.6f;

            // ...and a standalone image is not a stack. Only a POSITIVE seqId
            // names one ("0 = standalone, >0 = frame of that sequence"), and
            // seqId 0 is the whole rest of the program: two unrelated singles
            // must not fuse just because the scope says "per stack". The
            // negative pass is the same claim for an id that should never
            // exist - a doc carrying one must not be treated as a stack of all
            // the others.
            app.rangeRule = 1;
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
            app.rangeRule = saveScope;
        }

        {   // ---- P8: switching TO "one range" re-shares --------------------
            // This was the reported bug itself (#248 (a)): "Auto per frame"
            // used to REWRITE each frame's stored black/white as you stepped,
            // so a stack stepped in that mode held per-frame values, and
            // raising the scope flag alone let them reappear on every step:
            // 「Per stack にしても一枚毎に Auto されてしまう」. Since #321 the
            // per-frame rule writes nothing, but a stack can still hold
            // per-frame values (an old session, the fixture below), and going
            // back to one range must spread the frame on screen's fit.
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeRule;
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
            app.rangeRule = 0;                        // coming FROM auto-per-frame
            setRangeRule(RR_OneRange, pA);             // exactly what the rule row calls
            bool reshared = app.rangeRule == 1 && !app.linkRange &&
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
            app.rangeRule = saveScope;
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
            const int  saveScope = app.rangeRule, saveFlav = app.autoFlavor;
            const int  saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            app.medBlackMode = MB_Value; app.medBlackValue = 0.0f; app.medTarget = 0.5f;
            auto d = mkRamp("flav", 0.3f, 0.6f, 0, 0.0f);
            // black = 0 typed, median 0.45, target 0.5 -> 0 .. 0.9
            d->black = -5; d->white = 5;                   // clearly not any fit
            auto eachFrameAt = [&](int fl) {               // what the rule row + fit buttons call
                app.rangeRule = RR_OneRange;
                app.autoFlavor = fl;
                setRangeRule(RR_EachFrame, d.get());
            };
            eachFrameAt(AF_Median);
            bool medFit = app.autoFlavor == AF_Median &&
                          effBlack(*d) == 0.0f && fabsf(effWhite(*d) - 0.9f) < 1e-3f;
            fprintf(stderr, "rangeselftest: P13 flavor median %.6g..%.6g (expect 0..0.9) %s\n",
                    effBlack(*d), effWhite(*d), medFit ? "ok" : "WRONG");
            if (!medFit) bad++;
            // ...and the same call with the minmax flavor is the old behaviour,
            // unchanged. These two must not be the same numbers, or the test
            // above proves nothing.
            d->black = -5; d->white = 5;
            eachFrameAt(AF_MinMax);
            bool mmFit = app.autoFlavor == AF_MinMax &&
                         effBlack(*d) == d->vmin && effWhite(*d) == d->vmax;
            bool distinct = !(d->vmin == 0.0f && fabsf(d->vmax - 0.9f) < 1e-3f);
            fprintf(stderr, "rangeselftest: P13 flavor minmax %.6g..%.6g (expect %.6g..%.6g) %s"
                            " | the two flavors differ %s\n",
                    effBlack(*d), effWhite(*d), d->vmin, d->vmax, mmFit ? "ok" : "WRONG",
                    distinct ? "ok" : "ANTI-VACUITY LOST");
            if (!mmFit || !distinct) bad++;
            // ...and the pct flavor is wired too, to its own cached answer
            d->black = -5; d->white = 5;
            eachFrameAt(AF_Pct);
            float qlo = 0, qhi = 0;
            percentileRange(*d, 0.001f, 0.999f, qlo, qhi);
            bool pctFit = effBlack(*d) == qlo && effWhite(*d) == qhi && qlo > d->vmin &&
                          d->black == -5.0f && d->white == 5.0f;   // drawn, not written
            fprintf(stderr, "rangeselftest: P13 flavor pct %.6g..%.6g (expect %.6g..%.6g) %s\n",
                    effBlack(*d), effWhite(*d), qlo, qhi, pctFit ? "ok" : "WRONG");
            if (!pctFit) bad++;

            // ...and the OTHER re-fit site: stepping a frame (P6's shape).
            // selectImage is where the mode is applied while you work, and it
            // has to read the same flavor the combo just set.
            app.rangeRule = 0;
            app.autoFlavor = AF_Median;
            selectImage(fa[0]);
            selectImage(fa[1]);
            ImageDoc* f = cur();
            float wlo = 0, whi = 0;
            if (f) medianFit(*f, wlo, whi);
            bool stepped = f && effBlack(*f) == wlo && effWhite(*f) == whi;
            bool notMinMax = f && !(wlo == f->vmin && whi == f->vmax);
            fprintf(stderr, "rangeselftest: P13 stepped frame holds %.6g..%.6g "
                            "(its own median fit %.6g..%.6g, data %.6g..%.6g) %s | "
                            "not its min..max %s\n",
                    f ? effBlack(*f) : 0.f, f ? effWhite(*f) : 0.f, wlo, whi,
                    f ? f->vmin : 0.f, f ? f->vmax : 0.f,
                    stepped ? "ok" : "WRONG", notMinMax ? "ok" : "ANTI-VACUITY LOST");
            if (!stepped || !notMinMax) bad++;
            app.linkRange = saveLink; app.rangeRule = saveScope; app.autoFlavor = saveFlav;
            app.medBlackMode = saveBM; app.medTarget = saveTgt; app.medBlackValue = saveBV;
        }

        {   // ---- P14: the Med lamp, and the session round trip -------------
            const bool saveLink = app.linkRange;
            const int  saveScope = app.rangeRule, saveFlav = app.autoFlavor;
            const int  saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            app.linkRange = false;
            app.rangeRule = 0;                    // a lone fixture is not a stack
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
            app.linkRange = saveLink; app.rangeRule = saveScope; app.autoFlavor = saveFlav;
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
            const int  saveScope = app.rangeRule;
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
            app.rangeRule = 1;                        // per stack
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
            app.rangeRule = 0;
            bool perFrame = !rangeButtonMatches(*pB, RB_Auto, pB->black, pB->white) &&
                            rangeButtonMatches(*pB, RB_Auto, pB->vmin, pB->vmax);
            fprintf(stderr, "rangeselftest: P15 auto-per-frame asks the frame itself: %s\n",
                    perFrame ? "ok" : "WRONG");
            if (!perFrame) bad++;
            // ...and a reference that is gone is not silently replaced by the
            // frame on screen: no comparison to make, no claim to publish.
            app.rangeRule = 1;
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
            app.rangeRule = saveScope;
        }

        // ==================================================================
        // #293 - the LOAD-TIME default for integer types, held as numbers.
        // The user ruled (2026-10-27, option A) that i8 and i16 default to
        // 0..the type's positive maximum: negative pixels are black until
        // Auto (min-max or %) or a hand-typed black. Until here, in THIS
        // file, only the 0..1 magnet had a test (P5); media.inc M2 / M11 pin
        // u16 through a real file. i16 and i8 had none anywhere, so i16
        // could have moved to -32768.. or to vmin.. and the suite would have
        // stayed green. i32 has no branch at all and falls to the float
        // rules; one row here pins that i32 data outside [0,1] opens at its
        // own min..max (a type-fullscale branch would turn it red). The
        // 0..1-magnet half of that rule is not tested for i32.
        // In-memory fixtures; defaultRange reads nothing but the doc, so no
        // mode is borrowed.
        // ==================================================================
        {   // ---- P16: u8 / i8 / u16 / i16 defaults, the i32 fall-through,
            //      and the float asymmetry
            // One ramp per dtype. For the types that HAVE a branch the data is
            // chosen so that a default that fell through to either FLOAT
            // branch would print a different pair: every ramp but the last
            // holds negatives or exceeds 1.2 (so the 0..1 magnet cannot
            // answer) and never ends on the type's ceiling (so vmin..vmax
            // cannot answer either); the last sits inside the magnet on
            // purpose, so a float reading would answer 0..1, which is still
            // not 0..32767. The i32 row is the one exception: it has no
            // branch and pins that i32 data outside [0,1] opens at its own
            // min..max; a type-fullscale branch would turn it red (a branch
            // that also answered vmin..vmax would not, and need not).
            // dtype is a mirror of src, hence the sync.
            auto mkTyped = [&](const char* nm, const char* dt, float lo, float hi) {
                auto d = mkRamp(nm, lo, hi, 0, 0.0f);
                d->src->dtype = dt;
                d->syncMirrors();
                return d;
            };
            struct Case { const char* dt; float lo, hi, eb, ew; const char* why; };
            const Case cases[] = {
                { "u8",      0.0f,  200.0f, 0.0f,   255.0f, "type fullscale" },
                { "i8",   -100.0f,  100.0f, 0.0f,   127.0f, "0..positive max: 255 is outside int8" },
                { "u16",     0.0f, 4000.0f, 0.0f, 65535.0f, "type fullscale" },
                { "i16",     0.0f, 1000.0f, 0.0f, 32767.0f, "0..positive max" },
                { "i16",  -1000.0f, 1000.0f, 0.0f, 32767.0f, "negatives present: black STAYS 0 (the ruling)" },
                { "i32",  -1000.0f, 1000.0f, -1000.0f, 1000.0f, "no i32 branch: the float rules, so vmin..vmax and the negatives show (not 0..2^31-1)" },
                { "i16",     0.3f,    0.6f, 0.0f, 32767.0f, "dtype decides, not the data (a float here reads 0..1)" },
            };
            for (const Case& c : cases) {
                auto d = mkTyped("def_int", c.dt, c.lo, c.hi);
                defaultRange(*d);
                bool ok = d->black == c.eb && d->white == c.ew;
                fprintf(stderr, "rangeselftest: P16 %-3s data %.6g..%.6g -> default %.6g..%.6g "
                                "(expect %.6g..%.6g; %s) %s\n",
                        c.dt, d->vmin, d->vmax, d->black, d->white, c.eb, c.ew, c.why,
                        ok ? "ok" : "WRONG");
                if (!ok) bad++;
            }
            // The float side of the asymmetry, with the SAME negative data as
            // the i16 row that holds negatives: it lands in vmin..vmax and is
            // visible at once. Pinned so the int16-vs-float difference reads
            // as intended (the i32 row answers the same way, by having no
            // branch).
            auto f = mkTyped("def_f32neg", "f32", -1000.0f, 1000.0f);
            defaultRange(*f);
            bool fneg = f->black == f->vmin && f->white == f->vmax && f->black < 0.0f;
            fprintf(stderr, "rangeselftest: P16 f32 data %.6g..%.6g -> default %.6g..%.6g "
                            "(expect vmin..vmax, negatives visible) %s\n",
                    f->vmin, f->vmax, f->black, f->white, fneg ? "ok" : "WRONG");
            if (!fneg) bad++;
            // ...and the 0..1 magnet is still the float rule near [0,1]: the
            // same data the last row of the table gave i16, answered as a float.
            auto g = mkTyped("def_f32unit", "f32", 0.3f, 0.6f);
            defaultRange(*g);
            bool magnet = g->black == 0.0f && g->white == 1.0f;
            fprintf(stderr, "rangeselftest: P16 f32 data %.6g..%.6g -> default %.6g..%.6g "
                            "(expect 0..1, the magnet) %s\n",
                    g->vmin, g->vmax, g->black, g->white, magnet ? "ok" : "WRONG");
            if (!magnet) bad++;
        }

        // ==================================================================
        // #321 S1 - every OLD range setting a session can carry comes back
        // drawing the SAME screen bytes. The redesign renamed the scope into a
        // rule and stopped "each frame fits itself" from writing stored
        // numbers; neither may change a single byte of what an existing
        // session shows. The table below was RECORDED on main 1801384f, before
        // any of #321, by this very block (FNV-1a 64 over renderDocRGBA of A
        // then B, in the screen's view) - so a red row is a byte the redesign
        // moved.
        // The fixture is what main leaves behind in use: under "Auto per frame"
        // a frame that has been on screen holds its own fit (selectImage wrote
        // it there), so A's and B's stored numbers are their fits for that
        // setting; under "Per stack" they hold the stack's one range. A frame
        // main never put on screen held its stack's inherited numbers under
        // Auto per frame - a history the new model no longer has (it draws
        // every frame at its fit), and that difference is the point of #321's
        // I3, not a regression this table should hide; it is not a row here.
        // ==================================================================
        {
            static const uint64_t S1_WANT[] = {
                // compare off: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0xfd51611bc598bfe4ull, 0xe285ef5068831792ull, 0x0a7b8660233b9ab4ull,
                0x512381965dfbf738ull, 0x512381965dfbf738ull, 0x512381965dfbf738ull,
                0x35e3bf6297cde2b2ull, 0x35e3bf6297cde2b2ull, 0x35e3bf6297cde2b2ull,
                0x35e3bf6297cde2b2ull, 0x35e3bf6297cde2b2ull, 0x35e3bf6297cde2b2ull,
                // cmprange 0: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x61fe3bb558d7c089ull, 0x2f6629b19b91e702ull, 0x6381e4bfb0f6e931ull,
                0xfdba2f998ee13630ull, 0xfdba2f998ee13630ull, 0xfdba2f998ee13630ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                // cmprange 1: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x5262214215abc756ull, 0xc38f47fc101a1792ull, 0x2de41701f5da9ab4ull,
                0x64d0238827aaf738ull, 0x64d0238827aaf738ull, 0x64d0238827aaf738ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                // cmprange 2: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x24863570b5253d5eull, 0x24863570b5253d5eull, 0x24863570b5253d5eull,
                0x24863570b5253d5eull, 0x24863570b5253d5eull, 0x24863570b5253d5eull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                // cmprange 3: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x61fe3bb558d7c089ull, 0x2f6629b19b91e702ull, 0x6381e4bfb0f6e931ull,
                0x61fe3bb558d7c089ull, 0x2f6629b19b91e702ull, 0x6381e4bfb0f6e931ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                // cmpshare 0: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x61fe3bb558d7c089ull, 0x2f6629b19b91e702ull, 0x6381e4bfb0f6e931ull,
                0xfdba2f998ee13630ull, 0xfdba2f998ee13630ull, 0xfdba2f998ee13630ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                // cmpshare 1: link 0 (scope 0 fl 0..2, scope 1 fl 0..2), link 1 (same)
                0x5262214215abc756ull, 0xc38f47fc101a1792ull, 0x2de41701f5da9ab4ull,
                0x64d0238827aaf738ull, 0x64d0238827aaf738ull, 0x64d0238827aaf738ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
                0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull, 0x24108d5b2be4e2b2ull,
            };
            const size_t nWant = sizeof S1_WANT / sizeof S1_WANT[0];
            const bool recording = nWant == 1 && S1_WANT[0] == 0;
            const bool saveLink = app.linkRange;
            const float saveLB = app.linkBlack, saveLW = app.linkWhite;
            const int saveScope = app.rangeRule, saveFlav = app.autoFlavor, saveCmpR = app.compareRangeMode;
            const int saveBM = app.medBlackMode;
            const float saveTgt = app.medTarget, saveBV = app.medBlackValue;
            const auto saveCmpMode = app.compareMode;
            const bool saveFollow = app.compareFollowFrame;
            const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
            const std::string saveBName = app.compareB;
            const int saveBSeq = app.compareBSeq;
            const std::vector<uint64_t> saveExtra = app.cmpExtra;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            std::vector<std::unique_ptr<ImageDoc>> held;
            std::swap(app.images, held);          // loadSession closes every open
            // the stacks are COPIED out (the table has no swap) and put back
            const std::vector<App::SeqInfo> heldSeqs(app.seqs.begin(), app.seqs.end());
            app.seqs.clear();
            std::error_code tec;
            const std::string sess = (std::filesystem::temp_directory_path(tec) /
                                      "viewer_rangeselftest_s1.vsession").u8string();
            static const char* CMP[6] = { "cmprange 0", "cmprange 1", "cmprange 2", "cmprange 3",
                                          "cmpshare 0", "cmpshare 1" };
            auto fnv = [](uint64_t h, const std::vector<uint8_t>& b) {
                for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
                return h;
            };
            size_t row = 0;
            int s1bad = 0;
            std::string record;
            // compare off (cmp -1: A alone) and on (the six compare lines)
            for (int cmp = -1; cmp < 6; cmp++)
                for (int link = 0; link < 2; link++)
                    for (int scope = 0; scope < 2; scope++)
                        for (int fl = AF_MinMax; fl <= AF_Median; fl++) {
                            {
                                std::ofstream sf(pathFromUtf8(sess), std::ios::binary | std::ios::trunc);
                                sf << "viewer-session 1\n";
                                sf << "linkrange " << link << " 300 800\n";
                                sf << "rangescope " << scope << "\n";
                                sf << "autoflavor " << fl << " 0.25 1 0\n";
                                if (cmp >= 0) sf << CMP[cmp] << "\n";
                            }
                            const std::string err = loadSession(sess);
                            // the fixture, AFTER the load (which closes every open)
                            auto A0 = mkRamp("s1_A0", 100.0f, 400.0f, 4, 2000.0f);
                            auto A1 = mkRamp("s1_A1", 150.0f, 900.0f, 0, 0.0f);
                            auto B0 = mkRamp("s1_B0", 1000.0f, 1600.0f, 4, 5000.0f);
                            A0->seqId = 4250; A0->seqIndex = 0;
                            A1->seqId = 4250; A1->seqIndex = 1;
                            B0->seqId = 4251; B0->seqIndex = 0;
                            if (scope == 0) {               // what main's step left there
                                float lo = 0, hi = 0;
                                fitByFlavor(*A0, lo, hi); A0->black = lo; A0->white = hi;
                                fitByFlavor(*B0, lo, hi); B0->black = lo; B0->white = hi;
                                A1->black = A0->black; A1->white = A0->white;
                            } else {
                                A0->black = 200.0f; A0->white = 700.0f;
                                A1->black = 200.0f; A1->white = 700.0f;
                                B0->black = 900.0f; B0->white = 1700.0f;
                            }
                            ImageDoc* pA = A0.get(); ImageDoc* pB = B0.get();
                            app.images.push_back(std::move(A0));
                            app.images.push_back(std::move(A1));
                            app.images.push_back(std::move(B0));
                            App::SeqInfo sa; sa.id = 4250; sa.name = "s1a"; app.seqs.push_back(sa);
                            App::SeqInfo sb; sb.id = 4251; sb.name = "s1b"; app.seqs.push_back(sb);
                            app.current = 0;
                            app.cmpExtra.clear();
                            app.compareFollowFrame = false;
                            if (cmp >= 0) { setCompareB(pB); app.compareMode = App::CmpSplit; }
                            else          { setCompareB(nullptr); app.compareMode = App::CmpOff; }
                            const ViewSides v = comparedSides();
                            uint64_t h = 1469598103934665603ull;
                            std::vector<uint8_t> rgba;
                            renderDocRGBA(*pA, rgba, &v); h = fnv(h, rgba);
                            if (cmp >= 0) { renderDocRGBA(*pB, rgba, &v); h = fnv(h, rgba); }
                            const bool have = !recording && row < nWant;
                            const bool same = have && S1_WANT[row] == h;
                            fprintf(stderr, "rangeselftest: S1 %-11s link %d rangescope %d flavor %d | "
                                            "err=\"%s\" sides %d | %016llx %s\n",
                                    cmp < 0 ? "compare-off" : CMP[cmp], link, scope, fl, err.c_str(), v.n,
                                    (unsigned long long)h,
                                    !have ? "RECORD" : same ? "ok" : "WRONG (a byte moved)");
                            char rec[32];
                            snprintf(rec, sizeof rec, "0x%016llxull,", (unsigned long long)h);
                            record += rec;
                            record += (row % 4 == 3) ? "\n" : " ";
                            if (!same || !err.empty() || v.n != (cmp >= 0 ? 2 : 1)) s1bad++;
                            row++;
                            app.images.clear();
                            app.seqs.clear();
                        }
            if (recording) fprintf(stderr, "rangeselftest: S1 RECORD\n%s\n", record.c_str());
            if (row != nWant && !recording) {
                fprintf(stderr, "rangeselftest: S1 %zu rows, table has %zu  WRONG\n", row, nWant);
                s1bad++;
            }
            if (s1bad) bad++;
            std::filesystem::remove(pathFromUtf8(sess), tec);
            std::swap(app.images, held);
            app.seqs.clear();
            for (const App::SeqInfo& si : heldSeqs) app.seqs.push_back(si);
            app.linkRange = saveLink; app.linkBlack = saveLB; app.linkWhite = saveLW;
            app.rangeRule = saveScope; app.autoFlavor = saveFlav; app.compareRangeMode = saveCmpR;
            app.medBlackMode = saveBM; app.medTarget = saveTgt; app.medBlackValue = saveBV;
            app.compareMode = saveCmpMode; app.compareFollowFrame = saveFollow;
            app.compareBUid = saveBUid; app.lastCompareBUid = saveLastB;
            app.compareB = saveBName; app.compareBSeq = saveBSeq;
            app.cmpExtra = saveExtra; app.current = saveCurrent;
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: S1 app.images %zu -> %zu, seqs %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
        }

        // ==================================================================
        // #321 - the Range scope: who shares one range. A frame of a stack can
        // keep a range of its own (the `frame` scope, an explicit exception);
        // every write goes to the scope in force (I1), and a fixed range under
        // a fitting rule is a declaration of one range (I2).
        // In-memory fixtures in a SWAPPED app.images (the P4 discipline).
        // ==================================================================
        {   // ---- P19: the frame exception ----------------------------------
            const bool saveLink = app.linkRange;
            const int saveRule = app.rangeRule, saveCmpR = app.compareRangeMode;
            const auto saveCmpMode = app.compareMode;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            auto F0 = mkRamp("fx_0", 0.3f, 0.6f, 0, 0.0f);
            auto F1 = mkRamp("fx_1", 2.0f, 3.0f, 0, 0.0f);
            auto F2 = mkRamp("fx_2", 5.0f, 9.0f, 0, 0.0f);
            F0->seqId = 4260; F0->seqIndex = 0;
            F1->seqId = 4260; F1->seqIndex = 1;
            F2->seqId = 4260; F2->seqIndex = 2;
            ImageDoc* f0 = F0.get(); ImageDoc* f1 = F1.get(); ImageDoc* f2 = F2.get();
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(F0));
            pool.push_back(std::move(F1));
            pool.push_back(std::move(F2));
            std::swap(app.images, pool);
            App::SeqInfo si; si.id = 4260; si.name = "fx";
            app.seqs.push_back(si);
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            app.compareMode = App::CmpOff;
            app.current = 1;
            auto ok = [&](bool c, const char* what) {
                fprintf(stderr, "rangeselftest: P19 %s %s\n", what, c ? "ok" : "WRONG");
                if (!c) bad++;
            };
            setRange(*f0, 1.0f, 2.0f);
            ok(f1->black == 1.0f && f2->white == 2.0f, "fixture: one range for the stack, 1..2");
            ok(rangeScopeOf(f1) == RS_Stack, "the default scope is the stack");
            setRangeScope(RS_Frame, f1);
            ok(f1->rangeOwn && f1->black == 1.0f && f1->white == 2.0f && rangeScopeOf(f1) == RS_Frame,
               "frame: frame 1 keeps the range it was drawn at, and the scope says frame");
            f0->texDirty = f2->texDirty = false;
            setRange(*f1, 0.0f, 255.0f);
            ok(f1->black == 0.0f && f1->white == 255.0f && effBlack(*f1) == 0.0f && effWhite(*f1) == 255.0f,
               "a write on the excepted frame lands on it, and it is drawn at it");
            ok(f0->black == 1.0f && f0->white == 2.0f && f2->black == 1.0f && f2->white == 2.0f &&
               !f0->texDirty && !f2->texDirty,
               "...and on that frame only: the siblings are not written or redrawn");
            // ...and it is a write on THAT frame, not a declaration about the
            // stack: under "each frame fits itself" the stack's rule stays
            app.rangeRule = RR_EachFrame;
            setRange(*f1, 10.0f, 20.0f);
            ok(app.rangeRule == RR_EachFrame && f1->black == 10.0f && f0->black == 1.0f,
               "a write on the excepted frame leaves the stack's rule alone (each frame fits itself stays)");
            app.rangeRule = RR_OneRange;
            setRange(*f1, 0.0f, 255.0f);
            setRange(*f0, 5.0f, 6.0f);
            ok(f2->black == 5.0f && f2->white == 6.0f && f1->black == 0.0f && f1->white == 255.0f,
               "a write on the stack skips the frame that keeps its own");
            ok(rangeScopeOf(f0) == RS_Stack, "...and a sibling's scope is still the stack");
            ok(ownFramesLine(ownFramesElsewhere(f0)) == "1 frame of this stack keeps its own range" &&
               ownFramesElsewhere(f1) == 0,
               "Inspector, on a sibling: \"1 frame of this stack keeps its own range\"");
            ok(rangeScopeName(f1, RS_Frame) == "frame 2 of fx \u2014 its own range" &&
               inForceOf(*f1).text.find("   this frame only") != std::string::npos,
               "Inspector, on the frame: \"frame 2 of fx - its own range\", in force \"this frame only\"");
            setRangeScope(RS_Stack, f1);
            ok(!f1->rangeOwn && f1->black == 5.0f && f1->white == 6.0f && rangeScopeOf(f1) == RS_Stack,
               "stack: frame 1 rejoins, at the stack's range");
            // a single image is its own stack: there is no frame exception to take
            f1->seqId = 0;
            setRangeScope(RS_Frame, f1);
            ok(!f1->rangeOwn && rangeScopeOf(f1) == RS_Stack, "a single image takes no frame exception");
            f1->seqId = 4260;
            for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                if (it->id == 4260) { app.seqs.erase(it); break; }
            std::swap(app.images, pool);
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: P19 app.images %zu -> %zu, seqs %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
            app.linkRange = saveLink; app.rangeRule = saveRule; app.compareRangeMode = saveCmpR;
            app.compareMode = saveCmpMode; app.current = saveCurrent;
        }

        {   // ---- P18 / P20 / P22: the default, the display-only fit, one set of words
            const bool saveLink = app.linkRange;
            const float saveLB = app.linkBlack, saveLW = app.linkWhite;
            const int saveRule = app.rangeRule, saveCmpR = app.compareRangeMode, saveFlav = app.autoFlavor;
            const auto saveCmpMode = app.compareMode;
            const bool saveFollow = app.compareFollowFrame;
            const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
            const std::string saveBName = app.compareB;
            const int saveBSeq = app.compareBSeq;
            const std::vector<uint64_t> saveExtra = app.cmpExtra;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            // a 3-frame stack (4270) with hot pixels so the three fits differ,
            // and a B (4271) whose content lies INSIDE A's: the union of the
            // two IS A's min-max, and must still be called a union
            auto G0 = mkRamp("dw_0", 100.0f, 400.0f, 4, 2000.0f);
            auto G1 = mkRamp("dw_1", 150.0f, 900.0f, 4, 3000.0f);
            auto G2 = mkRamp("dw_2", 300.0f, 500.0f, 4, 2500.0f);
            auto BB = mkRamp("dw_B", 120.0f, 380.0f, 0, 0.0f);
            G0->seqId = 4270; G0->seqIndex = 0;
            G1->seqId = 4270; G1->seqIndex = 1;
            G2->seqId = 4270; G2->seqIndex = 2;
            BB->seqId = 4271; BB->seqIndex = 0;
            ImageDoc* g[3] = { G0.get(), G1.get(), G2.get() };
            ImageDoc* pB = BB.get();
            for (ImageDoc* d : g) { d->black = 0.0f; d->white = 4095.0f; }
            pB->black = 0.0f; pB->white = 4095.0f;
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(G0));
            pool.push_back(std::move(G1));
            pool.push_back(std::move(G2));
            pool.push_back(std::move(BB));
            std::swap(app.images, pool);
            App::SeqInfo s0; s0.id = 4270; s0.name = "dw"; app.seqs.push_back(s0);
            App::SeqInfo s1; s1.id = 4271; s1.name = "dwB"; app.seqs.push_back(s1);
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            app.compareRangeMode = 2;
            app.compareMode = App::CmpOff;
            app.compareFollowFrame = false;
            app.cmpExtra.clear();
            app.autoFlavor = AF_MinMax;
            app.current = 1;                                   // frame 2 on screen
            auto ok = [&](const char* tag, bool c, const char* what) {
                fprintf(stderr, "rangeselftest: %s %s %s\n", tag, what, c ? "ok" : "WRONG");
                if (!c) bad++;
            };
            // ---- P18: the default is the stack, one range ----
            setRange(*g[1], g[1]->vmin, g[1]->vmax);            // "Auto" on frame 2
            const App::SeqInfo* si = seqInfo(4270);
            bool all = true, lamps = true;
            for (ImageDoc* d : g) {
                all = all && d->black == g[1]->vmin && d->white == g[1]->vmax;
                lamps = lamps && rangeButtonMatches(*d, RB_Auto, effBlack(*d), effWhite(*d));
            }
            const std::string w18 = rangeWords(*g[0], ViewSides()).said;
            fprintf(stderr, "rangeselftest: P18 words on frame 1: \"%s\"\n", w18.c_str());
            ok("P18", rangeScopeOf(g[0]) == RS_Stack && app.rangeRule == RR_OneRange,
               "the default is scope stack, rule one range");
            ok("P18", all, "Auto on frame 2 spreads frame 2's min-max over the stack");
            ok("P18", si && si->rangeRefUid == g[1]->uid, "...and the stack remembers frame 2 as whose fit it wears");
            ok("P18", w18 == "one range for the stack · fit of frame 2", "...and says so: \"fit of frame 2\"");
            ok("P18", lamps, "Auto is lit on every frame of the stack");
            // ---- P20: each frame fits itself is display only ----
            float st[3][2];
            for (int k = 0; k < 3; k++) { st[k][0] = g[k]->black; st[k][1] = g[k]->white; }
            app.autoFlavor = AF_Pct;
            setRangeRule(RR_EachFrame, g[1]);
            bool drawn = true, kept = true;
            for (int k = 0; k < 3; k++) {
                float lo = 0, hi = 0;
                percentileRangeCached(*g[k], lo, hi);
                drawn = drawn && effBlack(*g[k]) == lo && effWhite(*g[k]) == hi && lo > g[k]->vmin;
                kept = kept && g[k]->black == st[k][0] && g[k]->white == st[k][1];
            }
            selectImage(2);                                    // a step writes nothing either
            selectImage(0);
            for (int k = 0; k < 3; k++) kept = kept && g[k]->black == st[k][0] && g[k]->white == st[k][1];
            ok("P20", drawn, "each frame fits itself (0.1-99.9%): every frame drawn at its own cut");
            ok("P20", kept, "...and no frame's stored range is written, stepping included");
            ok("P20", rangeWords(*g[0], ViewSides()).said == "each frame fits itself (0.1-99.9%)",
               "...and the words say each frame fits itself (0.1-99.9%)");
            // a frame that keeps its own is not fitted: it is the exception
            g[2]->rangeOwn = true;
            ok("P20", effBlack(*g[2]) == st[2][0] && effWhite(*g[2]) == st[2][1] &&
                      rangeWords(*g[2], ViewSides()).said == "this frame only",
               "a frame that keeps its own range wears it under each frame fits itself");
            g[2]->rangeOwn = false;
            // back to one range: the frame on screen's fit becomes the stack's
            float flo = 0, fhi = 0;
            percentileRangeCached(*g[0], flo, fhi);
            setRangeRule(RR_OneRange, g[0]);
            bool adopted = true;
            for (ImageDoc* d : g) adopted = adopted && d->black == flo && d->white == fhi;
            ok("P20", adopted, "one range again: the stack takes the fit of the frame on screen");
            // ---- P22: one set of words, everywhere ----
            app.current = 0;
            setCompareB(pB);
            app.compareMode = App::CmpSplit;
            bool same = true;
            for (int link = 0; link < 2; link++)
                for (int rule = 0; rule < 2; rule++)
                    for (int m = 0; m < 4; m++) {
                        app.linkRange = link != 0;
                        app.linkBlack = 10.0f; app.linkWhite = 20.0f;
                        app.rangeRule = rule;
                        app.compareRangeMode = m;
                        const ViewSides v = comparedSides();
                        const RangeWords rw = rangeWords(*g[0], v);
                        const std::string head = rangeWordsHead(rw);
                        recomputeHistogramIfNeeded(cur(), app.hist[0]);
                        const std::string lab = abHistXLabel(cur(), cmpB(), app.hist[0]);
                        const std::string note = exportRangeNote(*g[0], v, false);
                        const std::string inforce = inForceOf(*g[0]).text;
                        const bool c = !head.empty() && lab.find(head) != std::string::npos &&
                                       note.rfind("range: " + rw.said, 0) == 0 &&
                                       inforce.find("   " + rw.said) != std::string::npos;
                        if (!c) fprintf(stderr, "rangeselftest: P22 link %d rule %d mode %d | head \"%s\" | "
                                                "axis \"%s\" | note \"%s\" | in force \"%s\"  WRONG\n",
                                        link, rule, m, head.c_str(), lab.c_str(), note.c_str(), inforce.c_str());
                        same = same && c;
                    }
            ok("P22", same, "rangeWords: the in-force line, the axis (its head) and the export note, every scope x rule x mode");
            app.linkRange = false; app.rangeRule = RR_OneRange; app.compareRangeMode = 2;
            const float u0 = effBlack(*g[0]), u1 = effWhite(*g[0]);
            ok("P22", u0 == g[0]->vmin && u1 == g[0]->vmax &&
                      rangeWords(*g[0], comparedSides()).said == "union over A, B (min-max)",
               "a union that equals A's own min-max is still called a union");
            for (int i = 0; i < 2; i++) {
                for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                    if (it->id == 4270 || it->id == 4271) { app.seqs.erase(it); break; }
            }
            std::swap(app.images, pool);
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: P18 app.images %zu -> %zu, seqs %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
            app.linkRange = saveLink; app.linkBlack = saveLB; app.linkWhite = saveLW;
            app.rangeRule = saveRule; app.compareRangeMode = saveCmpR; app.autoFlavor = saveFlav;
            app.compareMode = saveCmpMode; app.compareFollowFrame = saveFollow;
            app.compareBUid = saveBUid; app.lastCompareBUid = saveLastB;
            app.compareB = saveBName; app.compareBSeq = saveBSeq;
            app.cmpExtra = saveExtra; app.current = saveCurrent;
        }

        {   // ---- P21 / P23 / P24: the compared sides, writes, and the lamps --
            const bool saveLink = app.linkRange;
            const float saveLB = app.linkBlack, saveLW = app.linkWhite;
            const int saveRule = app.rangeRule, saveCmpR = app.compareRangeMode, saveFlav = app.autoFlavor;
            const int saveRuleLast = app.compareRuleLast;
            const auto saveCmpMode = app.compareMode;
            const bool saveFollow = app.compareFollowFrame;
            const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
            const std::string saveBName = app.compareB;
            const int saveBSeq = app.compareBSeq;
            const std::vector<uint64_t> saveExtra = app.cmpExtra;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            auto A0 = mkRamp("cs_A0", 100.0f, 400.0f, 4, 2000.0f);
            auto A1 = mkRamp("cs_A1", 150.0f, 900.0f, 4, 3000.0f);
            auto B0 = mkRamp("cs_B0", 1000.0f, 1600.0f, 4, 5000.0f);
            auto B1 = mkRamp("cs_B1", 1100.0f, 1700.0f, 0, 0.0f);
            A0->seqId = 4280; A0->seqIndex = 0;
            A1->seqId = 4280; A1->seqIndex = 1;
            B0->seqId = 4281; B0->seqIndex = 0;
            B1->seqId = 4281; B1->seqIndex = 1;
            ImageDoc* a0 = A0.get(); ImageDoc* a1 = A1.get(); ImageDoc* b0 = B0.get(); ImageDoc* b1 = B1.get();
            a0->black = a1->black = 50.0f;  a0->white = a1->white = 950.0f;
            b0->black = b1->black = 900.0f; b0->white = b1->white = 1800.0f;
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A0));
            pool.push_back(std::move(A1));
            pool.push_back(std::move(B0));
            pool.push_back(std::move(B1));
            std::swap(app.images, pool);
            App::SeqInfo s0; s0.id = 4280; s0.name = "csA"; app.seqs.push_back(s0);
            App::SeqInfo s1; s1.id = 4281; s1.name = "csB"; app.seqs.push_back(s1);
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            app.autoFlavor = AF_MinMax;
            app.compareRuleLast = 2;
            app.compareFollowFrame = false;
            app.cmpExtra.clear();
            app.current = 0;
            app.compareMode = App::CmpOff;
            auto ok = [&](const char* tag, bool c, const char* what) {
                fprintf(stderr, "rangeselftest: %s %s %s\n", tag, what, c ? "ok" : "WRONG");
                if (!c) bad++;
            };
            auto segs = [&]() {
                std::string s;
                for (const RangeSeg& g : rangeScopeSegs(cur())) s += g.label + ",";
                return s;
            };
            // ---- P23: the A+B segment exists only while comparing ----
            ok("P23", segs() == "frame,stack,all open,", "compare off: frame, stack, all open - no A+B");
            setCompareB(b0);
            app.compareMode = App::CmpSplit;
            app.compareRangeMode = 2;
            ok("P23", segs() == "frame,stack,A+B,all open,", "compare on: the A+B segment appears");
            ok("P23", rangeScopeOf(a0) == RS_Compared, "...and the union lights it");
            setRangeScope(RS_Stack, a0);
            ok("P23", app.compareRangeMode == 0 && app.compareRuleLast == 2 && rangeScopeOf(a0) == RS_Stack,
               "stack: compare goes to each keeps its own, the union is remembered");
            ok("P23", effBlack(*a0) == 50.0f && effWhite(*b0) == 1800.0f,
               "...and every side shows its own stack's range");
            setRangeScope(RS_Compared, a0);
            ok("P23", app.compareRangeMode == 2, "A+B: back to the rule last chosen (union)");
            {   // a single image offers no frame segment
                const int sq = a0->seqId; a0->seqId = 0;
                ok("P23", segs().rfind("stack,", 0) == 0, "a single image: no frame segment");
                a0->seqId = sq;
            }
            // ---- P24: the lamp table ----
            float eb = effBlack(*a0), ew = effWhite(*a0);
            ok("P24", rangeButtonLit(*a0, RB_Auto, eb, ew) && !rangeButtonLit(*a0, RB_AutoPct, eb, ew) &&
                      !rangeButtonEnabled(*a0, RB_AutoPct) && !rangeButtonEnabled(*a0, RB_Median) &&
                      rangeButtonEnabled(*a0, RB_Auto) && !rangeButtonLit(*a0, RB_0_65535, eb, ew),
               "union: Auto lit as a fact, Auto % / Med not offered, presets dark");
            setCompareRule(3);
            app.autoFlavor = AF_Pct;
            eb = effBlack(*a0); ew = effWhite(*a0);
            ok("P24", rangeButtonLit(*a0, RB_AutoPct, eb, ew) && !rangeButtonLit(*a0, RB_Auto, eb, ew) &&
                      !rangeButtonLit(*a0, RB_Median, eb, ew) && rangeButtonEnabled(*a0, RB_Median),
               "each side fits itself: the chosen fit is lit, the other two are choices");
            setCompareRule(0);
            app.rangeRule = RR_EachFrame;
            app.autoFlavor = AF_Median;
            eb = effBlack(*a0); ew = effWhite(*a0);
            ok("P24", rangeButtonLit(*a0, RB_Median, eb, ew) && !rangeButtonLit(*a0, RB_Auto, eb, ew),
               "each frame fits itself: Med lit as the chosen fit");
            a0->black = 0.0f; a0->white = 255.0f;   // a stored preset is not what is drawn
            ok("P24", !rangeButtonLit(*a0, RB_0_255, effBlack(*a0), effWhite(*a0)),
               "...and the fixed presets dark");
            app.rangeRule = RR_OneRange;
            ok("P24", rangeButtonLit(*a0, RB_0_255, effBlack(*a0), effWhite(*a0)),
               "one range: the presets by comparison again");
            a0->rangeOwn = true;
            a0->black = a0->vmin; a0->white = a0->vmax;
            ok("P24", rangeButtonLit(*a0, RB_Auto, effBlack(*a0), effWhite(*a0)),
               "frame exception: Auto compared against the frame itself");
            a0->rangeOwn = false;
            a0->black = 50.0f; a0->white = 950.0f;
            // ---- P21: a written range is a declaration of one range ----
            app.rangeRule = RR_EachFrame;
            setRange(*a1, 10.0f, 20.0f);
            ok("P21", app.rangeRule == RR_OneRange && a0->black == 10.0f && a0->white == 20.0f &&
                      b0->black == 900.0f,
               "each frame fits itself + a typed range: rule -> one range, the stack takes it, B does not");
            setCompareRule(2);
            setRange(*a0, 0.0f, 255.0f);
            ok("P21", app.compareRangeMode == 1 && a0->black == 0.0f && a1->white == 255.0f &&
                      b0->black == 900.0f && b0->white == 1800.0f,
               "union + 0-255: rule -> A's range, A's stack written, B's numbers untouched");
            ok("P21", effBlack(*b0) == 0.0f && effWhite(*b0) == 255.0f, "...and B is drawn at A's range");
            setCompareRule(3);
            setRange(*b0, 5.0f, 6.0f);
            ok("P21", app.compareRangeMode == 1 && a0->black == 5.0f && b0->black == 900.0f,
               "each side fits itself + a write on B: rule -> A's range, the write goes to A's stack");
            for (int i = 0; i < 2; i++)
                for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                    if (it->id == 4280 || it->id == 4281) { app.seqs.erase(it); break; }
            std::swap(app.images, pool);
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: P23 app.images %zu -> %zu, seqs %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
            app.linkRange = saveLink; app.linkBlack = saveLB; app.linkWhite = saveLW;
            app.rangeRule = saveRule; app.compareRangeMode = saveCmpR; app.autoFlavor = saveFlav;
            app.compareRuleLast = saveRuleLast;
            app.compareMode = saveCmpMode; app.compareFollowFrame = saveFollow;
            app.compareBUid = saveBUid; app.lastCompareBUid = saveLastB;
            app.compareB = saveBName; app.compareBSeq = saveBSeq;
            app.cmpExtra = saveExtra; app.current = saveCurrent;
        }

        {   // ---- P25 / P26 / P27: why the union could look switched off (#321 §6)
            const int saveCmpR = app.compareRangeMode, saveRule = app.rangeRule;
            const bool saveLink = app.linkRange;
            const auto saveCmpMode = app.compareMode;
            const bool saveFollow = app.compareFollowFrame;
            const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
            const std::string saveBName = app.compareB;
            const int saveBSeq = app.compareBSeq;
            const std::vector<uint64_t> saveExtra = app.cmpExtra;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size();
            auto A = mkRamp("same.npy", 0.3f, 0.6f, 0, 0.0f);
            auto B = mkRamp("same.npy", 0.35f, 0.5f, 0, 0.0f);
            auto C = mkRamp("same.npy", 0.1f, 0.9f, 0, 0.0f);
            A->seqId = 4290; B->seqId = 4291; C->seqId = 4292;
            A->black = 0.3f; A->white = 0.6f;
            ImageDoc* pA = A.get(); ImageDoc* pB = B.get(); ImageDoc* pC = C.get();
            std::vector<std::unique_ptr<ImageDoc>> pool;
            pool.push_back(std::move(A));
            pool.push_back(std::move(B));
            pool.push_back(std::move(C));
            std::swap(app.images, pool);
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            app.compareRangeMode = 2;
            app.compareFollowFrame = false;
            app.cmpExtra.clear();
            app.current = 0;
            setCompareB(pB);
            app.compareMode = App::CmpSplit;
            auto ok = [&](const char* tag, bool c, const char* what) {
                fprintf(stderr, "rangeselftest: %s %s %s\n", tag, what, c ? "ok" : "WRONG");
                if (!c) bad++;
            };
            // ---- P25: a side nobody measured does not enter the union ----
            ok("P25", effBlack(*pA) == 0.3f && effWhite(*pA) == 0.6f, "fixture: B inside A, the union is 0.3..0.6");
            pB->src->vmin = 0.0f; pB->src->vmax = 1.0f; pB->src->measured = false;   // the default
            pB->syncMirrors();
            const RangeWords w25 = rangeWords(*pA, comparedSides());
            fprintf(stderr, "rangeselftest: P25 words \"%s\" amber %d | A drawn %.6g..%.6g\n",
                    w25.said.c_str(), w25.amber ? 1 : 0, effBlack(*pA), effWhite(*pA));
            ok("P25", effBlack(*pA) == 0.3f && effWhite(*pA) == 0.6f &&
                      effBlack(*pB) == 0.3f && effWhite(*pB) == 0.6f,
               "an unmeasured B: every side at A's range, not a union over B's 0..1 default");
            ok("P25", w25.amber && w25.said == "union over A, B — B not measured, showing A's range",
               "...and the words say \"B not measured, showing A's range\", in amber");
            computeMinMax(*pB);
            ok("P25", pB->src->measured && rangeWords(*pA, comparedSides()).said == "union over A, B (min-max)",
               "measured: the union again");
            // ---- P26: new pixels on B move A to the new union ----
            pA->texBlack = effBlack(*pA); pA->texWhite = effWhite(*pA);   // A's texture, built
            for (float& v : pB->src->data) v += 1.0f;                      // B reloaded: 1.35..1.5
            float mn = 0, mx = 0;
            minMaxOf(pB->src->data, mn, mx);
            pB->src->vmin = mn; pB->src->vmax = mx; pB->src->measured = true;
            pB->syncMirrors();                                             // the reload walk
            ok("P26", effBlack(*pA) == 0.3f && effWhite(*pA) == mx && pA->texWhite != effWhite(*pA),
               "B's source re-measured: A follows the new union, and A's texture no longer matches it");
            // ---- P27: a B that is gone does not come back as another of its name ----
            uint64_t bu = pB->uid;
            const uint64_t nu = app.nextUid++;
            carryCompareSeats(bu, nu);
            ok("P27", app.compareBUid == nu && app.lastCompareBUid == nu, "a doc replaced in place keeps the B seat");
            pB->uid = nu;
            std::unique_ptr<ImageDoc> gone = std::move(app.images[1]);     // B closed
            app.images.erase(app.images.begin() + 1);
            ImageDoc* r1 = resolveB();
            ImageDoc* r2 = resolveB();
            ok("P27", r1 == nullptr && r2 == nullptr && app.compareB.empty(),
               "B closed: no B - not the next doc that happens to share its name");
            (void)pC;
            std::swap(app.images, pool);
            if (app.images.size() != imgWas) {
                fprintf(stderr, "rangeselftest: P25 app.images %zu -> %zu  NOT RESTORED\n", imgWas, app.images.size());
                bad++;
            }
            app.compareRangeMode = saveCmpR; app.rangeRule = saveRule; app.linkRange = saveLink;
            app.compareMode = saveCmpMode; app.compareFollowFrame = saveFollow;
            app.compareBUid = saveBUid; app.lastCompareBUid = saveLastB;
            app.compareB = saveBName; app.compareBSeq = saveBSeq;
            app.cmpExtra = saveExtra; app.current = saveCurrent;
        }

        {   // ---- S1b: the frame exception travels in the session -------------
            // `framerange <seqIndex> <black> <white>`, one line per frame that
            // keeps its own, after its stack's image line. A frame-axis .npy is
            // a stack that is whole the moment it is opened, so the line is
            // answered while the file is read - refusal included, into the
            // session's report.
            const bool saveLink = app.linkRange;
            const int saveRule = app.rangeRule, saveCmpR = app.compareRangeMode;
            const auto saveCmpMode = app.compareMode;
            const int saveCurrent = app.current;
            const size_t imgWas = app.images.size(), seqWas = app.seqs.size();
            std::vector<std::unique_ptr<ImageDoc>> held;
            std::swap(app.images, held);
            const std::vector<App::SeqInfo> heldSeqs(app.seqs.begin(), app.seqs.end());
            app.seqs.clear();
            std::error_code tec;
            const std::filesystem::path tmp = std::filesystem::temp_directory_path(tec);
            const std::string npy = (tmp / "viewer_rangeselftest_fr.npy").u8string();
            const std::string sess = (tmp / "viewer_rangeselftest_fr.vsession").u8string();
            {   // (3, 8, 8) f32, frame f a ramp from 100 + 10 f
                const int F = 3, W = 8, H = 8;
                char dict[128];
                snprintf(dict, sizeof dict,
                         "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d, %d), }", F, H, W);
                std::string hdr = dict;
                hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
                hdr += '\n';
                std::ofstream f(pathFromUtf8(npy), std::ios::binary | std::ios::trunc);
                const uint16_t hl = (uint16_t)hdr.size();
                f.write("\x93NUMPY\x01\x00", 8);
                f.write((const char*)&hl, 2);
                f.write(hdr.data(), (std::streamsize)hdr.size());
                for (int fr = 0; fr < F; fr++) {
                    std::vector<float> px((size_t)W * H);
                    for (size_t k = 0; k < px.size(); k++) px[k] = 100.0f + 10.0f * fr + (float)k;
                    f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
                }
            }
            auto ok = [&](bool c, const char* what) {
                fprintf(stderr, "rangeselftest: S1b %s %s\n", what, c ? "ok" : "WRONG");
                if (!c) bad++;
            };
            app.linkRange = false;
            app.rangeRule = RR_OneRange;
            app.compareMode = App::CmpOff;
            const std::string lerr = loadNpy(npy);
            std::vector<int> fr = cur() && cur()->seqId > 0 ? framesOfSeq(cur()->seqId) : std::vector<int>();
            ok(lerr.empty() && fr.size() == 3, "fixture: a three-frame stack from one file");
            if (fr.size() == 3) {
                setRange(*app.images[fr[0]], 100.0f, 130.0f);
                selectImage(fr[1]);
                setRangeScope(RS_Frame, cur());
                setRange(*cur(), 1.0f, 2.0f);
                saveSession(sess, true);
                std::vector<uint8_t> raw;
                readFileBytes(sess, raw);
                const std::string text(raw.begin(), raw.end());
                ok(text.find("\nframerange 1 1 2\n") != std::string::npos, "the session writes \"framerange 1 1 2\"");
                SessionReport r1;
                const std::string e1 = loadSession(sess, &r1);
                std::vector<int> g = cur() && cur()->seqId > 0 ? framesOfSeq(cur()->seqId) : std::vector<int>();
                bool back = e1.empty() && g.size() == 3 && r1.count[LK_LineRejected] == 0;
                for (size_t k = 0; back && k < g.size(); k++) {
                    const ImageDoc* d = app.images[g[k]].get();
                    back = k == 1 ? (d->rangeOwn && d->black == 1.0f && d->white == 2.0f)
                                  : (!d->rangeOwn && d->black == 100.0f && d->white == 130.0f);
                }
                ok(back, "...and reads it back: frame 1 keeps 1..2, frames 0 and 2 share 100..130");
                // a frame the stack does not have is refused, and said
                std::string bent = text;
                const size_t at = bent.find("\nframerange 1 1 2\n");
                if (at != std::string::npos) bent.insert(at + 1, "framerange 7 3 4\n");
                {
                    std::ofstream bf(pathFromUtf8(sess), std::ios::binary | std::ios::trunc);
                    bf << bent;
                }
                SessionReport r2;
                loadSession(sess, &r2);
                bool said = r2.count[LK_LineRejected] == 1 && !r2.names[LK_LineRejected].empty() &&
                            r2.names[LK_LineRejected][0] == "framerange: frame 7 not in stack";
                fprintf(stderr, "rangeselftest: S1b refused lines %d (%s)\n", r2.count[LK_LineRejected],
                        r2.names[LK_LineRejected].empty() ? "" : r2.names[LK_LineRejected][0].c_str());
                ok(said, "a framerange for frame 7 of a 3-frame stack: one LK_LineRejected, named");
                // the old "everything" scope (rangescope 2) is the all-open overlay
                {
                    std::ofstream gf(pathFromUtf8(sess), std::ios::binary | std::ios::trunc);
                    gf << "viewer-session 1\nlinkrange 0 0 1\nrangescope 2\n";
                }
                loadSession(sess);
                ok(app.linkRange && app.rangeRule == RR_OneRange, "rangescope 2 reads as all open");
                {
                    std::ofstream gf(pathFromUtf8(sess), std::ios::binary | std::ios::trunc);
                    gf << "viewer-session 1\nlinkrange 0 0 1\nrangescope 7\n";
                }
                app.rangeRule = RR_EachFrame;
                loadSession(sess);
                ok(!app.linkRange && app.rangeRule == RR_EachFrame, "rangescope 7 is not a rule: the one in force stays");
            }
            closeAll();
            std::filesystem::remove(pathFromUtf8(sess), tec);
            std::filesystem::remove(pathFromUtf8(npy), tec);
            std::swap(app.images, held);
            app.seqs.clear();
            for (const App::SeqInfo& s : heldSeqs) app.seqs.push_back(s);
            if (app.images.size() != imgWas || app.seqs.size() != seqWas) {
                fprintf(stderr, "rangeselftest: S1b app.images %zu -> %zu, seqs %zu -> %zu  NOT RESTORED\n",
                        imgWas, app.images.size(), seqWas, app.seqs.size());
                bad++;
            }
            app.linkRange = saveLink; app.rangeRule = saveRule; app.compareRangeMode = saveCmpR;
            app.compareMode = saveCmpMode; app.current = saveCurrent;
        }

        // the shared home gets the modes it was lent, back
        app.rangeRule = 1;
        app.linkRange = false;
        app.autoFlavor = AF_MinMax;
        app.medTarget = 0.5f;
        app.medBlackMode = MB_Value;
        app.medBlackValue = 0;

        fprintf(stderr, "rangeselftest: %s\n", bad ? "FAILED" : "ok");
        if (app.seqThread.joinable()) app.seqThread.join();
        return bad ? 1 : 0;
    }
