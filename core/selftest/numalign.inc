// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --numalign-selftest: a column of numbers lines up on its decimal point --
//
// Issue #313. Stepping frames on an A/B comparison, the Projection table's
// HFPN/RN (and every other value column) moved its "." from row to row and
// frame to frame: each cell was its own %.6g, right-aligned by the END of the
// string, a clamp "*" pushed its row's digits left, and nothing made a digit
// of a proportional font as wide as another. The ruling, as asserted here on
// the pure layout (inspector.inc numLayout / numColDecide - no window, no font,
// a glyph-width table written out below):
//
//   N1  auto decimals give the column's LARGEST |v| four significant digits,
//       0..6, from every row of the column
//   N2  hysteresis: one step fewer is held, two fewer or any more is taken at
//       once - for the decimals and for the integer width alike
//   N3  a different key (another comparison) starts the column again; a fixed
//       decimals setting is used as it is
//   N4  1.2345 / 12.345 / -0.1234 / a clamped 3.5* in one column: the "." of
//       each is at ONE x, every fraction has the column's decimals, the mark
//       sits right of the fraction without moving a digit
//   N5  digits advance by the width of "0" whatever their own width is: "1"
//       and "8" in a proportional table occupy the same slots
//   N6  the TSV row is byte-identical to the one main printed, whatever the
//       decimals setting and whatever the columns decided
//   N7  a word (nan) ends at the column's point; the column width covers the
//       header, the words and the block
//
// NOGL and fixture-free. app.projDecimals is borrowed and put back.
if (g_numAlignSelftest) {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "numalignselftest: %-72s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // A proportional font, written out: no two of these digits are one width,
    // and "0" (the advance every digit and the sign get) is none of the others.
    NumMetrics M;
    M.digit = 6.0f;
    M.point = 2.0f;
    M.textW = [](const std::string& s) {
        float w = 0;
        for (char ch : s) {
            switch (ch) {
            case '0': w += 6; break;
            case '1': w += 3; break;
            case '8': w += 7; break;
            case '.': w += 2; break;
            case '-': w += 4; break;
            case '*': w += 5; break;
            default:  w += 5.5f; break;
            }
        }
        return w;
    };
    auto feq = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    const std::string K = "A=stackA#1/1;B=stackB#1/1;plane=-1";

    // ---- N1 ---------------------------------------------------------------
    check(numAutoDecimals(1234.5) == 0 && numAutoDecimals(12.345) == 2 &&
          numAutoDecimals(1.2345) == 3 && numAutoDecimals(0.1234) == 4 &&
          numAutoDecimals(0.01234) == 5 && numAutoDecimals(0.001234) == 6 &&
          numAutoDecimals(0.0000123) == 6 && numAutoDecimals(99999) == 0,
          "N1 four significant digits of the largest magnitude, clamped to 0..6");
    check(numAutoDecimals(0) == 3 && numAutoDecimals(std::nan("")) == 3,
          "N1 ...and zero or nothing finite is 0.000");
    {
        NumColState st;
        const NumCol c = numColDecide(st, K, { 1.2345, 12.345, -0.1234 }, -1, 0, 0, 0, M);
        check(c.shape.dec == 2 && c.shape.intW == 2,
              "N1 the column {1.2345, 12.345, -0.1234}: 2 decimals (12.35), 2 integer chars");
        NumColState st2;
        const NumCol c2 = numColDecide(st2, K, { -0.1234, 0.05 }, -1, 0, 0, 0, M);
        check(c2.shape.dec == 4 && c2.shape.intW == 2,
              "N1 {-0.1234, 0.05}: 4 decimals, and the sign is an integer char (-0)");
    }

    // ---- N2 ---------------------------------------------------------------
    {
        NumColState st;
        int d[6];
        d[0] = numColDecide(st, K, { 12.3 }, -1, 0, 0, 0, M).shape.dec;    // 2
        d[1] = numColDecide(st, K, { 9.87 }, -1, 0, 0, 0, M).shape.dec;    // more: 3 at once
        d[2] = numColDecide(st, K, { 12.3 }, -1, 0, 0, 0, M).shape.dec;    // one fewer: held 3
        d[3] = numColDecide(st, K, { 10.01 }, -1, 0, 0, 0, M).shape.dec;   // still held
        d[4] = numColDecide(st, K, { 123.4 }, -1, 0, 0, 0, M).shape.dec;   // two fewer: 1
        d[5] = numColDecide(st, K, { 12.3 }, -1, 0, 0, 0, M).shape.dec;    // more: 2
        fprintf(stderr, "numalignselftest: N2 decimals %d %d %d %d %d %d\n",
                d[0], d[1], d[2], d[3], d[4], d[5]);
        check(d[0] == 2 && d[1] == 3, "N2 more decimals are taken at once");
        check(d[2] == 3 && d[3] == 3, "N2 one fewer is held while it stays one fewer");
        check(d[4] == 1 && d[5] == 2, "N2 two fewer is taken at once, and so is growing back");
        NumColState w;
        int iw[4];
        iw[0] = numColDecide(w, K, { 123.4 }, 1, 0, 0, 0, M).shape.intW;   // 3
        iw[1] = numColDecide(w, K, { 12.3 }, 1, 0, 0, 0, M).shape.intW;    // held 3
        iw[2] = numColDecide(w, K, { 1.2 }, 1, 0, 0, 0, M).shape.intW;     // 1
        iw[3] = numColDecide(w, K, { -1234.5 }, 1, 0, 0, 0, M).shape.intW; // 5
        check(iw[0] == 3 && iw[1] == 3 && iw[2] == 1 && iw[3] == 5,
              "N2 the integer width (so the column width) has the same hysteresis");
    }

    // ---- N3 ---------------------------------------------------------------
    {
        NumColState st;
        numColDecide(st, K, { 9.87 }, -1, 0, 0, 0, M);
        const int held = numColDecide(st, K, { 12.3 }, -1, 0, 0, 0, M).shape.dec;
        const int other = numColDecide(st, K + "C=stackC#1/1;", { 12.3 }, -1, 0, 0, 0, M).shape.dec;
        check(held == 3 && other == 2, "N3 another comparison starts the column again");
        const int fixed = numColDecide(st, K, { 12.3 }, 5, 0, 0, 0, M).shape.dec;
        const int fixed0 = numColDecide(st, K, { 12.3 }, 0, 0, 0, 0, M).shape.dec;
        const int back = numColDecide(st, K, { 12.3 }, -1, 0, 0, 0, M).shape.dec;
        check(fixed == 5 && fixed0 == 0 && back == 2,
              "N3 fixed decimals are used as set; returning to auto decides afresh");
        const float wSfx = numColDecide(st, K, { 1.5 }, -1, M.textW("*"), 0, 0, M).shape.sfxW;
        const float wKept = numColDecide(st, K, { 1.5 }, -1, 0, 0, 0, M).shape.sfxW;
        const float wNew = numColDecide(st, K + "x", { 1.5 }, -1, 0, 0, 0, M).shape.sfxW;
        check(feq(wSfx, 5) && feq(wKept, 5) && feq(wNew, 0),
              "N3 the mark slot stays once a row had it, until the key changes");
    }

    // ---- N4 ---------------------------------------------------------------
    {
        NumColState st;
        const std::vector<double> vals = { 1.2345, 12.345, -0.1234, 3.5 };
        const NumCol c = numColDecide(st, K, vals, -1, M.textW("*"), 0, 0, M);
        std::vector<NumLayout> L;
        for (size_t i = 0; i < vals.size(); i++)
            L.push_back(numLayout(numFixed(vals[i], c.shape.dec), i == 3 ? "*" : "", c.shape, M));
        auto dotX = [](const NumLayout& l) {
            for (const NumPiece& p : l.pieces) if (p.s == ".") return p.slotX;
            return -1.0f;
        };
        bool same = true, frac = true, intEnd = true, block = true;
        std::string spelled;
        for (const NumLayout& l : L) {
            same &= feq(dotX(l), dotX(L[0])) && feq(l.pointX, L[0].pointX) && dotX(l) > 0;
            frac &= l.fracDigits == c.shape.dec;
            block &= feq(l.width, L[0].width);
            // the digit before the point ends exactly where the point starts
            for (size_t k = 0; k + 1 < l.pieces.size(); k++)
                if (l.pieces[k + 1].s == ".")
                    intEnd &= feq(l.pieces[k].slotX + M.digit, l.pointX);
            for (const NumPiece& p : l.pieces) spelled += p.s;
            spelled += " ";
        }
        fprintf(stderr, "numalignselftest: N4 column '%s' point x %.2f\n", spelled.c_str(),
                L[0].pointX);
        check(same, "N4 the '.' of 1.2345, 12.345, -0.1234 and 3.5* is at one x");
        check(frac, "N4 ...every one of them has the column's decimals, not its own");
        check(intEnd, "N4 ...and each integer part ends where the point starts");
        check(block, "N4 ...and the block is one width, mark slot included");
        check(spelled == "1.23 12.35 -0.12 3.50* ", "N4 fixture: the column printed what the rule says");
        const NumPiece& star = L[3].pieces.back();
        check(star.s == "*" && feq(star.slotX, L[3].pointX + M.point + 2 * M.digit) &&
              feq(dotX(L[3]), dotX(L[1])),
              "N4 the clamp '*' sits after the column's last decimal and moves no digit");
    }

    // ---- N5 ---------------------------------------------------------------
    {
        NumShape s;
        s.dec = 2; s.intW = 2;
        const NumLayout a = numLayout("18.81", "", s, M);
        const NumLayout b = numLayout("81.18", "", s, M);
        bool slots = a.pieces.size() == 5 && b.pieces.size() == 5;
        for (size_t k = 0; slots && k < 5; k++) slots &= feq(a.pieces[k].slotX, b.pieces[k].slotX);
        check(slots, "N5 '1' and '8' at the same position occupy the same slot");
        check(a.pieces.size() == 5 && feq(a.pieces[1].slotX - a.pieces[0].slotX, M.digit) &&
              feq(a.pieces[4].slotX - a.pieces[3].slotX, M.digit) &&
              feq(a.pieces[3].slotX - a.pieces[2].slotX, M.point),
              "N5 digits advance by the width of '0', the point by '.'");
        check(a.pieces.size() == 5 && feq(a.pieces[0].x, a.pieces[0].slotX + 1.5f) &&
              feq(a.pieces[1].x, a.pieces[1].slotX - 0.5f),
              "N5 ...and a narrow or wide glyph is centred in its slot");
        const NumLayout m = numLayout("-1.00", "", s, M);
        check(m.pieces.size() == 5 && feq(m.pieces[0].slotX, 0) && feq(m.pieces[1].slotX, M.digit),
              "N5 the sign takes a digit's advance");
    }

    // ---- N6 ---------------------------------------------------------------
    {
        const int keepDecimals = app.projDecimals;
        App::ProjState P;
        P.nSeries = 1;
        P.seriesNames[0] = "R";
        P.fStat[0].mean = 1023.5; P.fStat[0].sd = 2.25; P.fStat[0].pct = 0.25;
        P.fStat[0].pp = 12; P.fStat[0].n = 48; P.fStat[0].valid = true;
        P.vStat[0].mean = 1023.5; P.vStat[0].sd = 0.5; P.vStat[0].pct = 0.125;
        P.vStat[0].pp = 2; P.vStat[0].valid = true;
        P.hStat[0].mean = 1023.5; P.hStat[0].sd = 0.75; P.hStat[0].pct = 0.0625;
        P.hStat[0].pp = 3; P.hStat[0].valid = true;
        P.noise[0].status = profile_noise::Status::Ready;
        P.noise[0].width = 8; P.noise[0].height = 6;
        P.noise[0].pVariance = 4; P.noise[0].rowVariance = 1; P.noise[0].colVariance = 0.25;
        P.noise[0].rowClamped = true;
        // the bytes main's projTsvRow printed for this state, written out
        const std::string MAIN_ROW =
            "A\tR\t1023.5\t2.25\t0.5\t0.75\t0.25\t0.125\t0.0625\t12\t2\t3\t48\t0.5\t0.25\t"
            "ok; negative variance clamped: row; 8x6 plane pixels, full ROI\n";
        app.projDecimals = -1;
        const std::string autoRow = projTsvRow(P, "A", false, 0);
        app.projDecimals = 1;
        const std::string fixedRow = projTsvRow(P, "A", false, 0);
        NumColState st;
        numColDecide(st, K, { 0.5, 0.25 }, -1, M.textW("*"), 0, 0, M);
        const std::string afterRow = projTsvRow(P, "A", false, 0);
        fprintf(stderr, "numalignselftest: N6 row '%s'\n", autoRow.c_str());
        check(autoRow == MAIN_ROW, "N6 the TSV row is main's, byte for byte");
        check(fixedRow == MAIN_ROW && afterRow == MAIN_ROW,
              "N6 ...whatever the decimals setting and the columns decided");
        const ProjCell hc = projNoiseCell(P, 0, true), vc = projNoiseCell(P, 0, false);
        check(hc.kind == ProjCell::Num && hc.v == 0.5 && hc.clamped && vc.v == 0.25 &&
              projNoiseCell(P, 4, true).kind == ProjCell::Dash,
              "N6 the screen's HFPN/RN cell is the TSV's unrounded value, marked clamped");
        app.projDecimals = keepDecimals;
    }

    // ---- N7 ---------------------------------------------------------------
    {
        NumShape s;
        s.dec = 3; s.intW = 3;
        const NumLayout w = numLayout(numFixed(std::nan(""), 3), "", s, M);
        check(w.pieces.size() == 1 && feq(w.pieces[0].slotX + M.textW(w.pieces[0].s), w.pointX),
              "N7 a word ends at the column's point");
        NumColState st;
        const NumCol c = numColDecide(st, K, { 1.5 }, -1, 0, M.textW("computing..."),
                                      M.textW("sigma (frame) %"), M);
        check(feq(c.width, std::max(numBlockWidth(c.shape, M), M.textW("sigma (frame) %"))) &&
              c.width >= M.textW("computing..."),
              "N7 the column width covers the block, its header and its words");
    }

    fprintf(stderr, "numalignselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
