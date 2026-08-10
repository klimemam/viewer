// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: remote/解析
// ---- --rset-selftest: set analysis folded on the peer (MOP_SET_FOLD) --------
//
// The op this exercises exists so that a 480-frame dark on a compute node is
// folded THERE. Everything else here serves one claim: that routing a set
// analysis through a peer returns the SAME NUMBER as running it here, on the
// same data. A remote route that answers differently is the defect this must
// not introduce (docs/analysis-layers.md §3.5, and PR #127's P1..P5, which is
// where this repo learned that two writings of one estimator drift).
//
// So the comparison is `==` rather than a tolerance, and it is made on every
// field a row carries rather than on the headline: two sides that both clamp
// agree on 0 while agreeing on nothing, which is why the correction amount, the
// clamp, mu, the uncorrected bound, the sample count and the shading figure are
// each compared, and the provenance tag as a SENTENCE.
//
// The subject is a REAL peer process - viewer-serve over a pipe, started by
// remote::Session exactly as --remote-selftest and --rplugin-selftest start one
// - reading the fixture off its own disk with nothing opened on its side.
//
// The fixture is WRITTEN BY THIS TEST, into a scratch subdirectory it creates
// (the --stackavg / --derive precedent, including their rule that a test may
// only destroy what it made). It is analytic and its physics is chosen for one
// reason: an equality of zeros proves nothing. Run against a fixture with no
// injected fixed pattern every row clamps, and "local 0 == peer 0" would be
// green while the two sides agreed on nothing at all - which is exactly the
// trap #57 judgment 2 named and rtemporal's P3/P4 were added for. So the
// fixture resolves a real additive pattern AND a real multiplicative one on all
// four CFA planes, and RS0 asserts that it does before any equality is claimed.
// One stack ("quiet") is then deliberately degenerate, so the clamp path is
// compared too - on the amount subtracted, which is where a clamped pair can
// still disagree.
//
// Read as RGGB, so the plane map is under test: it is one of exactly two things
// the local fold and the peer's must agree on sample for sample (the other is
// the frame order), and they are written in different files with different
// tables (state.h's CFA_MAP and serve.cpp's CFA_MAP_S).
//
// VIEWER_SERVE_PROTOCOL=7 makes a second peer BE a peer that predates this op:
// it announces 7 in HELLO and answers the op with "unknown measure op", which
// is byte for byte what the build before this one does. The client then refuses
// from the number, before it sends, and says which mismatch it is.

// == on purpose. NaN == NaN counts as agreement: the ratio row is NaN on a
// plane with no level to be a ratio of, and "both sides declined for the same
// reason" is agreement rather than a failure to compare.
static bool rsetEq(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
}

static bool rsetSameResult(const SetResult& A, const SetResult& B, std::string& why) {
    auto fail = [&](const char* what, double a, double b) {
        char buf[224];
        snprintf(buf, sizeof buf, "%s: local %.17g vs peer %.17g", what, a, b);
        why = buf;
        return false;
    };
    if (A.ok != B.ok || A.refusal != B.refusal) {
        why = "one side refused: \"" + A.refusal + "\" / \"" + B.refusal + "\"";
        return false;
    }
    if (A.nPl != B.nPl) return fail("planes", A.nPl, B.nPl);
    for (int p = 0; p < A.nPl; p++) {
        if (!rsetEq(A.value[p], B.value[p]))   return fail("value", A.value[p], B.value[p]);
        if (!rsetEq(A.corrDn[p], B.corrDn[p])) return fail("fpn_corr", A.corrDn[p], B.corrDn[p]);
        if (!rsetEq(A.uncorr[p], B.uncorr[p])) return fail("uncorrected", A.uncorr[p], B.uncorr[p]);
        if (!rsetEq(A.mu[p], B.mu[p]))         return fail("mu", A.mu[p], B.mu[p]);
        if (!rsetEq(A.sigmaD[p], B.sigmaD[p])) return fail("sigma_d", A.sigmaD[p], B.sigmaD[p]);
        if (A.clamped[p] != B.clamped[p])      return fail("clamped", A.clamped[p], B.clamped[p]);
        if (A.samples[p] != B.samples[p])
            return fail("samples", (double)A.samples[p], (double)B.samples[p]);
        if (!rsetEq(A.shadePp[p], B.shadePp[p]))
            return fail("shade p-p", A.shadePp[p], B.shadePp[p]);
        if (A.shadePctOk[p] != B.shadePctOk[p])
            return fail("shade ratio defined", A.shadePctOk[p], B.shadePctOk[p]);
        if (A.shadePctOk[p] && !rsetEq(A.shadePct[p], B.shadePct[p]))
            return fail("shade %", A.shadePct[p], B.shadePct[p]);
    }
    if (setResultTitle(A) != setResultTitle(B)) {
        why = "tag: \"" + setResultTitle(A) + "\" vs \"" + setResultTitle(B) + "\"";
        return false;
    }
    if (A.form != B.form)     { why = "the estimator form differs"; return false; }
    if (A.cutoff != B.cutoff) { why = "cutoff \"" + A.cutoff + "\" vs \"" + B.cutoff + "\""; return false; }
    return true;
}

// (the body moved to core/app/util.inc when --remote-selftest needed the same
// seam for the protocol-9 gate; one #ifdef, not two)
static void rsetSetEnv(const char* name, const char* value) {
    selftestSetEnv(name, value);
}

