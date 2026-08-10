// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --bundled-selftest: the five shipped analyzers on the v3 mouth --------
//
// docs/abi-v3.md §1 calls migrating them "機械的作業 — 判断の外", and the
// mechanics really are small. What is NOT small is the pair of claims the
// migration makes, and this file exists to hold both of them to a number:
//
//   1. EVERY bundled analyzer declares a version, and that version reaches the
//      surfaces a report cites (§3.1, §11). Until it does, §10's name+version
//      parity refuses all five - a rule that is strictly correct and, with an
//      empty column on the other side of it, unusable with anything shipped.
//   2. NOTHING ELSE MOVED. Same rows, same units, same values, same errors,
//      same accented row, same TSV table. A registration change that quietly
//      changed a number would be the worst possible outcome here, because the
//      number would look exactly as plausible as it did before.
//
// Claim 2 is the reason this test prints so much to STDOUT and asserts on so
// little of it. The assertions below cannot see a value drift - they do not
// know what "R.noise" should be, and a test that hard-coded it would be
// asserting today's arithmetic, which is not what is under test. What IS under
// test is that the arithmetic did not change, and the instrument for that is a
// DIFF of this test's own stdout across the two commits: the fail-first commit
// (analyzers still V2, the version assertions red) and the migration commit
// (v3, green). Byte-identical stdout is then evidence produced by one piece of
// code run twice, not two descriptions of a result that were written down.
//
// So the split is deliberate and load-bearing:
//   STDOUT - results ONLY: keys, units, values, series, errors, accented row.
//            Nothing here may depend on a version, a path, a clock or a build.
//   STDERR - the PASS/FAIL lines and the provenance, which is exactly the part
//            the migration is SUPPOSED to change (the status line gains
//            "<version>"). Keeping it off stdout is what lets the diff be empty
//            rather than "empty except for the five lines we expected".
//
// Windowless on the --anaprov-selftest recipe and for its reasons: the fixtures
// are analytic and in memory, the panel driven is the real drawPanelAnalysis,
// and every assertion is a string or a number rather than a rectangle.

// The shipped five, named here in one place: this list IS the claim "the
// bundled analyzers", and a sixth one arriving should have to edit it.
static const char* const kBundledAnalyzers[] = {
    "iso12233/e-sfr", "noise/floor", "sharpness/gradient",
    "stats/moments", "uniformity/prnu-fpn",
};

// %.17g, not the panel's %.6g. The panel's strings are what a user copies and
// they are compared too (the TSV block below), but six digits can hide a change
// in the seventh - and "this is a registration change, not a numerical one" is
// a claim about the double, not about its rendering.
static std::vector<std::pair<std::string, std::string>>* g_bunRows;
static std::vector<std::string>* g_bunSeries;
static void bunNum(void*, const char* k, double v) {
    char b[64];
    snprintf(b, sizeof b, "%.17g", v);
    g_bunRows->emplace_back(k ? k : "", b);
}
static void bunTxt(void*, const char* k, const char* v) {
    g_bunRows->emplace_back(k ? k : "", v ? v : "");
}
// Curves are a result too - iso12233/e-sfr's whole answer is one - so they are
// dumped point by point rather than summarised. A checksum would compare equal
// for two curves that differ in an order the sum cannot see.
static void bunSer(void*, const char* n, const char* xl, const char* yl,
                   const float* x, const float* y, uint32_t cnt) {
    g_bunSeries->push_back(std::string("series ") + (n ? n : "") + "\t" +
                           (xl ? xl : "") + "\t" + (yl ? yl : "") + "\t" +
                           std::to_string(cnt));
    for (uint32_t i = 0; y && i < cnt; i++) {
        char b[96];
        snprintf(b, sizeof b, "  [%u] %.9g %.9g", i,
                 x ? (double)x[i] : (double)i, (double)y[i]);
        g_bunSeries->push_back(b);
    }
}
// The same dispatch every other caller of an analyzer does (panel_analysis.inc,
// serve.cpp, cli.inc). It is here so that the raw dump goes through the CURRENT
// registration path whatever that is: before the migration this takes the V2
// branch and after it the V3 one, and the point of the exercise is that the two
// print the same bytes.
static int32_t bunRunRaw(const AnalyzerPluginInfo& a, const psFrame* f,
                         const psRect* roi, char* err, size_t cap) {
    if (a.abi == 3) {
        psAnalyzeSink3 sk{ nullptr, bunNum, bunTxt, bunSer, {} };
        return a.v3.analyze(f, roi, &sk, err, cap);
    }
    if (a.abi == 2) {
        psAnalyzeSink2 sk{ nullptr, bunNum, bunTxt, bunSer, {} };
        return a.v2.analyze(f, roi, &sk, err, cap);
    }
    psAnalyzeSink sk{ nullptr, bunNum, bunTxt };
    return a.v1.analyze(f, roi, &sk, err, cap);
}
// A valid analyze() for the V1/V2 seats the last check registers into. It has
// to be a real pointer - validCommon refuses a NULL one, and the check would
// then be watching the wrong rule.
static int32_t bunProbeV2(const psFrame*, const psRect*, const psAnalyzeSink2*,
                          char*, size_t) {
    return 0;
}

