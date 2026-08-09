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
enum SetAnalyzerKind { SA_DSNU = 0, SA_PRNU = 1, SA_COUNT = 2 };

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
};

// "カットオフを必ず言う" (docs/flat-field-stats.md 約束1) makes the cutoff a
// REQUIRED field of the result, so a value and the spatial band it was measured
// over never travel apart. Stage 2 landed the field with one possible value;
// the detrend stage (判断6/7) has since landed, and the wording now comes from
// the stage itself (core/app/detrend.inc) rather than being written twice.
//
// The DEFAULT IS STILL RAW, and that is 判断7 rather than an omission: a
// default cutoff quietly becomes the house standard, so the self-describing
// default is "did nothing". What stops that from being a silent choice is the
// shading figure printed beside every value below - measured whether or not
// anything was removed, always with the same probe.
static const std::string kSetCutoffRaw = detrendCutoffText(DetrendSpec{});

// Which cutoff a bound member actually carries. A role resolves to a STACK, and
// a stack that this program produced with the detrend stage carries the method
// and the window on its own SeqInfo (docs/analysis-layers.md §5: the origin
// travels with the product). So the cutoff of a result is a FACT ABOUT ITS
// INPUTS, read off them here - never a switch on this analysis, which 判断6
// forbids, and never a field somebody has to remember to set.
static std::string setCutoffOfSeq(int seqId) {
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si || !si->dt.made()) return kSetCutoffRaw;
    return detrendCutoffText(si->dt.spec) + " (from the preprocessor product \"" +
           si->name + "\")";
}
// Two roles, two inputs, and they need not agree - so say both rather than
// picking one. A pixel-wise subtraction between a detrended image and a raw
// dark is a real thing to do by mistake, and the cutoff line is where it shows.
static std::string setCutoffOfRoles(const std::vector<std::pair<std::string, int>>& roles) {
    std::string s;
    bool same = true;
    for (size_t i = 1; i < roles.size(); i++)
        if (setCutoffOfSeq(roles[i].second) != setCutoffOfSeq(roles[0].second)) same = false;
    if (same && !roles.empty()) return setCutoffOfSeq(roles[0].second);
    for (size_t i = 0; i < roles.size(); i++)
        s += (i ? "; " : "") + roles[i].first + ": " + setCutoffOfSeq(roles[i].second);
    return s;
}

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
    int seqId = 0;                        // which stack: the cutoff is read off it
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
    F.seqId = seqId;
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
    // THE SHADING FIGURE, 判断7's other half: "既定は「なし (raw)」... その代わり、
    // シェーディング量は常に併記する ... 隣に shading [%] が立っていれば、
    // 「除去すべきだったか」は読み手が判定できる - 既定 OFF が黙認にならない
    // 仕掛けはこれ". Measured on the very picture this row's statistic is taken
    // over, always with the same degree-2 probe (core/app/detrend.inc), so it
    // means the same thing beside a raw row and beside a detrended one. Never
    // conditional on anything: a figure that appears only sometimes cannot be
    // what makes a default honest.
    double shadePp[4] = {};              // p-p of the low-frequency component, DN
    double shadePct[4] = {};             // ...as a % of the field centre
    bool shadePctOk[4] = {};             // false = no level to take a ratio against
    // provenance the display may not drop (docs/analysis-layers.md §8)
    std::string cutoff = kSetCutoffRaw;
    std::string form;
    std::vector<SetRoleUse> used;
};

// The figure, off a mean image the fold already built. The plane map is the
// FOLD's own, not a fresh one, so the shading reported is the shading of
// exactly the population the statistic beside it pooled.
static void setShadingOf(SetResult& R, const std::vector<double>& img, const SetFold& F) {
    DetrendReport S = detrendProbeT(img.data(), F.W, F.H, F.C, F.plane.data(), F.nPl);
    for (int p = 0; p < R.nPl && p < 4; p++) {
        R.shadePp[p] = S.pl[p].ppDn;
        R.shadePct[p] = S.pl[p].pct;
        R.shadePctOk[p] = S.pl[p].pctOk;
    }
}

// The result's own name (canon invariant (a): "解析結果は自分の層と (set 解析
// なら) set を名乗る"). docs/flat-field-stats.md (b) fixes the tag as
// "— set: image n/N, dark n/N" and forbids dropping it in a table; the SET NAME
// is added in front of the roles because the invariant asks the result to name
// its set, and a tag that lists roles alone cannot tell two sets apart in one
// report.
static std::string setResultTag(const SetResult& R) {
    std::string s = " - set \"" + R.setName + "\":";
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
    // The shading of the picture this row measured. On a dark the ratio to the
    // centre usually has no level to be a ratio of, and the p-p carries it
    // instead - which is the honest reading anyway: a dark's low-frequency
    // structure (a temperature gradient) IS part of this quantity and is not
    // something to remove (docs/flat-field-stats.md (b)). The figure is here so
    // it can be SEEN, not so it can be subtracted.
    setShadingOf(R, Fd.M, Fd);
    R.cutoff = setCutoffOfSeq(Fd.seqId);
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
    // The shading of the picture the ratio was taken over: D, the difference
    // image, not the raw flat. D is what var_spatial ran on, so D is what the
    // figure beside it has to describe - and its centre level IS mu, so here
    // the ratio to the centre is defined and reads as "this fraction of the
    // signal is low-frequency illumination structure".
    {
        std::vector<double> D(Fi.M.size(), std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 0; i < D.size(); i++)
            if (std::isfinite(Fi.M[i]) && std::isfinite(Fd.M[i])) D[i] = Fi.M[i] - Fd.M[i];
        setShadingOf(R, D, Fi);
    }
    R.cutoff = setCutoffOfRoles({ { "image", Fi.seqId }, { "dark", Fd.seqId } });
    R.used.emplace_back();
    setUseFromFold(R.used.back(), "image", Fi);
    R.used.emplace_back();
    setUseFromFold(R.used.back(), "dark", Fd);
    R.ok = true;
    return R;
}

static SetResult runSetAnalyzer(const App::ASet& S, int kind) {
    return kind == SA_PRNU ? runDirectPrnu(S) : runDirectDsnu(S);
}