static int rsetSelfTest(const char* exe, const char* dirArg) {
    int bad = 0;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "rsetselftest: %-74s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) bad++;
    };
    std::string base = dirArg;
    std::replace(base.begin(), base.end(), '\\', '/');
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    // A subdirectory this test CREATES, never the directory it was handed:
    // --derive-selftest's first version did remove_all on its argument and
    // deleted the shared fixture tree the first time the suite pointed it at
    // one. A test may only destroy what it made.
    const std::string root = base + "/rset-scratch";
    {
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(root), ec);
    }

    // ---- the fixture, written here so the PEER can read it off a disk -------
    //
    //   plane p = (y&1)*2 + (x&1)                       RGGB, so 16x16 per plane
    //   u = x>>1, v = y>>1                              the coordinates INSIDE a plane
    //   pixel = L0
    //         + (v&1 ? -B : +B)                         the additive fixed pattern
    //         + level*sens[p] * (1 + (u&1 ? -qg : +qg)) the multiplicative one
    //         + ((u^v)&1 ? -a : +a)                     the temporal mean's residual
    //         + TD[f]*kt                                the temporal spread
    //
    // The three +- patterns are Walsh-orthogonal over (u,v), so within a plane
    // the population spatial variance of the temporal mean is exactly the SUM
    // B^2 + (level*sens*qg)^2 + a^2, with no cross terms. TD = {-9,-3,0,3,9}
    // over 5 frames gives s_t^2 = 45kt^2 and C = 9kt^2 exactly. Every value is
    // dyadic, so the file holds exactly what this arithmetic says it does.
    const int FW = 32, FH = 32, NFR = 5;
    const double L0 = 1024.0, qg = 1.0 / 32.0, kt = 4.0;
    const double SENS[4] = { 1.0, 1.25, 1.25, 0.75 };
    static const double TD[5] = { -9, -3, 0, 3, 9 };
    // ...plus a deterministic sub-DN offset, different in every stack. It is
    // there for one reason and it is worth stating: WITHOUT it every value in
    // this fixture is a dyadic rational, every sum is exact in binary64, and
    // two algebraically equal routes to the same number agree to the last bit
    // whichever one is taken. Real sensor data is not dyadic, and an equality
    // that only holds on exact inputs is not the equality this test claims.
    // 97 is coprime with everything in the patterns above, so it disturbs none
    // of them; k/97 is not representable in binary, which is the whole point.
    auto grit = [](int x, int y, int seed) {
        return (double)((x * 29 + y * 13 + seed * 7) % 97) / 97.0;
    };
    int seed = 0;
    // W/H are parameters only so that ONE stack can be written at half the
    // linear size: the WxH refusal (確定 2026-08-10, #57) has to be exercised
    // over the WIRE and not only in memory, because the peer is where W and H
    // stop being the client's own facts and become numbers it was told.
    auto writeStack = [&](const char* name, double level, double B, double a,
                          int W, int H) {
        const std::string d = root + "/" + name;
        const int sd = ++seed;
        std::error_code ec;
        std::filesystem::create_directories(pathFromUtf8(d), ec);
        for (int f = 0; f < NFR; f++) {
            std::vector<float> px((size_t)W * H);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    const int p = (y & 1) * 2 + (x & 1);
                    const int u = x >> 1, v = y >> 1;
                    const double val =
                        L0 + ((v & 1) ? -B : B) +
                        level * SENS[p] * (1.0 + ((u & 1) ? -qg : qg)) +
                        (((u ^ v) & 1) ? -a : a) + TD[f] * kt + grit(x, y, sd);
                    px[(size_t)y * W + x] = (float)val;
                }
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }", H, W);
            std::string hdr = dict;
            hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
            hdr += '\n';
            char nm[64];
            snprintf(nm, sizeof nm, "/frame_%03d.npy", f);
            std::ofstream out(pathFromUtf8(d + nm), std::ios::binary);
            const uint16_t hl = (uint16_t)hdr.size();
            out.write("\x93NUMPY\x01\x00", 8);
            out.write((const char*)&hl, 2);
            out.write(hdr.data(), (std::streamsize)hdr.size());
            out.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        }
    };
    const double Bd = 8.0, Bl = 20.0, resid = 3.0 * kt;
    writeStack("dark", 0.0, Bd, resid, FW, FH);          // the dark: additive pattern only
    writeStack("lv0", 512.0, Bl, resid, FW, FH);         // four lit levels, one line in mu^2
    writeStack("lv1", 1024.0, Bl, resid, FW, FH);
    writeStack("lv2", 2048.0, Bl, resid, FW, FH);
    writeStack("lv3", 3072.0, Bl, resid, FW, FH);
    // ...and one stack whose fixed pattern is UNDER its own temporal floor, so
    // the clamp fires and the two sides have to agree about a 0 that is a
    // statement rather than a value.
    writeStack("quiet", 0.0, 0.0, 1.0, FW, FH);
    // ...and one at HALF the linear size, same channel count and same plane map,
    // for RS23. It is a perfectly good stack; what it is not is the sweep's own
    // shape (確定 2026-08-10, #57).
    writeStack("half", 2560.0, Bl, resid, FW / 2, FH / 2);

    // ---- opened HERE: one stack per folder ----------------------------------
    app.seqLoadMode = 1;                   // numbered files ARE a stack
    openFolder(root);
    {
        double t0 = nowSec();
        while (nowSec() - t0 < 300.0) {
            if (app.folderPickOpen && !app.folderPickRemote) {
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen && app.seqs.size() >= 7) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    struct Level {
        int seqId = 0;
        double value = 0;
        std::string name;                  // the folder it came out of
        std::vector<std::string> paths;    // in the order the local fold reads them
    };
    std::vector<Level> stacks;
    int cfaType = 0, cfaPattern = 0;
    for (const auto& sq : app.seqs) {
        std::vector<int> fr = framesOfSeq(sq.id);
        if (fr.empty()) continue;
        Level L;
        L.seqId = sq.id;
        for (int idx : fr) L.paths.push_back(app.images[idx]->src->path);
        std::string p = L.paths.front();
        std::replace(p.begin(), p.end(), '\\', '/');
        const size_t slash = p.find_last_of('/');
        const size_t slash2 = slash == std::string::npos ? std::string::npos
                                                         : p.find_last_of('/', slash - 1);
        L.name = (slash2 == std::string::npos) ? sq.name : p.substr(slash2 + 1, slash - slash2 - 1);
        cfaType = app.images[fr.front()]->cfa;
        cfaPattern = app.images[fr.front()]->cfaPattern;
        stacks.push_back(std::move(L));
    }
    fprintf(stderr, "rsetselftest: fixture %s: %d stack(s), cfa=%d/%d, %d frame(s) open\n",
            root.c_str(), (int)stacks.size(), cfaType, cfaPattern, (int)app.images.size());
    auto byName = [&](const char* n) -> Level* {
        for (Level& L : stacks) if (L.name == n) return &L;
        return nullptr;
    };
    const char* kLevelDirs[4] = { "lv0", "lv1", "lv2", "lv3" };
    const double kLevelVal[4] = { 512.0, 1024.0, 2048.0, 3072.0 };
    Level* darkP = byName("dark");
    Level* litP = byName("lv3");
    Level* quietP = byName("quiet");
    Level* halfP = byName("half");
    std::vector<Level*> levels;
    for (int i = 0; i < 4; i++) {
        Level* L = byName(kLevelDirs[i]);
        if (L) { L->value = kLevelVal[i]; levels.push_back(L); }
    }
    if (!darkP || !litP || !quietP || !halfP || levels.size() != 4) {
        fprintf(stderr, "rsetselftest: the scratch fixture did not open as 7 stacks\n");
        return 1;
    }
    const Level& dark = *darkP;
    const Level& lit = *litP;

    auto makeSet = [&](const char* name,
                       const std::vector<std::pair<std::string, int>>& roles) {
        App::ASet S;
        S.id = app.nextASetId++;
        S.batchId = 1;
        S.name = name;
        for (const auto& kv : roles) {
            App::ASetRole r;
            r.role = kv.first;
            r.seqId = kv.second;
            S.roles.push_back(r);
        }
        app.analysisSets.push_back(S);
        return S.id;
    };
    auto peerRole = [](const char* role, const Level& L) {
        SetPeerRole r;
        r.role = role;
        r.member = L.name;
        r.paths = L.paths;
        return r;
    };

    // ---- the peer -----------------------------------------------------------
    remote::Session s;
    std::string err;
    if (!s.start("", exe, err)) {
        fprintf(stderr, "rsetselftest: peer: %s\n", err.c_str());
        return 1;
    }
    check(s.peerVersion() >= 8, "RS1 the peer speaks protocol 8 - a set fold is askable");

    // ---- RS2/RS3: the additive row, folded there vs folded here -------------
    {
        const int sid = makeSet(dark.name.c_str(), { { "dark", dark.seqId } });
        SetResult L = runSetAnalyzer(*aSetById(sid), SA_DSNU);
        std::string e2;
        SetResult P = setPeerDsnu(s, dark.name, peerRole("dark", dark),
                                  cfaType, cfaPattern, e2);
        if (!e2.empty()) fprintf(stderr, "rsetselftest: RS2: %s\n", e2.c_str());
        fprintf(stderr, "rsetselftest: %s\nrsetselftest: %s\n",
                setResultTitle(L).c_str(), setResultTitle(P).c_str());
        for (int p = 0; p < L.nPl; p++)
            fprintf(stderr, "rsetselftest:   plane %d: local %.17g  peer %.17g  "
                            "(corr %.17g / %.17g, shading p-p %.17g / %.17g)\n",
                    p, L.value[p], P.value[p], L.corrDn[p], P.corrDn[p],
                    L.shadePp[p], P.shadePp[p]);
        // BEFORE any equality is claimed: the fixture has to be able to tell
        // the two sides apart. Every plane must resolve a real fixed pattern
        // above its own temporal floor, or "local == peer" is an agreement
        // about zero.
        bool resolves = L.ok && L.nPl == 4;
        for (int p = 0; p < L.nPl; p++)
            resolves = resolves && L.value[p] > 1.0 && !L.clamped[p] && L.corrDn[p] > 0;
        check(resolves,
              "RS0 the fixture resolves a fixed pattern on all 4 planes, unclamped");
        check(e2.empty() && L.ok && P.ok,
              "RS2 the dark folds on the peer and the row composes here");
        std::string why;
        const bool same = rsetSameResult(L, P, why);
        if (!same) fprintf(stderr, "rsetselftest: RS3 %s\n", why.c_str());
        check(same, "RS3 ...and every field of it equals the local run, bit for bit");
        // The peer folded 24 frames and the client never saw one of them as
        // pixels: n/N are the PEER's count (the #134 RP13 rule).
        check(P.ok && P.used.size() == 1 && P.used[0].n == (int)dark.paths.size() &&
                  P.used[0].role == "dark" && P.used[0].member == dark.name,
              "RS4 n/N are the peer's own count, under the client's role binding");
        // #46's 計算者欄 for a built-in: the peer's viewer version, and no dll.
        fprintf(stderr, "rsetselftest: computedBy: %s\n", P.computedBy.c_str());
        check(P.computedBy.find("set fold") != std::string::npos &&
                  P.computedBy.find("on the peer") != std::string::npos &&
                  L.computedBy.empty(),
              "RS5 the peer row names what folded it; the local row names nothing");
    }

    // ---- RS5b: the clamp, which is where two zeros can still disagree -------
    // A stack whose fixed pattern sits under its own temporal floor prints 0 on
    // both sides. That 0 is a statement about the measurement, not a value, and
    // what makes the two sides genuinely agree is the AMOUNT subtracted and the
    // declaration that it was a clamp - #57 judgment 2's rule, asked across a
    // link.
    {
        const int sid = makeSet("quiet", { { "dark", quietP->seqId } });
        SetResult L = runSetAnalyzer(*aSetById(sid), SA_DSNU);
        std::string e2;
        SetResult P = setPeerDsnu(s, "quiet", peerRole("dark", *quietP),
                                  cfaType, cfaPattern, e2);
        fprintf(stderr, "rsetselftest: clamped: value %.17g / %.17g  corr %.17g / %.17g  "
                        "clamped %d / %d  uncorrected %.17g / %.17g\n",
                L.value[0], P.value[0], L.corrDn[0], P.corrDn[0],
                (int)L.clamped[0], (int)P.clamped[0], L.uncorr[0], P.uncorr[0]);
        bool allClamped = L.ok && P.ok;
        for (int p = 0; p < L.nPl; p++)
            allClamped = allClamped && L.clamped[p] && L.value[p] == 0.0 && L.corrDn[p] > 1.0;
        check(allClamped, "RS5b the quiet stack really clamps, and declares it");
        std::string why;
        const bool same = rsetSameResult(L, P, why);
        if (!same) fprintf(stderr, "rsetselftest: RS5c %s\n", why.c_str());
        check(same, "RS5c ...and the two sides agree on the amount, not merely on the 0");
    }

    // ---- RS6/RS7: the ratio row - the one that needs a difference image -----
    {
        const std::string nm = lit.name + " vs " + dark.name;
        const int sid = makeSet(nm.c_str(),
                                { { "image", lit.seqId }, { "dark", dark.seqId } });
        SetResult L = runSetAnalyzer(*aSetById(sid), SA_PRNU);
        std::string e2;
        SetResult P = setPeerPrnu(s, nm, peerRole("image", lit), peerRole("dark", dark),
                                  cfaType, cfaPattern, e2);
        if (!e2.empty()) fprintf(stderr, "rsetselftest: RS6: %s\n", e2.c_str());
        fprintf(stderr, "rsetselftest: %s\nrsetselftest: %s\n",
                setResultTitle(L).c_str(), setResultTitle(P).c_str());
        for (int p = 0; p < L.nPl; p++)
            fprintf(stderr, "rsetselftest:   plane %d: local %.17g%%  peer %.17g%%  "
                            "(mu %.17g / %.17g, sigma_d %.17g / %.17g)\n",
                    p, L.value[p], P.value[p], L.mu[p], P.mu[p], L.sigmaD[p], P.sigmaD[p]);
        bool resolves = L.ok && L.nPl == 4;
        for (int p = 0; p < L.nPl; p++)
            resolves = resolves && L.value[p] > 1.0 && !L.clamped[p] && L.mu[p] > 100.0;
        check(resolves,
              "RS6a the pair resolves a multiplicative pattern on all 4 planes too");
        check(e2.empty() && L.ok && P.ok,
              "RS6 the lit/dark pair folds on the peer, difference and all");
        std::string why;
        const bool same = rsetSameResult(L, P, why);
        if (!same) fprintf(stderr, "rsetselftest: RS7 %s\n", why.c_str());
        // THE assertion this op exists for. sum(D^2) is not composable from the
        // two roles' own sums without changing the digits, so this equality is
        // what says the difference image was formed where the pictures were.
        check(same, "RS7 ...and the per-pixel difference row equals the local one exactly");
    }

    // ---- RS8..RS11: the separation fit - M+1 stacks, one request ------------
    {
        struct SM { int seqId; uint64_t uid; double value; bool include; int fold; };
        std::vector<SM> ms;
        std::vector<SetPeerLevel> pl;
        for (size_t i = 0; i < levels.size(); i++) {
            ms.push_back({ levels[i]->seqId, 0, levels[i]->value, true, FOLD_MEAN });
            SetPeerLevel L;
            L.role = peerRole("level", *levels[i]);
            L.role.role = "level" + std::to_string(i);    // distinct, per role
            L.value = levels[i]->value;
            pl.push_back(std::move(L));
        }
        App::Series Sr;
        Sr.id = app.nextSeriesId++;
        Sr.batchId = 1;
        Sr.name = "lx sweep";
        Sr.paramName = "illuminance";
        snprintf(Sr.unit, sizeof Sr.unit, "lx");
        for (const SM& m : ms) {
            App::Series::Member mm;
            mm.seqId = m.seqId; mm.frameUid = m.uid; mm.value = m.value;
            mm.include = m.include; mm.fold = m.fold;
            Sr.members.push_back(mm);
        }
        app.series.push_back(std::move(Sr));
        const int serId = app.series.back().id;
        App::ASet Ss;
        Ss.id = app.nextASetId++;
        Ss.batchId = 1;
        Ss.name = "sep";
        App::ASetRole rs; rs.role = "sweep"; rs.seriesId = serId; Ss.roles.push_back(rs);
        App::ASetRole rd; rd.role = "dark";  rd.seqId = dark.seqId; Ss.roles.push_back(rd);
        app.analysisSets.push_back(Ss);

        SepFit LF = runSeparationFit(*aSetById(Ss.id));
        SepFit PF;
        const std::string e3 = setPeerSeparation(s, "sep", "lx sweep", pl,
                                                 peerRole("dark", dark),
                                                 cfaType, cfaPattern, PF);
        if (!e3.empty()) fprintf(stderr, "rsetselftest: RS8: %s\n", e3.c_str());
        fprintf(stderr, "rsetselftest: fit M=%d/%d  intercept %.17g / %.17g  "
                        "slope %.17g / %.17g  R^2 %.17g / %.17g\n",
                LF.M, PF.M, LF.ok ? LF.inter[0] : 0.0, PF.ok ? PF.inter[0] : 0.0,
                LF.ok ? LF.slope[0] : 0.0, PF.ok ? PF.slope[0] : 0.0,
                LF.ok ? LF.r2[0] : 0.0, PF.ok ? PF.r2[0] : 0.0);
        // ...and again: a fit whose two coefficients both clamped would make
        // the equality below an agreement about two zeros.
        bool resolves = LF.ok && LF.nPl == 4;
        for (int p = 0; p < LF.nPl; p++)
            resolves = resolves && LF.inter[p] > 0 && LF.slope[p] > 0 &&
                       !LF.dsnu.clamped[p] && !LF.prnu.clamped[p] && LF.r2[p] > 0.999;
        check(resolves,
              "RS8a the sweep separates both terms on all 4 planes, nothing clamped");
        check(LF.ok && PF.ok && LF.M == PF.M && LF.M == (int)pl.size(),
              "RS8 the sweep's M+1 stacks fold on the peer in ONE request");
        bool coef = LF.ok && PF.ok && LF.nPl == PF.nPl;
        for (int p = 0; coef && p < LF.nPl; p++)
            coef = rsetEq(LF.inter[p], PF.inter[p]) && rsetEq(LF.slope[p], PF.slope[p]) &&
                   rsetEq(LF.r2[p], PF.r2[p]) && rsetEq(LF.rms[p], PF.rms[p]) &&
                   rsetEq(LF.interRaw[p], PF.interRaw[p]) &&
                   rsetEq(LF.slopeRaw[p], PF.slopeRaw[p]);
        check(coef, "RS9 ...and the OLS coefficients are identical, bit for bit");
        bool lvOk = LF.ok && PF.ok && LF.levels.size() == PF.levels.size();
        for (size_t i = 0; lvOk && i < LF.levels.size(); i++)
            for (int p = 0; p < LF.nPl; p++)
                lvOk = lvOk && rsetEq(LF.levels[i].mu[p], PF.levels[i].mu[p]) &&
                       rsetEq(LF.levels[i].fpnVar[p], PF.levels[i].fpnVar[p]) &&
                       rsetEq(LF.levels[i].resid[p], PF.levels[i].resid[p]) &&
                       rsetEq(LF.levels[i].corr[p], PF.levels[i].corr[p]) &&
                       LF.levels[i].n == PF.levels[i].n;
        check(lvOk, "RS10 ...and so is every level's mu, sigma_fpn^2 and residual");
        std::string w1, w2;
        check(rsetSameResult(LF.dsnu, PF.dsnu, w1) && rsetSameResult(LF.prnu, PF.prnu, w2),
              "RS11 ...so both named rows of the fit read the same on either side");
        if (!w1.empty()) fprintf(stderr, "rsetselftest: RS11 %s\n", w1.c_str());
        if (!w2.empty()) fprintf(stderr, "rsetselftest: RS11 %s\n", w2.c_str());
    }

    // ---- RS23 / RS24: the two rulings of 2026-08-10, over the wire ----------
    //
    // Both are CLIENT-side composition, so neither changes a byte of the
    // request or the fold's declared form - the parity token is unmoved and
    // rp::VERSION stays 8. What they do change is a sentence and a tag, and a
    // sentence that reads differently on the two transports is a statement
    // about a machine rather than about data. So the local run and the peer run
    // are compared, not merely exercised.
    //
    // `member` is the CLIENT's own word for a binding on both transports (it
    // never crosses the wire - the peer knows a path). Both sentences under
    // test quote it, so these two blocks hand the peer path the same name the
    // local path reads off its own SeqInfo; anything else would be comparing
    // two descriptions of one binding written by two different authors.
    auto peerRoleBound = [&](const char* role, const Level& L) {
        SetPeerRole r = peerRole(role, L);
        if (const App::SeqInfo* si = seqInfo(L.seqId)) r.member = si->name;
        return r;
    };
    struct SepMember { const Level* L; double value; };
    auto makeSweepSet = [&](const char* setName, const char* serName,
                            const std::vector<SepMember>& mem, const Level& dk) {
        App::Series Sr;
        Sr.id = app.nextSeriesId++;
        Sr.batchId = 1;
        Sr.name = serName;
        Sr.paramName = "illuminance";
        snprintf(Sr.unit, sizeof Sr.unit, "lx");
        for (const auto& m : mem) {
            App::Series::Member mm;
            mm.seqId = m.L->seqId;
            mm.value = m.value;
            mm.include = true;
            mm.fold = FOLD_MEAN;
            Sr.members.push_back(mm);
        }
        app.series.push_back(std::move(Sr));
        App::ASet Ss;
        Ss.id = app.nextASetId++;
        Ss.batchId = 1;
        Ss.name = setName;
        App::ASetRole rs; rs.role = "sweep"; rs.seriesId = app.series.back().id;
        Ss.roles.push_back(rs);
        App::ASetRole rd; rd.role = "dark"; rd.seqId = dk.seqId;
        Ss.roles.push_back(rd);
        app.analysisSets.push_back(Ss);
        return Ss.id;
    };
    auto peerLevels = [&](const std::vector<SepMember>& mem) {
        std::vector<SetPeerLevel> pl;
        for (size_t i = 0; i < mem.size(); i++) {
            SetPeerLevel L;
            L.role = peerRoleBound("level", *mem[i].L);
            L.role.role = "level" + std::to_string(i);     // distinct, per role
            L.value = mem[i].value;
            pl.push_back(std::move(L));
        }
        return pl;
    };

    {   // ---- RS23: a sweep member that is not the dark's shape --------------
        // 確定 2026-08-10 (#57): 「dark, sweep の WxH 不一致は拒否」. The member
        // is a perfectly good 16x16 stack with the same channel count and the
        // same plane map, and the fit's arithmetic would have run over it - the
        // dark contributes a per-plane scalar and nothing subtracts pixel from
        // pixel. On this transport W and H are not even the client's own facts:
        // they are numbers the PEER counted and sent back, which is the half of
        // this refusal that could quietly not exist.
        std::vector<SepMember> mem;
        for (size_t i = 0; i < levels.size(); i++)
            mem.push_back({ levels[i], levels[i]->value });
        mem.push_back({ halfP, 2560.0 });
        const int sid = makeSweepSet("sep-size", "lx sweep", mem, dark);
        SepFit LF = runSeparationFit(*aSetById(sid));
        SepFit PF;
        const std::string e = setPeerSeparation(s, "sep-size", "lx sweep", peerLevels(mem),
                                                peerRoleBound("dark", dark),
                                                cfaType, cfaPattern, PF);
        fprintf(stderr, "rsetselftest: RS23 local \"%s\"\n", LF.refusal.c_str());
        fprintf(stderr, "rsetselftest: RS23 peer  \"%s\"\n", PF.refusal.c_str());
        check(!LF.ok && LF.refusal.find("16x16x1ch") != std::string::npos &&
                  LF.refusal.find("32x32x1ch") != std::string::npos &&
                  LF.refusal.find("one shape") != std::string::npos,
              "RS23 a WxH mismatch is refused locally, with BOTH shapes named");
        check(!PF.ok && !PF.refusal.empty() && PF.refusal == LF.refusal &&
                  e == LF.refusal,
              "RS23 ...and the peer transport refuses in the very same sentence");
        check(!LF.dsnu.ok && !PF.dsnu.ok && !LF.prnu.ok && !PF.prnu.ok &&
                  LF.dsnu.refusal == PF.dsnu.refusal,
              "RS23 ...with no row composed on either side");
    }

    {   // ---- RS24: the dark ALSO standing as a level ------------------------
        // 確定 2026-08-10 (#57): 「dark が sweep のメンバを兼ねるは、そう
        // Analysis Set を作ったら許容」. The peer folds the same files twice,
        // once under each role, exactly as the local path folds the same stack
        // twice - so the anchor's mu is a number minus itself on both sides and
        // the equality below is ==, not a tolerance.
        std::vector<SepMember> mem;
        mem.push_back({ darkP, 0.0 });          // the dark, standing as this sweep's zero
        for (size_t i = 0; i < levels.size(); i++)
            mem.push_back({ levels[i], levels[i]->value });
        const int sid = makeSweepSet("sep-anchored", "lx sweep", mem, dark);
        SepFit LF = runSeparationFit(*aSetById(sid));
        SepFit PF;
        const std::string e = setPeerSeparation(s, "sep-anchored", "lx sweep",
                                                peerLevels(mem), peerRoleBound("dark", dark),
                                                cfaType, cfaPattern, PF);
        if (!e.empty()) fprintf(stderr, "rsetselftest: RS24: %s\n", e.c_str());
        fprintf(stderr, "rsetselftest: RS24 %s\n", setResultTitle(LF.dsnu).c_str());
        fprintf(stderr, "rsetselftest: RS24 %s\n", setResultTitle(PF.dsnu).c_str());
        fprintf(stderr, "rsetselftest: RS24 anchor mu %.17g / %.17g  y %.17g / %.17g  "
                        "intercept %.17g / %.17g\n",
                LF.ok ? LF.levels[0].mu[0] : -1.0, PF.ok ? PF.levels[0].mu[0] : -1.0,
                LF.ok ? LF.levels[0].fpnVar[0] : -1.0, PF.ok ? PF.levels[0].fpnVar[0] : -1.0,
                LF.ok ? LF.inter[0] : 0.0, PF.ok ? PF.inter[0] : 0.0);
        check(LF.ok && PF.ok && LF.M == 5 && PF.M == 5,
              "RS24 a dark that is also a level folds on the peer and fits, M+1 roles");
        bool zero = LF.ok && PF.ok;
        for (int p = 0; p < LF.nPl && zero; p++)
            zero = LF.levels[0].mu[p] == 0.0 && PF.levels[0].mu[p] == 0.0;
        check(zero, "RS24 ...its mu is EXACTLY 0 on both sides, on every plane");
        check(LF.alsoDark.size() == 1 && LF.alsoDark == PF.alsoDark &&
                  !LF.dualNote.empty() && LF.dualNote == PF.dualNote,
              "RS24 ...and both transports name the double duty in one sentence");
        std::string w3, w4;
        const bool same = rsetSameResult(LF.dsnu, PF.dsnu, w3) &&
                          rsetSameResult(LF.prnu, PF.prnu, w4);
        if (!w3.empty()) fprintf(stderr, "rsetselftest: RS24 %s\n", w3.c_str());
        if (!w4.empty()) fprintf(stderr, "rsetselftest: RS24 %s\n", w4.c_str());
        check(same, "RS24 ...so both rows, overlap clause and all, read the same");
        check(setResultTitle(LF.dsnu).find("is also the dark role") != std::string::npos,
              "RS24 ...and a number quoted out of either one carries the fact");
    }

    // ---- RS12: the peer computes nothing that has a name --------------------
    // The famous-name rule (PR #130's E1) says the bare names live in ONE file.
    // That is a scan of the sources; this is the same rule asked of the WIRE,
    // which is where it actually has to hold: the peer answers with sums, and
    // every named quantity is composed here.
    {
        remote::MeasureReq q;
        q.op = rp::MOP_SET_FOLD;
        q.join = rp::SJ_NONE;
        q.cfaType = cfaType;
        q.cfaPattern = cfaPattern;
        q.foldForm = setfold::foldForm(rp::SJ_NONE);
        remote::MeasureReq::Role rr;
        rr.role = "dark";
        rr.paths = dark.paths;
        q.roles.push_back(rr);
        remote::MeasureResult mr;
        std::string e4;
        const bool ok4 = s.measure(q, mr, e4);
        bool named = false;
        int items = 0;
        for (const auto& col : mr.cols)
            for (const auto& it : col) {
                items++;
                const std::string blob = it.key + " " + it.text;
                if (blob.find("DSNU") != std::string::npos ||
                    blob.find("PRNU") != std::string::npos) {
                    fprintf(stderr, "rsetselftest: RS12 the wire names it: \"%s\" = \"%s\"\n",
                            it.key.c_str(), it.text.c_str());
                    named = true;
                }
            }
        fprintf(stderr, "rsetselftest: RS12 %d item(s) came back, prov \"%s\" <%s>\n",
                items, mr.provName.c_str(), mr.provVersion.c_str());
        check(ok4 && items > 0 && !named,
              "RS12 the reply is sums and facts - it names no measurement");
        check(ok4 && mr.provName == "set fold (built-in)" && !mr.provVersion.empty() &&
                  mr.provFile.empty(),
              "RS13 ...and the trailer is a built-in's: a viewer version, no dll");
    }

    // ---- RS14..RS17: the refusals -------------------------------------------
    {   // A different fold. Nothing else about the request changes - this IS
        // what one machine being a release behind looks like on the wire, for a
        // built-in that has no descriptor version to differ in.
        remote::MeasureReq q;
        q.op = rp::MOP_SET_FOLD;
        q.join = rp::SJ_NONE;
        q.cfaType = cfaType;
        q.cfaPattern = cfaPattern;
        q.foldForm = setfold::foldForm(rp::SJ_NONE) + "; ddof = n_i (a different fold)";
        remote::MeasureReq::Role rr;
        rr.role = "dark";
        rr.paths = dark.paths;
        q.roles.push_back(rr);
        remote::MeasureResult mr;
        std::string e5;
        const bool ok5 = s.measure(q, mr, e5);
        fprintf(stderr, "rsetselftest: refusal(parity):\n%s\n", e5.c_str());
        check(!ok5 && e5.find("set fold parity refused") != std::string::npos &&
                  e5.find("a different fold") != std::string::npos &&
                  e5.find(setfold::foldForm(rp::SJ_NONE)) != std::string::npos,
              "RS14 a fold-form MISMATCH is refused, with BOTH forms on the line");
        check(!ok5 && mr.cols.empty() && mr.provName.empty(),
              "RS15 ...and NOTHING was folded - no sums, no quiet local fallback");
        // PR #135's lesson, asked of this refusal: no BUILD-DERIVED string may
        // appear in it. A parity check on one makes two peers that agree about
        // every line refuse each other, and even quoting a version beside a
        // form mismatch tells the reader the build is what differed. The peer
        // version is provenance and rides in the reply, where RS13 finds it.
        check(!ok5 && e5.find(viewerVersion()) == std::string::npos,
              "RS15b ...and quotes no build string - a version is never matched");
        // ...and an undeclared form, which two absences must not make a match.
        q.foldForm.clear();
        remote::MeasureResult m6;
        std::string e6;
        const bool ok6 = s.measure(q, m6, e6);
        fprintf(stderr, "rsetselftest: refusal(undeclared):\n%s\n", e6.c_str());
        check(!ok6 && e6.find("declares nothing on the client") != std::string::npos &&
                  e6.find("was never declared cannot be matched") != std::string::npos,
              "RS16 an UNDECLARED fold form is refused, not read as a match");
    }
    {   // A set analyzer reads the frame, so a request with a rectangle in it is
        // asking a different question under the same name.
        remote::MeasureReq q;
        q.op = rp::MOP_SET_FOLD;
        q.join = rp::SJ_NONE;
        q.foldForm = setfold::foldForm(rp::SJ_NONE);
        q.cfaType = cfaType;
        q.cfaPattern = cfaPattern;
        q.rois.push_back({ 0, 0, 16, 16 });
        remote::MeasureReq::Role rr;
        rr.role = "dark";
        rr.paths = dark.paths;
        q.roles.push_back(rr);
        remote::MeasureResult mr;
        std::string e7;
        const bool ok7 = s.measure(q, mr, e7);
        fprintf(stderr, "rsetselftest: refusal(roi): %s\n", e7.c_str());
        check(!ok7 && e7.find("whole frame") != std::string::npos,
              "RS17 an ROI is refused - a set analyzer does not measure a rectangle");
    }
    {   // The join is between TWO roles and says so, rather than the peer
        // guessing which two of three were meant.
        remote::MeasureReq q;
        q.op = rp::MOP_SET_FOLD;
        q.join = rp::SJ_DIFF;
        q.foldForm = setfold::foldForm(rp::SJ_DIFF);
        q.cfaType = cfaType;
        q.cfaPattern = cfaPattern;
        for (int i = 0; i < 3; i++) {
            remote::MeasureReq::Role rr;
            rr.role = "r" + std::to_string(i);
            rr.paths = levels[i]->paths;
            q.roles.push_back(rr);
        }
        remote::MeasureResult mr;
        std::string e8;
        const bool ok8 = s.measure(q, mr, e8);
        fprintf(stderr, "rsetselftest: refusal(join arity): %s\n", e8.c_str());
        check(!ok8 && e8.find("between two roles") != std::string::npos &&
                  e8.find("\"r2\"") != std::string::npos,
              "RS18 a difference join over 3 roles is refused, naming them");
    }
    {   // A role that owns no path. The companion count array's invariant is
        // checked rather than trusted: a mis-sliced request would fold one
        // role's frames into another's and look perfectly normal.
        remote::MeasureReq q;
        q.op = rp::MOP_SET_FOLD;
        q.join = rp::SJ_NONE;
        q.foldForm = setfold::foldForm(rp::SJ_NONE);
        remote::MeasureReq::Role a, b;
        a.role = "dark"; a.paths = dark.paths;
        b.role = "ghost";
        q.roles.push_back(a);
        q.roles.push_back(b);
        remote::MeasureResult mr;
        std::string e9;
        const bool ok9 = s.measure(q, mr, e9);
        fprintf(stderr, "rsetselftest: refusal(empty role): %s\n", e9.c_str());
        check(!ok9 && e9.find("\"ghost\" owns no path") != std::string::npos,
              "RS19 a role that owns no path is refused by name");
    }

    // ---- RS20: min_frames, counted THERE and worded HERE ---------------------
    // The peer answers with facts and no sums; the sentence a person reads is
    // composed here, by the same function the local path composes it with. So
    // the two are string-equal by construction, and this asks it.
    {
        SetPeerRole one = peerRole("dark", dark);
        one.paths.resize(1);                       // one frame has no time axis
        std::string e10;
        SetResult P = setPeerDsnu(s, "short", one, cfaType, cfaPattern, e10);
        SetRoleFold H;                             // the same state, described here
        H.role = "dark"; H.member = dark.name; H.folded = false;
        H.frames = 1; H.expected = 1;
        SetResult L = composeDirectDsnu("short", H);
        fprintf(stderr, "rsetselftest: refusal(min_frames): %s\n", P.refusal.c_str());
        check(e10.empty() && !P.ok && P.refusal == L.refusal && !P.refusal.empty(),
              "RS20 a 1-frame role refuses in the settled words, from the peer's count");
    }

    // ---- RS21/RS22: a peer that predates the op -----------------------------
    // Not a thought experiment about an old peer: an old peer, on this disk,
    // refusing - and refused BEFORE the request is sent, from the number it
    // announced, so the sentence says which mismatch it is.
    {
        rsetSetEnv("VIEWER_SERVE_PROTOCOL", "7");
        remote::Session old;
        std::string eo;
        if (!old.start("", exe, eo)) {
            fprintf(stderr, "rsetselftest: old peer: %s\n", eo.c_str());
            bad++;
        } else {
            check(old.peerVersion() == 7, "RS21 the second peer really speaks 7");
            std::string e11;
            SetResult P = setPeerDsnu(old, "old", peerRole("dark", dark),
                                      cfaType, cfaPattern, e11);
            fprintf(stderr, "rsetselftest: refusal(old peer):\n%s\n", e11.c_str());
            check(!e11.empty() && !P.ok &&
                      e11.find("too old for set analysis") != std::string::npos &&
                      e11.find("it speaks protocol 7") != std::string::npos &&
                      e11.find("MOP_SET_FOLD needs 8") != std::string::npos &&
                      e11.find("Nothing was folded") != std::string::npos,
                  "RS22 ...and the client refuses from the number, naming the mismatch");
            old.stop();
        }
        rsetSetEnv("VIEWER_SERVE_PROTOCOL", nullptr);
    }

    s.stop();
    closeAll();
    stopSequenceLoader();
    fprintf(stderr, "rsetselftest: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
