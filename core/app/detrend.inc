// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ------------------------------------------------- AnalysisPreProcessor: detrend (#57 判断6/7)
//
// docs/flat-field-stats.md "シェーディング除去 — 全部の土台に置く" (確定
// 2026-08-09, #57) and docs/analysis-layers.md §5. This file is the STAGE and
// nothing else: pure functions over a sample buffer, no app state, no ImGui.
// The product that carries it (core/app/preprocess.inc) and the readout that
// prints its figure (core/app/setanalysis.inc) both come here for the
// arithmetic AND for the words, so there is one definition of what was done
// and one spelling of how it is declared.
//
// -------- 判断6: this is a STAGE, not an option -------------------------------
// "detrend は AnalysisPreProcessor の1段である。それ以外の場所には置かない —
// General パネル (Histogram / Projection / ROIs / Temporal) に detrend
// オプションは付けない。付ければ同じ列名で別の量が出る". So there is no switch
// anywhere that changes what a General panel's numbers mean. Detrended data is
// obtained by PRODUCING detrended data - a first-class stack that every
// analyzer then applies to unchanged - and the method and window travel with
// the product rather than with a checkbox somebody may have left on.
//
// -------- 判断7: the default does nothing, and says how much it would have done
// "既定は「なし (raw)」... 自己記述的な既定は「何もしていない」だけである" and
// "その代わり、シェーディング量は常に併記する". Both halves are mechanical here:
//   - DetrendSpec's default-constructed method is DTR_NONE. Nothing removes
//     shading unless somebody asked for it by name.
//   - detrendProbe() is INDEPENDENT of that choice. It always measures, always
//     with the same degree-2 polynomial, so the figure means the same thing
//     beside a raw result and beside a detrended one, and "should this have
//     been detrended?" is a question the reader can answer every time.
// The ON default - degree 2, subtractive, per plane - is the rest of the same
// judgment, and it is the default-constructed value of the other fields.
//
// -------- the order this stage sits in ---------------------------------------
// docs/flat-field-stats.md "順序 (この順でしか安全に積めない)":
//     0 black level -> 1a saturation mask (on RAW values) -> 2 dark subtraction
//     -> 2b linearity -> 3 SHADING REMOVAL -> 3b defect mask -> 4 statistics
// with the branch that the column drawing hides: sigma_t comes off BEFORE 3 and
// spatial statistics after it. Stages 0 / 1a / 2b / 3b are not built; skipping
// an unbuilt stage is legal and skipping it silently is not, which is what the
// cutoff string below exists to prevent. Stage 2 (dark subtraction) is a reader
// today (docs/analysis-layers.md §5, 判断record 2) and the temporal fold is not
// a preprocessing stage at all - it is aggregation, downstream.
//
// 約束3 is why a detrended STACK fits its surface ONCE, on the stack's temporal
// mean, and subtracts that one surface from every frame: a per-pixel constant
// removed from every frame cannot change a per-pixel temporal variance, so
// sigma_t comes out of the product bit for bit unchanged and the stage really
// has touched only the spatial statistics it is allowed to touch. Fitting per
// frame would have been the obvious loop and would have quietly put fit noise
// into sigma_t.
//
// 約束4 ("除去した面そのものを出力にする") is honoured as far as this build can:
// the shading FIGURE (p-p in DN, and as a % of the field centre) is an output
// of every measurement and of every product. The shading MAP is map material
// and there is no map vessel yet - that is #49's, and inventing a second kind
// of image here to hold it would be inventing the vessel.

// DetrendMethod and DetrendSpec are declared in core/app/state.h, beside
// StackFold and for the same reason (SeqInfo carries one). What matters about
// them is here: DEFAULT-CONSTRUCTED IS RAW (method = DTR_NONE), and the other
// fields are 判断7's "when it IS on" - degree 2, subtractive, per plane.

// The degree the SHADING FIGURE is always measured with, whatever is being
// removed (or not removed). A figure whose own band moved with the option
// would not be comparable between a raw run and a detrended one, and comparing
// them is its entire job.
static const int kDetrendProbeDegree = 2;

