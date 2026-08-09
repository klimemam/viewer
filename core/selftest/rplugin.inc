// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: remote/plugin
// ---- --rplugin-selftest: plugin analysis over the link (ABI v3 §10) --------
//
// The op this exercises exists to make ONE thing impossible: a measurement that
// silently came from a different build of the plugin than the one that was
// asked for. Everything else here - the numbers, the stack pull, the partial
// facts - is in service of that, because a refusal nobody can trust is worth
// nothing and a refusal that fires when it should not is worse than none.
//
// So the subject is a REAL peer process (viewer-serve, started over a pipe by
// remote::Session, the same way --remote-selftest starts one) loading REAL
// fixture dlls out of plugins-abitest, which the test's ctest ENVIRONMENT hands
// it as VIEWER_SERVE_PLUGINS. Both ends load the same two files; what differs
// is what the CLIENT declares in the request, which is exactly what differs
// when two machines are one release apart.
//
// The three references are chosen the way this repo already chooses them:
//   - the frame analyzer's numbers come from running the SAME dll locally and
//     comparing %.9g strings, which is what --remote-selftest does for
//     MOP_ANALYZER (one implementation, two transports);
//   - the stack analyzer's numbers are recomputed client-side from TILES, a
//     path --remote-selftest already proved correct, in the fixture's own
//     accumulation order so the comparison can stay exact;
//   - the refusals are compared as SENTENCES, because a refusal whose wording
//     drifts is a refusal a person stops reading.
//
// VIEWER_SERVE_PIN_BUDGET_MB=1 is not decoration either: bench_stack's frames
// are 0.3125 MB, so eight of them cannot be resident at once inside a 1 MB
// budget. The stack fixture releases each frame before taking the next, and
// RP15 is the first time anything measures that the release actually recycles
// - which is the whole reason §5.1 has a release_frame at all (§9.2).

// %.9g on the numbers, verbatim on the text: the same rule --remote-selftest's
// MEASURE comparison uses, so a difference in the ninth digit is a failure
// rather than a rounding both sides agreed to hide.
static std::vector<std::pair<std::string, std::string>>* g_rpRows;
static void rpNum(void*, const char* k, double v) {
    char b[64];
    snprintf(b, 64, "%.9g", v);
    g_rpRows->emplace_back(k ? k : "", b);
}
static void rpTxt(void*, const char* k, const char* v) {
    g_rpRows->emplace_back(k ? k : "", v ? v : "");
}
static void rpSer(void*, const char*, const char*, const char*,
                  const float*, const float*, uint32_t) {}

static std::string rpFmt(const remote::MeasureItem& it) {
    if (it.kind != 0) return it.text;
    char b[64];
    snprintf(b, 64, "%.9g", it.num);
    return b;
}
static bool rpFind(const remote::MeasureResult& r, const char* key, std::string& out) {
    if (r.cols.empty()) return false;
    for (const auto& it : r.cols[0])
        if (it.key == key) { out = rpFmt(it); return true; }
    return false;
}

