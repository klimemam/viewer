// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --roi-export-selftest: the ROIs table as a DOCUMENT (issue #67) --------
//
// The ROIs panel had no clipboard surface at all: the numbers were readable on
// screen and could not reach a report. What makes this table different from
// every other one the viewer exports is that it carries WHERE the measurement
// was taken - the rectangle itself - as a value. On screen a row shows the
// ROI's name and its "WxH @X,Y"; a copy that dropped the geometry would paste
// numbers nobody could place. So x / y / w / h are first-class COLUMNS here,
// and the assertions below are as much about them as about the statistics.
//
// The format is the one docs/export-design.md 2 confirmed for the Temporal
// export (issue #61): '#'-headed sections, each section a complete rectangle,
// %.9g, ASCII. This is that format applied, never a second one - the builder
// uses the same ExportDoc / expCell / expNum.
//
// Windowless and analytic, on the roiStatsSelftest() pattern and for its
// reasons: buildRoiExport() is pure CPU (no ImGui frame is drawn at all), the
// fixtures are built in memory so no fixture directory can drift under the
// test, and every expected value below is derived BY HAND from the fixture in
// the comment beside it. Where a number is asserted as a STRING it is because
// the arithmetic is exact in double for that fixture - checked, not assumed:
// the accumulator's partial sums are integers under 2^53, so sum2/n - mean^2
// loses nothing and the app's double is bit-for-bit the test's.
static int roiExportSelftest() {
    bool ok = true;
    int nCheck = 0;
    auto check = [&](bool cond, const char* what) {
        nCheck++;
        fprintf(stderr, "roiexportselftest: %-64s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    const size_t npos = std::string::npos;
    auto has = [&](const std::string& d, const std::string& s) { return d.find(s) != npos; };
    // %.9g, the export's own spelling of a number - the same call expNum makes.
    // What is hand-derived is the VALUE passed in, never the formatting.
    auto num = [](double v) { char b[40]; snprintf(b, sizeof b, "%.9g", v); return std::string(b); };
    // the whole of the first line that begins with `pre`, or "" - so a row that
    // is MISSING fails the comparison instead of matching a truncated one
    auto lineFrom = [&](const std::string& d, const std::string& pre) {
        size_t k = d.find("\n" + pre);
        if (k == npos) return std::string();
        size_t e = d.find('\n', k + 1);
        return d.substr(k + 1, e == npos ? e : e - k - 1);
    };
    // a row asserted whole: print what came out either way, because the useful
    // half of a failure here is the line the builder actually wrote
    auto rowIs = [&](const std::string& d, const std::string& pre,
                     const std::string& want, const char* what) {
        std::string got = lineFrom(d, pre);
        fprintf(stderr, "roiexportselftest:   got  '%s'\n", got.c_str());
        if (got != want) fprintf(stderr, "roiexportselftest:   want '%s'\n", want.c_str());
        check(got == want, what);
    };
    // every non-'#' line between two offsets carries the same number of cells as
    // its header: docs/export-design.md 2's "each section is itself a complete
    // rectangle". Same reader as --export-tsv-selftest's.
    auto rect = [&](const std::string& d, size_t from, size_t to, const char* what) {
        int want = -1;
        bool good = true, any = false;
        size_t i = d.find('\n', from);
        i = i == npos ? to : i + 1;
        while (i < to && i < d.size()) {
            size_t e = d.find('\n', i);
            if (e == npos || e > to) e = to;
            if (e > i && d[i] != '#') {
                int tabs = (int)std::count(d.begin() + i, d.begin() + e, '\t');
                if (want < 0) want = tabs;
                else if (tabs != want) good = false;
                any = true;
            }
            i = e + 1;
        }
        check(good && any && want > 0, what);
    };

    // ---- the fixtures ------------------------------------------------------
    // Every row identical, so any full-width region gives exactly
    // mean = base (+rightOff on the right half), sd = amp, min/max = mean -+ amp.
    auto mkMono = [](const char* nm, int W2, int H2, double base, double amp, double rightOff) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
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
    // per-plane level plus a within-plane dither of period 4 in x, so a plane's
    // own sd is amp and is NOT zero (a flat plane would let a broken sd read 0
    // and look right)
    static const double PLV[4] = { 1000, 2000, 3000, 4000 };
    auto mkCfa = [](const char* nm, int W2, int H2, double amp) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
        d->cfa = 1; d->cfaPattern = 0; d->src->dtype = "u16";     // RGGB
        d->src->data.resize((size_t)W2 * H2);
        for (int y = 0; y < H2; y++)
            for (int x = 0; x < W2; x++)
                d->px()[(size_t)y * W2 + x] =
                    (float)(PLV[CFA_MAP[0][(y & 1) * 2 + (x & 1)]] +
                            (((x >> 1) & 1) ? amp : -amp));
        d->syncMirrors();
        d->uid = app.nextUid++;
        return d;
    };
    auto resetAnns = [] {
        app.anns.clear();
        app.nextAnnId = 1; app.roiSeq = 0; app.poiSeq = 0; app.selectedAnn = 0;
    };

    // ======================================================================
    // X1: one mono side. The document exists, says where it came from, states
    // its coordinate convention, and carries the rectangles.
    // ======================================================================
    closeAll();
    app.cmpExtra.clear();
    app.compareMode = App::CmpOff;
    app.compareFollowFrame = false;
    app.roiChannel = -1;
    // 64x48, 30000 -+ 2 DN, and +1000 DN on the right half (x >= 32)
    app.images.push_back(mkMono("x67mono", 64, 48, 30000.0, 2.0, 1000.0));
    app.current = 0;
    resetAnns();
    addAnn(0, 4, 6, 16, 8);        // ROI 1 - wholly in the left half
    addAnn(0, 40, 6, 16, 8);       // ROI 2 - wholly in the right half
    addAnn(1, 5, 7, 0, 0);         // P1    - one pixel, left half, odd x

    std::string tsv = buildRoiExport('\t');
    fputs(tsv.c_str(), stdout);    // the document itself, for eyeballs and diffs
    const int lines = (int)std::count(tsv.begin(), tsv.end(), '\n');
    fprintf(stderr, "roiexportselftest: X1 document: %zu bytes, %d lines\n", tsv.size(), lines);
    // ANTI-VACUITY (1 of 3): a builder that returned "" - which is exactly what
    // this panel did before issue #67 - would satisfy any find()-based assertion
    // that was written the wrong way round. Nothing below is worth reading if
    // this fails.
    check(tsv.size() > 800 && lines > 15, "X1 anti-vacuity: there IS a document");
    check(has(tsv, "# ROI / POI statistics"), "X1 title line");
    check(has(tsv, "# app: viewer "), "X1 provenance: app + version");
    check(has(tsv, "# sides: A"), "X1 provenance: sides listed");
    check(has(tsv, "# A: x67mono") && has(tsv, "path:"),
          "X1 provenance: side name and path");
    check(has(tsv, "64x48 u16"), "X1 provenance: size and dtype");
    // THE statement this issue turns on: a pasted rectangle means nothing until
    // the file says which corner and which direction. Top-left, 0-based, y down,
    // half-open - and it is docs/export-design.md 10 that this quotes.
    check(has(tsv, "x,y = the ROI's TOP-LEFT pixel") &&
          has(tsv, "0-based") && has(tsv, "x to the right and y downwards"),
          "X1 the coordinate convention is STATED, corner and direction");
    check(has(tsv, "x..x+w-1") && has(tsv, "[x, x+w)"),
          "X1 ...and its inclusivity, both ways of saying it");
    check(has(tsv, "docs/export-design.md"), "X1 ...and where it is documented");
    // ASCII ONLY (docs/export-design.md 3). Not decoration: this document is
    // pasted into Excel and read by pandas, and a section sign or a sigma
    // arrives there as whatever the reader's code page makes of it. Caught by
    // this assertion on the first implementation, which cited "§10" and put a
    // U+00A7 in the header of every export.
    {
        size_t bad = npos;
        for (size_t i = 0; i < tsv.size(); i++)
            if ((unsigned char)tsv[i] > 0x7f) { bad = i; break; }
        if (bad != npos)
            fprintf(stderr, "roiexportselftest:   first non-ASCII byte at %zu: '%s'\n",
                    bad, tsv.substr(bad < 40 ? 0 : bad - 40, 80).c_str());
        check(bad == npos, "X1 the document is ASCII, every byte of it");
    }

    size_t s1 = tsv.find("== ROI statistics");
    size_t s2 = tsv.find("== POI values");
    check(s1 != npos && s2 != npos && s1 < s2,
          "X1 two sections, scalars-per-region first (export-design 2)");
    // the geometry is IN the header, next to the numbers, not in a comment
    check(has(tsv, "side\troi\tx\ty\tw\th\tch\tfold\tmean [DN]\tstd [DN]\t"
                   "std / mean [%]\tmin [DN]\tmax [DN]\tn\tstride\tnote"),
          "X1 ROI section header: x/y/w/h are columns of the table");
    check(has(tsv, "side\tpoi\tx\ty\tch\tvalue [DN]\tnote"),
          "X1 POI section header");
    rect(tsv, s1, s2, "X1 ROI section is a rectangle");
    rect(tsv, s2, tsv.size(), "X1 POI section is a rectangle");

    // The All row: the whole 64x48 frame. Values 29998 / 30002 / 30998 / 31002
    // in equal quarters -> mean 30500, var = 500^2 + 2^2 = 250004 (population,
    // ddof=0 - docs/stats-taxonomy.md 7 item 1), min 29998, max 31002,
    // n = 3072 with no decimation (3072 < the 200000-sample budget) at stride 1.
    rowIs(tsv, "A\tAll (whole image)\t",
          "A\tAll (whole image)\t0\t0\t64\t48\tall\t-\t" + num(30500.0) + "\t" +
          num(sqrt(250004.0)) + "\t" + num(sqrt(250004.0) / 30500.0 * 100.0) +
          "\t" + num(29998.0) + "\t" + num(31002.0) + "\t3072\t1\t",
          "X1 All row: each side's own frame, as a rectangle 0,0 64x48");
    // ROI 1 at (4,6) 16x8 is wholly in the left half: 128 px, mean 30000,
    // var = 2^2 exactly, min 29998, max 30002, n = 128 at stride 1.
    rowIs(tsv, "A\tROI 1\t",
          "A\tROI 1\t4\t6\t16\t8\tall\t-\t" + num(30000.0) + "\t" + num(2.0) + "\t" +
          num(2.0 / 30000.0 * 100.0) + "\t" + num(29998.0) + "\t" + num(30002.0) +
          "\t128\t1\t",
          "X1 ROI 1: its rectangle and its numbers on one row");
    // ROI 2 at (40,6): same shape, right half, so +1000 DN on every value.
    rowIs(tsv, "A\tROI 2\t",
          "A\tROI 2\t40\t6\t16\t8\tall\t-\t" + num(31000.0) + "\t" + num(2.0) + "\t" +
          num(2.0 / 31000.0 * 100.0) + "\t" + num(30998.0) + "\t" + num(31002.0) +
          "\t128\t1\t",
          "X1 ROI 2: a different place, and different numbers");
    // ANTI-VACUITY (2 of 3): the three rows above must not be three copies of
    // one number. If the fixture ever went flat, every "the numbers are right"
    // assertion would pass against a table that said nothing.
    check(num(30500.0) != num(30000.0) && num(30000.0) != num(31000.0),
          "X1 anti-vacuity: the three regions really do differ");
    // P1 at (5,7): x is odd and left of 32, so the pixel is 30000 + 2 = 30002.
    // A POI is a VALUE, not a population - no sd, no min/max, hence its own
    // section rather than five blank columns (export-design 2).
    rowIs(tsv, "A\tP1\t", "A\tP1\t5\t7\tall\t" + num(30002.0) + "\t",
          "X1 POI row: one pixel, one value, its own section");

    // The rectangle is a VALUE of the row and not a caption: move the ROI and
    // both the geometry and the numbers must move with it. 44 is in the right
    // half, so a row that kept the old numbers is off by 1000 DN.
    findAnn(1)->x = 44;                              // ROI 1 is annotation id 1
    app.annRev++;
    {
        std::string moved = buildRoiExport('\t');
        rowIs(moved, "A\tROI 1\t",
              "A\tROI 1\t44\t6\t16\t8\tall\t-\t" + num(31000.0) + "\t" + num(2.0) + "\t" +
              num(2.0 / 31000.0 * 100.0) + "\t" + num(30998.0) + "\t" + num(31002.0) +
              "\t128\t1\t",
              "X1 moving the ROI moves its exported x AND its numbers");
        findAnn(1)->x = 4;                           // ...and back, for X2
        app.annRev++;
    }

    // ======================================================================
    // X2: the compared sides. The ROIs are ONE shared set (issue #76), so the
    // side is a column and the rectangle repeats down it - except the All row,
    // whose region is each side's OWN frame. That is why x/y/w/h cannot be a
    // header line, and it is the case that proves it.
    // ======================================================================
    app.images.push_back(mkMono("x67b", 64, 48, 20000.0, 2.0, 0.0));   // B
    app.images.push_back(mkMono("x67c", 32, 24, 40000.0, 2.0, 0.0));   // C, smaller
    app.abStatsLayout = App::AbSide;
    app.compareMode = App::CmpSplit;
    setCompareB(app.images[1].get());
    addCompareSlot(app.images[2].get());
    std::string tsv2 = buildRoiExport('\t');
    fprintf(stderr, "roiexportselftest: X2 document: %zu bytes, %d lines\n",
            tsv2.size(), (int)std::count(tsv2.begin(), tsv2.end(), '\n'));
    check(has(tsv2, "# sides: A, B, C"), "X2 every side on screen is in the document");
    // side-major, the order every export in this application uses
    // (docs/export-design.md 3.2): the file's row order never depends on what
    // the panel is doing with its column groups.
    size_t pA = tsv2.find("\nA\tAll"), pB = tsv2.find("\nB\tAll"), pC = tsv2.find("\nC\tAll");
    check(pA != npos && pB != npos && pC != npos && pA < pB && pB < pC,
          "X2 rows are side-major: A's block, then B's, then C's");
    rowIs(tsv2, "B\tROI 1\t",
          "B\tROI 1\t4\t6\t16\t8\tall\t-\t" + num(20000.0) + "\t" + num(2.0) + "\t" +
          num(2.0 / 20000.0 * 100.0) + "\t" + num(19998.0) + "\t" + num(20002.0) +
          "\t128\t1\t",
          "X2 B's row: the SAME rectangle, B's own numbers");
    // C is 32x24, so its All row measures 32x24 - a different region under the
    // same row name, which is the whole argument for a per-row geometry.
    rowIs(tsv2, "C\tAll (whole image)\t",
          "C\tAll (whole image)\t0\t0\t32\t24\tall\t-\t" + num(40000.0) + "\t" +
          num(2.0) + "\t" + num(2.0 / 40000.0 * 100.0) + "\t" + num(39998.0) + "\t" +
          num(40002.0) + "\t768\t1\t",
          "X2 the All row is each side's OWN frame, stated as its rectangle");
    // ROI 2 at (40,6) 16x8 does not fit in 32x24. roiBasicStats CLAMPS, so the
    // failure this refuses is not a blank - it is a plausible number measured on
    // the intersection, printed under the same ROI's name. Dashes, and the
    // reason on the same line.
    rowIs(tsv2, "C\tROI 2\t",
          "C\tROI 2\t40\t6\t16\t8\tall\t-\t-\t-\t-\t-\t-\t-\t-\t"
          "outside this frame (32x24)",
          "X2 a side that cannot hold the rectangle says so, on the row");
    rect(tsv2, tsv2.find("== ROI statistics"), tsv2.find("== POI values"),
         "X2 ...and the section is still a rectangle with it in");
    // ANTI-VACUITY (3 of 3): the three sides must really carry three different
    // levels, or "B's numbers are B's" is a claim about nothing.
    check(has(tsv2, "\t" + num(30500.0) + "\t") && has(tsv2, "\t" + num(20000.0) + "\t") &&
          has(tsv2, "\t" + num(40000.0) + "\t"),
          "X2 anti-vacuity: three sides, three levels, all three present");

    // ======================================================================
    // X3: a mosaic. Planes are never mixed: one row per plane, the pooled row
    // last and named 'all' - the Projection section's rule, and for its reason
    // (a pooled population is a different measurement, not a fifth plane).
    // ======================================================================
    app.cmpExtra.clear();
    setCompareB(nullptr);
    app.compareMode = App::CmpOff;
    closeAll();
    app.images.push_back(mkCfa("x67cfa", 64, 48, 3.0));
    app.current = 0;
    resetAnns();
    addAnn(0, 8, 4, 16, 8);        // 16x8 on whole CFA cells: 32 px per plane
    addAnn(1, 5, 7, 0, 0);         // RGGB at odd x, odd y -> the B plane
    std::string tsv3 = buildRoiExport('\t');
    fputs(tsv3.c_str(), stdout);
    check(has(tsv3, "CFA: RGGB (4 planes)"), "X3 provenance names the mosaic");
    // The sampler rounds its stride up to whole CFA cells, so a mosaic reports
    // stride 2 even where (as here) every row is in fact read. 128 px / 4 planes
    // = 32 samples each; each plane is its level -+ 3 DN, so sd = 3 exactly and
    // the ratio divides by that plane's OWN level: 0.3 / 0.15 / 0.1 / 0.075 %.
    for (int p = 0; p < 4; p++) {
        char pre[64];
        snprintf(pre, sizeof pre, "A\tROI 1\t8\t4\t16\t8\t%s\t", CFA_CH_NAMES[p]);
        rowIs(tsv3, pre,
              std::string(pre) + "-\t" + num(PLV[p]) + "\t" + num(3.0) + "\t" +
              num(3.0 / PLV[p] * 100.0) + "\t" + num(PLV[p] - 3.0) + "\t" +
              num(PLV[p] + 3.0) + "\t32\t2\t",
              p == 0 ? "X3 plane R: its own level, its own sigma, its own ratio"
                     : (p == 1 ? "X3 plane Gr" : (p == 2 ? "X3 plane Gb" : "X3 plane B")));
    }
    // The pooled row: 128 px over all four planes -> mean 2500 and a variance
    // that carries the LEVELS, ((1500^2+500^2+500^2+1500^2)/4) + 3^2 = 1250009.
    // min/max are the extremes of the whole mosaic, 997 and 4003.
    rowIs(tsv3, "A\tROI 1\t8\t4\t16\t8\tall\t",
          "A\tROI 1\t8\t4\t16\t8\tall\t-\t" + num(2500.0) + "\t" + num(sqrt(1250009.0)) +
          "\t" + num(sqrt(1250009.0) / 2500.0 * 100.0) + "\t" + num(997.0) + "\t" +
          num(4003.0) + "\t128\t2\t",
          "X3 the pooled 'all' row: a different measurement, not a fifth plane");
    {   // R, Gr, Gb, B, then all - fixed, and the pooled row LAST of its group
        size_t q[5];
        for (int p = 0; p < 4; p++)
            q[p] = tsv3.find(std::string("\nA\tROI 1\t8\t4\t16\t8\t") + CFA_CH_NAMES[p] + "\t");
        q[4] = tsv3.find("\nA\tROI 1\t8\t4\t16\t8\tall\t");
        bool asc = true;
        for (int p = 0; p < 4; p++) asc = asc && q[p] != npos && q[p] < q[p + 1];
        check(asc && q[4] != npos, "X3 plane order R/Gr/Gb/B with 'all' last");
    }
    // A pixel of a mosaic belongs to exactly ONE plane, and the row says which:
    // RGGB at (5,7) is B, whose level is 4000, dithered -3 at x=5. A row that
    // named the wrong plane would be off by a thousand DN.
    rowIs(tsv3, "A\tP1\t", "A\tP1\t5\t7\tB\t" + num(3997.0) + "\t",
          "X3 POI on a mosaic: the plane it lands on, and that plane's value");
    check(has(tsv3, "ddof=0") && has(tsv3, "flat-field-stats.md"),
          "X3 the note says which sigma this is, and where the named one lives");

    // ======================================================================
    // X4: the two sinks, one builder - and the two things a ROI table can put
    // in a cell that no other export can: a user-typed name, and a display
    // control the file must ignore.
    // ======================================================================
    {
        std::string csv = buildRoiExport(',');
        const auto nl = std::count(csv.begin(), csv.end(), '\n');
        // a length guard on both, or an empty document satisfies "same line
        // count" and "no tabs" at once - the two shapes of a vacuous pass here
        check(nl > 15 && nl == std::count(tsv3.begin(), tsv3.end(), '\n'),
              "X4 CSV and TSV are the same document, line for line");
        check(nl > 15 && csv.find('\t') == npos, "X4 CSV carries no tabs");
        check(has(csv, "1000,3,0.3,997,1003,32,2,"), "X4 ...and the same numbers");
    }
    // ROI names are FREE TEXT - the one cell in this application a user can put
    // a delimiter into. Nothing else exported has that exposure.
    findAnn(1)->label = "we,ird \"one\"\tx";
    app.annRev++;
    {
        std::string t = buildRoiExport('\t'), c = buildRoiExport(',');
        check(has(t, "A\twe,ird \"one\" x\t8\t4\t"),
              "X4 TSV: a tab inside a ROI name is squashed, the row stays a row");
        check(has(c, "A,\"we,ird \"\"one\"\"\tx\",8,4,"),
              "X4 CSV: a comma and a quote in a ROI name are RFC 4180 quoted");
    }
    findAnn(1)->label = "ROI 1";
    app.annRev++;
    // The channel combo is a DISPLAY control - it keeps the table narrow enough
    // to scan. The export is for reproduction, so it carries every plane
    // whatever the combo says (docs/export-design.md 3, the same rule that
    // makes the decimals setting not apply). Compared from the first section
    // marker on: the provenance genuinely differs, because it reports the
    // selector, and the generated-at stamp cannot be compared to itself.
    {
        app.roiChannel = -1;
        std::string all = buildRoiExport('\t');
        app.roiChannel = 2;                            // Gb only, on screen
        std::string gb = buildRoiExport('\t');
        app.roiChannel = -1;
        size_t ka = all.find("== ROI statistics"), kg = gb.find("== ROI statistics");
        check(ka != npos && kg != npos && all.substr(ka) == gb.substr(kg),
              "X4 the table does not change when the channel combo does");
        check(has(gb, "panel channel selector: Gb"),
              "X4 ...and the selector's state is provenance, so it is stated");
    }

    // ANTI-VACUITY, the last one: this test cannot report ok by having run
    // nothing. The count is the number of check() calls that actually reached
    // the printer above; 40 is under the number written (43 today, measured on
    // the failing run) and over any plausible early return.
    {
        const int ran = nCheck;
        check(ran >= 40, "X5 anti-vacuity: every assertion above really ran");
        fprintf(stderr, "roiexportselftest: %d assertions ran\n", ran);
    }

    resetAnns();
    app.abStatsLayout = App::AbAuto;
    app.compareFollowFrame = true;
    fprintf(stderr, "roiexportselftest: %s\n", ok ? "ok" : "FAILED");
    // the orderly shutdown every early-returning selftest does: an unjoined
    // worker at exit is std::terminate() under the last line printed
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
