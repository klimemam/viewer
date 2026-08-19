// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // Browser-fired temporal, verifiable without a human (docs/terminology.md):
    // the server aggregate for a stack NOBODY OPENED must land in the Temporal
    // panel as a detached [not opened] result and match an independent local
    // computation to full float64 precision.
    //
    // ...and then P1/P2 (#57 item 3): the SAME stack measured the other way -
    // opened, through the Temporal panel's own recomputeTemporalIfNeeded - must
    // give the same sigma_t. "local" and "server" are a transport; sigma_t is
    // one quantity, named once in docs/terminology.md and printed under one
    // label on screen. Nothing in this file compared the two sides to each
    // other before, which is how a ddof=0 / ddof=1 split lived here.
    if (!g_rtemporalSelftest.empty()) {
        std::string dir = g_rtemporalSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        std::vector<std::string> files;
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
                std::string n = e.path().filename().u8string();
                // the PEER's question (#111): MEASURE is served over the
                // protocol, so this collects what the peer can measure
                if (peerServesName(n)) files.push_back(dir + "/" + n);
            }
        }
        sortFramesNumerically(files);
        if (files.size() < 2) {
            fprintf(stderr, "rtemporalselftest: need >= 2 peer-servable files under %s\n",
                    dir.c_str());
            return 1;
        }
        // independent reference: per-pixel f64 accumulation over the local files.
        // Since #57 judgment 2 the reference computes the CORRECTED sigma_fpn -
        // this loop is the third implementation of the estimator and its job is
        // to disagree with the other two if either drifts, so it has to be
        // written against the settled formula and not against whichever of them
        // is being tested today.
        double refMean = 0, refSt = 0, refFpn = 0, refCorr = 0;
        bool refClamp = false;
        {
            std::vector<double> sum, sum2;
            size_t samples = 0;
            int N = 0;
            for (const auto& f : files) {
                std::string err;
                // WHICHEVER local door reads it, which since issue #148 is two:
                // this test is registered twice, once over a folder of .npy and
                // once over a folder of .png, and the second one is the whole
                // point - a peer that serves pictures for TILE and cannot be
                // measured over them would be the same "two answers" defect one
                // level down. The dispatch is the local door's own order
                // (core/app/loader_npz.inc: .npy first, then the format table).
                std::unique_ptr<ImageDoc> d = imagefile::forPath(f)
                                            ? decodeImageFileFrame(f, err)
                                            : decodeNpy(f, err);
                if (!d) {
                    fprintf(stderr, "rtemporalselftest: %s: %s\n", f.c_str(), err.c_str());
                    return 1;
                }
                if (!samples) {
                    samples = d->px().size();
                    sum.assign(samples, 0.0);
                    sum2.assign(samples, 0.0);
                }
                if (d->px().size() != samples) {
                    fprintf(stderr, "rtemporalselftest: shape mismatch in fixture\n");
                    return 1;
                }
                for (size_t i = 0; i < samples; i++) {
                    double v = d->px()[i];
                    sum[i] += v; sum2[i] += v * v;
                }
                N++;
            }
            double aM = 0, aM2 = 0, aV = 0, aC = 0;
            for (size_t i = 0; i < samples; i++) {
                double mm = sum[i] / N;
                double v = std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
                aM += mm; aM2 += mm * mm;
                aV += v;
                aC += v / N;                       // s_t,i^2 / n_i; here every n_i = N
            }
            double ns = (double)samples;
            refMean = aM / ns;
            refSt = sqrt(aV / ns);
            refCorr = aC / ns;                     // C = mean_i(s_t,i^2 / n_i)
            // var_spatial(M) at ddof=1, minus C, clamped and DECLARED
            // (docs/features/analysis/flat-field-stats.md (b))
            double sVar = std::max(0.0, aM2 / ns - refMean * refMean) * (ns / (ns - 1.0));
            refClamp = sVar - refCorr < 0.0;
            refFpn = sqrt(std::max(0.0, sVar - refCorr));
            refCorr = sqrt(refCorr);               // reported in DN, like sigma_fpn
        }
        // connect the browser, then fire the SAME call the group row's menu makes
        startRemote(rbMain(), "local://" + dir);
        double t0 = nowSec();
        bool fired = false, ok = true;
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            pumpMeasure();
            if (rbMain().b.connected && !fired) {
                std::string leaf = dir.substr(dir.find_last_of('/') + 1);
                // the label carries the fixture's OWN extension: this runs over
                // a .png folder as well now, and a label reading ".npy" over a
                // stack of PNGs is a printed statement that is simply false
                const size_t dot = files.front().find_last_of('.');
                const std::string ext = dot == std::string::npos
                                      ? std::string() : files.front().substr(dot);
                requestBrowseTemporal(rbMain().b.host, files, leaf + "/frame_???" + ext,
                                      rbMain().b.port);
                fired = true;
            }
            if (fired && !app.srvTemporal.pending) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        App::ServerTemporal& S = app.srvTemporal;
        auto rel = [](double a, double b) {
            return fabs(a - b) / std::max({ fabs(a), fabs(b), 1e-12 });
        };
        fprintf(stderr, "rtemporalselftest: [server %s, %d frames - not opened: %s] "
                        "valid=%d seqId=%d, %d image(s) open\n",
                S.host.empty() ? PEER_HERE : S.host.c_str(), S.frames,
                S.label.c_str(), S.valid ? 1 : 0, S.seqId, (int)app.images.size());
        if (!fired || !S.valid || S.seqId != -2 || S.frames != (int)files.size() ||
            !app.images.empty()) {
            fprintf(stderr, "rtemporalselftest: FAILED - no valid detached result (%s)\n",
                    S.err.c_str());
            ok = false;
        } else {
            fprintf(stderr, "rtemporalselftest: sigma_t server %.12g vs ref %.12g "
                            "(rel %.3g), sigma_fpn %.12g vs %.12g (rel %.3g), "
                            "fpn_corr %.12g vs %.12g (rel %.3g), clamped %d vs %d, "
                            "mean rel %.3g\n",
                    S.tempNoise[0], refSt, rel(S.tempNoise[0], refSt),
                    S.fixedPattern[0], refFpn, rel(S.fixedPattern[0], refFpn),
                    S.fpnCorr[0], refCorr, rel(S.fpnCorr[0], refCorr),
                    (int)S.fpnClamped[0], (int)refClamp,
                    rel(S.mean[0], refMean));
            if (rel(S.tempNoise[0], refSt) > 1e-9 || rel(S.fixedPattern[0], refFpn) > 1e-9 ||
                rel(S.fpnCorr[0], refCorr) > 1e-9 || S.fpnClamped[0] != refClamp ||
                rel(S.mean[0], refMean) > 1e-9) {
                fprintf(stderr, "rtemporalselftest: FAILED - mismatch vs reference\n");
                ok = false;
            }
        }
        // ---- P1/P2: local vs peer, one stack, one number ---------------------
        // Deliberately LAST: "no image is open" above is a real assertion about
        // the detached result, and it has to be made before anything opens the
        // stack. The peer's numbers are copied out first for the same reason -
        // app.srvTemporal is a single slot and this test is done reading it.
        const double srvSigT = S.tempNoise[0], srvFpn = S.fixedPattern[0];
        const double srvCorr = S.fpnCorr[0], srvTot = S.totalNoise[0];
        const bool srvClamp = S.fpnClamped[0];
        const int srvFrames = S.frames;
        if (ok) {
            auto chk = [&](bool cond, const char* what) {
                fprintf(stderr, "rtemporalselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
                if (!cond) ok = false;
            };
            app.seqLoadMode = 1;                  // one stack, no picker to answer
            openFolder(dir);
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            app.selectedAnn = 0;                  // whole frame: the peer sends no ROI
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            const App::TemporalState& T = app.temporal[0];
            const double ddofRatio = T.frames > 1
                ? sqrt((double)T.frames / (double)(T.frames - 1)) : 0.0;
            fprintf(stderr, "rtemporalselftest: P1 same stack both ways: local %d frame(s)/"
                            "%d plane(s) vs peer %d frame(s); sigma_t %.12g vs %.12g "
                            "(rel %.3g); sigma_fpn %.12g vs %.12g (rel %.3g); "
                            "fpn_corr %.12g vs %.12g (rel %.3g); clamped %d vs %d. "
                            "sqrt(N/(N-1)) at N=%d is %.9g - a mismatch of exactly that "
                            "is the ddof split, not a rounding difference\n",
                    T.frames, T.nPl, srvFrames,
                    T.tempNoise[0], srvSigT, rel(T.tempNoise[0], srvSigT),
                    T.fixedPattern[0], srvFpn, rel(T.fixedPattern[0], srvFpn),
                    T.fpnCorr[0], srvCorr, rel(T.fpnCorr[0], srvCorr),
                    (int)T.fpnClamped[0], (int)srvClamp,
                    T.frames, ddofRatio);
            chk(T.valid && T.nPl == 1 && T.frames == srvFrames &&
                T.frames == (int)files.size(),
                "P1 fixture: the opened stack is the one the peer measured");
            // BIT-for-bit, not "close". The two sides run the same formula over
            // the same f32 pixels in the same accumulation order, so there is no
            // rounding budget to spend and no reason to grant one: an equality
            // that holds today would be silently loosened by a tolerance the day
            // one side reorders its arithmetic. (The 1e-9 relative check above
            // is against the SEPARATE f64 reference, which does accumulate
            // differently and has earned its tolerance.)
            chk(T.tempNoise[0] == srvSigT,
                "P1 local and peer report the SAME sigma_t for one stack");
            chk(T.fixedPattern[0] == srvFpn,
                "P2 ...and the same CORRECTED sigma_fpn, bit for bit");
            // P3/P4 (#57 judgment 2): parity of the value alone is not parity of
            // the measurement. sigma_fpn clamps at 0, and two sides that both
            // clamp agree on 0 while agreeing on nothing - so the amount
            // subtracted and the clamp flag are compared too, and they are what
            // makes the agreement above mean something on a stack whose true
            // fixed pattern is (as this fixture's is) essentially zero.
            chk(T.fpnCorr[0] == srvCorr,
                "P3 ...and the same correction amount, bit for bit");
            chk(T.fpnClamped[0] == srvClamp,
                "P4 ...and they agree on whether the clamp fired");
            // P5: the row DERIVED from the other two. It is checked because it
            // is derived - the peer built it as sqrt(st*st + fvar) off an
            // already-square-rooted sigma_t, which is not the same binary64 as
            // the viewer's sqrt(tvar + fvar), and no test had ever put the two
            // beside each other to notice.
            chk(T.totalNoise[0] == srvTot,
                "P5 ...and the same sigma_tot, which is derived from both");
            closeAll();
        }
        fprintf(stderr, "rtemporalselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopMeasureWorker();
        stopRemoteFetcher();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