// ---- the words -------------------------------------------------------------
// 約束1: "カットオフを必ず言う。PRNU も sigma_fpn も空間周波数帯の関数であって
// ... 「4次面除去後」「64px ブロック除去後」「raw (除去なし)」まで含めて1つの
// 測定値". docs/analysis-layers.md §8 makes it a provenance column ("数値を変える
// パラメータ") and PR #130 made it a REQUIRED field of the result structure, so
// a display cannot omit it. This function is the only place that sentence is
// written, which is what keeps the product's note and the panel's cutoff line
// from drifting the way a column and its tooltip did for eleven weeks.
static std::string detrendCutoffText(const DetrendSpec& sp) {
    switch (sp.method) {
        case DTR_POLY: {
            char b[160];
            snprintf(b, sizeof b,
                     "polynomial surface removed, degree %d, %s, per plane",
                     sp.degree, sp.divide ? "divisive" : "subtractive");
            return b;
        }
        case DTR_BLOCKMED: {
            char b[160];
            snprintf(b, sizeof b,
                     "block median surface removed, %d px window, %s, per plane",
                     sp.block, sp.divide ? "divisive" : "subtractive");
            return b;
        }
        default:
            // NOT "the stage is not built" any more - it is, and saying so
            // would misdescribe a deliberate choice as a missing feature.
            return "raw (no shading removal)";
    }
}

// The short form, for a name. A name has to fit in a Files row.
static std::string detrendShortText(const DetrendSpec& sp) {
    char b[64];
    if (sp.method == DTR_POLY)
        snprintf(b, sizeof b, "poly%d %s", sp.degree, sp.divide ? "div" : "sub");
    else if (sp.method == DTR_BLOCKMED)
        snprintf(b, sizeof b, "blockmed%d %s", sp.block, sp.divide ? "div" : "sub");
    else
        snprintf(b, sizeof b, "raw");
    return b;
}

// ---- the plane map ----------------------------------------------------------
// 約束2: "プレーンごとに独立に除去する。シェーディングはプレーンで違う (色
// シェーディングは実在の現象で、それ自体が測りたい量になる)。R の面で B を割って
// はいけない". So a mosaic is four planes by CFA phase, and a colour frame is one
// plane PER CHANNEL - pooling an RGB frame's channels into one surface would be
// the same mistake with a different spelling.
struct DetrendPlanes {
    int nPl = 1;
    std::vector<uint8_t> of;      // per SAMPLE (y*W + x)*C + c
    std::string err;
};
static DetrendPlanes detrendPlanesOf(const ImageDoc& d) {
    DetrendPlanes P;
    const int W = d.w, H = d.h, C = d.ch;
    if (C > 4) {
        P.err = "more than 4 channels - the per-plane rule needs a plane per channel";
        return P;
    }
    const bool mosaic = d.cfa != 0 && C == 1;
    P.nPl = mosaic ? 4 : C;
    P.of.assign((size_t)W * H * C, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < C; c++)
                P.of[((size_t)y * W + x) * C + c] =
                    mosaic ? (uint8_t)cfaChannelAt(d, x, y) : (uint8_t)c;
    return P;
}

// ---- the figure -------------------------------------------------------------
// docs/flat-field-stats.md (a)'s table: "シェーディング | 低周波成分の p-p /
// 中心比 | %". Both halves are reported, because only one of them is always
// defined: the p-p of the low-frequency component is a DN spread and exists on
// any picture, while the ratio needs a level to be a ratio OF, and a dark has
// none. The refusal is worded the way the peer already refuses a ratio without
// a level (core/app/setanalysis.inc's mu <= 0).
struct DetrendPlaneReport {
    bool   valid  = false;
    double ppDn   = 0;     // max(S) - min(S) over this plane's samples
    double centre = 0;     // S at the field centre (u = v = 0), the ratio's base
    double pct    = 0;     // 100 * ppDn / centre
    bool   pctOk  = false; // ...when centre > 0. Never a ratio without a level
    size_t n      = 0;
};
struct DetrendReport {
    bool ok = false;
    std::string err;
    int nPl = 1;
    int degree = kDetrendProbeDegree;
    DetrendPlaneReport pl[4];
};

// ---- the surface ------------------------------------------------------------
struct DetrendFit {
    bool ok = false;
    std::string err;
    DetrendSpec spec;
    int nPl = 1;
    std::vector<float> surface;   // per sample; NaN where no surface exists
    double offset[4] = {};        // plane mean of S: what a removal ADDS BACK
    bool degenerate[4] = {};      // the fit collapsed and fell back to the mean
    DetrendReport before;         // the figure, measured before anything moved
};

