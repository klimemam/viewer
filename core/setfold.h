// The FOLD half of a set analysis: the part that has to run where the pixels
// are (docs/analysis-layers.md §3.5, "remote の stack 集計は画素の居る側 (peer)
// で走る ... set の集計も同じ線").
//
// -------- why this is a header and not two implementations -------------------
// A 480-frame dark on a compute node should be folded there, not pulled across
// ssh to be folded here - that is the whole reason the remote path exists. But
// PR #127's rtemporal P1..P5 is the standing lesson about what happens when the
// same estimator is written twice: the peer built sigma_tot as sqrt(st*st+fvar)
// off an already-square-rooted sigma_t, which is not the viewer's
// sqrt(tvar+fvar) in binary64, and nothing had ever put the two beside each
// other. So the arithmetic below is ONE function called by both ends:
// core/app/setanalysis.inc folds local stacks through it, core/serve.cpp folds
// the peer's files through it, and the equality asserted by --rset-selftest is
// structural rather than a coincidence two authors maintained.
//
// -------- what is here, and what deliberately is NOT -------------------------
// Here: the per-pixel temporal mean M and its residual C, and the reductions of
// those two pictures to a handful of PER-PLANE SCALARS. That is all that ever
// crosses the wire - docs/analysis-layers.md §6's "fit・KPI はスカラの上".
//
// Not here: the estimators. sqrt(max(0, var - C)), the clamp, mu, the ratio,
// the OLS - every one of those is composed by the CLIENT from the scalars
// below, in core/app/setanalysis.inc, which is also where the famous names
// live. The peer computes nothing that has a name; it computes the sums a
// named quantity is made of. That split is not tidiness: it is what lets the
// peer be built into viewer-serve without the famous-name rule (PR #130's E1
// scan, which reads core/*.h too) having to grow an exception.
//
// -------- the one thing that could not be composed from per-role scalars -----
// The direct ratio row subtracts two mean images PIXEL BY PIXEL before it takes
// any spatial statistic, and sum((Ma-Mb)^2) is not a function of the two roles'
// own sums. There IS an algebraic route - send the joint second moment
// sum(Ma*Mb) as a sixth scalar and recompose sum(Ma^2) + sum(Mb^2) -
// 2*sum(Ma*Mb) here - and it was rejected, for a reason worth being exact
// about rather than hand-waving as "cancellation":
//
//   the local estimator accumulates d*d over the difference image, and that
//   estimator is settled and tested to hand-derived numbers. Recomposition is
//   algebraically equal and is a DIFFERENT SEQUENCE of floating-point
//   operations, so it does not return the same binary64 - and a remote route
//   that answers differently than the local one for the same data is the defect
//   this must not introduce. Measured, on --rset-selftest's own fixture: the
//   two routes agree exactly while every value is a dyadic rational, and part
//   company at the 11th significant digit as soon as the pixels are not
//   (3.1073286294809335 % vs 3.1073286294716875 %). Real sensor data is never
//   dyadic. Matching the LOCAL route to the last bit is the requirement; being
//   algebraically right is not enough.
//
// So the difference image is formed and reduced ON THE PEER (reducePair below)
// and only its per-plane sums travel. The cost is stated rather than hidden:
// the peer holds BOTH folds resident at once, which is exactly what the local
// path already does, and is why kMaxSamples is the same ceiling on both sides.
// Nothing else needs two folds resident - the separation fit's M+1 roles are
// reduced and released one at a time.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "shading_probe.h"

