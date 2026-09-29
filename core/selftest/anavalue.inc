// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --anavalue-selftest: the three shipped analyzers that shipped with no
// ---- numeric assertion at all (板 304) ---------------------------------------
//
// docs/background/analysis/measurement-kernel-inventory.md §4.2 counted four
// shipped measurements with NO value assertion anywhere in the tree:
// uniformity/prnu-fpn, sharpness/gradient, iso12233/e-sfr and the Temporal
// per-frame TSV. --bundled-selftest fixes their key list and their version and
// says so itself: "a test that hard-coded [a value] would be asserting today's
// arithmetic". That is true of the fixtures IT uses - pseudo-random flats whose
// right answer nobody can write down. This file closes the hole from the other
// side: fixtures whose answer IS writable, with the derivation beside the
// assert. The per-frame TSV is the fourth and lives in
// core/selftest/framestats.inc, beside the table it prints.
//
// Nothing here touches stdout. --bundled-selftest's stdout is a byte-diff
// contract across commits and this test runs the same dlls on other fixtures;
// a line printed here would land in nobody's diff and could only confuse that
// one.
//
// NOGL and fixture-free for --bundled-selftest's reasons: the documents are
// analytic and in memory, and every claim is a number off the ABI.
if (g_anaValueSelftest) {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "anavalueselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // Relative to the expectation. The analyzers accumulate in double but write
    // their intermediate planes as FLOAT (boxblur2d, boxblur1d, the luma and ESF
    // planes), so an ulp at ~1000 DN is 6.1e-5 and nothing here can be pinned
    // tighter than that: every tolerance below names which float plane sets it.
    auto rel = [](double a, double b, double tol) {
        return std::isfinite(a) && std::fabs(a - b) <= tol * std::fabs(b);
    };

    // One analyzer, one frame, the rows it emitted - through bunRunRaw, which is
    // the same dispatch panel_analysis.inc / serve.cpp / cli.inc use.
    struct Res {
        std::map<std::string, double> num;
        std::map<std::string, std::string> txt;
        std::vector<float> sfrX, sfrY;
        int32_t rc = -1;
        std::string err;
        bool has(const char* k) const { return num.count(k) != 0; }
        double at(const char* k) const {
            auto it = num.find(k);
            return it == num.end() ? std::numeric_limits<double>::quiet_NaN() : it->second;
        }
    };
    auto run = [&](const char* which, ImageDoc& d, const psRect* roi) {
        Res R;
        const AnalyzerPluginInfo* a = nullptr;
        for (const auto& c : plugin_host::analyzers()) if (c.name == which) a = &c;
        if (!a) { R.err = "analyzer not loaded"; return R; }
        std::vector<std::pair<std::string, std::string>> rows;
        std::vector<std::string> series;
        g_bunRows = &rows; g_bunSeries = &series;
        psFrame fr = makeFrame(d);
        char perr[256] = { 0 };
        R.rc = bunRunRaw(*a, &fr, roi, perr, sizeof perr);
        g_bunRows = nullptr; g_bunSeries = nullptr;
        R.err = perr;
        for (const auto& kv : rows) {
            // bunNum writes %.17g, bunTxt writes the text: a cell that parses to
            // a finite number AND round-trips is the number, anything else is
            // text. (No analyzer here emits a numeric-looking string.)
            char* end = nullptr;
            const double v = strtod(kv.second.c_str(), &end);
            if (end && *end == '\0' && !kv.second.empty() && std::isfinite(v))
                R.num[kv.first] = v;
            else
                R.txt[kv.first] = kv.second;
        }
        // "series sfr\tcycles/px\tSFR\t101" then "  [i] x y" per point
        for (const auto& s : series) {
            if (s.compare(0, 3, "  [") == 0) {
                double xx = 0, yy = 0;
                unsigned idx = 0;
                if (sscanf(s.c_str(), "  [%u] %lf %lf", &idx, &xx, &yy) == 3) {
                    R.sfrX.push_back((float)xx);
                    R.sfrY.push_back((float)yy);
                }
            }
        }
        return R;
    };
    auto mk = [](const char* nm, int W, int H, int chn) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = H; d->src->ch = chn;
        d->src->dtype = "f32";
        d->src->data.assign((size_t)W * H * chn, 0.0f);
        d->syncMirrors();
        d->uid = app.nextUid++;
        return d;
    };

    // The 9-tap box blur (RADIUS 4) with edge clamp is the only shape in either
    // uniformity analyzer, and over a LINEAR ramp its effect is exactly four
    // numbers at each end and nothing in between:
    //   blur(i) - i = +(10, 6, 3, 1)/9 at i = 0..3, 0 in the interior,
    //               -(1, 3, 6, 10)/9 at the last four.
    // Derivation for i = 0: the taps are indices (0,0,0,0,0,1,2,3,4), so the
    // average is (5*0 + 1+2+3+4)/9 = 10/9. i = 1: (4*0 + 1..5)/9 = 15/9, minus
    // 1 is 6/9. i = 2: (3*0 + 1..6)/9 = 21/9, minus 2 is 3/9. i = 3:
    // (2*0 + 1..7)/9 = 28/9, minus 3 is 1/9. i = 4: (0 + 1..8)/9 = 36/9 = 4.
    // Hence sum over the line of (blur - i)^2 = 2*(100+36+9+1)/81 = 292/81, and
    // EVERY expectation in the uniformity block below is built from that one
    // number. It needs n >= 10 so the two edge zones do not overlap.
    const double E2 = 292.0 / 81;               // sum of the squared corrections

    // =====================================================================
    // uniformity/prnu-fpn  -  a bilinear ramp is a flat field with a known
    // shading, a known banding and a known high-pass residual.
    //   20x16, v(x,y) = 1000 + x + 3y   (every pixel an exact integer)
    //   mean = 1000 + 19/2 + 3*15/2 = 1032 exactly
    //   prnu_pct: the 2D blur of an ADDITIVE separable ramp is separable too
    //     (the horizontal pass leaves 3y alone, the vertical pass leaves the
    //     blurred x alone), so the residual is -(g*ex(x) + h*ey(y)). ex and ey
    //     each sum to zero, so the cross term drops and
    //       mean(d^2) = g^2*E2/W + h^2*E2/H = (292/81)(1/20 + 9/16) = 2.2080247
    //     prnu_pct = 100*sqrt(2.2080247)/1032 = 0.143986663
    //   shading_pct: the low band is the ramp with its ends pulled in by 10/9,
    //     so pp = g(W-1-20/9) + h(H-1-20/9) = 55.111111
    //     shading_pct = 100*55.111111/1032 = 5.34022394
    //   row_fpn_pct / col_fpn_pct: the row means are a ramp of slope 3 over 16
    //     lines and the column means a ramp of slope 1 over 20, and the plugin
    //     detrends with the SAME 9-tap box, so
    //       fpn = (slope/9)*sqrt(292/(n-1))
    //     row: (3/9)*sqrt(292/15) = 1.4707015 -> 0.142509837 %
    //     col: (1/9)*sqrt(292/19) = 0.4354983 -> 0.042207771 %
    // =====================================================================
    {
        const int W = 20, H = 16;
        auto d = mk("av_flat", W, H, 1);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                d->px()[(size_t)y * W + x] = (float)(1000 + x + 3 * y);
        computeMinMax(*d);
        const Res R = run("uniformity/prnu-fpn", *d, nullptr);
        const double M = 1032.0;
        const double PRNU = std::sqrt(E2 * (1.0 / W + 9.0 / H)) / M * 100.0;
        const double SHAD = (1.0 * (W - 1 - 20.0 / 9) + 3.0 * (H - 1 - 20.0 / 9)) / M * 100.0;
        const double RFPN = 3.0 / 9 * std::sqrt(292.0 / (H - 1)) / M * 100.0;
        const double CFPN = 1.0 / 9 * std::sqrt(292.0 / (W - 1)) / M * 100.0;
        fprintf(stderr, "anavalueselftest: prnu-fpn got mean=%.12g prnu=%.12g shading=%.12g "
                        "row=%.12g col=%.12g\n",
                R.at("ch0.mean"), R.at("ch0.prnu_pct"), R.at("ch0.shading_pct"),
                R.at("ch0.row_fpn_pct"), R.at("ch0.col_fpn_pct"));
        fprintf(stderr, "anavalueselftest: prnu-fpn want mean=1032 prnu=%.12g shading=%.12g "
                        "row=%.12g col=%.12g\n", PRNU, SHAD, RFPN, CFPN);
        check(R.rc == 0 && R.has("ch0.prnu_pct"), "U1 uniformity/prnu-fpn ran on the ramp");
        check(R.at("ch0.mean") == M, "U1 mean is 1032 exactly (the ramp's own average)");
        // The blur writes FLOAT, so the residual carries ~1 ulp of 1000 = 6.1e-5
        // against a d of order 1: 1e-4 relative is the floor here, not a guess.
        check(rel(R.at("ch0.prnu_pct"), PRNU, 1e-4),
              "U1 prnu_pct = 100*sqrt((292/81)(1/20+9/16))/1032 = 0.143986663");
        // 1e-5 and not 1e-6: the low band is stored as FLOAT, so lpMin and lpMax
        // each carry up to half an ulp of ~1000 (3.1e-5 DN) and their difference
        // up to one ulp. Measured residual is 6.2e-7, which is 40% of a 1e-6
        // bound - too little room for a runner whose libm rounds the blur's
        // divisions the other way.
        check(rel(R.at("ch0.shading_pct"), SHAD, 1e-5),
              "U1 shading_pct = 100*(g(W-1-20/9)+h(H-1-20/9))/1032 = 5.34022394");
        check(rel(R.at("ch0.row_fpn_pct"), RFPN, 1e-4) &&
              rel(R.at("ch0.col_fpn_pct"), CFPN, 1e-4),
              "U1 row_fpn_pct = 0.142509837 %, col_fpn_pct = 0.042207771 %");
        // Anti-vacuity: the four numbers are FOUR numbers. Nothing above could
        // pass by the plugin emitting one value under four keys.
        check(R.at("ch0.prnu_pct") != R.at("ch0.row_fpn_pct") &&
              R.at("ch0.row_fpn_pct") != R.at("ch0.col_fpn_pct") &&
              rel(R.at("ch0.row_fpn_pct") / R.at("ch0.col_fpn_pct"),
                  3.0 * std::sqrt(19.0 / 15), 1e-4),
              "U1 ...and row/col = 3*sqrt(19/15), the slope ratio times the lengths");

        // A flat field is DEFINED by its mean being a level, so a plane at 0 is
        // refused in words rather than divided by.
        auto z = mk("av_zero", W, H, 1);
        const Res Z = run("uniformity/prnu-fpn", *z, nullptr);
        check(Z.rc == 0 && Z.at("ch0.mean") == 0.0 &&
              Z.txt.count("ch0") && Z.txt.at("ch0") == "mean <= 0: not a flat field?",
              "U2 a zero plane is declined in words, not divided by");
        check(!Z.has("ch0.prnu_pct") && !Z.has("ch0.row_fpn_pct"),
              "U2 ...and emits none of the four percentages");
    }

    // =====================================================================
    // sharpness/gradient  -  two fixtures, each of which is the other's
    // anti-vacuity guard, because each sends one of the three numbers to
    // exactly zero.
    //
    // RAMP v = 100 + g*x, no y dependence:
    //   Laplacian = 4v(x,y) - v(x-1,y) - v(x+1,y) - v(x,y-1) - v(x,y+1)
    //             = 4(100+gx) - (100+gx-g) - (100+gx+g) - 2(100+gx) = 0
    //   so varlap = 0 EXACTLY, for every g.
    //   Sobel gx = 4v(x+1) - 4v(x-1) = 8g   (the three rows are equal)
    //   Sobel gy = 0                        (no y dependence)
    //   tenengrad = mean(gx^2+gy^2) = 64g^2 ; grad_mean = mean|g| = 8|g|
    // CHECKERBOARD v = 100 + a*(-1)^(x+y):
    //   Laplacian = 4(100+as) - 4(100-as) = 8as, s = +-1, so lap^2 = 64a^2
    //   everywhere; the interior of a 16x16 is 14x14 with 7 of each sign per
    //   row, so the MEAN lap is 0 exactly and varlap = 64a^2 exactly.
    //   Sobel x: (r0+2r1+r2)[x+1] = (100+as) + 2(100-as) + (100+as) = 400, and
    //   the same at x-1, so gx = 0; gy = 0 by the same cancellation.
    //   tenengrad = grad_mean = 0 EXACTLY.
    // =====================================================================
    {
        auto ramp = [&](int g) {
            auto d = mk("av_ramp", 16, 16, 1);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    d->px()[(size_t)y * 16 + x] = (float)(100 + g * x);
            return d;
        };
        auto d1 = ramp(1);
        const Res R1 = run("sharpness/gradient", *d1, nullptr);
        auto d2 = ramp(3);
        const Res R2 = run("sharpness/gradient", *d2, nullptr);
        auto chk = mk("av_check", 16, 16, 1);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                chk->px()[(size_t)y * 16 + x] = (float)(100 + (((x + y) & 1) ? 1 : -1));
        const Res RC = run("sharpness/gradient", *chk, nullptr);
        fprintf(stderr, "anavalueselftest: sharpness ramp g=1 varlap=%.17g tenengrad=%.17g "
                        "grad_mean=%.17g\n",
                R1.at("varlap"), R1.at("tenengrad"), R1.at("grad_mean"));
        fprintf(stderr, "anavalueselftest: sharpness g=3 tenengrad=%.17g grad_mean=%.17g  "
                        "checker varlap=%.17g tg=%.17g gm=%.17g\n",
                R2.at("tenengrad"), R2.at("grad_mean"), RC.at("varlap"),
                RC.at("tenengrad"), RC.at("grad_mean"));
        // Integers times integers: the ramp's Sobel is 8 and 24 with no rounding
        // anywhere, so these are ==, not tolerances.
        check(R1.rc == 0 && R1.at("varlap") == 0.0,
              "G1 a ramp has NO Laplacian: varlap is 0 exactly");
        check(R1.at("tenengrad") == 64.0 && R1.at("grad_mean") == 8.0,
              "G1 ...and a Sobel of 8g: tenengrad 64, grad_mean 8 at g=1");
        check(R2.at("tenengrad") == 576.0 && R2.at("grad_mean") == 24.0,
              "G2 slope 3: tenengrad 64*9 = 576 and grad_mean 8*3 = 24 (scaling)");
        check(RC.rc == 0 && RC.at("varlap") == 64.0,
              "G3 a +-1 checkerboard: lap is +-8 everywhere, varlap = 64 exactly");
        check(RC.at("tenengrad") == 0.0 && RC.at("grad_mean") == 0.0,
              "G3 ...and its Sobel cancels to 0 - the ramp's non-zero, inverted");
        // ...so the two fixtures together say all three numbers are computed:
        // neither could be a constant, and neither could be the other.
        check(R1.at("varlap") != RC.at("varlap") &&
              R1.at("tenengrad") != RC.at("tenengrad"),
              "G3 ...which is the anti-vacuity: each fixture zeroes what the other does not");
        // NOT `small`: MSVC's rpcndr.h has `#define small char`, so a local of
        // that name is three compile errors on one runner and none on the other
        // two (the same trap as #253 and #261).
        auto tooSmall = mk("av_tiny", 2, 9, 1);
        const Res RS = run("sharpness/gradient", *tooSmall, nullptr);
        check(RS.rc == 1 && RS.err == "sharpness: region too small (need >= 3x3)",
              "G4 under 3x3 it refuses in words rather than reporting 0");
    }

    // =====================================================================
    // iso12233/e-sfr  -  an IDEAL edge, whose MTF is a sinc.
    //
    // 96x64 f32, edge at 24 + 0.15y, and each pixel holds the FRACTION of
    // itself the bright side covers (clamped 0..1) times 800, with a dark level
    // of ZERO. The dark level is load-bearing and the reason is DEFECT(esfr-1)
    // at the foot of this group: a non-zero one puts a false impulse of its own
    // height into the LSF, because an empty leading bin is filled with 0 rather
    // than with the dark level (analyzer_esfr.c:150-158). At dark 0 that step
    // does not exist and the measurement follows the model below to 0.6% at
    // every f up to 0.85; at dark 100 the same curve ripples -5.6% to -10.1%
    // against it. This was measured by three independent Python ports of
    // analyzer_esfr.c during review of PR #273.
    //
    // 0 + 800*t is the exact integral of a step over a 1 px aperture, so the
    // line spread function is a box of width exactly 1 px and
    //     MTF(f) = |sinc(f)| = |sin(pi f)/(pi f)|      (0 at f = 1 cy/px)
    // The plugin measures the LSF as the difference of consecutive 4x bins,
    // which is a 0.25 px difference rather than a derivative, and normalizes by
    // the total rise. Writing the measured spectrum out:
    //     sum_i [E(p_i) - E(p_i-1)] e^-2pi j f p_i
    //        = (1 - e^-2pi j f/4) * sum_i E(p_i) e^-2pi j f p_i
    //   and |1 - e^-i0| / (2 pi f / 4 * ...) leaves exactly ONE extra factor
    //     sin(pi f/4)/(pi f/4) = sinc(f/4)
    // over the true MTF once the DC normalization is taken out. The projection
    // also AVERAGES the pixels that land in a 0.25 px bin, which is a SECOND box
    // of that width, so the model this test asserts against is
    //     SFR_model(f) = |sinc(f)| * sinc(f/4)^2
    //   sinc(0.5) = 2/pi = 0.63661977, sinc(0.125) = 0.97449594
    //   SFR_model(0.33) = 0.81182562     SFR_model(0.50) = 0.60456037
    //   its 0.5 crossing = 0.57694593    its 0.2 crossing = 0.80386988
    // and |sinc(f)| is used below as a one-sided UPPER bound. NOT as a theorem:
    // the Hamming window is a multiplication in space, so it is a CONVOLUTION in
    // frequency and can move magnitude upward at a given f. What makes the bound
    // safe HERE is the fixture: the window's half-width is pw*OS/2 = 192 bins =
    // 48 px around the LSF centroid and the LSF is 1 px wide, so over the support
    // that carries the energy the window is ~1 and contributes no lift. Read it
    // as "on this fixture nothing may exceed the ideal sinc", which still catches
    // a sharpening, an aliasing or a broken normalization; it is not a statement
    // about the pipeline in general. Ideal roots:
    //   |sinc(f)| = 0.5 at f = 0.60335456 ; = 0.2 at f = 0.82624941
    // With dark 0 the model is within 0.6% of the measurement at every f up to
    // 0.85, and the two CROSSINGS land within about 0.25%, so the asserts below
    // are 1% on the curve and 1% on the crossings. The residual is the binning
    // and the per-line edge-position scatter, not the missing sinc correction -
    // the sinc correction the file documents as absent would move the curve far
    // more than this and is already accounted for in the sinc(f/4)^2 factor.
    // Two numbers need no model at all:
    //   edge_angle_deg = atan(0.15)*180/pi = 8.5307656  (+-2e-4: the per-line
    //     centroid of the SQUARED derivative has a subpixel-phase bias that is
    //     periodic in y with period 1/0.15 lines, so the fitted slope comes out
    //     0.150019 rather than 0.15)
    //   lines_used = 64 - every line of the ROI has the edge in it
    // =====================================================================
    {
        const int W = 96, H = 64;
        // `dark` is a parameter and not a constant so that DEFECT(esfr-1) can
        // run the SAME edge at 100 and print what it does to mtf50.
        auto mkEdge = [&](const char* nm, int blur, double dark) {
            auto d = mk(nm, W, H, 1);
            for (int y = 0; y < H; y++) {
                const double edge = 24.0 + 0.15 * y;
                for (int x = 0; x < W; x++) {
                    double t = (double)x - edge + 0.5;      // coverage of pixel x
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    d->px()[(size_t)y * W + x] = (float)(dark + 800.0 * t);
                }
            }
            // ...and, for the second fixture, one 3-tap box across the edge:
            // the LSF becomes box(1) * box(3) and the model gains a sinc(3f).
            if (blur) {
                std::vector<float> src(d->px().begin(), d->px().end());
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        const int a = std::max(0, x - 1), b = std::min(W - 1, x + 1);
                        d->px()[(size_t)y * W + x] =
                            (float)(((double)src[(size_t)y * W + a] +
                                     src[(size_t)y * W + x] +
                                     src[(size_t)y * W + b]) / 3.0);
                    }
            }
            computeMinMax(*d);
            return d;
        };
        auto sinc = [](double t) {
            return t == 0.0 ? 1.0 : std::sin(3.14159265358979323846 * t) /
                                    (3.14159265358979323846 * t);
        };
        auto sharp = mkEdge("av_edge", 0, 0.0);
        const Res E = run("iso12233/e-sfr", *sharp, nullptr);
        auto soft = mkEdge("av_edge3", 1, 0.0);
        const Res F = run("iso12233/e-sfr", *soft, nullptr);
        // the model, and the ideal sinc it can never exceed
        const double M33 = sinc(0.33) * sinc(0.0825) * sinc(0.0825);   // 0.81182562
        const double M50 = sinc(0.50) * sinc(0.1250) * sinc(0.1250);   // 0.60456037
        const double IDEAL50 = sinc(0.5);                              // 0.63661977
        const double ANGLE = std::atan(0.15) * 180.0 / 3.14159265358979323846;
        fprintf(stderr, "anavalueselftest: e-sfr sharp angle=%.12g (want %.12g) lines=%.12g "
                        "mtf50=%.12g mtf20=%.12g nyq=%.12g\n",
                E.at("edge_angle_deg"), ANGLE, E.at("lines_used"),
                E.at("mtf50 (cy/px)"), E.at("mtf20 (cy/px)"), E.at("sfr@nyquist"));
        fprintf(stderr, "anavalueselftest: e-sfr model f=0.33 %.12g f=0.50 %.12g "
                        "(ideal sinc 0.5 = %.12g); crossings 0.57694593 / 0.80386988, "
                        "ideal 0.60335456 / 0.82624941\n", M33, M50, IDEAL50);
        if (E.sfrY.size() == 101)
            fprintf(stderr, "anavalueselftest: e-sfr sharp vs model: f=0.33 %+.3f%% "
                            "f=0.50 %+.3f%%  mtf50 %+.3f%%  mtf20 %+.3f%%\n",
                    ((double)E.sfrY[33] / M33 - 1) * 100,
                    (E.at("sfr@nyquist") / M50 - 1) * 100,
                    (E.at("mtf50 (cy/px)") / 0.57694593 - 1) * 100,
                    (E.at("mtf20 (cy/px)") / 0.80386988 - 1) * 100);
        if (E.sfrY.size() == 101 && F.sfrY.size() == 101)
            fprintf(stderr, "anavalueselftest: e-sfr curve f=0.33 sharp=%.6g blur=%.6g | "
                            "f=0.50 %.6g / %.6g | f=1.00 %.6g / %.6g | blur mtf50=%.6g\n",
                    (double)E.sfrY[33], (double)F.sfrY[33], (double)E.sfrY[50],
                    (double)F.sfrY[50], (double)E.sfrY[100], (double)F.sfrY[100],
                    F.at("mtf50 (cy/px)"));
        check(E.rc == 0 && E.sfrY.size() == 101 && E.sfrX.size() == 101,
              "S1 e-sfr returned the 101-point curve, 0 .. 1.0 cy/px");
        check(E.sfrY.size() == 101 && E.sfrY[0] == 1.0f && E.sfrX[50] == 0.5f,
              "S1 ...normalized at DC, and index 50 IS Nyquist");
        check(rel(E.at("edge_angle_deg"), ANGLE, 2e-4),
              "S1 edge_angle_deg = atan(0.15)*180/pi = 8.5307656 (+-2e-4, see above)");
        check(E.at("lines_used") == 64.0,
              "S1 lines_used = 64: every line of the ROI carries the edge");
        // ---- the CURVE, against the model, tightly ----------------------
        check(E.sfrY.size() == 101 && rel((double)E.sfrY[33], M33, 0.01),
              "S1 SFR(0.33) = |sinc(.33)|*sinc(.0825)^2 = 0.81182562 within 1%");
        check(E.rc == 0 && rel(E.at("sfr@nyquist"), M50, 0.015),
              "S1 sfr@nyquist = |sinc(.5)|*sinc(.125)^2 = 0.60456037 within 1.5%");
        // ...and the ideal sinc as a one-sided bound - safe on THIS fixture
        // because the Hamming window is ~1 over the 1 px the LSF occupies (see
        // the header; it is not a theorem, the window convolves in frequency).
        check(E.sfrY.size() == 101 && E.at("sfr@nyquist") < IDEAL50 &&
              (double)E.sfrY[33] < sinc(0.33),
              "S1 ...and both sit BELOW the ideal sinc (on this fixture, see header)");
        // ---- the crossings, at 1% now that the dark level is 0 -----------
        check(E.at("mtf50 (cy/px)") <= 0.60335456 &&
              rel(E.at("mtf50 (cy/px)"), 0.57694593, 0.01),
              "S1 mtf50 = 0.57694593 +-1%, and under the ideal 0.60335456");
        check(E.at("mtf20 (cy/px)") <= 0.82624941 &&
              rel(E.at("mtf20 (cy/px)"), 0.80386988, 0.01),
              "S1 mtf20 = 0.80386988 +-1%, and under the ideal 0.82624941");
        // ---- the 3-tap blur, whose sinc(3f) NULL at 1/3 is the strongest
        // shape claim available here: one extra box in the fixture takes one
        // point of the curve from 0.81 to under 0.10, and no constant, no
        // mis-scaled normalization and no swapped axis does that.
        check(F.rc == 0 && F.sfrY.size() == 101 && (double)F.sfrY[33] < 0.10,
              "S2 a 3-tap box puts a null at 1/3 cy/px: SFR(0.33) drops under 0.10");
        check(E.sfrY.size() == 101 && (double)E.sfrY[33] > 0.75,
              "S2 ...where the sharp edge reads 0.81 - a 13x separation at ONE f");
        check(F.rc == 0 && F.at("mtf50 (cy/px)") > 0.15 && F.at("mtf50 (cy/px)") < 0.25,
              "S2 ...and mtf50 falls to the 0.19228 that null forces");
        check(rel(F.at("edge_angle_deg"), E.at("edge_angle_deg"), 2e-3),
              "S2 ...while the edge is the SAME edge: the angle does not move");

        // ---- DEFECT(esfr-1), found while reviewing this group --------------
        // The SAME edge with a dark level of 100 instead of 0. Nothing about the
        // scene changed - the rise is still 800 DN and the derivative, so the
        // edge fit and the angle, is identical - but the analyzer fills an EMPTY
        // leading 4x bin with 0 rather than with the dark level or the first
        // value it has (analyzer_esfr.c:150-158, `lsf[i] = havePrev ? prev : 0`).
        // The first non-empty bin is then a step of the whole dark level, which
        // is a false impulse in the LSF at the ROI's left edge, and it ripples
        // the whole curve: three independent Python ports of analyzer_esfr.c
        // measured -5.6% to -10.1% against the model over f = 0.05..0.50, with
        // the zero crossings of the ripple near f = 0.33 and f = 0.50 - which is
        // exactly why the first draft of this group read as "within 1%" at the
        // two frequencies it sampled and needed a +-6% band on the crossings.
        //   NOT asserted, and analyzer_esfr.c NOT touched: fixing the fill is a
        //   change to the computation, so it moves the declared version 1.0.0
        //   and belongs on its own board row (Fable writes it). What this does is
        //   print the size of it, every run, so the next reader of a report that
        //   used a non-zero dark level knows the number is biased low.
        {
            auto pedestal = mkEdge("av_edge_dark100", 0, 100.0);
            const Res P = run("iso12233/e-sfr", *pedestal, nullptr);
            if (P.rc == 0 && P.sfrY.size() == 101)
                fprintf(stderr, "anavalueselftest: S2 DEFECT(esfr-1): the same edge on a "
                                "dark level of 100 reads mtf50 %.6g vs %.6g at dark 0 "
                                "(%+.2f%%), SFR(0.33) %.6g vs %.6g (%+.2f%%) - an empty "
                                "leading bin is filled with 0, not with the dark level, "
                                "so the LSF gains a false impulse of the pedestal's "
                                "height. Not asserted - reported for a ruling.\n",
                        P.at("mtf50 (cy/px)"), E.at("mtf50 (cy/px)"),
                        (P.at("mtf50 (cy/px)") / E.at("mtf50 (cy/px)") - 1) * 100,
                        (double)P.sfrY[33], (double)E.sfrY[33],
                        ((double)P.sfrY[33] / (double)E.sfrY[33] - 1) * 100);
            // ...and the half that IS unaffected, which is what proves the
            // diagnosis: a constant offset cannot change a difference, so the
            // edge fit sees the same edge.
            check(P.rc == 0 && rel(P.at("edge_angle_deg"), E.at("edge_angle_deg"), 1e-12) &&
                  P.at("lines_used") == E.at("lines_used"),
                  "S2 the pedestal moves the CURVE and not the edge fit (bit-identical angle)");
        }

        // the two refusals, in words
        auto bay = mkEdge("av_edge_cfa", 0, 0.0);
        bay->cfa = 1; bay->cfaPattern = 0;
        const Res B = run("iso12233/e-sfr", *bay, nullptr);
        check(B.rc == 1 && B.err == "e-sfr: CFA mosaic input - demosaic first",
              "S3 a mosaic is refused: the luma of a Bayer frame is not a luma");
        const psRect tiny{ 0, 0, 12, 40 };
        const Res T = run("iso12233/e-sfr", *sharp, &tiny);
        check(T.rc == 1 &&
              T.err == "e-sfr: ROI too small (need >= 16x16 over the edge)",
              "S3 ...and an ROI under 16 px refuses instead of fitting 12 lines");
    }

    fprintf(stderr, "anavalueselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