// Total-degree 2-D polynomial basis, u,v in [-1,1] with the field centre at
// exactly (0,0) whatever the parity of W and H - so "the centre value" is an
// evaluation and not an interpolation between two pixels.
static int dtrTerms(int deg) { return (deg + 1) * (deg + 2) / 2; }
static void dtrBasis(double u, double v, int deg, double* b) {
    int k = 0;
    double up = 1.0;
    for (int i = 0; i <= deg; i++) {
        double vp = 1.0;
        for (int j = 0; i + j <= deg; j++) {
            b[k++] = up * vp;
            vp *= v;
        }
        up *= u;
    }
}
// Normal equations, Gauss with partial pivoting. n <= 15 (degree 4), so the
// cost is nothing and the conditioning is fine with u,v normalised - but a
// degenerate plane (one row of samples, a constant field, too few points) must
// not come back as garbage, so a singular system reports itself and the caller
// falls back to the plane mean and DECLARES it.
static bool dtrSolve(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; c++) {
        int piv = c;
        for (int r = c + 1; r < n; r++)
            if (fabs(A[(size_t)r * n + c]) > fabs(A[(size_t)piv * n + c])) piv = r;
        if (!(fabs(A[(size_t)piv * n + c]) > 1e-12)) return false;
        if (piv != c) {
            for (int k = 0; k < n; k++) std::swap(A[(size_t)c * n + k], A[(size_t)piv * n + k]);
            std::swap(b[c], b[piv]);
        }
        const double d = A[(size_t)c * n + c];
        for (int r = c + 1; r < n; r++) {
            const double f = A[(size_t)r * n + c] / d;
            if (f == 0.0) continue;
            for (int k = c; k < n; k++) A[(size_t)r * n + k] -= f * A[(size_t)c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        double s = b[r];
        for (int k = r + 1; k < n; k++) s -= A[(size_t)r * n + k] * b[k];
        b[r] = s / A[(size_t)r * n + r];
    }
    return true;
}

// One plane's polynomial surface, written into `S` at that plane's samples.
// `coefOut` (optional) receives the coefficients so the caller can evaluate the
// surface at the field centre without hunting for a sample that sits there.
template <class T>
static bool dtrFitPolyPlane(const T* v, int W, int H, int C, const uint8_t* plane,
                            int p, int deg, std::vector<float>& S,
                            double* coefOut, size_t* nOut) {
    const int nt = dtrTerms(deg);
    std::vector<double> A((size_t)nt * nt, 0.0), rhs((size_t)nt, 0.0);
    std::vector<double> bb((size_t)nt);
    const double du = W > 1 ? 2.0 / (W - 1) : 0.0;
    const double dv = H > 1 ? 2.0 / (H - 1) : 0.0;
    size_t n = 0;
    for (int y = 0; y < H; y++) {
        const double vv = H > 1 ? y * dv - 1.0 : 0.0;
        for (int x = 0; x < W; x++) {
            const double uu = W > 1 ? x * du - 1.0 : 0.0;
            for (int c = 0; c < C; c++) {
                const size_t i = ((size_t)y * W + x) * C + c;
                if (plane[i] != (uint8_t)p) continue;
                const double z = (double)v[i];
                if (!std::isfinite(z)) continue;      // never folded into a divisor
                dtrBasis(uu, vv, deg, bb.data());
                for (int a = 0; a < nt; a++) {
                    rhs[a] += bb[a] * z;
                    for (int b2 = a; b2 < nt; b2++) A[(size_t)a * nt + b2] += bb[a] * bb[b2];
                }
                n++;
            }
        }
    }
    if (nOut) *nOut = n;
    for (int a = 0; a < nt; a++)
        for (int b2 = 0; b2 < a; b2++) A[(size_t)a * nt + b2] = A[(size_t)b2 * nt + a];
    bool solved = n >= (size_t)nt && dtrSolve(A, rhs, nt);
    if (!solved) {
        // fall back to the plane mean - a degree-0 surface, which removes
        // nothing and reports zero shading. The caller says so out loud.
        double s = 0; size_t k = 0;
        for (size_t i = 0; i < (size_t)W * H * C; i++)
            if (plane[i] == (uint8_t)p && std::isfinite((double)v[i])) { s += (double)v[i]; k++; }
        std::fill(rhs.begin(), rhs.end(), 0.0);
        rhs[0] = k ? s / (double)k : 0.0;
    }
    for (int y = 0; y < H; y++) {
        const double vv = H > 1 ? y * dv - 1.0 : 0.0;
        for (int x = 0; x < W; x++) {
            const double uu = W > 1 ? x * du - 1.0 : 0.0;
            dtrBasis(uu, vv, deg, bb.data());
            double z = 0;
            for (int a = 0; a < nt; a++) z += bb[a] * rhs[a];
            for (int c = 0; c < C; c++) {
                const size_t i = ((size_t)y * W + x) * C + c;
                if (plane[i] == (uint8_t)p) S[i] = (float)z;
            }
        }
    }
    if (coefOut) for (int a = 0; a < nt; a++) coefOut[a] = rhs[a];
    return solved;
}

// One plane's block-median surface. Blocks are in SOURCE pixels, so a 64 px
// window means the same thing on a mosaic and on a colour frame (on a Bayer
// plane it holds 32x32 of that plane's own samples). The median is the reason
// this method exists - 欠陥画素や絵柄に頑健 - and bilinear interpolation
// between block centres is what keeps the removed surface low-frequency
// instead of a staircase whose steps would read as fixed pattern.
template <class T>
static void dtrFitBlockPlane(const T* v, int W, int H, int C, const uint8_t* plane,
                             int p, int blk, std::vector<float>& S) {
    blk = std::max(2, blk);
    const int bx = std::max(1, (W + blk - 1) / blk), by = std::max(1, (H + blk - 1) / blk);
    std::vector<std::vector<float>> bag((size_t)bx * by);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < C; c++) {
                const size_t i = ((size_t)y * W + x) * C + c;
                if (plane[i] != (uint8_t)p) continue;
                const double z = (double)v[i];
                if (!std::isfinite(z)) continue;
                bag[(size_t)std::min(y / blk, by - 1) * bx + std::min(x / blk, bx - 1)]
                    .push_back((float)z);
            }
    std::vector<double> med((size_t)bx * by, std::numeric_limits<double>::quiet_NaN());
    double all = 0; size_t allN = 0;
    for (size_t k = 0; k < med.size(); k++) {
        std::vector<float>& g = bag[k];
        if (g.empty()) continue;
        std::nth_element(g.begin(), g.begin() + g.size() / 2, g.end());
        med[k] = g[g.size() / 2];
        all += med[k]; allN++;
    }
    const double fill = allN ? all / (double)allN : 0.0;
    for (double& m : med) if (!std::isfinite(m)) m = fill;   // an all-NaN block
    // bilinear over block CENTRES, clamped at the border
    for (int y = 0; y < H; y++) {
        const double fy = std::clamp(((y + 0.5) / blk) - 0.5, 0.0, (double)by - 1.0);
        const int y0 = (int)fy, y1 = std::min(y0 + 1, by - 1);
        const double ty = fy - y0;
        for (int x = 0; x < W; x++) {
            const double fx = std::clamp(((x + 0.5) / blk) - 0.5, 0.0, (double)bx - 1.0);
            const int x0 = (int)fx, x1 = std::min(x0 + 1, bx - 1);
            const double tx = fx - x0;
            const double z =
                med[(size_t)y0 * bx + x0] * (1 - tx) * (1 - ty) +
                med[(size_t)y0 * bx + x1] * tx * (1 - ty) +
                med[(size_t)y1 * bx + x0] * (1 - tx) * ty +
                med[(size_t)y1 * bx + x1] * tx * ty;
            for (int c = 0; c < C; c++) {
                const size_t i = ((size_t)y * W + x) * C + c;
                if (plane[i] == (uint8_t)p) S[i] = (float)z;
            }
        }
    }
}

