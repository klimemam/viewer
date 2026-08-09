// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // A sweep laid out as ONE multi-frame .npy per level (levelfiles/lv000/
    // capture.npy ...), which is an ordinary capture layout and which no other
    // fixture here has: every existing series fixture is 24 numbered files per
    // level, so every group is multi-file and every stack is minted by
    // startSequenceLoad - the one path that ever applied the group name. This
    // one goes through loadNpy (local) and openRemote (remote) instead, and it
    // asserts the two things that broke there: the NAME (the canon's
    // フォルダ/パターン, which the value proposal reads and the sweep matched on)
    // and a linearity fit recovering the numbers the fixture injected.
    if (!g_sweepFileSelftest.empty()) {
        std::string dir = g_sweepFileSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        bool ok = true;
        auto check = [&](const char* what, bool cond) {
            fprintf(stderr, "sweepfile: %-60s %s\n", what, cond ? "ok" : "FAILED");
            if (!cond) ok = false;
        };
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 300.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen && app.seriesPending.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto sortedNames = []() {
            std::vector<std::string> v;
            for (const auto& si : app.seqs) v.push_back(si.name);
            std::sort(v.begin(), v.end());
            return v;
        };
        // the fixture: gen_levelfiles.py, sens 8 DN/lx, offset 64 DN, K 2 DN/e-,
        // read 3 DN, one 8-frame capture.npy per folder
        const char* LV[7] = { "lv000", "lv010", "lv020", "lv040", "lv080", "lv160", "lv320" };
        const double VAL[7] = { 0, 10, 20, 40, 80, 160, 320 };
        auto reportNames = [&](const char* what, const std::vector<std::string>& nm) {
            std::string j;
            for (const auto& s : nm) j += (j.empty() ? "" : ", ") + s;
            fprintf(stderr, "sweepfile: %s stack names: %s\n", what, j.c_str());
        };
        auto checkSeries = [&](const char* what) {
            const App::Series* W = app.series.empty() ? nullptr : &app.series.front();
            std::string vals;
            if (W)
                for (const auto& m : W->members) {
                    char b[32];
                    snprintf(b, sizeof b, "%.6g", m.value);
                    vals += (vals.empty() ? "" : ",");
                    vals += std::isfinite(m.value) ? b : "unset";
                }
            fprintf(stderr, "sweepfile: %s sweep -> %d series, %d member(s) [%s]\n", what,
                    (int)app.series.size(), W ? (int)W->members.size() : -1, vals.c_str());
            bool valsOk = W && W->members.size() == 7;
            for (int i = 0; valsOk && i < 7; i++)
                valsOk = std::isfinite(W->members[i].value) &&
                         fabs(W->members[i].value - VAL[i]) < 1e-9;
            check("open as a sweep makes exactly ONE series", app.series.size() == 1);
            check("...holding every single-file level (none 'could not be matched')",
                  W && W->members.size() == 7);
            check("...each at the value its FOLDER gave, in value order", valsOk);
            check("...with the parameter and unit typed in the picker",
                  W && W->paramName == "illuminance" && std::string(W->unit) == "lx");
            return W ? W->id : 0;
        };

        // ---- local: File > Open Folder, ticked as a sweep ---------------------
        openFolder(dir);
        check("the picker opened over the level folders", app.folderPickOpen);
        // ...and none of these one-file groups is a LONE FRAME: they carry a
        // frame axis, so each becomes a stack and can be a member. The picker's
        // warning must not cry wolf over exactly the layout this test is about.
        check("a one-file group with a frame axis is not flagged as unjoinable",
              pickLoneImageGroups() == 0 && app.folderPick.size() == 7);
        app.pickSweep = true;
        snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
        snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
        pickerAccept();
        loadAll();
        {
            std::vector<std::string> nm = sortedNames();
            reportNames("local", nm);
            bool namesOk = nm.size() == 7;
            for (int i = 0; namesOk && i < 7; i++)
                namesOk = nm[i] == std::string(LV[i]) + "/capture.npy";
            check("a one-file stack keeps the canon's フォルダ/パターン name", namesOk);
        }
        int sid = checkSeries("local");
        if (sid) {
            linRecompute(sid);
            const App::LinState& L = app.lin;
            fprintf(stderr, "sweepfile: fit: %d point(s) sens=%.6g offs=%.6g r2=%.8f "
                            "LEmax=%.4f K=%.6g read=%.5g (%s)\n", L.nPts, L.slope[0],
                    L.offs[0], L.r2[0], L.leMax[0], L.ptcK[0], L.readDN[0],
                    L.readFromDark ? "dark stack" : "extrapolated");
            check("the fit runs over all seven levels", L.fitValid && L.nPts == 7);
            check("...recovering the injected sens 8.0 DN/lx",
                  L.fitValid && fabs(L.slope[0] - 8.0) <= 0.08);
            check("...offset 64.0 DN", L.fitValid && fabs(L.offs[0] - 64.0) <= 1.0);
            check("...K 2.0 DN/e-", L.fitValid && fabs(L.ptcK[0] - 2.0) <= 0.2);
            check("...read 3.0 DN, MEASURED in the lv000 dark stack",
                  L.fitValid && L.readFromDark && fabs(L.readDN[0] - 3.0) <= 0.15);
        }

        // ---- the session round trip of an IN-FILE stack -----------------------
        // This fixture is the only one in the suite whose stacks are carried by
        // the FILE (one 8-frame capture.npy per level), and that is the half of
        // the session format nothing was checking. The saved line is written
        // from the frame the stack was left on - its range, its LUT, its batch -
        // but the loader rebuilds the whole stack from that one line and leaves
        // `cur()` on frame 0, so everything the line said used to land there
        // while `seqframe` put the user back on the representative frame,
        // looking at the decode-time defaults. Worse than a display slip: the
        // batch moved ONE frame out of the stack, which breaks the canon's
        // frame ⊂ stack ⊂ batch (moveStackToBatch's own invariant) and makes
        // the NEXT autosave write the leaf batch and the default range back -
        // one save/load cycle and the user's grouping is silently gone.
        //
        // A folder stack does not have this: pumpSequence copies the head's
        // black/white/lut/cfa/batch onto every sibling as they land. The assert
        // is that both halves of the format agree.
        {
            std::error_code sec;
            int sidLv = 0;
            for (const auto& si : app.seqs) if (si.name == "lv080/capture.npy") sidLv = si.id;
            std::vector<int> fr = sidLv ? framesOfSeq(sidLv) : std::vector<int>();
            check("the in-file stack to round-trip is up with its 8 frames",
                  fr.size() == 8);
            if (fr.size() == 8) {
                const int REP = 5;                 // NOT frame 0: that is the point
                selectImage(fr[REP]);
                setRange(*app.images[fr[REP]], 100.0f, 4000.0f);
                app.images[fr[REP]]->displayLut = 0;
                int nb = newBatch(uniqueBatchName("night run"));
                moveStackToBatch(sidLv, nb);
                std::string sess = (std::filesystem::temp_directory_path(sec) /
                                    "viewer_sweepfile_infile.vsession").u8string();
                saveSession(sess, true);
                std::string lerr = loadSession(sess);
                check("the in-file session reloaded", lerr.empty());
                loadAll();
                int rid = 0;
                for (const auto& si : app.seqs) if (si.name == "lv080/capture.npy") rid = si.id;
                std::vector<int> rf = rid ? framesOfSeq(rid) : std::vector<int>();
                int night = 0;
                for (const auto& b : app.batches) if (b.name == "night run") night = b.id;
                int offRange = 0, offBatch = 0, offLut = 0;
                for (int idx : rf) {
                    const ImageDoc* d = app.images[idx].get();
                    if (d->black != 100.0f || d->white != 4000.0f) offRange++;
                    if (d->batchId != night) offBatch++;
                    if (d->displayLut != 0) offLut++;
                }
                const ImageDoc* c = cur();
                fprintf(stderr, "sweepfile: in-file round trip: %d frame(s) back, "
                                "%d off-range, %d off-batch, %d off-lut; standing on "
                                "frame %d at %.6g..%.6g (batch '%s')\n",
                        (int)rf.size(), offRange, offBatch, offLut,
                        c ? c->seqIndex : -1, c ? c->black : -1, c ? c->white : -1,
                        c ? batchNameOf(c->batchId).c_str() : "");
                check("an in-file stack comes back whole, 8 frames", rf.size() == 8);
                check("...with the saved range on EVERY frame, not just frame 0",
                      rf.size() == 8 && offRange == 0);
                check("...and the saved LUT on every frame", rf.size() == 8 && offLut == 0);
                check("...the WHOLE stack in the batch the line named",
                      night != 0 && rf.size() == 8 && offBatch == 0);
                check("...standing on the representative frame, showing its range",
                      c && c->seqIndex == REP && c->black == 100.0f && c->white == 4000.0f);
            }
            std::filesystem::remove(std::filesystem::u8path(
                (std::filesystem::temp_directory_path(sec) /
                 "viewer_sweepfile_infile.vsession").u8string()), sec);
        }

        // ---- a rename while the sweep is still loading -------------------------
        // Files renames a stack (F2 / right-click) the instant its head frame
        // lands, which is long before every queue drains and the sweep resolves.
        // Renaming one stack to ANOTHER group's exact name used to lose that
        // group's member AND hand the other group's LEVEL to the renamed stack:
        // a misplaced point on the axis the fit is taken against, reported only
        // as "1 could not be matched". Identity is stamped at creation, so the
        // name the user types is just a name.
        {
            closeAll();
            openFolder(dir);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            int renamed = 0;
            double t0 = nowSec();
            while (nowSec() - t0 < 300.0) {
                pumpSequenceAndQueue();
                if (!renamed && app.seqs.size() == 2) {      // lv000, lv010 are up
                    renamed = app.seqs[1].id;
                    seqInfo(renamed)->name = "lv320/capture.npy";   // another group's
                    fprintf(stderr, "sweepfile: renamed stack %d (was lv010) to "
                                    "\"lv320/capture.npy\" mid-load\n", renamed);
                }
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen && app.seriesPending.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* W = app.series.empty() ? nullptr : &app.series.front();
            double vRenamed = std::numeric_limits<double>::quiet_NaN();
            if (W)
                for (const auto& m : W->members) if (m.seqId == renamed) vRenamed = m.value;
            fprintf(stderr, "sweepfile: after the mid-load rename -> %d series, %d "
                            "member(s), the renamed stack is at %.6g\n",
                    (int)app.series.size(), W ? (int)W->members.size() : -1, vRenamed);
            check("a mid-load rename loses no member", W && W->members.size() == 7);
            check("...and the renamed stack keeps ITS level, not the name's",
                  renamed && fabs(vRenamed - 10.0) < 1e-9);
        }

        // ---- remote: the same layout over the peer ----------------------------
        // openRemote mints the frame-axis stack itself and openRemoteStack used
        // to return before it could say which folder it came from.
        closeAll();
        startRemote(rbMain(), "local://" + dir);
        {
            double t0 = nowSec();
            bool scanSent = false;
            while (nowSec() - t0 < 300.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                if (rbMain().b.connected && !scanSent) {
                    App::RbJob j; j.kind = App::RbScan;
                    j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = dir;
                    rbEnqueue(rbMain(), std::move(j));
                    scanSent = true;
                }
                if (app.folderPickOpen && app.folderPickRemote) {
                    app.pickSweep = true;
                    snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
                    snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
                    pickerAccept();
                }
                if (scanSent && !rbMain().busy && !app.seqs.empty() && !app.folderPickOpen &&
                    app.rbOpenQueue.empty() && app.rfPending == 0 &&
                    app.seriesPending.empty())
                    break;
                if (!rbMain().b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        {
            std::vector<std::string> nm = sortedNames();
            reportNames("remote", nm);
            bool namesOk = nm.size() == 7;
            for (int i = 0; namesOk && i < 7; i++)
                namesOk = nm[i] == std::string(LV[i]) + "/capture.npy";
            check("a remote frame-axis stack keeps it too", namesOk);
        }
        {   // ---- the Inspector's source row does not come and go mid-stack ----
            // ImageDoc::note is printed as a LINE OF ITS OWN in the Inspector's
            // Image section, with the Interpret combo and the rest of the controls
            // directly below it. openRemote wrote a note on the frame it opened
            // and pumpRemoteFetch minted the siblings without one, so stepping a
            // remote stack added and removed a row and everything under it moved
            // under the pointer. This is --derive-selftest's D4 ("every derived
            // frame carries the same origin note") for the remote producer, and
            // it is asserted the same way: EQUAL and non-empty. All-empty would
            // also hold the row count still, and would also be the regression.
            int stacks = 0, frames = 0;
            bool noteConst = true;
            std::string shown;
            for (const auto& si : app.seqs) {
                std::vector<int> fr = framesOfSeq(si.id);
                if (fr.size() < 2) { noteConst = false; continue; }   // no sibling to differ
                stacks++;
                frames += (int)fr.size();
                const std::string n0 = app.images[fr[0]]->note;
                if (n0.empty()) noteConst = false;
                shown = n0;
                for (int idx : fr)
                    if (app.images[idx]->note != n0) noteConst = false;
            }
            fprintf(stderr, "sweepfile: remote note across %d stack(s) / %d frame(s): "
                            "\"%s\"\n", stacks, frames, shown.c_str());
            check("every frame of a remote stack carries the SAME note",
                  stacks == 7 && noteConst);
            // ...and it does not repeat the size. docs/todo-open.md item 11 point 3:
            // the row directly above the note already prints "%dx%d %dch %s", so the
            // note carrying WxH put the same number twice in two adjacent rows. The
            // needle is taken from the doc, not typed in, because that is exactly
            // what the row above formats.
            char wh[64] = "";
            if (!app.images.empty())
                snprintf(wh, sizeof wh, "%dx%d", app.images[0]->w, app.images[0]->h);
            check("...and it does not repeat the WxH printed one row above",
                  wh[0] && shown.find(wh) == std::string::npos);
        }
        checkSeries("remote");
        fprintf(stderr, "sweepfile: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
