// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---- --precision-selftest: the numbers on screen are the file's, or say so --
//
// The subject is the sharpest promise this program makes. README's first
// paragraph and docs/terminology.md both rest on the number shown being the
// number in the file; docs/input-adapters.md §4.8 states it outright - "an
// integer rides onto f32 with its value intact" - and lists i32 and f64 among
// the dtypes it displays. FrameSource::data is std::vector<float>, float32
// carries a 24-bit significand, and so §4.8's sentence is FALSE above 2^24 and
// f64 loses digits unconditionally. A .npy holding 16777217 in a u4 used to
// print 16777216, in an integer column, with every digit spelled out and
// nothing anywhere saying otherwise.
//
// What is asserted here is therefore NOT "the viewer holds f64" - it does not,
// and making it is a project (see the PR's judgment items). It is the narrower
// and non-negotiable half: A NUMBER THIS PROGRAM CANNOT REPRESENT MUST NOT BE
// SHOWN AS IF IT WERE EXACT. Either the true value, or a statement that it is
// not the true value.
//
// Every fixture is written here, from integers and doubles chosen so the
// expected answer is arithmetic rather than a measurement of the code under
// test: 2^24+1 is the first integer float32 cannot hold, 4294967295 is a u32
// this program renders as 4294967296 (a value a u32 cannot even take),
// 1.0000000001 differs from 1 in the tenth digit, and 1e300 is a finite number
// that becomes inf. The fixtures that must produce NO complaint are half the
// test: a hedge that fires on the dtype would cry wolf on the f64 files that
// are perfectly fine, which is why the census is measured per array.
//
// NOGL, on the roiStatsSelftest() reasoning: every assertion is a number or a
// string. It asserts against fmtVal() and rp::f32LossNote() - the production
// formatter the Inspector's pixel table calls and the production sentence the
// Inspector's amber line prints, both by the same call with the same arguments
// - rather than by driving an ImGui frame, because unlike the ROI table (a
// clipper and a hash cache, which is why V19/roistats drives the real panel)
// there is no machinery between those two strings and the screen: the panel is
// `if (any()) TextWrapped(f32LossNote(...))`.
if (!g_precisionSelftest.empty()) {
    const std::string& dir = g_precisionSelftest;
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "precisionselftest: %-64s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };

    // A scratch subdirectory this test CREATES - never the directory it was
    // handed, which is a shared fixture root (the stackavg / derive discipline).
    std::string root = dir;
    std::replace(root.begin(), root.end(), '\\', '/');
    while (!root.empty() && root.back() == '/') root.pop_back();
    root += "/precision-scratch";
    std::error_code ec;
    std::filesystem::remove_all(pathFromUtf8(root), ec);
    std::filesystem::create_directories(pathFromUtf8(root), ec);

    // One .npy of ANY dtype, from raw sample bytes. The existing writers in
    // this tree are all f4-only (stackavg's takes std::vector<float>), and the
    // whole subject here is the dtypes that are not f4.
    auto writeNpy = [&](const std::string& name, const char* descr, int h, int w,
                        const void* payload, size_t bytes) {
        char dict[160];
        snprintf(dict, sizeof dict,
                 "{'descr': '%s', 'fortran_order': False, 'shape': (%d, %d), }", descr, h, w);
        std::string hdr = dict;
        size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
        hdr.append(pad, ' ');
        hdr += '\n';
        std::ofstream f(pathFromUtf8(root + "/" + name), std::ios::binary);
        uint16_t hl = (uint16_t)hdr.size();
        f.write("\x93NUMPY\x01\x00", 8);
        f.write((const char*)&hl, 2);
        f.write(hdr.data(), (std::streamsize)hdr.size());
        f.write((const char*)payload, (std::streamsize)bytes);
    };
    auto open1 = [&](const std::string& name) {
        std::string err;
        auto d = decodeNpy(root + "/" + name, err);
        if (!d) fprintf(stderr, "precisionselftest: %s did not open: %s\n",
                        name.c_str(), err.c_str());
        return d;
    };
    // What the Inspector's pixel table prints for one pixel: the production
    // call, verbatim (core/ui/inspector.inc's `fmtVal(v, im->dtype)`).
    auto shown = [&](const ImageDoc& d, int x, int y) {
        return fmtVal(d.sample(x, y, 0), d.dtype);
    };

    // ---------------------------------------------------------------- P1: u4
    // Sixteen u32 values straddling 2^24. Row 0 is entirely inside float32's
    // exact-integer range; rows 1-3 walk out of it. Which of these sixteen
    // survive is arithmetic and is stated per value below.
    const uint32_t U4[16] = {
        0u, 1u, 65535u, 16777215u,                 // all exact
        16777216u,                                 // 2^24 - the last exact one
        16777217u,                                 // ...and the FIRST that is not
        16777218u, 16777219u,                      // even survives, odd does not
        33554432u, 33554433u, 33554434u, 33554436u,// past 2^25: step is 2, then 4
        4294967295u,                               // u32 max -> 4294967296, not a u32
        4000000001u, 2147483647u, 123456789u,
    };
    writeNpy("u4_straddle.npy", "<u4", 4, 4, U4, sizeof U4);
    // The same dtype, entirely inside the exact range: this file must draw no
    // complaint whatsoever. Half the point of measuring rather than hedging.
    const uint32_t U4OK[4] = { 0u, 4095u, 65535u, 16777215u };
    writeNpy("u4_fits.npy", "<u4", 1, 4, U4OK, sizeof U4OK);

    auto du4 = open1("u4_straddle.npy");
    auto du4ok = open1("u4_fits.npy");
    if (!du4 || !du4ok) { fprintf(stderr, "precisionselftest: fixtures did not open\n"); return 1; }

    // THE CENSUS, printed whether it passes or fails: file value, held value,
    // and the string the Inspector puts on screen for it. This table IS the
    // evidence - a reader must be able to see the defect in the output of the
    // run that fails, not only in the name of the assertion.
    int u4bad = 0;
    for (int i = 0; i < 16; i++) {
        const int x = i % 4, y = i / 4;
        const double file = (double)U4[i];
        const float held = du4->sample(x, y, 0);
        const bool exact = (double)held == file;
        if (!exact) u4bad++;
        fprintf(stderr, "precisionselftest: P1 u4 file=%-10s held=%-14s shown=%-12s %s\n",
                rp::exactNum(file).c_str(), rp::exactNum((double)held).c_str(),
                shown(*du4, x, y).c_str(), exact ? "exact" : "NOT THE FILE'S VALUE");
    }
    // 16777217 / 16777219 / 33554433 / 33554434 / 4294967295 / 4000000001 /
    // 2147483647 / 123456789 - eight of the sixteen. Counted by hand from the
    // list above and asserted as a number, so a census that silently stopped
    // counting fails.
    check(u4bad == 8, "P1a eight of the sixteen u32 values are not representable");
    check(du4->src->f32loss.inexact == 8 && du4->src->f32loss.total == 16,
          "P1b the loader COUNTED them: 8 of 16");
    // The quoted sample is the largest RELATIVE deviation, which for a u32 is
    // the smallest magnitude that fails - 1 part in 16777217, against 1 part in
    // 4294967295 for the u32 max. That is the useful end: it is the level at
    // which this file stopped being readable, not the level at which the error
    // in DN happens to be largest (which is a tie - they are all one DN).
    check(du4->src->f32loss.from == 16777217.0 && du4->src->f32loss.to == 16777216.0,
          "P1c ...and NAMED the worst: 16777217 held as 16777216");

    // What is DISPLAYED. This is the assertion the whole task turns on.
    check(shown(*du4, 3, 0) == "16777215",
          "P1d 16777215 has no ambiguous neighbour and prints unmarked");
    // 2^24 IS exactly representable and is marked anyway, because 2^24+1 lands
    // on it: a float holding exactly 2^24 could have come from either file, so
    // the digits cannot be vouched for. The two assertions below are the same
    // float printed for two different files, and they must read alike.
    check(shown(*du4, 0, 1) == "~16777216",
          "P1e 2^24 is representable but AMBIGUOUS, so it is marked");
    check(shown(*du4, 1, 1) == "~16777216",
          "P1f the file's 16777217 is NOT shown as an exact 16777216");
    check(shown(*du4, 0, 3) == "~4294967296",
          "P1g the file's 4294967295 is NOT shown as an exact 4294967296");
    // ...and the sentence the Inspector puts beside them.
    const std::string s1 = rp::f32LossNote(du4->src->f32loss, du4->dtype);
    fprintf(stderr, "precisionselftest: P1 says | %s\n", s1.c_str());
    check(s1.find("8 of 16 sample(s) are NOT the value the file holds") != std::string::npos &&
          s1.find("16777217 is held as 16777216") != std::string::npos,
          "P1h the Inspector says how many, and quotes one");

    // No false alarm: same dtype, values that fit.
    check(!du4ok->src->f32loss.any() && du4ok->src->f32loss.total == 4,
          "P2a a u32 file whose values FIT says nothing (measured, not hedged)");
    check(rp::f32LossNote(du4ok->src->f32loss, du4ok->dtype).empty(),
          "P2b ...so the Inspector prints no line for it");
    check(shown(*du4ok, 3, 0) == "16777215", "P2c ...and its values print unmarked");

    // ---------------------------------------------------------------- P3: i4
    // The negative half of the same boundary, plus INT32_MIN, which IS exactly
    // representable (it is -2^31) and must therefore not be complained about.
    const int32_t I4[4] = { -16777216, -16777217, 16777217, -2147483647 - 1 };
    writeNpy("i4_straddle.npy", "<i4", 1, 4, I4, sizeof I4);
    auto di4 = open1("i4_straddle.npy");
    if (!di4) return 1;
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "precisionselftest: P3 i4 file=%-12s held=%-14s shown=%s\n",
                rp::exactNum((double)I4[i]).c_str(),
                rp::exactNum((double)di4->sample(i, 0, 0)).c_str(),
                shown(*di4, i, 0).c_str());
    check(di4->src->f32loss.inexact == 2 && di4->src->f32loss.total == 4,
          "P3a i32: 2 of 4 (-2^31 is exact, and is not COUNTED as a loss)");
    check(shown(*di4, 0, 0) == "~-16777216", "P3b -2^24 is ambiguous, so it is marked");
    check(shown(*di4, 1, 0) == "~-16777216", "P3c the file's -16777217 is marked");
    check(shown(*di4, 3, 0) == "~-2147483648",
          "P3d -2^31 is EXACT and still marked: the census and the marker answer "
          "different questions");

    // ---------------------------------------------------------------- P4: f8
    // The tenth digit. 1.0000000001 and 1.0 differ in the place §4.8's promise
    // is about, and float32 cannot tell them apart at all. The other three
    // values are exactly representable on purpose, so the quoted sample can
    // only be the one this assertion is about.
    const double F8D[4] = { 1.0000000001, 1.0, 0.5, 1024.25 };
    writeNpy("f8_digits.npy", "<f8", 1, 4, F8D, sizeof F8D);
    auto df8 = open1("f8_digits.npy");
    if (!df8) return 1;
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "precisionselftest: P4 f8 file=%-20s held=%-24s shown=%s\n",
                rp::exactNum(F8D[i]).c_str(),
                rp::exactNum((double)df8->sample(i, 0, 0)).c_str(),
                shown(*df8, i, 0).c_str());
    // Exactly one of the four fails, and it is the tenth-digit one.
    check(df8->src->f32loss.inexact == 1 && df8->src->f32loss.total == 4,
          "P4a f64: 1 of 4 - the three representable values are not counted");
    check((double)df8->sample(0, 0, 0) == 1.0,
          "P4b the file's 1.0000000001 is held as exactly 1.0 - the digit is GONE");
    const std::string s4 = rp::f32LossNote(df8->src->f32loss, df8->dtype);
    fprintf(stderr, "precisionselftest: P4 says | %s\n", s4.c_str());
    check(s4.find("1.0000000001 is held as 1") != std::string::npos,
          "P4c ...and the Inspector quotes the file's digits back, all ten of them");

    // f64 that fits: written from values a float32 holds exactly. No complaint.
    const double F8OK[4] = { 0.0, 1.0, 0.5, 1024.25 };
    writeNpy("f8_fits.npy", "<f8", 1, 4, F8OK, sizeof F8OK);
    auto df8ok = open1("f8_fits.npy");
    if (!df8ok) return 1;
    check(!df8ok->src->f32loss.any() && df8ok->src->f32loss.total == 4,
          "P4d an f64 file whose values FIT says nothing either");

    // ---------------------------------------------------------------- P5: f8
    // Not precision - EXISTENCE. float32's range is narrower than f64's, so a
    // finite measurement becomes inf and a small one becomes 0. This is the
    // failure mode that survives any amount of "it is only the last digits".
    const double F8R[4] = { 1e300, 1e-300, -1e300, 1.0 };
    writeNpy("f8_range.npy", "<f8", 1, 4, F8R, sizeof F8R);
    auto df8r = open1("f8_range.npy");
    if (!df8r) return 1;
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "precisionselftest: P5 f8 file=%-12s held=%-12s shown=%s\n",
                rp::exactNum(F8R[i]).c_str(),
                rp::exactNum((double)df8r->sample(i, 0, 0)).c_str(),
                shown(*df8r, i, 0).c_str());
    check(!std::isfinite(df8r->sample(0, 0, 0)) && df8r->sample(1, 0, 0) == 0.0f,
          "P5a a finite 1e300 becomes inf and 1e-300 becomes 0");
    check(df8r->src->f32loss.inexact == 3 && df8r->src->f32loss.total == 4,
          "P5b ...counted as loss, all three of them");

    // ---------------------------------------------------------------- P6
    // WHERE IT SHOWS BEYOND ONE PIXEL. The ROI statistics are the second
    // surface a measurement is read off, and they fold the float32 samples: the
    // panel's own accumulator is double, which does not recover anything,
    // because what it accumulates is already the wrong number. The exact mean
    // of the sixteen u32 values is arithmetic; the panel's is not it.
    //
    // Asserted as an implication, which is the contract of this whole change:
    // WHEREVER A SURFACE CANNOT SHOW THE FILE'S NUMBER, THE DOCUMENT SAYS SO.
    // The ROI mean is not fixed here (fixing it is the wide-type project), so
    // what must hold is that it is not presented in silence.
    double exactSum = 0;
    for (uint32_t v : U4) exactSum += (double)v;
    const double exactMean = exactSum / 16.0;
    RoiStat rs = roiBasicStatsUncached(*du4, 0, 0, du4->w, du4->h, -1);
    fprintf(stderr, "precisionselftest: P6 ROI mean | file %s | panel %.10g | "
                    "off by %s DN\n",
            rp::exactNum(exactMean).c_str(), rs.mean,
            rp::exactNum(rs.mean - exactMean).c_str());
    check(rs.valid && rs.mean != exactMean,
          "P6a the ROI mean of this file is NOT the mean of this file's values");
    check(du4->src->f32loss.any(),
          "P6b ...and the document declares that its samples are not the file's");
    // The same panel over the file that fits must agree to the last bit, or the
    // implication above would be vacuous.
    double okSum = 0;
    for (uint32_t v : U4OK) okSum += (double)v;
    RoiStat rok = roiBasicStatsUncached(*du4ok, 0, 0, du4ok->w, du4ok->h, -1);
    check(rok.valid && rok.mean == okSum / 4.0 && !du4ok->src->f32loss.any(),
          "P6c and where nothing was lost, the ROI mean IS exact and says nothing");

    fprintf(stderr, "precisionselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
