// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // The Temporal panel's unified export, verifiable without a human
    // (docs/export-design.md 9 「検証」). This machine cannot screenshot GL, so the
    // assertions are on the STRING the buttons put on the clipboard: the
    // provenance fields, the section order, the per-plane rows, the units,
    // n-of-N honesty, the region statements (including the temporal-vs-profile
    // mismatch a server aggregate produces), the CSV dialect - and that the
    // numbers are the panel's own state FORMATTED, not re-derived. Plus the
    // per-frame x axis: parsing, the count-mismatch refusal, the resolver the
    // chart draws from, the export column and the session round-trip.
    if (!g_exportTsvSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 300.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "texportselftest: %-58s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        openFolder(g_exportTsvSelftest);
        loadAll();
        if (app.seqs.empty()) {
            fprintf(stderr, "texportselftest: no stack loaded under %s\n",
                    g_exportTsvSelftest.c_str());
            stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
            return 1;
        }
        const size_t npos = std::string::npos;
        int sid = app.seqs[0].id;
        std::vector<int> fr = framesOfSeq(sid);
        selectImage(fr.front());
        ImageDoc* im = cur();
        auto has = [&](const std::string& doc2, const std::string& what) {
            return doc2.find(what) != npos;
        };

        // ---- E1: whole image, full stack, compare off ----
        std::string tsv = buildTemporalExport('\t');
        fputs(tsv.c_str(), stdout);   // the document itself, for eyeballs and diffs
        fprintf(stderr, "texportselftest: E1 document: %zu bytes, %d lines, "
                        "stack '%s' %dx%d %s cfa=%d frames=%d\n",
                tsv.size(), (int)std::count(tsv.begin(), tsv.end(), '\n'),
                abDocLabel(im).c_str(), im->w, im->h, im->dtype.c_str(),
                im->cfa, (int)fr.size());
        check(has(tsv, "# temporal + H/V profile statistics"), "E1 title line");
        check(has(tsv, "# app: viewer "), "E1 provenance: app + version");
        check(has(tsv, "# sides: A"), "E1 provenance: sides listed");
        char pb[512];
        snprintf(pb, sizeof pb, "# A: %s  |  path: ", abDocLabel(im).c_str());
        check(has(tsv, pb), "E1 provenance: stack name and path");
        snprintf(pb, sizeof pb, "%dx%d %s", im->w, im->h, im->dtype.c_str());
        check(has(tsv, pb), "E1 provenance: size and dtype");
        check(has(tsv, "CFA: RGGB (4 planes)"), "E1 provenance: CFA pattern named");
        snprintf(pb, sizeof pb, "frames n=%d/%d", (int)fr.size(), (int)fr.size());
        check(has(tsv, pb), "E1 n of N on a full stack");
        check(has(tsv, "# A temporal region: whole image") &&
              has(tsv, "# A profile region: whole image"),
              "E1 both regions stated, whole image");
        size_t p1 = tsv.find("== temporal summary");
        size_t p2 = tsv.find("== H/V profile statistics");
        size_t p3 = tsv.find("== per-frame statistics");
        check(p1 != npos && p2 != npos && p3 != npos && p1 < p2 && p2 < p3,
              "E1 three sections in the designed order");
        {   // CFA planes never mixed: one row per plane, in both scalar tables
            bool sum4 = true, prof4 = true;
            for (const char* pl : { "R", "Gr", "Gb", "B" }) {
                std::string row = std::string("\nA\t") + pl + "\t";
                size_t s2 = tsv.find(row, p1);
                sum4 &= s2 != npos && s2 < p2;
                size_t s3 = tsv.find(row, p2);
                prof4 &= s3 != npos && s3 < p3;
            }
            check(sum4, "E1 summary: per-plane rows R/Gr/Gb/B");
            check(prof4, "E1 profile stats: per-plane rows R/Gr/Gb/B");
        }
        check(has(tsv, "sigma_t [DN]") && has(tsv, "sigma_fpn [DN]") &&
              has(tsv, "sigma_tot [DN]"), "E1 summary header units named");
        check(has(tsv, "sigma_frame [DN]") && has(tsv, "sigma_col [%]") &&
              has(tsv, "sigma_row [%]") && has(tsv, "mean [DN]"),
              "E1 profile/per-frame header units named");
        check(has(tsv, "no sigma_t^2/N subtraction"),
              "E1 sigma_fpn states exactly what it is");
        // every section is a rectangle: all non-# lines in a section carry the
        // same number of cells as their header
        auto rect = [&](const std::string& d2, size_t from, size_t to, const char* what) {
            int want = -1;
            bool good = true;
            // `from` points INTO the section-marker comment line: start at the
            // first full line after it
            size_t i = d2.find('\n', from);
            i = i == npos ? to : i + 1;
            while (i < to && i < d2.size()) {
                size_t e = d2.find('\n', i);
                if (e == npos || e > to) e = to;
                if (e > i && d2[i] != '#') {
                    int tabs = (int)std::count(d2.begin() + i, d2.begin() + e, '\t');
                    if (want < 0) want = tabs;
                    else if (tabs != want) good = false;
                }
                i = e + 1;
            }
            check(good && want > 0, what);
        };
        rect(tsv, p1, p2, "E1 summary section is a rectangle");
        rect(tsv, p2, p3, "E1 profile section is a rectangle");
        rect(tsv, p3, tsv.size(), "E1 per-frame section is a rectangle");
        {   // per-frame block: one row per resident frame, honesty line present
            snprintf(pb, sizeof pb, "resident %d of %d frame(s)", (int)fr.size(),
                     (int)fr.size());
            check(tsv.find(pb, p3) != npos, "E1 per-frame block: resident n of N");
        }

        // ---- E2: numbers are the panel's state formatted, not re-derived ----
        const App::TemporalState& T = app.temporal[0];
        check(T.valid && T.seqId == sid && T.nPl == 4, "E2 fixture: temporal state valid, 4 planes");
        {
            char n1[48], n2[48];
            snprintf(n1, sizeof n1, "%.9g", T.tempNoise[0]);
            snprintf(n2, sizeof n2, "%.9g", T.fixedPattern[0]);
            std::string sumRow = std::string("\nA\tR\t") + std::to_string(T.frames) + "\t" +
                                 std::to_string(T.frames) + "\t" + n1 + "\t" + n2 + "\t";
            size_t at = tsv.find(sumRow, p1);
            fprintf(stderr, "texportselftest: E2 summary row R: sigma_t=%s sigma_fpn=%s\n",
                    n1, n2);
            check(at != npos && at < p2,
                  "E2 sigma_t / sigma_fpn are temporal[0]'s numbers verbatim");
            const App::ProjState& P = app.proj[0];
            char m9[48], f9[48], v9[48], h9[48];
            snprintf(m9, sizeof m9, "%.9g", P.fStat[0].mean);
            snprintf(f9, sizeof f9, "%.9g", P.fStat[0].sd);
            snprintf(v9, sizeof v9, "%.9g", P.vStat[0].sd);
            snprintf(h9, sizeof h9, "%.9g", P.hStat[0].sd);
            std::string prow = std::string("\nA\tR\t") + m9 + "\t" + f9 + "\t" + v9 + "\t" + h9 + "\t";
            size_t pat = tsv.find(prow, p2);
            check(pat != npos && pat < p3,
                  "E2 profile row R is proj[0]'s stats verbatim");
        }

        // ---- E3: partial-stack honesty (n of N against the EXPECTED total) --
        if (App::SeqInfo* si = seqInfo(sid)) si->expectedFrames = (int)fr.size() + 3;
        {
            std::string tsvP = buildTemporalExport('\t');
            snprintf(pb, sizeof pb, "frames n=%d/%d", (int)fr.size(), (int)fr.size() + 3);
            check(has(tsvP, pb), "E3 partial stack says n of N, not n of n");
        }
        if (App::SeqInfo* si = seqInfo(sid)) si->expectedFrames = 0;

        // ---- E4: ROI selected - both regions state it, no mismatch note ----
        {
            App::Ann a;
            a.id = app.nextAnnId++;
            a.type = 0;
            a.x = 4; a.y = 4; a.w = 32; a.h = 24;
            a.label = "roi";
            app.anns.push_back(a);
            app.selectedAnn = a.id;
            app.annRev++;
        }
        std::string tsvR = buildTemporalExport('\t');
        check(has(tsvR, "# A temporal region: ROI (4,4) 32x24"),
              "E4 temporal region states the ROI");
        check(has(tsvR, "# A profile region: ROI (4,4) 32x24"),
              "E4 profile region states the ROI");
        check(!has(tsvR, "DIFFERENT regions"),
              "E4 matching regions carry no mismatch NOTE");

        // ---- E5: a server aggregate is whole-frame while the profiles follow
        // the ROI: the export states BOTH regions and flags the mismatch ----
        {
            App::ServerTemporal& S = app.srvTemporal;
            S = App::ServerTemporal{};
            S.valid = true;
            S.seqId = sid;
            S.host = "testhost";
            S.frames = (int)fr.size();
            S.nPl = 1;
            S.tempNoise[0] = 1.25; S.fixedPattern[0] = 2.5; S.totalNoise[0] = 2.795;
        }
        {
            std::string tsvS = buildTemporalExport('\t');
            check(has(tsvS, "# A temporal region: whole image"),
                  "E5 server temporal region = whole image");
            check(has(tsvS, "# A profile region: ROI (4,4) 32x24"),
                  "E5 profile region still the ROI");
            check(has(tsvS, "DIFFERENT regions"), "E5 mismatch NOTE present");
            check(has(tsvS, "\tserver testhost"), "E5 source column: server + host");
            size_t q1 = tsvS.find("== temporal summary");
            check(q1 != npos && tsvS.find("\nA\tall\t", q1) != npos,
                  "E5 pooled server reply labelled plane=all");
        }
        app.srvTemporal = App::ServerTemporal{};

        // ---- E6: the CSV sink - same content, different dialect ----
        {
            std::string csv = buildTemporalExport(',');
            check(std::count(csv.begin(), csv.end(), '\n') ==
                  std::count(tsvR.begin(), tsvR.end(), '\n'),
                  "E6 CSV and TSV have the same line count");
            check(csv.find('\t') == npos, "E6 CSV carries no tabs");
            check(expCell("a,b", ',') == "\"a,b\"" &&
                  expCell("a\"b", ',') == "\"a\"\"b\"" &&
                  expCell("a\tb", '\t') == "a b",
                  "E6 cell quoting rules (RFC 4180 minimal / tab squash)");
        }

        // ---- E7: the per-frame x axis --------------------------------------
        {
            std::vector<double> v;
            std::string err;
            bool c1 = parseAxisValues("10,20,30,40,50", v, err) && v.size() == 5 && v[2] == 30;
            bool c2 = parseAxisValues("10 20 30 40 50", v, err) && v.size() == 5;
            bool c3 = parseAxisValues("10\t20\t30\t40\t50", v, err) && v.size() == 5;
            bool c4 = parseAxisValues("10, 20\t30\n40 50", v, err) && v.size() == 5;
            // the two shapes a vertical Excel-column paste actually produces:
            // CRLF-separated lines, and comma-terminated lines
            bool c5 = parseAxisValues("10\r\n20\r\n30\r\n40\r\n50", v, err) &&
                      v.size() == 5 && v[4] == 50;
            bool c6 = parseAxisValues("10,\n20,\n30,\n40,\n50", v, err) &&
                      v.size() == 5 && v[0] == 10 && v[4] == 50;
            check(c1 && c2 && c3 && c4, "E7 comma / space / tab / mixed all parse");
            check(c5 && c6, "E7 newline and comma+newline (Excel column paste) parse");
            bool bad = !parseAxisValues("10,20,x,40,1e", v, err);
            fprintf(stderr, "texportselftest: E7 parse error: %s\n", err.c_str());
            check(bad && has(err, "value 3") && has(err, "'x'") && has(err, "value 5") &&
                  has(err, "'1e'") && v.empty(),
                  "E7 non-numeric tokens: positioned errors, no silent skip");
            check(!applyFrameAxis(sid, "temp", "", "10 20 30 40 50", err) &&
                  has(err, "UNIT"), "E7 the unit is never defaulted");
            bool mm = !applyFrameAxis(sid, "temp", "C", "10 20 30", err);
            fprintf(stderr, "texportselftest: E7 count mismatch: %s\n", err.c_str());
            check(mm && has(err, "3 value(s)") && has(err, "5 frame(s)"),
                  "E7 count mismatch refused with BOTH numbers");
            check(applyFrameAxis(sid, "temp", "C", "10, 20, 30, 40, 50", err),
                  "E7 a complete axis applies");
            FrameAxis ax = frameAxisOf(sid);
            check(ax.custom && ax.label == "temp (C)" && ax.vals &&
                  (*ax.vals)[4] == 50 && ax.at(2) == 30,
                  "E7 the chart's resolver serves the applied values");
            std::string tsvX = buildTemporalExport('\t');
            check(has(tsvX, "\ttemp [C]\t"), "E7 export per-frame header carries name + unit");
            check(has(tsvX, "\n0\t10\t") && has(tsvX, "\n4\t50\t"),
                  "E7 export rows carry the axis value for their frame");
            // count honesty at USE time: the resolver refuses a stale axis
            if (App::SeqInfo* si = seqInfo(sid)) si->axisVals = { 1.0, 2.0, 3.0 };
            FrameAxis ax2 = frameAxisOf(sid);
            check(!ax2.custom && has(ax2.why, "3 value(s)") && has(ax2.why, "5 frame(s)"),
                  "E7 a stale axis falls back to frame index and says why");
            if (App::SeqInfo* si = seqInfo(sid)) si->axisVals = { 10, 20, 30, 40, 50 };
        }

        {   // ---- E10: the box-zoom mapping is the exact inverse of the plot's
            PlotRect pr;
            pr.p0 = ImVec2(100, 50); pr.p1 = ImVec2(500, 250);
            pr.xmin = 0; pr.xmax = 23; pr.ymin = 990; pr.ymax = 1010;
            pr.ok = true;
            ImVec2 sc = pr.at(7.0f, 1003.0f);
            ImVec2 da = pr.unmap(sc);
            check(fabsf(da.x - 7.0f) < 1e-3f && fabsf(da.y - 1003.0f) < 1e-3f,
                  "E10 unmap is the exact inverse of at");
            // a drag rectangle spans exactly the range it drew, whichever
            // corner the drag started from
            float x0, x1, y0, y1;
            bool okz = zoomRangeFromDrag(pr, pr.at(5, 995), pr.at(15, 1005), x0, x1, y0, y1);
            bool okz2 = zoomRangeFromDrag(pr, pr.at(15, 1005), pr.at(5, 995), x0, x1, y0, y1);
            check(okz && okz2 && fabsf(x0 - 5) < 1e-3f && fabsf(x1 - 15) < 1e-3f &&
                  fabsf(y0 - 995) < 1e-3f && fabsf(y1 - 1005) < 1e-3f,
                  "E10 the drag rectangle becomes exactly its data range");
            // a jittered click is not an intention
            check(!zoomRangeFromDrag(pr, ImVec2(200, 100), ImVec2(202, 140), x0, x1, y0, y1) &&
                  !zoomRangeFromDrag(pr, ImVec2(200, 100), ImVec2(280, 102), x0, x1, y0, y1),
                  "E10 a click with jitter does not zoom into nothing");
        }

        // ---- E8: the axis survives a session round-trip ----
        {
            std::error_code ec;
            std::string sess = (std::filesystem::temp_directory_path(ec) /
                                "viewer_texport.vsession").u8string();
            saveSession(sess, true);
            std::string lerr = loadSession(sess);
            loadAll();
            double tn = nowSec();
            while (nowSec() - tn < 60.0 &&
                   (!app.seqRestore.empty() || app.seqRunning || seqReadyPending())) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            loadAll();
            bool found = false;
            for (const auto& si : app.seqs)
                if (si.axisName == "temp" && si.axisUnit == "C" &&
                    si.axisVals.size() == 5 && si.axisVals[4] == 50)
                    found = true;
            fprintf(stderr, "texportselftest: E8 reloaded %zu stack(s), load err '%s'\n",
                    app.seqs.size(), lerr.c_str());
            check(lerr.empty() && found, "E8 x axis survives a session round-trip");
            std::filesystem::remove(std::filesystem::u8path(sess), ec);
        }

        // ---- E9: compare on - every side rides along under its letter ----
        if (app.seqs.size() >= 2) {
            selectImage(framesOfSeq(app.seqs[0].id).front());
            std::vector<int> frB = framesOfSeq(app.seqs[1].id);
            app.compareRangeMode = 1;
            setCompareB(app.images[frB.front()].get());
            app.compareMode = App::CmpSplit;
            std::string tsv2 = buildTemporalExport('\t');
            check(has(tsv2, "# sides: A, B"), "E9 sides list carries B");
            size_t q1 = tsv2.find("== temporal summary");
            size_t q2 = tsv2.find("== H/V profile statistics");
            size_t q3 = tsv2.find("== per-frame statistics");
            check(q1 != npos && q2 != npos && tsv2.find("\nB\tR\t", q1) != npos &&
                  tsv2.find("\nB\tR\t", q1) < q2, "E9 summary: B's plane rows present");
            check(q2 != npos && q3 != npos && tsv2.find("\nB\tR\t", q2) != npos &&
                  tsv2.find("\nB\tR\t", q2) < q3, "E9 profile stats: B rows present");
            check(q3 != npos && tsv2.find("-- side B:", q3) != npos,
                  "E9 per-frame block for B present");
            char nb[48];
            snprintf(nb, sizeof nb, "%.9g", app.temporal[1].tempNoise[0]);
            check(has(tsv2, std::string("\nB\tR\t") + std::to_string(app.temporal[1].frames) +
                            "\t" + std::to_string(app.temporal[1].frames) + "\t" + nb + "\t"),
                  "E9 B's sigma_t is temporal[1]'s number, not A's relabelled");
            check(app.temporal[1].tempNoise[0] != app.temporal[0].tempNoise[0],
                  "E9 fixture: A and B really differ");
            app.compareMode = App::CmpOff;
        } else check(false, "E9 needs 2 stacks under the fixture dir");

        fprintf(stderr, "texportselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
        return ok ? 0 : 1;
    }
