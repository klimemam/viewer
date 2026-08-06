// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // Frame-wise linearity (the Temporal panel's interim section), verifiable
    // without a human. Synthetic stacks whose frames have EXACT means - the
    // PIXELS are written and the real measurement path runs over them - with:
    // an analytic line to recover, a deliberate +2% bend to catch (sign and
    // magnitude), off-the-line frames pushed out of the window that MUST be
    // excluded from the fit yet flagged in the export, per-plane separation on
    // a CFA fixture, BOTH fit methods (agreeing on linear data, disagreeing in
    // the predicted direction on bent data), the export section, and the
    // panel's on-screen state through real ImGui frames (this machine cannot
    // screenshot GL - assertions are on state and strings).
    if (g_frameLinSelftest) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "framelinselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        const int W = 32, H = 32, NF = 12;
        const double XV[NF] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
        const char* XSTR = "10 20 30 40 50 60 70 80 90 100 110 120";
        // a numbered stack with pixels from a callback; the axis goes through
        // the REAL applyFrameAxis, the means through the real collector
        auto mkStack = [&](const char* name, bool cfa,
                           const std::function<float(int f, int x, int y, int pl)>& px) {
            closeAll();
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = name;
            app.seqs.push_back(si);
            for (int f = 0; f < NF; f++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = std::string(name) + "_" + std::to_string(f) + ".npy";
                d->src->w = W; d->src->h = H; d->src->ch = 1;
                d->src->dtype = "f32";
                if (cfa) { d->cfa = 1; d->cfaPattern = 0; }          // RGGB
                d->src->data.resize((size_t)W * H);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        int pl = cfa ? CFA_MAP[0][(y & 1) * 2 + (x & 1)] : 0;
                        d->px()[(size_t)y * W + x] = px(f, x, y, pl);
                    }
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->seqId = si.id;
                d->seqIndex = f;
                app.images.push_back(std::move(d));
            }
            app.current = (int)app.images.size() - NF;
            app.selectedAnn = 0;
            std::string err;
            if (!applyFrameAxis(si.id, "exposure", "ms", XSTR, err))
                fprintf(stderr, "framelinselftest: applyFrameAxis FAILED: %s\n", err.c_str());
            return si.id;
        };
        auto axOf = [&](int sid) { return frameAxisOf(sid); };
        const double A_GT = 8.0, B_GT = 64.0;      // the injected line: mean = 8x + 64

        // ---- F1: manual window, exact line, off-line frames OUT of window --
        // frame 0 (x=10): 100 DN - off the line (144) AND below the window;
        // frame 11 (x=120): 950 DN - off the line (1024) AND above the window.
        // If either leaks into the fit, the slope cannot come back as 8.
        int sid = mkStack("flin_manual", false, [&](int f, int, int, int) {
            if (f == 0) return 100.0f;
            if (f == 11) return 950.0f;
            return (float)(A_GT * XV[f] + B_GT);
        });
        g_frameLin = FrameLinCfg{};
        g_frameLin.manual = true;
        g_frameLin.ylo = 150; g_frameLin.yhi = 900;
        {
            FrameAxis ax = axOf(sid);
            check(ax.custom && ax.label == "exposure (ms)", "F1 axis applied and resolvable");
            FrameLinData D = frameLinCollect(sid, ax);
            check(D.nPl == 1 && (int)D.pts.size() == NF && D.resident == NF &&
                  D.expected == NF && D.nonFinite == 0 && !D.roiUsed,
                  "F1 collector: 12 exact frames, whole-frame region");
            FrameLinFit F0 = frameLinFitPlane(D, 0, 0, true, 150, 900);
            FrameLinFit F1r = frameLinFitPlane(D, 0, 1, true, 150, 900);
            fprintf(stderr, "framelinselftest: F1 OLS   a=%.12g b=%.12g r2=%.12f "
                            "LE+=%.6g LE-=%.6g nWin=%d nIn=%d\n",
                    F0.a, F0.b, F0.r2, F0.leMaxPos, F0.leMaxNeg, F0.nWin, F0.nIn);
            fprintf(stderr, "framelinselftest: F1 rev4  a=%.12g b=%.12g r2=%.12f "
                            "LE+=%.6g LE-=%.6g\n",
                    F1r.a, F1r.b, F1r.r2, F1r.leMaxPos, F1r.leMaxNeg);
            check(F0.valid && F0.nWin == 9 && F0.nIn == 9,
                  "F1 window keeps exactly the 9 on-line points");
            check(fabs(F0.a - A_GT) < 1e-9 && fabs(F0.b - B_GT) < 1e-7,
                  "F1 OLS recovers a=8, b=64 (off-line frames excluded)");
            check(F0.r2 > 0.999999999, "F1 R^2 = 1 on exact data");
            check(F0.leMaxPos < 1e-9 && F0.leMaxNeg > -1e-9,
                  "F1 LE ~ 0 on exact data (LE is against the FIT)");
            check(F1r.valid && fabs(F1r.a - A_GT) < 1e-6 && fabs(F1r.b - B_GT) < 1e-4,
                  "F1 rev4-style recovers the same line on linear data");
            check(fabs(F0.a - F1r.a) < 1e-6 && fabs(F0.b - F1r.b) < 1e-4,
                  "F1 the two methods agree on linear data");
            // the fixture has the POWER to catch inclusion: fitting all 12
            // points (window ignored) must give a visibly wrong slope
            std::vector<double> xa, ya;
            for (const auto& q : D.pts) { xa.push_back(q.x); ya.push_back(q.mean[0]); }
            double aBad, bBad, r2Bad;
            linFit(xa, ya, aBad, bBad, r2Bad);
            fprintf(stderr, "framelinselftest: F1 if out-of-window points were "
                            "included: a=%.6g (truth 8)\n", aBad);
            check(fabs(aBad - A_GT) > 0.05,
                  "F1 fixture power: including excluded points bends the slope");
        }

        // ---- F2: a +2% bend on the CENTER in-window point (x=60) -----------
        // x=60 is the in-window mean of x, so OLS keeps a=8 and absorbs 1/9 of
        // the bump into b: b = 64 + 10.88/9 = 65.2089, LE+ at frame 5 =
        // (554.88 - 545.209)/545.209 = +1.774%, worst negative at frame 1
        // (-1.2089/225.209 = -0.537%). The 2% bend is caught with its sign;
        // the fit itself absorbs the rest - that is what LE against a fitted
        // line MEANS, and the numbers say so exactly.
        for (auto& d : app.images)
            if (d->seqId == sid && d->seqIndex == 5)
                for (auto& v : d->px()) v *= 1.02f;
        double le5 = 0, mean5 = 0;
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit F0 = frameLinFitPlane(D, 0, 0, true, 150, 900);
            mean5 = D.pts[5].mean[0];
            le5 = 100.0 * (mean5 - (F0.a * 60 + F0.b)) / (F0.a * 60 + F0.b);
            fprintf(stderr, "framelinselftest: F2 bent frame 5: mean=%.6f a=%.9g "
                            "b=%.9g r2=%.9f LE+=%.4f%% (frame %d) LE-=%.4f%% (frame %d)\n",
                    mean5, F0.a, F0.b, F0.r2, F0.leMaxPos, F0.framePos,
                    F0.leMaxNeg, F0.frameNeg);
            check(F0.valid && fabs(F0.a - 8.0) < 1e-6 && fabs(F0.b - 65.2089) < 1e-3,
                  "F2 center bend: slope kept, 1/9 of the bump lands in b");
            check(F0.leMaxPos > 1.70 && F0.leMaxPos < 1.85 && F0.framePos == 5,
                  "F2 the bent frame is caught: LE_max+ ~ +1.77% AT frame 5");
            check(F0.leMaxNeg < -0.45 && F0.leMaxNeg > -0.60 && F0.frameNeg == 1,
                  "F2 the induced negative extreme lands on frame 1");
            check(F0.r2 > 0.9995 && F0.r2 < 0.99999, "F2 R^2 reflects the bend");
        }

        // ---- F3: the export section carries all of it ----------------------
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit F0 = frameLinFitPlane(D, 0, g_frameLin.method,
                                              g_frameLin.manual, g_frameLin.ylo,
                                              g_frameLin.yhi);
            std::string tsv = buildTemporalExport('\t');
            auto has = [&](const std::string& w) { return tsv.find(w) != std::string::npos; };
            check(has("== frame linearity"), "F3 export: the section exists");
            check(has("method: windowed OLS (3.1-style)"), "F3 export: method stated");
            check(has("NOT an EMVA 1288 compliance claim"),
                  "F3 export: no compliance claim, said in so many words");
            check(has("fit window: manual [150, 900] DN"), "F3 export: window stated");
            check(has("region: whole frame (32x32)") && has("x: exposure [ms]"),
                  "F3 export: region and axis stated");
            check(has("resident 12 of 12 frame(s)"), "F3 export: n of N");
            check(has("offset b is reported, not subtracted"),
                  "F3 export: the no-dark-subtraction honesty line");
            std::string srow = std::string("\nall\t150\t900\t9\t") + expNum(F0.a) +
                               "\t" + expNum(F0.b) + "\t" + expNum(F0.r2) + "\t" +
                               expNum(F0.leMaxPos) + "\t" + expNum(F0.leMaxNeg) + "\n";
            check(has(srow), "F3 export: summary row is the fit struct verbatim");
            double fit0 = F0.a * 10 + F0.b;
            std::string r0 = std::string("\n0\t10\t100\t") +
                             expNum(100.0 * (100.0 - fit0) / fit0) + "\t0\n";
            check(has(r0), "F3 export: out-of-window frame present, flagged 0");
            std::string r5 = std::string("\n5\t60\t") + expNum(mean5) + "\t" +
                             expNum(le5) + "\t1\n";
            check(has(r5), "F3 export: the bent frame's row carries its LE, flagged 1");
        }

        // ---- F4: the two methods disagree in the PREDICTED direction -------
        // Bend the TOP in-window point (x=100: 864 -> 881.28, still < 900).
        // OLS lets the bright point pull the whole fit (absolute residuals);
        // the relative-weighted rev4-style fit stays with the low points, so
        // the bright-end bend keeps MORE of its deviation: LE+ rev4 > LE+ OLS.
        sid = mkStack("flin_topbend", false, [&](int f, int, int, int) {
            if (f == 0) return 100.0f;
            if (f == 11) return 950.0f;
            float v = (float)(A_GT * XV[f] + B_GT);
            return f == 9 ? v * 1.02f : v;
        });
        g_frameLin.manual = true; g_frameLin.ylo = 150; g_frameLin.yhi = 900;
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit Fo = frameLinFitPlane(D, 0, 0, true, 150, 900);
            FrameLinFit Fr = frameLinFitPlane(D, 0, 1, true, 150, 900);
            fprintf(stderr, "framelinselftest: F4 top bend: OLS LE+=%.4f%% (frame %d)  "
                            "rev4 LE+=%.4f%% (frame %d)\n",
                    Fo.leMaxPos, Fo.framePos, Fr.leMaxPos, Fr.framePos);
            check(Fo.valid && Fr.valid && Fo.framePos == 9 && Fr.framePos == 9,
                  "F4 both methods put LE_max+ on the bent frame");
            check(Fo.leMaxPos > 0.8 && Fo.leMaxPos < 2.2 &&
                  Fr.leMaxPos > 0.8 && Fr.leMaxPos < 2.2,
                  "F4 both magnitudes are in the bend's ballpark");
            check(Fr.leMaxPos > Fo.leMaxPos,
                  "F4 rev4-style shows the LARGER |LE| for a bright-end bend");
        }

        // ---- F5: AUTO window = 5..95% of the plane's max observed mean -----
        sid = mkStack("flin_auto", false, [&](int f, int, int, int) {
            return (float)(A_GT * XV[f] + B_GT);       // all 12 exactly on the line
        });
        g_frameLin = FrameLinCfg{};                    // back to auto
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit F0 = frameLinFitPlane(D, 0, 0, false, 0, 0);
            fprintf(stderr, "framelinselftest: F5 auto window [%.6g, %.6g] nWin=%d "
                            "a=%.9g b=%.9g\n", F0.ylo, F0.yhi, F0.nWin, F0.a, F0.b);
            check(fabs(F0.ylo - 51.2) < 1e-9 && fabs(F0.yhi - 972.8) < 1e-9,
                  "F5 auto window = [51.2, 972.8] from max mean 1024");
            check(F0.nWin == 11 && F0.valid && fabs(F0.a - A_GT) < 1e-9 &&
                  fabs(F0.b - B_GT) < 1e-7,
                  "F5 the 1024 DN frame is out, the other 11 recover the line");
            check(frameLinWindowLabel(false, 0, 0).find("auto = 5%..95%") !=
                  std::string::npos, "F5 the shared window label says AUTO");
            // ...and the export flags exactly that frame out
            std::string tsv = buildTemporalExport('\t');
            check(tsv.find("\n11\t120\t1024\t") != std::string::npos &&
                  tsv.find(std::string("\n11\t120\t1024\t") + expNum(0.0) + "\t0\n") !=
                  std::string::npos,
                  "F5 export: the auto-excluded frame is flagged 0 (LE 0 on the line)");
        }

        // ---- F6: refusal below 3 in-window points, refusal said ------------
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit F0 = frameLinFitPlane(D, 0, 0, true, 300, 400);
            check(!F0.valid && F0.nWin == 2 && F0.nIn == 2,
                  "F6 2 in-window points: fit refused (2 points fit any line)");
            g_frameLin.manual = true; g_frameLin.ylo = 300; g_frameLin.yhi = 400;
            std::string tsv = buildTemporalExport('\t');
            check(tsv.find("fit refused - 2 in-window point(s), 3 required") !=
                  std::string::npos, "F6 export: the refusal is stated, not blank");
            check(tsv.find("\nall\t300\t400\t2\t-\t-\t-\t-\t-\n") != std::string::npos,
                  "F6 export: the refused plane's row holds '-', not numbers");
            g_frameLin = FrameLinCfg{};
        }

        // ---- F7: non-finite samples are excluded AND counted ---------------
        for (auto& d : app.images)
            if (d->seqId == sid && d->seqIndex == 2)
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W / 2; x++)         // half of frame 2
                        d->px()[(size_t)y * W + x] = std::numeric_limits<float>::quiet_NaN();
        {
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            FrameLinFit F0 = frameLinFitPlane(D, 0, 0, false, 0, 0);
            fprintf(stderr, "framelinselftest: F7 nonFinite=%llu mean2=%.6g\n",
                    (unsigned long long)D.nonFinite, D.pts[2].mean[0]);
            check(D.nonFinite == (uint64_t)(W / 2) * H,
                  "F7 512 NaN samples counted as excluded");
            check(fabs(D.pts[2].mean[0] - (A_GT * 30 + B_GT)) < 1e-6 &&
                  F0.valid && fabs(F0.a - A_GT) < 1e-9,
                  "F7 the finite half still measures the exact mean");
        }

        // ---- F8: per-plane separation on a CFA fixture ---------------------
        // Four planes, four different slopes; each must come back on ITS row,
        // with ITS auto window - a pooled fit would return ~8.5 everywhere.
        {
            static const double SENS[4] = { 8.0, 10.0, 10.0, 6.0 };
            sid = mkStack("flin_cfa", true, [&](int f, int, int, int pl) {
                return (float)(SENS[pl] * XV[f] + B_GT);
            });
            FrameLinData D = frameLinCollect(sid, axOf(sid));
            check(D.nPl == 4, "F8 a mosaiced stack fits four planes");
            bool aOk = true, wOk = true;
            FrameLinFit FP[4];
            for (int p = 0; p < 4; p++) {
                FP[p] = frameLinFitPlane(D, p, 0, false, 0, 0);
                FrameLinFit Fr = frameLinFitPlane(D, p, 1, false, 0, 0);
                fprintf(stderr, "framelinselftest: F8 plane %-2s a=%.9g (truth %g) "
                                "b=%.9g window [%.6g, %.6g] rev4 a=%.9g\n",
                        LIN_PLANES[p], FP[p].a, SENS[p], FP[p].b, FP[p].ylo,
                        FP[p].yhi, Fr.a);
                aOk &= FP[p].valid && fabs(FP[p].a - SENS[p]) < 1e-6 &&
                       fabs(FP[p].b - B_GT) < 1e-4 &&
                       Fr.valid && fabs(Fr.a - SENS[p]) < 1e-6;
                wOk &= fabs(FP[p].yhi - 0.95 * (SENS[p] * 120 + B_GT)) < 1e-9;
            }
            check(aOk, "F8 every plane recovers ITS slope, both methods");
            check(wOk, "F8 every plane gets ITS OWN auto window");
        }

        // ---- F9: the ROI convention - the region rule the panel already uses
        {
            sid = mkStack("flin_roi", false, [&](int f, int x, int, int) {
                double v = A_GT * XV[f] + B_GT;
                return (float)(x < W / 2 ? v : v + 1000.0);   // right half: garbage
            });
            App::Ann a;
            a.id = app.nextAnnId++;
            a.type = 0;
            a.x = 0; a.y = 0; a.w = W / 2; a.h = H;
            a.label = "roi";
            app.anns.push_back(a);
            app.selectedAnn = a.id;
            app.annRev++;
            FrameLinData Dr = frameLinCollect(sid, axOf(sid));
            FrameLinFit Fr = frameLinFitPlane(Dr, 0, 0, false, 0, 0);
            app.anns.clear(); app.selectedAnn = 0; app.annRev++;
            FrameLinData Dw = frameLinCollect(sid, axOf(sid));
            FrameLinFit Fw = frameLinFitPlane(Dw, 0, 0, false, 0, 0);
            fprintf(stderr, "framelinselftest: F9 ROI b=%.9g whole b=%.9g\n", Fr.b, Fw.b);
            check(Dr.roiUsed && Dr.rw == W / 2 && Fr.valid &&
                  fabs(Fr.a - A_GT) < 1e-9 && fabs(Fr.b - B_GT) < 1e-6,
                  "F9 selected ROI: means come from the ROI only");
            check(!Dw.roiUsed && Fw.valid && fabs(Fw.b - (B_GT + 500.0)) < 1e-6,
                  "F9 no ROI: whole frame (the +1000 half shifts b by 500)");
        }

        // ---- F10: no axis -> the section refuses, the export stays silent --
        {
            if (App::SeqInfo* si = seqInfo(sid)) {
                si->axisName.clear(); si->axisUnit.clear(); si->axisVals.clear();
            }
            std::string tsv = buildTemporalExport('\t');
            check(tsv.find("frame linearity") == std::string::npos,
                  "F10 without an axis the export has NO linearity section");
        }

        // ---- F11: the panel section itself, through real ImGui frames ------
        {
            if (!needWindow("--frame-lin-selftest (F11, the panel section)")) return 1;
            auto temporalFrame = [&](float w, float h) {
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                    ImGui::Begin("FrameLinProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    drawPanelTemporal();
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            // no axis (F10 cleared it): the OPEN section shows the one line
            // that says what to do, and nothing else
            g_frameLin.open = true;
            temporalFrame(560.0f, 700.0f);
            check(g_flin.state == 1, "F11 no axis: the section says 'set the x axis first'");
            std::string err;
            check(applyFrameAxis(sid, "exposure", "ms", XSTR, err),
                  "F11 fixture: axis re-applied");
            // 900 px: tall enough for the fixed content + two floored plots at
            // any plausible display scale - a SHORTER panel scrolls by design
            // (drawPanelLinearity's precedent), which is not this assertion
            temporalFrame(560.0f, 900.0f);
            fprintf(stderr, "framelinselftest: F11 open: state=%d nPl=%d nPts=%d "
                            "nIn0=%d plots=%d/%d bot=%.1f win=%.1f avail=%.1f "
                            "half=%.1f uiScale=%.2f  '%s'\n",
                    g_flin.state, g_flin.nPl, g_flin.nPts, g_flin.nIn[0],
                    g_flin.plotMean ? 1 : 0, g_flin.plotResid ? 1 : 0,
                    g_flin.plotBot, g_flin.winBot, g_flin.availBefore,
                    g_flin.half, app.uiScale, g_flin.window);
            check(g_flin.state == 3 && g_flin.nPl == 1 && g_flin.nPts == NF,
                  "F11 open section: fits drawn from 12 frames");
            check(g_flin.plotMean && g_flin.plotResid &&
                  g_flin.plotBot <= g_flin.winBot + 1.0f,
                  "F11 both plots laid out INSIDE the panel");
            check(std::string(g_flin.window) ==
                  frameLinWindowLabel(g_frameLin.manual, g_frameLin.ylo, g_frameLin.yhi),
                  "F11 the on-screen window label is the shared string");
            // collapsed: the pooled per-frame chart is back
            g_frameLin.open = false;
            temporalFrame(560.0f, 700.0f);
            check(g_flin.state == 0 && g_tchart.plotted,
                  "F11 collapsed: the pooled per-frame chart returns");
        }

        fprintf(stderr, "framelinselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
        return ok ? 0 : 1;
    }
