// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// Did startup make a window? False under --no-window, and nowhere else.
static bool g_haveWindow = false;
// The guard on the selftest groups that drive REAL ImGui frames.
//
// A frame goes through ImGui_ImplOpenGL3_NewFrame + ImGui_ImplGlfw_NewFrame,
// and those backends are not initialised when there is no window: calling them
// is a null device and a crash with nothing in it that names the test. Five
// groups do it (--tile-selftest T12, --verify-selftest's ROI probe,
// --abstats-selftest's T chart and the whole of
// --browse-keys-selftest, which runs the real loop), and every one of them is
// asserting on LAYOUT - which is exactly the thing that cannot be computed
// without the backend that reports the display.
//
// So they are the tests that keep needing a GL context, and this says so by
// name, with the line to change, if one is ever marked NOGL by mistake.
static bool needWindow(const char* what) {
    if (g_haveWindow) return true;
    fprintf(stderr, "%s drives real ImGui frames and needs a window: it cannot "
                    "run with --no-window.\n", what);
    fprintf(stderr, "  remove NOGL from its viewer_selftest() line in "
                    "CMakeLists.txt - that word is what passed --no-window.\n");
    // The caller returns from main() straight after this, and an unjoined
    // std::thread at exit is std::terminate() - "terminate called without an
    // active exception" printed right under the message above, which reads like
    // a crash in the thing being reported rather than the orderly refusal it
    // is. The selftests that exit early shut the same four workers down (see
    // --scan-selftest); by the time a test is drawing, it has started them.
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return false;
}

