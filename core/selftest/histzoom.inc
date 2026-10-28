// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --histzoom-selftest: the histogram zoom (#309) -------------------------
// A drag on the histogram re-lays its 256 bins on the dragged values; Shift (or
// the heading row's switch) makes the drag a value highlight instead; an option
// makes the zoom set the display range too. Issue #309's 試験 section is this
// file's running order.
//
// Windowless for histhl's reasons: every claim is a bin, a count, a range or a
// string the panel printed, and the fixtures are analytic and built in memory,
// with the expected numbers derived by hand beside the asserts.
//
// ONE selftest home is shared by every test in the suite, so every App field
// this file moves - the drag switch and the linked option (prefs keys), the
// zoom, the highlight, the Link scope and the range scope - is put back at the
// end, and nothing here calls savePrefs.
static int histZoomSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "histzoomselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto has = [](const std::string& s, const char* sub) {
        return s.find(sub) != std::string::npos;
    };
    auto probeField = [](const std::string& probe, const char* key) {
        const std::string k = std::string(key) + "=";
        size_t at = probe.find(k);
        if (at == std::string::npos) return std::string();
        at += k.size();
        size_t e = probe.find(';', at);
        return probe.substr(at, e == std::string::npos ? e : e - at);
    };
    auto hlField = [&](const char* key) { return probeField(g_histHlProbe, key); };

    // What this file borrows, handed back at the end.
    const int dragMode0 = app.histDragMode;
    const bool zoomSets0 = app.histZoomSetsRange;
    const bool link0 = app.linkRange;
    const float linkB0 = app.linkBlack, linkW0 = app.linkWhite;
    const int scope0 = app.rangeScope;
    const bool follow0 = app.compareFollowFrame;
    const uint64_t drawPass0 = app.drawPass;
    const int plane0 = app.histPlane;

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
    // The fixture every group uses: 64x64 u16 holding each value 0..4095 exactly
    // once, displayed 0..4095. Its display grid is
    //   span 4095, q = 15.996 -> integer width 16, origin 0: bins 0..4096.
    auto ramp = [&](const char* nm) {
        auto d = mk(nm, 64, 64, 1, "u16");
        for (size_t i = 0; i < d->px().size(); i++) d->px()[i] = (float)i;
        computeMinMax(*d);
        d->black = 0; d->white = 4095; d->texDirty = true;
        return d;
    };

    ImGuiIO& io = ImGui::GetIO();
    auto beginFrame = [&](ImVec2 mouse, bool down, bool shift) {
        if (g_haveWindow) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
        } else {
            io.DisplaySize = ImVec2(1600, 1000);
            io.DisplayFramebufferScale = ImVec2(1, 1);
            io.DeltaTime = 1.0f / 60.0f;
            if (!io.Fonts->IsBuilt()) io.Fonts->Build();
        }
        // after the backend, before NewFrame (verify.inc V19)
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_Always);
        ImGui::Begin("HistZoomProbe", nullptr,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
    };
    auto endFrame = [&]() { ImGui::End(); ImGui::EndFrame(); };
    // The real Statistics/Histogram panel, mouse parked off the plot.
    auto histFrame = [&]() {
        for (int pass = 0; pass < 2; pass++) {
            beginFrame(ImVec2(1500, 990), false, false);
            abStatsFrame();
            drawPanelHistogram();
            endFrame();
        }
    };
    // The gesture alone, on a rect spanning the grid the panel would put on the
    // axis for `d` - zoomed when the zoom is on, exactly as the panel bins it.
    PlotRect gpr;
    auto gestureFrame = [&](ImageDoc* d, ImVec2 at, bool down, bool shift) {
        beginFrame(at, down, shift);
        HistGrid zg;
        recomputeHistogramIfNeeded(d, app.hist[0], std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::quiet_NaN(), histZoomGridNow(zg));
        gpr = PlotRect{};
        gpr.p0 = ImVec2(60, 120);
        gpr.p1 = ImVec2(860, 420);
        gpr.xmin = app.hist[0].binOrigin;
        gpr.xmax = (float)((double)app.hist[0].binOrigin +
                           (double)app.hist[0].nBins * (double)app.hist[0].binW);
        gpr.ymin = 0; gpr.ymax = 1; gpr.ok = true;
        histDragGesture(gpr, d, app.hist[0]);
        endFrame();
    };
    auto dragOnce = [&](ImageDoc* d, float x0, float x1, bool shift) {
        gestureFrame(d, ImVec2(x0, 300), false, shift);   // hover
        gestureFrame(d, ImVec2(x0, 300), true, shift);    // press
        gestureFrame(d, ImVec2(x1, 300), true, shift);    // move
        gestureFrame(d, ImVec2(x1, 300), false, shift);   // release
    };
    auto doubleClick = [&](ImageDoc* d, float x) {
        gestureFrame(d, ImVec2(x, 300), false, false);
        gestureFrame(d, ImVec2(x, 300), true, false);
        gestureFrame(d, ImVec2(x, 300), false, false);
        gestureFrame(d, ImVec2(x, 300), true, false);     // the second click
        gestureFrame(d, ImVec2(x, 300), false, false);
    };
    // What a drag from screen x0 to x1 on the CURRENT axis would select - the
    // same pure mapping the release runs, asked before the drag.
    auto expectDrag = [&](ImageDoc* d, float x0, float x1) {
        gestureFrame(d, ImVec2(1500, 990), false, false);      // settle gpr / hist[0]
        return histHlFromDrag(gpr, ImVec2(x0, 300), ImVec2(x1, 300),
                              app.hist[0].binOrigin, app.hist[0].binW, !app.hist[0].zoomed,
                              app.hist[0].nBins);
    };

    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    app.cmpExtra.clear();
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = false;
    app.abStepBusyUntil = 0;
    app.linkRange = false;
    app.rangeScope = 0;              // per frame: setRange touches only the frame
    app.histPlane = -1;
    app.histDragMode = 0;
    app.histZoomSetsRange = false;
    app.histZoom = App::HistZoom{};
    histHlClear();
    app.drawPass++;

    // ======================================================================
    // Z1 - A ZOOM RE-BINS. Zoom onto [1024, 2048) from the 16 DN grid:
    //   span 1024, q = 4 -> width 4, origin floor(1024/4)*4 = 1024,
    //   so the grid is 1024..2048 in 4 DN bins: FOUR times finer.
    // Every value occurs once, so each zoomed bin holds exactly 4 pixels,
    // 1024 values (0..1023) fall below it and 2048 (2048..4095) above it.
    // ======================================================================
    {
        app.images.push_back(ramp("hz_z1"));
        ImageDoc* d = app.images.back().get();
        app.current = (int)app.images.size() - 1;
        histFrame();
        const App::HistState before = app.hist[0];
        check(before.binW == 16.0f && before.binOrigin == 0.0f && !before.zoomed,
              "Z1 fixture: unzoomed, the grid is 16 DN bins from 0");
        check(histZoomApply(d, 1024.0f, 2048.0f, before.binW), "Z1 the zoom is taken");
        check(app.histZoom.on && app.histZoom.lo == 1024.0f && app.histZoom.hi == 2048.0f,
              "Z1 ...onto the grid extent 1024..2048");
        histFrame();
        const App::HistState& H = app.hist[0];
        float o = 0, w = 0;
        int zn = 0;
        histZoomGrid(d->dtype, app.histZoom.lo, app.histZoom.hi, o, w, zn);
        fprintf(stderr, "histzoomselftest: Z1 grid origin=%.9g w=%.9g n=%d (histZoomGrid %.9g %.9g %d) "
                        "out %zu/%zu finite %zu\n", (double)H.binOrigin, (double)H.binW, H.nBins,
                (double)o, (double)w, zn, H.outLoN, H.outHiN, H.finite);
        check(H.zoomed && H.binOrigin == o && H.binW == w && H.nBins == zn && zn == 256,
              "Z1 binOrigin / binW / nBins are histZoomGrid(lo, hi) of the zoom");
        check(H.binW == 4.0f && H.binOrigin == 1024.0f,
              "Z1 ...which is 4 DN bins from 1024 (hand-derived): finer, not stretched");
        size_t sum = 0;
        bool every4 = true;
        for (int b = 0; b < 256; b++) { sum += H.bins[0][b]; every4 = every4 && H.bins[0][b] == 4; }
        check(every4, "Z1 every zoomed bin holds its 4 values");
        check(H.outLoN == 1024 && H.outHiN == 2048,
              "Z1 1024 values below the zoom, 2048 above it, binned nowhere");
        check(sum + H.outLoN + H.outHiN == H.finite && H.finite == 4096,
              "Z1 bins + outside == every finite sampled value");
        // A zoom is a view: the panel's numbers do not move.
        check(H.mean[0] == before.mean[0] && H.sd[0] == before.sd[0] &&
              H.sampled == before.sampled,
              "Z1 Quick stats (mean / std / sampled) are unchanged by the zoom");
        check(H.clipLo == before.clipLo && H.clipHi == before.clipHi,
              "Z1 ...and so are the <black / >white clip rates");
        // ...and the panel SAYS it: the note on the plot, the axis, the footer.
        const std::string zp = g_histZoomProbe;
        fprintf(stderr, "histzoomselftest: Z1 probe '%s'\n", zp.c_str());
        check(probeField(zp, "zoomed") == "1024..2048", "Z1 the panel declares the zoom");
        check(probeField(zp, "note") == "zoomed 1024..2048 - double-click to reset",
              "Z1 ...in the Temporal chart's words, top right");
        check(probeField(zp, "out") ==
                  "A:outside zoom <1024 25.00%  >=2048 50.00%",
              "Z1 ...and the footer counts what fell outside, as shares");
        check(probeField(zp, "marks").empty(),
              "Z1 the display range 0..4095 is not inside the zoom: no marks");
        const std::string xl = abHistXLabel(d, nullptr, H);
        fprintf(stderr, "histzoomselftest: Z1 axis '%s'\n", xl.c_str());
        check(has(xl.c_str(), "zoomed 1024..2048 DN, not the display range") &&
              has(xl.c_str(), "bins 1024..2048 DN, 4 DN/bin"),
              "Z1 the axis label names the zoom and the zoomed grid");
        // The display range moved INSIDE the zoom is marked at both ends.
        d->black = 1500; d->white = 1800; d->texDirty = true;
        histFrame();
        check(probeField(g_histZoomProbe, "marks") == "black,white",
              "Z1 a display range inside the zoom is marked at black and white");
        d->black = 0; d->white = 4095; d->texDirty = true;
        // The integer floor: a range under 2 DN has no histogram to show.
        app.toast.clear();
        check(!histZoomApply(d, 1024.0f, 1025.0f, 1.0f),
              "Z1 an integer zoom narrower than 2 DN is refused");
        check(has(app.toast.c_str(), "drag at least 2 DN"), "Z1 ...out loud");
        app.toast.clear();
        check(!histZoomApply(d, 0.0f, 4096.0f, 16.0f),
              "Z1 a zoom onto the whole axis is refused too (it would only shift it)");
        check(app.histZoom.on && app.histZoom.lo == 1024.0f,
              "Z1 ...and the zoom in force is left as it was");
        histZoomReset(d);
        histFrame();
        check(!app.hist[0].zoomed && app.hist[0].binW == 16.0f && app.hist[0].outLoN == 0,
              "Z1 reset: back on the display grid, end bins folding again");
        app.toast.clear();
    }

    // ======================================================================
    // Z2 - THE LINKED OPTION. Off: the range is never written. On: setRange,
    // and a reset gives the range before the FIRST zoom back - unless the user
    // set another range in between.
    // ======================================================================
    {
        ImageDoc* d = app.images.back().get();
        app.histZoomSetsRange = false;
        check(histZoomApply(d, 1024.0f, 2048.0f, 16.0f) && d->black == 0.0f && d->white == 4095.0f,
              "Z2 off: the zoom leaves the document's black / white alone");
        histZoomReset(d);
        check(d->black == 0.0f && d->white == 4095.0f, "Z2 off: ...and so does the reset");

        app.histZoomSetsRange = true;
        histZoomApply(d, 1024.0f, 2048.0f, 16.0f);
        check(d->black == 1024.0f && d->white == 2048.0f,
              "Z2 on: the zoom sets the display range to it (setRange)");
        // a zoom inside the zoom keeps the FIRST range as the way back
        histZoomApply(d, 1024.0f, 1536.0f, 4.0f);
        check(d->black == 1024.0f && d->white == 1536.0f && app.histZoom.prevWhite == 4095.0f,
              "Z2 on: a second zoom moves the range, the step back stays 0..4095");
        histZoomReset(d);
        check(d->black == 0.0f && d->white == 4095.0f && !app.histZoom.on,
              "Z2 on: reset gives back the range from before the first zoom");

        histZoomApply(d, 1024.0f, 2048.0f, 16.0f);
        setRange(*d, d->vmin, d->vmax);           // the Auto button's own call
        d->black = 7; d->white = 3000;            // ...or any other later word
        histZoomReset(d);
        check(d->black == 7.0f && d->white == 3000.0f && !app.histZoom.on,
              "Z2 on: a range set in between is NOT overwritten by the reset");

        // Linked across all images: setRange moves the LINK range, not the doc's.
        d->black = 0; d->white = 4095;
        app.linkRange = true; app.linkBlack = 0; app.linkWhite = 4095;
        histZoomApply(d, 1024.0f, 2048.0f, 16.0f);
        check(app.linkBlack == 1024.0f && app.linkWhite == 2048.0f &&
              d->black == 0.0f && d->white == 4095.0f,
              "Z2 on + Link range: the zoom moves the linked range");
        histZoomReset(d);
        check(app.linkBlack == 0.0f && app.linkWhite == 4095.0f,
              "Z2 on + Link range: ...and the reset gives it back");
        app.linkRange = false; app.linkBlack = linkB0; app.linkWhite = linkW0;
        app.histZoomSetsRange = false;
        markAllTexDirty();
    }

    // ======================================================================
    // Z3 - THE GESTURE. Plain drag = the lit switch, Shift = the other; the
    // switch swaps them, and the double-click undoes what a PLAIN drag does.
    // ======================================================================
    {
        ImageDoc* d = app.images.back().get();
        histHlClear();
        app.histZoom = App::HistZoom{};
        app.histDragMode = 0;
        HistHlRange e = expectDrag(d, 260, 460);
        dragOnce(d, 260, 460, false);
        float o = 0, w = 0;
        histBinGrid(d->dtype, e.lo, e.hi, o, w);
        check(app.histZoom.on && !app.highlight.on && app.histZoom.lo == o,
              "Z3 zoom mode: a plain drag zooms onto the dragged bins, highlights nothing");
        app.histZoom = App::HistZoom{};
        e = expectDrag(d, 300, 500);
        dragOnce(d, 300, 500, true);
        fprintf(stderr, "histzoomselftest: Z3 shift drag armed=%d lo=%.6g hi=%.6g "
                        "(expect %.6g..%.6g)\n", (int)app.highlight.on,
                (double)app.highlight.lo, (double)app.highlight.hi, (double)e.lo, (double)e.hi);
        check(app.highlight.on && !app.histZoom.on && app.highlight.lo == e.lo &&
              app.highlight.hi == e.hi && app.highlight.loOpen == e.loOpen &&
              app.highlight.hiOpen == e.hiOpen,
              "Z3 zoom mode: Shift+drag arms the highlight the drag always armed");
        // the double-click, in zoom mode, resets the zoom and keeps the highlight
        dragOnce(d, 260, 460, false);
        check(app.histZoom.on, "Z3 fixture: zoomed again before the double-click");
        doubleClick(d, 400);
        check(!app.histZoom.on && app.highlight.on,
              "Z3 zoom mode: double-click resets the zoom, the highlight stays");

        // ...and the switch swaps all three.
        app.histDragMode = 1;
        histHlClear();
        e = expectDrag(d, 300, 500);
        dragOnce(d, 300, 500, false);
        check(app.highlight.on && !app.histZoom.on && app.highlight.lo == e.lo,
              "Z3 highlight mode: a plain drag highlights");
        dragOnce(d, 260, 460, true);
        check(app.histZoom.on, "Z3 highlight mode: Shift+drag zooms");
        doubleClick(d, 400);
        check(!app.highlight.on && app.histZoom.on,
              "Z3 highlight mode: double-click clears the highlight, the zoom stays");
        histFrame();
        check(probeField(g_histZoomProbe, "mode") == "highlight" &&
              has(probeField(g_histZoomProbe, "note").c_str(), "- reset zoom above"),
              "Z3 highlight mode: the plot names the button that resets the zoom");
        app.histDragMode = 0;
        app.histZoom = App::HistZoom{};
        histHlClear();
    }

    // ======================================================================
    // Z4 - THE HIGHLIGHT ON A ZOOMED AXIS. A Shift+drag while zoomed snaps to
    // the zoomed (4 DN) bins, and touching an end bin does NOT open the side -
    // nothing folds into the end bins of a zoom. A zoom changes neither the
    // highlight's sentence nor Quick stats.
    // ======================================================================
    {
        ImageDoc* d = app.images.back().get();
        histZoomApply(d, 1024.0f, 2048.0f, 16.0f);
        HistHlRange e = expectDrag(d, 200, 40);      // to left of the plot: bin 0
        check(e.ok && e.b0 == 0 && !e.loOpen, "Z4 fixture: the drag touches zoomed bin 0");
        dragOnce(d, 200, 40, true);
        const float lo = app.highlight.lo, hi = app.highlight.hi;
        fprintf(stderr, "histzoomselftest: Z4 zoomed highlight %.6g..%.6g open %d/%d\n",
                (double)lo, (double)hi, (int)app.highlight.loOpen, (int)app.highlight.hiOpen);
        check(app.highlight.on && lo == 1024.0f && !app.highlight.loOpen &&
              std::fmod((double)hi - 1024.0, 4.0) == 0.0 && hi > lo && hi < 2048.0f,
              "Z4 the interval is on the zoomed 4 DN grid, closed at 1024");

        // The sentence: the count the paint measured, read through the panel,
        // zoomed and unzoomed. renderDocRGBA + histHlShown is the canvas's draw.
        std::vector<uint8_t> px;
        app.drawPass++;
        renderDocRGBA(*d, px); histHlShown(*d);
        histFrame();
        const std::string zoomedSaid = hlField("panel");
        const App::HistState zs = app.hist[0];
        histZoomReset(d);
        app.drawPass++;
        renderDocRGBA(*d, px); histHlShown(*d);
        histFrame();
        const std::string plainSaid = hlField("panel");
        fprintf(stderr, "histzoomselftest: Z4 said zoomed '%s' / plain '%s'\n",
                zoomedSaid.c_str(), plainSaid.c_str());
        check(!zoomedSaid.empty() && zoomedSaid == plainSaid,
              "Z4 the highlight's sentence is the same zoomed and unzoomed");
        check(zs.mean[0] == app.hist[0].mean[0] && zs.sd[0] == app.hist[0].sd[0],
              "Z4 ...and so is Quick stats");

        // A highlight wholly below a zoom is said to be there, not drawn as a
        // 1 px band at the edge.
        histHlArm(100, 200, false, false, -1, nullptr);
        histZoomApply(d, 1024.0f, 2048.0f, 16.0f);
        histFrame();
        check(probeField(g_histZoomProbe, "hl") == "left",
              "Z4 a highlight below the zoom is marked 'highlight outside' on the left");
        histHlArm(3000, 3100, false, false, -1, nullptr);
        histFrame();
        check(probeField(g_histZoomProbe, "hl") == "right",
              "Z4 ...and one above it on the right");
        histHlArm(1200, 1300, false, false, -1, nullptr);
        histFrame();
        check(probeField(g_histZoomProbe, "hl").empty(),
              "Z4 ...and one inside it gets its band, no mark (anti-vacuity)");
        // UNZOOMED, an off-axis highlight keeps main's 1 px edge band and no
        // text: the "outside" mark is a zoom's, and only a zoom's.
        app.histZoom = App::HistZoom{};
        histHlArm(5000, 5100, false, false, -1, nullptr);
        histFrame();
        check(probeField(g_histZoomProbe, "hl").empty(),
              "Z4 unzoomed, an off-axis highlight draws no 'outside' text");
        histHlClear();
        app.histZoom = App::HistZoom{};
    }

    // ======================================================================
    // Z5 - THE KEY. A frame step inside A's stack keeps the zoom; another
    // stack, a standalone image or another plane drops it.
    // ======================================================================
    {
        auto f0 = ramp("hz_seq_000"); f0->seqId = 93090; f0->seqIndex = 0;
        auto f1 = ramp("hz_seq_001"); f1->seqId = 93090; f1->seqIndex = 1;
        auto g0 = ramp("hz_other_000"); g0->seqId = 93091; g0->seqIndex = 0;
        app.images.push_back(std::move(f0));
        app.images.push_back(std::move(f1));
        app.images.push_back(std::move(g0));
        const int i0 = (int)app.images.size() - 3;
        app.current = i0;
        histFrame();
        histZoomApply(app.images[i0].get(), 1024.0f, 2048.0f, 16.0f);
        app.current = i0 + 1;                     // the frame step
        histFrame();
        check(app.histZoom.on && app.hist[0].zoomed && app.hist[0].binW == 4.0f,
              "Z5 a frame step inside the stack keeps the zoom");
        app.current = i0 + 2;                     // another stack
        histFrame();
        check(!app.histZoom.on && !app.hist[0].zoomed,
              "Z5 another stack drops it");
        app.current = i0;
        histZoomApply(app.images[i0].get(), 1024.0f, 2048.0f, 16.0f);
        app.current = 0;                          // Z1's standalone image
        histFrame();
        check(!app.histZoom.on, "Z5 a standalone image drops it");
        app.current = i0;
        histZoomApply(app.images[i0].get(), 1024.0f, 2048.0f, 16.0f);
        app.histPlane = 0;                        // another plane on the axis
        histFrame();
        check(!app.histZoom.on, "Z5 another plane drops it");
        app.histPlane = -1;

        // ---- every compared side is binned on the zoom too
        histZoomApply(app.images[i0].get(), 1024.0f, 2048.0f, 16.0f);
        app.compareMode = App::CmpSplit;
        setCompareB(app.images[i0 + 2].get());
        abStatsFrame();
        histFrame();
        check(app.hist[1].zoomed && app.hist[1].binOrigin == app.hist[0].binOrigin &&
              app.hist[1].binW == app.hist[0].binW && app.hist[1].outLoN == 1024,
              "Z5 B is binned on A's zoomed grid, and counts its own outside");
        check(has(probeField(g_histZoomProbe, "out").c_str(), "A:outside zoom"),
              "Z5 ...and the footer still says A's share");
        app.compareMode = App::CmpOff;
        setCompareB(nullptr);
        app.histZoom = App::HistZoom{};
    }

    // ======================================================================
    // Z7 - INTEGER ZOOMS NARROWER THAN 256 DN get one bin PER DN, so the axis
    // is exactly the dragged range. Without this an 8-bit image could not be
    // zoomed at all (its axis is already 1 DN/bin) and a 12-bit one never
    // below a 256 DN window.
    // u8 fixture: 16x16 holding each value 0..255 once, displayed 0..255, so
    // the display grid is 256 bins of 1 DN from 0. The zoom 100..140 (DN 100
    // through 140, the half-open [100, 141)) is 41 bins of 1 DN, 100 values
    // below them and 115 (141..255) above.
    // ======================================================================
    {
        auto u8 = mk("hz_u8", 16, 16, 1, "u8");
        for (size_t i = 0; i < u8->px().size(); i++) u8->px()[i] = (float)i;
        computeMinMax(*u8);
        u8->black = 0; u8->white = 255; u8->texDirty = true;
        app.images.push_back(std::move(u8));
        ImageDoc* d = app.images.back().get();
        app.current = (int)app.images.size() - 1;
        app.histZoom = App::HistZoom{};
        histHlClear();
        histFrame();
        check(app.hist[0].binW == 1.0f && app.hist[0].nBins == 256 && app.hist[0].binOrigin == 0.0f,
              "Z7 fixture: the u8 axis is already 256 bins of 1 DN");
        check(histZoomApply(d, 100.0f, 141.0f, app.hist[0].binW, app.hist[0].nBins),
              "Z7 an 8-bit image zooms (it used to be refused)");
        histFrame();
        const App::HistState& H = app.hist[0];
        size_t sum = 0;
        bool every1 = true;
        for (int b = 0; b < H.nBins; b++) { sum += H.bins[0][b]; every1 = every1 && H.bins[0][b] == 1; }
        bool restEmpty = true;
        for (int b = H.nBins; b < 256; b++) restEmpty = restEmpty && H.bins[0][b] == 0;
        fprintf(stderr, "histzoomselftest: Z7 u8 zoom origin=%.6g w=%.6g n=%d out %zu/%zu "
                        "finite %zu\n", (double)H.binOrigin, (double)H.binW, H.nBins,
                H.outLoN, H.outHiN, H.finite);
        check(H.zoomed && H.nBins == 41 && H.binW == 1.0f && H.binOrigin == 100.0f,
              "Z7 zoom 100..140 is 41 bins of 1 DN from 100");
        check(app.histZoom.lo == 100.0f && app.histZoom.hi == 141.0f,
              "Z7 ...and the zoom IS the dragged range, not a 256 DN window");
        check(every1 && restEmpty, "Z7 each DN in its own bin, no slot past the 41st used");
        check(H.outLoN == 100 && H.outHiN == 115 && sum + H.outLoN + H.outHiN == H.finite &&
              H.finite == 256,
              "Z7 bins + outside == every finite value (41 + 100 + 115)");
        check(probeField(g_histZoomProbe, "zoomed") == "100..141" &&
              has(abHistXLabel(d, nullptr, H).c_str(), "bins 100..141 DN, 1 DN/bin"),
              "Z7 the note and the axis span exactly 100..141");
        // A highlight dragged inside the 41-bin axis lands on whole DN, closed.
        HistHlRange e = expectDrag(d, 300, 900);    // past the right edge: last bin
        dragOnce(d, 300, 900, true);
        fprintf(stderr, "histzoomselftest: Z7 highlight %.6g..%.6g open %d/%d (bins %d..%d)\n",
                (double)app.highlight.lo, (double)app.highlight.hi, (int)app.highlight.loOpen,
                (int)app.highlight.hiOpen, e.b0, e.b1);
        check(app.highlight.on && app.highlight.lo == e.lo && app.highlight.hi == 141.0f &&
              e.b1 == 40 && !app.highlight.hiOpen &&
              app.highlight.lo == std::floor(app.highlight.lo) && app.highlight.lo > 100.0f,
              "Z7 a highlight on the zoomed axis is on DN boundaries, closed at 141");
        histHlClear();
        // The linked option sets exactly the dragged range.
        histZoomReset(d);
        app.histZoomSetsRange = true;
        histZoomApply(d, 100.0f, 141.0f, 1.0f, 256);
        check(d->black == 100.0f && d->white == 141.0f,
              "Z7 linked: the display range becomes 100..141");
        histZoomReset(d);
        check(d->black == 0.0f && d->white == 255.0f, "Z7 linked: ...and comes back");
        app.histZoomSetsRange = false;

        // 12-bit: a 60 DN zoom on the 0..4095 ramp is 60 bins of 1 DN.
        app.current = 0;                          // Z1's u16 0..4095 ramp
        ImageDoc* r = app.images[0].get();
        histFrame();
        check(histZoomApply(r, 2000.0f, 2060.0f, app.hist[0].binW, app.hist[0].nBins),
              "Z7 12-bit: a 60 DN zoom is taken");
        histFrame();
        const App::HistState& R = app.hist[0];
        size_t rs = 0;
        for (int b = 0; b < R.nBins; b++) rs += R.bins[0][b];
        check(R.nBins == 60 && R.binW == 1.0f && R.binOrigin == 2000.0f && rs == 60 &&
              R.outLoN == 2000 && R.outHiN == 2036 && rs + R.outLoN + R.outHiN == R.finite,
              "Z7 12-bit: 60 bins of 1 DN from 2000, bins + outside == 4096");
        app.histZoom = App::HistZoom{};
    }

    // ======================================================================
    // Z8 - EVERY SIDE ON A'S GRID, whatever its dtype. The zoom grid is
    // decided once from A's dtype and handed to B as-is: an f32 B re-deriving
    // its own grid from the zoom's extent would get 256 bins of 0.25 DN over
    // 100..164 against A's 41 bins of 1 DN over 100..141, and its curve would
    // be squeezed by 41/64 onto A's axis while its footer named A's bounds.
    // ======================================================================
    {
        auto A = mk("hz_mix_a8", 16, 16, 1, "u8");
        auto B = mk("hz_mix_bf", 16, 16, 1, "f32");
        for (size_t i = 0; i < A->px().size(); i++) { A->px()[i] = (float)i; B->px()[i] = (float)i; }
        computeMinMax(*A); computeMinMax(*B);
        A->black = 0; A->white = 255; B->black = 0; B->white = 255;
        app.images.push_back(std::move(A));
        app.images.push_back(std::move(B));
        ImageDoc* a = app.images[app.images.size() - 2].get();
        ImageDoc* b = app.images.back().get();
        app.current = (int)app.images.size() - 2;
        app.histZoom = App::HistZoom{};
        app.compareMode = App::CmpSplit;
        setCompareB(b);
        histFrame();
        histZoomApply(a, 100.0f, 141.0f, app.hist[0].binW, app.hist[0].nBins);
        histFrame();
        const App::HistState& HA = app.hist[0];
        const App::HistState& HB = app.hist[1];
        fprintf(stderr, "histzoomselftest: Z8 u8/f32 A %.6g/%.6g/%d  B %.6g/%.6g/%d  B out %zu/%zu\n",
                (double)HA.binOrigin, (double)HA.binW, HA.nBins,
                (double)HB.binOrigin, (double)HB.binW, HB.nBins, HB.outLoN, HB.outHiN);
        check(HA.nBins == 41 && HB.zoomed && HB.binOrigin == HA.binOrigin &&
              HB.binW == HA.binW && HB.nBins == HA.nBins,
              "Z8 u8 A / f32 B: B is binned on A's exact grid (origin, width, count)");
        check(HB.zoomHi == 141.0f && HB.outLoN == 100 && HB.outHiN == 115,
              "Z8 ...and B's outside counts use A's bounds (<100, >=141)");

        // u16 A / f32 B: 60 bins of 1 DN over 2000..2060 on both.
        auto A2 = ramp("hz_mix_a16");
        auto B2 = mk("hz_mix_bf2", 64, 64, 1, "f32");
        for (size_t i = 0; i < B2->px().size(); i++) B2->px()[i] = (float)i;
        computeMinMax(*B2); B2->black = 0; B2->white = 4095;
        app.images.push_back(std::move(A2));
        app.images.push_back(std::move(B2));
        ImageDoc* a2 = app.images[app.images.size() - 2].get();
        ImageDoc* b2 = app.images.back().get();
        app.current = (int)app.images.size() - 2;
        app.histZoom = App::HistZoom{};
        setCompareB(b2);
        histFrame();
        histZoomApply(a2, 2000.0f, 2060.0f, app.hist[0].binW, app.hist[0].nBins);
        histFrame();
        check(app.hist[0].nBins == 60 && app.hist[1].binOrigin == app.hist[0].binOrigin &&
              app.hist[1].binW == app.hist[0].binW && app.hist[1].nBins == 60 &&
              app.hist[1].outLoN == 2000 && app.hist[1].outHiN == 2036,
              "Z8 u16 A / f32 B: one 60-bin grid, B outside counted at A's bounds");
        app.compareMode = App::CmpOff;
        setCompareB(nullptr);
        app.histZoom = App::HistZoom{};

        // ---- Z9: the grid COVERS the drag, and is a fixed point of itself.
        // u16 [256, 1024): histBinGrid alone gives 3 DN bins from 255, ending
        // at 1023 - DN 1023 would be outside a range that contains it.
        float o = 0, w = 0; int n = 0;
        histZoomGrid("u16", 256.0f, 1024.0f, o, w, n);
        const double top = (double)o + (double)n * (double)w;
        fprintf(stderr, "histzoomselftest: Z9 u16 [256,1024) -> %.6g + %d x %.6g = %.6g\n",
                (double)o, n, (double)w, top);
        check(o <= 256.0f && top >= 1024.0, "Z9 u16 [256, 1024): the grid covers DN 1023");
        auto fixed = [&](const char* dt, float lo, float hi) {
            float o1 = 0, w1 = 0, o2 = 0, w2 = 0; int n1 = 0, n2 = 0;
            histZoomGrid(dt, lo, hi, o1, w1, n1);
            const float t1 = (float)((double)o1 + (double)n1 * (double)w1);
            histZoomGrid(dt, o1, t1, o2, w2, n2);
            const bool covers = o1 <= lo && (double)o1 + (double)n1 * (double)w1 >= (double)hi;
            fprintf(stderr, "histzoomselftest: Z9 %s [%.10g, %.10g) -> %.10g/%.6g/%d -> %.10g/%.6g/%d\n",
                    dt, (double)lo, (double)hi, (double)o1, (double)w1, n1, (double)o2,
                    (double)w2, n2);
            return covers && o1 == o2 && w1 == w2 && n1 == n2;
        };
        check(fixed("u16", 256.0f, 1024.0f) && fixed("u8", 100.0f, 141.0f) &&
              fixed("f32", -3.7f, 11.3f),
              "Z9 the grid of a grid's own extent is the same grid (u16, u8, f32)");
        check(fixed("u32", 4000000000.0f, 4000065536.0f) &&
              fixed("u32", 3000000000.0f, 3000100096.0f),
              "Z9 ...and for u32 values in the billions");
    }

    // ======================================================================
    // Z10 - "Auto per frame" + the linked option + a frame step. The step back
    // belongs to the DOCUMENT whose range the zoom wrote: reset restores that
    // frame, and never touches the frame now on screen, whose range is its own.
    // ======================================================================
    {
        auto f0 = ramp("hz_apf_000"); f0->seqId = 93092; f0->seqIndex = 0;
        auto f1 = ramp("hz_apf_001"); f1->seqId = 93092; f1->seqIndex = 1;
        f1->black = 10; f1->white = 4000;
        app.images.push_back(std::move(f0));
        app.images.push_back(std::move(f1));
        ImageDoc* d0 = app.images[app.images.size() - 2].get();
        ImageDoc* d1 = app.images.back().get();
        app.rangeScope = 0;
        app.histZoomSetsRange = true;
        app.current = (int)app.images.size() - 2;
        app.histZoom = App::HistZoom{};
        histFrame();
        histZoomApply(d0, 1024.0f, 2048.0f, app.hist[0].binW, app.hist[0].nBins);
        check(d0->black == 1024.0f && d0->white == 2048.0f && d1->black == 10.0f,
              "Z10 fixture: the linked zoom wrote frame 0's range only");
        app.current = (int)app.images.size() - 1;     // the frame step
        histFrame();
        check(app.histZoom.on, "Z10 ...and the step kept the zoom");
        histZoomReset(d1);                            // reset with frame 1 on screen
        check(d0->black == 0.0f && d0->white == 4095.0f,
              "Z10 reset gives the ZOOMED frame its range back");
        check(d1->black == 10.0f && d1->white == 4000.0f,
              "Z10 ...and leaves the frame on screen alone");
        app.histZoomSetsRange = false;
        app.histZoom = App::HistZoom{};
    }

    // ======================================================================
    // Z6 - THE TWO PREFERENCES are written, and read back by name.
    // ======================================================================
    {
        app.histDragMode = 1; app.histZoomSetsRange = true;
        std::ostringstream os;
        writePrefsTo(os);
        const std::string p = os.str();
        check(has(p.c_str(), "\nhistdrag 1\n") && has(p.c_str(), "\nhistzoomrange 1\n"),
              "Z6 prefs.txt carries the drag switch and the linked option");
        app.histDragMode = 0; app.histZoomSetsRange = false;
        std::ostringstream os0;
        writePrefsTo(os0);
        check(has(os0.str().c_str(), "\nhistdrag 0\n") &&
              has(os0.str().c_str(), "\nhistzoomrange 0\n"),
              "Z6 ...and their defaults (plain drag zooms, option off)");
    }

    // The home is shared: hand everything back.
    histHlClear();
    app.histZoom = App::HistZoom{};
    app.histDragMode = dragMode0;
    app.histZoomSetsRange = zoomSets0;
    app.linkRange = link0; app.linkBlack = linkB0; app.linkWhite = linkW0;
    app.rangeScope = scope0;
    app.compareFollowFrame = follow0;
    app.compareMode = App::CmpOff;
    setCompareB(nullptr);
    app.cmpExtra.clear();
    app.histPlane = plane0;
    app.toast.clear();
    app.drawPass = drawPass0;
    app.pendingSelect = -1;
    beginFrame(ImVec2(1500, 990), false, false);   // Shift released for whoever is next
    endFrame();
    closeAll();
    fprintf(stderr, "histzoomselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
