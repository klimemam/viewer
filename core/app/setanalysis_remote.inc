// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: remote/解析
// ------------------------------------------------- set folding on a peer (MOP_SET_FOLD)
//
// docs/analysis-layers.md §3.5's last line - 「set の集計も同じ線」 - and the
// half of it that is a wire question. PR #130 判断2 and PR #131 判断2 both
// recorded this as missing; PR #134 measured exactly what §10 gave it for free
// and what it did not: the transport, the pin budget, the on-demand reader, the
// parity discipline and the provenance trailer all transfer, and the REQUEST
// GRAMMAR does not, because MeasureReqHead carries one flat path list and a set
// is N stacks with named roles.
//
// This file is the client half of the grammar that closes that. It does three
// things and deliberately no more:
//   1. builds a MOP_SET_FOLD request out of {role: paths} and OUR fold's own
//      declared form (setfold::foldForm - the parity token for a built-in);
//   2. reads the reply's per-role columns back into the SAME SetRoleFold /
//      SetJoinFold the local fold reduces to;
//   3. hands them to the SAME composers in core/app/setanalysis.inc.
//
// Nothing here computes a number. That is the point: if this file could
// compute, there would be two implementations of an estimator again, and PR
// #127's P1..P5 is what that costs. The equality --rset-selftest asserts is
// structural.
//
// WHAT IS NOT HERE: any way for a PANEL to ask for this. The Analysis panel had
// the same gap when §10 landed (#134 judgment 3) and it is the same shape of
// work - the run path is synchronous on the UI thread over resident pixels, a
// peer route needs the async measure worker, a pending/failed state, and a
// decision about how the panel SAYS which side ran. Reading-order design,
// parked as 残課題【Fable】 rather than invented here.

// One role's half of the request: what to fold, and where it lives ON THE PEER.
// `member` never crosses - the peer knows a path, not which stack a set
// declared to be the dark, and the binding is the client's fact.
struct SetPeerRole {
    std::string role;
    std::string member;                  // ours, for the tag and the provenance
    std::vector<std::string> paths;      // >1 = one file per frame, in order
    int frame0 = 0, frameCount = 0;
};

// ---- reading the reply -------------------------------------------------------
static bool setPeerNum(const remote::MeasureResult& r, size_t col, const std::string& key,
                       double& out) {
    if (col >= r.cols.size()) return false;
    for (const auto& it : r.cols[col])
        if (it.kind == 0 && it.key == key) { out = it.num; return true; }
    return false;
}
static double setPeerNumOr(const remote::MeasureResult& r, size_t col,
                           const std::string& key, double dflt) {
    double v = dflt;
    setPeerNum(r, col, key, v);
    return v;
}
static std::string setPeerPlaneKey(int p, const char* leaf) {
    char b[48];
    snprintf(b, sizeof b, "p%d.%s", p, leaf);
    return b;
}
static void setPeerShade(const remote::MeasureResult& r, size_t col, int nPl,
                         DetrendReport& S) {
    S.ok = true;
    S.nPl = std::clamp(nPl, 1, 4);
    S.degree = kDetrendProbeDegree;
    for (int p = 0; p < S.nPl; p++) {
        double pp = 0, pct = 0, ok = 0;
        const bool have = setPeerNum(r, col, setPeerPlaneKey(p, "shade_pp"), pp);
        setPeerNum(r, col, setPeerPlaneKey(p, "shade_pct"), pct);
        setPeerNum(r, col, setPeerPlaneKey(p, "shade_pct_ok"), ok);
        S.pl[p].valid = have;
        S.pl[p].ppDn = pp;
        S.pl[p].pctOk = ok != 0.0;
        S.pl[p].pct = S.pl[p].pctOk ? pct : std::numeric_limits<double>::quiet_NaN();
    }
}

