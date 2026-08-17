// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --histhl-selftest: the value highlight (#68) ---------------------------
// Drag a value interval on the histogram's x axis; the pixels in it are painted
// on the image and the count is declared. docs/histogram-select-design.md is
// the specification and §9 is this file's running order.
//
// Windowless, on the abEqSelftest pattern and for the same reason: every claim
// here is a pixel value, a count or a string, and all of it is CPU on both
// sides of the window branch - so it runs on every runner in the matrix rather
// than on the one machine with a GL context. Fixtures are analytic and built in
// memory, so this test opens no file and cannot be made to pass by a fixture
// drifting; the expected numbers are derived by hand, and the derivation sits
// beside the assert.
//
// THE HIGHLIGHT IS CLEARED AT BOTH ENDS OF THIS FILE, and again between groups.
// One selftest home is shared by every test in the suite, and an armed
// highlight paints EVERY later render test magenta. T8 is the guard in the
// other direction and therefore runs first.
static int histHlSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "histhlselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto has = [](const std::string& s, const char* sub) {
        return s.find(sub) != std::string::npos;
    };

    // A document with nothing in it but the pixels the test puts there.
    auto mk = [](const char* nm, int W, int H, int chn, const char* dtype) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = H; d->src->ch = chn;
        d->src->dtype = dtype;
        d->src->data.assign((size_t)W * H * chn, 0.0f);
        d->syncMirrors();
        d->uid = app.nextUid++;
        return d;
    };
    // The display range is SET, never defaulted: every expected bin edge below
    // is derived from these two numbers by hand.
    auto setRange = [](ImageDoc& d, float b, float w) {
        computeMinMax(d);
        d.black = b; d.white = w;
        d.texDirty = true;
    };
    // The bin grid the panel would put on the axis for this document, and a
    // plot rectangle spanning exactly it - which is what the panel does
    // (xLo = binOrigin, xHi = binOrigin + 256*binW). Deliberately very wide in
    // screen pixels so that a drag of a fraction of a bin is still comfortably
    // past the 4 px "this was an intention" threshold.
    struct Grid { float origin = 0, w = 0; PlotRect pr; };
    auto gridOf = [](const ImageDoc& d) {
        Grid g;
        histBinGrid(d.dtype, effBlack(d), effWhite(d), g.origin, g.w);
        g.pr.p0 = ImVec2(100, 50);
        g.pr.p1 = ImVec2(100 + 256 * 16, 250);          // 16 screen px per bin
        g.pr.xmin = g.origin;
        g.pr.xmax = (float)((double)g.origin + 256.0 * (double)g.w);
        g.pr.ymin = 0; g.pr.ymax = 1;
        g.pr.ok = true;
        return g;
    };
    auto rgbaAt = [](const std::vector<uint8_t>& px, const ImageDoc& d, int x, int y) {
        const size_t o = ((size_t)y * d.w + x) * 4;
        return std::array<int, 4>{ px[o], px[o + 1], px[o + 2], px[o + 3] };
    };
    const std::array<int, 4> MAGENTA{ 255, 0, 255, 255 };

    // The Statistics panel, driven for real - abEqSelftest's frame, unchanged.
    ImGuiIO& io = ImGui::GetIO();
    auto histFrame = [&]() {
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
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_Always);
            ImGui::Begin("HistHlProbe", nullptr,
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
            abStatsFrame();
            drawPanelHistogram();
            ImGui::End();
            ImGui::EndFrame();
        }
    };
    auto probeField = [](const char* key) {
        const std::string k = std::string(key) + "=";
        size_t at = g_histHlProbe.find(k);
        if (at == std::string::npos) return std::string();
        at += k.size();
        size_t e = g_histHlProbe.find(';', at);
        return g_histHlProbe.substr(at, e == std::string::npos ? e : e - at);
    };

    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    app.cmpExtra.clear();
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = false;
    app.abStepBusyUntil = 0;
    app.linkRange = false;
    app.dispGamma = 1.0f;
    app.histPlane = -1;
    histHlClear();

    // ======================================================================
    // T8 - OFF IS UNCHANGED. First, and green from the start rather than
    // red-then-green: it is the regression guard the rest of the suite leans
    // on. A highlight that leaked into renderDocRGBA when nothing is armed
    // would repaint every export test in the tree, so the claim is made on the
    // BYTES: arm, clear, render == a render that was never armed at all.
    // ======================================================================
    {
        auto d = mk("hl_t8", 16, 16, 1, "u8");
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) d->px()[(size_t)y * 16 + x] = (float)(x + 16 * y);
        setRange(*d, 0, 255);
        std::vector<uint8_t> before, armed, after;
        renderDocRGBA(*d, before);
        histHlArm(100, 200, false, false, -1, nullptr);
        renderDocRGBA(*d, armed);
        histHlClear();
        renderDocRGBA(*d, after);
        check(before == after, "T8 off: the bytes are the ones that were always produced");
        // anti-vacuity: the same fixture DOES change while it is armed, so the
        // equality above is not the equality of two no-ops
        check(armed != before, "T8 ...and arming really does change them (anti-vacuity)");
        check(app.hlCount.empty(), "T8 ...and clearing drops the counts with it");
    }

    // ======================================================================
    // STAGE 1 - the mapping: a drag becomes an interval (§2). Pure, no mouse.
    // Fixture: u16, black 100, white 1123.
    //   span = 1023, q = 1023/256 = 3.996 -> integer width ceil(q) = 4
    //   origin = floor(100/4)*4 = 100, so the grid is 100..1124 in 4 DN bins.
    // ======================================================================
    {
        auto d = mk("hl_t1", 8, 8, 1, "u16");
        for (size_t i = 0; i < d->px().size(); i++) d->px()[i] = 100.0f + (float)i;
        setRange(*d, 100, 1123);
        const Grid g = gridOf(*d);
        check(g.w == 4.0f && g.origin == 100.0f,
              "T1 the grid under the drag is 4 DN bins from 100 (hand-derived)");

        // T1 - a drag from data x 110.5 to 121.0 touches bins
        //   floor((110.5-100)/4) = floor(2.625) = 2
        //   floor((121.0-100)/4) = floor(5.25)  = 5
        // so the interval snaps OUTWARD to [100+2*4, 100+6*4) = [108, 124),
        // and neither end is an end bin, so neither side opens.
        HistHlRange r = histHlFromDrag(g.pr, g.pr.at(110.5f, 0), g.pr.at(121.0f, 0),
                                       g.origin, g.w);
        check(r.ok && r.b0 == 2 && r.b1 == 5, "T1 the drag touches bins 2..5");
        check(r.lo == 108.0f && r.hi == 124.0f,
              "T1 ...and snaps outward to the half-open [108, 124)");
        check(!r.loOpen && !r.hiOpen, "T1 ...with neither side open");
        // ...and it is the same interval whichever corner the drag started from
        HistHlRange rb = histHlFromDrag(g.pr, g.pr.at(121.0f, 0), g.pr.at(110.5f, 0),
                                        g.origin, g.w);
        check(rb.ok && rb.lo == r.lo && rb.hi == r.hi,
              "T1 ...whichever direction it was dragged");

        // T2 - the END bin. 100.0 -> 107.9 touches bins 0 and 1. Bin 0 holds
        // every value the grid does not reach (recomputeHistogramIfNeeded folds
        // t<0 into it), so the bar means "and everything below" and the
        // selection has to mean the same: (-inf, 108).
        HistHlRange e = histHlFromDrag(g.pr, g.pr.at(100.0f, 0), g.pr.at(107.9f, 0),
                                       g.origin, g.w);
        check(e.ok && e.b0 == 0 && e.b1 == 1, "T2 a drag onto bin 0 touches bins 0..1");
        check(e.loOpen && !e.hiOpen && e.hi == 108.0f,
              "T2 ...and opens the low side at 108");
        histHlArm(e.lo, e.hi, e.loOpen, e.hiOpen, -1, nullptr);
        const std::string said = histHlRangeSaid(d->dtype);
        fprintf(stderr, "histhlselftest: T2 says '%s'\n", said.c_str());
        check(said == "highlight < 108 DN", "T2 ...and says '< 108 DN', not '100..108'");
        histHlClear();

        // T3 - less than an intention. Four screen pixels is zoomRangeFromDrag's
        // threshold and it is this gesture's too: a click that jittered must not
        // arm a selection nobody drew.
        check(!histHlFromDrag(g.pr, ImVec2(300, 100), ImVec2(302, 100),
                              g.origin, g.w).ok,
              "T3 a 2 px drag is a click with jitter, not a selection");
        check(!app.highlight.on, "T3 ...and nothing is armed after it");
    }

    // ======================================================================
    // STAGE 2 - the paint and the count (§4, §5).
    // ======================================================================

    // ---- T4: the paint, and the PLANE BINDING -----------------------------
    // 4x4 RGGB u8, black 0 white 255 -> span 255, q = 0.996, integer width
    // ceil(q) = 1, origin = floor(0/1)*1 = 0. One bin per DN.
    // RGGB puts R at even x AND even y, so the R plane is exactly the four
    // pixels (0,0) (2,0) (0,2) (2,2).
    {
        auto d = mk("hl_t4", 4, 4, 1, "u8");
        d->cfa = 1; d->cfaPattern = 0;        // RGGB
        d->cfaColorize = false;               // plain grey mapping, so the
                                              // un-highlighted pixel is checkable
        auto put = [&](int x, int y, float v) { d->px()[(size_t)y * 4 + x] = v; };
        put(0, 0, 150);      // R, inside  [100, 200)
        put(0, 2, 200);      // R, OUTSIDE - the interval is half-open
        put(2, 2, 99);       // R, outside below
        put(2, 0, 0);        // R, outside below
        put(1, 0, 150);      // Gr, in range by VALUE but outside the binding
        setRange(*d, 0, 255);
        const Grid g = gridOf(*d);
        check(g.w == 1.0f && g.origin == 0.0f, "T4 one bin per DN from 0");
        // the drag: 100.0 -> 199.5 touches bins 100..199 -> [100, 200)
        HistHlRange r = histHlFromDrag(g.pr, g.pr.at(100.0f, 0), g.pr.at(199.5f, 0),
                                       g.origin, g.w);
        check(r.ok && r.lo == 100.0f && r.hi == 200.0f && !r.loOpen && !r.hiOpen,
              "T4 the drag is [100, 200), closed at both ends");
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, 0, "R");

        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        check(rgbaAt(px, *d, 0, 0) == MAGENTA, "T4 the one matching R pixel is painted");
        check(rgbaAt(px, *d, 0, 2) != MAGENTA,
              "T4 ...200 is NOT in [100, 200) - the interval is half-open");
        check(rgbaAt(px, *d, 2, 2) != MAGENTA, "T4 ...99 is below it");
        // the Gr pixel holds the SAME value and is not painted: the binding is
        // the plane, not the value alone
        check(rgbaAt(px, *d, 1, 0) != MAGENTA,
              "T4 ...and the Gr pixel of the same value is outside the binding");
        // ...and it still went through the ordinary mapping: v=150 over 0..255
        // at gamma 1 is 150/255*255 = 150 in all three channels
        check((rgbaAt(px, *d, 1, 0) == std::array<int, 4>{ 150, 150, 150, 255 }),
              "T4 ...drawn by the normal mapping, untouched");
        // the count: 1 of the plane's 4 finite pixels
        check(histHlSideSaid(d.get(), "A") ==
              "A: 1 px (25.00% of finite R, whole frame)",
              "T4 the sentence is '1 px (25.00% of finite R, whole frame)'");

        // NaN never matches, and never sits in the denominator either - the
        // isfinite rule the bins and the clip percentages already keep. With
        // one R pixel made NaN the plane has three finite pixels, so the same
        // single hit is now 1/3.
        put(2, 0, std::numeric_limits<float>::quiet_NaN());
        d->texDirty = true;
        renderDocRGBA(*d, px);
        check(histHlSideSaid(d.get(), "A") ==
              "A: 1 px (33.33% of finite R, whole frame)",
              "T4 a NaN pixel neither matches nor counts in the denominator");
        histHlClear();
    }

    // ---- T5: the equation, inside the grid --------------------------------
    // 16x16 u8, p = x + 16y, so every value 0..255 occurs exactly once. Grid
    // 1 DN per bin from 0. Selecting [100, 200) must paint exactly the values
    // 100..199 - one each, 100 pixels - and that has to equal the sum of the
    // bins the selection covers. Two code paths, one equation. The fixture is
    // 256 px (under the ~1M sampling cap, so step = 1) with no ROI, which is
    // what makes the two populations comparable at all.
    {
        auto d = mk("hl_t5", 16, 16, 1, "u8");
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) d->px()[(size_t)y * 16 + x] = (float)(x + 16 * y);
        setRange(*d, 0, 255);
        const Grid g = gridOf(*d);
        HistHlRange r = histHlFromDrag(g.pr, g.pr.at(100.0f, 0), g.pr.at(199.5f, 0),
                                       g.origin, g.w);
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, -1, nullptr);
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        const App::HlCount& C = app.hlCount[d->uid];
        app.hist[0] = App::HistState{};
        recomputeHistogramIfNeeded(d.get(), app.hist[0]);
        size_t binSum = 0;
        for (int b = r.b0; b <= r.b1; b++) binSum += app.hist[0].bins[0][b];
        fprintf(stderr, "histhlselftest: T5 painted=%zu binsum=%zu sampled=%zu\n",
                C.hit[0], binSum, app.hist[0].sampled);
        check(C.hit[0] == 100, "T5 [100, 200) paints the 100 values 100..199");
        check(C.hit[0] == binSum, "T5 ...and that IS the sum of the bins it covers");
        histHlClear();
    }

    // ---- T6: the equation at the EDGE - why the open rule exists ----------
    // 32x32 u16, p = x + 32y, so every value 0..1023 occurs exactly once. The
    // display range is squeezed to 0..255: span 255, q = 0.996, integer width
    // 1, origin 0 - a grid of 0..256 over data that runs to 1023. Every value
    // from 256 up is folded into bin 255, so that bar stands for 1 + 768 = 769
    // pixels.
    // A drag onto bin 255 therefore means [255, +inf), and the paint must find
    // 1024 - 255 = 769. READ LITERALLY as [255, 256) it would find 1, and the
    // user who dragged over the saturation bar would be handed a number 768
    // short of the bar they aimed at. That difference is the whole reason §2's
    // open-interval rule exists, and this is the assert that holds it.
    {
        auto d = mk("hl_t6", 32, 32, 1, "u16");
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) d->px()[(size_t)y * 32 + x] = (float)(x + 32 * y);
        setRange(*d, 0, 255);
        const Grid g = gridOf(*d);
        check(g.w == 1.0f && g.origin == 0.0f, "T6 the squeezed range still grids 0..256");
        // a drag entirely inside the last bin
        HistHlRange r = histHlFromDrag(g.pr, g.pr.at(255.1f, 0), g.pr.at(255.9f, 0),
                                       g.origin, g.w);
        check(r.ok && r.b0 == 255 && r.b1 == 255, "T6 the drag lands on bin 255");
        check(r.hiOpen && !r.loOpen && r.lo == 255.0f,
              "T6 ...and the high side opens: [255, +inf)");
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, -1, nullptr);
        check(histHlRangeSaid(d->dtype) == "highlight >= 255 DN",
              "T6 ...which is said as '>= 255 DN'");
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        const size_t painted = app.hlCount[d->uid].hit[0];
        app.hist[0] = App::HistState{};
        recomputeHistogramIfNeeded(d.get(), app.hist[0]);
        fprintf(stderr, "histhlselftest: T6 painted=%zu bin255=%u (literal [255,256) "
                        "would be 1)\n", painted, app.hist[0].bins[0][255]);
        check(painted == 769, "T6 the paint finds 769 - every value from 255 up");
        check(painted == app.hist[0].bins[0][255],
              "T6 ...and that IS the height of the bar that was dragged over");
        // T7 - the export path and the screen path are ONE mapping, highlight
        // included. Not "they agree": they are the same function, and this is
        // the assert that keeps it that way.
        std::vector<uint8_t> ex; int ew = 0, eh = 0;
        check(exportDocRGBA(d.get(), ex, ew, eh) && ew == d->w && eh == d->h,
              "T7 the same document exports");
        check(ex == px, "T7 ...byte for byte what the screen was given");
        histHlClear();
    }

    // ======================================================================
    // STAGE 3 - what the panel and the badge SAY, through real frames.
    // ======================================================================
    // A = 4x4 RGGB, B = another 4x4 RGGB, C(mono) is swapped in for the
    // name-mismatch half of T10.
    {
        auto A = mk("hl_A", 4, 4, 1, "u8");
        A->cfa = 1; A->cfaPattern = 0;
        A->px()[(size_t)0 * 4 + 0] = 150;      // R  (0,0) in range
        A->px()[(size_t)0 * 4 + 1] = 150;      // Gr (1,0) in range
        setRange(*A, 0, 255);
        auto B = mk("hl_B", 4, 4, 1, "u8");
        B->cfa = 1; B->cfaPattern = 0;
        B->px()[(size_t)0 * 4 + 0] = 120;      // R  (0,0) in range
        B->px()[(size_t)2 * 4 + 2] = 130;      // R  (2,2) in range
        setRange(*B, 0, 255);
        auto M = mk("hl_M", 4, 4, 1, "u8");    // mono: it has no R plane at all
        setRange(*M, 0, 255);
        app.images.push_back(std::move(A));
        app.images.push_back(std::move(B));
        app.images.push_back(std::move(M));
        ImageDoc* a = app.images[0].get();
        ImageDoc* b = app.images[1].get();
        ImageDoc* m = app.images[2].get();
        app.current = 0;

        const Grid g = gridOf(*a);
        HistHlRange r = histHlFromDrag(g.pr, g.pr.at(100.0f, 0), g.pr.at(199.5f, 0),
                                       g.origin, g.w);

        // ---- T9b: a CFA "all" binding names the planes SEPARATELY ---------
        // 4x4 RGGB gives every plane exactly 4 pixels. One R and one Gr are in
        // range, so R is 1/4 and Gr is 1/4 and the other two are 0/4 - and the
        // four numbers are never pooled into one, because planes are not mixed.
        app.histPlane = -1;
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, -1, nullptr);
        std::vector<uint8_t> px;
        renderDocRGBA(*a, px);
        const std::string all = histHlSideSaid(a, "A");
        fprintf(stderr, "histhlselftest: T9b '%s'\n", all.c_str());
        check(all == "A: R 1 (25.00%) | Gr 1 (25.00%) | Gb 0 (0.00%) | B 0 (0.00%) px"
                     "  (whole frame)",
              "T9b an 'all' binding on a mosaic says every plane, unpooled");

        // ---- T9a: the panel says it, and stops saying it when cleared -----
        histFrame();
        std::string panel = probeField("panel");
        fprintf(stderr, "histhlselftest: T9a panel '%s'\n", panel.c_str());
        check(has(panel, "highlight 100..200 DN"),
              "T9a the panel prints the interval it is holding");
        check(has(panel, "A: R 1 (25.00%)"), "T9a ...and the count beside it");
        check(has(panel, "whole frame"),
              "T9a ...naming the region every time, ROI or no ROI");
        // the badge is the same sentence, short, and it is what the canvas draws
        const std::string badge = histHlBadgeSaid();
        fprintf(stderr, "histhlselftest: T9a badge '%s'\n", badge.c_str());
        check(has(badge, "highlight 100..200 DN") && has(badge, "R 1"),
              "T9a the canvas badge carries the same statement");

        // ---- T10: B answers with ITS OWN pixels ---------------------------
        // The interval belongs to the axis, so every pane drawn answers for
        // itself. Bound to R this time, so the name-matching half has a name to
        // match on.
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, 0, "R");
        app.histPlane = 0;
        app.compareMode = App::CmpSplit;
        setCompareB(b);
        std::vector<uint8_t> pxa, pxb;
        renderDocRGBA(*a, pxa);
        renderDocRGBA(*b, pxb);
        histFrame();
        panel = probeField("panel");
        fprintf(stderr, "histhlselftest: T10 panel '%s'\n", panel.c_str());
        check(has(panel, "A: 1 px (25.00% of finite R"),
              "T10 A answers 1 of its 4 R pixels");
        check(has(panel, "B: 2 px (50.00% of finite R"),
              "T10 ...and B answers 2 of its own, on the same interval");

        // ...and a B that has no plane by that name is not painted BY NUMBER.
        // ch0 is not R: matching positionally is the error the panel already
        // forbids, so the honest answer is to paint nothing and say why.
        setCompareB(m);
        std::vector<uint8_t> pxm;
        renderDocRGBA(*m, pxm);
        check(std::count(pxm.begin(), pxm.end(), (uint8_t)255) == 0 ||
              histHlSideSaid(m, "B") == "B: no R plane",
              "T10 a mono B under an R binding is not painted");
        check(histHlSideSaid(m, "B") == "B: no R plane",
              "T10 ...and the panel says 'B: no R plane'");
        histFrame();
        check(has(probeField("panel"), "no R plane"),
              "T10 ...on screen, not only in the state");
        setCompareB(nullptr);
        app.compareMode = App::CmpOff;

        // ---- T11: the refusal, and the limit the difference view has ------
        // A pixel of an RGB frame has three values, so "in range" would have to
        // mean one of three things. v1 declines and SAYS so rather than
        // inventing an "any channel" reading.
        auto rgb = mk("hl_rgb", 4, 4, 3, "u8");
        setRange(*rgb, 0, 255);
        const std::string no = histHlRefusalFor(rgb.get(), -1);
        fprintf(stderr, "histhlselftest: T11 refusal '%s'\n", no.c_str());
        check(no == "highlight needs ONE channel - pick it above",
              "T11 RGB with 'all' selected is refused, in words");
        check(histHlRefusalFor(rgb.get(), 1).empty(),
              "T11 ...and the same frame is fine once a channel is picked");
        check(histHlRefusalFor(a, -1).empty() && histHlRefusalFor(m, -1).empty(),
              "T11 ...while a mosaic and a mono are never refused (anti-vacuity)");
        // and if such a document is DRAWN under an 'all' binding it is not
        // painted on a guess - it carries the refusal as its answer
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, -1, nullptr);
        std::vector<uint8_t> pxr;
        renderDocRGBA(*rgb, pxr);
        check(histHlSideSaid(rgb.get(), "B") ==
              "B: highlight needs ONE channel - pick it above",
              "T11 ...and a pane that cannot answer says so instead of painting");

        // The difference view is built from DIFFERENCES, not values, on a path
        // that never reaches renderDocRGBA. So it is not painted - and the
        // badge says exactly that, because a limit nobody is told about is
        // indistinguishable from a bug.
        app.compareMode = App::CmpDiff;
        check(histHlBadgeSaid() == "highlight armed - not shown in difference view",
              "T11 the difference view names the limit instead of going quiet");
        app.compareMode = App::CmpOff;

        // ---- the axis state survives a uid change (the flagship use) -------
        // "Select the tail, step frames, watch which pixels enter and leave."
        // Every step is a new uid; held on the document the selection would
        // die on the first step. Held on the AXIS it does not - and the new
        // frame answers with its own pixels.
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, 0, "R");
        const uint64_t wasUid = a->uid;
        app.current = 1;                       // as a frame step lands: another uid
        check(app.highlight.on && app.highlight.plane == 0 &&
              std::string(app.highlight.planeName) == "R" && b->uid != wasUid,
              "T12 the selection survives the uid change a frame step is");
        renderDocRGBA(*b, pxb);
        check(histHlSideSaid(b, "A") == "B: 2 px (50.00% of finite R, whole frame)" ||
              histHlSideSaid(b, "A") == "A: 2 px (50.00% of finite R, whole frame)",
              "T12 ...and the frame stepped ONTO answers with its own pixels");
        app.current = 0;

        // ---- and it is NOT in the session ----------------------------------
        // A lens, not a declaration (§1). The guarantee that this is safe is
        // T9a: the screen always says the highlight is on, so a lens left
        // armed can never become an invisible state.
        {
            std::ostringstream ss;
            writeSessionTo(ss);
            const std::string s = ss.str();
            check(!has(s, "highlight") && !has(s, "hlrange"),
                  "T13 the session carries no highlight key");
        }

        // ---- the probe is EMPTY when nothing is armed ----------------------
        histHlClear();
        histFrame();
        fprintf(stderr, "histhlselftest: T9a cleared probe '%s'\n", g_histHlProbe.c_str());
        check(probeField("panel").empty(),
              "T9a cleared: the panel stops claiming a highlight");
        check(histHlBadgeSaid().empty(), "T9a ...and the badge goes with it");
    }

    // The home is shared: leave nothing armed behind for the next test.
    histHlClear();
    app.histPlane = -1;
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = true;
    closeAll();
    fprintf(stderr, "histhlselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