// THE MEASUREMENT. Always the same degree, whatever is being removed - see
// kDetrendProbeDegree. Reports per plane, and reports the ratio only when
// there is a level to take it against.
template <class T>
static DetrendReport detrendProbeT(const T* v, int W, int H, int C,
                                   const uint8_t* plane, int nPl) {
    DetrendReport R;
    R.nPl = std::clamp(nPl, 1, 4);
    R.degree = kDetrendProbeDegree;
    if (W <= 0 || H <= 0 || C <= 0) { R.err = "empty frame"; return R; }
    std::vector<float> S((size_t)W * H * C, std::numeric_limits<float>::quiet_NaN());
    const int nt = dtrTerms(kDetrendProbeDegree);
    for (int p = 0; p < R.nPl; p++) {
        std::vector<double> coef((size_t)nt, 0.0);
        size_t n = 0;
        dtrFitPolyPlane(v, W, H, C, plane, p, kDetrendProbeDegree, S, coef.data(), &n);
        DetrendPlaneReport& o = R.pl[p];
        o.n = n;
        if (!n) continue;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (size_t i = 0; i < S.size(); i++) {
            if (plane[i] != (uint8_t)p || !std::isfinite(S[i])) continue;
            lo = std::min(lo, (double)S[i]);
            hi = std::max(hi, (double)S[i]);
        }
        if (!std::isfinite(lo)) continue;
        std::vector<double> bb((size_t)nt);
        dtrBasis(0.0, 0.0, kDetrendProbeDegree, bb.data());      // the field centre
        double ctr = 0;
        for (int a = 0; a < nt; a++) ctr += bb[a] * coef[a];
        o.valid = true;
        o.ppDn = hi - lo;
        o.centre = ctr;
        o.pctOk = ctr > 0.0;
        o.pct = o.pctOk ? 100.0 * o.ppDn / ctr : std::numeric_limits<double>::quiet_NaN();
    }
    R.ok = true;
    return R;
}

