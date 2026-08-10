// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
    // Every A/B range combination, printed. See scratchpad/rangetest.py.
    if (!g_rangeSelftest.empty()) {
        std::string dir = g_rangeSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        app.seqLoadMode = 1;
        openFolder(dir);
        double t0 = nowSec();
        while (nowSec() - t0 < 300.0) {
            if (app.folderPickOpen && !app.folderPickRemote) {
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen && app.seqs.size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (app.seqs.size() < 2) {
            fprintf(stderr, "rangeselftest: need 2 stacks under %s\n", dir.c_str());
            return 1;
        }
        std::vector<int> fa = framesOfSeq(app.seqs[0].id), fb = framesOfSeq(app.seqs[1].id);
        if (fa.size() < 2 || fb.size() < 2) {
            fprintf(stderr, "rangeselftest: stacks need 2+ frames\n");
            return 1;
        }
        selectImage(fa[0]);
        setCompareB(app.images[fb[0]].get());
        app.compareMode = App::CmpSplit;
        app.compareFollowFrame = true;
        static const char* MODE[3] = { "auto-per-frame", "per-stack", "linked" };
        static const char* SHARE[3] = { "each-own", "B-uses-A", "union-auto" };
        int bad = 0;
        {   // The two stacks of a capture series hold identically NAMED frames
            // (00/frame_000.npy and 01/frame_000.npy are both frame_000.npy),
            // so this is also the check that the compare UI can tell them apart
            // at all: every label there goes through abDocLabel.
            std::string la = abDocLabel(cur()), lb = abDocLabel(cmpB());
            fprintf(stderr, "rangeselftest: labels | A = %s | B = %s | files %s / %s | %s\n",
                    la.c_str(), lb.c_str(), cur()->name.c_str(), cmpB()->name.c_str(),
                    la != lb ? "DISTINCT" : "AMBIGUOUS");
            if (la == lb) bad++;
        }
        {   // The out-of-the-box display-range mode. "B uses A's range" renders
            // a B captured at another level at >white 99.7% - one dashed line
            // jammed against the right edge, and an empty half in side by side.
            // Union bins both sides identically and clips neither.
            fprintf(stderr, "rangeselftest: default compareRangeMode at startup = %d (%s)%s\n",
                    g_abRangeDefault,
                    g_abRangeDefault >= 0 && g_abRangeDefault < 3 ? SHARE[g_abRangeDefault] : "?",
                    g_abRangeDefault == 2 ? "" : "  <- EXPECTED 2 (union-auto)");
            if (g_abRangeDefault != 2) bad++;
        }
        {   // The axis label has to name the range ACTUALLY in force. It read
            // "A's black-white range" in every mode, including the union - a
            // claim about the picture that the picture does not support.
            int save = app.compareRangeMode;
            bool saveLink = app.linkRange;
            // ...and "in force" includes the Value range scope. linkRange
            // short-circuits effBlack/effWhite before abRange is even consulted,
            // so with the scope set to everything the axis spans the linked pair
            // - widened by stacks that have nothing to do with the comparison -
            // whatever the A/B combo happens to say.
            for (int lk = 0; lk < 2; lk++) {
                app.linkRange = lk != 0;
                if (app.linkRange) {
                    app.linkBlack = app.images[fa[0]]->vmin;
                    app.linkWhite = app.images[fa[0]]->vmax;
                }
                for (int sh = 0; sh < 3; sh++) {
                    app.compareRangeMode = sh;
                    recomputeHistogramIfNeeded(cur(), app.hist[0]);
                    std::string lab = abHistXLabel(cur(), cmpB(), app.hist[0]);
                    bool named = app.linkRange
                                     ? lab.find("linked range") != std::string::npos
                                     : (sh == 2
                                            ? lab.find("A and B combined") != std::string::npos
                                            : lab.find("A\'s black-white") != std::string::npos);
                    // and the label has to match the NUMBERS, not just read well
                    bool spans = !app.linkRange || (effBlack(*cur()) == app.linkBlack &&
                                                    effWhite(*cur()) == app.linkWhite);
                    fprintf(stderr, "rangeselftest: hist x axis | %-6s %-11s | %s | %s\n",
                            app.linkRange ? "linked" : "own", SHARE[sh], lab.c_str(),
                            named && spans ? "names the range in force" : "WRONG");
                    if (!named || !spans) bad++;
                }
            }
            app.linkRange = saveLink;
            app.compareRangeMode = save;
        }
        for (int m = 0; m < 3; m++) {
            app.rangeScope = m == 0 ? 0 : 1;
            app.linkRange = m == 2;
            if (app.linkRange) { app.linkBlack = app.images[fa[0]]->vmin;
                                 app.linkWhite = app.images[fa[0]]->vmax; }
            for (int sh = 0; sh < 3; sh++) {
                app.compareRangeMode = sh;
                for (int step = 0; step < 2; step++) {
                    selectImage(fa[step]);
                    ImageDoc* A = cur();
                    ImageDoc* B = cmpB();
                    if (!A || !B) { fprintf(stderr, "rangeselftest: no A/B\n"); return 1; }
                    bool same = effBlack(*A) == effBlack(*B) && effWhite(*A) == effWhite(*B);
                    fprintf(stderr,
                            "rangeselftest: %-14s %-11s frame %d | A %.6g..%.6g  B %.6g..%.6g  "
                            "| B frame %d | %s\n",
                            MODE[m], SHARE[sh], step, effBlack(*A), effWhite(*A),
                            effBlack(*B), effWhite(*B), B->seqIndex,
                            same ? "MATCHED" : "differs");
                    // the contract: anything but "each-own" must produce one range
                    if (sh != 0 && !same) bad++;
                    // follow-frame must keep the sides on the same frame number
                    if (B->seqIndex != A->seqIndex) {
                        fprintf(stderr, "rangeselftest: FOLLOW BROKEN A#%d vs B#%d\n",
                                A->seqIndex, B->seqIndex);
                        bad++;
                    }
                }
            }
        }
        // Switching TO "auto per frame" must re-fit the frame on screen. The
        // policy is applied in selectImage, which only runs on a frame change,
        // so the mode used to look dead until the first step ("選んだ直後は
        // 画像がかわらず、コマ送りすると反映される").
        {
            app.linkRange = false;
            app.rangeScope = 1;                       // per stack
            selectImage(fa[1]);                       // a frame that inherited
            ImageDoc* d = cur();
            d->black = d->vmin - 1000; d->white = d->vmax + 1000;   // clearly not its own fit
            float wasB = d->black, wasW = d->white;
            applyRangeMode(0, d);              // exactly what the combo calls
            bool refit = d->black != wasB || d->white != wasW;
            fprintf(stderr, "rangeselftest: auto-per-frame applied on the spot: "
                            "%.6g..%.6g -> %.6g..%.6g (frame data %.6g..%.6g) %s\n",
                    wasB, wasW, d->black, d->white, d->vmin, d->vmax,
                    refit ? "ok" : "WRONG");
            if (!refit) bad++;
            app.rangeScope = 1;
        }
        fprintf(stderr, "rangeselftest: %s\n", bad ? "FAILED" : "ok");
        if (app.seqThread.joinable()) app.seqThread.join();
        return bad ? 1 : 0;
    }