// The whole round trip: ask, and read back into the local vessels. Returns "" or
// a one-line refusal, which is the peer's own sentence whenever the peer wrote
// one - a refusal that crossed a link and got reworded here is a refusal about
// a machine this one cannot see.
static std::string setPeerFold(remote::Session& s,
                               const std::vector<SetPeerRole>& roles, int join,
                               int cfaType, int cfaPattern,
                               std::vector<SetRoleFold>& out, SetJoinFold& jout,
                               std::string* computedBy = nullptr) {
    out.assign(roles.size(), SetRoleFold{});
    jout = SetJoinFold{};
    remote::MeasureReq q;
    q.op = rp::MOP_SET_FOLD;
    q.join = join;
    q.cfaType = cfaType;
    q.cfaPattern = cfaPattern;
    // VERBATIM from our own fold, never a guess and never a literal typed here:
    // the point of the check is that the two sides declare the same thing, and
    // a string written twice is a string that can be wrong on one side only.
    q.foldForm = setfold::foldForm((uint32_t)join);
    for (const auto& r : roles) {
        remote::MeasureReq::Role rr;
        rr.role = r.role;
        rr.paths = r.paths;
        rr.frame0 = r.frame0;
        rr.frameCount = r.frameCount;
        q.roles.push_back(std::move(rr));
    }
    remote::MeasureResult mr;
    std::string err;
    if (!s.measure(q, mr, err)) return err;
    if (mr.cols.size() < roles.size())
        return "the peer answered a set fold with " + std::to_string(mr.cols.size()) +
               " column(s) for " + std::to_string(roles.size()) + " role(s)";
    if (mr.provName.empty() || mr.provVersion.empty())
        // Same rule as §10's: an op that exists to make "which build computed
        // this" answerable has failed at the thing it was for if the reply
        // cannot answer it.
        return "the peer answered a set fold without naming what folded it";
    if (computedBy)
        *computedBy = mr.provName + " <" + mr.provVersion + "> on the peer" +
                      (mr.provPath.empty() ? std::string() : " (" + mr.provPath + ")");
    for (size_t i = 0; i < roles.size(); i++) {
        SetRoleFold& F = out[i];
        F.role = roles[i].role;
        F.member = roles[i].member;
        F.headPath = roles[i].paths.empty() ? std::string() : roles[i].paths.front();
        // n/N are the PEER's facts, counted there (the #134 RP13 rule): a client
        // that asserted its own numbers here would be describing a load it did
        // not do.
        F.frames = (int)setPeerNumOr(mr, i, "frames", 0);
        F.expected = (int)setPeerNumOr(mr, i, "expected", 0);
        F.W = (int)setPeerNumOr(mr, i, "w", 0);
        F.H = (int)setPeerNumOr(mr, i, "h", 0);
        F.C = (int)setPeerNumOr(mr, i, "ch", 1);
        F.nPl = (int)setPeerNumOr(mr, i, "planes", 1);
        F.cfa = (int)setPeerNumOr(mr, i, "cfa", 0);
        F.cfaPattern = (int)setPeerNumOr(mr, i, "cfa_pattern", 0);
        F.folded = setPeerNumOr(mr, i, "folded", 0) != 0.0;
        // A peer folds FILES, and a file on disk is raw: the detrend stage makes
        // a first-class stack, and it makes it here (docs/analysis-layers.md §5).
        F.cutoff = kSetCutoffRaw;
        if (!F.folded) continue;
        F.nonFinite = (uint64_t)setPeerNumOr(mr, i, "nonfinite", 0);
        F.dropped = (size_t)setPeerNumOr(mr, i, "dropped", 0);
        for (int p = 0; p < F.nPl && p < 4; p++) {
            F.pl[p].s1 = setPeerNumOr(mr, i, setPeerPlaneKey(p, "s1"), 0);
            F.pl[p].s2 = setPeerNumOr(mr, i, setPeerPlaneKey(p, "s2"), 0);
            F.pl[p].cs = setPeerNumOr(mr, i, setPeerPlaneKey(p, "cs"), 0);
            F.pl[p].n  = setPeerNumOr(mr, i, setPeerPlaneKey(p, "n"),  0);
        }
        setPeerShade(mr, i, F.nPl, F.shade);
    }
    if (join == rp::SJ_DIFF && mr.cols.size() > roles.size()) {
        const size_t jc = roles.size();
        jout.ok = setPeerNumOr(mr, jc, "join_ok", 0) != 0.0;
        jout.nPl = (int)setPeerNumOr(mr, jc, "planes", 1);
        if (jout.ok) {
            for (int p = 0; p < jout.nPl && p < 4; p++) {
                jout.pl[p].s1a = setPeerNumOr(mr, jc, setPeerPlaneKey(p, "s1a"), 0);
                jout.pl[p].s1b = setPeerNumOr(mr, jc, setPeerPlaneKey(p, "s1b"), 0);
                jout.pl[p].dq  = setPeerNumOr(mr, jc, setPeerPlaneKey(p, "dq"),  0);
                jout.pl[p].cs  = setPeerNumOr(mr, jc, setPeerPlaneKey(p, "cs"),  0);
                jout.pl[p].n   = setPeerNumOr(mr, jc, setPeerPlaneKey(p, "n"),   0);
            }
            setPeerShade(mr, jc, jout.nPl, jout.shade);
        }
    }
    return "";
}

// ---- the three rows, composed here out of what the peer folded --------------
// Each one is the local runner with its fold replaced and NOTHING else: the
// composer is the same function, so a difference between the two routes can
// only come from the folds, which is exactly what the equality asserts.

