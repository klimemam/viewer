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
                             const std::function<double(int, int, int)>& gen,
                             int CH = 1) {
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = name;
            si.expectedFrames = NF;
            app.seqs.push_back(si);
            for (int f = 0; f < NF; f++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = std::string(name) + "_" + std::to_string(f) + ".npy";
                d->src->w = W; d->src->h = H; d->src->ch = CH;
                d->src->dtype = "f32";
                d->src->data.resize((size_t)W * H * CH);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        for (int c = 0; c < CH; c++)
                            d->px()[((size_t)y * W + x) * CH + c] = (float)gen(x, y, f);
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


        // ================= the separation fit (#57 judgment 5) ================
        // docs/flat-field-stats.md (c) "加法性と乗法性の分離" (確定 2026-08-09).
        // The model is sigma_fpn^2(mu) = DSNU^2 + (PRNU*mu)^2, a straight line
        // in mu^2, fitted by UNWEIGHTED OLS over the levels of a sweep. What is
        // asserted:
        //
        //   F1  a fixture sweep whose TRUE gain and TRUE per-pixel gain spread
        //       are known by construction, against the fit's hand-derived
        //       intercept and slope
        //   F2  a level's sigma_fpn^2 IS the direct row's estimator - bit for
        //       bit against a set that binds the same stack as "dark"
        //   F3  the lit levels are NOT dark-subtracted, and the fixture can
        //       tell: the dark carries a DIFFERENT additive pattern
        //   F4  the residual display, and both coefficients clamping
        //       independently, on a fixture perturbed by a known amount
        //   F5  frame / sum / not-included / valueless members are excluded and
        //       COUNTED, in one sentence the screen and this test share
        //   F6  every refusal: fewer than 3 levels, a Series() role bound to a
        //       stack and to a frame, both missing roles, mismatched planes
        //   F7  the result names its set, its sweep and M
        //   F8  cutoff, estimator form, and the uncorrelatedness note are
        //       required fields that are actually filled
        //   F9  a sweep member and the dark that are not one shape: REFUSED,
        //       with both shapes on the line (確定 2026-08-10, #57)
        //   F10 a dark that ALSO stands as a level of its own sweep: allowed,
        //       numerically the intercept's anchor (mu exactly 0, y bit for bit
        //       the direct row's), and the overlap SAID (確定 2026-08-10, #57)
        //
        // Every fixture below is EXACT: the pixel values are integers under
        // 2^24, the accumulators are integers under 2^53, and the temporal
        // spread {-9,-3,0,3,9}*kt over 5 frames was chosen so that
        //   s_t^2 = 45*kt^2 and C = s_t^2/5 = 9*kt^2
        // are both exact in binary, and so that the deterministic residual left
        // in the temporal mean (+-3*kt on the (x^y) parity) has population
        // variance 9*kt^2 = C - i.e. the fixture's mean image carries exactly
        // the residual the correction is there to take off. The ONE inexact
        // number in the whole construction is the ddof=1 factor
        // n/(n-1) = 1024/1023, which is not a dyadic rational; it is written
        // into every expected value below rather than hidden in a tolerance.
        {
            const int Wf = 32, Hf = 32, NFs = 5;
            const double L0 = 1024.0;        // black level
            const double Bl = 32.0;          // the LEVELS' additive pattern (y parity)
            const double Bd = 8.0;           // the DARK's own - deliberately different
            const double qg = 1.0 / 32.0;    // per-pixel gain spread (x parity)
            const double kt = 8.0;           // temporal scale
            const double gainDN = 128.0;     // DN per lx: the sweep's true sensitivity
            static const double TD[5] = { -9, -3, 0, 3, 9 };
            const double nS = (double)(Wf * Hf);
            const double rr = nS / (nS - 1.0);       // the ddof=1 factor, 1024/1023
            const double Cl = 9.0 * kt * kt;         // C at every level: 576, exactly

            // pixel = L0 +- B (y parity)                      the additive pattern
            //            + mu0 * (1 +- qg) (x parity)         the multiplicative one
            //            +- 3*kt (x^y parity)                 the temporal mean's residual
            //            + TD[f]*kt                           the temporal spread
            // The three +- patterns are mutually orthogonal (Walsh), so the
            // population spatial variance of the temporal mean is exactly
            // B^2 + (mu0*qg)^2 + 9*kt^2 - the sum, with no cross terms.
            auto sweepPix = [&](double mu0, double Bthis, int x, int y, int f) {
                const double t = ((x ^ y) & 1) ? -3.0 * kt : 3.0 * kt;
                return L0 + ((y & 1) ? -Bthis : Bthis)
                          + mu0 * (1.0 + ((x & 1) ? -qg : qg))
                          + t + TD[f] * kt;
            };
            auto makeLevel = [&](const char* name, double mu0, double Bthis) {
                return makeStack(name, Wf, Hf, NFs, [&, mu0, Bthis](int x, int y, int f) {
                    return sweepPix(mu0, Bthis, x, y, f);
                });
            };
            auto makeDark = [&](const char* name, int NF) {
                return makeStack(name, Wf, Hf, NF, [&](int x, int y, int f) {
                    return sweepPix(0.0, Bd, x, y, f);
                });
            };
            struct SM { int seqId; uint64_t uid; double value; bool include; int fold; };
            auto makeSeries = [&](const char* name, const char* param, const char* unit,
                                  const std::vector<SM>& ms) {
                App::Series Sr;
                Sr.id = app.nextSeriesId++;
                Sr.batchId = 1;
                Sr.name = name;
                Sr.paramName = param;
                snprintf(Sr.unit, sizeof Sr.unit, "%s", unit);
                for (const SM& m : ms) {
                    App::Series::Member mm;
                    mm.seqId = m.seqId;
                    mm.frameUid = m.uid;
                    mm.value = m.value;
                    mm.include = m.include;
                    mm.fold = m.fold;
                    Sr.members.push_back(mm);
                }
                app.series.push_back(std::move(Sr));
                return app.series.back().id;
            };
            auto makeSepSet = [&](const char* name, int seriesId, int darkSeq) {
                App::ASet Ss;
                Ss.id = app.nextASetId++;
                Ss.batchId = 1;
                Ss.name = name;
                App::ASetRole rs; rs.role = "sweep"; rs.seriesId = seriesId;
                Ss.roles.push_back(rs);
                App::ASetRole rd; rd.role = "dark"; rd.seqId = darkSeq;
                Ss.roles.push_back(rd);
                app.analysisSets.push_back(Ss);
                return Ss.id;
            };

            {   // ---- F1 / F2 / F3: the exact sweep -------------------------
                // TRUE by construction: gain 128 DN/lx, additive pattern 32 DN,
                // per-pixel gain spread 1/32 = 3.125 %.
                //
                //   mu_l = mean(M_l) - mean(M_dark) = (L0 + mu0_l) - L0 = mu0_l
                //          EXACTLY (both +- patterns average to 0 over even W,H)
                //   y_l  = var_spatial(M_l, ddof=1) - C
                //        = (Bl^2 + (mu0_l*qg)^2 + 9kt^2) * rr - 9kt^2
                //        = [(Bl^2 + C)*rr - C]  +  [qg^2 * rr] * mu0_l^2
                //
                // so the line is known WITHOUT running a fit, and the residuals
                // are zero by construction:
                //   intercept = (1024 + 576)*1024/1023 - 576 = 1025.56402737
                //   slope     = (1/1024)*(1024/1023)        = 1/1023
                //   DSNU      = sqrt(1025.56402737)         = 32.0244286 DN
                //   PRNU      = 100/sqrt(1023)              = 3.12652700 %
                // against a true 32 DN and 3.125 %: the estimator sits 0.076 %
                // and 0.049 % high, and that gap is the ddof=1 factor, computed
                // here rather than tolerated.
                //
                // Skipping the -C at every level adds a CONSTANT 576 to every
                // y, so it lands entirely in the intercept and leaves the slope
                // untouched - the spec's own symptom, "DSNU だけが大きく出る":
                //   uncorrected DSNU = sqrt(1600*1024/1023) = 40.01954557 DN
                //                      (+24.97 %), PRNU unchanged.
                closeAll();
                const double lux[4] = { 4, 9, 16, 25 };
                int seqs[4];
                char nm[32];
                std::vector<SM> ms;
                for (int i = 0; i < 4; i++) {
                    snprintf(nm, sizeof nm, "%glx", lux[i]);
                    seqs[i] = makeLevel(nm, gainDN * lux[i], Bl);
                    ms.push_back({ seqs[i], 0, lux[i], true, FOLD_MEAN });
                }
                const int dseq = makeDark("dark", NFs);
                const int ser = makeSeries("lx sweep", "illuminance", "lx", ms);
                const int sid = makeSepSet("sep", ser, dseq);
                SepFit F = runSeparationFit(*aSetById(sid));

                const double interExp = (Bl * Bl + Cl) * rr - Cl;
                const double slopeExp = qg * qg * rr;
                const double dsnuExp = sqrt(interExp);
                const double prnuExp = 100.0 * sqrt(slopeExp);
                const double dsnuRawExp = sqrt((Bl * Bl + Cl) * rr);
                // ...and what a per-pixel dark subtraction WOULD have printed:
                // the levels' pattern minus the dark's, 32 - 8 = 24 DN.
                const double ifSubtracted = sqrt(((Bl - Bd) * (Bl - Bd) + Cl) * rr - Cl);
                fprintf(stderr,
                        "setanalysisselftest: F1 M=%d  intercept %.9g (exp %.9g)  "
                        "slope %.12g (exp %.12g)\n"
                        "setanalysisselftest: F1 dsnu %.9g (exp %.9g, true %.4g)  "
                        "prnu %.9g%% (exp %.9g%%, true %.4g%%)\n"
                        "setanalysisselftest: F1 uncorrected dsnu %.9g (exp %.9g, "
                        "%+.2f%%)  prnu %.9g%% (unchanged)  |  a per-pixel dark "
                        "subtraction would print %.6g\n",
                        F.M, F.ok ? F.inter[0] : 0.0, interExp,
                        F.ok ? F.slope[0] : 0.0, slopeExp,
                        F.ok ? F.dsnu.value[0] : -1.0, dsnuExp, Bl,
                        F.ok ? F.prnu.value[0] : -1.0, prnuExp, 100.0 * qg,
                        F.ok ? F.dsnu.uncorr[0] : -1.0, dsnuRawExp,
                        100.0 * (dsnuRawExp / dsnuExp - 1.0),
                        F.ok ? F.prnu.uncorr[0] : -1.0, ifSubtracted);
                check(F.ok && F.M == 4 && F.nPl == 1,
                      "F1 the sweep fits: 4 levels, one plane");
                check(F.ok && fabs(F.inter[0] - interExp) < 1e-6,
                      "F1 intercept = (B^2 + C)*n/(n-1) - C, derived by hand");
                check(F.ok && fabs(F.slope[0] - slopeExp) < 1e-15,
                      "F1 slope = qg^2 * n/(n-1), derived by hand");
                check(F.ok && fabs(F.dsnu.value[0] - dsnuExp) < 1e-9,
                      "F1 the intercept row is sqrt(intercept) [DN]");
                check(F.ok && fabs(F.prnu.value[0] - prnuExp) < 1e-9,
                      "F1 the slope row is 100*sqrt(slope) [%]");
                check(fabs(dsnuExp / Bl - 1.0) < 0.001 &&
                      fabs(prnuExp / (100.0 * qg) - 1.0) < 0.001,
                      "F1 ...and both land within 0.1% of the injected truth");
                check(F.ok && fabs(F.dsnu.uncorr[0] - dsnuRawExp) < 1e-9 &&
                      F.dsnu.uncorr[0] > 1.24 * F.dsnu.value[0],
                      "F1 without the -C the additive term reads ~25% high");
                check(F.ok && fabs(F.prnu.uncorr[0] - F.prnu.value[0]) < 1e-9,
                      "F1 ...and the multiplicative term is untouched by it");
                bool zeroResid = F.ok;
                for (const auto& L : F.levels)
                    if (fabs(L.resid[0]) > 1e-6) zeroResid = false;
                check(zeroResid, "F1 a sweep that obeys the model leaves no residual");
                check(F.ok && F.levels.size() == 4 &&
                      F.levels[0].mu[0] == gainDN * lux[0] &&
                      F.levels[3].mu[0] == gainDN * lux[3],
                      "F1 mu is mean(M_l) - mean(M_dark), exactly the injected level");
                check(F.ok && !F.dsnu.clamped[0] && !F.prnu.clamped[0] && F.r2[0] > 0.9999,
                      "F1 nothing clamped, and R^2 is 1 on an exact line");

                // ---- F2: a level's sigma_fpn^2 IS the direct estimator ------
                // Not "the same formula": the same function on the same
                // accumulator, so the comparison is ==. Bind level 2's stack as
                // a "dark" in its own set and the direct row must print exactly
                // the square root of what the fit used for it.
                if (F.ok && F.levels.size() == 4) {
                    int sD = makeSet("as dark", { { "dark", seqs[2] } });
                    SetResult D = runSetAnalyzer(*aSetById(sD), SA_DSNU);
                    fprintf(stderr, "setanalysisselftest: F2 level sigma_fpn %.17g  "
                                    "direct %.17g\n",
                            sqrt(F.levels[2].fpnVar[0]), D.value[0]);
                    check(D.ok && sqrt(F.levels[2].fpnVar[0]) == D.value[0],
                          "F2 a level's sigma_fpn is the direct row's, bit for bit");
                    check(D.ok && F.levels[2].corr[0] == Cl,
                          "F2 ...and its C is the level's own, exactly 9*kt^2");
                }
                // ---- F3: the lit levels are not dark-subtracted -------------
                // The dark's additive pattern is 8 DN and the levels' is 32. A
                // per-pixel subtraction would leave 24 and print 24.02 here;
                // the settled estimator leaves the levels alone and prints
                // 32.02. The two are 8 DN apart, so this is a fixture that can
                // actually tell them apart rather than a comment claiming it.
                check(F.ok && fabs(F.dsnu.value[0] - ifSubtracted) > 7.0,
                      "F3 the intercept is the LEVELS' additive term, not "
                      "levels-minus-dark");
                // ---- F7 / F8 on the same result -----------------------------
                fprintf(stderr, "setanalysisselftest: F7 \"%s\" / \"%s\"\n",
                        setResultTitle(F.dsnu).c_str(), setResultTitle(F.prnu).c_str());
                check(setResultTitle(F.dsnu) ==
                          "DSNU [DN] - set \"sep\": sweep \"lx sweep\" 4 levels, dark 5/5",
                      "F7 the additive row names its set, its sweep and M");
                check(setResultTitle(F.prnu) ==
                          "PRNU [%] - set \"sep\": sweep \"lx sweep\" 4 levels, dark 5/5",
                      "F7 ...and so does the multiplicative one");
                check(F.cutoff == kSetCutoffRaw && F.dsnu.cutoff == kSetCutoffRaw,
                      "F8 the cutoff is a required field and states 'raw'");
                check(F.dsnu.form.find("intercept") != std::string::npos &&
                      F.prnu.form.find("NOT dark-subtracted") != std::string::npos,
                      "F8 each row says which coefficient it is, and that the "
                      "levels are not dark-subtracted");
                check(F.note.find("UNCORRELATED") != std::string::npos &&
                      F.note.find("rho") != std::string::npos,
                      "F8 the uncorrelatedness assumption rides in the result");
                check(F.sweepWarn.find("LIGHT, not the exposure time") != std::string::npos,
                      "F8 ...and so does what the sweep has to be");
                check(sepLevelCensus(F) == "levels: 4 of 4 sweep member(s) used",
                      "F8 the census is said even when nothing was excluded");
                closeAll();
            }

            {   // ---- F4: residuals, and the two clamps ----------------------
                // Same construction, three levels, and the TOP one is given a
                // 96 DN additive pattern instead of 32. That moves its y by
                //   d = (96^2 - 32^2) * rr = 8192 * 1024/1023
                // and nothing else, so the perturbed fit follows from the
                // unperturbed one by the OLS influence identity - a different
                // route to the answer than the implementation's sums:
                //   slope' = slope + d*(x2-xb)/Sxx
                //   inter' = inter + d*(1/3 - xb*(x2-xb)/Sxx)
                //   resid_2 =  d*(1 - 1/3 - (x2-xb)^2/Sxx)
                //   resid_j = -d*(1/3 + (xj-xb)(x2-xb)/Sxx)      j = 0,1
                // With x = mu^2 = {262144, 1327104, 4194304} the intercept
                // multiplier is NEGATIVE (-0.1949), so a positive d drives the
                // intercept to about -572: the additive row clamps at 0 and
                // says so, while the multiplicative row stays positive. The two
                // coefficients clamp independently, and this fixture shows it.
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                const double Bp = 96.0;
                int seqs[3];
                std::vector<SM> ms;
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    seqs[i] = makeLevel(nm, mu0[i], i == 2 ? Bp : Bl);
                    ms.push_back({ seqs[i], 0, mu0[i] / gainDN, true, FOLD_MEAN });
                }
                const int dseq = makeDark("dark", NFs);
                const int sid = makeSepSet("perturbed",
                                           makeSeries("bumped", "illuminance", "lx", ms),
                                           dseq);
                SepFit F = runSeparationFit(*aSetById(sid));

                const double x[3] = { mu0[0] * mu0[0], mu0[1] * mu0[1], mu0[2] * mu0[2] };
                const double xb = (x[0] + x[1] + x[2]) / 3.0;
                const double Sxx = (x[0] - xb) * (x[0] - xb) + (x[1] - xb) * (x[1] - xb) +
                                   (x[2] - xb) * (x[2] - xb);
                const double d = (Bp * Bp - Bl * Bl) * rr;
                const double inter0 = (Bl * Bl + Cl) * rr - Cl;
                const double slope0 = qg * qg * rr;
                const double interP = inter0 + d * (1.0 / 3.0 - xb * (x[2] - xb) / Sxx);
                const double slopeP = slope0 + d * (x[2] - xb) / Sxx;
                const double rE[3] = {
                    -d * (1.0 / 3.0 + (x[0] - xb) * (x[2] - xb) / Sxx),
                    -d * (1.0 / 3.0 + (x[1] - xb) * (x[2] - xb) / Sxx),
                     d * (1.0 - 1.0 / 3.0 - (x[2] - xb) * (x[2] - xb) / Sxx),
                };
                fprintf(stderr,
                        "setanalysisselftest: F4 intercept %.9g (exp %.9g)  slope %.12g "
                        "(exp %.12g)  residuals %.7g / %.7g / %.7g (exp %.7g / %.7g / %.7g)\n",
                        F.ok ? F.inter[0] : 0.0, interP, F.ok ? F.slope[0] : 0.0, slopeP,
                        F.ok && F.M == 3 ? F.levels[0].resid[0] : 0.0,
                        F.ok && F.M == 3 ? F.levels[1].resid[0] : 0.0,
                        F.ok && F.M == 3 ? F.levels[2].resid[0] : 0.0,
                        rE[0], rE[1], rE[2]);
                check(F.ok && F.M == 3, "F4 three levels are enough to fit");
                check(F.ok && fabs(F.inter[0] - interP) < 1e-6,
                      "F4 the perturbed intercept matches the influence identity");
                check(F.ok && fabs(F.slope[0] - slopeP) < 1e-12,
                      "F4 ...and so does the perturbed slope");
                bool res = F.ok && F.M == 3;
                for (int i = 0; i < 3 && res; i++)
                    if (fabs(F.levels[i].resid[0] - rE[i]) > 1e-6) res = false;
                check(res, "F4 every level's residual is the identity's, per level");
                check(F.ok && interP < 0 && F.dsnu.clamped[0] && F.dsnu.value[0] == 0.0,
                      "F4 a negative intercept clamps at 0 and DECLARES the clamp");
                check(F.ok && !F.prnu.clamped[0] && F.prnu.value[0] > 0,
                      "F4 ...and the other coefficient is unaffected: they clamp "
                      "independently");
                check(F.ok && F.rms[0] > 100.0 && F.r2[0] < 1.0,
                      "F4 the residual summary is not zero on data that is not the model");
                closeAll();
            }

            {   // ---- F5: the exclusions, counted out loud -------------------
                // Three usable levels plus one of each thing that cannot be
                // one. None of them refuses the fit - they leave it, and the
                // sentence that says so is the one the panel prints.
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                std::vector<SM> ms;
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    ms.push_back({ makeLevel(nm, mu0[i], Bl), 0, mu0[i] / gainDN,
                                   true, FOLD_MEAN });
                }
                ms.push_back({ makeLevel("summed", 2560.0, Bl), 0, 20.0, true, FOLD_SUM });
                ms.push_back({ makeLevel("dropped", 3072.0, Bl), 0, 24.0, false, FOLD_MEAN });
                ms.push_back({ makeLevel("valueless", 3584.0, Bl), 0,
                               std::numeric_limits<double>::quiet_NaN(), true, FOLD_MEAN });
                // a STACK with one frame: min_frames = 2 for the same reason a
                // frame member cannot be a level - no s_t, no correction
                ms.push_back({ makeStack("oneframe", Wf, Hf, 1,
                                         [&](int x, int y, int f) {
                                             return sweepPix(4096.0, Bl, x, y, f);
                                         }),
                               0, 32.0, true, FOLD_MEAN });
                {   // ...and a standalone FRAME member: no time axis at all
                    auto fd = std::make_unique<ImageDoc>();
                    fd->name = "loose.npy";
                    fd->src->w = Wf; fd->src->h = Hf; fd->src->ch = 1;
                    fd->src->dtype = "f32";
                    fd->src->data.assign((size_t)Wf * Hf, 1200.0f);
                    fd->syncMirrors();
                    fd->uid = app.nextUid++;
                    const uint64_t uid = fd->uid;
                    app.images.push_back(std::move(fd));
                    ms.push_back({ 0, uid, 12.0, true, FOLD_MEAN });
                }
                const int dseq = makeDark("dark", NFs);
                const int sid = makeSepSet("census",
                                           makeSeries("mixed", "illuminance", "lx", ms),
                                           dseq);
                SepFit F = runSeparationFit(*aSetById(sid));
                const std::string census = sepLevelCensus(F);
                fprintf(stderr, "setanalysisselftest: F5 %s\n", census.c_str());
                for (const auto& sk : F.skipped)
                    fprintf(stderr, "setanalysisselftest: F5   excluded \"%s\": %s\n",
                            sk.member.c_str(), sk.why.c_str());
                check(F.ok && F.M == 3,
                      "F5 the three usable levels still fit; the rest only leave");
                check(census ==
                          "levels: 3 of 8 sweep member(s) used - excluded: "
                          "1 frame member(s) - no time axis, so no -sigma_t^2/N "
                          "correction, 1 standing as sum - a sum is n x the mean's "
                          "scale, 1 stack(s) with fewer than 2 frames loaded - same "
                          "reason as a frame, 1 not included, 1 with no value set",
                      "F5 every exclusion is counted, with its reason, in one line");
                check(F.skipped.size() == 5,
                      "F5 ...and each excluded member is named individually");
                closeAll();
            }

            {   // ---- F6: the refusals ---------------------------------------
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                std::vector<SM> ms;
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    ms.push_back({ makeLevel(nm, mu0[i], Bl), 0, mu0[i] / gainDN,
                                   true, FOLD_MEAN });
                }
                const int dseq = makeDark("dark", NFs);
                const int ser = makeSeries("lx sweep", "illuminance", "lx", ms);
                // (a) no "sweep" role at all
                {
                    int sid = makeSet("R-nosweep", { { "dark", dseq } });
                    SepFit F = runSeparationFit(*aSetById(sid));
                    fprintf(stderr, "setanalysisselftest: F6a \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "this set has no \"sweep\" role - a role binding is "
                              "declared, never inferred",
                          "F6a no sweep role: refused, and the reason is the rule");
                }
                // (b) a sweep but no dark - mu has nothing to be corrected against
                {
                    App::ASet Ss;
                    Ss.id = app.nextASetId++; Ss.batchId = 1; Ss.name = "R-nodark";
                    App::ASetRole rs; rs.role = "sweep"; rs.seriesId = ser;
                    Ss.roles.push_back(rs);
                    app.analysisSets.push_back(Ss);
                    SepFit F = runSeparationFit(*aSetById(Ss.id));
                    fprintf(stderr, "setanalysisselftest: F6b \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "this set has no \"dark\" role - a role binding is "
                              "declared, never inferred",
                          "F6b no dark role: mu is dark-corrected, so there is no fit");
                }
                // (c) "sweep" bound to a STACK - the schema is Series()
                {
                    int sid = makeSet("R-stack", { { "sweep", dseq }, { "dark", dseq } });
                    SepFit F = runSeparationFit(*aSetById(sid));
                    fprintf(stderr, "setanalysisselftest: F6c \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "\"sweep\" is bound to a stack - this analysis takes a "
                              "Series() role: one stack is ONE level, and separating "
                              "the additive term from the multiplicative one needs 3",
                          "F6c a stack in a Series() role is refused in one line");
                }
                // (d) "sweep" bound to a loose FRAME
                {
                    auto fd = std::make_unique<ImageDoc>();
                    fd->name = "one.npy";
                    fd->src->w = Wf; fd->src->h = Hf; fd->src->ch = 1;
                    fd->src->dtype = "f32";
                    fd->src->data.assign((size_t)Wf * Hf, 100.0f);
                    fd->syncMirrors();
                    fd->uid = app.nextUid++;
                    const uint64_t uid = fd->uid;
                    app.images.push_back(std::move(fd));
                    App::ASet Ss;
                    Ss.id = app.nextASetId++; Ss.batchId = 1; Ss.name = "R-frame";
                    App::ASetRole rs; rs.role = "sweep"; rs.frameUid = uid;
                    Ss.roles.push_back(rs);
                    App::ASetRole rd; rd.role = "dark"; rd.seqId = dseq;
                    Ss.roles.push_back(rd);
                    app.analysisSets.push_back(Ss);
                    SepFit F = runSeparationFit(*aSetById(Ss.id));
                    fprintf(stderr, "setanalysisselftest: F6d \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "\"sweep\" is bound to a frame - this analysis takes a "
                              "Series() role: one frame is ONE level, and separating "
                              "the additive term from the multiplicative one needs 3",
                          "F6d a frame in a Series() role is refused in one line");
                }
                // (e) M < 3: two levels fit any line exactly
                {
                    std::vector<SM> two(ms.begin(), ms.begin() + 2);
                    int sid = makeSepSet("R-two",
                                         makeSeries("short", "illuminance", "lx", two), dseq);
                    SepFit F = runSeparationFit(*aSetById(sid));
                    fprintf(stderr, "setanalysisselftest: F6e \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "the sweep \"short\" has 2 usable level(s) - the "
                              "separation needs 3 (two points fit a line exactly, so "
                              "there is no residual left to check the model with). "
                              "levels: 2 of 2 sweep member(s) used",
                          "F6e fewer than 3 levels is refused, and the census is in "
                          "the same sentence");
                }
                // (f) ...and the census in that sentence explains WHY it is short
                {
                    std::vector<SM> mixed(ms.begin(), ms.begin() + 2);
                    mixed.push_back({ makeLevel("summed", 2560.0, Bl), 0, 20.0,
                                      true, FOLD_SUM });
                    int sid = makeSepSet("R-two-sum",
                                         makeSeries("short2", "illuminance", "lx", mixed),
                                         dseq);
                    SepFit F = runSeparationFit(*aSetById(sid));
                    fprintf(stderr, "setanalysisselftest: F6f \"%s\"\n", F.refusal.c_str());
                    check(!F.ok &&
                          F.refusal.find("levels: 2 of 3 sweep member(s) used - "
                                         "excluded: 1 standing as sum") != std::string::npos,
                          "F6f a sweep that is short BECAUSE of an exclusion says so");
                }
                // (g) planes that do not mean the same thing on both sides
                {
                    int rgb = makeStack("rgb", Wf, Hf, NFs,
                                        [&](int x, int y, int f) {
                                            return sweepPix(1024.0, Bl, x, y, f);
                                        }, 3);
                    std::vector<SM> mm(ms.begin(), ms.begin() + 3);
                    mm.push_back({ rgb, 0, 8.0, true, FOLD_MEAN });
                    int sid = makeSepSet("R-planes",
                                         makeSeries("mixedch", "illuminance", "lx", mm),
                                         dseq);
                    SepFit F = runSeparationFit(*aSetById(sid));
                    fprintf(stderr, "setanalysisselftest: F6g \"%s\"\n", F.refusal.c_str());
                    check(!F.ok && F.refusal ==
                              "sweep member \"rgb\" is 3ch / 1 plane(s) and \"dark\" is "
                              "1ch / 1 plane(s) - mu is a per-plane difference, so the "
                              "planes have to mean the same thing on both sides",
                          "F6g mismatched planes are refused, and BOTH are named");
                }
                closeAll();
            }

            {   // ---- F9: the sweep and the dark are not one shape -----------
                // 確定 2026-08-10 (#57): 「dark, sweep の WxH 不一致は拒否」.
                //
                // This is the case F6g's plane check lets straight through: the
                // member below has the SAME channel count and the SAME plane
                // map as the dark and differs only in W x H - and the fit's
                // arithmetic does not care, because mu takes a per-plane SCALAR
                // off the dark and nothing subtracts pixel from pixel. It would
                // have computed. That is exactly why it needed a ruling rather
                // than a bug report, and why the refusal says so in its own
                // sentence instead of implying the sum could not be formed.
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                std::vector<SM> ms;
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    ms.push_back({ makeLevel(nm, mu0[i], Bl), 0, mu0[i] / gainDN,
                                   true, FOLD_MEAN });
                }
                ms.push_back({ makeStack("half", Wf / 2, Hf / 2, NFs,
                                         [&](int x, int y, int f) {
                                             return sweepPix(2560.0, Bl, x, y, f);
                                         }),
                               0, 20.0, true, FOLD_MEAN });
                const int dseq = makeDark("dark", NFs);
                const int sid = makeSepSet("R-size",
                                           makeSeries("halfsize", "illuminance", "lx", ms),
                                           dseq);
                SepFit F = runSeparationFit(*aSetById(sid));
                fprintf(stderr, "setanalysisselftest: F9 \"%s\"\n", F.refusal.c_str());
                check(!F.ok && F.refusal ==
                          "sweep member \"half\" is 16x16x1ch and \"dark\" is "
                          "32x32x1ch - the sweep and the dark have to be one shape. "
                          "Nothing here subtracts pixel from pixel, so this would "
                          "have computed; a dark that is not the sweep's own size is "
                          "a MIS-BINDING, and a number computed off one reads exactly "
                          "like a number computed off the right dark.",
                      "F9 a WxH mismatch between sweep and dark is refused");
                // PR #121's lesson, as a test rather than as a review comment: a
                // refusal that does not name what it refused is a dead end.
                check(F.refusal.find("16x16x1ch") != std::string::npos &&
                      F.refusal.find("32x32x1ch") != std::string::npos,
                      "F9 ...and BOTH shapes are on the line, not just the fact of "
                      "a mismatch");
                check(!F.dsnu.ok && !F.prnu.ok && F.dsnu.refusal == F.refusal &&
                      F.prnu.refusal == F.refusal,
                      "F9 ...on the fit and on both rows it would have named");
                closeAll();
            }

            {   // ---- F10: the dark ALSO standing as a level -----------------
                // 確定 2026-08-10 (#57): 「dark が sweep のメンバを兼ねるは、
                // そう Analysis Set を作ったら許容」.
                //
                // The dark here IS this sweep's zero: the same construction with
                // mu0 = 0 and the LEVELS' own additive pattern, because a dark
                // that is level 0 of the same detector carries the pattern the
                // levels carry. Two things then follow, and both are asserted
                // rather than described:
                //
                //   mu = mean(M_dark) - mean(M_dark) = 0 EXACTLY. Not "about 0":
                //   the same stack folded twice by the same function gives the
                //   same accumulator, so the subtraction is of a number from
                //   itself. The comparison below is ==.
                //
                //   y  = the dark's own corrected sigma_fpn^2 = (32^2 + 576)*rr
                //        - 576, which is the intercept of F1's line. So the four
                //   points are exactly collinear INCLUDING the one at x = 0, the
                //   fit is F1's line read off one more measurement, and the
                //   anchor's own y is the intercept:
                //        intercept = 1025.56402737   ->  DSNU 32.0244286 DN
                //        slope     = 1/1023          ->  PRNU 3.12652700 %
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                const int dseq = makeLevel("dark", 0.0, Bl);
                std::vector<SM> ms;
                ms.push_back({ dseq, 0, 0.0, true, FOLD_MEAN });   // ...standing at 0 lx
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    ms.push_back({ makeLevel(nm, mu0[i], Bl), 0, mu0[i] / gainDN,
                                   true, FOLD_MEAN });
                }
                const int sid = makeSepSet("anchored",
                                           makeSeries("lx sweep", "illuminance", "lx", ms),
                                           dseq);
                SepFit F = runSeparationFit(*aSetById(sid));
                const double interExp = (Bl * Bl + Cl) * rr - Cl;
                const double slopeExp = qg * qg * rr;
                fprintf(stderr,
                        "setanalysisselftest: F10 M=%d  intercept %.9g (exp %.9g)  "
                        "slope %.12g (exp %.12g)  dsnu %.9g  prnu %.9g%%\n"
                        "setanalysisselftest: F10 anchor: mu %.17g  y %.17g  "
                        "resid %.7g  intercept %.17g\n",
                        F.M, F.ok ? F.inter[0] : 0.0, interExp,
                        F.ok ? F.slope[0] : 0.0, slopeExp,
                        F.ok ? F.dsnu.value[0] : -1.0, F.ok ? F.prnu.value[0] : -1.0,
                        F.ok && F.M == 4 ? F.levels[0].mu[0] : -1.0,
                        F.ok && F.M == 4 ? F.levels[0].fpnVar[0] : -1.0,
                        F.ok && F.M == 4 ? F.levels[0].resid[0] : -1.0,
                        F.ok ? F.inter[0] : 0.0);
                check(F.ok && F.M == 4,
                      "F10 a dark that is also a level is ACCEPTED, and counts as one");
                check(F.ok && F.M == 4 && F.levels[0].mu[0] == 0.0,
                      "F10 ...its mu is EXACTLY 0 - the same fold minus itself");
                check(F.ok && fabs(F.inter[0] - interExp) < 1e-6 &&
                      fabs(F.slope[0] - slopeExp) < 1e-15,
                      "F10 ...and the line is F1's, derived by hand, one point richer");
                bool zeroResid = F.ok && F.M == 4;
                for (const auto& L : F.levels)
                    if (fabs(L.resid[0]) > 1e-6) zeroResid = false;
                check(zeroResid,
                      "F10 ...with the anchor ON the line: no residual anywhere");
                check(F.ok && F.M == 4 &&
                      fabs(F.inter[0] - F.levels[0].fpnVar[0]) < 1e-6,
                      "F10 ...and the intercept IS the anchor's own measurement");
                // ...and that measurement is the direct row's, bit for bit - the
                // F2 idiom, because the anchor is a DSNU by another name.
                if (F.ok && F.M == 4) {
                    int sD = makeSet("as dark", { { "dark", dseq } });
                    SetResult D = runSetAnalyzer(*aSetById(sD), SA_DSNU);
                    fprintf(stderr, "setanalysisselftest: F10 anchor sigma_fpn %.17g  "
                                    "direct %.17g (exp %.9g)\n",
                            sqrt(F.levels[0].fpnVar[0]), D.value[0], sqrt(interExp));
                    check(D.ok && sqrt(F.levels[0].fpnVar[0]) == D.value[0],
                          "F10 ...which is the direct row's own number, bit for bit");
                    check(D.ok && fabs(D.value[0] - sqrt(interExp)) < 1e-9,
                          "F10 ...and both are sqrt(intercept) = 32.0244286 DN");
                }
                // ---- the overlap, SAID -------------------------------------
                fprintf(stderr, "setanalysisselftest: F10 \"%s\"\n",
                        setResultTitle(F.dsnu).c_str());
                fprintf(stderr, "setanalysisselftest: F10 %s\n", F.dualNote.c_str());
                check(F.ok && F.M == 4 && F.levels[0].isDark && !F.levels[1].isDark &&
                      !F.levels[2].isDark && !F.levels[3].isDark,
                      "F10 the level that is the dark is the one marked, and only it");
                check(F.alsoDark.size() == 1 && F.alsoDark[0] == "dark",
                      "F10 ...and the fit names it");
                check(setResultTitle(F.dsnu) ==
                          "DSNU [DN] - set \"anchored\": sweep \"lx sweep\" 4 levels "
                          "(level \"dark\" is also the dark role), dark 5/5",
                      "F10 the TAG says the same stack plays two parts");
                check(setResultTitle(F.prnu) ==
                          "PRNU [%] - set \"anchored\": sweep \"lx sweep\" 4 levels "
                          "(level \"dark\" is also the dark role), dark 5/5",
                      "F10 ...on both named rows, so a quoted number carries it");
                check(F.dualNote.find("ONE STACK, TWO ROLES") != std::string::npos &&
                      F.dualNote.find("\"dark\"") != std::string::npos &&
                      F.dualNote.find("its mu is exactly 0") != std::string::npos &&
                      F.dualNote.find("counted twice") != std::string::npos,
                      "F10 ...and the result explains the double duty in full");
                closeAll();
            }

            {   // ---- F10b: the anchor is a MEASUREMENT, not a free pass -----
                // The same double duty with the dark's OWN pattern (8 DN, not
                // the levels' 32). The anchor is then a real point that does not
                // sit on the levels' line, and the fit says so rather than
                // absorbing it: its y moves by
                //     d = (Bd^2 - Bl^2) * rr = (64 - 1024) * 1024/1023
                // and nothing else does, so the perturbed fit follows from the
                // unperturbed one by the OLS influence identity - a different
                // route to the answer than the implementation's sums, and the
                // same identity F4 uses, applied at x = 0 this time:
                //     slope'  = slope + d*(x0-xb)/Sxx
                //     inter'  = inter + d*(1/4 - xb*(x0-xb)/Sxx)
                //     resid_0 =  d*(1 - 1/4 - (x0-xb)^2/Sxx)
                //     resid_j = -d*(1/4 + (xj-xb)(x0-xb)/Sxx)      j = 1,2,3
                // This is what "allowed" has to mean to be worth allowing: the
                // anchor carries the fit's weight like any other level (the fit
                // is unweighted, 判断5), so a dark that is not the levels' zero
                // pulls the additive term DOWN and leaves the largest residual
                // in the table. It is visible, not silent.
                closeAll();
                const double mu0[3] = { 512.0, 1152.0, 2048.0 };
                const int dseq = makeDark("dark", NFs);        // Bd = 8
                std::vector<SM> ms;
                ms.push_back({ dseq, 0, 0.0, true, FOLD_MEAN });
                char nm[32];
                for (int i = 0; i < 3; i++) {
                    snprintf(nm, sizeof nm, "L%d", i);
                    ms.push_back({ makeLevel(nm, mu0[i], Bl), 0, mu0[i] / gainDN,
                                   true, FOLD_MEAN });
                }
                const int sid = makeSepSet("anchored, other dark",
                                           makeSeries("lx sweep", "illuminance", "lx", ms),
                                           dseq);
                SepFit F = runSeparationFit(*aSetById(sid));
                const double x[4] = { 0.0, mu0[0] * mu0[0], mu0[1] * mu0[1],
                                      mu0[2] * mu0[2] };
                double xb = 0;
                for (int i = 0; i < 4; i++) xb += x[i];
                xb /= 4.0;
                double Sxx = 0;
                for (int i = 0; i < 4; i++) Sxx += (x[i] - xb) * (x[i] - xb);
                const double d = (Bd * Bd - Bl * Bl) * rr;
                const double inter0 = (Bl * Bl + Cl) * rr - Cl;
                const double slope0 = qg * qg * rr;
                const double interP = inter0 + d * (0.25 - xb * (x[0] - xb) / Sxx);
                const double slopeP = slope0 + d * (x[0] - xb) / Sxx;
                double rE[4];
                rE[0] = d * (1.0 - 0.25 - (x[0] - xb) * (x[0] - xb) / Sxx);
                for (int j = 1; j < 4; j++)
                    rE[j] = -d * (0.25 + (x[j] - xb) * (x[0] - xb) / Sxx);
                fprintf(stderr,
                        "setanalysisselftest: F10b intercept %.9g (exp %.9g)  slope "
                        "%.12g (exp %.12g)\n"
                        "setanalysisselftest: F10b residuals %.7g / %.7g / %.7g / %.7g "
                        "(exp %.7g / %.7g / %.7g / %.7g)\n"
                        "setanalysisselftest: F10b dsnu %.9g (the levels alone would "
                        "read %.9g)\n",
                        F.ok ? F.inter[0] : 0.0, interP, F.ok ? F.slope[0] : 0.0, slopeP,
                        F.ok && F.M == 4 ? F.levels[0].resid[0] : 0.0,
                        F.ok && F.M == 4 ? F.levels[1].resid[0] : 0.0,
                        F.ok && F.M == 4 ? F.levels[2].resid[0] : 0.0,
                        F.ok && F.M == 4 ? F.levels[3].resid[0] : 0.0,
                        rE[0], rE[1], rE[2], rE[3],
                        F.ok ? F.dsnu.value[0] : -1.0, sqrt(inter0));
                check(F.ok && F.M == 4, "F10b the double duty still fits, four levels");
                check(F.ok && fabs(F.inter[0] - interP) < 1e-6,
                      "F10b the intercept is the influence identity's, applied at x=0");
                check(F.ok && fabs(F.slope[0] - slopeP) < 1e-12,
                      "F10b ...and so is the slope");
                bool res = F.ok && F.M == 4;
                for (int i = 0; i < 4 && res; i++)
                    if (fabs(F.levels[i].resid[0] - rE[i]) > 1e-6) res = false;
                check(res, "F10b ...and every level's residual, the anchor's included");
                check(F.ok && F.M == 4 && fabs(F.levels[0].resid[0]) > 100.0 &&
                      fabs(F.levels[0].resid[0]) > fabs(F.levels[1].resid[0]) &&
                      fabs(F.levels[0].resid[0]) > fabs(F.levels[2].resid[0]) &&
                      fabs(F.levels[0].resid[0]) > fabs(F.levels[3].resid[0]),
                      "F10b a dark that is not the levels' zero leaves the LARGEST "
                      "residual - it shows, it is not absorbed");
                check(F.ok && F.dsnu.value[0] < 0.85 * sqrt(inter0),
                      "F10b ...and it moves the additive term, which is the anchor "
                      "doing its job rather than being ignored");
                check(F.alsoDark.size() == 1 && F.levels[0].isDark,
                      "F10b ...and the overlap is stated here too");
                closeAll();
            }
        }

        fprintf(stderr, "setanalysisselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
