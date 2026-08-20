// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 横断
    // AnalysisSet (docs/features/adapters/reader-analysisset.md, "ras"): the viewer side.
    // Containers are built BY HAND, no Python involved, for the V25 reason:
    // what is under test is the READER of the transport - given the reserved
    // members (§4), does the viewer build the set, bind the roles in §2.2
    // order, keep the version rule, survive a session (§5) and close like a
    // record rather than an owner (§6). The Python half (types, harness,
    // write side) is tools/import/test_adapters.py.
    if (!g_asetSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "asetselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        // drain: the seriesRestore gate, plus the two set queues - a bind that
        // waits for a rescan resolves here, exactly as it does in the app loop
        auto drain = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    app.seqRestore.empty() && app.aSetRestore.empty() &&
                    app.aSetBindPending.empty())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        std::error_code aec;
        // The folder this test was HANDED, counted before it runs a line: these
        // fixtures used to be "aset-scratch" INSIDE it, which is tools/testdata
        // itself - the root every other selftest is anchored at. A run that did
        // not reach its cleanup left a directory there for the next test to walk.
        const SharedFixture asetHanded(g_asetSelftest);
        // ...so the fixtures go to the per-test TMP. Still ABSOLUTE, which was
        // the point of the old std::filesystem::absolute: a Ref's path is what a
        // reader writes, and the relative form is anchored at the origin
        // (resolveSetRef), so these fixtures must name exact places.
        std::filesystem::path adir = pathFromUtf8(selftestScratch("viewer_aset"));
        std::filesystem::create_directories(adir / "darks", aec);
        std::filesystem::create_directories(adir / "two", aec);

        // ---- fixtures: plain .npy files ---------------------------------------
        std::vector<uint16_t> px64(64);
        for (int i = 0; i < 64; i++) px64[(size_t)i] = (uint16_t)(100 + i);
        const std::string darkA =
            npyWriteFile((adir / "darks" / "dark_000.npy").u8string(), "<u2",
                         { 8, 8 }, px64.data(), px64.size() * 2);
        npyWriteFile((adir / "darks" / "dark_001.npy").u8string(), "<u2",
                     { 8, 8 }, px64.data(), px64.size() * 2);
        for (const char* nm : { "a_000.npy", "a_001.npy", "b_000.npy", "b_001.npy" })
            npyWriteFile((adir / "two" / nm).u8string(), "<u2",
                         { 8, 8 }, px64.data(), px64.size() * 2);
        std::vector<uint16_t> px3(3 * 64);
        for (size_t i = 0; i < px3.size(); i++) px3[i] = (uint16_t)(i % 400);
        const std::string img10 = npyWriteFile((adir / "img10.npy").u8string(), "<u2",
                                               { 3, 8, 8 }, px3.data(), px3.size() * 2);
        const std::string flat = npyWriteFile((adir / "flat.npy").u8string(), "<u2",
                                              { 8, 8 }, px64.data(), px64.size() * 2);
        const std::string darksDir = (adir / "darks").u8string();
        const std::string twoDir = (adir / "two").u8string();

        // ---- a viewer container, by hand (the V25 writer, in the same shape) --
        // The .npy/.npz writers themselves are selftest/util.inc's: this group
        // and --verify-selftest's V25 each had a copy, and the next fixture
        // would have been a third.
        auto npyB = [](const char* descr, const std::vector<int64_t>& shape,
                       const void* data, size_t nbytes) {
            return selftestNpyBytes(descr, shape, data, nbytes);
        };
        auto strNpy = [&](const std::string& s) {
            size_t item = s.empty() ? 1 : s.size();
            std::vector<uint32_t> cps(item, 0);
            for (size_t i = 0; i < s.size(); i++) cps[i] = (uint32_t)(unsigned char)s[i];
            return npyB(("<U" + std::to_string(item)).c_str(), {},
                        cps.data(), cps.size() * 4);
        };
        auto i32Npy = [&](int32_t v) { return npyB("<i4", {}, &v, 4); };
        auto writeNpz2 = [](const std::string& path,
                            const std::vector<std::pair<std::string,
                                                        std::vector<uint8_t>>>& mem) {
            selftestWriteNpz(path, mem);
        };
        auto jesc = [](const std::string& s) {   // JSON escape for a fixture path
            std::string o;
            for (char c : s) {
                if (c == '\\' || c == '"') { o += '\\'; o += c; }
                else o += c;
            }
            return o;
        };

        // ---- A1: transport + §2.2 case 3 + folder group -----------------------
        // batch(0) > set(1, inline image, Ref dark -> folder, Ref flat -> file)
        //          > stack(2, role image)
        {
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(2) },
                { "__n",        i32Npy(3) },
                { "__layer_0",  strNpy("batch") },
                { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("analysisset") },
                { "__parent_1", i32Npy(0) },
                { "__name_1",   strNpy("PRNU 10lx") },
                { "__refs_1",   strNpy("{\"dark\": {\"path\": \"" + jesc(darksDir) +
                                       "\", \"member\": \"\"}, \"flat\": {\"path\": \"" +
                                       jesc(flat) + "\", \"member\": \"\"}}") },
                { "__layer_2",  strNpy("stack") },
                { "__parent_2", i32Npy(0 + 1) },
                { "__role_2",   strNpy("image") },
                { "__pixels_2", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
            };
            std::string cpath = (adir / "a1.npz").u8string();
            writeNpz2(cpath, cm);
            closeAll();
            std::string err = loadNpz(cpath);
            drain();
            fprintf(stderr, "asetselftest: A1 err=\"%s\" sets=%zu docs=%zu seqs=%zu\n",
                    err.c_str(), app.analysisSets.size(), app.images.size(),
                    app.seqs.size());
            check(err.empty() && app.analysisSets.size() == 1,
                  "A1 a set-bearing v2 container opens and lands ONE set");
            if (app.analysisSets.size() == 1) {
                const App::ASet& S = app.analysisSets.front();
                check(S.name == "PRNU 10lx", "A1 the set keeps its name");
                check(S.roles.size() == 3 && S.roles[0].role == "image" &&
                          S.roles[1].role == "dark" && S.roles[2].role == "flat",
                      "A1 roles in declaration order: inline, then refs by key");
                bool imageOk = S.roles[0].seqId != 0 &&
                               batchOfStack(S.roles[0].seqId) == S.batchId;
                check(imageOk, "A1 the inline member binds and LIVES in the batch");
                bool darkOk = S.roles[1].bound() && S.roles[1].seqId != 0 &&
                              framesOfSeq(S.roles[1].seqId).size() == 2;
                fprintf(stderr, "asetselftest: A1 dark seq=%d frames=%zu reason=\"%s\"\n",
                        S.roles[1].seqId,
                        S.roles[1].seqId ? framesOfSeq(S.roles[1].seqId).size() : 0,
                        S.roles[1].reason.c_str());
                check(darkOk, "A1 a folder Ref opens its ONE group as a 2-frame stack");
                bool flatOk = S.roles[2].frameUid != 0;
                int flatBatch = -1;
                for (const auto& d : app.images)
                    if (d->uid == S.roles[2].frameUid) flatBatch = d->batchId;
                check(flatOk && flatBatch == S.batchId,
                      "A1 an unopened file Ref opens INTO the call's batch (2.2-3)");
                // ras 6: the row text, exact - what the Files panel prints
                check(aSetRowText(S) == "PRNU 10lx  (3/3 roles)",
                      "A1 the set row says name + role fill");
            }
        }

        // ---- A2: default-name twins are numbered, never refused (1.4) ---------
        {
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(2) },
                { "__n",        i32Npy(5) },
                { "__layer_0",  strNpy("batch") },  { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("analysisset") }, { "__parent_1", i32Npy(0) },
                { "__layer_2",  strNpy("stack") },  { "__parent_2", i32Npy(1) },
                { "__role_2",   strNpy("image") },
                { "__pixels_2", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
                { "__layer_3",  strNpy("analysisset") }, { "__parent_3", i32Npy(0) },
                { "__layer_4",  strNpy("stack") },  { "__parent_4", i32Npy(3) },
                { "__role_4",   strNpy("image") },
                { "__pixels_4", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
            };
            std::string cpath = (adir / "a2.npz").u8string();
            writeNpz2(cpath, cm);
            closeAll();
            std::string err = loadNpz(cpath);
            drain();
            bool two = err.empty() && app.analysisSets.size() == 2;
            fprintf(stderr, "asetselftest: A2 err=\"%s\" names=\"%s\" / \"%s\"\n",
                    err.c_str(),
                    two ? app.analysisSets[0].name.c_str() : "?",
                    two ? app.analysisSets[1].name.c_str() : "?");
            check(two && app.analysisSets[0].name == "{image}" &&
                      app.analysisSets[1].name == "{image} (2)",
                  "A2 default-name twins get numbered, schema-derived names");
        }

        // ---- A3: the version rule, both directions (4) -------------------------
        {
            // (a) a set-free v1 container: opens exactly as before, no sets
            std::vector<std::pair<std::string, std::vector<uint8_t>>> v1 = {
                { "__viewer",   i32Npy(1) },
                { "__n",        i32Npy(1) },
                { "__layer_0",  strNpy("stack") }, { "__parent_0", i32Npy(-1) },
                { "__pixels_0", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
            };
            std::string p1 = (adir / "a3v1.npz").u8string();
            writeNpz2(p1, v1);
            closeAll();
            std::string e1 = loadNpz(p1);
            check(e1.empty() && app.images.size() == 3 && app.seqs.size() == 1 &&
                      app.analysisSets.empty(),
                  "A3 a set-free v1 file round-trips unchanged (compat untouched)");
            // (b) a v1 file CLAIMING a set: refused by name - the version gate
            // is the old viewers' whole protection, so the converse holds too
            std::vector<std::pair<std::string, std::vector<uint8_t>>> v1s = {
                { "__viewer",   i32Npy(1) },
                { "__n",        i32Npy(3) },
                { "__layer_0",  strNpy("batch") }, { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("stack") }, { "__parent_1", i32Npy(0) },
                { "__pixels_1", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
                { "__layer_2",  strNpy("analysisset") }, { "__parent_2", i32Npy(0) },
                { "__refs_2",   strNpy("{\"dark\": {\"path\": \"" + jesc(darksDir) +
                                       "\"}}") },
            };
            std::string p2 = (adir / "a3v1set.npz").u8string();
            writeNpz2(p2, v1s);
            closeAll();
            std::string e2 = loadNpz(p2);
            fprintf(stderr, "asetselftest: A3b err=\"%s\"\n", e2.c_str());
            check(!e2.empty() && e2.find("version 2") != std::string::npos,
                  "A3 a v1 file holding a set is refused, version named");
            // (c) a version from the future is refused by name, as ever
            std::vector<std::pair<std::string, std::vector<uint8_t>>> v4 = v1;
            v4[0] = { "__viewer", i32Npy(4) };
            std::string p3 = (adir / "a3v4.npz").u8string();
            writeNpz2(p3, v4);
            closeAll();
            std::string e3 = loadNpz(p3);
            check(!e3.empty() && e3.find("newer") != std::string::npos,
                  "A3 a future version is still refused by name");
            check(!viewerNpzVersionError(3, 2).empty() &&
                      !vns::versionError(3, 2).empty(),
                  "A3 a generation-2 NPZ/stream reader refuses generation 3 whole");
        }

        // ---- A4: Ref resolution order (2.2), all three cases -------------------
        auto setOnly = [&](const std::string& file, const std::string& refPath) {
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(2) },
                { "__n",        i32Npy(1) },
                { "__layer_0",  strNpy("analysisset") }, { "__parent_0", i32Npy(-1) },
                { "__name_0",   strNpy("only") },
                { "__refs_0",   strNpy("{\"img\": {\"path\": \"" + jesc(refPath) +
                                       "\", \"member\": \"\"}}") },
            };
            std::string cpath = (adir / file).u8string();
            writeNpz2(cpath, cm);
            return cpath;
        };
        {
            closeAll();
            std::string e0 = loadNpy(img10);
            drain();
            int openSeq = app.images.empty() ? 0 : app.images.front()->seqId;
            size_t docsBefore = app.images.size();
            std::string err = loadNpz(setOnly("a4a.npz", img10));
            drain();
            const App::ASet* S = app.analysisSets.empty() ? nullptr
                                                          : &app.analysisSets.back();
            fprintf(stderr, "asetselftest: A4a err=\"%s|%s\" docs %zu->%zu seq=%d/%d\n",
                    e0.c_str(), err.c_str(), docsBefore, app.images.size(),
                    S ? S->roles[0].seqId : -1, openSeq);
            check(err.empty() && S && S->roles[0].seqId == openSeq &&
                      app.images.size() == docsBefore,
                  "A4 tuple match SHARES the open stack - nothing re-decoded");
            check(S && S->roles[0].note.empty(),
                  "A4 a clean share carries no declaration");

            // the disk moves: same path, different bytes -> bind + declare
            std::vector<uint16_t> px4(4 * 64, 7);
            npyWriteFile(img10, "<u2", { 4, 8, 8 }, px4.data(), px4.size() * 2);
            std::string err2 = loadNpz(setOnly("a4b.npz", img10));
            drain();
            const App::ASet* S2 = app.analysisSets.empty() ? nullptr
                                                           : &app.analysisSets.back();
            fprintf(stderr, "asetselftest: A4b err=\"%s\" note=\"%s\"\n", err2.c_str(),
                    S2 ? S2->roles[0].note.c_str() : "?");
            check(err2.empty() && S2 && S2->roles[0].seqId == openSeq &&
                      S2->roles[0].note.find("bound to the open copy") != std::string::npos,
                  "A4 a moved tuple binds the OPEN copy and says the disk changed");

            // not open at all -> opened into the call's batch
            closeAll();
            std::string err3 = loadNpz(setOnly("a4c.npz", flat));
            drain();
            const App::ASet* S3 = app.analysisSets.empty() ? nullptr
                                                           : &app.analysisSets.back();
            bool opened = S3 && S3->roles[0].frameUid != 0 && app.images.size() == 1;
            int fb = -1;
            if (opened)
                for (const auto& d : app.images)
                    if (d->uid == S3->roles[0].frameUid) fb = d->batchId;
            check(err3.empty() && opened && S3 && fb == S3->batchId,
                  "A4 an unopened Ref opens, and into the calling batch");
        }

        // ---- A5: the folder refusals, by name (2.1 / decision 3) ---------------
        {
            closeAll();
            std::string err = loadNpz(setOnly("a5a.npz", twoDir));
            drain();
            const App::ASet* S = app.analysisSets.empty() ? nullptr
                                                          : &app.analysisSets.back();
            fprintf(stderr, "asetselftest: A5 two groups reason=\"%s\"\n",
                    S ? S->roles[0].reason.c_str() : "?");
            check(err.empty() && S && !S->roles[0].bound() &&
                      S->roles[0].reason.find("2 numbered groups") != std::string::npos,
                  "A5 two groups in a folder: unbound, groups counted and named");
            std::string err2 = loadNpz(setOnly("a5b.npz",
                                               (adir / "nope").u8string()));
            drain();
            const App::ASet* S2 = app.analysisSets.empty() ? nullptr
                                                           : &app.analysisSets.back();
            check(err2.empty() && S2 && !S2->roles[0].bound() &&
                      S2->roles[0].reason.find("not found") != std::string::npos,
                  "A5 a missing path: the set OPENS, the role says not found (3.1)");
            check(S2 && aSetRowText(*S2).find("(0/1 roles - img unbound)") !=
                            std::string::npos,
                  "A5 the row text carries the shortfall");
        }

        // ---- A6: a node ref - one instance, two roles, one carriage (1.2) ------
        {
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(2) },
                { "__n",        i32Npy(3) },
                { "__layer_0",  strNpy("batch") }, { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("analysisset") }, { "__parent_1", i32Npy(0) },
                { "__name_1",   strNpy("twice") },
                { "__refs_1",   strNpy("{\"bias\": {\"node\": 2}}") },
                { "__layer_2",  strNpy("stack") }, { "__parent_2", i32Npy(1) },
                { "__role_2",   strNpy("image") },
                { "__pixels_2", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
            };
            std::string cpath = (adir / "a6.npz").u8string();
            writeNpz2(cpath, cm);
            closeAll();
            std::string err = loadNpz(cpath);
            drain();
            const App::ASet* S = app.analysisSets.empty() ? nullptr
                                                          : &app.analysisSets.front();
            check(err.empty() && S && S->roles.size() == 2 &&
                      S->roles[0].seqId != 0 && S->roles[0].seqId == S->roles[1].seqId,
                  "A6 a node ref binds the SAME stack - pixels traveled once");

            // ---- A7 on the same state: closes ----------------------------------
            if (S && S->roles[0].seqId != 0) {
                int sid = S->roles[0].seqId;
                closeStack(sid);
                const App::ASet& SS = app.analysisSets.front();
                fprintf(stderr, "asetselftest: A7 after closeStack: reasons \"%s\" / \"%s\"\n",
                        SS.roles[0].reason.c_str(), SS.roles[1].reason.c_str());
                check(!SS.roles[0].bound() && !SS.roles[1].bound() &&
                          SS.roles[0].reason.find("closed") != std::string::npos,
                      "A7 closing the member unbinds the roles and says why");
                size_t docs = app.images.size();
                closeAnalysisSet(SS.id);
                check(app.analysisSets.empty() && app.images.size() == docs,
                      "A7 closing the SET drops the record only - members untouched");
            }
        }

        // ---- A8: the session block, end to end (5.1 / 5.2 adopt) ---------------
        {
            // rebuild img10 as the 3-frame stack (A4 rewrote it)
            npyWriteFile(img10, "<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2);
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(2) },
                { "__n",        i32Npy(3) },
                { "__layer_0",  strNpy("batch") }, { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("analysisset") }, { "__parent_1", i32Npy(0) },
                { "__name_1",   strNpy("S8") },
                { "__refs_1",   strNpy("{\"dark\": {\"path\": \"" + jesc(darksDir) +
                                       "\"}, \"miss\": {\"path\": \"" +
                                       jesc((adir / "gone.npy").u8string()) + "\"}}") },
                { "__layer_2",  strNpy("stack") }, { "__parent_2", i32Npy(1) },
                { "__role_2",   strNpy("image") },
                { "__pixels_2", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
            };
            std::string cpath = (adir / "a8.npz").u8string();
            writeNpz2(cpath, cm);
            closeAll();
            std::string err = loadNpz(cpath);
            drain();
            bool built = err.empty() && app.analysisSets.size() == 1 &&
                         app.analysisSets[0].roles.size() == 3 &&
                         app.analysisSets[0].roles[1].seqId != 0 &&
                         !app.analysisSets[0].roles[2].bound();
            check(built, "A8 the fixture set is up: image+dark bound, miss unbound");

            std::string sess = (adir / "a8.vsession").u8string();
            saveSession(sess, true);
            std::string body;
            {
                std::ifstream sf(pathFromUtf8(sess));
                std::stringstream s2;
                s2 << sf.rdbuf();
                body = s2.str();
            }
            bool blockOk = body.find("analysisset S8") != std::string::npos &&
                           body.find("setbatch ") != std::string::npos &&
                           body.find("setrole image stack ") != std::string::npos &&
                           body.find("setrole dark stack ") != std::string::npos &&
                           body.find("setrole miss unbound ") != std::string::npos &&
                           body.find("setend") != std::string::npos;
            fprintf(stderr, "asetselftest: A8 block ok=%d\n", (int)blockOk);
            check(blockOk, "A8 the session writes the 5.1 block, unbound role included");
            check(body.find("setreader") == std::string::npos,
                  "A8 no reader made this set, so no setreader line (5.1)");

            closeAll();
            std::string serr = loadSession(sess);
            drain();
            int darkStacks = 0;
            for (const auto& sq : app.seqs) {
                std::vector<int> fr = framesOfSeq(sq.id);
                if (!fr.empty() && app.images[fr.front()]->src->path == darkA)
                    darkStacks++;
            }
            fprintf(stderr, "asetselftest: A8 restore err=\"%s\" sets=%zu darkstacks=%d\n",
                    serr.c_str(), app.analysisSets.size(), darkStacks);
            check(serr.empty() && app.analysisSets.size() == 1,
                  "A8 restore: block + rebuild declare ONE set, adopted, not two");
            if (app.analysisSets.size() == 1) {
                const App::ASet& S = app.analysisSets.front();
                check(S.name == "S8" && S.roles.size() == 3 &&
                          S.roles[1].seqId != 0 &&
                          framesOfSeq(S.roles[1].seqId).size() == 2,
                      "A8 restore: the dark role is bound again, 2 frames");
                check(!S.roles[2].bound() &&
                          S.roles[2].reason.find("not found") != std::string::npos,
                      "A8 restore: the unbound role is still unbound, reason kept");
            }
            check(darkStacks == 1,
                  "A8 restore: the referenced stack exists ONCE (no twin per cycle)");

            // ---- A9 on the restored state: the block-only and adopt paths ------
            std::string bn;
            int bid = 0;
            if (!app.analysisSets.empty()) {
                bid = app.analysisSets[0].batchId;
                bn = batchNameOf(bid);
            }
            size_t setsBefore = app.analysisSets.size();
            {
                App::ASetRestore R;              // block-only: no reader ran for it
                R.name = "hand";
                R.batchName = bn;
                App::ASetRestore::R rr;
                rr.role = "dk";
                rr.kind = "stack";
                rr.batchName = bn;
                rr.key = darkA;
                R.roles.push_back(rr);
                app.aSetRestore.push_back(std::move(R));
            }
            drain();
            const App::ASet* H = nullptr;
            for (const auto& s : app.analysisSets) if (s.name == "hand") H = &s;
            check(app.analysisSets.size() == setsBefore + 1 && H &&
                      H->roles.size() == 1 && H->roles[0].seqId != 0,
                  "A9 a block with no reader restores from its records (5.2)");
            {
                App::ASetRestore R;              // adopt: same (batch, name) again
                R.name = "S8";
                R.batchName = bn;
                App::ASetRestore::R rr;
                rr.role = "image";
                rr.kind = "stack";
                rr.batchName = bn;
                rr.key = img10;
                R.roles.push_back(rr);
                app.aSetRestore.push_back(std::move(R));
            }
            size_t before9 = app.analysisSets.size();
            drain();
            check(app.analysisSets.size() == before9,
                  "A9 a matching (setbatch, name) block ADOPTS - no second node");
        }

        // ---- A12: a container is ONE unit and MANY session lines --------------
        // The dedupe that keeps a container from being rebuilt once per line is
        // an early return: line 1 builds the whole tree, lines 2..N find their
        // pixels already here and do nothing. But a session line is not only an
        // open - it carries that doc's range, LUT and mosaic reading - and after
        // an early return `cur()` is still whatever the rebuild left selected,
        // so every later line's display state landed on the FIRST line's doc
        // (overwriting it) and the doc the line was actually about kept the
        // container's defaults. Two stacks, two ranges, one file: the assert is
        // that each stack comes back with its own.
        //
        // Here rather than in --verify-selftest's V25 (the other place that
        // builds containers by hand) because this test is NOGL and therefore
        // gates on all three runners; V25 needs a window and gates on one.
        {
            std::vector<uint16_t> pxD((size_t)3 * 64), pxB((size_t)3 * 64);
            for (int f = 0; f < 3; f++)
                for (int i = 0; i < 64; i++) {
                    pxD[(size_t)f * 64 + i] = (uint16_t)(100 + f);
                    pxB[(size_t)f * 64 + i] = (uint16_t)(900 + f);
                }
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32Npy(1) },
                { "__n",        i32Npy(3) },
                { "__layer_0",  strNpy("batch") }, { "__parent_0", i32Npy(-1) },
                { "__layer_1",  strNpy("stack") }, { "__parent_1", i32Npy(0) },
                { "__name_1",   strNpy("dark") },
                { "__pixels_1", npyB("<u2", { 3, 8, 8 }, pxD.data(), pxD.size() * 2) },
                { "__layer_2",  strNpy("stack") }, { "__parent_2", i32Npy(0) },
                { "__name_2",   strNpy("bright") },
                { "__pixels_2", npyB("<u2", { 3, 8, 8 }, pxB.data(), pxB.size() * 2) },
            };
            std::string cpath = (adir / "a12.npz").u8string();
            writeNpz2(cpath, cm);
            closeAll();
            std::string err = loadNpz(cpath);
            drain();
            // the stack name is the node's, plus the loader's " (3 frames)"
            auto seqNamed = [](const char* nm) {
                for (const auto& s : app.seqs) if (s.name.rfind(nm, 0) == 0) return s.id;
                return 0;
            };
            int sD = seqNamed("dark"), sB = seqNamed("bright");
            {
                std::string nm;
                for (const auto& s : app.seqs) nm += (nm.empty() ? "" : ", ") + s.name;
                fprintf(stderr, "asetselftest: A12 open err=\"%s\" docs=%zu stacks=%zu "
                                "[%s]\n", err.c_str(), app.images.size(), app.seqs.size(),
                        nm.c_str());
            }
            check(err.empty() && app.images.size() == 6 && sD && sB && sD != sB,
                  "A12 the two-stack container opens as two stacks of three");
            if (sD && sB) {
                // each stack left on a frame of its own, with a range and a LUT
                // of its own: what the session line for that stack must carry
                std::vector<int> fD = framesOfSeq(sD), fB = framesOfSeq(sB);
                selectImage(fD[1]);
                setRange(*app.images[fD[1]], 10.0f, 20.0f);
                app.images[fD[1]]->displayLut = 0;
                selectImage(fB[2]);
                setRange(*app.images[fB[2]], 30.0f, 40.0f);
                std::string sess = (adir / "a12.vsession").u8string();
                saveSession(sess, true);
                closeAll();
                std::string serr = loadSession(sess);
                drain();
                int rD = seqNamed("dark"), rB = seqNamed("bright");
                std::vector<int> gD = rD ? framesOfSeq(rD) : std::vector<int>();
                std::vector<int> gB = rB ? framesOfSeq(rB) : std::vector<int>();
                int offD = 0, offB = 0, offLut = 0;
                for (int idx : gD) {
                    const ImageDoc* d = app.images[idx].get();
                    if (d->black != 10.0f || d->white != 20.0f) offD++;
                    if (d->displayLut != 0) offLut++;
                }
                for (int idx : gB) {
                    const ImageDoc* d = app.images[idx].get();
                    if (d->black != 30.0f || d->white != 40.0f) offB++;
                }
                fprintf(stderr, "asetselftest: A12 restore err=\"%s\" docs=%zu "
                                "dark %zu frame(s) %d off-range %d off-lut, "
                                "bright %zu frame(s) %d off-range\n",
                        serr.c_str(), app.images.size(), gD.size(), offD, offLut,
                        gB.size(), offB);
                check(serr.empty() && app.images.size() == 6 &&
                          gD.size() == 3 && gB.size() == 3,
                      "A12 the restore rebuilds the tree ONCE, six docs, two stacks");
                check(!gD.empty() && offD == 0 && offLut == 0,
                      "A12 the FIRST line's stack keeps its own range and LUT");
                check(!gB.empty() && offB == 0,
                      "A12 the SECOND line's stack gets ITS range, not the first's");
            }
        }

        // ---- A11: end to end - a READER returns a set, down the STREAM --------
        // The npz carrier is A1-A9's; the framed stream is the door a real
        // reader's result arrives through, and its role/refs lines deserve one
        // pass through the actual pipe. Python-gated, skipping out loud, the
        // V25h rule: a red line meaning "this machine has no numpy" would
        // train everyone to ignore red lines. Prefs are snapshotted around it
        // because a remembered reader writes prefs.txt (the V25 discipline).
        {
            std::string pyWhy;
            std::string py = adapter::findPython(app.pythonExe, pyWhy);
            std::string script = runAdapterScript();
            if (py.empty() || script.empty()) {
                fprintf(stderr, "asetselftest: A11 SKIPPED - %s\n",
                        py.empty() ? pyWhy.c_str()
                                   : "tools/import/run_adapter.py not found");
            } else {
                std::vector<App::ReaderMemo> memoSnapshot = app.readerMemo;
                std::vector<std::string> bmSnapshot = app.rbBookmarks;
                std::vector<std::string> rcSnapshot = app.rbRecents;
                std::string srcPath = (adir / "a11_src.npz").u8string();
                writeNpz2(srcPath, {
                    { "data", npyB("<u2", { 3, 8, 8 }, px3.data(), px3.size() * 2) },
                });
                std::string fwd = darksDir;      // Python source: forward slashes
                for (char& c : fwd) if (c == '\\') c = '/';
                std::string pyPath = (adir / "a11_reader.py").u8string();
                {
                    std::ofstream pf(pathFromUtf8(pyPath), std::ios::binary);
                    pf << "import numpy as np\n"
                       << "from viewer_import import AnalysisSet, Ref, Stack\n"
                       << "def load(path):\n"
                       << "    z = np.load(path)\n"
                       << "    return AnalysisSet({'image': Stack(z['data']),\n"
                       << "                        'dark': Ref('" << fwd << "')},\n"
                       << "                       name='E2E')\n";
                }
                closeAll();
                std::string herr = openWithReader(srcPath, pyPath + ":load");
                drain();
                fprintf(stderr, "asetselftest: A11 err=\"%s\" sets=%zu docs=%zu\n",
                        herr.c_str(), app.analysisSets.size(), app.images.size());
                bool built = herr.empty() && app.analysisSets.size() == 1;
                check(built, "A11 a reader's set arrives down the stream");
                if (built) {
                    const App::ASet& S = app.analysisSets.front();
                    check(S.name == "E2E" && S.readerSpec.find("a11_reader.py") !=
                              std::string::npos,
                          "A11 the set carries its reader's spec (provenance)");
                    check(S.roles.size() == 2 && S.roles[0].role == "image" &&
                              S.roles[0].seqId != 0,
                          "A11 the inline role binds through the stream too");
                    check(S.roles[1].role == "dark" && S.roles[1].seqId != 0 &&
                              framesOfSeq(S.roles[1].seqId).size() == 2,
                          "A11 the folder Ref resolves after the drain");
                }
                app.readerMemo = memoSnapshot;   // the user's prefs, as found
                app.rbBookmarks = bmSnapshot;
                app.rbRecents = rcSnapshot;
                savePrefs();
            }
        }

        // ---- A10: the row/role texts, exact (6) --------------------------------
        {
            App::ASet L;
            L.name = "L";
            App::ASetRole r1;
            r1.role = "dark2";
            r1.reason = "x not found";
            L.roles.push_back(r1);
            check(aSetRowText(L) == "L  (0/1 roles - dark2 unbound)",
                  "A10 the unbound shortfall names the first unbound role");
            check(aSetRoleText(L.roles[0]) == "dark2: unbound (x not found)",
                  "A10 the unbound role row carries its one-line reason");
        }

        closeAll();
        std::filesystem::remove_all(adir, aec);
        // the folder this test was handed must be exactly as it was found
        if (!asetHanded.intact("asetselftest")) ok = false;
        fprintf(stderr, "asetselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
