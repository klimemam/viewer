// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ------------------------------------------------- SetAnalyzer: direct DSNU / PRNU (#57 判断4)
//
// docs/flat-field-stats.md (b) "直接 DSNU / PRNU" (確定 2026-08-09, #57) and the
// role table of docs/analysis-layers.md §6. Two SetAnalyzers over ONE bound
// AnalysisSet:
//
//     DSNU [DN]   {"dark": Stack()}
//         DSNU = sqrt(max(0, var_spatial(M_dark) - C_dark))         per plane
//
//     PRNU [%]    {"image": Stack(), "dark": Stack()}
//         D     = M_image - M_dark                                  per pixel
//         sd^2  = max(0, var_spatial(D) - C_image - C_dark)
//         mu    = mean_spatial(M_image) - mean_spatial(M_dark)
//         PRNU  = 100 * sd / mu
//
// with M the per-pixel temporal mean, C = mean_i(s_t,i^2 / n_i) the temporal
// residual STILL IN that mean, and every spatial variance at ddof = 1. This is
// the same estimator PR #127 put on the Temporal panel's sigma_fpn row: DSNU is
// that quantity applied to a dark stack and NOTHING ELSE, which is why
// setFoldStack below carries the identical per-sample arithmetic and the
// selftest compares the two bit for bit rather than to a tolerance. What stage
// 2 adds is not arithmetic, it is the ROLE BINDING - and the name that binding
// earns.
//
// The PRNU form is the PIXEL-WISE SUBTRACTION one, settled in #57 judgment 4.
// The variance-difference form sqrt(s^2_flat - s^2_dark) / (mu_flat - mu_dark)
// was explicitly NOT chosen: under y = d + g*mu the pixel-wise subtraction
// gives mu^2*var(g) exactly, while the variance difference keeps a cross term
// 2*mu*cov(offset, gain) that column amplifiers make real. It is reserved as a
// separately NAMED quantity for after the standards check, never as a silent
// swap under this name - so every result states which form it is
// (SetResult::form, a required field, printed on every row).
//
// NO STANDARD IS NAMED HERE, and none may be: naming one is a gate that is
// still shut (docs/flat-field-stats.md (c), docs/analysis-layers.md §6). What
// this file may say is what it computes.
//
// -------- the famous names live HERE, and only here --------------------------
// docs/terminology.md's invariant: "前処理・参照を含意する有名名 (PRNU / DSNU
// …) は SetAnalyzer だけが名乗る。General は中立な量の名前だけを出す — 例外
// なし". A rule nobody can check is a rule that decays: the ROIs column was
// renamed for it in 5d8cb72 and the tooltip UNDER that column went on printing
// "PRNU" for eleven weeks. So the bare names exist as string literals in this
// one file, every result label is built from this table, and
// --setanalysis-selftest scans the shipping sources to hold that true. Adding a
// famous name anywhere else is a failing test, not a review comment.
enum SetAnalyzerKind {
    SA_DSNU = 0, SA_PRNU = 1,     // the DIRECT pair, over Stack() roles (判断4)
    SA_SEP_DSNU = 2,              // the separation fit's two outputs, from ONE
    SA_SEP_PRNU = 3,              // OLS over a Series() role (判断5)
    SA_COUNT = 4
};
// The rows that take Stack() roles only and run per selected set. The
// separation fit is not one of them: it folds every level of a sweep, so it
// runs when it is asked to (the Series Analysis panel's Compute, for the same
// reason that panel has one).
static const int kSetDirectCount = 2;

// Every Stack role of every row here demands 2 frames: the correction is made
// of s_t, and one frame has no s_t (docs/flat-field-stats.md (b)).
static const int kSetMinFrames = 2;