// Four fixtures, built in memory and integer-valued wherever a value can be:
// an exactly representable f32 cannot drift by a rounding that depends on the
// order a compiler folded a constant in, which would make a diff of this test's
// output say something about the toolchain instead of about the plugins.
//
//   0 flat   - a flat field with row/column structure. The home ground of
//              noise/floor and uniformity/prnu-fpn.
//   1 bayer  - THE SAME PIXELS declared mosaiced. A different code path in
//              three of the five (planes bucketed R/Gr/Gb/B, never pooled) and
//              a refusal in a fourth, so it is the fixture that would catch a
//              migration that dropped cfa_type on the way through.
//   2 rgb    - three channels, plus one NaN and one +Inf at fixed positions:
//              the non-finite handling is per-plugin and each does it its own
//              way, which is exactly the kind of thing a rewrite loses.
//   3 edge   - a slanted edge at atan(0.15) = 8.5 deg, inside iso12233/e-sfr's
//              documented 5-40 deg window, so the one analyzer with a curve
//              actually produces one.
static std::unique_ptr<ImageDoc> bunFixture(int which) {
    auto d = std::make_unique<ImageDoc>();
    if (which == 0 || which == 1) {
        d->name = which == 0 ? "bundled/flat" : "bundled/bayer";
        d->src->w = 96; d->src->h = 64; d->src->ch = 1; d->src->dtype = "u16";
        d->src->data.resize((size_t)96 * 64);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 96; x++)
                d->src->data[(size_t)y * 96 + x] =
                    (float)(1000 + ((x * 7 + y * 13) % 17) + (x & 1) * 3 + (y & 1) * 5);
        if (which == 1) { d->cfa = 1; d->cfaPattern = 0; }   // RGGB
    } else if (which == 2) {
        d->name = "bundled/rgb";
        d->src->w = 48; d->src->h = 32; d->src->ch = 3; d->src->dtype = "f32";
        d->src->data.resize((size_t)48 * 32 * 3);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 48; x++)
                for (int c = 0; c < 3; c++)
                    d->src->data[((size_t)y * 48 + x) * 3 + c] =
                        (float)(100 * (c + 1) + ((x * 5 + y * 11 + c * 3) % 13));
        d->src->data[((size_t)10 * 48 + 10) * 3 + 1] = std::numeric_limits<float>::quiet_NaN();
        d->src->data[((size_t)4 * 48 + 20) * 3 + 2] = std::numeric_limits<float>::infinity();
    } else {
        d->name = "bundled/edge";
        d->src->w = 96; d->src->h = 64; d->src->ch = 1; d->src->dtype = "f32";
        d->src->data.resize((size_t)96 * 64);
        for (int y = 0; y < 64; y++) {
            const double edge = 24.0 + 0.15 * y;
            for (int x = 0; x < 96; x++) {
                double t = (double)x - edge + 0.5;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                d->src->data[(size_t)y * 96 + x] = (float)(100.0 + 800.0 * t);
            }
        }
    }
    d->syncMirrors();
    d->uid = app.nextUid++;
    return d;
}

