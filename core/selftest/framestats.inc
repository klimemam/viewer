// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // The per-frame table, verifiable without a human: load the stack given on
    // the command line, run the exact clipboard path, print the TSV to stdout.
    // An independent numpy implementation must reproduce every number.
    //
    // ...and, since 板 304, it does not WAIT for that implementation. The
    // inventory (docs/background/analysis/measurement-kernel-inventory.md §4.2)
    // counted this file at 21 lines and zero assertions, and named the accident
    // it leaves open: stats-taxonomy.md §6.6 flipped two ddof=1 formulas to
    // ddof=0 and all 42 tests passed, because the only verification of this
    // table was a numpy script nobody in CI runs. So the stdout above is kept
    // unchanged - it is the material an outside reader reproduces - and a SECOND
    // stack follows it whose every number is a rational derived by hand.
    //
    // Everything below writes to STDERR only. stdout stays byte-identical for a
    // given fixture, because that is the half an outside implementation diffs.
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

        // ================= the synthetic stack, and its numbers ===========
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "fstatselftest: %-66s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        // The cells are %.9g, so nine significant digits is everything there is.
        // Compared RELATIVE to the expectation, at 1e-8: half of the ninth digit
        // is 5e-9 of relative error when the leading digit is a 1 (1.16365308
        // below is exactly that case), so a tighter bound would be asserting the
        // export's rounding rather than its arithmetic. This still pins all nine.
        auto near9 = [](double a, double b) {
            return std::isfinite(a) && std::fabs(a - b) <= 1e-8 * std::fabs(b);
        };
        // FIVE frames of 8x8, ch 1, non-mosaic. v_k(x,y) = 100*(k+1) + x + 2y
        // for k = 0..2, the same pixels again with ONE NaN for k = 3, and a
        // frame with no pixels resident for k = 4.
        //   mean_k    = 100(k+1) + 7/2 + 7 = 100(k+1) + 21/2
        //               -> 110.5, 210.5, 310.5
        //   sigma     = ddof=0 over the region's own pixels, and a shift does
        //               not move it: Var_pop(x + 2y) = Var_pop(0..7)*(1 + 4)
        //               = (63/12)*5 = 105/4, so sigma = 5.12347538 for all three
        //   sigma_col = ddof=1 of the COLUMN means, which are 100(k+1) + 7 + x:
        //               a ramp of slope 1 over 8, Var_ddof1 = 8*9/12 = 6
        //   sigma_row = the ROW means are 100(k+1) + 7/2 + 2y: slope 2 over 8,
        //               so 4*6 = 24
        //   and the [%] columns divide by mean_k, which is what makes the three
        //   otherwise identical frames print three different percentages:
        //     k=0  2.2167328 / 4.4334656
        //     k=1  1.16365308 / 2.32730617
        //     k=2  0.788885585 / 1.57777117
        // The NaN frame (k=3, the pixel at x=7,y=0, whose value was 107) is the
        // per-pixel skip rule stated as five numbers: the pixel leaves the mean,
        // the sigma, its own column's mean and its own row's mean, and nothing
        // else moves.
        //   n = 63, mean = 995/9 = 110.555556, sigma^2 = 2144/81
        //   column 7's mean = (sum_y(107+2y) - 107)/7 = 115 (was 114)
        //   row 0's mean    = (sum_x(100+x) - 107)/7  = 103 (was 103.5)
        //   so Var_ddof1(columns) = 57/8 and Var_ddof1(rows) = 801/32
        // =================================================================
        const int saveAnn = app.selectedAnn;
        closeAll();
        app.anns.clear(); app.nextAnnId = 1; app.roiSeq = 0;
        app.selectedAnn = -1;                          // whole frame
        app.seqs.clear();
        {
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = "fstat-synth";
            app.seqs.push_back(si);
            const int sid = si.id;
            for (int k = 0; k < 5; k++) {
                auto d = std::make_unique<ImageDoc>();
                d->name = std::string("synth") + std::to_string(k) + ".npy";
                d->src->w = 8; d->src->h = 8; d->src->ch = 1;
                d->src->dtype = "f32";
                if (k < 4) {
                    d->src->data.assign(64, 0.0f);
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            d->src->data[(size_t)y * 8 + x] =
                                (float)(100 * (k < 3 ? k + 1 : 1) + x + 2 * y);
                    if (k == 3)
                        d->src->data[7] = std::numeric_limits<float>::quiet_NaN();
                }   // k == 4: no pixels at all - not resident
                d->syncMirrors();
                d->uid = app.nextUid++;
                d->seqId = sid;
                d->seqIndex = k;
                app.images.push_back(std::move(d));
            }
            app.imagesRev++;
            app.current = 0;

            copyPerFrameStats(sid);
            const char* c2 = ImGui::GetClipboardText();
            const std::string tsv = c2 ? c2 : "";
            // header + one row per RESIDENT frame, in frame order
            std::vector<std::vector<std::string>> line;
            for (size_t i = 0; i <= tsv.size();) {
                size_t e = tsv.find('\n', i);
                if (e == std::string::npos) e = tsv.size();
                if (e > i) {
                    std::vector<std::string> cell;
                    for (size_t a = i, b; a <= e; a = b + 1) {
                        b = tsv.find('\t', a);
                        if (b == std::string::npos || b > e) b = e;
                        cell.push_back(tsv.substr(a, b - a));
                        if (b == e) break;
                    }
                    line.push_back(cell);
                }
                if (e == tsv.size()) break;
                i = e + 1;
            }
            for (const auto& L : line) {
                std::string j;
                for (const auto& c : L) j += (j.empty() ? "" : " | ") + c;
                fprintf(stderr, "fstatselftest: synth %s\n", j.c_str());
            }
            const std::vector<std::string> want = {
                "frame", "file", "mean [DN]", "sigma [DN](ddof=0)",
                "sigma_col [%](ddof=1)", "sigma_row [%](ddof=1)"
            };
            check(line.size() == 5 && line[0] == want,
                  "F1 four resident rows, and the header names every ddof");
            if (line.size() == 5 && line[0].size() == 6) {
                auto num = [&](int row, int col) { return atof(line[row][col].c_str()); };
                const double SD = std::sqrt(105.0 / 4);           // 5.12347538
                const double SC = std::sqrt(6.0), SR = std::sqrt(24.0);
                bool means = true, sds = true, pcts = true, files = true;
                for (int k = 0; k < 3; k++) {
                    const double m = 100.0 * (k + 1) + 10.5;
                    means &= line[k + 1][0] == std::to_string(k) && num(k + 1, 2) == m;
                    sds &= near9(num(k + 1, 3), SD);
                    pcts &= near9(num(k + 1, 4), SC / m * 100.0) &&
                            near9(num(k + 1, 5), SR / m * 100.0);
                    files &= line[k + 1][1] == "synth" + std::to_string(k) + ".npy";
                }
                check(means, "F2 mean is 110.5 / 210.5 / 310.5 - exactly, to the printed digit");
                check(sds, "F2 sigma is sqrt(105/4) = 5.12347538 on all three (ddof=0)");
                check(pcts,
                      "F2 sigma_col/row [%] = 100*sqrt(6)/mean and 100*sqrt(24)/mean");
                check(files, "F2 ...and each row names the frame it describes");
                // The one number this table is NOT allowed to hold: ddof=1 of
                // the region's own pixels. sqrt(105/4 * 64/63) = 5.16405476,
                // which differs from the printed value in the THIRD digit - so
                // the assert above cannot pass under the other convention.
                check(!near9(num(1, 3), std::sqrt(105.0 / 4 * 64.0 / 63)),
                      "F3 ...and NOT the ddof=1 value 5.16405476 (anti-vacuity)");
                // the three frames differ only by 100 DN, so an implementation
                // that printed one frame's numbers for all of them would pass
                // F2's sigma and fail here
                check(num(1, 4) != num(2, 4) && num(2, 4) != num(3, 4),
                      "F3 ...and the three % columns are three numbers, not one");
                // ---- the NaN row: five numbers, one skipped pixel ----------
                const double MN = 995.0 / 9, VN = 2144.0 / 81;
                const double AN = 57.0 / 8, BN = 801.0 / 32;
                check(near9(num(4, 2), MN) && near9(num(4, 3), std::sqrt(VN)),
                      "F4 the NaN frame: mean 995/9 and sigma sqrt(2144/81) over 63 px");
                check(near9(num(4, 4), std::sqrt(AN) / MN * 100.0) &&
                      near9(num(4, 5), std::sqrt(BN) / MN * 100.0),
                      "F4 ...its column 7 and row 0 lost a sample: 57/8 and 801/32");
                // the same pixels WITH the value would be frame 0's row: the
                // skip is what makes these four numbers different, all four
                check(num(4, 2) != num(1, 2) && num(4, 3) != num(1, 3) &&
                      num(4, 4) != num(1, 4) && num(4, 5) != num(1, 5),
                      "F4 ...and all four differ from the same frame without the NaN");
            }
            // the frame with no pixels is not a row, and is not silent either
            check(tsv.find("synth4.npy") == std::string::npos,
                  "F5 a frame with no pixels resident gets no row");
            check(app.toast.find("4 frame row(s)") != std::string::npos &&
                  app.toast.find("1 not resident") != std::string::npos,
                  "F5 ...and the copy says so: '4 frame row(s) ... 1 not resident'");
        }
        closeAll();
        app.seqs.clear();
        app.selectedAnn = saveAnn;
        fprintf(stderr, "fstatselftest: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
