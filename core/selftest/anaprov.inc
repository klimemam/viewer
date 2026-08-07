// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --anaprov-selftest: which dll produced these numbers (#46 stage 1) ----
//
// The Analysis grid's provenance line answers where, on what, over which
// target and when - but not WHO computed the numbers, and every analyzer is a
// plugin: a result a report cites has to name the file it came from, or the
// citation cannot be checked against anything. The plugin ABI carries no
// version field (that waits for ABI v3), but the FILE never needed the ABI at
// all, because the host is the one doing the loading - so the ledger lives in
// plugin_host (AnalyzerPluginInfo::file/path, written at registration) and the
// panel stamps it into the same prov string every export already carries
// verbatim (docs/analysis-layers.md §8, 判断record 6).
//
// Windowless on the roiStatsSelftest() pattern and for the same reason: every
// assertion here is about STRINGS - the ledger, the prov line, the TSV
// document - none about layout. The panel is the REAL one (drawPanelAnalysis),
// and the analyzers are the real dlls startup loaded from exeDir()/plugins
// before parseCli ran; a test against a fake registration would prove nothing
// about the ledger, whose whole job is to witness the actual load.
static int anaProvSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "anaprovselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // the same orderly shutdown every early-returning selftest does: an
    // unjoined worker at exit is std::terminate() under the last line printed
    auto shutdown = [&]() {
        stopRbWorker(); stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker();
    };
    const auto& anas = plugin_host::analyzers();
    if (anas.empty()) {
        fprintf(stderr, "anaprovselftest: no analyzer plugins loaded - is "
                        "exeDir()/plugins missing? Nothing to attribute.\n");
        shutdown();
        return 1;
    }

    // ---- the ledger itself: every analyzer names the dll that registered it.
    // "Every", not "one": a second dll registering later must not inherit the
    // first one's name through a stale g_loading, and this is where that shows.
    {
        bool named = true, resolvable = true;
        for (const auto& a : anas) {
            fprintf(stderr, "anaprovselftest: ledger: %-24s <- %s\n", a.name.c_str(),
                    a.path.empty() ? "(no path)" : a.path.c_str());
            if (a.file.empty()) named = false;
            std::error_code ec;
            if (a.path.size() <= a.file.size() || a.file.empty() ||
                a.path.compare(a.path.size() - a.file.size(), a.file.size(), a.file) != 0 ||
                !std::filesystem::exists(pathFromUtf8(a.path), ec))
                resolvable = false;
        }
        check(named, "AP1 ledger: every analyzer names its dll file");
        check(resolvable, "AP2 ledger: the path IS that file, and it exists");
    }

    // the same analyzer the remote MEASURE selftest picks, for the same
    // reason: stats/moments accepts any frame
    int sel = -1;
    for (int i = 0; i < (int)anas.size(); i++)
        if (anas[i].name.find("stats") != std::string::npos) { sel = i; break; }
    if (sel < 0) sel = 0;

    // analytic fixture, in memory: this test opens no file and cannot be made
    // to pass (or fail) by a fixture drifting
    closeAll();
    {
        auto d = std::make_unique<ImageDoc>();
        d->name = "anaprov"; d->src->w = 64; d->src->h = 64; d->src->ch = 1;
        d->src->dtype = "u16";
        d->src->data.resize((size_t)64 * 64);
        for (int i = 0; i < 64 * 64; i++) d->px()[i] = (float)(1000 + (i & 7));
        d->syncMirrors();
        d->uid = app.nextUid++;
        app.images.push_back(std::move(d));
    }
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.selectedAnn = 0;   // whole image
    app.anaSel = sel;
    app.anaRunRequest = true;          // exactly what the Measure menu sets

    // one frame of the REAL panel, with or without the backends - the
    // roiStatsSelftest() recipe, see the NewFrame comment there
    ImGuiIO& io = ImGui::GetIO();
    {
        if (g_haveWindow) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
        } else {
            io.DisplaySize = ImVec2(1600, 1000);
            io.DisplayFramebufferScale = ImVec2(1, 1);
            io.DeltaTime = 1.0f / 60.0f;
            if (!io.Fonts->IsBuilt()) io.Fonts->Build();
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_Always);
        ImGui::Begin("AnaProvProbe", nullptr,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
        drawPanelAnalysis();
        ImGui::End();
        ImGui::EndFrame();
    }

    const auto& ana = app.ana;
    fprintf(stderr, "anaprovselftest: prov: %s\n", ana.prov.c_str());
    check(ana.err.empty(), "AP3 the analyzer ran clean on the fixture");
    check(ana.prov.rfind("[local]", 0) == 0,
          "AP4 a run produced the [local] provenance line");
    check(ana.prov.find(anas[sel].name) != std::string::npos,
          "AP5 prov line names the analyzer");
    check(!anas[sel].file.empty() &&
          ana.prov.find("(" + anas[sel].file + ")") != std::string::npos,
          "AP6 prov line names the dll from the ledger");
    check(!ana.provDll.empty() && ana.provDll == anas[sel].path,
          "AP7 the grid keeps the dll's full path (tooltip / export)");

    // ---- the surface a report actually cites: the Copy-table document ------
    std::string tsv = buildAnalysisTsv();
    fputs(tsv.c_str(), stdout);        // the document itself, for eyeballs and diffs
    check(tsv.rfind("# [local]", 0) == 0, "AP8 TSV: prov comment leads the document");
    check(!anas[sel].file.empty() &&
          tsv.find(anas[sel].file) != std::string::npos,
          "AP9 TSV: the pasted grid names the dll");
    check(tsv.find("# plugin: " + anas[sel].path + "\n") != std::string::npos,
          "AP10 TSV: '# plugin:' carries the full path");

    // ---- and builtin results wear NO plugin tag -----------------------------
    // The Temporal export's numbers are computed by the viewer itself; its
    // provenance says "app: viewer <version>". A dll tag on a builtin number
    // would be a false citation - the attribution must stay exactly as narrow
    // as the ledger's knowledge.
    std::string bt = buildTemporalExport('\t');
    check(!bt.empty() && bt.find("# app: viewer ") != std::string::npos,
          "AP11 builtin export still cites the viewer as computer");
    check(bt.find("# plugin:") == std::string::npos &&
          (anas[sel].file.empty() ||
           bt.find(anas[sel].file) == std::string::npos),
          "AP12 builtin results wear no plugin tag");

    fprintf(stderr, "anaprovselftest: %s\n", ok ? "ok" : "FAILED");
    shutdown();
    return ok ? 0 : 1;
}
