// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // The way back into a saved session (#307), verifiable without a human:
    // File > Recent Sessions (add, one row per file, the cap, "(missing)", Clear,
    // the prefs.txt round trip), the two open dialogs' filters, order and start
    // folder as openFileDialogSpec / openSessionDialogSpec hand them to pfd, and
    // a .vsession handed to openPath - which must load it, REPLACE what was open
    // and say so, and land in Recent unless it is the autosave.
    //
    // The selftest home is shared with every other test, so prefs.txt and the
    // in-memory list are put back exactly as they were found, whatever fails.
    if (!g_sessionDoorSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "sessiondoorselftest: %-60s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        std::error_code ec;
        const std::filesystem::path scratchP =
            std::filesystem::temp_directory_path(ec) / "viewer_sessiondoor_selftest";
        std::filesystem::remove_all(scratchP, ec);
        std::filesystem::create_directories(scratchP / "a", ec);
        std::filesystem::create_directories(scratchP / "b", ec);
        const std::string scratch = scratchP.generic_u8string();
        const std::string head = g_sessionDoorSelftest + "/00/frame_000.npy";
        const std::string other = g_sessionDoorSelftest + "/01/frame_000.npy";

        // ---- the shared state this test writes, captured before it writes ----
        const std::string pp = prefsPath();
        std::vector<uint8_t> prefsBefore;
        const bool hadPrefs = !pp.empty() && readFileBytes(pp, prefsBefore);
        const std::vector<std::string> recentsBefore = app.sessionRecents;
        const auto bmBefore = app.rbBookmarks;
        const auto rcBefore = app.rbRecents;
        const auto memoBefore = app.readerMemo;
        auto reload = [&]() {
            // loadPrefs APPENDS to the list-valued prefs: clear them first, as
            // newwin's N8 does, so a reload reads the file and only the file.
            app.rbBookmarks.clear();
            app.rbRecents.clear();
            app.readerMemo.clear();
            app.sessionRecents.clear();
            loadPrefs();
        };
        auto key = [](const std::string& p) { return sessionRecentKey(sessionRecentSpelling(p)); };

        // ---- D1: the list itself, pure -------------------------------------
        {
            std::vector<std::string> v;
            sessionRecentPush(v, scratch + "/a/one.vsession");
            sessionRecentPush(v, scratch + "/a/two.vsession");
            sessionRecentPush(v, scratch + "/a/one.vsession");
            check(v.size() == 2 && v[0] == scratch + "/a/one.vsession" &&
                  v[1] == scratch + "/a/two.vsession",
                  "D1 a file opened again moves to the front, one row");
            std::string back = scratch + "/a/two.vsession";
            std::replace(back.begin(), back.end(), '/', '\\');
            sessionRecentPush(v, back);
            check(v.size() == 2 && v[0] == back,
                  "D1 another spelling of the same file is the same row");
            v.clear();
            for (int i = 0; i < 11; i++)
                sessionRecentPush(v, scratch + "/a/s" + std::to_string(i) + ".vsession");
            check(v.size() == SESSION_RECENT_MAX && SESSION_RECENT_MAX == 8,
                  "D1 at most 8 rows");
            check(v.front() == scratch + "/a/s10.vsession" &&
                  v.back() == scratch + "/a/s3.vsession",
                  "D1 newest first, the oldest fall off the end");
        }

        // ---- D2: the autosave is not a named session ----------------------
        {
            const std::string cfg = viewerConfigDir();
            check(!cfg.empty(), "D2 the selftest home has a config dir");
            check(isAutosaveSessionPath(cfg + "/autosave.vsession", cfg) &&
                  isAutosaveSessionPath(cfg + "/autosave-3.vsession", cfg),
                  "D2 every slot's autosave in the config dir is the autosave");
            check(!isAutosaveSessionPath(cfg + "/autosave-x.vsession", cfg) &&
                  !isAutosaveSessionPath(cfg + "/mine.vsession", cfg) &&
                  !isAutosaveSessionPath(scratch + "/a/autosave.vsession", cfg),
                  "D2 ...and nothing else is, a moved copy included");
        }

        // ---- D3: one row of the submenu ------------------------------------
        {
            const std::string p = scratch + "/a/run3.vsession";
            const std::string there = recentSessionLabel(p, true);
            const std::string gone = recentSessionLabel(p, false);
            check(there.compare(0, 14, "run3.vsession ") == 0 &&
                  there.find(scratch + "/a") != std::string::npos &&
                  there.find("(missing)") == std::string::npos,
                  "D3 a row reads name first, then its folder");
            check(gone.size() > 9 && gone.compare(gone.size() - 9, 9, "(missing)") == 0,
                  "D3 a file that has gone says (missing)");
            check(!recentSessionExists(p), "D3 ...and is detected as gone");
        }

        // ---- D4: prefs.txt round trip and Clear ---------------------------
        {
            app.sessionRecents = { scratch + "/a/with space.vsession",
                                   scratch + "/b/two.vsession" };
            savePrefs();
            std::vector<uint8_t> bytes;
            readFileBytes(pp, bytes);
            const std::string txt(bytes.begin(), bytes.end());
            check(txt.find("\nsessionrecent " + scratch + "/a/with space.vsession\n") !=
                      std::string::npos,
                  "D4 prefs.txt carries one sessionrecent line per row");
            const std::vector<std::string> want = app.sessionRecents;
            reload();
            check(app.sessionRecents == want, "D4 the list survives prefs.txt, in order");
            check(g_prefsUnknownKeys.empty(), "D4 the key written is a key read");
            loadPrefs();                      // a second read in one process
            check(app.sessionRecents == want, "D4 reading prefs twice does not double it");

            // A hand-edited prefs.txt: twelve rows, one of them twice.
            std::string ed = "viewer-prefs 1\n";
            for (int i = 0; i < 12; i++)
                ed += "sessionrecent " + scratch + "/a/h" + std::to_string(i) + ".vsession\n";
            ed += "sessionrecent " + scratch + "/a/h0.vsession\n";
            {
                std::ofstream f(pathFromUtf8(pp), std::ios::binary);
                f << ed;
            }
            reload();
            check(app.sessionRecents.size() == SESSION_RECENT_MAX &&
                  app.sessionRecents.front() == scratch + "/a/h0.vsession" &&
                  app.sessionRecents.back() == scratch + "/a/h7.vsession",
                  "D4 a hand-edited file is capped on the way in");

            clearRecentSessions();
            check(app.sessionRecents.empty(), "D4 Clear empties the list");
            reload();
            check(app.sessionRecents.empty(), "D4 ...and the empty list is what persists");
        }

        // ---- D5: the two dialogs, as pfd is asked for them ---------------
        {
            const OpenDialogSpec o = openFileDialogSpec();
            const std::string pat = openImagesFilter();
            check(o.filters.size() == 8 && o.multiselect,
                  "D5 Open... keeps four filters and multiselect");
            check(o.filters.size() == 8 &&
                  o.filters[0].compare(0, 20, "Images and sessions ") == 0 &&
                  o.filters[1] == pat + " *.vsession",
                  "D5 Open...'s FIRST filter lists images and .vsession");
            check(o.filters.size() == 8 && o.filters[2] == "Images (" + pat + ")" &&
                  o.filters[3] == pat && o.filters[4] == "Session (*.vsession)" &&
                  o.filters[5] == "*.vsession" && o.filters[6] == "All files" &&
                  o.filters[7] == "*",
                  "D5 ...and the filters after it are the ones it always had");

            app.sessionRecents.clear();
            OpenDialogSpec s = openSessionDialogSpec();
            check(s.filters.size() >= 2 && s.filters[0] == "viewer session (*.vsession)" &&
                  s.filters[1] == "*.vsession" && !s.multiselect,
                  "D5 Open Session... starts on the .vsession filter, one file");
            check(s.startDir.empty(), "D5 with nothing remembered it starts where the OS does");
            app.sessionRecents = { scratch + "/gone/x.vsession", scratch + "/b/y.vsession",
                                   scratch + "/a/z.vsession" };
            s = openSessionDialogSpec();
            check(key(s.startDir) == key(scratch + "/b"),
                  "D5 it starts in the newest remembered folder that still exists");
            app.sessionRecents.clear();
        }

        // ---- D6: a .vsession through openPath --------------------------------
        {
            auto drain = [&]() {
                double t0 = nowSec();
                while (nowSec() - t0 < 60.0) {
                    pumpSequenceAndQueue();
                    if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                        app.seqRestore.empty() && app.restoreWait.empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            const std::string sp = scratch + "/a/door.vsession";
            closeAll();
            check(loadNpy(head).empty(), "D6 a frame to save is open");
            check(saveSession(sp, true), "D6 saveSession reports the file written");
            check(app.sessionRecents.empty(), "D6 a quiet (scripted) save is not remembered");

            closeAll();
            check(loadNpy(other).empty() && loadNpy(head).empty() && app.images.size() == 2,
                  "D6 two other images are open before the session");
            openPath(sp);
            drain();
            check(app.images.size() == 1 && key(app.images[0]->src->path) == key(head),
                  "D6 the session was loaded and replaced what was open");
            check(app.toast.find("session restored: door.vsession") != std::string::npos &&
                  app.toast.find("replaced 2 open image(s)") != std::string::npos,
                  "D6 the toast says what it replaced");
            check(app.sessionRecents.size() == 1 && key(app.sessionRecents[0]) == key(sp),
                  "D6 the opened session is the first Recent row");
            reload();
            check(app.sessionRecents.size() == 1 && key(app.sessionRecents[0]) == key(sp),
                  "D6 ...and prefs.txt already has it");

            // A path that is not there: nothing closed, nothing remembered.
            openPath(scratch + "/a/never.vsession");
            check(app.images.size() == 1 && app.sessionRecents.size() == 1,
                  "D6 a session that cannot be read closes nothing, adds nothing");

            // The autosave goes through the SAME door (Restore last session) and
            // is refused at Recent's.
            const std::string ap = viewerConfigDir() + "/autosave-63.vsession";
            check(saveSession(ap, true), "D6 an autosave-slot file is written");
            openPath(ap);
            drain();
            check(app.sessionRecents.size() == 1 && key(app.sessionRecents[0]) == key(sp),
                  "D6 opening the autosave does not enter Recent");
            std::filesystem::remove(pathFromUtf8(ap), ec);
            closeAll();
        }

        // ---- put the shared state back -----------------------------------
        app.sessionRecents = recentsBefore;
        if (hadPrefs) {
            std::ofstream f(pathFromUtf8(pp), std::ios::binary);
            f.write((const char*)prefsBefore.data(), (std::streamsize)prefsBefore.size());
        } else if (!pp.empty()) {
            std::filesystem::remove(pathFromUtf8(pp), ec);
        }
        app.rbBookmarks = bmBefore;
        app.rbRecents = rcBefore;
        app.readerMemo = memoBefore;
        std::filesystem::remove_all(scratchP, ec);
        stopSequenceLoader();
        fprintf(stderr, "sessiondoorselftest: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