static SetResult setPeerDsnu(remote::Session& s, const std::string& setName,
                             const SetPeerRole& dark, int cfaType, int cfaPattern,
                             std::string& err) {
    std::vector<SetRoleFold> F;
    SetJoinFold J;
    std::string who;
    err = setPeerFold(s, { dark }, rp::SJ_NONE, cfaType, cfaPattern, F, J, &who);
    if (!err.empty()) return SetResult{};
    SetResult R = composeDirectDsnu(setName, F[0]);
    R.computedBy = who;
    return R;
}

static SetResult setPeerPrnu(remote::Session& s, const std::string& setName,
                             const SetPeerRole& image, const SetPeerRole& dark,
                             int cfaType, int cfaPattern, std::string& err) {
    std::vector<SetRoleFold> F;
    SetJoinFold J;
    std::string who;
    // The roles go in SCHEMA ORDER, and the join is role 0 minus role 1 - which
    // is what makes "image - dark" a declaration in the request rather than an
    // inference on the peer.
    err = setPeerFold(s, { image, dark }, rp::SJ_DIFF, cfaType, cfaPattern, F, J, &who);
    if (!err.empty()) return SetResult{};
    SetResult R = composeDirectPrnu(setName, F[0], F[1], J);
    R.computedBy = who;
    return R;
}

// The separation fit: M levels and a dark, ONE request, SJ_NONE. Nothing here
// needs a picture - the fit's whole input is per-level scalars - which is why
// PR #131 was right that "only the fold has to move".
//
// `levels` are the sweep's usable members in the caller's own order, each with
// the series value it stands at; the census, the include switch and the
// UNSET rule stay on the client, where the series lives.
struct SetPeerLevel {
    SetPeerRole role;                    // role name is decorative here: "level<i>"
    double value = 0;                    // the series' own x
};

static std::string setPeerSeparation(remote::Session& s, const std::string& setName,
                                     const std::string& seriesName,
                                     const std::vector<SetPeerLevel>& levels,
                                     const SetPeerRole& dark,
                                     int cfaType, int cfaPattern, SepFit& F) {
    F.setName = setName;
    F.seriesName = seriesName;
    F.members = (int)levels.size();
    for (int i = 0; i < 2; i++) {
        SetResult& R = i ? F.prnu : F.dsnu;
        R.kind = i ? SA_SEP_PRNU : SA_SEP_DSNU;
        R.setName = setName;
        R.form = kSetAnalyzers[R.kind].form;
    }
    std::vector<SetPeerRole> roles;
    for (const auto& L : levels) roles.push_back(L.role);
    roles.push_back(dark);                          // the dark LAST: one scalar per plane
    std::vector<SetRoleFold> RF;
    SetJoinFold J;
    std::string who;
    std::string err = setPeerFold(s, roles, rp::SJ_NONE, cfaType, cfaPattern, RF, J, &who);
    if (!err.empty()) { sepRefuse(F, err); return err; }
    const SetRoleFold& DK = RF.back();
    if (!DK.folded) { sepRefuse(F, setShortRoleRefusal(DK)); return F.refusal; }
    F.nPl = DK.nPl; F.cfa = DK.cfa; F.cfaPattern = DK.cfaPattern;
    double darkMean[4];
    sepDarkMeans(DK.pl, darkMean);
    for (size_t i = 0; i < levels.size(); i++) {
        const SetRoleFold& Fl = RF[i];
        if (!Fl.folded) {
            F.exShort++;
            F.skipped.push_back({ Fl.member,
                "needs >= 2 frames - " + std::to_string(Fl.frames) + " of " +
                std::to_string(Fl.expected) + " loaded" });
            continue;
        }
        // The SAME sentence the local runner produces, by being the same
        // function: a shape refusal that reads differently on the two
        // transports would be a refusal about a machine, not about the data.
        {
            const std::string bad = sepShapeRefusal(
                Fl.member, Fl.W, Fl.H, Fl.C, Fl.nPl, Fl.cfa, Fl.cfaPattern,
                DK.W, DK.H, DK.C, DK.nPl, DK.cfa, DK.cfaPattern);
            if (!bad.empty()) { sepRefuse(F, bad); return F.refusal; }
        }
        SepLevel Lv;
        Lv.member = Fl.member;
        Lv.headPath = Fl.headPath;
        Lv.value = levels[i].value;
        Lv.n = Fl.frames; Lv.N = Fl.expected;
        Lv.nonFinite = Fl.nonFinite; Lv.dropped = Fl.dropped;
        for (int p = 0; p < F.nPl; p++) sepLevelFromAcc(Lv, p, Fl.pl[p], darkMean[p]);
        F.levels.push_back(std::move(Lv));
    }
    sepComposeFit(F, DK);
    F.dsnu.computedBy = who;
    F.prnu.computedBy = who;
    return F.refusal;
}
