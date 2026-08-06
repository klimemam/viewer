// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // Move-to-batch, verifiable without a human (docs/terminology.md): stacks
    // move between batches WHOLE, emptied batches are pruned, and the batch
    // name survives a session save/load round-trip (imgbatch travels by name).
    if (!g_batchSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        openFolder(g_batchSelftest);
        if (!app.folderPickOpen) {
            fprintf(stderr, "batchselftest: picker did not open\n");
            return 1;
        }
        pickerAccept();
        loadAll();
        if (app.seqs.empty() || app.images.empty()) {
            fprintf(stderr, "batchselftest: nothing loaded\n");
            return 1;
        }
        int oldBatch = app.images[0]->batchId;
        // ---- a move must not reorder the Files list ----
        // Three batches, then the FIRST stack moved into the LAST batch - the
        // direction that used to reorder everything, because the old order was
        // "whichever batch this walk of the stacks meets first" and the moved
        // stack is met first. The headings the panel draws ARE buildFileGroups'
        // labels, so this is what a human reads down the Files panel.
        //
        // The invariant is not "identical": a batch left empty by the move is
        // pruned, and its heading rightly disappears. It is that what remains
        // is still in the SAME RELATIVE ORDER - a subsequence of what was there
        // before. Nothing may overtake anything.
        if (app.seqs.size() >= 3) {
            std::vector<int> tri;
            for (const auto& si : app.seqs) tri.push_back(si.id);
            // one at a time: an empty batch is pruned by the next move, so a
            // batch has to be created immediately before something goes into it
            int bA = newBatch(uniqueBatchName("ord A"));
            moveStackToBatch(tri[0], bA);
            int bB = newBatch(uniqueBatchName("ord B"));
            moveStackToBatch(tri[1], bB);
            int bC = newBatch(uniqueBatchName("ord C"));
            for (size_t i = 2; i < tri.size(); i++) moveStackToBatch(tri[i], bC);
            auto order = [](const std::vector<FileGroup>& g) {
                std::vector<std::string> o;
                for (const auto& q : g) o.push_back(q.label);
                return o;
            };
            auto join = [](const std::vector<std::string>& v) {
                std::string o;
                for (const auto& x : v) o += (o.empty() ? "" : " ") + x;
                return o;
            };
            std::vector<std::string> before = order(buildFileGroups(stacksCached()));
            moveStackToBatch(tri[0], bC);        // first batch -> last batch
            std::vector<std::string> after = order(buildFileGroups(stacksCached()));
            size_t k = 0;                        // after must be a SUBSEQUENCE
            for (const auto& lb : before) if (k < after.size() && after[k] == lb) k++;
            bool kept = k == after.size();
            fprintf(stderr, "batchselftest: headings [%s] -> after moving the first "
                            "stack into the last batch [%s]: %s\n",
                    join(before).c_str(), join(after).c_str(), kept ? "ok" : "FAIL");
            if (!kept) {
                fprintf(stderr, "batchselftest: FAILED - the move reordered the list\n");
                ok = false;
            }
            (void)bA; (void)bB;
        }
        int nb = newBatch(uniqueBatchName("moved"));
        std::vector<int> sids;
        for (const auto& si : app.seqs) sids.push_back(si.id);
        for (int s : sids) moveStackToBatch(s, nb);
        int offBatch = 0;
        for (const auto& d : app.images) if (d->batchId != nb) offBatch++;
        bool oldGone = true;
        for (const auto& b : app.batches) if (b.id == oldBatch) oldGone = false;
        fprintf(stderr, "batchselftest: moved %d stack(s) to batch '%s': "
                        "%d of %d frames off-batch, old batch %d pruned=%d\n",
                (int)sids.size(), "moved", offBatch, (int)app.images.size(),
                oldBatch, oldGone ? 1 : 0);
        if (offBatch != 0 || !oldGone) {
            fprintf(stderr, "batchselftest: FAILED - move left strays\n");
            ok = false;
        }
        // ---- session round-trip: the batch travels BY NAME ----
        std::error_code tec;
        std::string sess = (std::filesystem::temp_directory_path(tec) /
                            "viewer_batchselftest.vsession").u8string();
        saveSession(sess, true);
        std::string lerr = loadSession(sess);      // closeAll happens inside
        if (!lerr.empty()) {
            fprintf(stderr, "batchselftest: FAILED - session reload: %s\n", lerr.c_str());
            ok = false;
        }
        loadAll();                                 // seqload rescans the folders
        int movedId = 0;
        for (const auto& b : app.batches) if (b.name == "moved") movedId = b.id;
        int strays = 0;
        for (const auto& d : app.images) if (d->batchId != movedId) strays++;
        fprintf(stderr, "batchselftest: reloaded session: %d image(s), %d stack(s), "
                        "batch 'moved' %s, %d stray frame(s)\n",
                (int)app.images.size(), (int)app.seqs.size(),
                movedId ? "restored" : "MISSING", strays);
        if (!movedId || strays != 0 || app.images.empty() ||
            app.seqs.size() != sids.size()) {
            fprintf(stderr, "batchselftest: FAILED - session did not restore the batch\n");
            ok = false;
        }
        {   // ---- loose opens: the batch is the source DIRECTORY, not its leaf
            // ~/runA/10lx and ~/runB/10lx are different measurements. Keyed on
            // the leaf name they became one "10lx" batch, so Close batch took
            // files from a directory the user never named and the series modal
            // offered two unrelated runs as one sweep with every row ticked.
            closeAll();
            int a1 = batchReuse("10lx", "/tmp/runA/10lx");
            int a2 = batchReuse("10lx", "/tmp/runA/10lx");   // same dir: same batch
            int b1 = batchReuse("10lx", "/tmp/runB/10lx");   // same leaf, other dir
            std::string n1, n2;
            for (const auto& b : app.batches) {
                if (b.id == a1) n1 = b.name;
                if (b.id == b1) n2 = b.name;
            }
            // ...while session restore still matches by NAME, which is how the
            // session format works and is the canon's rule
            int r1 = batchReuse(n2);
            bool dirOk = a1 == a2 && a1 != b1 && n1 == "10lx" && n2 == "10lx (2)" &&
                         r1 == b1;
            fprintf(stderr, "batchselftest: loose opens: runA/10lx -> %d ('%s'), again "
                            "-> %d, runB/10lx -> %d ('%s'), restore-by-name '%s' -> %d: %s\n",
                    a1, n1.c_str(), a2, b1, n2.c_str(), n2.c_str(), r1,
                    dirOk ? "ok" : "FAIL");
            if (!dirOk) ok = false;
        }
        {   // ---- rename keeps the uniqueness invariant -----------------------
            // Sessions restore batches by NAME (imgbatch -> batchReuse), so two
            // batches sharing one name silently merge on the next load. Every
            // CREATION path funnels through uniqueBatchName; the rename popup
            // was the one writer that did not.
            closeAll();
            int p1 = newBatch(uniqueBatchName("data"));
            int p2 = newBatch(uniqueBatchName("data"));
            std::string was;
            for (const auto& b : app.batches) if (b.id == p2) was = b.name;
            // exactly what the rename popup now does
            std::string want = "data", got;
            for (auto& b : app.batches)
                if (b.id == p2) {
                    std::string keep = b.name;
                    b.name.clear();
                    got = uniqueBatchName(want);
                    b.name = keep;
                    b.name = got;
                    break;
                }
            int dupes = 0;
            for (size_t i = 0; i < app.batches.size(); i++)
                for (size_t j = i + 1; j < app.batches.size(); j++)
                    if (app.batches[i].name == app.batches[j].name) dupes++;
            // "data" is taken by p1, so the closest free name is "data (2)" -
            // which is what it already was: the rename is a no-op, not a
            // runaway "data (3)"
            bool renOk = was == "data (2)" && got == "data (2)" && dupes == 0;
            fprintf(stderr, "batchselftest: rename '%s' -> 'data' became '%s', "
                            "%d duplicate name(s) among %d batch(es): %s\n",
                    was.c_str(), got.c_str(), dupes, (int)app.batches.size(),
                    renOk ? "ok" : "FAIL");
            if (!renOk) ok = false;
            (void)p1;
        }
        fprintf(stderr, "batchselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        std::filesystem::remove(std::filesystem::u8path(sess), tec);
        return ok ? 0 : 1;
    }
