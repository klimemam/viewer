// NOGL: deterministic single-frame spatial decomposition and its unavailable
// states. Expected rational constants are derived from the fixture's sums,
// not from calling the production estimator or reproducing it as an oracle.
if (g_profileNoiseSelftest) {
    using profile_noise::Status;
    bool ok = true;
    auto check = [&](bool condition, const char* what) {
        fprintf(stderr, "profilenoiseselftest: %-70s %s\n", what, condition ? "PASS" : "FAIL");
        if (!condition) ok = false;
    };
    auto approximatelyEqual = [](double a, double b) {
        return std::isfinite(a) && std::abs(a - b) <= 1e-10 * std::max(1.0, std::abs(b));
    };
    auto make = [](int w, int h, int ch = 1) {
        auto d = std::make_unique<ImageDoc>();
        d->src->w = w; d->src->h = h; d->src->ch = ch;
        d->src->dtype = "f32";
        d->px().resize((size_t)w * h * ch);
        d->syncMirrors();
        return d;
    };
    auto fill = [](ImageDoc& d, auto value) {
        for (int y = 0; y < d.h; ++y)
            for (int x = 0; x < d.w; ++x)
                for (int c = 0; c < d.ch; ++c)
                    d.px()[((size_t)y * d.w + x) * d.ch + c] = value(x, y, c);
    };
    auto whole = [](const ImageDoc& d) { return computeProfileNoise(d, 0, 0, d.w, d.h); };

    // 6x8: column signs +/-3, row signs +/-5, checkerboard +/-4.
    // Each component is balanced and orthogonal. Raw sum=0, sumsq=2400;
    // the six column means have sumsq=54, eight row means have sumsq=200.
    // Exact reduced expectations: p2=461616/27965, row2=722064/27965,
    // col2=244320/27965. Thus unequal H/V ratios cannot pass if swapped.
    auto d = make(6, 8);
    auto signal = [](int x, int y) {
        return ((x & 1) ? 3.0f : -3.0f) + ((y & 1) ? 5.0f : -5.0f) +
               (((x ^ y) & 1) ? 4.0f : -4.0f);
    };
    fill(*d, [&](int x, int y, int) { return signal(x, y); });
    const auto base = whole(*d)[0];
    check(base.valid() && base.width == 6 && base.height == 8 && base.samples == 48,
          "P1 one full 6x8 plane, 48 samples");
    check(approximatelyEqual(base.pVariance, 461616.0 / 27965) &&
          approximatelyEqual(base.rowVariance, 722064.0 / 27965) &&
          approximatelyEqual(base.colVariance, 244320.0 / 27965) && !base.clamped(),
          "P1 independent rational pixel/row/column variances");
    check(approximatelyEqual(base.rowRatio(), std::sqrt(15043.0 / 9617)) &&
          approximatelyEqual(base.colRatio(), std::sqrt(5090.0 / 9617)),
          "P1 horizontal banding is row/p; vertical banding is col/p");

    fill(*d, [&](int x, int y, int) { return 1048576.0f + signal(x, y); });
    const auto shifted = whole(*d)[0];
    check(shifted.pVariance == base.pVariance && shifted.rowVariance == base.rowVariance &&
          shifted.colVariance == base.colVariance,
          "P2 a large representable DC offset does not erase small variation");
    fill(*d, [&](int x, int y, int) { return -2.0f * signal(x, y); });
    const auto scaled = whole(*d)[0];
    check(approximatelyEqual(scaled.pVariance, 4 * base.pVariance) &&
          approximatelyEqual(scaled.rowVariance, 4 * base.rowVariance) &&
          approximatelyEqual(scaled.colVariance, 4 * base.colVariance) &&
          approximatelyEqual(scaled.rowRatio(), base.rowRatio()) && approximatelyEqual(scaled.colRatio(), base.colRatio()),
          "P2 scale/sign changes scale variances, preserve ratios");
    auto transposed = make(8, 6);
    fill(*transposed, [&](int x, int y, int) { return signal(y, x); });
    const auto tr = whole(*transposed)[0];
    check(approximatelyEqual(tr.pVariance, base.pVariance) && approximatelyEqual(tr.rowVariance, base.colVariance) &&
          approximatelyEqual(tr.colVariance, base.rowVariance), "P2 transpose swaps row/column only");

    fill(*d, [](int x, int y, int) { return ((x ^ y) & 1) ? 4.0f : -4.0f; });
    const auto residual = whole(*d)[0];
    check(residual.ratioValid() && approximatelyEqual(residual.pVariance, 18432.0 / 799) &&
          residual.rowRatio() == 0 && residual.colRatio() == 0 &&
          !residual.pClamped && residual.rowClamped && residual.colClamped,
          "P3 pure balanced residual resolves no bands, discloses both clamps");
    fill(*d, [](int, int y, int) { return (y & 1) ? 5.0f : -5.0f; });
    const auto rows = whole(*d)[0];
    check(rows.valid() && rows.pClamped && !rows.rowClamped && !rows.colClamped &&
          approximatelyEqual(rows.rowVariance, 200.0 / 7) && rows.colVariance == 0 &&
          !rows.ratioValid() && std::isnan(rows.rowRatio()),
          "P3 row-only input discloses negative p estimate and has no ratio");
    fill(*d, [](int, int, int) { return 123.0f; });
    const auto constant = whole(*d)[0];
    check(constant.valid() && !constant.ratioValid() && !constant.clamped() &&
          constant.pVariance == 0 && constant.rowVariance == 0 && constant.colVariance == 0 &&
          std::isnan(constant.rowRatio()) && std::isnan(constant.colRatio()),
          "P3 constant input has RN=0, no clamp and no invented finite ratio");

    for (const auto& shape : { std::pair<int, int>{1, 8}, {8, 1}, {2, 2} }) {
        auto small = make(shape.first, shape.second);
        const auto r = whole(*small)[0];
        check(r.status == Status::InsufficientPlane && !r.ratioValid(),
              "P4 degenerate 1xN/Nx1/2x2 denominator is unavailable");
    }
    for (float bad : { std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity() }) {
        d->px()[7] = bad;
        const auto r = whole(*d)[0];
        check(r.status == Status::NonFinite && r.samples == 48 && !r.ratioValid(),
              "P4 non-finite pixel rejects its whole plane, never masks silently");
    }
    d->src->remoteStep = 2;
    check(whole(*d)[0].status == Status::RemotePreview,
          "P4 remote decimated preview is unavailable before any pixel scan");
    d->src->remoteStep = 1;
    d->px().pop_back();
    check(whole(*d)[0].status == Status::MissingPixels, "P4 incomplete pixel storage is unavailable");
    check(computeProfileNoise(*d, -1, 0, 2, 2)[0].status == Status::InvalidRegion &&
          computeProfileNoise(*d, 5, 0, 2, 2)[0].status == Status::InvalidRegion,
          "P4 invalid ROI is rejected without out-of-range reads");

    // RGB is three independent planes, not one pooled noise measurement.
    auto rgb = make(6, 8, 3);
    fill(*rgb, [&](int x, int y, int c) { return 100.0f * c + (c + 1) * signal(x, y); });
    const auto rgbResult = whole(*rgb);
    bool rgbOK = rgbResult[3].status == Status::NotComputed;
    for (int c = 0; c < 3; ++c)
        rgbOK &= rgbResult[c].samples == 48 &&
                 approximatelyEqual(rgbResult[c].pVariance, (c + 1) * (c + 1) * base.pVariance) &&
                 approximatelyEqual(rgbResult[c].rowRatio(), base.rowRatio());
    check(rgbOK, "P5 RGB planes stay separate; no fourth/pooled ratio");

    auto bayer = make(11, 11);
    bayer->cfa = 1; bayer->cfaPattern = 0;
    fill(*bayer, [](int x, int y, int) { return 100.0f * ((y & 1) * 2 + (x & 1)); });
    const auto br = computeProfileNoise(*bayer, 1, 3, 7, 7);
    const int bW[4] = {3, 4, 3, 4}, bH[4] = {3, 3, 4, 4};
    bool bayerOK = true;
    for (int c = 0; c < 4; ++c)
        bayerOK &= br[c].valid() && br[c].width == bW[c] && br[c].height == bH[c] &&
                   br[c].samples == (size_t)bW[c] * bH[c] && br[c].pVariance == 0 &&
                   br[c].rowVariance == 0 && br[c].colVariance == 0;
    check(bayerOK, "P5 odd-offset/odd-size Bayer ROI has exact per-plane geometry");
    bayer->px()[(size_t)4 * bayer->w + 4] = std::numeric_limits<float>::quiet_NaN();
    const auto badBayer = computeProfileNoise(*bayer, 1, 3, 7, 7);
    check(badBayer[0].status == Status::NonFinite && badBayer[1].valid() &&
          badBayer[2].valid() && badBayer[3].valid(), "P5 non-finite CFA sample only rejects its own plane");

    auto quad = make(13, 13);
    quad->cfa = 2; quad->cfaPattern = 0;
    fill(*quad, [](int x, int y, int) { return 100.0f * (((y / 2) & 1) * 2 + ((x / 2) & 1)); });
    const auto qr = computeProfileNoise(*quad, 1, 1, 9, 9);
    const int qW[4] = {5, 4, 5, 4}, qH[4] = {5, 5, 4, 4};
    bool quadOK = true;
    for (int c = 0; c < 4; ++c)
        quadOK &= qr[c].valid() && qr[c].width == qW[c] && qr[c].height == qH[c] &&
                  qr[c].samples == (size_t)qW[c] * qH[c] && qr[c].pVariance == 0 &&
                  qr[c].rowVariance == 0 && qr[c].colVariance == 0;
    check(quadOK, "P5 Quad Bayer counts actual plane rows/columns, including partial cells");

    // Embed P1's nonconstant 6x8 plane four times, with different DC levels
    // and scales. The fixtures map physical coordinates themselves, without
    // cfaChannelAt: a wrong mosaic split must not construct its own oracle.
    // Poison pixels outside the ROI also catch using the whole frame instead.
    auto bandPlanesOK = [&](const auto& planes) {
        bool good = true;
        for (int c = 0; c < 4; ++c) {
            const auto& p = planes[c];
            const double scale2 = (c + 1) * (c + 1);
            good &= p.valid() && !p.clamped() && p.width == 6 && p.height == 8 &&
                    p.samples == 48 && approximatelyEqual(p.pVariance, scale2 * 461616.0 / 27965) &&
                    approximatelyEqual(p.rowVariance, scale2 * 722064.0 / 27965) &&
                    approximatelyEqual(p.colVariance, scale2 * 244320.0 / 27965) &&
                    approximatelyEqual(p.rowRatio(), std::sqrt(15043.0 / 9617)) &&
                    approximatelyEqual(p.colRatio(), std::sqrt(5090.0 / 9617));
        }
        return good;
    };
    auto bayerBands = make(14, 18);
    bayerBands->cfa = 1; bayerBands->cfaPattern = 0;
    fill(*bayerBands, [&](int x, int y, int) {
        if (x < 1 || x >= 13 || y < 1 || y >= 17) return 100000.0f;
        const int c = (y & 1) * 2 + (x & 1);
        return 1000.0f * c + (c + 1) * signal((x - 1) / 2, (y - 1) / 2);
    });
    check(bandPlanesOK(computeProfileNoise(*bayerBands, 1, 1, 12, 16)),
          "P5 Bayer nonconstant planes preserve independent rational noise/ratios in shifted ROI");
    auto quadBands = make(16, 20);
    quadBands->cfa = 2; quadBands->cfaPattern = 0;
    fill(*quadBands, [&](int x, int y, int) {
        if (x < 2 || x >= 14 || y < 2 || y >= 18) return 100000.0f;
        const int c = ((y / 2) & 1) * 2 + ((x / 2) & 1);
        const int u = ((x - 2) / 4) * 2 + (x & 1);
        const int v = ((y - 2) / 4) * 2 + (y & 1);
        return 1000.0f * c + (c + 1) * signal(u, v);
    });
    check(bandPlanesOK(computeProfileNoise(*quadBands, 2, 2, 12, 16)),
          "P5 Quad nonconstant planes preserve independent rational noise/ratios in shifted ROI");

    // 4,198,400 pixels exceeds BOTH display-profile pass caps. Row 1 is the
    // row an every-second-row pass misses; checkerboard +/-2 supplies residual.
    // Independent exact moments give the rational constants below. In
    // particular p2 is not the zero of either decimated checkerboard profile.
    auto big = make(2048, 2050);
    fill(*big, [](int x, int y, int) {
        return (y == 1 ? 8.0f : 0.0f) + (((x ^ y) & 1) ? 2.0f : -2.0f);
    });
    const auto large = whole(*big)[0];
    check(large.samples == 4198400 && large.width == 2048 && large.height == 2050 &&
          large.ratioValid() && approximatelyEqual(large.pVariance, 35252990967808.0 / 8804676661249.0) &&
          approximatelyEqual(large.rowVariance, 264105944301568.0 / 9024793577780225.0) &&
          large.colVariance == 0 && !large.pClamped && !large.rowClamped && large.colClamped,
          "P6 >4M pixels: full coherent rectangle retains row 1 and checkerboard residual");

    // Integration with the REAL Profile cache and shared clipboard formatter.
    // Its plot reduce changes the old descriptive statistics, not these ratios.
    app.selectedAnn = -1;
    auto integrated = make(6, 8);
    fill(*integrated, [&](int x, int y, int) { return signal(x, y); });
    App::ProjState P;
    bool modeOK = true;
    for (int mode = 0; mode < 3; ++mode) {
        app.projMode = mode;
        recomputeProjectionIfNeeded(integrated.get(), P);
        modeOK &= P.noise[0].pVariance == base.pVariance &&
                  P.noise[0].rowVariance == base.rowVariance &&
                  P.noise[0].colVariance == base.colVariance;
    }
    check(modeOK, "P7 cache ratios remain mean-based through mean/max/min plot reduce");
    char hExpected[48], vExpected[48];
    snprintf(hExpected, sizeof hExpected, "%.9g", std::sqrt(15043.0 / 9617));
    snprintf(vExpected, sizeof vExpected, "%.9g", std::sqrt(5090.0 / 9617));
    check(projNoiseValue(P, 0, true) == hExpected && projNoiseValue(P, 0, false) == vExpected &&
          projNoiseStatus(P, 0).find("6x8 plane pixels, full ROI") != std::string::npos,
          "P7 clipboard ratios use unrounded values and report full plane geometry");
    app.projDecimals = 2;
    check(projNoiseValue(P, 0, true, true) == "1.25" &&
          projNoiseValue(P, 0, false, true) == "0.73" && projNoiseValue(P, 0, true) == hExpected,
          "P7 display decimals do not reduce clipboard precision");
    check(projNoiseValue(P, 4, true) == "-" &&
          projNoiseStatus(P, 4) == "unavailable: pooled planes",
          "P7 pooled all is unavailable without accessing a nonexistent plane");
    fill(*integrated, [](int, int, int) { return 0.0f; });
    ++integrated->dataRev;
    recomputeProjectionIfNeeded(integrated.get(), P);
    check(projNoiseValue(P, 0, true) == "-" &&
          projNoiseStatus(P, 0).find("RN = 0") != std::string::npos,
          "P7 pixel revision invalidates ratio cache and RN=0 has a reason");
    fill(*integrated, [](int, int, int) { return std::numeric_limits<float>::quiet_NaN(); });
    ++integrated->dataRev;
    recomputeProjectionIfNeeded(integrated.get(), P);
    check(!P.fStat[0].valid && P.noise[0].status == Status::NonFinite &&
          projNoiseValue(P, 0, true) == "-",
          "P7 all-NaN plane keeps a refusal even when old statistics are invalid");

    fprintf(stderr, "profilenoiseselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
