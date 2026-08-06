// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // The per-frame table, verifiable without a human: load the stack given on
    // the command line, run the exact clipboard path, print the TSV to stdout.
    // An independent numpy implementation must reproduce every number.
    if (g_fstatSelftest) {
        double t0 = nowSec();
        while (nowSec() - t0 < 600.0) {
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();   // headless accept
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (app.seqs.empty()) { fprintf(stderr, "fstatselftest: no stack loaded\n"); return 1; }
        copyPerFrameStats(app.seqs[0].id);
        const char* clip = ImGui::GetClipboardText();
        if (!clip || !*clip) { fprintf(stderr, "fstatselftest: empty clipboard\n"); return 1; }
        fputs(clip, stdout);
        if (app.seqThread.joinable()) app.seqThread.join();
        return 0;
    }