// ---- --roistats-selftest: the ROI table's NUMBERS, anywhere ----------------
// These assertions used to be the first half of --verify-selftest's V19, and
// V19 needs a window, so they ran on exactly one runner of the three: Linux,
// where CI installs xvfb. The PRNU column shipped having been measured on one
// OS. That is the same shape of mistake as test_prnu labelled `nogl` only and
// abstats-cfa-bayer measuring the wrong thing, and it is not a property of the
// arithmetic - it is a property of how the arithmetic was reached.
//
// What actually needs the window is LAYOUT: where the delete button landed and
// whether a click reaches it. That stays in V19. Everything here is a number,
// and a number does not need a display to be right.
//
// It is still the REAL panel, not a second implementation: drawPanelRois() is
// called, the numbers go through roiBasicStats' hash cache and the clipper, and
// what is asserted is what the PRNU cell printed, read back through the same
// g_roiRowProbe V19 uses. The only thing that changes is who supplies the frame.
//
// Fixtures are analytic and built in memory, so this test opens no file, needs
// no fixture directory, and cannot be made to pass by a fixture drifting.
static int roiStatsSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "roistatsselftest: %-46s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // every row is identical, so whatever rows the stride lands on give exactly
    // mean=base(+off), sd=amp, min/max=base(+off)-+amp
    auto mkMono = [](int W2, int H2, double base, double amp, double rightOff) {
        auto d = std::make_unique<ImageDoc>();
        d->name = "v19mono"; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
        d->src->dtype = "u16";
        d->src->data.resize((size_t)W2 * H2);
        for (int y = 0; y < H2; y++)
            for (int x = 0; x < W2; x++)
                d->px()[(size_t)y * W2 + x] =
                    (float)(base + ((x & 1) ? amp : -amp) + (x >= W2 / 2 ? rightOff : 0));
        d->syncMirrors();
        d->uid = app.nextUid++;
        return d;
    };
    // per-plane level plus a within-plane dither, so a plane's sd is NOT zero:
    // a flat plane would let a broken sd read 0 and look right
    auto mkCfaD = [](int W2, int H2, double amp) {
        auto d = std::make_unique<ImageDoc>();
        d->name = "v19cfa"; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
        d->cfa = 1; d->cfaPattern = 0; d->src->dtype = "u16";
        static const double LVL[4] = { 1000, 2000, 3000, 4000 };
        d->src->data.resize((size_t)W2 * H2);
        for (int y = 0; y < H2; y++)
            for (int x = 0; x < W2; x++)
                d->px()[(size_t)y * W2 + x] =
                    (float)(LVL[CFA_MAP[0][(y & 1) * 2 + (x & 1)]] +
                            (((x >> 1) & 1) ? amp : -amp));
        d->syncMirrors();
        d->uid = app.nextUid++;
        return d;
    };

    // One frame of the ROI panel, with or without the backends.
    //
    // ImGui_ImplGlfw_NewFrame and ImGui_ImplOpenGL3_NewFrame are the only part
    // of a frame that touches a device, and the only things they contribute
    // that ImGui cannot proceed without are three values: a display to lay out
    // in, a delta time, and a font atlas that has actually been rasterised.
    // Startup builds the ImGui context, the style and the atlas' font list even
    // under --no-window (it says so at the CreateContext call) - rasterising
    // the atlas is the RENDERER backend's job, and that is the one thing left
    // to do by hand. Everything the ROI table is made of downstream of this -
    // the table, the clipper, every widget's rect, and every number in it - is
    // pure CPU on both sides of the branch.
    //
    // Which is exactly the claim this test would be worthless without, so it is
    // MEASURED rather than asserted here: run the binary with and without
    // --no-window and diff the output. See the NOGL comment in CMakeLists.txt.
    ImGuiIO& io = ImGui::GetIO();
    auto roiFrame = [&]() {
        if (g_haveWindow) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
        } else {
            io.DisplaySize = ImVec2(1600, 1000);        // the real window's size
            io.DisplayFramebufferScale = ImVec2(1, 1);
            io.DeltaTime = 1.0f / 60.0f;
            if (!io.Fonts->IsBuilt()) io.Fonts->Build();
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_Always);
        ImGui::Begin("RoiStatsProbe", nullptr,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
        g_roiRowProbeOn = true;
        drawPanelRois();
        ImGui::End();
        ImGui::EndFrame();
    };
    auto settle = [&] { for (int f = 0; f < 4; f++) roiFrame(); };
    auto row = [&](int id) -> const RoiRowProbe* {
        for (const auto& r : g_roiRowProbe) if (r.id == id) return &r;
        return nullptr;
    };
    // No rect in this dump, deliberately. Where a row landed is a function of
    // the display the frame was laid out in, and this test's whole point is
    // that its output does not depend on that: the two runs the NOGL claim
    // rests on have to be comparable byte for byte.
    auto dump = [&](const char* what) {
        for (const auto& r : g_roiRowProbe)
            fprintf(stderr, "roistatsselftest: V19 %-16s %-3s mean=%.10g sd=%.10g "
                            "min=%.10g max=%.10g prnu=%.10g(ok=%d) n=%zu step=%zu "
                            "valid=%d\n",
                    what, r.id ? "roi" : "all", r.s.mean, r.s.sd, r.s.mn, r.s.mx,
                    r.prnu, (int)r.prnuOk, r.s.n, r.s.step, (int)r.s.valid);
    };
    auto nearEq = [](double a, double b) { return fabs(a - b) <= 1e-6 * std::max(1.0, fabs(b)); };

    closeAll();
    app.images.push_back(mkMono(1024, 1024, 30000.0, 2.0, 1000.0));
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 100, 100, 256, 256);          // wholly in the left half
    const int roiId = app.anns.back().id;
    app.roiChannel = -1;
    settle();
    dump("mono left");
    const RoiRowProbe* ra = row(0);
    const RoiRowProbe* rr = row(roiId);
    check(ra && ra->s.valid, "V19 panel prints the All row");
    check(rr && rr->s.valid, "V19 panel prints a placed ROI row");
    // sd is the whole point of this panel: assert it is the real sigma, never
    // 0. All row: values 29998/30002/30998/31002 in equal quarters -> mean
    // 30500, var = 500^2 + 2^2.
    if (ra) {
        check(nearEq(ra->s.mean, 30500.0), "V19 All row mean is the true mean");
        check(nearEq(ra->s.sd, sqrt(250004.0)), "V19 All row sd is the true sigma");
        check(nearEq(ra->s.mn, 29998.0) && nearEq(ra->s.mx, 31002.0),
              "V19 All row min/max are the true extremes");
        check(ra->s.n > 0 && ra->s.n < (size_t)1024 * 1024,
              "V19 All row decimates and can say n");
        // PRNU is those two numbers divided, so it is pinned to them rather
        // than to a second scan: on a decimated row that is the whole point -
        // the ratio describes the sample the sd describes
        check(ra->prnuOk && nearEq(ra->prnu, sqrt(250004.0) / 30500.0 * 100.0),
              "V19 All row PRNU is the sd and mean it printed, divided");
    }
    if (rr) {
        check(nearEq(rr->s.mean, 30000.0), "V19 ROI row mean is the true mean");
        check(nearEq(rr->s.sd, 2.0), "V19 ROI row sd is the true sigma");
        check(nearEq(rr->s.mn, 29998.0) && nearEq(rr->s.mx, 30002.0),
              "V19 ROI row min/max are the true extremes");
        check(rr->prnuOk && nearEq(rr->prnu, 2.0 / 30000.0 * 100.0),
              "V19 ROI row PRNU is sd / mean x 100");
    }
    // ...and the cached panel path must FOLLOW the ROI when it moves
    findAnn(roiId)->x = 600;                 // wholly in the right half
    app.annRev++;
    settle();
    dump("mono right");
    rr = row(roiId);
    check(rr && nearEq(rr->s.mean, 31000.0) && nearEq(rr->s.sd, 2.0),
          "V19 moving the ROI updates the printed numbers");
    // the same sd over a higher mean is a SMALLER percentage: a PRNU that
    // stayed at the old row's value would still look plausible
    check(rr && rr->prnuOk && nearEq(rr->prnu, 2.0 / 31000.0 * 100.0),
          "V19 PRNU moves with the ROI, not with the cache");

    // sigma at the noise floor, on a large pedestal: the regime this panel
    // exists for. sum2/n - mean^2 cancels ~10 of double's 16 digits here
    // (3.6e9 against a variance of 0.25), so if the two-pass form is fragile
    // for real 16-bit data this is where it shows. It is also the assertion
    // that most deserved to run on more than one compiler's floating point.
    closeAll();
    app.images.push_back(mkMono(1024, 1024, 60000.0, 0.5, 0.0));
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 0, 0, 512, 512);
    const int floorRoi = app.anns.back().id;
    app.roiChannel = -1;
    settle();
    dump("60000+/-0.5");
    if (const RoiRowProbe* rf = row(floorRoi)) {
        fprintf(stderr, "roistatsselftest: V19 noise floor sd err = %.3e (want 0.5)\n",
                rf->s.sd - 0.5);
        check(nearEq(rf->s.mean, 60000.0), "V19 noise-floor mean survives the pedestal");
        check(fabs(rf->s.sd - 0.5) < 1e-9, "V19 sigma 0.5 DN on a 60000 DN pedestal");
    }

    // channel switching, through the panel, on a Bayer frame: each plane is its
    // own population and must never be mixed with its neighbours
    closeAll();
    app.images.push_back(mkCfaD(1024, 1024, 3.0));
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 0, 0, 512, 512);
    const int cfaRoi = app.anns.back().id;
    static const double PLV[4] = { 1000, 2000, 3000, 4000 };
    bool chOk = true, chSd = true, chPr = true;
    double pooledPr = -1, planeSumPr = 0;
    app.roiChannel = -1;
    settle();
    dump("cfa all");
    if (const RoiRowProbe* r0 = row(cfaRoi)) {
        chOk = chOk && nearEq(r0->s.mean, 2500.0) && nearEq(r0->s.sd, sqrt(1250009.0));
        if (r0->prnuOk) pooledPr = r0->prnu;
    }
    for (int c = 0; c < 4; c++) {
        app.roiChannel = c;
        settle();
        dump(CFA_CH_NAMES[c]);
        const RoiRowProbe* rc = row(cfaRoi);
        if (!rc || !rc->s.valid || !nearEq(rc->s.mean, PLV[c])) chOk = false;
        if (!rc || !nearEq(rc->s.sd, 3.0)) chSd = false;
        // each plane divides by ITS OWN level, so the same 3 DN of dither is
        // 0.3% on R and 0.075% on B
        if (!rc || !rc->prnuOk || !nearEq(rc->prnu, 3.0 / PLV[c] * 100.0)) chPr = false;
        if (rc && rc->prnuOk) planeSumPr += rc->prnu;
    }
    check(chOk, "V19 channel switch repoints the panel's numbers");
    check(chSd, "V19 per-plane sd is the plane's own sigma");
    check(chPr, "V19 per-plane PRNU divides by that plane's own mean");
    // The pooled row is a different measurement, not a summary of the four: its
    // sd carries the level differences BETWEEN the planes, so its PRNU (~44.7%)
    // is two orders above the planes' (~0.16% mean). Anyone tempted to average
    // the plane column would land on the second number and it would look like
    // the answer.
    fprintf(stderr, "roistatsselftest: V19 pooled PRNU %.10g %%, planes averaged "
                    "%.10g %%\n", pooledPr, planeSumPr / 4.0);
    check(nearEq(pooledPr, sqrt(1250009.0) / 2500.0 * 100.0),
          "V19 the pooled row's PRNU is pooled sd over pooled mean");
    check(pooledPr > planeSumPr / 4.0 * 100.0,
          "V19 the pooled PRNU is not the planes' PRNUs averaged");
    app.roiChannel = -1;

    // A 1x1 ROI is reachable from the editor's x/y/w/h fields and from a
    // restored session. n = 1 makes sd exactly 0 and mean = min = max, which
    // reads exactly like a broken panel - so the panel has to be able to say n.
    // Pin both the number and the disclosure.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 4, 4, 1, 1);
    const int oneRoi = app.anns.back().id;
    app.roiChannel = -1;
    settle();
    dump("1x1");
    if (const RoiRowProbe* r1 = row(oneRoi)) {
        check(r1->s.valid && r1->s.n == 1, "V19 a 1x1 ROI measures exactly one pixel");
        check(r1->s.sd == 0.0 && r1->s.mn == r1->s.mx,
              "V19 n=1 gives sd 0 - the panel's n is what explains it");
        // no second convention for n=1: PRNU follows sd, which is 0, so the
        // column reads 0 and the same n line explains both
        check(r1->prnuOk && r1->prnu == 0.0,
              "V19 n=1 gives PRNU 0 for the same reason sd is 0");
        check(r1->s.step >= 1, "V19 the stat carries the stride it sampled at");
    }
    // ...and a decimated ROI must report the stride that produced its n
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 0, 0, 1024, 1024);
    const int bigRoi = app.anns.back().id;
    settle();
    dump("full 1024^2");
    if (const RoiRowProbe* rb = row(bigRoi))
        check(rb->s.step > 1 && rb->s.n < (size_t)1024 * 1024,
              "V19 a decimated ROI says both n and its stride");

    // the channel selector's clamp: one past the last plane is not a plane, and
    // CFA_SEL has no fifth entry
    app.roiChannel = 99;
    settle();
    check(app.roiChannel == 3, "V19 roiChannel clamps to the last CFA plane");
    app.roiChannel = -1;

    // ---- mean == 0 ---------------------------------------------------------
    // sd/mean has no value there, and this table's numbers are pasted into
    // reports: a nan or an inf in a pasted cell is worse than a blank, because
    // it travels as if it were a measurement. The row still has a real mean and
    // a real sd - only the ratio is withheld - so the test pins the sd too, or
    // "-" could mean the row died. A bipolar frame is the everyday way here (a
    // difference image).
    closeAll();
    {
        auto z = mkMono(256, 256, 0.0, 2.0, 0.0);
        z->src->dtype = "f32";          // values below zero are not a u16
        z->syncMirrors();
        app.images.push_back(std::move(z));
    }
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    addAnn(0, 0, 0, 256, 256);
    const int zeroRoi = app.anns.back().id;
    app.roiChannel = -1;
    settle();
    dump("mean 0");
    if (const RoiRowProbe* rz = row(zeroRoi)) {
        check(rz->s.valid && rz->s.mean == 0.0 && nearEq(rz->s.sd, 2.0),
              "V19 a bipolar ROI measures mean 0 with a real sd");
        check(!rz->prnuOk, "V19 mean 0 prints no PRNU - not a nan, not an inf");
    }
    if (const RoiRowProbe* rz0 = row(0))
        check(!rz0->prnuOk, "V19 the All row withholds PRNU at mean 0 too");

    g_roiRowProbeOn = false;
    fprintf(stderr, "roistatsselftest: %s\n", ok ? "ok" : "FAILED");
    // the same orderly shutdown every early-returning selftest does: an
    // unjoined worker at exit is std::terminate() under the last line printed
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}

