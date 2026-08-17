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
    // What is asserted here is a NUMBER, because that is the only claim worth
    // making about a measurement - and it is asserted against TWO independent
    // things every time: an f64 reference computed in this file from the
    // fixture's own integers, and what this viewer's own estimator says about
    // the very same stack, BIT FOR BIT.
    //
    // Three of the checks exist because a measurement can be wrong while
    // looking perfectly normal, and each of those three is a way it was:
    //   a container's node is addressed by the ZIP ENTRY the SCAN issued and
    //   not by the tree's own index (M6) - where the two collide, the peer
    //   answers with ANOTHER member's pixels under this stack's name;
    //   a tree lands SEVERAL stacks and only one can hold the single result
    //   slot, so it must be the one on screen and the others must be able to
    //   ask for themselves (M6c);
    //   a stack's MOSAIC has to reach the peer, or four planes are folded into
    //   one over there while this side splits them (M7).
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

        // ---- the fixtures -----------------------------------------------------
        // (a) ONE cube, carried two ways. The reader's origin is rank 5, which
        //     nothing native reads (§3.2), so the reader is the only way that
        //     file becomes a document - on either machine. The .npz member holds
        //     the SAME samples in (F,H,W), so the two keyed measurements are
        //     measurements of one array and may be compared to each other.
        //
        //     The values carry a real fixed pattern (a spatial ramp) UNDER a
        //     small temporal jitter, so sigma_fpn is far from its clamp: two
        //     sides that both clamp at 0 agree on 0 while agreeing on nothing.
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

        // (b) a VIEWER CONTAINER with TWO stacks, and it is built to make three
        //     silent wrongs loud:
        //       the stacks are tree nodes 2 and 3 while their `__pixels_`
        //       members are ZIP entries 15 and 19 - so an address computed from
        //       the tree index reaches a LAYER STRING, not pixels;
        //       the two hold DIFFERENT samples, so measuring the wrong one is a
        //       different number rather than the same one;
        //       node 2 declares a Bayer mosaic and node 3 does not, so the
        //       stack's CFA has to reach the peer for its four planes to exist;
        //       node 3 is 1608 samples wide, so it LANDS DECIMATED (step 2, the
        //       ceil(max(w,h)/1600) rule) and is refined by a second request -
        //       which quotes the node too, and would fetch another member's
        //       pixels into this stack if the address were the tree index.
        const int TF = 4, TH = 8, TW = 8, TWB = 1608;
        std::vector<uint16_t> cubeA((size_t)TF * TH * TW), cubeB((size_t)TF * TH * TWB);
        for (int f = 0; f < TF; f++)
            for (int y = 0; y < TH; y++) {
                for (int x = 0; x < TW; x++) {
                    const int plane = (y & 1) * 2 + (x & 1);      // RGGB
                    cubeA[((size_t)f * TH + y) * TW + x] =
                        (uint16_t)(300 + 100 * plane + 2 * (y * TW + x) +
                                   ((f * 29 + y * 7 + x * 13) % 7));
                }
                for (int x = 0; x < TWB; x++)
                    cubeB[((size_t)f * TH + y) * TWB + x] =
                        (uint16_t)(900 + ((y * TWB + x) % 137) + ((f * 11 + x * 5) % 13));
            }
        auto i32 = [](int32_t v) { return selftestNpyBytes("<i4", {}, &v, 4); };
        auto f64v = [](const std::vector<double>& v) {
            return selftestNpyBytes("<f8", { (int64_t)v.size() }, v.data(), v.size() * 8);
        };
        auto str1 = [](const std::string& s) {
            size_t item = s.empty() ? 1 : s.size();
            std::vector<uint32_t> cps(item, 0);
            for (size_t i = 0; i < s.size(); i++) cps[i] = (uint32_t)(unsigned char)s[i];
            return selftestNpyBytes(("<U" + std::to_string(item)).c_str(), {},
                                    cps.data(), cps.size() * 4);
        };
        const std::string cont = root + "/tree.npz";
        selftestWriteNpz(cont, {
            { "__viewer",   i32(1) },                                  // entry 0
            { "__n",        i32(4) },
            { "__layer_0",  str1("batch") },
            { "__parent_0", i32(-1) },
            { "__name_0",   str1("sweep") },
            { "__layer_1",  str1("series") },
            { "__parent_1", i32(0) },
            { "__name_1",   str1("exposure sweep") },
            { "__conditions_values_1", f64v({ 2.0, 8.0 }) },
            { "__conditions_name_1",   str1("exposure") },
            { "__conditions_unit_1",   str1("ms") },
            { "__layer_2",  str1("stack") },                           // entry 11
            { "__parent_2", i32(1) },
            { "__name_2",   str1("e2") },
            { "__cfa_2",    str1("RGGB") },
            { "__pixels_2", selftestNpyBytes("<u2", { TF, TH, TW },    // entry 15
                                             cubeA.data(), cubeA.size() * 2) },
            { "__layer_3",  str1("stack") },
            { "__parent_3", i32(1) },
            { "__name_3",   str1("e8") },
            { "__pixels_3", selftestNpyBytes("<u2", { TF, TH, TWB },   // entry 19
                                             cubeB.data(), cubeB.size() * 2) },
        });

        // ---- the independent reference (rtemporal's third implementation) -----
        // Written against the settled formula (docs/flat-field-stats.md (b)) and
        // not against either of the two under test, because its job is to
        // disagree with them if either drifts. u16 widens to float32 exactly, so
        // accumulating the integers in f64 is accumulating the samples.
        struct TRef { double mean = 0, sigT = 0, fpn = 0, corr = 0; bool clamped = false; };
        auto reference = [](const std::vector<uint16_t>& px, int N, size_t samples) {
            TRef R;
            std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
            for (int f = 0; f < N; f++)
                for (size_t i = 0; i < samples; i++) {
                    const double v = px[(size_t)f * samples + i];
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
            R.mean = aM / ns;
            R.sigT = sqrt(aV / ns);
            R.corr = aC / ns;
            const double sVar = std::max(0.0, aM2 / ns - R.mean * R.mean) * (ns / (ns - 1.0));
            R.clamped = sVar - R.corr < 0.0;
            R.fpn = sqrt(std::max(0.0, sVar - R.corr));
            R.corr = sqrt(R.corr);
            return R;
        };
        const TRef ref = reference(cube, SF, (size_t)HH * WW);

        const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
        app.remoteExe = exe;
        const std::string url = "local://" + origin;
        const std::string npzUrl = "local://" + many;
        const std::string treeUrl = "local://" + cont;
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
        // A result slot that CANNOT be mistaken for the last test's. closeAll()
        // does not clear it - it is a measurement, not a document - so a check
        // that only looked at the numbers would pass on a stale answer about
        // the same pixels. Reset, and assert the token moved.
        auto armSlot = [&]() {
            app.srvTemporal = App::ServerTemporal{};
            return app.srvTemporal.token;
        };
        // What this viewer computes over the stack that is SELECTED, whole-frame
        // (the peer puts no ROI on the wire). The panel's own door.
        auto localTemporal = [&]() -> const App::TemporalState& {
            app.selectedAnn = 0;
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            return app.temporal[0];
        };
        auto selectStack = [&](int seqId) {
            for (size_t i = 0; i < app.images.size(); i++)
                if (app.images[i]->seqId == seqId) { app.current = (int)i; return true; }
            return false;
        };
        // Server and local, plane for plane, with no rounding budget granted:
        // one machine, one formula, one accumulation order, so anything but
        // equality is the transport changing a number (rtemporal P1-P5's line).
        auto sameAsLocal = [&](const App::ServerTemporal& S, const App::TemporalState& T) {
            if (!S.valid || !T.valid || S.nPl != T.nPl || S.frames != T.frames) return false;
            // (the local state carries no per-plane MEAN - the panel prints the
            // stack's mean from the frame series - so the five that both sides
            // do carry are the five compared; M1c/M2c hold the mean against the
            // f64 reference instead)
            for (int p = 0; p < S.nPl; p++)
                if (S.tempNoise[p] != T.tempNoise[p] || S.fixedPattern[p] != T.fixedPattern[p] ||
                    S.fpnCorr[p] != T.fpnCorr[p] || S.fpnClamped[p] != T.fpnClamped[p] ||
                    S.totalNoise[p] != T.totalNoise[p])
                    return false;
            return true;
        };
        auto matchesRef = [&](const App::ServerTemporal& S, const TRef& R) {
            return S.valid && rel(S.tempNoise[0], R.sigT) <= 1e-9 &&
                   rel(S.fixedPattern[0], R.fpn) <= 1e-9 &&
                   rel(S.fpnCorr[0], R.corr) <= 1e-9 &&
                   rel(S.mean[0], R.mean) <= 1e-9 && S.fpnClamped[0] == R.clamped;
        };

        double sigT[2] = {}, fpn[2] = {}, corr[2] = {}, tot[2] = {}, mean[2] = {};
        bool clamped[2] = {};
        int srvFrames[2] = {};
        uint64_t tokenM1 = 0;

        // ---- M1: a reader's stack, measured on the peer -----------------------
        {
            closeAll();
            armSlot();
            const std::string rerr = openRemoteWithReader(url, spec);
            check(rerr.empty() && app.images.size() == (size_t)SF,
                  "M1a (setup) the peer's reader output is open, frame for frame");
            settle(120.0);
            const App::ServerTemporal& S = app.srvTemporal;
            fprintf(stderr, "rmeasureselftest:   M1 server: valid=%d frames=%d seq=%d nPl=%d "
                            "sigma_t %.12g err=\"%s\"\n",
                    S.valid ? 1 : 0, S.frames, S.seqId, S.nPl, S.tempNoise[0], S.err.c_str());
            check(S.valid && S.frames == SF && S.token != 0 && cur() && S.seqId == cur()->seqId,
                  "M1b a reader's stack IS measured on the peer, over every frame");
            check(matchesRef(S, ref),
                  "M1c ...and its four numbers match an independent f64 reference");
            sigT[0] = S.tempNoise[0]; fpn[0] = S.fixedPattern[0];
            corr[0] = S.fpnCorr[0];   tot[0] = S.totalNoise[0];
            mean[0] = S.mean[0];      clamped[0] = S.fpnClamped[0];
            srvFrames[0] = S.frames;
            tokenM1 = S.token;

            // ...and the SAME reader over the SAME file, run HERE, measured by
            // the panel's own estimator.
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
            armSlot();                       // M1's answer must not be reusable
            const bool got = openRemoteNpz(npzUrl, "frames");
            settle(120.0);
            check(got && app.images.size() == (size_t)SF,
                  "M2a (setup) one member of a container is open over the link");
            const App::ServerTemporal& S = app.srvTemporal;
            fprintf(stderr, "rmeasureselftest:   M2 server: valid=%d frames=%d token %llu->%llu "
                            "sigma_t %.12g err=\"%s\"\n",
                    S.valid ? 1 : 0, S.frames, (unsigned long long)tokenM1,
                    (unsigned long long)S.token, S.tempNoise[0], S.err.c_str());
            check(S.valid && S.frames == SF && S.token != tokenM1 && cur() &&
                      S.seqId == cur()->seqId,
                  "M2b a member's stack IS measured on the peer - a NEW measurement, "
                  "not the last one still in the slot");
            check(matchesRef(S, ref),
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

        // ---- M6: a TREE, where the address is not the index --------------------
        // The container's stacks are tree nodes 2 and 3; their pixels are ZIP
        // entries 15 and 19. Every later request for a node - the full-
        // resolution swap, a sibling frame, and now its statistics - has to
        // quote the number the PEER issued (§10.5). Quoting the tree index
        // reaches `__layer_2`, a string.
        //
        // ...and a tree lands SEVERAL stacks with ONE result slot. The one that
        // is measured at landing must be the one on screen, and the others must
        // be able to ask for themselves.
        int seqCur = 0, seqOther = 0;
        double sigCur = 0, sigOther = 0;
        {
            closeAll();
            armSlot();
            const bool got = openRemoteNpz(treeUrl, std::string());
            settle(120.0);
            check(got && app.images.size() == (size_t)(2 * TF),
                  "M6a (setup) a two-stack viewer container opens over the link");
            std::vector<int> seqs;
            for (const auto& d : app.images)
                if (d->seqId && std::find(seqs.begin(), seqs.end(), d->seqId) == seqs.end())
                    seqs.push_back(d->seqId);
            check(seqs.size() == 2, "M6b (setup) ...as two stacks");
            const App::ServerTemporal& S = app.srvTemporal;
            seqCur = cur() ? cur()->seqId : 0;
            for (int s : seqs) if (s != seqCur) seqOther = s;
            fprintf(stderr, "rmeasureselftest:   M6 landing: current seq %d, measured seq %d, "
                            "valid=%d sigma_t %.12g err=\"%s\"\n",
                    seqCur, S.seqId, S.valid ? 1 : 0, S.tempNoise[0], S.err.c_str());
            check(S.valid && S.seqId == seqCur && seqCur != 0,
                  "M6c the stack that is ON SCREEN is the one measured at landing");
            // The address test, twice over. The FIRST fetch used the peer's own
            // entry; the REFINE and the STATISTIC quote a node recorded at
            // landing. If any of the three disagrees about which array this is,
            // the numbers below do too - and the refine is the one that would
            // silently graft another member's pixels into this stack.
            selectStack(seqCur);
            bool refined = !app.images.empty();
            int fullW = 0;
            for (const auto& d : app.images)
                if (d->seqId == seqCur) {
                    if (d->src->remoteStep != 1) refined = false;
                    fullW = d->w;
                }
            fprintf(stderr, "rmeasureselftest:   M6 refine: step back to 1 = %d, width %d "
                            "(landed decimated at %d)\n",
                    refined ? 1 : 0, fullW, (TWB + 1) / 2);
            check(refined && fullW == TWB,
                  "M6c2 ...and the full-resolution refine came back to the SAME array: "
                  "a stack that landed decimated is whole, at its own width");
            const App::TemporalState& T = localTemporal();
            fprintf(stderr, "rmeasureselftest:   M6 current stack: server nPl=%d sigma_t %.12g "
                            "vs local nPl=%d %.12g\n",
                    S.nPl, S.tempNoise[0], T.nPl, T.tempNoise[0]);
            check(sameAsLocal(S, T),
                  "M6d ...and it measured THAT array: every plane matches the local "
                  "estimator over the same stack, bit for bit");
            sigCur = S.tempNoise[0];

            // The OTHER stack, asked for the way a selection asks (the panel
            // calls exactly this before it draws a server row).
            armSlot();
            selectStack(seqOther);
            ensureServerTemporalFor(seqOther);
            settle(120.0);
            const App::ServerTemporal& S2 = app.srvTemporal;
            const App::TemporalState& T2 = localTemporal();
            fprintf(stderr, "rmeasureselftest:   M6 other stack %d: valid=%d nPl=%d "
                            "sigma_t %.12g vs local %.12g\n",
                    S2.seqId, S2.valid ? 1 : 0, S2.nPl, S2.tempNoise[0], T2.tempNoise[0]);
            check(S2.valid && S2.seqId == seqOther,
                  "M6e the second stack of the tree asks for itself when selected");
            check(sameAsLocal(S2, T2),
                  "M6f ...and it too measured its OWN array, bit for bit");
            sigOther = S2.tempNoise[0];
            // Without this the two checks above could both be satisfied by one
            // array measured twice.
            check(sigCur != sigOther,
                  "M6g (fixture) the two stacks are genuinely different arrays");
            // ...and asking again for a stack already answered starts no second
            // job: a server aggregate is a real job on a real machine.
            const uint64_t t2 = app.srvTemporal.token;
            ensureServerTemporalFor(seqOther);
            ensureServerTemporalFor(seqCur);
            check(app.srvTemporal.token == t2 && !app.srvTemporal.pending,
                  "M6h ...and a stack that has been asked is never asked twice");
            closeAll();
        }

        // ---- M7: the MOSAIC reaches the peer -----------------------------------
        // One of the two stacks declares `__cfa_2 RGGB`. vnzBuild puts that on
        // every ImageDoc; the wire carries the STACK's mosaic. A stack row left
        // at 0/0 makes the peer pool R/Gr/Gb/B into one plane while this side
        // splits them into four - the same stack with two meanings, which is
        // the defect this whole file is about, one level down.
        {
            closeAll();
            armSlot();
            openRemoteNpz(treeUrl, std::string());
            settle(120.0);
            // find the stack whose documents carry the mosaic
            int seqCfa = 0;
            for (const auto& d : app.images) if (d->cfa && d->seqId) { seqCfa = d->seqId; break; }
            check(seqCfa != 0, "M7a (setup) one stack of the container declares a mosaic");
            armSlot();
            selectStack(seqCfa);
            ensureServerTemporalFor(seqCfa);
            settle(120.0);
            const App::ServerTemporal& S = app.srvTemporal;
            const App::TemporalState& T = localTemporal();
            fprintf(stderr, "rmeasureselftest:   M7 CFA stack %d: server nPl=%d "
                            "[%.9g %.9g %.9g %.9g] local nPl=%d [%.9g %.9g %.9g %.9g]\n",
                    seqCfa, S.nPl, S.tempNoise[0], S.tempNoise[1], S.tempNoise[2],
                    S.tempNoise[3], T.nPl, T.tempNoise[0], T.tempNoise[1],
                    T.tempNoise[2], T.tempNoise[3]);
            check(S.valid && S.seqId == seqCfa && S.nPl == 4,
                  "M7b a mosaic stack is measured PER PLANE on the peer, not pooled");
            check(T.nPl == 4 && sameAsLocal(S, T),
                  "M7c ...and all four planes are the local estimator's, bit for bit");
            check(S.tempNoise[0] != S.tempNoise[1] || S.tempNoise[0] != S.tempNoise[3],
                  "M7d (fixture) the planes really differ, so a pooled answer could not pass");
            closeAll();
        }

        // ---- M3: the gate is about CODE, not about containers ------------------
        // §2's flag decides whether code sent by a client may run on this
        // machine; §10 records that it has nothing to say about a .npz, where
        // the only thing that runs over there is the peer's own. So with the
        // gate shut there is no reader document to measure AT ALL - and the
        // member's measurement is untouched.
        //
        // Both halves go through ONE peer that was started with the gate shut.
        // The app's uiSession alone would not do: the MEASURE worker builds a
        // session of its own, whose gate is open by default, so an assertion
        // made through it would be about a different process.
        {
            closeAll();
            std::string harness = runAdapterScript();
            std::string files[3];
            auto slurp = [&](const std::string& p, std::string& into) {
                std::ifstream f(pathFromUtf8(p), std::ios::binary);
                if (!f) return false;
                into.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return true;
            };
            const bool texts = !harness.empty() && slurp(rdPerm, files[0]) &&
                slurp((pathFromUtf8(harness).parent_path() / "viewer_import.py").u8string(),
                      files[1]) && slurp(harness, files[2]);
            check(texts, "M3a (setup) the three texts a RUN carries are readable here");
            remote::Session shut;
            shut.setServeReaders(false);
            std::string serr;
            if (!texts || !shut.startOn("", 0, exe, serr)) {
                fprintf(stderr, "rmeasureselftest: cannot start the shut peer: %s\n", serr.c_str());
                return 1;
            }
            remote::ReaderRun run;
            std::string rerr;
            const bool sent = shut.readerRun(origin, "load", files, run, rerr);
            check(sent && run.outcome == rp::RO_GATE_CLOSED && run.key.empty(),
                  "M3b on a shut peer a reader produces nothing - so there is no key, "
                  "no document, and nothing to measure");
            // ...and the container, on the very same process.
            remote::NpzScan sc;
            std::string nerr;
            const bool listed = shut.npzScan(many, sc, nerr);
            check(listed && !sc.members.empty(),
                  "M3c ...while the same shut peer still lists a container");
            remote::MeasureReq q;
            q.op = rp::MOP_TEMPORAL_STATS;
            q.hasKeyed = true;
            q.keyed.key = sc.key;
            q.keyed.node = listed && !sc.members.empty() ? (int)sc.members[0].entry : 0;
            q.keyed.kind = remote::KeyedRef::FromNpz;
            remote::MeasureResult res;
            std::string merr;
            const bool got = shut.measure(q, res, merr);
            double shutSigT = 0;
            if (got && !res.cols.empty())
                for (const auto& it : res.cols[0])
                    if (it.kind == 0 && it.key == "sigma_t [DN]") shutSigT = it.num;
            fprintf(stderr, "rmeasureselftest:   M3 shut peer measure: got=%d frames=%d "
                            "sigma_t %.12g err=\"%s\"\n",
                    got ? 1 : 0, res.framesUsed, shutSigT, merr.c_str());
            check(got && res.framesUsed == SF && shutSigT == sigT[0],
                  "M3d ...and MEASURES its member, to the same number: the flag guards "
                  "code, not data");
            shut.stop();
        }

        // ---- M4: a peer that predates the flag is refused BEFORE sending -------
        // The sharpest of the sentences in remote_proto.h. A v13 peer reads the
        // head it knows and never reads the key behind the rois; had the request
        // also carried the origin path, it would have answered with a real
        // statistic over the whole container. So no path is sent, and the client
        // refuses from the number rather than learning it by asking.
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
            const bool listed = ses.npzScan(cont, sc, err);
            // The container's pixels sit at entries 15 and 19 - a NON-ZERO
            // address, which is the only kind that can tell "the node was
            // carried" from "the node was ignored".
            uint32_t node = 0;
            for (const auto& m : sc.members)
                if (m.name == "__pixels_3") node = (uint32_t)m.entry;
            check(listed && node > 3, "M5b (setup) the container lists, and its second "
                                      "stack's address is neither 0 nor its tree index");

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
            // ...and it really IS that op. A count of frames and a non-empty
            // series list are true of the temporal op as well, so what is
            // checked is the thing only this op emits: one point per frame of
            // the ROI's own mean, at values computed here from the fixture.
            const remote::MeasureSeries* rm = nullptr;
            const remote::MeasureSeries* rv = nullptr;
            for (const auto& s : roi.series) {
                if (s.name == "roi mean") rm = &s;
                if (s.name == "roi var")  rv = &s;
            }
            double worst = 0;
            if (rm && rm->ys.size() == (size_t)TF)
                for (int f = 0; f < TF; f++) {
                    double s1 = 0;
                    for (size_t i = 0; i < (size_t)TH * TWB; i++)
                        s1 += cubeB[(size_t)f * TH * TWB + i];
                    worst = std::max(worst, rel(rm->ys[(size_t)f],
                                                (float)(s1 / ((double)TH * TWB))));
                }
            fprintf(stderr, "rmeasureselftest:   M5 roi series: mean=%d(%zu) var=%d(%zu) "
                            "worst rel %.3g\n",
                    rm ? 1 : 0, rm ? rm->ys.size() : 0, rv ? 1 : 0, rv ? rv->ys.size() : 0,
                    worst);
            check(gotRoi && roi.framesUsed == TF && rm && rv &&
                      rm->ys.size() == (size_t)TF && rv->ys.size() == (size_t)TF &&
                      rm->xs.size() == (size_t)TF && worst <= 1e-6,
                  "M5c the per-frame statistics reach a member too - one ROI mean per "
                  "frame, at the values this fixture holds");

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

            // two SUBJECTS. The wire cannot say it, so a client that filled both
            // would have had its file list silently dropped.
            remote::MeasureReq twoSubjects = q;
            twoSubjects.op = rp::MOP_TEMPORAL_STATS;
            twoSubjects.paths = { origin };
            std::string e5;
            check(!ses.measure(twoSubjects, roi, e5) &&
                      e5 == "a keyed measurement addresses one array, not a list of files",
                  "M5f a key beside a path list is refused, not quietly resolved to one");

            // a key this peer never issued: refused by name, in a sentence that
            // does not claim to know who issued it
            remote::MeasureReq gone = q;
            gone.op = rp::MOP_TEMPORAL_STATS;
            gone.keyed.key = "0000000000000000";
            std::string e4;
            const bool answered = ses.measure(gone, roi, e4);
            fprintf(stderr, "rmeasureselftest:   M5 unknown key: \"%s\"\n", e4.c_str());
            check(!answered && e4 == rp::keyGoneText(),
                  "M5g an unresolvable key is refused without guessing at its issuer");
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
