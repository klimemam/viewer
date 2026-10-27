// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Inspector/status bar
// ---- --hover-selftest: the three numbers of one hovered sample (#292) ----------
// A sample under the cursor is shown as three numbers - 生値 raw, 表示値 x and
// 画面値 screen - on two surfaces, the Inspector's Pixel table and the status
// bar, and the third of them is the byte the texture holds. The claim under
// test is that the three surfaces come from ONE function (DisplayMap::byteOf
// through hoverReadoutOf): every pixel below asserts
//
//     Pixel table row == status bar numbers == renderDocRGBA's byte
//
// for samples placed at a negative value, below black, exactly black, inside
// the range, exactly white and above white, at gamma 1.0 and at 2.2. The
// equality is what "one function" means once it is a claim rather than a
// comment: a second spelling of the byte in the Inspector goes red here the
// moment gamma is not 1.
//
// Windowless, on histHlSelftest's pattern: fixtures are built in memory, the
// Inspector is driven through real ImGui frames and read back through
// g_pixelProbe (the strings that went to ImGui), the status bar through
// hoverStatusText, and the texture through renderDocRGBA - rebuildTexture is
// that call plus glTexImage2D of the same bytes, and the GL half cannot run
// here. Expected numbers are derived by hand beside each assert; the gamma 2.2
// bytes were checked against a float32 emulation of the sqrt-domain table.
//
// Gamma is PUT BACK at the end: one selftest home is shared by every test.
static int hoverSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "hoverselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto checkEq = [&](const std::string& got, const std::string& want, const char* what) {
        const bool cond = got == want;
        fprintf(stderr, "hoverselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) {
            fprintf(stderr, "hoverselftest:     got  '%s'\n", got.c_str());
            fprintf(stderr, "hoverselftest:     want '%s'\n", want.c_str());
            ok = false;
        }
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
    // The display range is SET, never defaulted: every expected number below is
    // derived from these two by hand. (The default range for signed integers is
    // issue #293 and not this test's subject.)
    auto setRange = [](ImageDoc& d, float b, float w) {
        computeMinMax(d);
        d.black = b; d.white = w;
        d.texDirty = true;
    };
    auto install = [&](std::unique_ptr<ImageDoc> d) {
        closeAll();
        app.images.push_back(std::move(d));
        selectImage(0);
        return app.images[0].get();
    };

    // The Inspector, driven for real - histHlSelftest's frame with drawInspector
    // in it. Two passes: the first frame sizes the window.
    ImGuiIO& io = ImGui::GetIO();
    auto inspectorFrame = [&]() {
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
            ImGui::SetNextWindowSize(ImVec2(420, 900), ImGuiCond_Always);
            ImGui::Begin("HoverProbe", nullptr,
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
            drawInspector();
            ImGui::End();
            ImGui::EndFrame();
        }
    };

    // ---- reading the two surfaces back ----------------------------------
    // The Pixel table: "hdr=<raw>|<x>|<screen>;row=<ch>|<raw>|<x>|<screen>;...".
    struct Row { std::string ch, raw, x, screen; };
    auto split = [](const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t at = 0;
        while (at <= s.size()) {
            const size_t e = s.find(sep, at);
            out.push_back(s.substr(at, e == std::string::npos ? e : e - at));
            if (e == std::string::npos) break;
            at = e + 1;
        }
        return out;
    };
    auto probeRows = [&](const std::string& probe) {
        std::vector<Row> rows;
        for (const std::string& e : split(probe, ';')) {
            if (e.rfind("row=", 0) != 0) continue;
            const std::vector<std::string> f = split(e.substr(4), '|');
            if (f.size() == 4) rows.push_back(Row{ f[0], f[1], f[2], f[3] });
        }
        return rows;
    };
    auto probeHdr = [&](const std::string& probe) {
        for (const std::string& e : split(probe, ';'))
            if (e.rfind("hdr=", 0) == 0) return e.substr(4);
        return std::string();
    };
    // The status bar's hover part: "  |  (x, y) = <raws>  x <xs>  →<screens>".
    // Each group holds every channel, space-separated.
    struct Status { std::string raws, xs, screens; bool hasX = false, hasScreen = false; };
    auto parseStatus = [](const std::string& s) {
        Status st;
        const size_t eq = s.find(" = ");
        if (eq == std::string::npos) return st;
        std::string rest = s.substr(eq + 3);
        const size_t sx = rest.find("  x ");
        if (sx != std::string::npos) {
            st.hasX = true;
            st.raws = rest.substr(0, sx);
            rest = rest.substr(sx + 4);
            const size_t ss = rest.find("  " HOVER_SCREEN_MARK);
            if (ss != std::string::npos) {
                st.hasScreen = true;
                st.xs = rest.substr(0, ss);
                st.screens = rest.substr(ss + 2 + strlen(HOVER_SCREEN_MARK));
            } else st.xs = rest;
        } else st.raws = rest;
        return st;
    };
    auto joined = [](const std::vector<Row>& rows, std::string Row::*m) {
        std::string s;
        for (size_t i = 0; i < rows.size(); i++) s += (i ? " " : "") + rows[i].*m;
        return s;
    };
    auto rgbaAt = [](const std::vector<uint8_t>& px, const ImageDoc& d, int x, int y) {
        const size_t o = ((size_t)y * d.w + x) * 4;
        return std::array<int, 4>{ px[o], px[o + 1], px[o + 2], px[o + 3] };
    };
    // Hover (x, y) on the current document, draw the Inspector, and bring back
    // its rows together with the status bar's text at full width.
    auto hoverAt = [&](ImageDoc& d, int x, int y, std::vector<Row>& rows, std::string& status) {
        app.hoverX = x; app.hoverY = y; app.hoverInB = false;
        inspectorFrame();
        rows = probeRows(g_pixelProbe);
        status = hoverStatusText(d, false, false, x, y, 511);
    };

    closeAll();
    app.compareMode = App::CmpOff;
    app.hoverInB = false;
    const float gamma0 = app.dispGamma;
    app.dispGamma = 1.0f;
    histHlClear();

    // ======================================================================
    // Fixture A: f32, 8 x 1, display range black 100 .. white 1123
    //   span = 1023, inv = 1/1023
    //     x    value    表示値 x = (v - 100)/1023      byte at gamma 1.0
    //     0     -50     -150/1023 = -0.1466   negative      clamp 0  ->   0
    //     1      50      -50/1023 = -0.0489   below black   clamp 0  ->   0
    //     2     100        0/1023 =  0.0000   exactly black          ->   0
    //     3     355.75   255.75/1023 = 0.2500  in range: 0.25*255+0.5 = 64.25 ->  64
    //     4     611.5    511.5/1023 = 0.5000           0.5*255+0.5 = 128.0   -> 128
    //     5    1000       900/1023 = 0.8798         0.8798*255+0.5 = 224.84 -> 224
    //     6    1123      1023/1023 = 1.0000   exactly white          -> 255
    //     7    1500      1400/1023 = 1.3685   above white   clamp 1  -> 255
    //   The three in-range pixels are the ones gamma moves; the five at the
    //   clamp ends are the ones that must NOT move, and are where a formula
    //   that forgot the gamma would still agree - so both kinds are here.
    // ======================================================================
    const float VALS[8] = { -50, 50, 100, 355.75f, 611.5f, 1000, 1123, 1500 };
    const char* RAW[8] = { "-50", "50", "100", "355.75", "611.5", "1000", "1123", "1500" };
    const char* XS[8] = { "-0.1466", "-0.0489", "0.0000", "0.2500", "0.5000", "0.8798",
                          "1.0000", "1.3685" };
    const int BYTE1[8] = { 0, 0, 0, 64, 128, 224, 255, 255 };
    // gamma 2.2 through the sqrt-domain table (float32 emulation):
    //   0.25 -> 0.5325 -> 136.29 -> 136; 0.5 -> 0.7297 -> 186.58 -> 186;
    //   0.8798 -> 0.9434 -> 241.08 -> 241 (within 0.08 of the rounding edge, so
    //   that one is asserted as a bound, not a digit). The ends stay 0 / 255.
    const int BYTE22[8] = { 0, 0, 0, 136, 186, 241, 255, 255 };
    {
        ImageDoc* d = install(mk("hover_a", 8, 1, 1, "f32"));
        for (int x = 0; x < 8; x++) d->px()[x] = VALS[x];
        setRange(*d, 100, 1123);

        // ---- T1: gamma 1.0 - the three surfaces, pixel by pixel ----------
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        std::vector<Row> rows;
        std::string status;
        hoverAt(*d, 0, 0, rows, status);
        checkEq(probeHdr(g_pixelProbe), "raw|norm x|screen",
                "T1 f32 header: raw (no unit) / norm x / screen");
        for (int x = 0; x < 8; x++) {
            hoverAt(*d, x, 0, rows, status);
            char what[128];
            if (rows.size() != 1) {
                snprintf(what, sizeof what, "T1 x=%d: one row for a 1ch document", x);
                check(false, what);
                continue;
            }
            const Row& r = rows[0];
            const std::array<int, 4> b = rgbaAt(px, *d, x, 0);
            // the hand-derived numbers, so the equalities below are not the
            // equalities of three wrong answers
            snprintf(what, sizeof what, "T1 x=%d (%s): Inspector raw is '%s'", x, RAW[x], RAW[x]);
            checkEq(r.raw, RAW[x], what);
            snprintf(what, sizeof what, "T1 x=%d (%s): Inspector norm x is '%s'", x, RAW[x], XS[x]);
            checkEq(r.x, XS[x], what);
            snprintf(what, sizeof what, "T1 x=%d (%s): texture byte is %d", x, RAW[x], BYTE1[x]);
            check(b[0] == BYTE1[x] && b[1] == BYTE1[x] && b[2] == BYTE1[x], what);
            // THE claim: Inspector == status bar == texture
            snprintf(what, sizeof what, "T1 x=%d (%s): Inspector screen == texture byte", x, RAW[x]);
            checkEq(r.screen, std::to_string(b[0]), what);
            const Status st = parseStatus(status);
            snprintf(what, sizeof what, "T1 x=%d (%s): status bar carries x and screen", x, RAW[x]);
            check(st.hasX && st.hasScreen, what);
            snprintf(what, sizeof what, "T1 x=%d (%s): status bar raw == Inspector raw", x, RAW[x]);
            checkEq(st.raws, r.raw, what);
            snprintf(what, sizeof what, "T1 x=%d (%s): status bar x == Inspector x", x, RAW[x]);
            checkEq(st.xs, r.x, what);
            snprintf(what, sizeof what, "T1 x=%d (%s): status bar screen == Inspector screen", x, RAW[x]);
            checkEq(st.screens, r.screen, what);
        }
        // the exact string the status bar shows for the in-range pixel
        hoverAt(*d, 3, 0, rows, status);
        checkEq(status, "  |  (3, 0) = 355.75  x 0.2500  " HOVER_SCREEN_MARK "64",
                "T1 status bar text: '  |  (3, 0) = 355.75  x 0.2500  ->64'");

        // ---- T2: unclamped - the x column says HOW FAR outside ------------
        hoverAt(*d, 0, 0, rows, status);
        check(rows.size() == 1 && rows[0].x[0] == '-' && rows[0].x != "-0.0000",
              "T2 negative sample: norm x is printed negative (not clamped to 0)");
        check(rows.size() == 1 && rows[0].screen == "0",
              "T2 ...while its screen value is the clamped 0");
        hoverAt(*d, 7, 0, rows, status);
        check(rows.size() == 1 && rows[0].x == "1.3685",
              "T2 above white: norm x is printed > 1 (not clamped to 1)");
        check(rows.size() == 1 && rows[0].screen == "255",
              "T2 ...while its screen value is the clamped 255");

        // ---- T3: gamma 2.2 moves the screen value and nothing else -------
        app.dispGamma = 2.2f;
        d->texDirty = true;
        std::vector<uint8_t> px22;
        renderDocRGBA(*d, px22);
        for (int x = 0; x < 8; x++) {
            hoverAt(*d, x, 0, rows, status);
            char what[128];
            if (rows.size() != 1) {
                snprintf(what, sizeof what, "T3 x=%d: one row for a 1ch document", x);
                check(false, what);
                continue;
            }
            const Row& r = rows[0];
            const std::array<int, 4> b = rgbaAt(px22, *d, x, 0);
            const Status st = parseStatus(status);
            snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): raw and x unchanged", x, RAW[x]);
            check(r.raw == RAW[x] && r.x == XS[x] && st.raws == RAW[x] && st.xs == XS[x], what);
            snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): Inspector screen == texture byte", x, RAW[x]);
            checkEq(r.screen, std::to_string(b[0]), what);
            snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): status bar screen == Inspector screen", x, RAW[x]);
            checkEq(st.screens, r.screen, what);
            const bool inRange = x >= 3 && x <= 5;
            snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): byte %s (%d -> %d)", x, RAW[x],
                     inRange ? "moved" : "stayed at the clamp end", BYTE1[x], b[0]);
            check(inRange ? b[0] != BYTE1[x] : b[0] == BYTE1[x], what);
            if (x == 5) {
                snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): byte within 1 of %d", x, RAW[x], BYTE22[x]);
                check(std::abs(b[0] - BYTE22[x]) <= 1, what);
            } else {
                snprintf(what, sizeof what, "T3 g2.2 x=%d (%s): byte is %d (hand-derived)", x, RAW[x], BYTE22[x]);
                check(b[0] == BYTE22[x], what);
            }
        }
        app.dispGamma = 1.0f;
        d->texDirty = true;

        // ---- T5: a short status bar drops screen first, then x, never raw --
        hoverAt(*d, 3, 0, rows, status);
        const std::string full = status;
        const std::string withX = full.substr(0, full.find("  " HOVER_SCREEN_MARK));
        const std::string rawOnly = full.substr(0, full.find("  x "));
        checkEq(hoverStatusText(*d, false, false, 3, 0, full.size()), full,
                "T5 budget == full length: all three");
        checkEq(hoverStatusText(*d, false, false, 3, 0, full.size() - 1), withX,
                "T5 one byte short: the screen value goes, x stays");
        checkEq(hoverStatusText(*d, false, false, 3, 0, withX.size() - 1), rawOnly,
                "T5 shorter still: x goes, the raw value stays");
        checkEq(hoverStatusText(*d, false, false, 3, 0, 0), rawOnly,
                "T5 no room at all: the raw value is still never dropped");
        // and the pane letter in a compare, which the Inspector does not print
        check(hoverStatusText(*d, true, false, 3, 0, 511).rfind("  |  A (3, 0) = ", 0) == 0,
              "T5 compare: pane A is named");
        check(hoverStatusText(*d, true, true, 3, 0, 511).rfind("  |  B (3, 0) = ", 0) == 0,
              "T5 compare: pane B is named when the cursor is in B");
    }

    // ======================================================================
    // Fixture B: u16, 1 x 1, value 1023, range 0 .. 65535 - the issue's own
    // example. x = 1023/65535 = 0.015610 -> "0.0156"; byte = 0.01561*255+0.5
    // = 4.48 -> 4. Integer dtype, so the raw header carries [DN].
    // ======================================================================
    {
        ImageDoc* d = install(mk("hover_b", 1, 1, 1, "u16"));
        d->px()[0] = 1023;
        setRange(*d, 0, 65535);
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        std::vector<Row> rows;
        std::string status;
        hoverAt(*d, 0, 0, rows, status);
        checkEq(probeHdr(g_pixelProbe), "raw [DN]|norm x|screen",
                "T6 u16 header: raw [DN] / norm x / screen");
        checkEq(status, "  |  (0, 0) = 1023  x 0.0156  " HOVER_SCREEN_MARK "4",
                "T6 u16 1023 in 0..65535: '  |  (0, 0) = 1023  x 0.0156  ->4'");
        check(rows.size() == 1 && rows[0].raw == "1023" && rows[0].x == "0.0156" &&
              rows[0].screen == "4" && rgbaAt(px, *d, 0, 0)[0] == 4,
              "T6 ...and the Inspector row and the texture say the same");
    }

    // ======================================================================
    // Fixture C: f32, 1 x 1, 3 channels (100, 611.5, 1500), range 100 .. 1123:
    // one row per input channel, and the status bar's groups hold every
    // channel - raw group, then x group, then screen group. Bytes 0 / 128 / 255
    // land in R / G / B of the texture in that order.
    // ======================================================================
    {
        ImageDoc* d = install(mk("hover_c", 1, 1, 3, "f32"));
        d->px()[0] = 100; d->px()[1] = 611.5f; d->px()[2] = 1500;
        setRange(*d, 100, 1123);
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        std::vector<Row> rows;
        std::string status;
        hoverAt(*d, 0, 0, rows, status);
        check(rows.size() == 3 && rows[0].ch == "R" && rows[1].ch == "G" && rows[2].ch == "B",
              "T7 3ch: three rows, R / G / B");
        const std::array<int, 4> b = rgbaAt(px, *d, 0, 0);
        checkEq(joined(rows, &Row::screen),
                std::to_string(b[0]) + " " + std::to_string(b[1]) + " " + std::to_string(b[2]),
                "T7 3ch: each row's screen value is that channel's texture byte");
        check(b[0] == 0 && b[1] == 128 && b[2] == 255, "T7 3ch: texture bytes are 0 / 128 / 255");
        checkEq(status, "  |  (0, 0) = 100 611.5 1500  x 0.0000 0.5000 1.3685  " HOVER_SCREEN_MARK "0 128 255",
                "T7 3ch status bar: '(0, 0) = 100 611.5 1500  x 0.0000 0.5000 1.3685  ->0 128 255'");
        const Status st = parseStatus(status);
        check(st.raws == joined(rows, &Row::raw) && st.xs == joined(rows, &Row::x) &&
              st.screens == joined(rows, &Row::screen),
              "T7 3ch: status bar groups == Inspector columns, channel by channel");
    }

    // ---- leave the shared home as it was found -------------------------------
    app.hoverX = app.hoverY = -1;
    app.dispGamma = gamma0;
    closeAll();
    fprintf(stderr, "hoverselftest: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
