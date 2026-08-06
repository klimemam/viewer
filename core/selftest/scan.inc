// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    if (!g_scanSelftest.empty()) {
        std::string dir = g_scanSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        startRemote(rbMain(), "local://" + dir);
        double t0 = nowSec();
        bool scanSent = false;
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            pumpRemoteFetch();
            pumpRemoteOpenQueue();
            pumpSequenceAndQueue();
            if (rbMain().b.connected && !scanSent) {
                App::RbJob j; j.kind = App::RbScan;
                j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = dir;
                rbEnqueue(rbMain(), std::move(j));
                scanSent = true;
            }
            // headless "Load selected": accept the picker with everything checked,
            // through the SAME function the Load button calls
            if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
            if (scanSent && !rbMain().busy && !app.seqs.empty() &&
                app.rbOpenQueue.empty() && app.rfPending == 0) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (const auto& si : app.seqs)
            fprintf(stderr, "scanselftest: stack '%s' expected=%d\n",
                    si.name.c_str(), si.expectedFrames);
        bool ok = !app.seqs.empty() && rbMain().b.err.empty();
        fprintf(stderr, "scanselftest: %d stack(s): %s\n", (int)app.seqs.size(),
                ok ? "ok" : ("FAILED " + rbMain().b.err).c_str());
        stopRbWorker();
        stopSequenceLoader();
        // opening stacks spun up the fetch and measure workers too; an unjoined
        // std::thread at exit is std::terminate, i.e. a "passing" test that dies
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
