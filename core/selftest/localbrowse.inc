// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // The local-browse entry, verifiable without a human: the function behind
    // File > Browse Folder (Local)... must land the picked folder in the Browse
    // panel through the LOCAL peer - connected, empty host, entries listed.
    if (!g_localbrowseSelftest.empty()) {
        browseLocalFolder(g_localbrowseSelftest);   // exactly what the menu does
        double t0 = nowSec();
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (rbMain().b.connected && !rbMain().b.entries.empty()) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = rbMain().b;
        bool ok = B.connected && B.host.empty() && !B.entries.empty() &&
                  app.showRemote;
        fprintf(stderr, "localbrowseselftest: connected=%d host='%s' dir=%s "
                        "entries=%d showBrowse=%d%s%s\n",
                B.connected ? 1 : 0, B.host.c_str(), B.dir.c_str(),
                (int)B.entries.size(), app.showRemote ? 1 : 0,
                B.err.empty() ? "" : " err=", B.err.c_str());
        // ...and what it SAYS, which is the half that was wrong: a browse of
        // this disk claimed a machine in the window title, an ssh session in
        // the status bar and a "local://" scheme in the picker's header. The
        // mechanism above is unchanged - only the words are checked here.
        {
            std::string title = windowTitleText();
            std::string tag = peerTag(B.host);
            std::string label = peerLabel(B.host);
            std::string pickHdr = B.host.empty() ? B.dir : makeRemoteUrl(B.host, B.dir);
            bool said = title.find('[') == std::string::npos &&
                        title.find("ssh") == std::string::npos &&
                        viewingHost().empty() &&
                        tag == ICON_LOCAL && label == PEER_HERE &&
                        pickHdr.find("local://") == std::string::npos;
            fprintf(stderr, "localbrowseselftest: title=\"%s\" tag=%s peer=\"%s\" "
                            "pickerheader=\"%s\" othermachine=%d: %s\n",
                    title.c_str(), tag.c_str(), label.c_str(), pickHdr.c_str(),
                    viewingHost().empty() ? 0 : 1, said ? "ok" : "FAIL");
            if (!said) ok = false;
        }
        {   // WHICH END DECIDES THE ORDER. The peer sends a LIST in plain
            // lexicographic order - serve.cpp merges directories, singles and
            // groups into one row list and sorts it with `<` on the name - and
            // this end sorts it AGAIN before drawing, with rbSortShown. That
            // is the reason a local listing and an ssh listing of the same
            // folder read identically, and the reason the natural default
            // needed no protocol change and no peer update: the bytes on the
            // wire are unchanged and the order is decided here.
            //
            // rb/unpadded is where the claim is falsifiable, because its names
            // are not zero-padded: the REPLY arrives lv1, lv10, lv2 and the
            // SCREEN must read lv1, lv2, lv10. If the client ever stopped
            // sorting and drew the reply's order, this is the assertion that
            // notices - on the local door, which is the same LIST path.
            std::string sub = B.dir + "/unpadded";
            remoteBrowseTo(rbMain(), sub);
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == sub && !B.entries.empty()) break;
                if (!B.err.empty()) break;    // a dead link: say so now, not in 120 s
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<std::string> wire, screen;
            for (const auto& e : B.entries)
                if (e.dir && e.name.rfind("lv", 0) == 0) wire.push_back(e.name);
            rbMain().nameNatural = true;
            std::vector<RbRow> v = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            std::vector<int> idx(v.size());
            for (int i = 0; i < (int)v.size(); i++) idx[i] = i;
            rbSortShown(rbMain(), v, idx);
            for (int i : idx)
                if (v[i].isDir() && !v[i].up && v[i].name().rfind("lv", 0) == 0)
                    screen.push_back(v[i].name());
            auto say = [](const std::vector<std::string>& s) {
                std::string t;
                for (const auto& n : s) t += (t.empty() ? "" : " ") + n;
                return t;
            };
            bool ordOk = wire == std::vector<std::string>{ "lv1", "lv10", "lv2" } &&
                         screen == std::vector<std::string>{ "lv1", "lv2", "lv10" };
            fprintf(stderr, "localbrowseselftest: order in %s: peer sent [%s], "
                            "panel lays out [%s]: %s\n",
                    sub.c_str(), say(wire).c_str(), say(screen).c_str(),
                    ordOk ? "ok" : "FAIL");
            if (!ordOk) ok = false;
        }
        // ---- #289: the local reader is started by THIS FILE, from anywhere -----
        // The variable is the guard on the child block below, and it also names
        // the child's scratch: the child inherits this test's TMP, and a scratch
        // of the same name would remove_all the parent's - which is the child's
        // own cwd and APPDATA while it runs.
        const bool isChild = getenv("VIEWER_LOCALBROWSE_CHILD") != nullptr;
        std::error_code ec;
        const std::string farDir =
            selftestScratch(isChild ? "viewer_localbrowse_far_child" : "viewer_localbrowse_far");
        const std::string fixture =
            std::filesystem::absolute(pathFromUtf8(g_localbrowseSelftest), ec).u8string();
        {   // WHICH FILE THE LOCAL READER IS STARTED BY. rbEnqueue hands a job
            // with an empty host app.exePath, and Session::startOn runs
            // `<that> --serve` through CreateProcessW(nullptr, cmd) / execvp,
            // which resolve a relative first token against the cwd AT THAT
            // MOMENT. So the value has to be absolute; it has to be the OS's own
            // answer (selfExePath: GetModuleFileNameW / _NSGetExecutablePath /
            // /proc/self/exe) and not argv[0]'s spelling; and - the ACP half of
            // the same report - it has
            // to name a file that EXISTS when read as UTF-8, which the cp932
            // bytes of a Japanese path do not. remoteExe is the formula's other
            // operand and is asserted unset, or the three facts above would be
            // about a path nothing in this run started.
            std::filesystem::path exe = pathFromUtf8(app.exePath);
            const bool absolute = !app.exePath.empty() && exe.is_absolute();
            const bool self = app.exePath == selfExePath();
            const bool exists = std::filesystem::is_regular_file(exe, ec);
            const bool noOverride = app.remoteExe.empty();
            const bool shape = absolute && self && exists && noOverride;
            fprintf(stderr, "localbrowseselftest: local peer exe=\"%s\" absolute=%d "
                            "self=%d exists=%d remoteExe-unset=%d: %s\n",
                    app.exePath.c_str(), absolute ? 1 : 0, self ? 1 : 0, exists ? 1 : 0,
                    noOverride ? 1 : 0, shape ? "ok" : "FAIL");
            if (!shape) ok = false;
        }
        if (!isChild) {
            // THE SAME TEST, STARTED THE WAY A LAUNCHER STARTS IT: argv[0] is a
            // relative spelling WITH A DIRECTORY IN IT (`.\viewer.exe` -
            // PowerShell's own way of running the file in front of you; `./viewer`
            // elsewhere) and the cwd is a folder that does NOT hold the binary.
            // ctest cannot stage this - it hands the binary its absolute path -
            // so this process stages it for a child, which runs this very
            // selftest (minus this block: the variable is the guard) and must
            // land its local Browse. With app.exePath = argv[0] the child's
            // worker fails here with "cannot start: .\viewer.exe --serve": the
            // first token is resolved against that cwd. The directory in the
            // spelling is the point - CreateProcess looks for a BARE name in the
            // application's own directory before the cwd, so `viewer.exe` alone
            // would land either way and prove nothing.
            auto getEnvCopy = [](const char* k) {
                const char* v = getenv(k);
                return v ? std::string(v) : std::string();
            };
            auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
                _putenv_s(k, v.c_str());
#else
                if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
            };
            const std::string oldAppdata = getEnvCopy("APPDATA");
            const std::string oldHome = getEnvCopy("HOME");
            const std::string childHome = farDir + "/home";   // its own prefs and autosave slot
            std::filesystem::create_directories(pathFromUtf8(childHome), ec);
            setEnv("APPDATA", childHome);
            setEnv("HOME", childHome);
            setEnv("VIEWER_LOCALBROWSE_CHILD", "1");
            adapter::RunOpts o;
            o.timeoutMs = 300000;
            o.cwd = farDir;
