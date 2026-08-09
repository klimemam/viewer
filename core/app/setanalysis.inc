// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ------------------------------------------------- SetAnalyzer: direct DSNU / PRNU (#57 判断4)
//
// RED. This file is the shape of the two SetAnalyzers docs/flat-field-stats.md
// (b) settled on 2026-08-09, and nothing else: the registry, the result vessel,
// the naming - and one runner that refuses everything. The selftest beside it
// says what they have to answer; none of it is answered yet, which is the point
// of this commit.
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
// residual STILL IN that mean, and every spatial variance at ddof = 1.
//
// The PRNU form is the PIXEL-WISE SUBTRACTION one, settled in #57 judgment 4;
// the variance-difference form is a different quantity and is reserved for a
// different name after the standards check. NO STANDARD IS NAMED HERE.
//
// -------- the famous names live HERE, and only here --------------------------
// docs/terminology.md's invariant: "前処理・参照を含意する有名名 (PRNU / DSNU
// …) は SetAnalyzer だけが名乗る。General は中立な量の名前だけを出す — 例外
// なし". A rule nobody can check is a rule that decays: the ROIs column was
// renamed for it in 5d8cb72 and the tooltip UNDER that column has gone on
// printing "PRNU" over a General/frame ratio ever since. So the bare names are
// string literals of this one file, and --setanalysis-selftest scans the
// shipping sources to hold that true. It does not hold today: the scan is red.
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

// The detrend stage (docs/flat-field-stats.md 判断6/7) is not built. Skipping an
// unbuilt stage is legal; skipping it SILENTLY is not - "カットオフを必ず言う"
// makes the cutoff a required field of the result.
static const char* kSetCutoffRaw = "raw (no shading removal - the detrend stage is not built)";

struct SetRoleUse {
    std::string role, member, headPath;
    int n = 0, N = 0;                    // frames aggregated of frames expected
    uint64_t nonFinite = 0;
    size_t dropped = 0;
};

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
    std::string cutoff = kSetCutoffRaw;
    std::string form;
    std::vector<SetRoleUse> used;
};

// The result's own name (canon invariant (a): "解析結果は自分の層と (set 解析
// なら) set を名乗る"), with the tag docs/flat-field-stats.md (b) fixes.
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

// Nothing runs yet: no fold, no role resolution, no refusal that says anything
// about THIS set.
static SetResult runSetAnalyzer(const App::ASet& S, int kind) {
    SetResult R;
    R.kind = kind;
    R.setName = S.name;
    R.form = kSetAnalyzers[kind].form;
    R.refusal = "not implemented";
    return R;
}
