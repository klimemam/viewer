// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- direct DSNU / PRNU: the SetAnalyzers of #57 judgment 4 ---------------
    // docs/flat-field-stats.md (b), settled 2026-08-09. What is asserted here:
    //
    //   E1  the famous names are ENFORCED, not merely followed: a scan of the
    //       shipping sources for bare "PRNU"/"DSNU" in UI-visible string
    //       literals, and for the standard-name gate
    //   S1  DSNU against hand-derived values, the derivation beside the assert
    //   S2  DSNU IS the corrected sigma_fpn of the dark stack - bit for bit
    //       against the Temporal panel, not to a tolerance
    //   S3  PRNU against hand-derived values, pixel-wise subtraction form
    //   S4  both clamp, and say they clamped
    //   S5  ground truth from tools/testdata/multi, where the injected fixed
    //       pattern is SHARED between stacks and must subtract to zero
    //   S6  every refusal path, in the settled words
    //   S7  the result names its set and its roles n/N
    //   S8  the required provenance fields (cutoff, estimator form) are there
    if (!g_setAnalysisSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "setanalysisselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        // the range selftest's loader loop, verbatim: numbered files load as a
        // stack without asking, and the folder picker is accepted whole
        auto loadAll = [&]() {
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
                    !app.folderPickOpen && app.seqs.size() >= 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };

        // ================= E1: the famous-name rule, mechanically ==============
        // docs/terminology.md's invariant (b): "前処理・参照を含意する有名名
        // (PRNU / DSNU …) は SetAnalyzer だけが名乗る。General は中立な量の名前
        // だけを出す — 例外なし". The ROIs column was renamed for this rule in
        // 5d8cb72 and the TOOLTIP under that column kept printing "PRNU" over a
        // General/frame ratio for eleven weeks - which is what a rule with no
        // check looks like from the inside. So: the bare names may appear in
        // UI-visible string literals of the shipping sources in ONE file, the
        // SetAnalyzer table.
        //
        // Deliberately narrow, so it stays true rather than becoming a
        // suppression list:
        //   - STRING LITERALS only. Comments explain the rule and must be free
        //     to name it; the scanner strips them, so this very paragraph is
        //     not a violation of itself.
        //   - the UPPERCASE spelling only. The canon writes the famous names in
        //     caps ("裸の PRNU / DSNU"), while the Specific analyzer's QUALIFIED
        //     proxy names are lowercase identifiers (`prnu_pct`,
        //     `uniformity/prnu-fpn`) and are explicitly allowed to stay
        //     (docs/analysis-layers.md §7). Case is what tells a claim from an
        //     identifier here, and it is checkable.
        //   - core/selftest is out of scope: it prints to a terminal, not to a
        //     screen, and its fixtures legitimately NAME sets "PRNU 10lx" (a
        //     set's human name is the user's, not ours).
        // The standard-name gate rides along: "EMVA 1288" may not reach any
        // screen until the text has been checked (docs/analysis-layers.md §6).
        {
            const char* kOwner = "core/app/setanalysis.inc";
            const char* kRoots[] = { "core", "plugins", "include" };
            // The standard-name gate has FOUR pre-existing occurrences, all in
            // the Series Analysis panel and all of them disclaimers ("NOT
            // verified against the standard text - do not cite either method as
            // ... compliant"). Whether a disclaimer that names the standard is
            // itself a screen naming the standard is a judgment for #57's owner,
            // not for this PR to settle by rewriting another panel's settled
            // wording - so they are PINNED here, by their exact text. The list
            // is a freeze, not an amnesty: nothing new can join it without
            // editing this file, and the day the gate is answered these four
            // either go or the entry does.
            const char* kPinned[] = {
                "matches EMVA 1288 Release 4's published approach as best",
                "cite either method as EMVA 1288 compliant.",
                "and on the chart. Own naming, NOT an EMVA 1288 compliance",
                "EMVA-style window, own naming - NOT an EMVA 1288 compliance",
            };
            struct Hit { std::string file; int line; std::string lit, token; };
            std::vector<Hit> hits;
            bool pinHit[sizeof kPinned / sizeof *kPinned] = {};
            int scanned = 0;
            std::vector<std::string> files;
            for (const char* root : kRoots) {
                std::error_code ec;
                for (std::filesystem::recursive_directory_iterator
                         it(std::filesystem::path(root), ec), end;
                     it != end; it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    std::string p = it->path().generic_u8string();
                    if (p.find("core/selftest/") != std::string::npos) continue;
                    std::string ext = it->path().extension().u8string();
                    if (ext != ".cpp" && ext != ".c" && ext != ".h" && ext != ".inc") continue;
                    files.push_back(p);
                }
            }
            std::sort(files.begin(), files.end());
            for (const std::string& p : files) {
                std::ifstream in(pathFromUtf8(p), std::ios::binary);
                if (!in) continue;
                std::string src((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
                scanned++;
                const bool owner = p.size() >= strlen(kOwner) &&
                                   p.compare(p.size() - strlen(kOwner), strlen(kOwner), kOwner) == 0;
                // one pass: strip // and /* */ and '...', collect "..." bodies
                int line = 1;
                for (size_t i = 0; i < src.size(); i++) {
                    char c = src[i];
                    if (c == '\n') { line++; continue; }
                    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
                        while (i < src.size() && src[i] != '\n') i++;
                        line++;
                        continue;
                    }
                    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
                        i += 2;
                        while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                            if (src[i] == '\n') line++;
                            i++;
                        }
                        i++;
                        continue;
                    }
                    if (c == '\'') {                 // a char literal, never text
                        i++;
                        while (i < src.size() && src[i] != '\'') {
                            if (src[i] == '\\') i++;
                            i++;
                        }
                        continue;
                    }
                    if (c != '"') continue;
                    const int startLine = line;
                    std::string lit;
                    i++;
                    while (i < src.size() && src[i] != '"') {
                        if (src[i] == '\\') { lit += src[i]; i++; }
                        if (i < src.size()) {
                            if (src[i] == '\n') line++;
                            lit += src[i];
                            i++;
                        }
                    }
                    const char* toks[] = { "PRNU", "DSNU", "EMVA 1288", "EMVA1288" };
                    for (const char* t : toks) {
                        const bool famous = t[0] == 'P' || t[0] == 'D';
                        if (famous && owner) continue;      // the one place they live
                        size_t at = lit.find(t);
                        while (at != std::string::npos) {
                            // whole word: an uppercase run inside a longer
                            // identifier is not a claim about a measurement
                            const size_t after = at + strlen(t);
                            auto wordy = [](char q) {
                                return (q >= 'A' && q <= 'Z') || (q >= 'a' && q <= 'z') ||
                                       (q >= '0' && q <= '9') || q == '_';
                            };
                            const bool bare = (at == 0 || !wordy(lit[at - 1])) &&
                                              (after >= lit.size() || !wordy(lit[after]));
                            bool pinned = false;
                            for (size_t pi = 0; pi < sizeof kPinned / sizeof *kPinned; pi++)
                                if (lit.find(kPinned[pi]) != std::string::npos) {
                                    pinned = true;
                                    pinHit[pi] = true;
                                }
                            if (bare && !pinned) {
                                hits.push_back({ p, startLine, lit, t });
                                break;
                            }
                            if (pinned) break;
                            at = lit.find(t, at + 1);
                        }
                    }
                }
            }
            for (const Hit& h : hits)
                fprintf(stderr, "setanalysisselftest: E1 VIOLATION %s:%d  \"%s\"  <- %s\n",
                        h.file.c_str(), h.line, h.lit.c_str(), h.token.c_str());
            fprintf(stderr, "setanalysisselftest: E1 scanned %d shipping source file(s), "
                            "%d violation(s)\n", scanned, (int)hits.size());
            bool allPins = true;
            for (size_t pi = 0; pi < sizeof kPinned / sizeof *kPinned; pi++)
                if (!pinHit[pi]) {
                    allPins = false;
                    fprintf(stderr, "setanalysisselftest: E1 STALE PIN \"%s\"\n", kPinned[pi]);
                }
            check(scanned > 60, "E1 the scan actually reached the sources");
            check(hits.empty(),
                  "E1 bare PRNU/DSNU (and the standard's name) live in one file only");
            check(allPins,
                  "E1 every pinned pre-existing occurrence still exists (a dead pin "
                  "is a hole)");
        }

        // ================= fixtures built in memory ============================
        // Analytic, exact in float32, so every expected value below is
        // arithmetic rather than a fit - the V27 discipline.
        auto makeStack = [&](const char* name, int W, int H, int NF,
                             const std::function<double(int, int, int)>& gen) {
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = name;
            si.expectedFrames = NF;
            app.seqs.push_back(si);
            for (int f = 0; f < NF; f++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = std::string(name) + "_" + std::to_string(f) + ".npy";
                d->src->w = W; d->src->h = H; d->src->ch = 1;
                d->src->dtype = "f32";
                d->src->data.resize((size_t)W * H);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        d->px()[(size_t)y * W + x] = (float)gen(x, y, f);
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->seqId = si.id;
                d->seqIndex = f;
                app.images.push_back(std::move(d));
            }
            return si.id;
        };
        auto makeSet = [&](const char* name,
                           const std::vector<std::pair<std::string, int>>& stackRoles) {
            App::ASet S;
            S.id = app.nextASetId++;
            S.batchId = 1;
            S.name = name;
            for (const auto& kv : stackRoles) {
                App::ASetRole r;
                r.role = kv.first;
                r.seqId = kv.second;
                S.roles.push_back(r);
            }
            app.analysisSets.push_back(S);
            return S.id;
        };
        auto setById = [&](int id) -> const App::ASet& { return *aSetById(id); };

        const int W = 32, H = 32, NF = 4;
        const double L = 1000.0;

        {   // ---- S5: ground truth from the fixture generator ------------------
            // tools/gen_testdata.py writes multi/ as THREE stacks of 5 frames at
            // 1000 / 1200 / 1400 DN, each frame = level + fixed_m + N(0, 28),
            // where fixed_m ~ N(0, 13.5) is drawn ONCE and shared by all three.
            // Two things follow, and both are ground truth rather than a
            // regression baseline:
            //
            //   DSNU on one stack must recover 13.5 DN. The uncorrected bound is
            //   sqrt(13.5^2 + 28^2/5) = sqrt(182.25 + 156.8) = 18.41 DN, i.e.
            //   36% high - the error stage 1 measured at 38% on this fixture.
            //
            //   PRNU with image = the 1200 stack and dark = the 1000 stack must
            //   come out at ~0: the fixed pattern is IDENTICAL in the two, so
            //   the per-pixel subtraction removes it exactly and what remains is
            //   temporal residual, which is what C_image + C_dark takes off. Get
            //   the estimator wrong and it does not fail subtly: skip the
            //   subtraction and you print DSNU/mu = 13.5/200 = 6.75%.
            closeAll();
            app.seqLoadMode = 1;                  // numbered files ARE a stack here
            openFolder(g_setAnalysisSelftest);
            loadAll();
            fprintf(stderr, "setanalysisselftest: S5 opened %d stack(s), %d frame(s)\n",
                    (int)app.seqs.size(), (int)app.images.size());
            // Order the fixture's stacks by level so the test does not depend on
            // the order the loader happened to land them in. This is the TEST
            // AUTHOR choosing which stack to declare as which role - the program
            // itself never looks at a level to decide that (the canon's "役割の
            // 束縛は宣言である。推論しない"), and everything below goes through
            // an explicit binding.
            std::vector<std::pair<double, int>> byLevel;
            for (const auto& s : app.seqs) {
                StackStats st = computeStackStats(s.id, 0, 0, 0, 0);
                if (st.valid) byLevel.push_back({ st.mean[0], s.id });
            }
            std::sort(byLevel.begin(), byLevel.end());
            std::vector<int> seqs;
            for (const auto& kv : byLevel) seqs.push_back(kv.second);
            check(seqs.size() >= 2, "S5 the multi fixture opened its stacks");
            if (seqs.size() >= 2) {
                int sDark = makeSet("GT dark", { { "dark", seqs[0] } });
                SetResult D0 = runSetAnalyzer(setById(sDark), SA_DSNU);
                int sD1 = makeSet("GT dark 1", { { "dark", seqs[1] } });
                SetResult D1 = runSetAnalyzer(setById(sD1), SA_DSNU);
                int sPrnu = makeSet("GT prnu", { { "image", seqs[1] }, { "dark", seqs[0] } });
                SetResult P = runSetAnalyzer(setById(sPrnu), SA_PRNU);
                const double truth = 13.5;              // gen_testdata.py's sigma
                const double naive = D1.ok && P.ok && P.mu[0] > 0
                                     ? 100.0 * D1.value[0] / P.mu[0] : 0.0;
                fprintf(stderr, "setanalysisselftest: S5 DSNU %.6g DN (injected %.4g, "
                                "uncorrected %.6g = %.1f%% high)  mu %.6g  "
                                "PRNU %.6g%% (sigma_d %.6g, clamped %d; without the "
                                "subtraction %.4g%%, without the correction %.4g%%)\n",
                        D0.value[0], truth, D0.uncorr[0],
                        D0.value[0] > 0 ? 100.0 * (D0.uncorr[0] / D0.value[0] - 1.0) : 0.0,
                        P.ok ? P.mu[0] : 0.0, P.ok ? P.value[0] : -1.0,
                        P.ok ? P.sigmaD[0] : -1.0, P.ok ? (int)P.clamped[0] : -1,
                        naive, P.ok ? P.uncorr[0] : -1.0);
                check(D0.ok && fabs(D0.value[0] - truth) < 0.10 * truth,
                      "S5 DSNU recovers the injected 13.5 DN fixed pattern");
                check(D0.ok && D0.uncorr[0] > 1.30 * D0.value[0],
                      "S5 ...which the uncorrected bound overstates by >30%");
                check(P.ok && P.mu[0] > 150.0 && P.mu[0] < 250.0,
                      "S5 mu is the 200 DN level step between the two stacks");
                check(P.ok && P.value[0] < 1.5,
                      "S5 a fixed pattern shared by image and dark subtracts to ~0");
                check(naive > 5.0,
                      "S5 ...where forgetting the subtraction would read ~6.7%");
                // and forgetting the -C is worse than forgetting nothing: the
                // temporal residual of BOTH stacks lands in var(D), so the
                // uncorrected form reports ~8.8% of "fixed pattern" on a pair
                // that has none to find
                check(P.ok && P.uncorr[0] > 5.0,
                      "S5 ...and forgetting the correction would read ~8.8%");
            }
            closeAll();
        }

        {   // ---- S1 / S2 / S4: DSNU over a dark stack -------------------------
            // The fixture, and therefore the derivation:
            //   pixel (x,y) of frame f = L + (x odd ? -B : +B) + (f odd ? +A : -A)
            // Per pixel the NF=4 values are L+-B+-A, so
            //   the temporal mean M is L+-B EXACTLY (the +-A cancels over 4),
            //   sum of squared deviations = 4A^2, so with ddof = n_i-1 = NF-1
            //       s_t,i^2 = 4A^2/(NF-1)
            //       C       = mean_i(s_t,i^2 / n_i) = s_t,i^2 / NF   (all n_i = NF)
            //   half the pixels sit at L+B and half at L-B, so with n = W*H
            //       var_spatial(M) at ddof=1 = B^2 * n/(n-1)
            //   DSNU = sqrt(max(0, var_spatial(M) - C))
            //
            // B=40, A=30, NF=4, n=1024:
            //   s_t^2 = 4*900/3 = 1200          C   = 300      fpn_corr = 17.32050808
            //   var   = 1600*1024/1023 = 1601.564027468        (uncorrected: 40.01954557)
            //   DSNU  = sqrt(1301.564027468)   = 36.07719527 DN
            // against a TRUE fixed pattern of 40 DN seen through only 4 frames.
            // B=5, A=30: var = 25.02443793 < C = 300, so the subtraction goes
            //   negative, the value CLAMPS at 0 and says so. Uncorrected this
            //   would report 5.002 DN of "fixed pattern" that is entirely the
            //   leftover of a 30 DN temporal swing averaged four ways.
            struct DCase { double B, A; bool clamp; const char* what; };
            const DCase CASES[2] = {
                { 40.0, 30.0, false, "S1 DSNU = sqrt(var_spatial,ddof1(M_dark) - C_dark)" },
                {  5.0, 30.0, true,  "S4 DSNU clamps at 0 when C exceeds the spread" },
            };
            for (const DCase& dc : CASES) {
                closeAll();
                const double B = dc.B, A = dc.A;
                int seq = makeStack("dark fixture", W, H, NF,
                                    [&](int x, int y, int f) {
                                        (void)y;
                                        return L + (x & 1 ? -B : B) + (f % 2 ? A : -A);
                                    });
                int sid = makeSet("D1", { { "dark", seq } });
                SetResult R = runSetAnalyzer(setById(sid), SA_DSNU);
                // the derivation, as expressions, next to the assert that uses it
                const double n = (double)W * H;
                const double stVar = 4.0 * A * A / (NF - 1.0);
                const double corr = stVar / NF;
                const double sVar = B * B * n / (n - 1.0);
                const double fVar = sVar - corr;
                const double dsnuExpect = fVar > 0 ? sqrt(fVar) : 0.0;
                fprintf(stderr, "setanalysisselftest: S1 B=%.4g A=%.4g NF=%d n=%.0f: "
                                "value %.9g (exp %.9g)  fpn_corr %.9g (exp %.9g)  "
                                "uncorrected %.9g (exp %.9g)  clamped %d (exp %d)\n",
                        B, A, NF, n, R.value[0], dsnuExpect, R.corrDn[0], sqrt(corr),
                        R.uncorr[0], sqrt(sVar), (int)R.clamped[0], (int)dc.clamp);
                check(R.ok && R.nPl == 1, "S1 the set runs and reports one plane");
                check(fabs(R.value[0] - dsnuExpect) < 1e-9, dc.what);
                check(fabs(R.corrDn[0] - sqrt(corr)) < 1e-9,
                      "S1 the correction AMOUNT rides with the value, in DN");
                check(R.clamped[0] == dc.clamp,
                      "S1 the clamp is declared exactly when it fired");
                check(fabs(R.uncorr[0] - sqrt(sVar)) < 1e-9,
                      "S1 the uncorrected bound is reported beside it");
                check(R.samples[0] == (size_t)(W * H),
                      "S1 every pixel entered - a SetAnalyzer does not decimate");
                check(R.used.size() == 1 && R.used[0].role == "dark" &&
                      R.used[0].n == NF && R.used[0].N == NF,
                      "S1 provenance names the role it used, n of N");

                // ---- S2: this IS the Temporal panel's sigma_fpn -------------
                // "applying the corrected sigma_fpn to a dark stack already IS
                // DSNU" (#57 judgment 4). Not approximately: the same
                // arithmetic in the same order, so the comparison is ==. The
                // fixture is 1024 samples, under the panel's 40000-sample
                // decimation cap, so both see every pixel; one channel, so the
                // panel's channel-0 sampling and the fold's all-channel pooling
                // are the same population.
                app.current = 0;
                app.selectedAnn = 0;
                recomputeTemporalIfNeeded(cur(), app.temporal[0]);
                const App::TemporalState& T = app.temporal[0];
                fprintf(stderr, "setanalysisselftest: S2 DSNU %.17g  sigma_fpn %.17g  "
                                "fpn_corr %.17g / %.17g\n",
                        R.value[0], T.fixedPattern[0], R.corrDn[0], T.fpnCorr[0]);
                check(T.valid && R.value[0] == T.fixedPattern[0],
                      "S2 DSNU == the dark stack's corrected sigma_fpn, bit for bit");
                check(R.corrDn[0] == T.fpnCorr[0] && R.clamped[0] == T.fpnClamped[0],
                      "S2 ...and so do the correction and the clamp");
                closeAll();
            }
        }

        {   // ---- S3 / S4: PRNU, the per-pixel subtraction form ----------------
            // dark  (x,y,f) = L + colPat(x)                       + (f odd ? +Ad : -Ad)
            // image (x,y,f) = L + colPat(x) + MU + rowPat(y)      + (f odd ? +Ai : -Ai)
            // so the temporal means are exact and
            //   D = M_image - M_dark = MU + rowPat(y)     (the dark's own column
            //       pattern cancels EXACTLY - which is the whole point of the
            //       per-pixel form, and what a variance difference cannot do)
            //   var_spatial(D) at ddof=1 = P^2 * n/(n-1)
            //   C_image = 4Ai^2/(NF-1)/NF,  C_dark = 4Ad^2/(NF-1)/NF
            //   sd^2 = max(0, var_spatial(D) - C_image - C_dark)
            //   mu   = mean(M_image) - mean(M_dark) = MU  (rowPat sums to 0 over
            //          an even H, colPat cancels term by term)
            //   PRNU = 100 * sd / mu
            //
            // P=40, Ai=Ad=30, MU=2000, NF=4, n=1024:
            //   var  = 1600*1024/1023 = 1601.564027468
            //   C    = 300 + 300      = 600         fpn_corr = 24.49489743
            //   sd   = sqrt(1001.564027468) = 31.64749638 DN
            //   PRNU = 100*31.64749638/2000 = 1.582374819 %
            //   uncorrected: 100*40.01954557/2000 = 2.000977278 % - 26.5% high,
            //   on a fixture where both numbers are exact.
            // P=5: var = 25.02443793 < 600, so PRNU clamps at 0 and says so.
            struct PCase { double P; bool clamp; const char* what; };
            const PCase CASES[2] = {
                { 40.0, false, "S3 PRNU = 100*sqrt(var(D) - C_image - C_dark)/mu" },
                {  5.0, true,  "S4 PRNU clamps at 0 when the corrections exceed var(D)" },
            };
            const double Bd = 40.0, Ai = 30.0, Ad = 30.0, MU = 2000.0;
            for (const PCase& pc : CASES) {
                closeAll();
                const double P = pc.P;
                int dseq = makeStack("dark fixture", W, H, NF,
                                     [&](int x, int y, int f) {
                                         (void)y;
                                         return L + (x & 1 ? -Bd : Bd) + (f % 2 ? Ad : -Ad);
                                     });
                int iseq = makeStack("image fixture", W, H, NF,
                                     [&](int x, int y, int f) {
                                         return L + (x & 1 ? -Bd : Bd) + MU +
                                                (y & 1 ? -P : P) + (f % 2 ? Ai : -Ai);
                                     });
                int sid = makeSet("P1", { { "image", iseq }, { "dark", dseq } });
                SetResult R = runSetAnalyzer(setById(sid), SA_PRNU);
                const double n = (double)W * H;
                const double corr = 4.0 * Ai * Ai / (NF - 1.0) / NF +
                                    4.0 * Ad * Ad / (NF - 1.0) / NF;
                const double vD = P * P * n / (n - 1.0);
                const double sdVar = vD - corr;
                const double sdExpect = sdVar > 0 ? sqrt(sdVar) : 0.0;
                const double prnuExpect = 100.0 * sdExpect / MU;
                fprintf(stderr, "setanalysisselftest: S3 P=%.4g MU=%.6g: PRNU %.9g%% "
                                "(exp %.9g%%; uncorrected %.9g%%)  sigma_d %.9g (exp %.9g)  "
                                "mu %.9g (exp %.9g)  fpn_corr %.9g (exp %.9g)  clamped %d\n",
                        P, MU, R.value[0], prnuExpect, 100.0 * sqrt(vD) / MU,
                        R.sigmaD[0], sdExpect, R.mu[0], MU, R.corrDn[0], sqrt(corr),
                        (int)R.clamped[0]);
                check(R.ok && R.nPl == 1, "S3 the two-role set runs");
                check(R.mu[0] == MU,
                      "S3 mu is mean(M_image) - mean(M_dark), and the dark's own "
                      "pattern cancels");
                check(fabs(R.sigmaD[0] - sdExpect) < 1e-9,
                      "S3 sigma_d subtracts BOTH stacks' temporal residuals");
                check(fabs(R.value[0] - prnuExpect) < 1e-9, pc.what);
                check(fabs(R.corrDn[0] - sqrt(corr)) < 1e-9,
                      "S3 the declared correction is C_image + C_dark");
                check(R.clamped[0] == pc.clamp,
                      "S3 the clamp is declared exactly when it fired");
                check(R.used.size() == 2 && R.used[0].role == "image" &&
                      R.used[1].role == "dark",
                      "S3 provenance names both roles, in schema order");
                closeAll();
            }
        }

        {   // ---- S7 / S8: the result names itself, and declares its terms -----
            closeAll();
            int dseq = makeStack("dark fixture", W, H, NF,
                                 [&](int x, int y, int f) {
                                     (void)y; return L + (x & 1 ? -40.0 : 40.0) +
                                                     (f % 2 ? 30.0 : -30.0);
                                 });
            int iseq = makeStack("image fixture", W, H, NF,
                                 [&](int x, int y, int f) {
                                     return L + (x & 1 ? -40.0 : 40.0) + 2000.0 +
                                            (y & 1 ? -40.0 : 40.0) + (f % 2 ? 30.0 : -30.0);
                                 });
            int sid = makeSet("10lx vs dark", { { "image", iseq }, { "dark", dseq } });
            SetResult D = runSetAnalyzer(setById(sid), SA_DSNU);
            SetResult P = runSetAnalyzer(setById(sid), SA_PRNU);
            fprintf(stderr, "setanalysisselftest: S7 \"%s\" / \"%s\"\n",
                    setResultTitle(D).c_str(), setResultTitle(P).c_str());
            // the canon's invariant (a): a result names its layer and its set.
            // The extra role bound for PRNU does not stop DSNU running, and the
            // tag names only the roles it USED (docs/reader-analysisset.md §1.5).
            check(setResultTitle(D) == "DSNU [DN] - set \"10lx vs dark\": dark 4/4",
                  "S7 DSNU names its set and the one role it used");
            check(setResultTitle(P) ==
                      "PRNU [%] - set \"10lx vs dark\": image 4/4, dark 4/4",
                  "S7 PRNU names its set and both roles, n/N each");
            check(D.cutoff == kSetCutoffRaw && P.cutoff == kSetCutoffRaw && !D.cutoff.empty(),
                  "S8 the cutoff is a required field and states 'raw'");
            check(P.form.find("per-pixel subtraction") != std::string::npos &&
                  P.form.find("NOT the variance-difference form") != std::string::npos,
                  "S8 the result says WHICH estimator wrote it");
            check(D.form.find("var_spatial(M_dark)") != std::string::npos,
                  "S8 ...and so does DSNU");
            closeAll();
        }

        {   // ---- S6: the refusals, in the settled words -----------------------
            closeAll();
            int good = makeStack("good", W, H, NF,
                                 [&](int x, int y, int f) {
                                     (void)y; return L + (x & 1 ? -40.0 : 40.0) +
                                                     (f % 2 ? 30.0 : -30.0);
                                 });
            // (a) the role was never declared
            {
                int sid = makeSet("R-none", {});
                SetResult R = runSetAnalyzer(setById(sid), SA_DSNU);
                fprintf(stderr, "setanalysisselftest: S6a \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "this set has no \"dark\" role - a role binding is declared, "
                          "never inferred",
                      "S6a no dark role: refused, and the reason is the rule");
            }
            // (b) declared, unbound - the Ref's own reason becomes the refusal
            {
                App::ASet S;
                S.id = app.nextASetId++; S.batchId = 1; S.name = "R-unbound";
                App::ASetRole r;
                r.role = "dark";
                r.reason = "darks/dark_50ms/ not found";
                S.roles.push_back(r);
                app.analysisSets.push_back(S);
                SetResult R = runSetAnalyzer(setById(S.id), SA_DSNU);
                fprintf(stderr, "setanalysisselftest: S6b \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "\"dark\" is unbound - darks/dark_50ms/ not found",
                      "S6b unbound dark: the binding's own reason IS the refusal");
            }
            // (c) bound to a loose frame - the settled sentence, verbatim
            {
                auto d = std::make_unique<ImageDoc>();
                d->name = "one.npy";
                d->src->w = W; d->src->h = H; d->src->ch = 1; d->src->dtype = "f32";
                d->src->data.assign((size_t)W * H, 100.0f);
                d->syncMirrors();
                d->uid = app.nextUid++;
                const uint64_t uid = d->uid;
                app.images.push_back(std::move(d));
                App::ASet S;
                S.id = app.nextASetId++; S.batchId = 1; S.name = "R-frame";
                App::ASetRole r;
                r.role = "dark";
                r.frameUid = uid;
                S.roles.push_back(r);
                app.analysisSets.push_back(S);
                SetResult R = runSetAnalyzer(setById(S.id), SA_DSNU);
                fprintf(stderr, "setanalysisselftest: S6c \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "\"dark\": 1 frame has no time axis - "
                          "the -sigma_t^2/N correction needs a stack",
                      "S6c a frame role is refused in the settled words");
            }
            // (d) bound to a series - the schema is Stack-only
            {
                App::Series ser;
                ser.id = app.nextSeriesId++;
                ser.batchId = 1;
                ser.name = "sweep";
                app.series.push_back(ser);
                App::ASet S;
                S.id = app.nextASetId++; S.batchId = 1; S.name = "R-series";
                App::ASetRole r;
                r.role = "dark";
                r.seriesId = ser.id;
                S.roles.push_back(r);
                app.analysisSets.push_back(S);
                SetResult R = runSetAnalyzer(setById(S.id), SA_DSNU);
                fprintf(stderr, "setanalysisselftest: S6d \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "\"dark\" is bound to a series - this analysis takes Stack() "
                          "roles only (a series is a swept parameter, not a time axis)",
                      "S6d a series role is refused: the schema is Stack-only");
            }
            // (e) min_frames = 2, because the correction is made of s_t
            {
                int one = makeStack("lonely", W, H, 1,
                                    [&](int x, int y, int f) {
                                        (void)y; (void)f; return L + x; });
                int sid = makeSet("R-one", { { "dark", one } });
                SetResult R = runSetAnalyzer(setById(sid), SA_DSNU);
                fprintf(stderr, "setanalysisselftest: S6e \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "\"dark\": needs >= 2 frames - \"lonely\" has 1 of 1 loaded",
                      "S6e min_frames = 2, and the shortfall is n of N");
            }
            // (f) PRNU across two shapes
            {
                // NOT `small`: <rpcndr.h>, which arrives with windows.h, is
                // `#define small char`, so MSVC read the declaration as "int
                // char" and this file stopped compiling on one of the three
                // platforms. The stack's NAME can still say it.
                int tiny = makeStack("small", 16, 16, NF,
                                     [&](int x, int y, int f) {
                                         (void)y; return L + x + (f % 2 ? 1.0 : -1.0); });
                int sid = makeSet("R-shape", { { "image", good }, { "dark", tiny } });
                SetResult R = runSetAnalyzer(setById(sid), SA_PRNU);
                fprintf(stderr, "setanalysisselftest: S6f \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "\"image\" is 32x32x1ch and \"dark\" is 16x16x1ch - "
                          "the per-pixel subtraction needs one shape",
                      "S6f shape disagreement is refused, and NAMES both shapes");
            }
            // (g) mu <= 0. Binding one stack to both roles is LEGAL
            // (docs/reader-analysisset.md §1.2 refuses to forbid it) - so the
            // analysis is the thing that answers, with a number and a reason.
            {
                int sid = makeSet("R-mu", { { "image", good }, { "dark", good } });
                SetResult R = runSetAnalyzer(setById(sid), SA_PRNU);
                fprintf(stderr, "setanalysisselftest: S6g \"%s\"\n", R.refusal.c_str());
                check(!R.ok && R.refusal ==
                          "mean(M_image) - mean(M_dark) = 0 DN is not above 0 - "
                          "a ratio to a level needs a level",
                      "S6g mu <= 0 prints no PRNU, and says why in one line");
            }
            // (h) the image role, missing
            {
                int sid = makeSet("R-noimage", { { "dark", good } });
                SetResult R = runSetAnalyzer(setById(sid), SA_PRNU);
                check(!R.ok && R.refusal ==
                          "this set has no \"image\" role - a role binding is declared, "
                          "never inferred",
                      "S6h PRNU needs image AND dark; the missing one is named");
                // ...and the extra-role rule, from the other side: the same set
                // runs DSNU, because a role it does not need cannot stop it
                SetResult D = runSetAnalyzer(setById(sid), SA_DSNU);
                check(D.ok, "S6h a set that cannot do PRNU still does DSNU");
            }
            closeAll();
        }


        fprintf(stderr, "setanalysisselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
