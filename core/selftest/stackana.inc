// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- --stackana-selftest: a stack handed to a plugin (ABI v3 §5 / §6) ------
//
// Stage 1 gave v3 a frame mouth. This is the stack mouth, and the thing that
// makes it worth a selftest of its own is that almost none of it is visible in
// the header: §5 is a set of promises about what happens DURING a call - who
// serves what, who may hold it for how long, what a NULL means - and §6 is a
// rule about the order two different things are decided in. A struct compiles
// whether or not any of that is true.
//
// So the subject here is a REAL plugin (plugins/test/abi_stack_twin.c, loaded
// as a dll through the same plugin_host::loadAll every plugin goes through)
// pulling a REAL stack, and the number it returns is derived by hand from the
// fixture right beside the assert. The fixture is analytic and built in memory
// - this test opens no file, so it cannot be made to pass or fail by a fixture
// on disk drifting - and it is windowless for the roiStatsSelftest() reason:
// every assertion is a number or a string.
//
// The one thing a test like this must not do is hold a SeqInfo* across the
// plugin call. It does not, and neither does runStackAnalyzer: App::SeqTable
// reseats on every mutation by design (PR #128), so the two values the call
// needs are copied out and the pointer is dropped on the same line.

// The sink a v3 analyze_stack is handed. Numbers are kept as DOUBLES, not as
// the panel's "%.6g" strings: the fixture separates a correct exclusion from a
// plausible wrong one in the third decimal, and formatting first would throw
// away the evidence before the assert could look at it.
struct StackAnaRows {
    std::vector<std::pair<std::string, double>> nums;
    std::vector<std::pair<std::string, std::string>> texts;
    int series = 0;
};
static void stackAnaNumber(void* ctx, const char* key, double v) {
    ((StackAnaRows*)ctx)->nums.emplace_back(key ? key : "", v);
}
static void stackAnaText(void* ctx, const char* key, const char* v) {
    ((StackAnaRows*)ctx)->texts.emplace_back(key ? key : "", v ? v : "");
}
static void stackAnaSeries(void* ctx, const char* name, const char* xl, const char* yl,
                           const float* x, const float* y, uint32_t n) {
    (void)name; (void)xl; (void)yl; (void)x; (void)y; (void)n;
    ((StackAnaRows*)ctx)->series++;
}
// Present? and what was it. Absent is NOT zero - "no row" and "a row saying 0"
// are different claims and this test asserts on both.
static bool stackAnaNum(const StackAnaRows& r, const char* key, double* out) {
    for (const auto& kv : r.nums)
        if (kv.first == key) { if (out) *out = kv.second; return true; }
    return false;
}
// A valid analyze_stack for the descriptors the registration checks ask the
// host to REFUSE: validCommon rejects a NULL function pointer, and then the
// test would be watching the wrong rule turn the descriptor away (the AP27
// pattern, for the same reason).
static int32_t stackAnaProbe(const psStack*, const psRect*,
                             const psAnalyzeSink3*, char*, size_t) {
    return 0;
}

