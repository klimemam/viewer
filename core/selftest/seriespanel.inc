// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    // ---- --seriespanel-selftest: the Series Analysis panel's CONTRACT --------
    // docs/analysis-layers.md §3.3 + 判断record 7, asserted on values and
    // strings (NOGL - nothing here is a rectangle):
    //   - series members are frame | stack MIXED; a stack member stands in as
    //     a frame through §3.2's fold, declared per member;
    //   - the fit takes MEAN statistics only - a sum-standing member REFUSES
    //     the fit out loud (sums over differing n are different quantities
    //     point to point), it is never coerced;
    //   - a frame member has no sigma_t (the canon: σ_t is a stack's
    //     attribute) - no sigma column, no PTC point, no SNR point, and the
    //     counts say so instead of hiding it;
    //   - the frame-linearity machinery lives HERE now (§3.3 migration, board
    //     row 104): fit window (none / auto 5..95% / manual) and method
    //     (OLS / rev4-style relative-weighted), settings in the SESSION;
    //   - results name their layer (invariant (a)): one shared provenance
    //     line for screen, export and this test.
    // The A group runs on synthetic in-memory docs with EXACT means (the
    // frame-lin selftest's fixture discipline, migrated with the analysis);
    // the B group writes real .npy files and round-trips the session.
    if (!g_seriesPanelSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "seriespanelselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto audit = [&]() {
            std::string why;
            if (seriesAudit(why)) return true;
            fprintf(stderr, "seriespanelselftest: AUDIT: %s\n", why.c_str());
            return false;
        };
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen && app.seqRestore.empty() &&
                    app.seriesRestore.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        const int W = 8, H = 8;
        const double A_GT = 8.0, B_GT = 64.0;      // the injected line: mean = 8v + 64
        const double SIG_GT = 3.0;                 // per-pixel time series m-3, m, m+3
        // ---- synthetic doc builders (batch first: containment is checked) ----
        auto mkBatch = [&](const char* name) {
            app.batches.push_back({ app.nextBatchId++, name, "" });
            return app.batches.back().id;
        };
        auto mkFrame = [&](int batchId, const char* name, bool cfa,
                           const std::function<float(int x, int y, int pl)>& px) {
            auto d = std::make_unique<ImageDoc>();
            d->name = name;
            d->src->w = W; d->src->h = H; d->src->ch = 1;
            d->src->dtype = "f32";
            if (cfa) { d->cfa = 1; d->cfaPattern = 0; }              // RGGB
            d->src->data.resize((size_t)W * H);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    int pl = cfa ? CFA_MAP[0][(y & 1) * 2 + (x & 1)] : 0;
                    d->px()[(size_t)y * W + x] = px(x, y, pl);
                }
            d->syncMirrors();
            d->uid = app.nextUid++;
            d->batchId = batchId;
            app.images.push_back(std::move(d));
            app.imagesRev++;
            return app.images.back()->uid;
        };
        auto mkStack = [&](int batchId, const char* name, bool cfa,
                           const std::function<float(int f, int x, int y, int pl)>& px) {
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = name;
            app.seqs.push_back(si);
            for (int f = 0; f < 3; f++) {
                char nm[96];
                snprintf(nm, sizeof nm, "%s_%d.npy", name, f);
                uint64_t u = mkFrame(batchId, nm, cfa, [&](int x, int y, int pl) {
                    return px(f, x, y, pl);
                });
                for (auto& d : app.images)
                    if (d->uid == u) { d->seqId = si.id; d->seqIndex = f; }
            }
            return si.id;
        };
        // one flat frame whose mean is exactly the line at value v
        auto lineFrame = [&](int b, const char* nm, double v) {
            return mkFrame(b, nm, false, [&](int, int, int) {
                return (float)(A_GT * v + B_GT);
            });
        };
        // one stack whose per-pixel time series is m-3, m, m+3: mean exactly
        // the line at v, sigma_t exactly 3
        auto lineStack = [&](int b, const char* nm, double v) {
            return mkStack(b, nm, false, [&](int f, int, int, int) {
                return (float)(A_GT * v + B_GT + SIG_GT * (f - 1));
            });
        };
        auto memberOfSeq = [&](int sid, int seqId) -> App::Series::Member* {
            App::Series* S = seriesById(sid);
            if (S)
                for (auto& m : S->members)
                    if (m.seqId == seqId) return &m;
            return nullptr;
        };
        auto rowOfName = [&](const std::string& nm) -> const App::LinState::Row* {
            for (const auto& r : app.lin.rows)
                if (r.name == nm) return &r;
            return nullptr;
        };

        // ==== A1: the model - frames join, containment holds, at most one ====
        closeAll();
        {
            int b1 = mkBatch("spbatch"), b2 = mkBatch("spother");
            int s10 = lineStack(b1, "st010", 10);
            uint64_t f40 = lineFrame(b1, "fr040.npy", 40);
            uint64_t fx = lineFrame(b2, "elsewhere.npy", 99);
            int sid = newSeries(b1, "mixed");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            S->paramName = "level";
            check(addToSeries(sid, s10, 10), "A1 a stack joins");
            check(addFrameToSeries(sid, f40, 40), "A1 a standalone frame joins as a member");
            check(!addFrameToSeries(sid, fx, 99),
                  "A1 same-batch invariant: a frame from another batch is refused");
            check(seriesOfFrame(f40) == seriesById(sid),
                  "A1 seriesOfFrame finds the membership");
            check(seriesFitPoints(*S) == 2, "A1 fit points count frame members too");
            check(audit(), "A1 audit passes on a mixed series");
            int sid2 = newSeries(b1, "second");
            bool moved = addFrameToSeries(sid2, f40, 41);
            bool inNew = seriesById(sid2) != nullptr &&
                         seriesOfFrame(f40) == seriesById(sid2);
            bool outOld = true;          // the old series keeps its STACK member
            if (App::Series* SM = seriesById(sid))
                for (const auto& m : SM->members)
                    if (m.frameUid == f40) outOld = false;
            check(moved && inNew && outOld,
                  "A1 a frame belongs to AT MOST one series (joining moves it)");
            check(audit(), "A1 audit after the move");
        }

        // ==== A1b: mixed Move / Close act on MEMBERS, never just seqIds =====
        // The loose control is created FIRST on purpose. The old Files lookup
        // asked for seqId 0 for every frame member, so it resolved every one of
        // them to this first loose row, repeated it inside the series, and then
        // drew the real member (and the control) again below the series.
        closeAll();
        {
            int b1 = mkBatch("actions source"), b2 = mkBatch("actions target");
            uint64_t control = lineFrame(b1, "control.npy", 1);
            int stack = lineStack(b1, "action-stack", 10);
            uint64_t frame = lineFrame(b1, "action-frame.npy", 20);
            int sid = newSeries(b1, "mixed actions");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            S->paramName = "level";
            check(addFrameToSeries(sid, frame, 20),
                  "A1b fixture: standalone frame joins first");
            check(addToSeries(sid, stack, 10), "A1b fixture: stack joins second");
            S = seriesById(sid);
            if (S && S->members.size() == 2) {
                S->members[0].include = false;       // metadata Move must not rewrite
                S->members[1].fold = FOLD_SUM;
            }

            // Four bindings distinguish preservation from dangling ids: three
            // targets this Close removes, and one unrelated control it must not.
            App::ASet refs;
            refs.id = app.nextASetId++;
            refs.batchId = b1;
            refs.name = "action refs";
            App::ASetRole rr;
            rr.role = "series"; rr.seriesId = sid; refs.roles.push_back(rr);
            rr = App::ASetRole{};
            rr.role = "stack"; rr.seqId = stack; refs.roles.push_back(rr);
            rr = App::ASetRole{};
            rr.role = "frame"; rr.frameUid = frame; refs.roles.push_back(rr);
            rr = App::ASetRole{};
            rr.role = "control"; rr.frameUid = control; refs.roles.push_back(rr);
            const int refsId = refs.id;
            app.analysisSets.push_back(std::move(refs));
            app.imagesRev++;

            // The pure identity lookup used by the actual Files walk: the frame
            // row must be action-frame, not the earlier control with seqId 0.
            {
                const auto& rows = stacksCached();
                std::vector<FileGroup> groups = buildFileGroups(rows);
                const FileGroup* g = nullptr;
                for (const auto& x : groups) if (x.batch == b1) g = &x;
                const std::vector<int>* rf =
                    g && S ? filesRowOfSeriesMember(*g, S->members[0]) : nullptr;
                const std::vector<int>* rs =
                    g && S ? filesRowOfSeriesMember(*g, S->members[1]) : nullptr;
                check(rf && rs && app.images[rf->front()]->uid == frame &&
                      app.images[rs->front()]->seqId == stack,
                      "A1b Files resolves frame/stack members by their own identity");
                check(S && filesSeriesMemberLead(*S, S->members[0], 0,
                                                  "action-frame.npy") ==
                               "[0]  20 lx  action-frame.npy" &&
                      filesSeriesMemberLead(*S, S->members[1], 1,
                                            "action-stack") ==
                               "[1]  10 lx  action-stack",
                      "A1b Files gives both physical layers one member vocabulary");
                const int seriesJump = S ? filesImageIndexOfSeries(*S) : -1;
                check(seriesJump >= 0 && app.images[seriesJump]->uid == frame,
                      "A1b series-role navigation selects its first frame member");
                ImageDoc* memberDoc = frameMemberDoc(frame);
                ImageDoc* controlDoc = frameMemberDoc(control);
                check(memberDoc && controlDoc && filesRowBelongsToSeries(*memberDoc) &&
                      !filesRowBelongsToSeries(*controlDoc),
                      "A1b Files suppresses only the member's duplicate loose row");
            }

            const std::vector<App::Series::Member> before = S ? S->members
                                                               : std::vector<App::Series::Member>{};
            moveSeriesToBatch(sid, b2);
            S = seriesById(sid);
            bool sameMembers = S && S->members.size() == before.size();
            for (size_t i = 0; sameMembers && i < before.size(); i++) {
                const auto& a = before[i];
                const auto& z = S->members[i];
                sameMembers = a.seqId == z.seqId && a.frameUid == z.frameUid &&
                              a.value == z.value && a.include == z.include &&
                              a.fold == z.fold;
            }
            bool allMoved = S && S->batchId == b2;
            if (ImageDoc* d = frameMemberDoc(frame)) allMoved &= d->batchId == b2;
            else allMoved = false;
            std::vector<int> movedStackFrames = framesOfSeq(stack);
            allMoved &= seqInfo(stack) && movedStackFrames.size() == 3;
            for (int i : movedStackFrames) allMoved &= app.images[i]->batchId == b2;
            ImageDoc* controlDoc = frameMemberDoc(control);
            check(allMoved, "A1b Move sends stack + standalone frame to one target batch");
            check(sameMembers, "A1b Move preserves member order, identity, value/include/fold");
            check(controlDoc && controlDoc->batchId == b1,
                  "A1b Move leaves the unrelated control in the source batch");
            App::ASet* R = aSetById(refsId);
            check(R && R->roles.size() == 4 && R->roles[0].seriesId == sid &&
                  R->roles[1].seqId == stack && R->roles[2].frameUid == frame &&
                  R->roles[3].frameUid == control,
                  "A1b Move preserves series/member/control role references");
            check(audit(), "A1b audit after mixed Move");

            closeSeries(sid);
            bool stackGone = !seqInfo(stack);
            for (const auto& d : app.images) if (d->seqId == stack) stackGone = false;
            controlDoc = frameMemberDoc(control);
            check(!seriesById(sid) && !frameMemberDoc(frame) && stackGone,
                  "A1b Close removes every stack/frame member and the series");
            check(controlDoc && controlDoc->batchId == b1 && app.images.size() == 1,
                  "A1b Close leaves the unrelated control open");
            R = aSetById(refsId);
            check(R && R->roles.size() == 4 && !R->roles[0].bound() &&
                  R->roles[0].reason.find("series") != std::string::npos &&
                  !R->roles[1].bound() &&
                  R->roles[1].reason.find("stack") != std::string::npos &&
                  !R->roles[2].bound() &&
                  R->roles[2].reason.find("frame") != std::string::npos &&
                  R->roles[3].frameUid == control && R->roles[3].bound(),
                  "A1b Close unbinds removed roles and keeps the control binding");
            check(app.toast == "closed series \"mixed actions\": 2 member(s)",
                  "A1b Close toast counts mixed members in member vocabulary");
            check(audit(), "A1b audit after mixed Close");

            // Ungroup is the other series action whose old toast/UI claimed all
            // members were stacks. It keeps both physical layers intact.
            int us = lineStack(b1, "ungroup-stack", 30);
            uint64_t uf = lineFrame(b1, "ungroup-frame.npy", 40);
            int uid = newSeries(b1, "mixed ungroup");
            addToSeries(uid, us, 30);
            addFrameToSeries(uid, uf, 40);
            const size_t openBefore = app.images.size();
            ungroupSeries(uid);
            check(!seriesById(uid) && seqInfo(us) && frameMemberDoc(uf) &&
                  app.images.size() == openBefore,
                  "A1b Ungroup keeps every mixed member open");
            check(app.toast == "ungrouped \"mixed ungroup\": 2 member(s) stay open",
                  "A1b Ungroup toast uses member vocabulary");
            check(audit(), "A1b audit after mixed Ungroup");
        }

        // ==== A2/A3: mixed measurement + fit over mean statistics =============
        closeAll();
        int bA = mkBatch("spbatch");
        int st10 = lineStack(bA, "st010", 10);
        int st20 = lineStack(bA, "st020", 20);
        uint64_t fr40 = lineFrame(bA, "fr040.npy", 40);
        uint64_t fr80 = lineFrame(bA, "fr080.npy", 80);
        int sidA = newSeries(bA, "sweep A");
        {
            App::Series* S = seriesById(sidA);
            snprintf(S->unit, sizeof S->unit, "lx");
            S->paramName = "level";
            addToSeries(sidA, st10, 10);
            addToSeries(sidA, st20, 20);
            addFrameToSeries(sidA, fr40, 40);
            addFrameToSeries(sidA, fr80, 80);
        }
        app.lin = App::LinState{};                 // defaults: no window, OLS
        linRecompute(sidA);
        {
            const App::LinState& L = app.lin;
            const App::LinState::Row* rs = rowOfName("st010");
            const App::LinState::Row* rf = rowOfName("fr040.npy");
            check(L.rows.size() == 4 && rs && rf, "A2 one row per member, frames included");
            check(rs && rs->valid && rs->hasSigma && rs->frames == 3 &&
                  fabs(rs->mean[0] - (A_GT * 10 + B_GT)) < 1e-6 &&
                  fabs(rs->sigmaT[0] - SIG_GT) < 1e-6,
                  "A2 stack row: mean statistics, sigma_t = 3 exactly");
            check(rf && rf->valid && rf->isFrame && !rf->hasSigma && rf->frames == 1 &&
                  fabs(rf->mean[0] - (A_GT * 40 + B_GT)) < 1e-6,
                  "A2 frame row: spatial mean, frames=1, NO sigma_t");
            check(L.fitValid && fabs(L.slope[0] - A_GT) < 1e-9 &&
                  fabs(L.offs[0] - B_GT) < 1e-6 && L.r2[0] > 0.999999999,
                  "A3 the fit recovers a=8, b=64 over mixed members");
            check(L.nFit[0] == 4, "A3 nFit says 4 points on the one plane");
            // ---- A13: results name their layer (invariant (a)) --------------
            std::string prov = seriesProvLine(*seriesById(sidA), L);
            fprintf(stderr, "seriespanelselftest: prov: %s\n", prov.c_str());
            check(prov.find("layer: series") != std::string::npos &&
                  prov.find("sweep A") != std::string::npos &&
                  prov.find("2 stack") != std::string::npos &&
                  prov.find("2 frame") != std::string::npos,
                  "A13 the provenance line names the layer and the mix");
        }

        // ==== A4: a sum-standing member refuses the fit, out loud =============
        {
            App::Series::Member* m = memberOfSeq(sidA, st10);
            check(m != nullptr, "A4 fixture: the st010 member exists");
            if (m) m->fold = FOLD_SUM;
            linRecompute(sidA);
            const App::LinState& L = app.lin;
            fprintf(stderr, "seriespanelselftest: refusal: %s\n", L.refusal.c_str());
            check(!L.fitValid && !L.refusal.empty(),
                  "A4 a sum-standing member: the fit does NOT run");
            check(L.refusal.find("st010") != std::string::npos &&
                  L.refusal.find("sum") != std::string::npos,
                  "A4 the refusal names the member and the fold");
            const App::LinState::Row* rs = rowOfName("st010");
            check(rs && rs->valid && rs->fold == FOLD_SUM,
                  "A4 the member is still MEASURED (mean statistics) and tagged");
            // excluded sum member: no refusal - it is not entering the fit
            if (m) { m->fold = FOLD_SUM; m->include = false; }
            linRecompute(sidA);
            check(app.lin.refusal.empty() && app.lin.fitValid,
                  "A4 an EXCLUDED sum member does not refuse");
            if (m) { m->include = true; m->fold = FOLD_MEAN; }
            // a frame member's fold is meaningless and must not refuse
            if (App::Series* S = seriesById(sidA))
                for (auto& mm : S->members)
                    if (mm.frameUid == fr40) mm.fold = FOLD_SUM;
            linRecompute(sidA);
            check(app.lin.refusal.empty() && app.lin.fitValid,
                  "A4 a frame member ignores the fold (it IS a frame)");
            if (App::Series* S = seriesById(sidA))
                for (auto& mm : S->members)
                    if (mm.frameUid == fr40) mm.fold = FOLD_MEAN;
        }

        // ==== A5: manual window excludes off-line members (migrated F1) ======
        {
            uint64_t lo = mkFrame(bA, "off_lo.npy", false,
                                  [&](int, int, int) { return 100.0f; });   // line: 104
            uint64_t hi = mkFrame(bA, "off_hi.npy", false,
                                  [&](int, int, int) { return 1500.0f; });  // line: 1264
            addFrameToSeries(sidA, lo, 5);
            addFrameToSeries(sidA, hi, 150);
            app.lin.winMode = 0;
            app.lin.fitMethod = 0;
            linRecompute(sidA);
            check(app.lin.fitValid && fabs(app.lin.slope[0] - A_GT) > 0.05,
                  "A5 fixture power: without the window the slope bends");
            app.lin.winMode = 2;
            app.lin.winLo = 110; app.lin.winHi = 1400;
            linRecompute(sidA);
            const App::LinState& L = app.lin;
            check(L.fitValid && L.nFit[0] == 4 &&
                  fabs(L.slope[0] - A_GT) < 1e-9 && fabs(L.offs[0] - B_GT) < 1e-6,
                  "A5 manual window keeps the 4 on-line points, recovers the line");
            check(fabs(L.winPlaneLo[0] - 110) < 1e-12 && fabs(L.winPlaneHi[0] - 1400) < 1e-12,
                  "A5 the APPLIED window is recorded per plane");
            // the shared label: screen and TSV read the same string
            check(seriesWinLabel(L).find("manual [110, 1400] DN") != std::string::npos,
                  "A5 the shared window label states the applied window");
            removeFrameFromSeries(lo);
            removeFrameFromSeries(hi);
        }

        // ==== A6: auto window = 5..95% of the plane's max member mean =========
        {
            uint64_t f120 = lineFrame(bA, "fr120.npy", 120);   // mean 1024
            addFrameToSeries(sidA, f120, 120);
            app.lin.winMode = 1;
            linRecompute(sidA);
            const App::LinState& L = app.lin;
            fprintf(stderr, "seriespanelselftest: A6 window [%.6g, %.6g] nFit=%d\n",
                    L.winPlaneLo[0], L.winPlaneHi[0], L.nFit[0]);
            check(fabs(L.winPlaneLo[0] - 51.2) < 1e-9 && fabs(L.winPlaneHi[0] - 972.8) < 1e-9,
                  "A6 auto window = [51.2, 972.8] from max member mean 1024");
            check(L.fitValid && L.nFit[0] == 4 && fabs(L.slope[0] - A_GT) < 1e-9,
                  "A6 the 1024 DN member is out, the rest recover the line");
            check(seriesWinLabel(L).find("auto = 5%..95%") != std::string::npos,
                  "A6 the shared window label says AUTO");
            removeFrameFromSeries(f120);
            app.lin.winMode = 0;
        }

        // ==== A7: the two methods disagree in the PREDICTED direction ========
        // The interim tool's F4 fixture, member-shaped: 9 levels at LINEAR
        // spacing (the geometry matters - on a sparse geometric axis OLS's
        // worst RELATIVE deviation walks down to the small-denominator end),
        // the TOP one bent +2%. OLS lets the bright point pull the whole fit;
        // the relative-weighted rev4-style fit stays with the low points, so
        // a bright-end bend keeps MORE of its deviation: LE rev4 > LE OLS
        // (verified analytically: OLS 1.235%, rev4 1.556%, both at the bend).
        {
            int bM = mkBatch("spmeth");
            int sidM = newSeries(bM, "methods");
            App::Series* SM = seriesById(sidM);
            snprintf(SM->unit, sizeof SM->unit, "lx");
            for (int v = 20; v <= 90; v += 10) {
                char nm[32];
                snprintf(nm, sizeof nm, "m%03d.npy", v);
                addFrameToSeries(sidM, lineFrame(bM, nm, v), v);
            }
            // on LINEAR data the two methods must agree - the weights only
            // matter when there is a residual to weigh
            app.lin.winMode = 0;
            app.lin.fitMethod = 0;
            linRecompute(sidM);
            double aLin = app.lin.slope[0], bLin = app.lin.offs[0];
            app.lin.fitMethod = 1;
            linRecompute(sidM);
            check(app.lin.fitValid && fabs(aLin - A_GT) < 1e-9 &&
                  fabs(app.lin.slope[0] - aLin) < 1e-6 &&
                  fabs(app.lin.offs[0] - bLin) < 1e-4,
                  "A7 the two methods agree on linear data");
            uint64_t bent = mkFrame(bM, "bent100.npy", false, [&](int, int, int) {
                return (float)((A_GT * 100 + B_GT) * 1.02);
            });
            addFrameToSeries(sidM, bent, 100);
            app.lin.winMode = 0;
            app.lin.fitMethod = 0;
            linRecompute(sidM);
            double leOls = app.lin.leMax[0];
            bool okOls = app.lin.fitValid;
            app.lin.fitMethod = 1;
            linRecompute(sidM);
            double leRev = app.lin.leMax[0];
            fprintf(stderr, "seriespanelselftest: A7 LE OLS=%.4f%% rev4=%.4f%%\n",
                    leOls, leRev);
            check(okOls && app.lin.fitValid && leOls > 0.8 && leOls < 2.2 &&
                  leRev > 0.8 && leRev < 2.2,
                  "A7 both methods catch the 2% bend in its ballpark");
            check(leRev > leOls, "A7 rev4-style shows the LARGER LE for a bright-end bend");
            app.lin.fitMethod = 0;
        }

        // ==== A8: a WINDOWED fit needs 3 points; unwindowed keeps the old 2 ==
        {
            app.lin.winMode = 2;
            app.lin.winLo = 200; app.lin.winHi = 400;    // keeps 224 and 384 only
            linRecompute(sidA);
            check(!app.lin.fitValid && app.lin.nFit[0] == 2,
                  "A8 2 in-window points: refused (2 points fit any line)");
            app.lin.winMode = 0;
            int b2 = mkBatch("sptwo");
            uint64_t g1 = lineFrame(b2, "g10.npy", 10), g2 = lineFrame(b2, "g20.npy", 20);
            int sid2 = newSeries(b2, "two");
            App::Series* S2 = seriesById(sid2);
            snprintf(S2->unit, sizeof S2->unit, "lx");
            addFrameToSeries(sid2, g1, 10);
            addFrameToSeries(sid2, g2, 20);
            linRecompute(sid2);
            check(app.lin.fitValid && fabs(app.lin.slope[0] - A_GT) < 1e-9,
                  "A8 no window: 2 points still fit (the panel's historic rule)");
        }

        // ==== A9: sigma_t honesty - SNR points, dark, read noise ==============
        // A dark FRAME member measures the black LEVEL (a mean statistic), so
        // the SNR curve subtracts a measured dark - but read noise needs
        // sigma_t, which a frame does not have: it stays an extrapolation and
        // the two flags legitimately diverge.
        closeAll();
        {
            int b = mkBatch("spdark");
            uint64_t dk = lineFrame(b, "dark0.npy", 0);              // mean 64
            int s1 = lineStack(b, "st010", 10);
            int s2 = lineStack(b, "st020", 20);
            uint64_t lf = lineFrame(b, "fr040.npy", 40);
            int sid = newSeries(b, "dark sweep");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            addFrameToSeries(sid, dk, 0);
            addToSeries(sid, s1, 10);
            addToSeries(sid, s2, 20);
            addFrameToSeries(sid, lf, 40);
            app.lin = App::LinState{};
            linRecompute(sid);
            check(app.lin.fitValid && !app.lin.readFromDark,
                  "A9 a dark FRAME cannot measure read noise (no sigma_t)");
            std::vector<LinSnrPt> sp;
            bool fromDark = false;
            int dropped = 0, noSigma = 0;
            linSnrPoints(app.lin, sp, &fromDark, &dropped, &noSigma);
            fprintf(stderr, "seriespanelselftest: A9 snr pts=%d fromDark=%d noSigma=%d\n",
                    (int)sp.size(), fromDark ? 1 : 0, noSigma);
            check(fromDark, "A9 ...but its MEAN is a measured dark for the SNR curve");
            check(sp.size() == 2 && noSigma == 1,
                  "A9 SNR: stack points only; the lit frame member is counted out");
            bool vOk = sp.size() == 2;
            for (const auto& q : sp)
                vOk &= fabs(q.snr - q.x / SIG_GT) < 1e-6 &&
                       (fabs(q.x - 80.0) < 1e-6 || fabs(q.x - 160.0) < 1e-6);
            check(vOk, "A9 SNR points: signal = mean - measured dark, exactly");
        }

        // ==== A10: CFA planes stay separate, windows per plane ================
        {
            closeAll();
            static const double SENS[4] = { 8.0, 10.0, 10.0, 6.0 };
            int b = mkBatch("spcfa");
            auto cfaStack = [&](const char* nm, double v) {
                return mkStack(b, nm, true, [&](int f, int, int, int pl) {
                    return (float)(SENS[pl] * v + B_GT + SIG_GT * (f - 1));
                });
            };
            auto cfaFrame = [&](const char* nm, double v) {
                return mkFrame(b, nm, true, [&](int, int, int pl) {
                    return (float)(SENS[pl] * v + B_GT);
                });
            };
            int c1 = cfaStack("c010", 10), c2 = cfaStack("c020", 20);
            uint64_t c4 = cfaFrame("c040.npy", 40), c8 = cfaFrame("c080.npy", 80);
            int sid = newSeries(b, "cfa sweep");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            addToSeries(sid, c1, 10);
            addToSeries(sid, c2, 20);
            addFrameToSeries(sid, c4, 40);
            addFrameToSeries(sid, c8, 80);
            app.lin = App::LinState{};
            app.lin.winMode = 1;
            linRecompute(sid);
            const App::LinState& L = app.lin;
            bool aOk = L.fitValid && L.nPl == 4, wOk = aOk;
            for (int p = 0; p < 4 && aOk; p++) {
                fprintf(stderr, "seriespanelselftest: A10 plane %-2s a=%.9g (truth %g) "
                                "window hi %.6g nFit=%d\n", LIN_PLANES[p], L.slope[p],
                        SENS[p], L.winPlaneHi[p], L.nFit[p]);
                aOk &= fabs(L.slope[p] - SENS[p]) < 1e-6 && L.nFit[p] == 3;
                wOk &= fabs(L.winPlaneHi[p] - 0.95 * (SENS[p] * 80 + B_GT)) < 1e-9;
            }
            check(aOk, "A10 every plane recovers ITS slope over mixed members");
            check(wOk, "A10 every plane gets ITS OWN auto window");
        }

        // ==== A11: NaN in a frame member: excluded per sample AND counted ====
        {
            closeAll();
            int b = mkBatch("spnan");
            const float NA = std::numeric_limits<float>::quiet_NaN();
            uint64_t f1 = mkFrame(b, "half.npy", false, [&](int x, int, int) {
                return x < W / 2 ? NA : (float)(A_GT * 40 + B_GT);
            });
            uint64_t f2 = lineFrame(b, "full.npy", 10);
            int sid = newSeries(b, "nan sweep");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            addFrameToSeries(sid, f1, 40);
            addFrameToSeries(sid, f2, 10);
            app.lin = App::LinState{};
            linRecompute(sid);
            const App::LinState::Row* r = rowOfName("half.npy");
            check(r && r->valid && fabs(r->mean[0] - (A_GT * 40 + B_GT)) < 1e-6,
                  "A11 the finite half still measures the exact mean");
            check(r && r->nonFinite == (uint64_t)(W / 2) * H,
                  "A11 32 NaN samples counted as excluded on the row");
        }

        // ==== A12: the ROI convention (the region rule every panel uses) =====
        {
            closeAll();
            int b = mkBatch("sproi");
            uint64_t f1 = mkFrame(b, "roi.npy", false, [&](int x, int, int) {
                double v = A_GT * 40 + B_GT;
                return (float)(x < W / 2 ? v : v + 1000.0);
            });
            uint64_t f2 = lineFrame(b, "flat.npy", 10);
            int sid = newSeries(b, "roi sweep");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            addFrameToSeries(sid, f1, 40);
            addFrameToSeries(sid, f2, 10);
            App::Ann a;
            a.id = app.nextAnnId++;
            a.type = 0;
            a.x = 0; a.y = 0; a.w = W / 2; a.h = H;
            a.label = "roi";
            app.anns.push_back(a);
            app.selectedAnn = a.id;
            app.annRev++;
            app.lin = App::LinState{};
            linRecompute(sid);
            const App::LinState::Row* rr = rowOfName("roi.npy");
            check(app.lin.roiUsed && rr && rr->valid &&
                  fabs(rr->mean[0] - (A_GT * 40 + B_GT)) < 1e-6,
                  "A12 selected ROI: the frame member's mean comes from the ROI");
            app.anns.clear(); app.selectedAnn = 0; app.annRev++;
            linRecompute(sid);
            rr = rowOfName("roi.npy");
            check(!app.lin.roiUsed && rr && rr->valid &&
                  fabs(rr->mean[0] - (A_GT * 40 + B_GT + 500.0)) < 1e-6,
                  "A12 no ROI: whole frame (the +1000 half shifts the mean by 500)");
        }

        // ==== A14: the modal lists frames, and folds survive Save ============
        {
            closeAll();
            int b = mkBatch("spmodal");
            int s1 = lineStack(b, "st010", 10);
            uint64_t f1 = lineFrame(b, "fr040.npy", 40);
            openSeriesModal(b, 0);
            int frameRows = 0, stackRows = 0;
            int frameRowIdx = -1;
            for (int i = 0; i < (int)app.seriesEdit.rows.size(); i++) {
                const auto& r = app.seriesEdit.rows[i];
                if (r.uid != 0) { frameRows++; frameRowIdx = i; }
                else if (r.seqId != 0) stackRows++;
            }
            check(frameRows == 1 && stackRows == 1,
                  "A14 the create modal lists the batch's standalone frames");
            if (frameRowIdx >= 0) {
                auto& r = app.seriesEdit.rows[frameRowIdx];
                snprintf(r.value, sizeof r.value, "40");
                r.check = true;
                r.touched = true;
            }
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "lx");
            int sid = seriesModalAccept();
            App::Series* S = seriesById(sid);
            bool haveFrame = false;
            if (S)
                for (const auto& m : S->members)
                    if (m.frameUid == f1 && fabs(m.value - 40) < 1e-12) haveFrame = true;
            check(S != nullptr && haveFrame,
                  "A14 Save commits the frame member with its typed value");
            check(audit(), "A14 audit after the modal round trip");
            // fold survives an untouched Edit -> Save (the include precedent)
            if (S)
                for (auto& m : S->members)
                    if (m.seqId == s1) m.fold = FOLD_SUM;
            openSeriesModal(b, sid);
            seriesModalAccept();
            S = seriesById(sid);
            bool foldKept = false;
            if (S)
                for (const auto& m : S->members)
                    if (m.seqId == s1 && m.fold == FOLD_SUM) foldKept = true;
            check(foldKept, "A14 an untouched Save keeps the declared fold");
        }

        // ==== B: real files - the batch doors and the session =================
        // f4 .npy writer (the stackavg selftest's fixture discipline: write
        // exactly the numbers CI asserts on, under a scratch dir we create).
        auto writeNpy = [](const std::string& path, int w, int h,
                           const std::vector<float>& px) {
            char dict[128];
            snprintf(dict, sizeof dict,
                     "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d), }", h, w);
            std::string hdr = dict;
            size_t pad = (64 - (10 + hdr.size() + 1) % 64) % 64;
            hdr.append(pad, ' ');
            hdr += '\n';
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            uint16_t hl = (uint16_t)hdr.size();
            f.write("\x93NUMPY\x01\x00", 8);
            f.write((const char*)&hl, 2);
            f.write(hdr.data(), (std::streamsize)hdr.size());
            f.write((const char*)px.data(), (std::streamsize)(px.size() * sizeof(float)));
        };
        auto flat = [&](float v) { return std::vector<float>((size_t)W * H, v); };
        std::string root = g_seriesPanelSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/seriespanel-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        const std::string dSweeps = root + "/sweeps", dLoose = root + "/loose";
        const double BV[3] = { 10, 20, 40 };               // the stacks' values
        for (int s = 0; s < 3; s++) {
            char dir[32];
            snprintf(dir, sizeof dir, "/s%03d", (int)BV[s]);
            std::string d = dSweeps + dir;
            std::filesystem::create_directories(pathFromUtf8(d), ec);
            for (int f = 0; f < 3; f++) {
                char nm[32];
                snprintf(nm, sizeof nm, "/f_%03d.npy", f);
                writeNpy(d + nm, W, H, flat((float)(A_GT * BV[s] + B_GT + SIG_GT * (f - 1))));
            }
        }
        std::filesystem::create_directories(pathFromUtf8(dLoose), ec);
        writeNpy(dLoose + "/bright.npy", W, H, flat((float)(A_GT * 80 + B_GT)));   // 704
        writeNpy(dLoose + "/dim.npy", W, H, flat((float)(A_GT * 5 + B_GT)));       // 104
        closeAll();
        app.seqLoadMode = 1;                       // numbered runs load silently
        openFolder(dSweeps);
        loadAll();
        int bSweep = app.images.empty() ? 0 : app.images[0]->batchId;
        app.seqLoadMode = 2;                       // loose singles STAY single
        openPath(dLoose + "/bright.npy");
        openPath(dLoose + "/dim.npy");
        loadAll();
        uint64_t uBright = 0, uDim = 0;
        for (const auto& d : app.images) {
            if (d->seqId != 0) continue;
            if (d->name.find("bright") != std::string::npos) uBright = d->uid;
            if (d->name.find("dim") != std::string::npos) uDim = d->uid;
        }
        check(app.seqs.size() == 3 && uBright && uDim && bSweep,
              "B1 fixture: 3 stacks + 2 loose frames are open");
        {
            // the same-batch invariant refuses ACROSS batches, then Move fixes it
            int sid = newSeries(bSweep, "file sweep");
            App::Series* S = seriesById(sid);
            snprintf(S->unit, sizeof S->unit, "lx");
            S->paramName = "level";
            check(!addFrameToSeries(sid, uBright, 80),
                  "B1 a loose frame in ITS OWN batch is refused (move first)");
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->uid == uBright || app.images[i]->uid == uDim)
                    moveImageToBatch(i, bSweep);
            check(addFrameToSeries(sid, uBright, 80) && addFrameToSeries(sid, uDim, 5),
                  "B1 after Move to batch, both frames join");
            for (const auto& si : app.seqs)
                addToSeries(sid, si.id, extractLevelFromName(si.name));
            S = seriesById(sid);
            check(S->members.size() == 5 && audit(), "B1 5 members, audit passes");
            // one stack declared sum - it must survive the session below
            bool declared = false;
            for (auto& m : S->members)
                if (m.seqId != 0 && !declared) { m.fold = FOLD_SUM; declared = true; }
            linRecompute(sid);
            check(!app.lin.fitValid && !app.lin.refusal.empty(),
                  "B2 the sum declaration refuses the fit through the real doors");
            for (auto& m : S->members) m.fold = FOLD_MEAN;
            linRecompute(sid);
            check(app.lin.fitValid && fabs(app.lin.slope[0] - A_GT) < 1e-6 &&
                  fabs(app.lin.offs[0] - B_GT) < 1e-4,
                  "B2 mean-standing members fit the line over files");
            for (auto& m : S->members)
                if (m.seqId != 0) { m.fold = FOLD_SUM; break; }   // re-declare for B3
        }
        // ---- B3: the session round-trips members, folds and panel settings --
        app.lin.tablePlane = 0;
        app.lin.snrDb = true;
        app.lin.fitMethod = 1;
        app.lin.winMode = 2;
        app.lin.winLo = 110;
        app.lin.winHi = 1400;
        std::string sess = root + "/seriespanel.vsession";
        saveSession(sess, true);
        closeAll();
        app.lin = App::LinState{};
        app.seqLoadMode = 1;
        {
            std::string lerr = loadSession(sess);
            if (!lerr.empty())
                fprintf(stderr, "seriespanelselftest: loadSession: %s\n", lerr.c_str());
        }
        loadAll();
        {
            check(app.series.size() == 1, "B3 the series came back");
            const App::Series* S = app.series.empty() ? nullptr : &app.series.front();
            int nStack = 0, nFrame = 0, nSum = 0;
            bool vals = true;
            if (S)
                for (const auto& m : S->members) {
                    if (m.seqId != 0) {
                        nStack++;
                        if (m.fold == FOLD_SUM) nSum++;
                    } else if (m.frameUid != 0) {
                        nFrame++;
                        ImageDoc* fd = frameMemberDoc(m.frameUid);
                        if (!fd) { vals = false; continue; }
                        bool bright = fd->name.find("bright") != std::string::npos;
                        vals &= fabs(m.value - (bright ? 80.0 : 5.0)) < 1e-12;
                    }
                }
            fprintf(stderr, "seriespanelselftest: B3 restored %d stack + %d frame "
                            "(%d sum)\n", nStack, nFrame, nSum);
            check(nStack == 3 && nFrame == 2, "B3 members: 3 stack + 2 frame");
            check(nSum == 1, "B3 the declared fold came back (sum stays sum)");
            check(vals, "B3 frame member values survived by path");
            std::string why;
            check(seriesAudit(why), "B3 audit after restore");
            check(app.lin.tablePlane == 0 && app.lin.snrDb && app.lin.fitMethod == 1 &&
                  app.lin.winMode == 2 && fabs(app.lin.winLo - 110) < 1e-12 &&
                  fabs(app.lin.winHi - 1400) < 1e-12,
                  "B3 panel settings persisted (row 104's session gap, closed)");
        }
        // ---- B4: closing a frame member drops the membership, says stale ----
        {
            const App::Series* S = app.series.empty() ? nullptr : &app.series.front();
            int sid = S ? S->id : 0;
            if (App::Series* SW = seriesById(sid))
                for (auto& m : SW->members) m.fold = FOLD_MEAN;
            app.lin.winMode = 0;
            app.lin.fitMethod = 0;
            linRecompute(sid);
            bool wasValid = app.lin.fitValid;
            int idx = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->seqId == 0 &&
                    app.images[i]->name.find("dim") != std::string::npos)
                    idx = i;
            check(wasValid && idx >= 0, "B4 fixture: fit valid, dim.npy resident");
            if (idx >= 0) {
                app.current = idx;
                closeCurrent();                    // the real Ctrl+W door
            }
            S = app.series.empty() ? nullptr : &app.series.front();
            bool gone = true;
            if (S)
                for (const auto& m : S->members)
                    if (m.frameUid != 0 && frameMemberDoc(m.frameUid) == nullptr) gone = false;
            check(S && S->members.size() == 4 && gone,
                  "B4 the closed frame left the series (no dangling member)");
            check(!app.lin.fitValid,
                  "B4 the fit is stale: it measured a membership that is gone");
            std::string why;
            check(seriesAudit(why), "B4 audit after the close");
        }

        // ---- C1: a row's error message is PRINTED, never INTERPRETED --------
        // The mean cell of an invalid row shows that row's err, and err is a
        // RUNTIME string - it comes from computeStackStats / computeFrameStats,
        // i.e. from whoever next writes a refusal. ImGui's text calls are
        // printf-shaped, so handing err over as the format silently makes every
        // error message a format specification; -Wformat-security is the only
        // reason anyone knew.
        //
        // The poison is "100%%" rather than "%s" ON PURPOSE. "%s" would make the
        // bug read a pointer that was never passed - undefined behaviour, so a
        // test built on it measures the day's luck. "%%" takes NO argument at
        // all: printf collapses it to one '%' and reads nothing, so the failure
        // is a difference between two strings and nothing else. Before the fix
        // this cell showed "100%" where the row said "100%%"; the assert is that
        // what the row says is what the panel shows, character for character.
        {
            App::Series* SC = app.series.empty() ? nullptr : &app.series.front();
            int sid = SC ? SC->id : 0;
            linRecompute(sid);
            static const char* const POISON =
                "no finite samples (100%% of the ROI masked)";
            bool poisoned = false;
            for (auto& r : app.lin.rows)
                if (!poisoned) { r.valid = false; r.err = POISON; poisoned = true; }
            check(poisoned, "C1 fixture: one member's row carries an error message");
            // One frame of the REAL panel, with or without the backends - the
            // roiStatsSelftest() recipe, see the NewFrame comment there.
            ImGuiIO& io = ImGui::GetIO();
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
            ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_Always);
            ImGui::Begin("SeriesPanelProbe", nullptr,
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
            // ImGui's own text capture, which is the only reader that sees what
            // a widget PRINTED rather than what it was handed: LogRenderedText
            // runs downstream of the format pass. It also sets ItemUnclipByLog,
            // so a row scrolled out of the table still reports its text.
            ImGui::LogToBuffer();
            drawPanelSeriesAnalysis();
            // Read it BEFORE LogFinish: LogFinish clears LogBuffer (it is the
            // clipboard path's staging area), so afterwards there is nothing
            // left to look at - which is a silent empty pass, not an error.
            std::string shown = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::EndFrame();
            check(!shown.empty(), "C1 fixture: the panel drew and was captured");
            const bool verbatim = shown.find(POISON) != std::string::npos;
            if (!verbatim)
                fprintf(stderr, "seriespanelselftest: C1 the panel showed: %s\n",
                        shown.c_str());
            check(verbatim, "C1 the panel prints a row's error text verbatim");
        }

        fprintf(stderr, "seriespanelselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
        return ok ? 0 : 1;
    }