// ---- --abeq-selftest: two sides on ONE document ----------------------------
// Putting the same document on both sides of a comparison is something people
// do ON PURPOSE - to confirm the two sides agree. The viewer used to treat it
// as a mistake to be smoothed over: resolveB answered null, so the histogram
// drew one curve instead of two, the projection and temporal panels lost their
// B column, the Inspector lost its B and A-B columns, the canvas put up an
// "A = B (paused)" banner, and the Files row you were standing on would not
// even offer "Set as compare B". The user's ruling (2026-08-04): 「A/B比較で，
// A=Bの時，ヒストグラムの表示を切り替えているけど，このケアはかえってみずらい．
// 比較の際に同じものを選んだ時に，同じことを確認できる方がよいので，こういうの
// 全般不要です」- the kindness makes it HARDER to read, because a panel that
// hides B at that moment makes the confirmation impossible: "B is identical to
// A" and "B is not being drawn" look exactly the same.
//
// So what is asserted here is the panel's OUTPUT, never the absence of a
// branch: with B set to A the Statistics panel reports TWO sides, two rows,
// two curves, in two different inks - and the numbers behind them coincide,
// which is the thing the user opened the comparison to see.
//
// Windowless, on the roiStatsSelftest() pattern and for the same reason: this
// is a statement about what a panel puts on screen, not about layout, and a
// statement that only runs on the one runner with a GL context has been
// verified on one OS. Fixtures are analytic and built in memory, so this test
// opens no file and cannot be made to pass by a fixture drifting.
//
// And the windowless claim is MEASURED, not asserted, exactly as roistats
// measures its own: run the binary with and without --no-window and diff the
// output. 25 lines, byte for byte identical (MinGW, 2026-08-05). Everything the
// Statistics panel is made of downstream of the backends' NewFrame - the table,
// the clipper, the bins, and the probe string this reads back - is pure CPU.
static int abEqSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "abeqselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto mk = [](const char* nm, int W2, int H2, double base, double amp) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
        d->src->dtype = "u16";
        d->src->data.resize((size_t)W2 * H2);
        for (int y = 0; y < H2; y++)
            for (int x = 0; x < W2; x++)
                d->px()[(size_t)y * W2 + x] = (float)(base + ((x & 1) ? amp : -amp));
        d->syncMirrors();
        computeMinMax(*d);
        defaultRange(*d);
        d->uid = app.nextUid++;
        return d;
    };
    ImGuiIO& io = ImGui::GetIO();
    auto histFrame = [&]() {
        // twice: the first frame sizes the window the table lays out in
        for (int pass = 0; pass < 2; pass++) {
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
            ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_Always);
            ImGui::Begin("AbEqProbe", nullptr,
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
            abStatsFrame();
            drawPanelHistogram();
            ImGui::End();
            ImGui::EndFrame();
        }
    };
    // "rows=" is written one letter at a time as "rowX;"; the other two fields
    // are letter sets. Same readers the N group of --abstats-selftest uses.
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

    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    app.cmpExtra.clear();
    app.compareFollowFrame = false;
    app.abStepBusyUntil = 0;
    app.abStatsLayout = App::AbOverlay;   // one axis, so "on the plot" means all of them
    app.histPlane = 0;                    // one plane, so the plot is not palette-limited
    app.images.push_back(mk("abeq_a", 512, 512, 30000.0, 4.0));
    app.images.push_back(mk("abeq_b", 512, 512, 20000.0, 4.0));
    app.current = 0;

    // ---- E1: B set to the document A is on -> TWO sides ---------------------
    app.compareMode = App::CmpWipe;
    setCompareB(cur());                   // Shift+B on the frame you are looking at
    check(cmpB() == cur(), "E1 B set to A resolves to A, not to silence");
    check(abStatsB() == cur(), "E1 ...and the statistics panels are given that B");
    histFrame();
    std::string p = g_histSideProbe;
    fprintf(stderr, "abeqselftest: E1 probe '%s'\n", p.c_str());
    check(rowsOf(p) == "AB", "E1 the statistics table gives A and B a row each");
    check(field(p, "curves=") == "AB", "E1 ...and both are drawn on the plot");
    check(field(p, "said-no-curve=").empty(),
          "E1 ...with neither side left off and named");
    // The coincidence has to be LEGIBLE, not merely present: two identical
    // curves in one colour are one curve. slotInk gives each side its own.
    check(slotInk(0) != slotInk(1), "E1 A and B are drawn in different inks");

    // ---- E2: and the two sides really do coincide ---------------------------
    // This is the confirmation the user opened the comparison for, so assert it
    // as the panel computed it: same document, same bin axis, same bins.
    {
        const App::HistState& A = app.hist[0];
        const App::HistState& B = app.hist[1];
        check(A.uid == cur()->uid && B.uid == cur()->uid,
              "E2 both sides measured the same document");
        check(A.black == B.black && A.white == B.white,
              "E2 ...on the same bin axis");
        bool sameBins = A.nSeries == B.nSeries && A.sampled == B.sampled &&
                        memcmp(A.bins, B.bins, sizeof A.bins) == 0;
        fprintf(stderr, "abeqselftest: E2 A n=%zu mean=%.10g / B n=%zu mean=%.10g\n",
                A.sampled, A.mean[0], B.sampled, B.mean[0]);
        check(sameBins, "E2 ...and the two curves are bin-for-bin identical");
    }

    // ---- E3: the chip names the pair, with no second vocabulary -------------
    // "A = B (paused: this image IS the pinned B - move A to resume)" existed
    // only to explain the silence. There is no silence, so there is nothing to
    // explain, and a state with its own sentence reads as a state you are stuck
    // in rather than one you chose.
    {
        std::string chip = abStatusChipText();
        fprintf(stderr, "abeqselftest: E3 chip '%s'\n", chip.c_str());
        check(chip.find("B:") != std::string::npos, "E3 the chip names B");
        check(chip.find("paused") == std::string::npos &&
              chip.find("no B image") == std::string::npos,
              "E3 ...and has no 'paused' / 'no B image' vocabulary for A == B");
    }

    // ---- E4: every Files row can be set as B, the current one included ------
    check(abRowItem(cur()) == AbSetB,
          "E4 the row you are standing on offers Set as compare B");
    check(abRowItem(app.images[1].get()) == AbSetB,
          "E4 ...and so does every other row");

    // ---- E5: a LETTER past B on A's document keeps its own side -------------
    // Same rule, one seat further out: two letters landing on one document is a
    // coincidence to be shown, not folded away. A slot standing on A used to
    // vanish from resolveSlots outright.
    setCompareB(app.images[1].get());     // B is the other image again
    addCompareSlot(cur());                // ...and C is the document A is on
    check(slotOf(cur()) == "C", "E5 the document A is on can hold a letter");
    check(resolveSlots().size() == 1 && resolveSlots()[0].doc == cur(),
          "E5 ...and it still resolves");
    histFrame();
    p = g_histSideProbe;
    fprintf(stderr, "abeqselftest: E5 probe '%s'\n", p.c_str());
    check(rowsOf(p) == "ABC", "E5 A, B and a C that IS A all get a row");
    check(field(p, "curves=") == "ABC", "E5 ...and all three are on the plot");

    // ---- E6: one pane per LETTER, not per document --------------------------
    // Side by side is where the coincidence is seen, so a pane is owed to every
    // seat. Three seats, three panes, even though two of them hold one image.
    {
        app.compareMode = App::CmpSplit;
        std::vector<TileSlot> sl = tileSlots();
        std::string tags;
        for (const TileSlot& t : sl) tags += t.tag;
        fprintf(stderr, "abeqselftest: E6 panes '%s'\n", tags.c_str());
        check(tags == "ABC", "E6 three seats give three panes, two of them one image");
        check(sl.size() == 3 && sl[0].doc == sl[2].doc,
              "E6 ...and A's pane and C's pane really are the same document");
    }

    app.cmpExtra.clear();
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = true;
    app.abStatsLayout = App::AbAuto;
    app.histPlane = -1;
    fprintf(stderr, "abeqselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