struct SetAnalyzerDef {
    const char* key;          // stable ASCII, for exports and asserts
    const char* famous;       // THE bare famous name (see the block above)
    const char* unit;
    const char* roles[2];     // the role schema, nullptr-padded
    const char* form;         // which estimator this row reports, in words
};
static const SetAnalyzerDef kSetAnalyzers[SA_COUNT] = {
    { "dsnu", "DSNU", "DN",
      { "dark", nullptr },
      "corrected sigma_fpn of the dark stack: "
      "sqrt(max(0, var_spatial(M_dark) - C_dark)), var at ddof=1" },
    { "prnu", "PRNU", "%",
      { "image", "dark" },
      "per-pixel subtraction: D = M_image - M_dark, "
      "100 * sqrt(max(0, var_spatial(D) - C_image - C_dark)) / mu, var at ddof=1 "
      "(NOT the variance-difference form, which is a different quantity)" },
    // The SAME two bare names over a DIFFERENT estimator, which is exactly why
    // docs/analysis-layers.md §7 makes the set tag carry the difference: the
    // direct pair measures one lit stack against one dark, this pair separates
    // the level-independent term from the level-proportional one by fitting
    // several levels. A table that prints both without their tags is the thing
    // §7 forbids.
    { "sep_dsnu", "DSNU", "DN",
      { "sweep", "dark" },
      "separation fit: unweighted OLS of the corrected sigma_fpn^2 on mu^2 over "
      "the sweep's levels; this row is sqrt(max(0, intercept)) - the term that "
      "does NOT scale with the level" },
    { "sep_prnu", "PRNU", "%",
      { "sweep", "dark" },
      "separation fit: the same unweighted OLS; this row is "
      "100 * sqrt(max(0, slope)) - the term proportional to mu. The lit levels "
      "are NOT dark-subtracted (that would take the intercept away, and the "
      "intercept is the point); only mu is dark-corrected" },
};

// The detrend stage (docs/flat-field-stats.md 判断6/7) is not built. Skipping an
// unbuilt stage is legal; skipping it SILENTLY is not - "カットオフを必ず言う"
// makes the cutoff a required field of the result, so the value and the spatial
// band it was measured over never travel apart. Today every row declares the
// same one, and the day a detrend lands this string is what changes.
static const char* kSetCutoffRaw = "raw (no shading removal - the detrend stage is not built)";

// ---- the fold: one stack -> per-pixel mean M and per-pixel correction --------
// The same accumulator as recomputeTemporalIfNeeded / computeStackStats /
// serve.cpp's runTemporalStats (sum, sum^2, per-sample cnt), with two
// differences that are deliberate:
//   - EVERY pixel, no decimation. The Temporal panel samples at most 40000
//     points to stay interactive at 60 Hz; a SetAnalyzer runs when a set is
//     selected and reports a KPI, so it reads the frame.
//   - the per-pixel M and s_t,i^2/n_i are KEPT, because PRNU subtracts two
//     mean images pixel by pixel before it takes any spatial statistic.
// A sample fewer than 2 frames saw has no s_t and therefore no correction; it
// is EXCLUDED from both the spatial variance and C (folding it in with corr = 0
// would understate C and inflate the answer), and counted so the result can say
// so. NaN is excluded from its own pixel's population, never from the divisor.
struct SetFold {
    bool valid = false;
    std::string err;
    int W = 0, H = 0, C = 0, nPl = 1;
    int cfa = 0, cfaPattern = 0;
    int frames = 0, expected = 0;         // n of N (the canon's mandatory pair)
    std::string name, headPath;
    std::vector<double> M;                // per-sample temporal mean; NaN = no mean
    std::vector<double> corr;             // per-sample s_t,i^2 / n_i; NaN with it
    std::vector<uint8_t> plane;           // per-sample CFA plane (0 when not mosaiced)
    uint64_t nonFinite = 0;               // samples excluded, reported not hidden
    size_t dropped = 0;                   // samples with fewer than 2 valid frames
};

