// Included after state.h. This is deliberately independent of the display
// profile's reduce mode and orthogonal sampling caps: T, A and B must describe
// ONE complete rectangle of ONE plane (flat-field-stats.md (a)).
static std::array<profile_noise::Result, 4> computeProfileNoise(
        const ImageDoc& im, int rx, int ry, int rw, int rh) {
    using profile_noise::Status;
    std::array<profile_noise::Result, 4> result{};
    const bool cfa = im.ch == 1 && im.cfa != 0;
    const int planes = cfa ? 4 : std::clamp(im.ch, 0, 3);
    auto unavailable = [&](Status status) {
        for (auto& r : result) r.status = status;
        return result;
    };
    if (im.w < 1 || im.h < 1 || im.ch < 1 || rx < 0 || ry < 0 ||
        rx > im.w || ry > im.h || rw < 1 || rh < 1 ||
        rw > im.w - rx || rh > im.h - ry)
        return unavailable(Status::InvalidRegion);
    if (!im.src) return unavailable(Status::MissingPixels);
    if (im.src->remoteStep > 1) return unavailable(Status::RemotePreview);
    // Division avoids overflowing a malformed w*h*ch product.
    if ((size_t)im.w > im.px().size() / (size_t)im.ch / (size_t)im.h)
        return unavailable(Status::MissingPixels);

    struct Acc {
        std::vector<double> rows, cols;
        std::vector<size_t> rowN, colN;
        double origin = 0, sum = 0, sum2 = 0;
        bool started = false, nonFinite = false;
    } acc[4];
    for (int s = 0; s < planes; ++s) {
        acc[s].rows.assign(rh, 0); acc[s].cols.assign(rw, 0);
        acc[s].rowN.assign(rh, 0); acc[s].colN.assign(rw, 0);
    }
    // Center BEFORE summing/squaring. A large DC offset must not erase small
    // variations through E[x*x] - E[x]^2 cancellation. All input pixels are
    // float32; their differences and these accumulators remain double.
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const float* pixel = &im.px()[((size_t)(ry + y) * im.w + rx + x) * im.ch];
            const int first = cfa ? cfaChannelAt(im, rx + x, ry + y) : 0;
            const int end = cfa ? first + 1 : planes;
            for (int s = first; s < end; ++s) {
                Acc& a = acc[s];
                ++result[s].samples; ++a.rowN[y]; ++a.colN[x];
                const double v = pixel[cfa ? 0 : s];
                if (!std::isfinite(v)) { a.nonFinite = true; continue; }
                if (!a.started) { a.origin = v; a.started = true; }
                const double d = v - a.origin;
                a.sum += d; a.sum2 += d * d;
                a.rows[y] += d; a.cols[x] += d;
            }
        }
    auto varianceOfMeans = [](const std::vector<double>& sums,
                              const std::vector<size_t>& counts) {
        double origin = 0, sum = 0, sum2 = 0;
        size_t n = 0;
        for (size_t i = 0; i < sums.size(); ++i) {
            if (!counts[i]) continue;
            const double v = sums[i] / counts[i];
            if (!n) origin = v;
            const double d = v - origin;
            sum += d; sum2 += d * d; ++n;
        }
        return n > 1 ? std::max(0.0, (sum2 - sum * sum / n) / (n - 1)) : 0.0;
    };
    for (int s = 0; s < planes; ++s) {
        auto& r = result[s];
        const Acc& a = acc[s];
        for (size_t n : a.colN) if (n) ++r.width;
        for (size_t n : a.rowN) if (n) ++r.height;
        if (a.nonFinite) { r.status = Status::NonFinite; continue; }
        const double denom = r.width > 0 && r.height > 0
            ? 1.0 - 1.0 / r.height - 1.0 / r.width : 0.0;
        if (r.width < 2 || r.height < 2 || denom <= 0) {
            r.status = Status::InsufficientPlane; continue;
        }
        const double n = (double)r.samples;
        const double T = std::max(0.0, (a.sum2 - a.sum * a.sum / n) / (n - 1));
        const double A = varianceOfMeans(a.cols, a.colN);
        const double B = varianceOfMeans(a.rows, a.rowN);
        // The documented finite-size approximation, with ddof=1 for all three
        // ingredients. It is not a standards-conformance claim. Negative
        // estimates are reported as clamps, not silently interpreted as zero.
        const double p = (T - A - B) / denom;
        r.pClamped = p < 0; r.pVariance = std::max(0.0, p);
        const double row = B - r.pVariance / r.width;
        const double col = A - r.pVariance / r.height;
        r.rowClamped = row < 0; r.rowVariance = std::max(0.0, row);
        r.colClamped = col < 0; r.colVariance = std::max(0.0, col);
        r.status = Status::Ready;
    }
    return result;
}