static int rpluginSelfTest(const char* exe, const char* path) {
    int bad = 0;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "rpluginselftest: %-70s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) bad++;
    };
    // The fixtures, loaded HERE the same deliberate way --anaprov-selftest and
    // --stackana-selftest load them - out of their own directory, through the
    // real loader. The peer loads the same directory from its own side; that
    // the two lists agree is not assumed anywhere below, it is asked.
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins-abitest",
                           plugin_host::exeDir() + "/../plugins-abitest" },
                         [](const std::string& m, bool e) {
                             fprintf(stderr, "rpluginselftest: load: %s%s\n",
                                     e ? "ERROR " : "", m.c_str());
                         });
    const AnalyzerPluginInfo* v3 = nullptr;
    const AnalyzerPluginInfo* v2 = nullptr;
    for (const auto& a : plugin_host::analyzers()) {
        if (a.name == "abitest/twin-v3") v3 = &a;
        if (a.name == "abitest/twin-v2") v2 = &a;
    }
    if (!v3 || !v2) {
        fprintf(stderr, "rpluginselftest: the ABI twins are not in plugins-abitest "
                        "(built?) - nothing to compare\n");
        return 1;
    }

    remote::Session s;
    std::string err;
    if (!s.start("", exe, err)) {
        fprintf(stderr, "rpluginselftest: peer: %s\n", err.c_str());
        return 1;
    }
    check(s.peerVersion() >= 7,
          "RP1 the peer speaks protocol 7 - parity is askable at all");

    std::string localErr = loadNpy(path);
    if (!localErr.empty() || !cur()) {
        fprintf(stderr, "rpluginselftest: local load failed: %s\n", localErr.c_str());
        return 1;
    }
    const ImageDoc& ref = *cur();
    remote::Meta meta;
    if (!s.meta(path, meta, err)) {
        fprintf(stderr, "rpluginselftest: META: %s\n", err.c_str());
        return 1;
    }
    const int RX = 3, RY = 5, RW = 64, RH = 48;

    // ---- 1. a FRAME analyzer whose versions agree ---------------------------
    std::vector<std::pair<std::string, std::string>> local;
    {
        g_rpRows = &local;
        psFrame f = makeFrame(ref);
        psRect roi{ (uint32_t)RX, (uint32_t)RY, (uint32_t)RW, (uint32_t)RH };
        psAnalyzeSink3 sk{ nullptr, rpNum, rpTxt, rpSer, {} };
        char perr[256] = { 0 };
        if (v3->v3.analyze(&f, &roi, &sk, perr, sizeof perr) != 0) {
            fprintf(stderr, "rpluginselftest: the local run failed: %s\n", perr);
            return 1;
        }
        g_rpRows = nullptr;
    }
    remote::MeasureReq q;
    q.op = rp::MOP_PLUGIN_ANALYZE;
    q.target = rp::MT_FRAME;
    q.paths = { path };
    q.analyzer = v3->name;
    q.analyzerVersion = v3->version;         // verbatim from OUR ledger
    q.cfaType = ref.cfa;
    q.cfaPattern = ref.cfaPattern;
    q.black = effBlack(ref);
    q.white = effWhite(ref);
    q.rois.push_back({ RX, RY, RW, RH });
    remote::MeasureResult mr;
    bool ran = s.measure(q, mr, err);
    check(ran, "RP2 a v3 frame analyzer with matching name+version runs on the peer");
    if (!ran) fprintf(stderr, "rpluginselftest: RP2: %s\n", err.c_str());
    bool same = ran && mr.cols.size() == 1 && mr.cols[0].size() == local.size();
    if (same)
        for (size_t i = 0; i < local.size(); i++) {
            const remote::MeasureItem& it = mr.cols[0][i];
            if (it.key != local[i].first || rpFmt(it) != local[i].second) {
                fprintf(stderr, "rpluginselftest: RP3 %s: peer '%s' vs local '%s'\n",
                        it.key.c_str(), rpFmt(it).c_str(), local[i].second.c_str());
                same = false;
            }
        }
    check(same, "RP3 ...and its numbers are the local dll's, key for key, %.9g exact");
    // #46's remaining half. Before this op a remote measurement came back with
    // no answer at all to "which plugin, which build, which file" - the one
    // thing every local result has carried since PR #98.
    fprintf(stderr, "rpluginselftest: prov(frame): %s\n",
            analyzerTag(mr.provName, mr.provVersion, mr.provFile).c_str());
    check(ran && mr.provName == v3->name && mr.provVersion == "3.1.4-twin (2026-08-10)" &&
              mr.provFile.find("abi_v3_twin") != std::string::npos,
          "RP4 ...and the reply names the PEER's analyzer, version and dll (#46)");
    check(ran && !mr.provPath.empty() && mr.provPath.find(mr.provFile) != std::string::npos,
          "RP5 ...and the peer's own absolute path, for the tooltip and the export");
    check(analyzerTag(mr.provName, mr.provVersion, mr.provFile) ==
              analyzerTag(v3->name, v3->version, v3->file) &&
          analyzerCitation(mr.provVersion, mr.provPath) ==
              "<" + mr.provVersion + "> " + mr.provPath,
          "RP20 ...spelled by the one citation both a local and a peer result use");

    // ---- 2. the refusals ----------------------------------------------------
    {   // A different build. Nothing else about the request changes - this IS
        // what one machine being a release behind looks like on the wire.
        remote::MeasureReq q2 = q;
        q2.analyzerVersion = v3->version + "+dirty";
        remote::MeasureResult r2;
        std::string e2;
        bool ok2 = s.measure(q2, r2, e2);
        fprintf(stderr, "rpluginselftest: refusal(mismatch):\n%s\n", e2.c_str());
        check(!ok2, "RP6 a version MISMATCH is refused");
        check(e2.find("<" + q2.analyzerVersion + ">") != std::string::npos &&
                  e2.find("<" + v3->version + ">") != std::string::npos,
              "RP7 ...with BOTH versions quoted on the line, neither one winning");
        check(!ok2 && r2.cols.empty() && r2.series.empty() && r2.provName.empty(),
              "RP8 ...and NOTHING was measured - no rows, no quiet local fallback");
    }
    {   // The client declares nothing (a V1/V2 descriptor). Two absences are not
        // a match: §10's parity is a comparison of DECLARATIONS, and there is
        // nothing here to compare.
        remote::MeasureReq q2 = q;
        q2.analyzerVersion.clear();
        remote::MeasureResult r2;
        std::string e2;
        bool ok2 = s.measure(q2, r2, e2);
        check(!ok2 && e2.find("declares no version on the client") != std::string::npos,
              "RP9 an UNDECLARED client version is refused, not read as a match");
        // ...and symmetrically for the peer: abitest/twin-v2 is a V2 descriptor
        // and carries no version field at all, so no client string can match it.
        remote::MeasureReq q3 = q;
        q3.analyzer = v2->name;
        q3.analyzerVersion = "1.0";
        remote::MeasureResult r3;
        std::string e3;
        bool ok3 = s.measure(q3, r3, e3);
        fprintf(stderr, "rpluginselftest: refusal(undeclared peer):\n%s\n", e3.c_str());
        check(!ok3 && e3.find("none on the peer") != std::string::npos &&
                  e3.find("MOP_ANALYZER") != std::string::npos,
              "RP10 ...and a V2 analyzer on the PEER is refused, pointing at the old op");
    }
    {   // Not there at all. The peer says what it does have, because "not found"
        // over a link is usually a deployment fact and never a typo.
        remote::MeasureReq q2 = q;
        q2.analyzer = "abitest/not-installed";
        remote::MeasureResult r2;
        std::string e2;
        bool ok2 = s.measure(q2, r2, e2);
        check(!ok2 && e2.find("no frame analyzer named") != std::string::npos &&
                  e2.find("abitest/twin-v3") != std::string::npos,
              "RP11 an unknown name is refused, naming what the peer DOES have");
    }

    // ---- 3. the stack mouth over the wire (§10's frame/stack distinction) ---
    const uint32_t NFR = 8;
    {
        remote::MeasureReq qs;
        qs.op = rp::MOP_PLUGIN_ANALYZE;
        qs.target = rp::MT_STACK;
        qs.paths = { path };
        qs.frame0 = 0;
        qs.frameCount = (int)NFR;
        qs.analyzer = "abitest/stack-tmean";
        qs.analyzerVersion = "stack-2.0.0-twin (2026-08-10)";
        qs.rois.push_back({ RX, RY, RW, RH });
        remote::MeasureResult rs;
        std::string es;
        bool oks = s.measure(qs, rs, es);
        check(oks, "RP12 a STACK analyzer runs on the peer, pulled frame by frame");
        if (!oks) fprintf(stderr, "rpluginselftest: RP12: %s\n", es.c_str());
        // The partial-load FACTS are the peer's own count (§6, §10): it read 8
        // of the 120 the file declares, and it says both. A client that
        // asserted its own n/N here would be describing a load it did not do.
        check(oks && rs.framesUsed == (int)NFR && rs.expected == meta.frames,
              "RP13 ...and n/N are the PEER's facts: 8 of 120, counted there");
        // The reference: the same aggregation over tiles, in the fixture's own
        // order (sum per sample across frames, divide, then average over the
        // samples that had a value) so the comparison can be string-exact.
        double acc = 0.0, valued = 0.0, s0 = 0.0;
        {
            const size_t nS = (size_t)RW * RH * ref.ch;
            std::vector<double> sum(nS, 0.0);
            std::vector<uint32_t> cnt(nS, 0);
            bool fetched = true;
            for (uint32_t f = 0; f < NFR && fetched; f++) {
                std::vector<float> px;
                int tw = 0, th = 0, tch = 0;
                std::string dt;
                if (!s.tile(path, (int)f, RX, RY, RW, RH, 1, px, tw, th, tch, dt, err) ||
                    px.size() != nS) {
                    fprintf(stderr, "rpluginselftest: reference tile %u: %s\n", f, err.c_str());
                    fetched = false;
                    break;
                }
                for (size_t i = 0; i < nS; i++) {
                    double v = px[i];
                    if (!std::isfinite(v)) continue;
                    sum[i] += v; cnt[i]++;
                }
            }
            if (!fetched) { bad++; }
            for (size_t i = 0; i < nS; i++)
                if (cnt[i]) { acc += sum[i] / (double)cnt[i]; valued += 1.0; }
            s0 = cnt[0] ? sum[0] / (double)cnt[0] : 0.0;
        }
        char want[64], want0[64];
        snprintf(want, sizeof want, "%.9g", valued > 0 ? acc / valued : 0.0);
        snprintf(want0, sizeof want0, "%.9g", s0);
        std::string gotMean, got0, gotFrames, gotExpected;
        rpFind(rs, "ch0.tmean", gotMean);
        rpFind(rs, "s0.tmean", got0);
        rpFind(rs, "frames", gotFrames);
        rpFind(rs, "expected", gotExpected);
        fprintf(stderr, "rpluginselftest: stack ch0.tmean peer=%s ref=%s | s0 peer=%s ref=%s"
                        " | frames=%s expected=%s\n",
                gotMean.c_str(), want, got0.c_str(), want0,
                gotFrames.c_str(), gotExpected.c_str());
        check(oks && gotMean == want && got0 == want0,
              "RP14 ...and its numbers equal the same aggregation over tiles, exactly");
        // Eight frames of 0.3125 MB inside a 1 MB pin budget: arithmetically
        // impossible unless release_frame really gives the slot back. This is
        // §9.2's bounded window, measured rather than promised.
        check(oks, "RP15 ...inside a 1 MB pin budget over 8 x 0.31 MB: release recycles");
        check(oks && rs.provName == "abitest/stack-tmean" &&
                  rs.provVersion == "stack-2.0.0-twin (2026-08-10)" &&
                  rs.provFile.find("abi_stack_twin") != std::string::npos,
              "RP16 ...and the stack result names the peer's dll and version too");
    }
    {   // The gate (§6) on the peer's side of the link, and the same order:
        // min_frames is settled BEFORE the call, from frames the peer counted.
        remote::MeasureReq qg;
        qg.op = rp::MOP_PLUGIN_ANALYZE;
        qg.target = rp::MT_STACK;
        qg.paths = { path };
        qg.frameCount = 3;
        qg.analyzer = "abitest/stack-needs-8";
        qg.analyzerVersion = "stack-2.0.0-twin (2026-08-10)";
        remote::MeasureResult rg;
        std::string eg;
        bool okg = s.measure(qg, rg, eg);
        fprintf(stderr, "rpluginselftest: gate: %s\n", eg.c_str());
        check(!okg && eg.find("needs at least 8 frames") != std::string::npos &&
                  eg.find("3 of 120") != std::string::npos,
              "RP17 min_frames is gated on the PEER, quoting n of N it counted");
        // The fixture's body emits gate.breached if it ever runs. Its ABSENCE is
        // the assertion: a refusal computed after the call looks identical in
        // the error string, and only the silence tells them apart.
        std::string breached;
        check(!rpFind(rg, "gate.breached", breached),
              "RP18 ...BEFORE the call: the body that would have shouted never ran");
    }
    {   // One past the end. §5.1 makes NULL the honest failure; the plugin turns
        // it into err + nonzero, and the peer appends WHICH null it was -
        // knowledge that only exists on this side of the ABI.
        remote::MeasureReq qo;
        qo.op = rp::MOP_PLUGIN_ANALYZE;
        qo.target = rp::MT_STACK;
        qo.paths = { path };
        qo.frameCount = 4;
        qo.analyzer = "abitest/stack-overrun";
        qo.analyzerVersion = "stack-2.0.0-twin (2026-08-10)";
        remote::MeasureResult ro;
        std::string eo;
        bool oko = s.measure(qo, ro, eo);
        fprintf(stderr, "rpluginselftest: overrun: %s\n", eo.c_str());
        check(!oko && eo.find("past the end") != std::string::npos &&
                  eo.find("[peer:") != std::string::npos,
              "RP19 an out-of-range pull is a NULL, and the peer says which NULL");
    }

    // ---- 4. the op that was already there, unchanged ------------------------
    {   // MOP_ANALYZER matches on the NAME and checks nothing else. That is what
        // it did before §10 and what it must go on doing: third-party callers
        // and every shipped V2 analyzer are on it, and "the old path is
        // unchanged" is a claim worth a line rather than a paragraph.
        remote::MeasureReq qa = q;
        qa.op = rp::MOP_ANALYZER;
        qa.analyzerVersion = "9.9.9-would-be-refused";
        remote::MeasureResult ra;
        std::string ea;
        bool oka = s.measure(qa, ra, ea);
        bool eq = oka && ra.cols.size() == 1 && ra.cols[0].size() == local.size();
        if (eq)
            for (size_t i = 0; i < local.size(); i++)
                if (ra.cols[0][i].key != local[i].first ||
                    rpFmt(ra.cols[0][i]) != local[i].second) eq = false;
        check(eq, "RP21 MOP_ANALYZER is untouched: name-only, same numbers as before");
        check(oka && ra.provName.empty() && ra.expected == 0,
              "RP22 ...and it carries no provenance trailer - it never claimed one");
    }

    s.stop();
    fprintf(stderr, "rpluginselftest: %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
