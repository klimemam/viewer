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
// A valid analyze() for the descriptors AP27 asks the host to REFUSE. It has
// to be a real function pointer: validCommon rejects a NULL one, and then the
// test would be watching the wrong rule turn a descriptor away.
static int32_t anaProbeAnalyze(const psFrame*, const psRect*,
                               const psAnalyzeSink3*, char*, size_t) {
    return 0;
}

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
    // The ABI compatibility fixtures (#46 stage 2): two dlls carrying ONE
    // measurement through the two register paths - V2, the shape every bundled
    // analyzer uses, and V3 with a declared version. They live outside the
    // shipped plugins directory precisely so that loading them is a deliberate
    // act of this test rather than a change to what the viewer offers, and
    // they are loaded HERE, through the same plugin_host::loadAll every other
    // plugin goes through: a fixture registered by hand would prove nothing
    // about the loader, which is the half of the ledger under test.
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins-abitest",
                           plugin_host::exeDir() + "/../plugins-abitest" },
                         [](const std::string& m, bool e) {
                             fprintf(stderr, "anaprovselftest: load: %s%s\n",
                                     e ? "ERROR " : "", m.c_str());
                         });

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
    // one frame of the REAL panel, with or without the backends - the
    // roiStatsSelftest() recipe, see the NewFrame comment there. A lambda
    // because the version half below measures the SAME fixture with three
    // different analyzers, and the comparison is only worth anything if all
    // three go through the identical path.
    ImGuiIO& io = ImGui::GetIO();
    auto runPanel = [&](int which) {
        app.anaSel = which;
        app.anaRunRequest = true;      // exactly what the Measure menu sets
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
    };
    runPanel(sel);

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
    // The full path, spelled by the ONE citation every mouth uses. It was the
    // bare path when this was written, because stats/moments was a V2
    // registration and had no version to put in front of it; the §1 migration
    // gave it one, so the line is now "<1.0.0> C:\...\analyzer_stats.dll".
    // Asserting through analyzerCitation rather than re-spelling it keeps this
    // check about what it was always about - that the pasted document cites the
    // FILE - and leaves "which shape does a citation have" to AP23/AP24, where
    // both shapes are asked of the twins on purpose.
    check(tsv.find("# plugin: " + analyzerCitation(anas[sel].version, anas[sel].path) +
                   "\n") != std::string::npos &&
          tsv.find(anas[sel].path) != std::string::npos,
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

    // ======================= #46 stage 2: the version ======================
    // Stage 1 (above) answered "which file". The other half of the citation is
    // the version, and it is the half the host CANNOT source itself: only the
    // plugin knows what it calls itself, which is why it waited for an ABI
    // field (docs/reference/abi-v3.md §3.1, §11). What follows measures the same fixture
    // with the V2 twin and the V3 twin - one measurement, two ABI paths - so
    // "the version surfaces" and "V2 still works" are asked of the same grid.
    int v2i = -1, v3i = -1;
    for (int i = 0; i < (int)anas.size(); i++) {
        if (anas[i].name == "abitest/twin-v2") v2i = i;
        if (anas[i].name == "abitest/twin-v3") v3i = i;
    }
    check(v2i >= 0 && v3i >= 0,
          "AP13 both ABI twins loaded (V2 and V3 registered side by side)");
    // A missing fixture must REDDEN the checks it feeds, never skip them: a
    // v3 registration the host declines is exactly the state this section is
    // here to catch, and a guarded block would report it as twelve silences.
    static const AnalyzerPluginInfo kAbsent{};
    const AnalyzerPluginInfo& a2 = v2i >= 0 ? anas[v2i] : kAbsent;
    const AnalyzerPluginInfo& a3 = v3i >= 0 ? anas[v3i] : kAbsent;

    // ---- the ledger's new column ------------------------------------------
    check(a3.abi == 3 && a3.version == "3.1.4-twin (2026-08-10)",
          "AP14 ledger carries the V3 plugin's declared version, verbatim");
    check(a3.headline == "mean",
          "AP15 ledger carries the V3 plugin's declared headline key");
    // and the invariant that makes the column trustworthy: a blank is a fact
    // about the descriptor, not a gap the host may fill in later
    bool blankV12 = true;
    for (const auto& a : anas)
        if (a.abi < 3 && !a.version.empty()) blankV12 = false;
    check(blankV12 && a2.version.empty() && a2.abi == 2,
          "AP16 every V1/V2 analyzer's version is EMPTY - nothing guessed");

    // ---- the same measurement through both paths --------------------------
    std::vector<std::string> k2, u2;
    std::vector<std::vector<std::string>> val2;
    std::string prov2, dll2, ver2, tsv2, tsv3;
    std::vector<char> hl2;
    bool err2 = false;
    if (v2i >= 0) {
        runPanel(v2i);
        k2 = ana.keys; u2 = ana.units; val2 = ana.vals; hl2 = ana.headline;
        prov2 = ana.prov; dll2 = ana.provDll; ver2 = ana.provVer;
        tsv2 = buildAnalysisTsv();
        err2 = ana.err.empty();
        fprintf(stderr, "anaprovselftest: prov(v2): %s\n", prov2.c_str());
    }
    if (v3i >= 0) {
        runPanel(v3i);
        tsv3 = buildAnalysisTsv();
        fputs(tsv3.c_str(), stdout);       // the V3 document, for eyeballs
        fprintf(stderr, "anaprovselftest: prov(v3): %s\n", ana.prov.c_str());
    }
    check(err2 && v3i >= 0 && ana.err.empty(),
          "AP17 both twins ran clean on the fixture");
    check(v3i >= 0 && !k2.empty() &&
          k2 == ana.keys && val2 == ana.vals && u2 == ana.units,
          "AP18 V2 and V3 paths produce IDENTICAL rows, values and units");

    // ---- where the version surfaces ---------------------------------------
    // §11: the status line becomes `アナライザ名 <version> (dll名)`.
    check(!a3.version.empty() &&
          ana.prov.find(a3.name + " <" + a3.version + "> (" + a3.file + ")") !=
              std::string::npos,
          "AP19 status line reads 'name <version> (dll)' for a V3 analyzer");
    check(!prov2.empty() &&
          prov2.find(a2.name + " (" + a2.file + ")") != std::string::npos &&
          prov2.find('<') == std::string::npos,
          "AP20 ...and 'name (dll)' with NO version slot for a V2 one");
    // the tooltip and the exports cite the same two things from the same two
    // fields - the panel's tooltip is provVer + provDll verbatim
    check(!a3.version.empty() && ana.provVer == a3.version &&
          ana.provDll == a3.path,
          "AP21 the grid keeps version + full path (tooltip / export)");
    check(!dll2.empty() && ver2.empty() && dll2 == a2.path,
          "AP22 ...and keeps the path with an empty version for V2");
    check(!a3.version.empty() &&
          tsv3.find("# plugin: <" + a3.version + "> " + a3.path + "\n") !=
              std::string::npos,
          "AP23 TSV '# plugin:' carries <version> then the full path");
    check(!tsv2.empty() &&
          tsv2.find("# plugin: " + a2.path + "\n") != std::string::npos,
          "AP24 ...and is byte-for-byte the stage-1 line for a V2 analyzer");

    // ---- headline: a declaration beats the host's table --------------------
    // The host table (analyzerHeadlineSuffix) knows neither twin, so a flagged
    // row can only have come from the descriptor.
    {
        int hl3 = -1, hl2n = 0;
        for (int i = 0; i < (int)ana.keys.size(); i++)
            if (i < (int)ana.headline.size() && ana.headline[i]) hl3 = i;
        for (int i = 0; i < (int)hl2.size(); i++) if (hl2[i]) hl2n++;
        check(v3i >= 0 && hl3 >= 0 && ana.keys[hl3] == "ch0.mean",
              "AP25 the V3 headline accents the declared key, channel stripped");
        check(!hl2.empty() && hl2n == 0,
              "AP26 the V2 twin gets no headline - the host table declares none");
    }

    // ---- a v3 descriptor with no version is refused --------------------
    // §3.1: declaring v3 IS declaring a version, so "no version" is a
    // contradiction the host must not wave through. Asked of the real
    // registration function, with the real analyzer list as the witness.
    {
        const psHostApi* api = plugin_host::hostApi();
        size_t before = plugin_host::analyzers().size();
        bool refused = api && api->register_analyzer3;
        if (refused) {
            psAnalyzerV3 bad{};
            bad.abi_version = 3u;
            bad.caps = PS_CAP_CPU;
            bad.name = "abitest/no-version";
            bad.analyze = anaProbeAnalyze;
            bad.version = nullptr;
            if (api->register_analyzer3(api->ctx, &bad) == 0) refused = false;
            bad.version = "";
            if (api->register_analyzer3(api->ctx, &bad) == 0) refused = false;
        }
        check(refused && plugin_host::analyzers().size() == before,
              "AP27 a v3 descriptor with NULL/empty version is refused");
    }

    fprintf(stderr, "anaprovselftest: %s\n", ok ? "ok" : "FAILED");
    shutdown();
    return ok ? 0 : 1;
}