static bool setFoldStack(int seqId, SetFold& F) {
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) { F.err = "that stack is not open"; return false; }
    F.name = si->name;
    F.expected = si->expectedFrames;
    std::vector<int> fr = framesOfSeq(seqId);
    if (!fr.empty()) F.headPath = app.images[fr.front()]->src->path;
    const ImageDoc* first = nullptr;
    std::vector<const ImageDoc*> use;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->src->remoteStep > 1 || d->px().empty()) continue;   // previews lie
        if (!first) first = d;
        if (d->w != first->w || d->h != first->h || d->ch != first->ch) continue;
        use.push_back(d);
    }
    F.frames = (int)use.size();
    if (F.expected < (int)fr.size()) F.expected = (int)fr.size();
    if (F.expected < F.frames) F.expected = F.frames;
    if (F.frames < kSetMinFrames) {          // the caller words this one: it is a
        F.err = "min_frames";                // REFUSAL of the analysis, not an error
        return false;
    }
    const int W = first->w, H = first->h, C = first->ch;
    const size_t samples = (size_t)W * H * C;
    if (samples > ((size_t)32 << 20)) {      // computeStackStats' own ceiling
        F.err = "the frame is larger than 32M samples";
        return false;
    }
    std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
    std::vector<uint32_t> cnt(samples, 0);
    for (const ImageDoc* d : use) {
        const float* p = d->px().data();
        for (size_t i = 0; i < samples; i++) {
            double v = p[i];
            if (!std::isfinite(v)) { F.nonFinite++; continue; }
            sum[i] += v; sum2[i] += v * v; cnt[i]++;
        }
    }
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    F.M.assign(samples, kNaN);
    F.corr.assign(samples, kNaN);
    F.plane.assign(samples, 0);
    F.W = W; F.H = H; F.C = C;
    F.cfa = first->cfa; F.cfaPattern = first->cfaPattern;
    // CFA planes are NEVER mixed in a statistic (docs/terminology.md): on a
    // mosaiced flat the plane levels are hundreds of DN apart and a pooled
    // spatial sigma reports that mosaic as fixed-pattern noise.
    F.nPl = (first->cfa != 0 && C == 1) ? 4 : 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t p = F.nPl == 4 ? (uint8_t)cfaChannelAt(*first, x, y) : (uint8_t)0;
            for (int c = 0; c < C; c++) {
                const size_t i = ((size_t)y * W + x) * C + c;
                F.plane[i] = p;
                const double n = (double)cnt[i];
                if (n < 2.0) { F.dropped++; continue; }
                const double m = sum[i] / n;
                // ddof = n_i - 1 (#57 judgment 3), clamped BEFORE the scale
                // exactly as the other three sites do it, so "the same
                // arithmetic" is bit-for-bit and not merely close.
                const double v = std::max(0.0, sum2[i] / n - m * m) * (n / (n - 1.0));
                F.M[i] = m;
                F.corr[i] = v / n;           // s_t,i^2 / n_i, this pixel's own n_i
            }
        }
    F.valid = true;
    return true;
}

// ---- role resolution: a binding is DECLARED, never inferred ------------------
// docs/terminology.md: "役割の束縛は宣言である。推論しない — フォルダ名や
// level 0 から dark を推測して束ねない". Nothing below looks at a folder name, a
// level, or the order stacks were opened: it asks the set for a role by name and
// says one line when the answer is not a usable stack.
struct SetRoleUse {
    std::string role, member, headPath;
    int n = 0, N = 0;                    // frames aggregated of frames expected
    uint64_t nonFinite = 0;
    size_t dropped = 0;
};

static const App::ASetRole* aSetRoleNamed(const App::ASet& S, const char* role) {
    for (const auto& r : S.roles) if (r.role == role) return &r;
    return nullptr;
}

