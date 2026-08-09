// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- --detrend-selftest: the AnalysisPreProcessor stage (#57 判断6/7) -----
    // docs/flat-field-stats.md "シェーディング除去" (確定 2026-08-09). The whole
    // point of judgment 7 is that the DEFAULT DOES NOTHING and says how much it
    // would have done, so the two things this file must not let slip are the
    // default and the figure. What is asserted:
    //
    //   D0  the default is RAW - the spec, the cutoff wording, and the refusal
    //       to mint a product that would only claim to have done something
    //   D1  判断6 mechanically: no General panel has a detrend switch
    //   D2  the shading figure matches the INJECTED profile, exactly, and the
    //       derivation is beside the assert
    //   D3  the raw path is unchanged - the same stack reads the same numbers
    //       before and after a product of it exists, bit for bit
    //   D4  the detrended product's statistics differ in the PREDICTED
    //       direction, to closed-form values
    //   D5  約束3: sigma_t is untouched, and so is the level
    //   D6  provenance - name, note, and the cutoff a SetAnalyzer reports over
    //       the product, naming the method and the window
    //   D7  the figure is printed BESIDE a raw value, which is what makes the
    //       raw default honest rather than silent
    //   D8  the refusals
    //   D9  through a session, as a RECIPE
    //
    // The fixture is analytic and its fixed pattern is ORTHOGONAL BY
    // CONSTRUCTION to the degree-2 basis (see below), so every expected number
    // here is arithmetic rather than a fit - the V27 discipline.
    if (!g_detrendSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "detrendselftest: %-64s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto writeNpy = [](const std::string& path, int w, int h,
                           const std::vector<float>& px) {
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }", h, w);
            std::string hdr = dict;
            size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
            hdr.append(pad, ' ');
            hdr += '\n';
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            uint16_t hl = (uint16_t)hdr.size();
            f.write("\x93NUMPY\x01\x00", 8);
            f.write((const char*)&hl, 2);
            f.write(hdr.data(), (std::streamsize)hdr.size());
            f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        };

        // ================= the fixture =========================================
        // W = H = 64, NF = 4, and every pixel is
        //
        //   px(x,y,f) = S(x,y) + fixed(x,y) + (f odd ? +A : -A)
        //
        //   u = (2x - (W-1)) / (W-1),  v likewise      -> u,v in [-1,1]
        //   S(x,y) = L - (Q/2)(u^2 + v^2)              THE INJECTED SHADING
        //   fixed(x,y) = B * q(x) * q(y)               a fixed pattern
        //   q(x) = +-1 by the parity of popcount(x & 7)
        //
        // q is the Thue-Morse sign pattern over blocks of 8. Its defining
        // property is the one this fixture needs: over each block of 2^n
        // consecutive integers it balances every power sum up to degree n-1, so
        // with n = 3
        //
        //   sum_x q(x) * x^k = 0   for k = 0, 1, 2
        //
        // and therefore sum_x q(x) u^i = 0 for i <= 2, because u is linear in x.
        // The 2-D sums factorise - sum_xy q(x)q(y) u^i v^j = (sum_x q u^i)(sum_y
        // q v^j) - so `fixed` is EXACTLY ORTHOGONAL to every term of the degree-2
        // basis {1, u, v, u^2, uv, v^2}. Three consequences, all load-bearing:
        //
        //   - the degree-2 fit of the temporal mean is S EXACTLY. Not "close":
        //     the normal equations see sum(b * fixed) = 0, so they return S's
        //     own coefficients. The shading figure is therefore predictable in
        //     closed form and not a fit result compared against itself.
        //   - var_spatial(S + fixed) = var(S) + var(fixed) with NO cross term,
        //     so the "before" and "after" statistics are both closed-form.
        //   - a checkerboard would NOT do: sum_x (-1)^x u is not zero, so it
        //     leaks into the uv coefficient and the figure would come out
        //     ~0.06% off with nothing in the test able to say why.
        const int W = 64, H = 64, NF = 4;
        const double L = 1000.0, Q = 300.0, B = 40.0, A = 30.0;
        auto qsign = [](int x) {
            int p = 0;
            for (int b = 0; b < 3; b++) p ^= (x >> b) & 1;
            return p ? -1.0 : 1.0;
        };
        auto uOf = [&](int x) { return (2.0 * x - (W - 1)) / (W - 1); };
        auto vOf = [&](int y) { return (2.0 * y - (H - 1)) / (H - 1); };
        auto surfaceAt = [&](int x, int y) {
            const double u = uOf(x), v = vOf(y);
            return L - 0.5 * Q * (u * u + v * v);
        };
        auto fixedAt = [&](int x, int y) { return B * qsign(x) * qsign(y); };

        // ---- the closed-form expectations, derived once, used below ----------
        const double n = (double)W * H;
        // The p-p of the injected surface OVER THE SAMPLED GRID. Not Q: the
        // maximum of S is at u = v = 0 and W is even, so no sample sits there -
        // the nearest is |u| = 1/(W-1). The minimum IS sampled (the corners are
        // u,v = +-1). So
        //     p-p = [L - (Q/2)(2/(W-1)^2)] - [L - Q] = Q * (1 - 1/(W-1)^2)
        const double ppExpect = Q * (1.0 - 1.0 / ((W - 1.0) * (W - 1.0)));
        // The centre is an EVALUATION of the fitted surface at u = v = 0, which
        // is why it is exactly L whether or not a pixel sits there.
        const double pctExpect = 100.0 * ppExpect / L;
        // var_spatial of the injected surface, ddof = 1, over the same grid the
        // estimator pools - written out here rather than taken off the program.
        double sS1 = 0, sS2 = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) { const double s = surfaceAt(x, y); sS1 += s; sS2 += s * s; }
        const double meanS = sS1 / n;
        const double varS1 = (sS2 - n * meanS * meanS) / (n - 1.0);
        // ...and of the fixed pattern: every sample is +-B about a zero mean.
        const double varF1 = B * B * n / (n - 1.0);
        // The temporal residual the corrected estimator takes off. Per pixel the
        // NF = 4 values are +-A about the pixel's own level, so the deviations
        // are exactly +-A, sum of squares 4A^2, and with ddof = n_i - 1
        //     s_t,i^2 = 4A^2/(NF-1),  C = mean_i(s_t,i^2/n_i) = s_t,i^2/NF
        const double stVar = 4.0 * A * A / (NF - 1.0);
        const double Cexp = stVar / NF;
        // A = 30, NF = 4: s_t^2 = 1200, C = 300, sigma_t = sqrt(1200) = 34.641.
        const double sigmaTExpect = sqrt(stVar);
        // The two numbers this whole PR is about. The +-A cancels EXACTLY over
        // four frames, so the temporal mean is S + fixed and
        //     raw      = sqrt(var(S) + var(fixed) - C)
        //     detrended= sqrt(          var(fixed) - C)
        // B = 40, W = H = 64: var(fixed) = 1600.39, C = 300, so the detrended
        // value is 36.06 DN against a TRUE fixed pattern of 40 - the estimator
        // subtracts C on the assumption that the temporal noise is random, and
        // this fixture's is deterministic and cancels. That gap is a property of
        // the fixture, it is predicted here, and it is the same one PR #130's S1
        // predicts for the same reason.
        const double fpnRawExpect = sqrt(std::max(0.0, varS1 + varF1 - Cexp));
        const double fpnDetExpect = sqrt(std::max(0.0, varF1 - Cexp));
        fprintf(stderr, "detrendselftest: derivation: shading p-p %.9g DN = %.9g%% of "
                        "centre %.9g; var(S)=%.9g var(fixed)=%.9g C=%.9g; "
                        "fixed-pattern sigma raw %.9g -> detrended %.9g DN; "
                        "sigma_t %.9g DN either way\n",
                ppExpect, pctExpect, L, varS1, varF1, Cexp,
                fpnRawExpect, fpnDetExpect, sigmaTExpect);

        std::string root = g_detrendSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/detrend-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        const std::string dF = root + "/flat";
        std::filesystem::create_directories(pathFromUtf8(dF), ec);
        for (int f = 0; f < NF; f++) {
            std::vector<float> px((size_t)W * H);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    px[(size_t)y * W + x] =
                        (float)(surfaceAt(x, y) + fixedAt(x, y) + (f % 2 ? A : -A));
            char nm[64];
            snprintf(nm, sizeof nm, "/flat_%03d.npy", f);
            writeNpy(dF + nm, W, H, px);
        }

        // in-memory set plumbing, the setanalysis selftest's own
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
        auto setById = [&](int id) -> const App::ASet& { return *aSetById(id); };

        {   // ================= D0: the default is RAW ==========================
            // 判断7 in three facts, none of which needs a fixture: the settled
            // default value, the settled words it declares itself with, and the
            // ON default (degree 2, subtractive, per plane) sitting beside it
            // ready but not selected.
            DetrendSpec def;
            fprintf(stderr, "detrendselftest: D0 default method=%d degree=%d block=%d "
                            "divide=%d cutoff=\"%s\"\n", def.method, def.degree, def.block,
                    (int)def.divide, detrendCutoffText(def).c_str());
            check(def.method == DTR_NONE,
                  "D0 the default-constructed spec REMOVES NOTHING (judgment 7)");
            check(detrendCutoffText(def) == "raw (no shading removal)",
                  "D0 ...and says so, in the cutoff field, self-describingly");
            check(def.degree == 2 && !def.divide,
                  "D0 the ON default is degree 2 subtractive - ready, not selected");
            check(kSetCutoffRaw == detrendCutoffText(DetrendSpec{}),
                  "D0 the SetAnalyzer's cutoff wording IS the stage's, not a copy");
        }

        {   // ================= D1: 判断6, mechanically =========================
            // "General パネル (Histogram / Projection / ROIs / Temporal) に
            // detrend オプションは付けない。付ければ同じ列名で別の量が出る".
            // A rule nobody can check is a rule that decays (the famous-name
            // scan's own lesson, PR #130), so this is a scan: the word may not
            // appear in the CODE of a General panel at all. Comments are
            // stripped - a panel is free to explain why it has no such option,
            // and panel_setanalysis.inc does exactly that.
            const char* kGeneral[] = {
                "core/ui/panel_histogram.inc", "core/ui/panel_projection.inc",
                "core/ui/panel_rois.inc", "core/ui/panel_temporal.inc",
            };
            int scanned = 0, hits = 0;
            for (const char* p : kGeneral) {
                std::ifstream in(pathFromUtf8(p), std::ios::binary);
                if (!in) continue;
                std::string src((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
                scanned++;
                std::string code;
                for (size_t i = 0; i < src.size(); i++) {
                    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
                        while (i < src.size() && src[i] != '\n') i++;
                        continue;
                    }
                    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
                        i += 2;
                        while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) i++;
                        i++;
                        continue;
                    }
                    code += (char)tolower((unsigned char)src[i]);
                }
                if (code.find("detrend") != std::string::npos) {
                    hits++;
                    fprintf(stderr, "detrendselftest: D1 VIOLATION %s names detrend in code\n", p);
                }
            }
            fprintf(stderr, "detrendselftest: D1 scanned %d General panel source(s), "
                            "%d violation(s)\n", scanned, hits);
            check(scanned == 4, "D1 the scan reached all four General panels");
            check(hits == 0,
                  "D1 no General panel carries a detrend switch (judgment 6)");
        }

        // ================= open the fixture ====================================
        closeAll();
        app.seqLoadMode = 1;
        openFolder(dF);
        loadAll();
        int srcSeq = 0;
        for (const auto& s : app.seqs) if (!s.dt.made()) { srcSeq = s.id; break; }
        fprintf(stderr, "detrendselftest: opened %d stack(s), %d frame(s)\n",
                (int)app.seqs.size(), (int)app.images.size());
        check(srcSeq != 0 && (int)app.images.size() == NF,
              "the fixture opened as one stack of 4 frames");
        if (srcSeq == 0) {
            fprintf(stderr, "detrendselftest: FAILED\n");
            stopRbWorker(); stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker();
            return 1;
        }

        // the raw reading, taken BEFORE anything is produced
        const int rawSet = makeSet("raw", { { "dark", srcSeq } });
        const SetResult rawBefore = runSetAnalyzer(setById(rawSet), SA_DSNU);
        const StackStats rawStats = computeStackStats(srcSeq, 0, 0, W, H);

        {   // ================= D2: the figure matches the injection ============
            fprintf(stderr, "detrendselftest: D2 shading %.9g%% (exp %.9g%%), p-p %.9g DN "
                            "(exp %.9g), pctOk %d\n",
                    rawBefore.shadePct[0], pctExpect, rawBefore.shadePp[0], ppExpect,
                    (int)rawBefore.shadePctOk[0]);
            // Tolerance: the pixels are float32 (0.5 ulp = 6.0e-5 DN at 1000 DN)
            // and the fit runs on them in double, so the recovered surface can
            // differ from the injected one by a few times that. 1e-3 DN is ~16
            // ulp and 3e-6 of the figure being asserted.
            check(rawBefore.ok, "D2 the raw set runs");
            check(fabs(rawBefore.shadePp[0] - ppExpect) < 1e-3,
                  "D2 shading p-p = Q*(1 - 1/(W-1)^2), the injected profile's own p-p");
            check(fabs(rawBefore.shadePct[0] - pctExpect) < 1e-3,
                  "D2 ...and as a % of the field centre, which is L exactly");
            check(rawBefore.shadePctOk[0],
                  "D2 the ratio is reported because this picture HAS a level");
        }

        {   // ================= D7: the figure is beside a RAW value ============
            // 判断7's mechanism: "raw の PRNU の隣に shading [%] が立っていれば、
            // 「除去すべきだったか」は読み手が判定できる - 既定 OFF が黙認に
            // ならない仕掛けはこれ". So the figure must be present on a result
            // that removed nothing, and the cutoff must say it removed nothing.
            check(rawBefore.cutoff == "raw (no shading removal)",
                  "D7 a result over an untouched stack declares the raw cutoff");
            check(rawBefore.shadePp[0] > 0 && rawBefore.shadePctOk[0],
                  "D7 ...and carries the shading figure anyway - always, not on a flag");
        }

        // ================= produce the product ================================
        DetrendSpec sp;
        sp.method = DTR_POLY;
        sp.degree = 2;                       // 判断7's ON default, spelled out
        std::string err;
        const bool made = detrendStackProduct(srcSeq, sp, err);
        check(made, "the settled default (degree 2, subtractive) produces a product");
        if (!made) fprintf(stderr, "detrendselftest: product refused: %s\n", err.c_str());
        int prodSeq = 0;
        for (const auto& s : app.seqs) if (s.dt.made()) prodSeq = s.id;

        {   // ================= D3: the raw path is UNCHANGED ===================
            // The stage produces; it never edits. The source stack must read the
            // same numbers it read before the product existed - bit for bit,
            // because "the same" here is an identity and not a tolerance.
            const SetResult rawAfter = runSetAnalyzer(setById(rawSet), SA_DSNU);
            const StackStats rawStats2 = computeStackStats(srcSeq, 0, 0, W, H);
            fprintf(stderr, "detrendselftest: D3 source before %.17g after %.17g; "
                            "sigma_t %.17g / %.17g\n", rawBefore.value[0], rawAfter.value[0],
                    rawStats.sigmaT[0], rawStats2.sigmaT[0]);
            check(rawAfter.ok && rawAfter.value[0] == rawBefore.value[0] &&
                  rawAfter.corrDn[0] == rawBefore.corrDn[0],
                  "D3 the source stack reads the SAME value after the product exists");
            check(rawStats2.sigmaT[0] == rawStats.sigmaT[0] &&
                  rawStats2.mean[0] == rawStats.mean[0],
                  "D3 ...and the same sigma_t and level, bit for bit");
            check(rawAfter.cutoff == "raw (no shading removal)",
                  "D3 ...and still declares itself raw - the product is a NEW node");
            check(prodSeq != 0 && prodSeq != srcSeq && (int)app.seqs.size() == 2,
                  "D3 the product is a second stack, beside the source, not in place of it");
        }

        {   // ================= D4: the predicted direction =====================
            const int prodSet = makeSet("detrended", { { "dark", prodSeq } });
            const SetResult P = runSetAnalyzer(setById(prodSet), SA_DSNU);
            fprintf(stderr, "detrendselftest: D4 fixed-pattern sigma raw %.9g (exp %.9g) "
                            "-> detrended %.9g (exp %.9g); shading %.9g%% -> %.9g%%; "
                            "fpn_corr %.9g / %.9g (exp %.9g)\n",
                    rawBefore.value[0], fpnRawExpect, P.ok ? P.value[0] : -1.0, fpnDetExpect,
                    rawBefore.shadePct[0], P.ok ? P.shadePct[0] : -1.0,
                    rawBefore.corrDn[0], P.ok ? P.corrDn[0] : -1.0, sqrt(Cexp));
            check(P.ok, "D4 the product is ordinary data - the same analysis runs on it");
            // 1e-2 DN on values of 36..73 DN: four float32 stores deep, and
            // three orders of magnitude tighter than the effect being asserted.
            check(fabs(rawBefore.value[0] - fpnRawExpect) < 1e-2,
                  "D4 raw reads sqrt(var(S) + var(fixed) - C) - the shading is IN it");
            check(P.ok && fabs(P.value[0] - fpnDetExpect) < 1e-2,
                  "D4 detrended reads sqrt(var(fixed) - C) - only the fixed pattern left");
            check(P.ok && P.value[0] < 0.6 * rawBefore.value[0],
                  "D4 ...which is the PREDICTED DIRECTION: the value falls, by half here");
            check(P.ok && fabs(P.shadePct[0]) < 1e-3,
                  "D4 and the shading the product reports is ~0 - it really went");
            check(P.ok && fabs(P.corrDn[0] - sqrt(Cexp)) < 1e-3,
                  "D4 the temporal correction is unchanged: detrend touched no time axis");
        }

        {   // ================= D5: 約束3 and the level =========================
            // "sigma_t には適用しない ... detrend は空間統計にだけ掛かる段とする".
            // Kept by construction rather than by care: the surface is fitted
            // ONCE on the temporal mean and the SAME per-pixel constant leaves
            // every frame, and a constant cannot move a temporal variance.
            // The residue is float32 rounding of the store - 0.5 ulp = 6.0e-5 DN
            // at 1000 DN, against a sigma_t of 34.6 DN.
            const StackStats P = computeStackStats(prodSeq, 0, 0, W, H);
            fprintf(stderr, "detrendselftest: D5 sigma_t %.17g -> %.17g (exp %.9g); "
                            "level %.17g -> %.17g (exp %.9g)\n",
                    rawStats.sigmaT[0], P.sigmaT[0], sigmaTExpect,
                    rawStats.mean[0], P.mean[0], meanS);
            check(P.valid && fabs(rawStats.sigmaT[0] - sigmaTExpect) < 1e-3,
                  "D5 the fixture's sigma_t is sqrt(4A^2/(NF-1)) as derived");
            check(P.valid && fabs(P.sigmaT[0] - rawStats.sigmaT[0]) < 1e-3,
                  "D5 ...and the product's sigma_t is the SAME (約束3)");
            check(P.valid && fabs(P.mean[0] - rawStats.mean[0]) < 1e-3,
                  "D5 the level survives too - each plane's mean surface is added back");
            check(P.valid && P.frames == NF,
                  "D5 every frame of the source became a frame of the product");
        }

        {   // ================= D6: provenance ==================================
            const App::SeqInfo* ps = seqInfo(prodSeq);
            const std::vector<int> pf = framesOfSeq(prodSeq);
            const ImageDoc* pd = pf.empty() ? nullptr : app.images[pf.front()].get();
            fprintf(stderr, "detrendselftest: D6 stack name \"%s\"\n",
                    ps ? ps->name.c_str() : "");
            if (pd) fprintf(stderr, "detrendselftest: D6 note \"%s\"\n", pd->note.c_str());
            check(ps && ps->name.find("detrended") != std::string::npos &&
                  ps->name.find("poly2 sub") != std::string::npos,
                  "D6 the NAME says it is computed and by which method");
            check(pd && pd->note.find("DETREND PRODUCT") != std::string::npos &&
                  pd->note.find("computed, not captured") != std::string::npos,
                  "D6 the NOTE says it is not a capture (the 0e4d4ec invariant)");
            check(pd && pd->note.find("polynomial surface removed, degree 2, "
                                      "subtractive, per plane") != std::string::npos,
                  "D6 ...and carries the cutoff verbatim: method AND window");
            check(pd && pd->note.find("sigma_t is unchanged") != std::string::npos,
                  "D6 ...and states which statistics it did not touch");
            check(pd && pd->src->path.empty(),
                  "D6 no file behind it - a session cannot hand it back as a capture");
            check(ps && ps->dt.spec.method == DTR_POLY && ps->dt.spec.degree == 2 &&
                  !ps->dt.spec.divide && ps->dt.srcSeqId == srcSeq,
                  "D6 the record on the stack names the method, the window and the source");
            check(ps && fabs(ps->dt.pctBefore[0] - pctExpect) < 1e-3 &&
                  fabs(ps->dt.pctAfter[0]) < 1e-3,
                  "D6 ...and the shading before AND after (約束5), not just after");
            // THE POINT OF THE FIELD (PR #130 put `cutoff` on the result so this
            // stage could arrive without changing what old results claim): a
            // role bound to the product reports the product's band, and nothing
            // on the analysis or the panel had to be asked.
            const int prodSet2 = makeSet("prov", { { "dark", prodSeq } });
            const SetResult P = runSetAnalyzer(setById(prodSet2), SA_DSNU);
            fprintf(stderr, "detrendselftest: D6 cutoff \"%s\"\n", P.cutoff.c_str());
            check(P.ok && P.cutoff.find("polynomial surface removed, degree 2, "
                                        "subtractive, per plane") != std::string::npos,
                  "D6 a SetAnalyzer over the product declares the product's cutoff");
            check(P.ok && ps && P.cutoff.find(ps->name) != std::string::npos,
                  "D6 ...and names the product the band came from");
        }

        {   // ================= D8: the refusals ================================
            std::string e;
            check(!detrendStackProduct(srcSeq, DetrendSpec{}, e) &&
                  e.find("\"raw\", which is what this stack already is") != std::string::npos,
                  "D8 a raw product is refused: raw is a value, not a second copy");
            fprintf(stderr, "detrendselftest: D8a \"%s\"\n", e.c_str());
            check(!detrendStackProduct(prodSeq, sp, e) &&
                  e.find("is already a detrend product") != std::string::npos,
                  "D8 one stage, one cutoff: a product cannot be detrended again");
            fprintf(stderr, "detrendselftest: D8b \"%s\"\n", e.c_str());
            check(!detrendStackProduct(0, sp, e) &&
                  e.find("not open") != std::string::npos,
                  "D8 a stack that is not open is refused in one line");
            DetrendSpec bad = sp;
            bad.degree = 7;
            check(!detrendStackProduct(srcSeq, bad, e) &&
                  e.find("outside 1..4") != std::string::npos,
                  "D8 a degree outside the settled table is refused, and named");
            fprintf(stderr, "detrendselftest: D8c \"%s\"\n", e.c_str());
        }

        {   // ================= D9: through a session, as a RECIPE ==============
            // docs/analysis-layers.md §5: "ファイルは持たず、セッションには
            // レシピを書いて再計算する". The pixels cannot be saved and must not
            // come back as captures wearing this name.
            const std::string sess = root + "/detrend.vsession";
            saveSession(sess, true);
            std::string txt;
            {
                std::vector<uint8_t> b;
                if (readFileBytes(sess, b)) txt.assign((const char*)b.data(), b.size());
            }
            check(txt.find("detrend 1 2 64 0 ") != std::string::npos,
                  "D9 the session writes the SPEC and the source path, not the pixels");
            check(txt.find("detrendname ") != std::string::npos,
                  "D9 ...and the node's name, which the user owns");
            const std::string lerr = loadSession(sess);
            loadAll();
            for (int i = 0; i < 400 && !app.dtRestore.empty(); i++) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int prods = 0, back = 0;
            for (const auto& s : app.seqs) if (s.dt.made()) { prods++; back = s.id; }
            fprintf(stderr, "detrendselftest: D9 load='%s', %d product(s), %d stack(s), "
                            "%zu image(s)\n", lerr.c_str(), prods, (int)app.seqs.size(),
                    app.images.size());
            check(lerr.empty() && prods == 1 && (int)app.seqs.size() == 2,
                  "D9 exactly one product comes back, beside its source");
            const App::SeqInfo* bs = back ? seqInfo(back) : nullptr;
            check(bs && bs->dt.spec.method == DTR_POLY && bs->dt.spec.degree == 2 &&
                  !bs->dt.spec.divide,
                  "D9 ...recomputed with the SAME band, not a default one");
            if (back) {
                const int s2 = makeSet("after session", { { "dark", back } });
                const SetResult P = runSetAnalyzer(setById(s2), SA_DSNU);
                fprintf(stderr, "detrendselftest: D9 restored value %.9g (exp %.9g), "
                                "cutoff \"%s\"\n", P.ok ? P.value[0] : -1.0, fpnDetExpect,
                        P.ok ? P.cutoff.c_str() : "");
                check(P.ok && fabs(P.value[0] - fpnDetExpect) < 1e-2,
                      "D9 ...and reads the same number it read before the session");
                check(P.ok && P.cutoff.find("degree 2") != std::string::npos,
                      "D9 ...and still declares the band it was measured over");
            }
            int loose = 0;
            for (const auto& d : app.images) if (d->seqId == 0) loose++;
            check(loose == 0,
                  "D9 nothing came back as a loose capture wearing the product's name");
        }

        closeAll();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "detrendselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
