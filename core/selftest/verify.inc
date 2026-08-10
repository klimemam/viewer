// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 横断
    if (!g_verifySelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto reload = [&]() {
            closeAll();
            openFolder(g_verifySelftest);
            if (app.folderPickOpen) pickerAccept();
            loadAll();
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "verifyselftest: %-46s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };

        // ---- WHERE THE SCRATCH GOES, AND WHY IT MOVED --------------------------
        // Five blocks below write fixtures they build themselves. They used to
        // write them into the folder UNDER TEST (<multi>/vfy-*-scratch, and one
        // in its parent) and remove them at the END. A run that dies never
        // reaches an end - PR #116's fail-first killed one on purpose - so the
        // directory stayed, and the NEXT run's V12 scanned <multi>, found folder
        // groups nobody made, and failed:
        //
        //     stack: vfy-native-scratch/a_hw3‥4.npy - 2 files (npy)
        //     verifyselftest: V12 6 local stack(s): 1 without an expected count...
        //
        // ...and THAT run then cleaned up, so the run after it passed. A red that
        // appears once and erases its own cause is the worst shape an
        // intermittent can have, and this repo has already spent real days on
        // exactly that class (the browse-keys diagnosis).
        //
        // So the scratch moves into the per-test TMP directory CMake hands every
        // selftest (viewer_selftest(): TMP/TEMP/TMPDIR = <build>/selftest-tmp/
        // <name>). That is the scheme the rest of the suite already uses -
        // --picker-selftest UC7, --series-selftest's .npz members,
        // --browse-selftest's hlenbomb - and it does more than tidy the leak: no
        // selftest scans TMP, so a directory a crash leaves there cannot be read
        // as data by anything, ever. Cleaning at the END was the defect; cleaning
        // at the START of a block that still wrote under <multi> would leave a
        // stray folder in front of V12 for as long as the block ran, and V12
        // runs after two of these blocks.
        //
        // Each scratch is also cleared BEFORE it is written, not only after: a
        // crash leaves files in TMP too, and a block should start from what it
        // wrote, not from what the last run got as far as writing.
        auto vfyScratch = [](const char* name) {
            std::error_code ec;
            std::filesystem::path p = std::filesystem::temp_directory_path(ec) / name;
            std::filesystem::remove_all(p, ec);
            std::filesystem::create_directories(p, ec);
            return p;
        };
        {   // ...and the trees an OLDER build already poisoned heal here, once,
            // before anything scans: exactly the five names this test used to
            // write, removed from exactly where it used to write them. Nothing
            // else is touched - a selftest that deletes directories it did not
            // create is a worse hazard than the one being fixed.
            // tools/gen_testdata.py rewrites multi/ from scratch on every full
            // suite run, so this matters to a bare `ctest` - which is how the
            // leak was found in the first place, and how it would be met again.
            std::error_code lec;
            const std::filesystem::path root(g_verifySelftest);
            for (const char* legacy : { "vfy-session-scratch", "vfy-npz-scratch",
                                        "vfy-native-scratch", "vfy-adapter-scratch" })
                if (std::filesystem::exists(root / legacy, lec)) {
                    fprintf(stderr, "verifyselftest: removing %s left by an aborted run\n",
                            legacy);
                    std::filesystem::remove_all(root / legacy, lec);
                }
            const std::filesystem::path up = root.parent_path() / "vfy-frameaxis-scratch";
            if (std::filesystem::exists(up, lec)) {
                fprintf(stderr, "verifyselftest: removing vfy-frameaxis-scratch left by "
                                "an aborted run\n");
                std::filesystem::remove_all(up, lec);
            }
        }

        // ---- V1: a stack whose frames are NOT contiguous in app.images ----
        reload();
        if (app.seqs.size() < 3) {
            fprintf(stderr, "verifyselftest: need 3 stacks under %s\n", g_verifySelftest.c_str());
            return 1;
        }
        {   // interleave: round-robin the three stacks so no stack is a run
            std::vector<std::unique_ptr<ImageDoc>> mixed;
            for (size_t k = 0; mixed.size() < app.images.size(); k++)
                for (auto& d : app.images)
                    if (d && (size_t)d->seqIndex == k) mixed.push_back(std::move(d));
            for (auto& d : app.images) if (d) mixed.push_back(std::move(d));
            app.images.swap(mixed);
            app.current = 0;
        }
        {
            int a0 = app.seqs[0].id, a1 = app.seqs[1].id, a2 = app.seqs[2].id;
            std::vector<int> f1 = framesOfSeq(a1);
            bool contiguous = true;                 // prove the fixture is nasty
            for (size_t i = 1; i < f1.size(); i++)
                if (f1[i] != f1[i - 1] + 1) contiguous = false;
            int before = (int)app.images.size(), n1 = (int)f1.size();
            closeStack(a1);
            fprintf(stderr, "verifyselftest: V1 interleaved(contig=%d) closeStack(%d): "
                            "images %d->%d (-%d), survivors %zu/%zu frames\n",
                    contiguous ? 1 : 0, a1, before, (int)app.images.size(), n1,
                    framesOfSeq(a0).size(), framesOfSeq(a2).size());
            check(!contiguous, "V1 fixture really is non-contiguous");
            check((int)app.images.size() == before - n1 && framesOfSeq(a1).empty() &&
                  framesOfSeq(a0).size() == 5 && framesOfSeq(a2).size() == 5,
                  "V1 non-contiguous closeStack keeps neighbours");
        }

        // ---- V2: Ctrl+Alt+W on a middle frame leaves a coherent stack ----
        reload();
        {
            int sid = app.seqs[1].id;
            std::vector<int> fr = framesOfSeq(sid);
            int want = (int)fr.size() - 1;
            uint64_t revBefore = app.imagesRev;
            selectImage(fr[fr.size() / 2]);
            int goneIndex = cur()->seqIndex;
            closeCurrent(true);                     // Ctrl+Alt+W
            std::vector<int> after = framesOfSeq(sid);
            bool gap = false;                       // the removed seqIndex is absent
            for (int idx : after) if (app.images[idx]->seqIndex == goneIndex) gap = true;
            fprintf(stderr, "verifyselftest: V2 Ctrl+Alt+W frame %d of stack %d: "
                            "%zu frames left, seqs=%zu, imagesRev %llu->%llu\n",
                    goneIndex, sid, after.size(), app.seqs.size(),
                    (unsigned long long)revBefore, (unsigned long long)app.imagesRev);
            check((int)after.size() == want && !gap, "V2 single-frame close leaves N-1 frames");
            check(seqInfo(sid) != nullptr, "V2 stack survives a single-frame close");
            check(app.imagesRev != revBefore, "V2 imagesRev bumped (Files cache)");
        }

        // ---- V3: closing the LAST frame of a stack drops the SeqInfo ----
        reload();
        {
            int sid = app.seqs[1].id;
            while (framesOfSeq(sid).size() > 1) {
                selectImage(framesOfSeq(sid).front());
                closeCurrent(true);
            }
            selectImage(framesOfSeq(sid).front());
            closeCurrent(true);                     // the last one
            fprintf(stderr, "verifyselftest: V3 emptied stack %d one frame at a time: "
                            "seqInfo=%s, frames=%zu\n",
                    sid, seqInfo(sid) ? "STILL THERE" : "gone", framesOfSeq(sid).size());
            check(seqInfo(sid) == nullptr && framesOfSeq(sid).empty(),
                  "V3 last-frame close drops the SeqInfo");
        }

        // ---- V4: compare-B pointing into a closed stack ----
        reload();
        {
            int sid = app.seqs[2].id;
            selectImage(framesOfSeq(app.seqs[0].id).front());
            setCompareB(app.images[framesOfSeq(sid)[1]].get());
            app.compareMode = App::CmpWipe;
            bool had = resolveB() != nullptr;
            closeStack(sid);
            fprintf(stderr, "verifyselftest: V4 closeStack(%d) with B inside: "
                            "hadB=%d uid=%llu name='%s' seq=%d resolveB=%p\n",
                    sid, had ? 1 : 0, (unsigned long long)app.compareBUid,
                    app.compareB.c_str(), app.compareBSeq, (void*)resolveB());
            check(had, "V4 B really pointed into the stack");
            check(app.compareBUid == 0 && app.compareB.empty() && !resolveB(),
                  "V4 closeStack clears a dangling compare-B");
        }
        // ---- V4b: the Ctrl+Alt+W escape hatch and a dangling compare-B.
        // ensureCompareB() re-points B away from cur(), so "close the frame that
        // IS B" cannot be reached from the UI; this pins B by hand to probe
        // whether closeCurrent(frameOnly) does the closeImages() cleanup at all.
        reload();
        {
            int sid = app.seqs[2].id;
            selectImage(framesOfSeq(sid)[1]);
            app.compareBUid = cur()->uid;           // white-box: B == the current frame
            app.compareB = cur()->name;
            app.compareBSeq = cur()->seqIndex;
            app.compareMode = App::CmpWipe;
            std::string bname = app.compareB;
            closeCurrent(true);                     // close exactly the B frame
            std::string leftName = app.compareB;
            // first resolveB() only drops the stale uid and returns null; the
            // SECOND one falls through to the name path, which is why
            // closeImages() clears the name and this path must too
            ImageDoc* b1 = resolveB();
            ImageDoc* b2 = resolveB();
            fprintf(stderr, "verifyselftest: V4b Ctrl+Alt+W on the B frame (B pinned by "
                            "hand): saved '%s', name left behind '%s', "
                            "resolveB #1 -> %s, resolveB #2 -> %s\n",
                    bname.c_str(), leftName.c_str(),
                    b1 ? b1->name.c_str() : "(null)",
                    b2 ? (b2->src->path + " seq " + std::to_string(b2->seqId)).c_str() : "(null)");
            check(leftName.empty(), "V4b frameOnly close clears the compare-B name");
            check(b2 == nullptr, "V4b closed B does not re-latch onto a same-named frame");
        }

        // ---- V5: prune must keep batches referenced only by a queue ----
        reload();
        {
            int b1 = newBatch(uniqueBatchName("qseq"));
            int b2 = newBatch(uniqueBatchName("qrb"));
            int b3 = newBatch(uniqueBatchName("qload"));
            int b4 = newBatch(uniqueBatchName("qnone"));
            App::PendingGroup pg; pg.name = "x"; pg.files.push_back("x.npy"); pg.batchId = b1;
            app.seqQueue.push_back(pg);
            app.rbOpenQueue.push_back({ "", { "y.npy" }, "y", b2 });
            app.loadBatchId = b3;
            pruneEmptyBatches();
            auto live = [&](int id) {
                for (const auto& b : app.batches) if (b.id == id) return true;
                return false;
            };
            fprintf(stderr, "verifyselftest: V5 prune with refs: seqQueue=%d rbOpen=%d "
                            "loadBatchId=%d unreferenced=%d\n",
                    live(b1) ? 1 : 0, live(b2) ? 1 : 0, live(b3) ? 1 : 0, live(b4) ? 1 : 0);
            check(live(b1) && live(b2) && live(b3), "V5 prune keeps queue-referenced batches");
            check(!live(b4), "V5 prune drops the unreferenced batch");
            // ---- V5b: closeBatch removes the queued remote open too ----
            size_t rbBefore = app.rbOpenQueue.size();
            closeBatch(b2);
            fprintf(stderr, "verifyselftest: V5b closeBatch(qrb): rbOpenQueue %zu->%zu, "
                            "batch %s\n", rbBefore, app.rbOpenQueue.size(),
                    live(b2) ? "STILL THERE" : "gone");
            check(app.rbOpenQueue.size() == rbBefore - 1 && !live(b2),
                  "V5b closeBatch purges its rbOpenQueue entry");
            app.seqQueue.clear();
            app.rbOpenQueue.clear();
            app.loadBatchId = 0;
        }

        // ---- V6: move a stack that is still loading ----
        {
            closeAll();
            openFolder(g_verifySelftest);
            if (app.folderPickOpen) pickerAccept();
            int movedSeq = 0, target = 0;
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {    // catch it mid-load
                pumpSequenceAndQueue();
                if (!movedSeq && app.seqRunning && app.seqLoadingId &&
                    framesOfSeq(app.seqLoadingId).size() >= 1 &&
                    framesOfSeq(app.seqLoadingId).size() < 5) {
                    movedSeq = app.seqLoadingId;
                    target = newBatch(uniqueBatchName("midload"));
                    moveStackToBatch(movedSeq, target);
                }
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            int off = 0;
            std::vector<int> fr = movedSeq ? framesOfSeq(movedSeq) : std::vector<int>();
            for (int idx : fr) if (app.images[idx]->batchId != target) off++;
            fprintf(stderr, "verifyselftest: V6 move-mid-load stack %d: %zu frames, "
                            "%d off-batch\n", movedSeq, fr.size(), off);
            check(movedSeq != 0, "V6 caught a stack mid-load");
            check(fr.size() == 5 && off == 0, "V6 late frames follow the moved stack");
        }

        // ---- V7: two same-named batches survive a session round trip apart ----
        reload();
        {
            // create-then-move one at a time: moveStackToBatch prunes, and an
            // empty second batch would be pruned before it could be filled
            int b1 = newBatch(uniqueBatchName("dup"));
            moveStackToBatch(app.seqs[0].id, b1);
            int b2 = newBatch(uniqueBatchName("dup"));
            moveStackToBatch(app.seqs[1].id, b2);
            std::string n1, n2;
            for (const auto& b : app.batches) {
                if (b.id == b1) n1 = b.name;
                if (b.id == b2) n2 = b.name;
            }
            std::error_code vec;
            std::string sess = (std::filesystem::temp_directory_path(vec) /
                                "viewer_verifyselftest.vsession").u8string();
            saveSession(sess, true);
            loadSession(sess);
            loadAll();
            int f1 = 0, f2 = 0, id1 = 0, id2 = 0;
            for (const auto& b : app.batches) {
                if (b.name == n1) id1 = b.id;
                if (b.name == n2) id2 = b.id;
            }
            for (const auto& d : app.images) {
                if (d->batchId == id1) f1++;
                if (d->batchId == id2) f2++;
            }
            fprintf(stderr, "verifyselftest: V7 '%s' + '%s' round trip: both present=%d, "
                            "frames %d / %d, merged=%d\n",
                    n1.c_str(), n2.c_str(), (id1 && id2) ? 1 : 0, f1, f2,
                    (id1 && id1 == id2) ? 1 : 0);
            check(n1 != n2, "V7 uniqueBatchName disambiguates the collision");
            check(id1 && id2 && id1 != id2, "V7 both batches survive the round trip apart");
            std::filesystem::remove(std::filesystem::u8path(sess), vec);
        }

        // ---- V8: the remote IN-FLIGHT drop, not just the queue sweep ----
        {
            closeAll();
            std::string scanroot = g_verifySelftest;
            std::replace(scanroot.begin(), scanroot.end(), '\\', '/');
            size_t sl = scanroot.find_last_of('/');
            scanroot = (sl == std::string::npos ? std::string(".")
                                                : scanroot.substr(0, sl)) + "/rb/scanroot";
            startRemote(rbMain(), "local://" + scanroot);
            double t0 = nowSec();
            bool scanSent = false, closed = false;
            int rsid = 0, queuedForSeq = -1, pendingAfter = -1;
            while (nowSec() - t0 < 120.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                if (rbMain().b.connected && !scanSent) {
                    App::RbJob j;
                    j.kind = App::RbScan;
                    j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = scanroot;
                    rbEnqueue(rbMain(), std::move(j));
                    scanSent = true;
                }
                if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
                // Wait for a moment when the WORKER holds a job: rfPending counts
                // queued + in-flight, so rfPending > (jobs still in rfQueue) means
                // exactly one is out of the queue's reach. That is the only state
                // in which the pumpRemoteFetch drop is load-bearing.
                if (!closed && !app.seqs.empty() && app.rfPending > 0) {
                    int sid2 = app.seqs.back().id, q = 0;
                    {
                        std::lock_guard<std::mutex> lk(app.rfMtx);
                        for (const auto& jj : app.rfQueue) if (jj.seqId == sid2) q++;
                    }
                    if (app.rfPending > q) {        // a job is IN FLIGHT right now
                        rsid = sid2; queuedForSeq = q;
                        closeStack(rsid);
                        pendingAfter = app.rfPending;
                        closed = true;
                        break;
                    }
                }
                if (!rbMain().b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            double t1 = nowSec();
            while (nowSec() - t1 < 3.0) {
                pumpRemoteBrowse(); pumpRemoteFetch();
                pumpRemoteOpenQueue(); pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int orphans = 0;
            for (const auto& d : app.images) if (d->seqId == rsid) orphans++;
            fprintf(stderr, "verifyselftest: V8 closeStack(%d): %d job(s) swept from the "
                            "queue, rfPending after = %d (in flight), %d frame(s) regrew\n",
                    rsid, queuedForSeq, pendingAfter, orphans);
            check(closed, "V8 caught a remote stack with a fetch outstanding");
            check(orphans == 0, "V8 nothing of the closed stack regrew");
            fprintf(stderr, "verifyselftest: V8 note: the in-flight drop path was%s "
                            "the one that had to catch it\n",
                    pendingAfter > 0 ? "" : " NOT");
        }

        // ---- V9: dropPreview must not silently re-target A -------------------
        // The preview sits below app.current whenever anything is opened after
        // it. Erasing by index and clamping only the TAIL leaves app.current
        // naming the document that used to sit one slot higher - and every
        // uid-keyed cache then recomputes for an image the user never chose.
        // openRemote calls dropPreview() and has four failure returns after it,
        // so on a failed open nothing puts it back.
        {
            reload();
            auto mkPreview = [] {                  // a preview at index 0
                auto d = std::make_unique<ImageDoc>();
                d->name = "preview.npy";
                d->src->w = d->src->h = 2; d->src->ch = 1;
                d->src->data.assign(4, 0.f);
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->preview = true;
                app.previewUid = d->uid;
                app.images.insert(app.images.begin(), std::move(d));
                app.imagesRev++;
            };
            mkPreview();
            app.current = 3;                       // neither the preview nor the last
            uint64_t was = cur()->uid;
            std::string wasName = cur()->name;
            dropPreview();
            uint64_t now = cur() ? cur()->uid : 0;
            fprintf(stderr, "verifyselftest: V9 dropPreview with the preview BELOW "
                            "current: A was uid %llu (%s), is now uid %llu (%s)\n",
                    (unsigned long long)was, wasName.c_str(),
                    (unsigned long long)now, cur() ? cur()->name.c_str() : "-");
            check(now == was, "V9 dropPreview keeps A pointing at the same document");
            // ...and dropping the preview WHILE it is current still lands
            // somewhere valid rather than off the end
            mkPreview();
            app.current = 0;
            dropPreview();
            check(app.previewUid == 0 && app.current >= 0 &&
                  app.current < (int)app.images.size(),
                  "V9 dropping the current preview re-points A in range");

            // ...and so does Ctrl+W on it. A preview is an ordinary entry in
            // app.images that happens to be current, so the close path the key
            // takes is closeCurrent, not dropPreview - and the slot is a single
            // uid, so closing the document that occupies it without releasing
            // it leaves the app believing a preview is live that no longer
            // exists. dropPreview() heals it on the next look (it clears the
            // uid after failing to find it), which is exactly why this went
            // unnoticed: nothing between the two ever reads it and says so.
            {
                mkPreview();
                app.current = 0;
                size_t nBefore2 = app.images.size();
                closeCurrent();                     // Ctrl+W, not dropPreview
                fprintf(stderr, "verifyselftest: V9d Ctrl+W on the preview: "
                                "previewUid=%llu, images %zu->%zu\n",
                        (unsigned long long)app.previewUid, nBefore2, app.images.size());
                check(app.previewUid == 0 && app.images.size() == nBefore2 - 1,
                      "V9d closing the preview releases the preview slot");
            }

            // ---- V9c: a clicked path segment must rebuild a path that EXISTS.
            // A NAS reported it: clicking the path errored while ".." kept
            // working, because "//nas/share/x" was split on '/' and rebuilt as
            // "/nas" - a UNC prefix does not survive being read as two folders.
            {
                struct { const char* dir; size_t n; const char* first; const char* last; } CASES[] = {
                    { "//nas/share/2026/dark", 3, "//nas/share", "//nas/share/2026/dark" },
                    { "//nas/share",           1, "//nas/share", "//nas/share" },
                    { "C:/capt/run42",         3, "C:/",         "C:/capt/run42" },
                    { "/data/run42",           3, "/",           "/data/run42" },
                    { "~/capt",                2, "~",           "~/capt" },
                };
                bool segOk = true;
                for (const auto& c : CASES) {
                    std::vector<PathSeg> v = pathSegments(c.dir);
                    std::string f = v.empty() ? "<none>" : v.front().path;
                    std::string l = v.empty() ? "<none>" : v.back().path;
                    fprintf(stderr, "verifyselftest: V9c \"%s\" -> %zu seg(s), first \"%s\", "
                                    "last \"%s\"\n", c.dir, v.size(), f.c_str(), l.c_str());
                    if (v.size() != c.n || f != c.first || l != c.last) segOk = false;
                }
                check(segOk, "V9c every path segment rebuilds a path that exists");
            }

            // ...and SAVING a session must not turn the look into an open.
            // The autosave fires a few seconds after any change and went
            // through saveSession, which promoted every preview first - so a
            // frame the user had merely clicked appeared under the batch on
            // its own, and a double-click then opened the stack NEXT TO it
            // ("stack と一枚がそれぞれ登録される").
            mkPreview();
            for (auto& d : app.images)
                if (d->uid == app.previewUid) d->src->path = "pv-scratch/preview.npy";
            uint64_t pvUid = app.previewUid;
            size_t nBefore = app.images.size();
            std::error_code sec;
            std::filesystem::path sdir = vfyScratch("vfy-session-scratch");
            std::string spath = (sdir / "s.vsession").u8string();
            saveSession(spath, true);
            bool stillPreview = false;
            for (const auto& d : app.images)
                if (d->uid == pvUid) stillPreview = d->preview;
            bool named = false;
            {
                std::ifstream in(pathFromUtf8(spath), std::ios::binary);
                for (std::string ln; std::getline(in, ln); )
                    if (ln.find("preview.npy") != std::string::npos) named = true;
            }
            fprintf(stderr, "verifyselftest: V9b autosave with a live preview: "
                            "still a preview=%d, images %zu->%zu, named in file=%d\n",
                    (int)stillPreview, nBefore, app.images.size(), (int)named);
            check(stillPreview, "V9b saving a session leaves the preview a preview");
            check(app.images.size() == nBefore, "V9b saving a session opens nothing");
            check(!named, "V9b the preview is not written into the session file");
            std::filesystem::remove_all(sdir, sec);
            dropPreview();
        }

        // ---- V10: CFA planes are never mixed in a temporal statistic --------
        // docs/terminology.md's iron rule. The fixture is exact on purpose: a
        // Bayer stack in which every plane is perfectly flat and perfectly
        // stable except for a +-10 DN alternation, and the four planes sit at
        // 100 / 400 / 400 / 900 DN. Per plane the truth is sigma_fpn = 0 and a
        // sigma_t that is the +-10 DN swing read with the ddof the canon picked
        // (see sigTExpect below). Pooled, sigma_fpn becomes the spatial spread of the
        // four LEVELS - sqrt(285000 - 450^2) = 287.228 DN of pure fabrication,
        // reported as fixed-pattern noise on a flat field.
        {
            closeAll();
            const int W = 32, H = 32, NF = 4;
            const double LVL[4] = { 100, 400, 400, 900 };
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = "cfa fixture";
            app.seqs.push_back(si);
            for (int f = 0; f < NF; f++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = "cfa_" + std::to_string(f) + ".npy";
                d->src->w = W; d->src->h = H; d->src->ch = 1;
                d->cfa = 1; d->cfaPattern = 0;              // RGGB
                d->src->dtype = "f32";
                d->src->data.resize((size_t)W * H);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        d->px()[(size_t)y * W + x] =
                            (float)(LVL[CFA_MAP[0][(y & 1) * 2 + (x & 1)]] + (f % 2 ? 10 : -10));
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->seqId = si.id;
                d->seqIndex = f;
                app.images.push_back(std::move(d));
            }
            app.current = 0;
            app.selectedAnn = 0;
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            const App::TemporalState& T = app.temporal[0];
            // What sigma_t must be here, DERIVED rather than remembered. Each
            // pixel's four values are L-10, L+10, L-10, L+10: mean L, sum of
            // squared deviations 4 x 100 = 400. ddof=1 (#57 item 3, and what
            // serve.cpp always did) divides by NF-1, so sigma_t =
            // sqrt(400/3) = 10 x sqrt(4/3) = 11.5470054. The 10.0 that stood
            // here was sqrt(400/4) - the ddof=0 answer, i.e. this square wave's
            // POPULATION sigma. Both describe the same +-10 DN alternation;
            // which one the panel prints is the estimator question #57 settled,
            // and the fixture's amplitude is still the 10 DN the comment says.
            // Written as an expression so the next reader can see where the
            // divisor comes from instead of trusting five digits.
            const double sigTExpect = 10.0 * sqrt((double)NF / (double)(NF - 1));
            double worstFpn = 0, worstSigT = 0;
            for (int p = 0; p < T.nPl; p++) {
                worstFpn = std::max(worstFpn, fabs(T.fixedPattern[p]));
                worstSigT = std::max(worstSigT, fabs(T.tempNoise[p] - sigTExpect));
            }
            fprintf(stderr, "verifyselftest: V10 Bayer fixture (levels 100/400/400/900, "
                            "+-10 DN in time over %d frames): nPl=%d, sigma_t "
                            "%.6g/%.6g/%.6g/%.6g (expected %.9g = 10 x sqrt(%d/%d)), "
                            "sigma_fpn %.6g/%.6g/%.6g/%.6g\n",
                    NF, T.nPl, T.tempNoise[0], T.tempNoise[1], T.tempNoise[2], T.tempNoise[3],
                    sigTExpect, NF, NF - 1,
                    T.fixedPattern[0], T.fixedPattern[1], T.fixedPattern[2], T.fixedPattern[3]);
            check(T.valid && T.nPl == 4, "V10 a mosaiced stack reports four planes");
            check(worstFpn < 1e-6, "V10 each plane's FPN is 0 on a per-plane-flat field");
            // ...and since #57 judgment 2 that 0 is a CLAMP, which has to say so.
            // Per plane the mean image is exactly constant, so var_spatial = 0,
            // while the correction is C = mean(s_t,i^2/n_i) = (400/3)/4 = 100/3:
            // the subtraction goes to -33.33 and the value clamps. A bare 0 here
            // would read as "this sensor has no fixed pattern" when what the
            // fixture says is "a 4-frame stack of +-10 DN cannot see one".
            // fpn_corr is reported in DN, so sqrt(100/3) = 5.77350269.
            {
                const double corrExpect = sqrt((4.0 * 10.0 * 10.0 / (NF - 1.0)) / NF);
                bool allClamped = true, corrOk = true;
                for (int p = 0; p < T.nPl; p++) {
                    allClamped &= T.fpnClamped[p];
                    corrOk &= fabs(T.fpnCorr[p] - corrExpect) < 1e-9;
                }
                fprintf(stderr, "verifyselftest: V10 clamp: clamped %d/%d/%d/%d, "
                                "fpn_corr %.9g (expected %.9g = sqrt((4x10^2/%d)/%d))\n",
                        (int)T.fpnClamped[0], (int)T.fpnClamped[1], (int)T.fpnClamped[2],
                        (int)T.fpnClamped[3], T.fpnCorr[0], corrExpect, NF - 1, NF);
                check(allClamped, "V10 that 0 is DECLARED a clamp, not printed silently");
                check(corrOk, "V10 the amount subtracted is reported, in DN");
            }
            check(worstSigT < 1e-6,
                  "V10 each plane's sigma_t is the +-10 DN alternation, ddof=1");
            // ...and the same pixels read as gray give the pooled number the
            // panel used to print, which is the inter-plane level spread and
            // nothing else. This also proves the mosaic is part of the cache
            // key: without it the stale four-plane answer would come back.
            for (auto& d : app.images) if (d->seqId == si.id) d->cfa = 0;
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            double pooled = app.temporal[0].fixedPattern[0];
            // DERIVED, because #57 judgment 2 moved it and a five-digit constant
            // cannot say why. n = 32x32 = 1024 samples in one plane; the mean
            // image takes the four levels 256 times each, so the ddof=0 spatial
            // variance is 285000 - 450^2 = 82500 and the ddof=1 form the settled
            // estimator asks for is 82500 x 1024/1023 = 82580.6452. Off that
            // comes C = 100/3, giving sqrt(82547.3118) = 287.310480.
            // The 287.228014 that stood here was sqrt(82500) - the same level
            // spread with neither the ddof nor the correction. Both are the
            // inter-plane spread this assert is about; which one the panel
            // prints is what #57 settled, and the fabrication the assert names
            // is 287 DN either way.
            const double pooledVar0 = 285000.0 - 450.0 * 450.0;      // 82500
            const double pooledExpect =
                sqrt(pooledVar0 * (1024.0 / 1023.0) - (4.0 * 10.0 * 10.0 / (NF - 1.0)) / NF);
            fprintf(stderr, "verifyselftest: V10 the same pixels as gray: nPl=%d, "
                            "sigma_fpn %.9g (the level spread, expected %.9g), "
                            "clamped=%d\n",
                    app.temporal[0].nPl, pooled, pooledExpect,
                    (int)app.temporal[0].fpnClamped[0]);
            check(app.temporal[0].nPl == 1 && fabs(pooled - pooledExpect) < 1e-6,
                  "V10 the mosaic is part of the cache key");
            check(!app.temporal[0].fpnClamped[0],
                  "V10 a 287 DN spread is far above the residual: no clamp");
            closeAll();
        }

        {   // ---- V27: sigma_fpn is the CORRECTED quantity (#57 judgment 2) --
            // docs/flat-field-stats.md (b) pins the estimator:
            //
            //     C         = mean_i(s_t,i^2 / n_i)
            //     sigma_fpn = sqrt(max(0, var_spatial(M) - C))      ... var ddof=1
            //
            // and requires the amount and any clamp to be declared. Two
            // synthetic stacks with a KNOWN fixed pattern and a KNOWN temporal
            // noise, both exact in float32 so the expected value is arithmetic
            // and not a fit:
            //
            //   pixels are L +- B by column (the fixed pattern) and +- A by
            //   frame (the temporal noise), NF = 4 frames alternating.
            //   Per pixel the four values are L+-B+-A, so the temporal mean is
            //   L+-B EXACTLY and the sum of squared deviations is 4A^2:
            //       s_t,i^2 = 4A^2/(NF-1)  (ddof = n_i-1, #57 item 3)
            //       C       = s_t,i^2 / NF                (every n_i = NF here)
            //   Half the pixels sit at L+B and half at L-B, so
            //       var_spatial,ddof0(M) = B^2,  var_spatial,ddof1 = B^2 n/(n-1)
            //   with n = 32x32 = 1024 samples in the single plane.
            //
            // FIXTURE 1 (B=40, A=30): var = 1601.564027, C = 300, so
            //   sigma_fpn = sqrt(1301.564027) = 36.0771954 DN, against a TRUE
            //   fixed pattern of 40 DN measured over only 4 frames. The
            //   uncorrected number this used to print is 40 exactly (sqrt(B^2)):
            //   the correction is a 10% move on a fixture designed so both
            //   numbers are exact, which is what makes red-vs-green readable.
            // FIXTURE 2 (B=5, A=30): var = 25.024438 < C = 300, so the
            //   subtraction goes negative: the value CLAMPS at 0 and says so.
            //   Uncorrected this printed 5 DN of "fixed pattern" that is
            //   entirely the leftover of a 30 DN temporal swing averaged 4 ways.
            struct FpnCase { double B, A; bool clamp; const char* what; };
            const FpnCase CASES[2] = {
                { 40.0, 30.0, false, "V27 sigma_fpn = sqrt(var_spatial,ddof1 - C), derived" },
                {  5.0, 30.0, true,  "V27 ...and clamps at 0 when C exceeds the spread" },
            };
            for (const FpnCase& fc : CASES) {
                closeAll();
                const int W = 32, H = 32, NF = 4;
                const double L = 1000.0;
                App::SeqInfo si;
                si.id = app.nextSeqId++;
                si.name = "fpn fixture";
                app.seqs.push_back(si);
                for (int f = 0; f < NF; f++) {
                    auto d = std::make_unique<ImageDoc>();
                    d->name = "fpn_" + std::to_string(f) + ".npy";
                    d->src->w = W; d->src->h = H; d->src->ch = 1;
                    d->src->dtype = "f32";
                    d->src->data.resize((size_t)W * H);
                    for (int y = 0; y < H; y++)
                        for (int x = 0; x < W; x++)
                            d->px()[(size_t)y * W + x] =
                                (float)(L + (x & 1 ? -fc.B : fc.B) + (f % 2 ? fc.A : -fc.A));
                    d->syncMirrors();
                    d->uid = app.nextUid++;
                    d->seqId = si.id;
                    d->seqIndex = f;
                    app.images.push_back(std::move(d));
                }
                app.current = 0;
                app.selectedAnn = 0;
                recomputeTemporalIfNeeded(cur(), app.temporal[0]);
                const App::TemporalState& T2 = app.temporal[0];
                // the derivation, as expressions, next to the assert that uses it
                const double n = (double)W * H;                       // 1024 samples, 1 plane
                const double stVar = 4.0 * fc.A * fc.A / (NF - 1.0);  // s_t,i^2, ddof = n_i-1
                const double corr  = stVar / NF;                      // C = mean(s_t,i^2 / n_i)
                const double sVar  = fc.B * fc.B * n / (n - 1.0);     // var_spatial(M), ddof=1
                const double fVar  = sVar - corr;
                const double fpnExpect  = fVar > 0 ? sqrt(fVar) : 0.0;
                const double corrExpect = sqrt(corr);
                const double sigTExpect = sqrt(stVar);
                const double totExpect  = sqrt(stVar + std::max(0.0, fVar));
                fprintf(stderr, "verifyselftest: V27 B=%.4g A=%.4g NF=%d n=%.0f: "
                                "sigma_t %.9g (exp %.9g)  sigma_fpn %.9g (exp %.9g; "
                                "uncorrected would be %.9g)  fpn_corr %.9g (exp %.9g)  "
                                "clamped %d (exp %d)  sigma_tot %.9g (exp %.9g)\n",
                        fc.B, fc.A, NF, n, T2.tempNoise[0], sigTExpect,
                        T2.fixedPattern[0], fpnExpect, fc.B,
                        T2.fpnCorr[0], corrExpect,
                        (int)T2.fpnClamped[0], (int)fc.clamp,
                        T2.totalNoise[0], totExpect);
                check(T2.valid && T2.nPl == 1 && T2.frames == NF,
                      "V27 fixture: one plane, all frames");
                check(fabs(T2.tempNoise[0] - sigTExpect) < 1e-9,
                      "V27 sigma_t is the +-A swing at ddof = n_i-1");
                check(fabs(T2.fixedPattern[0] - fpnExpect) < 1e-9, fc.what);
                check(fabs(T2.fpnCorr[0] - corrExpect) < 1e-9,
                      "V27 the correction AMOUNT is reported, in DN");
                check(T2.fpnClamped[0] == fc.clamp,
                      "V27 the clamp is declared exactly when it fired");
                check(fabs(T2.totalNoise[0] - totExpect) < 1e-9,
                      "V27 sigma_tot is the quadrature sum of the CORRECTED pair");
                // The export tells readers the uncorrected upper bound is
                // recoverable per row as sqrt(sigma_fpn^2 + fpn_corr^2), so that
                // claim is checked here rather than merely written. It holds
                // only where nothing was clamped - a clamp DISCARDS the negative
                // remainder by definition, which is precisely why it has to be
                // declared instead of silently reported as 0.
                if (!fc.clamp)
                    check(fabs(sqrt(T2.fixedPattern[0] * T2.fixedPattern[0] +
                                    T2.fpnCorr[0] * T2.fpnCorr[0]) - sqrt(sVar)) < 1e-9,
                          "V27 sqrt(sigma_fpn^2 + fpn_corr^2) recovers the uncorrected bound");
                closeAll();
            }
        }

        {   // ---- V15: a folder stack's user-given name survives a session ---
            // The save side deliberately writes seqname AFTER seqload, but
            // seqload only QUEUES the rescan, so at parse time cur()->seqId is
            // still 0 and the reader dropped the rename with no message: the
            // stack came back under its folder-derived pattern. terminology.md
            // says stack renames are session-saved.
            reload();
            int sid = app.seqs.front().id;
            std::string given = "25C dark";
            seqInfo(sid)->name = given;                    // F2 rename
            std::error_code vec2;
            std::string sess2 = (std::filesystem::temp_directory_path(vec2) /
                                 "viewer_seqname.vsession").u8string();
            saveSession(sess2, true);
            std::string lerr2 = loadSession(sess2);
            loadAll();
            double tn = nowSec();
            while (nowSec() - tn < 60.0 &&
                   (!app.seqRestore.empty() || app.seqRunning || seqReadyPending())) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            loadAll();
            int found = 0;
            std::string names;
            for (const auto& si : app.seqs) {
                if (si.name == given) found++;
                if (names.size() < 120) names += (names.empty() ? "" : ", ") + si.name;
            }
            fprintf(stderr, "verifyselftest: V15 renamed a folder stack '%s', saved and "
                            "reloaded: %d stack(s) [%s]%s\n",
                    given.c_str(), (int)app.seqs.size(), names.c_str(),
                    lerr2.empty() ? "" : (" load err: " + lerr2).c_str());
            check(lerr2.empty() && found == 1,
                  "V15 seqname survives the restore of a folder stack");
            std::filesystem::remove(std::filesystem::u8path(sess2), vec2);
        }

        {   // ---- V14: the memory budget counts what is COMMITTED ------------
            // residentImageBytes() sums app.images only, so frames already
            // enqueued and in transfer counted as zero. Two browser opens a
            // second apart each computed the same free room and each committed
            // a full budget's worth. rbOpenQueue serialises the scan path for
            // exactly this reason; every direct open bypasses it.
            closeAll();
            std::string scanroot2 = g_verifySelftest;
            std::replace(scanroot2.begin(), scanroot2.end(), '\\', '/');
            size_t sl2 = scanroot2.find_last_of('/');
            scanroot2 = (sl2 == std::string::npos ? std::string(".")
                                                  : scanroot2.substr(0, sl2)) + "/rb/scanroot";
            rbMain().b = App::RemoteBrowse{};
            startRemote(rbMain(), "local://" + scanroot2);
            double t0b = nowSec();
            while (nowSec() - t0b < 120.0) {
                pumpRemoteBrowse();
                if (rbMain().b.connected && !rbMain().b.entries.empty()) break;
                if (!rbMain().b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<std::string> f10;
            for (const auto& e : rbMain().b.entries)
                if (e.dir && e.name == "10lx") {
                    // list it, then take its frames
                    remoteBrowseTo(rbMain(), scanroot2 + "/10lx");
                    double t1b = nowSec();
                    while (nowSec() - t1b < 120.0) {
                        pumpRemoteBrowse();
                        if (rbMain().b.dir == scanroot2 + "/10lx") break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    for (const auto& e2 : rbMain().b.entries)
                        if (e2.group)
                            for (const auto& m : e2.members)
                                f10.push_back(rbMain().b.dir + "/" + m);
                    break;
                }
            if (f10.size() < 3) {
                fprintf(stderr, "verifyselftest: V14 no remote stack under %s\n",
                        scanroot2.c_str());
                ok = false;
            } else {
                size_t res0 = residentImageBytes(), claim0 = claimedImageBytes();
                openRemoteStack("", f10, "10lx/x", 0);        // no pumping: all in flight
                size_t res1 = residentImageBytes(), claim1 = claimedImageBytes();
                bool budgetOk = claim1 > res1 && app.rfBytesInFlight > 0 &&
                                claim0 == res0;
                fprintf(stderr, "verifyselftest: V14 after one stack open with nothing "
                                "pumped: resident %zu -> %zu B, claimed %zu -> %zu B, "
                                "in flight %zu B (%d job(s))\n",
                        res0, res1, claim0, claim1, app.rfBytesInFlight.load(),
                        app.rfPending.load());
                check(budgetOk, "V14 committed-but-unlanded bytes count against the budget");
                // ...and they are given back as the frames land
                double t2b = nowSec();
                while (nowSec() - t2b < 60.0 && app.rfPending > 0) {
                    pumpRemoteFetch();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                fprintf(stderr, "verifyselftest: V14 after draining: in flight %zu B, "
                                "claimed == resident: %d\n",
                        app.rfBytesInFlight.load(),
                        claimedImageBytes() == residentImageBytes() ? 1 : 0);
                check(app.rfBytesInFlight == 0 &&
                      claimedImageBytes() == residentImageBytes(),
                      "V14 the commitment is released when the frame lands");
            }
            stopRbWorker();
            for (auto& bp : app.browsePanels) bp->stop = false;
            closeAll();
        }

        {   // ---- V13: a non-finite sample is EXCLUDED, not folded into N ---
            // The fixture is exact: a perfectly constant 100 DN stack of four
            // frames with ONE pixel NaN in two of them. The truth is sigma_t =
            // 0 and sigma_fpn = 0 - that pixel simply has two valid frames.
            // Skipping the NaN in the accumulator while keeping N = 4 as the
            // divisor makes that pixel read 50 DN with a variance of 2500, and
            // pooling gives sigma_t = sigma_fpn = 1.5625 DN out of nowhere.
            closeAll();
            const int W = 32, H = 32, NF = 4;
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = "nan fixture";
            si.expectedFrames = NF;
            app.seqs.push_back(si);
            for (int f = 0; f < NF; f++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = "nan_" + std::to_string(f) + ".npy";
                d->src->w = W; d->src->h = H; d->src->ch = 1;
                d->src->dtype = "f32";
                d->src->data.assign((size_t)W * H, 100.0f);
                if (f < 2) d->px()[0] = std::numeric_limits<float>::quiet_NaN();
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->seqId = si.id;
                d->seqIndex = f;
                app.images.push_back(std::move(d));
            }
            app.current = 0;
            app.selectedAnn = 0;
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            const App::TemporalState& T = app.temporal[0];
            StackStats st = computeStackStats(si.id, 0, 0, W, H);
            fprintf(stderr, "verifyselftest: V13 constant-100 stack with 1 pixel NaN in "
                            "2 of 4 frames: local temporal sigma_t=%.6g sigma_fpn=%.6g "
                            "(excluded %llu); computeStackStats mean=%.6g sigma_t=%.6g "
                            "(excluded %llu, dropped %zu)\n",
                    T.tempNoise[0], T.fixedPattern[0], (unsigned long long)T.nonFinite,
                    st.mean[0], st.sigmaT[0], (unsigned long long)st.nonFinite, st.dropped);
            check(T.valid && T.nonFinite == 2 && fabs(T.tempNoise[0]) < 1e-9 &&
                  fabs(T.fixedPattern[0]) < 1e-9,
                  "V13 the Temporal panel excludes NaN instead of folding it in");
            check(st.valid && st.nonFinite == 2 && st.dropped == 0 &&
                  fabs(st.mean[0] - 100.0) < 1e-9 && fabs(st.sigmaT[0]) < 1e-9,
                  "V13 computeStackStats agrees, and reports what it excluded");
            closeAll();
        }

        {   // ---- V12: a LOCAL stack knows how many frames it expects -------
            // docs/terminology.md requires a partially loaded stack's
            // aggregates to state "how many of how many". expectedFrames was
            // populated only on the three REMOTE open paths, so for a local
            // folder stack it stayed 0: `expected > frames` was false, and the
            // A/B temporal header printed "A (n=5/5)" - complete - while the
            // loader was still decoding frames 6..24.
            reload();
            int lack = 0, mismatch = 0;
            for (const auto& si : app.seqs) {
                if (si.expectedFrames <= 0) lack++;
                else if (si.expectedFrames != (int)framesOfSeq(si.id).size()) mismatch++;
            }
            selectImage(framesOfSeq(app.seqs.front().id).front());
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            AbTemporal TA = abTemporalOf(cur(), app.temporal[0], app.srvTemporal);
            fprintf(stderr, "verifyselftest: V12 %d local stack(s): %d without an "
                            "expected count, %d disagreeing with what is resident; "
                            "A/B header would read n=%d/%d\n",
                    (int)app.seqs.size(), lack, mismatch, TA.frames, TA.expected);
            check(lack == 0 && mismatch == 0,
                  "V12 a local folder stack records its expected frame count");
            check(TA.expected == TA.frames && TA.expected > 0,
                  "V12 a fully loaded stack states n=N/N truthfully");
        }

        {   // ---- V11: a pixel value is [DN] whatever its storage dtype -----
            // c418e31 settled this for the histogram, projection and temporal
            // axes; the Analysis grid's unit column was the site it did not
            // reach, and it exports through "Copy table (TSV)".
            struct { const char* key; const char* dt; const char* want; } U[] = {
                { "ch0.mean", "f32", "DN" }, { "ch0.std", "f32", "DN" },
                { "ch0.var",  "f32", "DN^2" }, { "R.p99", "f64", "DN" },
                { "ch0.mean", "u16", "DN" }, { "ch0.var", "u16", "DN^2" },
                { "noise/floor.snr_db", "f32", "dB" },      // named units still win
                { "prnu_pct", "f32", "%" }, { "pixels", "f32", "px" },
                { "ch0.entropy", "f32", "bit" },
            };
            bool uOk = true;
            std::string got;
            for (const auto& u : U) {
                std::string g = unitForAnalysisKey(u.key, u.dt);
                got += (got.empty() ? "" : ", ") + std::string(u.key) + "/" + u.dt +
                       "=" + g;
                if (g != u.want) uOk = false;
            }
            fprintf(stderr, "verifyselftest: V11 analysis units: %s\n", got.c_str());
            check(uOk, "V11 the Analysis grid quotes pixel values in DN");
        }

        {   // ---- V17: the pooled "all" row is a different measurement -------
            // It must pool the SAME samples the plane rows saw - not re-scan,
            // not sample differently - or the row that is supposed to be the
            // whole picture disagrees with the parts it is made of. And it must
            // never be counted as a plane: nSeries is the plane count, allRow
            // is a separate flag, because a fifth "plane" is exactly the
            // reading the CFA split exists to prevent.
            ImageDoc* im0 = cur();
            if (im0 && im0->w > 1) {
                int savedCfa = im0->cfa, savedPat = im0->cfaPattern;
                bool savedAll = app.projAllRow;
                im0->cfa = 1; im0->cfaPattern = 0;          // treat it as RGGB
                app.selectedAnn = 0;                        // whole image
                app.projAllRow = false;
                App::ProjState T{};
                recomputeProjectionIfNeeded(im0, T);
                int planesOff = T.nSeries;
                bool offClean = !T.allRow;
                app.projAllRow = true;
                App::ProjState T2{};
                recomputeProjectionIfNeeded(im0, T2);
                fprintf(stderr, "verifyselftest: V17 planes=%d allRow=%d (off: planes=%d allRow=%d)\n",
                        T2.nSeries, T2.allRow ? 1 : 0, planesOff, T.allRow ? 1 : 0);
                check(offClean && planesOff == 4, "V17 four planes, and no pooled row unless asked");
                // ...and all four actually have SAMPLES. The sample stride used
                // to be forced even for CFA, so the H pass read only even rows
                // and the V pass only even columns: for RGGB the B plane got
                // nothing at all and its row vanished. The first version of
                // this test could not see that - it skipped planes whose stats
                // were invalid, which is exactly the ones the bug emptied.
                bool allPlanes = true;
                std::string missing;
                for (int i = 0; i < 4; i++) {
                    const bool ok3 = T2.fStat[i].valid && T2.hStat[i].valid && T2.vStat[i].valid;
                    if (!ok3) { allPlanes = false; missing += T2.seriesNames[i]; missing += " "; }
                }
                fprintf(stderr, "verifyselftest: V17 planes with region+H+V samples: %s\n",
                        allPlanes ? "all four" : ("MISSING " + missing).c_str());
                check(allPlanes, "V17 every CFA plane is sampled by both passes");
                check(T2.nSeries == 4 && T2.allRow,
                      "V17 the pooled row does not become a fifth plane");
                // the pooled mean is the sample-weighted mean of the planes
                double num = 0; size_t den = 0;
                bool allValid = T2.fStat[4].valid;
                for (int i = 0; i < 4; i++) {
                    if (!T2.fStat[i].valid) continue;
                    num += T2.fStat[i].mean * (double)T2.h[i].size();
                    den += T2.h[i].size();
                }
                double want = den ? num / den : 0;
                double got = T2.fStat[4].mean;
                fprintf(stderr, "verifyselftest: V17 pooled mean %.9g, planes combined %.9g\n",
                        got, want);
                check(allValid && fabs(got - want) < fabs(want) * 1e-3 + 1e-6,
                      "V17 the pooled row is the planes pooled, not a second scan");
                // ...and the pooled spread obeys the law of total variance:
                // pooled var = mean of the plane variances + variance of the
                // plane means. (NOT ">= the widest plane": pooling takes the
                // MEAN of the variances, so one very noisy plane among quiet
                // ones legitimately pools narrower than itself.)
                double mv = 0, mm = 0; int nv = 0;
                for (int i = 0; i < 4; i++)
                    if (T2.fStat[i].valid) { mv += T2.fStat[i].sd * T2.fStat[i].sd;
                                             mm += T2.fStat[i].mean; nv++; }
                if (nv) { mv /= nv; mm /= nv; }
                double between = 0;
                for (int i = 0; i < 4; i++)
                    if (T2.fStat[i].valid)
                        between += (T2.fStat[i].mean - mm) * (T2.fStat[i].mean - mm);
                if (nv) between /= nv;
                double wantVar = mv + between, gotVar = T2.fStat[4].sd * T2.fStat[4].sd;
                fprintf(stderr, "verifyselftest: V17 pooled var %.9g, within %.9g + between %.9g = %.9g\n",
                        gotVar, mv, between, wantVar);
                check(fabs(gotVar - wantVar) < wantVar * 1e-3 + 1e-9,
                      "V17 pooled variance = within-plane + between-plane");
                im0->cfa = savedCfa; im0->cfaPattern = savedPat;
                app.projAllRow = savedAll;
            }
        }

        // ---- V16: the raw recipe belongs to the Open that answered for it ------
        // The queue spans Opens now (a second Open Folder while the first drains
        // APPENDS instead of replacing), but g_folderRecipe was process-global
        // with three touch points. Opening raw folder B while raw folder A
        // drained reset folderRecipeValid - so A's dialog came back, showing an
        // A file, to a user who had just opened B - and the answer was then
        // applied to every remaining raw group of BOTH folders. Two folders with
        // different geometry cannot both be right.
        {
            closeAll();
            app.seqQueue.clear();
            app.folderRecipeValid = false;
            rawDlg.open = false;
            rawDlg.forQueue = false;
            std::error_code rec;
            std::filesystem::path rroot = std::filesystem::temp_directory_path(rec) /
                                          "viewer_rawrecipe";
            std::filesystem::remove_all(rroot, rec);
            std::filesystem::create_directories(rroot / "A", rec);
            std::filesystem::create_directories(rroot / "B", rec);
            // A: 32x32 u8 = 1024 bytes per file. B: 64x64 u8 = 4096 - a real
            // geometry difference, and B's files are big enough that A's recipe
            // would decode them without complaining.
            auto mkraw = [](const std::filesystem::path& p, size_t n) {
                std::ofstream o(p, std::ios::binary);
                std::vector<char> buf(n, 7);
                o.write(buf.data(), (std::streamsize)n);
            };
            for (int i = 0; i < 2; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "a_%03d.raw", i);
                mkraw(rroot / "A" / nm, 32 * 32);
                snprintf(nm, sizeof nm, "b_%03d.raw", i);
                mkraw(rroot / "B" / nm, 64 * 64);
            }
            auto oneGroup = [](const std::string& file, const std::string& name) {
                App::PendingGroup g;
                g.files.push_back(file);
                g.name = name;
                g.isRaw = true;
                return g;
            };
            std::vector<App::PendingGroup> ga;
            ga.push_back(oneGroup((rroot / "A" / "a_000.raw").u8string(), "A/a0"));
            ga.push_back(oneGroup((rroot / "A" / "a_001.raw").u8string(), "A/a1"));
            enqueueGroups(std::move(ga));      // raises the dialog for A's first
            int openA = app.seqQueue.empty() ? 0 : app.seqQueue.front().openId;
            bool askedA = rawDlg.open && rawDlg.forQueue;
            // the user answers it: exactly what the dialog's Load does
            g_folderRecipe = rawDlg;
            g_folderRecipe.w = 32; g_folderRecipe.h = 32;
            g_folderRecipe.dtype = RD_U8; g_folderRecipe.interp = RI_GRAY;
            g_folderRecipe.offset = 0;
            g_folderRecipe.forQueue = false;
            g_folderRecipe.replaceIdx = -1;
            g_folderRecipe.cropOn = false;
            app.folderRecipeValid = true;
            app.folderRecipeOpenId = openA;
            rawDlg.open = false; rawDlg.forQueue = false;
            std::vector<App::PendingGroup> gb;  // ...and NOW a second Open
            gb.push_back(oneGroup((rroot / "B" / "b_000.raw").u8string(), "B/b0"));
            enqueueGroups(std::move(gb));
            bool keptA = app.folderRecipeValid && app.folderRecipeOpenId == openA;
            int qBefore = (int)app.seqQueue.size();
            startNextQueuedGroup();             // A's second group: uses A's recipe
            bool aDecoded = !app.images.empty() && app.images.back()->w == 32 &&
                            app.images.back()->h == 32 && !rawDlg.open;
            startNextQueuedGroup();             // B's group: must ASK, not assume
            bool askedB = rawDlg.open && rawDlg.forQueue;
            bool noGarbage = true;              // nothing decoded with A's geometry
            for (const auto& d : app.images)
                if (d->src->path.find("b_") != std::string::npos) noGarbage = false;
            fprintf(stderr, "verifyselftest: V16 raw recipe: asked for A=%d, kept over the "
                            "second Open=%d, A's 2nd stack %dx%d, asked again for B=%d, "
                            "queue %d\n", askedA ? 1 : 0, keptA ? 1 : 0,
                    app.images.empty() ? 0 : app.images.back()->w,
                    app.images.empty() ? 0 : app.images.back()->h, askedB ? 1 : 0, qBefore);
            check(askedA, "V16 a raw queue asks for a recipe once");
            check(keptA, "V16 a second Open does not cancel the first's answered recipe");
            check(aDecoded, "V16 ...which still applies to the rest of ITS OWN Open");
            check(askedB, "V16 the other Open's raw group asks for its own recipe");
            check(noGarbage, "V16 ...and nothing of it was decoded with the wrong geometry");
            // Cancel takes only the Open that asked with it.
            int bOpen = app.seqQueue.empty() ? 0 : app.seqQueue.front().openId;
            std::vector<App::PendingGroup> gc;
            gc.push_back(oneGroup((rroot / "A" / "a_000.raw").u8string(), "C/c0"));
            enqueueGroups(std::move(gc));       // a THIRD Open queues behind B
            int qAll = (int)app.seqQueue.size();
            rawDlg.forQueue = true;
            {   // the Cancel branch of drawRawDialog, verbatim
                rawDlg.forQueue = false;
                int oid = app.seqQueue.empty() ? 0 : app.seqQueue.front().openId;
                int dropped = 0;
                for (auto it = app.seqQueue.begin(); it != app.seqQueue.end();)
                    if (it->openId == oid) { it = app.seqQueue.erase(it); dropped++; }
                    else ++it;
                fprintf(stderr, "verifyselftest: V16 Cancel dropped %d of %d queued "
                                "group(s), %d left\n", dropped, qAll,
                        (int)app.seqQueue.size());
            }
            bool othersKept = !app.seqQueue.empty();
            for (const auto& q : app.seqQueue) if (q.openId == bOpen) othersKept = false;
            check(othersKept, "V16 Cancel drops only the asking Open's queued stacks");
            app.seqQueue.clear();
            app.folderRecipeValid = false;
            rawDlg.open = false; rawDlg.forQueue = false;
            std::filesystem::remove_all(rroot, rec);
        }

        {   // ---- V18: histogram and ROI stats sample every CFA plane --------
            // Same family as V17's projection defect, different mechanism.
            // Both loops strided a LINEAR pixel index (x = rx + p % rw): once
            // step and rw are both even - a sensor width essentially always is
            // - p stays even, x stays even, and no odd column is ever read.
            // For RGGB that empties Gr and B while the surviving planes keep
            // printing normal numbers, so nothing on screen says half the
            // sensor is missing. Assert every plane has samples and the counts
            // are close, at sizes where the stride exceeds 1. V17's lesson
            // kept: never skip planes with invalid stats - the bug empties
            // exactly those.
            app.selectedAnn = 0;                          // whole image
            auto planeN = [](const App::HistState& HS, int s) {
                size_t nn = 0;
                for (int b = 0; b < 256; b++) nn += HS.bins[s][b];
                return nn;
            };
            auto mkCfa = [](int W2, int H2, int mode) {   // mode: 1=Bayer 2=Quad
                auto d = std::make_unique<ImageDoc>();
                d->name = "v18"; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
                d->cfa = mode; d->cfaPattern = 0; d->src->dtype = "f32";
                static const float LVL[4] = { 100, 300, 500, 900 };
                d->src->data.resize((size_t)W2 * H2);
                for (int y = 0; y < H2; y++)
                    for (int x = 0; x < W2; x++) {
                        int cx = mode == 2 ? (x >> 1) & 1 : x & 1;
                        int cy = mode == 2 ? (y >> 1) & 1 : y & 1;
                        d->px()[(size_t)y * W2 + x] = LVL[CFA_MAP[0][cy * 2 + cx]];
                    }
                d->syncMirrors();
                d->uid = app.nextUid++;
                return d;
            };
            // The size ladder pins the onset of each stride: the last clean
            // size, the first even stride (where the old loops went blind),
            // one odd stride (which never broke - the bias is NOT monotonic
            // in pixels), and a quad Bayer size whose stride hits 0 mod 4.
            bool histAll = true, histBal = true, roiAll = true, roiBal = true;
            const struct { int w, h, mode; } rungs[] = {
                { 800,  499,  1 },   // roi step 1 - last clean ROI size
                { 800,  500,  1 },   // roi step 2 - the ROI-stats onset (0.4 Mpx)
                { 2000, 999,  1 },   // hist step 1 - last clean histogram size
                { 2000, 1000, 1 },   // hist step 2 - the histogram onset (2 Mpx)
                { 2000, 1500, 1 },   // hist step 3 (odd: even columns come back)
                { 2000, 2000, 1 },   // hist step 4
                { 2048, 2048, 2 },   // quad: hist step 4, roi step 20, both 0 mod 4
            };
            for (const auto& r : rungs) {
                auto d = mkCfa(r.w, r.h, r.mode);
                App::HistState T{};
                recomputeHistogramIfNeeded(d.get(), T);
                size_t hn[4], hmin = SIZE_MAX, hmax = 0, rn[4], rmin = SIZE_MAX, rmax = 0;
                for (int s = 0; s < 4; s++) {
                    hn[s] = planeN(T, s);
                    hmin = std::min(hmin, hn[s]); hmax = std::max(hmax, hn[s]);
                    rn[s] = roiBasicStatsUncached(*d, 0, 0, r.w, r.h, s).n;
                    rmin = std::min(rmin, rn[s]); rmax = std::max(rmax, rn[s]);
                }
                fprintf(stderr, "verifyselftest: V18 %dx%d %s: hist n R=%zu Gr=%zu "
                                "Gb=%zu B=%zu | roi n R=%zu Gr=%zu Gb=%zu B=%zu\n",
                        r.w, r.h, r.mode == 2 ? "quad" : "bayer",
                        hn[0], hn[1], hn[2], hn[3], rn[0], rn[1], rn[2], rn[3]);
                if (!hmin) histAll = false;
                if (hmin < hmax - hmax / 25) histBal = false;   // within 4%
                if (!rmin) roiAll = false;
                if (rmin < rmax - rmax / 25) roiBal = false;
            }
            check(histAll, "V18 histogram samples every CFA plane");
            check(histBal, "V18 histogram plane sample counts are close");
            check(roiAll, "V18 ROI stats sample every CFA plane");
            check(roiBal, "V18 ROI stats plane sample counts are close");
            // ...and through the real loader, on the fixture gen_testdata.py
            // writes for exactly this: flat RGGB planes at 100/300/500/900 in
            // a 2064x1032 (2.13 Mpx) frame, so a sampler that reads the wrong
            // pixels cannot return the right per-plane means.
            std::string berr;
            auto big = decodeNpy((std::filesystem::path(g_verifySelftest).parent_path() /
                                  "cfa_big_rggb_2064x1032_u16.npy").u8string(), berr);
            check(big != nullptr, "V18 big Bayer fixture loads");
            if (big) {
                big->cfa = 1; big->cfaPattern = 0;         // RGGB
                big->uid = app.nextUid++;
                static const double LVL[4] = { 100, 300, 500, 900 };
                App::HistState T{};
                recomputeHistogramIfNeeded(big.get(), T);
                bool hOk = true, hmOk = true;
                std::string hMiss;
                for (int s = 0; s < 4; s++) {
                    if (!planeN(T, s)) { hOk = false; hMiss += T.seriesNames[s]; hMiss += " "; }
                    if (fabs(T.mean[s] - LVL[s]) > 1e-3) hmOk = false;
                }
                fprintf(stderr, "verifyselftest: V18 fixture hist: %s; mean R=%.6g "
                                "Gr=%.6g Gb=%.6g B=%.6g (want 100/300/500/900)\n",
                        hOk ? "all four planes sampled" : ("MISSING " + hMiss).c_str(),
                        T.mean[0], T.mean[1], T.mean[2], T.mean[3]);
                check(hOk, "V18 fixture histogram sees all four planes");
                check(hmOk, "V18 fixture histogram means hit the plane levels");
                bool rOk = true, rmOk = true;
                std::string rMiss;
                for (int s = 0; s < 4; s++) {
                    RoiStat rs = roiBasicStatsUncached(*big, 0, 0, big->w, big->h, s);
                    if (!rs.valid || !rs.n) { rOk = false; rMiss += CFA_CH_NAMES[s]; rMiss += " "; }
                    else if (fabs(rs.mean - LVL[s]) > 1e-3) rmOk = false;
                }
                fprintf(stderr, "verifyselftest: V18 fixture roi: %s\n",
                        rOk ? "all four planes sampled" : ("MISSING " + rMiss).c_str());
                check(rOk, "V18 fixture ROI stats see all four planes");
                check(rmOk, "V18 fixture ROI means hit the plane levels");
            }
        }

        {   // ---- V19: the ROI table's LAYOUT, and its delete button ----------
            // The panel reaches its numbers through a hash cache, prints them
            // from inside a clipper, and puts its delete button in the last
            // column of a row whose label Selectable spans ALL columns. None of
            // those three layers had a test. So drive the real panel in real
            // frames and read back where the button actually is.
            //
            // The NUMBERS moved out, to --roistats-selftest (roiStatsSelftest()).
            // They are the same panel, the same cache, the same clipper and the
            // same PRNU cell, read back through this same probe - but a number
            // is not a rectangle, and asking for a GL context to check one had
            // the PRNU column measured on one runner of the three. What stays
            // here is what a display genuinely decides: where the row's delete
            // button landed, and whether a click reaches it past the Selectable
            // that covers it.
            //
            // Fixture is analytic, so this needs no file: per-plane levels plus
            // a within-plane dither, on a Bayer frame.
            auto mkCfaD = [](int W2, int H2, double amp) {
                auto d = std::make_unique<ImageDoc>();
                d->name = "v19cfa"; d->src->w = W2; d->src->h = H2; d->src->ch = 1;
                d->cfa = 1; d->cfaPattern = 0; d->src->dtype = "u16";
                static const double LVL[4] = { 1000, 2000, 3000, 4000 };
                d->src->data.resize((size_t)W2 * H2);
                for (int y = 0; y < H2; y++)
                    for (int x = 0; x < W2; x++)
                        d->px()[(size_t)y * W2 + x] =
                            (float)(LVL[CFA_MAP[0][(y & 1) * 2 + (x & 1)]] +
                                    (((x >> 1) & 1) ? amp : -amp));
                d->syncMirrors();
                d->uid = app.nextUid++;
                return d;
            };

            if (!needWindow("--verify-selftest (the ROI row probe)")) return 1;
            ImGuiIO& pio = ImGui::GetIO();
            int held = -1;
            auto roiFrame = [&](ImVec2 mouse, int btn) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                // after the backend, before NewFrame: the GLFW backend rewrites
                // MousePos from the OS cursor every frame (see g_injMouse)
                if (mouse.x >= 0) pio.AddMousePosEvent(mouse.x, mouse.y);
                if (held != btn) {
                    if (held >= 0) pio.AddMouseButtonEvent(held, false);
                    if (btn >= 0) pio.AddMouseButtonEvent(btn, true);
                    held = btn;
                }
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_Always);
                ImGui::Begin("RoiProbe", nullptr,
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
                g_roiRowProbeOn = true;
                drawPanelRois();
                ImGui::End();
                ImGui::EndFrame();
            };
            auto settle = [&] { for (int f = 0; f < 4; f++) roiFrame(ImVec2(-1, -1), -1); };
            auto row = [&](int id) -> const RoiRowProbe* {
                for (const auto& r : g_roiRowProbe) if (r.id == id) return &r;
                return nullptr;
            };

            closeAll();
            app.images.push_back(mkCfaD(1024, 1024, 3.0));
            app.current = 0;
            app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0; app.selectedAnn = 0;
            addAnn(0, 0, 0, 512, 512);
            const int cfaRoi2 = app.anns.back().id;
            app.roiChannel = -1;
            settle();
            for (const auto& r : g_roiRowProbe)
                fprintf(stderr, "verifyselftest: V19 row %-3s valid=%d lbl=%.0f,%.0f "
                                "del=%.0f,%.0f\n", r.id ? "roi" : "all", (int)r.s.valid,
                        r.lbl.x, r.lbl.y, r.del.x, r.del.y);
            check(row(0) && row(0)->s.valid, "V19 panel prints the All row");
            check(row(cfaRoi2) && row(cfaRoi2)->s.valid, "V19 panel prints a placed ROI row");

            // ...but the row label must STILL select: the button is only reachable
            // because the Selectable now allows overlap, and that must not cost
            // the click the Selectable is there for.
            app.selectedAnn = 0;
            settle();
            if (const RoiRowProbe* rl = row(cfaRoi2)) {
                roiFrame(rl->lbl, -1);
                roiFrame(rl->lbl, 0);
                roiFrame(rl->lbl, -1);
                roiFrame(rl->lbl, -1);
                fprintf(stderr, "verifyselftest: V19 label click at %.0f,%.0f: sel=%d\n",
                        rl->lbl.x, rl->lbl.y, app.selectedAnn);
                check(app.selectedAnn == cfaRoi2, "V19 clicking the row still selects it");
            }

            // ---- the delete button ------------------------------------------
            // Its click has to survive the row label's SpanAllColumns Selectable,
            // which is submitted first and covers the button's rect.
            settle();
            const RoiRowProbe* rd = row(cfaRoi2);
            check(rd && rd->del.x > 0, "V19 the row's delete button is on screen");
            if (rd) {
                ImVec2 at = rd->del;
                size_t before = app.anns.size();
                roiFrame(at, -1);                    // hover
                roiFrame(at, 0);                     // press
                roiFrame(at, -1);                    // release
                roiFrame(at, -1);
                fprintf(stderr, "verifyselftest: V19 delete click at %.0f,%.0f: "
                                "anns %zu -> %zu\n", at.x, at.y, before, app.anns.size());
                check(app.anns.size() == before - 1, "V19 the x button deletes its ROI");
            }

            g_roiRowProbeOn = false;
            held = -1;
            pio.AddMouseButtonEvent(0, false);
        }

        // ---- V21: an .npz is a BATCH of members; a 1-D key is not a picture --
        // docs/npz-design.md §1: loadNpz opened EVERY *.npy member in silence and
        // decodeNpyBuffer maps a 1-D (N,) array to H=1,W=N - so a per-frame
        // exposure vector arrived as a one-pixel-tall PICTURE and a scalar as a
        // 1x1 one. That is fabrication, which this project forbids.
        //
        // The fixture is what a capture script actually writes, built HERE so the
        // test needs no Python (miniz is linked; npzList/npzExtract are the format):
        //     data        (4,8,8) uint16   -> the stack
        //     dark        (8,8)   uint16   -> a frame
        //     exposure_ms (4,)    float64  -> the per-frame x AXIS, not pixels
        //     gain        scalar  float64  -> metadata
        // What has to hold: nothing that is not an image becomes an ImageDoc, the
        // frame-axis member opens as ONE stack of 4, the 1-D member whose length
        // matches can BECOME that stack's axis at full precision, the counts are
        // said in both directions by name, and a session round-trip brings back
        // the same member with its axis.
        {
            closeAll();
            // (4,8,8) used to be the OTHER ambiguity: a leading 4 is as
            // plausible a channel count as a frame count, and the old heuristic
            // read it as CHW - so V21 had to set --npy-axis frames to get the
            // four frames it is about. There is nothing to set now. §3.1 reads
            // the LAST axis, (4,8,8) ends in 8, and eight is not a channel
            // count, so it is four frames because that is what native means.
            std::error_code nec;
            std::filesystem::path ndir = vfyScratch("vfy-npz-scratch");
            // one .npy member as bytes
            auto npyBytes = [](const char* descr, const std::vector<int64_t>& shape,
                               const void* data, size_t nbytes) {
                std::string sh;
                if (shape.empty()) sh = "()";
                else if (shape.size() == 1) sh = "(" + std::to_string(shape[0]) + ",)";
                else {
                    sh = "(";
                    for (size_t i = 0; i < shape.size(); i++)
                        sh += std::to_string(shape[i]) + (i + 1 < shape.size() ? ", " : "");
                    sh += ")";
                }
                std::string hdr = std::string("{'descr': '") + descr +
                                  "', 'fortran_order': False, 'shape': " + sh + ", }";
                hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
                hdr += '\n';
                std::vector<uint8_t> out;
                const char magic[8] = { '\x93', 'N', 'U', 'M', 'P', 'Y', '\x01', '\x00' };
                out.insert(out.end(), magic, magic + 8);
                out.push_back((uint8_t)(hdr.size() & 0xff));
                out.push_back((uint8_t)(hdr.size() >> 8));
                out.insert(out.end(), hdr.begin(), hdr.end());
                const uint8_t* p = (const uint8_t*)data;
                out.insert(out.end(), p, p + nbytes);
                return out;
            };
            // a stored (method 0) zip: local headers, central directory, EOCD
            // One zip member as it will sit in the file: the BYTES to store, the
            // uncompressed size the directory advertises, and the method. Stored
            // (0) and deflate (8) both have to work - the classifier reads only
            // a header-sized prefix now, which is a different code path for each.
            struct ZipMem {
                std::string name;
                std::vector<uint8_t> bytes;
                size_t usize;
                uint16_t method;
                uint32_t crc;
            };
            auto writeNpzRaw = [](const std::string& path, std::vector<ZipMem> mem) {
                std::vector<uint8_t> z;
                auto p16 = [&](uint32_t x) {
                    z.push_back((uint8_t)(x & 0xff)); z.push_back((uint8_t)((x >> 8) & 0xff));
                };
                auto p32 = [&](uint32_t x) {
                    for (int i = 0; i < 4; i++) z.push_back((uint8_t)((x >> (8 * i)) & 0xff));
                };
                std::vector<uint32_t> offs;
                for (const auto& m : mem) {
                    offs.push_back((uint32_t)z.size());
                    p32(0x04034b50); p16(20); p16(0); p16(m.method); p16(0); p16(0);
                    p32(m.crc); p32((uint32_t)m.bytes.size()); p32((uint32_t)m.usize);
                    p16((uint32_t)m.name.size()); p16(0);
                    z.insert(z.end(), m.name.begin(), m.name.end());
                    z.insert(z.end(), m.bytes.begin(), m.bytes.end());
                }
                uint32_t cdOff = (uint32_t)z.size();
                for (size_t i = 0; i < mem.size(); i++) {
                    p32(0x02014b50); p16(20); p16(20); p16(0); p16(mem[i].method);
                    p16(0); p16(0);
                    p32(mem[i].crc); p32((uint32_t)mem[i].bytes.size());
                    p32((uint32_t)mem[i].usize);
                    p16((uint32_t)mem[i].name.size()); p16(0); p16(0);
                    p16(0); p16(0); p32(0); p32(offs[i]);
                    z.insert(z.end(), mem[i].name.begin(), mem[i].name.end());
                }
                uint32_t cdSize = (uint32_t)z.size() - cdOff;   // before the EOCD
                p32(0x06054b50); p16(0); p16(0);
                p16((uint32_t)mem.size()); p16((uint32_t)mem.size());
                p32(cdSize); p32(cdOff); p16(0);
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write((const char*)z.data(), (std::streamsize)z.size());
            };
            // the common case: every member STORED, straight from its .npy bytes
            auto writeNpz = [&](const std::string& path,
                                const std::vector<std::pair<std::string,
                                                            std::vector<uint8_t>>>& mem) {
                std::vector<ZipMem> zm;
                for (const auto& m : mem)
                    zm.push_back({ m.first, m.second, m.second.size(), 0,
                                   (uint32_t)mz_crc32(MZ_CRC32_INIT, m.second.data(),
                                                      m.second.size()) });
                writeNpzRaw(path, std::move(zm));
            };
            std::vector<uint16_t> data((size_t)4 * 8 * 8);
            for (int f = 0; f < 4; f++)
                for (int i = 0; i < 64; i++) data[(size_t)f * 64 + i] = (uint16_t)(1000 + f * 100 + i);
            std::vector<uint16_t> dark(64, 7);
            // values a float32 round trip would destroy: an axis is DATA
            const double EXP[4] = { 0.5, 2.5, 12.75, 12345.678901234567 };
            const double GAIN = 1.5;
            std::string npzPath = (ndir / "sweep.npz").u8string();
            writeNpz(npzPath, {
                { "data.npy",        npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
                { "dark.npy",        npyBytes("<u2", { 8, 8 },    dark.data(), dark.size() * 2) },
                { "exposure_ms.npy", npyBytes("<f8", { 4 },       EXP, sizeof EXP) },
                { "gain.npy",        npyBytes("<f8", { },         &GAIN, sizeof GAIN) },
            });

            // Two image-shaped members and a 1-D key as long as one of them:
            // there is something to decide, so the picker comes up - and it is
            // driven here through the same call the button makes.
            std::string oerr = loadNpz(npzPath);
            check(app.npzPickOpen && app.npzPick.size() == 4,
                  "V21 a multi-member npz raises the member picker");
            {
                std::string roles;
                for (const auto& m : app.npzPick)
                    roles += (roles.empty() ? "" : ", ") + m.name + " " + m.shapeText +
                             " " + m.dtype + " -> " + m.becomes;
                fprintf(stderr, "verifyselftest: V21 picker: %s\n", roles.c_str());
            }
            // one flag per member: an accumulator would have made the result
            // depend on the order the fixture happened to be written in
            bool sawStack = false, sawFrame = false, sawAxis = false, sawScalar = false;
            for (auto& m : app.npzPick) {
                if (m.name == "data")        sawStack = m.role == NpzMember::RImage && m.frames == 4;
                if (m.name == "dark")        sawFrame = m.role == NpzMember::RImage && m.frames == 1;
                if (m.name == "exposure_ms") sawAxis  = m.role == NpzMember::RAxis && m.vals.size() == 4;
                if (m.name == "gain")        sawScalar = m.role == NpzMember::RMeta;
            }
            check(sawStack && sawFrame && sawAxis && sawScalar,
                  "V21 the picker classifies every member by shape");
            // tick the axis, name and unit typed by the "user" - never inferred
            for (auto& m : app.npzPick) if (m.role == NpzMember::RAxis) m.selected = true;
            snprintf(app.npzAxisName, sizeof app.npzAxisName, "exposure_ms");
            snprintf(app.npzAxisUnit, sizeof app.npzAxisUnit, "ms");
            npzPickAccept();
            // (a) NOTHING that is not an image may become one
            int fabricated = 0;
            std::string fabNames;
            for (const auto& d : app.images)
                if (d->src->member == "exposure_ms" || d->src->member == "gain") {
                    fabricated++;
                    if (fabNames.size() < 90)
                        fabNames += (fabNames.empty() ? "" : ", ") + d->name +
                                    " " + std::to_string(d->w) + "x" + std::to_string(d->h);
                }
            fprintf(stderr, "verifyselftest: V21 opened %zu image(s), %zu stack(s)%s%s\n",
                    app.images.size(), app.seqs.size(),
                    fabricated ? "; FABRICATED: " : "", fabNames.c_str());
            check(fabricated == 0, "V21 a 1-D / scalar member never becomes an image");
            // (b) the frame-axis member is ONE stack of 4; dark is the lone frame
            int v21frames = app.seqs.empty() ? 0 : (int)framesOfSeq(app.seqs.front().id).size();
            check(app.seqs.size() == 1 && v21frames == 4,
                  "V21 the frame-axis member opens as ONE stack of 4");
            check(app.images.size() == 5, "V21 only the image-shaped members opened");
            // ...and every one of them carries the Watch baseline of the FILE
            // it came out of. This is the third of statSourceFile's decode
            // paths; docs/verify-functional.md A-12 traced all three by eye and
            // recorded 要probe because nothing printed mtime/fsize. srcMapDump
            // prints them, so the npz path is now checked by execution, against
            // the container's own size on disk.
            srcMapDump("V21 npz members");
            {
                std::error_code zec;
                uint64_t zsz = (uint64_t)std::filesystem::file_size(pathFromUtf8(npzPath), zec);
                bool baseOk = !zec && !app.images.empty();
                for (const auto& d : app.images)
                    if (d->src->mtime == 0 || d->src->fsize != zsz) baseOk = false;
                check(baseOk, "V21 every npz member carries the container's Watch baseline");
            }
            // the FILE is the batch: two members of different shapes are two
            // stacks under one heading, not two unrelated opens
            {
                int b0 = app.images.empty() ? 0 : app.images.front()->batchId;
                bool oneBatch = b0 != 0;
                for (const auto& d : app.images) if (d->batchId != b0) oneBatch = false;
                std::string bname;
                for (const auto& b : app.batches) if (b.id == b0) bname = b.name;
                fprintf(stderr, "verifyselftest: V21 batch: one=%d name='%s'\n",
                        (int)oneBatch, bname.c_str());
                check(oneBatch && bname == "sweep.npz",
                      "V21 the npz FILE is one batch, named after the file");
            }
            // (c) the matching 1-D member becomes the axis, EXACTLY
            App::SeqInfo* v21si = app.seqs.empty() ? nullptr : seqInfo(app.seqs.front().id);
            bool axOk = v21si && v21si->axisVals.size() == 4 &&
                        v21si->axisVals[3] == 12345.678901234567 &&
                        v21si->axisName == "exposure_ms" && v21si->axisUnit == "ms";
            fprintf(stderr, "verifyselftest: V21 axis: %d value(s) '%s' [%s], last=%s\n",
                    v21si ? (int)v21si->axisVals.size() : -1,
                    v21si ? v21si->axisName.c_str() : "-",
                    v21si ? v21si->axisUnit.c_str() : "-",
                    v21si && !v21si->axisVals.empty() ? fmtExact(v21si->axisVals.back()).c_str()
                                                      : "-");
            check(axOk, "V21 the matching 1-D member lands in axisVals exactly");
            // (d) counts BOTH directions, by name, never silent
            std::string v21msg = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            bool said = v21msg.find("exposure_ms") != std::string::npos &&
                        v21msg.find("gain") != std::string::npos &&
                        v21msg.find("4 frames") != std::string::npos;
            fprintf(stderr, "verifyselftest: V21 message: \"%s\"%s\n", v21msg.c_str(),
                    oerr.empty() ? "" : (" err: " + oerr).c_str());
            check(said, "V21 the open names what opened AND what did not");
            // (e) the session brings back the same member, with its axis
            std::string v21sess = (ndir / "npz.vsession").u8string();
            saveSession(v21sess, true);
            std::string v21lerr = loadSession(v21sess);
            loadAll();
            bool member = false;
            for (const auto& d : app.images) if (d->src->member == "data") member = true;
            App::SeqInfo* v21r = app.seqs.empty() ? nullptr : seqInfo(app.seqs.front().id);
            bool raxOk = v21r && v21r->axisVals.size() == 4 &&
                         v21r->axisVals[3] == 12345.678901234567 &&
                         v21r->axisName == "exposure_ms" && v21r->axisUnit == "ms";
            fprintf(stderr, "verifyselftest: V21 after restore: %zu image(s), member=%d, "
                            "axis=%d%s\n", app.images.size(), (int)member, (int)raxOk,
                    v21lerr.empty() ? "" : (" load err: " + v21lerr).c_str());
            check(v21lerr.empty() && app.images.size() == 5 && member,
                  "V21 the session restores the same member");
            check(raxOk, "V21 the session restores the axis at full precision");

            // ---- V21b: one image and nothing to decide opens with NO dialog --
            // The picker exists because a container CAN hold a choice, not
            // because opening a file should now cost a click.
            closeAll();
            std::string oneNpz = (ndir / "one.npz").u8string();
            writeNpz(oneNpz, {
                { "data.npy", npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
            });
            std::string oneErr = loadNpz(oneNpz);
            fprintf(stderr, "verifyselftest: V21b single-array npz: picker=%d, %zu image(s)"
                            "%s\n", (int)app.npzPickOpen, app.images.size(),
                    oneErr.empty() ? "" : (" err: " + oneErr).c_str());
            check(oneErr.empty() && !app.npzPickOpen && app.images.size() == 4 &&
                  app.seqs.size() == 1,
                  "V21b one image-shaped member opens with no picker");

            // ---- V21c: deeper than (F,H,W,C) is refused BY NAME, not sliced --
            // It used to erase leading axes and take [0] of each in silence.
            // A string member is metadata and is READ, not merely counted.
            closeAll();
            std::vector<uint16_t> cube((size_t)2 * 2 * 4 * 8 * 8, 3);
            uint32_t note[8] = { 'd', 'a', 'r', 'k', ' ', 'r', 'u', 'n' };
            std::string oddNpz = (ndir / "odd.npz").u8string();
            writeNpz(oddNpz, {
                { "cube.npy", npyBytes("<u2", { 2, 2, 4, 8, 8 }, cube.data(), cube.size() * 2) },
                { "data.npy", npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
                { "note.npy", npyBytes("<U8", { },         note, sizeof note) },
            });
            std::string oddErr = loadNpz(oddNpz);
            std::string oddMsg = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            fprintf(stderr, "verifyselftest: V21c \"%s\"\n", oddMsg.c_str());
            check(oddErr.empty() && app.images.size() == 4,
                  "V21c a >4-D member is not sliced into a picture");
            check(oddMsg.find("cube") != std::string::npos &&
                  oddMsg.find("(2, 2, 4, 8, 8)") != std::string::npos,
                  "V21c the refusal names the member and its shape");
            bool sawNote = false;
            for (const auto& e : app.npzMeta)
                if (e.path == oddNpz)
                    for (const auto& kv : e.items)
                        if (kv.first == "note" && kv.second.find("dark run") != std::string::npos)
                            sawNote = true;
            check(sawNote, "V21c a string member is kept as readable metadata");

            // ---- V21d: a unit of " " is not a unit, on EITHER path -----------
            // applyFrameAxisVals tested unit.empty() and only the pasted caller
            // trimmed, so a single space was refused from the Temporal panel and
            // accepted from the .npz picker - rendering "x axis in  ". One rule,
            // enforced where the rule lives.
            closeAll();
            std::string spNpz = (ndir / "space.npz").u8string();
            writeNpz(spNpz, {
                { "data.npy",        npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
                { "dark.npy",        npyBytes("<u2", { 8, 8 },    dark.data(), dark.size() * 2) },
                { "exposure_ms.npy", npyBytes("<f8", { 4 },       EXP, sizeof EXP) },
            });
            loadNpz(spNpz);
            for (auto& m : app.npzPick) if (m.role == NpzMember::RAxis) m.selected = true;
            snprintf(app.npzAxisName, sizeof app.npzAxisName, "exposure_ms");
            snprintf(app.npzAxisUnit, sizeof app.npzAxisUnit, " ");
            npzPickAccept();
            App::SeqInfo* spSi = app.seqs.empty() ? nullptr : seqInfo(app.seqs.front().id);
            std::string spMsg = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            fprintf(stderr, "verifyselftest: V21d blank unit: axisVals=%d \"%s\"\n",
                    spSi ? (int)spSi->axisVals.size() : -1, spMsg.c_str());
            check(spSi && spSi->axisVals.empty(),
                  "V21d a whitespace unit is refused from the npz path too");
            // ...and the pasted path still refuses it, so the two agree
            {
                std::string perr;
                bool pasted = app.seqs.empty() ? true
                    : applyFrameAxis(app.seqs.front().id, "exposure_ms", "  ",
                                     "0.5 2.5 12.75 12345.678901234567", perr);
                check(!pasted, "V21d the pasted path refuses it identically");
            }

            // ---- V21e: a zero-length axis is not an image --------------------
            // (8,8,0) and (0,8,8,3) passed: W and H were bounded, F and C were
            // not, so one built an ImageDoc with ch=0 and the other read H*W*C
            // values out of a payload holding none.
            closeAll();
            std::vector<uint16_t> none16(1, 0);
            std::string zeroNpz = (ndir / "zero.npz").u8string();
            writeNpz(zeroNpz, {
                { "flatc.npy", npyBytes("<u2", { 8, 8, 0 },    none16.data(), 0) },
                { "flatf.npy", npyBytes("<u2", { 0, 8, 8, 3 }, none16.data(), 0) },
                { "data.npy",  npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
            });
            std::string zErr = loadNpz(zeroNpz);
            std::string zMsg = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            int zeroCh = 0;
            for (const auto& d : app.images) if (d->ch < 1) zeroCh++;
            fprintf(stderr, "verifyselftest: V21e \"%s\" (%zu image(s), ch<1: %d)\n",
                    zMsg.c_str(), app.images.size(), zeroCh);
            check(zErr.empty() && app.images.size() == 4 && zeroCh == 0,
                  "V21e a zero-length axis never becomes an ImageDoc");
            check(zMsg.find("flatc") != std::string::npos &&
                  zMsg.find("flatf") != std::string::npos,
                  "V21e both zero-axis members are named in the message");

            // ---- V21f: a deflate member, read header-first --------------------
            // npzScan reads only the header prefix now; this member is DEFLATED,
            // so the streamed partial inflate and the full read are both exercised.
            closeAll();
            std::vector<uint8_t> rawNpy =
                npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2);
            size_t clen = 0;
            void* comp = tdefl_compress_mem_to_heap(rawNpy.data(), rawNpy.size(), &clen,
                                                    TDEFL_DEFAULT_MAX_PROBES);
            check(comp != nullptr && clen > 0, "V21f the fixture really is deflated");
            if (comp) {
                std::vector<uint8_t> cbytes((uint8_t*)comp, (uint8_t*)comp + clen);
                mz_free(comp);
                std::string defNpz = (ndir / "packed.npz").u8string();
                writeNpzRaw(defNpz, { { "data.npy", cbytes, rawNpy.size(), 8,
                                        (uint32_t)mz_crc32(MZ_CRC32_INIT, rawNpy.data(),
                                                           rawNpy.size()) } });
                std::string dErr = loadNpz(defNpz);
                fprintf(stderr, "verifyselftest: V21f deflate member: %zu image(s)%s\n",
                        app.images.size(), dErr.empty() ? "" : (" err: " + dErr).c_str());
                check(dErr.empty() && app.images.size() == 4 && app.seqs.size() == 1,
                      "V21f a deflated member classifies and opens");
            }

            // ---- V21g: dropping two multi-member .npz files loses neither ----
            // openPath is called in a LOOP by the drop callback and the file
            // dialog. One picker can be up at a time, and the second loadNpz
            // used to overwrite the first one's pending members with nothing
            // said - so a file the user dropped simply never opened.
            closeAll();
            std::string twoA = (ndir / "runA.npz").u8string();
            std::string twoB = (ndir / "runB.npz").u8string();
            for (const std::string& p : { twoA, twoB })
                writeNpz(p, {
                    { "data.npy", npyBytes("<u2", { 4, 8, 8 }, data.data(), data.size() * 2) },
                    { "dark.npy", npyBytes("<u2", { 8, 8 },    dark.data(), dark.size() * 2) },
                });
            loadNpz(twoA);
            bool firstUp = app.npzPickOpen && baseName(app.npzPickPath) == "runA.npz";
            loadNpz(twoB);                      // the loop's second file
            std::string qMsg = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            bool queued = app.npzPickQueue.size() == 1 &&
                          baseName(app.npzPickPath) == "runA.npz" &&
                          qMsg.find("runB.npz") != std::string::npos;
            fprintf(stderr, "verifyselftest: V21g first=%d queued=%zu \"%s\"\n",
                    (int)firstUp, app.npzPickQueue.size(), qMsg.c_str());
            check(firstUp && queued, "V21g the second .npz waits and is NAMED");
            npzPickAccept();                    // accepting A must raise B
            bool drained = app.npzPickOpen && baseName(app.npzPickPath) == "runB.npz" &&
                           app.npzPickQueue.empty();
            fprintf(stderr, "verifyselftest: V21g after accepting A: picker=%s, queue=%zu\n",
                    app.npzPickOpen ? baseName(app.npzPickPath).c_str() : "(none)",
                    app.npzPickQueue.size());
            check(drained, "V21g accepting the first raises the one that waited");
            npzPickAccept();
            check(!app.npzPickOpen && app.npzPickQueue.empty() && app.images.size() == 10,
                  "V21g both files end up open");
            app.npzPick.clear();
            app.npzPickOpen = false;
            app.npzPickQueue.clear();

            std::filesystem::remove_all(ndir, nec);
            closeAll();
        }

        // ---- V22: native reads FOUR shapes, and SAYS SO when it will not -----
        // docs/input-adapters.md §3.1: the decision is RANK and the LAST AXIS,
        // and nothing else. The LEADING axis is never consulted - "a small
        // leading axis must be channels" is the guess that turned three frames
        // into one 3-channel picture, and a 1-D exposure vector into a
        // one-pixel-tall image. Both are fabrication, and both flip here.
        //
        //   (H,W)        1 frame, 1 ch
        //   (H,W,C<=4)   1 frame of C channels <- the ONE size-based exception,
        //                                        and it is on the LAST AXIS
        //   (F,H,W)      F frames, 1 ch each   <- so (3,H,W) is THREE FRAMES
        //   (F,H,W,C)    F frames of C channels
        //
        // The exception is a CEILING, not the set {3,4} (issue #71). (H,W,1) is
        // ONE mono frame and (H,W,2) is ONE two-channel frame - the shapes that
        // img[:, :, None] and np.expand_dims(a, -1) produce every day, and the
        // shapes this table had no row for, which is why the local reader and
        // core/serve.cpp were free to disagree about them for as long as they
        // did. They have rows now.
        //
        // Anything else is refused BY NAME and the refusal names what native
        // does read (§3.2), because "cannot open" sends nobody anywhere.
        {
            closeAll();
            std::error_code vec;
            std::filesystem::path vdir = vfyScratch("vfy-native-scratch");
            // one .npy FILE on disk from a shape and its bytes. V21 builds .npy
            // members inside a zip; these have to be loose files because the
            // rule under test is the FILE loader's, and loadNpy is its door.
            auto writeNpy = [&](const std::string& name, const char* descr,
                                const std::vector<int64_t>& shape,
                                const void* data, size_t nbytes) {
                return npyWriteFile((vdir / name).u8string(), descr, shape, data, nbytes);
            };
            // every fixture is a slice of ONE ramp, so a re-reading of the same
            // file is comparable to the reading it replaced pixel for pixel
            std::vector<uint16_t> pix(4096);
            for (size_t i = 0; i < pix.size(); i++) pix[i] = (uint16_t)(i * 7 + 3);
            auto npyOf = [&](const std::string& name, const std::vector<int64_t>& shape) {
                size_t n = 1;
                for (int64_t d : shape) n *= (size_t)d;
                return writeNpy(name, "<u2", shape, pix.data(), n * 2);
            };
            struct Opened { std::string err; size_t docs; int w, h, ch; };
            auto openOne = [&](const std::string& path) {
                closeAll();
                Opened o{};
                o.err = loadNpy(path);
                o.docs = app.images.size();
                if (!app.images.empty()) {
                    o.w = app.images[0]->w; o.h = app.images[0]->h; o.ch = app.images[0]->ch;
                }
                return o;
            };

            // ---- V22a: the four native forms open, and open as themselves ----
            struct Want { const char* file; std::vector<int64_t> shape;
                          size_t docs; int w, h, ch; };
            const Want ACCEPT[] = {
                { "a_hw.npy",   { 8, 8 },       1, 8, 8, 1 },   // (H,W)
                // "a_mono"/"a_uv" and not "a_hw1"/"a_hw2": a_hw1..a_hw4 would be
                // four NUMBERED SIBLINGS, and the sequence loader groups those
                // into one stack. It no longer STACKS four different shapes -
                // a sibling that is not the reference frame's shape is refused
                // by name now, and --picker-selftest UC7 is where that is
                // asserted - but the names would still manufacture a second
                // subject here (a stack, its refusals, and a load to wait for)
                // in a test that is about the shape rule alone.
                { "a_mono.npy", { 8, 8, 1 },    1, 8, 8, 1 },   // (H,W,1)  ONE mono frame
                { "a_uv.npy",   { 8, 8, 2 },    1, 8, 8, 2 },   // (H,W,2)  ONE 2ch frame
                { "a_hw3.npy",  { 8, 8, 3 },    1, 8, 8, 3 },   // (H,W,3)  colour
                { "a_hw4.npy",  { 8, 8, 4 },    1, 8, 8, 4 },   // (H,W,4)  RGBA
                { "a_fhw.npy",  { 3, 8, 8 },    3, 8, 8, 1 },   // (3,H,W) = 3 FRAMES
                { "a_fhwc.npy", { 2, 8, 8, 3 }, 2, 8, 8, 3 },   // (F,H,W,C)
            };
            for (const auto& t : ACCEPT) {
                Opened o = openOne(npyOf(t.file, t.shape));
                bool good = o.err.empty() && o.docs == t.docs &&
                            o.w == t.w && o.h == t.h && o.ch == t.ch;
                fprintf(stderr, "verifyselftest: V22a %-14s -> %zu doc(s) %dx%d %dch"
                                " (want %zu doc(s) %dx%d %dch)%s%s\n",
                        npyShapeText(t.shape).c_str(), o.docs, o.w, o.h, o.ch,
                        t.docs, t.w, t.h, t.ch, o.err.empty() ? "" : "  err=", o.err.c_str());
                check(good, ("V22a native reads " + npyShapeText(t.shape)).c_str());
            }

            // ---- V22a2: (H,W,2) actually REACHES the 2-channel labels --------
            // The C0/C1 branch of the Pixel readout exists because "2ch is
            // usually UV or complex, not RG" - and until issue #71 no local
            // .npy could reach it, because (H,W,2) was read as a stack of
            // 1-channel frames. Accepting the shape is only half the change;
            // asking the SAME function drawInspector asks is what turns "the
            // branch should now be reachable" into something that is checked.
            {
                Opened o = openOne(npyOf("a_uvlabel.npy", { 8, 8, 2 }));
                const char* const* lb = channelLabels(o.ch);
                bool two = o.err.empty() && o.ch == 2 &&
                           !strcmp(lb[0], "C0") && !strcmp(lb[1], "C1");
                fprintf(stderr, "verifyselftest: V22a (8, 8, 2) -> %d ch, pixel labels "
                                "%s/%s (2ch is UV/complex, not RG)\n",
                        o.ch, lb[0], lb[1]);
                check(two, "V22a (H,W,2) opens 2ch and reads out as C0/C1");
            }

            // ---- V22b: everything else is REFUSED, and nothing is built ------
            struct Bad { const char* file; std::vector<int64_t> shape; const char* why; };
            const Bad REFUSE[] = {
                { "b_1d.npy",   { 6 },             "1-D is an axis, not a picture" },
                { "b_0d.npy",   {},                "a scalar is a number" },
                { "b_5d.npy",   { 2, 2, 4, 4, 3 }, "deeper than four axes" },
                { "b_fchw.npy", { 2, 3, 8, 8 },    "(F,C,H,W): the last axis is not channels" },
                { "b_zero.npy", { 0, 8, 8 },       "a zero-length axis" },
            };
            for (const auto& t : REFUSE) {
                Opened o = openOne(npyOf(t.file, t.shape));
                bool good = !o.err.empty() && o.docs == 0;
                fprintf(stderr, "verifyselftest: V22b %-16s %-28s -> %zu doc(s) err=\"%s\"\n",
                        npyShapeText(t.shape).c_str(), t.why, o.docs, o.err.c_str());
                check(good, ("V22b native refuses " + npyShapeText(t.shape)).c_str());
            }

            // ---- V22c: the refusal names the shape AND the accepted forms ----
            // §3.2. A refusal that does not say what WOULD have worked leaves
            // the reader to guess, which is the habit this whole change ends.
            {
                Opened o = openOne(npyOf("c_odd.npy", { 4, 8, 8, 8, 2 }));
                bool namesShape = o.err.find("(4, 8, 8, 8, 2)") != std::string::npos;
                bool namesForms = o.err.find("(H,W)") != std::string::npos &&
                                  o.err.find("(H,W,C<=4)") != std::string::npos &&
                                  o.err.find("(F,H,W)") != std::string::npos &&
                                  o.err.find("(F,H,W,C<=4)") != std::string::npos;
                fprintf(stderr, "verifyselftest: V22c refusal = \"%s\"\n", o.err.c_str());
                check(namesShape, "V22c the refusal names the shape that arrived");
                check(namesForms, "V22c the refusal names every form native reads");
            }

            // ---- V22d: the way out - say it again, per file (§3.3) -----------
            // (3,8,8) is the shape the old heuristic got wrong, so it is the one
            // that has to be sayable in BOTH directions. The pixels must be the
            // same bytes read a different way round: frame c of the stack is
            // plane c of the colour frame, or one of the two readings is lying.
            {
                std::string p = npyOf("d_fhw.npy", { 3, 8, 8 });
                closeAll();
                std::string e = loadNpy(p);
                bool asStack = e.empty() && app.images.size() == 3 &&
                               app.images[0]->ch == 1 && app.images[0]->w == 8 &&
                               app.images[0]->h == 8;
                // the menu offers only what this shape permits: (3,8,8) can be a
                // 3-frame stack or a plane-first colour frame, but NOT (H,W,C) -
                // that would be 8 channels, and it must not be on the menu
                std::vector<int64_t> shp = { 3, 8, 8 };
                std::vector<int> opts = npyReadOptions(shp);
                bool hasStack = false, hasChw = false, hasHwc = false;
                for (int r : opts) {
                    if (r == NR_STACK) hasStack = true;
                    if (r == NR_CHW)   hasChw = true;
                    if (r == NR_HWC)   hasHwc = true;
                }
                fprintf(stderr, "verifyselftest: V22d (3, 8, 8) read as \"%s\", %zu option(s):",
                        npyReadLabel(shp, NR_STACK).c_str(), opts.size());
                for (int r : opts) fprintf(stderr, " [%s]", npyReadLabel(shp, r).c_str());
                fprintf(stderr, "\n");
                check(asStack, "V22d (3,H,W) opens as three frames, not one 3ch image");
                check(hasStack && hasChw && !hasHwc && opts.size() == 2,
                      "V22d the menu is computed: (C,H,W) yes, (H,W,C) impossible");
                // what the three frames hold, to compare against the re-reading
                std::vector<float> plane[3];
                for (int f = 0; f < 3 && f < (int)app.images.size(); f++)
                    plane[f] = app.images[f]->src->data;
                // ...and what it costs, said BEFORE it happens
                std::string loss = npyReReadLoss(app.images[0].get());
                fprintf(stderr, "verifyselftest: V22d loss = \"%s\"\n", loss.c_str());
                check(loss.find("rebuilds") != std::string::npos &&
                      loss.find("3 frames") != std::string::npos,
                      "V22d the cost of re-reading is stated before it runs");

                std::string re = npyReReadAs(app.images[0].get(), NR_CHW);
                bool oneColour = re.empty() && app.images.size() == 1 &&
                                 app.images[0]->ch == 3 && app.images[0]->w == 8 &&
                                 app.images[0]->h == 8;
                bool samePixels = oneColour;
                if (oneColour) {
                    const FrameSource& S = *app.images[0]->src;
                    for (int c = 0; c < 3 && samePixels; c++)
                        for (int i = 0; i < 64 && samePixels; i++)
                            if (S.data[(size_t)i * 3 + c] != plane[c][i]) samePixels = false;
                }
                fprintf(stderr, "verifyselftest: V22d re-read as (C,H,W): %zu doc(s) %dch"
                                " err=\"%s\" samePixels=%d npyRead=%d\n",
                        app.images.size(), app.images.empty() ? 0 : app.images[0]->ch,
                        re.c_str(), (int)samePixels,
                        app.images.empty() ? -1 : app.images[0]->src->npyRead);
                check(oneColour, "V22d re-read as (C,H,W) gives ONE 3ch frame");
                check(samePixels, "V22d the re-reading is the same bytes, read round");
                check(!app.images.empty() && app.images[0]->src->npyRead == NR_CHW,
                      "V22d the declaration is remembered on the source");

                // ...and back. The reverse must return the stack it came from,
                // frame for frame - a one-way door is not a way out.
                std::string back = npyReReadAs(app.images[0].get(), NR_STACK);
                bool backOk = back.empty() && app.images.size() == 3 &&
                              app.images[0]->ch == 1;
                for (int f = 0; f < 3 && backOk; f++)
                    if (app.images[f]->src->data != plane[f]) backOk = false;
                fprintf(stderr, "verifyselftest: V22d back to (F,H,W): %zu doc(s) err=\"%s\"\n",
                        app.images.size(), back.c_str());
                check(backOk, "V22d re-reading back gives the same three frames");
            }

            // ---- V22e: the declaration survives a session round trip ---------
            // §3.3: "saved in the session, and it opens the same way next time".
            // An ADDITIVE key: a viewer that predates it skips the line and gets
            // the native reading, which is what it would have done anyway.
            {
                std::string p = npyOf("e_fhw.npy", { 3, 8, 8 });
                closeAll();
                loadNpy(p);
                std::string re = npyReReadAs(app.images[0].get(), NR_CHW);
                std::string sess = (vdir / "reread.vsession").u8string();
                saveSession(sess, true);
                std::string body;
                {
                    std::ifstream sf(pathFromUtf8(sess));
                    std::stringstream ss2; ss2 << sf.rdbuf(); body = ss2.str();
                }
                bool wrote = body.find("npyread " + std::to_string((int)NR_CHW)) !=
                             std::string::npos;
                bool noGlobal = body.find("npyaxis") == std::string::npos;
                closeAll();
                std::string lerr2 = loadSession(sess);
                bool restored = lerr2.empty() && app.images.size() == 1 &&
                                app.images[0]->ch == 3 &&
                                app.images[0]->src->npyRead == NR_CHW;
                fprintf(stderr, "verifyselftest: V22e session: wrote npyread=%d, npyaxis gone=%d,"
                                " restored %zu doc(s) %dch read=%d err=\"%s\"\n",
                        (int)wrote, (int)noGlobal, app.images.size(),
                        app.images.empty() ? 0 : app.images[0]->ch,
                        app.images.empty() ? -1 : app.images[0]->src->npyRead, lerr2.c_str());
                check(re.empty() && wrote, "V22e the session records the reading, per image");
                check(noGlobal, "V22e the global npyaxis line is gone from sessions");
                check(restored, "V22e reopening reads it the way it was declared");
            }

            // ---- V22f: opening one .npy file reads it ONCE -------------------
            // loadNpy used to own a copy of loadNpyBuffer's frame loop calling
            // decodeNpyFrame, and decodeNpyFrame reads the whole file EVERY time.
            // Opening a 12-frame file read it 12 times; at 120 frames and 252 MB
            // that was 12.2 s of pure re-reading. Nothing caught it because the
            // pictures were right - it was only slow, and slow has no assertion
            // unless someone writes one.
            {
                std::string mpath = npyOf("reads_once.npy", { 12, 8, 8 });
                closeAll();
                long long r0 = g_fileReads;
                std::string merr = loadNpy(mpath);
                long long reads = g_fileReads - r0;
                int got = (int)app.images.size();
                fprintf(stderr, "verifyselftest: V22f (12,8,8): err=\"%s\" %d frame(s),"
                                " %lld whole-file read(s)\n", merr.c_str(), got, reads);
                check(merr.empty() && got == 12, "V22f the 12-frame file opens as 12 frames");
                check(reads == 1, "V22f ...having been read from disk exactly once");

                // The Watch baseline is what decodeNpyFrame used to stamp per
                // frame. The buffer path cannot stamp it (for an .npz member the
                // file is the container), so the file path has to, and every
                // frame needs it - not just the one that opened the stack.
                int stamped = 0, total = 0;
                for (const auto& d : app.images)
                    if (d->src) { total++; if (d->src->fsize > 0) stamped++; }
                fprintf(stderr, "verifyselftest: V22f Watch baseline on %d of %d frame(s)\n",
                        stamped, total);
                check(total == 12 && stamped == total,
                      "V22f every frame of the stack carries the Watch baseline");
            }

            std::filesystem::remove_all(vdir, vec);
            closeAll();
        }

        // ---- V25: an npz that declares its own layers (docs/input-adapters.md §4.11)
        // The container is built BY HAND here, with no Python involved, because
        // what is under test is the READER: given the reserved members, does the
        // viewer build the layers they declare, and does it put the conditions on
        // the SERIES rather than on the stacks. Python enters only where the
        // subject is actually the harness or the cache (V25h/V25i), and those
        // skip out loud rather than failing when it is absent.
        {
            std::error_code aec;
            std::filesystem::path adir = vfyScratch("vfy-adapter-scratch");

            auto npyBytes = [](const char* descr, const std::vector<int64_t>& shape,
                               const void* data, size_t nbytes) {
                std::string sh;
                if (shape.empty()) sh = "()";
                else if (shape.size() == 1) sh = "(" + std::to_string(shape[0]) + ",)";
                else {
                    sh = "(";
                    for (size_t i = 0; i < shape.size(); i++)
                        sh += std::to_string(shape[i]) + (i + 1 < shape.size() ? ", " : "");
                    sh += ")";
                }
                std::string hdr = std::string("{'descr': '") + descr +
                                  "', 'fortran_order': False, 'shape': " + sh + ", }";
                hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
                hdr += '\n';
                std::vector<uint8_t> out;
                const char magic[8] = { '\x93', 'N', 'U', 'M', 'P', 'Y', '\x01', '\x00' };
                out.insert(out.end(), magic, magic + 8);
                out.push_back((uint8_t)(hdr.size() & 0xff));
                out.push_back((uint8_t)(hdr.size() >> 8));
                out.insert(out.end(), hdr.begin(), hdr.end());
                const uint8_t* p = (const uint8_t*)data;
                out.insert(out.end(), p, p + nbytes);
                return out;
            };
            // numpy writes a str_ scalar as <U<n> in UCS-4; the reader has to
            // take them exactly as numpy lays them down, so the fixture does too.
            auto strNpy = [&](const std::string& s) {
                size_t item = s.empty() ? 1 : s.size();
                std::vector<uint32_t> cps(item, 0);
                for (size_t i = 0; i < s.size(); i++) cps[i] = (uint32_t)(unsigned char)s[i];
                return npyBytes(("<U" + std::to_string(item)).c_str(), {},
                                cps.data(), cps.size() * 4);
            };
            auto i32Npy = [&](int32_t v) { return npyBytes("<i4", {}, &v, 4); };
            auto f64Npy = [&](const std::vector<double>& v) {
                return npyBytes("<f8", { (int64_t)v.size() }, v.data(), v.size() * 8);
            };
            struct ZipMem2 {
                std::string name; std::vector<uint8_t> bytes;
                size_t usize; uint16_t method; uint32_t crc;
            };
            auto writeZip = [](const std::string& path, std::vector<ZipMem2> mem) {
                std::vector<uint8_t> z;
                auto p16 = [&](uint32_t x) {
                    z.push_back((uint8_t)(x & 0xff)); z.push_back((uint8_t)((x >> 8) & 0xff));
                };
                auto p32 = [&](uint32_t x) {
                    for (int i = 0; i < 4; i++) z.push_back((uint8_t)((x >> (8 * i)) & 0xff));
                };
                std::vector<uint32_t> offs;
                for (const auto& m : mem) {
                    offs.push_back((uint32_t)z.size());
                    p32(0x04034b50); p16(20); p16(0); p16(m.method); p16(0); p16(0);
                    p32(m.crc); p32((uint32_t)m.bytes.size()); p32((uint32_t)m.usize);
                    p16((uint32_t)m.name.size()); p16(0);
                    z.insert(z.end(), m.name.begin(), m.name.end());
                    z.insert(z.end(), m.bytes.begin(), m.bytes.end());
                }
                uint32_t cdOff = (uint32_t)z.size();
                for (size_t i = 0; i < mem.size(); i++) {
                    p32(0x02014b50); p16(20); p16(20); p16(0); p16(mem[i].method);
                    p16(0); p16(0);
                    p32(mem[i].crc); p32((uint32_t)mem[i].bytes.size());
                    p32((uint32_t)mem[i].usize);
                    p16((uint32_t)mem[i].name.size()); p16(0); p16(0);
                    p16(0); p16(0); p32(0); p32(offs[i]);
                    z.insert(z.end(), mem[i].name.begin(), mem[i].name.end());
                }
                uint32_t cdSize = (uint32_t)z.size() - cdOff;
                p32(0x06054b50); p16(0); p16(0);
                p16((uint32_t)mem.size()); p16((uint32_t)mem.size());
                p32(cdSize); p32(cdOff); p16(0);
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write((const char*)z.data(), (std::streamsize)z.size());
            };
            auto writeNpz2 = [&](const std::string& path,
                                 const std::vector<std::pair<std::string,
                                                             std::vector<uint8_t>>>& mem) {
                std::vector<ZipMem2> zm;
                for (const auto& m : mem)
                    zm.push_back({ m.first + ".npy", m.second, m.second.size(), 0,
                                   (uint32_t)mz_crc32(MZ_CRC32_INIT, m.second.data(),
                                                      m.second.size()) });
                writeZip(path, std::move(zm));
            };

            // Two stacks of three frames each, swept over exposure. Pixel values
            // carry their (stack, frame) so a mis-wired tree shows up as numbers.
            std::vector<uint16_t> pxA((size_t)3 * 8 * 8), pxB((size_t)3 * 8 * 8);
            for (int f = 0; f < 3; f++)
                for (int i = 0; i < 64; i++) {
                    pxA[(size_t)f * 64 + i] = (uint16_t)(100 + f);
                    pxB[(size_t)f * 64 + i] = (uint16_t)(200 + f);
                }
            const double COND[2] = { 1.5, 40.25 };
            const double TS[3] = { 0.0, 0.5, 1.0 };
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",              i32Npy(1) },
                { "__n",                   i32Npy(3) },
                { "__layer_0",             strNpy("series") },
                { "__parent_0",            i32Npy(-1) },
                { "__name_0",              strNpy("exposure sweep") },
                { "__note_0",              strNpy("") },
                { "__conditions_values_0", f64Npy({ COND[0], COND[1] }) },
                { "__conditions_name_0",   strNpy("exposure") },
                { "__conditions_unit_0",   strNpy("ms") },
                { "__meta_sensor",         strNpy("\"IMX999\"") },
                { "__layer_1",             strNpy("stack") },
                { "__parent_1",            i32Npy(0) },
                { "__name_1",              strNpy("e1") },
                { "__note_1",              strNpy("first point") },
                { "__pixels_1",            npyBytes("<u2", { 3, 8, 8 }, pxA.data(), pxA.size() * 2) },
                { "__cfa_1",               strNpy("RGGB") },
                { "__timestamps_values_1", f64Npy({ TS[0], TS[1], TS[2] }) },
                { "__timestamps_name_1",   strNpy("time") },
                { "__timestamps_unit_1",   strNpy("s") },
                { "__layer_2",             strNpy("stack") },
                { "__parent_2",            i32Npy(0) },
                { "__name_2",              strNpy("e2") },
                { "__note_2",              strNpy("") },
                { "__pixels_2",            npyBytes("<u2", { 3, 8, 8 }, pxB.data(), pxB.size() * 2) },
            };
            std::string cpath = (adir / "container.npz").u8string();
            writeNpz2(cpath, cm);

            closeAll();
            std::string cerr = loadNpz(cpath);
            int cSeqs = (int)app.seqs.size();
            int cSeries = (int)app.series.size();
            fprintf(stderr, "verifyselftest: V25 container: err=\"%s\" %zu doc(s) %d stack(s) "
                            "%d series picker=%d\n",
                    cerr.c_str(), app.images.size(), cSeqs, cSeries, (int)app.npzPickOpen);
            check(cerr.empty() && !app.npzPickOpen,
                  "V25a a container opens without the member picker");
            check(app.images.size() == 6 && cSeqs == 2 && cSeries == 1,
                  "V25a the declared tree becomes 1 series, 2 stacks, 6 frames");

            // §4.5: the condition belongs to the SERIES. If it lands on the
            // stacks instead, sigma_t across a sweep becomes computable again,
            // which is the exact failure the layer split exists to prevent.
            bool condOk = false, condOffStacks = true;
            if (cSeries == 1) {
                const App::Series& s = app.series.front();
                condOk = s.paramName == "exposure" && std::string(s.unit) == "ms" &&
                         s.members.size() == 2 &&
                         s.members[0].value == COND[0] && s.members[1].value == COND[1];
                fprintf(stderr, "verifyselftest: V25 series: param=\"%s\" unit=\"%s\" "
                                "members=%zu v0=%g v1=%g\n",
                        s.paramName.c_str(), s.unit, s.members.size(),
                        s.members.empty() ? -1 : s.members[0].value,
                        s.members.size() < 2 ? -1 : s.members[1].value);
            }
            for (const auto& sq : app.seqs)
                if (sq.axisName == "exposure") condOffStacks = false;
            check(condOk, "V25b the conditions land on the series, with name and unit");
            check(condOffStacks, "V25b the conditions are NOT copied onto the stacks");

            // timestamps are the stack's own axis and must reach the same field
            // the paste path and the .npz picker write, or the panels see nothing
            bool tsOk = false;
            for (const auto& sq : app.seqs)
                if (sq.axisVals.size() == 3 && sq.axisName == "time" && sq.axisUnit == "s" &&
                    sq.axisVals[1] == 0.5)
                    tsOk = true;
            check(tsOk, "V25c timestamps land on the stack as its frame axis");

            bool cfaOk = false, noteOk = false;
            for (const auto& d : app.images) {
                if (d->cfa == 1 && d->cfaPattern == 0) cfaOk = true;
                if (d->note.find("first point") != std::string::npos) noteOk = true;
            }
            check(cfaOk, "V25d the declared CFA reaches the frames");
            check(noteOk, "V25d the declared note reaches the frames");

            // §4.11.1: without __viewer this is an ordinary npz and the shape
            // classifier owns it, untouched. Same bytes, one member removed.
            {
                std::vector<std::pair<std::string, std::vector<uint8_t>>> pm;
                for (const auto& m : cm) if (m.first != "__viewer") pm.push_back(m);
                std::string ppath = (adir / "plain.npz").u8string();
                writeNpz2(ppath, pm);
                closeAll();
                std::string perr = loadNpz(ppath);
                bool old = app.npzPickOpen || !perr.empty();
                fprintf(stderr, "verifyselftest: V25e plain: err=\"%s\" picker=%d series=%zu\n",
                        perr.c_str(), (int)app.npzPickOpen, app.series.size());
                check(old && app.series.empty(),
                      "V25e without __viewer the plain-npz path still owns the file");
                app.npzPickOpen = false;
                app.npzPick.clear();
            }

            // §4.3.2: no unit means not applied - it does NOT mean discarded.
            {
                std::vector<std::pair<std::string, std::vector<uint8_t>>> um;
                for (const auto& m : cm)
                    um.push_back(m.first == "__conditions_unit_0"
                                     ? std::make_pair(m.first, strNpy("")) : m);
                std::string upath = (adir / "nounit.npz").u8string();
                writeNpz2(upath, um);
                closeAll();
                std::string uerr = loadNpz(upath);
                bool kept = app.series.size() == 1 &&
                            app.series.front().members.size() == 2 &&
                            app.series.front().members[0].value == COND[0] &&
                            app.series.front().members[1].value == COND[1] &&
                            std::string(app.series.front().unit).empty();
                fprintf(stderr, "verifyselftest: V25f no unit: err=\"%s\" series=%zu "
                                "unit=\"%s\" v0=%g\n",
                        uerr.c_str(), app.series.size(),
                        app.series.empty() ? "?" : app.series.front().unit,
                        app.series.empty() || app.series.front().members.empty()
                            ? -1 : app.series.front().members[0].value);
                check(kept, "V25f a unit-less condition keeps its values, unit left empty");
            }

            // §4.9 / stage 6: a broken container names what is broken. "could not
            // open" with no reason is the refusal this whole document exists to
            // stop, and a container is refused by the same standard as an adapter.
            {
                std::vector<std::pair<std::string, std::vector<uint8_t>>> bm;
                for (const auto& m : cm) if (m.first != "__layer_2") bm.push_back(m);
                std::string bpath = (adir / "broken.npz").u8string();
                writeNpz2(bpath, bm);
                closeAll();
                std::string berr = loadNpz(bpath);
                fprintf(stderr, "verifyselftest: V25g broken: err=\"%s\"\n", berr.c_str());
                check(!berr.empty() && berr.find("__layer_2") != std::string::npos,
                      "V25g a malformed container names the member that is wrong");
            }

            // ---- V25h/V25i: the harness, and the cache around it -------------
            // These are the only parts whose SUBJECT is Python. CI runners
            // differ, so a missing interpreter skips them out loud rather than
            // failing: a red line that means "this machine has no numpy" would
            // train everyone to ignore red lines.
            // The user's own prefs must survive this: remembering a reader
            // writes prefs.txt, and these paths are scratch. Snapshot, then put
            // it back exactly as it was. The bookmark lists come along because
            // loadPrefs APPENDS to them - reading prefs twice (which V25k does
            // on purpose) would otherwise write the user's bookmarks back
            // doubled, and a selftest that corrupts real settings is worse than
            // no selftest.
            std::vector<App::ReaderMemo> memoSnapshot = app.readerMemo;
            std::vector<std::string> bmSnapshot = app.rbBookmarks;
            std::vector<std::string> rcSnapshot = app.rbRecents;
            std::vector<std::string> placeSnapshot = app.readerPlaces;   // §4.13.2
            {
                std::string pyWhy;
                std::string py = adapter::findPython(app.pythonExe, pyWhy);
                std::string script = runAdapterScript();
                if (py.empty() || script.empty()) {
                    fprintf(stderr, "verifyselftest: V25h SKIPPED - %s\n",
                            py.empty() ? pyWhy.c_str()
                                       : "tools/import/run_adapter.py not found");
                } else {
                    // a plain npz for the adapter to read, and an adapter to read it
                    std::vector<uint16_t> raw((size_t)2 * 3 * 8 * 8);
                    for (size_t i = 0; i < raw.size(); i++) raw[i] = (uint16_t)(i % 500);
                    const double EXP2[2] = { 10.0, 20.0 };
                    std::string srcPath = (adir / "sweep_src.npz").u8string();
                    writeNpz2(srcPath, {
                        { "data", npyBytes("<u2", { 2, 3, 8, 8 }, raw.data(), raw.size() * 2) },
                        { "exp",  npyBytes("<f8", { 2 }, EXP2, sizeof EXP2) },
                    });
                    std::string pyPath = (adir / "vfy_reader.py").u8string();
                    auto writeReader = [&](const char* version) {
                        std::ofstream pf(pathFromUtf8(pyPath), std::ios::binary);
                        pf << "import numpy as np\n"
                           << "from viewer_import import Series, Stack, Values\n"
                           << "VERSION = " << version << "\n"
                           << "def load(path):\n"
                           << "    z = np.load(path)\n"
                           << "    return Series([Stack(z['data'][i]) for i in range(2)],\n"
                           << "                  conditions=Values(z['exp'], 'exposure', 'ms'))\n";
                    };
                    writeReader("1");
                    std::string spec = pyPath + ":load";

                    closeAll();
                    int before = g_adapterRuns;
                    std::string herr = openWithReader(srcPath, spec);
                    int ranFirst = g_adapterRuns - before;
                    bool built = herr.empty() && app.series.size() == 1 &&
                                 app.series.front().members.size() == 2 &&
                                 std::string(app.series.front().unit) == "ms" &&
                                 app.series.front().members[1].value == EXP2[1] &&
                                 app.images.size() == 6;
                    fprintf(stderr, "verifyselftest: V25h harness: err=\"%s\" runs=%d "
                                    "%zu doc(s) %zu series unit=\"%s\"\n",
                            herr.c_str(), ranFirst, app.images.size(), app.series.size(),
                            app.series.empty() ? "?" : app.series.front().unit);
                    check(built, "V25h the harness's npz becomes the layers it declares");
                    check(ranFirst == 1, "V25h the reader ran exactly once");

                    // second open: same everything, so Python must NOT start
                    closeAll();
                    int before2 = g_adapterRuns;
                    std::string h2 = openWithReader(srcPath, spec);
                    int ranSecond = g_adapterRuns - before2;
                    fprintf(stderr, "verifyselftest: V25i cache: err=\"%s\" runs=%d %zu doc(s)\n",
                            h2.c_str(), ranSecond, app.images.size());
                    check(h2.empty() && app.images.size() == 6 && ranSecond == 0,
                          "V25i a second open is served from cache, Python not started");

                    // edit the adapter: the key carries its mtime, so it re-runs
                    writeReader("1");
                    std::error_code tec;
                    auto now = std::filesystem::last_write_time(pathFromUtf8(pyPath), tec);
                    std::filesystem::last_write_time(pathFromUtf8(pyPath),
                                                     now + std::chrono::seconds(5), tec);
                    closeAll();
                    int before3 = g_adapterRuns;
                    std::string h3 = openWithReader(srcPath, spec);
                    int ranThird = g_adapterRuns - before3;
                    fprintf(stderr, "verifyselftest: V25i touched: err=\"%s\" runs=%d\n",
                            h3.c_str(), ranThird);
                    check(h3.empty() && ranThird == 1,
                          "V25i editing the reader makes the next open run it again");

                    // §4.9: the adapter's own words reach the user unchanged
                    {
                        std::ofstream pf(pathFromUtf8(pyPath), std::ios::binary);
                        pf << "def load(path):\n"
                           << "    raise ValueError('header says 12 planes but the file holds 9')\n";
                    }
                    closeAll();
                    std::string berr = openWithReader(srcPath, spec);
                    fprintf(stderr, "verifyselftest: V25j refusal: \"%s\"\n",
                            berr.substr(0, 200).c_str());
                    check(berr.find("header says 12 planes but the file holds 9") !=
                              std::string::npos,
                          "V25j a failing reader's own message reaches the user");
                    // §4.13.0: the panel gets the traceback WHOLE. Folding it
                    // into one line would remove the file and line that raised,
                    // which is the only thing the author can act on.
                    int outLines = 1;
                    for (char c : app.readerLastOut) if (c == '\n') outLines++;
                    bool whole = app.readerLastOut.find("Traceback") != std::string::npos &&
                                 app.readerLastOut.find("vfy_reader.py") != std::string::npos &&
                                 app.readerLastOut.find("header says 12 planes") !=
                                     std::string::npos &&
                                 outLines >= 3;
                    fprintf(stderr, "verifyselftest: V25j panel holds %d line(s), "
                                    "traceback=%d file=%d\n", outLines,
                            (int)(app.readerLastOut.find("Traceback") != std::string::npos),
                            (int)(app.readerLastOut.find("vfy_reader.py") != std::string::npos));
                    check(whole && !app.readerLastOk,
                          "V25j the panel keeps the whole traceback, file and line");

                    // ---- V25l: the loop, in one assert -----------------------
                    // §4.13.0: write, load, read the failure, FIX, load again.
                    // The third press must actually re-run - a cache that
                    // served the previous answer here would kill the feature,
                    // because the author would be staring at a stale traceback
                    // for a line they already fixed.
                    writeReader("1");                 // the fix
                    auto fixedAt = std::filesystem::last_write_time(pathFromUtf8(pyPath), tec);
                    std::filesystem::last_write_time(pathFromUtf8(pyPath),
                                                     fixedAt + std::chrono::seconds(11), tec);
                    closeAll();
                    int before4 = g_adapterRuns;
                    std::string h4 = openWithReader(srcPath, spec);
                    int ranFourth = g_adapterRuns - before4;
                    fprintf(stderr, "verifyselftest: V25l loop: err=\"%s\" runs=%d ok=%d "
                                    "%zu doc(s)\n",
                            h4.c_str(), ranFourth, (int)app.readerLastOk, app.images.size());
                    check(h4.empty() && ranFourth == 1 && app.readerLastOk &&
                              app.images.size() == 6,
                          "V25l fixing the reader and loading again re-runs it and reads");

                    // ---- V25m: an adapter-read file survives a session -------
                    // The docs are named after the SOURCE, not the cache file,
                    // so the session records the source - and the restore has to
                    // route back through the reader, because the member it
                    // recorded (__pixels_1) exists only in what the reader makes.
                    {
                        std::string asess = (adir / "adapter.vsession").u8string();
                        saveSession(asess, true);
                        std::string body;
                        {
                            std::ifstream sf(pathFromUtf8(asess));
                            std::stringstream s2; s2 << sf.rdbuf(); body = s2.str();
                        }
                        bool namesSource = body.find("sweep_src.npz") != std::string::npos;
                        closeAll();
                        std::string serr = loadSession(asess);
                        loadAll();
                        bool back = serr.empty() && app.images.size() == 6 &&
                                    app.series.size() == 1 &&
                                    app.series.front().members.size() == 2;
                        fprintf(stderr, "verifyselftest: V25m session: names source=%d "
                                        "err=\"%s\" %zu doc(s) %zu series\n",
                                (int)namesSource, serr.c_str(), app.images.size(),
                                app.series.size());
                        check(namesSource,
                              "V25m the session records the source file, not the cache");
                        check(back, "V25m reopening the session reads it through the reader");
                    }

                    // ---- V25n: a reader found through a REGISTERED place -----
                    // §4.13.2 / issue #51, end to end and with Python actually
                    // starting: the spec names no path at all, so the only way
                    // this file can be located is the registered place. Before
                    // registration existed, a bare name reached run_adapter.py
                    // and died there as an import error about a module nobody
                    // could point at.
                    {
                        std::vector<std::string> ps = app.readerPlaces;
                        app.readerPlaces.clear();
                        std::string bare = "vfy_reader.py:load";
                        closeAll();
                        std::string unreg = openWithReader(srcPath, bare);
                        app.readerPlaces.push_back(dispPath(adir.u8string()));
                        closeAll();
                        std::string reg = openWithReader(srcPath, bare);
                        fprintf(stderr, "verifyselftest: V25n bare spec: unregistered=\"%s\" "
                                        "registered err=\"%s\" %zu doc(s)\n",
                                unreg.substr(0, 120).c_str(), reg.c_str(), app.images.size());
                        check(!unreg.empty() &&
                                  unreg.find("no registered place") != std::string::npos,
                              "V25n an unregistered reader is refused, by name");
                        check(reg.empty() && app.images.size() == 6,
                              "V25n the same reader, registered, is found and read");
                        app.readerPlaces = ps;
                    }
                }
            }

            // ---- V25k: the memory survives being written and read back -------
            {
                std::string fake = (adir / "remembered.dat").u8string();
                rememberReader(fake, "acme.py:load");
                bool hasIt = readerFor(fake) == "acme.py:load";
                app.readerMemo.clear();
                loadPrefs();
                bool survived = readerFor(fake) == "acme.py:load";
                // §4.12: bounded. The cap holds on the way in as well as out.
                for (int i = 0; i < 80; i++)
                    rememberReader((adir / ("f" + std::to_string(i) + ".dat")).u8string(),
                                   "acme.py:load");
                bool capped = app.readerMemo.size() == READER_MEMO_MAX;
                // most recent first: the last one added is still known, the
                // first one added has been dropped
                bool lru = !readerFor((adir / "f79.dat").u8string()).empty() &&
                           readerFor((adir / "f0.dat").u8string()).empty();
                app.readerMemo.clear();
                loadPrefs();
                bool cappedOnRead = app.readerMemo.size() <= READER_MEMO_MAX;
                fprintf(stderr, "verifyselftest: V25k memo: had=%d survived=%d size=%zu "
                                "capped=%d lru=%d\n",
                        (int)hasIt, (int)survived, app.readerMemo.size(),
                        (int)capped, (int)lru);
                check(hasIt && survived,
                      "V25k the per-file reader survives a prefs round trip");
                check(capped && cappedOnRead, "V25k the memory is bounded at 64 (LRU)");
                check(lru, "V25k the least recently chosen entry is the one dropped");
            }
            // ---- V25o: the search path itself (§4.13.2, issue #51) -----------
            // Everything here is filesystem and string work, so it runs on every
            // machine whether or not V25h found a Python. Four questions, and
            // the first three are the ones the issue asked: is a reader in a
            // SECOND registered place found, does an unregistered one fail
            // LEGIBLY (naming every place looked in), and is a place that has
            // gone away reported rather than dropped.
            {
                std::vector<std::string> psave = app.readerPlaces;
                std::error_code pec;
                std::filesystem::path pA = adir / "placeA", pB = adir / "placeB",
                                      pL = adir / "lone",   pG = adir / "ghost";
                for (const auto& d : { pA, pB, pL, pG })
                    std::filesystem::create_directories(d, pec);
                auto put = [&](const std::filesystem::path& p) {
                    std::ofstream f(pathFromUtf8(p.u8string()), std::ios::binary);
                    f << "def load(path):\n    return None\n";
                };
                put(pA / "alpha.py");
                put(pB / "alpha.py");            // the same NAME in two places
                put(pB / "beta.py");
                put(pL / "solo.py");
                std::string dA = dispPath(pA.u8string()), dB = dispPath(pB.u8string()),
                            dG = dispPath(pG.u8string()),
                            dSolo = dispPath((pL / "solo.py").u8string());

                app.readerPlaces.clear();
                std::string rwhy;
                bool addedA = registerReaderPlace(pA.u8string(), &rwhy);
                bool addedB = registerReaderPlace(pB.u8string(), &rwhy);
                bool addedF = registerReaderPlace((pL / "solo.py").u8string(), &rwhy);
                bool dupRefused = !registerReaderPlace(pA.u8string(), &rwhy);
                bool ghostRefused = !registerReaderPlace((adir / "never").u8string(), &rwhy);

                // 1. the SECOND place is searched at all - the whole defect
                ReaderLookup beta = resolveReaderFile("beta.py:load");
                check(addedA && addedB && addedF && dupRefused && ghostRefused,
                      "V25o a folder and a file register once each; a path that is not "
                      "there is refused");
                fprintf(stderr, "verifyselftest: V25o beta -> \"%s\"\n",
                        dispPath(beta.file).c_str());
                check(dispPath(beta.file) == dispPath((pB / "beta.py").u8string()),
                      "V25o a reader in the SECOND registered place is found");
                // a FILE is a place too (the ruling said file OR folder)
                ReaderLookup solo = resolveReaderFile("solo.py:load");
                check(dispPath(solo.file) == dSolo,
                      "V25o a registered FILE offers its one reader");

                // 2. the refusal names every place it looked in - a list, not a
                //    single guess. The shipped folder is on that list too.
                ReaderLookup none = resolveReaderFile("nowhere.py:load");
                std::string rep = none.why;
                bool listsAll = rep.find(dA) != std::string::npos &&
                                rep.find(dB) != std::string::npos &&
                                rep.find(dSolo) != std::string::npos;
                std::string script = runAdapterScript();
                if (!script.empty())
                    listsAll = listsAll &&
                               rep.find(dispPath((pathFromUtf8(script).parent_path() /
                                                  "adapters").u8string())) != std::string::npos;
                fprintf(stderr, "verifyselftest: V25o refusal:\n%s\n", rep.c_str());
                check(none.file.empty() && listsAll &&
                          rep.find("no registered place") != std::string::npos,
                      "V25o an unregistered reader is refused and every place looked in "
                      "is named");

                // 3. a registered place that has gone away is REPORTED
                registerReaderPlace(pG.u8string(), &rwhy);
                std::filesystem::remove_all(pG, pec);
                std::string ghostRep = readerPlacesReport(readerSearchPath());
                bool ghostSaid = ghostRep.find(dG) != std::string::npos &&
                                 ghostRep.find("MISSING") != std::string::npos;
                fprintf(stderr, "verifyselftest: V25o after the folder went away:\n%s\n",
                        ghostRep.c_str());
                check(ghostSaid,
                      "V25o a registered place that no longer exists is reported, not dropped");
                bool stillListed = app.readerPlaces.size() == 4;

                // 4. the ORDER is the setting: first match wins, the loser is
                //    named, and moving the place moves the answer.
                ReaderLookup a1 = resolveReaderFile("alpha.py:load");
                bool wonA = dispPath(a1.file) == dispPath((pA / "alpha.py").u8string()) &&
                            a1.shadowed.size() == 1 &&
                            dispPath(a1.shadowed[0]) == dispPath((pB / "alpha.py").u8string());
                moveReaderPlace(1, -1);                       // B above A
                ReaderLookup a2 = resolveReaderFile("alpha.py:load");
                bool wonB = dispPath(a2.file) == dispPath((pB / "alpha.py").u8string());
                fprintf(stderr, "verifyselftest: V25o alpha: first=\"%s\" shadowed=%zu "
                                "after reorder=\"%s\"\n",
                        dispPath(a1.file).c_str(), a1.shadowed.size(),
                        dispPath(a2.file).c_str());
                check(wonA, "V25o the first place holding a name wins, and the loser is named");
                check(wonB, "V25o reordering the places changes which one wins");

                // 5. the places survive a prefs round trip, in order - and the
                //    single folder they replaced is read as the first place, so
                //    an existing prefs.txt keeps finding what it found.
                savePrefs();
                std::vector<std::string> wrote = app.readerPlaces;
                app.readerPlaces.clear();
                loadPrefs();
                bool roundTrip = app.readerPlaces == wrote;
                {
                    std::string pp = prefsPath();
                    std::string body;
                    { std::ifstream pf(pathFromUtf8(pp)); std::stringstream s2;
                      s2 << pf.rdbuf(); body = s2.str(); }
                    bool writesPlaces = body.find("readerplace " + dB) != std::string::npos;
                    bool writesOldKey = body.find("readerdir ") != std::string::npos;
                    // the migration, exercised: an old key, read once, becomes a place
                    std::ofstream of(pathFromUtf8(pp), std::ios::binary | std::ios::app);
                    of << "readerdir " << dispPath((adir / "legacy").u8string()) << "\n";
                    of.close();
                    app.readerPlaces.clear();
                    loadPrefs();
                    bool migrated = !app.readerPlaces.empty() &&
                                    app.readerPlaces.back() ==
                                        dispPath((adir / "legacy").u8string());
                    fprintf(stderr, "verifyselftest: V25o prefs: roundtrip=%d places=%d "
                                    "oldkey=%d migrated=%d listed=%d\n",
                            (int)roundTrip, (int)writesPlaces, (int)writesOldKey,
                            (int)migrated, (int)stillListed);
                    check(roundTrip && writesPlaces && !writesOldKey && stillListed,
                          "V25o the places survive a prefs round trip, in order, under "
                          "the new key");
                    check(migrated, "V25o the single folder they replaced is read as a place");
                }

                // 6. registration cannot change how a remembered file opens.
                //    The memory holds a PATH, and a path never goes through the
                //    search - so adding a place is additive by construction.
                {
                    std::string data = (adir / "kept.dat").u8string();
                    std::string mine = (adir / "mine_alpha.py").u8string();
                    put(pathFromUtf8(mine));
                    rememberReader(data, mine + ":load");
                    app.readerPlaces.clear();
                    registerReaderPlace(pA.u8string(), &rwhy);
                    ReaderLookup kept = resolveReaderFile(readerFor(data));
                    bool unchanged = dispPath(kept.file) == dispPath(mine);
                    // ...and when the remembered reader is GONE, it is reported,
                    // never swapped for a same-named file that happens to be
                    // registered. §4.13: only an explicitly chosen reader runs.
                    put(pA / "mine_alpha.py");
                    std::filesystem::remove(pathFromUtf8(mine), pec);
                    ReaderLookup gone = resolveReaderFile(readerFor(data));
                    bool reported = gone.file.empty() &&
                                    gone.why.find("not there any more") != std::string::npos &&
                                    gone.why.find(dispPath(
                                        (pA / "mine_alpha.py").u8string())) != std::string::npos;
                    fprintf(stderr, "verifyselftest: V25o memory: unchanged=%d gone:\n%s\n",
                            (int)unchanged, gone.why.c_str());
                    check(unchanged,
                          "V25o registering a place does not change a remembered reader");
                    check(reported,
                          "V25o a remembered reader that is gone is reported, and a "
                          "same-named registered one is offered, not substituted");
                    forgetReader(data);
                }
                app.readerPlaces = psave;
            }

            app.readerMemo = memoSnapshot;      // the user's prefs, as we found them
            app.rbBookmarks = bmSnapshot;
            app.rbRecents = rcSnapshot;
            app.readerPlaces = placeSnapshot;
            savePrefs();

            std::filesystem::remove_all(adir, aec);
            closeAll();
        }

        // ---- V26: the Messages panel past its own cap -------------------------
        // msgLog holds 300 and then drops from the front, so its SIZE is 300 for
        // message 300 and for message 3000 alike. The panel used to key its
        // rebuilt text on that size, so it stopped updating at the cap - it went
        // blind precisely when messages were arriving fastest. The probe walks
        // past the cap and asks whether the newest line is actually reachable.
        {
            std::vector<App::Msg> logSnapshot = app.msgLog;
            size_t seqSnapshot = app.msgSeq;
            app.msgLog.clear();

            size_t seqAt300 = 0;
            for (int i = 0; i < 400; i++) {
                logMsg("probe message " + std::to_string(i));
                if (i == 299) seqAt300 = app.msgSeq;
            }
            check(app.msgLog.size() == 300, "V26 the log is capped at 300");
            check(app.msgSeq > seqAt300,
                  "V26 the change counter keeps moving after the cap (size does not)");
            check(app.msgLog.back().text == "probe message 399",
                  "V26 the newest message is the one at the back");
            check(app.msgLog.front().text == "probe message 100",
                  "V26 the oldest 100 were dropped from the front, in order");

            // The panel's own cache key, exercised the way the panel uses it.
            size_t builtFrom = app.msgSeq;
            logMsg("one more");
            check(builtFrom != app.msgSeq,
                  "V26 one more message invalidates the panel's cache");

            // Consecutive duplicates collapse; a repeat after something else does not.
            size_t beforeDup = app.msgSeq;
            logMsg("one more");
            check(beforeDup == app.msgSeq, "V26 an immediate repeat is not logged twice");
            logMsg("something else");
            logMsg("one more");
            check(app.msgLog.back().text == "one more" && app.msgSeq > beforeDup,
                  "V26 the same text after another message is logged again");

            app.msgLog = logSnapshot;
            app.msgSeq = seqSnapshot + 1;       // the panel must rebuild after all that
        }

        // ---- V23: stage-2 sharing (docs/reference-design.md §6.2/§7/§4/§3.1).
        // The first real sharing trigger: the SAME folder opened twice maps the
        // same sources - srcId equal, resident bytes 1x, pixels outlive one
        // side's close (and the close says so), Ctrl+Alt+W stays membership-only.
        reload();
        {
            const size_t bytes1 = residentImageBytes();
            const int docs1 = (int)app.images.size();
            std::vector<int> batches1;
            for (const auto& d : app.images)
                if (std::find(batches1.begin(), batches1.end(), d->batchId) == batches1.end())
                    batches1.push_back(d->batchId);
            auto inFirst = [&](int b) {
                return std::find(batches1.begin(), batches1.end(), b) != batches1.end();
            };
            openFolder(g_verifySelftest);          // the SAME folder, again
            if (app.folderPickOpen) pickerAccept();
            loadAll();
            const size_t bytes2 = residentImageBytes();
            const int docs2 = (int)app.images.size();
            int shared = 0, unshared = 0;
            ImageDoc* twin = nullptr;              // a second-open doc...
            ImageDoc* counter = nullptr;           // ...and its first-open counterpart
            for (const auto& d : app.images) {
                if (inFirst(d->batchId)) continue;
                ImageDoc* c = nullptr;
                for (const auto& e : app.images)
                    if (e.get() != d.get() && inFirst(e->batchId) &&
                        e->src->path == d->src->path) { c = e.get(); break; }
                if (c && c->src.get() == d->src.get() &&
                    c->src->srcId == d->src->srcId) {
                    shared++;
                    if (!twin) { twin = d.get(); counter = c; }
                } else {
                    unshared++;
                }
            }
            fprintf(stderr, "verifyselftest: V23 same folder twice: docs %d->%d, "
                            "bytes %zu->%zu, shared %d unshared %d, use_count %ld\n",
                    docs1, docs2, bytes1, bytes2, shared, unshared,
                    twin ? (long)twin->src.use_count() : -1L);
            check(docs2 == 2 * docs1, "V23 second open adds its own memberships");
            check(shared == docs1 && unshared == 0,
                  "V23 every second-open frame shares its source (srcId equal)");
            check(bytes2 == bytes1, "V23 resident bytes stay 1x, not 2x");
            check(twin && twin->src.use_count() == 2,
                  "V23 a shared source is held by exactly two memberships");

            // §3.1: A standing on one membership, B pinned to the other - the
            // chip says WHY the difference is all zero
            if (twin && counter) {
                for (int i = 0; i < (int)app.images.size(); i++)
                    if (app.images[i].get() == twin) { selectImage(i); break; }
                setCompareB(counter);
                app.compareMode = App::CmpWipe;
                std::string chip = abStatusChipText();
                bool said = chip.find("share the same pixels") != std::string::npos;
                // control: B in ANOTHER stack at the same frame number -
                // follow-frame resolves B to A's frame number inside B's own
                // stack, so the control must already stand on a frame whose
                // pixels are NOT twin's (picking merely a different path lands
                // follow-frame right back on the shared counterpart)
                ImageDoc* other = nullptr;
                for (const auto& e : app.images)
                    if (e->seqId != 0 && e->seqId != twin->seqId &&
                        e->seqIndex == twin->seqIndex &&
                        e->src.get() != twin->src.get()) { other = e.get(); break; }
                setCompareB(other);
                std::string chip2 = abStatusChipText();
                fprintf(stderr, "verifyselftest: V23 chip '%s' | control '%s'\n",
                        chip.c_str(), chip2.c_str());
                check(said, "V23 chip says A and B share the same pixels");
                check(chip2.find("share the same pixels") == std::string::npos,
                      "V23 chip stays quiet for different pixels");
                app.compareMode = App::CmpOff;
                app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
            }

            // Ctrl+Alt+W: ONE membership goes; the other stack keeps the pixels
            int sid2 = 0;                          // a stack of the second open
            for (const auto& d : app.images)
                if (!inFirst(d->batchId) && d->seqId != 0) { sid2 = d->seqId; break; }
            std::vector<int> fr2 = framesOfSeq(sid2);
            const int membersBefore = (int)fr2.size();
            selectImage(fr2[fr2.size() / 2]);
            const uint64_t closedSrcId = cur()->src->srcId;
            closeCurrent(true);                    // Ctrl+Alt+W
            ImageDoc* keeper = nullptr;
            for (const auto& e : app.images)
                if (e->src->srcId == closedSrcId) { keeper = e.get(); break; }
            fprintf(stderr, "verifyselftest: V23 Ctrl+Alt+W on shared frame: stack %d "
                            "%d->%zu members, pixels %s\n",
                    sid2, membersBefore, framesOfSeq(sid2).size(),
                    keeper ? "kept elsewhere" : "GONE");
            check((int)framesOfSeq(sid2).size() == membersBefore - 1,
                  "V23 Ctrl+Alt+W removes exactly one membership");
            check(keeper && inFirst(keeper->batchId) && !keeper->px().empty() &&
                  keeper->src.use_count() == 1,
                  "V23 the other stack keeps the pixels alive");

            // close the whole second open: every frame survives via the first,
            // and the close says so, both directions (§4)
            g_lastCloseNote.clear();
            std::vector<int> batches2;
            for (const auto& d : app.images)
                if (!inFirst(d->batchId) &&
                    std::find(batches2.begin(), batches2.end(), d->batchId) == batches2.end())
                    batches2.push_back(d->batchId);
            for (int b : batches2) closeBatch(b);
            const size_t bytes3 = residentImageBytes();
            fprintf(stderr, "verifyselftest: V23 closed the second open: docs %d, "
                            "bytes %zu, note '%s'\n",
                    (int)app.images.size(), bytes3, g_lastCloseNote.c_str());
            check((int)app.images.size() == docs1 && bytes3 == bytes1,
                  "V23 closing one side frees no shared pixels");
            check(g_lastCloseNote.find("freed") != std::string::npos &&
                  g_lastCloseNote.find("still referenced by") != std::string::npos,
                  "V23 the close said what survived, both directions");

            // ---- the same rule INSIDE one file: a frame-axis .npy holds many
            // frames under ONE path+mtime+fsize, so the identity tuple carries
            // fileFrame. Frame k of two opens shares; frames k and k+1 must
            // NEVER collapse into one source (every frame would show frame 0
            // and a shared stack's sigma_t would go to zero). Scratch fixture in
            // a subdirectory this test creates - only what it made is removed.
            {
                std::string fxroot = vfyScratch("vfy-frameaxis-scratch").u8string();
                std::error_code ec;
                std::string mf = fxroot + "/movie.npy";
                {   // 6 frames of 8x8 f32, pixel value = frame index
                    const int FR = 6, W = 8, H = 8;
                    char dict[128];
                    snprintf(dict, sizeof dict, "{'descr': '<f4', 'fortran_order': "
                             "False, 'shape': (%d, %d, %d), }", FR, H, W);
                    std::string hdr = dict;
                    size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
                    hdr.append(pad, ' ');
                    hdr += '\n';
                    std::ofstream f(pathFromUtf8(mf), std::ios::binary);
                    uint16_t hl = (uint16_t)hdr.size();
                    f.write("\x93NUMPY\x01\x00", 8);
                    f.write((const char*)&hl, 2);
                    f.write(hdr.data(), (std::streamsize)hdr.size());
                    for (int fr = 0; fr < FR; fr++) {
                        std::vector<float> px((size_t)W * H, (float)fr);
                        f.write((const char*)px.data(),
                                (std::streamsize)(px.size() * sizeof(float)));
                    }
                }
                closeAll();
                std::string e1 = loadNpy(mf);
                const int n1 = (int)app.images.size();
                const size_t fb1 = residentImageBytes();
                std::string e2 = loadNpy(mf);
                const int n2 = (int)app.images.size();
                const size_t fb2 = residentImageBytes();
                bool kShares = true, neighborsDistinct = true, values = true;
                for (int i = 0; i < n1 && n2 == 2 * n1; i++) {
                    const ImageDoc& a2 = *app.images[i];
                    const ImageDoc& b2 = *app.images[n1 + i];
                    if (a2.seqIndex != b2.seqIndex ||
                        a2.src.get() != b2.src.get()) kShares = false;
                    if (fabsf(a2.px()[0] - (float)i) > 1e-6f ||
                        fabsf(b2.px()[0] - (float)i) > 1e-6f) values = false;
                    if (i + 1 < n1 &&
                        app.images[i + 1]->src->srcId == a2.src->srcId)
                        neighborsDistinct = false;
                }
                fprintf(stderr, "verifyselftest: V23 frame-axis file twice: "
                                "%d->%d docs, bytes %zu->%zu, kShares=%d "
                                "neighborsDistinct=%d values=%d\n",
                        n1, n2, fb1, fb2, kShares ? 1 : 0,
                        neighborsDistinct ? 1 : 0, values ? 1 : 0);
                check(e1.empty() && e2.empty() && n1 == 6 && n2 == 12,
                      "V23 a frame-axis npy opens as two 6-frame stacks");
                check(kShares && fb2 == fb1,
                      "V23 frame k of both opens shares ONE source (fileFrame)");
                check(neighborsDistinct && values,
                      "V23 frames k and k+1 never collapse into one source");
                closeAll();
                std::filesystem::remove_all(pathFromUtf8(fxroot), ec);
            }
        }

        // ---- V20: the close notice is scoped to the USER ACTION and the share
        // text names every counterpart (review findings 3/4/5/6/17/18/19/20/26).
        // (a) one batch holding a source stack AND its derived stack (the
        //     derive default): the batch close frees everything, so the close
        //     must read as freed - no toast may name the derived stack as a
        //     survivor the same action then frees.
        reload();
        {
            int s0 = app.seqs[0].id;
            const int n0 = (int)framesOfSeq(s0).size();
            DerivePlan P = buildDerivePlan(s0, DR_RANGE, 0, 0, n0 - 1, nullptr);
            int did = applyDerivePlan(P, false);           // same batch as the source
            check(did != 0 && seqInfo(did) != nullptr, "V20a derived stack lands");
            const int batch = app.images[framesOfSeq(s0).front()]->batchId;
            const size_t logBefore = app.msgLog.size();
            closeBatch(batch);
            std::string lied;                              // any survivor claim spoken
            for (size_t i = logBefore; i < app.msgLog.size(); i++)
                if (app.msgLog[i].text.find("still referenced") != std::string::npos)
                    lied = app.msgLog[i].text;
            fprintf(stderr, "verifyselftest: V20a batch(source+derived) closed: docs %d, "
                            "note '%s', survivor-toast '%s'\n",
                    (int)app.images.size(), g_lastCloseNote.c_str(), lied.c_str());
            check(g_lastCloseNote.empty() && lied.empty(),
                  "V20a source+derived batch close reads as freed");
        }
        // (b) a LOOSE frame (seqId==0) whose pixels survive in a stack: its
        //     close says the survival sentence too (finding 18) - and while
        //     TWO other holders exist, the single-frame tooltip names both
        //     (finding 17), not just the first one found.
        reload();
        {
            int s0 = app.seqs[0].id;
            const std::string s0name = seqInfo(s0)->name;
            const FrameSource* msrc = app.images[framesOfSeq(s0).front()]->src.get();
            const std::string mpath = msrc->path;
            std::string e1 = loadNpy(mpath);               // loose holder #1
            ImageDoc* l1 = app.images.back().get();
            std::string e2 = loadNpy(mpath);               // loose holder #2
            check(e1.empty() && e2.empty() && l1->seqId == 0 &&
                  l1->src.get() == msrc,
                  "V20b loose re-open adopts the stack's source");
            std::string sent = filesShareSentence({ l1 });
            std::string wantStack = "frame 0 of \"" + s0name + "\"";
            std::string wantLoose = "open as \"" + std::string(l1->name) + "\"";
            fprintf(stderr, "verifyselftest: V20b single-frame sentence: '%s' "
                            "(want both '%s' and '%s')\n",
                    sent.c_str(), wantStack.c_str(), wantLoose.c_str());
            check(sent.find(wantStack) != std::string::npos &&
                  sent.find(wantLoose) != std::string::npos,
                  "V20b single-frame tooltip names ALL other holders");
            // close loose holder #2 first (silently sharing on), then #1: both
            // closes leave survivors, so both must say so
            selectImage((int)app.images.size() - 1);       // l2
            closeCurrent();
            g_lastCloseNote = "unwritten";
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i].get() == l1) { selectImage(i); break; }
            closeCurrent();                                // Ctrl+W on the loose frame
            fprintf(stderr, "verifyselftest: V20b loose close note: '%s'\n",
                    g_lastCloseNote.c_str());
            check(g_lastCloseNote.find("still referenced by") != std::string::npos,
                  "V20b loose-frame close says its sharer survives");
        }
        // (c) the stack row's tooltip with the sharing SPLIT over two derived
        //     stacks: per-counterpart counts, never the row-wide total pinned
        //     on whichever counterpart sits first in app.images (findings 3/17).
        reload();
        {
            int s0 = app.seqs[0].id;
            DerivePlan P1 = buildDerivePlan(s0, DR_RANGE, 0, 0, 1, nullptr);
            int d1 = applyDerivePlan(P1, false);
            DerivePlan P2 = buildDerivePlan(s0, DR_RANGE, 0, 2, 3, nullptr);
            int d2 = applyDerivePlan(P2, false);
            check(d1 != 0 && d2 != 0, "V20c two partial derived stacks land");
            const std::string n1 = seqInfo(d1)->name, n2 = seqInfo(d2)->name;
            // the row's sentence, built exactly as drawFileList builds it
            std::unordered_map<uint64_t, int> owners;
            for (const auto& d : app.images) owners[d->src->srcId]++;
            std::vector<const ImageDoc*> sharedFrames;
            for (int fidx : framesOfSeq(s0)) {
                const ImageDoc& f = *app.images[fidx];
                auto sh = owners.find(f.src->srcId);
                if (sh != owners.end() && sh->second > 1)
                    sharedFrames.push_back(&f);
            }
            std::string sent = filesShareSentence(sharedFrames);
            const std::string w1 = "2 with \"" + n1 + "\"";
            const std::string w2 = "2 with \"" + n2 + "\"";
            fprintf(stderr, "verifyselftest: V20c stack-row sentence: '%s' "
                            "(want '4 frame(s) shared', '%s', '%s')\n",
                    sent.c_str(), w1.c_str(), w2.c_str());
            check(sent.find("4 frame(s) shared") != std::string::npos &&
                  sent.find(w1) != std::string::npos &&
                  sent.find(w2) != std::string::npos,
                  "V20c tooltip counts per counterpart stack");
        }
        // (d) the compare chip resolves slots THROUGH follow-frame, exactly as
        //     the panes do (findings 5/19). Slots have no dormant state any
        //     more (9c8f7bb abolished the A==B special case: every live slot
        //     resolves), so a slot pinned to A ITSELF says the share - that is
        //     the honest sentence, the same pixels really are on both sides -
        //     and a pin that follow-resolves onto A's twin frame says it too,
        //     even though the PINNED frame's pixels differ from A's: the chip
        //     speaks for what the panes show, never for the pinned uid.
        reload();
        {
            openFolder(g_verifySelftest);                  // twins: same folder again
            if (app.folderPickOpen) pickerAccept();
            loadAll();
            int s0 = app.seqs[0].id;
            std::vector<int> f0 = framesOfSeq(s0);
            int twinSeq = 0;                               // second open of stack 0
            for (const auto& d : app.images)
                if (d->seqId != s0 && d->seqId != 0 &&
                    d->src.get() == app.images[f0[0]]->src.get())
                    twinSeq = d->seqId;
            int s1 = 0;                                    // an UNRELATED stack for B
            for (const auto& s : app.seqs)
                if (s.id != s0 && s.id != twinSeq) { s1 = s.id; break; }
            check(twinSeq != 0 && s1 != 0, "V20d twin stack and a B stack exist");
            app.compareFollowFrame = true;
            for (int idx : f0)
                if (app.images[idx]->seqIndex == 1) { selectImage(idx); break; }
            ImageDoc* bpick = nullptr;
            for (int idx : framesOfSeq(s1))
                if (app.images[idx]->seqIndex == 1) bpick = app.images[idx].get();
            setCompareB(bpick);
            app.compareMode = App::CmpWipe;
            app.cmpExtra.clear();
            app.cmpExtra.push_back(cur()->uid);            // pin C = A itself
            std::string chipSelf = abStatusChipText();
            app.cmpExtra.clear();
            for (int idx : framesOfSeq(twinSeq))           // pin C = twin frame 3...
                if (app.images[idx]->seqIndex == 3)
                    app.cmpExtra.push_back(app.images[idx]->uid);
            std::string chipFollow = abStatusChipText();   // ...pane shows twin frame 1
            fprintf(stderr, "verifyselftest: V20d chip self '%s' | followed '%s'\n",
                    chipSelf.c_str(), chipFollow.c_str());
            check(chipSelf.find("A and C share the same pixels") != std::string::npos,
                  "V20d a slot holding A itself says the share (A==B canon)");
            check(chipFollow.find("A and C share the same pixels") != std::string::npos,
                  "V20d chip speaks for the RESOLVED slot frame");
            app.cmpExtra.clear();
            app.compareMode = App::CmpOff;
            app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
        }
        // (e) sources held only by an in-flight load's decoded-but-not-landed
        //     queue count as ALIVE (finding 26): "N frame(s) freed" must not
        //     overstate while the loader still holds the shared_ptr.
        reload();
        {
            int s0 = app.seqs[0].id, s1 = app.seqs[1].id;
            std::vector<int> f0 = framesOfSeq(s0);
            {   // white-box: what the seq worker queues after ADOPTING s0's
                // first source for a second open of the same folder
                auto qd = std::make_unique<ImageDoc>();
                qd->src = app.images[f0[0]]->src;
                qd->syncMirrors();
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(0, std::move(qd));
            }
            app.seqLoadingId = s1;                         // loading ANOTHER stack
            closeStack(s0);
            std::string note = g_lastCloseNote;
            app.seqLoadingId = 0;
            {   // only what this test queued
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.clear();
            }
            fprintf(stderr, "verifyselftest: V20e close under in-flight adoption: "
                            "note '%s'\n", note.c_str());
            check(note.find("4 frame(s) freed") != std::string::npos &&
                  note.find("1 still referenced by") != std::string::npos,
                  "V20e queued-but-not-landed holder counts as alive");
        }
        // (f) promotion that ADOPTS must invalidate. A remote tuple carries no
        //     mtime/fsize generation, so the resident a promotion adopts can
        //     hold an OLDER fetch than the preview just displayed - after the
        //     src swap, the doc's texture and caches still describe the
        //     preview's bytes. The full-res swap path already runs the
        //     invalidation walk (dataRev/texDirty/forgetImage) after its
        //     adoption; promotion must run the same walk.
        reload();
        {
            ImageDoc* a = app.images[framesOfSeq(app.seqs[0].id)[0]].get();
            auto pv = std::make_unique<ImageDoc>();
            pv->name = "promoted.npy";
            {   // same identity tuple as a's registered source, decoded
                // "independently" (what a preview fetch is) - with DIFFERENT
                // pixels, so a silent generation swap is observable
                FrameSource& S = *pv->src;
                S.path = a->src->path;
                S.remoteUrl = "local://" + a->src->path;
                S.member = a->src->member;
                S.fileFrame = a->src->fileFrame; S.remoteFrame = a->src->remoteFrame;
                S.npyShape = a->src->npyShape; S.npyRead = a->src->npyRead;
                S.mtime = a->src->mtime; S.fsize = a->src->fsize;
                S.w = a->w; S.h = a->h; S.ch = a->ch; S.dtype = a->dtype;
                S.data.assign((size_t)a->w * a->h * a->ch, -7.0f);
                S.vmin = -7.0f; S.vmax = -7.0f;
            }
            pv->syncMirrors();
            pv->preview = true;
            pv->uid = app.nextUid++;
            pv->texDirty = false;              // "already drawn" preview
            ImageDoc* p = pv.get();
            app.images.push_back(std::move(pv));
            app.previewUid = p->uid;
            const int rev0 = p->dataRev;
            promotePreview(p);
            fprintf(stderr, "verifyselftest: V20f promote-adopt: src %s, "
                            "dataRev %d -> %d, texDirty %d\n",
                    p->src == a->src ? "adopted" : "PRIVATE",
                    rev0, p->dataRev, (int)p->texDirty);
            check(p->src == a->src, "V20f promotion adopts the resident tuple");
            check(p->dataRev > rev0 && p->texDirty,
                  "V20f promote-adopt invalidates (dataRev + texture)");
            check(!p->preview && app.previewUid == 0,
                  "V20f promoted doc has left the preview slot");
        }

        fprintf(stderr, "verifyselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
