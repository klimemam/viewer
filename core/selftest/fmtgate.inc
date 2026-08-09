// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // ---- #111: a listing asks whichever question its HOST makes true ---------
    // What this test is FOR, in one line: the Browse panel must not answer the
    // PEER's question about files sitting on this machine's own disk.
    //
    // One predicate (isNpyName) used to answer both "can this viewer open it"
    // and "can this peer serve it", and the panel asked it for both. So every
    // format core/imagefile.h learned - PNG, JPEG, TIFF, OpenEXR, y4m - arrived
    // in Browse as a dead row: drawn dim, ignored by the click, ignored by the
    // double-click, with nothing on screen saying why. Five formats, one cause,
    // and a sixth would have joined them the day it landed.
    //
    // media/ is the whole matrix in one folder (tools/gen_testdata.py):
    //
    //   capture.npy    viewer reads it, peer serves it   -> opens either way
    //   *.png *.jpg *.tif *.y4m (*.exr)
    //                  viewer reads it, peer does NOT    -> opens LOCALLY
    //   capture.mp4    neither reads it                  -> refused, by name
    //
    // The assertions are about the OFFER, not about a row existing. Rows were
    // never dropped from the listing - that part of #111's wording was wrong,
    // and F2 pins it so nobody re-derives it from the issue text - what was
    // missing is that the row could not be opened and would not say why.
    if (!g_fmtgateSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "fmtgateselftest: %-64s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        const std::string root = g_fmtgateSelftest;
        browseLocalFolder(root);                    // exactly what the menu does
        double t0 = nowSec();
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (rbMain().b.connected && !rbMain().b.entries.empty()) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = rbMain().b;
        fprintf(stderr, "fmtgateselftest: browsing %s: connected=%d entries=%d%s%s\n",
                B.dir.c_str(), B.connected ? 1 : 0, (int)B.entries.size(),
                B.err.empty() ? "" : " err=", B.err.c_str());
        if (!B.connected || B.entries.empty()) {
            fprintf(stderr, "fmtgateselftest: FAILED (nothing listed)\n");
            stopRbWorker(); stopRemoteFetcher(); stopMeasureWorker(); stopSequenceLoader();
            return 1;
        }
        std::vector<RbRow> view = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
        auto listed = [&](const char* n) {
            for (const RbRow& r : view)
                if (!r.up && !r.ph && r.name() == n) return true;
            return false;
        };
        auto has = [](const std::string& hay, const char* needle) {
            return hay.find(needle) != std::string::npos;
        };
        // `peer` is whether viewer-serve can serve it - the one column that is
        // NOT a property of the format table, and the one the old predicate
        // applied to all of them.
        struct Want { const char* name; bool peer; };
        std::vector<Want> want = {
            { "capture.npy",  true  },
            { "gray8.png",    false },
            { "gradient.jpg", false },
            { "gray8.tif",    false },
            { "one.y4m",      false },
#ifdef VIEWER_WITH_EXR
            // Conditional for the reason media.inc's M1 is: OpenEXR is the row
            // that can legitimately be in either state, and a build without it
            // has no .exr fixture to browse at all.
            { "rgb_half.exr", false },
#endif
        };

        // ---- F1: listed AND openable, on this machine ------------------------
        // The open goes through the Browse panel's OWN door - the url its
        // double-click mints (makeRemoteUrl over an empty host) - and not
        // through openPath, because openPath was never the broken path. What
        // was broken is that this url went to a peer that reads .npy.
        for (const Want& w : want) {
            closeAll();
            app.toast.clear();
            const bool inList = listed(w.name);
            const bool opened = openRemote("local://" + B.dir + "/" + w.name, false, 0) &&
                                !app.images.empty();
            char what[160];
            snprintf(what, sizeof what, "F1 %s is listed and opens from Browse (local)",
                     w.name);
            fprintf(stderr, "fmtgateselftest:   %s: listed=%d opened=%d docs=%d toast=\"%s\"\n",
                    w.name, inList ? 1 : 0, opened ? 1 : 0, (int)app.images.size(),
                    app.toast.c_str());
            check(inList && opened, what);
        }
        closeAll();

        // ---- F2: what nothing here reads is SHOWN, and refused BY NAME -------
        // The row stays in the listing (it always did) and the refusal is the
        // measured one this repository already wrote for video containers -
        // not the peer's "not a .npy file", which blames the file for a limit
        // that belongs to the link. Before #111 the local door produced exactly
        // that peer sentence, which is what makes this assert red at base.
        {
            app.toast.clear();
            const bool inList = listed("capture.mp4");
            const bool opened = openRemote("local://" + B.dir + "/capture.mp4", false, 0);
            const std::string msg = app.toast;
            fprintf(stderr, "fmtgateselftest:   capture.mp4: listed=%d opened=%d "
                            "toast=\"%s\"\n",
                    inList ? 1 : 0, opened ? 1 : 0, msg.c_str());
            check(inList && !opened && has(msg, "capture.mp4") && has(msg, "codec"),
                  "F2 a file nothing here reads stays listed and says why (by name)");
        }
        closeAll();

        // ---- F3: the RULING - a remote listing shows what it cannot serve ----
        // 「見せて理由を言う」: the row is NOT dropped when the peer cannot
        // serve it, it is drawn dim and it carries the reason. This drives the
        // panel's own gate (rbRowOpenable / rbRowWhyNot, browse.h) rather than
        // a second copy of the rule, so a panel that stopped consulting it
        // would fail here.
        //
        // The view is host-independent by construction - rbBuildView never took
        // a host - so "shown" is the same row list either way, and that is the
        // half of the ruling this can assert directly.
        {
            bool shown = true, local = true, remote = true, said = true;
            std::string firstWhy;
            for (const Want& w : want) {
                if (!listed(w.name)) shown = false;
                if (!rbRowOpenable("", w.name)) local = false;
                if (rbRowOpenable("trc2", w.name) != w.peer) remote = false;
                const std::string why = rbRowWhyNot("trc2", w.name);
                if (w.peer) {
                    if (!why.empty()) said = false;      // it CAN be served: no reason
                } else {
                    // named, reasoned, way out attached - §3.2's three parts
                    if (why.empty() || !has(why, ".npy")) said = false;
                    if (firstWhy.empty()) firstWhy = why;
                }
            }
            fprintf(stderr, "fmtgateselftest:   remote gate: shown=%d localOpenable=%d "
                            "remoteMatchesPeer=%d\n"
                            "fmtgateselftest:   remote reason: \"%s\"\n",
                    shown ? 1 : 0, local ? 1 : 0, remote ? 1 : 0, firstWhy.c_str());
            check(shown && local && remote && said,
                  "F3 remote: the row is shown, dimmed, and names the peer's limit");
            // ...and the local half of the same pair, on the file no reader here
            // takes: dim locally too, with the video refusal rather than a
            // sentence about a peer that is not involved.
            const std::string lw = rbRowWhyNot("", "capture.mp4");
            fprintf(stderr, "fmtgateselftest:   local reason for capture.mp4: \"%s\"\n",
                    lw.c_str());
            check(!rbRowOpenable("", "capture.mp4") && has(lw, "codec"),
                  "F3 local: an unreadable row names ITS reason, not the peer's");
        }

        // ---- F4: the two predicates cannot silently re-merge ------------------
        // The property the fix is FOR, stated as an invariant rather than as a
        // list of five extensions: everything the peer serves, this viewer also
        // reads (so a local listing is never narrower than a remote one), and
        // the format table is what decides the local half - not a literal here.
        {
            bool superset = true;
            for (const imagefile::Backend& b : imagefile::backends()) {
                std::string one = b.exts;                  // ".tif .tiff" -> first
                size_t sp = one.find(' ');
                if (sp != std::string::npos) one = one.substr(0, sp);
                if (!viewerReadsName("x" + one)) superset = false;
            }
            const bool npy = viewerReadsName("a.npy") && peerServesName("a.npy");
            const bool upper = viewerReadsName("A.NPY") && peerServesName("FRAME_001.NPY");
            fprintf(stderr, "fmtgateselftest:   table = [%s], npy both=%d case=%d\n",
                    imagefile::decodableFormats().c_str(), npy ? 1 : 0, upper ? 1 : 0);
            check(superset && npy && upper,
                  "F4 every table row reads locally; .npy answers both; case-insensitive");
        }

        fprintf(stderr, "fmtgateselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
