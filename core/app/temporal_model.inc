// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---------------------------------------------------------------- temporal analysis
// Built-in (L2): per-frame mean/std over the current ROI plus the temporal /
// fixed-pattern noise split — the same decomposition EMVA 1288 is built on.
// The cache slot is a parameter, not app.temporal: slot 0 is A, slot 1 is the
// compare B side. The body is otherwise unchanged - which is the point, A's
// numbers must not move because a B exists.
static void recomputeTemporalIfNeeded(const ImageDoc* im, App::TemporalState& T) {
    if (!im || im->seqId == 0) { T.valid = false; T.seqId = -1; return; }
    std::vector<int> f = framesOfSeq(im->seqId);
    if ((int)f.size() < 2) { T.valid = false; T.seqId = -1; return; }
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), chh = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && chh > 0) { rx = cx; ry = cy; rw = cw; rh = chh; roiUsed = true; }
        }
    }
    // keyed on the resolved rect; this pass costs samples x frames, so it must
    // not run for annotation churn or mid-drag. stackRev is the stack's DATA
    // revision: a reload swaps pixels under an unchanged (seqId, frames, ROI,
    // CFA) tuple, and without it this cache would hand back the dead pixels'
    // sigma_t (docs/reference-design.md §3.2)
    const App::SeqInfo* ksi = seqInfo(im->seqId);
    const int kStackRev = ksi ? ksi->stackRev : 0;
    if (T.seqId == im->seqId && T.frames == (int)f.size() && T.stackRev == kStackRev &&
        T.rx == rx && T.ry == ry && T.rw == rw && T.rh == rh &&
        T.cfa == im->cfa && T.cfaPattern == im->cfaPattern)
        return;
    if (app.annBusy && T.seqId == im->seqId) return;
    T.seqId = im->seqId; T.frames = (int)f.size();
    T.rx = rx; T.ry = ry; T.rw = rw; T.rh = rh; T.roiUsed = roiUsed;
    T.cfa = im->cfa; T.cfaPattern = im->cfaPattern; T.stackRev = kStackRev;
    T.idx.clear(); T.frameMean.clear(); T.frameStd.clear();
    for (int p = 0; p < 4; p++) {
        T.tempNoise[p] = T.fixedPattern[p] = T.totalNoise[p] = T.fpnCorr[p] = 0;
        T.fpnClamped[p] = false;
    }
    T.valid = false;
    // CFA planes are NEVER mixed in a statistic (docs/terminology.md). On a
    // mosaiced flat field the R/Gr/Gb/B level difference is hundreds of DN, so
    // a pooled sigma_fpn reports that difference as fixed-pattern noise -
    // hundreds of DN of fake FPN, and totalNoise inherits it. Sample whole CFA
    // CELLS rather than a strided run, so every plane gets exactly the same
    // count and the decimation cannot land on one parity.
    const bool cfa = im->cfa != 0 && im->ch == 1;
    const int cell = im->cfa == 2 ? 4 : 2;         // Quad Bayer repeats over 4x4
    T.nPl = cfa && rw >= cell && rh >= cell ? 4 : 1;
    // fixed sampling grid, capped so full-frame sequences stay interactive
    const size_t MAX_SAMPLES = 40000;
    std::vector<size_t> offs;
    std::vector<uint8_t> plane;
    if (T.nPl == 1) {
        size_t total = (size_t)rw * rh;
        size_t step = std::max<size_t>(1, total / MAX_SAMPLES);
        for (size_t p = 0; p < total; p += step) {
            int x = rx + (int)(p % rw), y = ry + (int)(p / rw);
            offs.push_back(((size_t)y * im->w + x) * im->ch);   // channel 0
            plane.push_back(0);
        }
    } else {
        size_t bw = (size_t)(rw / cell), bh = (size_t)(rh / cell);
        size_t blocks = bw * bh, perBlock = (size_t)cell * cell;
        size_t bstep = std::max<size_t>(1, blocks / std::max<size_t>(1, MAX_SAMPLES / perBlock));
        for (size_t b = 0; b < blocks; b += bstep)
            for (int dy = 0; dy < cell; dy++)
                for (int dx = 0; dx < cell; dx++) {
                    int x = rx + (int)(b % bw) * cell + dx;
                    int y = ry + (int)(b / bw) * cell + dy;
                    offs.push_back(((size_t)y * im->w + x) * im->ch);
                    plane.push_back((uint8_t)cfaChannelAt(*im, x, y));
                }
    }
    if (offs.empty()) return;
    // per-SAMPLE valid counts, as computeStackStats and serve.cpp keep them:
    // a NaN is excluded from its pixel's population, never folded into the
    // divisor (which would understate the noise and look plausible doing it)
    std::vector<double> sum(offs.size(), 0), sum2(offs.size(), 0);
    std::vector<uint32_t> cnt(offs.size(), 0);
    uint64_t nonFinite = 0;
    int used = 0;
    for (int fi : f) {
        const ImageDoc& fr = *app.images[fi];
        if (fr.w != im->w || fr.h != im->h || fr.ch != im->ch) continue;   // dims changed
        double s = 0, s2 = 0;
        size_t n = 0;
        for (size_t k = 0; k < offs.size(); k++) {
            float v = fr.px()[offs[k]];
            if (!std::isfinite(v)) { nonFinite++; continue; }
            sum[k] += v; sum2[k] += (double)v * v; cnt[k]++;
            s += v; s2 += (double)v * v; n++;
        }
        if (!n) continue;
        double m = s / n, var = s2 / n - m * m;
        T.idx.push_back((float)fr.seqIndex);
        T.frameMean.push_back((float)m);
        T.frameStd.push_back((float)sqrt(var > 0 ? var : 0));
        used++;
    }
    if (used < 2) return;
    // per-sample temporal variance -> temporal noise; spatial spread of the
    // time-averaged samples MINUS the temporal residual still in that mean ->
    // fixed pattern. Both are accumulated PER PLANE; with no mosaic there is
    // one plane.
    //
    // corrSum is the third accumulator #57 item 2 asks for and it costs one
    // divide per sample: the correction is mean_i(s_t,i^2 / n_i), PER PIXEL,
    // because n_i is per pixel here (NaN is excluded from its own pixel's
    // population, never from the stack). mean(s_t^2)/mean(n) would be a
    // different number the moment a pixel loses frames, and the accumulator
    // already carries the per-pixel cnt, so there is nothing to buy.
    double tvarSum[4] = {}, corrSum[4] = {}, pmSum[4] = {}, pmSum2[4] = {};
    size_t plc[4] = {};
    for (size_t k = 0; k < offs.size(); k++) {
        int p = plane[k];
        double n = (double)cnt[k];
        if (n < 2) continue;                    // not enough valid frames here
        double m = sum[k] / n;
        // ddof=1 (#57 item 3). This line divided by n and the two OTHER places
        // that compute the same quantity - core/serve.cpp's runTemporalStats
        // and computeStackStats - divide by n-1, so one stack's sigma_t came
        // out sqrt(N/(N-1)) larger or smaller depending on WHERE it was
        // measured: 6.9% at N=8, 3.3% at N=16. "local" and "server" are a
        // transport, not a quantity, and the canon names sigma_t once
        // (docs/terminology.md, docs/stats-taxonomy.md), so this is a parity
        // repair and not a change of behaviour - the number this returned was
        // simply the wrong one of two. The unbiased form is also what the
        // -sigma_t^2/N correction of docs/flat-field-stats.md is written
        // against; ddof=0 would leave it (N-1)/N short.
        // Clamp BEFORE scaling, exactly as serve.cpp does: a rounding-negative
        // variance is 0 either way, and doing it in the same order keeps the
        // two sides bit-comparable rather than merely close.
        double v = std::max(0.0, sum2[k] / n - m * m) * (n / (n - 1.0));
        tvarSum[p] += v;
        corrSum[p] += v / n;             // s_t,i^2 / n_i, this pixel's own n_i
        pmSum[p] += m; pmSum2[p] += m * m;
        plc[p]++;
    }
    T.nonFinite = nonFinite;
    T.dropped = offs.size() - (plc[0] + plc[1] + plc[2] + plc[3]);
    for (int p = 0; p < T.nPl; p++) {
        if (!plc[p]) continue;
        double n = (double)plc[p];
        double tvar = tvarSum[p] / n;
        double corr = corrSum[p] / n;                     // C = mean_i(s_t,i^2 / n_i)
        double pmean = pmSum[p] / n;
        // ddof=1 on the SPATIAL variance too, as the settled estimator writes it
        // (docs/flat-field-stats.md (b): "var は ddof=1", and (a)'s "ddof は全部
        // ddof=1" is stated to carry into (b)/(c)). It is not decoration: the
        // subtraction below is exact only against an UNBIASED spatial variance.
        // E[S^2_ddof1] = sigma_fpn^2 + C, so S^2 - C is unbiased for sigma_fpn^2;
        // the ddof=0 form has expectation (n-1)/n x (sigma_fpn^2 + C), which
        // leaves (sigma_fpn^2 + C)/n behind after subtracting an unbiased C.
        // n here is samples per plane, so the two forms differ by parts in
        // 10^4 on a full frame and by percent on a small ROI - which is exactly
        // where a fixed pattern is measured.
        // (This is the sigma_fpn estimator, NOT the per-frame spatial sigma of
        // one frame that board row "空間σの ddof がパネル間で不一致" is about:
        // that is a different quantity in ROIs / canvas / histogram and is not
        // touched here.)
        // ddof=1 on the SPATIAL variance too, as the settled estimator writes it
        // (docs/flat-field-stats.md (b): "var は ddof=1", and (a)'s "ddof は全部
        // ddof=1" is stated to carry into (b)/(c)). It is not decoration: the
        // subtraction below is exact only against an UNBIASED spatial variance.
        // E[S^2_ddof1] = sigma_fpn^2 + C, so S^2 - C is unbiased for sigma_fpn^2;
        // the ddof=0 form has expectation (n-1)/n x (sigma_fpn^2 + C), which
        // leaves (sigma_fpn^2 + C)/n behind after subtracting an unbiased C.
        // n here is samples per plane, so the two forms differ by parts in
        // 10^4 on a full frame and by percent on a small ROI - which is exactly
        // where a fixed pattern is measured.
        // (This is the sigma_fpn estimator, NOT the per-frame spatial sigma of
        // one frame that board row "空間σの ddof がパネル間で不一致" is about:
        // that is a different quantity in ROIs / canvas / histogram and is not
        // touched here.)
        double pvar = n > 1.0
            ? std::max(0.0, pmSum2[p] / n - pmean * pmean) * (n / (n - 1.0)) : 0.0;
        // The correction itself (#57 judgment 2). Averaging N frames drops the
        // temporal noise by sqrt(N) and no further, so the spatial spread of the
        // mean image is sigma_fpn^2 + C and calling it sigma_fpn names an UPPER
        // BOUND after the quantity. At N=16, sigma_t=10 DN a true 5 DN pattern
        // reads 5.59 DN uncorrected.
        double fvar = pvar - corr;
        T.fpnClamped[p] = fvar < 0.0;   // said out loud below; never a silent 0
        if (fvar < 0.0) fvar = 0.0;
        T.tempNoise[p] = sqrt(tvar);
        T.fpnCorr[p] = sqrt(corr);      // in DN, so it reads beside sigma_fpn
        T.fixedPattern[p] = sqrt(fvar);
        // sigma_tot's DEFINITION is unchanged (flat-field-stats.md (b)): it is
        // sqrt(<sigma_t^2> + sigma_fpn^2) as it always was. It only becomes the
        // honest "expected spatial sigma of ONE frame" now - before the
        // correction it counted sigma_t^2/N twice.
        T.totalNoise[p] = sqrt(tvar + fvar);
    }
    T.valid = true;
}