// "" = this role is a stack that can be folded; otherwise the ONE-LINE refusal,
// in the words docs/flat-field-stats.md (b) fixes where it fixes them.
static std::string setRoleRefusal(const App::ASet& S, const char* role, int* seqIdOut) {
    *seqIdOut = 0;
    const App::ASetRole* r = aSetRoleNamed(S, role);
    const std::string q = std::string("\"") + role + "\"";
    if (!r)
        // NOT declared and declared-but-unbound are different states and get
        // different sentences: the first is a set that was never built for this
        // analysis, the second is a set whose world moved (ras §3.1).
        return "this set has no " + q + " role - a role binding is declared, "
               "never inferred";
    if (!r->bound())
        return q + " is unbound - " +
               (r->reason.empty() ? std::string("no reason recorded") : r->reason);
    if (r->frameUid != 0)
        // the settled refusal, verbatim (docs/flat-field-stats.md (b)); the
        // math is spelled in ASCII because every key and unit on this screen is
        // (sigma_t, sigma_fpn, DN - flat-field-stats (a) "ASCII キー + 単位")
        return q + ": 1 frame has no time axis - "
                   "the -sigma_t^2/N correction needs a stack";
    if (r->seriesId != 0)
        return q + " is bound to a series - this analysis takes Stack() roles only "
                   "(a series is a swept parameter, not a time axis)";
    *seqIdOut = r->seqId;
    return "";
}

// The same resolution for a role whose schema says Series() - the separation
// fit's "sweep". Written as its own function rather than a flag on the one
// above, because the two schemas refuse OPPOSITE things: a Stack() role refuses
// a series, a Series() role refuses a stack, and a sentence that has to say
// which way round it is reads worse than two that each say one thing.
static std::string setSeriesRoleRefusal(const App::ASet& S, const char* role,
                                        int* seriesIdOut) {
    *seriesIdOut = 0;
    const App::ASetRole* r = aSetRoleNamed(S, role);
    const std::string q = std::string("\"") + role + "\"";
    if (!r)
        return "this set has no " + q + " role - a role binding is declared, "
               "never inferred";
    if (!r->bound())
        return q + " is unbound - " +
               (r->reason.empty() ? std::string("no reason recorded") : r->reason);
    if (r->seqId != 0)
        return q + " is bound to a stack - this analysis takes a Series() role: "
                   "one stack is ONE level, and separating the additive term "
                   "from the multiplicative one needs 3";
    if (r->frameUid != 0)
        return q + " is bound to a frame - this analysis takes a Series() role: "
                   "one frame is ONE level, and separating the additive term "
                   "from the multiplicative one needs 3";
    *seriesIdOut = r->seriesId;
    return "";
}

// ---- the results -------------------------------------------------------------
struct SetResult {
    int kind = SA_DSNU;
    bool ok = false;
    std::string refusal;                 // one line, when !ok
    std::string setName;
    int nPl = 1, cfa = 0, cfaPattern = 0;
    double value[4] = {};                // DSNU [DN] | PRNU [%]
    double corrDn[4] = {};               // sqrt(C total), in DN, beside the value
    double uncorr[4] = {};               // what the SAME row reads without the -C
    bool clamped[4] = {};                // the subtraction went negative: declared
    double mu[4] = {};                   // PRNU: the level the ratio is against
    double sigmaD[4] = {};               // PRNU: the corrected fixed pattern, DN
    size_t samples[4] = {};
    // provenance the display may not drop (docs/analysis-layers.md §8)
    std::string cutoff = kSetCutoffRaw;
    std::string form;
    std::vector<SetRoleUse> used;
    // ...and, for a row whose input is a SWEEP, the settled second half of the
    // tag: "- sweep: <series>, M levels" (docs/flat-field-stats.md (c),
    // docs/analysis-layers.md §7). levels == 0 = this row took no sweep, and
    // the clause does not appear.
    std::string sweep;
    int levels = 0;
};

// The result's own name (canon invariant (a): "解析結果は自分の層と (set 解析
// なら) set を名乗る"). docs/flat-field-stats.md (b) fixes the tag as
// "— set: image n/N, dark n/N" and forbids dropping it in a table; the SET NAME
// is added in front of the roles because the invariant asks the result to name
// its set, and a tag that lists roles alone cannot tell two sets apart in one
// report.
static std::string setResultTag(const SetResult& R) {
    std::string s = " - set \"" + R.setName + "\":";
    // The sweep clause comes FIRST because it is what tells this row apart from
    // the direct row of the same name: the number of levels is the estimator's
    // own n, and a reader who sees "4 levels" is not looking at a single pair.
    if (R.levels > 0)
        s += " sweep \"" + R.sweep + "\" " + std::to_string(R.levels) + " levels,";
    for (size_t i = 0; i < R.used.size(); i++) {
        s += i ? "," : "";
        s += " " + R.used[i].role + " " + std::to_string(R.used[i].n) + "/" +
             std::to_string(R.used[i].N);
    }
    return s;
}
static std::string setResultTitle(const SetResult& R) {
    const SetAnalyzerDef& D = kSetAnalyzers[R.kind];
    std::string s = std::string(D.famous) + " [" + D.unit + "]";
    if (R.ok) s += setResultTag(R);
    return s;
}

