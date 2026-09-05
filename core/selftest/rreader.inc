// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- READERS RUN ON THE PEER (issue #180, docs/features/remote/remote-reader-design.md) ---
    //
    // docs/features/adapters/input-adapters.md §4.13.1 settled on 2026-08-03 that an adapter runs
    // where the file lives. This is the door, stages 1 and 2: the RUN op and
    // the gate, then a document opening over the link out of what a reader
    // produced on the far side.
    //
    // A REAL PEER PROCESS, always, and here the reason is not a nicety - it is
    // the claim under test. "The reader ran in the PEER's python" cannot be
    // told from "it ran in mine" by a client talking to itself, and every
    // sentence below about a machine would be vacuous. So --remote-exe and the
    // standalone viewer-serve binary, exactly as rwatch and rtemporal do it.
    //
    // NOTHING HERE IS TIMED. The peer's cache is observed by its FILE (does it
    // exist, did its mtime move), which proves that the reader/harness did not
    // run again. The environment provenance probe deliberately still starts
    // Python once for every RUN, including a cache hit.
    if (!g_rreaderSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "rreaderselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto has = [](const std::string& hay, const std::string& needle) {
            return hay.find(needle) != std::string::npos;
        };
        // The peer reads its settings out of the environment (it has no window
        // to put them in), and it is a CHILD of this process - so this is how a
        // test says "that peer has no python" or "keep your cache here".
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };

        std::string root = g_rreaderSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/rreader-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(root), ec);
        const std::string cacheDir = root + "/peer-cache";
        std::filesystem::create_directories(pathFromUtf8(cacheDir), ec);
        setEnv("VIEWER_SERVE_CACHE", cacheDir);
        setEnv("VIEWER_SERVE_PYTHON", "");

        auto writeText = [&](const std::string& p, const std::string& s) {
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            f.write(s.data(), (std::streamsize)s.size());
        };

        // ---- the fixture: a 5-D .npy nothing native reads ---------------------
        // Deliberately a shape §3.2 refuses (rank 5 has no native form), so the
        // reader is not a convenience here - it is the ONLY way this file
        // becomes a document, on either machine. u2 because stage 2's assertion
        // is bit-identity and a u16 is exact in float32 by construction, so a
        // difference could only come from the transport.
        const int S = 3, H = 8, W = 6;
        const std::string origin = root + "/cube.npy";
        {
            std::vector<uint16_t> px((size_t)S * H * W * 1 * 1);
            for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(i * 7 + 11);
            std::vector<uint8_t> b = selftestNpyBytes("<u2", { S, H, W, 1, 1 },
                                                      px.data(), px.size() * 2);
            std::ofstream f(pathFromUtf8(origin), std::ios::binary);
            f.write((const char*)b.data(), (std::streamsize)b.size());
        }
        const std::string rdPerm = root + "/perm.py";
        writeText(rdPerm,
                  "import numpy as np\n"
                  "from viewer_import import Stack\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    a = np.load(path)\n"
                  "    return Stack(np.ascontiguousarray(a.reshape(a.shape[0], a.shape[1],\n"
                  "                                                a.shape[2])),\n"
                  "                 name='cube', note='reshaped 5-D to (F,H,W)')\n");
        const std::string rdRaise = root + "/boom.py";
        writeText(rdRaise,
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    raise ValueError('boom-1234: this reader was asked to fail')\n");
        const std::string rdWideCh = root + "/wide.py";
        writeText(rdWideCh,
                  "import numpy as np\n"
                  "from viewer_import import Stack\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    a = np.load(path).reshape(2, 4, 3, 6)\n"
                  "    return Stack(np.ascontiguousarray(a))\n");
        // A SERIES: two stacks under one varied condition, with a unit. Stage 3
        // is about the TREE crossing the link, and a series is the smallest
        // thing that has one - a batch, a series node carrying the conditions,
        // and two stacks under it, each addressed by its own node number.
        const std::string rdSeries = root + "/sweep.py";
        writeText(rdSeries,
                  "import numpy as np\n"
                  "from viewer_import import Series, Stack, Values\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    a = np.load(path).reshape(3, 8, 6)\n"
                  "    s0 = Stack(np.ascontiguousarray(a[:2]), name='e2')\n"
                  "    s1 = Stack(np.ascontiguousarray(a[1:]), name='e8')\n"
                  "    return Series([s0, s1],\n"
                  "                  conditions=Values([2.0, 8.0], name='exposure',\n"
                  "                                    unit='ms'))\n");
        const std::string rdSet = root + "/withset.py";
        writeText(rdSet,
                  "import numpy as np\n"
                  "from viewer_import import AnalysisSet, Stack\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    a = np.load(path).reshape(3, 8, 6)\n"
                  "    return AnalysisSet({'dark': Stack(np.ascontiguousarray(a)),\n"
                  "                        'flat': Stack(np.ascontiguousarray(a))})\n");
        // Phase 4's complete boundary in one return value: transposed Frame and
        // Stack children in a MIXED Series. Values are axis-coded so an HWC
        // memcpy wearing a CHW/FCHW header cannot accidentally pass parity.
        const std::string rdTyped = root + "/typed.py";
        writeText(rdTyped,
                  "import numpy as np\n"
                  "from viewer_import import Frame, Series, Stack, Values\n"
                  "VERSION = 1\n"
                  "def load(path):\n"
                  "    f = (100 + np.arange(3*2*4, dtype=np.uint16)).reshape(3,2,4)\n"
                  "    s = (1000 + np.arange(2*2*2*3, dtype=np.uint16)).reshape(2,2,2,3)\n"
                  "    one = (2000 + np.arange(1*2*2*3, dtype=np.uint16)).reshape(1,2,2,3)\n"
                  "    return Series([Frame(f, layout='CHW', name='colour'),\n"
                  "                   Stack(s, layout='FCHW', name='burst'),\n"
                  "                   Stack(one, layout='FCHW', name='singleton')],\n"
                  "                  conditions=Values([1.25, 9.5, 11.0], 'exposure', 'ms'),\n"
                  "                  name='mixed typed')\n");
        // Blank-layout typed Stack whose last axis is <=4. Native shape
        // inference calls this one HWC Frame, so its layer declaration itself
        // is protocol-15 evidence even though no named layout is present.
        const std::string rdNarrowA = root + "/narrow_a.py";
        const std::string rdNarrowB = root + "/narrow_b.py";
        auto writeNarrow = [&](const std::string& path, int base, const char* name) {
            writeText(path,
                      "import numpy as np\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    a = (" + std::to_string(base) +
                      " + np.arange(2*2002*4, dtype=np.uint16)).reshape(2,2002,4)\n"
                      "    return Stack(a, name='" + std::string(name) + "')\n");
        };
        writeNarrow(rdNarrowA, 300, "narrow-a");
        writeNarrow(rdNarrowB, 40000, "narrow-b");

        const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
        const std::string url = "local://" + origin;
        // The three texts the op carries, read the way the client reads them.
        std::string harness = runAdapterScript();
        std::string files[3];
        auto slurp = [&](const std::string& p, std::string& into) {
            std::ifstream f(pathFromUtf8(p), std::ios::binary);
            if (!f) return false;
            into.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return true;
        };
        bool haveTexts = !harness.empty() && slurp(rdPerm, files[0]) &&
                         slurp((pathFromUtf8(harness).parent_path() / "viewer_import.py").u8string(),
                               files[1]) &&
                         slurp(harness, files[2]);
        check(haveTexts, "R0 the three texts a RUN carries are all readable here");
        if (!haveTexts) {
            fprintf(stderr, "rreaderselftest: FAILED (no harness)\n");
            return 1;
        }

        auto openSession = [&](remote::Session& s, bool gateOpen) {
            s.setServeReaders(gateOpen);
            std::string serr;
            if (!s.startOn("", 0, exe, serr)) {
                fprintf(stderr, "rreaderselftest: cannot start the peer (%s): %s\n",
                        exe.c_str(), serr.c_str());
                return false;
            }
            return true;
        };
        auto cacheFiles = [&]() {
            std::vector<std::filesystem::path> out;
            std::error_code e;
            for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), e)) {
                const std::string name = d.path().filename().u8string();
                if (name.size() >= 8 &&
                    name.compare(name.size() - 8, 8, ".vstream") == 0)
                    out.push_back(d.path());
            }
            return out;
        };

        std::string keyPerm;
        {   // ---- R1: the reader runs THERE and leaves its result THERE --------
            remote::Session ses;
            if (!openSession(ses, true)) return 1;
            check(ses.peerVersion() == (int)rp::VERSION,
                  "R1a the peer speaks the protocol this build does (15)");
            remote::ReaderRun run;
            std::string err;
            const bool sent = ses.readerRun(origin, "load", files, run, err);
            if (!sent || run.outcome != rp::RO_OK)
                fprintf(stderr,
                        "rreaderselftest:   R1 RUN diagnostic: sent=%d outcome=%d "
                        "transport=\"%s\" err=\"%s\" stderr=\"%s\" provenance=\"%s\"\n",
                        sent ? 1 : 0, sent ? (int)run.outcome : -1, err.c_str(),
                        run.err.c_str(), run.stderrText.c_str(), run.provenance.c_str());
            check(sent, "R1b the RUN reached the peer");
            check(sent && run.outcome == rp::RO_OK, "R1c ...and the reader ran: outcome 0");
            check(sent && has(run.stderrText, "1 stack"),
                  "R1d the harness's summary comes back whole (\"read 1 stack\")");
            check(sent && has(run.provenance, "Python") && has(run.provenance, "numpy"),
                  "R1e ...with WHICH python ran it, named");
            check(sent && !run.key.empty() && !run.header.empty(),
                  "R1f a key and the .vstream header verbatim");
            check(sent && has(run.header, "VIEWERSTREAM ") && has(run.header, "layer 0 stack"),
                  "R1g the header is the harness's own text, not a re-rendering");
            const auto files1 = cacheFiles();
            check(files1.size() == 1,
                  "R1h the materialisation is one .vstream on the PEER's disk");
            // ...and NOT here: the whole of §4.13.1's second line is that the
            // converted array does not cross the wire. What crossed so far is
            // a header of a few hundred bytes.
            check(run.header.size() < 4096,
                  "R1i what crossed the wire is the header, not the array");
            keyPerm = run.key;

            // (f) The three texts were written into a directory of their own on
            // the peer and left with it. Nothing of the client's code stays.
            std::error_code te;
            int strays = 0;
            for (auto& d : std::filesystem::directory_iterator(
                     std::filesystem::temp_directory_path(te), te))
                if (d.path().filename().u8string().compare(0, 20, "viewer_serve_reader_") == 0)
                    strays++;
            check(strays == 0, "R1j the carried reader is gone from the peer's temp dir");

            {   // ---- R2: a hit skips the reader, not the provenance probe ------
                // Guard the indexed diagnostic even after R1h has recorded a
                // failure. A selftest should report the cause, not crash while
                // trying to gather the next piece of evidence.
                const bool haveCacheFile = files1.size() == 1;
                const auto before = haveCacheFile
                    ? std::filesystem::last_write_time(files1[0], ec)
                    : std::filesystem::file_time_type{};
                const uintmax_t beforeSize = haveCacheFile
                    ? std::filesystem::file_size(files1[0], ec) : 0;
                remote::ReaderRun again;
                std::string e2;
                const bool got = ses.readerRun(origin, "load", files, again, e2);
                check(got && again.outcome == rp::RO_OK && again.key == keyPerm,
                      "R2a the same RUN answers with the same key");
                check(got && has(again.stderrText,
                                 "reader and harness were not re-run; Python ran only the "
                                 "environment provenance probe"),
                      "R2b ...and distinguishes the skipped reader from the required probe");
                const auto after = haveCacheFile
                    ? std::filesystem::last_write_time(files1[0], ec)
                    : std::filesystem::file_time_type{};
                const uintmax_t afterSize = haveCacheFile
                    ? std::filesystem::file_size(files1[0], ec) : 0;
                check(haveCacheFile && before == after && beforeSize == afterSize,
                      "R2c the cache file did not move: the reader/harness did not run");
                check(cacheFiles().size() == 1, "R2d ...and no second copy was made");
            }

            {   // ---- R6: an exception comes back WHOLE ------------------------
                std::string bf[3] = { std::string(), files[1], files[2] };
                check(slurp(rdRaise, bf[0]), "R6a (setup) the raising reader is readable");
                remote::ReaderRun bad;
                std::string e3;
                const bool got = ses.readerRun(origin, "load", bf, bad, e3);
                check(got && bad.outcome == rp::RO_EXITED,
                      "R6b a reader that raises is outcome 5, not a link error");
                check(got && has(bad.stderrText, "Traceback") &&
                          has(bad.stderrText, "boom-1234"),
                      "R6c the traceback arrives verbatim, message and all");
                check(got && bad.key.empty(), "R6d ...and no key: nothing was produced");
                check(cacheFiles().size() == 1,
                      "R6e a failed run leaves no .vstream materialisation behind");
            }

            {   // ---- R7: Python rejects an impossible typed object before it
                //          can become a misleading carrier ---------------------
                std::string wf[3] = { std::string(), files[1], files[2] };
                check(slurp(rdWideCh, wf[0]), "R7a (setup) the 6-channel reader is readable");
                remote::ReaderRun wide;
                std::string e4;
                const bool got = ses.readerRun(origin, "load", wf, wide, e4);
                const std::string direct =
                    "Stack: layout \"FHWC\" reads C=6 from shape (2, 4, 3, 6) -- "
                    "the viewer accepts C=1..4";
                check(got && wide.outcome == rp::RO_EXITED &&
                          wide.err == "the reader exited with status 2",
                      "R7b typed C>4 is rejected by the carried Python boundary");
                check(got && has(wide.stderrText, direct),
                      "R7c ...with the declared layer/layout/shape and C=1..4 ceiling");
            }
            ses.stop();
        }

        {   // ---- R3: a peer that predates the op is refused BEFORE sending ----
            // The client writes this from the number in HELLO. It must not be
            // learned by asking: a v11 peer does not refuse the META trailer,
            // it never reads it and answers with the ORIGIN's pixels instead.
            setEnv("VIEWER_SERVE_PROTOCOL", "11");
            remote::Session old;
            if (!openSession(old, true)) return 1;
            check(old.peerVersion() == 11, "R3a (setup) the peer is being a v11");
            remote::ReaderRun run;
            std::string err;
            const bool sent = old.readerRun(origin, "load", files, run, err);
            check(!sent && err == rp::readerTooOldText(11, origin),
                  "R3b a reader on a v11 peer is refused by the CLIENT, from the number");
            // ...and the trailer never goes either, for the sharper reason.
            remote::KeyedRef ref{ "0123456789abcdef", 0 };
            remote::Meta m;
            std::string merr;
            check(!old.meta(origin, m, merr, 0, nullptr, &ref) &&
                      merr == rp::readerTooOldText(11, origin),
                  "R3c ...and so is a META that would carry a reader trailer");
            // The peer's OWN answer, for the record: a different sentence again,
            // and the one a typo would also get - which is why the client does
            // not wait for it (remote_proto.h's VERSION note).
            old.stop();
            setEnv("VIEWER_SERVE_PROTOCOL", "");
        }

        {   // ---- R4: the gate is CLOSED unless the launcher opened it ---------
            remote::Session shut;
            if (!openSession(shut, false)) return 1;
            check(shut.peerVersion() == (int)rp::VERSION,
                  "R4a (setup) a current peer, started without --serve-readers");
            remote::ReaderRun run;
            std::string err;
            const bool sent = shut.readerRun(origin, "load", files, run, err);
            check(sent && run.outcome == rp::RO_GATE_CLOSED,
                  "R4b a closed gate is an OUTCOME, not a link failure");
            check(sent && run.err == rp::readerGateClosedText(),
                  "R4c ...and it names the flag and whose decision it is");
            check(run.err != rp::readerTooOldText(11, origin) &&
                      !has(run.err, "protocol") && !has(run.err, "unknown request"),
                  "R4d the three states read as three different sentences");
            shut.stop();
        }

        {   // ---- R5: no Python THERE says so, and says where -----------------
            // VIEWER_SERVE_PYTHON is the operator's choice of interpreter, and
            // the peer honours it strictly: no settings window over there means
            // a silent fall-back to some other python is a result nobody can
            // reproduce.
            setEnv("VIEWER_SERVE_PYTHON", root + "/no-such-python");
            remote::Session nopy;
            if (!openSession(nopy, true)) return 1;
            remote::ReaderRun run;
            std::string err;
            const bool sent = nopy.readerRun(origin, "load", files, run, err);
            check(sent && run.outcome == rp::RO_NO_PYTHON,
                  "R5a an interpreter that is not there is outcome 2");
            check(sent && has(run.err, "VIEWER_SERVE_PYTHON") && has(run.err, "no-such-python"),
                  "R5b ...naming the setting and the path, which is what a person can fix");
            const std::string shown = readerNoPythonText(
                " on the local peer", run.err,
                "install numpy for it, or set VIEWER_SERVE_PYTHON where viewer-serve runs");
            check(has(shown, "on the local peer") && has(shown, "VIEWER_SERVE_PYTHON where"),
                  "R5c the sentence the user reads names the MACHINE and the fix over there");
            nopy.stop();
            setEnv("VIEWER_SERVE_PYTHON", "");
        }

        {   // ---- R7d: the two doors, on the same bytes ------------------------
            // A materialisation the peer will refuse, written into its cache by
            // hand so that BOTH doors can be asked about the same file. The
            // peer answers through META (openReaderCache -> serveLayout); this
            // process answers through loadViewerStream. Same sentence or the
            // shared TU has stopped being shared.
            const std::string key = "deadbeefdeadbee0";
            const std::string path = cacheDir + "/" + key + ".vstream";
            std::string hdr = "VIEWERSTREAM 1\nn 1\nlayer 0 frame\nparent 0 -1\n"
                              "pixels 0 u2 5 2 2 3 4 5 480\nend\n";
            {
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write(hdr.data(), (std::streamsize)hdr.size());
                std::vector<uint8_t> zero(480, 0);
                f.write((const char*)zero.data(), 480);
            }
            remote::Session ses;
            if (!openSession(ses, true)) return 1;
            remote::KeyedRef ref{ key, 0 };
            remote::Meta m;
            std::string perr;
            const bool got = ses.meta(url, m, perr, 0, nullptr, &ref);
            FrameSource st;
            const std::string lerr = loadViewerStream(path, "x.py:load", origin, st);
            check(!got, "R7d a rank-5 node is refused by the peer");
            const std::string rank5Err =
                "node 0: " + rp::npyNotNativeText({ 2, 2, 3, 4, 5 });
            check(perr == rank5Err,
                  "R7e ...with §3.2's sentence");
            check(lerr == rank5Err,
                  "R7f ...and the local door refuses the same bytes with the same sentence");

            // Construction normally catches this one (R7b/c). A hand-written
            // old carrier proves the C++ materialiser independently enforces
            // the same C=1..4 boundary rather than trusting its producer.
            const std::string wideKey = "deadbeefdeadbee1";
            const std::string widePath = cacheDir + "/" + wideKey + ".vstream";
            std::string wideHdr = "VIEWERSTREAM 1\nn 1\nlayer 0 stack\nparent 0 -1\n"
                                  "pixels 0 u2 4 2 4 3 6 288\nend\n";
            {
                std::ofstream f(pathFromUtf8(widePath), std::ios::binary);
                f.write(wideHdr.data(), (std::streamsize)wideHdr.size());
                std::vector<uint8_t> zero(288, 0);
                f.write((const char*)zero.data(), 288);
            }
            remote::KeyedRef wideRef{ wideKey, 0 };
            std::string widePeerErr;
            FrameSource wideStat;
            const bool wideGot = ses.meta(url, m, widePeerErr, 0, nullptr, &wideRef);
            const std::string wideLocalErr =
                loadViewerStream(widePath, "x.py:load", origin, wideStat);
            const std::string wideWant =
                "node 0: " + rp::npyChannelCeilingText({ 2, 4, 3, 6 }, 6);
            check(!wideGot && widePeerErr == wideWant && wideLocalErr == wideWant,
                  "R7f2 a hand-written C>4 carrier is refused identically by both C++ doors");

            // Whole-declaration preflight. Missing and duplicate pixels lines
            // used to be worse than a plain refusal: the client could choose
            // the last duplicate while the peer chose the first, and a later
            // missing node could be discovered after an earlier doc landed.
            auto writeBadCarrier = [&](const std::string& p, const std::string& h,
                                       size_t bytes) {
                std::ofstream f(pathFromUtf8(p), std::ios::binary);
                f.write(h.data(), (std::streamsize)h.size());
                std::vector<uint8_t> zero(bytes, 0);
                f.write((const char*)zero.data(), (std::streamsize)zero.size());
            };
            const std::string missingKey = "deadbeefdeadbee2";
            const std::string missingPath = cacheDir + "/" + missingKey + ".vstream";
            writeBadCarrier(missingPath,
                "VIEWERSTREAM 1\nn 3\nlayer 0 batch\nparent 0 -1\n"
                "layer 1 frame\nparent 1 0\npixels 1 u2 2 2 2 8\n"
                "layer 2 frame\nparent 2 0\nend\n", 8);
            remote::KeyedRef missingRef{ missingKey, 1 };
            std::string missingPeer;
            const bool missingGot = ses.meta(url, m, missingPeer, 0, nullptr, &missingRef);
            closeAll();
            FrameSource missingStat;
            const std::string missingLocal =
                loadViewerStream(missingPath, "x.py:load", origin, missingStat);
            const std::string missingWant =
                "node 2 is a frame with 0 pixels declaration(s): exactly one is required";
            check(!missingGot && missingPeer == missingWant &&
                      missingLocal == missingWant && app.images.empty(),
                  "R7h a missing pixels node is refused whole and identically before any doc");

            const std::string duplicateKey = "deadbeefdeadbee3";
            const std::string duplicatePath = cacheDir + "/" + duplicateKey + ".vstream";
            writeBadCarrier(duplicatePath,
                "VIEWERSTREAM 1\nn 1\nlayer 0 frame\nparent 0 -1\n"
                "pixels 0 u2 2 2 2 8\npixels 0 u2 2 2 2 8\nend\n", 16);
            remote::KeyedRef duplicateRef{ duplicateKey, 0 };
            std::string duplicatePeer;
            const bool duplicateGot =
                ses.meta(url, m, duplicatePeer, 0, nullptr, &duplicateRef);
            closeAll();
            FrameSource duplicateStat;
            const std::string duplicateLocal =
                loadViewerStream(duplicatePath, "x.py:load", origin, duplicateStat);
            const std::string duplicateWant =
                "node 0 is a frame with 2 pixels declaration(s): exactly one is required";
            check(!duplicateGot && duplicatePeer == duplicateWant &&
                      duplicateLocal == duplicateWant && app.images.empty(),
                  "R7i duplicate pixels are refused whole, never first/last by endpoint");
            // A key that names nothing is a refusal too, and not a crash.
            remote::KeyedRef gone{ "0000000000000000", 0 };
            std::string gerr;
            // ...and the sentence names the MATERIALISATION, not a reader: an
            // unresolvable key has no knowable issuer (rp::keyGoneText), and
            // "run the reader again" was correct for half of them.
            check(!ses.meta(url, m, gerr, 0, nullptr, &gone) && gerr == rp::keyGoneText(),
                  "R7g a key the peer has never issued is refused by name");
            ses.stop();
            std::filesystem::remove(pathFromUtf8(path), ec);
            std::filesystem::remove(pathFromUtf8(widePath), ec);
            std::filesystem::remove(pathFromUtf8(missingPath), ec);
            std::filesystem::remove(pathFromUtf8(duplicatePath), ec);
        }

        // ---- stage 2: a document, over the link, out of what a reader made ----
        {
            const size_t before = app.images.size();
            const std::string spec = rdPerm + ":load";
            const std::string rerr = openRemoteWithReader(url, spec);
            check(rerr.empty(), "R8a the peer's reader output opens as a document");
            const size_t remoteFirst = before;
            const size_t remoteCount = app.images.size() - before;
            check(remoteCount == (size_t)S,
                  "R8b ...as the stack the reader declared, frame for frame");
            check(readerFor(url) == spec,
                  "R8c the memo is keyed on the WHOLE url, so the Inspector finds it");
            check(app.readerLastOk && has(app.readerLastOut, "on the local peer"),
                  "R8d the panel says which machine it ran on");

            // ...and the SAME reader over the SAME file, run here. Bit-identity
            // is the acceptance condition of stage 2: one machine, one python,
            // one decoder - so anything but equality is the transport changing
            // a number, which is the one thing it may never do.
            const size_t localFirst = app.images.size();
            const std::string lerr = openWithReader(origin, spec, false);
            check(lerr.empty(), "R8e the same reader, run HERE, opens the same file");
            const size_t localCount = app.images.size() - localFirst;
            check(localCount == remoteCount, "R8f ...into the same number of frames");
            bool identical = localCount == remoteCount && remoteCount > 0;
            bool sameDtype = identical;
            for (size_t k = 0; k < remoteCount && identical; k++) {
                const ImageDoc* r = app.images[remoteFirst + k].get();
                const ImageDoc* l = app.images[localFirst + k].get();
                if (r->w != l->w || r->h != l->h || r->ch != l->ch) { identical = false; break; }
                if (r->src->data.size() != l->src->data.size()) { identical = false; break; }
                if (memcmp(r->src->data.data(), l->src->data.data(),
                           r->src->data.size() * sizeof(float)) != 0)
                    identical = false;
                if (r->src->dtype != l->src->dtype) sameDtype = false;
            }
            check(identical, "R8g every pixel is BIT-IDENTICAL to the local run");
            // The design writes this one as "u2 は両側 u2"; the SOURCE dtype a
            // document displays is spelled the way core/app/loader_npz.inc
            // spells it ("u16" for a numpy u2), and that spelling is what has
            // to match. The claim is the same one either way: a reader's array
            // does not change type by being read through a link.
            check(sameDtype && !app.images.empty() &&
                      app.images[remoteFirst]->src->dtype == "u16",
                  "R9 ...and the dtype reads the same on both sides (u16, from a numpy u2)");
        }

        {   // ---- stage 3: the TREE crosses the link ---------------------------
            //
            // Stage 2 opened a single-rooted stack, and the node number in the
            // trailer was always 0 by construction. A series has four nodes and
            // two of them carry pixels, so this is the first time [key][node]
            // addresses anything: node 2 and node 3 are DIFFERENT arrays inside
            // one materialisation, and the peer has to answer for each.
            //
            // What is asserted is the same thing R16 asserts for a native .npz
            // container, and that is the point of asserting it twice: the
            // declaration travels verbatim and ONE builder on this side turns
            // it into documents, so a reader's tree and a container's tree
            // cannot come out differently.
            const std::string spec = rdSeries + ":load";
            closeAll();
            const size_t rFirst = app.images.size();
            const std::string rerr = openRemoteWithReader(url, spec);
            const size_t rDocs = app.images.size() - rFirst;
            std::vector<double> rVals;
            std::string rUnit, rParam;
            if (!app.series.empty()) {
                for (const auto& mm : app.series.back().members) rVals.push_back(mm.value);
                rUnit = app.series.back().unit;
                rParam = app.series.back().paramName;
            }
            check(rerr.empty() && rDocs == 4,
                  "R12a a series reader opens over the link, both stacks of it");
            closeAll();
            const size_t lFirst = app.images.size();
            const std::string lerr = openWithReader(origin, spec, false);
            const size_t lDocs = app.images.size() - lFirst;
            std::vector<double> lVals;
            std::string lUnit, lParam;
            if (!app.series.empty()) {
                for (const auto& mm : app.series.back().members) lVals.push_back(mm.value);
                lUnit = app.series.back().unit;
                lParam = app.series.back().paramName;
            }
            fprintf(stderr, "rreaderselftest:   R12 remote %zu doc(s) unit=\"%s\"; "
                            "local %zu doc(s) unit=\"%s\"\n",
                    rDocs, rUnit.c_str(), lDocs, lUnit.c_str());
            check(lerr.empty() && lDocs == rDocs,
                  "R12b ...into the same number of documents the local run makes");
            check(rVals == lVals && rVals.size() == 2,
                  "R12c the series conditions are the same values on both sides");
            check(rUnit == lUnit && rUnit == "ms" && rParam == lParam && rParam == "exposure",
                  "R12d ...and so are the condition's name and unit");
            closeAll();

            // ---- R13: a node number that names nothing ----------------------
            // The address space is the peer's and it says so by name rather
            // than clamping onto some other node's pixels - which is the same
            // call peerStackGet makes for a frame past the end (§5.1: NULL is
            // the one honest failure, and a clamp answers a question about
            // frame 9 with frame 2).
            remote::Session ses;
            if (!openSession(ses, true)) return 1;
            remote::ReaderRun run;
            std::string err;
            std::string sf[3] = { std::string(), files[1], files[2] };
            check(slurp(rdSeries, sf[0]), "R13a (setup) the series reader is readable");
            const bool sent = ses.readerRun(origin, "load", sf, run, err);
            check(sent && run.outcome == rp::RO_OK, "R13b (setup) it ran on the peer");
            remote::Meta m;
            std::string good, bad;
            remote::KeyedRef okRef{ run.key, 2, remote::KeyedRef::FromReader };
            remote::KeyedRef offRef{ run.key, 99, remote::KeyedRef::FromReader };
            const bool okGot = ses.meta(url, m, good, 0, nullptr, &okRef);
            const bool offGot = ses.meta(url, m, bad, 0, nullptr, &offRef);
            fprintf(stderr, "rreaderselftest:   R13 node 99: \"%s\"\n", bad.c_str());
            check(okGot, "R13c node 2 of the tree is a real array");
            check(!offGot && has(bad, "node 99") && has(bad, "no pixels"),
                  "R13d ...and node 99 is refused BY NUMBER, never clamped onto one");
            ses.stop();
        }

        {   // ---- R21: typed axes + mixed Series, both carriers + session ------
            closeAll();
            const std::string spec = rdTyped + ":load";
            const std::string rerr = openRemoteWithReader(url, spec);
            check(rerr.empty() && app.images.size() == 4,
                  "R21a CHW/FCHW Reader output opens as Frame + 2-frame + singleton Stack");
            bool rShape = app.images.size() == 4 &&
                          app.images[0]->w == 4 && app.images[0]->h == 2 && app.images[0]->ch == 3 &&
                          app.images[1]->w == 3 && app.images[1]->h == 2 && app.images[1]->ch == 2 &&
                          app.images[2]->w == 3 && app.images[2]->h == 2 && app.images[2]->ch == 2 &&
                          app.images[3]->w == 3 && app.images[3]->h == 2 && app.images[3]->ch == 2;
            check(rShape, "R21b ...with Frame=CHW and Stack=FCHW axes, never native guesses");
            bool rPixels = rShape && app.images[0]->src->data.size() >= 4 &&
                           app.images[0]->src->data[0] == 100 &&
                           app.images[0]->src->data[1] == 108 &&
                           app.images[0]->src->data[2] == 116 &&
                           app.images[0]->src->data[3] == 101 &&
                           app.images[1]->src->data[0] == 1000 &&
                           app.images[1]->src->data[1] == 1006 &&
                           app.images[2]->src->data[0] == 1012 &&
                           app.images[2]->src->data[1] == 1018 &&
                           app.images[3]->src->data[0] == 2000 &&
                           app.images[3]->src->data[1] == 2006;
            check(rPixels, "R21c ...and transposition puts each channel/sample at the declared pixel");
            bool rMembers = app.series.size() == 1 && app.series[0].members.size() == 3 &&
                            app.series[0].members[0].seqId == 0 &&
                            app.series[0].members[0].frameUid == app.images[0]->uid &&
                            app.series[0].members[1].seqId == app.images[1]->seqId &&
                            app.series[0].members[1].frameUid == 0 &&
                            app.series[0].members[2].seqId == app.images[3]->seqId &&
                            app.series[0].members[2].frameUid == 0 &&
                            app.images[3]->seqId != 0 &&
                            app.images[3]->seqId != app.images[1]->seqId &&
                            app.series[0].members[0].value == 1.25 &&
                            app.series[0].members[1].value == 9.5 &&
                            app.series[0].members[2].value == 11.0;
            check(rMembers, "R21d mixed Series keeps Frame and singleton/multi Stack kinds in order");
            bool rBoundary = app.images.size() == 4 &&
                             app.images[0]->src->npyRead == NR_CHW &&
                             app.images[0]->src->remoteKeyRead == NR_NATIVE &&
                            app.images[0]->src->remoteKeyLayout == "CHW" &&
                             app.images[0]->src->remoteKeyRequires15 &&
                             app.images[1]->src->npyRead == NR_FCHW &&
                             app.images[1]->src->remoteKeyRead == NR_NATIVE &&
                             app.images[1]->src->remoteKeyLayout == "FCHW" &&
                             app.images[1]->src->remoteKeyRequires15 &&
                             app.images[3]->src->npyRead == NR_FCHW &&
                             app.images[3]->src->remoteKeyRead == NR_NATIVE &&
                             app.images[3]->src->remoteKeyLayout == "FCHW" &&
                             app.images[3]->src->remoteKeyRequires15;
            check(rBoundary,
                  "R21e logical axes survive beside a Reader's native wire reading");
            bool rNoReRead = app.images.size() == 4;
            for (const auto& d : app.images)
                rNoReRead = rNoReRead && !npyReReadAvailable(*d->src) &&
                            !d->src->npyShape.empty();
            check(rNoReRead,
                  "R21e2 typed Reader shows its read-as fact without a re-read affordance");

            std::vector<std::vector<float>> remotePixels;
            std::vector<std::tuple<int,int,int>> remoteDims;
            for (const auto& d : app.images) {
                remotePixels.push_back(d->src->data);
                remoteDims.emplace_back(d->w, d->h, d->ch);
            }
            // Save while these are still REMOTE sources. A restored reader
            // reconnects, so this is where losing layout/wire separation would
            // either turn identity native or let a later v14 peer serve it.
            const std::string sess = root + "/typed-mixed.vsession";
            { std::ofstream f(pathFromUtf8(sess), std::ios::binary); writeSessionTo(f); }
            std::ifstream sf(pathFromUtf8(sess), std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(sf)),
                                   std::istreambuf_iterator<char>());
            const size_t frameLine = body.find("seriesframe 1.25");
            const size_t stackLine = body.find("seriesmember 9.5");
            const size_t singletonLine = body.find("seriesmember 11");
            check(frameLine != std::string::npos && stackLine != std::string::npos &&
                      singletonLine != std::string::npos && frameLine < stackLine &&
                      stackLine < singletonLine &&
                      body.find("npyread " + std::to_string((int)NR_CHW)) != std::string::npos &&
                      body.find("npyread " + std::to_string((int)NR_FCHW)) != std::string::npos,
                  "R21f session writes mixed members and both logical typed readings");
            closeAll();
            const std::string sr = loadSession(sess);
            bool restored = sr.empty() && app.series.size() == 1 &&
                            app.series[0].members.size() == 3 &&
                            app.series[0].members[0].seqId == 0 &&
                            app.series[0].members[0].frameUid != 0 &&
                            app.series[0].members[1].seqId != 0 &&
                            app.series[0].members[1].frameUid == 0 &&
                            app.series[0].members[2].seqId != 0 &&
                            app.series[0].members[2].frameUid == 0 &&
                            app.series[0].members[0].value == 1.25 &&
                            app.series[0].members[1].value == 9.5 &&
                            app.series[0].members[2].value == 11.0 &&
                            app.images.size() == 4 &&
                            app.images[0]->src->npyRead == NR_CHW &&
                            app.images[0]->src->remoteKeyRead == NR_NATIVE &&
                             app.images[0]->src->remoteKeyLayout == "CHW" &&
                            app.images[0]->src->remoteKeyRequires15 &&
                            app.images[1]->src->npyRead == NR_FCHW &&
                            app.images[1]->src->remoteKeyRead == NR_NATIVE &&
                            app.images[1]->src->remoteKeyLayout == "FCHW" &&
                            app.images[1]->src->remoteKeyRequires15 &&
                            app.images[3]->seqId == app.series[0].members[2].seqId &&
                            app.images[3]->src->npyRead == NR_FCHW &&
                            app.images[3]->src->remoteKeyRead == NR_NATIVE &&
                            app.images[3]->src->remoteKeyLayout == "FCHW" &&
                            app.images[3]->src->remoteKeyRequires15;
            check(restored,
                  "R21g remote session restores membership, logical identity and wire layout");
            closeAll();

            const std::string lerr = openWithReader(origin, spec, false);
            bool parity = lerr.empty() && app.images.size() == remotePixels.size();
            for (size_t i = 0; i < app.images.size() && parity; i++)
                parity = std::make_tuple(app.images[i]->w, app.images[i]->h, app.images[i]->ch) ==
                             remoteDims[i] && app.images[i]->src->data == remotePixels[i] &&
                         !npyReReadAvailable(*app.images[i]->src) &&
                         !app.images[i]->src->npyShape.empty();
            check(parity, "R21h local stream and remote stream are dimension/pixel identical");
            closeAll();

            // One URL can legitimately have BOTH issuers: a Reader may ignore
            // the origin bytes and a Viewer-NPZ may materialise those same bytes.
            // Give both the same reserved member names, then order the groups
            // Reader -> NPZ -> Reader.  Without issuer-scoped, bounded restore
            // runs the final Reader line can borrow the intervening NPZ's
            // __pixels_1/__pixels_2 slots and its group never comes back.
            const std::string issuerPath = root + "/issuer-cross.npz";
            const std::string issuerUrl = "local://" + issuerPath;
            auto issuerI32 = [](int32_t v) {
                return selftestNpyBytes("<i4", {}, &v, sizeof(v));
            };
            auto issuerStr = [](const std::string& s) {
                const size_t n = s.empty() ? 1 : s.size();
                std::vector<uint32_t> cps(n, 0);
                for (size_t i = 0; i < s.size(); i++)
                    cps[i] = (uint32_t)(unsigned char)s[i];
                return selftestNpyBytes(("<U" + std::to_string(n)).c_str(), {},
                                        cps.data(), cps.size() * sizeof(uint32_t));
            };
            std::vector<uint16_t> issuerFrame(3 * 2 * 4);
            std::vector<uint16_t> issuerStack(2 * 2 * 2 * 3);
            for (size_t i = 0; i < issuerFrame.size(); i++)
                issuerFrame[i] = (uint16_t)(3000 + i);
            for (size_t i = 0; i < issuerStack.size(); i++)
                issuerStack[i] = (uint16_t)(4000 + i);
            const double issuerConditions[2] = { 2.0, 8.0 };
            selftestWriteNpz(issuerPath, {
                { "__viewer", issuerI32(3) },
                { "__n", issuerI32(3) },
                { "__layer_0", issuerStr("series") },
                { "__parent_0", issuerI32(-1) },
                { "__name_0", issuerStr("NPZ between Readers") },
                { "__conditions_values_0", selftestNpyBytes(
                    "<f8", { 2 }, issuerConditions, sizeof(issuerConditions)) },
                { "__conditions_name_0", issuerStr("exposure") },
                { "__conditions_unit_0", issuerStr("ms") },
                { "__layer_1", issuerStr("frame") },
                { "__parent_1", issuerI32(0) },
                { "__layout_1", issuerStr("CHW") },
                { "__pixels_1", selftestNpyBytes(
                    "<u2", { 3, 2, 4 }, issuerFrame.data(), issuerFrame.size() * 2) },
                { "__layer_2", issuerStr("stack") },
                { "__parent_2", issuerI32(0) },
                { "__layout_2", issuerStr("FCHW") },
                { "__pixels_2", selftestNpyBytes(
                    "<u2", { 2, 2, 2, 3 }, issuerStack.data(), issuerStack.size() * 2) },
            });
            const std::string crossReader1 = openRemoteWithReader(issuerUrl, spec);
            const size_t crossNpzAt = app.images.size();
            const bool crossNpz = crossReader1.empty() &&
                                  openRemoteNpz(issuerUrl, "__pixels_1");
            const size_t crossReader2At = app.images.size();
            const std::string crossReader2 = crossNpz
                ? openRemoteWithReader(issuerUrl, spec) : std::string("NPZ did not open");
            check(crossReader1.empty() && crossNpz && crossReader2.empty() &&
                      crossNpzAt == 4 && crossReader2At == 7 &&
                      app.images.size() == 11 && app.series.size() == 3 &&
                      app.images[4]->src->member == "__pixels_1" &&
                      app.images[7]->src->member == "__pixels_1",
                  "R21h2 one URL opens as Reader, NPZ, then Reader without dedupe");
            for (size_t i = 0; i < app.images.size(); i++) {
                const bool firstReader = i < crossNpzAt;
                const bool npzGroup = i >= crossNpzAt && i < crossReader2At;
                app.images[i]->black = firstReader ? 10.0f : npzGroup ? 20.0f : 30.0f;
                app.images[i]->white = firstReader ? 110.0f : npzGroup ? 120.0f : 130.0f;
                app.images[i]->displayLut = firstReader ? 1 : npzGroup ? 2 : 3;
            }
            // Leave NPZ __pixels_1 UNSAVED while keeping __pixels_2. Its restore
            // still rebuilds the whole container and therefore creates a free
            // __pixels_1 slot between Reader 1 and Reader 2. An issuer-blind or
            // unbounded Reader run will steal precisely that slot for Reader 2.
            if (app.images.size() == 11) closeImages({ (int)crossNpzAt });
            check(app.images.size() == 10 && app.series.size() == 3,
                  "R21h2b the NPZ run deliberately leaves an unsaved member slot");
            const std::string crossSession = root + "/issuer-cross.vsession";
            { std::ofstream f(pathFromUtf8(crossSession), std::ios::binary); writeSessionTo(f); }
            std::ifstream crossIn(pathFromUtf8(crossSession), std::ios::binary);
            const std::string crossBody((std::istreambuf_iterator<char>(crossIn)),
                                        std::istreambuf_iterator<char>());
            int crossReaderLines = 0, crossNpzLines = 0, crossRunLines = 0;
            for (size_t at = 0; (at = crossBody.find("materializedissuer reader", at)) !=
                 std::string::npos; at += strlen("materializedissuer reader"))
                crossReaderLines++;
            for (size_t at = 0; (at = crossBody.find("materializedissuer npz", at)) !=
                 std::string::npos; at += strlen("materializedissuer npz"))
                crossNpzLines++;
            std::vector<unsigned long long> crossRuns;
            std::istringstream crossLines(crossBody);
            std::string crossLine;
            while (std::getline(crossLines, crossLine)) {
                std::istringstream one(crossLine);
                std::string key;
                unsigned long long run = 0;
                one >> key >> run;
                if (key != "materializedrun" || run == 0) continue;
                crossRunLines++;
                if (std::find(crossRuns.begin(), crossRuns.end(), run) == crossRuns.end())
                    crossRuns.push_back(run);
            }
            check(crossReaderLines == 6 && crossNpzLines == 1 &&
                      crossRunLines == 7 && crossRuns.size() == 3,
                  "R21h3 the session distinguishes every issuer line and materialisation run");

            closeAll();
            const std::string crossRestore = loadSession(crossSession);
            const double crossSeriesStart = nowSec();
            while (nowSec() - crossSeriesStart < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int readerDocs = 0, npzDocs = 0;
            int state10 = 0, state20 = 0, state30 = 0;
            bool npzFreeSlotSafe = true;
            bool crossFacts = crossRestore.empty() && app.images.size() == 11 &&
                              app.series.size() == 4;
            for (const auto& d : app.images) {
                const bool fromReader =
                    d->src->materializedIssuer == remote::KeyedRef::FromReader &&
                    d->src->remoteKeyKind == remote::KeyedRef::FromReader;
                const bool fromNpz =
                    d->src->materializedIssuer == remote::KeyedRef::FromNpz &&
                    d->src->remoteKeyKind == remote::KeyedRef::FromNpz;
                readerDocs += fromReader ? 1 : 0;
                npzDocs += fromNpz ? 1 : 0;
                state10 += d->black == 10.0f && d->white == 110.0f &&
                           d->displayLut == 1 ? 1 : 0;
                state20 += d->black == 20.0f && d->white == 120.0f &&
                           d->displayLut == 2 ? 1 : 0;
                state30 += d->black == 30.0f && d->white == 130.0f &&
                           d->displayLut == 3 ? 1 : 0;
                if (fromNpz && d->src->member == "__pixels_1")
                    npzFreeSlotSafe = npzFreeSlotSafe &&
                        !(d->black == 30.0f && d->white == 130.0f &&
                          d->displayLut == 3);
                crossFacts = crossFacts && (fromReader || fromNpz);
            }
            int readerSeries = 0, npzSeries = 0;
            int readerState10 = 0, readerState30 = 0;
            int npzState20 = 0, npzFreeFrame = 0;
            for (const auto& s : app.series) {
                int issuer = -1;
                bool oneIssuer = !s.members.empty();
                bool all10 = !s.members.empty(), all30 = !s.members.empty();
                bool npzM1Seen = false, npzM1Safe = true;
                bool npzM2Seen = false, npzM2State20 = true;
                for (const auto& m : s.members) {
                    const ImageDoc* memberDoc = nullptr;
                    bool memberAny = false, member10 = true, member20 = true, member30 = true;
                    for (const auto& d : app.images) {
                        if ((m.frameUid != 0 && d->uid == m.frameUid) ||
                            (m.seqId != 0 && d->seqId == m.seqId)) {
                            if (!memberDoc) memberDoc = d.get();
                            memberAny = true;
                            member10 = member10 && d->black == 10.0f &&
                                d->white == 110.0f && d->displayLut == 1;
                            member20 = member20 && d->black == 20.0f &&
                                d->white == 120.0f && d->displayLut == 2;
                            member30 = member30 && d->black == 30.0f &&
                                d->white == 130.0f && d->displayLut == 3;
                        }
                    }
                    if (!memberDoc || !memberAny) { oneIssuer = false; continue; }
                    if (issuer < 0) issuer = memberDoc->src->materializedIssuer;
                    else if (issuer != memberDoc->src->materializedIssuer) oneIssuer = false;
                    all10 = all10 && member10 && !member20 && !member30;
                    all30 = all30 && member30 && !member10 && !member20;
                    if (memberDoc->src->materializedIssuer == remote::KeyedRef::FromNpz &&
                        memberDoc->src->member == "__pixels_1") {
                        npzM1Seen = true;
                        npzM1Safe = npzM1Safe && !member10 && !member20 && !member30;
                    }
                    if (memberDoc->src->materializedIssuer == remote::KeyedRef::FromNpz &&
                        memberDoc->src->member == "__pixels_2") {
                        npzM2Seen = true;
                        npzM2State20 = npzM2State20 && member20 &&
                            !member10 && !member30;
                    }
                }
                crossFacts = crossFacts && oneIssuer;
                if (oneIssuer && issuer == remote::KeyedRef::FromReader) {
                    readerSeries++;
                    readerState10 += all10 ? 1 : 0;
                    readerState30 += all30 ? 1 : 0;
                    crossFacts = crossFacts && (all10 || all30);
                }
                if (oneIssuer && issuer == remote::KeyedRef::FromNpz) {
                    npzSeries++;
                    const bool npzStackBound = !npzM1Seen && npzM2Seen && npzM2State20;
                    const bool npzFreeBound = npzM1Seen && npzM1Safe && !npzM2Seen;
                    npzState20 += npzStackBound ? 1 : 0;
                    npzFreeFrame += npzFreeBound ? 1 : 0;
                    crossFacts = crossFacts && (npzStackBound || npzFreeBound);
                }
            }
            fprintf(stderr, "rreaderselftest:   R21h4 err=\"%s\" docs=%zu series=%zu "
                            "issuer=%d/%d state=%d/%d/%d readerSeries=%d(%d/%d) "
                            "npzSeries=%d(stack=%d free=%d) facts=%d\n",
                    crossRestore.c_str(), app.images.size(), app.series.size(),
                    readerDocs, npzDocs, state10, state20, state30,
                    readerSeries, readerState10, readerState30,
                    npzSeries, npzState20, npzFreeFrame, crossFacts ? 1 : 0);
            check(crossFacts && readerDocs == 8 && npzDocs == 3 &&
                      state10 == 4 && state20 == 2 && state30 == 4 &&
                      npzFreeSlotSafe && readerSeries == 2 && npzSeries == 2 &&
                      readerState10 == 1 && readerState30 == 1 && npzState20 == 1 &&
                      npzFreeFrame == 1,
                  "R21h4 session restores three issuer-scoped runs plus the free-frame Series");
            forgetReader(issuerUrl);
            closeAll();

            std::string tf[3] = { std::string(), files[1], files[2] };
            check(slurp(rdTyped, tf[0]), "R21i (setup) the typed reader is readable");
            std::string oldFiles[3] = { tf[0], tf[1], tf[2] };
            auto replaceOnce = [](std::string& s, const std::string& a,
                                  const std::string& b) {
                const size_t p = s.find(a);
                if (p == std::string::npos) return false;
                s.replace(p, a.size(), b);
                return true;
            };
            const bool oldHarness =
                replaceOnce(oldFiles[2], "CONTAINER_VERSION = 3", "CONTAINER_VERSION = 1") &&
                replaceOnce(oldFiles[2], "STREAM_VERSION = 3", "STREAM_VERSION = 1");
            oldFiles[0] += "\n# distinct old-client compatibility run\n";
            check(oldHarness, "R21k (setup) the v14 client harness seam is a v1 writer");

            // Carrier and wire generations are distinct. A historical v14
            // peer reads carriers through v2, so today's unconditional v3
            // writer is refused whole before it can issue a key. With the v1
            // harness that actually shipped in that generation, canonical
            // pixels still work and only typed axes require protocol 15.
            setEnv("VIEWER_SERVE_PROTOCOL", "14");
            remote::Session old;
            if (!openSession(old, true)) return 1;
            remote::ReaderRun currentOnOld;
            std::string currentOldErr;
            const bool currentOldSent =
                old.readerRun(origin, "load", tf, currentOnOld, currentOldErr);
            check(currentOldSent && currentOnOld.outcome == rp::RO_UNREADABLE &&
                      currentOnOld.key.empty() && currentOnOld.header.empty() &&
                      currentOnOld.err == vns::versionError(3, 2),
                  "R21j current v3 Reader carrier is refused whole by a v14 peer");

            std::string oldPeerFiles[3] = { oldFiles[0], oldFiles[1], oldFiles[2] };
            oldPeerFiles[0] += "\n# distinct old-peer typed compatibility run\n";
            remote::ReaderRun run;
            std::string oe;
            const bool ran = old.readerRun(origin, "load", oldPeerFiles, run, oe);
            remote::KeyedRef ref{ run.key, 1, remote::KeyedRef::FromReader };
            ref.layout = "CHW";
            ref.requires15 = true;
            remote::Meta mm;
            std::string ge;
            const bool fetched = ran && run.outcome == rp::RO_OK &&
                                 old.meta(url, mm, ge, NR_NATIVE, nullptr, &ref);
            check(ran && run.outcome == rp::RO_OK && !fetched &&
                      ge == rp::typedAxesTooOldText(14, url, "Reader CHW"),
                  "R21j2 a typed v1 carrier reaches the v14 wire gate before META/TILE");

            std::string safeOldFiles[3] = { files[0], oldFiles[1], oldFiles[2] };
            safeOldFiles[0] += "\n# distinct old-peer canonical compatibility run\n";
            remote::ReaderRun safeOldRun;
            std::string safeOldErr;
            const bool safeOldSent =
                old.readerRun(origin, "load", safeOldFiles, safeOldRun, safeOldErr);
            remote::KeyedRef safeOldRef{
                safeOldRun.key, 0, remote::KeyedRef::FromReader };
            remote::Meta safeOldMeta;
            std::string safeOldMetaErr;
            const bool safeOldFetched = safeOldSent &&
                safeOldRun.outcome == rp::RO_OK &&
                old.meta(url, safeOldMeta, safeOldMetaErr, NR_NATIVE, nullptr, &safeOldRef);
            check(safeOldFetched && safeOldMeta.frames == 3 &&
                      safeOldMeta.h == 8 && safeOldMeta.w == 6 && safeOldMeta.ch == 1,
                  "R21j3 a canonical v1 Reader carrier remains usable on a v14 peer");
            old.stop();
            setEnv("VIEWER_SERVE_PROTOCOL", "");

            // The reverse seam matters too: a current peer must not hand a
            // typed header/key to a v14 CLIENT, which would ignore layout and
            // ask for native pixels. Use the v1 carrier an actual v14 client
            // shipped, not today's v3 writer wearing an old HELLO.

            // Seed a valid canonical under the exact same hash with a v15
            // client. The old-client refusal must not classify/delete it.
            remote::Session seed;
            if (!openSession(seed, true)) return 1;
            remote::ReaderRun seeded;
            std::string seedErr;
            const bool seedSent = seed.readerRun(origin, "load", oldFiles, seeded, seedErr);
            const std::string seededPath = cacheDir + "/" + seeded.key + ".vstream";
            check(seedSent && seeded.outcome == rp::RO_OK && !seeded.key.empty() &&
                      std::filesystem::exists(pathFromUtf8(seededPath), ec),
                  "R21l a v15 client first publishes the typed v1 carrier canonical");
            closeAll();
            const std::string v1Land = seedSent && seeded.outcome == rp::RO_OK
                                         ? remoteReaderLand(url, spec, std::string(), seeded)
                                         : std::string("v1 seed did not run");
            const bool v1Pixels = v1Land.empty() && app.images.size() == 4 &&
                                  app.images[0]->w == 4 && app.images[0]->h == 2 &&
                                  app.images[0]->ch == 3 &&
                                  app.images[0]->src->data.size() >= 3 &&
                                  app.images[0]->src->data[0] == 100 &&
                                  app.images[0]->src->data[1] == 108 &&
                                  app.images[0]->src->data[2] == 116 &&
                                  app.images[1]->w == 3 && app.images[1]->h == 2 &&
                                  app.images[1]->ch == 2 &&
                                  app.images[1]->src->data[0] == 1000 &&
                                  app.images[1]->src->data[1] == 1006;
            check(v1Pixels,
                  "R21l2 current reader materialises a typed v1 carrier with corrected axes");
            closeAll();

            remote::Session oldClient;
            oldClient.setHelloVersionForTest(14);
            if (!openSession(oldClient, true)) return 1;
            remote::ReaderRun ocRun;
            std::string ocErr;
            const bool ocSent = oldClient.readerRun(origin, "load", oldFiles, ocRun, ocErr);
            check(oldClient.peerVersion() == 15 && ocSent &&
                      ocRun.outcome == rp::RO_UNREADABLE && ocRun.key.empty() &&
                      ocRun.header.empty() &&
                      ocRun.err == rp::typedAxesClientTooOldText(14, "Reader CHW"),
                  "R21m a v15 peer refuses named typed output before issuing it to a v14 client");
            remote::ReaderRun still;
            std::string stillErr;
            const bool stillSent = seed.readerRun(origin, "load", oldFiles, still, stillErr);
            check(stillSent && still.outcome == rp::RO_OK && still.key == seeded.key &&
                      has(still.stderrText, "read from cache") &&
                      std::filesystem::exists(pathFromUtf8(seededPath), ec),
                  "R21n the old-client refusal leaves the valid canonical cache untouched");

            // Anti-vacuity: a canonical Stack(F,H,W), W>4, needs no typed wire
            // capability and continues to work for the old client.
            oldFiles[0] = files[0];
            remote::ReaderRun ocSafe;
            std::string ocSafeErr;
            const bool ocSafeSent = oldClient.readerRun(
                origin, "load", oldFiles, ocSafe, ocSafeErr);
            check(ocSafeSent && ocSafe.outcome == rp::RO_OK && !ocSafe.key.empty() &&
                      has(ocSafe.header, "VIEWERSTREAM 1"),
                  "R21o a canonical safe Reader still passes to a v14 client");

            check(slurp(rdNarrowA, oldFiles[0]),
                  "R21p (setup) the narrow Stack reader is readable");
            remote::ReaderRun ocNarrow;
            std::string ocNarrowErr;
            const bool ocNarrowSent = oldClient.readerRun(
                origin, "load", oldFiles, ocNarrow, ocNarrowErr);
            check(ocNarrowSent && ocNarrow.outcome == rp::RO_UNREADABLE &&
                      ocNarrow.key.empty() && ocNarrow.header.empty() &&
                      ocNarrow.err == rp::typedAxesClientTooOldText(
                          14, "Reader Stack/FHW"),
                  "R21q ...but refuses the blank-layout narrow Stack seam");
            oldClient.stop();
            seed.stop();
        }

        {   // ---- R22: loose remote .npy preview -> full keeps its reading -----
            // (3,2002,4) is natively HWC, but both declarations below are
            // deliberately different. The long axis forces a 1/2 preview, so
            // the background Session has to quote the declaration again.
            closeAll();
            const int BH = 2002, BW = 4;
            std::vector<uint16_t> big((size_t)3 * BH * BW);
            for (size_t i = 0; i < big.size(); i++) big[i] = (uint16_t)(200 + i);
            const std::string bigPath = root + "/full-reading.npy";
            {
                const std::vector<uint8_t> b = selftestNpyBytes(
                    "<u2", { 3, BH, BW }, big.data(), big.size() * 2);
                std::ofstream f(pathFromUtf8(bigPath), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            }
            const std::string bigUrl = "local://" + bigPath;
            forgetReader(bigUrl);
            auto settleFetch = [&]() {
                const double begun = nowSec();
                while (app.rfPending > 0 && nowSec() - begun < 30.0) {
                    pumpRemoteFetch();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pumpRemoteFetch();
            };

            const bool chwOpen = openRemote(bigUrl, false, 0, NR_CHW,
                                            nullptr, nullptr, std::string());
            const int chwPreviewStep = app.images.empty() ? 0 : app.images[0]->src->remoteStep;
            settleFetch();
            bool chwFull = chwOpen && app.images.size() == 1 && chwPreviewStep > 1 &&
                           app.images[0]->src->remoteStep == 1 &&
                           app.images[0]->src->npyRead == NR_CHW &&
                           app.images[0]->w == BW && app.images[0]->h == BH &&
                           app.images[0]->ch == 3 &&
                           app.images[0]->src->data[0] == 200 &&
                           app.images[0]->src->data[1] == 200 + BH * BW &&
                           app.images[0]->src->data[2] == 200 + 2 * BH * BW;
            check(chwFull,
                  "R22a a non-keyed CHW preview refines under CHW, not native HWC");

            closeAll();
            const bool stackOpen = openRemote(bigUrl, false, 0, NR_STACK,
                                              nullptr, nullptr, std::string());
            const int stackPreviewStep = app.images.empty() ? 0 : app.images[0]->src->remoteStep;
            settleFetch();
            bool stackFull = stackOpen && app.images.size() == 3 && stackPreviewStep > 1;
            for (const auto& d : app.images) {
                stackFull = stackFull && d->src->remoteStep == 1 &&
                            d->src->npyRead == NR_STACK && d->w == BW && d->h == BH &&
                            d->ch == 1 && !d->src->data.empty() &&
                            d->src->data[0] == 200 + d->seqIndex * BH * BW;
            }
            check(stackFull,
                  "R22b a non-keyed NR_STACK preview and every sibling refine as that stack");
            closeAll();
            forgetReader(bigUrl);
        }

        {   // ---- R23: blank-layout typed gate survives worker reconnect -------
            closeAll();
            const int savedPolicy = app.procPolicy;
            app.procPolicy = App::PolAuto;
            auto settleWorkers = [&](double budget) {
                const double begun = nowSec();
                while ((app.rfPending > 0 || app.mPending > 0) &&
                       nowSec() - begun < budget) {
                    pumpRemoteFetch();
                    pumpMeasure();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pumpRemoteFetch();
                pumpMeasure();
            };

            const std::string aerr = openRemoteWithReader(url, rdNarrowA + ":load");
            settleWorkers(120.0);
            const size_t bFirst = app.images.size();
            const std::string berr = openRemoteWithReader(url, rdNarrowB + ":load");
            settleWorkers(120.0);
            bool distinct = aerr.empty() && berr.empty() && bFirst == 2 &&
                            app.images.size() == 4 &&
                            app.images[0]->src != app.images[bFirst]->src &&
                            app.images[0]->src->data[0] == 300 &&
                            app.images[bFirst]->src->data[0] == 40000 &&
                            app.images[0]->src->remoteStep == 1 &&
                            app.images[bFirst]->src->remoteStep == 1;
            check(distinct,
                  "R23a large same-origin outputs of two Readers stay distinct after full landing");

            ImageDoc* seam = app.images.size() > bFirst ? app.images[bFirst].get() : nullptr;
            bool carried = seam && seam->seqId != 0 &&
                           seam->src->npyRead == NR_STACK &&
                           seam->src->remoteKeyRead == NR_NATIVE &&
                           seam->src->remoteKeyLayout.empty() &&
                           seam->src->remoteKeyRequires15;
            check(carried,
                  "R23b a blank-layout narrow Stack retains independent requires15 evidence");

            // The open used v15. Force only the two worker Sessions to restart
            // as v14, then ask for pixels and statistics from the landed source.
            // Neither may rely on app.uiSession's still-v15 HELLO.
            stopRemoteFetcher();
            stopMeasureWorker();
            app.rfStop = false;
            app.mStop = false;
            setEnv("VIEWER_SERVE_PROTOCOL", "14");
            std::vector<float> before;
            int seq = 0;
            if (seam) {
                before = seam->src->data;
                seam->src->remoteStep = 2;       // re-arm the exact refinement door
                seam->src->remoteErr.clear();
                seq = seam->seqId;
                requestFullRemote(seam);
                requestServerTemporal(seq, app.srvTemporal);
            }
            settleWorkers(30.0);
            const bool fullRefused = seam && seam->src->remoteStep == 2 &&
                                     seam->src->data == before &&
                                     has(seam->src->remoteErr, "protocol 14") &&
                                     has(seam->src->remoteErr,
                                         "typed materialisation axes need 15");
            check(fullRefused,
                  "R23c a separately reconnected v14 full-fetch worker refuses before pixels");
            check(seq != 0 && !app.srvTemporal.valid && !app.srvTemporal.pending &&
                      has(app.srvTemporal.err, "protocol 14") &&
                      has(app.srvTemporal.err, "typed materialisation axes need 15"),
                  "R23d the separately reconnected v14 MEASURE worker refuses the same subject");

            stopRemoteFetcher();
            stopMeasureWorker();
            app.rfStop = false;
            app.mStop = false;
            setEnv("VIEWER_SERVE_PROTOCOL", "");
            app.procPolicy = savedPolicy;
            closeAll();
            forgetReader(url);
        }

        {   // ---- R11: a set does not cross the link in v1 (§7) ----------------
            const std::string spec = rdSet + ":load";
            const size_t before = app.images.size();
            const std::string rerr = openRemoteWithReader(url, spec);
            check(!rerr.empty() && has(rerr, "analysis set") && has(rerr, "does not cross the link"),
                  "R11a a set-bearing reader is refused by name, not half-built");
            check(app.images.size() == before, "R11b ...and nothing landed");
        }

        {   // ---- R14: the memo is asked BEFORE the url is (#218 review) -------
            //
            // The bug: openPath answered the url SCHEME first and openRemote
            // answers the format table first, so a url this user had chosen a
            // reader for reopened as the PEER's own array whenever the peer
            // could read it natively - the origin's pixels, under the reader's
            // name, with nothing said. The fixture is therefore a file the peer
            // reads perfectly well: a plain (F,H,W) u2 that native META answers,
            // and a reader whose output is the same shape with a constant added
            // so that "whose pixels are these" is a number and not an inference.
            //
            // The exclusions are half the fix and get their own assertions: a
            // preview, an explicit §3.3 reading and a session line that recorded
            // NATIVE pixels must each come back native with the memo sitting
            // right there.
            closeAll();
            const int NF = 3, NH = 5, NW = 6;   // W > 4: §3.1 reads this as (F,H,W)
            const uint16_t BASE = 100, BUMP = 1000;
            const std::string natOrigin = root + "/plain.npy";
            {
                std::vector<uint16_t> px((size_t)NF * NH * NW);
                for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(BASE + i);
                std::vector<uint8_t> b = selftestNpyBytes("<u2", { NF, NH, NW },
                                                          px.data(), px.size() * 2);
                std::ofstream f(pathFromUtf8(natOrigin), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            }
            const std::string rdBump = root + "/bump.py";
            writeText(rdBump,
                      "import numpy as np\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    a = np.load(path).astype(np.uint16) + 1000\n"
                      "    return Stack(np.ascontiguousarray(a), name='bumped')\n");
            const std::string natUrl = "local://" + natOrigin;
            const std::string natSpec = rdBump + ":load";
            // The first sample of the document on screen, and whether the
            // document is a reader's at all. A reader's member is __pixels_N
            // (that is what a materialisation names its nodes); the peer's own
            // array has no member.
            auto firstPx = [&]() -> float {
                return cur() && !cur()->src->data.empty() ? cur()->src->data[0] : -1.0f;
            };
            auto isReaderDoc = [&]() {
                return cur() && cur()->src->member.rfind("__pixels_", 0) == 0;
            };

            // (a) the baseline: no memo yet, and the peer answers it natively.
            forgetReader(natUrl);
            const bool nat0 = browseOpenRemote(natUrl, false, 0, rp::NR_NATIVE);
            check(nat0 && !isReaderDoc() && firstPx() == (float)BASE,
                  "R14a (setup) the peer reads this file natively, memo or no memo");

            // (b) the choice, made once. This is the only thing that may ever
            // put an entry in the memo.
            closeAll();
            const std::string rerr = openRemoteWithReader(natUrl, natSpec);
            check(rerr.empty() && isReaderDoc() && firstPx() == (float)(BASE + BUMP),
                  "R14b the reader runs on the peer and its pixels are tellable apart");
            check(readerFor(natUrl) == natSpec, "R14c ...and the choice is remembered");

            // (c) FRESH OPEN of a natively-supported url: the memo wins.
            closeAll();
            openPath(natUrl);
            check(isReaderDoc() && firstPx() == (float)(BASE + BUMP),
                  "R14d a fresh open of a NATIVE-readable url returns the reader's pixels");
            fprintf(stderr, "rreaderselftest:   R14 fresh open: %zu doc(s) member=\"%s\"\n",
                    app.images.size(), cur() ? cur()->src->member.c_str() : "");
            check(app.images.size() == (size_t)NF,
                  "R14e ...as the stack the reader declared, not the peer's array");

            // (d) ...and the Browse panel's registered open, the other door.
            closeAll();
            const bool bo = browseOpenRemote(natUrl, false, 0, rp::NR_NATIVE);
            check(bo && isReaderDoc() && firstPx() == (float)(BASE + BUMP),
                  "R14f Browse Open returns them too - one rule, both doors");

            // (e) a PREVIEW may not start Python on another machine.
            closeAll();
            browseOpenRemote(natUrl, true, 0, rp::NR_NATIVE);
            check(!app.images.empty() && !isReaderDoc(),
                  "R14g a preview is the peer's own tile: the memo does not run");

            // (f) an explicit §3.3 reading is a statement about the PEER's
            // array, so it opens the peer's array.
            closeAll();
            // NR_CHW: (3,5,6) read as 3 planes of one frame. A reading the
            // shape ADMITS and native would not have chosen, which is what
            // makes it a statement rather than a synonym.
            browseOpenRemote(natUrl, false, 0, rp::NR_CHW);
            check(!app.images.empty() && !isReaderDoc(),
                  "R14h an explicit reading opens the peer's array, not the reader's");

            // (g) a session that saved NATIVE pixels restores native pixels,
            // with the memo present the whole time (#179 ruling C: the LINE
            // decides, and #219's restore must not start moving either).
            closeAll();
            const std::string natSess = root + "/native.vsession";
            openRemote(natUrl, false, 0, rp::NR_NATIVE, nullptr, nullptr, std::string());
            check(!app.images.empty() && !isReaderDoc(),
                  "R14i (setup) a natively-opened remote document");
            // writeSessionTo rather than saveSession: the same writer, without
            // the atomic-rename plumbing around it. What this line is about is
            // what the RESTORE does with a natively-saved line, and V25p below
            // already exercises saveSession end to end on this same directory.
            {
                std::ofstream f(pathFromUtf8(natSess), std::ios::binary);
                writeSessionTo(f);
            }
            closeAll();
            const std::string sr = loadSession(natSess);
            fprintf(stderr, "rreaderselftest:   R14j restore err=\"%s\" %zu doc(s) "
                            "member=\"%s\" px0=%g\n",
                    sr.c_str(), app.images.size(),
                    cur() ? cur()->src->member.c_str() : "", (double)firstPx());
            check(sr.empty() && !app.images.empty() && !isReaderDoc() &&
                      firstPx() == (float)BASE,
                  "R14j a session saved as NATIVE restores native, memo or no memo");

            // (h) and the case the review names second: a url the peer CANNOT
            // read. Before the fix this reached openRemote and was refused;
            // now the memo answers it first, exactly as it does on this disk.
            closeAll();
            const std::string permSpec = rdPerm + ":load";
            const std::string rerr2 = openRemoteWithReader(url, permSpec);
            check(rerr2.empty(), "R14k (setup) the rank-5 origin opens through its reader");
            closeAll();
            openPath(url);
            check(app.images.size() == (size_t)S && isReaderDoc(),
                  "R14l a fresh open of an UNSUPPORTED url returns the reader's pixels too");

            closeAll();
            forgetReader(natUrl);          // the memo is shared prefs; leave none
            forgetReader(url);
        }

        {   // ---- R15: the key covers the PYTHON that produced the pixels ------
            //
            // #218 review: the key covered the origin, the reader's three texts
            // and the function, and stopped. `prov` - "Python 3.11.4 (...),
            // numpy 1.26.4" - was reported beside every reply, hit or run, but
            // was not part of what made a hit a hit. So pointing the peer at a
            // different interpreter, or upgrading numpy under the one it names,
            // handed back the OLD environment's pixels under the NEW
            // environment's provenance line. Not a stale cache: a label that is
            // untrue about the numbers it labels.
            //
            // Two environments, for real, and the reader can TELL THEM APART:
            //   A  whatever adapter::findPython settles on;
            //   B  a venv over the same base (--system-site-packages, so numpy
            //      is the same numpy) carrying a module A does not have.
            // The reader adds that module's TAG to every sample, so "which
            // environment produced these pixels" is a number and not a story.
            //
            // Everything here goes through raw remote::Session, never
            // app.uiSession: the peer reads VIEWER_SERVE_PYTHON out of its
            // environment at START, so "a different environment" means a
            // different peer process, and app.uiSession's is already up.
            closeAll();
            const int EF = 2, EH = 4, EW = 6;
            const uint16_t EBASE = 7;
            const std::string envOrigin = root + "/envsrc.npy";
            {
                std::vector<uint16_t> px((size_t)EF * EH * EW);
                for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(EBASE + i);
                std::vector<uint8_t> b = selftestNpyBytes("<u2", { EF, EH, EW },
                                                          px.data(), px.size() * 2);
                std::ofstream f(pathFromUtf8(envOrigin), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            }
            const std::string rdEnv = root + "/envtag.py";
            writeText(rdEnv,
                      "import numpy as np\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    try:\n"
                      "        import viewer_env_tag as T\n"
                      "        tag = int(T.TAG)\n"
                      "    except Exception:\n"
                      "        tag = 0\n"
                      "    a = np.load(path).astype(np.uint16) + tag\n"
                      "    return Stack(np.ascontiguousarray(a), name='envtag')\n");
            std::string envFiles[3] = { std::string(), files[1], files[2] };
            const bool haveEnvText = slurp(rdEnv, envFiles[0]);
            check(haveEnvText, "R15a (setup) the environment-tagging reader is readable");

            // The first sample of node 0, fetched THROUGH the key the RUN
            // issued - the same road a document takes, so "B's pixels" is the
            // peer's own answer and not this side's reconstruction.
            auto keyedFirst = [&](remote::Session& s, const std::string& k, double& out) {
                remote::KeyedRef ref{ k, 0, remote::KeyedRef::FromReader };
                remote::Meta m;
                std::string e;
                const std::string eu = "local://" + envOrigin;
                if (!s.meta(eu, m, e, 0, nullptr, &ref)) return false;
                std::vector<uint8_t> raw;
                int tw = 0, th = 0, tch = 0;
                uint32_t dt = 0;
                if (!s.tileBytes(eu, 0, 0, 0, m.w, m.h, 1, raw, tw, th, tch, dt, e,
                                 0, nullptr, &ref))
                    return false;
                if (raw.size() < 2) return false;
                uint16_t v = 0;
                memcpy(&v, raw.data(), 2);
                out = (double)v;
                return true;
            };
            auto runOn = [&](const std::string& pyEnv, std::string& keyOut,
                             std::string& provOut, double& pxOut, bool& cachedOut) {
                setEnv("VIEWER_SERVE_PYTHON", pyEnv);
                remote::Session s;
                if (!openSession(s, true)) return false;
                remote::ReaderRun r;
                std::string e;
                const bool sent = s.readerRun(envOrigin, "load", envFiles, r, e);
                bool got = sent && r.outcome == rp::RO_OK;
                if (got) {
                    keyOut = r.key;
                    provOut = r.provenance;
                    cachedOut = has(r.stderrText, "read from cache");
                    got = keyedFirst(s, r.key, pxOut);
                }
                s.stop();
                return got;
            };

            std::string kA, pA;
            double pxA = 0;
            bool cachedA = false;
            const bool okA = haveEnvText && runOn("", kA, pA, pxA, cachedA);
            check(okA && pxA == (double)EBASE,
                  "R15b environment A runs the reader and its pixels carry no tag");

            // ---- environment B: a venv over the same base, plus one module ----
            // --system-site-packages so numpy is the SAME numpy (the difference
            // under test is the interpreter, not whether one exists);
            // --without-pip because this has to be cheap enough to run every
            // time. If the machine cannot build one, the environment half is
            // SKIPPED BY NAME rather than passed vacuously.
            // ABSOLUTE, and in the platform's own spelling: this path becomes
            // argv[0] of a process, and CreateProcess does not resolve a
            // relative path written with forward slashes (windows error 2).
            std::filesystem::path envBp =
                std::filesystem::absolute(pathFromUtf8(root + "/envB"), ec).lexically_normal();
            envBp.make_preferred();
            const std::string envB = envBp.u8string();
            std::string pyB;
            std::string baseWhy;
            std::string base;
            adapter::Run mk;
            {
                base = adapter::findPython("", baseWhy);
                if (!base.empty()) {
                    mk = adapter::run({ base, "-m", "venv", "--system-site-packages",
                                        "--without-pip", envB }, 120000);
                    for (const char* sub : { "Scripts/python.exe", "bin/python3",
                                             "bin/python" }) {
                        std::filesystem::path c = envBp / std::filesystem::u8path(sub);
                        c.make_preferred();
                        if (std::filesystem::exists(c, ec)) { pyB = c.u8string(); break; }
                    }
                    if (!pyB.empty()) {
                        adapter::Run probe = adapter::run({ pyB, "-c", "import numpy" }, 60000);
                        if (!probe.started || probe.exit != 0) pyB.clear();
                    }
                }
            }
            if (pyB.empty())
                fprintf(stderr, "rreaderselftest:   R15 venv unavailable "
                                "(base=%s why=\"%s\" exit=%d fail=\"%s\"): "
                                "SKIPPING R15c-R15s\n",
                        base.empty() ? "<none>" : base.c_str(), baseWhy.c_str(), mk.exit,
                        mk.fail.c_str());
            if (!pyB.empty()) {
                // The module only environment B has. A venv's own site-packages
                // precedes the base's, so this is B's and B's alone.
                std::string sp;
                for (auto& d : std::filesystem::recursive_directory_iterator(
                         pathFromUtf8(envB), ec))
                    if (d.is_directory(ec) &&
                        d.path().filename().u8string() == "site-packages") {
                        sp = d.path().u8string();
                        break;
                    }
                check(!sp.empty(), "R15c (setup) environment B has a site-packages of its own");
                if (sp.empty()) {
                    fprintf(stderr,
                            "rreaderselftest:   R15 venv has no site-packages: "
                            "SKIPPING R15d-R15s\n");
                } else {
                writeText(sp + "/viewer_env_tag.py", "TAG = 1000\n");

                std::string kB, pB;
                double pxB = 0;
                bool cachedB = false;
                const bool okB = runOn(pyB, kB, pB, pxB, cachedB);
                fprintf(stderr, "rreaderselftest:   R15 A key=%s prov=\"%s\" px=%g\n",
                        kA.c_str(), pA.c_str(), pxA);
                fprintf(stderr, "rreaderselftest:   R15 B key=%s prov=\"%s\" px=%g\n",
                        kB.c_str(), pB.c_str(), pxB);
                check(okB && kB != kA,
                      "R15d the same origin/reader/function under B is a DIFFERENT key");
                check(okB && !cachedB,
                      "R15e ...so B is a miss and the reader is re-run there");
                check(okB && pxB == (double)(EBASE + 1000),
                      "R15f ...and the pixels that come back are B's, not A's");
                check(okB && pB != pA && has(pB, "envB"),
                      "R15g ...beside a provenance line that names B's interpreter");

                // B -> B: the same environment IS a hit. A key that changed for
                // any reason other than the environment would show up here.
                std::string kB2, pB2;
                double pxB2 = 0;
                bool cachedB2 = false;
                const bool okB2 = runOn(pyB, kB2, pB2, pxB2, cachedB2);
                check(okB2 && kB2 == kB && cachedB2 && pxB2 == pxB,
                      "R15h B twice is one key and one run: the cache answers the second");

                // The upgrade case, which is the one the persistent cache makes
                // dangerous: the SAME interpreter path reporting a different
                // numpy. sitecustomize is imported at startup, so this is what
                // `python -c "import numpy; print(numpy.__version__)"` - the
                // provenance probe itself - sees, exactly as a real upgrade
                // would be seen by the next peer process.
                writeText(sp + "/sitecustomize.py",
                          "import numpy\n"
                          "numpy.__version__ = '99.9.9'\n");
                std::string kB3, pB3;
                double pxB3 = 0;
                bool cachedB3 = false;
                const bool okB3 = runOn(pyB, kB3, pB3, pxB3, cachedB3);
                fprintf(stderr, "rreaderselftest:   R15 B' key=%s prov=\"%s\"\n",
                        kB3.c_str(), pB3.c_str());
                check(okB3 && has(pB3, "99.9.9"),
                      "R15i (setup) the same interpreter path now reports another numpy");
                check(okB3 && kB3 != kB && !cachedB3,
                      "R15j an upgrade under one interpreter path is a miss, not a hit");

                // ---- R15k-o: the same PEER PROCESS, and numpy moves under it --
                //
                // #180 codex review of #224. R15i/j start a new viewer-serve for
                // each environment, so they can only ever show that the NEXT
                // process notices an upgrade. The peer that matters is the one
                // that does not restart: a --serve session lives for hours, and
                // `prov` was asked once per interpreter per process and kept.
                // While that string stood still the key stood still with it, so
                // the peer kept answering out of a materialisation the OLD numpy
                // produced - and had anything else in the key moved, it would
                // have published the NEW numpy's pixels under the OLD
                // environment's name, which is the exact thing putting `prov` in
                // the key was for.
                //
                // So: ONE session, held open across the change, and a reader
                // whose PIXELS carry numpy's own version number. "The generated
                // pixels and the provenance line beside them come from the same
                // environment" is then a number, not a story.
                const std::string rdNpv = root + "/npver.py";
                writeText(rdNpv,
                          "import numpy as np\n"
                          "from viewer_import import Stack\n"
                          "VERSION = 1\n"
                          "def load(path):\n"
                          "    tag = int(np.__version__.split('.')[0])\n"
                          "    a = np.load(path).astype(np.uint16) + tag\n"
                          "    return Stack(np.ascontiguousarray(a), name='npver')\n");
                std::string npFiles[3] = { std::string(), files[1], files[2] };
                const bool haveNpv = slurp(rdNpv, npFiles[0]);
                check(haveNpv, "R15k (setup) the numpy-tagging reader is readable");
                if (haveNpv) {
                    setEnv("VIEWER_SERVE_PYTHON", pyB);
                    remote::Session one;
                    if (!openSession(one, true)) return 1;
                    auto sameProcess = [&](std::string& k, std::string& pv, double& px,
                                           bool& cached) {
                        remote::ReaderRun r;
                        std::string e;
                        const bool sent = one.readerRun(envOrigin, "load", npFiles, r, e);
                        bool got = sent && r.outcome == rp::RO_OK;
                        if (got) {
                            k = r.key;
                            pv = r.provenance;
                            cached = has(r.stderrText, "read from cache");
                            got = keyedFirst(one, r.key, px);
                        }
                        return got;
                    };
                    // sitecustomize runs at interpreter startup, so this is what
                    // the provenance probe AND the reader both see - exactly as a
                    // real `pip install -U numpy` under this path would be seen.
                    //
                    // __pycache__ GOES WITH IT. Python validates a .pyc against
                    // the source's mtime AND SIZE, both of which are unchanged
                    // when one version string is overwritten by another of the
                    // same length within the same second - so the interpreter
                    // re-ran the PREVIOUS version and this test measured its own
                    // fixture instead of the peer.
                    auto setNumpyVersion = [&](const char* v) {
                        writeText(sp + "/sitecustomize.py",
                                  std::string("import numpy\nnumpy.__version__ = '") +
                                      v + "'\n");
                        std::error_code pe;
                        std::filesystem::remove_all(pathFromUtf8(sp + "/__pycache__"), pe);
                    };
                    setNumpyVersion("31.0.0");
                    std::string k1, pv1;
                    double px1 = 0;
                    bool c1 = false;
                    const bool r1 = sameProcess(k1, pv1, px1, c1);
                    check(r1 && has(pv1, "31.0.0") && px1 == (double)(EBASE + 31),
                          "R15l one peer process, numpy 31: the pixels carry 31 and the "
                          "provenance says 31");

                    setNumpyVersion("42.0.0");
                    std::string k2, pv2;
                    double px2 = 0;
                    bool c2 = false;
                    const bool r2 = sameProcess(k2, pv2, px2, c2);
                    fprintf(stderr, "rreaderselftest:   R15 same process: "
                                    "%s/\"%s\"/%g -> %s/\"%s\"/%g cached=%d\n",
                            k1.c_str(), pv1.c_str(), px1, k2.c_str(), pv2.c_str(),
                            px2, c2 ? 1 : 0);
                    check(r2 && has(pv2, "42.0.0"),
                          "R15m ...numpy is upgraded under it and THIS process sees it");
                    check(r2 && k2 != k1 && !c2,
                          "R15n ...so the very next run is a MISS, not a hit on the old "
                          "environment's materialisation");
                    check(r2 && px2 == (double)(EBASE + 42),
                          "R15o ...and the pixels that come back were made by the numpy "
                          "the provenance line names");
                    one.stop();

                    // One configured RUN has one authoritative provenance
                    // probe. A miss starts Python once more for the harness; a
                    // hit does not. sitecustomize counts interpreter starts,
                    // making that contract observable without timing it.
                    const std::string probeCount = root + "/provenance-starts.txt";
                    std::filesystem::remove(pathFromUtf8(probeCount), ec);
                    writeText(sp + "/sitecustomize.py",
                              "import numpy, pathlib\n"
                              "p = pathlib.Path(r'" + probeCount + "')\n"
                              "try:\n"
                              "    n = int(p.read_text())\n"
                              "except Exception:\n"
                              "    n = 0\n"
                              "p.write_text(str(n + 1))\n"
                              "numpy.__version__ = '53.0.0'\n");
                    std::filesystem::remove_all(pathFromUtf8(sp + "/__pycache__"), ec);
                    auto starts = [&]() {
                        std::ifstream f(pathFromUtf8(probeCount), std::ios::binary);
                        int n = -1;
                        if (f) f >> n;
                        return n;
                    };
                    setEnv("VIEWER_SERVE_PYTHON", pyB);
                    remote::Session counted;
                    if (!openSession(counted, true)) return 1;
                    remote::ReaderRun cr1, cr2;
                    std::string ce1, ce2;
                    const bool cs1 = counted.readerRun(envOrigin, "load", npFiles, cr1, ce1);
                    const int startsAfterMiss = starts();
                    const bool cs2 = counted.readerRun(envOrigin, "load", npFiles, cr2, ce2);
                    const int startsAfterHit = starts();
                    check(cs1 && cr1.outcome == rp::RO_OK && startsAfterMiss == 2,
                          "R15p configured miss starts one provenance probe and one harness");
                    check(cs2 && cr2.outcome == rp::RO_OK && cr2.key == cr1.key &&
                              has(cr2.stderrText, "read from cache") &&
                              startsAfterHit == startsAfterMiss + 1,
                          "R15q configured hit starts only its authoritative provenance probe");
                    counted.stop();

                    // A command that starts and imports numpy can still fail to
                    // identify it. That is no provenance, hence no runnable
                    // Python for this protocol outcome; the harness must not be
                    // started under a made-up fallback label.
                    writeText(sp + "/sitecustomize.py",
                              "import numpy\n"
                              "del numpy.__version__\n");
                    std::filesystem::remove_all(pathFromUtf8(sp + "/__pycache__"), ec);
                    remote::Session noProv;
                    if (!openSession(noProv, true)) return 1;
                    remote::ReaderRun nr;
                    std::string ne;
                    const bool ns = noProv.readerRun(envOrigin, "load", npFiles, nr, ne);
                    check(ns && nr.outcome == rp::RO_NO_PYTHON && nr.provenance.empty() &&
                              nr.key.empty(),
                          "R15r failed provenance is RO_NO_PYTHON, not a path-shaped label");
                    noProv.stop();

                    // Exit 0 with no identity is no better: success is the
                    // presence of the provenance fact, not merely a process
                    // status. Suppress the probe's print at startup to make the
                    // otherwise unusual empty-answer case deterministic.
                    writeText(sp + "/sitecustomize.py",
                              "import builtins, numpy\n"
                              "builtins.print = lambda *args, **kwargs: None\n");
                    std::filesystem::remove_all(pathFromUtf8(sp + "/__pycache__"), ec);
                    remote::Session emptyProv;
                    if (!openSession(emptyProv, true)) return 1;
                    remote::ReaderRun er;
                    std::string ee;
                    const bool es = emptyProv.readerRun(envOrigin, "load", npFiles, er, ee);
                    check(es && er.outcome == rp::RO_NO_PYTHON &&
                              has(er.err, "returned no Python/numpy identity") &&
                              er.provenance.empty() && er.key.empty(),
                          "R15s empty provenance is RO_NO_PYTHON and names the missing fact");
                    emptyProv.stop();
                }
                }
            }
            setEnv("VIEWER_SERVE_PYTHON", "");
            closeAll();
        }

        {   // ---- R16: a folder's identity is its CHILDREN, both ends ----------
            //
            // #218 review, and codex's note that the same hole was in the LOCAL
            // key: both recorded the directory's OWN mtime and left the size at
            // zero. That mtime moves when a child is added or removed and NOT
            // when an existing child is rewritten - which is what happens to a
            // capture directory between two acquisitions. So a re-run over a
            // folder whose every frame had been replaced returned the previous
            // pixels, on this disk and on the peer's.
            //
            // The fixture reproduces that exactly: the child is overwritten with
            // a file OF THE SAME NAME AND THE SAME SIZE, and the parent
            // directory's mtime is then put back to what it was. Nothing about
            // the directory itself has changed; everything inside it has.
            closeAll();
            const std::string capt = root + "/capture";
            const std::string captSub = capt + "/sub";
            std::filesystem::create_directories(pathFromUtf8(captSub), ec);
            const int FH = 4, FW = 6;
            auto writeFrame = [&](const std::string& p, uint16_t v) {
                std::vector<uint16_t> px((size_t)FH * FW, v);
                std::vector<uint8_t> b = selftestNpyBytes("<u2", { FH, FW },
                                                          px.data(), px.size() * 2);
                std::ofstream f(pathFromUtf8(p), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            };
            const std::string childA = capt + "/a.npy";
            const std::string childB = captSub + "/b.npy";
            // A rewritten child gets a LATER mtime, set rather than waited for:
            // this host's last_write_time has one-second granularity, and a
            // test that slept for it would be paying a second per acquisition
            // to observe something every real acquisition has anyway.
            auto reframe = [&](const std::string& p, uint16_t v) {
                const auto was = std::filesystem::last_write_time(pathFromUtf8(p), ec);
                writeFrame(p, v);
                std::filesystem::last_write_time(pathFromUtf8(p),
                                                 was + std::chrono::seconds(10), ec);
            };
            writeFrame(childA, 11);
            writeFrame(childB, 22);
            const std::string rdFolder = root + "/folder.py";
            writeText(rdFolder,
                      "import numpy as np, os\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    fs = []\n"
                      "    for r, d, f in os.walk(path):\n"
                      "        for n in f:\n"
                      "            if n.endswith('.npy'):\n"
                      "                fs.append(os.path.join(r, n))\n"
                      "    a = np.stack([np.load(p) for p in sorted(fs)])\n"
                      "    return Stack(np.ascontiguousarray(a.astype(np.uint16)),\n"
                      "                 name='capture')\n");
            const std::string folderSpec = rdFolder + ":load";
            std::string folderText[3] = { std::string(), files[1], files[2] };
            check(slurp(rdFolder, folderText[0]),
                  "R16a (setup) the folder reader is readable");

            // A directory's mtime, taken and PUT BACK. This is the whole
            // premise: on every filesystem this build runs on, rewriting a
            // child does not have to move it, and here it provably does not.
            auto dirTime = [&](const std::string& d) {
                return std::filesystem::last_write_time(pathFromUtf8(d), ec);
            };
            auto setDirTime = [&](const std::string& d,
                                  std::filesystem::file_time_type t) {
                std::filesystem::last_write_time(pathFromUtf8(d), t, ec);
            };

            // ---- the LOCAL door -----------------------------------------------
            auto localFirstPx = [&]() -> float {
                return cur() && !cur()->src->data.empty() ? cur()->src->data[0] : -1.0f;
            };
            const auto captAt0 = dirTime(capt);
            const auto subAt0 = dirTime(captSub);
            const std::string l1 = openWithReader(capt, folderSpec);
            check(l1.empty() && localFirstPx() == 11.0f,
                  "R16b the local door reads the folder: first child is 11");
            closeAll();
            // Same name, same size, new content - and the parent put back.
            reframe(childA, 33);
            setDirTime(capt, captAt0);
            const int lruns = g_adapterRuns;
            const std::string l2 = openWithReader(capt, folderSpec);
            fprintf(stderr, "rreaderselftest:   R16 local re-run: err=\"%s\" runs %d -> %d "
                            "px0=%g\n",
                    l2.c_str(), lruns, g_adapterRuns, (double)localFirstPx());
            check(l2.empty() && g_adapterRuns > lruns,
                  "R16c a rewritten child is a MISS on this disk, and the reader re-runs");
            check(localFirstPx() == 33.0f,
                  "R16d ...and what comes back is the new child, not the cached one");
            closeAll();
            // ...and a NESTED child, with both directories put back.
            const auto captAt1 = dirTime(capt);
            reframe(childB, 44);
            setDirTime(captSub, subAt0);
            setDirTime(capt, captAt1);
            const int lruns2 = g_adapterRuns;
            const std::string l3 = openWithReader(capt, folderSpec);
            bool sawNested = false;
            for (const auto& d : app.images)
                if (!d->src->data.empty() && d->src->data[0] == 44.0f) sawNested = true;
            check(l3.empty() && g_adapterRuns > lruns2 && sawNested,
                  "R16e a nested child counts too: miss, re-run, the new pixels");
            closeAll();
            forgetReader(capt);

            // ---- the PEER's key -----------------------------------------------
            // Same three states, asked of a real viewer-serve. The witness is
            // the key it issues plus whether its own reply says it read from
            // cache: a key that did not move IS the defect.
            auto folderRun = [&](const char* label, std::string& keyOut, bool& cachedOut) {
                keyOut.clear();
                cachedOut = false;
                remote::Session s;
                if (!openSession(s, true)) {
                    fprintf(stderr, "rreaderselftest:   %s could not start peer\n", label);
                    return false;
                }
                remote::ReaderRun r;
                std::string e;
                const bool sent = s.readerRun(capt, "load", folderText, r, e);
                const bool got = sent && r.outcome == rp::RO_OK;
                if (got) { keyOut = r.key; cachedOut = has(r.stderrText, "read from cache"); }
                else
                    fprintf(stderr,
                            "rreaderselftest:   %s failed: sent=%d outcome=%d transport=\"%s\" "
                            "err=\"%s\" stderr=\"%s\"\n",
                            label, sent ? 1 : 0, sent ? (int)r.outcome : -1, e.c_str(),
                            r.err.c_str(), r.stderrText.c_str());
                s.stop();
                return got;
            };
            std::string fk1, fk2, fk3;
            bool fc1 = false, fc2 = false, fc3 = false;
            const auto captAt2 = dirTime(capt);
            const bool f1 = folderRun("R16f", fk1, fc1);
            check(f1, "R16f the peer runs a reader over a folder");
            reframe(childA, 55);
            setDirTime(capt, captAt2);
            const bool f2 = folderRun("R16g", fk2, fc2);
            fprintf(stderr, "rreaderselftest:   R16 peer keys %s / %s / ", fk1.c_str(),
                    fk2.c_str());
            check(f2 && fk2 != fk1 && !fc2,
                  "R16g a rewritten child is a different key over there too, and a miss");
            const auto captAt3 = dirTime(capt);
            const auto subAt3 = dirTime(captSub);
            reframe(childB, 66);
            setDirTime(captSub, subAt3);
            setDirTime(capt, captAt3);
            const bool f3 = folderRun("R16h", fk3, fc3);
            fprintf(stderr, "%s\n", fk3.c_str());
            check(f3 && fk3 != fk2 && !fc3,
                  "R16h ...and so is a nested one");
            // ...and NOTHING changing is still a hit: an identity that moved on
            // its own would make every folder read uncacheable, which is the
            // opposite mistake and just as wrong.
            std::string fk4;
            bool fc4 = false;
            const bool f4 = folderRun("R16i", fk4, fc4);
            check(f4 && fk4 == fk3 && fc4,
                  "R16i an unchanged folder is one key and one run: the cache answers");
            closeAll();
        }

        {   // ---- R17: two peers, one key, and nothing half-written -----------
            //
            // #218 review r3794154621. Two viewer sessions asking the same
            // uncached key at the same time are two viewer-serve PROCESSES, and
            // both used to point their harness straight at the cache file. One
            // then validated, deleted or overwrote what the other was still
            // writing; when the two runs' outputs had compatible lengths the
            // mixture even passed the structural check.
            //
            // The two contenders here carry the SAME reader text, so they
            // contend for the same key - and the reader itself decides which of
            // them is which, atomically, with an O_EXCL lock file. That makes
            // the roles deterministic without making the timing so: whoever
            // gets there first sleeps for three seconds and succeeds, the other
            // one raises at once. Which is the shape the review describes -
            // a failing contender running while a winner is mid-write.
            closeAll();
            const std::string rdRace = root + "/race.py";
            writeText(rdRace,
                      "import numpy as np, os, time\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    lock = path + '.first'\n"
                      "    try:\n"
                      "        os.close(os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY))\n"
                      "    except FileExistsError:\n"
                      "        raise ValueError('race-2: this contender was second')\n"
                      "    time.sleep(3.0)\n"
                      "    a = np.load(path)\n"
                      "    return Stack(np.ascontiguousarray(\n"
                      "        a.reshape(a.shape[0], a.shape[1], a.shape[2])), name='race')\n");
            std::string raceText[3] = { std::string(), files[1], files[2] };
            check(slurp(rdRace, raceText[0]), "R17a (setup) the racing reader is readable");
            auto countSuffix = [&](const char* suf) {
                int n = 0;
                std::error_code e;
                const std::string s = suf;
                for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), e)) {
                    const std::string nm = d.path().filename().u8string();
                    if (nm.size() > s.size() &&
                        nm.compare(nm.size() - s.size(), s.size(), s) == 0) n++;
                }
                return n;
            };
            const int vstream0 = countSuffix(".vstream");

            struct Contender { bool sent = false; int outcome = -1; std::string key, err; };
            Contender c[2];
            std::atomic<int> ready{ 0 }, done{ 0 };
            std::atomic<bool> go{ false };
            auto contend = [&](int i) {
                remote::Session s;
                if (!openSession(s, true)) { done.fetch_add(1); return; }
                ready.fetch_add(1);
                while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                remote::ReaderRun r;
                std::string e;
                c[i].sent = s.readerRun(origin, "load", raceText, r, e);
                if (c[i].sent) { c[i].outcome = (int)r.outcome; c[i].key = r.key; }
                c[i].err = e;
                s.stop();
                done.fetch_add(1);
            };
            std::thread t0(contend, 0), t1(contend, 1);
            // The barrier: both peers are up before either is asked, so the two
            // requests really do overlap rather than merely being adjacent.
            for (int k = 0; k < 2000 && ready.load() < 2; k++)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            go.store(true);
            // ...and what the cache directory looks like WHILE they run. This is
            // the assertion the fix is: a run in progress is a `.part` of its
            // own, and the file the key names does not appear until it is
            // whole. Sampled inside the winner's three-second sleep.
            bool sawPart = false;
            int maxDuring = vstream0;
            const auto t0c = std::chrono::steady_clock::now();
            for (;;) {
                const double el = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - t0c).count();
                if (el > 2.5 || done.load() >= 2) break;
                if (el > 0.3) {
                    if (countSuffix(".part") > 0) sawPart = true;
                    maxDuring = std::max(maxDuring, countSuffix(".vstream"));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            t0.join();
            t1.join();
            const int oks = (c[0].outcome == (int)rp::RO_OK) + (c[1].outcome == (int)rp::RO_OK);
            const int bad = (c[0].outcome == (int)rp::RO_EXITED) +
                            (c[1].outcome == (int)rp::RO_EXITED);
            fprintf(stderr, "rreaderselftest:   R17 outcomes %d/%d keys \"%s\"/\"%s\"; "
                            ".vstream %d -> %d (max %d while running), part seen=%d\n",
                    c[0].outcome, c[1].outcome, c[0].key.c_str(), c[1].key.c_str(),
                    vstream0, countSuffix(".vstream"), maxDuring, sawPart ? 1 : 0);
            check(c[0].sent && c[1].sent && oks == 1 && bad == 1,
                  "R17b both contenders are answered: one ran, one refused");
            check(sawPart,
                  "R17c a run in progress is a temp of its own, not the cache file");
            check(maxDuring == vstream0,
                  "R17d ...so nothing appears under the key until it is WHOLE");
            check(countSuffix(".vstream") == vstream0 + 1,
                  "R17e exactly one result was published");
            check(countSuffix(".part") == 0,
                  "R17f and no temp is left behind by either of them");
            // The loser must not have taken the winner's file with it: the
            // third ask is a HIT, which is only possible if what was published
            // is still there and still validates.
            {
                remote::Session s;
                if (!openSession(s, true)) return 1;
                remote::ReaderRun r;
                std::string e;
                const bool sent = s.readerRun(origin, "load", raceText, r, e);
                const std::string winner = c[0].outcome == (int)rp::RO_OK ? c[0].key : c[1].key;
                fprintf(stderr, "rreaderselftest:   R17 third ask: outcome=%d key=\"%s\" \"%s\"\n",
                        sent ? (int)r.outcome : -1, sent ? r.key.c_str() : "",
                        sent ? r.stderrText.c_str() : e.c_str());
                check(sent && r.outcome == rp::RO_OK && r.key == winner &&
                          has(r.stderrText, "read from cache"),
                      "R17g the failing contender did not delete the winner: the cache answers");
                s.stop();
            }
            closeAll();
        }

        // ---- stage 4: the memo crosses the link, the hint never runs ----------
        // The V25p pair of core/selftest/verify.inc, over a peer. It lives HERE
        // rather than there for this file's own reason: half of what is asserted
        // is that NOTHING was asked of the far side, and a claim about the wire
        // needs a real process on the other end of it.
        //
        // #179 ruling C, unchanged by the link (design §2.2): which reader is
        // the CLIENT's question - the memo is this user's recorded choice on
        // this machine - and a session may NAME a reader and may never run one.
        // So the memo re-runs it (on the peer, where the file lives); the hint
        // alone does not even ask.
        {
            closeAll();
            const std::string spec = rdPerm + ":load";
            const std::string sess = root + "/reader.vsession";
            const std::string open1 = openRemoteWithReader(url, spec);
            check(open1.empty() && app.images.size() == (size_t)S,
                  "V25p-r0 (setup) the peer's reader output is open, frame for frame");

            // ---- the WRITER half: what a session says about a remote reader --
            saveSession(sess, true);
            std::string hint, member, imageLine;
            {
                std::ifstream sf(pathFromUtf8(sess), std::ios::binary);
                std::string line;
                while (std::getline(sf, line)) {
                    if (line.rfind("readerhint ", 0) == 0) hint = line.substr(11);
                    else if (line.rfind("member ", 0) == 0) member = line.substr(7);
                    else if (line.rfind("image ", 0) == 0) imageLine = line;
                }
            }
            fprintf(stderr, "rreaderselftest: V25p-r1 saved hint=\"%s\" member=\"%s\"\n",
                    hint.c_str(), member.c_str());
            check(hint == spec,
                  "V25p-r1a the session names the reader that ran on the peer");
            check(member.rfind("__pixels_", 0) == 0 && has(imageLine, url),
                  "V25p-r1b ...beside the member, on a line whose file is the URL");

            // ---- WITH the memo: it comes back without re-running the reader ----
            // The RUN is sent again and the peer answers out of its cache (R2's
            // observation, one layer up). The peer still starts its one
            // provenance probe: the materialisation's mtime is the witness that
            // the reader/harness was not re-executed over there, and
            // g_adapterRuns is the witness that it was not executed here.
            closeAll();
            // THIS reader's materialisation, by the key R1 saw the peer issue -
            // the cache holds one per reader that has run, so a count alone
            // would be answering about withset.py's as much as about this one.
            const std::string permCache = cacheDir + "/" + keyPerm + ".vstream";
            const size_t cacheCount = cacheFiles().size();
            check(std::filesystem::exists(pathFromUtf8(permCache), ec),
                  "V25p-r1c (setup) this reader's materialisation sits on the peer");
            const auto cacheAt = std::filesystem::last_write_time(pathFromUtf8(permCache), ec);
            const int runsBefore = g_adapterRuns;
            const std::string s1 = loadSession(sess);
            fprintf(stderr, "rreaderselftest: V25p-r1 restore: err=\"%s\" %zu doc(s) "
                            "runs %d -> %d\n",
                    s1.c_str(), app.images.size(), runsBefore, g_adapterRuns);
            check(s1.empty() && app.images.size() == (size_t)S,
                  "V25p-r1 with the memo, the restore reopens it through the peer's reader");
            check(readerFor(url) == spec && cur() &&
                      cur()->src->member.rfind("__pixels_", 0) == 0,
                  "V25p-r1d ...as the reader's own document, not the origin file");
            check(g_adapterRuns == runsBefore,
                  "V25p-r1e ...and no adapter ran HERE (the reader runs where the file is)");
            check(cacheFiles().size() == cacheCount &&
                      std::filesystem::last_write_time(pathFromUtf8(permCache), ec) == cacheAt,
                  "V25p-r1f ...nor THERE: the peer's RUN cache answered and did not move");

            // ---- WITHOUT it: named, and that is all that happens --------------
            // The reader file still exists and the peer is still up and still
            // consenting, so every ingredient of a run is present. What is
            // missing is the one thing that may start one: the user's own
            // record. Nothing may go out on the wire - not even the request.
            closeAll();
            forgetReader(url);
            const int runsBefore2 = g_adapterRuns;
            const uint64_t rxBefore = app.uiSession.bytesReceived();
            const size_t logBefore = app.msgLog.size();
            const std::string s2 = loadSession(sess);
            bool named = false, saidRun = false;
            for (size_t i = logBefore; i < app.msgLog.size(); i++) {
                if (!app.msgLog[i].err) continue;
                if (has(app.msgLog[i].text, "perm.py")) named = true;
                if (has(app.msgLog[i].text, "never runs one")) saidRun = true;
            }
            fprintf(stderr, "rreaderselftest: V25p-r2 restore without memo: named=%d "
                            "%zu doc(s) runs %d -> %d rx %llu -> %llu cache %zu\n",
                    (int)named, app.images.size(), runsBefore2, g_adapterRuns,
                    (unsigned long long)rxBefore,
                    (unsigned long long)app.uiSession.bytesReceived(), cacheFiles().size());
            check(named && saidRun,
                  "V25p-r2 without the memo the failure NAMES the reader, and says "
                  "a session never runs one");
            check(s2.empty() && app.images.empty(),
                  "V25p-r2b ...and nothing came back under some other reading");
            check(g_adapterRuns == runsBefore2,
                  "V25p-r2c ...no reader was started here");
            check(app.uiSession.bytesReceived() == rxBefore &&
                      cacheFiles().size() == cacheCount &&
                      std::filesystem::last_write_time(pathFromUtf8(permCache), ec) == cacheAt,
                  "V25p-r2d ...and no RUN went out on the wire: the peer was never asked");
            closeAll();
            forgetReader(url);            // the memo is shared prefs; leave none
        }

        {   // ---- R18: the same file Loaded TWICE comes back twice -------------
            //
            // #219 review r3794522023. The Reader panel's Load is repeatable, so
            // a session can hold the same path - or the same url - twice, as two
            // independent groups with their own ranges. The restore asked "is
            // any open image's path this one", which every line of the SECOND
            // group answered yes to: it skipped its run, resolved onto the first
            // group's documents, and wrote its saved range over theirs. Half the
            // session simply did not come back.
            //
            // The witnesses are the RANGE and the LUT, because per-group display
            // state is what the review says is overwritten: group 1 is left at
            // 10/100 lut 3 and group 2 at 20/200 lut 5, and a restore that lost
            // the grouping cannot produce both.
            //
            // What this reader produces matters, and is why the first fix did
            // not work: perm.py:load returns a 3-frame STACK, and a stack is one
            // session LINE (isSavedLine) over three documents whose (path,
            // member) are identical. So "has this run an unclaimed DOCUMENT for
            // my member" still answered yes to the second group - it found the
            // first group's frames 1 and 2. The claim has to be per line, which
            // means per stack. A reader whose output were three loose documents
            // would never have shown it.
            //
            // Both restores go through the reader CACHE (the setup's own two
            // Loads filled it), which is the review's "cache経由でも後続group
            // を失わない": a cache hit must not be a reason to fold the groups.
            const std::string spec = rdPerm + ":load";
            auto twoGroups = [&](const std::string& what, bool remote, const char* tag0,
                                 const char* tag1, const char* tag2, const char* tag3) {
                closeAll();
                const std::string e1 = remote ? openRemoteWithReader(what, spec)
                                              : openWithReader(what, spec);
                const std::string e2 = remote ? openRemoteWithReader(what, spec)
                                              : openWithReader(what, spec);
                check(e1.empty() && e2.empty() && app.images.size() == (size_t)(2 * S), tag0);
                if (app.images.size() != (size_t)(2 * S)) return;
                auto wantBlack = [&](int i) { return i < S ? 10.0f : 20.0f; };
                auto wantWhite = [&](int i) { return i < S ? 100.0f : 200.0f; };
                auto wantLut   = [&](int i) { return i < S ? 3 : 5; };
                for (int i = 0; i < 2 * S; i++) {
                    app.images[i]->black = wantBlack(i);
                    app.images[i]->white = wantWhite(i);
                    app.images[i]->displayLut = wantLut(i);
                }
                const std::string sess = root + (remote ? "/twice-r.vsession"
                                                        : "/twice-l.vsession");
                saveSession(sess, true);
                closeAll();
                const int runsBefore = g_adapterRuns;
                const std::string re = loadSession(sess);
                fprintf(stderr, "rreaderselftest:   R18 %s: err=\"%s\" %zu doc(s) "
                                "runs %d -> %d |",
                        remote ? "peer" : "local", re.c_str(), app.images.size(),
                        runsBefore, g_adapterRuns);
                for (const auto& d : app.images)
                    fprintf(stderr, " %g/%g lut%d", (double)d->black, (double)d->white,
                            d->displayLut);
                fprintf(stderr, "\n");
                check(re.empty() && app.images.size() == (size_t)(2 * S), tag1);
                bool split = app.images.size() == (size_t)(2 * S);
                for (int i = 0; i < 2 * S && split; i++)
                    if (app.images[i]->black != wantBlack(i) ||
                        app.images[i]->white != wantWhite(i) ||
                        app.images[i]->displayLut != wantLut(i)) split = false;
                check(split, tag2);
                // Two groups, and two SEQUENCES: the second Load is a group of
                // its own, not extra frames appended to the first one's stack.
                std::vector<int> seqs;
                for (const auto& d : app.images)
                    if (d->seqId != 0 &&
                        std::find(seqs.begin(), seqs.end(), d->seqId) == seqs.end())
                        seqs.push_back(d->seqId);
                check(seqs.size() == 2, tag3);
                check(g_adapterRuns == runsBefore,
                      remote ? "R18h ...and no adapter ran HERE either way"
                             : "R18d ...both groups came back through the reader CACHE");
                closeAll();
            };
            forgetReader(origin);
            twoGroups(origin, false,
                      "R18a (setup) the local door Loads one file twice: two groups",
                      "R18b ...and a restore brings BOTH groups back",
                      "R18c ...each with its own range and LUT, not the first group's twice",
                      "R18e ...as two stacks, not one stack of six frames");
            forgetReader(origin);
            forgetReader(url);
            twoGroups(url, true,
                      "R18f (setup) the peer's door does the same over the link",
                      "R18g ...and that restore brings both groups back too",
                      "R18i ...with the range and LUT still one set per group",
                      "R18j ...and two stacks over the link as well");
            forgetReader(url);
            closeAll();
        }

        {   // ---- R19: a folder symlink, or one below it, has no identity -------
            //
            // #180 codex review of #224. adapter::folderIdentity summarised a
            // symlink by its TARGET AS WRITTEN and returned success. That is a
            // fact about the link, and the reader does not read the link - it
            // follows it. So rewriting the target's CONTENTS left this hash, and
            // therefore both cache keys, exactly where they were, and the run
            // that followed handed back the previous acquisition's pixels. A
            // capture directory holding a `latest` link is the ordinary case;
            // R16 is all regular files and cannot see it.
            //
            // The repair is a refusal: a folder with a link below it is
            // UNCACHEABLE. Following links to hash what they resolve to means an
            // unbounded walk of somebody else's tree, a visited set so a cycle
            // is not an identity, and a rule for directory links - three ways to
            // be quietly wrong, against a cost of "the reader runs every time".
            closeAll();
            const std::string linked = root + "/linked";
            const std::string outside = root + "/outside";
            std::filesystem::create_directories(pathFromUtf8(linked), ec);
            std::filesystem::create_directories(pathFromUtf8(outside), ec);
            const int LH = 4, LW = 6;
            auto writeLFrame = [&](const std::string& p, uint16_t v) {
                std::vector<uint16_t> px((size_t)LH * LW, v);
                std::vector<uint8_t> b = selftestNpyBytes("<u2", { LH, LW },
                                                          px.data(), px.size() * 2);
                std::ofstream f(pathFromUtf8(p), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            };
            const std::string target = outside + "/target.npy";
            const std::string link = linked + "/aaa_link.npy";   // sorts first
            const std::string rootLink = root + "/linked-root";
            const std::string plain = linked + "/zzz_plain.npy";
            writeLFrame(target, 77);
            writeLFrame(plain, 5);
            std::error_code se, rse;
            // ABSOLUTE. A symlink's target is resolved relative to the LINK's
            // own directory, not to this process's working directory, and every
            // path in this test is relative to the latter (tools/testdata/...) -
            // so the written-as-given target produced a DANGLING link, and the
            // reader met FileNotFoundError instead of the target's pixels.
            const std::filesystem::path targetAbs =
                std::filesystem::absolute(pathFromUtf8(target), ec).lexically_normal();
            const std::filesystem::path outsideAbs =
                std::filesystem::absolute(pathFromUtf8(outside), ec).lexically_normal();
            std::filesystem::create_symlink(targetAbs, pathFromUtf8(link), se);
            std::filesystem::create_directory_symlink(outsideAbs, pathFromUtf8(rootLink), rse);
            bool haveLinks = !se && !rse;
            if (haveLinks) {
                std::error_code le;
                haveLinks = std::filesystem::is_symlink(pathFromUtf8(link), le) && !le;
                // ...and it has to RESOLVE, or this test would assert that a
                // reader cannot read a broken link, which is not the question.
                if (haveLinks && !std::filesystem::exists(pathFromUtf8(link), le))
                    haveLinks = false;
                if (haveLinks)
                    haveLinks = std::filesystem::is_symlink(pathFromUtf8(rootLink), le) &&
                                !le && std::filesystem::is_directory(pathFromUtf8(rootLink), le) &&
                                !le;
            }
            auto knownWindowsSymlinkLimit = [](const std::error_code& e) {
#if defined(_WIN32)
                return e.value() == ERROR_PRIVILEGE_NOT_HELD ||
                       e.value() == ERROR_ACCESS_DENIED ||
                       e.value() == ERROR_NOT_SUPPORTED ||
                       e.value() == ERROR_INVALID_FUNCTION ||
                       e == std::errc::operation_not_permitted ||
                       e == std::errc::permission_denied ||
                       e == std::errc::function_not_supported;
#else
                (void)e;
                return false;
#endif
            };
            if (!haveLinks) {
                // Only the known Windows privilege/capability limitation is a
                // skip. A typo, dangling fixture or unexpected filesystem error
                // is a failed test on every platform, not a green blind spot.
                const bool maySkip = (se || rse) &&
                                     (!se || knownWindowsSymlinkLimit(se)) &&
                                     (!rse || knownWindowsSymlinkLimit(rse));
                if (maySkip)
                    fprintf(stderr, "rreaderselftest:   R19 symlinks unavailable on this "
                                    "Windows host (file=\"%s\" root=\"%s\"): "
                                    "SKIPPING R19a-R19o\n",
                            se.message().c_str(), rse.message().c_str());
                else
                    check(false,
                          "R19a (setup) file and root-directory symlinks resolve correctly");
            } else {
                check(true, "R19a (setup) file and root-directory symlinks resolve correctly");
                const std::string rdLink = root + "/linkfolder.py";
                writeText(rdLink,
                          "import numpy as np, os\n"
                          "from viewer_import import Stack\n"
                          "VERSION = 1\n"
                          "def load(path):\n"
                          "    fs = []\n"
                          "    for r, d, f in os.walk(path):\n"
                          "        for n in f:\n"
                          "            if n.endswith('.npy'):\n"
                          "                fs.append(os.path.join(r, n))\n"
                          "    a = np.stack([np.load(p) for p in sorted(fs)])\n"
                          "    return Stack(np.ascontiguousarray(a.astype(np.uint16)),\n"
                          "                 name='linked')\n");
                const std::string linkSpec = rdLink + ":load";
                std::string linkText[3] = { std::string(), files[1], files[2] };
                check(slurp(rdLink, linkText[0]),
                      "R19b (setup) the link-walking reader is readable");

                std::string fid;
                check(!adapter::folderIdentity(linked, fid),
                      "R19c a folder with a link below it has NO identity, so nothing "
                      "may cache it");
                std::string rid;
                check(!adapter::folderIdentity(rootLink, rid),
                      "R19d a root directory symlink has NO identity either");
                std::string pid;
                check(adapter::folderIdentity(outside, pid) && !pid.empty(),
                      "R19e ...while its real target has one: the refusal is the link");

                // ---- the LOCAL door ---------------------------------------------
                auto firstPx = [&]() -> float {
                    return cur() && !cur()->src->data.empty() ? cur()->src->data[0] : -1.0f;
                };
                const std::string o1 = openWithReader(linked, linkSpec);
                check(o1.empty() && firstPx() == 77.0f,
                      "R19f the local door reads it: the child link's target is first");
                closeAll();
                // ONLY the target's bytes change. The link still says the same
                // thing, the folder still holds the same two names, and every
                // stat inside `linked` is what it was.
                {
                    const auto was = std::filesystem::last_write_time(pathFromUtf8(target), ec);
                    writeLFrame(target, 88);
                    std::filesystem::last_write_time(pathFromUtf8(target),
                                                     was + std::chrono::seconds(10), ec);
                }
                const int lruns = g_adapterRuns;
                const std::string o2 = openWithReader(linked, linkSpec);
                fprintf(stderr, "rreaderselftest:   R19 local: err=\"%s\" runs %d -> %d "
                                "px0=%g\n",
                        o2.c_str(), lruns, g_adapterRuns, (double)firstPx());
                check(o2.empty() && g_adapterRuns > lruns,
                      "R19g a target rewritten behind the child link re-runs locally");
                check(firstPx() == 88.0f,
                      "R19h ...and returns the new target, not a cached one");
                closeAll();
                forgetReader(linked);

                // ---- the PEER's key ---------------------------------------------
                // Over there the reply must always carry a key (that is how the
                // client addresses what it is about to read), so "uncacheable"
                // means the key is unique to the RUN: two asks with nothing
                // changed are two keys and two runs, and a rewritten target can
                // never be answered out of the cache.
                auto linkRun = [&](const std::string& source, std::string& keyOut,
                                   bool& cachedOut) {
                    remote::Session s;
                    if (!openSession(s, true)) return false;
                    remote::ReaderRun r;
                    std::string e;
                    const bool sent = s.readerRun(source, "load", linkText, r, e);
                    const bool got = sent && r.outcome == rp::RO_OK;
                    if (got) { keyOut = r.key; cachedOut = has(r.stderrText, "read from cache"); }
                    else
                        fprintf(stderr,
                                "rreaderselftest:   R19 peer run failed: source=\"%s\" "
                                "sent=%d outcome=%d transport=\"%s\" err=\"%s\" stderr=\"%s\"\n",
                                source.c_str(), sent ? 1 : 0,
                                sent ? (int)r.outcome : -1, e.c_str(), r.err.c_str(),
                                r.stderrText.c_str());
                    s.stop();
                    return got;
                };
                std::string pk1, pk2, pk3;
                bool pc1 = false, pc2 = false, pc3 = false;
                const bool p1 = linkRun(linked, pk1, pc1);
                {
                    const auto was = std::filesystem::last_write_time(pathFromUtf8(target), ec);
                    writeLFrame(target, 99);
                    std::filesystem::last_write_time(pathFromUtf8(target),
                                                     was + std::chrono::seconds(10), ec);
                }
                const bool p2 = linkRun(linked, pk2, pc2);
                const bool p3 = linkRun(linked, pk3, pc3);
                fprintf(stderr, "rreaderselftest:   R19 peer keys %s / %s / %s "
                                "cached %d/%d/%d\n",
                        pk1.c_str(), pk2.c_str(), pk3.c_str(), pc1 ? 1 : 0, pc2 ? 1 : 0,
                        pc3 ? 1 : 0);
                check(p1 && p2 && pk2 != pk1 && !pc2,
                      "R19i the rewritten child-link target is a new peer key and miss");
                check(p3 && pk3 != pk2 && !pc3,
                      "R19j the peer never caches a folder containing a child link");

                // The root link is a different hole: recursive iteration never
                // yields the entry it started from. Exercise both doors, not
                // merely folderIdentity, so a caller cannot accidentally follow
                // it before consulting the shared helper.
                closeAll();
                const std::string ro1 = openWithReader(rootLink, linkSpec);
                const int rootRuns = g_adapterRuns;
                closeAll();
                const std::string ro2 = openWithReader(rootLink, linkSpec);
                check(ro1.empty() && ro2.empty() && g_adapterRuns > rootRuns,
                      "R19k the local door re-runs an unchanged root directory symlink");
                check(firstPx() == 99.0f,
                      "R19l ...and reads the current target rather than a materialisation");
                closeAll();
                forgetReader(rootLink);

                std::string rk1, rk2;
                bool rc1 = false, rc2 = false;
                const bool rp1 = linkRun(rootLink, rk1, rc1);
                const bool rp2 = linkRun(rootLink, rk2, rc2);
                check(rp1 && rp2 && !rk1.empty() && rk2 != rk1,
                      "R19m the peer gives each root-directory-symlink run a new key");
                check(rp1 && rp2 && !rc1 && !rc2,
                      "R19n ...and neither unchanged ask is reported as a cache hit");
                std::string rootId;
                check(!adapter::folderIdentity(rootLink, rootId) && rootId.empty(),
                      "R19o the shared identity helper remains an explicit refusal");
            }
            closeAll();
        }

        {   // ---- R20: one key, one exclusion for removal AND every publish ----
            //
            // A stale invalid verdict and a producer publish must use the same
            // per-key exclusion. A stops after observing rubbish. C removes it
            // and begins producing, but waits inside the reader. B then produces
            // and publishes first. Before A or C is released, B's issued key is
            // used for real META and TILE requests. Finally A (old verdict) and C
            // (old producer) resume together. Both must recheck under the lock,
            // accept B's valid canonical and leave the exact file in place.
            // C adds 1000 to its pixels, deliberately, so a forbidden late
            // replacement is visible even on a filesystem whose mtime cannot
            // distinguish the two publishes.
            //
            // Every ordering is a file handshake. No scheduler luck or timeout
            // is used to decide which participant has reached which state.
            closeAll();
            const std::string rdRace3 = root + "/race3.py";
            writeText(rdRace3,
                      "import numpy as np, os, time\n"
                      "from viewer_import import Stack\n"
                      "VERSION = 1\n"
                      "def load(path):\n"
                      "    slow = False\n"
                      "    if os.path.exists(path + '.producer-arm'):\n"
                      "        try:\n"
                      "            os.close(os.open(path + '.producer-started',\n"
                      "                             os.O_CREAT | os.O_EXCL | os.O_WRONLY))\n"
                      "            slow = True\n"
                      "            while not os.path.exists(path + '.producer-release'):\n"
                      "                time.sleep(0.01)\n"
                      "        except FileExistsError:\n"
                      "            pass\n"
                      "    a = np.load(path)\n"
                      "    if slow:\n"
                      "        a = a + 1000\n"
                      "    return Stack(np.ascontiguousarray(\n"
                      "        a.reshape(a.shape[0], a.shape[1], a.shape[2])), name='race3')\n");
            std::string race3Text[3] = { std::string(), files[1], files[2] };
            check(slurp(rdRace3, race3Text[0]),
                  "R20a (setup) the three-party reader is readable");
            const std::string hold = root + "/judge.hold";
            const std::string producerArm = origin + ".producer-arm";
            const std::string producerStarted = origin + ".producer-started";
            const std::string producerRelease = origin + ".producer-release";
            std::filesystem::remove(pathFromUtf8(hold), ec);
            std::filesystem::remove(pathFromUtf8(hold + ".at"), ec);
            std::filesystem::remove(pathFromUtf8(producerArm), ec);
            std::filesystem::remove(pathFromUtf8(producerStarted), ec);
            std::filesystem::remove(pathFromUtf8(producerRelease), ec);

            // ---- the setup: learn the key, then put rubbish under it ----------
            std::string raceKey;
            {
                setEnv("VIEWER_SERVE_CACHE_JUDGE_HOLD", "");
                remote::Session s;
                if (!openSession(s, true)) return 1;
                remote::ReaderRun r;
                std::string e;
                const bool sent = s.readerRun(origin, "load", race3Text, r, e);
                if (sent && r.outcome == rp::RO_OK) raceKey = r.key;
                s.stop();
            }
            const std::string racePath = cacheDir + "/" + raceKey + ".vstream";
            check(!raceKey.empty() &&
                      std::filesystem::exists(pathFromUtf8(racePath), ec),
                  "R20b (setup) one run publishes under a key this test can now name");
            writeText(racePath, "this is not a viewer stream at all");

            struct Side {
                bool sent = false;
                int outcome = -1;
                std::string key, text, transport, err;
            };
            Side A, B, C;
            // A is held after the invalid observation; B and C are ordinary.
            setEnv("VIEWER_SERVE_CACHE_JUDGE_HOLD", hold);
            remote::Session sa;
            if (!openSession(sa, true)) return 1;
            setEnv("VIEWER_SERVE_CACHE_JUDGE_HOLD", "");
            remote::Session sb, sc;
            if (!openSession(sb, true)) return 1;
            if (!openSession(sc, true)) return 1;

            std::thread ta([&] {
                remote::ReaderRun r;
                std::string e;
                A.sent = sa.readerRun(origin, "load", race3Text, r, e);
                A.transport = e;
                if (A.sent) {
                    A.outcome = (int)r.outcome; A.key = r.key;
                    A.text = r.stderrText; A.err = r.err;
                }
            });
            // A has read the entry, called it rubbish, and is now standing in
            // the window. That is not a guess: the peer says so by writing this
            // file, so what follows really is "another process publishes while
            // the first still believes there is nothing here".
            bool held = false;
            for (int i = 0; i < 1500 && !held; i++) {
                held = std::filesystem::exists(pathFromUtf8(hold + ".at"), ec);
                if (!held) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            check(held, "R20c the first peer judged the entry unusable and is in the window");

            // C owns the slow-producer handshake. It removes the rubbish under
            // the key lock, releases that lock, and writes only to its .part.
            writeText(producerArm, "armed\n");
            std::thread tc([&] {
                remote::ReaderRun r;
                std::string e;
                C.sent = sc.readerRun(origin, "load", race3Text, r, e);
                C.transport = e;
                if (C.sent) {
                    C.outcome = (int)r.outcome; C.key = r.key;
                    C.text = r.stderrText; C.err = r.err;
                }
            });
            bool producing = false;
            for (int i = 0; i < 1500 && !producing; i++) {
                producing = std::filesystem::exists(pathFromUtf8(producerStarted), ec);
                if (!producing) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            check(producing, "R20d a third peer is producing the same key into its own temp");
            if (!producing)
                // Keep the failure diagnostic finite: if C never reached the
                // handshake, a later participant must not become the waiter.
                writeText(producerRelease, "abort\n");

            // B is the fast producer and publishes while both other peers wait.
            {
                remote::ReaderRun r;
                std::string e;
                B.sent = sb.readerRun(origin, "load", race3Text, r, e);
                B.transport = e;
                if (B.sent) {
                    B.outcome = (int)r.outcome; B.key = r.key;
                    B.text = r.stderrText; B.err = r.err;
                }
            }
            check(B.sent && B.outcome == (int)rp::RO_OK && B.key == raceKey &&
                      !has(B.text, "read from cache"),
                  "R20e B publishes while stale A and producer C are unfinished");

            // The key is a capability, not a promise that becomes true when the
            // other requests finish. Consume it now, before either is released.
            bool bMeta = false, bTile = false;
            uint16_t bFirst = 0;
            if (B.sent && B.outcome == (int)rp::RO_OK) {
                remote::KeyedRef bref{ B.key, 0, remote::KeyedRef::FromReader };
                remote::Meta bm;
                std::string be;
                bMeta = sb.meta(url, bm, be, 0, nullptr, &bref);
                std::vector<uint8_t> raw;
                int tw = 0, th = 0, tch = 0;
                uint32_t dt = 0;
                bTile = bMeta && sb.tileBytes(url, 0, 0, 0, bm.w, bm.h, 1, raw,
                                              tw, th, tch, dt, be, 0, nullptr, &bref);
                if (bTile && raw.size() >= sizeof bFirst)
                    memcpy(&bFirst, raw.data(), sizeof bFirst);
            }
            check(bMeta && bTile && bFirst == 11,
                  "R20f B's issued key serves META and TILE before A or C finishes");
            const auto publishedAt = std::filesystem::last_write_time(pathFromUtf8(racePath), ec);

            // A's old verdict and C's old producer now compete for the same lock.
            // Neither may move or replace the valid canonical they find there.
            writeText(hold, "go\n");
            writeText(producerRelease, "go\n");
            ta.join();
            tc.join();
            int vstreams = 0, parts = 0, bads = 0;
            {
                std::error_code de;
                for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), de)) {
                    const std::string nm = d.path().filename().u8string();
                    auto ends = [&](const std::string& s) {
                        return nm.size() > s.size() &&
                               nm.compare(nm.size() - s.size(), s.size(), s) == 0;
                    };
                    if (ends(".vstream")) vstreams++;
                    else if (ends(".part")) parts++;
                    else if (ends(".bad")) bads++;
                }
            }
            fprintf(stderr, "rreaderselftest:   R20 A=%d/%s \"%s\" B=%d/%s "
                            "C=%d/%s \"%s\" | cache %d vstream / %d part / %d bad\n",
                    A.outcome, A.key.c_str(), A.text.c_str(), B.outcome, B.key.c_str(),
                    C.outcome, C.key.c_str(), C.text.c_str(), vstreams, parts, bads);
            check(A.sent && A.outcome == (int)rp::RO_OK && A.key == raceKey &&
                      has(A.text, "read from cache"),
                  "R20g stale A rechecks under the lock and accepts B's canonical");
            check(C.sent && C.outcome == (int)rp::RO_OK && C.key == raceKey &&
                      !has(C.text, "read from cache"),
                  "R20h producer C loses publication without replacing B's result");
            const auto afterInterleave =
                std::filesystem::last_write_time(pathFromUtf8(racePath), ec);
            bool afterMeta = false, afterTile = false;
            uint16_t afterFirst = 0;
            if (B.sent && B.outcome == (int)rp::RO_OK) {
                remote::KeyedRef bref{ B.key, 0, remote::KeyedRef::FromReader };
                remote::Meta bm;
                std::string be;
                afterMeta = sb.meta(url, bm, be, 0, nullptr, &bref);
                std::vector<uint8_t> raw;
                int tw = 0, th = 0, tch = 0;
                uint32_t dt = 0;
                afterTile = afterMeta &&
                    sb.tileBytes(url, 0, 0, 0, bm.w, bm.h, 1, raw, tw, th, tch, dt,
                                 be, 0, nullptr, &bref);
                if (afterTile && raw.size() >= sizeof afterFirst)
                    memcpy(&afterFirst, raw.data(), sizeof afterFirst);
            }
            check(std::filesystem::exists(pathFromUtf8(racePath), ec) &&
                      publishedAt == afterInterleave && afterMeta && afterTile &&
                      afterFirst == bFirst && afterFirst == 11 &&
                      bads == 0 && parts == 0,
                  "R20i B's canonical is unchanged and no private temp is left behind");
            {   // ...and it still answers, after every stale participant finished.
                remote::Session s;
                if (!openSession(s, true)) return 1;
                remote::ReaderRun r;
                std::string e;
                const bool sent = s.readerRun(origin, "load", race3Text, r, e);
                check(sent && r.outcome == rp::RO_OK && r.key == raceKey &&
                          has(r.stderrText, "read from cache"),
                      "R20j a later RUN is a hit on the same whole result");
                s.stop();
            }
            sa.stop();
            sb.stop();
            sc.stop();
            setEnv("VIEWER_SERVE_CACHE_JUDGE_HOLD", "");
            std::filesystem::remove(pathFromUtf8(producerArm), ec);
            std::filesystem::remove(pathFromUtf8(producerStarted), ec);
            std::filesystem::remove(pathFromUtf8(producerRelease), ec);
            closeAll();
        }

        setEnv("VIEWER_SERVE_CACHE", "");
        closeAll();
        app.uiSession.stop();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "rreaderselftest: %s\n", ok ? "ok" : "FAILED");
        stopWatchWorker();
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
