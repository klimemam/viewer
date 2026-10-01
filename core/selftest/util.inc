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

// ...and the OTHER half of the same question, which 板 304 is about: a test
// whose GL groups are one or two of eight does not have to cost the rest on
// two of the three runners. docs/background/analysis/measurement-kernel-inventory.md
// §4.3 counted what that was costing - --abstats-selftest's A1/A2/A3 (the
// 4x256 histogram memcmp, mean/sd, and the per-plane sigma_t that is the ONLY
// parity for that kernel) and --verify-selftest's V13 ran on one OS of three,
// because each test also holds a group that lays out a real ImGui frame (V19
// was verify's only one until 2026-09-28's V29; each takes its own gate).
//
// So a test with a GL group is registered TWICE: once as it was, and once NOGL
// with --nogl-groups-skipped, which is this flag. The pattern at the call site
// is two lines and no re-indentation:
//
//     if (!glGroup("--x-selftest (the group's name)")) return 1;
//     if (g_haveWindow) {
//         ...the group...
//     }
//
// It cannot silently skip: without the flag this is needWindow() exactly as
// before, which prints the line to change in CMakeLists.txt and fails. The
// SIBLING is the only run allowed to skip, and it says which group it skipped.
static bool glGroup(const char* what) {
    if (g_haveWindow) return true;
    if (g_noglGroupsSkipped) {
        fprintf(stderr, "%s: SKIPPED - no GL context, and this run is the NOGL "
                        "sibling that exists to carry every other group.\n", what);
        return true;
    }
    return needWindow(what);
}