namespace setfold {

// Every Stack role of every set analyzer demands 2 frames: the correction is
// made of s_t, and one frame has no s_t (docs/features/analysis/flat-field-stats.md (b)).
static const int kMinFrames = 2;
// computeStackStats' own ceiling, and the local fold's. Two folds of this size
// is ~1 GB of accumulators on the peer, which is the honest price of the
// per-pixel difference above.
static const size_t kMaxSamples = (size_t)32 << 20;

// One role, one plane: everything the direct DSNU row and every level of the
// separation fit consume. Four numbers.
struct PlaneAcc { double s1 = 0, s2 = 0, cs = 0, n = 0; };
// Two roles, one plane, after the per-pixel difference: everything the direct
// PRNU row consumes. Five numbers, and s1a/s1b are taken over the samples BOTH
// roles could measure, which is why they are not the roles' own s1.
struct PairAcc { double s1a = 0, s1b = 0, dq = 0, cs = 0, n = 0; };

// One pixel's temporal mean and the temporal residual still in it.
//   M     = sum / n
//   s_t^2 = max(0, sum2/n - M^2) * n/(n-1)        ddof = n_i - 1 (#57 item 3)
//   C     = s_t^2 / n                             this pixel's OWN n_i
// A pixel fewer than 2 frames saw has no s_t and therefore no correction; it is
// EXCLUDED from both the spatial variance and C rather than folded in with
// corr = 0, which would understate C and inflate the answer. The clamp sits
// BEFORE the ddof scale, exactly as recomputeTemporalIfNeeded and
// serve.cpp's runTemporalStats do it, so "the same arithmetic" is bit-for-bit
// and not merely close.
inline bool pixelMeanCorr(double sum, double sum2, uint32_t cnt,
                          double& M, double& corr) {
    if (cnt < 2) return false;
    const double n = (double)cnt;
    const double m = sum / n;
    const double v = std::max(0.0, sum2 / n - m * m) * (n / (n - 1.0));
    M = m;
    corr = v / n;
    return true;
}

// M/corr -> per-plane (sum M, sum M^2, sum C, n). Linear sample order, which is
// the order the local fold has always reduced in.
inline void reduceOne(const double* M, const double* corr, const uint8_t* plane,
                      size_t samples, PlaneAcc* out) {
    for (size_t i = 0; i < samples; i++) {
        if (!std::isfinite(M[i])) continue;
        PlaneAcc& a = out[plane[i]];
        a.s1 += M[i];
        a.s2 += M[i] * M[i];
        a.cs += corr[i];
        a.n += 1.0;
    }
}

// The pair. A sample either side could not measure has no difference either, so
// it leaves BOTH the variance and the means - which is why n here is the pair's
// own count and mu is built from s1a/s1b rather than from the roles' sums.
inline void reducePair(const double* Ma, const double* corrA,
                       const double* Mb, const double* corrB,
                       const uint8_t* plane, size_t samples, PairAcc* out) {
    for (size_t i = 0; i < samples; i++) {
        if (!std::isfinite(Ma[i]) || !std::isfinite(Mb[i])) continue;
        PairAcc& a = out[plane[i]];
        const double d = Ma[i] - Mb[i];
        a.s1a += Ma[i];
        a.s1b += Mb[i];
        a.dq += d * d;
        a.cs += corrA[i] + corrB[i];
        a.n += 1.0;
    }
}

// D, for the shading probe: the figure beside a ratio has to describe the
// picture the ratio was taken over, and for the pair that picture is D.
inline void differenceImage(const double* Ma, const double* Mb, size_t samples,
                            double* D) {
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < samples; i++)
        D[i] = (std::isfinite(Ma[i]) && std::isfinite(Mb[i])) ? Ma[i] - Mb[i] : kNaN;
}

// ---- the parity declaration --------------------------------------------------
// docs/reference/abi-v3.md §10 matches a plugin by name AND version because two machines
// with the same folder of dlls and different builds inside them answered the
// same question differently and nothing said so. A set analyzer is a BUILT-IN,
// so there is no dll and no descriptor version to compare - and the viewer's
// own version is the wrong thing to compare, because it moves for reasons that
// cannot change a number and, when it does refuse, says nothing about WHAT
// differs.
//
// What must be equal is what the peer actually did, and this string is that
// declaration. It is deliberately assembled from the constants the code above
// uses (the probe degree) rather than typed out beside them, and it names the
// join, because a fold with a difference in it is a different fold.
//
// The row's OWN estimator form (SetAnalyzerDef::form, PR #130) is not compared
// and does not travel: that estimator runs on the client, on the client's own
// code, so there is nothing across the link to disagree with it. This is
// parity on the half that crossed the machine boundary - and, exactly as §10
// says of itself, it is the discipline of DECLARATION and not a proof of
// identical arithmetic. Two builds can declare one form and differ; a build
// that changes the fold and forgets this string is the author's fault and is
// visible, while no declaration at all is nobody's fault and is not, which is
// why an empty form on either side is refused rather than waved through.
enum Join : uint32_t {
    JOIN_NONE = 0,   // every role folded on its own
    JOIN_DIFF = 1,   // ...and role 0 minus role 1, per pixel, reduced HERE
};

inline std::string foldForm(uint32_t join) {
    std::string s =
        "set fold v1: per-pixel M = temporal mean over the frames a sample was "
        "finite in (n_i >= " + std::to_string(kMinFrames) + "), "
        "C_i = s_t,i^2/n_i with s_t,i^2 at ddof = n_i-1; reduced per CFA plane "
        "to (sum M, sum M^2, sum C, n)";
    if (join == JOIN_DIFF)
        s += "; join = per-pixel difference D = M[role 0] - M[role 1] formed "
             "here and reduced to (sum M_a, sum M_b, sum D^2, sum C_a + C_b, n) "
             "over the samples both roles measured";
    s += "; shading probe = polynomial surface of degree " +
         std::to_string(kDetrendProbeDegree) +
         " per plane, p-p in DN and the ratio to the field centre";
    return s;
}

}  // namespace setfold
