// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- A CONTAINER CROSSES THE LINK (issue #217, docs/remote-reader-design
    // ---- §10, protocol 13) ---------------------------------------------------
    //
    // The user's report was ".npz is still greyed out on a peer". The refusal
    // was a decided one ("the peer serves one array per file, not a container")
    // and the thing it named as impossible - addressing one array inside a file
    // - had been built for a different reason three weeks earlier: protocol
    // 12's [key][node] trailer says "one array inside something the peer
    // materialised", and a reader's output was only the first thing that could
    // be. So this is not an npz feature; it is the second issuer of a key.
    //
    // A REAL PEER PROCESS, always, for rreader's reason: "the member was
    // inflated over there" cannot be told from "it was inflated here" by a
    // client talking to itself. --remote-exe and the standalone viewer-serve,
    // exactly as rwatch, rtemporal and rreader do it.
    if (!g_rnpzSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "rnpzselftest: %-72s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto has = [](const std::string& hay, const std::string& needle) {
            return hay.find(needle) != std::string::npos;
        };
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };

        std::string root = g_rnpzSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/rnpz-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(root), ec);
        const std::string cacheDir = root + "/peer-cache";
        std::filesystem::create_directories(pathFromUtf8(cacheDir), ec);
        setEnv("VIEWER_SERVE_CACHE", cacheDir);

        auto i32 = [](int32_t v) { return selftestNpyBytes("<i4", {}, &v, 4); };
        auto f64v = [](const std::vector<double>& v) {
            return selftestNpyBytes("<f8", { (int64_t)v.size() }, v.data(), v.size() * 8);
        };
        auto str1 = [](const std::string& s) {
            size_t item = s.empty() ? 1 : s.size();
            std::vector<uint32_t> cps(item, 0);
            for (size_t i = 0; i < s.size(); i++) cps[i] = (uint32_t)(unsigned char)s[i];
            return selftestNpyBytes(("<U" + std::to_string(item)).c_str(), {},
                                    cps.data(), cps.size() * 4);
        };

        // ---- the fixtures ----------------------------------------------------
        // (a) an ORDINARY npz: two image members of different shapes, a 1-D
        //     vector exactly as long as one stack's frame axis (an axis
        //     candidate), a scalar and a string. Every role the classifier has
        //     an opinion about, in one file, so R14 compares OPINIONS and not
        //     just pixels.
        const int S = 4, HH = 9, WW = 7;
        std::vector<uint16_t> cube((size_t)S * HH * WW);
        for (size_t i = 0; i < cube.size(); i++) cube[i] = (uint16_t)(i * 13 + 7);
        std::vector<uint16_t> flat((size_t)HH * WW);
        for (size_t i = 0; i < flat.size(); i++) flat[i] = (uint16_t)(60000 - i * 11);
        const std::vector<double> expo = { 1.5, 3.25, 7.0, 12.125 };
        const double gain = 2.75;
        const std::string many = root + "/many.npz";
        selftestWriteNpz(many, {
            { "frames",   selftestNpyBytes("<u2", { S, HH, WW }, cube.data(), cube.size() * 2) },
            { "dark",     selftestNpyBytes("<u2", { HH, WW }, flat.data(), flat.size() * 2) },
            { "exposure", f64v(expo) },
            { "gain",     selftestNpyBytes("<f8", {}, &gain, 8) },
            { "note",     str1("captured on the bench") },
        });
        // (b) ONE image member and nothing to decide - loadNpz opens it with no
        //     dialog, and the remote door has to make the same call (R15).
        const std::string lone = root + "/lone.npz";
        selftestWriteNpz(lone, {
            { "img", selftestNpyBytes("<u2", { HH, WW }, flat.data(), flat.size() * 2) },
        });
        // (c) a VIEWER CONTAINER - a series of two stacks under a batch, with
        //     conditions and a unit. This is the one that proves the TREE code
        //     is a single implementation: R12 opens a reader's tree over the
        //     link and R16 opens this one, and both go through vnzBuild.
        std::vector<uint16_t> sa((size_t)2 * HH * WW), sb((size_t)2 * HH * WW);
        for (size_t i = 0; i < sa.size(); i++) { sa[i] = (uint16_t)(100 + i); sb[i] = (uint16_t)(900 + i); }
        const std::string cont = root + "/tree.npz";
        selftestWriteNpz(cont, {
            { "__viewer",   i32(1) },
            { "__n",        i32(4) },
            { "__layer_0",  str1("batch") },
            { "__parent_0", i32(-1) },
            { "__name_0",   str1("sweep") },
            { "__layer_1",  str1("series") },
            { "__parent_1", i32(0) },
            { "__name_1",   str1("exposure sweep") },
            { "__conditions_values_1", f64v({ 2.0, 8.0 }) },
            { "__conditions_name_1",   str1("exposure") },
            { "__conditions_unit_1",   str1("ms") },
            { "__layer_2",  str1("stack") },
            { "__parent_2", i32(1) },
            { "__name_2",   str1("e2") },
            { "__pixels_2", selftestNpyBytes("<u2", { 2, HH, WW }, sa.data(), sa.size() * 2) },
            { "__layer_3",  str1("stack") },
            { "__parent_3", i32(1) },
            { "__name_3",   str1("e8") },
            { "__pixels_3", selftestNpyBytes("<u2", { 2, HH, WW }, sb.data(), sb.size() * 2) },
        });

        const std::string exe = app.remoteExe.empty() ? app.exePath : app.remoteExe;
        auto urlOf = [&](const std::string& p) { return "local://" + p; };

        // Everything below drives the REMOTE door directly. That is deliberate
        // and it is what §10.3 records: over local:// the ordinary gesture
        // still falls through to the LOCAL door (openRemote's #111 block), so
        // this is the ssh:// road being exercised on a machine where both
        // answers can be compared. The comparison is the whole point.
        auto connect = [&]() {
            std::string serr;
            if (!ensureUiSession("", serr, 0)) {
                fprintf(stderr, "rnpzselftest: cannot start the peer (%s): %s\n",
                        exe.c_str(), serr.c_str());
                return false;
            }
            return true;
        };
        // A member with a frame axis lands ONE frame at a time: openRemote puts
        // the head on screen and the rest arrive through the background fetcher
        // (which is what makes a 300-frame stack over ssh usable at all). The
        // UI thread swaps them in, so a headless test has to be that thread.
        auto drainRemote = [&](double budget) {
            double t = nowSec();
            while (nowSec() - t < budget) {
                pumpRemoteFetch();
                pumpSequenceAndQueue();
                if (app.rfPending == 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            pumpRemoteFetch();
        };
        app.remoteExe = exe;
        if (!connect()) return 1;
        check(app.uiSession.peerVersion() == (int)rp::VERSION,
              "R14a the peer speaks the protocol this build does (13)");

        // ---- R14: the same file, both doors -----------------------------------
        {
            std::string err;
            remote::NpzScan sc;
            const bool got = app.uiSession.npzScan(many, sc, err);
            check(got, "R14b the peer lists what is inside a container");
            check(got && !sc.key.empty() && sc.kind == rp::NK_ORDINARY,
                  "R14c ...with a key it issued, and it is not a viewer container");
            // The rows, from the SAME classifier, over facts from two carriers.
            std::vector<uint8_t> zip;
            std::vector<NpzEntry> ents;
            std::string zerr;
            const bool local = readFileBytes(many, zip) && npzList(zip, ents, zerr);
            check(local, "R14d (setup) the same file reads locally");
            std::vector<NpzMember> lm = local ? npzScan(zip, ents) : std::vector<NpzMember>();
            std::vector<NpzMember> rm = got ? npzClassify(sc.members) : std::vector<NpzMember>();
            bool sameRows = lm.size() == rm.size() && !lm.empty();
            std::string firstDiff;
            for (size_t i = 0; i < lm.size() && sameRows; i++) {
                const NpzMember& a = lm[i];
                const NpzMember& b = rm[i];
                if (a.name != b.name || a.role != b.role || a.shapeText != b.shapeText ||
                    a.dtype != b.dtype || a.becomes != b.becomes || a.why != b.why ||
                    a.text != b.text || a.frames != b.frames || a.vals != b.vals) {
                    sameRows = false;
                    firstDiff = a.name + ": \"" + a.becomes + "\" / \"" + b.becomes +
                                "\", \"" + a.text + "\" / \"" + b.text + "\"";
                }
            }
            fprintf(stderr, "rnpzselftest:   R14 rows: %zu local, %zu remote%s\n",
                    lm.size(), rm.size(), firstDiff.empty() ? "" : ("  first diff: " + firstDiff).c_str());
            check(sameRows,
                  "R14e every picker row is the SAME role and the same words either way");
            // ...and the values a row carries, which is what an axis IS
            bool axisSame = false;
            for (const auto& b : rm)
                if (b.name == "exposure" && b.role == NpzMember::RAxis && b.vals == expo)
                    axisSame = true;
            check(axisSame, "R14f the axis candidate's values came across at full precision");

            // The PIXELS. One member, opened through the peer, against the same
            // member opened off this disk: bit-identical or the transport
            // changed a number, which is the one thing it may never do (R8's
            // line, one container down).
            closeAll();
            const size_t rFirst = app.images.size();
            bool opened = false;
            for (const auto& b : rm)
                if (b.name == "frames") opened = openRemoteNpzMember(urlOf(many), sc.key, b, false);
            drainRemote(120.0);
            const size_t rCount = app.images.size() - rFirst;
            check(opened && rCount == (size_t)S,
                  "R14g a member opens over the link as the stack it is");
            const size_t lFirst = app.images.size();
            const std::string lerr = loadNpz(many, "frames");
            const size_t lCount = app.images.size() - lFirst;
            check(lerr.empty() && lCount == rCount,
                  "R14h ...and the same member opens locally into the same frames");
            bool identical = rCount == lCount && rCount > 0;
            for (size_t k = 0; k < rCount && identical; k++) {
                const ImageDoc* r = app.images[rFirst + k].get();
                const ImageDoc* l = app.images[lFirst + k].get();
                if (r->w != l->w || r->h != l->h || r->ch != l->ch ||
                    r->src->data.size() != l->src->data.size()) { identical = false; break; }
                if (memcmp(r->src->data.data(), l->src->data.data(),
                           r->src->data.size() * sizeof(float)) != 0) identical = false;
            }
            check(identical, "R14i every pixel is BIT-IDENTICAL to the local open");
            check(rCount && app.images[rFirst]->src->dtype == "u16",
                  "R14j ...and the dtype reads the same on both sides (u16, from a numpy u2)");
            check(rCount && app.images[rFirst]->src->member == "frames",
                  "R14k the member is on the source, which is what a session restores from");
            // ...and the peer inflated ONE member, not the file: the cache holds
            // exactly one materialised array plus the note naming the key.
            int members = 0, notes = 0;
            for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), ec)) {
                const std::string n = d.path().filename().u8string();
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".npy") == 0) members++;
                if (n.size() > 7 && n.compare(n.size() - 7, 7, ".npzsrc") == 0) notes++;
            }
            fprintf(stderr, "rnpzselftest:   R14 peer cache: %d member(s), %d note(s)\n",
                    members, notes);
            check(members == 1 && notes == 1,
                  "R14l the peer materialised ONE member of five, lazily");
            closeAll();
        }

        // ---- R15: one image member, no dialog ---------------------------------
        {
            closeAll();
            app.npzPickOpen = false;
            app.npzPick.clear();
            const size_t before = app.images.size();
            const bool got = openRemoteNpz(urlOf(lone), std::string());
            fprintf(stderr, "rnpzselftest:   R15 lone.npz: opened=%d docs=%zu dialog=%d\n",
                    got ? 1 : 0, app.images.size() - before, app.npzPickOpen ? 1 : 0);
            check(got && app.images.size() == before + 1 && !app.npzPickOpen,
                  "R15 an npz holding one image opens with no dialog, remote as local");
            closeAll();
        }

        // ---- R15b: the PICKER, end to end, over the link ----------------------
        // The gesture a person actually makes: a container with a choice in it
        // puts the same modal up, and saying yes opens what was ticked. What is
        // asserted is that the SECOND half knows about a peer too - npzPickAccept
        // re-lists before it opens anything, because the dialog stood open for
        // as long as a person took, and over a link "re-list" is a re-SCAN.
        {
            closeAll();
            app.npzPickOpen = false;
            app.npzPick.clear();
            app.npzPickQueue.clear();
            const bool asked = openRemoteNpz(urlOf(many), std::string());
            check(asked && app.npzPickOpen && app.npzPick.size() == 5,
                  "R15b a container with a choice in it puts up the SAME picker");
            check(app.npzPickPath == urlOf(many),
                  "R15b ...against the url, which is what the modal shows");
            for (auto& m : app.npzPick) m.selected = m.name == "dark";
            const size_t before = app.images.size();
            npzPickAccept();
            drainRemote(120.0);
            fprintf(stderr, "rnpzselftest:   R15b accepted: %zu doc(s), member=\"%s\"\n",
                    app.images.size() - before,
                    app.images.size() > before ? app.images[before]->src->member.c_str() : "");
            check(app.images.size() == before + 1 && !app.npzPickOpen &&
                      app.images[before]->src->member == "dark",
                  "R15b ...and what was ticked opens, by name, through the peer");
            closeAll();
        }

        // ---- R16: the TREE, over the link -------------------------------------
        // The proof that the code a declaration travels through is ONE: R12
        // (rreader) does this with a reader's tree and this does it with an
        // npz's, and both land through vnzBuild.
        {
            closeAll();
            const size_t rFirst = app.images.size();
            const bool got = openRemoteNpz(urlOf(cont), std::string());
            const size_t rDocs = app.images.size() - rFirst;
            int rSeries = (int)app.series.size();
            std::vector<double> rVals;
            std::string rUnit, rParam;
            if (rSeries) {
                for (const auto& m : app.series.back().members) rVals.push_back(m.value);
                rUnit = app.series.back().unit;
                rParam = app.series.back().paramName;
            }
            check(got && rDocs == 4,
                  "R16a a viewer container opens over the link, every frame of it");
            closeAll();
            const size_t lFirst = app.images.size();
            const std::string lerr = loadNpz(cont);
            const size_t lDocs = app.images.size() - lFirst;
            std::vector<double> lVals;
            std::string lUnit, lParam;
            if (!app.series.empty()) {
                for (const auto& m : app.series.back().members) lVals.push_back(m.value);
                lUnit = app.series.back().unit;
                lParam = app.series.back().paramName;
            }
            fprintf(stderr, "rnpzselftest:   R16 remote %zu doc(s) unit=\"%s\" param=\"%s\"; "
                            "local %zu doc(s) unit=\"%s\" param=\"%s\"\n",
                    rDocs, rUnit.c_str(), rParam.c_str(), lDocs, lUnit.c_str(), lParam.c_str());
            check(lerr.empty() && lDocs == rDocs,
                  "R16b ...into the same number of documents the local door makes");
            check(rVals == lVals && !rVals.empty(),
                  "R16c the series conditions are the same values on both sides");
            check(rUnit == lUnit && rUnit == "ms" && rParam == lParam && rParam == "exposure",
                  "R16d ...and so are the condition's name and unit");
            closeAll();
        }

        // ---- R17: the two ways a container can be unreachable ------------------
        // (a) a peer that predates the verb. Refused BEFORE anything is sent,
        //     from the number in HELLO, because a v12 peer's own answer is
        //     "unknown request" - the sentence a typo gets.
        {
            app.uiSession.stop();
            setEnv("VIEWER_SERVE_PROTOCOL", "12");
            std::string serr;
            if (!ensureUiSession("", serr, 0)) return 1;
            check(app.uiSession.peerVersion() == 12, "R17a (setup) the peer is being a v12");
            remote::NpzScan sc;
            std::string err;
            const bool sent = app.uiSession.npzScan(many, sc, err);
            check(!sent && err == rp::npzTooOldText(12, "many.npz"),
                  "R17b a container on a v12 peer is refused by the CLIENT, from the number");
            check(!has(err, "reader"),
                  "R17c ...with the container's sentence and not the reader's");
            // ...and so is a META that would carry the trailer, for the same
            // reason: an older peer does not refuse it, it never reads it.
            remote::KeyedRef ref{ "0123456789abcdef", 0, remote::KeyedRef::FromNpz };
            remote::Meta m;
            std::string merr;
            check(!app.uiSession.meta(urlOf(many), m, merr, 0, nullptr, &ref) &&
                      merr == rp::npzTooOldText(12, urlOf(many)),
                  "R17d ...and so is a META that would address a member");
            app.uiSession.stop();
            setEnv("VIEWER_SERVE_PROTOCOL", "");
        }
        // (b) the file changed on the peer after it was listed. identity BEFORE
        //     the bytes, over a link: locally the whole zip is in hand before a
        //     member is decoded, so an overwrite cannot land between the two -
        //     here the inflate happens later than the listing did, so the tuple
        //     is checked again and a change is REFUSED rather than re-bound.
        {
            if (!connect()) return 1;
            const std::string moving = root + "/moving.npz";
            selftestWriteNpz(moving, {
                { "a", selftestNpyBytes("<u2", { HH, WW }, flat.data(), flat.size() * 2) },
                { "b", selftestNpyBytes("<u2", { S, HH, WW }, cube.data(), cube.size() * 2) },
            });
            remote::NpzScan sc;
            std::string err;
            const bool got = app.uiSession.npzScan(moving, sc, err);
            check(got && sc.members.size() == 2, "R17e (setup) a two-member container is listed");
            // rewritten under the listing, with a DIFFERENT size so the stat
            // moves on every filesystem this runs on
            std::vector<uint16_t> other((size_t)HH * WW * 2, 42);
            selftestWriteNpz(moving, {
                { "a", selftestNpyBytes("<u2", { HH, WW * 2 }, other.data(), other.size() * 2) },
                { "b", selftestNpyBytes("<u2", { S, HH, WW }, cube.data(), cube.size() * 2) },
            });
            remote::Meta m;
            std::string merr;
            remote::KeyedRef ref{ sc.key, got && !sc.members.empty() ? (int)sc.members[0].entry : 0,
                                  remote::KeyedRef::FromNpz };
            const bool answered = app.uiSession.meta(urlOf(moving), m, merr, 0, nullptr, &ref);
            fprintf(stderr, "rnpzselftest:   R17 after overwrite: answered=%d \"%s\"\n",
                    answered ? 1 : 0, merr.c_str());
            check(!answered && has(merr, "changed on the peer") && has(merr, "reopen"),
                  "R17f a member inflated after the file moved is refused BY NAME");
        }

        setEnv("VIEWER_SERVE_CACHE", "");
        closeAll();
        app.uiSession.stop();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "rnpzselftest: %s\n", ok ? "ok" : "FAILED");
        stopWatchWorker();
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