// ---- fixtures no Python can be assumed for: .npy bytes, and an .npz ---------
// Selftests build .npz fixtures BY HAND (the V25 rule: CI installs numpy and
// nothing else, and a fixture that needs anything more is a fixture CI has
// never seen). Two of them - the container groups in --aset-selftest and
// --verify-selftest - each carried their own copy of this; a third copy was
// what adding one more npz fixture would have cost, so it lives here instead.
// Members are named WITHOUT the ".npy" the zip entry carries: that suffix is
// the npz format's, not the array's, and every caller was appending it.
// A directory a test was HANDED is not that test's to grow.
//
// The failure is positional and lands somewhere else: --browse-keys-selftest
// walks tools/testdata/rb with `home` then N x `down`, so ONE extra entry
// shifts every gesture and every check after it - measured 28 FAIL / 300.8 s,
// in a different test, with nothing pointing at the cause. CI never sees it
// (a fresh checkout every run); a developer's tree keeps it until someone
// deletes the folder by hand.
//
// So: count on the way in, compare on the way out. Deliberately about the
// DIRECTORY and not about any particular fixture - whatever a test grows next,
// it cannot leak an entry into the folder the next test walks without saying so
// where the cause is.
struct SharedFixture {
    std::string dir;
    int before = 0;
    explicit SharedFixture(const std::string& d) : dir(d) { before = count(); }
    int count() const {
        int n = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), ec))
            { (void)e; n++; }
        return n;
    }
    bool intact(const char* who) const {
        const int after = count();
        fprintf(stderr, "%s: shared fixture %s: %d entr(ies) before, %d after: %s\n",
                who, dir.c_str(), before, after, after == before ? "ok" : "FAIL");
        return after == before;
    }
};
// ...and where a test's OWN fixtures belong instead: the per-test TMP, which is
// why CMake gives every selftest its own TMP/TEMP/TMPDIR. A folder no other test
// can see whether this one reaches its cleanup or not. Returns an empty, freshly
// created directory (absolute, forward slashes).
static std::string selftestScratch(const char* name) {
    std::error_code ec;
    std::string s = (std::filesystem::temp_directory_path(ec) / name).u8string();
    std::replace(s.begin(), s.end(), '\\', '/');
    std::filesystem::remove_all(pathFromUtf8(s), ec);
    std::filesystem::create_directories(pathFromUtf8(s), ec);
    return s;
}
static std::vector<uint8_t> selftestNpyBytes(const char* descr,
                                             const std::vector<int64_t>& shape,
                                             const void* data, size_t nbytes) {
    std::string sh;
    if (shape.empty()) sh = "()";
    else if (shape.size() == 1) sh = "(" + std::to_string(shape[0]) + ",)";
    else {
        sh = "(";
        for (size_t i = 0; i < shape.size(); i++)
            sh += std::to_string(shape[i]) + (i + 1 < shape.size() ? ", " : "");
        sh += ")";
    }
    std::string hdr = std::string("{'descr': '") + descr +
                      "', 'fortran_order': False, 'shape': " + sh + ", }";
    hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
    hdr += '\n';
    std::vector<uint8_t> out;
    const char magic[8] = { '\x93', 'N', 'U', 'M', 'P', 'Y', '\x01', '\x00' };
    out.insert(out.end(), magic, magic + 8);
    out.push_back((uint8_t)(hdr.size() & 0xff));
    out.push_back((uint8_t)(hdr.size() >> 8));
    out.insert(out.end(), hdr.begin(), hdr.end());
    const uint8_t* p = (const uint8_t*)data;
    out.insert(out.end(), p, p + nbytes);
    return out;
}
// a STORED (method 0) zip of those members - what np.savez writes
static void selftestWriteNpz(const std::string& path,
                             const std::vector<std::pair<std::string,
                                                         std::vector<uint8_t>>>& mem) {
    std::vector<uint8_t> z;
    auto p16 = [&](uint32_t x) {
        z.push_back((uint8_t)(x & 0xff));
        z.push_back((uint8_t)((x >> 8) & 0xff));
    };
    auto p32 = [&](uint32_t x) {
        for (int i = 0; i < 4; i++) z.push_back((uint8_t)((x >> (8 * i)) & 0xff));
    };
    std::vector<uint32_t> offs;
    for (const auto& m : mem) {
        const std::string nm = m.first + ".npy";
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, m.second.data(), m.second.size());
        offs.push_back((uint32_t)z.size());
        p32(0x04034b50); p16(20); p16(0); p16(0); p16(0); p16(0);
        p32(crc); p32((uint32_t)m.second.size()); p32((uint32_t)m.second.size());
        p16((uint32_t)nm.size()); p16(0);
        z.insert(z.end(), nm.begin(), nm.end());
        z.insert(z.end(), m.second.begin(), m.second.end());
    }
    uint32_t cdOff = (uint32_t)z.size();
    for (size_t i = 0; i < mem.size(); i++) {
        const std::string nm = mem[i].first + ".npy";
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, mem[i].second.data(),
                                          mem[i].second.size());
        p32(0x02014b50); p16(20); p16(20); p16(0); p16(0); p16(0); p16(0);
        p32(crc); p32((uint32_t)mem[i].second.size()); p32((uint32_t)mem[i].second.size());
        p16((uint32_t)nm.size()); p16(0); p16(0); p16(0); p16(0); p32(0);
        p32(offs[i]);
        z.insert(z.end(), nm.begin(), nm.end());
    }
    uint32_t cdSize = (uint32_t)z.size() - cdOff;   // before the EOCD
    p32(0x06054b50); p16(0); p16(0);
    p16((uint32_t)mem.size()); p16((uint32_t)mem.size());
    p32(cdSize); p32(cdOff); p16(0);
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    f.write((const char*)z.data(), (std::streamsize)z.size());
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
    // ---- the input the W groups drive (see W12) -----------------------------
    // Pushed INSIDE the frame, after the platform backend: ImGui_ImplGlfw_NewFrame
    // feeds the real cursor and the real buttons every frame, the event queue is
    // applied in order, and the last event wins. Pushing before the frame is
    // therefore a no-op as soon as a window exists - measured, as a W12 that
    // passed with --no-window and failed without it, which is the one thing
    // CMakeLists does not allow this test to do.
    //
    // The POSITION is re-stated on every frame for the same reason: the backend
    // would otherwise put the cursor back where the person's hand left it. Keys
    // and characters are one-shot, because a key held down is a key repeating.
    struct RoiInject {
        bool on = false;                       // drive the mouse at all
        ImVec2 pos = ImVec2(-FLT_MAX, -FLT_MAX);
        bool down = false;
        ImGuiKey key = ImGuiKey_None;          // one key event, this frame
        bool keyDown = false;
        bool mod = false;                      // Ctrl held (re-stated per frame)
        std::string chars;                     // ...and characters, this frame
    } inj;
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
        if (inj.on) {
            io.AddMousePosEvent(inj.pos.x, inj.pos.y);
            io.AddMouseButtonEvent(0, inj.down);
        }
        if (inj.on) io.AddKeyEvent(ImGuiMod_Ctrl, inj.mod);
        if (inj.key != ImGuiKey_None) {
            io.AddKeyEvent(inj.key, inj.keyDown);
            inj.key = ImGuiKey_None;
        }
        for (char c : inj.chars) io.AddInputCharacter((unsigned int)(unsigned char)c);
        inj.chars.clear();
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
            fprintf(stderr, "roistatsselftest: V19 %-16s %-3s %c mean=%.10g sd=%.10g "
                            "min=%.10g max=%.10g prnu=%.10g(ok=%d) n=%zu step=%zu "
                            "valid=%d\n",
                    what, r.id ? "roi" : "all", r.side, r.s.mean, r.s.sd, r.s.mn, r.s.mx,
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

    // ---- R: the same ROIs, on more than one compared slot (issue #76) -------
    // The user's ruling, 2026-08-09: 「roiは共通、blinkのときは表示切り替え、
    // sidebysideのときは横に並べる(ヒストグラムとかと同様)」. Three claims, and
    // each is asserted on what the CELLS printed rather than on the state that
    // fed them, because the defect being fixed was a panel that had A's numbers
    // in it whatever the rest of the screen was showing:
    //   * blink       - the numbers are the BLINKED slot's, and they change when
    //                   the blink does. A table cannot overlay, so this is its
    //                   only way to put two rows in one place.
    //   * side by side- every slot gets a column group, at once.
    //   * shared      - ONE rectangle, N sets of numbers. Never N rectangles.
    // Analytic fixtures again, so a level is a level: A 30000, B 20000, C 10000,
    // all with the same 2 DN dither, so a cell holding the wrong slot's number
    // is off by ten thousand rather than by a rounding.
    auto rowSide = [&](int id, char side) -> const RoiRowProbe* {
        for (const auto& r : g_roiRowProbe) if (r.id == id && r.side == side) return &r;
        return nullptr;
    };
    auto sidesOn = [&](int id) {
        std::string s;
        for (const auto& r : g_roiRowProbe) if (r.id == id) s += r.side;
        return s;
    };
    // the probe's fields, read the way --abeq-selftest reads the histogram's
    auto pfield = [](const std::string& p, const char* key) {
        size_t k = p.find(key);
        if (k == std::string::npos) return std::string();
        k += strlen(key);
        size_t e = p.find(';', k);
        return p.substr(k, e == std::string::npos ? e : e - k);
    };

    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
    app.cmpExtra.clear();
    app.compareFollowFrame = false;
    app.abStepBusyUntil = 0;
    app.roiChannel = -1;
    app.images.push_back(mkMono(512, 512, 30000.0, 2.0, 0.0));   // A
    app.images.push_back(mkMono(512, 512, 20000.0, 2.0, 0.0));   // B
    app.images.push_back(mkMono(512, 512, 10000.0, 2.0, 0.0));   // C
    app.current = 0;
    addAnn(0, 32, 32, 128, 128);
    const int shRoi = app.anns.back().id;

    // ---- R1/R2: blink, and the table blinking WITH it -----------------------
    app.compareMode = App::CmpFlip;
    app.abStatsLayout = App::AbAuto;      // Auto over blink is one image area
    setCompareB(app.images[1].get());
    app.flipShowB = false;
    settle();
    std::string p = g_roiSideProbe;
    fprintf(stderr, "roistatsselftest: R1 probe '%s'\n", p.c_str());
    check(pfield(p, "sides=") == "AB", "R1 blink: the panel resolves A and B");
    check(pfield(p, "shown=") == "A", "R1 ...and shows A while the blink is on A");
    const RoiRowProbe* rA = rowSide(shRoi, 'A');
    check(rA && nearEq(rA->s.mean, 30000.0), "R1 ...printing A's numbers");

    app.flipShowB = true;                 // B / Space, or the auto timer
    settle();
    p = g_roiSideProbe;
    fprintf(stderr, "roistatsselftest: R2 probe '%s'\n", p.c_str());
    check(pfield(p, "shown=") == "B", "R2 blink to B: the table follows the blink");
    check(sidesOn(shRoi) == "B", "R2 ...and the row carries B's letter, not A's");
    const RoiRowProbe* rB = rowSide(shRoi, 'B');
    check(rB && nearEq(rB->s.mean, 20000.0),
          "R2 ...and B's numbers, not the ones that were there");
    // the letter that is NOT on the table is named: a slot the panel is silent
    // about is the defect #76 opened on, whichever direction it points
    check(pfield(p, "said-not-shown=") == "A", "R2 ...and A is named as not shown");

    // ---- R3: side by side - the slots next to each other, at once -----------
    app.abStatsLayout = App::AbSide;
    settle();
    p = g_roiSideProbe;
    fprintf(stderr, "roistatsselftest: R3 probe '%s'\n", p.c_str());
    check(pfield(p, "cols=") == "AB", "R3 side by side: both slots get a column group");
    check(pfield(p, "shown=").empty(), "R3 ...and nothing is collapsed away");
    check(sidesOn(shRoi) == "AB", "R3 ...the one ROI row carries both letters");
    rA = rowSide(shRoi, 'A'); rB = rowSide(shRoi, 'B');
    check(rA && rB && nearEq(rA->s.mean, 30000.0) && nearEq(rB->s.mean, 20000.0),
          "R3 ...each group holding its own slot's numbers");

    // ---- R4: a LETTER past B, because there are N slots and not two ---------
    addCompareSlot(app.images[2].get());
    settle();
    p = g_roiSideProbe;
    fprintf(stderr, "roistatsselftest: R4 probe '%s'\n", p.c_str());
    check(pfield(p, "sides=") == "ABC" && pfield(p, "cols=") == "ABC",
          "R4 a third slot gets a third column group");
    check(sidesOn(shRoi) == "ABC", "R4 ...on the same single ROI row");
    const RoiRowProbe* rC = rowSide(shRoi, 'C');
    check(rC && nearEq(rC->s.mean, 10000.0), "R4 ...carrying C's own numbers");
    check(rowSide(0, 'C') != nullptr, "R4 ...and the All row has a C group too");

    // ---- R5: the rectangles are SHARED --------------------------------------
    // One annotation, three sets of numbers - never three annotations. And it is
    // one rectangle in the sense that matters: move it, and every slot moves.
    check(app.anns.size() == 1, "R5 three slots did not make three ROIs");
    findAnn(shRoi)->x = 300;
    app.annRev++;
    settle();
    rA = rowSide(shRoi, 'A'); rB = rowSide(shRoi, 'B'); rC = rowSide(shRoi, 'C');
    check(rA && rB && rC && nearEq(rA->s.mean, 30000.0) &&
          nearEq(rB->s.mean, 20000.0) && nearEq(rC->s.mean, 10000.0),
          "R5 moving the shared ROI re-measures every slot");

    // ---- R6: a slot whose frame does not hold the rectangle -----------------
    // roiBasicStats CLAMPS, so the failure mode is not an empty cell - it is a
    // plausible number measured on the intersection and printed under the same
    // ROI's name. The panel must decline and SAY so, and A must be unaffected.
    app.images.push_back(mkMono(256, 256, 40000.0, 2.0, 0.0));   // D, smaller
    addCompareSlot(app.images[3].get());
    settle();
    p = g_roiSideProbe;
    fprintf(stderr, "roistatsselftest: R6 probe '%s'\n", p.c_str());
    check(pfield(p, "sides=") == "ABCD", "R6 the smaller frame is still a slot");
    check(pfield(p, "said-no-numbers=") == "D",
          "R6 ...and is NAMED as having none for a ROI it cannot hold");
    const RoiRowProbe* rD = rowSide(shRoi, 'D');
    check(rD && !rD->s.valid, "R6 ...its cell is empty, not a clamped intersection");
    check(rD && !rD->prnuOk, "R6 ...and no ratio is derived from numbers it lacks");
    rA = rowSide(shRoi, 'A');
    check(rA && nearEq(rA->s.mean, 30000.0), "R6 ...while A keeps printing");
    // the whole-image row is a per-image target, so D can always answer it
    const RoiRowProbe* aD = rowSide(0, 'D');
    check(aD && aD->s.valid && nearEq(aD->s.mean, 40000.0),
          "R6 ...and D's All row measures D's own frame");

    // ---- R7: no letter is dropped in silence --------------------------------
    // The N group's equation, transposed onto a table: a side is on the table
    // (a column group, or the one collapsed slot) or it is named as not being.
    // It holds on both forms, so it is asked on both.
    for (int form = 0; form < 2; form++) {
        app.abStatsLayout = form ? App::AbOverlay : App::AbSide;
        settle();
        p = g_roiSideProbe;
        const std::string all = pfield(p, "sides="),
                          acc = pfield(p, "cols=") + pfield(p, "shown=") +
                                pfield(p, "said-not-shown=");
        std::string lost;
        for (char ch : all) if (acc.find(ch) == std::string::npos) lost += ch;
        fprintf(stderr, "roistatsselftest: R7 %s probe '%s'\n",
                form ? "collapsed" : "laid out", p.c_str());
        check(lost.empty(), form ? "R7 collapsed: every side is shown or named"
                                 : "R7 laid out: every side is shown or named");
    }

    app.cmpExtra.clear();
    setCompareB(nullptr);
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = true;
    app.abStatsLayout = App::AbAuto;
    app.flipShowB = false;

    // ---- W: a WRITE to an annotation never clamps, and a band memory never
    //         crosses a frame of another size (user report 2026-09-29) -------
    // 「新しいファイルを開くと、ROI の原点がずれてあっちこっちいく」. Two of that
    // report's roots are writes, and both are measured here on the reporter's
    // own numbers: a 4096x3000 sensor, a 640x480 file and a 2048x1500 one, all
    // three open at once - which is the situation, because app.anns is shared
    // by every open document BY DESIGN (Ann has no document id; opening a file
    // does not clear the list) and switching documents is what "opening the
    // next file" is.
    //
    // What is asserted is what the rectangle IS after the write, never what the
    // widget looked like: both defects were in the arithmetic of the commit,
    // and the numbers do not need a display to be wrong.
    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0; app.roiChannel = -1;
    app.images.push_back(mkMono(4096, 3000, 1000.0, 1.0, 0.0));   // 0: the sensor
    app.images.push_back(mkMono(640, 480, 1000.0, 1.0, 0.0));     // 1: a small file
    app.images.push_back(mkMono(2048, 1500, 1000.0, 1.0, 0.0));   // 2: half of it
    app.current = 0;
    // Every refusal sentence this group reaches, in the order it was said. A
    // refusal nobody runs is a refusal that rots, so the ledger is asserted at
    // the end: one entry per wording the code can produce, all distinct.
    std::vector<std::string> wSaid;
    auto wSay = [&](const char* what, const App::Ann& a) {
        fprintf(stderr, "roistatsselftest: W %-24s %dx%d @%d,%d   prev x=%d w=%d "
                        "y=%d h=%d\n", what, a.w, a.h, a.x, a.y,
                a.prevX, a.prevW, a.prevY, a.prevH);
    };

    // ---- W1/W2: the reported gesture - edit ONE field with a small file
    //             open, on a ROI placed on the big one ----------------------
    addAnn(0, 3000, 2000, 200, 200);
    const int wRoi = app.anns.back().id;
    const App::Ann wPlaced = *findAnn(wRoi);
    app.current = 1;                       // "open" the 640x480 file
    // ...and DRAW the panel on it. Asserting straight after the assignment
    // pinned nothing - no code runs between the two lines, so it passed on
    // every build including the broken one. What has to be observed is the
    // PANEL's response to a ROI it cannot measure, and on all eight numbers:
    // a well-meant repair that moved only the origin would walk past a check
    // on x and w.
    settle();
    {
        const App::Ann* g1 = findAnn(wRoi);
        check(app.anns.size() == 1 && g1 && g1->x == wPlaced.x && g1->y == wPlaced.y &&
              g1->w == wPlaced.w && g1->h == wPlaced.h && g1->prevX == wPlaced.prevX &&
              g1->prevW == wPlaced.prevW && g1->prevY == wPlaced.prevY &&
              g1->prevH == wPlaced.prevH,
              "W1 drawing the panel on a smaller frame rewrites no number of the ROI");
    }
    {
        App::Ann* a = findAnn(wRoi);
        int f[4] = { a->x, a->y, a->w, 100 };            // h 200 -> 100, nothing else
        std::string why;
        bool wrote = annWriteFields(*a, f, cur()->w, cur()->h, &why);
        wSay("h 200 -> 100 on 640x480", *a);
        fprintf(stderr, "roistatsselftest: W2 wrote=%d why=[%s]\n",
                (int)wrote, why.c_str());
        check(wrote, "W2 the edit lands: a ROI off this frame is editable");
        check(a->x == 3000 && a->y == 2000 && a->w == 200 && a->h == 100,
              "W2 ...h is 100 and x / y / w untouched (not the clamped 1x1 @639,479)");
    }
    // ---- W3/W4: an edit that takes a ROI this frame HOLDS and puts it
    //             outside is refused, in the panel's own words --------------
    addAnn(0, 400, 300, 100, 100);                        // fits 640x480
    {
        App::Ann* a = findAnn(app.selectedAnn);
        int f[4] = { a->x, a->y, 500, a->h };             // w 100 -> 500: x+w = 900
        std::string why;
        bool wrote = annWriteFields(*a, f, cur()->w, cur()->h, &why);
        fprintf(stderr, "roistatsselftest: W3 wrote=%d why=[%s]\n",
                (int)wrote, why.c_str());
        check(!wrote && a->w == 100 && a->x == 400,
              "W3 an edit that would push a fitting ROI off the frame is refused");
        check(why.find("outside this frame (640x480)") != std::string::npos,
              "W3 ...and says so in the table's own words");
        wSaid.push_back(why);
        int g[4] = { a->x, a->y, a->w, -3 };              // h = -3 is not a size
        why.clear();
        check(!annWriteFields(*a, g, cur()->w, cur()->h, &why) && a->h == 100,
              "W4 a component that is not a rectangle at all is refused");
        fprintf(stderr, "roistatsselftest: W4 why=[%s]\n", why.c_str());
        check(why.find("h = -3") != std::string::npos,
              "W4 ...naming the component that was edited");
        wSaid.push_back(why);
    }
    // ---- W5: the pin - raising x by one must not move y -------------------
    addAnn(1, 3000, 2000, 0, 0);                          // a pin off this frame
    {
        App::Ann* a = findAnn(app.selectedAnn);
        int f[2] = { 3001, a->y };
        std::string why;
        bool wrote = annWriteFields(*a, f, cur()->w, cur()->h, &why);
        fprintf(stderr, "roistatsselftest: W5 pin @%d,%d wrote=%d why=[%s]\n",
                a->x, a->y, (int)wrote, why.c_str());
        check(wrote && a->x == 3001, "W5 a pin takes the number typed into its x");
        check(a->y == 2000, "W5 ...and y, which was not edited, does not move with it");
        // ...and the pin's own refusal sentence, which is not the rectangle's:
        // a pin has no w or h to name. Reached from a pin this frame HOLDS,
        // edited off it.
        a->x = 100; a->y = 300;                           // on the 640x480 frame
        int q[2] = { 700, a->y };
        why.clear();
        check(!annWriteFields(*a, q, cur()->w, cur()->h, &why) && a->x == 100,
              "W5b an edit that would push a fitting pin off the frame is refused");
        fprintf(stderr, "roistatsselftest: W5b why=[%s]\n", why.c_str());
        check(why.find("@700,300 would be outside this frame (640x480)") != std::string::npos &&
              why.find("the pin is as it was") != std::string::npos,
              "W5b ...naming the pixel it would have written, not a rectangle");
        wSaid.push_back(why);
    }

    // ---- W6..W9: the band memory (prevX/prevW) and a frame of another size
    // The report's second root. prev* is a rectangle in the pixels of whatever
    // frame was open when X was pressed, and Ann has no room to say which frame
    // that was - so the toggle refuses rather than guesses, and above all does
    // not overwrite the one copy of the rectangle it is holding.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 0;                                      // 4096x3000
    addAnn(0, 100, 100, 200, 200);
    const int bRoi = app.anns.back().id;
    toggleBand(true);                                     // X: the row band
    wSay("X on 4096x3000", *findAnn(bRoi));
    {
        const App::Ann* b = findAnn(bRoi);
        check(b->x == 0 && b->w == 4096 && b->h == 200,
              "W6 X bands the ROI to the full width of the frame it is on");
        check(b->prevX == 100 && b->prevW == 200,
              "W6 ...and remembers the rect it came from");
    }
    app.current = 2;                                      // "open" 2048x1500
    app.toast.clear();
    toggleBand(true);                                     // X on the smaller frame
    wSay("X on 2048x1500", *findAnn(bRoi));
    fprintf(stderr, "roistatsselftest: W7 toast [%s]\n", app.toast.c_str());
    {
        const App::Ann* b = findAnn(bRoi);
        check(b->prevX == 100 && b->prevW == 200,
              "W7 the rect the band was made from survives a smaller frame");
        check(app.toast.find("outside this frame (2048x1500)") != std::string::npos,
              "W7 ...and the refusal says which frame, in the panel words");
        check(app.toast.find("the rect it remembers is kept") != std::string::npos,
              "W7 ...and promises the memory it really is holding");
        wSaid.push_back(app.toast);
        const App::Ann was = *b;
        toggleBand(true);                                 // ...and again
        b = findAnn(bRoi);
        wSay("X again on 2048x1500", *b);
        check(b->x == was.x && b->y == was.y && b->w == was.w && b->h == was.h &&
              b->prevX == was.prevX && b->prevW == was.prevW,
              "W8 pressing X again writes nothing - no 4096-wide ROI is restored");
    }
    app.current = 0;                                      // back on 4096x3000
    toggleBand(true);
    wSay("X back on 4096x3000", *findAnn(bRoi));
    {
        const App::Ann* b = findAnn(bRoi);
        check(b->x == 100 && b->w == 200 && b->y == 100 && b->h == 200,
              "W9 back on a frame that holds it, X restores the ORIGINAL rect");
        check(b->prevW == -1, "W9 ...and the memory is spent, not left to fire twice");
    }
    // the column axis is the same code with y/h, and is asserted so that a fix
    // applied to one axis cannot pass for both
    toggleBand(false);                                    // Y: the column band
    {
        const App::Ann* b = findAnn(bRoi);
        check(b->y == 0 && b->h == 3000 && b->prevY == 100 && b->prevH == 200,
              "W9b Y bands the other axis and remembers that one");
    }
    app.current = 1;                                      // 640x480
    app.toast.clear();
    toggleBand(false);
    {
        const App::Ann* b = findAnn(bRoi);
        fprintf(stderr, "roistatsselftest: W9b toast [%s]\n", app.toast.c_str());
        check(b->prevY == 100 && b->prevH == 200,
              "W9b ...and the y memory survives the smaller frame too");
        wSaid.push_back(app.toast);
    }

    // ---- W10: a ROI outside the frame is a NORMAL state -------------------
    // The report is "the ROI moves"; the wrong cure is a viewer that tidies
    // ROIs onto whatever frame is showing. Opening another sensor file MUST
    // leave every number alone - the panel says "outside this frame" and that
    // is the whole of its response. Asserted through real frames of the real
    // panel, because the draw is where a well-meant repair would live.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 0;
    addAnn(0, 3000, 2000, 200, 200);
    addAnn(1, 4000, 2900, 0, 0);
    const std::vector<App::Ann> snap = app.anns;
    for (int c : { 1, 2, 0, 1, 2 }) { app.current = c; settle(); }
    bool wSame = app.anns.size() == snap.size();
    for (size_t i = 0; wSame && i < snap.size(); i++) {
        const App::Ann& g = app.anns[i];
        const App::Ann& e = snap[i];
        wSame = g.x == e.x && g.y == e.y && g.w == e.w && g.h == e.h &&
                g.prevX == e.prevX && g.prevW == e.prevW &&
                g.prevY == e.prevY && g.prevH == e.prevH;
        if (!wSame)
            fprintf(stderr, "roistatsselftest: W10 ann %zu moved: %dx%d @%d,%d -> "
                            "%dx%d @%d,%d\n", i, e.w, e.h, e.x, e.y, g.w, g.h, g.x, g.y);
    }
    check(wSame, "W10 opening frames of other sizes rewrites no annotation at all");

    // ---- W11: annFits must not overflow ----------------------------------
    // x + w in int is signed overflow for a w typed into the panel's field or
    // read from a session, and an overflowed compare reads as "it fits" - so
    // the editor would WRITE a rectangle 2^31 wide. The boundary is asserted on
    // both sides of it, and the overflow through annWriteFields, which is the
    // door a person actually gets to.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(0, 100, 100, 100, 100);
    {
        App::Ann* a = findAnn(app.selectedAnn);
        App::Ann t = *a;
        t.x = 639; t.y = 479; t.w = 1; t.h = 1;
        check(annFits(t, 640, 480), "W11 the last pixel of the frame fits");
        t.w = 2;
        check(!annFits(t, 640, 480), "W11 ...one past it does not");
        t.x = 0; t.y = 0; t.w = 640; t.h = 480;
        check(annFits(t, 640, 480), "W11 the whole frame fits exactly");
        t.w = 641;
        check(!annFits(t, 640, 480), "W11 ...one column more does not");
        t.x = 1; t.w = INT_MAX; t.h = 480;
        check(!annFits(t, 640, 480), "W11 x + w = 2^31 does not fit (it overflowed)");
        t.x = 0; t.w = 640; t.y = 1; t.h = INT_MAX;
        check(!annFits(t, 640, 480), "W11 ...nor does y + h");
        // and the door: the editor must refuse it rather than write it
        int g[4] = { a->x, a->y, INT_MAX, a->h };
        std::string why;
        const bool wrote = annWriteFields(*a, g, cur()->w, cur()->h, &why);
        fprintf(stderr, "roistatsselftest: W11 wrote=%d w=%d why=[%s]\n",
                (int)wrote, a->w, why.c_str());
        check(!wrote && a->w == 100, "W11 w = 2147483647 is refused, not written");
    }

    // ---- W12: a refused number must not stay in the box ------------------
    // The 2026-09-30 review's first finding, driven the way it is reached: real
    // clicks and real keys through the real panel. InputInt4 is a GROUP of four
    // children, and EndGroup reports the active child's id AND the deactivated
    // flag independently (imgui.cpp:11666-11686), so Tab from w to h makes
    // IsItemActive() and IsItemDeactivatedAfterEdit() both true in one frame.
    // wBoth below MEASURES that rather than taking the comment's word for it.
    // A buffer whose life was derived from IsItemActive() alone kept the
    // refused 500, and the next field's commit counted it as an edit: after
    // that, nothing could be written to the ROI at all.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(0, 400, 300, 100, 100);                        // fits, with room to fail
    const int eRoi = app.anns.back().id;
    bool wBoth = false;      // Active and DeactivatedAfterEdit in ONE frame
    bool wDrop = false;      // ...and a number commit dropped as another ROI's
    bool wNameDrop = false;  // ...and a name commit dropped the same way
    auto wStep = [&]() {
        roiFrame();
        if (g_roiEditProbe.act && g_roiEditProbe.done) wBoth = true;
        if (g_roiEditProbe.dropped) wDrop = true;
        if (g_roiEditProbe.nameDropped) wNameDrop = true;
    };
    auto wSettle = [&]() { for (int f = 0; f < 4; f++) wStep(); };
    auto wClick = [&](float x, float y) {
        inj.on = true; inj.pos = ImVec2(x, y); inj.down = false; wStep();
        inj.down = true;                                         wStep();
        inj.down = false;                                        wStep();
    };
    // ctrl = hold Ctrl over the key, which is how a text field is selected
    // all before typing over it (a click alone only places the caret).
    auto wKey = [&](ImGuiKey k, bool ctrl = false) {
        if (ctrl) { inj.mod = true;  wStep(); }
        inj.key = k; inj.keyDown = true;   wStep();
        inj.key = k; inj.keyDown = false;  wStep();
        if (ctrl) { inj.mod = false; wStep(); }
    };
    auto wType = [&](const char* t) {
        inj.chars = t;
        wStep(); wStep();
    };
    wSettle();
    const ImVec2 eMin = g_roiEditProbe.min, eMax = g_roiEditProbe.max;
    fprintf(stderr, "roistatsselftest: W12 fields %s, seeded for %d, buf %d %d %d %d\n",
            (eMax.x > eMin.x && eMax.y > eMin.y) ? "on screen" : "MISSING",
            g_roiEditProbe.seededFor, g_roiEditProbe.buf[0], g_roiEditProbe.buf[1],
            g_roiEditProbe.buf[2], g_roiEditProbe.buf[3]);
    // Not decoration: every claim below is made by clicking inside this rect,
    // so a panel that stopped drawing the fields has to FAIL here rather than
    // pass the rest by never reaching a widget.
    check(eMax.x > eMin.x && eMax.y > eMin.y && g_roiEditProbe.seededFor == eRoi &&
          g_roiEditProbe.buf[0] == 400 && g_roiEditProbe.buf[2] == 100,
          "W12 the editor's four fields are on screen, seeded from the ROI");
    const float eY = (eMin.y + eMax.y) * 0.5f;
    wClick(eMin.x + 4.0f, eY);                            // into x
    wKey(ImGuiKey_Tab);                                   // -> y
    wKey(ImGuiKey_Tab);                                   // -> w, text selected
    app.toast.clear();
    wType("500");                                         // w = 500: x + w = 900
    wKey(ImGuiKey_Tab);                                   // -> h: w commits HERE
    fprintf(stderr, "roistatsselftest: W12 after Tab: toast [%s] buf %d %d %d %d "
                    "act+done-in-one-frame=%d\n", app.toast.c_str(),
            g_roiEditProbe.buf[0], g_roiEditProbe.buf[1], g_roiEditProbe.buf[2],
            g_roiEditProbe.buf[3], (int)wBoth);
    check(wBoth, "W12 IsItemActive() and IsItemDeactivatedAfterEdit() in ONE frame");
    {
        const App::Ann* a = findAnn(eRoi);
        check(a->w == 100 && a->x == 400,
              "W12 ...the 500 is refused, so w is still 100");
        check(app.toast.find("500x100 @400,300 would be outside this frame (640x480)")
                  != std::string::npos,
              "W12 ...and the refusal names the rectangle it would have written");
        wSaid.push_back(app.toast);
        // The commit is this frame; the box is re-seeded on the NEXT one, which
        // is the frame the user sees. Measured: on the commit frame the buffer
        // still reads 500 (it is what was just refused).
        check(g_roiEditProbe.buf[2] == 500,
              "W12 ...the refused number is still in the buffer on the commit frame");
        wStep();
        check(g_roiEditProbe.buf[2] == 100 && g_roiEditProbe.buf[3] == 100,
              "W12 ...and the next frame puts the ROI's own numbers back in the boxes");
    }
    app.toast.clear();
    wType("50");                                          // h, still focused
    wKey(ImGuiKey_Enter);
    {
        const App::Ann* a = findAnn(eRoi);
        fprintf(stderr, "roistatsselftest: W12 after h=50: %dx%d @%d,%d toast [%s]\n",
                a->w, a->h, a->x, a->y, app.toast.c_str());
        check(a->x == 400 && a->y == 300 && a->w == 100 && a->h == 50,
              "W12 the NEXT edit writes its own component - h only");
        check(app.toast.find("500") == std::string::npos,
              "W12 ...and the refused 500 is not re-offered as this edit");
    }

    // ---- W13: a commit must not land on whichever ROI is selected now ----
    // The review's second finding. An active text field does not hold the
    // mouse (InputTextEx sets g.ActiveIdAllowOverlap = !io.MouseDown[0],
    // imgui_widgets.cpp:4694, and NewFrame does not clear it), so a press on
    // the canvas takes ActiveId and moves app.selectedAnn - and the canvas is
    // drawn BEFORE this panel in the same frame (core/main.cpp), so the panel
    // commits with the NEW selection in hand while InputTextEx re-applies the
    // typed text (imgui_widgets.cpp:5094-5105).
    //
    // The canvas itself needs a GL context, so the test reproduces the
    // ORDERING rather than the canvas: the selection moves before the panel
    // draws, and the field ends in that frame. That is the invariant either
    // way - a buffer belongs to the annotation it was seeded from.
    addAnn(0, 100, 100, 50, 50);                          // B, also on this frame
    const int e2Roi = app.anns.back().id;
    app.selectedAnn = eRoi;                               // ...editing A
    wSettle();
    wDrop = false;                                        // from here on it means W13
    wClick(eMin.x + 4.0f, eY);                            // into A's x
    wKey(ImGuiKey_Tab);                                   // -> A's y, selected
    wType("7");                                           // legal for BOTH ROIs
    app.selectedAnn = e2Roi;                              // the canvas' press
    app.toast.clear();
    wKey(ImGuiKey_Enter);                                 // ...and the commit lands
    {
        const App::Ann* a = findAnn(eRoi);
        const App::Ann* b2 = findAnn(e2Roi);
        fprintf(stderr, "roistatsselftest: W13 A %dx%d @%d,%d  B %dx%d @%d,%d  "
                        "dropped=%d\n", a->w, a->h, a->x, a->y, b2->w, b2->h, b2->x,
                b2->y, (int)g_roiEditProbe.dropped);
        check(b2->x == 100 && b2->y == 100 && b2->w == 50 && b2->h == 50,
              "W13 the number typed into A is not written to B");
        check(a->x == 400 && a->y == 300 && a->w == 100 && a->h == 50,
              "W13 ...and A is left as it was: an uncommitted edit is dropped");
        check(app.selectedAnn == e2Roi, "W13 ...while the selection still moved");
        // Without this the group would pass on a build where the commit never
        // arrived at all, which is not what is being claimed: the commit DID
        // arrive, carrying A's typed number, and was dropped for B.
        check(wDrop, "W13 ...and the drop is what stopped it, not a missing commit");
    }
    wKey(ImGuiKey_Escape);                                // leave no field active
    inj.pos = ImVec2(-FLT_MAX, -FLT_MAX); inj.down = false;
    wSettle();
    inj.on = false;                                       // the cursor is the user's again

    // ---- W14: the refusals nothing was running ---------------------------
    // Three wordings had no test at all, and one of them - the band memory's -
    // is the whole of this PR's second half. A refusal nobody runs is a
    // refusal that rots.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    // (a) a ROI off the frame with NO memory: the refusal must NOT promise to
    //     keep a rectangle it is not holding.
    app.current = 1;                                      // 640x480
    addAnn(0, 3000, 2000, 200, 200);
    app.toast.clear();
    toggleBand(true);
    fprintf(stderr, "roistatsselftest: W14a toast [%s]\n", app.toast.c_str());
    check(app.toast.find("is outside this frame (640x480)") != std::string::npos,
          "W14a X on a ROI this frame cannot hold is refused");
    check(app.toast.find("remembers") == std::string::npos,
          "W14a ...and promises no memory, because it is holding none");
    check(findAnn(app.selectedAnn)->prevW == -1 && findAnn(app.selectedAnn)->x == 3000,
          "W14a ...and writes nothing at all");
    wSaid.push_back(app.toast);
    // (b) a memory this frame cannot hold, reached the way a person reaches it:
    //     band on the big frame, open the small one, narrow the band to fit,
    //     press X. The ROI fits now; the RANGE it remembers does not.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 0;                                      // 4096x3000
    addAnn(0, 3000, 100, 200, 200);
    const int mRoi = app.anns.back().id;
    toggleBand(true);                                     // band, remembering x=3000 w=200
    app.current = 2;                                      // 2048x1500
    {
        App::Ann* a = findAnn(mRoi);
        int g[4] = { a->x, a->y, 2048, a->h };            // narrow it onto this frame
        std::string why;
        check(annWriteFields(*a, g, cur()->w, cur()->h, &why) && annFits(*a, 2048, 1500),
              "W14b a band narrowed onto the smaller frame fits it");
        // ...and edit the OTHER axis, which the x memory knows nothing about.
        // Now every w-by-h rectangle a refusal could print is one that never
        // existed: not the original (200x200), not the band (2048x200), not
        // what is on screen (2048x50). Only the RANGE is real.
        int g2[4] = { a->x, a->y, a->w, 50 };
        check(annWriteFields(*a, g2, cur()->w, cur()->h, &why) && a->h == 50,
              "W14b ...and the other axis is still editable");
        app.toast.clear();
        toggleBand(true);
        fprintf(stderr, "roistatsselftest: W14b toast [%s]\n", app.toast.c_str());
        check(app.toast.find("the x range this band was made from (x=3000 w=200)")
                  != std::string::npos,
              "W14b X names the RANGE it remembers");
        check(app.toast.find("@") == std::string::npos,
              "W14b ...and names no rectangle: none of them ever existed");
        check(app.toast.find("outside this frame (2048x1500)") != std::string::npos,
              "W14b ...while saying which frame it is measured against");
        check(a->prevX == 3000 && a->prevW == 200 && a->x == 0 && a->w == 2048 &&
              a->h == 50, "W14b ...keeps the range and writes nothing");
        wSaid.push_back(app.toast);
    }
    // (c) the same on the other axis, so one wording cannot cover for two
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 0;
    addAnn(0, 100, 2000, 200, 200);
    const int m2Roi = app.anns.back().id;
    toggleBand(false);                                    // remembers y=2000 h=200
    app.current = 1;                                      // 640x480
    {
        App::Ann* a = findAnn(m2Roi);
        int g[4] = { a->x, a->y, a->w, 480 };
        std::string why;
        check(annWriteFields(*a, g, cur()->w, cur()->h, &why) && annFits(*a, 640, 480),
              "W14c the column band narrowed onto the smaller frame fits it");
        app.toast.clear();
        toggleBand(false);
        fprintf(stderr, "roistatsselftest: W14c toast [%s]\n", app.toast.c_str());
        check(app.toast.find("the y range this band was made from (y=2000 h=200)")
                  != std::string::npos,
              "W14c Y names its own axis' range");
        check(a->prevY == 2000 && a->prevH == 200 && a->y == 0 && a->h == 480,
              "W14c ...keeps it and writes nothing");
        wSaid.push_back(app.toast);
    }
    // (d) a PIN off the frame: the old line told the user to select a pin
    //     while their pin was selected. Say the reason instead.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(1, 3000, 2000, 0, 0);
    app.hoverX = app.hoverY = -1;
    app.toast.clear();
    toggleBand(true);
    fprintf(stderr, "roistatsselftest: W14d toast [%s]\n", app.toast.c_str());
    check(app.toast.find("@3000,2000) is outside this frame (640x480)") != std::string::npos,
          "W14d X on a pin off this frame says which pin and which frame");
    check(app.toast.find("takes the row / column through the pin") != std::string::npos,
          "W14d ...and what X / Y does with a pin");
    check(app.toast.find("select a pin") == std::string::npos,
          "W14d ...not advice for a state the user is not in");
    check(app.anns.size() == 1, "W14d ...and makes no band");
    wSaid.push_back(app.toast);

    // ---- W16: the name field writes to the annotation it was typed for ---
    // The 2026-09-30 review's third finding, and the same shape as W13 three
    // lines above it in the panel: `if (InputText(...)) sel->label = buf;`
    // wrote on every keystroke, so the canvas press that moves
    // app.selectedAnn landed the name being typed for A on B. Not a
    // measurement (the rectangle and every number stay as they were, and the
    // three places that show a label agree the moment it lands), but a write
    // has to go where the user aimed it - and a label IS the column name of a
    // measurement in the Analysis grid.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(0, 400, 300, 100, 100);                        // A = "ROI 1"
    const int nRoiA = app.anns.back().id;
    addAnn(0, 100, 100, 50, 50);                          // B = "ROI 2"
    const int nRoiB = app.anns.back().id;
    app.selectedAnn = nRoiA;
    wSettle();
    const ImVec2 nMin = g_roiEditProbe.nameMin, nMax = g_roiEditProbe.nameMax;
    check(nMax.x > nMin.x && nMax.y > nMin.y && g_roiEditProbe.nameFor == nRoiA,
          "W16 the name field is on screen and seeded from the selected ROI");
    const float nY = (nMin.y + nMax.y) * 0.5f;
    // a name that commits normally: click in, select all, type, Enter
    wClick(nMin.x + 4.0f, nY);
    wKey(ImGuiKey_A, true);                               // Ctrl+A: select all
    wType("dark");
    wKey(ImGuiKey_Enter);
    fprintf(stderr, "roistatsselftest: W16 A label [%s]  B label [%s]\n",
            findAnn(nRoiA)->label.c_str(), findAnn(nRoiB)->label.c_str());
    check(findAnn(nRoiA)->label == "dark", "W16 a committed name lands on its own ROI");
    check(findAnn(nRoiB)->label == "ROI 2", "W16 ...and only on that one");
    // ...and one that does NOT commit, with the selection moving first
    wDrop = false;
    wClick(nMin.x + 4.0f, nY);
    wKey(ImGuiKey_A, true);
    wType("bright");
    app.selectedAnn = nRoiB;                              // the canvas' press
    wKey(ImGuiKey_Enter);
    fprintf(stderr, "roistatsselftest: W16 after the selection moved: A [%s]  B [%s]  "
                    "nameDropped=%d\n", findAnn(nRoiA)->label.c_str(),
            findAnn(nRoiB)->label.c_str(), (int)wNameDrop);
    check(findAnn(nRoiB)->label == "ROI 2",
          "W16 a name typed for A is NOT written to B");
    check(findAnn(nRoiA)->label == "dark",
          "W16 ...and A keeps the name it had: an uncommitted edit is dropped");
    check(wNameDrop, "W16 ...and the drop is what stopped it, not a missing commit");

    // ---- W17: the pin's own two fields, driven by real input -------------
    // The same seam as the rectangle, and until now asserted only by calling
    // annWriteFields directly - which cannot see the buffer discipline around
    // it. Driven here through the widget, like W12/W13.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(1, 100, 200, 0, 0);                            // a pin this frame holds
    const int pA = app.anns.back().id;
    addAnn(1, 300, 400, 0, 0);
    const int pB = app.anns.back().id;
    app.selectedAnn = pA;
    wSettle();
    const ImVec2 pMin = g_roiEditProbe.min, pMax = g_roiEditProbe.max;
    check(pMax.x > pMin.x && g_roiEditProbe.seededFor == pA &&
          g_roiEditProbe.buf[0] == 100 && g_roiEditProbe.buf[1] == 200,
          "W17 the pin's two fields are on screen, seeded from the pin");
    const float pY = (pMin.y + pMax.y) * 0.5f;
    wClick(pMin.x + 4.0f, pY);                            // into x
    wKey(ImGuiKey_A, true);
    wType("101");                                         // x + 1, y untouched
    wKey(ImGuiKey_Enter);
    {
        const App::Ann* a = findAnn(pA);
        fprintf(stderr, "roistatsselftest: W17 pin @%d,%d\n", a->x, a->y);
        check(a->x == 101, "W17 the pin's x takes the number typed into it");
        check(a->y == 200, "W17 ...and y, which was not edited, does not move");
    }
    wDrop = false;
    wClick(pMin.x + 4.0f, pY);
    wKey(ImGuiKey_A, true);
    wType("7");
    app.selectedAnn = pB;                                 // ...and the selection moves
    wKey(ImGuiKey_Enter);
    {
        const App::Ann* a = findAnn(pA);
        const App::Ann* b2 = findAnn(pB);
        fprintf(stderr, "roistatsselftest: W17 A @%d,%d  B @%d,%d  dropped=%d\n",
                a->x, a->y, b2->x, b2->y, (int)wDrop);
        check(b2->x == 300 && b2->y == 400, "W17 a number typed for one pin is not written to another");
        check(a->x == 101 && a->y == 200, "W17 ...and the first pin is left as it was");
        check(wDrop, "W17 ...and the drop is what stopped it");
    }
    wKey(ImGuiKey_Escape);
    inj.pos = ImVec2(-FLT_MAX, -FLT_MAX); inj.down = false;
    wSettle();
    inj.on = false;                                       // the cursor is the user's again

    // ---- W18: the write gate keeps the overflow OUT ----------------------
    // annFits is 64-bit safe (W11), but a ROI that is already off the frame
    // skips its gate, so w = 2147483647 used to be WRITABLE - and from there
    // every x + w in int downstream is undefined. The gate has an upper bound
    // now, and it refuses rather than clamps.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // 640x480
    addAnn(0, 3000, 2000, 200, 200);                      // OFF this frame: no fit gate
    {
        App::Ann* a = findAnn(app.selectedAnn);
        std::string why;
        int g[4] = { a->x, a->y, INT_MAX, a->h };
        check(!annWriteFields(*a, g, cur()->w, cur()->h, &why) && a->w == 200,
              "W18 w = 2147483647 on an off-frame ROI is refused, not written");
        fprintf(stderr, "roistatsselftest: W18 why=[%s]\n", why.c_str());
        check(why.find("past this viewer's 32768 px limit") != std::string::npos,
              "W18 ...naming the limit it is past");
        wSaid.push_back(why);
        int g2[4] = { a->x, a->y, 32768, a->h };          // the limit itself
        check(annWriteFields(*a, g2, cur()->w, cur()->h, &why) && a->w == 32768,
              "W18 the limit itself is writable - the biggest frame is not special");
        int g3[4] = { a->x, a->y, 32769, a->h };
        check(!annWriteFields(*a, g3, cur()->w, cur()->h, &why) && a->w == 32768,
              "W18 ...and one past it is not");
        int g4[4] = { 40000, a->y, a->w, a->h };
        check(!annWriteFields(*a, g4, cur()->w, cur()->h, &why) && a->x == 3000,
              "W18 the same bound holds for an origin");
    }

    // ---- W19: X / Y over a pixel of the OTHER pane, and the band it makes -
    // Two things nothing was running. The refusal first: a bigger B beside A
    // puts the cursor on pixels that are not A's (the readout follows the pane
    // on purpose), and the old line answered "hover a pixel" to someone who
    // was hovering one.
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.current = 1;                                      // A = 640x480
    app.hoverX = 2000; app.hoverY = 1000; app.hoverInB = true;   // a pixel of B
    app.toast.clear();
    toggleBand(true);
    fprintf(stderr, "roistatsselftest: W19 toast [%s]\n", app.toast.c_str());
    check(app.toast.find("the pixel under the cursor (@2000,1000)") != std::string::npos,
          "W19 X over a pixel of the compared pane names that pixel");
    check(app.toast.find("on the compared pane and outside this frame (640x480)")
              != std::string::npos,
          "W19 ...and says it is not a pixel of the frame the ROIs are on");
    check(app.toast.find("hover a pixel") == std::string::npos,
          "W19 ...not advice to do what the user is already doing");
    check(app.anns.empty(), "W19 ...and makes no band");
    wSaid.push_back(app.toast);
    // ...and the arm itself, which had no test at all: a hovered pixel of THIS
    // frame makes the row band, and X again collapses it onto that one pixel.
    app.hoverX = 40; app.hoverY = 60; app.hoverInB = false;
    app.toast.clear();
    toggleBand(true);
    check(app.anns.size() == 1 && app.anns[0].x == 0 && app.anns[0].y == 60 &&
          app.anns[0].w == 640 && app.anns[0].h == 1,
          "W19b a hovered pixel makes the row band of THIS frame");
    check(app.anns[0].label == "row 60", "W19b ...named by the row it is");
    check(app.anns[0].prevX == 40 && app.anns[0].prevW == 1,
          "W19b ...remembering the pixel to collapse back onto");
    toggleBand(true);
    check(app.anns.size() == 1 && app.anns[0].x == 40 && app.anns[0].w == 1,
          "W19b X again collapses the band onto that pixel");
    // the column band is the other axis of the same arm
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    toggleBand(false);
    check(app.anns.size() == 1 && app.anns[0].x == 40 && app.anns[0].y == 0 &&
          app.anns[0].w == 1 && app.anns[0].h == 480 &&
          app.anns[0].label == "col 40",
          "W19b ...and Y makes the column band of this frame");
    // a pin that IS on this frame goes through the same arm
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.hoverX = app.hoverY = -1;
    addAnn(1, 11, 22, 0, 0);
    toggleBand(true);
    check(app.anns.size() == 2 && app.anns[1].y == 22 && app.anns[1].h == 1 &&
          app.anns[1].w == 640, "W19c X on a pin this frame holds makes the pin's row");
    app.hoverX = app.hoverY = -1;

    // ---- W15: every refusal this panel can say was said above ------------
    // Eight wordings, eight entries, all different. A copy-paste that made two
    // refusals identical, or a wording that stopped being reachable, lands
    // here rather than in a bug report.
    for (size_t i = 0; i < wSaid.size(); i++)
        fprintf(stderr, "roistatsselftest: W15 said[%zu] %s\n", i, wSaid[i].c_str());
    // Coverage is over WORDINGS, not over rendered strings: two refusals of the
    // same kind carry different numbers, and the same kind reached twice (from
    // the seam and through the real panel) is one wording. `deny` is how the
    // band gate's two forms are told apart - one is the other plus a clause.
    struct WSay { const char* need; const char* deny; const char* what; };
    const WSay WANT[] = {
        { "not written, the ROI is as it was", nullptr, "rect edit refused" },
        { "or more, so nothing was written",   nullptr, "component is not a rectangle" },
        { "not written, the pin is as it was", nullptr, "pin edit refused" },
        { "bands a ROI against the frame it is on; the rect it remembers is kept",
          nullptr, "band gate, holding a memory" },
        { "bands a ROI against the frame it is on", "remembers",
          "band gate, holding none" },
        { "the x range this band was made from", nullptr, "x band memory refused" },
        { "the y range this band was made from", nullptr, "y band memory refused" },
        { "takes the row / column through the pin", nullptr, "pin band refused" },
        { "past this viewer's 32768 px limit", nullptr, "past the frame-size limit" },
        { "the pixel under the cursor", nullptr, "cursor is on another pane" },
    };
    bool wCovered = true;
    for (const WSay& k : WANT) {
        bool hit = false;
        for (const std::string& t : wSaid)
            if (t.find(k.need) != std::string::npos &&
                (k.deny == nullptr || t.find(k.deny) == std::string::npos))
                hit = true;
        if (!hit) {
            wCovered = false;
            fprintf(stderr, "roistatsselftest: W15 NEVER RUN: %s\n", k.what);
        }
    }
    check(wCovered && wSaid.size() >= 10,
          "W15 every refusal wording this panel can say was run by a test above");
    // the list belongs to whatever runs next in this process: leave it empty,
    // and leave no pin hovered and no mouse button down either
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0;
    app.selectedAnn = 0;
    app.hoverX = app.hoverY = -1;
    closeAll();

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
