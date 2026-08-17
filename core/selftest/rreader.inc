// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- READERS RUN ON THE PEER (issue #180, docs/remote-reader-design.md) ---
    //
    // docs/input-adapters.md §4.13.1 settled on 2026-08-03 that an adapter runs
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
    // exist, did its mtime move), which is what makes "the second run did not
    // start Python" a measurement rather than a stopwatch.
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
            for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), e))
                out.push_back(d.path());
            return out;
        };

        std::string keyPerm;
        {   // ---- R1: the reader runs THERE and leaves its result THERE --------
            remote::Session ses;
            if (!openSession(ses, true)) return 1;
            check(ses.peerVersion() == (int)rp::VERSION,
                  "R1a the peer speaks the protocol this build does (14)");
            remote::ReaderRun run;
            std::string err;
            const bool sent = ses.readerRun(origin, "load", files, run, err);
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
            auto files1 = cacheFiles();
            check(files1.size() == 1,
                  "R1h the materialisation is a file on the PEER's disk, and one file");
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

            {   // ---- R2: the second RUN does not start Python -----------------
                const auto before = std::filesystem::last_write_time(files1[0], ec);
                const uintmax_t beforeSize = std::filesystem::file_size(files1[0], ec);
                remote::ReaderRun again;
                std::string e2;
                const bool got = ses.readerRun(origin, "load", files, again, e2);
                check(got && again.outcome == rp::RO_OK && again.key == keyPerm,
                      "R2a the same RUN answers with the same key");
                check(got && has(again.stderrText, "read from cache - the reader was not re-run"),
                      "R2b ...and says so in the local door's own words");
                const auto after = std::filesystem::last_write_time(files1[0], ec);
                check(before == after &&
                          beforeSize == std::filesystem::file_size(files1[0], ec),
                      "R2c the cache file did not move: Python was never started");
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
                      "R6e a failed run leaves nothing behind in the cache");
            }

            {   // ---- R7: what the reader returned is checked with the LOCAL
                //          door's own sentence (§5.2, one shared TU) ------------
                std::string wf[3] = { std::string(), files[1], files[2] };
                check(slurp(rdWideCh, wf[0]), "R7a (setup) the 6-channel reader is readable");
                remote::ReaderRun wide;
                std::string e4;
                const bool got = ses.readerRun(origin, "load", wf, wide, e4);
                const std::string both =
                    rp::npyChannelCeilingText({ 2, 4, 3, 6 }, 6);
                check(got && wide.outcome == rp::RO_UNREADABLE,
                      "R7b a shape this viewer cannot show is outcome 6, not a traceback");
                check(got && wide.err == both,
                      "R7c ...and the peer's sentence IS the local door's, character for character");
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
            check(perr == rp::npyNotNativeText({ 2, 2, 3, 4, 5 }),
                  "R7e ...with §3.2's sentence");
            check(!lerr.empty() && has(lerr, rp::npyNotNativeText({ 2, 2, 3, 4, 5 })),
                  "R7f ...and the local door refuses the same bytes with the same sentence");
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
            {
                std::string why;
                const std::string base = adapter::findPython("", why);
                if (!base.empty()) {
                    adapter::Run mk = adapter::run({ base, "-m", "venv",
                                                     "--system-site-packages",
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
                    if (pyB.empty())
                        fprintf(stderr, "rreaderselftest:   R15 venv unavailable "
                                        "(base=%s exit=%d fail=\"%s\"): SKIPPING R15c-R15j\n",
                                base.c_str(), mk.exit, mk.fail.c_str());
                }
            }
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
            }
            setEnv("VIEWER_SERVE_PYTHON", "");
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

            // ---- WITH the memo: it comes back, and no Python starts anywhere --
            // The RUN is sent again and the peer answers out of its cache (R2's
            // observation, one layer up): the materialisation's mtime is the
            // witness that the reader was not re-executed over there, and
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