#if defined(_WIN32)
            o.argv0 = ".\\viewer.exe";
#else
            o.argv0 = "./viewer";
#endif
            adapter::Run r = adapter::run(
                { app.exePath, "--no-window", "--localbrowse-selftest", fixture }, o);
            setEnv("APPDATA", oldAppdata);               // before ANY assertion can
            setEnv("HOME", oldHome);                     // return early
            setEnv("VIEWER_LOCALBROWSE_CHILD", "");
            // the child's first report line, as this process printed its own above
            const bool connected =
                r.err.find("localbrowseselftest: connected=1 host=''") != std::string::npos;
            const bool landed = r.started && r.exit == 0 && connected;
            fprintf(stderr, "localbrowseselftest: child as %s from %s: started=%d exit=%d "
                            "connected=%d: %s\n",
                    o.argv0.c_str(), farDir.c_str(), r.started ? 1 : 0, r.exit,
                    connected ? 1 : 0, landed ? "ok" : "FAIL");
            if (!landed) {
                if (!r.fail.empty()) fprintf(stderr, "localbrowseselftest:   %s\n", r.fail.c_str());
                fprintf(stderr, "---- the child's stderr ----\n%s---- end ----\n", r.err.c_str());
                ok = false;
            }
        }
        {   // ...AND WHAT IT SAYS WHEN THE FILE IS NOT THERE, through the Browse
            // door: a connect job whose exe names a file that does not exist, in
            // a folder that does. The sentence that lands in the panel must name
            // THAT file and quote the OS; "cannot start: <command line>" did
            // neither in a form a user could act on. The OS clause is asserted
            // by its number (2, file not found, on every OS this builds on):
            // the words are in the user's language. The disconnect first is not
            // ceremony - a worker whose session is alive on the same host does
            // not start a peer at all, bogus exe or not.
            std::filesystem::path bogusP = pathFromUtf8(farDir) / "no-such-viewer.exe";
            const std::string bogus = bogusP.make_preferred().u8string();
            App::BrowseInstance& I = rbMain();
            App::RbJob off;
            off.kind = App::RbDisconnect;
            rbEnqueue(I, std::move(off));
            App::RbJob j;
            j.kind = App::RbConnect;
            j.dir = fixture;
            j.exe = bogus;
            I.b.err.clear();
            rbEnqueue(I, std::move(j));
            double t2 = nowSec();
            while (nowSec() - t2 < 120.0) {
                pumpRemoteBrowse();
                if (!I.b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const std::string e = I.b.err;
            const bool names = e.find(bogus) != std::string::npos;
            const bool local = e.rfind("the local reader (", 0) == 0;
#if defined(_WIN32)
            const char* osNumber = "(windows error 2)";
#else
            const char* osNumber = "(errno 2)";
#endif
            const bool osSaid = e.find(") could not be started: ") != std::string::npos &&
                                e.find(osNumber) != std::string::npos;
            const bool said = names && local && osSaid && !I.b.connected;
            fprintf(stderr, "localbrowseselftest: bogus exe: panel says \"%s\": names-the-file=%d "
                            "local-sentence=%d os-said=%d connected=%d: %s\n",
                    e.c_str(), names ? 1 : 0, local ? 1 : 0, osSaid ? 1 : 0,
                    I.b.connected ? 1 : 0, said ? "ok" : "FAIL");
            if (!said) ok = false;
        }
        fprintf(stderr, "localbrowseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
