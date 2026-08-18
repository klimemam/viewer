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
              "R14a the peer speaks the protocol this build does (14)");

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

        // ---- R18: a zip64 record that does not hold together -------------------
        //
        // #221 review r3794616476. The central directory says 0xffffffff for a
        // size, which means "the real number is in the zip64 extra field", and
        // the walk then read three 64-bit values out of that field having
        // checked only that its four-byte HEADER fitted. An extra field with id
        // 1 and a declared size of 0 - a truncated file, or a writer with a bug
        // - therefore read up to 24 bytes past the end of the buffer. On the
        // PEER that is a crash, caused by a file a client did no more than
        // name; and a peer that dies takes the session with it.
        //
        // The fixtures are the three truncations in turn, because "which of the
        // three values was missing" is what the sentence has to be able to say.
        {
            // A zip written the way a zip64 writer writes one: the 32-bit
            // fields in the central directory are 0xffffffff and the real
            // numbers live in the extra field. `z64n` is how many of the three
            // u64s that field actually carries - 3 is well-formed.
            auto writeZip64 = [&](const std::string& path, const std::string& member,
                                  const std::vector<uint8_t>& body, int z64n) {
                std::vector<uint8_t> z;
                auto p16 = [&](uint32_t x) {
                    z.push_back((uint8_t)(x & 0xff));
                    z.push_back((uint8_t)((x >> 8) & 0xff));
                };
                auto p32 = [&](uint32_t x) {
                    for (int i = 0; i < 4; i++) z.push_back((uint8_t)((x >> (8 * i)) & 0xff));
                };
                auto p64 = [&](uint64_t x) {
                    for (int i = 0; i < 8; i++) z.push_back((uint8_t)((x >> (8 * i)) & 0xff));
                };
                const std::string nm = member + ".npy";
                const uint32_t crc =
                    (uint32_t)mz_crc32(MZ_CRC32_INIT, body.data(), body.size());
                const uint32_t lo = (uint32_t)z.size();
                // The LOCAL header carries the real 32-bit sizes: the fixture is
                // small, and what is under test is the CENTRAL directory's
                // zip64 record, so nothing else is made strange.
                p32(0x04034b50); p16(45); p16(0); p16(0); p16(0); p16(0);
                p32(crc); p32((uint32_t)body.size()); p32((uint32_t)body.size());
                p16((uint32_t)nm.size()); p16(0);
                z.insert(z.end(), nm.begin(), nm.end());
                z.insert(z.end(), body.begin(), body.end());
                const uint32_t cdOff = (uint32_t)z.size();
                const uint32_t elen = (uint32_t)(4 + 8 * z64n);
                p32(0x02014b50); p16(45); p16(45); p16(0); p16(0); p16(0); p16(0);
                p32(crc);
                p32(0xffffffffu); p32(0xffffffffu);           // csize, usize: "see the extra"
                p16((uint32_t)nm.size()); p16(elen); p16(0); p16(0); p16(0); p32(0);
                p32(0xffffffffu);                             // localOff: the same
                z.insert(z.end(), nm.begin(), nm.end());
                p16(1); p16((uint32_t)(8 * z64n));            // the zip64 extra field
                const uint64_t vals[3] = { (uint64_t)body.size(), (uint64_t)body.size(),
                                           (uint64_t)lo };
                for (int i = 0; i < z64n; i++) p64(vals[i]);
                const uint32_t cdSize = (uint32_t)z.size() - cdOff;
                p32(0x06054b50); p16(0); p16(0); p16(1); p16(1);
                p32(cdSize); p32(cdOff); p16(0);
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write((const char*)z.data(), (std::streamsize)z.size());
            };
            const std::vector<uint8_t> imgNpy =
                selftestNpyBytes("<u2", { HH, WW }, flat.data(), flat.size() * 2);

            if (!connect()) return 1;
            // (a) the WELL-FORMED one still opens. A bounds check that refused
            //     real zip64 files would be the same outage wearing a message.
            const std::string z64ok = root + "/z64ok.npz";
            writeZip64(z64ok, "img", imgNpy, 3);
            {
                std::vector<uint8_t> zip;
                std::vector<NpzEntry> ents;
                std::string zerr;
                const bool got = readFileBytes(z64ok, zip) && npzList(zip, ents, zerr);
                check(got && ents.size() == 1 && ents[0].usize == imgNpy.size() &&
                          ents[0].csize == imgNpy.size(),
                      "R18a a real zip64 record still reads: the sizes come out of the extra");
                remote::NpzScan sc;
                std::string err;
                const bool sent = app.uiSession.npzScan(z64ok, sc, err);
                check(sent && sc.members.size() == 1,
                      "R18b ...and the peer lists it over the link");
            }
            // (b) the three truncations, each named.
            const char* who[3] = { "uncompressed size", "compressed size",
                                   "local header offset" };
            const char* tag[3] = { "R18c a zip64 field with no room for the USIZE is refused BY NAME",
                                   "R18d ...and one with no room for the CSIZE",
                                   "R18e ...and one with no room for the LOCAL OFFSET" };
            for (int n = 0; n < 3; n++) {
                const std::string p = root + "/z64cut" + std::to_string(n) + ".npz";
                writeZip64(p, "img", imgNpy, n);
                // The LOCAL walk first - it is the same parser, and a peer that
                // refused what this door accepted would be two answers again.
                std::vector<uint8_t> zip;
                std::vector<NpzEntry> ents;
                std::string zerr;
                const bool listed = readFileBytes(p, zip) && npzList(zip, ents, zerr);
                remote::NpzScan sc;
                std::string err;
                const bool sent = app.uiSession.npzScan(p, sc, err);
                fprintf(stderr, "rnpzselftest:   R18 cut%d local=\"%s\" peer=\"%s\"\n",
                        n, zerr.c_str(), err.c_str());
                check(!listed && !sent && has(err, "img.npy") && has(err, who[n]) &&
                          err == zerr,
                      tag[n]);
            }
            // (c) THE PEER IS STILL THERE. That is the whole point: an out-of-
            //     bounds read ends the process, and a refusal does not.
            remote::NpzScan sc;
            std::string err;
            const bool alive = app.uiSession.npzScan(many, sc, err);
            check(alive && sc.members.size() == 5,
                  "R18f the same session lists a good container afterwards: the peer lived");
        }

        // ---- R19: the container's version, in the byte order the file states ---
        //
        // #221 review r3794616468. The peer read `__viewer`'s value with a
        // host-order memcpy written out at the call site, so a container whose
        // version member is a valid big-endian dtype - `>i4`, which numpy
        // writes the moment its author asks for it - arrived as 16777216 where
        // the file says 1. The client carries that number straight into
        // loadViewerNpz's ONE version gate (§5.2), which refused it as "written
        // by a newer viewer": a file that opens perfectly well off this disk was
        // unopenable over a link. Two machines, two readings of four bytes.
        //
        // The fix is not a second byte-order switch on the peer. There is ONE
        // decoder now - nz::elem, beside the peekHeader that says which order
        // the bytes are in - and both doors call it. R19a/b assert that decoder
        // directly against the raw bytes, because that is the statement the fix
        // makes; R19c carries it to the wire and R19d/e to the screen.
        {
            if (!connect()) return 1;
            // `>i4` holding 1. The payload is 00 00 00 01, which is 16777216 to
            // anything that reads it in this host's order.
            auto i32be = [](int32_t v) {
                const uint8_t b[4] = { (uint8_t)((v >> 24) & 0xff),
                                       (uint8_t)((v >> 16) & 0xff),
                                       (uint8_t)((v >> 8) & 0xff),
                                       (uint8_t)(v & 0xff) };
                return selftestNpyBytes(">i4", {}, b, 4);
            };
            const std::vector<uint8_t> vb = i32be(1);

            // ---- R19a/b: the decoder, on the bytes, with no link in sight ----
            nz::Head VH;
            std::string vherr;
            const bool peeked = nz::peekHeader(vb, VH, vherr);
            uint32_t raw = 0;                       // what a host-order read says
            if (peeked && VH.dataOff + 4 <= vb.size()) memcpy(&raw, vb.data() + VH.dataOff, 4);
            const double dec = peeked ? nz::elem(vb, VH, 0) : -1.0;
            fprintf(stderr, "rnpzselftest:   R19 __viewer as '>i4' 1: be=%d code=%s "
                            "host-order=%u  nz::elem=%.0f\n",
                    peeked && VH.be ? 1 : 0, peeked ? VH.code.c_str() : "?",
                    (unsigned)raw, dec);
            check(peeked && VH.be && VH.code == "i4" && VH.esize == 4,
                  "R19a (setup) the fixture's __viewer really IS a big-endian i4");
            check(raw == 16777216u,
                  "R19a ...and a host-order read of those four bytes really says 16777216");
            check(dec == 1.0,
                  "R19b nz::elem - the one decoder both doors call - reads the 1 the file says");

            // ---- the fixture: R16's container with that version member -------
            // Everything else is byte-for-byte what tree.npz holds, so the only
            // thing that can differ below is the version.
            const std::string contBE = root + "/tree_be.npz";
            selftestWriteNpz(contBE, {
                { "__viewer",   vb },
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

            // ---- R19c: what the PEER says the file declares -------------------
            remote::NpzScan sc;
            std::string err;
            const bool got = app.uiSession.npzScan(contBE, sc, err);
            fprintf(stderr, "rnpzselftest:   R19 peer: got=%d kind=%d version=%d \"%s\"\n",
                    got ? 1 : 0, got ? sc.kind : -1, got ? sc.containerVersion : -1,
                    err.c_str());
            check(got && sc.kind == rp::NK_CONTAINER,
                  "R19c the peer knows it is a viewer container");
            check(got && sc.containerVersion == 1,
                  "R19c ...and carries the version the FILE declares, not the host's reading");

            // ---- R19d/e: and therefore it opens, both ways ---------------------
            closeAll();
            const size_t rFirst = app.images.size();
            const bool ropen = openRemoteNpz(urlOf(contBE), std::string());
            const size_t rDocs = app.images.size() - rFirst;
            fprintf(stderr, "rnpzselftest:   R19 remote open=%d docs=%zu toast=\"%s\"\n",
                    ropen ? 1 : 0, rDocs, app.toast.c_str());
            check(ropen && rDocs == 4,
                  "R19d a big-endian container opens over the LINK, every frame of it");
            closeAll();
            const std::string lerr = loadNpz(contBE);
            fprintf(stderr, "rnpzselftest:   R19 local open docs=%zu \"%s\"\n",
                    app.images.size(), lerr.c_str());
            check(lerr.empty() && app.images.size() == 4,
                  "R19e ...and the local door opens the same file the same way");
            closeAll();

            // ---- R19f: ANTI-VACUITY - the gate R19c feeds is a real gate ------
            // A container that really does declare 16777216 is still refused. So
            // R19d/e are not "the version check is gone"; they are "the number
            // that reached the check was the file's".
            const std::string contFuture = root + "/tree_future.npz";
            selftestWriteNpz(contFuture, {
                { "__viewer",   i32(16777216) },
                { "__n",        i32(1) },
                { "__layer_0",  str1("stack") },
                { "__parent_0", i32(-1) },
                { "__name_0",   str1("e2") },
                { "__pixels_0", selftestNpyBytes("<u2", { 2, HH, WW }, sa.data(), sa.size() * 2) },
            });
            const std::string ferr = loadNpz(contFuture);
            fprintf(stderr, "rnpzselftest:   R19 declared-16777216: \"%s\"\n", ferr.c_str());
            check(has(ferr, "16777216") && has(ferr, "newer viewer"),
                  "R19f a container that really declares 16777216 is still refused BY NUMBER");
            closeAll();
        }

        // ---- R20: the SUM of the values, not just each one ---------------------
        //
        // #221 review r3794616480. Every member of an .npz can pass the
        // per-member inline rule and the file still be unlistable:
        // nz::INLINE_MAX_BYTES lets an 8 MiB axis through, a valid container may
        // hold hundreds, and 65 one-million-element f8 axes are 520 MiB of reply
        // for a file nothing is wrong with. core/remote.cpp refuses anything over
        // 512 MiB as "oversized reply from the peer" - a sentence about the
        // transport, said about a file, that nobody can act on. So the total has
        // an owner now, and it is the side that fills the message.
        //
        // THE FIXTURE IS SCALED, NOT SHAPED DIFFERENTLY. Writing the 256 MiB that
        // trips the real ceiling to prove an arithmetic comparison is a test
        // nobody would run, so the peer's ceiling is injected small
        // (VIEWER_SERVE_NPZ_SCAN_MAX) and the members are kilobytes. What is
        // being asserted is exactly the reported shape: every member is far under
        // the per-member rule (R20a), and the file is still refused (R20b).
        {
            const std::vector<double> ax(200, 1.5);
            const uint64_t one = (uint64_t)f64v(ax).size();   // header + payload
            const std::string heavy = root + "/heavy.npz";
            selftestWriteNpz(heavy, {
                { "a", f64v(ax) }, { "b", f64v(ax) }, { "c", f64v(ax) },
            });
            // Two of the three fit and the third does not - so the refusal is
            // provably about the SUM: it arrives with two members already
            // counted, not on the first thing it looked at.
            //
            // What a member COSTS is the whole of what it puts on the wire
            // (#180 codex review): its values, its one-character name, and the
            // fields that frame them. Computed from rp::'s own two numbers, so
            // this test and the peer read one definition of what a reply is.
            const uint64_t cost = rp::NPZ_SCAN_FACT_FIXED + 1 + one;   // name "a"/"b"/"c"
            const uint64_t twoFit = rp::NPZ_SCAN_REPLY_FIXED + cost * 2;
            const uint64_t ceiling = twoFit + 1;
            fprintf(stderr, "rnpzselftest:   R20 each member %llu B of values, %llu B "
                            "on the wire, per-member rule %llu B, injected ceiling "
                            "%llu B\n",
                    (unsigned long long)one, (unsigned long long)cost,
                    (unsigned long long)nz::INLINE_MAX_BYTES,
                    (unsigned long long)ceiling);
            check(cost * 3 + rp::NPZ_SCAN_REPLY_FIXED > ceiling &&
                      one < nz::INLINE_MAX_BYTES / 16,
                  "R20a every member passes the PER-MEMBER rule with room to spare");

            app.uiSession.stop();
            setEnv("VIEWER_SERVE_NPZ_SCAN_MAX", std::to_string(ceiling));
            if (!connect()) return 1;

            auto notes = [&] {
                int n = 0;
                std::error_code lec;
                for (auto& d : std::filesystem::directory_iterator(pathFromUtf8(cacheDir), lec)) {
                    const std::string nm = d.path().filename().u8string();
                    if (nm.size() > 7 && nm.compare(nm.size() - 7, 7, ".npzsrc") == 0) n++;
                }
                return n;
            };
            const int notesBefore = notes();
            remote::NpzScan sc;
            std::string err;
            const bool got = app.uiSession.npzScan(heavy, sc, err);
            const int notesAfter = notes();
            fprintf(stderr, "rnpzselftest:   R20 heavy.npz: got=%d members=%zu key=\"%s\" "
                            "notes %d->%d  \"%s\"\n",
                    got ? 1 : 0, sc.members.size(), sc.key.c_str(), notesBefore,
                    notesAfter, err.c_str());
            check(!got && sc.members.empty() && sc.key.empty(),
                  "R20b the whole scan is refused: no key, no rows, nothing trimmed to fit");
            check(has(err, "\"c\"") && has(err, std::to_string(twoFit)) &&
                      has(err, std::to_string(ceiling)) && has(err, "heavy.npz"),
                  "R20b ...naming the member that crossed it, the sum so far and the file");
            // The budget is spent BEFORE the key is published, so a refused file
            // leaves nothing behind for a client to quote back at this peer.
            check(notesAfter == notesBefore,
                  "R20c ...before anything was materialised: the peer published no key");

            // ANTI-VACUITY, both directions. Under the SAME tiny ceiling a file
            // that fits still lists - the peer did not simply stop answering...
            {
                remote::NpzScan s2;
                std::string e2;
                const bool ok2 = app.uiSession.npzScan(lone, s2, e2);
                fprintf(stderr, "rnpzselftest:   R20 lone.npz under the same ceiling: "
                                "got=%d members=%zu \"%s\"\n",
                        ok2 ? 1 : 0, s2.members.size(), e2.c_str());
                check(ok2 && s2.members.size() == 1,
                      "R20d ...and the peer lives: a file that FITS still lists under it");
            }
            // ...and the very same heavy.npz lists in full once the ceiling is
            // the real one, so R20b was about the sum and not about the file.
            app.uiSession.stop();
            setEnv("VIEWER_SERVE_NPZ_SCAN_MAX", "");
            if (!connect()) return 1;
            {
                remote::NpzScan s3;
                std::string e3;
                const bool ok3 = app.uiSession.npzScan(heavy, s3, e3);
                bool whole = ok3 && s3.members.size() == 3;
                for (const auto& m : s3.members) if (!m.whole) whole = false;
                fprintf(stderr, "rnpzselftest:   R20 heavy.npz at the real ceiling: "
                                "got=%d members=%zu \"%s\"\n",
                        ok3 ? 1 : 0, s3.members.size(), e3.c_str());
                check(whole,
                      "R20e ...and the same file lists all three, values and all, at 256 MiB");
            }
            closeAll();
        }

        // ---- R21: the budget is the MESSAGE, not the values in it --------------
        //
        // #180 codex review of #224. R20 gave the reply a ceiling and then spent
        // it on `f.bytes` alone - so everything else the message carries was
        // unbudgeted, and a zip member name is a SIXTEEN-BIT length. 5,000
        // members of 60,000 characters each are 300 MB of name; a file whose
        // values summed to 238.4 MiB passed a 256 MiB ceiling and put 524.5 MiB
        // on the wire, which came back to the person as "oversized reply from
        // the peer" - a sentence about the transport, said about a file, that
        // nobody can act on. R20's three members are named "a", "b" and "c", so
        // it could not see it.
        //
        // The fixture is that shape at test scale: the VALUES are a few hundred
        // bytes all together and the NAMES are the file. Same discipline as R20 -
        // scale the ceiling, not the shape.
        {
            const size_t NLEN = 20000;
            const std::string wa = "aaa_" + std::string(NLEN, 'a');
            const std::string wb = "bbb_" + std::string(NLEN, 'b');
            const std::string wc = "ccc_" + std::string(NLEN, 'c');
            const std::vector<uint8_t> wval = f64v({ 1.5, 2.5 });
            const std::string wide = root + "/wide.npz";
            selftestWriteNpz(wide, { { wa, wval }, { wb, wval }, { wc, wval } });
            // What each member COSTS on the wire, computed from the two numbers
            // rp:: states beside the message it describes - so this test and the
            // peer cannot disagree about what a reply is made of.
            auto costOf = [&](const std::string& nm) {
                return rp::NPZ_SCAN_FACT_FIXED + (uint64_t)nm.size() + (uint64_t)wval.size();
            };
            const uint64_t whole3 =
                rp::NPZ_SCAN_REPLY_FIXED + costOf(wa) + costOf(wb) + costOf(wc);
            // Two fit and the third does not, so the refusal is provably about
            // the SUM and arrives with two members already counted.
            const uint64_t ceiling =
                rp::NPZ_SCAN_REPLY_FIXED + costOf(wa) + costOf(wb) + 1;
            const uint64_t valuesOnly = 3 * (uint64_t)wval.size();
            fprintf(stderr, "rnpzselftest:   R21 names %zu B each, values %llu B all "
                            "together, ceiling %llu B, whole reply %llu B\n",
                    wa.size(), (unsigned long long)valuesOnly,
                    (unsigned long long)ceiling, (unsigned long long)whole3);
            // THE REPORTED SHAPE, stated as an assertion: under this ceiling the
            // old budget - which counted `f.bytes` and nothing else - had room
            // for all three members many times over, and let the file through.
            check(valuesOnly * 8 < ceiling && (uint64_t)wval.size() < nz::INLINE_MAX_BYTES,
                  "R21a every member is far under the per-member rule, and their VALUES "
                  "all together are far under the ceiling");
            check(rp::NPZ_SCAN_INLINE_MAX < (512ull << 20),
                  "R21b the peer's own ceiling is under what core/remote.cpp accepts, "
                  "so a reply that fits it can never be \"oversized\"");

            app.uiSession.stop();
            setEnv("VIEWER_SERVE_NPZ_SCAN_MAX", std::to_string(ceiling));
            if (!connect()) return 1;
            {
                remote::NpzScan sc;
                std::string err;
                const bool got = app.uiSession.npzScan(wide, sc, err);
                fprintf(stderr, "rnpzselftest:   R21 wide.npz: got=%d members=%zu "
                                "key=\"%s\" err[0..120]=\"%s\"\n",
                        got ? 1 : 0, sc.members.size(), sc.key.c_str(),
                        err.substr(0, 120).c_str());
                check(!got && sc.members.empty() && sc.key.empty(),
                      "R21c the whole scan is refused: the NAMES are counted too");
                check(has(err, "ccc_") &&
                          has(err, std::to_string(rp::NPZ_SCAN_REPLY_FIXED +
                                                  costOf(wa) + costOf(wb))) &&
                          has(err, std::to_string(ceiling)) && has(err, "wide.npz"),
                      "R21d ...naming the member that crossed it, the sum so far and the file");
            }
            // ...and at a ceiling one byte above the whole reply it lists, which
            // is what makes R21c about the sum and not about the file. The
            // WITNESS is the byte count: what came back over the link must be
            // exactly what the budget said it would be, or the ceiling is an
            // estimate again.
            app.uiSession.stop();
            setEnv("VIEWER_SERVE_NPZ_SCAN_MAX", std::to_string(whole3));
            if (!connect()) return 1;
            {
                const uint64_t rx0 = app.uiSession.bytesReceived();
                remote::NpzScan sc;
                std::string err;
                const bool got = app.uiSession.npzScan(wide, sc, err);
                const uint64_t rx = app.uiSession.bytesReceived() - rx0;
                fprintf(stderr, "rnpzselftest:   R21 wide.npz at %llu B: got=%d "
                                "members=%zu received %llu B (predicted %llu + %zu "
                                "header)\n",
                        (unsigned long long)whole3, got ? 1 : 0, sc.members.size(),
                        (unsigned long long)rx, (unsigned long long)whole3,
                        sizeof(rp::Header));
                bool whole = got && sc.members.size() == 3;
                for (const auto& m : sc.members) if (!m.whole) whole = false;
                check(whole,
                      "R21e ...and the same file lists all three, values and all, one "
                      "byte above the reply it produces");
                check(rx == whole3 + sizeof(rp::Header),
                      "R21f ...and the reply that arrived is the size the budget "
                      "predicted, to the byte");
            }
            app.uiSession.stop();
            setEnv("VIEWER_SERVE_NPZ_SCAN_MAX", "");
            if (!connect()) return 1;
            closeAll();
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