static int bundledSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "bundledselftest: %-72s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto shutdown = [&]() {
        stopRbWorker(); stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker();
    };

    // The REAL dlls, loaded by startup out of exeDir()/plugins before parseCli
    // ran - the same ones a user gets. No fixture directory is loaded here on
    // purpose: this test is about what is SHIPPED, and a twin standing in for
    // one would prove something about the twin.
    const auto& anas = plugin_host::analyzers();
    const int nBundled = (int)(sizeof kBundledAnalyzers / sizeof *kBundledAnalyzers);
    std::vector<int> idx(nBundled, -1);
    for (int b = 0; b < nBundled; b++)
        for (int i = 0; i < (int)anas.size(); i++)
            if (anas[i].name == kBundledAnalyzers[b]) { idx[b] = i; break; }
    bool allHere = true;
    for (int b = 0; b < nBundled; b++) {
        fprintf(stderr, "bundledselftest: ledger: %-22s abi=%u version=%s <- %s\n",
                kBundledAnalyzers[b],
                idx[b] < 0 ? 0u : anas[idx[b]].abi,
                idx[b] < 0 ? "(absent)"
                           : (anas[idx[b]].version.empty() ? "(none)"
                                                           : anas[idx[b]].version.c_str()),
                idx[b] < 0 ? "(absent)" : anas[idx[b]].file.c_str());
        if (idx[b] < 0) allHere = false;
    }
    check(allHere, "B1 all five bundled analyzers loaded from the shipped directory");
    if (!allHere) {
        fprintf(stderr, "bundledselftest: FAILED\n");
        shutdown();
        return 1;
    }

    // ---- 1. what each one now DECLARES (docs/abi-v3.md §3.1) ---------------
    {
        bool v3 = true, versioned = true;
        for (int b = 0; b < nBundled; b++) {
            if (anas[idx[b]].abi != 3) v3 = false;
            if (anas[idx[b]].version.empty()) versioned = false;
        }
        check(v3, "B2 every one of them registered through the v3 mouth");
        check(versioned, "B3 ...and declares a non-empty version (§3.1)");
    }
    {
        // The judgment this scheme rests on, asked as a test rather than left in
        // a commit message. §10 compares versions for EQUALITY between two
        // machines, so a version derived from the build would refuse two peers
        // that agree about every line of the computation - which is the failure
        // mode parity exists to prevent, arriving from the other direction. The
        // viewer's own version IS a build id (cmake/gitversion.cmake, "<hash>"
        // or "<hash>+local"), and that is right for a binary and wrong here.
        const std::string vv = viewerVersion();
        bool independent = true;
        for (int b = 0; b < nBundled; b++) {
            const std::string& v = anas[idx[b]].version;
            if (v == vv || v.find("+local") != std::string::npos) independent = false;
        }
        check(independent,
              "B5 ...and it is NOT the build id: two peers that agree must not refuse");
    }

    // ---- 2. where it surfaces (§11) ----------------------------------------
    // Asserted through the REAL panel and the REAL Copy-table document, on the
    // first fixture, for every one of the five: a version in the ledger that
    // never reaches a citation has closed nothing.
    closeAll();
    for (int f = 0; f < 4; f++) app.images.push_back(bunFixture(f));
    app.current = 0;
    app.anns.clear(); app.nextAnnId = 1; app.selectedAnn = 0;

    ImGuiIO& io = ImGui::GetIO();
    auto runPanel = [&](int which) {
        app.anaSel = which;
        app.anaRunRequest = true;          // exactly what the Measure menu sets
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
        ImGui::Begin("BundledProbe", nullptr,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
        drawPanelAnalysis();
        ImGui::End();
        ImGui::EndFrame();
    };

    const auto& ana = app.ana;
    {
        bool statusLine = true, tsvLine = true;
        for (int b = 0; b < nBundled; b++) {
            const AnalyzerPluginInfo& a = anas[idx[b]];
            runPanel(idx[b]);
            fprintf(stderr, "bundledselftest: prov: %s\n",
                    ana.prov.substr(ana.prov.rfind("  |  ") == std::string::npos
                                        ? 0 : ana.prov.rfind("  |  ") + 5).c_str());
            if (ana.prov.find(analyzerTag(a.name, a.version, a.file)) == std::string::npos ||
                a.version.empty())
                statusLine = false;
            const std::string tsv = buildAnalysisTsv();
            if (tsv.find("# plugin: " + analyzerCitation(a.version, a.path) + "\n") ==
                    std::string::npos || a.version.empty())
                tsvLine = false;
        }
        check(statusLine, "B6 the status line reads 'name <version> (dll)' for all five");
        check(tsvLine, "B7 ...and the Copy-table '# plugin:' line carries it too");
    }

    // ---- 2b. the version, pinned beside the contract it is ABOUT ------------
    // A version that nobody ever bumps passes every check above forever and
    // makes §10's parity say "same build" about two builds that differ - the
    // mirror image of a build-hash version, and the harder failure to see,
    // because nothing is ever red. Nothing portable can catch the whole of it
    // (a value that moves by a rounding differs across the three compilers in
    // the matrix, so no committed number could be asserted on all of them) -
    // but the RESULT CONTRACT can: keys and units are strings, identical on
    // every OS, and they are what a caller on the other end of §10 actually
    // depends on. So each declared version is written down here next to the
    // key/unit list it names. Gain a key, lose one, rename one or change a
    // unit, and this reddens with a diff and asks for the version line in the
    // same breath. It is a ratchet, not a proof; the values are held by the
    // stdout diff described at the top of this file.
    struct BunContract { const char* name; const char* version; int fixture; const char* keys; };
    static const BunContract kContract[] = {
        { "stats/moments", "1.0.0", 0,
          "pixels:px|ch0.mean:DN|ch0.var:DN^2|ch0.std:DN|ch0.entropy:bit|ch0.min:DN|"
          "ch0.max:DN|ch0.p1:DN|ch0.p50:DN|ch0.p99:DN|finite ratio:ratio" },
        { "noise/floor", "1.0.0", 0,
          "ch0.mean:DN|ch0.std:DN|ch0.noise:DN|ch0.snr_db:dB|method:" },
        { "uniformity/prnu-fpn", "1.0.0", 0,
          "ch0.mean:DN|ch0.prnu_pct:%|ch0.shading_pct:%|ch0.row_fpn_pct:%|"
          "ch0.col_fpn_pct:%|method:" },
        { "sharpness/gradient", "1.0.0", 0,
          "varlap:a.u.|tenengrad:a.u.|grad_mean:a.u." },
        // the edge fixture: the only one iso12233/e-sfr answers on, and the
        // only analyzer whose main product is a curve rather than a row
        { "iso12233/e-sfr", "1.0.0", 3,
          "mtf50 (cy/px):cy/px|mtf20 (cy/px):cy/px|sfr@nyquist:ratio|"
          "edge_angle_deg:deg|lines_used:lines|method:" },
    };
    {
        bool contractOk = true;
        for (const BunContract& c : kContract) {
            int ai = -1;
            for (int i = 0; i < (int)anas.size(); i++)
                if (anas[i].name == c.name) { ai = i; break; }
            if (ai < 0) { contractOk = false; continue; }
            app.current = c.fixture;
            app.anns.clear(); app.nextAnnId = 1; app.selectedAnn = 0;
            app.annRev++;
            runPanel(ai);
            std::string got;
            for (size_t k = 0; k < ana.keys.size(); k++) {
                if (k) got += "|";
                got += ana.keys[k] + ":" +
                       (k < ana.units.size() ? ana.units[k] : std::string());
            }
            if (got != c.keys || anas[ai].version != c.version) {
                contractOk = false;
                fprintf(stderr,
                        "bundledselftest: %s: the result contract or the version moved.\n"
                        "  version want <%s> got <%s>\n"
                        "  keys    want %s\n"
                        "  keys    got  %s\n"
                        "  If the change is intended, bump the version in the plugin and\n"
                        "  this line together - they are one statement.\n",
                        c.name, c.version, anas[ai].version.c_str(), c.keys, got.c_str());
            }
        }
        check(contractOk,
              "B4 each version is pinned beside the key/unit contract it names");
    }

    // ---- 3. nothing else moved ---------------------------------------------
    // The accent, first, because it is the one behaviour that CHANGES MECHANISM
    // without being allowed to change outcome: before the migration the host's
    // table (analyzerHeadlineSuffix) picks the row; after it the descriptor's
    // own `headline` does, and the table is not consulted at all for a v3
    // analyzer (§3.2). So the expectation is computed FROM THE TABLE, here, and
    // compared against what the panel actually accented. Before the migration
    // that is a tautology and passes; after it, it is the whole handover.
    bool accentSame = true;
    // ...and the results themselves. Everything below goes to stdout and
    // nothing about it may depend on a version, a clock or a path.
    for (int f = 0; f < 4; f++) {
        app.current = f;
        for (int pass = 0; pass < 2; pass++) {
            app.anns.clear();
            app.nextAnnId = 1;
            app.selectedAnn = 0;
            if (pass == 1) {
                App::Ann roi;
                roi.id = 1; roi.type = 0;
                roi.x = 8; roi.y = 6; roi.w = 48; roi.h = 40;
                roi.label = "R1"; roi.color = 1; roi.visible = true;
                app.anns.push_back(roi);
                app.nextAnnId = 2;
            }
            app.annRev++;
            for (int b = 0; b < nBundled; b++) {
                const AnalyzerPluginInfo& a = anas[idx[b]];
                runPanel(idx[b]);
                printf("=== %s | %s | %s\n", a.name.c_str(),
                       app.images[f]->name.c_str(), pass ? "roi" : "whole");
                if (!ana.err.empty()) printf("err: %s", ana.err.c_str());
                // The document a report pastes, minus its '# ' comment lines:
                // those are the provenance (a clock, an elapsed time, and the
                // version this PR adds), and none of them is a result.
                const std::string tsv = buildAnalysisTsv();
                for (size_t i = 0; i < tsv.size();) {
                    size_t e = tsv.find('\n', i);
                    if (e == std::string::npos) e = tsv.size();
                    if (tsv.compare(i, 2, "# ") != 0)
                        printf("%.*s\n", (int)(e - i), tsv.c_str() + i);
                    i = e + 1;
                }
                // Which row wears the accent, by name rather than by index.
                for (size_t k = 0; k < ana.keys.size(); k++)
                    if (k < ana.headline.size() && ana.headline[k])
                        printf("accent: %s\n", ana.keys[k].c_str());
                // ...and the same question asked of the host table that used to
                // answer it. A v3 headline is declared already channel-stripped
                // (§3.2) while the table's entries are suffixes (".noise"), so
                // the two rules differ - the OUTCOME may not.
                {
                    const char* hl = analyzerHeadlineSuffix(a.name);
                    const size_t hn = hl ? strlen(hl) : 0;
                    for (size_t k = 0; k < ana.keys.size(); k++) {
                        const std::string& key = ana.keys[k];
                        const bool want = hl && key.size() >= hn &&
                                          key.compare(key.size() - hn, hn, hl) == 0;
                        const bool got = k < ana.headline.size() && ana.headline[k] != 0;
                        if (want != got) {
                            accentSame = false;
                            fprintf(stderr, "bundledselftest: accent moved: %s / %s: "
                                            "'%s' table=%d declared=%d\n",
                                    a.name.c_str(), app.images[f]->name.c_str(),
                                    key.c_str(), (int)want, (int)got);
                        }
                    }
                }
                // The doubles, at full precision, straight off the ABI - the
                // panel's %.6g above is what a person reads, this is what the
                // plugin actually returned.
                {
                    std::vector<std::pair<std::string, std::string>> rows;
                    std::vector<std::string> series;
                    g_bunRows = &rows;
                    g_bunSeries = &series;
                    psFrame fr = makeFrame(*app.images[f]);
                    psRect rr{ 8, 6, 48, 40 };
                    char perr[256] = { 0 };
                    const int32_t rc = bunRunRaw(a, &fr, pass ? &rr : nullptr,
                                                 perr, sizeof perr);
                    g_bunRows = nullptr;
                    g_bunSeries = nullptr;
                    printf("raw rc=%d err=%s\n", (int)rc, perr);
                    for (const auto& kv : rows)
                        printf("raw %s\t%s\n", kv.first.c_str(), kv.second.c_str());
                    for (const auto& s : series) printf("raw %s\n", s.c_str());
                }
                printf("\n");
            }
        }
    }
    check(accentSame,
          "B8 the declared headline accents exactly the rows the host table did");

    // ---- 4. and the paths that must outlive the migration -------------------
    // The five were the last shipped users of register_analyzer2, so from this
    // commit on the V1/V2 mouths are exercised only by third-party dlls and by
    // the ABI twins in plugins-abitest (selftest.anaprov AP13/AP18 runs one
    // measurement through both paths and compares the rows). What THIS test can
    // still ask, cheaply, is that the seats are there and that a V2 descriptor
    // registered right now lands as V2 with an empty version - the host is not
    // to start inventing one just because everything shipped has one.
    {
        const psHostApi* api = plugin_host::hostApi();
        const size_t before = plugin_host::analyzers().size();
        bool seats = api && api->register_analyzer && api->register_analyzer2 &&
                     api->register_analyzer3 && api->register_stack_analyzer3;
        bool tookIt = false, blank = false;
        if (seats) {
            psAnalyzerV2 late{};
            late.abi_version = 2u;
            late.caps = PS_CAP_CPU;
            late.name = "bundledtest/late-v2";
            late.description = "a third-party V2 dll, arriving after the migration";
            late.analyze = bunProbeV2;
            tookIt = api->register_analyzer2(api->ctx, &late) == 0 &&
                     plugin_host::analyzers().size() == before + 1;
            if (tookIt) {
                const AnalyzerPluginInfo& l = plugin_host::analyzers().back();
                blank = l.abi == 2 && l.version.empty() && l.headline.empty();
            }
        }
        check(seats, "B9 all four register seats are still filled (V1/V2 included)");
        check(tookIt && blank,
              "B10 ...and a V2 descriptor still registers, with NO invented version");
    }

    fprintf(stderr, "bundledselftest: %s\n", ok ? "ok" : "FAILED");
    shutdown();
    return ok ? 0 : 1;
}
