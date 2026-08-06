// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // Linearity, verifiable without a human: load everything, fit, print the
    // numbers. A synthetic set with a known sensitivity and a known gain has to
    // come back out of this, or the panel is showing decoration.
    if (g_linSelftest) {
        double t0 = nowSec();
        while (nowSec() - t0 < 600.0) {       // wall clock: the loader is a thread
            // headless "Load selected": Open Folder ALWAYS shows the picker,
            // so a selftest accepts it the way the Load button would
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // The fit is per SERIES now, and the application never creates one by
        // itself (docs/terminology.md) - so this selftest creates it, in the
        // open, through selftestMakeSeries: one series per BATCH over that
        // batch's stacks, member values from extractLevelFromName (the same
        // proposal the Create modal shows a human), unit = the prefill
        // (prefs linunit, "lx" unless the user changed it). That is the ONLY
        // auto-creation in the program and it lives behind this flag.
        bool fitOk = false, snrOk = true;
        std::vector<int> bids;
        for (const auto& b : app.batches) bids.push_back(b.id);
        for (int bid : bids) {
            int sid = selftestMakeSeries(bid, app.lin.unit);
            if (!sid) continue;
            linRecompute(sid);
            const App::LinState& L = app.lin;
            const App::Series* S = seriesById(sid);
            fprintf(stderr, "linselftest: series '%s' (batch '%s') unit '%s': %d member(s)\n",
                    S->name.c_str(), batchNameOf(bid).c_str(), S->unit,
                    (int)S->members.size());
            for (const auto& r : L.rows)
                fprintf(stderr, "linselftest: stack '%s' level=%.6g frames=%d planes=%d "
                                "mean=%.6g sigma_t=%.6g %s\n",
                        r.name.c_str(), r.level, r.frames, r.nPl, r.mean[0], r.sigmaT[0],
                        r.valid ? "" : ("ERR:" + r.err).c_str());
            for (int p = 0; p < L.nPl; p++)
                fprintf(stderr, "linselftest: plane %s sens=%.6g offs=%.6g r2=%.8f "
                                "LEmax=%.4f K=%.6g read=%.5g\n",
                        L.nPl > 1 ? LIN_PLANES[p] : "all", L.slope[p], L.offs[p], L.r2[p],
                        L.leMax[p], L.ptcK[p], L.readDN[p]);
            fprintf(stderr, "linselftest: %d points, fit %s\n", L.nPts,
                    L.fitValid ? "ok" : "FAILED");
            fitOk |= L.fitValid;

            // ---- SNR curve, through the panel's own linSnrPoints (same rules,
            // same code path - the selftest re-derives NOTHING) ---------------
            if (L.fitValid) {
                std::vector<LinSnrPt> sp;
                bool fromDark = false;
                int droppedPts = 0;
                linSnrPoints(L, sp, &fromDark, &droppedPts);
                auto rowOf = [&](int seqId) -> const App::LinState::Row* {
                    for (const auto& rr : L.rows) if (rr.seqId == seqId) return &rr;
                    return nullptr;
                };
                int perPl[4] = {};
                for (const auto& q : sp) {
                    const App::LinState::Row* r = rowOf(q.seqId);
                    if (q.plane >= 0 && q.plane < 4) perPl[q.plane]++;
                    fprintf(stderr, "linselftest: snr %-3s level=%.6g signal=%.6g snr=%.6g\n",
                            L.nPl > 1 ? LIN_PLANES[q.plane] : "all", r ? r->level : -1.0,
                            q.x, q.snr);
                    // a point must map to an included, measured, levelled row -
                    // the fit's own inclusion, never a wider one
                    if (!r || !r->include || !r->valid || !std::isfinite(r->level)) {
                        fprintf(stderr, "linselftest: snr point outside the fit's rows FAILED\n");
                        snrOk = false;
                    }
                }
                fprintf(stderr, "linselftest: snr %d point(s) (%d/%d/%d/%d per plane), "
                                "dark %s, %d dropped\n", (int)sp.size(), perPl[0], perPl[1],
                        perPl[2], perPl[3], fromDark ? "measured" : "extrapolated", droppedPts);
                // one dark rule for the whole panel: the curve subtracts a
                // MEASURED dark exactly when the read noise says readFromDark
                if (fromDark != L.readFromDark) {
                    fprintf(stderr, "linselftest: snr dark rule disagrees with "
                                    "readFromDark FAILED\n");
                    snrOk = false;
                }
                // ---- analytic ground truth of the linset fixture ------------
                // gen_lin.py injects, per RGGB plane p: mean = SENS[p]*lv + 64,
                // sigma_t^2 = 2.0*SENS[p]*lv + 3^2, 24 frames per level, dark at
                // lv 0. So the curve must land on SNR = S/sqrt(2S+9), S=SENS[p]*lv.
                // Tolerance: the plane sigma_t averages 1024 per-pixel variances
                // of N=24 samples -> relative sd ~ sqrt(2/(N-1))/(2*sqrt(1024))
                // ~ 0.46%; the worst realized deviation on this fixture measures
                // 1.02% (~2.2 sigma). 3% is ~6.5 sigma - not a false-failure
                // budget, while sigma_fpn in sigma_t's place (~x4.9) or a skipped
                // dark correction (+107% at 10 lx) land far outside. The signal
                // check is tighter (2%): plane means realize to ~0.05%.
                static const double GT_SENS[4] = { 8.0, 10.0, 10.0, 6.0 };
                static const double GT_LV[7] = { 0, 10, 20, 40, 80, 160, 320 };
                bool isLinset = L.nPl == 4 && L.rows.size() == 7 &&
                                S && std::string(S->unit) == "lx";
                if (isLinset) {
                    std::vector<double> lvs;
                    for (const auto& r : L.rows) {
                        if (!std::isfinite(r.level) || r.frames != 24) { isLinset = false; break; }
                        lvs.push_back(r.level);
                    }
                    std::sort(lvs.begin(), lvs.end());
                    for (int i = 0; isLinset && i < 7; i++) isLinset = lvs[i] == GT_LV[i];
                }
                if (isLinset) {
                    bool aOk = true;
                    if (!fromDark || sp.size() != 24 || perPl[0] != 6 || perPl[1] != 6 ||
                        perPl[2] != 6 || perPl[3] != 6) {
                        fprintf(stderr, "linselftest: snr expected 4 separate plane curves "
                                        "of 6 dark-corrected points FAILED\n");
                        aOk = false;
                    }
                    double worstX = 0, worstS = 0;
                    for (const auto& q : sp) {
                        const App::LinState::Row* r = rowOf(q.seqId);
                        if (!r) continue;
                        double Sg = GT_SENS[q.plane] * r->level;
                        double expSnr = Sg / sqrt(2.0 * Sg + 9.0);
                        double ex = fabs(q.x / Sg - 1.0), es = fabs(q.snr / expSnr - 1.0);
                        worstX = std::max(worstX, ex);
                        worstS = std::max(worstS, es);
                        if (ex > 0.02 || es > 0.03) {
                            fprintf(stderr, "linselftest: snr %s lv %.6g: signal %.6g "
                                            "(expect %.6g), snr %.6g (expect %.6g) FAILED\n",
                                    LIN_PLANES[q.plane], r->level, q.x, Sg, q.snr, expSnr);
                            aOk = false;
                        }
                    }
                    fprintf(stderr, "linselftest: snr analytic check (linset): worst "
                                    "|signal err| %.3f%%, worst |SNR err| %.3f%% "
                                    "(tol 2%% / 3%%) %s\n", worstX * 100, worstS * 100,
                            aOk ? "ok" : "FAILED");
                    if (!aOk) snrOk = false;
                } else {
                    fprintf(stderr, "linselftest: snr analytic check skipped "
                                    "(not the linset signature)\n");
                }
            }
        }
        if (!fitOk) fprintf(stderr, "linselftest: no series could be fitted\n");
        if (!snrOk) fprintf(stderr, "linselftest: SNR curve FAILED\n");
        if (app.seqThread.joinable()) app.seqThread.join();
        return (fitOk && snrOk) ? 0 : 1;
    }
