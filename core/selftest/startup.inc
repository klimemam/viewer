// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // A start comes back where the last one left off (#319), verifiable without
    // a human: the decision startupRestoreDecide makes for every case of its
    // inputs, what parseCli reports about a command line, which autosave the
    // scan offers, what startupRestoreApply does with a good, a broken and an
    // unwanted one, the head row of File > Recent Sessions - and, first, that
    // THIS run, a scripted one, opened nothing of the user's.
    //
    // Fixtures live in a scratch folder of their own and the scan is pointed at
    // it, not at the config dir, so no window's autosave is read or written.
    // Every app field and global this test moves is put back.
    if (!g_startupSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "startupselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        std::error_code ec;
        const std::filesystem::path scratchP =
            std::filesystem::temp_directory_path(ec) / "viewer_startup_selftest";
        std::filesystem::remove_all(scratchP, ec);
        std::filesystem::create_directories(scratchP, ec);
        const std::string dir = scratchP.generic_u8string();
        const std::string head = g_startupSelftest + "/00/frame_000.npy";
        const std::string toastBefore = app.toast;
        const bool toastErrBefore = app.toastErr;

        // ---- S0: this run itself ---------------------------------------------
        // Every selftest goes through the same startup, so this is the line that
        // says none of them opens the user's autosave: the decision was taken,
        // it was "scripted", and nothing was open when the test began.
        check(g_startupRestore.why == SR_Scripted && g_startupRestore.path.empty(),
              "S0 this scripted run decided SR_Scripted and named no file");
        check(app.images.empty(), "S0 ...and nothing was opened for it");

        // ---- S1: the decision, every case -----------------------------------
        {
            const std::string f = dir + "/autosave.vsession";
            auto run = [&](bool scripted, bool noSession, bool cliOpens,
                           const std::string& offer, uintmax_t bytes) {
                StartupRestoreIn in;
                in.scripted = scripted; in.noSession = noSession; in.cliOpens = cliOpens;
                in.offer = offer; in.offerBytes = bytes;
                return startupRestoreDecide(in);
            };
            StartupRestore d = run(false, false, false, f, 100);
            check(d.why == SR_Open && d.path == f, "S1 nothing named, an autosave: open it");
            d = run(false, false, true, f, 100);
            check(d.why == SR_CommandLine && d.path.empty(),
                  "S1 the command line names something: do not open it");
            d = run(false, true, false, f, 100);
            check(d.why == SR_NoSession && d.path.empty(), "S1 --no-session: do not open it");
            d = run(false, true, true, f, 100);
            check(d.why == SR_NoSession && d.path.empty(),
                  "S1 --no-session beside a file: not opened, said as --no-session");
            d = run(true, false, false, f, 100);
            check(d.why == SR_Scripted && d.path.empty(), "S1 a scripted run: do not open it");
            d = run(true, true, true, f, 100);
            check(d.why == SR_Scripted, "S1 scripted outranks every other reason");
            d = run(false, false, false, "", 0);
            check(d.why == SR_NoAutosave && d.path.empty(), "S1 no autosave: start empty");
            d = run(false, false, false, f, 0);
            check(d.why == SR_EmptyAutosave && d.path.empty(), "S1 an empty autosave: start empty");
            check(std::string(startupRestoreWhyName(SR_Open)) == "open" &&
                  std::string(startupRestoreWhyName(SR_NoSession)) == "--no-session",
                  "S1 the reasons have names");
        }

        // ---- S2: what parseCli says about a command line ---------------------
        {
            const bool noBefore = g_cliNoSession, opensBefore = g_cliNamesOpen;
            const int cmpBefore = app.pendingCompare;
            const bool zBefore = app.pendingViewZoom;
            const float zoomBefore = app.pendingZoom;
            auto parse = [&](std::vector<std::string> a) {
                g_cliNoSession = false; g_cliNamesOpen = false;
                a.insert(a.begin(), "viewer");
                std::vector<char*> av;
                for (auto& s : a) av.push_back(&s[0]);
                parseCli((int)av.size(), av.data());
            };
            parse({});
            check(!g_cliNoSession && !g_cliNamesOpen, "S2 a bare start names nothing");
            parse({ "--no-session" });
            check(g_cliNoSession && !g_cliNamesOpen, "S2 --no-session is read, and names nothing");
            parse({ "--compare", "wipe" });
            check(g_cliNamesOpen, "S2 --compare counts as naming something");
            parse({ "--raw-size", "8x8", "--zoom", "2" });
            check(!g_cliNamesOpen, "S2 declarations and view flags alone name nothing");
            parse({ "--session", dir + "/not_there.vsession" });
            check(g_cliNamesOpen, "S2 --session names something (even a missing file)");
            parse({ dir + "/not_there.npy" });
            check(g_cliNamesOpen, "S2 a positional file names something");
            parse({ dir });
            check(g_cliNamesOpen, "S2 a positional folder names something");
            closeAll();
            g_cliNoSession = noBefore; g_cliNamesOpen = opensBefore;
            app.pendingCompare = cmpBefore;
            app.pendingViewZoom = zBefore;
            app.pendingZoom = zoomBefore;
        }

        // ---- S3: which autosave the scan offers -------------------------------
        auto put = [&](const std::string& name, const std::string& body, int minutesAgo) {
            const std::string p = dir + "/" + name;
            { std::ofstream f(pathFromUtf8(p), std::ios::binary); f << body; }
            std::filesystem::last_write_time(pathFromUtf8(p),
                std::filesystem::file_time_type::clock::now() - std::chrono::minutes(minutesAgo), ec);
            return p;
        };
        {
            put("autosave-2.vsession", "viewer-session 1\n", 30);
            put("autosave.vsession", "", 1);       // newer, but a crash file never written
            std::string nm;
            recoveryOfferIn(dir, 0, &nm);
            check(nm == "autosave-2.vsession",
                  "S3 a 0-byte autosave is not offered over an older real one");
            std::filesystem::remove(pathFromUtf8(dir + "/autosave.vsession"), ec);
            std::filesystem::remove(pathFromUtf8(dir + "/autosave-2.vsession"), ec);
            check(recoveryOfferIn(dir, 0, &nm).empty() && nm.empty(), "S3 no autosave: no offer");
        }

        // ---- S4: the apply - a good one, a broken one, an unwanted one -------
        {
            closeAll();
            check(loadNpy(head).empty(), "S4 a frame to autosave is open");
            const std::string ap = dir + "/autosave.vsession";
            check(saveSession(ap, true), "S4 an autosave is written");
            closeAll();
            StartupRestore d; d.why = SR_Open; d.path = ap;
            startupRestoreApply(d);
            check(d.why == SR_Open && app.images.size() == 1 &&
                      app.images[0]->src->path.find("frame_000.npy") != std::string::npos,
                  "S4 the autosave is opened");
            check(app.toast == "restored last session (autosave, " + autosaveWhen(ap) +
                                   ") - start with --no-session to skip" && !app.toastErr,
                  "S4 the toast says so, with the file's time and the way out");
            check(autosaveWhen(ap).size() == 16, "S4 ...the time reads YYYY-MM-DD HH:MM");
            check(std::filesystem::exists(pathFromUtf8(ap), ec),
                  "S4 the autosave is still there after it was opened");
            closeAll();

            put("autosave.vsession", "this is not a session\n", 0);
            const size_t logBefore = app.msgLog.size();
            d = StartupRestore(); d.why = SR_Open; d.path = ap;
            startupRestoreApply(d);
            int lines = 0;
            for (size_t i = logBefore; i < app.msgLog.size(); i++)
                if (app.msgLog[i].text.rfind("last session not restored: autosave.vsession", 0) == 0)
                    lines++;
            check(d.why == SR_Broken && app.images.empty() && lines == 1,
                  "S4 a broken autosave: one Messages line, start empty");

            d = StartupRestore(); d.why = SR_NoSession;
            startupRestoreApply(d);
            check(app.images.empty() && std::filesystem::exists(pathFromUtf8(ap), ec),
                  "S4 --no-session opens nothing and keeps the autosave");
        }

        // ---- S5: File > Recent Sessions' head row -----------------------------
        {
            std::filesystem::remove(pathFromUtf8(dir + "/autosave.vsession"), ec);
            check(lastAutosaveRow(dir, 0).path.empty(), "S5 no autosave: no Last autosave row");
            const std::string ap = put("autosave.vsession", "viewer-session 1\n", 5);
            const LastAutosaveRow r = lastAutosaveRow(dir, 0);
            check(r.path == ap && r.label == "Last autosave (" + autosaveWhen(ap) + ")",
                  "S5 an autosave: the row is 'Last autosave (<time>)' and opens it");
            // The door this row replaces is gone from the File menu. Read from the
            // source, because the menu is drawn rather than modelled: the label
            // must not come back as a second door to the same file.
            std::vector<uint8_t> src;
            // (the working directory is the source root, as for every selftest)
            const bool read = readFileBytes("core/ui/menus.inc", src);
            const std::string s(src.begin(), src.end());
            check(read && s.find("BeginMenu(\"Recent Sessions\"") != std::string::npos &&
                      s.find("Restore last session") == std::string::npos,
                  "S5 the File menu has no 'Restore last session'");
            check(read && s.find("lastAutosaveRow(") != std::string::npos,
                  "S5 ...and Recent Sessions draws the Last autosave row");
        }

        // ---- S6: the crash fd is this process's alone -----------------------
        // A child that inherits it keeps the autosave open after
        // releaseCrashFileFor closes ours, and the rename that replaces the
        // autosave fails: a windowed run's reader peer did exactly that, and no
        // autosave of a windowed run on Windows ever landed.
        {
            bool inheritable = true;
#if defined(_WIN32)
            DWORD fl = 0;
            const HANDLE h = (HANDLE)_get_osfhandle(g_crashFd);
            if (g_crashFd >= 0 && h != INVALID_HANDLE_VALUE && GetHandleInformation(h, &fl))
                inheritable = (fl & HANDLE_FLAG_INHERIT) != 0;
#else
            const int fl = g_crashFd >= 0 ? fcntl(g_crashFd, F_GETFD) : -1;
            if (fl >= 0) inheritable = (fl & FD_CLOEXEC) == 0;
#endif
            check(g_crashFd >= 0 && !inheritable,
                  "S6 the crash fd on the autosave is not inherited by children");
        }

        std::filesystem::remove_all(scratchP, ec);
        closeAll();
        app.toast = toastBefore;
        app.toastErr = toastErrBefore;
        stopSequenceLoader();
        fprintf(stderr, "startupselftest: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
