// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // Browser-fired temporal, verifiable without a human (docs/terminology.md):
    // the server aggregate for a stack NOBODY OPENED must land in the Temporal
    // panel as a detached [not opened] result and match an independent local
    // computation to full float64 precision.
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
            fprintf(stderr, "rtemporalselftest: need >= 2 npy under %s\n", dir.c_str());
            return 1;
        }
        // independent reference: per-pixel f64 accumulation over the local files
        double refMean = 0, refSt = 0, refFpn = 0;
        {
            std::vector<double> sum, sum2;
            size_t samples = 0;
            int N = 0;
            for (const auto& f : files) {
                std::string err;
                std::unique_ptr<ImageDoc> d = decodeNpy(f, err);
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
            double aM = 0, aM2 = 0, aV = 0;
            for (size_t i = 0; i < samples; i++) {
                double mm = sum[i] / N;
                aM += mm; aM2 += mm * mm;
                aV += std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
            }
            refMean = aM / samples;
            refSt = sqrt(aV / samples);
            refFpn = sqrt(std::max(0.0, aM2 / samples - refMean * refMean));
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
                requestBrowseTemporal(rbMain().b.host, files, leaf + "/frame_???.npy", rbMain().b.port);
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
                            "mean rel %.3g\n",
                    S.tempNoise[0], refSt, rel(S.tempNoise[0], refSt),
                    S.fixedPattern[0], refFpn, rel(S.fixedPattern[0], refFpn),
                    rel(S.mean[0], refMean));
            if (rel(S.tempNoise[0], refSt) > 1e-9 || rel(S.fixedPattern[0], refFpn) > 1e-9 ||
                rel(S.mean[0], refMean) > 1e-9) {
                fprintf(stderr, "rtemporalselftest: FAILED - mismatch vs reference\n");
                ok = false;
            }
        }
        fprintf(stderr, "rtemporalselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopMeasureWorker();
        stopRemoteFetcher();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
