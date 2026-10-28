// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: リモート
// ---- --twowin-selftest: a second window on the same host loads the stack ------
//
// Issue #330: two windows of this PC, each connected to the same host, and the
// stack did not load. Three causes were found, and this asserts each one gone:
//
//   W1  THROUGHPUT. The pipe a peer's replies arrive on had the default buffer
//       of a few KB, and the reader's sliced loop sleeps 20 ms whenever it finds
//       nothing waiting - so a transport that hands over small writes (wsl.exe;
//       an ssh passing on small channel packets) delivered about 4 KB per
//       slice: 3.6 s per 1 MB frame, measured. A 4 MB tile is fetched here
//       through `<this binary> --fake-ssh` host `relay`, which runs the real
//       peer and passes its stdout on in 4 KB writes, and must arrive in
//       seconds with the right pixels. (POSIX pipes never had this: poll()
//       wakes on data. The check runs there too, as the same bound.)
//   W2  OPEN IN NEW WINDOW. The child is told `--stack always <head url>`, and
//       for a url that opened the head frame and nothing else - the second
//       window showed one frame of the stack the first was showing all of. A
//       remote folder stack is opened the Browse way, its newWindowArgv is
//       handed to parseCli exactly as the child receives it, and the stack
//       must come back whole: every frame, one stack. `--stack never` still
//       opens the one frame, and a lone file under `always` opens as itself.
//   W3  INSTALLING AT ONCE. Two windows that both found the peer missing or
//       stale both wrote ~/.viewer/viewer-serve.new; the temporary is now the
//       installing shell's own. The commands are checked for that on every
//       platform, and on POSIX the install is RUN with another install's
//       temporary standing at the old fixed name (as a directory, so writing
//       through it fails): it must install regardless and leave no temporary.
//
// Over local:// and a pipe, like the rest of the remote suite: the url shape,
// the code path and the listing are the ssh ones. NOGL; the ssh program and
// the stack mode are put back before it returns.
if (!g_twowinSelftest.empty()) {
    bool ok = true;
    auto check = [&](bool cond, const std::string& what) {
        fprintf(stderr, "twowinselftest: %-72s %s\n", what.c_str(), cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    std::error_code ec;
    const std::string dir = g_twowinSelftest + "/twowin-scratch";
    std::filesystem::remove_all(pathFromUtf8(dir), ec);
    std::filesystem::create_directories(pathFromUtf8(dir + "/stk"), ec);
    std::filesystem::create_directories(pathFromUtf8(dir + "/lone"), ec);
    auto writeNpy = [](const std::string& path, int w, int h, float base) {
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
        // Noise, not a ramp: the link compresses, and a ramp would cross it as
        // a few KB however large the frame - W1 needs megabytes on the wire.
        std::vector<float> px((size_t)w * h);
        uint32_t lcg = 12345u;
        for (size_t i = 0; i < px.size(); i++) {
            lcg = lcg * 1664525u + 1013904223u;
            px[i] = base + (float)(lcg >> 8);
        }
        f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
    };

    // ---- W1: a 4 MB tile through a relay that hands over 4 KB at a time ----
    {
        const int kW = 1024, kH = 1024;
        const std::string big = dir + "/big.npy";
        writeNpy(big, kW, kH, 0.0f);
        remote::setSshProgramForTest({ app.exePath, "--fake-ssh" });
        remote::Session ses;
        ses.setIdleTimeout(30.0);          // the sliced read, as app.uiSession has it
        std::string err;
        const bool up = ses.startOn("relay", 0, app.exePath, err);
        check(up, "W1 a session through the relaying ssh starts (" + err + ")");
        if (up) {
            std::vector<float> px;
            int w = 0, h = 0, ch = 0;
            std::string dtype;
            const double t0 = nowSec();
            const bool got = ses.tile(big, 0, 0, 0, 1 << 30, 1 << 30, 1, px, w, h, ch, dtype, err);
            const double took = nowSec() - t0;
            fprintf(stderr, "twowinselftest: W1 4 MB tile through the relay took %.2f s\n", took);
            check(got && w == kW && h == kH && ch == 1 && px.size() == (size_t)kW * kH,
                  "W1 the whole tile arrives (" + err + ")");
            bool same = got && px.size() == (size_t)kW * kH;
            uint32_t lcg = 12345u;
            for (size_t i = 0; same && i < px.size(); i++) {
                lcg = lcg * 1664525u + 1013904223u;
                same = px[i] == (float)(lcg >> 8);
            }
            check(same, "W1 ...with the file's pixels, every one");
            // The bound is a ceiling, not a benchmark: through this relay the
            // defect took 10 s for this tile on the machine that found it, and
            // the fix 0.4 s.
            const double kCeiling = 4.0;
            check(got && took < kCeiling, "W1 ...in seconds, not a 20 ms slice per 4 KB");
        }
        ses.stop();
        remote::setSshProgramForTest({});
    }

    // Drain every remote landing: the head opens synchronously, the rest of a
    // stack arrives on the fetch worker.
    auto drain = [&](double budget) {
        const double t = nowSec();
        while (nowSec() - t < budget) {
            pumpRemoteFetch();
            pumpSequenceAndQueue();
            if (app.rfPending == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    auto parse = [&](std::vector<std::string> a) {
        std::vector<char*> av;
        for (auto& s : a) av.push_back(&s[0]);
        parseCli((int)av.size(), av.data());
    };
    auto oneStackOf = [&](int n) {
        if (app.seqs.size() != 1 || (int)app.images.size() != n) return false;
        for (const auto& d : app.images)
            if (d->seqId != app.seqs.front().id) return false;
        return true;
    };

    // ---- W2: the argv "Open in new window" builds, opened as the child does ----
    {
        const int seqModeBefore = app.seqLoadMode;
        const int kFrames = 5;
        std::vector<std::string> files;
        for (int i = 0; i < kFrames; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "/frame_%03d.npy", i);
            files.push_back(dir + "/stk" + nm);
            writeNpy(files.back(), 16, 16, (float)(100 * i));
        }
        const std::string headUrl = makeRemoteUrl("", files.front(), 0);
        closeAll();
        // window 1: the stack, opened the way a Browse double-click opens it
        openRemoteStack("", files, "stk/frame_###.npy", 0);
        drain(60.0);
        check(oneStackOf(kFrames), "W2 window 1 has the stack (" +
              std::to_string(app.images.size()) + " frame(s))");
        const ImageDoc* head = nullptr;
        for (const auto& d : app.images) if (d->seqIndex == 0) head = d.get();
        const std::vector<std::string> av = newWindowArgv(head);
        check(av.size() >= 3 && av.back() == headUrl && av[av.size() - 2] == "always" &&
              av[av.size() - 3] == "--stack",
              "W2 its new-window argv names the head url under --stack always");
        // window 2: exactly that argv, through the command line's own door
        closeAll();
        parse(av);
        drain(60.0);
        check(oneStackOf(kFrames), "W2 window 2 opens the WHOLE stack from it (" +
              std::to_string(app.images.size()) + " frame(s), " +
              std::to_string(app.seqs.size()) + " stack(s))");
        bool allHere = app.images.size() == (size_t)kFrames;
        for (const auto& f : files) {
            bool found = false;
            for (const auto& d : app.images)
                if (d->src->remoteUrl == makeRemoteUrl("", f, 0)) found = true;
            allHere = allHere && found;
        }
        check(allHere, "W2 ...every frame of it, each from its own file");
        // never means never, and a lone file is not a stack
        closeAll();
        parse({ app.exePath, "--stack", "never", headUrl });
        drain(30.0);
        check(app.images.size() == 1 && app.seqs.empty(), "W2 --stack never opens the one frame");
        closeAll();
        const std::string lone = dir + "/lone/single_7.npy";
        writeNpy(lone, 16, 16, 7.0f);
        parse({ app.exePath, "--stack", "always", makeRemoteUrl("", lone, 0) });
        drain(30.0);
        check(app.images.size() == 1 && app.seqs.empty(),
              "W2 a lone file under --stack always opens as itself");
        closeAll();
        app.seqLoadMode = seqModeBefore;
    }

    // ---- W3: the install's temporary is the installing shell's own ----------
    {
        const std::string inst = peerInstallCommand();
        const std::string plug = pluginInstallCommand("analyzer_x.so");
        const std::string boot = bootstrapScript();
        check(inst.find("viewer-serve.new.$$") != std::string::npos &&
              inst.find("viewer-serve.new &&") == std::string::npos,
              "W3 the peer is written through a temporary of this shell's own");
        check(plug.find("analyzer_x.so.new.$$") != std::string::npos,
              "W3 ...and so is a plugin");
        check(boot.find("viewer-serve.new.$$") != std::string::npos &&
              boot.find("viewer-serve.new\"") == std::string::npos,
              "W3 ...and so is the server's own install from git");
#if !defined(_WIN32)
        // RUN it, with the old fixed name taken by another install in flight
        // (a directory, so writing through that name fails outright).
        const std::string home = dir + "/home";
        std::filesystem::create_directories(pathFromUtf8(home + "/.viewer/viewer-serve.new"), ec);
        const char* homeWas = getenv("HOME");
        const std::string homeBefore = homeWas ? homeWas : "";
        setenv("HOME", home.c_str(), 1);
        const std::string payload = "#!/bin/sh\necho viewer-serve protocol 0\n";
        std::string out, err;
        const bool ran = remote::runSshCommand("", 0, inst, payload, out, err, 30.0);
        if (homeWas) setenv("HOME", homeBefore.c_str(), 1); else unsetenv("HOME");
        std::string installed;
        {
            std::ifstream f(pathFromUtf8(home + "/.viewer/viewer-serve"), std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            installed = ss.str();
        }
        check(ran && out.find("VIEWER_SERVE_OK") != std::string::npos && installed == payload,
              "W3 it installs while another install holds the old name (" + err + ")");
        int leftovers = 0;
        for (auto& e : std::filesystem::directory_iterator(pathFromUtf8(home + "/.viewer"), ec)) {
            const std::string n = e.path().filename().u8string();
            if (n.compare(0, 17, "viewer-serve.new.") == 0) leftovers++;
        }
        check(leftovers == 0, "W3 ...and leaves no temporary of its own behind");
#else
        fprintf(stderr, "twowinselftest: W3 the install is not run here: there is no "
                        "local sh on Windows (runSshCommand refuses an empty host)\n");
#endif
    }

    // the orderly shutdown every early-returning selftest does: an unjoined
    // worker at exit is std::terminate() under the last line printed (W2's
    // stack starts the fetch worker and, for its server temporal, the measure one)
    stopRbWorker(); stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker();
    app.uiSession.stop();
    std::filesystem::remove_all(pathFromUtf8(dir), ec);
    fprintf(stderr, "twowinselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