static void setUseFromFold(SetRoleUse& u, const char* role, const SetFold& F) {
    u.role = role; u.member = F.name; u.headPath = F.headPath;
    u.n = F.frames; u.N = F.expected;
    u.nonFinite = F.nonFinite; u.dropped = F.dropped;
}

// One plane's corrected fixed-pattern variance from a spatial accumulator.
// var_spatial at ddof=1 minus the temporal residual C, clamped at 0 with the
// clamp DECLARED - never a silent 0, which would read as "this sensor has no
// fixed pattern" when what happened is "this stack cannot see one under its own
// temporal floor" (docs/flat-field-stats.md (a)'s guard rule).
struct SetPlaneFpn { double var = 0, corr = 0, fvar = 0; bool clamped = false; };
static SetPlaneFpn setPlaneFpn(double s1, double s2, double corrSum, double n) {
    SetPlaneFpn o;
    if (n < 2.0) return o;
    const double m = s1 / n;
    o.var = std::max(0.0, s2 / n - m * m) * (n / (n - 1.0));
    o.corr = corrSum / n;
    o.fvar = o.var - o.corr;
    o.clamped = o.fvar < 0.0;
    if (o.clamped) o.fvar = 0.0;
    return o;
}

// ---- DSNU: {"dark": Stack()} -------------------------------------------------
static SetResult runDirectDsnu(const App::ASet& S) {
    SetResult R;
    R.kind = SA_DSNU;
    R.setName = S.name;
    R.form = kSetAnalyzers[SA_DSNU].form;
    int darkSeq = 0;
    R.refusal = setRoleRefusal(S, "dark", &darkSeq);
    if (!R.refusal.empty()) return R;
    SetFold Fd;
    if (!setFoldStack(darkSeq, Fd)) {
        R.refusal = Fd.err == "min_frames"
            ? "\"dark\": needs >= 2 frames - \"" + Fd.name + "\" has " +
              std::to_string(Fd.frames) + " of " + std::to_string(Fd.expected) + " loaded"
            : "\"dark\": " + Fd.err;
        return R;
    }
    // No detrend on a dark, ever - and for a reason worth keeping written down:
    // there is no illumination, so there is no shading to remove, and the
    // low-frequency structure a dark does carry (a temperature gradient) IS part
    // of the DSNU rather than something to subtract off it
    // (docs/flat-field-stats.md (b)).
    double s1[4] = {}, s2[4] = {}, cs[4] = {}, n[4] = {};
    for (size_t i = 0; i < Fd.M.size(); i++) {
        if (!std::isfinite(Fd.M[i])) continue;
        const int p = Fd.plane[i];
        s1[p] += Fd.M[i]; s2[p] += Fd.M[i] * Fd.M[i]; cs[p] += Fd.corr[i]; n[p] += 1.0;
    }
    R.nPl = Fd.nPl; R.cfa = Fd.cfa; R.cfaPattern = Fd.cfaPattern;
    for (int p = 0; p < R.nPl; p++) {
        const SetPlaneFpn f = setPlaneFpn(s1[p], s2[p], cs[p], n[p]);
        R.value[p] = sqrt(f.fvar);
        R.corrDn[p] = sqrt(f.corr);
        R.uncorr[p] = sqrt(f.var);        // the bound this row used to be
        R.clamped[p] = f.clamped;
        R.samples[p] = (size_t)n[p];
    }
    R.used.emplace_back();
    setUseFromFold(R.used.back(), "dark", Fd);
    R.ok = true;
    return R;
}