// THE REMOVAL. `spec.method == DTR_NONE` still measures (fit.before) and
// produces no surface: raw is a value of this stage, not an absence of it.
template <class T>
static DetrendFit detrendFitT(const T* v, int W, int H, int C,
                              const uint8_t* plane, int nPl, const DetrendSpec& sp) {
    DetrendFit F;
    F.spec = sp;
    F.nPl = std::clamp(nPl, 1, 4);
    F.before = detrendProbeT(v, W, H, C, plane, nPl);
    if (!F.before.ok) { F.err = F.before.err; return F; }
    if (sp.method == DTR_NONE) { F.ok = true; return F; }
    if (sp.method == DTR_POLY && (sp.degree < 1 || sp.degree > 4)) {
        F.err = "polynomial degree " + std::to_string(sp.degree) +
                " is outside 1..4 - the settled table is 2..4 and degree 1 is the plane";
        return F;
    }
    F.surface.assign((size_t)W * H * C, std::numeric_limits<float>::quiet_NaN());
    const int nt = dtrTerms(std::clamp(sp.degree, 1, 4));
    for (int p = 0; p < F.nPl; p++) {
        if (sp.method == DTR_BLOCKMED) {
            dtrFitBlockPlane(v, W, H, C, plane, p, sp.block, F.surface);
        } else {
            std::vector<double> coef((size_t)nt, 0.0);
            size_t n = 0;
            F.degenerate[p] = !dtrFitPolyPlane(v, W, H, C, plane, p, sp.degree,
                                               F.surface, coef.data(), &n);
        }
        // The level the removal ADDS BACK: this plane's mean surface value.
        // Subtracting the surface outright would take the picture to ~0 and
        // with it the level every ratio in the program is taken against; with
        // the plane mean added back, mean_spatial is preserved EXACTLY and the
        // only thing the stage removed is the low-frequency spread. That is
        // what makes a before/after comparison a comparison.
        double s = 0; size_t k = 0;
        for (size_t i = 0; i < F.surface.size(); i++)
            if (plane[i] == (uint8_t)p && std::isfinite(F.surface[i])) { s += F.surface[i]; k++; }
        F.offset[p] = k ? s / (double)k : 0.0;
    }
    F.ok = true;
    return F;
}

// Apply a fitted surface to one frame's samples. Separate from the fit on
// purpose: a STACK is detrended by fitting ONCE on its temporal mean and
// applying that one surface to every frame (約束3 - see the header), so this
// runs N times against one fit.
static void detrendApply(const DetrendFit& F, const uint8_t* plane, float* px, size_t n) {
    if (!F.ok || F.spec.method == DTR_NONE || F.surface.empty()) return;
    for (size_t i = 0; i < n; i++) {
        const float s = F.surface[i];
        if (!std::isfinite(s)) continue;
        const double off = F.offset[plane[i] & 3];
        const double x = (double)px[i];
        if (!std::isfinite(x)) continue;
        // 約束6: subtractive by default, because dividing by a surface scales
        // the noise by 1/s and makes the variance a function of position -
        // which every estimator downstream assumes it is not. Division is
        // offered for genuinely multiplicative shading and the cutoff says so.
        px[i] = (float)(F.spec.divide ? (s > 0 ? x * off / s : x) : x - s + off);
    }
}
