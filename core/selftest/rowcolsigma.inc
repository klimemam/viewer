// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --rowcol-sigma-selftest: the FOUR implementations of "sigma of the row /
// ---- column means", pinned against each other (板 303) -----------------------
//
// docs/background/analysis/measurement-kernel-inventory.md K2 found four sites
// computing "the sigma of the row means / of the column means" and NO assert
// relating any pair of them (P14 / P15). Two of them print side by side in the
// Projection table; two print in the same export FILE, sections 2 and 3, under
// the same column name. Fable's ruling was: do not unify yet - build one
// analytic fixture where every difference is predictable in closed form, print
// all four numbers with their definitions, and assert each against a
// hand-derived expectation AND the relations between them.
//
// WHICH NUMBER IS WHICH. Read this table before changing anything below.
//
//  #  site                                   numerator            denominator
//  -- -------------------------------------- -------------------- ------------------
//  1  core/ui/panel_histogram.inc            (sum2/n - m^2)*n/(n-1)  the PROFILE's own
//     recomputeProjectionIfNeeded, hStat /    over the plotted        mean (hStat.mean /
//     vStat  ->  Projection "sigma (axis)",   profile, which is       vStat.mean) for the
//     export section 2 "sigma_col [DN]" /     stored as FLOAT         % column
//     "sigma_row [DN]" and their [%]
//  2  core/ui/panel_temporal.inc              the same algebraic     the PLANE's mean m
//     exportPerFrameBlock, profCv  ->         form, in DOUBLE,       (every finite pixel
//     export section 3 "sigma_col [%]" /      over the full          of the region)
//     "sigma_row [%]"                         rectangle
//  3  core/app/profile_noise.inc              ORIGIN-SHIFTED form    DIMENSIONLESS - the
//     varianceOfMeans, reached through         (sum2 - sum^2/n)      number printed is
//     computeProfileNoise  ->  Projection      /(n-1), in DOUBLE,    sqrt(rowVariance /
//     "HFPN/RN" / "VFPN/RN", export s.2        full rectangle        pVariance)
//  4  plugins/analyzer_prnu.c  ->  Analysis    sum(d^2)/(n-1) where  the plane's mean
//     "row_fpn_pct" / "col_fpn_pct"            d = profile MINUS a
//                                              9-tap boxblur of it
//
//  #  sample                        NaN policy                  precision
//  -- ----------------------------- --------------------------- ----------------
//  1  strided in the ORTHOGONAL     skips the pixel             profile rounded
//     direction above 2M samples                                to float once
//  2  full                          skips the pixel             double throughout
//  3  full                          REFUSES the whole plane     double, shifted
//  4  strided grid on CFA           SUBSTITUTES 0 for it        double sums,
//                                                               float profile
//
// Four stages, each isolating one axis:
//   S1  a bilinear ramp plus a zero-sum residual: every number in closed form,
//       and #1 == #3 on A and B up to the p/W, p/H the decomposition removes.
//   S2  the same fixture with ONE NaN at the exact centre: #1 and #2 print the
//       IDENTICAL numbers, #3 refuses the plane, #4's mean moves.
//   S3  4.2M pixels, so #1 strides: the sigmas stay equal (a ramp's variance is
//       shift invariant) and only the DENOMINATORS part, by an exact factor.
//   S4  a spread of one float ulp on a dyadic pedestal: #1 reports exactly 0
//       for a column-to-column difference #2 and #3 both measure.
//   S5  a MOSAIC. Every stage above is one plane, so the CFA path through all
//       four sites had no assertion of any kind - and it is the path the
//       Projection panel is mostly used on. A mosaic plane's column means are
//       over its OWN columns (half of the frame's, at its own phase), so the
//       one thing this stage exists to catch is a site that divides by the
//       frame's column count, folds the other planes' NaNs in, or reads the
//       profile at the wrong phase. The fixture is four independent bilinear
//       ramps at four levels, so every number is in closed form per plane and
//       any phase error shows up as the WRONG LEVEL, which is unmissable.
//
// NOGL and fixture-free, for --histhl-selftest's reasons: every claim is a
// number, the documents are analytic and built in memory, and the expected
// values are rationals derived by hand with the derivation beside the assert.
if (g_rowColSigmaSelftest) {
    using profile_noise::Status;
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "rowcolsigmaselftest: %-74s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // PURELY relative, and never on a quantity that is exactly 0 - those are
    // compared with == below, because "collapsed to nothing" is the claim there.
    // A max(1, |b|) floor was here first and it was wrong: it turns every
    // expectation under 1 into an ABSOLUTE bound, so S4's variances (1.7e-8) and
    // S1's col_fpn (0.045%) were being held eight and two orders looser than the
    // number beside them said. The tolerances below mean what they say.
    auto near_ = [](double a, double b, double rel) {
        return std::isfinite(a) && std::fabs(a - b) <= rel * std::fabs(b);
    };
    // #2 is read back out of the EXPORT, where every number is %.9g - nine
    // significant digits is all there is, so its comparisons are relative to the
    // value and never absolute. 1e-8 and not 1e-9: half of the ninth digit is
    // 5e-9 of relative error when the leading digit is a 1, so anything tighter
    // asserts the export's rounding rather than its arithmetic. This is the same
    // bound and the same reasoning as core/selftest/framestats.inc.
    auto near9 = [](double a, double b) {
        return std::isfinite(a) && std::fabs(a - b) <= 1e-8 * std::fabs(b);
    };

    // ---- the four callers, each behind one lambda ---------------------------
    // The document goes INTO app.images: #2 reaches its pixels through
    // framesOfSeq, which walks the open list, so a fixture held only by a local
    // unique_ptr is a fixture that implementation cannot see.
    auto mk = [](const char* nm, int W, int H) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = H; d->src->ch = 1;
        d->src->dtype = "f32";
        d->src->data.assign((size_t)W * H, 0.0f);
        d->syncMirrors();
        d->uid = app.nextUid++;
        app.images.push_back(std::move(d));
        app.imagesRev++;
        return app.images.back().get();
    };
    // #1: the REAL Projection cache. app.projMode 0 = mean, allRow off - both
    // restored at the end of the file, because the selftest home is shared.
    auto proj1 = [](ImageDoc& d, App::ProjState& P) {
        d.dataRev++;                       // the cache keys on it
        recomputeProjectionIfNeeded(&d, P);
    };
    // #2: the REAL per-frame export block, which needs a stack to walk. One
    // frame is a stack of one; the block reads px() and the ROI convention and
    // nothing else about the sequence.
    struct Row2 { double mean = 0, sd0 = 0, colPct = 0, rowPct = 0; bool got = false; };
    // `sfx` is the per-plane column suffix the block writes ("_R" on a mosaic,
    // "" on a plain 1-channel frame) - the export names its columns per plane
    // and this is how a caller asks for one.
    auto perFrame2 = [&](ImageDoc* d, const char* sfx = "") {
        Row2 r;
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = "rcs";
        app.seqs.push_back(si);
        d->seqId = si.id; d->seqIndex = 0;
        // ONE exit that puts the borrowed state back, taken on the failure path
        // as well - the home is shared, and a stack left in app.seqs by a test
        // that gave up is a stack every later test has to cope with.
        auto finish = [&](Row2 out) {
            app.seqs.clear();
            d->seqId = 0;
            d->seqIndex = 0;
            return out;
        };
        ExportDoc doc('\t');
        const PerFrameBlock blk = exportPerFrameBlock(si.id, doc);
        if (blk.rows != 1) return finish(r);
        // split the two lines and pick the columns BY NAME: the header is the
        // documented way to read this file and a positional index here would
        // silently follow a column that moved
        std::vector<std::string> head, cell;
        size_t nl = doc.out.find('\n');
        auto split = [](const std::string& s, std::vector<std::string>& into) {
            for (size_t i = 0, j; i <= s.size(); i = j + 1) {
                j = s.find('\t', i);
                if (j == std::string::npos) j = s.size();
                into.push_back(s.substr(i, j - i));
                if (j == s.size()) break;
            }
        };
        split(doc.out.substr(0, nl), head);
        split(doc.out.substr(nl + 1, doc.out.find('\n', nl + 1) - nl - 1), cell);
        auto at = [&](const char* name) {
            for (size_t i = 0; i < head.size() && i < cell.size(); i++)
                if (head[i] == name) return atof(cell[i].c_str());
            return std::numeric_limits<double>::quiet_NaN();
        };
        const std::string sf = sfx;
        r.mean = at(("mean" + sf + " [DN]").c_str());
        r.sd0 = at(("sigma" + sf + " [DN](ddof=0)").c_str());
        r.colPct = at(("sigma_col" + sf + " [%](ddof=1)").c_str());
        r.rowPct = at(("sigma_row" + sf + " [%](ddof=1)").c_str());
        r.got = std::isfinite(r.mean);
        return finish(r);
    };
    // #3: the decomposition, called directly. A and B are not fields of the
    // result - the published numbers are rowVariance = B - p/W and
    // colVariance = A - p/H - so the test reconstructs them, which is itself
    // one of the relations under test.
    auto planeB = [](const profile_noise::Result& r) {
        return r.rowVariance + r.pVariance / r.width;      // = B
    };
    auto planeA = [](const profile_noise::Result& r) {
        return r.colVariance + r.pVariance / r.height;     // = A
    };
    // #4: the shipped uniformity/prnu-fpn dll, on the same pixels, through the
    // same mouth every other caller uses. Numbers only - the row keys are
    // "ch0.*" for a 1-channel non-mosaic frame.
    struct Row4 { double mean = 0, rowFpn = 0, colFpn = 0; std::string note; bool got = false; };
    // `key` is the row-key prefix: "ch0" for a plain 1-channel frame, the plane
    // name ("R", "Gr", ...) on a mosaic.
    auto prnu4 = [&](ImageDoc& d, const char* key = "ch0") {
        Row4 r;
        const auto& anas = plugin_host::analyzers();
        const AnalyzerPluginInfo* a = nullptr;
        for (const auto& c : anas) if (c.name == "uniformity/prnu-fpn") a = &c;
        if (!a) return r;
        std::vector<std::pair<std::string, std::string>> rows;
        std::vector<std::string> series;
        g_bunRows = &rows; g_bunSeries = &series;
        psFrame fr = makeFrame(d);
        char perr[256] = { 0 };
        const int32_t rc = bunRunRaw(*a, &fr, nullptr, perr, sizeof perr);
        g_bunRows = nullptr; g_bunSeries = nullptr;
        if (rc != 0) { r.note = perr; return r; }
        const std::string k = key;
        for (const auto& kv : rows) {
            if (kv.first == k + ".mean") { r.mean = atof(kv.second.c_str()); r.got = true; }
            else if (kv.first == k + ".row_fpn_pct") r.rowFpn = atof(kv.second.c_str());
            else if (kv.first == k + ".col_fpn_pct") r.colFpn = atof(kv.second.c_str());
            else if (kv.first == k) r.note = kv.second;            // a refusal, in words
        }
        return r;
    };

    // The home is shared and these three decide what #1 measures.
    const int saveMode = app.projMode;
    const bool saveAllRow = app.projAllRow;
    const int saveAnn = app.selectedAnn;
    closeAll();
    app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0;
    app.selectedAnn = -1;                  // whole frame, not an ROI
    app.projMode = 0;                      // mean, not max/min
    app.projAllRow = false;
    app.seqs.clear();

    // =====================================================================
    // S1 - the analytic fixture. 17x17, v(x,y) = 1000 + x + 4y + 8*s((x+y)%17)
    // with s(k) = +1 for k in 0..7, -1 for k in 8..15 and 0 for k = 16.
    //
    // 17 is prime, so (x+y)%17 runs over every residue exactly once along each
    // row and each column; sum(s) = 8 - 8 + 0 = 0, so the residual leaves EVERY
    // row mean and EVERY column mean untouched while adding pixel-level
    // variance. That is what lets A and B be exact while p is not zero.
    //   column means  c_x = 1032 + x           (slope 1)
    //   row means     r_y = 1008 + 4y          (slope 4)
    //   plane mean    M   = 1040
    // and every pixel value is an integer in 1000..1080, so the float profile of
    // #1 and the double profile of #2 hold the SAME numbers - stage 4 is where
    // that stops being true.
    //   A = Var_ddof1(c) = 1 * 17*18/12 = 51/2 = 25.5
    //   B = Var_ddof1(r) = 16 * 51/2    = 408
    //   Var_pop(all)     = 24 + 16*24 + 64*16/17 = 7960/17
    //   T = Var_ddof1(all) = (7960/17)*(289/288) = 16915/36
    //   denom = 1 - 1/17 - 1/17 = 15/17
    //   p = (T - A - B)/denom = (-289/12 + ... ) -> 22253/540   (positive)
    //   rowVariance = B - p/17 = 219011/540 ; colVariance = A - p/17 = 12461/540
    // =====================================================================
    const double A1 = 51.0 / 2, B1 = 408.0, M1 = 1040.0;
    const double P1 = 22253.0 / 540, ROWV1 = 219011.0 / 540, COLV1 = 12461.0 / 540;
    App::ProjState P;
    Row2 r2a;
    Row4 r4a;
    {
        ImageDoc* d = mk("rcs_s1", 17, 17);
        for (int y = 0; y < 17; y++)
            for (int x = 0; x < 17; x++) {
                const int k = (x + y) % 17;
                const int s = k <= 7 ? 1 : (k <= 15 ? -1 : 0);
                d->px()[(size_t)y * 17 + x] = (float)(1000 + x + 4 * y + 8 * s);
            }
        computeMinMax(*d);
        proj1(*d, P);
        const profile_noise::Result& N = P.noise[0];
        r2a = perFrame2(d);
        r4a = prnu4(*d);

        fprintf(stderr, "rowcolsigmaselftest: S1 #1 sigma_col=%.12g sigma_row=%.12g "
                        "(mean %.12g / %.12g)\n",
                P.hStat[0].sd, P.vStat[0].sd, P.hStat[0].mean, P.vStat[0].mean);
        fprintf(stderr, "rowcolsigmaselftest: S1 #1 pct col=%.12g row=%.12g   "
                        "#2 pct col=%.12g row=%.12g\n",
                P.hStat[0].pct, P.vStat[0].pct, r2a.colPct, r2a.rowPct);
        fprintf(stderr, "rowcolsigmaselftest: S1 #3 p=%.12g rowVar=%.12g colVar=%.12g "
                        "(A=%.12g B=%.12g)\n",
                N.pVariance, N.rowVariance, N.colVariance, planeA(N), planeB(N));
        fprintf(stderr, "rowcolsigmaselftest: S1 #4 mean=%.12g row_fpn=%.12g "
                        "col_fpn=%.12g%s\n",
                r4a.mean, r4a.rowFpn, r4a.colFpn, r4a.note.empty() ? "" : r4a.note.c_str());

        // ---- #1, against its own definition ----------------------------
        check(P.hStat[0].valid && P.vStat[0].valid && P.nSeries == 1,
              "S1 fixture: one plane, both profiles valid");
        check(P.hStat[0].mean == 1040.0 && P.vStat[0].mean == 1040.0,
              "S1 #1 both profiles average to the plane mean 1040 exactly");
        check(near_(P.hStat[0].sd * P.hStat[0].sd, A1, 1e-12) &&
              near_(P.vStat[0].sd * P.vStat[0].sd, B1, 1e-12),
              "S1 #1 sigma_col^2 = 51/2 and sigma_row^2 = 408 (hand-derived)");
        // the region sigma beside them is a DIFFERENT quantity (ddof=0 over the
        // pixels): pinned here so that a change that moved all four could not
        // hide behind the profile ones
        check(near_(P.fStat[0].sd * P.fStat[0].sd, 7960.0 / 17, 1e-12),
              "S1 #1 sigma_frame^2 = 7960/17, ddof=0 over the pixels");
        check(near_(P.hStat[0].pct, std::sqrt(A1) / M1 * 100.0, 1e-12) &&
              near_(P.vStat[0].pct, std::sqrt(B1) / M1 * 100.0, 1e-12),
              "S1 #1 the % divides by the PROFILE mean, which here is 1040");

        // ---- #2, against its own definition ----------------------------
        check(r2a.got && r2a.mean == M1 && near9(r2a.sd0, std::sqrt(7960.0 / 17)),
              "S1 #2 the per-frame row: mean 1040, sigma ddof=0 = sqrt(7960/17)");
        check(near9(r2a.colPct, std::sqrt(A1) / M1 * 100.0) &&
              near9(r2a.rowPct, std::sqrt(B1) / M1 * 100.0),
              "S1 #2 sigma_col/row [%] divide by the PLANE mean, also 1040");

        // ---- #1 <-> #2 : the pair the export file prints twice (P14) ----
        // The denominators are DIFFERENT QUANTITIES with the same value here -
        // the mean of the column means equals the plane mean whenever every
        // column carries the same sample count. Stage 3 is where they part.
        check(near9(P.hStat[0].pct, r2a.colPct) && near9(P.vStat[0].pct, r2a.rowPct),
              "S1 #1 == #2 on both sigma_col [%] and sigma_row [%]");

        // ---- #3, against its own definition ----------------------------
        check(N.valid() && N.width == 17 && N.height == 17 && N.samples == 289,
              "S1 #3 one full 17x17 plane, 289 samples");
        check(near_(N.pVariance, P1, 1e-10) && near_(N.rowVariance, ROWV1, 1e-10) &&
              near_(N.colVariance, COLV1, 1e-10) && !N.clamped(),
              "S1 #3 p=22253/540, rowVar=219011/540, colVar=12461/540, no clamp");

        // ---- #1 <-> #3 : the pair the Projection table prints side by side
        // (P15). The canon forbids UNIFYING them, so what this asserts is the
        // exact algebraic relation between the two: #3's published numbers are
        // #1's variances minus the pixel term the decomposition removes. Written
        // as "reconstruct A and B and compare", because that is the sentence a
        // reader of the table needs.
        check(near_(planeA(N), A1, 1e-10) && near_(planeB(N), B1, 1e-10),
              "S1 #3 colVariance + p/H == #1 sigma_col^2 (and the row twin)");
        // ...and they are NOT equal as printed: 12461/540 is 9.5% below 51/2,
        // and 219011/540 is 0.6% below 408. Anti-vacuity for the line above.
        check(std::fabs(N.colVariance - A1) > 1e-6 * A1 &&
              std::fabs(N.rowVariance - B1) > 1e-6 * B1,
              "S1 ...while the printed pair differs by p/H and p/W (anti-vacuity)");

        // ---- #4, against its own definition ----------------------------
        // The plugin detrends before it measures: m1 is the profile, m2 is a
        // 9-tap boxblur of m1 with edge clamp, and the sigma is of d = m1 - m2.
        // For a LINEAR profile of slope g the interior of d is exactly 0 (the
        // taps are symmetric) and only the four clamped positions at each end
        // survive: d = -(10,6,3,1)*g/9 at the low end and +(1,3,6,10)*g/9 at the
        // high end. So sum(d^2) = 2*(100+36+9+1)*g^2/81 = 292*g^2/81 and
        //   fpn = sqrt(292/81 * g^2/(n-1))   with n = 17, so n-1 = 16.
        // row profile slope 4 -> sqrt(292)/9 ; column profile slope 1 -> /4 of
        // that. Tolerance 1e-4 and not 1e-12 because m2 is stored as FLOAT: an
        // ulp at ~1040 is 6.1e-5, so m2 carries up to 3.1e-5 of absolute error,
        // and the SMALLEST |d| in the set is g/9 - 0.111 for the column profile,
        // where that is 2.7e-4 relative. Propagated through sum(d^2) and the
        // square root it is about 4e-5 on col_fpn (measured 2.5e-5) and four
        // times less on row_fpn, whose slope is 4. One bound for both, set by the
        // worse of the two. (anavalue.inc's U1 holds the same quantity at the
        // same 1e-4 for the same reason.)
        const double FPN_ROW = std::sqrt(292.0) / 9 / M1 * 100.0;
        const double FPN_COL = std::sqrt(292.0) / 36 / M1 * 100.0;
        check(r4a.got && r4a.mean == M1, "S1 #4 the plugin's plane mean is 1040 exactly");
        check(near_(r4a.rowFpn, FPN_ROW, 1e-4) && near_(r4a.colFpn, FPN_COL, 1e-4),
              "S1 #4 row_fpn_pct = 100*sqrt(292)/9/1040, col_fpn_pct = a quarter of it");
        check(near_(r4a.rowFpn / r4a.colFpn, 4.0, 1e-4),
              "S1 #4 ...so the ratio is exactly the slope ratio 4");

        // ---- #4 <-> #1 : why the names had to differ (P16) --------------
        // Same pixels, same words ("sigma of the row means"), and the numbers
        // are a factor of ~11 apart, because the plugin removed the ramp first.
        // 100*sqrt(408)/1040 = 1.9422 %, 100*sqrt(292)/9/1040 = 0.18256 %.
        check(near_(P.vStat[0].pct / r4a.rowFpn, std::sqrt(B1) * 9 / std::sqrt(292.0), 1e-4),
              "S1 #4 vs #1 differ by exactly the detrend: sqrt(408)*9/sqrt(292) = 10.64x");
    }

    // =====================================================================
    // S2 - ONE NaN, at the exact centre (8,8). 17 is odd, so removing the
    // middle index from a column leaves that column's mean unchanged:
    // (sum(0..16) - 8)/16 = (136-8)/16 = 8. And s((8+8)%17) = s(16) = 0, so
    // v(8,8) = 1040 = the plane mean, the row-8 mean AND the column-8 mean -
    // which is why nothing that SKIPS the pixel moves at all.
    // The four policies therefore separate cleanly:
    //   #1 skip  -> every number identical to S1
    //   #2 skip  -> every number identical to S1
    //   #3 REFUSES the plane (Status::NonFinite): no number at all
    //   #4 substitutes 0 -> the mean drops to 1040*288/289 exactly
    // =====================================================================
    {
        ImageDoc* d = mk("rcs_s2", 17, 17);
        for (int y = 0; y < 17; y++)
            for (int x = 0; x < 17; x++) {
                const int k = (x + y) % 17;
                const int s = k <= 7 ? 1 : (k <= 15 ? -1 : 0);
                d->px()[(size_t)y * 17 + x] = (float)(1000 + x + 4 * y + 8 * s);
            }
        check(d->px()[(size_t)8 * 17 + 8] == 1040.0f,
              "S2 fixture: the centre pixel IS the plane mean before it is voided");
        d->px()[(size_t)8 * 17 + 8] = std::numeric_limits<float>::quiet_NaN();
        computeMinMax(*d);
        App::ProjState Q;
        proj1(*d, Q);
        const profile_noise::Result& N = Q.noise[0];
        const Row2 r2 = perFrame2(d);
        const Row4 r4 = prnu4(*d);
        fprintf(stderr, "rowcolsigmaselftest: S2 one NaN: #1 col=%.12g row=%.12g   "
                        "#2 col%%=%.12g row%%=%.12g   #3 '%s'   #4 mean=%.12g\n",
                Q.hStat[0].sd, Q.vStat[0].sd, r2.colPct, r2.rowPct, N.reason(), r4.mean);

        check(near_(Q.hStat[0].sd * Q.hStat[0].sd, A1, 1e-12) &&
              near_(Q.vStat[0].sd * Q.vStat[0].sd, B1, 1e-12) &&
              Q.hStat[0].mean == 1040.0,
              "S2 #1 skips the pixel: sigma_col/row are the S1 numbers, unmoved");
        check(r2.got && near9(r2.colPct, std::sqrt(A1) / M1 * 100.0) &&
              near9(r2.rowPct, std::sqrt(B1) / M1 * 100.0) && r2.mean == M1 &&
              r2.colPct == r2a.colPct && r2.rowPct == r2a.rowPct,
              "S2 #2 skips it too, and prints the same two percentages");
        check(N.status == Status::NonFinite && !N.valid() &&
              std::string(N.reason()) == "plane contains non-finite pixels",
              "S2 #3 refuses the whole plane instead of producing a number");
        // 1040 * 288/289 = 1036.4013840830449. Substituting 0 for one pixel of
        // 289 is a -0.35% step in the DENOMINATOR of all five of the plugin's
        // percentages, which is how a single NaN moves numbers it never touched.
        check(r4.got && near_(r4.mean, 1040.0 * 288.0 / 289.0, 1e-12),
              "S2 #4 substitutes 0: its mean is 1040*288/289, not 1040");
        check(!near_(r4.rowFpn, r4a.rowFpn, 1e-4) && !near_(r4.colFpn, r4a.colFpn, 1e-4),
              "S2 #4 ...and both fpn numbers move, while #1 and #2 did not");
    }

    // =====================================================================
    // S3 - the SAMPLE axis. 2048x2048 = 4194304 pixels, v = 1000 + x + 4y.
    // PROJ_MAX_SAMPLES is 2000000, so #1's step is 4194304/2000000 = 2: its H
    // pass sees the EVEN rows only and its V pass the even columns only, while
    // #2 and #3 walk every pixel.
    //
    // A ramp's variance is shift invariant, so this moves NOTHING in the
    // sigmas and EVERYTHING it does move is in the means:
    //   #1 column means  = 1000 + x + 4*mean(even y) = 5092 + x   -> mean 6115.5
    //   #1 row means     = 1000 + 4y + mean(even x)  = 2023 + 4y  -> mean 6117
    //   #2/#3 full       = 5094 + x and 2023.5 + 4y  -> mean 6117.5 = M
    //   Var_ddof1(0..2047) = 2048*2049/12 = 349696, x16 = 5595136
    // so the sigmas are equal to the last bits and the percentages differ by
    // exactly 6117.5/6115.5 (col) and 6117.5/6117 (row) - +3.27e-4 and +8.17e-5
    // relative. THE DIFFERENCE IS ONLY EVER IN THE DENOMINATOR.
    // =====================================================================
    {
        const int N3 = 2048;
        ImageDoc* d = mk("rcs_s3", N3, N3);
        for (int y = 0; y < N3; y++)
            for (int x = 0; x < N3; x++)
                d->px()[(size_t)y * N3 + x] = (float)(1000 + x + 4 * y);
        computeMinMax(*d);
        App::ProjState R;
        proj1(*d, R);
        const profile_noise::Result& N = R.noise[0];
        const Row2 r2 = perFrame2(d);
        const double VC = 2048.0 * 2049 / 12, VR = 16 * VC;    // 349696 and 5595136
        fprintf(stderr, "rowcolsigmaselftest: S3 #1 means col=%.12g row=%.12g   "
                        "#2 mean=%.12g\n", R.hStat[0].mean, R.vStat[0].mean, r2.mean);
        fprintf(stderr, "rowcolsigmaselftest: S3 sigma_col: #1=%.12g #2(implied)=%.12g "
                        "#3(A)=%.12g want %.12g\n",
                R.hStat[0].sd, r2.colPct / 100.0 * r2.mean, std::sqrt(planeA(N)),
                std::sqrt(VC));

        check(R.hStat[0].mean == 6115.5 && R.vStat[0].mean == 6117.0,
              "S3 #1 strides: 6115.5 and 6117, and the two axes do not even agree");
        check(r2.mean == 6117.5,
              "S3 #2 walks every pixel: its denominator is the plane mean 6117.5");
        check(near_(R.hStat[0].sd * R.hStat[0].sd, VC, 1e-9) &&
              near_(R.vStat[0].sd * R.vStat[0].sd, VR, 1e-9),
              "S3 #1 sigma_col^2 = 349696, sigma_row^2 = 5595136 despite the stride");
        check(near_(planeA(N), VC, 1e-9) && near_(planeB(N), VR, 1e-9),
              "S3 #3 measures the SAME two variances off the full rectangle");
        check(N.pClamped && N.pVariance == 0,
              "S3 #3 a pure ramp resolves no pixel noise: p clamps, so A and B print raw");
        // the sigmas agree, so the PERCENTAGES must differ by exactly the ratio
        // of the two denominators - the whole of P14, in one number
        check(near_(R.hStat[0].pct / r2.colPct, 6117.5 / 6115.5, 1e-8) &&
              near_(R.vStat[0].pct / r2.rowPct, 6117.5 / 6117.0, 1e-8),
              "S3 #1 / #2 [%] = 6117.5/6115.5 and 6117.5/6117 - denominators only");
        check(R.hStat[0].pct != r2.colPct && R.vStat[0].pct != r2.rowPct,
              "S3 ...and they are NOT the same number here (anti-vacuity for S1)");
    }

    // =====================================================================
    // S4 - the PRECISION axis, and the one place it is not a rounding detail.
    // 8x16 on a dyadic pedestal: v = 4096 + 2^-11 where x >= 4 and y < 8, else
    // 4096. 2^-11 is exactly one float ulp at 4096, so every pixel is exact.
    //   column means: 4096 (x<4) and 4096 + 2^-12 (x>=4)
    //   row means:    4096 + 2^-12 (y<8) and 4096 (y>=8)
    // 2^-12 is exactly HALF an ulp, and 4096.0 has the even mantissa, so
    // (float)(4096 + 2^-12) is 4096.0: #1's profile holds ONE value.
    //   #1  sigma_col = sigma_row = 0, exactly
    //   #2  sigma_col = 2^-13*sqrt(8/7)  = 1.3049865e-4 DN  (as a % of the
    //       plane mean 4096 + 2^-13: 3.1860022e-6)
    //   #3  A = 2^-26*8/7, B = 2^-26*16/15, p = 1357/90887946240 (not clamped)
    //   #4  declines: 8 columns is under its 2*RADIUS+2 = 10 minimum
    // Every double above is exact - the pedestal and the offsets are dyadic, so
    // sum2/n - m^2 loses nothing even at 4096.
    // =====================================================================
    {
        ImageDoc* d = mk("rcs_s4", 8, 16);
        const float dd = std::ldexp(1.0f, -11);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                d->px()[(size_t)y * 8 + x] = 4096.0f + (x >= 4 && y < 8 ? dd : 0.0f);
        computeMinMax(*d);
        App::ProjState S;
        proj1(*d, S);
        const profile_noise::Result& N = S.noise[0];
        const Row2 r2 = perFrame2(d);
        const Row4 r4 = prnu4(*d);
        const double A4 = std::ldexp(8.0 / 7, -26), B4 = std::ldexp(16.0 / 15, -26);
        const double M4 = 4096.0 + std::ldexp(1.0, -13);
        fprintf(stderr, "rowcolsigmaselftest: S4 #1 sigma_col=%.17g sigma_row=%.17g "
                        "(profile values %.17g..%.17g)\n",
                S.hStat[0].sd, S.vStat[0].sd, (double)S.hMin, (double)S.hMax);
        fprintf(stderr, "rowcolsigmaselftest: S4 #2 col%%=%.12g row%%=%.12g   "
                        "#3 A=%.12g B=%.12g   #4 '%s'\n",
                r2.colPct, r2.rowPct, planeA(N), planeB(N), r4.note.c_str());

        check(S.hStat[0].valid && S.hStat[0].sd == 0.0 && S.vStat[0].sd == 0.0 &&
              S.hStat[0].pct == 0.0,
              "S4 #1 the float profile collapses: sigma_col and sigma_row are 0");
        check(S.hMin == S.hMax && S.hMin == 4096.0f,
              "S4 ...because all eight column means rounded to the same float");
        check(r2.got && near9(r2.colPct, std::sqrt(A4) / M4 * 100.0) &&
              near9(r2.rowPct, std::sqrt(B4) / M4 * 100.0),
              "S4 #2 in double: 3.1860022e-6 % and 3.0779705e-6 %, both non-zero");
        check(N.valid() && near_(planeA(N), A4, 1e-12) && near_(planeB(N), B4, 1e-12),
              "S4 #3 in double: A = 2^-26*8/7 and B = 2^-26*16/15");
        check(near_(N.pVariance, 1357.0 / 90887946240.0, 1e-10) && !N.pClamped,
              "S4 #3 ...and p = 1357/90887946240, so the plane is not degenerate");
        check(!r4.got && r4.note == "region too small",
              "S4 #4 declines an 8-wide plane: under its 9-tap blur's minimum");
        // THE POINT OF THIS STAGE, said as one assert: two of the four print a
        // real column-to-column difference and one prints nothing.
        check(S.hStat[0].sd == 0.0 && r2.colPct > 0.0 && planeA(N) > 0.0,
              "S4 same pixels: #1 says 0, #2 and #3 both measure it");
    }

    // =====================================================================
    // S5 - A MOSAIC, which none of S1-S4 is. 32x32 RGGB, and each plane is an
    // INDEPENDENT 16x16 bilinear ramp on its own pedestal:
    //
    //   i = x >> 1, j = y >> 1              (both 0..15, whatever the phase)
    //   p = cfaChannelAt(x, y)              (RGGB: p = (y&1)*2 + (x&1))
    //   v = 1000 + 100*p + i + 4*j + 8*e    with e = +1 when (i+j) is even,
    //                                            -1 when it is odd
    //
    // e sums to zero along every i and along every j of a plane (16 alternating
    // terms), so it leaves EVERY column mean and EVERY row mean of every plane
    // untouched while adding pixel variance - S1's trick, so that p > 0 and the
    // decomposition is not clamped. Its cross terms with i and with 4j vanish
    // for the same reason.
    //
    // Per plane, over that plane's OWN 16 columns and 16 rows:
    //   column means  c_i = 1030   + 100p + i     (slope 1)
    //   row means     r_j = 1007.5 + 100p + 4j    (slope 4)
    //   plane mean    M_p = 1037.5 + 100p
    //   A = Var_ddof1(c) = 16*17/12       = 68/3
    //   B = Var_ddof1(r) = 16 * 68/3      = 1088/3
    //   Var_pop(plane) = 255/12 + 16*255/12 + 64 = 425.25
    //   T = Var_ddof1(plane) = 425.25 * 256/255   = 36288/85
    //   denom = 1 - 1/16 - 1/16 = 7/8
    //   p = (T - A - B) * 8/7             = 84832/1785 (> 0)
    //   rowVariance = B - p/16 ; colVariance = A - p/16
    //
    // A and B do NOT depend on p, and the MEAN does - by exactly 100 per plane.
    // That is the fixture's whole design: a site reading the profile at the
    // wrong phase gets another plane's pedestal and misses by 100 DN, a site
    // folding the interleaved NaNs in gets a different n, and a site dividing
    // by the frame's 32 columns instead of the plane's 16 gets neither A nor B.
    // =====================================================================
    {
        const double A5 = 68.0 / 3, B5 = 1088.0 / 3;
        const double T5 = 36288.0 / 85;
        const double P5 = (T5 - A5 - B5) * 8.0 / 7;
        const double ROWV5 = B5 - P5 / 16, COLV5 = A5 - P5 / 16;
        ImageDoc* d = mk("rcs_s5", 32, 32);
        d->cfa = 1; d->cfaPattern = 0;                 // RGGB
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                const int i = x >> 1, j = y >> 1;
                const int pl = cfaChannelAt(*d, x, y);
                d->px()[(size_t)y * 32 + x] =
                    (float)(1000 + 100 * pl + i + 4 * j + (((i + j) & 1) ? -8 : 8));
            }
        computeMinMax(*d);
        App::ProjState S;
        proj1(*d, S);
        check(S.nSeries == 4 && S.cell == 2,
              "S5 fixture: four planes on a 2x2 mosaic");
        check(P5 > 0, "S5 fixture: the residual keeps p positive, as S1's does");
        static const char* PN[4] = { "R", "Gr", "Gb", "B" };
        for (int pl = 0; pl < 4; pl++) {
            const profile_noise::Result& N = S.noise[(size_t)pl];
            const Row2 r2 = perFrame2(d, (std::string("_") + PN[pl]).c_str());
            const Row4 r4 = prnu4(*d, PN[pl]);
            const double M5 = 1037.5 + 100.0 * pl;
            fprintf(stderr, "rowcolsigmaselftest: S5 %-2s #1 sigma_col=%.12g "
                            "sigma_row=%.12g mean=%.12g (profile %zu long)\n",
                    PN[pl], S.hStat[pl].sd, S.vStat[pl].sd, S.hStat[pl].mean,
                    S.h[pl].size());
            fprintf(stderr, "rowcolsigmaselftest: S5 %-2s #2 mean=%.12g col%%=%.12g "
                            "row%%=%.12g   #3 %dx%d A=%.12g B=%.12g p=%.12g   "
                            "#4 mean=%.12g row=%.12g col=%.12g%s\n",
                    PN[pl], r2.mean, r2.colPct, r2.rowPct, N.width, N.height,
                    planeA(N), planeB(N), N.pVariance,
                    r4.mean, r4.rowFpn, r4.colFpn,
                    r4.note.empty() ? "" : (" note=" + r4.note).c_str());

            char what[160];
            // ---- #1, over the PLANE'S OWN samples ----------------------
            // The profile is 32 long with a NaN at every column the plane does
            // not own, and the statistic is over the 16 it does. n = 16 is the
            // claim: fold the NaNs in and the mean is the same but the ddof=1
            // factor is 32/31 instead of 16/15, and A comes out 3.4% low.
            snprintf(what, sizeof what,
                     "S5 #1 %s: mean %.1f - the plane's OWN pedestal, not a neighbour's",
                     PN[pl], M5);
            check(S.hStat[pl].valid && S.hStat[pl].mean == M5 &&
                  S.vStat[pl].mean == M5, what);
            snprintf(what, sizeof what,
                     "S5 #1 %s: sigma_col = sqrt(68/3) and sigma_row = sqrt(1088/3)",
                     PN[pl]);
            check(near_(S.hStat[pl].sd, std::sqrt(A5), 1e-12) &&
                  near_(S.vStat[pl].sd, std::sqrt(B5), 1e-12), what);
            snprintf(what, sizeof what,
                     "S5 #1 %s: 32 entries, and the 16 the plane owns are the sample",
                     PN[pl]);
            check(S.h[pl].size() == 32 && S.v[pl].size() == 32 &&
                  near_(S.hStat[pl].pct, std::sqrt(A5) / M5 * 100.0, 1e-12), what);
            // ---- #2, the export's per-plane columns --------------------
            snprintf(what, sizeof what,
                     "S5 #2 %s: the same two percentages, in double, over the full plane",
                     PN[pl]);
            check(r2.got && near9(r2.mean, M5) &&
                  near9(r2.colPct, std::sqrt(A5) / M5 * 100.0) &&
                  near9(r2.rowPct, std::sqrt(B5) / M5 * 100.0), what);
            // ---- #3, the decomposition, per plane ----------------------
            snprintf(what, sizeof what,
                     "S5 #3 %s: a 16x16 PLANE, not a 32x32 frame", PN[pl]);
            check(N.valid() && N.width == 16 && N.height == 16, what);
            snprintf(what, sizeof what,
                     "S5 #3 %s: A = 68/3, B = 1088/3, p = 84832/1785, nothing clamped",
                     PN[pl]);
            check(near_(planeA(N), A5, 1e-12) && near_(planeB(N), B5, 1e-12) &&
                  near_(N.pVariance, P5, 1e-12) && !N.clamped(), what);
            snprintf(what, sizeof what,
                     "S5 #3 %s: rowVariance = B - p/16 and colVariance = A - p/16",
                     PN[pl]);
            check(near_(N.rowVariance, ROWV5, 1e-12) &&
                  near_(N.colVariance, COLV5, 1e-12), what);
            // ---- the RELATION #1 <-> #3, on the mosaic path ------------
            // S1 pins this for one plane; here it is pinned four times over,
            // at four phases, which is the part that had no assertion at all.
            snprintf(what, sizeof what,
                     "S5 %s: #1's sigma_col squared IS #3's A - same plane, same samples",
                     PN[pl]);
            check(near_(S.hStat[pl].sd * S.hStat[pl].sd, planeA(N), 1e-11) &&
                  near_(S.vStat[pl].sd * S.vStat[pl].sd, planeB(N), 1e-11), what);
            // ---- #4 reaches the mosaic and answers PER PLANE -----------
            // Its value is a 9-tap high-pass of the profile and has no short
            // closed form on a ramp, so what is asserted is that the mosaic
            // path reaches it and that the plane it answers about is this one -
            // the mean is the pedestal, and the pedestals are 100 apart.
            snprintf(what, sizeof what,
                     "S5 #4 %s: answers under the PLANE's key, about the plane's level",
                     PN[pl]);
            check(r4.got && near_(r4.mean, M5, 1e-6), what);
        }
        // ...and the four planes really are four different levels, so none of
        // the checks above could have passed by reading a neighbour.
        check(S.hStat[0].mean == 1037.5 && S.hStat[3].mean == 1337.5,
              "S5 the pedestals are 100 DN apart: a phase error cannot hide");
    }

    // The home is shared: put back every mode this file borrowed.
    closeAll();
    app.seqs.clear();
    app.projMode = saveMode;
    app.projAllRow = saveAllRow;
    app.selectedAnn = saveAnn;
    fprintf(stderr, "rowcolsigmaselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