// ---- PRNU: {"image": Stack(), "dark": Stack()} -------------------------------
static SetResult runDirectPrnu(const App::ASet& S) {
    SetResult R;
    R.kind = SA_PRNU;
    R.setName = S.name;
    R.form = kSetAnalyzers[SA_PRNU].form;
    int imgSeq = 0, darkSeq = 0;
    R.refusal = setRoleRefusal(S, "image", &imgSeq);
    if (!R.refusal.empty()) return R;
    R.refusal = setRoleRefusal(S, "dark", &darkSeq);
    if (!R.refusal.empty()) return R;
    SetFold Fi, Fd;
    for (int which = 0; which < 2; which++) {
        SetFold& F = which ? Fd : Fi;
        const char* role = which ? "dark" : "image";
        if (setFoldStack(which ? darkSeq : imgSeq, F)) continue;
        R.refusal = F.err == "min_frames"
            ? std::string("\"") + role + "\": needs >= 2 frames - \"" + F.name +
              "\" has " + std::to_string(F.frames) + " of " +
              std::to_string(F.expected) + " loaded"
            : std::string("\"") + role + "\": " + F.err;
        return R;
    }
    // Shape disagreement. The pixel-wise subtraction is what makes this
    // estimator exact under y = d + g*mu, and it is only defined between two
    // pictures of the same sensor: the refusal NAMES both shapes, the peer's
    // house style since #71.
    if (Fi.W != Fd.W || Fi.H != Fd.H || Fi.C != Fd.C) {
        char b[224];
        snprintf(b, sizeof b,
                 "\"image\" is %dx%dx%dch and \"dark\" is %dx%dx%dch - "
                 "the per-pixel subtraction needs one shape",
                 Fi.W, Fi.H, Fi.C, Fd.W, Fd.H, Fd.C);
        R.refusal = b;
        return R;
    }
    if (Fi.cfa != Fd.cfa || (Fi.cfa != 0 && Fi.cfaPattern != Fd.cfaPattern)) {
        R.refusal = "\"image\" and \"dark\" do not read as the same mosaic - "
                    "CFA planes are never mixed, so the planes have to line up";
        return R;
    }
    double sI[4] = {}, sD[4] = {}, dq[4] = {}, cs[4] = {}, n[4] = {};
    for (size_t i = 0; i < Fi.M.size(); i++) {
        // a sample either side could not measure has no difference either: it
        // leaves BOTH the variance and the mean, and is counted as dropped
        if (!std::isfinite(Fi.M[i]) || !std::isfinite(Fd.M[i])) continue;
        const int p = Fi.plane[i];
        const double d = Fi.M[i] - Fd.M[i];
        sI[p] += Fi.M[i]; sD[p] += Fd.M[i];
        dq[p] += d * d; cs[p] += Fi.corr[i] + Fd.corr[i]; n[p] += 1.0;
    }
    R.nPl = Fi.nPl; R.cfa = Fi.cfa; R.cfaPattern = Fi.cfaPattern;
    bool anyMu = false;
    for (int p = 0; p < R.nPl; p++) {
        if (n[p] < 2.0) continue;
        // mu is written as the settled formula writes it - the difference of the
        // two spatial means, each over the samples that actually entered.
        const double mu = sI[p] / n[p] - sD[p] / n[p];
        const double s1 = sI[p] - sD[p];                 // sum of D over the plane
        const SetPlaneFpn f = setPlaneFpn(s1, dq[p], cs[p], n[p]);
        R.mu[p] = mu;
        R.sigmaD[p] = sqrt(f.fvar);
        R.corrDn[p] = sqrt(f.corr);
        R.clamped[p] = f.clamped;
        R.samples[p] = (size_t)n[p];
        if (mu > 0) {
            R.value[p] = 100.0 * sqrt(f.fvar) / mu;
            R.uncorr[p] = 100.0 * sqrt(f.var) / mu;
            anyMu = true;
        } else {
            R.value[p] = R.uncorr[p] = std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (!anyMu) {
        // "mu <= 0 なら出さない。理由1文" - the ROIs precedent's reason, which is
        // the true one: a ratio to a level needs a level, and zero is not one.
        char b[224];
        snprintf(b, sizeof b,
                 "mean(M_image) - mean(M_dark) = %.6g DN is not above 0 - "
                 "a ratio to a level needs a level", R.mu[0]);
        R.refusal = b;
        return R;
    }
    R.used.emplace_back();
    setUseFromFold(R.used.back(), "image", Fi);
    R.used.emplace_back();
    setUseFromFold(R.used.back(), "dark", Fd);
    R.ok = true;
    return R;
}

// The DIRECT pair only (kind < kSetDirectCount). The separation fit is one
// computation with two named outputs, so it has its own entry point below and
// is never run twice to print two rows.
static SetResult runSetAnalyzer(const App::ASet& S, int kind) {
    return kind == SA_PRNU ? runDirectPrnu(S) : runDirectDsnu(S);
}

// ---- the separation fit: {"sweep": Series(), "dark": Stack()} (#57 判断5) ----
//
// docs/flat-field-stats.md (c) "加法性と乗法性の分離" (確定 2026-08-09, #57).
// One flat level cannot tell an additive fixed pattern from a multiplicative
// one; several levels can, because the two enter the SAME measured quantity
// differently:
//
//     sigma_fpn^2(mu) = DSNU^2 + (PRNU*mu)^2
//
// which is a STRAIGHT LINE in mu^2. So the estimator is a linear regression,
// not a non-linear fit: intercept = DSNU^2, slope = PRNU^2.
//
//   - UNWEIGHTED OLS (判断5, settled). A weight needs a per-point variance
//     estimate that is itself noisy, and with M a handful of points a wrong
//     weight distorts more than it helps. What checks the model here is the
//     RESIDUAL DISPLAY, which is why every level's residual is a field of the
//     result rather than something the panel may or may not draw.
//   - per plane, always. Colour shading is a real phenomenon and pooling R with
//     B would report the mosaic as fixed pattern (docs/terminology.md).
//   - each level's sigma_fpn,l^2 is the (b) CORRECTED estimator, and it is the
//     one setPlaneFpn already computes for the direct rows - the selftest
//     proves that by comparing a level's value to the DSNU of the same stack
//     bit for bit, rather than to a tolerance.
//   - the lit levels are NOT dark-subtracted. Subtracting the dark takes the
//     additive term away, and the additive term IS the intercept; measuring it
//     is the whole reason this fit exists.
//   - mu IS dark-corrected: mu_l = mean(M_l) - mean(M_dark). Run on raw means,
//     the (mu+c)^2 cross term breaks the model. Note what this needs of the
//     dark: a per-plane SCALAR. Nothing subtracts pixel from pixel here, so
//     unlike the direct PRNU this analysis has no reason to demand one shape -
//     what it does demand is that plane p means the same thing on both sides.
//   - M >= 3. Two points fit a line exactly, so M = 2 always looks perfect and
//     the residuals cannot check anything.
//   - STACK members only. A frame member has no s_t, so the -C correction
//     cannot be taken off it, and a sum-standing member is n x the mean's
//     scale (docs/analysis-layers.md §3.3). Both are excluded, and the counts
//     are said out loud - a quiet exclusion is how a sweep silently becomes a
//     different sweep.
static const char* kSepModel = "sigma_fpn^2 = DSNU^2 + (PRNU*mu)^2";
// The assumption the two-parameter model rides on, carried IN the result
// (docs/flat-field-stats.md (c): "無相関を仮定していることを結果の note に
// 明記する"). It lives in this file with the famous names it uses.
static const char* kSepNote =
    "the additive and the multiplicative fixed pattern are assumed "
    "UNCORRELATED. Correlated (column amplifiers do correlate them) the true "
    "form is DSNU^2 + 2*rho*DSNU*PRNU*mu + (PRNU*mu)^2, and a two-parameter fit "
    "cannot see the cross term - it lands inside both coefficients. The "
    "per-level residuals below are what checks the model.";
static const char* kSepSweepWarn =
    "sweep the LIGHT, not the exposure time: swept by time, dark current moves "
    "the additive term itself, the intercept stops being a constant and the "
    "separation means nothing. Nothing here can check which was swept - the "
    "series says what it is.";

struct SepLevel {
    int seqId = 0;
    std::string member, headPath;
    double value = std::numeric_limits<double>::quiet_NaN();  // the series' own
    int n = 0, N = 0;                    // frames aggregated of frames expected
    uint64_t nonFinite = 0;
    size_t dropped = 0;
    double mu[4] = {};                   // mean(M_l) - mean(M_dark), per plane
    double fpnVar[4] = {};               // sigma_fpn,l^2, CORRECTED (the fit's y)
    double fpnVarRaw[4] = {};            // ...and what it reads without the -C
    double corr[4] = {};                 // C_l, this level's own
    bool clamped[4] = {};                // the -C took it below 0: declared
    size_t samples[4] = {};
    double fit[4] = {}, resid[4] = {};   // filled once the line is known
};
struct SepSkip { std::string member, why; };

struct SepFit {
    bool ok = false;
    std::string refusal;
    std::string setName, seriesName, paramName, unit;
    int nPl = 1, cfa = 0, cfaPattern = 0;
    int members = 0, M = 0;              // sweep members seen / levels fitted
    int exFrame = 0, exSum = 0, exShort = 0, exNotInc = 0, exNoValue = 0, exGone = 0;
    std::vector<SepSkip> skipped;        // every exclusion, named and reasoned
    std::vector<SepLevel> levels;        // the ones that ENTERED the fit
    bool planeFit[4] = {};
    std::string planeWhy[4];
    double slope[4] = {}, inter[4] = {}, r2[4] = {}, rms[4] = {};
    double slopeRaw[4] = {}, interRaw[4] = {};   // the same OLS with no -C
    SetResult dsnu, prnu;                // the two named outputs of ONE fit
    std::string cutoff = kSetCutoffRaw;
    std::string note = kSepNote, sweepWarn = kSepSweepWarn;
};

// The census, out loud. ONE function so the screen, the M < 3 refusal and the
// selftest cannot describe the same exclusion differently.
static std::string sepLevelCensus(const SepFit& F) {
    std::string s = "levels: " + std::to_string(F.M) + " of " +
                    std::to_string(F.members) + " sweep member(s) used";
    std::vector<std::string> ex;
    auto add = [&](int n, const char* why) {
        if (n) ex.push_back(std::to_string(n) + " " + why);
    };
    add(F.exFrame,  "frame member(s) - no time axis, so no -sigma_t^2/N correction");
    add(F.exSum,    "standing as sum - a sum is n x the mean's scale");
    add(F.exShort,  "stack(s) with fewer than 2 frames loaded - same reason as a frame");
    add(F.exNotInc, "not included");
    add(F.exNoValue, "with no value set");
    add(F.exGone,   "whose member is gone");
    if (ex.empty()) return s;
    s += " - excluded: ";
    for (size_t i = 0; i < ex.size(); i++) s += (i ? ", " : "") + ex[i];
    return s;
}

static SepFit runSeparationFit(const App::ASet& S) {
    // NOT BUILT YET. The registry row, the role schema, the result vessel and
    // the naming are here; the estimator is what the next commit adds. Every
    // caller therefore gets the same one line, and every F* assert of
    // --setanalysis-selftest is red.
    SepFit F;
    F.setName = S.name;
    for (int i = 0; i < 2; i++) {
        SetResult& R = i ? F.prnu : F.dsnu;
        R.kind = i ? SA_SEP_PRNU : SA_SEP_DSNU;
        R.setName = S.name;
        R.form = kSetAnalyzers[R.kind].form;
    }
    F.refusal = "the separation fit is not built yet";
    F.dsnu.refusal = F.prnu.refusal = F.refusal;
    (void)setSeriesRoleRefusal;      // the Series() resolver, waiting for a caller
    (void)kSepModel;                 // ...and the caption the panel will print
    return F;
}
