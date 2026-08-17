// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- MEASURE WHERE THE DATA IS, FOR A DOCUMENT THAT IS NOT A FILE --------
    // (issue #180 stage 5, docs/remote-reader-design.md §8, protocol 14)
    //
    // Twelve taught META and TILE to address a node of what a reader produced on
    // the peer; 13 added a member of a container it listed. Both open, both
    // refine, both draw - and both had one thing they could not do, which is the
    // thing the link exists for: have a statistic computed where the pixels are.
    // MEASURE could only name FILES, and neither of those subjects IS one.
    //
    // What is asserted here is a NUMBER, three times over, because that is the
    // only claim worth making about a measurement:
    //   the peer's sigma_t for a reader's stack equals an independent f64
    //   reference computed here, and equals - BIT FOR BIT - what this viewer
    //   computes over the same stack opened locally;
    //   the same for a .npz member, whose pixels are deliberately the SAME
    //   pixels, so the two issuers of a key must also agree with each other;
    //   and a peer that predates the flag is refused BEFORE anything is sent,
    //   in a sentence that is neither the reader's nor the container's.
    //
    // A REAL PEER PROCESS, always - rreader's and rnpz's reason, one level up.
    // "It ran over there" cannot be told from "it ran here" by a client talking
    // to itself, and every sentence below about where a number came from would
    // be vacuous. --remote-exe and the standalone viewer-serve.
    if (!g_rmeasureSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "rmeasureselftest: %-74s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto has = [](const std::string& hay, const std::string& needle) {
            return hay.find(needle) != std::string::npos;
        };
        auto rel = [](double a, double b) {
            return fabs(a - b) / std::max({ fabs(a), fabs(b), 1e-12 });
        };
        // The peer reads its settings out of the environment (it has no window
        // to put them in) and is a CHILD of this process. VIEWER_SERVE_CACHE in
        // particular is what makes the key resolvable at all: the MEASURE
        // worker's peer is a DIFFERENT PROCESS from the one the document was
        // opened through, so the note beside a key has to be on disk.
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };

        std::string root = g_rmeasureSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/rmeasure-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(root), ec);
        const std::string cacheDir = root + "/peer-cache";
        std::filesystem::create_directories(pathFromUtf8(cacheDir), ec);
        setEnv("VIEWER_SERVE_CACHE", cacheDir);
        setEnv("VIEWER_SERVE_PYTHON", "");
        setEnv("VIEWER_SERVE_PROTOCOL", "");

        // ---- the fixture: ONE cube, carried two ways --------------------------
        // The reader's origin is rank 5, which nothing native reads (§3.2), so
        // the reader is the only way that file becomes a document - on either
        // machine. The .npz member holds the SAME samples in (F,H,W), so the two
        // keyed measurements are measurements of one array and may be compared
        // to each other as well as to the reference.
        //
        // The values carry a real fixed pattern (a spatial ramp) UNDER a small
        // temporal jitter, so sigma_fpn is far from its clamp: two sides that
        // both clamp at 0 agree on 0 while agreeing on nothing.
        const int SF = 6, HH = 12, WW = 10;
        std::vector<uint16_t> cube((size_t)SF * HH * WW);
        for (int f = 0; f < SF; f++)
            for (int y = 0; y < HH; y++)
                for (int x = 0; x < WW; x++)
                    cube[((size_t)f * HH + y) * WW + x] =
                        (uint16_t)(400 + 3 * (y * WW + x) + ((f * 37 + y * 11 + x * 7) % 9));
        const std::string origin = root + "/cube.npy";
        {
            std::vector<uint8_t> b = selftestNpyBytes("<u2", { SF, HH, WW, 1, 1 },
                                                      cube.data(), cube.size() * 2);
            std::ofstream f(pathFromUtf8(origin), std::ios::binary);
            f.write((const char*)b.data(), (std::streamsize)b.size());
        }
        const std::string many = root + "/many.npz";
        selftestWriteNpz(many, {
            { "frames", selftestNpyBytes("<u2", { SF, HH, WW }, cube.data(), cube.size() * 2) },
        });
        auto writeText = [&](const std::string& p, const std::string& s) {
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            f.write(s.data(), (std::streamsize)s.size());
        };
        const std::string rdPerm = root + "/perm.py";
        writeText(rdPerm,
                  "import numpy as np\n"
                  "from viewer_import import Stack\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    a = np.load(path)\n"
                  "    return Stack(np.ascontiguousarray(a.reshape(a.shape[0], a.shape[1],\n"
                  "                                                a.shape[2])),\n"
                  "                 name='cube')\n");

        // ---- the independent reference (rtemporal's third implementation) -----
        // Written against the settled formula (docs/flat-field-stats.md (b)) and
        // not against either of the two under test, because its job is to
        // disagree with them if either drifts. u16 widens to float32 exactly, so
        // accumulating the integers in f64 is accumulating the samples.
        double refMean = 0, refSt = 0, refFpn = 0, refCorr = 0;
        bool refClamp = false;
        {
            const size_t samples = (size_t)HH * WW;
            const int N = SF;
            std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
            for (int f = 0; f < N; f++)
                for (size_t i = 0; i < samples; i++) {
                    const double v = cube[(size_t)f * samples + i];
                    sum[i] += v; sum2[i] += v * v;
                }
            double aM = 0, aM2 = 0, aV = 0, aC = 0;
            for (size_t i = 0; i < samples; i++) {
                const double mm = sum[i] / N;
                const double v = std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
                aM += mm; aM2 += mm * mm;
                aV += v;
                aC += v / N;                       // s_t,i^2 / n_i; every n_i = N
            }
            const double ns = (double)samples;
            refMean = aM / ns;
            refSt = sqrt(aV / ns);
            refCorr = aC / ns;
            const double sVar = std::max(0.0, aM2 / ns - refMean * refMean) * (ns / (ns - 1.0));
            refClamp = sVar - refCorr < 0.0;
            refFpn = sqrt(std::max(0.0, sVar - refCorr));
            refCorr = sqrt(refCorr);
        }

        const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
        app.remoteExe = exe;
        const std::string url = "local://" + origin;
        const std::string npzUrl = "local://" + many;
        const std::string spec = rdPerm + ":load";

        // The stack aggregate is fired by the code that mints the stack and
        // answered on the MEASURE worker, so a headless test has to be the UI
        // thread for both halves: pump the fetches (the sibling frames of a
        // member's stack arrive that way) and pump the measurement.
        auto settle = [&](double budget) {
            const double t0 = nowSec();
            while (nowSec() - t0 < budget) {
                pumpRemoteFetch();
                pumpSequenceAndQueue();
                pumpMeasure();
                if (app.rfPending == 0 && !app.srvTemporal.pending) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            pumpRemoteFetch();
            pumpMeasure();
        };
        // What this viewer computes over the same stack when the frames are HERE.
        // The panel's own door, whole-frame (the peer puts no ROI on the wire).
        auto localTemporal = [&]() -> const App::TemporalState& {
            app.selectedAnn = 0;
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            return app.temporal[0];
        };

        double sigT[2] = {}, fpn[2] = {}, corr[2] = {}, tot[2] = {}, mean[2] = {};
        bool clamped[2] = {};
        int srvFrames[2] = {};

        // ---- M1: a reader's stack, measured on the peer -----------------------
        {
            closeAll();
            const std::string rerr = openRemoteWithReader(url, spec);
            check(rerr.empty() && app.images.size() == (size_t)SF,
                  "M1a (setup) the peer's reader output is open, frame for frame");
            settle(120.0);
            const App::ServerTemporal& S = app.srvTemporal;
            fprintf(stderr, "rmeasureselftest:   M1 server: valid=%d frames=%d seq=%d "
                            "sigma_t %.12g err=\"%s\"\n",
                    S.valid ? 1 : 0, S.frames, S.seqId, S.tempNoise[0], S.err.c_str());
            check(S.valid && S.frames == SF,
                  "M1b a reader's stack IS measured on the peer, over every frame");
            check(S.valid && rel(S.tempNoise[0], refSt) <= 1e-9 &&
                      rel(S.fixedPattern[0], refFpn) <= 1e-9 &&
                      rel(S.fpnCorr[0], refCorr) <= 1e-9 &&
                      rel(S.mean[0], refMean) <= 1e-9 && S.fpnClamped[0] == refClamp,
                  "M1c ...and its four numbers match an independent f64 reference");
            sigT[0] = S.tempNoise[0]; fpn[0] = S.fixedPattern[0];
            corr[0] = S.fpnCorr[0];   tot[0] = S.totalNoise[0];
            mean[0] = S.mean[0];      clamped[0] = S.fpnClamped[0];
            srvFrames[0] = S.frames;

            // ...and the SAME reader over the SAME file, run HERE, measured by
            // the panel's own estimator. BIT-for-bit and not "close": one
            // machine, one formula, one accumulation order, so there is no
            // rounding budget to spend and no reason to grant one (rtemporal
            // P1/P2's line, over a subject that is not a file).
            closeAll();
            const std::string lerr = openWithReader(origin, spec, false);
            check(lerr.empty() && app.images.size() == (size_t)SF,
                  "M1d (setup) the same reader, run here, opens the same stack");
            const App::TemporalState& T = localTemporal();
            fprintf(stderr, "rmeasureselftest:   M1 local: valid=%d frames=%d nPl=%d "
                            "sigma_t %.12g vs peer %.12g\n",
                    T.valid ? 1 : 0, T.frames, T.nPl, T.tempNoise[0], sigT[0]);
            check(T.valid && T.nPl == 1 && T.frames == srvFrames[0],
                  "M1e (setup) the local stack is the one the peer measured");
            check(T.tempNoise[0] == sigT[0] && T.fixedPattern[0] == fpn[0],
                  "M1f local and peer report the SAME sigma_t and sigma_fpn, bit for bit");
            check(T.fpnCorr[0] == corr[0] && T.fpnClamped[0] == clamped[0] &&
                      T.totalNoise[0] == tot[0],
                  "M1g ...and the same correction, clamp flag and sigma_tot");
            check(!clamped[0],
                  "M1h (fixture) sigma_fpn is far from its clamp, so the agreement means something");
            closeAll();
        }

        // ---- M2: a .npz member, the second issuer of a key --------------------
        {
            closeAll();
            const bool got = openRemoteNpz(npzUrl, "frames");
            settle(120.0);
            check(got && app.images.size() == (size_t)SF,
                  "M2a (setup) one member of a container is open over the link");
            const App::ServerTemporal& S = app.srvTemporal;
            fprintf(stderr, "rmeasureselftest:   M2 server: valid=%d frames=%d "
                            "sigma_t %.12g err=\"%s\"\n",
                    S.valid ? 1 : 0, S.frames, S.tempNoise[0], S.err.c_str());
            check(S.valid && S.frames == SF,
                  "M2b a member's stack IS measured on the peer, over every frame");
            check(S.valid && rel(S.tempNoise[0], refSt) <= 1e-9 &&
                      rel(S.fixedPattern[0], refFpn) <= 1e-9 &&
                      rel(S.fpnCorr[0], refCorr) <= 1e-9 &&
                      rel(S.mean[0], refMean) <= 1e-9 && S.fpnClamped[0] == refClamp,
                  "M2c ...and its four numbers match the same f64 reference");
            sigT[1] = S.tempNoise[0]; fpn[1] = S.fixedPattern[0];
            corr[1] = S.fpnCorr[0];   tot[1] = S.totalNoise[0];
            mean[1] = S.mean[0];      clamped[1] = S.fpnClamped[0];
            srvFrames[1] = S.frames;
            // The two subjects hold the same samples and were reached through
            // two different issuers of a key. One array, one number - and this
            // is the assertion that the wire does not know which issued it.
            check(sigT[1] == sigT[0] && fpn[1] == fpn[0] && corr[1] == corr[0] &&
                      tot[1] == tot[0] && mean[1] == mean[0] && clamped[1] == clamped[0],
                  "M2d reader-issued and npz-issued keys measure to the SAME numbers");

            closeAll();
            const std::string lerr = loadNpz(many, "frames");
            check(lerr.empty() && app.images.size() == (size_t)SF,
                  "M2e (setup) the same member opens off this disk");
            const App::TemporalState& T = localTemporal();
            check(T.valid && T.frames == srvFrames[1] &&
                      T.tempNoise[0] == sigT[1] && T.fixedPattern[0] == fpn[1] &&
                      T.fpnCorr[0] == corr[1] && T.fpnClamped[0] == clamped[1] &&
                      T.totalNoise[0] == tot[1],
                  "M2f ...and the local door computes the peer's numbers, bit for bit");
            closeAll();
        }

        // ---- M3: the gate is about CODE, not about containers ------------------
        // §2's flag decides whether code sent by a client may run on this
        // machine; §10 records that it has nothing to say about a .npz, where
        // the only thing that runs over there is the peer's own. So with the
        // gate shut there is no reader document to measure AT ALL - and the
        // member's measurement is untouched. The asymmetry is the assertion.
        {
            closeAll();
            app.uiSession.stop();
            app.uiSession.setServeReaders(false);
            std::string serr;
            if (!ensureUiSession("", serr, 0)) {
                fprintf(stderr, "rmeasureselftest: cannot start the peer: %s\n", serr.c_str());
                return 1;
            }
            const size_t before = app.images.size();
            const std::string rerr = openRemoteWithReader(url, spec);
            fprintf(stderr, "rmeasureselftest:   M3 gate shut: \"%s\"\n", rerr.c_str());
            check(!rerr.empty() && has(rerr, "--serve-readers") &&
                      app.images.size() == before,
                  "M3a with the gate shut there is no reader document, so nothing to measure");
            // ...and the container, on the very same shut peer.
            app.srvTemporal = App::ServerTemporal{};
            const bool got = openRemoteNpz(npzUrl, "frames");
            settle(120.0);
            const App::ServerTemporal& S = app.srvTemporal;
            fprintf(stderr, "rmeasureselftest:   M3 npz on a shut peer: opened=%d valid=%d "
                            "sigma_t %.12g err=\"%s\"\n",
                    got ? 1 : 0, S.valid ? 1 : 0, S.tempNoise[0], S.err.c_str());
            check(got && S.valid && S.frames == SF && S.tempNoise[0] == sigT[0],
                  "M3b ...while the member's MEASURE runs there unchanged: the flag "
                  "guards code, not data");
            closeAll();
            app.uiSession.stop();
            app.uiSession.setServeReaders(true);
        }

        // ---- M4: a peer that predates the flag is refused BEFORE sending -------
        // The sharpest of the six sentences in remote_proto.h. A v13 peer reads
        // the head it knows and never reads the key behind the rois; had the
        // request also carried the origin path, it would have answered with a
        // real statistic over the whole container. So no path is sent, and the
        // client refuses from the number rather than learning it by asking.
        {
            setEnv("VIEWER_SERVE_PROTOCOL", "13");
            remote::Session old;
            std::string serr;
            if (!old.startOn("", 0, exe, serr)) {
                fprintf(stderr, "rmeasureselftest: cannot start the v13 peer: %s\n", serr.c_str());
                return 1;
            }
            check(old.peerVersion() == 13, "M4a (setup) the peer is being a v13");
            remote::MeasureReq q;
            q.op = rp::MOP_TEMPORAL_STATS;
            q.hasKeyed = true;
            q.keyed.key = "0123456789abcdef";
            q.keyed.node = 0;
            q.keyed.kind = remote::KeyedRef::FromNpz;
            remote::MeasureResult res;
            std::string err;
            const bool sent = old.measure(q, res, err);
            fprintf(stderr, "rmeasureselftest:   M4 \"%s\"\n", err.c_str());
            check(!sent && err == rp::measureKeyedTooOldText(13),
                  "M4b a keyed MEASURE on a v13 peer is refused by the CLIENT, from the number");
            check(!has(err, "a reader cannot run") && !has(err, "list what is inside") &&
                      !has(err, q.keyed.key),
                  "M4c ...in its own sentence, neither the reader's nor the container's, "
                  "and it quotes no opaque token at a person");
            // A reader-issued key gets the SAME sentence: 14 is one build and
            // one fix, unlike META's 12-or-13 where the two send a person to
            // different places.
            q.keyed.kind = remote::KeyedRef::FromReader;
            std::string err2;
            check(!old.measure(q, res, err2) && err2 == err,
                  "M4d ...and it is one sentence for both families of key");
            old.stop();
            setEnv("VIEWER_SERVE_PROTOCOL", "");
        }

        // ---- M5: the edges of the grammar --------------------------------------
        // What a key can and cannot be asked, said by name rather than
        // approximated. These go through a Session of their own: a well-behaved
        // client never builds them, and that is exactly why they are worth
        // asserting.
        {
            remote::Session ses;
            std::string serr;
            if (!ses.startOn("", 0, exe, serr)) {
                fprintf(stderr, "rmeasureselftest: cannot start the peer: %s\n", serr.c_str());
                return 1;
            }
            check(ses.peerVersion() == (int)rp::VERSION,
                  "M5a (setup) the peer speaks the protocol this build does (14)");
            remote::NpzScan sc;
            std::string err;
            const bool listed = ses.npzScan(many, sc, err);
            check(listed && !sc.members.empty(), "M5b (setup) the container lists");
            const uint32_t node = listed && !sc.members.empty()
                                ? (uint32_t)sc.members[0].entry : 0u;

            // the SECOND aggregate op, so that "keyed" is a property of the
            // subject and not of one measurement
            remote::MeasureReq q;
            q.op = rp::MOP_FRAME_ROI_STATS;
            q.hasKeyed = true;
            q.keyed.key = sc.key; q.keyed.node = (int)node;
            q.keyed.kind = remote::KeyedRef::FromNpz;
            remote::MeasureResult roi;
            std::string e1;
            const bool gotRoi = ses.measure(q, roi, e1);
            check(gotRoi && roi.framesUsed == SF && !roi.series.empty(),
                  "M5c the per-frame statistics reach a member too, over every frame");

            // an op whose own block names FILES
            remote::MeasureReq bad = q;
            bad.op = rp::MOP_SET_FOLD;
            std::string e2;
            check(!ses.measure(bad, roi, e2) && has(e2, "addresses files"),
                  "M5d a set fold over a key is refused by name, not folded over something else");

            // two declarations of what the pixels are
            remote::MeasureReq both = q;
            both.hasRecipe = true;
            std::string e3;
            check(!ses.measure(both, roi, e3) &&
                      has(e3, "does not apply to an array inside a materialisation"),
                  "M5e a recipe beside a key is refused in META's own words");

            // a key this peer never issued: refused by name, and not a crash
            remote::MeasureReq gone = q;
            gone.op = rp::MOP_TEMPORAL_STATS;
            gone.keyed.key = "0000000000000000";
            std::string e4;
            const bool answered = ses.measure(gone, roi, e4);
            fprintf(stderr, "rmeasureselftest:   M5 unknown key: \"%s\"\n", e4.c_str());
            check(!answered && !e4.empty(),
                  "M5f a key the peer has never issued is refused, by name");
            ses.stop();
        }

        setEnv("VIEWER_SERVE_CACHE", "");
        closeAll();
        app.uiSession.stop();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "rmeasureselftest: %s\n", ok ? "ok" : "FAILED");
        stopWatchWorker();
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
