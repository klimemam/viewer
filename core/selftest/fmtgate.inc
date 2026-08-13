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
        // first two parts of docs/input-adapters.md §3.2 and, until G9, without
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
        {
            static const char* PROBE[] = { ".npy", ".npz", ".bin", ".raw", ".yuv",
                                           ".dat", ".rggb", ".png", ".tif", ".exr" };
            bool agree = true;
            std::string disagreed;
            for (const char* e : PROBE) {
                if (isLoadableExt(e) && !viewerReadsName(std::string("x") + e)) {
                    agree = false;
                    disagreed += std::string(disagreed.empty() ? "" : " ") + e;
                }
            }
            // anti-vacuity: the probe has to contain something isLoadableExt
            // actually accepts, or "they agree" is a statement about nothing
            int accepted = 0;
            for (const char* e : PROBE) if (isLoadableExt(e)) accepted++;
            fprintf(stderr, "fmtgateselftest:   Open Folder accepts %d of %d probed; "
                            "Browse disagrees on [%s]\n",
                    accepted, (int)(sizeof PROBE / sizeof PROBE[0]),
                    disagreed.empty() ? "-" : disagreed.c_str());
            check(accepted >= 7, "F4b the probe really covers the loadable set");
            check(agree, "F4b what Open Folder loads, Browse says it can open");
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
        // This stage does not open the door (that is protocol 11,
        // docs/remote-headerless-design.md stage 2). It makes the refusal TRUE:
        // the shape of this file is a declaration that lives on this machine
        // and the wire has no field to carry it, which is one step more
        // specific than the fall-through and names a door that does work.
        //
        // Walked over headerlessExts() rather than a list here, for F4c's
        // reason: an extension added to that array tomorrow is checked without
        // anybody editing this test.
        {
            bool served = false, said = true, generic = false;
            std::string worst;
            for (const std::string& e : imagefile::headerlessExts()) {
                const std::string n = "capture" + e;
                if (peerServesName(n)) served = true;          // stage 2 flips this
                const std::string why = peerRefusalFor(n);
                if (!has(why, "headerless") || !has(why, "File > Open") ||
                    !has(why, e.c_str())) { said = false; if (worst.empty()) worst = why; }
                // ...and it must not be the fall-through wearing a new coat
                if (why.rfind("the peer serves", 0) == 0) generic = true;
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
            // (protocol 11, docs/remote-headerless-design.md).
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
