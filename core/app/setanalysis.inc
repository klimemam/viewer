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
//
// Takes the cutoff TEXT and not a seqId, because a role folded on a peer has no
// local SeqInfo to read one off: what the peer folded is a file, and a file on
// disk is raw. The detrend stage makes a first-class stack HERE, so a detrended
// role is by construction a local one.
static std::string setCutoffOfRoles(const std::vector<std::pair<std::string, std::string>>& roles) {
    std::string s;
    bool same = true;
    for (size_t i = 1; i < roles.size(); i++)
        if (roles[i].second != roles[0].second) same = false;
    if (same && !roles.empty()) return roles[0].second;
    for (size_t i = 0; i < roles.size(); i++)
        s += (i ? "; " : "") + roles[i].first + ": " + roles[i].second;
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

// ---- the fold, REDUCED: the only thing that has to cross a machine ----------
//
// docs/analysis-layers.md §3.5 draws the line at "the aggregate runs where the
// pixels are", and §6 at "fit・KPI はスカラの上". Everything below this point is
// composition - sqrt(max(0, var - C)), the clamp, mu, the ratio, the OLS - and
// every one of those runs HERE, on this machine, out of the handful of scalars
// in these two structs. What produced the scalars is a transport question: the
// local fold above, or a peer's MOP_SET_FOLD (core/app/setanalysis_remote.inc).
//
// So the equality between a local run and a peer run is STRUCTURAL rather than
// maintained: there is one composer per row and it does not know which side
// folded. The reductions themselves are one function too (core/setfold.h),
// called by this file and by core/serve.cpp.
struct SetRoleFold {
    std::string role;                 // the schema's name
    // The BINDING is the client's: a peer knows a path, not which stack a set
    // declared to be the dark. n/N are the peer's facts (#134 RP13); the name
    // and the head path are ours.
    std::string member, headPath;
    bool folded = false;              // false = fewer than 2 frames, which is a
                                      // REFUSAL of the analysis and worded below
    int W = 0, H = 0, C = 0, nPl = 1;
    int cfa = 0, cfaPattern = 0;
    int frames = 0, expected = 0;     // n of N (the canon's mandatory pair)
    uint64_t nonFinite = 0;
    size_t dropped = 0;
    setfold::PlaneAcc pl[4];
    DetrendReport shade;              // of THIS role's mean image, when asked for
    std::string cutoff;               // "" = raw (a peer folds files, and files are raw)
};
// The join: what two roles' mean images make together, reduced where they both
// are. Never composable from the two roles' own sums - see core/setfold.h.
struct SetJoinFold {
    bool ok = false;
    int nPl = 1;
    setfold::PairAcc pl[4];
    DetrendReport shade;              // of D, which is the picture the ratio is over
};

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
    // WHO folded it (docs/analysis-layers.md §8's 計算者欄, #46). Empty = this
    // machine, which is what the panel has always printed as "[local]"; a peer
    // fold carries the peer's own viewer version, read off its reply and never
    // filled in from ours - a citation names the computer.
    std::string computedBy;
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

static void setUseFromFold(SetRoleUse& u, const SetRoleFold& F) {
    u.role = F.role; u.member = F.member; u.headPath = F.headPath;
    u.n = F.frames; u.N = F.expected;
    u.nonFinite = F.nonFinite; u.dropped = F.dropped;
}

// The local fold, reduced. `wantShade` because the figure beside a row has to
// describe the picture THAT ROW measured: DSNU measures the dark's mean image
// and asks for it here, while PRNU measures D and asks for it on the join - so
// nothing fits a surface it will not print.
static void setReduceLocal(SetRoleFold& R, const char* role, const SetFold& F,
                           bool wantShade) {
    R.role = role;
    R.member = F.name;
    R.headPath = F.headPath;
    R.folded = F.valid;
    R.W = F.W; R.H = F.H; R.C = F.C; R.nPl = F.nPl;
    R.cfa = F.cfa; R.cfaPattern = F.cfaPattern;
    R.frames = F.frames; R.expected = F.expected;
    R.nonFinite = F.nonFinite; R.dropped = F.dropped;
    if (!F.valid) return;
    R.cutoff = setCutoffOfSeq(F.seqId);
    setfold::reduceOne(F.M.data(), F.corr.data(), F.plane.data(), F.M.size(), R.pl);
    if (wantShade)
        R.shade = detrendProbeT(F.M.data(), F.W, F.H, F.C, F.plane.data(), F.nPl);
}
static void setReduceLocalPair(SetJoinFold& J, const SetFold& Fa, const SetFold& Fb) {
    J.nPl = Fa.nPl;
    setfold::reducePair(Fa.M.data(), Fa.corr.data(), Fb.M.data(), Fb.corr.data(),
                        Fa.plane.data(), Fa.M.size(), J.pl);
    std::vector<double> D(Fa.M.size());
    setfold::differenceImage(Fa.M.data(), Fb.M.data(), Fa.M.size(), D.data());
    J.shade = detrendProbeT(D.data(), Fa.W, Fa.H, Fa.C, Fa.plane.data(), Fa.nPl);
    J.ok = true;
}

// The shading figure, off whichever probe the row's own picture produced.
static void setShadingFrom(SetResult& R, const DetrendReport& S) {
    for (int p = 0; p < R.nPl && p < 4; p++) {
        R.shadePp[p] = S.pl[p].ppDn;
        R.shadePct[p] = S.pl[p].pct;
        R.shadePctOk[p] = S.pl[p].pctOk;
    }
}

// The one-line refusal for a role that could not be folded, in the settled
// words. Composed from the role's own facts, so it reads identically whether
// the frames were counted here or on a peer.
static std::string setShortRoleRefusal(const SetRoleFold& F) {
    return "\"" + F.role + "\": needs >= " + std::to_string(setfold::kMinFrames) +
           " frames - \"" + F.member + "\" has " + std::to_string(F.frames) +
           " of " + std::to_string(F.expected) + " loaded";
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
//
// The COMPOSITION, out of the four numbers per plane a fold reduces to. It does
// not know, and must not know, which machine produced them - that is the whole
// content of "one estimator, two transports" (docs/analysis-layers.md §3.5).
static SetResult composeDirectDsnu(const std::string& setName, const SetRoleFold& D) {
    SetResult R;
    R.kind = SA_DSNU;
    R.setName = setName;
    R.form = kSetAnalyzers[SA_DSNU].form;
    if (!D.folded) { R.refusal = setShortRoleRefusal(D); return R; }
    // No detrend on a dark, ever - and for a reason worth keeping written down:
    // there is no illumination, so there is no shading to remove, and the
    // low-frequency structure a dark does carry (a temperature gradient) IS part
    // of the DSNU rather than something to subtract off it
    // (docs/flat-field-stats.md (b)).
    R.nPl = D.nPl; R.cfa = D.cfa; R.cfaPattern = D.cfaPattern;
    for (int p = 0; p < R.nPl; p++) {
        const SetPlaneFpn f = setPlaneFpn(D.pl[p].s1, D.pl[p].s2, D.pl[p].cs, D.pl[p].n);
        R.value[p] = sqrt(f.fvar);
        R.corrDn[p] = sqrt(f.corr);
        R.uncorr[p] = sqrt(f.var);        // the bound this row used to be
        R.clamped[p] = f.clamped;
        R.samples[p] = (size_t)D.pl[p].n;
    }
    // The shading of the picture this row measured. On a dark the ratio to the
    // centre usually has no level to be a ratio of, and the p-p carries it
    // instead - which is the honest reading anyway: a dark's low-frequency
    // structure (a temperature gradient) IS part of this quantity and is not
    // something to remove (docs/flat-field-stats.md (b)). The figure is here so
    // it can be SEEN, not so it can be subtracted.
    setShadingFrom(R, D.shade);
    R.cutoff = D.cutoff.empty() ? kSetCutoffRaw : D.cutoff;
    R.used.emplace_back();
    setUseFromFold(R.used.back(), D);
    R.ok = true;
    return R;
}

static SetResult runDirectDsnu(const App::ASet& S) {
    SetResult R;
    R.kind = SA_DSNU;
    R.setName = S.name;
    R.form = kSetAnalyzers[SA_DSNU].form;
    int darkSeq = 0;
    R.refusal = setRoleRefusal(S, "dark", &darkSeq);
    if (!R.refusal.empty()) return R;
    SetFold Fd;
    if (!setFoldStack(darkSeq, Fd) && Fd.err != "min_frames") {
        R.refusal = "\"dark\": " + Fd.err;
        return R;
    }
    SetRoleFold D;
    setReduceLocal(D, "dark", Fd, true);
    return composeDirectDsnu(S.name, D);
}

// ---- PRNU: {"image": Stack(), "dark": Stack()} -------------------------------
//
// The composition again - but note what it takes: the JOIN, not the two roles'
// own sums. sum(D^2) is not a function of sum(M_i^2) and sum(M_d^2), and the
// route that would make it one (a sixth scalar, the joint moment sum(M_i*M_d))
// is algebraically equal and a different sequence of operations, so it does not
// return this row's binary64. The difference image is therefore formed where
// both pictures are, and the five numbers per plane below are what travels -
// core/setfold.h has the measurement that settled it.
static SetResult composeDirectPrnu(const std::string& setName, const SetRoleFold& I,
                                   const SetRoleFold& D, const SetJoinFold& J) {
    SetResult R;
    R.kind = SA_PRNU;
    R.setName = setName;
    R.form = kSetAnalyzers[SA_PRNU].form;
    if (!I.folded) { R.refusal = setShortRoleRefusal(I); return R; }
    if (!D.folded) { R.refusal = setShortRoleRefusal(D); return R; }
    // Shape disagreement. The pixel-wise subtraction is what makes this
    // estimator exact under y = d + g*mu, and it is only defined between two
    // pictures of the same sensor: the refusal NAMES both shapes, the peer's
    // house style since #71.
    if (I.W != D.W || I.H != D.H || I.C != D.C) {
        char b[224];
        snprintf(b, sizeof b,
                 "\"image\" is %dx%dx%dch and \"dark\" is %dx%dx%dch - "
                 "the per-pixel subtraction needs one shape",
                 I.W, I.H, I.C, D.W, D.H, D.C);
        R.refusal = b;
        return R;
    }
    if (I.cfa != D.cfa || (I.cfa != 0 && I.cfaPattern != D.cfaPattern)) {
        R.refusal = "\"image\" and \"dark\" do not read as the same mosaic - "
                    "CFA planes are never mixed, so the planes have to line up";
        return R;
    }
    if (!J.ok) {
        // The two roles line up and the join still did not arrive: that is a
        // transport fact, not a statement about the data, and it says so.
        R.refusal = "the per-pixel difference of \"image\" and \"dark\" was not "
                    "computed - this row is not composed from anything else";
        return R;
    }
    R.nPl = I.nPl; R.cfa = I.cfa; R.cfaPattern = I.cfaPattern;
    bool anyMu = false;
    for (int p = 0; p < R.nPl; p++) {
        const setfold::PairAcc& a = J.pl[p];
        if (a.n < 2.0) continue;
        // mu is written as the settled formula writes it - the difference of the
        // two spatial means, each over the samples that actually entered.
        const double mu = a.s1a / a.n - a.s1b / a.n;
        const double s1 = a.s1a - a.s1b;                 // sum of D over the plane
        const SetPlaneFpn f = setPlaneFpn(s1, a.dq, a.cs, a.n);
        R.mu[p] = mu;
        R.sigmaD[p] = sqrt(f.fvar);
        R.corrDn[p] = sqrt(f.corr);
        R.clamped[p] = f.clamped;
        R.samples[p] = (size_t)a.n;
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
    setShadingFrom(R, J.shade);
    R.cutoff = setCutoffOfRoles({ { "image", I.cutoff.empty() ? kSetCutoffRaw : I.cutoff },
                                  { "dark",  D.cutoff.empty() ? kSetCutoffRaw : D.cutoff } });
    R.used.emplace_back();
    setUseFromFold(R.used.back(), I);
    R.used.emplace_back();
    setUseFromFold(R.used.back(), D);
    R.ok = true;
    return R;
}

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
        if (F.err == "min_frames") continue;   // composed below, in the settled words
        R.refusal = std::string("\"") + role + "\": " + F.err;
        return R;
    }
    SetRoleFold I, D;
    setReduceLocal(I, "image", Fi, false);
    setReduceLocal(D, "dark", Fd, false);
    SetJoinFold J;
    if (I.folded && D.folded && I.W == D.W && I.H == D.H && I.C == D.C &&
        I.nPl == D.nPl)
        setReduceLocalPair(J, Fi, Fd);
    return composeDirectPrnu(S.name, I, D, J);
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

// What the separation fit takes off the dark: a per-plane SCALAR, and nothing
// else. Unlike the direct PRNU nothing subtracts pixel from pixel here, which
// is why this row needs no join on the wire at all - one MOP_SET_FOLD with
// SJ_NONE over M+1 roles is the whole request.
static void sepDarkMeans(const setfold::PlaneAcc* acc, double* out) {
    for (int p = 0; p < 4; p++)
        out[p] = acc[p].n > 0 ? acc[p].s1 / acc[p].n
                              : std::numeric_limits<double>::quiet_NaN();
}

// One level's contribution to the fit, from that level's four numbers and the
// dark's one. The SAME per-plane arithmetic the direct rows use - not "the same
// formula", the same function - and the same one whichever side reduced.
static void sepLevelFromAcc(SepLevel& Lv, int p, const setfold::PlaneAcc& a,
                            double darkMean) {
    const SetPlaneFpn f = setPlaneFpn(a.s1, a.s2, a.cs, a.n);
    Lv.mu[p] = a.n > 0 ? a.s1 / a.n - darkMean
                       : std::numeric_limits<double>::quiet_NaN();
    Lv.fpnVar[p] = f.fvar;
    Lv.fpnVarRaw[p] = f.var;
    Lv.corr[p] = f.corr;
    Lv.clamped[p] = f.clamped;
    Lv.samples[p] = (size_t)a.n;
}

// The refusal, on all three faces of the same fit: the fit itself and the two
// named rows it would have produced.
static SepFit& sepRefuse(SepFit& F, const std::string& why) {
    F.refusal = why;
    F.dsnu.refusal = why;
    F.prnu.refusal = why;
    return F;
}

// The FIT, composed out of levels that are already per-plane scalars. Split
// from the gathering above it for the same reason the direct rows were: what a
// level contributes is (sum M, sum M^2, sum C, n) and a mu, so the levels can
// have been folded anywhere. This is the row PR #131 said needed the peer most
// - it folds M+1 stacks - and it is also the row that needed the least, because
// nothing here ever wanted a picture.
static SepFit& sepComposeFit(SepFit& F, const SetRoleFold& dark) {
    F.M = (int)F.levels.size();
    if (F.M < 3)
        return sepRefuse(F, "the sweep \"" + F.seriesName + "\" has " +
                            std::to_string(F.M) + " usable level(s) - the separation "
                            "needs 3 (two points fit a line exactly, so there is no "
                            "residual left to check the model with). " +
                            sepLevelCensus(F));

    // ---- the OLS, per plane, unweighted (判断5) -----------------------------
    // x = mu^2, y = the corrected sigma_fpn^2. Written in the centred form
    // (Sxy/Sxx) rather than the normal-equation one: with mu ~ 3000 DN, x ~ 1e7
    // and the raw form squares that again, which is a real loss of digits on a
    // quantity whose intercept is the answer.
    int fitted = 0;
    for (int p = 0; p < F.nPl; p++) {
        std::vector<double> xs, ys, yr;
        for (const auto& L : F.levels) {
            if (!std::isfinite(L.mu[p])) continue;
            xs.push_back(L.mu[p] * L.mu[p]);
            ys.push_back(L.fpnVar[p]);
            yr.push_back(L.fpnVarRaw[p]);
        }
        if ((int)xs.size() < 3) {
            F.planeWhy[p] = "fewer than 3 levels have a mu on this plane";
            continue;
        }
        const double n = (double)xs.size();
        double xb = 0, yb = 0, rb = 0;
        for (size_t i = 0; i < xs.size(); i++) { xb += xs[i]; yb += ys[i]; rb += yr[i]; }
        xb /= n; yb /= n; rb /= n;
        double Sxx = 0, Sxy = 0, Sxr = 0;
        for (size_t i = 0; i < xs.size(); i++) {
            const double dx = xs[i] - xb;
            Sxx += dx * dx;
            Sxy += dx * (ys[i] - yb);
            Sxr += dx * (yr[i] - rb);
        }
        if (!(Sxx > 0)) {
            // every level landed at the same mu: there is one x, and one x
            // cannot tell a constant from something proportional to it
            F.planeWhy[p] = "every level lands at the same mu - one abscissa "
                            "cannot separate a constant from a slope";
            continue;
        }
        F.slope[p] = Sxy / Sxx;
        F.inter[p] = yb - F.slope[p] * xb;
        F.slopeRaw[p] = Sxr / Sxx;
        F.interRaw[p] = rb - F.slopeRaw[p] * xb;
        double ssr = 0, sst = 0;
        for (size_t i = 0; i < xs.size(); i++) {
            const double f = F.slope[p] * xs[i] + F.inter[p];
            ssr += (ys[i] - f) * (ys[i] - f);
            sst += (ys[i] - yb) * (ys[i] - yb);
        }
        F.rms[p] = sqrt(ssr / n);
        F.r2[p] = sst > 0 ? 1.0 - ssr / sst
                          : std::numeric_limits<double>::quiet_NaN();
        F.planeFit[p] = true;
        fitted++;
        for (auto& L : F.levels) {
            if (!std::isfinite(L.mu[p])) continue;
            L.fit[p] = F.slope[p] * L.mu[p] * L.mu[p] + F.inter[p];
            L.resid[p] = L.fpnVar[p] - L.fit[p];
        }
    }
    if (!fitted) {
        // no plane produced a line, so there is no number anywhere: the whole
        // analysis refuses, in the first plane's own words
        for (int p = 0; p < F.nPl; p++)
            if (!F.planeWhy[p].empty()) return sepRefuse(F, F.planeWhy[p]);
        return sepRefuse(F, "the levels do not make a line on any plane");
    }

    // ---- the two named outputs ---------------------------------------------
    // Both coefficients are variances, so both can come out negative on data
    // whose fixed pattern is under the fit's own scatter. Clamped at 0 with the
    // clamp DECLARED - the guard rule of docs/flat-field-stats.md (a), and the
    // same one the direct rows follow.
    for (int i = 0; i < 2; i++) {
        SetResult& R = i ? F.prnu : F.dsnu;
        R.nPl = F.nPl; R.cfa = F.cfa; R.cfaPattern = F.cfaPattern;
        R.sweep = F.seriesName;
        R.levels = F.M;
        for (int p = 0; p < F.nPl; p++) {
            if (!F.planeFit[p]) {
                R.value[p] = R.uncorr[p] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            const double c = i ? F.slope[p] : F.inter[p];
            const double cr = i ? F.slopeRaw[p] : F.interRaw[p];
            const double k = i ? 100.0 : 1.0;
            R.value[p] = k * sqrt(std::max(0.0, c));
            R.uncorr[p] = k * sqrt(std::max(0.0, cr));
            R.clamped[p] = c < 0.0;
            R.samples[p] = (size_t)F.M;
        }
        R.used.emplace_back();
        setUseFromFold(R.used.back(), dark);
        R.ok = true;
    }
    F.ok = true;
    return F;
}

static SepFit runSeparationFit(const App::ASet& S) {
    SepFit F;
    F.setName = S.name;
    for (int i = 0; i < 2; i++) {
        SetResult& R = i ? F.prnu : F.dsnu;
        R.kind = i ? SA_SEP_PRNU : SA_SEP_DSNU;
        R.setName = S.name;
        R.form = kSetAnalyzers[R.kind].form;
    }
    auto refuse = [&F](const std::string& why) -> SepFit& { return sepRefuse(F, why); };
    int seriesId = 0, darkSeq = 0;
    std::string why = setSeriesRoleRefusal(S, "sweep", &seriesId);
    if (!why.empty()) return refuse(why);
    why = setRoleRefusal(S, "dark", &darkSeq);
    if (!why.empty()) return refuse(why);
    const App::Series* Ser = seriesById(seriesId);
    if (!Ser) return refuse("\"sweep\" names a series that is not open anymore");
    F.seriesName = Ser->name;
    F.paramName = Ser->paramName;
    F.unit = Ser->unit;

    // ---- the dark, which contributes ONE SCALAR PER PLANE ------------------
    SetFold Fd;
    if (!setFoldStack(darkSeq, Fd))
        return refuse(Fd.err == "min_frames"
            ? "\"dark\": needs >= 2 frames - \"" + Fd.name + "\" has " +
              std::to_string(Fd.frames) + " of " + std::to_string(Fd.expected) + " loaded"
            : "\"dark\": " + Fd.err);
    setfold::PlaneAcc dacc[4];
    setfold::reduceOne(Fd.M.data(), Fd.corr.data(), Fd.plane.data(), Fd.M.size(), dacc);
    double darkMean[4];
    sepDarkMeans(dacc, darkMean);
    F.nPl = Fd.nPl; F.cfa = Fd.cfa; F.cfaPattern = Fd.cfaPattern;

    // ---- the levels --------------------------------------------------------
    // Order of the tests is deliberate: what the member IS comes before what
    // the user DECLARED about it, so "this can never be a level" and "you took
    // this one out" never get reported as each other.
    F.members = (int)Ser->members.size();
    for (const auto& m : Ser->members) {
        if (m.seqId == 0) {
            const ImageDoc* fd = frameMemberDoc(m.frameUid);
            if (!fd) { F.exGone++; continue; }
            F.exFrame++;
            F.skipped.push_back({ fd->name,
                "a frame member has no time axis, so its s_t - and the "
                "-sigma_t^2/N correction made of it - do not exist" });
            continue;
        }
        const App::SeqInfo* si = seqInfo(m.seqId);
        if (!si) { F.exGone++; continue; }
        if (m.fold == FOLD_SUM) {
            F.exSum++;
            F.skipped.push_back({ si->name,
                "stands as sum - a sum is n x the mean's scale, so its "
                "sigma_fpn^2 and its mu are not the other levels' quantities" });
            continue;
        }
        if (!m.include) {
            F.exNotInc++;
            F.skipped.push_back({ si->name, "not included (the series' own switch)" });
            continue;
        }
        if (!std::isfinite(m.value)) {
            F.exNoValue++;
            F.skipped.push_back({ si->name,
                "no value set - UNSET is not 0, and the series' rule is that it "
                "does not enter a fit" });
            continue;
        }
        SetFold Fl;
        if (!setFoldStack(m.seqId, Fl)) {
            if (Fl.err == "min_frames") {
                F.exShort++;
                F.skipped.push_back({ si->name,
                    "needs >= 2 frames - " + std::to_string(Fl.frames) + " of " +
                    std::to_string(Fl.expected) + " loaded" });
                continue;
            }
            return refuse("\"sweep\" member \"" + si->name + "\": " + Fl.err);
        }
        // Planes, not pixels. mu takes a per-plane MEAN off the dark, so W x H
        // may differ; what may not differ is what plane p means, or the two
        // sides are not the same detector read the same way.
        if (Fl.C != Fd.C || Fl.nPl != Fd.nPl || Fl.cfa != Fd.cfa ||
            (Fl.cfa != 0 && Fl.cfaPattern != Fd.cfaPattern)) {
            char b[288];
            snprintf(b, sizeof b,
                     "sweep member \"%s\" is %dch / %d plane(s) and \"dark\" is "
                     "%dch / %d plane(s) - mu is a per-plane difference, so the "
                     "planes have to mean the same thing on both sides",
                     si->name.c_str(), Fl.C, Fl.nPl, Fd.C, Fd.nPl);
            return refuse(b);
        }
        // the same reduction the peer runs, by being the same function
        setfold::PlaneAcc acc[4];
        setfold::reduceOne(Fl.M.data(), Fl.corr.data(), Fl.plane.data(), Fl.M.size(), acc);
        SepLevel Lv;
        Lv.seqId = m.seqId;
        Lv.member = Fl.name;
        Lv.headPath = Fl.headPath;
        Lv.value = m.value;
        Lv.n = Fl.frames; Lv.N = Fl.expected;
        Lv.nonFinite = Fl.nonFinite; Lv.dropped = Fl.dropped;
        for (int p = 0; p < F.nPl; p++)
            sepLevelFromAcc(Lv, p, acc[p], darkMean[p]);
        F.levels.push_back(std::move(Lv));
    }
    SetRoleFold DK;
    setReduceLocal(DK, "dark", Fd, false);
    return sepComposeFit(F, DK);
}
