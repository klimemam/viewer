// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // Close per layer, verifiable without a human (docs/terminology.md): Ctrl+W's
    // closeCurrent() on a stack member removes the WHOLE stack and every trace of
    // it - and a stack closed mid-fetch must not regrow from the prefetch queue.
    if (!g_closeSelftest.empty()) {
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
        openFolder(g_closeSelftest);
        if (!app.folderPickOpen) {
            fprintf(stderr, "closeselftest: picker did not open\n");
            return 1;
        }
        pickerAccept();
        loadAll();
        // rows exist, so residue is detectable - and the stack being closed is a
        // series member, so closeStack's series hook is on the hook too
        linRecompute(selftestMakeSeries(app.images.empty() ? 0 : app.images[0]->batchId,
                                        app.lin.unit));
        int imagesBefore = (int)app.images.size(), seqsBefore = (int)app.seqs.size();
        if (seqsBefore < 3) {
            fprintf(stderr, "closeselftest: expected 3 stacks under %s, got %d\n",
                    g_closeSelftest.c_str(), seqsBefore);
            return 1;
        }
        int sid = app.seqs[seqsBefore / 2].id;
        std::vector<int> fr = framesOfSeq(sid);
        int stackFrames = (int)fr.size();
        selectImage(fr[fr.size() / 2]);    // a MIDDLE frame: the reported bug
        // Every temporal cache, not just the A slot. TemporalState is keyed on
        // the STACK, so closing the stack is what has to empty it - and there
        // are 2 + N of them: A, the compare B side, and one per lettered slot
        // (docs/reference-design.md §3.2 hole 2). A close that emptied only
        // slot 0 would leave a dead stack's sigma_t sitting under a letter,
        // ready to be read the next time the panel drew a comparison. Latched
        // here, deliberately, so "gone afterwards" is a fact about the close
        // and not about a cache that was empty all along.
        recomputeTemporalIfNeeded(app.images[fr[fr.size() / 2]].get(), app.temporal[0]);
        app.temporal[1] = app.temporal[0];
        app.temporalExtra.assign(2, app.temporal[0]);
        bool latched = app.temporal[0].seqId == sid && app.temporal[1].seqId == sid &&
                       app.temporalExtra[0].seqId == sid && app.temporalExtra[1].seqId == sid;
        closeCurrent();                    // Ctrl+W's path: must close the stack
        bool linResidue = false;
        for (const auto& r : app.lin.rows) if (r.seqId == sid) linResidue = true;
        int tempResidue = 0;
        if (app.temporal[0].seqId != -1) tempResidue++;
        if (app.temporal[1].seqId != -1) tempResidue++;
        for (const auto& T : app.temporalExtra) if (T.seqId != -1) tempResidue++;
        fprintf(stderr, "closeselftest: closeCurrent on mid frame of stack %d: "
                        "images %d->%d, seqs %d->%d, frames(seq)=%d, "
                        "temporal.seqId=%d, latched %d slot(s) before the close, "
                        "%d still naming a stack, lin rows for seq: %s\n",
                sid, imagesBefore, (int)app.images.size(), seqsBefore,
                (int)app.seqs.size(), (int)framesOfSeq(sid).size(),
                app.temporal[0].seqId, latched ? 4 : 0, tempResidue,
                linResidue ? "RESIDUE" : "none");
        if ((int)app.images.size() != imagesBefore - stackFrames ||
            (int)app.seqs.size() != seqsBefore - 1 || !framesOfSeq(sid).empty() ||
            !latched || tempResidue != 0 || linResidue) {
            fprintf(stderr, "closeselftest: FAILED - stack close left residue\n");
            ok = false;
        }
        // ---- a stack closed while its remote prefetch is still in flight ----
        std::string scanroot = g_closeSelftest;
        std::replace(scanroot.begin(), scanroot.end(), '\\', '/');
        {
            size_t sl = scanroot.find_last_of('/');
            scanroot = (sl == std::string::npos ? std::string(".")
                                                : scanroot.substr(0, sl)) + "/rb/scanroot";
        }
        int seqsNow = (int)app.seqs.size();
        startRemote(rbMain(), "local://" + scanroot);
        double t0 = nowSec();
        bool scanSent = false, closed = false;
        int rsid = 0, rfLeft = -1;
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
            if (!closed && (int)app.seqs.size() > seqsNow && app.rfPending > 0) {
                rsid = app.seqs.back().id;     // the stack the fetcher is filling
                closeStack(rsid);
                closed = true;
                std::lock_guard<std::mutex> lk(app.rfMtx);
                rfLeft = 0;
                for (const auto& jj : app.rfQueue) if (jj.seqId == rsid) rfLeft++;
                break;
            }
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!closed) {
            fprintf(stderr, "closeselftest: FAILED - never caught a stack mid-fetch (%s)\n",
                    rbMain().b.err.c_str());
            ok = false;
        } else {
            // the in-flight job (and the rest of the queue) gets 2 s to land;
            // nothing of the closed stack may grow back
            double t1 = nowSec();
            while (nowSec() - t1 < 2.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int orphans = 0;
            for (const auto& d : app.images) if (d->seqId == rsid) orphans++;
            fprintf(stderr, "closeselftest: closeStack(%d) mid-fetch: %d queued job(s) "
                            "for it left, %d frame(s) regrew after 2 s pump\n",
                    rsid, rfLeft, orphans);
            if (rfLeft != 0 || orphans != 0) {
                fprintf(stderr, "closeselftest: FAILED - closed stack regrew\n");
                ok = false;
            }
        }
        // ---- Close All while the producers are still running ----------------
        // The menu item is a bare closeAll(). It used to clear images/seqs/
        // batches and leave the sequence loader RUNNING with seqQueue and
        // rbOpenQueue full, so the closed content reopened itself seconds
        // later under batch ids app.batches no longer contained. Close has to
        // stop the producers first; this asserts that it does, on both the
        // local queue and the remote one.
        {
            closeAll();
            openFolder(g_closeSelftest);
            if (app.folderPickOpen) pickerAccept();
            int queued0 = (int)app.seqQueue.size();
            bool running0 = app.seqRunning;
            closeAll();                       // exactly what File > Close All does
            bool stopped = app.seqQueue.empty() && app.seqRestore.empty() &&
                           !app.seqRunning && app.seqLoadingId == 0 &&
                           app.rbOpenQueue.empty();
            double t2 = nowSec();        // give the producers 2 s to regrow
            while (nowSec() - t2 < 2.0) {
                pumpSequenceAndQueue();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int dead = 0;                     // frames under a batch nobody names
            for (const auto& d : app.images) {
                bool named = d->batchId == 0;
                for (const auto& bb : app.batches) if (bb.id == d->batchId) named = true;
                if (!named) dead++;
            }
            bool closeOk = stopped && app.images.empty() && app.seqs.empty() && dead == 0;
            fprintf(stderr, "closeselftest: Close All mid-load (%d queued, loader "
                            "running=%d): queues emptied+loader stopped=%d; after a 2 s "
                            "pump images=%d seqs=%d batches=%d, %d frame(s) under a dead "
                            "batch: %s\n",
                    queued0, running0 ? 1 : 0, stopped ? 1 : 0, (int)app.images.size(),
                    (int)app.seqs.size(), (int)app.batches.size(), dead,
                    closeOk ? "ok" : "FAIL");
            if (!closeOk) ok = false;
        }
        {   // the same, for the REMOTE open queue: accept a scan of N stacks and
            // Close All while stack 1 is still transferring
            closeAll();
            rbMain().b = App::RemoteBrowse{};
            startRemote(rbMain(), "local://" + scanroot);
            double t3 = nowSec();
            bool sent = false, caught = false;
            int rq0 = 0;
            while (nowSec() - t3 < 120.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                if (rbMain().b.connected && !sent) {
                    App::RbJob j;
                    j.kind = App::RbScan;
                    j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = scanroot;
                    rbEnqueue(rbMain(), std::move(j));
                    sent = true;
                }
                if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
                if (!app.rbOpenQueue.empty()) {
                    rq0 = (int)app.rbOpenQueue.size();
                    caught = true;
                    break;
                }
                if (!rbMain().b.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!caught) {
                fprintf(stderr, "closeselftest: FAILED - never caught a queued remote "
                                "open (%s)\n", rbMain().b.err.c_str());
                ok = false;
            } else {
                closeAll();
                bool emptied = app.rbOpenQueue.empty();
                double t4 = nowSec();
                while (nowSec() - t4 < 3.0) {
                    pumpRemoteBrowse();
                    pumpRemoteFetch();
                    pumpRemoteOpenQueue();
                    pumpSequenceAndQueue();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                bool rok = emptied && app.images.empty() && app.seqs.empty();
                fprintf(stderr, "closeselftest: Close All with %d remote stack(s) "
                                "queued: queue emptied=%d; after a 3 s pump images=%d "
                                "seqs=%d: %s\n",
                        rq0, emptied ? 1 : 0, (int)app.images.size(),
                        (int)app.seqs.size(), rok ? "ok" : "FAIL");
                if (!rok) ok = false;
            }
        }
        fprintf(stderr, "closeselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
