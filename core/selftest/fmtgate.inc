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
    //   *.png *.jpg *.tif *.exr *.y4m
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
        // A file is "listed" when the listing SHOWS it - as its own row, or as a
        // member of a stack-group row. The second case arrived with #148 B and
        // is not a special case for pictures: the peer folds a numbered family
        // into one row, and media/ happens to hold three of them (gray4 /
        // gray8 / gray16, as .png and again as .tif). That fold is the SAME
        // rule that has always folded frame_001..003.npy, applied to a format
        // the peer can now read - so the row is there and the file is in it,
        // which is what this predicate has to be about.
        auto listed = [&](const char* n) {
            for (const RbRow& r : view)
                if (!r.up && !r.ph && r.name() == n) return true;
            for (const remote::Entry& e : B.entries)
                for (const std::string& m : e.members)
                    if (m == n) return true;
            return false;
        };
        auto has = [](const std::string& hay, const char* needle) {
            return hay.find(needle) != std::string::npos;
        };
        // `peer` is whether viewer-serve can serve it - the one column that is
        // NOT a property of the format table, and the one the old predicate
        // applied to all of them.
        //
        // Every picture row said `false` until issue #148's judgment B. What
        // changed is not this table's meaning but the peer's BUILD: viewer-serve
        // links core/imagefile.h now, so the column that used to separate .npy
        // from the picture formats separates the picture formats from the one
        // row whose limit is a licence (vendor RAW / LibRaw's CDDL - the .dng
        // row below, and P6 further down for the sentence it produces).
        struct Want { const char* name; bool peer; };
        std::vector<Want> want = {
            { "capture.npy",  true },
            { "gray8.png",    true },
            { "gradient.jpg", true },
            { "gray8.tif",    true },
            { "one.y4m",      true },
            // Unconditional since #53's ruling: there is no build of this
            // viewer without OpenEXR, so there is none without an .exr fixture
            // in media/ either, and this row is as ordinary as the four above.
            { "rgb_half.exr", true },
            // ...and the row that is NOT about a reader. This file opens here
            // and is refused over the link, and the refusal has to NAME the
            // reason, because "we cannot read it" would be false.
            { "sensor_rggb.dng", false },
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
                    // The reason is NAMED now rather than "the peer serves
                    // .npy only": what the peer will not carry is a LIBRARY,
                    // and a sentence about .npy would be a true-sounding lie
                    // about a peer that serves five picture formats.
                    if (why.empty() || !has(why, "LibRaw") ||
                        !has(why, "read on this machine")) said = false;
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

        // ---- F3b: the mirror of the line above, which nobody was watching ----
        // F3 asserts that a LOCAL row keeps its own measured reason instead of
        // the peer's. The same row over the LINK had lost it: peerRefusal fell
        // through to "the peer serves .npy, PNG, ..." - a list the operator can
        // already see this file is not in - and with it went the measurement
        // (8-bit lossy destroys sigma_t) and the ffmpeg line that converts the
        // file honestly. Same file, same defect, facing the other way
        // (verify-matrix G7).
        //
        // And the fall-through itself: whatever lands there is refused with the
        // first two parts of docs/features/adapters/input-adapters.md §3.2 and, until G9, without
        // the third. A refusal with no way out is where an operator stops.
        {
            const std::string rw = rbRowWhyNot("trc2", "capture.mp4");
            // A name with NO row, no decoder, no measured refusal and - since
            // G1 stage 1 - no headerless sentence either: the true
            // fall-through. It used to be capture.raw, which now has a branch
            // of its own (F4d), and a test whose subject has been given a
            // better answer has to move to what is still unanswered rather
            // than keep asserting the old sentence about the new one.
            const std::string gen = rbRowWhyNot("trc2", "capture.unknownfmt");
            fprintf(stderr, "fmtgateselftest:   peer reason for capture.mp4: \"%s\"\n"
                            "fmtgateselftest:   peer reason for capture.raw: \"%s\"\n",
                    rw.c_str(), gen.c_str());
            check(!rbRowOpenable("trc2", "capture.mp4") && has(rw, "codec") &&
                  has(rw, "ffmpeg"),
                  "F3b over the link too, .mp4 keeps the measured reason and the way out");
            check(!gen.empty() && has(gen, "the peer serves") && has(gen, "copy the file"),
                  "F3b the fall-through refusal names a way out as well");
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
            // ...and the LINK half of the same invariant, which is what #148 B
            // added: the client's gate and the peer's limit are now ONE
            // predicate over ONE table, so a row added tomorrow is checked by
            // this loop without anybody editing it. It is stated as an equality
            // with the table's own column, not as a list of extensions - a list
            // here would be the second place that knows, which is the defect.
            bool linkGate = true;
            for (const imagefile::Backend& b : imagefile::backends()) {
                std::string one = b.exts;
                size_t sp = one.find(' ');
                if (sp != std::string::npos) one = one.substr(0, sp);
                if (peerServesName("x" + one) != b.overLink) linkGate = false;
                if (b.overLink && !viewerReadsName("x" + one)) linkGate = false;
                std::string up = one;                 // the peer lower-cases too
                for (char& c : up) c = (char)toupper((unsigned char)c);
                if (peerServesName("X" + up) != b.overLink) linkGate = false;
            }
            fprintf(stderr, "fmtgateselftest:   table = [%s], npy both=%d case=%d "
                            "linkGate=%d\n",
                    imagefile::decodableFormats().c_str(), npy ? 1 : 0, upper ? 1 : 0,
                    linkGate ? 1 : 0);
            check(superset && npy && upper && linkGate,
                  "F4 every table row reads locally; .npy answers both; case-insensitive");
        }

        // ---- F4b: the doors OUTSIDE the table are inside an invariant too ----
        // F4 above walks every row of the format table. That is exactly what it
        // was written to do, and it is also why it never saw this: the formats
        // that are core/main.cpp's own doors have no row. `.bin` and friends
        // reach the headerless dialog and `.npy`/`.npz` reach the npy loader,
        // so Open Folder accepts all seven - but `viewerReadsName` listed only
        // two of them, and a Browse row on THIS MACHINE was greyed out for a
        // file File > Open opens without complaint, with a refusal naming a
        // reason that was not the reason (verify-matrix G2).
        //
        // Stated as the equality between the two predicates rather than as a
        // list of seven: an extension added to sequence.inc's arrays tomorrow
        // is checked here without anybody editing this test, which is the
        // property F4 has for the table and this half was missing.
        //
        // Since G8 the two predicates share their body, so this equality now
        // holds by construction - which is the END STATE it was written to
        // reach, not a reason to retire it. It fails again the day somebody
        // re-types the list here.
        //
        // ...and it is now stated over the ARRAYS, the way F4c below already
        // states its own claim. It used to walk a hand-typed probe of ten
        // strings, which reproduced inside this test the very thing the
        // G1-G3-G8 family is about: the invariant enumerated the complement of
        // the table BY HAND, so an extension added to sequence.inc's array or
        // to imagefile's tomorrow is a door no invariant sees, because the hand
        // does not know. There is no list here now.
        {
            int checked = 0;
            bool agree = true;
            std::string disagreed;
            auto want = [&](const std::string& e) {
                checked++;
                if (isLoadableExt(e) && !viewerReadsName(std::string("x") + e)) {
                    agree = false;
                    disagreed += std::string(disagreed.empty() ? "" : " ") + e;
                }
            };
            for (const char* e : SELFDESC_EXTS)          want(e);
            for (const std::string& e : HEADERLESS_EXTS) want(e);
            // ...and the table's own rows through the same predicate pair, so
            // the two halves of "can this build open it" are asserted together
            // rather than in two tests that could come to disagree.
            for (const imagefile::Backend& b : imagefile::backends()) {
                std::string all = b.exts;
                size_t i = 0;
                while (i < all.size()) {
                    size_t sp = all.find(' ', i);
                    want(all.substr(i, sp == std::string::npos ? sp : sp - i));
                    if (sp == std::string::npos) break;
                    i = sp + 1;
                }
            }
            int accepted = 0;
            for (const char* e : SELFDESC_EXTS)          if (isLoadableExt(e)) accepted++;
            for (const std::string& e : HEADERLESS_EXTS) if (isLoadableExt(e)) accepted++;
            fprintf(stderr, "fmtgateselftest:   Open Folder accepts %d own-door of %d "
                            "extensions walked; Browse disagrees on [%s]\n",
                    accepted, checked, disagreed.empty() ? "-" : disagreed.c_str());
            // anti-vacuity: the arrays have to be non-empty and the own doors
            // have to be accepted, or "they agree" is a statement about nothing
            check(accepted >= 7 && checked >= 12,
                  "F4b the arrays really cover the loadable set");
            check(agree, "F4b what Open Folder loads, Browse says it can open");
        }

        // ---- F4b2: the ONE door that is in no array and no table row ----------
        // `.vsession` is a door - a session is opened, not decoded - and it is
        // the last name belonging to none of the three enumerations F4, F4b and
        // F4c walk. Every invariant above is therefore true of it VACUOUSLY,
        // and its contract has never been asserted anywhere. It is four
        // answers, and three of them are NO:
        //
        //   viewerReadsName  yes - Browse must not grey out a file File > Open
        //                          opens (the G2 shape)
        //   isLoadableName   NO  - Open Folder must not bundle sessions into a
        //                          stack, which is why menus.inc adds it at the
        //                          one door and nowhere else
        //   peerServesName   NO  - the peer serves pixels; a session names
        //                          files on the machine that wrote it
        //   isHeaderless     NO  - and it is not a geometry prompt either
        //
        // Asserted as the four TOGETHER because the value is in the
        // disagreement: any one alone is satisfied by a predicate that says yes
        // to everything or no to everything.
        {
            const std::string v = "restore.vsession";
            const bool reads  = viewerReadsName(v);
            const bool bundle = isLoadableName(v);
            const bool served = peerServesName(v);
            const bool hless  = imagefile::isHeaderless(v);
            fprintf(stderr, "fmtgateselftest:   .vsession: browse-opens=%d "
                            "folder-bundles=%d peer-serves=%d headerless=%d\n",
                    reads ? 1 : 0, bundle ? 1 : 0, served ? 1 : 0, hless ? 1 : 0);
            check(reads && !bundle && !served && !hless,
                  "F4b2 .vsession opens from Browse and is refused by the other three doors");
            // ...and the same four asked of a name that IS in the table, which
            // is the anti-vacuity: it must answer differently, or the four
            // predicates are not distinguishing anything.
            const std::string n = "frame.npy";
            check(viewerReadsName(n) && isLoadableName(n) && peerServesName(n),
                  "F4b2b ...while a .npy answers yes to the first three - the "
                  "four predicates are not one predicate");
        }

        // ---- F4c: ...and the FILE DIALOG offers every one of them -------------
        // The third door onto the same set, and the one that had drifted: File >
        // Open's "Images" filter was a typed string, and `.rggb` was in the other
        // three lists and not in that one. Nothing failed loudly - the file was
        // simply not there, and the way out was to know that "All files" exists
        // (verify-matrix G8).
        //
        // Stated over the arrays and the table rather than over a probe, so
        // there is no list in this test at all: an extension added to either
        // side tomorrow is checked here by construction. The dot-token has to
        // match WHOLE (" *.raw " never satisfies ".rggb"), because a substring
        // test is how a missing extension hides behind a longer one.
        {
            const std::string pat = " " + openImagesFilter() + " ";
            auto offered = [&](const std::string& e) {
                return pat.find(" *" + e + " ") != std::string::npos;
            };
            std::string missing;
            auto want = [&](const std::string& e) {
                if (!offered(e)) missing += std::string(missing.empty() ? "" : " ") + e;
            };
            int checked = 0;
            for (const char* e : SELFDESC_EXTS)   { want(e); checked++; }
            for (const std::string& e : HEADERLESS_EXTS) { want(e); checked++; }
            for (const imagefile::Backend& b : imagefile::backends()) {
                std::string all = b.exts;             // ".tif .tiff" - every one
                size_t i = 0;
                while (i < all.size()) {
                    size_t sp = all.find(' ', i);
                    want(all.substr(i, sp == std::string::npos ? sp : sp - i));
                    checked++;
                    if (sp == std::string::npos) break;
                    i = sp + 1;
                }
            }
            // ...and the refusal sentence names that same set plus the session
            // door, because it is what an operator reads before going to look.
            const std::string why = viewerRefusalFor("notes.txt");
            const bool refusalAgrees = has(why, openImagesFilter().c_str()) &&
                                       has(why, "*.vsession");
            fprintf(stderr, "fmtgateselftest:   dialog filter covers %d extensions, "
                            "missing [%s]; refusal names the same set=%d\n",
                    checked, missing.empty() ? "-" : missing.c_str(), refusalAgrees ? 1 : 0);
            // anti-vacuity: an empty pattern would satisfy an empty want-list
            check(checked >= 12 && pat.size() > 20,
                  "F4c there really is a set of extensions and a filter to check it against");
            check(missing.empty(), "F4c every extension a door accepts is offered by File > Open");
            check(refusalAgrees, "F4c the refusal names the set the file dialog offers");
        }

        // ---- F4d: the headerless refusal is a DECISION, not a fall-through ---
        // verify-matrix G1, the one gap left and the one the user reported:
        // ".raw over the link does not open". Nobody ever decided that. The
        // format table correctly has no row for a headerless file (a row means
        // "the bytes decide", and for these they do not), so peerServes said
        // false, and the refusal fell to the generic "the peer serves .npy,
        // PNG, ..." - a list the operator can already see this file is not in.
        //
        // The door IS open (protocol 11, docs/features/remote/remote-headerless-design.md
        // stage 2, shipped the same day stage 1 did) - so what this checks is
        // no longer "the wire cannot carry a declaration", which is what the
        // sentence said for six weeks after it stopped being true. It is the
        // replacement: a headerless file states its shape in a RECIPE, this
        // request carried none, and here is how to state one. The assertions
        // below pin the ABSENCE of the old wording, because pinning only the
        // words that stayed is how the stale half survived (PR #278).
        //
        // Walked over headerlessExts() rather than a list here, for F4c's
        // reason: an extension added to that array tomorrow is checked without
        // anybody editing this test.
        {
            bool served = false, said = true, generic = false, stale = false;
            std::string worst;
            for (const std::string& e : imagefile::headerlessExts()) {
                const std::string n = "capture" + e;
                if (peerServesName(n)) served = true;          // stage 2 flips this
                const std::string why = peerRefusalFor(n);
                if (!has(why, "headerless") || !has(why, "File > Open") ||
                    !has(why, e.c_str()) || !has(why, "no recipe") ||
                    !has(why, "--raw-size")) { said = false; if (worst.empty()) worst = why; }
                // ...and it must not be the fall-through wearing a new coat
                if (why.rfind("the peer serves", 0) == 0) generic = true;
                // ...NOR stage 1's sentence, which is the one this test let
                // through for six weeks. "this link cannot yet carry that
                // declaration" was true until protocol 11 shipped on the SAME
                // DAY, and a user pasted it back on 2026-09-29. The wording
                // was asserted here and the FACT behind it was not, so this is
                // the fact: nothing in this refusal may say the wire is the
                // thing that cannot carry a recipe.
                if (has(why, "cannot yet carry") || has(why, "recipe-carrying build") ||
                    has(why, "update both ends")) stale = true;
            }
            const size_t n = imagefile::headerlessExts().size();
            fprintf(stderr, "fmtgateselftest:   headerless: %zu ext(s), peerServes=%d "
                            "named=%d generic=%d\n"
                            "fmtgateselftest:   headerless reason: \"%s\"\n",
                    n, served ? 1 : 0, said ? 1 : 0, generic ? 1 : 0,
                    worst.empty() ? peerRefusalFor("capture.raw").c_str() : worst.c_str());
            // anti-vacuity: there is a set to walk, and it is the whole one
            // ...and the ROW is alive on a peer since protocol 11. A row that
            // dimmed for a headerless file would be saying "this cannot be
            // opened" about a file the very next double-click opens - the
            // shape of the defect #111 and #148 each removed once.
            //
            // The one-click preview gate is deliberately NOT widened with it:
            // a preview is a decimated tile and a tile needs a rect, which
            // needs the geometry, which does not exist yet. Both halves are
            // asserted together so neither can be changed alone.
            bool rowsLive = true, previewsStillOff = false;
            for (const std::string& e : imagefile::headerlessExts()) {
                const std::string nm = "capture" + e;
                if (!rbRowOpenable("trc2", nm)) rowsLive = false;
                if (peerServesName(nm)) previewsStillOff = true;   // must stay false
            }
            fprintf(stderr, "fmtgateselftest:   headerless rows on a peer: live=%d, "
                            "one-click preview gate widened=%d (must be 0)\n",
                    rowsLive ? 1 : 0, previewsStillOff ? 1 : 0);
            check(rowsLive, "F4d a headerless row is OPENABLE on a peer (protocol 11)");
            check(!previewsStillOff,
                  "F4d ...and the one-click preview gate was not widened with it");
            check(n >= 5, "F4d there really is a headerless set to check");
            check(!served, "F4d the peer still does not serve a headerless file");
            check(said, "F4d ...and says so by NAME, with the door that does work");
            check(!generic, "F4d ...instead of the generic list it used to fall to");
            check(!stale, "F4d ...and never again claims this build cannot carry a recipe");
        }

        // ---- F4g: a CONTAINER on a peer (issue #217, R18) --------------------
        // The user's report was ".npz is still greyed out on a peer", and the
        // sentence behind the grey said the peer serves "one array per file,
        // not a container". That was true, and it stopped being true the day
        // MSG_NPZ_SCAN shipped - a refusal that denies a door which exists is
        // verify-matrix G11 all over again, which is why the wording is
        // asserted here rather than left to whoever reads it next.
        //
        // The two halves go together exactly as F4d's do, and for the same
        // reason: the ROW is openable (peerServesDeclared) and the ONE-CLICK
        // PREVIEW is not (peerServes). A preview is a decimated tile of one
        // geometry; a container has members instead of a geometry, so widening
        // the preview gate would make a row promise a picture that cannot be
        // made. Neither half can be changed without the other failing.
        {
            const std::string nm = "capture.npz";
            const bool rowLive = rbRowOpenable("trc2", nm);
            const bool previewGate = peerServesName(nm);      // must stay false
            const bool declared = imagefile::peerServesDeclared(nm);
            const std::string why = peerRefusalFor(nm);
            const bool saysMembers = has(why, "member list") && has(why, "container");
            const bool saysOld = has(why, "one array per file");
            const bool localIntact = has(viewerRefusalFor(nm), "reader") ||
                                     viewerReadsName(nm);
            fprintf(stderr, "fmtgateselftest:   F4g .npz on a peer: row=%d preview=%d "
                            "declared=%d\n"
                            "fmtgateselftest:   F4g reason: \"%s\"\n",
                    rowLive ? 1 : 0, previewGate ? 1 : 0, declared ? 1 : 0, why.c_str());
            check(rowLive && declared,
                  "F4g a .npz row is OPENABLE on a peer (protocol 13)");
            check(!previewGate,
                  "F4g ...and the one-click preview gate was not widened with it");
            check(saysMembers,
                  "F4g the refusal names the member list, which is the door that exists");
            check(!saysOld,
                  "F4g ...and no longer claims the peer serves one array per file");
            check(localIntact, "F4g the LOCAL door for a .npz is untouched");
        }

        // ---- F4d2: the fall-through says what was DECIDED about readers ------
        // verify-matrix G11 / issue #180 ruling B, stage 0 of
        // docs/features/remote/remote-reader-design.md. F4d moved headerless files OFF the
        // fall-through; this is about what still lands on it - a name with no
        // row, no decoder here, and no measured refusal of its own.
        //
        // For such a file the served list is true and useless: the operator can
        // already see this name is not in it, and carrying the file over does
        // not make this build read it natively either. The door that would read
        // it is a READER (docs/features/adapters/input-adapters.md §4.13) - and §4.13.1 decided,
        // on 2026-08-03, that readers run where the file lives. That decision
        // has no implementation. Until this the refusal did not mention it, so
        // "decided and not built" was indistinguishable from "never considered":
        // neither a judged refusal nor an open door, which is all of G11.
        //
        // The sentence belongs to the LINK's refusal only. The local one offers
        // a reader for real ("choose a reader") and must not start saying a door
        // is missing - the missing door is the peer's.
        //
        // ...AND THE DAY CAME. Stage 1-2 of that design landed (protocol 12,
        // MSG_READER_RUN, --serve-readers), so this check is now the other way
        // round: the refusal must NOT say the door is missing, and it must make
        // the local door's own offer - `choose a reader` - because a reader
        // sent to the peer is a thing that happens now. The replacement is
        // noticed here exactly as the note above promised it would be.
        //
        // What did NOT change is the pair of facts on either side of it: what
        // the peer does serve is still named (an addition, never a swap), and
        // the local refusal is still the local one.
        {
            const std::string why  = peerRefusalFor("capture.unknownfmt");
            const std::string here = viewerRefusalFor("capture.unknownfmt");
            const bool decided  = has(why, "reader") && has(why, "4.13.1");
            // The door is BUILT: the sentence offers it, names the flag whose
            // absence is the one remaining refusal, and no longer says
            // "not built yet" anywhere.
            const bool built    = has(why, "choose a reader") &&
                                  has(why, "--serve-readers") &&
                                  !has(why, "not built yet");
            const bool wayOut   = has(why, "copy the file here");
            // ...and the Browse row is the surface an operator reads it on, so
            // it has to be the same sentence and not a second one (F3/F3b's
            // rule: one refusal, quoted wherever it is shown).
            const bool panelSame = rbRowWhyNot("trc2", "capture.unknownfmt") == why;
            const bool localIntact = has(here, "choose a reader") &&
                                     !has(here, "not built yet");
            fprintf(stderr, "fmtgateselftest:   reader ruling in the refusal: named=%d "
                            "built=%d wayOut=%d panelSame=%d localIntact=%d\n"
                            "fmtgateselftest:   fall-through reason: \"%s\"\n",
                    decided ? 1 : 0, built ? 1 : 0, wayOut ? 1 : 0,
                    panelSame ? 1 : 0, localIntact ? 1 : 0, why.c_str());
            // anti-vacuity: the sentence is an ADDITION - what the peer does
            // serve is still named (F3b reads that half)
            check(!why.empty() && has(why, "the peer serves"),
                  "F4d2 the fall-through still names what the peer does serve");
            check(decided, "F4d2 ...and names the ruling that readers run on the peer");
            check(built,   "F4d2 ...and OFFERS that door now that it is built (#180 stage 2)");
            check(wayOut,  "F4d2 ...with the other way out kept: copy it here");
            check(panelSame, "F4d2 ...and the Browse row shows that same sentence");
            check(localIntact, "F4d2 the LOCAL refusal is untouched: it offers a reader");
        }

        // ---- F4e: the DOOR - a headerless file opened over the link ----------
        // P8 proved the wire; this proves the door every caller uses. It is the
        // call the CLI branch makes (`viewer local://.../x.raw --raw-dtype u16
        // --raw-size 8x6`) and the one stage 4's Browse dialog will make, so
        // what is asserted here is what a user gets, not what a protocol can do.
        //
        // Three things have to be true at once, and the second and third are
        // the ones a working-pixels test would miss:
        //   the PIXELS are the bytes that were written;
        //   the SOURCE carries the recipe - srcIdentityKey's raw branch keys on
        //     exactly those fields, so two recipes over one url must be two
        //     residents (docs/reference-design.md §6.2);
        //   the NOTE declares the whole read - M1, and it has to be the SAME
        //     sentence the local door writes or one file describes itself two
        //     ways depending on which end opened it.
        {
            closeAll();
            std::error_code dec;
            std::filesystem::path ddir =
                std::filesystem::temp_directory_path(dec) / "viewer_rawdoor";
            std::filesystem::remove_all(ddir, dec);
            std::filesystem::create_directories(ddir, dec);
            const int DW = 8, DH = 6, DHDR = 12;
            std::vector<uint16_t> px((size_t)DW * DH);
            for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(i * 7 + 3);
            const std::string dpath = (ddir / "probe.raw").u8string();
            {
                std::ofstream df(pathFromUtf8(dpath), std::ios::binary);
                const std::vector<uint8_t> junk(DHDR, 0xA5);
                df.write((const char*)junk.data(), DHDR);
                df.write((const char*)px.data(), (std::streamsize)(px.size() * 2));
            }
            RawDialog rd;
            rd.dtype = RD_U16; rd.interp = RI_GRAY;
            rd.w = DW; rd.h = DH; rd.offset = DHDR; rd.littleEndian = true;
            const rp::RawWire rw = rawWireFrom(rd);
            std::string url = "local://" + dpath;
            std::replace(url.begin(), url.end(), '\\', '/');
            const bool opened = openRemote(url, false, 0, 0, &rw);
            const ImageDoc* d = app.images.empty() ? nullptr : app.images.front().get();
            bool same = d && d->w == DW && d->h == DH && d->ch == 1 &&
                        d->px().size() == px.size();
            if (same)
                for (size_t i = 0; i < px.size(); i++)
                    if (d->px()[i] != (float)px[i]) same = false;
            const bool keyed = d && d->src->rawDtype == RD_U16 &&
                               d->src->rawInterp == RI_GRAY &&
                               d->src->rawOffset == DHDR && d->src->rawLE &&
                               d->src->srcW == DW && d->src->srcH == DH;
            const std::string note = d ? d->note : std::string();
            const bool declared = has(note, "8x6") && has(note, "header 12 B") &&
                                  has(note, "little-endian") && has(note, "declared");
            fprintf(stderr, "fmtgateselftest:   F4e remote raw door: opened=%d %dx%d ch%d "
                            "pixels equal=%d keyed=%d\n"
                            "fmtgateselftest:   F4e note: \"%s\"\n",
                    opened ? 1 : 0, d ? d->w : 0, d ? d->h : 0, d ? d->ch : 0,
                    same ? 1 : 0, keyed ? 1 : 0, note.c_str());
            check(opened && same,
                  "F4e a headerless file opens over the link, sample for sample");
            check(keyed, "F4e ...and the source carries the recipe (identity keys on it)");
            check(declared, "F4e ...and the note declares the whole read (M1)");
            // ...and the same url with NO recipe does not quietly open
            // anything. Over local:// it goes where #111 sends every file this
            // disk can read - the local door - which for a headerless name is
            // the dialog that ASKS for the geometry. That is the way out, and
            // asserting it keeps the two halves of one rule together: a recipe
            // crosses the link, and the absence of one is a question rather
            // than a guess. (Over ssh:// there is no local door and the refusal
            // is F4d's sentence, asserted there.)
            closeAll();
            app.toast.clear();
            rawDlg.open = false;
            const bool bare = openRemote(url, false, 0, 0, nullptr);
            fprintf(stderr, "fmtgateselftest:   F4e without a recipe: opened=%d "
                            "docs=%zu dialog=%d\n",
                    bare ? 1 : 0, app.images.size(), rawDlg.open ? 1 : 0);
            check(!bare && app.images.empty() && rawDlg.open,
                  "F4e ...and with no recipe it asks for the geometry, never guesses");
            rawDlg.open = false;

            // ---- F4f: the BROWSE gesture, end to end ---------------------
            // What a user actually does. openRemoteRawRow is the door the
            // panel's double-click reaches, and it takes only the url and the
            // byte count - which is all a listing has.
            //
            // Two paths through it, and the difference is #166's rule applied
            // to a peer: a session that has ALREADY been told how to read a
            // file of exactly this many bytes applies that and says so; one
            // that has not ASKS. A headerless file is the one shape that does
            // not defend itself (4096x3000 and 3000x4096 are one byte count),
            // so nothing may apply that the operator did not pick.
            {
                const uint64_t sz = (uint64_t)DHDR + px.size() * 2;
                closeAll();
                rawDlg.open = false;
                app.toast.clear();
                // (a) nothing bound yet: the gesture raises the dialog, on the
                //     URL, with the byte count the listing carried
                openRemoteRawRow(url, sz);
                const bool asked = rawDlg.open && rawDlg.path == url &&
                                   rawDlg.fileSize == (size_t)sz;
                fprintf(stderr, "fmtgateselftest:   F4f unbound: dialog=%d path=%s "
                                "size=%zu (want %llu)\n",
                        rawDlg.open ? 1 : 0, rawDlg.path.c_str(), rawDlg.fileSize,
                        (unsigned long long)sz);
                check(asked, "F4f a headerless row asks for the geometry, on the url");
                // (b) accept it: the dialog's Load lands on the PEER, and the
                //     size binding is made by a read that worked
                rawDlg.dtype = RD_U16; rawDlg.interp = RI_GRAY;
                rawDlg.w = DW; rawDlg.h = DH; rawDlg.offset = DHDR;
                rawDlg.littleEndian = true;
                const std::string aerr = rawDialogAccept(rawDlg);
                rawDlg.open = false;
                const ImageDoc* ad = app.images.empty() ? nullptr : app.images.front().get();
                bool landed = aerr.empty() && ad && ad->w == DW && ad->h == DH &&
                              !ad->src->remoteUrl.empty();
                fprintf(stderr, "fmtgateselftest:   F4f accept: err=\"%s\" %dx%d url=%s "
                                "bound=%d\n", aerr.c_str(), ad ? ad->w : 0, ad ? ad->h : 0,
                        ad ? ad->src->remoteUrl.c_str() : "-",
                        rawRecipeForSize((size_t)sz) ? 1 : 0);
                check(landed, "F4f ...and Load lands on the PEER, not on the local disk");
                check(rawRecipeForSize((size_t)sz) != nullptr,
                      "F4f ...and a read that worked binds the size for this session");
                // (c) the same gesture again: the binding applies, no dialog
                closeAll();
                rawDlg.open = false;
                app.toast.clear();
                openRemoteRawRow(url, sz);
                const ImageDoc* bd = app.images.empty() ? nullptr : app.images.front().get();
                fprintf(stderr, "fmtgateselftest:   F4f bound: dialog=%d docs=%zu "
                                "toast=\"%s\"\n",
                        rawDlg.open ? 1 : 0, app.images.size(), app.toast.c_str());
                check(!rawDlg.open && bd && bd->w == DW && bd->h == DH,
                      "F4f the second time, this session's binding opens it without asking");
                check(has(app.toast, "recipe") || has(app.toast, "bytes"),
                      "F4f ...and says which recipe it applied, never silently");
                g_rawSizeRecipe.clear();
                closeAll();
            }

            // ---- F4h: THE OTHER DOORS, and the sentence when there is none ---
            // The user's report (2026-09-29) was a toast, pasted verbatim:
            //
            //   [error] remote: a headerless .raw states its shape in a recipe
            //   on this machine, and this link cannot yet carry that declaration
            //
            // That sentence is stage 1's, and it stopped being true when
            // protocol 11 shipped - the wire HAS carried a recipe since #186.
            // What was actually wrong was narrower and worse: only ONE door
            // asked for a recipe (openRemoteRawRow, reached from a Browse
            // row's double-click). Every other way to name a headerless file
            // on a peer - a url on the command line, a drop, File > Open, a
            // folder opened as a stack - called openRemote with a null recipe
            // and got a refusal that blamed the build.
            //
            // So this block asserts the two halves together, because either
            // one alone is a lie: the doors ASK, and the refusal that is left
            // says what is actually missing.
            {
                // ---- (1) the SENTENCE the client is left with ---------------
                // Reproduced where the user met it: Session::meta with no
                // recipe, which is the call openRemote makes and the source of
                // the "remote: " prefix in the report.
                closeAll();
                rawDlg.open = false;
                g_rawSizeRecipe.clear();
                // The session this drives, said out loud: F4f's accept left one
                // alive on `dpath`'s host, and a test that silently inherits
                // one it did not ask for proves nothing the day the order of
                // these blocks changes (review: "F4h(1) depends on
                // app.uiSession implicitly").
                std::string uerr0;
                const bool haveUi = ensureUiSession("", uerr0, 0) &&
                                    app.uiSession.peerVersion() >= 11;
                fprintf(stderr, "fmtgateselftest:   F4h session: alive=%d protocol=%d %s\n",
                        haveUi ? 1 : 0, app.uiSession.peerVersion(), uerr0.c_str());
                check(haveUi, "F4h (setup) a local:// peer that can be told a recipe");
                remote::Meta mm{};
                std::string merr;
                const bool mok = app.uiSession.meta(dpath, mm, merr, NR_NATIVE, nullptr);
                fprintf(stderr, "fmtgateselftest:   F4h client refusal: ok=%d \"%s\"\n",
                        mok ? 1 : 0, merr.c_str());
                check(!mok, "F4h a headerless request with no recipe is still refused");
                check(!has(merr, "cannot yet carry") && !has(merr, "recipe-carrying build"),
                      "F4h ...and the refusal no longer claims this build cannot carry one");
                check(has(merr, "no recipe") && has(merr, "--raw-size"),
                      "F4h ...it names what is missing and how to declare it");

                // ---- (2) the SIZE, from the listing that already carries it --
                // §5.2: no new wire. MSG_LIST has returned a byte count since
                // protocol 3, and handleList answers for a non-directory path
                // with that one entry. Derived: 12 B of junk header + 8*6
                // samples * 2 B/sample = 108 bytes.
                const uint64_t WANT = (uint64_t)DHDR + (uint64_t)DW * DH * 2;
                std::vector<remote::Entry> le;
                std::string lerr;
                const bool lok = app.uiSession.list(dpath, le, lerr);
                fprintf(stderr, "fmtgateselftest:   F4h LIST of one file: ok=%d n=%zu "
                                "size=%llu (want %llu) %s\n",
                        lok ? 1 : 0, le.size(),
                        (unsigned long long)(le.empty() ? 0 : le[0].size),
                        (unsigned long long)WANT, lerr.c_str());
                check(lok && le.size() == 1 && le[0].size == WANT,
                      "F4h the peer's own listing carries the byte count a dialog needs");

                // ---- (2b) THE PEER's half of the same refusal ---------------
                // Two sentences, one lead-in, chosen by WHO noticed
                // (imagefile::RefusedBy). The peer's names THE VIEWER THE
                // READER IS RUNNING, which is the only thing anybody can act on
                // when a request arrives without a recipe - and it must not say
                // "the other end", because this sentence travels back over the
                // link and the other end, from where it is read, is the peer
                // (the one machine that is not the problem). It must also NOT
                // offer this build's doors, which a peer has none of. Asserted
                // together so neither can be edited into a copy of the other.
                {
                    const std::string cl = imagefile::peerRefusal("capture.raw");
                    const std::string pe = imagefile::peerRefusal(
                        "capture.raw", imagefile::RefusedBy::Peer);
                    fprintf(stderr, "fmtgateselftest:   F4h peer-side refusal: \"%s\"\n",
                            pe.c_str());
                    check(has(pe, "carries no header") && has(pe, "no recipe") &&
                          has(pe, "protocol-11") && has(pe, "you are running") &&
                          !has(pe, "other end"),
                          "F4h the PEER's refusal names the viewer the READER is running");
                    check(!has(pe, "cannot yet carry") && !has(pe, "--raw-size") &&
                          !has(pe, "File > Open"),
                          "F4h ...and offers no door of a build it is not running");
                    check(cl != pe && has(cl, "carries no header"),
                          "F4h the two sides share the lead-in and differ in the way out");
                }

                // ---- (3) the URL DOOR: it asks, on the url, for that size ---
                // openRemoteRawDeclared is the one function every human door
                // now goes through - the Browse row, a url named on the
                // command line, a drop, File > Open. Called with 0 for the
                // size, which is all a door holding a url knows: it LISTs once
                // and puts what came back in the dialog.
                closeAll();
                rawDlg.open = false;
                app.toast.clear();
                const int raisedBefore = g_rawDialogsRaised;
                openRemoteRawDeclared(url, 0);
                fprintf(stderr, "fmtgateselftest:   F4h url door: dialog=%d asks=%d "
                                "path=%s size=%zu (want %llu)\n",
                        rawDlg.open ? 1 : 0, g_rawDialogsRaised - raisedBefore,
                        rawDlg.path.c_str(), rawDlg.fileSize,
                        (unsigned long long)WANT);
                check(rawDlg.open && g_rawDialogsRaised == raisedBefore + 1 &&
                      rawDlg.path == url && rawDlg.fileSize == (size_t)WANT,
                      "F4h a url named on its own asks for the geometry, sized from LIST");
                // accept it, and compare with what the LOCAL decoder makes of
                // the same bytes - sample for sample, no tolerance. The peer
                // normalises endianness and swaps BGR itself, so an `==` here
                // is what proves the two ends agree rather than restating it.
                rawDlg.dtype = RD_U16; rawDlg.interp = RI_GRAY;
                rawDlg.w = DW; rawDlg.h = DH; rawDlg.offset = DHDR;
                rawDlg.littleEndian = true;
                const std::string uerr = rawDialogAccept(rawDlg);
                rawDlg.open = false;
                const ImageDoc* ud = app.images.empty() ? nullptr : app.images.front().get();
                RawDialog lref;
                lref.dtype = RD_U16; lref.interp = RI_GRAY;
                lref.w = DW; lref.h = DH; lref.offset = DHDR; lref.littleEndian = true;
                lref.path = dpath;
                std::string rerr;
                std::unique_ptr<ImageDoc> localRef = decodeRawFrame(lref, rerr);
                // ...and that the left-hand side is the PEER's answer and not
                // a second local decode wearing a url. Without this the whole
                // comparison could pass with the remote door removed (review:
                // "the bit-equality assert does not pin where the pixels came
                // from"): a doc that came through openRemote carries the url it
                // was fetched from and the protocol the peer announced.
                const bool cameOverTheLink = ud && !ud->src->remoteUrl.empty() &&
                                             ud->src->remoteUrl == url &&
                                             ud->src->remoteProto >= 11;
                bool bitEqual = ud && localRef && ud->w == localRef->w &&
                                ud->h == localRef->h && ud->ch == localRef->ch &&
                                ud->px().size() == localRef->px().size();
                if (bitEqual)
                    for (size_t i = 0; i < localRef->px().size(); i++)
                        if (ud->px()[i] != localRef->px()[i]) bitEqual = false;
                // anti-vacuity: two empty decodes agree perfectly
                bool varied = false;
                if (localRef && localRef->px().size() == (size_t)DW * DH)
                    for (size_t i = 1; i < localRef->px().size(); i++)
                        if (localRef->px()[i] != localRef->px()[0]) varied = true;
                fprintf(stderr, "fmtgateselftest:   F4h accept: err=\"%s\" local=%s "
                                "bit-equal=%d varied=%d\n", uerr.c_str(),
                        localRef ? "decoded" : rerr.c_str(), bitEqual ? 1 : 0,
                        varied ? 1 : 0);
                check(varied, "F4h the local reference really decoded 48 varying samples");
                check(cameOverTheLink,
                      "F4h ...and the compared document came over the LINK, not off the disk");
                check(uerr.empty() && bitEqual,
                      "F4h ...and the peer's pixels are the local decoder's, bit for bit");

                // ---- (4) BOUND: the second file of the same size, no dialog --
                // #166's rule over a link. probe2.raw holds different values
                // and exactly as many bytes, so nothing but the binding can
                // answer for it - and the door has to SAY which recipe it
                // applied, which is the one property that makes applying one
                // without asking safe.
                {
                    std::vector<uint16_t> p2((size_t)DW * DH);
                    for (size_t i = 0; i < p2.size(); i++) p2[i] = (uint16_t)(i * 11 + 5);
                    const std::string p2path = (ddir / "probe2.raw").u8string();
                    {
                        std::ofstream f2(pathFromUtf8(p2path), std::ios::binary);
                        const std::vector<uint8_t> junk(DHDR, 0x5A);
                        f2.write((const char*)junk.data(), DHDR);
                        f2.write((const char*)p2.data(), (std::streamsize)(p2.size() * 2));
                    }
                    std::string u2 = "local://" + p2path;
                    std::replace(u2.begin(), u2.end(), '\\', '/');
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    const int r2 = g_rawDialogsRaised;
                    openRemoteRawDeclared(u2, 0);
                    const ImageDoc* bd2 = app.images.empty() ? nullptr : app.images.front().get();
                    bool same2 = bd2 && bd2->px().size() == p2.size();
                    if (same2)
                        for (size_t i = 0; i < p2.size(); i++)
                            if (bd2->px()[i] != (float)p2[i]) same2 = false;
                    fprintf(stderr, "fmtgateselftest:   F4h bound: asks=%d docs=%zu "
                                    "pixels=%d toast=\"%s\"\n",
                            g_rawDialogsRaised - r2, app.images.size(), same2 ? 1 : 0,
                            app.toast.c_str());
                    check(g_rawDialogsRaised == r2 && same2,
                          "F4h a second file of the same size opens from the binding");
                    const std::string want2 = "this session's recipe for " +
                                              std::to_string(WANT) + "-byte files";
                    check(has(app.toast, want2.c_str()),
                          "F4h ...and says which recipe it applied (rawRecipeAppliedLine)");
                }

                // ---- (5) a FOLDER as one stack: one question, every member --
                // §11 stage 4: the recipe belongs to the OPEN. A stack whose
                // members disagreed about the geometry would not be one array
                // read N times - and a headerless file cannot defend itself, so
                // the bytes would still make a picture.
                //
                // Three members of 108 bytes each, different values per frame.
                std::filesystem::path sdir = ddir / "stk";
                std::filesystem::create_directories(sdir, dec);
                std::vector<std::string> sfiles;
                std::vector<std::vector<uint16_t>> svals;
                for (int k = 0; k < 3; k++) {
                    std::vector<uint16_t> v((size_t)DW * DH);
                    for (size_t i = 0; i < v.size(); i++) v[i] = (uint16_t)(i * 7 + 3 + k * 100);
                    char nm[32];
                    snprintf(nm, sizeof nm, "f_%03d.raw", k + 1);
                    const std::string fp = (sdir / nm).u8string();
                    std::ofstream f3(pathFromUtf8(fp), std::ios::binary);
                    const std::vector<uint8_t> junk(DHDR, 0xA5);
                    f3.write((const char*)junk.data(), DHDR);
                    f3.write((const char*)v.data(), (std::streamsize)(v.size() * 2));
                    f3.close();
                    std::string up = fp;
                    std::replace(up.begin(), up.end(), '\\', '/');
                    sfiles.push_back(up);
                    svals.push_back(std::move(v));
                }
                {
                    // (3)'s accept bound 108-byte files for this session, so
                    // this Open must not ask at all - and the counter is what
                    // says so: rawDlg.open alone cannot tell one question from
                    // three asked and answered in between.
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    const int r5 = g_rawDialogsRaised;
                    openRemoteStack("", sfiles, "stk/f_???.raw", 0, 0);
                    // the members arrive on the background fetcher; the UI
                    // thread swaps them in, so a headless test has to be it
                    double tw = nowSec();
                    while (nowSec() - tw < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        if (app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    // every member: the recipe on its source, and the pixels
                    // the local decoder makes of its own bytes
                    int withRecipe = 0, bitOk = 0;
                    for (const auto& d3 : app.images) {
                        if (d3->src->rawDtype == RD_U16 && d3->src->rawInterp == RI_GRAY &&
                            d3->src->rawOffset == DHDR && d3->src->rawLE &&
                            d3->src->srcW == DW && d3->src->srcH == DH) withRecipe++;
                        for (int k = 0; k < 3; k++)
                            if (d3->name == baseName(sfiles[k])) {
                                bool eq = d3->px().size() == svals[k].size();
                                if (eq)
                                    for (size_t i = 0; i < svals[k].size(); i++)
                                        if (d3->px()[i] != (float)svals[k][i]) eq = false;
                                if (eq) bitOk++;
                            }
                    }
                    fprintf(stderr, "fmtgateselftest:   F4h stack: docs=%zu seqs=%zu "
                                    "asks=%d withRecipe=%d bitOk=%d\n",
                            app.images.size(), app.seqs.size(),
                            g_rawDialogsRaised - r5, withRecipe, bitOk);
                    check(app.images.size() == 3 && app.seqs.size() == 1,
                          "F4h a headerless folder opens as ONE stack of three frames");
                    check(g_rawDialogsRaised == r5,
                          "F4h ...asking nothing: this session's binding answered for the Open");
                    check(withRecipe == 3,
                          "F4h ...and every member carries the SAME recipe the head opened with");
                    check(bitOk == 3,
                          "F4h ...with every member's pixels the local decoder's, bit for bit");
                }

                // ---- (5b) ...and the same folder with NOTHING bound ---------
                // The first-time path, which is the one a new operator takes:
                // the Open is PARKED, the dialog comes up on the head member
                // with the byte count the folder listing gave, and the answer
                // opens all three. Driven the way drawRawModal's Load drives it
                // (rawDialogAccept then resumeRemoteRawStack) because the modal
                // itself needs ImGui and this test never draws - the same reason
                // --rawrecipe-selftest drives rawDialogAccept rather than the
                // button.
                {
                    g_rawSizeRecipe.clear();          // nothing is bound now
                    closeAll();
                    rawDlg.open = false;
                    rawDlg.forRemoteStack = false;
                    app.remoteRawStack = App::PendingRemoteRawStack();
                    app.toast.clear();
                    const int r5b = g_rawDialogsRaised;
                    openRemoteStack("", sfiles, "stk/f_???.raw", 0, 0);
                    const bool parked = rawDlg.open && rawDlg.forRemoteStack &&
                                        app.remoteRawStack.alive &&
                                        app.remoteRawStack.files.size() == 3 &&
                                        rawDlg.fileSize == (size_t)WANT &&
                                        g_rawDialogsRaised == r5b + 1;
                    fprintf(stderr, "fmtgateselftest:   F4h unbound stack: asks=%d "
                                    "parked=%d files=%zu size=%zu (want %llu) docs=%zu\n",
                            g_rawDialogsRaised - r5b, app.remoteRawStack.alive ? 1 : 0,
                            app.remoteRawStack.files.size(), rawDlg.fileSize,
                            (unsigned long long)WANT, app.images.size());
                    check(parked && app.images.empty(),
                          "F4h an unbound headerless folder PARKS the Open and asks once");
                    // the modal's Load, in two calls
                    rawDlg.dtype = RD_U16; rawDlg.interp = RI_GRAY;
                    rawDlg.w = DW; rawDlg.h = DH; rawDlg.offset = DHDR;
                    rawDlg.littleEndian = true;
                    const std::string serr = rawDialogAccept(rawDlg);
                    rawDlg.forRemoteStack = false;
                    rawDlg.open = false;
                    resumeRemoteRawStack();
                    double tw2 = nowSec();
                    while (nowSec() - tw2 < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        if (app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    int rec2 = 0;
                    for (const auto& d4 : app.images)
                        if (d4->src->rawDtype == RD_U16 && d4->src->srcW == DW &&
                            d4->src->srcH == DH && d4->src->rawOffset == DHDR) rec2++;
                    fprintf(stderr, "fmtgateselftest:   F4h resumed: err=\"%s\" docs=%zu "
                                    "seqs=%zu withRecipe=%d slot-cleared=%d\n",
                            serr.c_str(), app.images.size(), app.seqs.size(), rec2,
                            app.remoteRawStack.alive ? 0 : 1);
                    check(serr.empty() && app.images.size() == 3 && app.seqs.size() == 1 &&
                          rec2 == 3,
                          "F4h ...and one answer opens all three members with it");
                    check(!app.remoteRawStack.alive,
                          "F4h ...and the parked slot is released, so the queue moves on");
                    // ...and the answer BOUND the size, which is what makes (5)
                    // above ask nothing - the two halves of #166's rule, over a
                    // link, asserted together.
                    check(rawRecipeForSize((size_t)WANT) != nullptr,
                          "F4h ...and the answer bound the byte count for this session");
                }

                // ---- (6) MEMBERS OF DIFFERENT SIZES, and the two shapes that
                //          a SUM cannot tell apart -------------------------
                // A headerless file does not defend itself: 108 bytes and 110
                // bytes both decode as 8x6 u16 with 12 B skipped, and the
                // second one comes back as a PLAUSIBLE picture of the wrong
                // bytes. So the folder is refused whole, before anything is
                // read, and both numbers are quoted.
                //
                // THREE fixtures, and the last two are the review's
                // counterexamples to the check this test first shipped with
                // (which compared the group's SUM against count x headSize):
                //
                //   uneven   108 / 110 / 112   sum 330 != 3x108=324. The sum
                //            catches this one, which is why it looked like a
                //            check at all.
                //   cancel   108 / 110 / 106   sum 324 == 3x108 EXACTLY. The
                //            deviations cancel. The peer refuses 106 (its
                //            offset+W*H*ch*elem = 108 is over that file's
                //            size) but says nothing about 110, so the sum
                //            check let a 110-byte member be read as its first
                //            108 bytes.
                //   silent   108 / 112 / 104 read at OFFSET 0, where the
                //            recipe needs only 8*6*2 = 96 bytes. Sum 324 ==
                //            3x108 again, and now every member is at or over
                //            96 - so the peer refuses NOTHING and the stack
                //            finishes with the face of a complete one while
                //            two of its three frames were read from files of
                //            other lengths. This is the case that has no
                //            second line of defence anywhere, and the one that
                //            makes min == max the check rather than the sum.
                //
                // All three must open nothing, ask nothing, and quote numbers.
                {
                    struct OddCase {
                        const char* dir;
                        int extra[3];        // samples added to DW*DH per member
                        int offset;          // the recipe's header skip
                        const char* lo;      // the two numbers the refusal owes
                        const char* hi;
                    };
                    // derived: base = 12 + 8*6*2 = 108 bytes; one extra u16
                    // sample is +2 bytes, so +1 -> 110, +2 -> 112, -1 -> 106,
                    // -2 -> 104.
                    const OddCase cases[] = {
                        { "odd",    {  0,  1,  2 }, DHDR, "108", "112" },
                        { "cancel", {  0,  1, -1 }, DHDR, "106", "110" },
                        { "silent", {  0,  2, -2 }, 0,    "104", "112" },
                    };
                    for (const OddCase& c : cases) {
                        std::filesystem::path odir = ddir / c.dir;
                        std::filesystem::remove_all(odir, dec);
                        std::filesystem::create_directories(odir, dec);
                        std::vector<std::string> ofiles;
                        uint64_t sum = 0;
                        for (int k = 0; k < 3; k++) {
                            const size_t n = (size_t)((int)(DW * DH) + c.extra[k]);
                            std::vector<uint16_t> v(n, (uint16_t)(k + 1));
                            char nm[32];
                            snprintf(nm, sizeof nm, "g_%03d.raw", k + 1);
                            const std::string fp = (odir / nm).u8string();
                            std::ofstream f4(pathFromUtf8(fp), std::ios::binary);
                            const std::vector<uint8_t> junk(DHDR, 0xA5);
                            f4.write((const char*)junk.data(), DHDR);
                            f4.write((const char*)v.data(), (std::streamsize)(v.size() * 2));
                            f4.close();
                            sum += (uint64_t)DHDR + n * 2;
                            std::string up = fp;
                            std::replace(up.begin(), up.end(), '\\', '/');
                            ofiles.push_back(up);
                        }
                        // the binding this Open would apply, so the refusal is
                        // the ONLY thing that can stop it (the "silent" case
                        // reads at offset 0, which is a different byte count
                        // key - bind that one too)
                        g_rawSizeRecipe.clear();
                        RawDialog bind;
                        bind.dtype = RD_U16; bind.interp = RI_GRAY;
                        bind.w = DW; bind.h = DH; bind.offset = c.offset;
                        bind.littleEndian = true;
                        rawRecipeBindSize((size_t)WANT, rawRecipeFromDialog(bind));
                        closeAll();
                        rawDlg.open = false;
                        app.toast.clear();
                        const int r6 = g_rawDialogsRaised;
                        openRemoteStack("", ofiles, std::string(c.dir) + "/g_???.raw", 0, 0);
                        const std::string t = app.toast;
                        const bool quiet = app.images.empty() && !rawDlg.open &&
                                           g_rawDialogsRaised == r6;
                        const bool numbers = has(t, c.lo) && has(t, c.hi);
                        fprintf(stderr, "fmtgateselftest:   F4h %s (sum=%llu, 3x108=324): "
                                        "docs=%zu asks=%d numbers=%d\n"
                                        "fmtgateselftest:   F4h %s said: \"%s\"\n",
                                c.dir, (unsigned long long)sum, app.images.size(),
                                g_rawDialogsRaised - r6, numbers ? 1 : 0, c.dir, t.c_str());
                        const std::string lab1 = std::string("F4h a folder whose members "
                            "differ in size opens nothing and asks nothing (") + c.dir + ")";
                        const std::string lab2 = std::string("F4h ...and quotes the two real "
                            "byte counts (") + c.dir + ")";
                        check(quiet, lab1.c_str());
                        check(numbers, lab2.c_str());
                    }
                    g_rawSizeRecipe.clear();
                    closeAll();
                }

                // ---- (6c) ...and when the listing names the files one by
                //           one, the refusal names the ODD ONE --------------
                // A numbered family comes back as a GROUP row, whose only
                // per-member facts are the two extremes - so the refusal above
                // can quote the numbers but not a name. Files that do NOT form
                // a numbered run come back as their own rows, and then the
                // refusal owes a name and gives one. Both halves are asserted
                // because 12.1 claimed the naming for every case, which was not
                // true of the one the fixtures happened to use (review #11).
                {
                    std::filesystem::path sdir2 = ddir / "singles";
                    std::filesystem::remove_all(sdir2, dec);
                    std::filesystem::create_directories(sdir2, dec);
                    const char* nm2[2] = { "alpha.raw", "beta.raw" };
                    const int extra2[2] = { 0, 1 };        // 108 and 110 bytes
                    std::vector<std::string> pfiles;
                    for (int k = 0; k < 2; k++) {
                        std::vector<uint16_t> v((size_t)((int)(DW * DH) + extra2[k]), 7);
                        const std::string fp = (sdir2 / nm2[k]).u8string();
                        std::ofstream f5(pathFromUtf8(fp), std::ios::binary);
                        const std::vector<uint8_t> junk(DHDR, 0xA5);
                        f5.write((const char*)junk.data(), DHDR);
                        f5.write((const char*)v.data(), (std::streamsize)(v.size() * 2));
                        f5.close();
                        std::string up = fp;
                        std::replace(up.begin(), up.end(), (char)92, '/');
                        pfiles.push_back(up);
                    }
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    const int r6c = g_rawDialogsRaised;
                    openRemoteStack("", pfiles, "singles/*.raw", 0, 0);
                    const std::string t6c = app.toast;
                    fprintf(stderr, "fmtgateselftest:   F4h singles: docs=%zu asks=%d\n"
                                    "fmtgateselftest:   F4h singles said: \"%s\"\n",
                            app.images.size(), g_rawDialogsRaised - r6c, t6c.c_str());
                    check(app.images.empty() && g_rawDialogsRaised == r6c,
                          "F4h two loose headerless files of different sizes open nothing");
                    check(has(t6c, "alpha.raw") && has(t6c, "beta.raw") &&
                          has(t6c, "108") && has(t6c, "110"),
                          "F4h ...and the refusal names BOTH files and both byte counts");
                    closeAll();
                }

                // ---- (6b) the group row is what ANSWERS it, and a pre-16 peer
                //           cannot -----------------------------------------
                // min == max is the check, and it is one field pair on the LIST
                // group row (protocol 16). Two halves, asserted together: the
                // peer reports them, and a peer that does not is refused by
                // name rather than approximated to "probably fine".
                {
                    std::vector<remote::Entry> ge;
                    std::string gerr;
                    const std::string gdir = (ddir / "cancel").u8string();
                    const bool glok = app.uiSession.list(gdir, ge, gerr);
                    const remote::Entry* grp = nullptr;
                    for (const remote::Entry& e : ge) if (e.group) grp = &e;
                    fprintf(stderr, "fmtgateselftest:   F4h group row: ok=%d group=%d "
                                    "n=%u sum=%llu min=%llu max=%llu has=%d\n",
                            glok ? 1 : 0, grp ? 1 : 0, grp ? grp->frames : 0,
                            (unsigned long long)(grp ? grp->size : 0),
                            (unsigned long long)(grp ? grp->minMemberSize : 0),
                            (unsigned long long)(grp ? grp->maxMemberSize : 0),
                            grp && grp->hasMemberSizes ? 1 : 0);
                    // derived: 108 + 110 + 106 = 324 total, extremes 106 and 110
                    check(glok && grp && grp->frames == 3 && grp->size == 324,
                          "F4h the peer folds the family into one row whose size is the SUM");
                    check(grp && grp->hasMemberSizes && grp->minMemberSize == 106 &&
                          grp->maxMemberSize == 110,
                          "F4h ...and protocol 16 carries the two extremes the sum lost");
                    check(grp && grp->size == (uint64_t)grp->frames * 108,
                          "F4h ...the sum being EXACTLY 3x108, which is why it was no check");
                }

                // ---- (7) a PREVIEW still does not ask (§5.3), and says so ---
                // One click is selection. A geometry prompt from a single
                // click would break the listing's own rule, so the preview
                // door neither opens nor asks - and the sentence it is left
                // with is the new one, asserted in F4d.
                {
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    const bool pv = openRemote(url, true, 0, 0, nullptr);
                    fprintf(stderr, "fmtgateselftest:   F4h preview: opened=%d docs=%zu "
                                    "dialog=%d\n", pv ? 1 : 0, app.images.size(),
                            rawDlg.open ? 1 : 0);
                    check(!pv && app.images.empty() && !rawDlg.open,
                          "F4h one click on a headerless row neither opens nor asks (§5.3)");
                }
                // ---- (8) THE WIRING, not just the door ---------------------
                // The whole point of this PR is that a headerless name on a
                // peer REACHES openRemoteRawDeclared from wherever it was
                // named. Every assertion above drives that function directly,
                // so the review found that the block in openRemote which routes
                // into it could be deleted with all of them still green
                // (review #17). g_rawDoorEntries is what makes the routing
                // itself observable.
                //
                // An ssh:// url, because local:// is answered one block earlier
                // by #111's reroute to the local door (5.4, and F4e asserts
                // that half). The host does not resolve, so what comes back is
                // ensureUiSession's failure - which is itself the proof: the
                // only way to reach ssh at all from here is through the door.
                // ssh runs with BatchMode=yes and ConnectTimeout=10, so this
                // costs a bounded failure and no password prompt.
                {
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    const int d0 = g_rawDoorEntries;
                    const bool opened = openRemote("ssh://viewer.invalid/tmp/x.raw",
                                                   false, 0, 0, nullptr);
                    const int entered = g_rawDoorEntries - d0;
                    const std::string t8 = app.toast;
                    fprintf(stderr, "fmtgateselftest:   F4h routing: entries=%d opened=%d "
                                    "docs=%zu\n"
                                    "fmtgateselftest:   F4h routing said: \"%s\"\n",
                            entered, opened ? 1 : 0, app.images.size(), t8.c_str());
                    check(entered == 1 && !opened && app.images.empty(),
                          "F4h a headerless name on a PEER is routed to the recipe door");
                    // ...and NOT to the generic refusal it used to reach. That
                    // sentence is the client one (peerRefusal) prefixed with
                    // the file name; the door's own failures all begin
                    // "remote: " or name the protocol.
                    check(!has(t8, "carries no header") && !has(t8, "--raw-interp bayer"),
                          "F4h ...instead of the by-name refusal that door replaced");
                    // ...and a PREVIEW is still not routed into it: one click
                    // may not raise a geometry prompt (5.3).
                    const int d1 = g_rawDoorEntries;
                    openRemote("ssh://viewer.invalid/tmp/x.raw", true, 0, 0, nullptr);
                    fprintf(stderr, "fmtgateselftest:   F4h routing (preview): entries=%d\n",
                            g_rawDoorEntries - d1);
                    check(g_rawDoorEntries == d1,
                          "F4h ...and a one-click preview is never routed into it (5.3)");
                    // ...nor is a name that STATES its own shape.
                    const int d2 = g_rawDoorEntries;
                    openRemote("ssh://viewer.invalid/tmp/x.npy", false, 0, 0, nullptr);
                    check(g_rawDoorEntries == d2,
                          "F4h ...nor a file that states its own shape");
                    closeAll();
                    app.toast.clear();
                }

                // ---- (9) a second question must not orphan the park --------
                // The park is one slot, and the Open sitting in it is waiting
                // for an answer. A question raised over it used to take the
                // dialog and drop only `forRemoteStack`, so App::remoteRawStack
                // stayed alive forever and pumpRemoteOpenQueue - which holds
                // the queue while it is alive - stopped in silence for the rest
                // of the run (review #5/#15). The first question keeps the
                // dialog; the second is refused by name.
                {
                    g_rawSizeRecipe.clear();
                    closeAll();
                    rawDlg.open = false;
                    rawDlg.forRemoteStack = false;
                    app.remoteRawStack = App::PendingRemoteRawStack();
                    app.toast.clear();
                    openRemoteStack("", sfiles, "stk/f_???.raw", 0, 0);
                    const bool parked = app.remoteRawStack.alive && rawDlg.open &&
                                        rawDlg.forRemoteStack;
                    const std::string parkedPath = rawDlg.path;
                    // the interrupting question: any other headerless file
                    app.toast.clear();
                    const int r9 = g_rawDialogsRaised;
                    const bool second = openRawDialogFor(url, (uint64_t)WANT);
                    fprintf(stderr, "fmtgateselftest:   F4h second ask: parked=%d refused=%d "
                                    "raised=%d alive=%d path-kept=%d\n"
                                    "fmtgateselftest:   F4h second ask said: \"%s\"\n",
                            parked ? 1 : 0, second ? 0 : 1, g_rawDialogsRaised - r9,
                            app.remoteRawStack.alive ? 1 : 0,
                            rawDlg.path == parkedPath ? 1 : 0, app.toast.c_str());
                    check(parked, "F4h (setup) the folder Open is parked on its question");
                    check(!second && g_rawDialogsRaised == r9,
                          "F4h a second geometry question over a live park is refused");
                    check(app.remoteRawStack.alive && rawDlg.open &&
                          rawDlg.path == parkedPath && rawDlg.forRemoteStack,
                          "F4h ...and the parked Open keeps its dialog, so it can still be answered");
                    check(has(app.toast, "already being asked"),
                          "F4h ...and the refusal says which Open is waiting");
                    // answer it, so the slot is released and nothing leaks into
                    // the blocks below (one selftest home, shared state)
                    rawDlg.dtype = RD_U16; rawDlg.interp = RI_GRAY;
                    rawDlg.w = DW; rawDlg.h = DH; rawDlg.offset = DHDR;
                    rawDlg.littleEndian = true;
                    rawDialogAccept(rawDlg);
                    rawDlg.forRemoteStack = false;
                    rawDlg.open = false;
                    resumeRemoteRawStack();
                    double tw9 = nowSec();
                    while (nowSec() - tw9 < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        if (app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    check(!app.remoteRawStack.alive,
                          "F4h ...and answering it releases the slot");
                    g_rawSizeRecipe.clear();
                    closeAll();
                }

                // ---- (10) ...and the QUEUE stops while one is parked --------
                // pumpRemoteOpenQueue drains the folder-scan chain one stack at
                // a time. Starting the next one while an earlier Open is still
                // waiting to be told how to read its frames would raise a
                // second dialog over the first and orphan whichever lost the
                // slot - so the guard is asserted here, with a real queue,
                // because F4h had none (review #18).
                {
                    g_rawSizeRecipe.clear();
                    closeAll();
                    rawDlg.open = false;
                    rawDlg.forRemoteStack = false;
                    app.remoteRawStack = App::PendingRemoteRawStack();
                    app.rbOpenQueue.clear();
                    app.toast.clear();
                    // park one Open on its question...
                    openRemoteStack("", sfiles, "stk/f_???.raw", 0, 0);
                    const bool parked = app.remoteRawStack.alive;
                    // ...and queue a second folder behind it
                    App::RemoteOpen ro;
                    ro.host = "";
                    ro.files = sfiles;
                    ro.name = "stk/f_???.raw";
                    ro.port = 0;
                    ro.token = 0;
                    app.rbOpenQueue.push_back(ro);
                    const int r10 = g_rawDialogsRaised;
                    pumpRemoteOpenQueue();
                    const bool held = app.rbOpenQueue.size() == 1 &&
                                      g_rawDialogsRaised == r10;
                    fprintf(stderr, "fmtgateselftest:   F4h queue held: parked=%d queued=%zu "
                                    "asks=%d\n", parked ? 1 : 0, app.rbOpenQueue.size(),
                            g_rawDialogsRaised - r10);
                    check(parked, "F4h (setup) one Open is parked on its question");
                    check(held,
                          "F4h the remote Open queue waits while an Open is parked on a question");
                    // release it and confirm the queue moves again, so the
                    // guard is a WAIT and not a wedge
                    app.remoteRawStack = App::PendingRemoteRawStack();
                    rawDlg.forRemoteStack = false;
                    rawDlg.open = false;
                    app.toast.clear();
                    pumpRemoteOpenQueue();
                    double tw10 = nowSec();
                    while (nowSec() - tw10 < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        if (app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    fprintf(stderr, "fmtgateselftest:   F4h queue moved: queued=%zu docs=%zu\n",
                            app.rbOpenQueue.size(), app.images.size());
                    check(app.rbOpenQueue.empty(),
                          "F4h ...and it moves again once the slot is released");
                    app.rbOpenQueue.clear();
                    g_rawSizeRecipe.clear();
                    rawDlg.open = false;
                    rawDlg.forRemoteStack = false;
                    app.remoteRawStack = App::PendingRemoteRawStack();
                    closeAll();
                }

                // ---- (11) the CFA PHASE the operator picked, over the link --
                // RGGB and BGGR change no sample - they change which plane is
                // which - so a phase that does not travel is not a wrong
                // number, it is a wrong COLOUR, and that is the same class of
                // defect (settings-inventory 4.1 measuring class). Two halves,
                // and the review found both broken: the phase never reached a
                // remote document at all, and a saved session came back RGGB
                // whatever it stored (review #2 / #3).
                //
                // The fixture is a 1-channel Bayer read of the same 108 bytes,
                // opened with GRBG (index 2), which is neither the default nor
                // symmetric with it.
                {
                    g_rawSizeRecipe.clear();
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    RawDialog bd;
                    bd.dtype = RD_U16; bd.interp = RI_BAYER;
                    bd.w = DW; bd.h = DH; bd.offset = DHDR; bd.littleEndian = true;
                    bd.cfaPattern = 2;                   // GRBG
                    bd.path = url;
                    bd.fileSize = (size_t)WANT;
                    const rp::RawWire brw = rawWireFrom(bd);
                    const bool bopened = openRemote(url, false, 0, 0, &brw);
                    const ImageDoc* bdoc = app.images.empty() ? nullptr
                                                              : app.images.back().get();
                    fprintf(stderr, "fmtgateselftest:   F4h cfa: opened=%d cfa=%d pattern=%d "
                                    "(want 1/2) flags=0x%x\n"
                                    "fmtgateselftest:   F4h cfa note: \"%s\"\n",
                            bopened ? 1 : 0, bdoc ? bdoc->cfa : -1,
                            bdoc ? bdoc->cfaPattern : -1, brw.flags,
                            bdoc ? bdoc->note.c_str() : "-");
                    check(rp::rawWireCfaPattern(brw.flags) == 2 &&
                          (brw.flags & rp::RW_LITTLE_ENDIAN) != 0,
                          "F4h the recipe carries the phase without disturbing the endian bit");
                    check(bopened && bdoc && bdoc->cfa == 1 && bdoc->cfaPattern == 2,
                          "F4h a remote Bayer frame is demosaiced in the phase that was PICKED");
                    check(bdoc && has(bdoc->note, CFA_PATTERNS[2]),
                          "F4h ...and the note declares that phase, not the default (M1)");
                    // the SIBLINGS of a stack take the same phase: a folder
                    // whose frames disagreed would change colour as it is
                    // stepped
                    closeAll();
                    rawDlg.open = false;
                    app.toast.clear();
                    g_rawSizeRecipe.clear();
                    rawRecipeBindSize((size_t)WANT, rawRecipeFromDialog(bd));
                    openRemoteStack("", sfiles, "stk/f_???.raw", 0, 0);
                    double twb = nowSec();
                    while (nowSec() - twb < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        if (app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    int phased = 0;
                    for (const auto& dq : app.images)
                        if (dq->cfa == 1 && dq->cfaPattern == 2) phased++;
                    fprintf(stderr, "fmtgateselftest:   F4h cfa siblings: docs=%zu phased=%d\n",
                            app.images.size(), phased);
                    check(app.images.size() == 3 && phased == 3,
                          "F4h every frame of a remote Bayer stack is in the SAME phase");
                    // ...and a SESSION round trip keeps it. Saved and restored
                    // through the real doors, and the pixels compared sample
                    // for sample - a colour that changed is a number that
                    // changed.
                    std::vector<float> before;
                    int bw = 0, bh = 0, bch = 0, bpat = -1, bcfa = -1;
                    if (!app.images.empty()) {
                        const ImageDoc* d0 = app.images.front().get();
                        before = d0->px();
                        bw = d0->w; bh = d0->h; bch = d0->ch;
                        bpat = d0->cfaPattern; bcfa = d0->cfa;
                    }
                    const std::string sess = (ddir / "cfa.vsession").u8string();
                    saveSession(sess, true);          // quiet: no toast to clobber
                    const bool wrote = std::filesystem::exists(pathFromUtf8(sess));
                    closeAll();
                    g_rawSizeRecipe.clear();          // nothing may answer but the LINE
                    const std::string lerr2 = loadSession(sess);
                    double tws = nowSec();
                    while (nowSec() - tws < 60.0) {
                        pumpRemoteFetch();
                        pumpSequenceAndQueue();
                        pumpRestoreWaits();
                        if (app.rfPending == 0 && loadingSettled()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpRemoteFetch();
                    const ImageDoc* rd0 = app.images.empty() ? nullptr
                                                             : app.images.front().get();
                    bool same = rd0 && !before.empty() && rd0->w == bw && rd0->h == bh &&
                                rd0->ch == bch && rd0->px().size() == before.size();
                    if (same)
                        for (size_t i = 0; i < before.size(); i++)
                            if (rd0->px()[i] != before[i]) same = false;
                    fprintf(stderr, "fmtgateselftest:   F4h session: wrote=%d load=\"%s\" "
                                    "docs=%zu pattern=%d (was %d) cfa=%d (was %d) bits=%d\n",
                            wrote ? 1 : 0, lerr2.c_str(), app.images.size(),
                            rd0 ? rd0->cfaPattern : -1, bpat, rd0 ? rd0->cfa : -1, bcfa,
                            same ? 1 : 0);
                    check(wrote && lerr2.empty() && rd0,
                          "F4h a remote headerless doc saves and reopens from its session line");
                    check(rd0 && rd0->cfaPattern == bpat && rd0->cfa == bcfa,
                          "F4h ...in the phase it was saved in, not the default");
                    check(same,
                          "F4h ...and the restored pixels are the saved ones, sample for sample");
                    g_rawSizeRecipe.clear();
                    closeAll();
                }

                g_rawSizeRecipe.clear();
                closeAll();
            }
            closeAll();
            std::filesystem::remove_all(ddir, dec);
        }

        // ---- P: the PEER, and the same file opened both ways ------------------
        //
        // Everything above this line is the CLIENT's gate. It could be made to
        // say anything without a peer existing, which is exactly what happened
        // for five formats until #111 - so the assertions below talk to a real
        // viewer-serve process (--remote-exe, or this binary when none was
        // given) and compare what came back against what the LOCAL door decodes
        // out of the same bytes.
        //
        // The strongest form available: BIT-IDENTICAL pixels. There is no
        // rounding budget to spend and no reason to grant one. The peer decodes
        // through core/imagefile.h, converts to the file's own dtype and sends
        // that; the client converts it back to float. Every dtype these formats
        // store is exactly representable in float32 (u16 max 65535 is 256 times
        // under 2^24, and a half widens to a float without loss), so the round
        // trip changes no bit - and an `==` here is what proves that claim
        // rather than restating it. A tolerance would silently absorb the day
        // somebody makes the peer send f32 for everything.
        //
        // ANTI-VACUITY IS HALF THE TEST. Two sides that both returned nothing
        // agree perfectly, so every comparison below first insists that the
        // local decode produced the expected number of samples AND that those
        // samples are not all one value - a fixture that went uniform would
        // otherwise make a broken crop compare equal.
        {
            const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
            remote::Session S;
            std::string serr;
            S.setIdleTimeout(60.0);
            const bool up = S.startOn("", 0, exe, serr);
            fprintf(stderr, "fmtgateselftest: P peer = %s : %s (protocol %d)\n",
                    exe.c_str(), up ? "connected" : serr.c_str(), S.peerVersion());
            check(up, "P0 the standalone peer answers HELLO");
            check(up && S.peerVersion() >= 10,
                  "P0 ...and announces protocol 10 or later (the picture formats)");

            // One file, opened both ways. `frames` is the file's own count, so
            // a multi-page TIFF is compared page by page.
            auto bothWays = [&](const char* fname, const char* wantDtype, int wantFrames,
                                int step) {
                if (!up) { ok = false; return; }
                const std::string path = B.dir + "/" + fname;
                remote::Meta m{};
                std::string e2;
                if (!S.meta(path, m, e2)) {
                    fprintf(stderr, "fmtgateselftest:   %s: peer META FAILED: %s\n",
                            fname, e2.c_str());
                    check(false, "P1 the peer answers META for this file");
                    return;
                }
                // the local door, on the same bytes
                closeAll();
                openPath(path);
                if (app.images.empty()) {
                    fprintf(stderr, "fmtgateselftest:   %s: did not open locally\n", fname);
                    check(false, "P1 fixture: the file opens locally");
                    return;
                }
                const ImageDoc* head = app.images.front().get();
                const int localFrames = (int)app.images.size();
                char what[192];
                snprintf(what, sizeof what, "P1 %s: peer META == the local decode", fname);
                fprintf(stderr, "fmtgateselftest:   %s: peer %dx%d ch%d %s frames=%d | "
                                "local %dx%d ch%d %s frames=%d\n",
                        fname, m.w, m.h, m.ch, m.dtype.c_str(), m.frames,
                        head->w, head->h, head->ch, head->dtype.c_str(), localFrames);
                check(m.w == head->w && m.h == head->h && m.ch == head->ch &&
                      m.dtype == head->dtype && m.dtype == wantDtype &&
                      m.frames == wantFrames && localFrames == wantFrames, what);
                if (m.w != head->w || m.h != head->h || m.ch != head->ch) return;

                std::vector<float> frame0;
                bool framesDiffer = false;
                for (int f = 0; f < m.frames; f++) {
                    std::vector<float> pix;
                    int ow = 0, oh = 0, och = 0;
                    std::string dt;
                    if (!S.tile(path, f, 0, 0, m.w, m.h, step, pix, ow, oh, och, dt, e2)) {
                        fprintf(stderr, "fmtgateselftest:   %s frame %d: peer TILE "
                                        "FAILED: %s\n", fname, f, e2.c_str());
                        check(false, "P2 the peer answers TILE for this file");
                        return;
                    }
                    // the same rect at the same stride, taken from the LOCAL
                    // document: hand-derived, not read back off the peer
                    const ImageDoc* d = app.images[(size_t)f].get();
                    std::vector<float> want;
                    for (int y = 0; y < m.h; y += step)
                        for (int x = 0; x < m.w; x += step)
                            for (int c = 0; c < m.ch; c++)
                                want.push_back(d->px()[((size_t)y * m.w + x) * m.ch + c]);
                    float lo = want.empty() ? 0.0f : want[0], hi = lo;
                    for (float v : want) { lo = std::min(lo, v); hi = std::max(hi, v); }
                    if (f == 0) frame0 = want;
                    else if (want != frame0) framesDiffer = true;
                    // ANTI-VACUITY. A frame of one value would let a crop that
                    // read the wrong rows compare equal, so a single-picture
                    // file must VARY inside the frame. stack3.tif's pages are
                    // each uniform on purpose (1000 / 2000 / 3000), so for a
                    // multi-picture file the variation that has to exist is
                    // ACROSS frames instead - asserted after the loop, because
                    // "the peer returned frame 0 three times" is exactly the
                    // bug a per-frame check cannot see.
                    const bool varies = m.frames > 1 ? true : hi > lo;
                    const size_t expectN =
                        (size_t)((m.w + step - 1) / step) * ((m.h + step - 1) / step) * m.ch;
                    size_t diff = 0;
                    float firstA = 0, firstB = 0;
                    long firstAt = -1;
                    for (size_t i = 0; i < std::min(pix.size(), want.size()); i++)
                        if (!(pix[i] == want[i])) {
                            if (!diff) { firstA = pix[i]; firstB = want[i]; firstAt = (long)i; }
                            diff++;
                        }
                    fprintf(stderr, "fmtgateselftest:   %s frame %d step %d: peer %dx%d "
                                    "ch%d %s n=%zu | local n=%zu range %.9g..%.9g | "
                                    "differing samples %zu%s\n",
                            fname, f, step, ow, oh, och, dt.c_str(), pix.size(),
                            want.size(), (double)lo, (double)hi, diff,
                            diff ? "" : " (bit for bit)");
                    if (diff)
                        fprintf(stderr, "fmtgateselftest:     first at %ld: peer %.9g "
                                        "vs local %.9g\n", firstAt, (double)firstA,
                                (double)firstB);
                    snprintf(what, sizeof what,
                             "P2 %s frame %d: peer pixels == local pixels, bit for bit",
                             fname, f);
                    // the three anti-vacuity clauses, then the equality
                    check(want.size() == expectN && pix.size() == expectN &&
                          varies && dt == wantDtype && diff == 0, what);
                }
                if (m.frames > 1) {
                    char what2[192];
                    snprintf(what2, sizeof what2,
                             "P2 %s: the %d frames are not the same frame %d times",
                             fname, m.frames, m.frames);
                    check(framesDiffer, what2);
                }
                closeAll();
            };

            // The matrix. One row per backend that the peer serves, chosen so
            // that the dtype, the channel count and the frame count are all
            // exercised somewhere - and the decimated case (step 2) is on the
            // 16-bit file, because a stride bug that dropped a byte would be
            // invisible on u8.
            bothWays("gray16.png",  "u16", 1, 2);   // u16, 1 ch, decimated
            bothWays("rgb8.png",    "u8",  1, 1);   // u8, 3 ch, full res
            bothWays("gradient.jpg","u8",  1, 1);   // the other stb door
            bothWays("gray16.tif",  "u16", 1, 1);   // our own TIFF reader
            bothWays("float32.tif", "f32", 1, 1);   // f32 straight through
            bothWays("stack3.tif",  "u16", 3, 1);   // THREE frames from ONE file
            bothWays("rgb_half.exr","f16", 1, 1);   // half, carried AS half (DT_F16)
            bothWays("mono16.y4m",  "u16", 5, 1);   // a video container's frames

            // ---- P3: a number derived by hand, not read off either side ------
            // P2 compares the peer against the local decoder, so if BOTH were
            // wrong in the same way it would still pass. This one is arithmetic
            // from the fixture's own definition (tools/gen_testdata.py):
            //
            //   g16[y][x] = ((y * 64 + x) * 21) % 65536, written as 16-bit
            //               big-endian greyscale PNG, 64 wide by 48 high
            //
            // A step-2 tile of the whole frame takes x = 0,2,4,... and
            // y = 0,2,4,..., so output sample (ox, oy) is source (2*ox, 2*oy):
            //
            //   (ox,oy) = (1,1) -> (x,y) = (2,2) -> ((2*64 + 2) * 21) % 65536
            //                                     = (130 * 21) = 2730
            //   (ox,oy) = (0,0) -> (0,0)         -> 0
            //   (ox,oy) = (31,23) -> (62,46)     -> ((46*64 + 62) * 21) % 65536
            //                                     = (3006 * 21) = 63126
            //
            // 63126 is also the maximum P2 printed for this file, which is the
            // cheapest possible check that the range line above is not a
            // coincidence. All three are exact in u16 and exact in float32.
            if (up) {
                std::vector<float> pix;
                int ow = 0, oh = 0, och = 0;
                std::string dt, e2;
                const bool got = S.tile(B.dir + "/gray16.png", 0, 0, 0, 64, 48, 2,
                                        pix, ow, oh, och, dt, e2);
                const float a = got && pix.size() > (size_t)(1 * ow + 1) ? pix[1 * ow + 1] : -1;
                const float b = got && !pix.empty() ? pix[0] : -1;
                const float c = got && pix.size() == 32 * 24 ? pix[23 * 32 + 31] : -1;
                fprintf(stderr, "fmtgateselftest:   gray16.png step 2: (1,1)=%.9g "
                                "want 2730, (0,0)=%.9g want 0, (31,23)=%.9g want 63126\n",
                        (double)a, (double)b, (double)c);
                check(got && ow == 32 && oh == 24 && och == 1 && pix.size() == 32 * 24 &&
                      a == 2730.0f && b == 0.0f && c == 63126.0f,
                      "P3 a decimated tile's samples are the ones arithmetic says");
            }

            // ---- P6: what the peer will NOT serve, and why ------------------
            // The one row whose limit is a licence. Two things are asserted and
            // the second is the one that matters: the peer must not answer this
            // with "not a .npy file" - a sentence about the FILE, for a limit
            // that belongs to the peer's build.
            if (up) {
                remote::Meta m{};
                std::string e2;
                const bool served = S.meta(B.dir + "/sensor_rggb.dng", m, e2);
                fprintf(stderr, "fmtgateselftest:   sensor_rggb.dng over the peer: "
                                "served=%d msg=\"%s\"\n", served ? 1 : 0, e2.c_str());
                check(!served && has(e2, "vendor RAW") && has(e2, "CDDL") &&
                      !has(e2, "not a .npy file"),
                      "P6 the peer refuses vendor RAW by name, with the licence reason");
            }

            // ---- P7: SCAN folds a numbered PICTURE sequence ------------------
            // Issue #148's headline: Open Folder over the peer returned "no .npy
            // stacks under ..." for a folder of PNGs the Browse list was showing
            // at the time. One group, four members, and the pattern says what
            // the stack IS - the extent, not "????".
            if (up) {
                std::vector<remote::ScanGroup> gs;
                bool trunc = false;
                int skipped = 0;
                std::string e2;
                const bool got = S.scan(B.dir + "/pngstack", 4, 64, gs, trunc, skipped, e2);
                fprintf(stderr, "fmtgateselftest:   SCAN pngstack: ok=%d groups=%zu%s%s\n",
                        got ? 1 : 0, gs.size(), e2.empty() ? "" : " err=", e2.c_str());
                for (const auto& g : gs)
                    fprintf(stderr, "fmtgateselftest:     group \"%s\" x%zu\n",
                            g.entry.name.c_str(), g.entry.members.size());
                check(got && gs.size() == 1 && gs[0].entry.members.size() == 4 &&
                      has(gs[0].entry.name, ".png"),
                      "P7 the peer folds frame_001..004.png into ONE stack group");
            }
            // ---- P8: a HEADERLESS file, read under a carried recipe ---------
            // verify-matrix G1, and the user report behind it. Everything
            // before this line is a format that states its own shape; this one
            // states nothing, so the geometry travels with the request
            // (protocol 11, docs/features/remote/remote-headerless-design.md).
            //
            // The fixture is written HERE, byte by byte, and the expected
            // samples are the ones this test wrote - not a second decoder's
            // opinion of them. For a headerless file that is the only honest
            // reference: there is no header to disagree with.
            if (up) {
                std::error_code rec;
                std::filesystem::path rdir =
                    std::filesystem::temp_directory_path(rec) / "viewer_rawwire";
                std::filesystem::remove_all(rdir, rec);
                std::filesystem::create_directories(rdir, rec);
                const int RW_W = 8, RW_H = 6, HDR = 12;
                std::vector<uint16_t> px((size_t)RW_W * RW_H);
                for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(i * 7 + 3);
                const std::string rpath = (rdir / "probe.raw").u8string();
                {   // HDR bytes of junk in front, so the offset field is doing work
                    std::ofstream rf(pathFromUtf8(rpath), std::ios::binary);
                    const std::vector<uint8_t> junk(HDR, 0xA5);
                    rf.write((const char*)junk.data(), HDR);
                    rf.write((const char*)px.data(), (std::streamsize)(px.size() * 2));
                }
                rp::RawWire rw{};
                rw.dtype = 1;                 // u16
                rw.interp = 0;                // gray
                rw.w = RW_W; rw.h = RW_H;
                rw.offset = HDR;
                rw.flags = rp::RW_LITTLE_ENDIAN;

                remote::Meta m{};
                std::string e2;
                const bool gotMeta = S.meta(rpath, m, e2, 0, &rw);
                std::vector<float> got;
                int gw = 0, gh = 0, gc = 0;
                std::string gdt;
                std::string e3;
                const bool gotTile = gotMeta &&
                    S.tile(rpath, 0, 0, 0, RW_W, RW_H, 1, got, gw, gh, gc, gdt, e3, 0,
                           nullptr, &rw);
                bool same = gotTile && got.size() == px.size();
                if (same)
                    for (size_t i = 0; i < px.size(); i++)
                        if (got[i] != (float)px[i]) same = false;
                // anti-vacuity: the fixture is not a flat field, so "equal"
                // cannot be satisfied by two runs of zeros
                bool varied = false;
                for (size_t i = 1; i < px.size(); i++) if (px[i] != px[0]) varied = true;
                fprintf(stderr, "fmtgateselftest:   P8 raw over the wire: meta=%d "
                                "%dx%d ch%d %s frames=%d | tile=%d %dx%d ch%d %s | "
                                "samples equal=%d varied=%d%s%s\n",
                        gotMeta ? 1 : 0, m.w, m.h, m.ch, m.dtype.c_str(), m.frames,
                        gotTile ? 1 : 0, gw, gh, gc, gdt.c_str(), same ? 1 : 0,
                        varied ? 1 : 0, e2.empty() ? "" : "  metaErr=", e2.c_str());
                check(gotMeta && m.w == RW_W && m.h == RW_H && m.ch == 1 &&
                      m.frames == 1 && m.dtype == "u16",
                      "P8 the peer answers META for a headerless file under a recipe");
                check(varied && same,
                      "P8 ...and TILE returns the bytes that were written, sample for sample");

                // ...and the three ways to be wrong, each refused BY NAME.
                {
                    remote::Meta m2{};
                    std::string noRec, badSize, onNpy;
                    // (i) no recipe: nothing on either end can supply a shape
                    S.meta(rpath, m2, noRec);
                    // (ii) a recipe the file cannot satisfy - both numbers, and
                    // the arithmetic that produced the second
                    rp::RawWire big = rw; big.w = 4096; big.h = 4096;
                    S.meta(rpath, m2, badSize, 0, &big);
                    // (iii) a recipe on a file that states its own shape
                    S.meta(B.dir + "/capture.npy", m2, onNpy, 0, &rw);
                    fprintf(stderr, "fmtgateselftest:   P8 refusals:\n"
                                    "fmtgateselftest:     no recipe : \"%s\"\n"
                                    "fmtgateselftest:     too small : \"%s\"\n"
                                    "fmtgateselftest:     on a .npy : \"%s\"\n",
                            noRec.c_str(), badSize.c_str(), onNpy.c_str());
                    check(has(noRec, "headerless"),
                          "P8 a headerless request with no recipe is refused by name");
                    check(has(badSize, "this recipe needs") && has(badSize, "bytes"),
                          "P8 a recipe the file cannot satisfy quotes both numbers");
                    check(has(onNpy, "states its own shape"),
                          "P8 a recipe on a self-describing file is refused, not dropped");
                }
                // ---- P7b: SCAN folds a headerless folder too (#148) ----------
                // The other half of "one folder, two answers depending on the
                // door". The single-file door opened for headerless at
                // protocol 11; the LISTING still asked peerServes, so a folder
                // of thirty .raw files that Open Folder bundles came back from
                // the peer as nothing at all - the very defect this issue is
                // named after, surviving in the one format that had just been
                // taught to cross.
                //
                // Gated on the CLIENT's number, so P9's v10 peer is not the
                // test for it: what has to be checked is that a client which
                // cannot send a recipe is not offered a group it cannot open.
                {
                    std::error_code sec;
                    std::filesystem::path sdir =
                        std::filesystem::temp_directory_path(sec) / "viewer_rawscan";
                    std::filesystem::remove_all(sdir, sec);
                    std::filesystem::create_directories(sdir, sec);
                    std::vector<uint16_t> sp((size_t)8 * 6, 1000);
                    for (int f = 1; f <= 4; f++) {
                        char nm[32];
                        snprintf(nm, sizeof nm, "shot_%03d.raw", f);
                        std::ofstream sf(pathFromUtf8((sdir / nm).u8string()),
                                         std::ios::binary);
                        sf.write((const char*)sp.data(), (std::streamsize)(sp.size() * 2));
                    }
                    std::string sroot = sdir.u8string();
                    std::replace(sroot.begin(), sroot.end(), '\\', '/');
                    std::vector<remote::ScanGroup> gs;
                    bool trunc = false;
                    int skipped = 0;
                    std::string e5;
                    const bool got = S.scan(sroot, 4, 64, gs, trunc, skipped, e5);
                    fprintf(stderr, "fmtgateselftest:   P7b SCAN a .raw folder: ok=%d "
                                    "groups=%zu%s%s\n",
                            got ? 1 : 0, gs.size(), e5.empty() ? "" : " err=", e5.c_str());
                    for (const auto& g : gs)
                        fprintf(stderr, "fmtgateselftest:     group \"%s\" x%zu\n",
                                g.entry.name.c_str(), g.entry.members.size());
                    check(got && gs.size() == 1 && gs[0].entry.members.size() == 4 &&
                          has(gs[0].entry.name, ".raw"),
                          "P7b the peer folds shot_001..004.raw into ONE stack group");
                    std::filesystem::remove_all(sdir, sec);
                }

                // ---- P8b: MEASURE over a headerless STACK --------------------
                // The remote's whole reason, applied to raw: 8 frames' worth of
                // statistics cross as a few hundred bytes instead of as pixels.
                // The frames are written HERE so sigma_t has a closed form -
                // pixel (x,y) alternates mean+d, mean-d, so with ddof=1 (#57
                // item 3, and the same estimator the local model uses)
                //
                //   var = sum (v - mean)^2 / (N-1) = N*d^2/(N-1)
                //   sigma_t = d * sqrt(N/(N-1)) = 8 * sqrt(8/7) = 8.5523597...
                //
                // and every pixel has the same one, so no sampling grid or
                // plane split can move it.
                {
                    const int MW = 8, MH = 6, MN = 8, MD = 8;
                    std::vector<std::string> frames;
                    for (int f = 0; f < MN; f++) {
                        std::vector<uint16_t> fp((size_t)MW * MH);
                        for (size_t i = 0; i < fp.size(); i++)
                            fp[i] = (uint16_t)(1000 + (f % 2 ? -MD : MD));
                        char nm[32];
                        snprintf(nm, sizeof nm, "m_%03d.raw", f);
                        const std::string fpath = (rdir / nm).u8string();
                        std::ofstream mf(pathFromUtf8(fpath), std::ios::binary);
                        mf.write((const char*)fp.data(), (std::streamsize)(fp.size() * 2));
                        frames.push_back(fpath);
                    }
                    remote::MeasureReq q;
                    q.op = rp::MOP_TEMPORAL_STATS;
                    q.paths = frames;
                    q.hasRecipe = true;
                    q.recipe = rw;
                    q.recipe.offset = 0;            // these carry no header
                    remote::MeasureResult res;
                    std::string me;
                    const bool got = S.measure(q, res, me);
                    double sigT = -1;
                    for (const auto& col : res.cols)
                        for (const auto& it : col)
                            if (it.key.find("sigma_t") != std::string::npos ||
                                it.key == "sigma_temporal") sigT = it.num;
                    const double want = (double)MD * sqrt((double)MN / (MN - 1.0));
                    fprintf(stderr, "fmtgateselftest:   P8b MEASURE over raw: ok=%d "
                                    "frames=%d sigma_t=%.9g (want %.9g)%s%s\n",
                            got ? 1 : 0, res.framesUsed, sigT, want,
                            me.empty() ? "" : "  err=", me.c_str());
                    check(got && res.framesUsed == MN,
                          "P8b the peer measures a headerless stack under one recipe");
                    check(sigT > 0 && fabs(sigT - want) < 1e-4,
                          "P8b ...and sigma_t is the closed form (ddof=1)");
                    // ...and the ops this grammar cannot carry a recipe for are
                    // refused BEFORE the request goes out. A set names several
                    // stacks; one recipe per request cannot say that, and
                    // approximating it measures the wrong bytes convincingly.
                    remote::MeasureReq bad = q;
                    bad.op = rp::MOP_SET_FOLD;
                    std::string be;
                    const bool sent = S.measure(bad, res, be);
                    fprintf(stderr, "fmtgateselftest:   P8b set fold: sent=%d \"%s\"\n",
                            sent ? 1 : 0, be.c_str());
                    check(!sent && has(be, "recipe"),
                          "P8b an op that cannot carry a recipe is refused before sending");
                    for (const auto& f : frames)
                        std::filesystem::remove(pathFromUtf8(f), rec);
                }

                // ---- P9: a peer that predates the carried recipe ------------
                // Not a thought experiment: a v10 peer, on this disk, refused
                // BEFORE the request is sent, from the number it announced.
                // The reason this cannot be left to the peer is sharper here
                // than anywhere else in this header's history - a v10 peer
                // answers a .raw with "not a .npy file", a sentence about the
                // FILE, while silently ignoring the trailer that would have
                // made the request answerable.
                {
                    selftestSetEnv("VIEWER_SERVE_PROTOCOL", "10");
                    remote::Session old;
                    std::string eo;
                    if (!old.start("", exe, eo)) {
                        fprintf(stderr, "fmtgateselftest:   P9 old peer: %s\n", eo.c_str());
                        check(false, "P9 the second peer starts");
                    } else {
                        remote::Meta m3{};
                        std::string e4;
                        const bool got = old.meta(rpath, m3, e4, 0, &rw);
                        fprintf(stderr, "fmtgateselftest:   P9 peer speaks %d, meta=%d\n"
                                        "fmtgateselftest:   P9 refusal: \"%s\"\n",
                                old.peerVersion(), got ? 1 : 0, e4.c_str());
                        check(old.peerVersion() == 10, "P9 the second peer really speaks 10");
                        check(!got && has(e4, "protocol 10") && has(e4, "needs 11") &&
                              has(e4, "Nothing was opened") && has(e4, "File > Open"),
                              "P9 ...and the client refuses from the number, naming the "
                              "mismatch and the door that works");
                        old.stop();
                    }
                    selftestSetEnv("VIEWER_SERVE_PROTOCOL", nullptr);
                }

                // ---- P9b: a peer that predates the MEMBER SIZES -------------
                // "A refusal nobody runs is a refusal that rots" (the note over
                // servedVersion, and the reason this seam exists). The check that
                // decides whether a headerless FOLDER may be opened at all is
                // `min == max` over the group row's members, and that pair of
                // numbers arrived in protocol 16 - so a v15 peer cannot answer the
                // question, and the answer to "cannot answer" is not "probably
                // fine": a folder read under one recipe when the members disagree
                // is a plausible picture of the wrong bytes.
                //
                // The three members here are EQUAL, deliberately. The refusal must
                // therefore blame the PEER and not the files - it is the one thing
                // that can go wrong in a sentence about a check that could not be
                // made - and the single-file door over the same peer must still
                // work, because one file needs no cross-member check.
                {
                    std::filesystem::path g15dir = rdir / "v15grp";
                    std::filesystem::remove_all(g15dir, rec);
                    std::filesystem::create_directories(g15dir, rec);
                    std::vector<std::string> g15names;
                    for (int k = 0; k < 3; k++) {
                        std::vector<uint16_t> v((size_t)RW_W * RW_H, (uint16_t)(k + 1));
                        char nm[32];
                        snprintf(nm, sizeof nm, "v_%03d.raw", k + 1);
                        std::ofstream vf(pathFromUtf8((g15dir / nm).u8string()),
                                         std::ios::binary);
                        const std::vector<uint8_t> junk(HDR, 0xA5);
                        vf.write((const char*)junk.data(), HDR);
                        vf.write((const char*)v.data(), (std::streamsize)(v.size() * 2));
                        vf.close();
                        g15names.push_back(nm);
                    }
                    selftestSetEnv("VIEWER_SERVE_PROTOCOL", "15");
                    remote::Session old15;
                    std::string eo15;
                    if (!old15.start("", exe, eo15)) {
                        fprintf(stderr, "fmtgateselftest:   P9b old peer: %s\n", eo15.c_str());
                        check(false, "P9b the protocol-15 peer starts");
                    } else {
                        std::vector<remote::Entry> e15;
                        std::string le15;
                        const bool l15 = old15.list(g15dir.u8string(), e15, le15);
                        const remote::Entry* g15 = nullptr;
                        for (const remote::Entry& e : e15) if (e.group) g15 = &e;
                        uint64_t sz15 = 0;
                        std::string why15;
                        const bool ok15 = remoteRawSameSize(old15, old15.peerVersion(),
                                                            g15dir.u8string(), g15names,
                                                            sz15, why15);
                        fprintf(stderr, "fmtgateselftest:   P9b peer speaks %d, list=%d "
                                        "group=%d hasSizes=%d checked=%d\n"
                                        "fmtgateselftest:   P9b refusal: \"%s\"\n",
                                old15.peerVersion(), l15 ? 1 : 0, g15 ? 1 : 0,
                                g15 && g15->hasMemberSizes ? 1 : 0, ok15 ? 1 : 0,
                                why15.c_str());
                        check(old15.peerVersion() == 15,
                              "P9b the second peer really speaks 15");
                        check(l15 && g15 && g15->members.size() == 3,
                              "P9b ...and still folds the family into a group row");
                        check(g15 && !g15->hasMemberSizes,
                              "P9b ...without the member sizes, which is the point");
                        check(!ok15 && has(why15, "protocol 15") &&
                              has(why15, "each member") && has(why15, "update viewer-serve"),
                              "P9b so a headerless FOLDER is refused by name and by number");
                        check(!has(why15, "not all one size"),
                              "P9b ...and not blamed on the files, which ARE all one size");
                        remote::Meta m15{};
                        std::string e15b;
                        const bool got15 = old15.meta(rpath, m15, e15b, 0, &rw);
                        fprintf(stderr, "fmtgateselftest:   P9b single file over v15: "
                                        "meta=%d %dx%d %s\n",
                                got15 ? 1 : 0, m15.w, m15.h, e15b.c_str());
                        check(got15 && m15.w == RW_W && m15.h == RW_H,
                              "P9b ...while a SINGLE headerless file still opens over it");
                        old15.stop();
                    }
                    selftestSetEnv("VIEWER_SERVE_PROTOCOL", nullptr);
                }
                std::filesystem::remove_all(rdir, rec);
            }
            S.stop();
        }
        closeAll();

        fprintf(stderr, "fmtgateselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
