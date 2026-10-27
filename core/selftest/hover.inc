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
// the range, exactly white and above white, and at NaN, +inf and -inf, at
// gamma 1.0 and at 2.2 (and the non-finite three again under a range whose
// width overflows a float). The
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
    // The status bar's hover part: "  |  (x, y) = <raws>  x <xs>  →<screens>"
    // ("->" in place of the arrow when no CJK face loaded, g_hoverScreenMark).
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
            const size_t ss = rest.find(std::string("  ") + g_hoverScreenMark);
            if (ss != std::string::npos) {
                st.hasScreen = true;
                st.xs = rest.substr(0, ss);
                st.screens = rest.substr(ss + 2 + strlen(g_hoverScreenMark));
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

    // MARK is whatever this run's font load chose; T9 pins the choice itself.
    const std::string MARK = g_hoverScreenMark;

    // ---- T9: the screen-value mark without a CJK face (issue #297 item 3) ----
    // ProggyClean has no U+2192, so the status bar would show "?64".
    checkEq(hoverScreenMarkFor(false), "->", "T9 atlas without U+2192: the screen mark is ASCII '->'");
    checkEq(hoverScreenMarkFor(true), "\xe2\x86\x92", "T9 atlas with U+2192: the screen mark is U+2192");
    // ...and the WIRING: start-up baked this run's atlas (headless too - the
    // font load does not depend on the window), and the mark in use has to be
    // the one that atlas calls for. The glyph is looked up here directly, not
    // through fontAtlasHasArrow, so a bake that forgot to set the mark, or set
    // it from anything but the atlas, disagrees with it. On a machine whose
    // atlas has the arrow this also tells "set" from "left at the '->'
    // default"; on one without, both read "->" and only the inverted wiring
    // goes red.
    {
        ImFontAtlas* fa = ImGui::GetIO().Fonts;
        const bool has = fa->Fonts.Size > 0 &&
                         fa->Fonts[0]->FindGlyphNoFallback((ImWchar)0x2192) != nullptr;
        fprintf(stderr, "hoverselftest: (this run's atlas %s U+2192)\n", has ? "has" : "lacks");
        checkEq(g_hoverScreenMark, has ? "\xe2\x86\x92" : "->",
                "T9 the mark in use is the one the baked atlas calls for");
    }

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
        checkEq(status, "  |  (3, 0) = 355.75  x 0.2500  " + MARK + "64",
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
        const std::string withX = full.substr(0, full.find("  " + MARK));
        const std::string rawOnly = full.substr(0, full.find("  x "));
        checkEq(hoverStatusText(*d, false, false, 3, 0, full.size()), full,
                "T5 budget == full length: all three");
        checkEq(hoverStatusText(*d, false, false, 3, 0, full.size() - 1), withX,
                "T5 one byte short: the screen value goes, x stays");
        checkEq(hoverStatusText(*d, false, false, 3, 0, withX.size() - 1), rawOnly,
                "T5 shorter still: x goes, the raw value stays");
        checkEq(hoverStatusText(*d, false, false, 3, 0, rawOnly.size()), rawOnly,
                "T5 room for exactly the raw value: it is shown whole");
        checkEq(hoverStatusText(*d, false, false, 3, 0, rawOnly.size() - 1), "",
                "T5 one byte short of the raw value: nothing, not a cut number");
        checkEq(hoverStatusText(*d, false, false, 3, 0, 0), "",
                "T5 no room at all: nothing");
        // and the pane letter in a compare, which the Inspector does not print
        check(hoverStatusText(*d, true, false, 3, 0, 511).rfind("  |  A (3, 0) = ", 0) == 0,
              "T5 compare: pane A is named");
        check(hoverStatusText(*d, true, true, 3, 0, 511).rfind("  |  B (3, 0) = ", 0) == 0,
              "T5 compare: pane B is named when the cursor is in B");

        // ---- T10: the SURFACE under a long name (issue #297 item 5) --------
        // statusBarText is what the status bar draws: name + "   8x1 1ch f32"
        // (14 bytes) + the hover part, in 511 bytes. Names of N copies of 画
        // (3 bytes each): at 159 the prefix is 491 bytes and the raw part
        // "  |  (3, 0) = 355.75" (20) fits exactly; at 160 the old code wrote
        // "  |  (3, 0) = 355" - the issue's 355.75 -> 355 - and now writes no
        // hover part; 162 and 166 (whose prefix alone is 512, cut by the
        // snprintf before any hover text) likewise. 150 has room for all three.
        app.hoverX = 3; app.hoverY = 0; app.hoverInB = false;
        const std::string name0 = d->name;
        const int NAMES[5] = { 150, 159, 160, 162, 166 };
        for (int n : NAMES) {
            std::string nm;
            for (int i = 0; i < n; i++) nm += "\xe7\x94\xbb";   // 画
            d->name = nm;
            const std::string s = statusBarText();
            const size_t bar = s.find("  |  ");
            char what[128];
            snprintf(what, sizeof what, "T10 name of %d CJK chars: never '355' without '.75'", n);
            const size_t at = s.find("355");
            check(at == std::string::npos || s.compare(at, 6, "355.75") == 0, what);
            if (n == 150) {
                snprintf(what, sizeof what, "T10 name of %d CJK chars: all three numbers", n);
                check(bar != std::string::npos &&
                      s.substr(bar) == "  |  (3, 0) = 355.75  x 0.2500  " + MARK + "64", what);
            } else if (n == 159) {
                snprintf(what, sizeof what, "T10 name of %d CJK chars: ends with the whole raw '355.75'", n);
                check(bar != std::string::npos && s.substr(bar) == "  |  (3, 0) = 355.75", what);
            } else {
                snprintf(what, sizeof what, "T10 name of %d CJK chars: no hover part at all", n);
                check(bar == std::string::npos, what);
            }
            snprintf(what, sizeof what, "T10 name of %d CJK chars: within the 511-byte bar", n);
            check(s.size() <= 511, what);
        }
        d->name = name0;
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
        checkEq(status, "  |  (0, 0) = 1023  x 0.0156  " + MARK + "4",
                "T6 u16 1023 in 0..65535: '  |  (0, 0) = 1023  x 0.0156  ->4'");
        check(rows.size() == 1 && rows[0].raw == "1023" && rows[0].x == "0.0156" &&
              rows[0].screen == "4" && rgbaAt(px, *d, 0, 0)[0] == 4,
              "T6 ...and the Inspector row and the texture say the same");

        // ---- T11: the same pixel through a display LUT (issue #297 item 4) --
        // The display plugins are loaded under --no-window too
        // (display_falsecolor is part of the build), so the colormap path runs
        // here: the header says "screen idx", the index is still byteOf's 4,
        // the status bar says the same index, and the texture holds the LUT's
        // RGB at that index - not the gray byte.
        const auto& disp = plugin_host::displays();
        check(!disp.empty(), "T11 a display plugin is loaded in this run");
        if (!disp.empty()) {
            d->displayLut = 0;
            d->texDirty = true;
            std::vector<uint8_t> pxl;
            renderDocRGBA(*d, pxl);
            hoverAt(*d, 0, 0, rows, status);
            checkEq(probeHdr(g_pixelProbe), "raw [DN]|norm x|screen idx",
                    "T11 LUT: header is raw [DN] / norm x / screen idx");
            check(rows.size() == 1 && rows[0].screen == "4",
                  "T11 LUT: the Inspector's screen idx is byteOf's 4");
            checkEq(parseStatus(status).screens, "4", "T11 LUT: the status bar's screen value is the same index");
            const std::array<int, 4> c = rgbaAt(pxl, *d, 0, 0);
            const auto& lut = disp[0].lut;
            check(c[0] == lut[4 * 3] && c[1] == lut[4 * 3 + 1] && c[2] == lut[4 * 3 + 2] && c[3] == 255,
                  "T11 LUT: texture rgba == lut[idx*3 ..] at the index the table shows");
            d->displayLut = -1;
            d->texDirty = true;
        }
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
        checkEq(status, "  |  (0, 0) = 100 611.5 1500  x 0.0000 0.5000 1.3685  " + MARK + "0 128 255",
                "T7 3ch status bar: '(0, 0) = 100 611.5 1500  x 0.0000 0.5000 1.3685  ->0 128 255'");
        const Status st = parseStatus(status);
        check(st.raws == joined(rows, &Row::raw) && st.xs == joined(rows, &Row::x) &&
              st.screens == joined(rows, &Row::screen),
              "T7 3ch: status bar groups == Inspector columns, channel by channel");
    }

    // ======================================================================
    // Fixture D: f32, 3 x 1, (NaN, +inf, -inf), range 100 .. 1123 - the
    // non-finite samples (issue #296). The contract is byteOf's: a NaN sample
    // is screen 0 at EVERY gamma, and +-inf are the clamp ends, 255 / 0. The
    // other two columns hide none of it: fmtVal prints nan / inf / -inf, and
    // x = (NaN - 100) * inv is NaN, printed "nan" - the column is unclamped on
    // purpose, and a NaN shown as 0.0000 would be a lie about the sample.
    // Spelling: the fixture's NaN is quiet_NaN() with the sign bit clear, and
    // the subtraction and the multiply keep the sign, so every C library prints
    // it "nan" (a sign-bit NaN prints "-nan" or "-nan(ind)" depending on the
    // library; no fixture here has one).
    // Gamma 2.2 is the half that used to kill the process: byteOf's (int) of
    // NaN indexed the gamma table at INT_MIN, so the renderDocRGBA below
    // segfaulted before any assert ran. On x86-64, surviving IS the first
    // assert (elsewhere the out-of-range read need not fault); the second,
    // which holds everywhere, is that all three surfaces then say 0.
    // ======================================================================
    {
        ImageDoc* d = install(mk("hover_d", 3, 1, 1, "f32"));
        d->px()[0] = std::numeric_limits<float>::quiet_NaN();
        d->px()[1] = std::numeric_limits<float>::infinity();
        d->px()[2] = -std::numeric_limits<float>::infinity();
        setRange(*d, 100, 1123);
        const char* RAWD[3] = { "nan", "inf", "-inf" };
        const char* XD[3] = { "nan", "inf", "-inf" };
        const int BYTED[3] = { 0, 255, 0 };
        std::vector<Row> rows;
        std::string status;
        for (int pass = 0; pass < 2; pass++) {
            const float gamma = pass ? 2.2f : 1.0f;
            app.dispGamma = gamma;
            d->texDirty = true;
            std::vector<uint8_t> px;
            renderDocRGBA(*d, px);
            for (int x = 0; x < 3; x++) {
                hoverAt(*d, x, 0, rows, status);
                char what[128];
                if (rows.size() != 1) {
                    snprintf(what, sizeof what, "T8 g%.1f x=%d: one row for a 1ch document", gamma, x);
                    check(false, what);
                    continue;
                }
                const Row& r = rows[0];
                const std::array<int, 4> b = rgbaAt(px, *d, x, 0);
                const Status st = parseStatus(status);
                snprintf(what, sizeof what, "T8 g%.1f x=%d (%s): Inspector raw is '%s'", gamma, x, RAWD[x], RAWD[x]);
                checkEq(r.raw, RAWD[x], what);
                snprintf(what, sizeof what, "T8 g%.1f x=%d (%s): Inspector norm x is '%s' (not clamped, not hidden)",
                         gamma, x, RAWD[x], XD[x]);
                checkEq(r.x, XD[x], what);
                snprintf(what, sizeof what, "T8 g%.1f x=%d (%s): texture byte is %d", gamma, x, RAWD[x], BYTED[x]);
                check(b[0] == BYTED[x] && b[1] == BYTED[x] && b[2] == BYTED[x] && b[3] == 255, what);
                snprintf(what, sizeof what, "T8 g%.1f x=%d (%s): Inspector screen == texture byte", gamma, x, RAWD[x]);
                checkEq(r.screen, std::to_string(b[0]), what);
                snprintf(what, sizeof what, "T8 g%.1f x=%d (%s): status bar raw / x / screen == Inspector", gamma, x, RAWD[x]);
                check(st.hasX && st.hasScreen && st.raws == r.raw && st.xs == r.x && st.screens == r.screen, what);
            }
        }
        // the exact status bar text for the NaN sample, at the gamma that crashed
        hoverAt(*d, 0, 0, rows, status);
        checkEq(status, "  |  (0, 0) = nan  x nan  " + MARK + "0",
                "T8 g2.2 status bar text for NaN: '  |  (0, 0) = nan  x nan  ->0'");
        app.dispGamma = 1.0f;
        d->texDirty = true;

        // ---- T12: ranges too wide for a float (issue #297 item 10) -------
        // black -3e38 / white 3e38: the float width is +inf, and inv = 1/inf
        // = 0 made x of +inf inf * 0 = NaN, i.e. screen 0. displayMapOf takes
        // that width in double, so inv is a small non-zero float and +inf is
        // the clamp end 255 again; NaN stays 0 and -inf 0.
        // black -3e38 / white +inf: the width is non-finite in double too, and
        // is held at 2 * FLT_MAX - without that, inv is 0 again and +inf reads
        // 0. Both ranges, the same three samples, the same three answers.
        struct Wide { float black, white; const char* name; };
        const Wide WIDE[2] = { { -3e38f, 3e38f, "-3e38..3e38" },
                               { -3e38f, std::numeric_limits<float>::infinity(), "-3e38..+inf" } };
        for (const Wide& wr : WIDE) {
            setRange(*d, wr.black, wr.white);
            char what[128];
            snprintf(what, sizeof what, "T12 range %s: inv is finite and non-zero", wr.name);
            check(std::isfinite(displayMapOf(*d).inv) && displayMapOf(*d).inv > 0, what);
            std::vector<uint8_t> pxw;
            renderDocRGBA(*d, pxw);
            const char* NAMEW[3] = { "NaN", "+inf", "-inf" };
            const int BYTEW[3] = { 0, 255, 0 };
            for (int x = 0; x < 3; x++) {
                hoverAt(*d, x, 0, rows, status);
                const std::array<int, 4> b = rgbaAt(pxw, *d, x, 0);
                snprintf(what, sizeof what, "T12 range %s x=%d (%s): texture byte is %d",
                         wr.name, x, NAMEW[x], BYTEW[x]);
                check(b[0] == BYTEW[x] && b[1] == BYTEW[x] && b[2] == BYTEW[x], what);
                snprintf(what, sizeof what, "T12 range %s x=%d (%s): Inspector and status bar say %d",
                         wr.name, x, NAMEW[x], BYTEW[x]);
                check(rows.size() == 1 && rows[0].screen == std::to_string(BYTEW[x]) &&
                      parseStatus(status).screens == rows[0].screen, what);
            }
        }
    }

    // ======================================================================
    // T15: ORDINARY float ranges keep main's bytes bit for bit (PR #303
    // review P2). The wide-range fix must not touch a finite float width: a
    // double 1/width differs from the float 1.0f/width by an ulp whenever the
    // float subtraction is inexact, and that ulp moves bytes at rounding
    // edges. Two such edges, checked against a float32 emulation of both
    // expressions (float inv / double inv):
    //   black 0.1, white 0.9, v 0.107843138      ->  3  (double inv: 2)
    //   black 4/255, white 14/255, v 5/255        -> 26  (double inv: 25)
    // ======================================================================
    {
        ImageDoc* d = install(mk("hover_ord", 1, 1, 1, "f32"));
        struct Ord { float black, white, v; int byte; const char* name; };
        const Ord ORD[2] = { { 0.1f, 0.9f, 0.107843138f, 3, "0.1..0.9 v=0.107843138" },
                             { 4.0f / 255.0f, 14.0f / 255.0f, 5.0f / 255.0f, 26, "4/255..14/255 v=5/255" } };
        std::vector<Row> rows;
        std::string status;
        for (const Ord& o : ORD) {
            d->px()[0] = o.v;
            setRange(*d, o.black, o.white);
            std::vector<uint8_t> px;
            renderDocRGBA(*d, px);
            hoverAt(*d, 0, 0, rows, status);
            char what[128];
            snprintf(what, sizeof what, "T15 ordinary range %s: texture byte is %d", o.name, o.byte);
            check(rgbaAt(px, *d, 0, 0)[0] == o.byte, what);
            snprintf(what, sizeof what, "T15 ordinary range %s: Inspector screen is %d", o.name, o.byte);
            check(rows.size() == 1 && rows[0].screen == std::to_string(o.byte), what);
        }
    }

    // ======================================================================
    // Fixture E: the difference view's ramp (issue #297 item 9). A and B are
    // f32 3 x 1 with A-B = +5, -5, +5 at gain 10, so t = +0.5, -0.5 and (abs
    // mode) 0.5. Both sides read the mode from app.diffAbs, which the test sets
    // (and puts back). The texture's bytes come from renderDiffRGBA -
    // ensureDiffTexture is that plus glTexImage2D - and the colour bar's from
    // diffBarColor at
    // the column that stands for the same t: barW 180, so column 135 is
    // 135/180*2-1 = +0.5, column 45 is -0.5 and, in abs mode, column 90 is
    // 0.5 - all exact in float. Hand-derived: +0.5 -> (128, 45, 13),
    // -0.5 -> (13, 57, 128), abs 0.5 -> (128, 128, 128).
    // ======================================================================
    {
        auto A = mk("hover_diff_a", 3, 1, 1, "f32");
        auto B = mk("hover_diff_b", 3, 1, 1, "f32");
        A->px()[0] = 15; B->px()[0] = 10;
        A->px()[1] = 10; B->px()[1] = 15;
        A->px()[2] = 15; B->px()[2] = 10;
        const bool abs0 = app.diffAbs;
        std::vector<uint8_t> sgn, ab;
        app.diffAbs = false;
        renderDiffRGBA(*A, *B, 3, 1, 1, 10.0f, sgn);
        app.diffAbs = true;
        renderDiffRGBA(*A, *B, 3, 1, 1, 10.0f, ab);
        struct Case { const char* what; const std::vector<uint8_t>* px; int x, col; bool absMode; int r, g, b; };
        const Case cases[3] = {
            { "t=+0.5", &sgn, 0, 135, false, 128, 45, 13 },
            { "t=-0.5", &sgn, 1, 45, false, 13, 57, 128 },
            { "abs 0.5", &ab, 2, 90, true, 128, 128, 128 },
        };
        for (const Case& k : cases) {
            const size_t o = (size_t)k.x * 4;
            const int tr = (*k.px)[o], tg = (*k.px)[o + 1], tb = (*k.px)[o + 2];
            app.diffAbs = k.absMode;
            const DiffRGB bar = diffBarColor(k.col, 180.0f);
            char what[128];
            snprintf(what, sizeof what, "T13 diff %s: texture is (%d, %d, %d)", k.what, k.r, k.g, k.b);
            check(tr == k.r && tg == k.g && tb == k.b, what);
            snprintf(what, sizeof what, "T13 diff %s: colour bar == texture (%d, %d, %d) vs (%d, %d, %d)",
                     k.what, bar.r, bar.g, bar.b, tr, tg, tb);
            check(bar.r == tr && bar.g == tg && bar.b == tb, what);
        }
        app.diffAbs = abs0;
    }

    // ======================================================================
    // Fixture F: CFA colorize, the one place DisplayMap::cfaColor is decided
    // (issue #297 item 8). f32 2 x 1 Bayer, both samples 611.5 in 100 .. 1123
    // (byte 128), colorize on AND a display LUT picked: colorize wins, so the
    // map has no colormap and each pixel's byte lands in its own CFA channel
    // only - the others get byteOf(black) = 0.
    // ======================================================================
    {
        ImageDoc* d = install(mk("hover_cfa", 2, 1, 1, "f32"));
        d->px()[0] = 611.5f; d->px()[1] = 611.5f;
        d->cfa = 1; d->cfaPattern = 0; d->cfaColorize = true;
        if (!plugin_host::displays().empty()) d->displayLut = 0;
        setRange(*d, 100, 1123);
        const DisplayMap M = displayMapOf(*d);
        check(M.cfaColor && M.colormap == nullptr,
              "T14 CFA colorize: the map says cfaColor, and no colormap even with a LUT picked");
        std::vector<uint8_t> px;
        renderDocRGBA(*d, px);
        for (int x = 0; x < 2; x++) {
            const int c = cfaChannelAt(*d, x, 0);
            const int wr = c == 0 ? 128 : 0, wg = (c == 1 || c == 2) ? 128 : 0, wb = c == 3 ? 128 : 0;
            const std::array<int, 4> b = rgbaAt(px, *d, x, 0);
            char what[128];
            snprintf(what, sizeof what, "T14 CFA colorize x=%d [%s]: texture (%d, %d, %d)",
                     x, CFA_CH_NAMES[c], wr, wg, wb);
            check(b[0] == wr && b[1] == wg && b[2] == wb, what);
        }
        // ...and colorize is a 1-channel thing: a 3-channel document with the
        // same CFA flags is plain RGB, each channel its own byte (0 / 128 /
        // 255 for 100 / 611.5 / 1500), not the first sample tinted.
        ImageDoc* d3 = install(mk("hover_cfa3", 1, 1, 3, "f32"));
        d3->px()[0] = 100; d3->px()[1] = 611.5f; d3->px()[2] = 1500;
        d3->cfa = 1; d3->cfaPattern = 0; d3->cfaColorize = true;
        setRange(*d3, 100, 1123);
        check(!displayMapOf(*d3).cfaColor, "T14 CFA flags on a 3ch document: cfaColor is false");
        std::vector<uint8_t> px3;
        renderDocRGBA(*d3, px3);
        const std::array<int, 4> b3 = rgbaAt(px3, *d3, 0, 0);
        check(b3[0] == 0 && b3[1] == 128 && b3[2] == 255,
              "T14 CFA flags on a 3ch document: texture is plain RGB (0, 128, 255)");
    }

    // ---- leave the shared home as it was found -------------------------------
    app.hoverX = app.hoverY = -1;
    app.dispGamma = gamma0;
    closeAll();
    fprintf(stderr, "hoverselftest: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