static int stackAnaSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "stackanaselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto shutdown = [&]() {
        stopRbWorker(); stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker();
    };
    // The ABI fixtures, loaded the same deliberate way --anaprov-selftest loads
    // them: out of their own directory, through the real loader, so what is
    // under test includes the registration path and not only the call.
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins-abitest",
                           plugin_host::exeDir() + "/../plugins-abitest" },
                         [](const std::string& m, bool e) {
                             fprintf(stderr, "stackanaselftest: load: %s%s\n",
                                     e ? "ERROR " : "", m.c_str());
                         });

    const float NA = std::numeric_limits<float>::quiet_NaN();
    const int W = 4, H = 2, NS = W * H;            // 8 samples, 1 channel

    // THE FIXTURE, and the whole derivation lives here.
    //
    //   frame k is uniform 100*(k+1)  ->  100, 200, 300
    //   sample 0 is NaN in frame 1 only   (the middle one)
    //   sample 1 is NaN in EVERY frame    (no value exists there at all)
    //
    // so, per sample, over 3 frames:
    //   s0      : 100, ---, 300   -> 2 values, mean (100+300)/2       = 200
    //   s1      : ---, ---, ---   -> 0 values, no mean exists
    //   s2..s7  : 100, 200, 300   -> 3 values, mean (100+200+300)/3   = 200
    //   excluded non-finite samples: 1 (s0) + 3 (s1)                  = 4
    //   samples with no value at all:                                   1
    //   mean over the 7 samples that HAVE a mean: 7*200 / 7           = 200
    //
    // The two plausible wrong answers, which is why this fixture and not a
    // uniform one: fold the NaN in as 0 over N=3, or keep 3 as the divisor
    // while skipping it, and BOTH give s0 = 400/3 = 133.333... and s1 = 0,
    // hence 200*6/8 + 133.333/8 = 166.667 for the overall mean. Every number
    // below is 200 exactly when the exclusion is per pixel and counted, and
    // nothing near it when it is not.
    auto buildStack = [&](int frames, int expected) -> int {
        closeAll();
        const int seqId = 1;
        for (int k = 0; k < frames; k++) {
            auto d = std::make_unique<ImageDoc>();
            d->name = "stackana f" + std::to_string(k);
            d->src->w = W; d->src->h = H; d->src->ch = 1;
            d->src->dtype = "f32";
            d->src->data.assign((size_t)NS, 100.0f * (float)(k + 1));
            if (k == 1) d->px()[0] = NA;
            d->px()[1] = NA;
            d->syncMirrors();
            d->uid = app.nextUid++;
            d->seqId = seqId;
            d->seqIndex = k;
            app.images.push_back(std::move(d));
        }
        App::SeqInfo si;
        si.id = seqId;
        si.name = "stackana";
        si.expectedFrames = expected;         // the DECLARED size, n/N's N
        app.seqs.push_back(si);
        app.current = 0;
        app.anns.clear(); app.nextAnnId = 1; app.selectedAnn = 0;   // whole frame
        return seqId;
    };

    // ---- the ledger: three stack analyzers, each with its declaration --------
    const auto& sas = plugin_host::stackAnalyzers();
    for (const auto& s : sas)
        fprintf(stderr, "stackanaselftest: ledger: %-24s <%s> min_frames=%u <- %s\n",
                s.name.c_str(), s.version.c_str(), s.minFrames,
                s.file.empty() ? "(no file)" : s.file.c_str());
    int iT = -1, iG = -1, iO = -1;
    for (int i = 0; i < (int)sas.size(); i++) {
        if (sas[i].name == "abitest/stack-tmean")   iT = i;
        if (sas[i].name == "abitest/stack-needs-8") iG = i;
        if (sas[i].name == "abitest/stack-overrun") iO = i;
    }
    check(iT >= 0 && iG >= 0 && iO >= 0,
          "S1 the stack mouth exists: three stack analyzers registered");
    // A missing fixture must REDDEN what depends on it rather than skip it: a
    // registration the host declined is exactly the state this test is for.
    static const StackAnalyzerPluginInfo kAbsent{};
    const StackAnalyzerPluginInfo& saT = iT >= 0 ? sas[iT] : kAbsent;
    const StackAnalyzerPluginInfo& saG = iG >= 0 ? sas[iG] : kAbsent;
    const StackAnalyzerPluginInfo& saO = iO >= 0 ? sas[iO] : kAbsent;
    check(saT.abi == 3 && saT.version == "stack-2.0.0-twin (2026-08-10)",
          "S2 the ledger carries the declared version, verbatim");
    check(saT.headline == "tmean" && saG.headline.empty(),
          "S3 ...and the declared headline, including 'none' as an answer");
    check(saT.minFrames == 2 && saG.minFrames == 8 && saO.minFrames == 1,
          "S4 ...and min_frames as DECLARED, never defaulted");
    check(!saT.file.empty() && !saT.path.empty(),
          "S5 the #46 stage-1 ledger names the dll for a stack analyzer too");

    // ---- S6-S12: the measurement, and who owned what while it ran -----------
    {
        int seq = buildStack(3, 3);
        StackAnaRows rows;
        StackServe sv;
        psAnalyzeSink3 sink = { &rows, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
        std::string err;
        bool rc = iT >= 0 && runStackAnalyzer(saT, seq, nullptr, &sink, err, &sv);
        for (const auto& kv : rows.nums)
            fprintf(stderr, "stackanaselftest: row: %-14s %.9g\n", kv.first.c_str(), kv.second);
        fprintf(stderr, "stackanaselftest: err='%s'  gets=%llu releases=%llu peak=%u\n",
                err.c_str(), (unsigned long long)sv.gets,
                (unsigned long long)sv.releases, sv.peakHeld);
        check(rc && err.empty(), "S6 a stack analyzer runs on an open stack");

        double v = 0;
        check(stackAnaNum(rows, "s0.tmean", &v) && v == 200.0,
              "S7 s0 (NaN in the middle frame) is (100+300)/2 = 200 exactly");
        check(stackAnaNum(rows, "ch0.tmean", &v) && v == 200.0,
              "S7 ...and the mean over the samples that HAVE one is 200, not 166.667");
        // The declaration §5.1 demands: excluded, counted, carried OUT. A
        // right number with no count is half the contract - the point is that
        // the reader can see what was left out, not that it was left out.
        check(stackAnaNum(rows, "nan_excluded", &v) && v == 4.0,
              "S8 the exclusion count is DECLARED in the result: 1 + 3 = 4");
        check(stackAnaNum(rows, "no_value", &v) && v == 1.0,
              "S8 ...and so is the sample no frame had a value for");
        bool policy = false;
        for (const auto& kv : rows.texts)
            if (kv.first == "nan_policy" &&
                kv.second.find("excluded per pixel and counted") != std::string::npos)
                policy = true;
        check(policy, "S8 ...in words as well as in a count");

        // The pull mouth, measured from the host end. One get and one release
        // per frame with a peak of ONE pinned at a time is what §9.2's bounded
        // window needs to be possible later; a plugin that never released
        // would still be correct here and would pin the whole stack there.
        check(sv.called && sv.gets == 3 && sv.releases == 3 && sv.nulls == 0,
              "S9 the plugin PULLED each frame once and gave each one back");
        check(sv.peakHeld == 1,
              "S9 ...holding one frame at a time, so a windowed transport fits");

        // ROI means for a stack what it means for a frame. The left column of
        // the fixture is where both NaN samples live, so an ROI that excludes
        // it must report zero exclusions - which is also the check that the
        // window is applied to every frame and not only to the first.
        {
            StackAnaRows r2;
            StackServe sv2;
            psAnalyzeSink3 s2 = { &r2, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
            psRect roi = { 2, 0, 2, 2 };            // the two right-hand columns
            std::string e2;
            bool rc2 = iT >= 0 && runStackAnalyzer(saT, seq, &roi, &s2, e2, &sv2);
            double x = -1, y = -1;
            check(rc2 && stackAnaNum(r2, "nan_excluded", &x) && x == 0.0 &&
                  stackAnaNum(r2, "ch0.tmean", &y) && y == 200.0,
                  "S10 an ROI measures the ROI: 0 exclusions, still 200");
        }

        // ---- the gate: refused BEFORE the call, and provably not inside it --
        {
            StackAnaRows r3;
            StackServe sv3;
            psAnalyzeSink3 s3 = { &r3, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
            std::string e3;
            bool rc3 = iG >= 0 && runStackAnalyzer(saG, seq, nullptr, &s3, e3, &sv3);
            fprintf(stderr, "stackanaselftest: gate err='%s'\n", e3.c_str());
            check(!rc3 && e3.find("at least 8") != std::string::npos &&
                  e3.find("has 3") != std::string::npos,
                  "S11 min_frames refuses with ONE line naming the need and the fact");
            // The witness. abitest/stack-needs-8 emits gate.breached from its
            // body, so the row's ABSENCE is what proves the body never ran -
            // a refusal computed after the call would look identical in `err`.
            check(!sv3.called && r3.nums.empty() && sv3.gets == 0,
                  "S11 ...BEFORE calling: the body never ran and no frame was served");
        }

        // ---- a NULL from get_frame is an honest failure ---------------------
        {
            StackAnaRows r4;
            StackServe sv4;
            psAnalyzeSink3 s4 = { &r4, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
            std::string e4;
            bool rc4 = iO >= 0 && runStackAnalyzer(saO, seq, nullptr, &s4, e4, &sv4);
            fprintf(stderr, "stackanaselftest: overrun err='%s'\n", e4.c_str());
            check(!rc4 && e4.find("past the end") != std::string::npos && sv4.nulls == 1,
                  "S12 an out-of-range index is NULL, and NULL comes back as a failure");
            check(!stackAnaNum(r4, "overrun.served", nullptr),
                  "S12 ...never a fabricated frame");
        }
    }

    // ---- S13: the partial load, carried as FACTS ---------------------------
    // 判断3's other half. The same three frames, now declaring a stack of 24:
    // the plugin is called (min_frames 2 <= 3) and does NOT get to refuse for
    // being given 3 of 24 - that veto was spent at registration. What it gets
    // is the honest pair, and it carries it out.
    {
        int seq = buildStack(3, 24);
        StackAnaRows rows;
        StackServe sv;
        psAnalyzeSink3 sink = { &rows, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
        std::string err;
        bool rc = iT >= 0 && runStackAnalyzer(saT, seq, nullptr, &sink, err, &sv);
        double n = 0, N = 0, m = 0;
        stackAnaNum(rows, "frames", &n);
        stackAnaNum(rows, "expected", &N);
        stackAnaNum(rows, "ch0.tmean", &m);
        fprintf(stderr, "stackanaselftest: partial n=%.0f N=%.0f mean=%.9g err='%s'\n",
                n, N, m, err.c_str());
        check(rc && err.empty(), "S13 a partly loaded stack is MEASURED, not refused");
        check(n == 3.0 && N == 24.0,
              "S13 ...and n / N cross the ABI as facts: 3 of 24, never 3 of 3");
        check(m == 200.0,
              "S13 ...over the frames that are here, not a different denominator");

        // ...and the gate reads the same fact, so its refusal can name it.
        {
            StackAnaRows r5;
            psAnalyzeSink3 s5 = { &r5, stackAnaNumber, stackAnaText, stackAnaSeries, {} };
            std::string e5;
            bool rc5 = iG >= 0 && runStackAnalyzer(saG, seq, nullptr, &s5, e5);
            fprintf(stderr, "stackanaselftest: partial gate err='%s'\n", e5.c_str());
            check(!rc5 && e5.find("3 of 24") != std::string::npos,
                  "S14 the refusal quotes n of N, so 3 of 24 never reads as 3 of 3");
        }
    }

    // ---- S15-S17: what the host refuses at REGISTRATION ---------------------
    // Asked of the real register function with the real list as the witness,
    // exactly as AP27 asks it of the frame mouth.
    {
        const psHostApi* api = plugin_host::hostApi();
        size_t before = plugin_host::stackAnalyzers().size();
        bool haveSeat = api && api->register_stack_analyzer3;
        check(haveSeat, "S15 register_stack_analyzer3 is a NAMED, non-NULL seat");

        bool zeroRefused = haveSeat, noVerRefused = haveSeat, dupRefused = haveSeat;
        if (haveSeat) {
            psStackAnalyzerV3 bad{};
            bad.abi_version = 3u;
            bad.caps = PS_CAP_CPU;
            bad.name = "abitest/stack-zero";
            bad.version = "0";
            bad.analyze_stack = stackAnaProbe;
            bad.min_frames = 0;                       // §6: declare, don't default
            if (api->register_stack_analyzer3(api->ctx, &bad) == 0) zeroRefused = false;

            bad.name = "abitest/stack-noversion";
            bad.min_frames = 1;
            bad.version = nullptr;
            if (api->register_stack_analyzer3(api->ctx, &bad) == 0) noVerRefused = false;
            bad.version = "";
            if (api->register_stack_analyzer3(api->ctx, &bad) == 0) noVerRefused = false;

            // One namespace across both mouths: "category/name" is what the
            // Measure menu shows and what remote parity compares (§10), so a
            // stack analyzer may not take a frame analyzer's name.
            bad.version = "1";
            bad.name = "abitest/twin-v3";             // the V3 FRAME twin's name
            if (api->register_stack_analyzer3(api->ctx, &bad) == 0) dupRefused = false;
        }
        check(zeroRefused, "S16 min_frames == 0 is refused at registration");
        check(noVerRefused, "S16 ...and so is a v3 descriptor with NULL/empty version");
        check(dupRefused && plugin_host::stackAnalyzers().size() == before,
              "S17 a name already taken by a frame analyzer is refused, list unchanged");
    }

    // ---- S18-S19: the V1/V2 path is untouched by any of this ----------------
    // Naming a reserved seat must move nothing. The compile-time half is the
    // static_assert block in plugin_host.cpp; this is the runtime half, asked
    // of the two dlls that carry ONE measurement through the V2 and the V3
    // frame paths - if the stack mouth had disturbed either, they would stop
    // agreeing, and the agreement is the thing under test.
    {
        closeAll();
        {
            auto d = std::make_unique<ImageDoc>();
            d->name = "stackana-frame";
            d->src->w = 4; d->src->h = 4; d->src->ch = 1;
            d->src->dtype = "f32";
            d->src->data.resize(16);
            for (int i = 0; i < 16; i++) d->px()[i] = (float)(100 + i);   // mean 107.5
            d->syncMirrors();
            d->uid = app.nextUid++;
            app.images.push_back(std::move(d));
        }
        app.current = 0;
        const auto& anas = plugin_host::analyzers();
        int v2i = -1, v3i = -1;
        for (int i = 0; i < (int)anas.size(); i++) {
            if (anas[i].name == "abitest/twin-v2") v2i = i;
            if (anas[i].name == "abitest/twin-v3") v3i = i;
        }
        std::vector<std::pair<std::string, std::string>> r2, r3;
        char e2[256] = { 0 }, e3[256] = { 0 };
        int32_t rc2 = 1, rc3 = 1;
        if (v2i >= 0 && v3i >= 0) {
            psFrame f = makeFrame(*app.images[0]);
            AnaEmit2Ctx c2 = { &r2, &app.ana, 0, -1 };
            psAnalyzeSink2 sink2 = { &c2, ana2Number, ana2Text, ana2Series, {} };
            rc2 = anas[v2i].v2.analyze(&f, nullptr, &sink2, e2, sizeof e2);
            AnaEmit2Ctx c3 = { &r3, &app.ana, 0, -1 };
            psAnalyzeSink3 sink3 = { &c3, ana2Number, ana2Text, ana2Series, {} };
            rc3 = anas[v3i].v3.analyze(&f, nullptr, &sink3, e3, sizeof e3);
        }
        check(v2i >= 0 && v3i >= 0 && rc2 == 0 && rc3 == 0,
              "S18 both frame twins still load and still run after the seat was named");
        check(!r2.empty() && r2 == r3,
              "S18 ...and the V2 and V3 frame paths still produce IDENTICAL rows");
        // The other half of "untouched": the V1/V2 register slots are still
        // there, still where they were, and a V2 descriptor still declares no
        // version that the host might be tempted to invent.
        const psHostApi* api = plugin_host::hostApi();
        check(api && api->register_analyzer && api->register_analyzer2 &&
              api->register_analyzer3 && api->struct_size == sizeof(psHostApi),
              "S19 every earlier register slot is still filled, psHostApi still its size");
        check(v2i >= 0 && anas[v2i].abi == 2 && anas[v2i].version.empty(),
              "S19 ...and a V2 analyzer's version stays EMPTY - nothing guessed");
    }

    closeAll();
    fprintf(stderr, "stackanaselftest: %s\n", ok ? "ok" : "FAILED");
    shutdown();
    return ok ? 0 : 1;
}
