// The SHADING FIGURE, and the polynomial surface it is measured with.
//
// Lifted verbatim out of core/app/detrend.inc (#57 judgment 6/7) when the peer
// gained set folding: the figure is 判断7's other half - "既定は「なし (raw)」
// ... その代わり、シェーディング量は常に併記する" - so it is a REQUIRED field of
// every set result, and a set folded on a peer has to come back carrying it.
// A figure measured over a picture can only be measured where that picture is,
// and the peer is where it is; so this arithmetic has to be reachable from
// core/serve.cpp as well as from the viewer.
//
// It is a MOVE and not a copy, for the reason this repo keeps repeating: a
// second implementation of one estimator is a second answer waiting to happen
// (the ROIs column and its tooltip drifted for eleven weeks, PR #130). Both
// ends call these functions; nothing anywhere re-derives them.
//
// What did NOT move: the removal stage (DTR_POLY / DTR_BLOCKMED, DetrendSpec,
// DetrendFit, the product's naming). Removing shading is a PreProcessor stage
// that makes a first-class stack, and a stack the peer holds is a file the peer
// reads - there is nothing for the peer to remove. Only the probe crosses.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// The degree the SHADING FIGURE is always measured with, whatever is being
// removed (or not removed). A figure whose own band moved with the option
// would not be comparable between a raw run and a detrended one, and comparing
// them is its entire job.
static const int kDetrendProbeDegree = 2;

// ---- the figure -------------------------------------------------------------
// docs/features/analysis/flat-field-stats.md (a)'s table: "シェーディング | 低周波成分の p-p /
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

// Total-degree 2-D polynomial basis, u,v in [-1,1] with the field centre at
// exactly (0,0) whatever the parity of W and H - so "the centre value" is an
// evaluation and not an interpolation between two pixels.
inline int dtrTerms(int deg) { return (deg + 1) * (deg + 2) / 2; }
inline void dtrBasis(double u, double v, int deg, double* b) {
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
inline bool dtrSolve(std::vector<double>& A, std::vector<double>& b, int n) {
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
inline bool dtrFitPolyPlane(const T* v, int W, int H, int C, const uint8_t* plane,
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

// THE MEASUREMENT. Always the same degree, whatever is being removed - see
// kDetrendProbeDegree. Reports per plane, and reports the ratio only when
// there is a level to take it against.
template <class T>
inline DetrendReport detrendProbeT(const T* v, int W, int H, int C,
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
