// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
    if (!g_seriesSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 600.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](const char* what, bool cond) {
            fprintf(stderr, "seriesselftest: %-62s %s\n", what, cond ? "ok" : "FAILED");
            if (!cond) ok = false;
        };
        auto audit = [&]() {
            std::string why;
            if (seriesAudit(why)) return true;
            fprintf(stderr, "seriesselftest: AUDIT: %s\n", why.c_str());
            return false;
        };
        openFolder(g_seriesSelftest);
        loadAll();
        std::vector<int> sids;
        for (const auto& si : app.seqs) sids.push_back(si.id);
        if (sids.size() < 4 || app.images.empty()) {
            fprintf(stderr, "seriesselftest: need >= 4 stacks under %s, got %d\n",
                    g_seriesSelftest.c_str(), (int)sids.size());
            return 1;
        }
        int b0 = app.images[0]->batchId;

        // ---- 1. build one series out of every stack of one batch --------------
        int S1 = newSeries(b0, "");                 // "" = the canon's default name
        {
            App::Series* S = seriesById(S1);
            S->paramName = "illuminance";
            snprintf(S->unit, sizeof S->unit, "lx");
        }
        int added = 0;
        for (int s : sids)
            if (addToSeries(S1, s, extractLevelFromName(seqInfo(s)->name))) added++;
        App::Series* S = seriesById(S1);
        int valued = 0, batchOk = 0;
        for (const auto& m : S->members) {
            if (std::isfinite(m.value)) valued++;
            for (const auto& d : app.images)
                if (d->seqId == m.seqId && d->batchId == S->batchId) { batchOk++; break; }
        }
        fprintf(stderr, "seriesselftest: created series '%s' in batch '%s': %d member(s), "
                        "%d valued, param '%s' unit '%s' kind %d\n",
                S->name.c_str(), batchNameOf(b0).c_str(), (int)S->members.size(), valued,
                S->paramName.c_str(), S->unit, S->kind);
        check("every stack of the batch joined", added == (int)sids.size() &&
                                                 S->members.size() == sids.size());
        check("every member's value came from its name", valued == (int)S->members.size());
        check("every member's frames are in the series' batch", batchOk == (int)S->members.size());
        check("default name is \"<batch> 掃引\"", S->name == batchNameOf(b0) + " 掃引");
        check("invariant 1: audit after create", audit());
        {   // ---- what a member row is allowed to stop saying -----------------
            // A member row used to be "<condition> · <stack name>", and for a
            // stack that came out of a container the name was itself a compound
            // - "sweep.vnz:0" - because the row had to name its own sweep when
            // the sweep's heading had scrolled off the top of the panel. The
            // heading is held there now and the row leads with its own [n], so
            // the ":0" is the same fact twice and the file name goes to the end
            // of the row, where a narrowing panel drops it first.
            //
            // The tail comes off only when it is DIGITS: an .npz member is
            // "data.npz:arr_0", and an array name is not a position - nothing
            // else on the row repeats it.
            check("a container member drops the index the row now carries",
                  filesMemberName("sweep.vnz:0") == "sweep.vnz" &&
                  filesMemberName("sweep.vnz:12") == "sweep.vnz");
            check("an .npz member name is not an index and stays",
                  filesMemberName("data.npz:arr_0") == "data.npz:arr_0" &&
                  filesMemberName("data.npz:dark") == "data.npz:dark");
            check("a plain stack name is untouched",
                  filesMemberName("40lx") == "40lx" &&
                  filesMemberName("scanroot/frame_###.npy") == "scanroot/frame_###.npy" &&
                  filesMemberName("odd:") == "odd:" && filesMemberName(":7") == ":7");
        }
        check("N members + a unit -> seriesCanFit", seriesCanFit(*S));
        {   // the unit is never assumed: without one there is no fit, ever
            char keep[16];
            snprintf(keep, sizeof keep, "%s", S->unit);
            S->unit[0] = '\0';
            check("unit unset -> no fit (never assumed)", !seriesCanFit(*S));
            snprintf(S->unit, sizeof S->unit, "%s", keep);
        }

        // ---- persistence: save -> load -> lazy resolve ------------------------
        std::error_code tec;
        std::string sess = (std::filesystem::temp_directory_path(tec) /
                            "viewer_seriesselftest.vsession").u8string();
        std::string legacy = (std::filesystem::temp_directory_path(tec) /
                              "viewer_serieslegacy.vsession").u8string();
        int legacyCount = 0;
        {   // an OLD-format session, written now while the paths are known. It
            // has to be built by hand because saveSession has NEVER emitted
            // "seqlevel" - which is the honest reason the migration below is
            // nearly always a no-op.
            std::ofstream lf(pathFromUtf8(legacy), std::ios::binary);
            lf << "viewer-session 1\n" << std::setprecision(9);
            for (const auto& m : S->members) {
                std::vector<int> fr = framesOfSeq(m.seqId);
                if (fr.empty() || !std::isfinite(m.value)) continue;
                lf << "image 0 1 npy3 0 0 0 0 0 0 0 " << app.images[fr.front()]->src->path << "\n";
                lf << "imgbatch legacyset\n";
                lf << "seqload 1\n";
                lf << "seqlevel " << m.value << "\n";
                legacyCount++;
            }
        }
        const char* RENAMED = "renamed member stack";
        {
            // Doctor the series first, so the round trip has something to lose:
            // a renamed stack, an UNSET value, an excluded member, and a value
            // with more significant digits than the file's default precision -
            // the axis of a fit is the last place to accept a rounded number.
            seqInfo(S->members[1].seqId)->name = RENAMED;
            S->members[2].value = std::numeric_limits<double>::quiet_NaN();
            S->members[3].include = false;
            S->members[4].value = 1234567.891234;
            struct Want { std::string path; double value; bool include; };
            std::vector<Want> want;
            for (const auto& m : S->members) {
                std::vector<int> fr = framesOfSeq(m.seqId);
                want.push_back({ fr.empty() ? std::string() : app.images[fr.front()]->src->path,
                                 m.value, m.include });
            }
            std::string wantName = S->name, wantParam = S->paramName, wantUnit = S->unit;
            std::string wantBatch = batchNameOf(S->batchId);
            int wantKind = S->kind;
            saveSession(sess, true);
            std::string lerr = loadSession(sess);       // closeAll happens inside
            check("session reloaded", lerr.empty());
            loadAll();                                  // stacks come back first
            double tr = nowSec();                  // ...the series after them
            while (nowSec() - tr < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            App::Series* R = app.series.empty() ? nullptr : &app.series.front();
            fprintf(stderr, "seriesselftest: round trip: %d series; '%s' param '%s' "
                            "unit '%s' kind %d, %d member(s), batch '%s'\n",
                    (int)app.series.size(), R ? R->name.c_str() : "",
                    R ? R->paramName.c_str() : "", R ? R->unit : "", R ? R->kind : -1,
                    R ? (int)R->members.size() : -1,
                    R ? batchNameOf(R->batchId).c_str() : "");
            check("exactly one series came back", app.series.size() == 1);
            check("name / parameter / unit / kind survive",
                  R && R->name == wantName && R->paramName == wantParam &&
                  wantUnit == R->unit && R->kind == wantKind);
            check("the series is in its batch, restored BY NAME",
                  R && batchNameOf(R->batchId) == wantBatch);
            bool memOk = R && R->members.size() == want.size();
            if (memOk)
                for (size_t i = 0; i < want.size(); i++) {
                    std::vector<int> fr = framesOfSeq(R->members[i].seqId);
                    std::string p = fr.empty() ? std::string() : app.images[fr.front()]->src->path;
                    double a = want[i].value, b = R->members[i].value;
                    bool vOk = std::isfinite(a) == std::isfinite(b) &&
                               (!std::isfinite(a) ||
                                fabs(a - b) <= 1e-9 * std::max(1.0, fabs(a)));
                    if (p != want[i].path || !vOk ||
                        R->members[i].include != want[i].include) memOk = false;
                }
            check("members: order, first-frame path, value and include all survive", memOk);
            check("a value with more digits than the file's precision survives EXACTLY",
                  R && R->members.size() > 4 && R->members[4].value == 1234567.891234);
            check("the unset member came back UNSET, not 0",
                  R && R->members.size() > 2 && !std::isfinite(R->members[2].value));
            check("the excluded member came back excluded",
                  R && R->members.size() > 3 && !R->members[3].include);
            check("invariant 1: audit after restore", audit());
            // Members are keyed by PATH and not by stack name because a NAME is
            // renameable and not unique - two folders can hold the same stack
            // name, and the user may change it between save and load. (The
            // older reason, "seqname does not survive at all for a folder
            // stack", was a BUG and is fixed: it now travels on the queued
            // App::SeqRestore entry and is applied when the rescan creates the
            // stack. terminology.md says stack renames are session-saved.)
            bool nameSurvived = false;
            for (const auto& si : app.seqs) if (si.name == RENAMED) nameSurvived = true;
            fprintf(stderr, "seriesselftest: the renamed stack's name after restore: %s\n",
                    nameSurvived ? "survived" : "LOST");
            check("a folder stack's user-given name survives the session",
                  nameSurvived);
        }
        // everything below works on what came back: the reload minted new ids
        if (app.series.empty() || app.images.empty() || app.series.front().members.size() < 4) {
            fprintf(stderr, "seriesselftest: nothing usable survived the round trip\n");
            return 1;
        }
        S1 = app.series.front().id;
        S = seriesById(S1);
        b0 = S->batchId;
        sids.clear();
        for (const auto& m : S->members) sids.push_back(m.seqId);
        // Undo the exclusion; the value deliberately left UNSET stays unset - it
        // is the point of the next block. (It cannot be re-derived from the name
        // either: a restored folder stack is called "frame_000‥023.npy", with no
        // folder part to read a level out of. That is the same seqname hole,
        // seen from the other side.)
        for (auto& m : S->members) m.include = true;

        // ---- invariant 6: an unset value is UNSET, never 0 --------------------
        int ptsAll = seriesFitPoints(*S);
        {
            double keep = S->members[0].value;
            S->members[0].value = std::numeric_limits<double>::quiet_NaN();
            int nanPts = seriesFitPoints(*S);
            S->members[0].value = keep;
            S->members[0].include = false;
            int excPts = seriesFitPoints(*S);
            S->members[0].include = true;
            fprintf(stderr, "seriesselftest: fit points %d -> %d with one value unset, "
                            "%d with one excluded\n", ptsAll, nanPts, excPts);
            check("invariant 6: an unset value leaves the fit", nanPts == ptsAll - 1);
            check("an excluded member leaves the fit", excPts == ptsAll - 1);
            check("both are reversible", seriesFitPoints(*S) == ptsAll);
        }

        // ---- invariant 2: a stack is in AT MOST ONE series --------------------
        int S2 = newSeries(b0, "second");
        {
            int moved = sids[0];
            bool didMove = addToSeries(S2, moved, 1.0);
            S = seriesById(S1);
            App::Series* T = seriesById(S2);
            fprintf(stderr, "seriesselftest: stack %d added to '%s': '%s' now %d member(s), "
                            "'%s' %d\n", moved, T->name.c_str(), S->name.c_str(),
                    (int)S->members.size(), T->name.c_str(), (int)T->members.size());
            check("invariant 2: adding elsewhere MOVES the stack",
                  didMove && T->members.size() == 1 && S->members.size() == sids.size() - 1 &&
                  seriesOfStack(moved) == T);
            check("invariant 5: a single member cannot be fitted", !seriesCanFit(*T));
            check("invariant 1: audit after the move", audit());
            addToSeries(S1, moved, extractLevelFromName(seqInfo(moved)->name));
            pruneEmptySeries();
            check("emptied series is pruned", seriesById(S2) == nullptr &&
                  seriesById(S1)->members.size() == sids.size());
        }

        // ---- invariant 4: a member moved out alone LEAVES the series ----------
        {
            int other = newBatch(uniqueBatchName("other"));
            int mv = sids[1];
            size_t before = seriesById(S1)->members.size();
            // A membership edit made from OUTSIDE the modal invalidates just as
            // one made inside it does: the panel keys freshness on the series id
            // and fitValid alone, and a move leaves the series alive.
            linRecompute(S1);
            bool fitBefore = app.lin.fitValid;
            int ptsBefore = app.lin.nPts;
            app.toast.clear();
            moveStackToBatch(mv, other);
            S = seriesById(S1);
            fprintf(stderr, "seriesselftest: moveStackToBatch(%d -> '%s'): %d -> %d member(s), "
                            "toast \"%s\"\n", mv, "other", (int)before,
                    (int)S->members.size(), app.toast.c_str());
            fprintf(stderr, "seriesselftest: fit before the move %s (%d point(s)), after %s\n",
                    fitBefore ? "ok" : "none", ptsBefore,
                    app.lin.fitValid ? "STILL ok" : "dropped");
            check("invariant 4: the moved stack left the series",
                  S->members.size() == before - 1 && seriesOfStack(mv) == nullptr);
            check("...and the screen was told", app.toast.find("left series") != std::string::npos);
            check("...and the fit that counted it is dropped",
                  fitBefore && ptsBefore > 0 && !app.lin.fitValid);
            check("invariant 1: audit after the move-to-batch", audit());
            bool crossed = addToSeries(S1, mv, 1.0);
            fprintf(stderr, "seriesselftest: addToSeries across batches returned %d\n",
                    crossed ? 1 : 0);
            check("a stack of another batch cannot join", !crossed && seriesOfStack(mv) == nullptr);
        }

        // ---- invariant 3: closeStack removes the member ----------------------
        {
            int cl = seriesById(S1)->members.front().seqId;
            size_t before = seriesById(S1)->members.size();
            closeStack(cl);
            S = seriesById(S1);
            fprintf(stderr, "seriesselftest: closeStack(%d): %d -> %d member(s)\n",
                    cl, (int)before, S ? (int)S->members.size() : -1);
            check("invariant 3: closeStack drops the member",
                  S && S->members.size() == before - 1 && seriesOfStack(cl) == nullptr);
            check("invariant 1: audit after closeStack", audit());
        }

        // ---- invariant 5: one member is legal, just not fittable --------------
        while (seriesById(S1) && seriesById(S1)->members.size() > 1)
            removeFromSeries(seriesById(S1)->members.back().seqId);
        S = seriesById(S1);
        check("a one-member series is legal", S != nullptr && S->members.size() == 1);
        check("invariant 5: ...and seriesCanFit() is false", S && !seriesCanFit(*S));

        // ---- invariant 3: closeBatch takes its series with it -----------------
        {
            int before = (int)app.series.size();
            closeBatch(b0);
            fprintf(stderr, "seriesselftest: closeBatch(%d): %d -> %d series\n",
                    b0, before, (int)app.series.size());
            check("invariant 3: closeBatch discards its series",
                  seriesById(S1) == nullptr && app.series.empty());
            check("invariant 1: audit after closeBatch", audit());
        }

        // ---- migration: an OLD session that carried per-stack levels -----------
        {
            std::string lerr = loadSession(legacy);
            check("legacy (seqlevel) session loaded", lerr.empty());
            loadAll();
            double tm = nowSec();
            while (nowSec() - tm < 120.0 && !app.seqLevelLegacy.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* M = app.series.empty() ? nullptr : &app.series.front();
            fprintf(stderr, "seriesselftest: seqlevel migration: %d series, %d member(s), "
                            "name '%s' unit '%s' kind %d (%d levelled stacks in the file)\n",
                    (int)app.series.size(), M ? (int)M->members.size() : -1,
                    M ? M->name.c_str() : "", M ? M->unit : "", M ? M->kind : -1,
                    legacyCount);
            check("one series migrated out of the levels", app.series.size() == 1);
            check("...holding every levelled stack",
                  M && (int)M->members.size() == legacyCount);
            check("...named \"<batch> 掃引\", kind linearity",
                  M && M->name == "legacyset 掃引" && M->kind == App::Series::KLinearity);
            check("invariant 1: audit after migration", audit());
            // The negative half of the same rule: ONE levelled stack is not a
            // sweep, and nothing may be invented from it.
            std::vector<std::string> lpaths;
            if (M)
                for (const auto& m : M->members) {
                    std::vector<int> fr = framesOfSeq(m.seqId);
                    if (!fr.empty()) lpaths.push_back(app.images[fr.front()]->src->path);
                }
            std::string onePath = lpaths.empty() ? std::string() : lpaths.front();
            {
                std::ofstream lf(pathFromUtf8(legacy), std::ios::binary);
                lf << "viewer-session 1\n";
                lf << "image 0 1 npy3 0 0 0 0 0 0 0 " << onePath << "\n";
                lf << "imgbatch lonelyset\n";
                lf << "seqload 1\n";
                lf << "seqlevel 42\n";
            }
            loadSession(legacy);
            loadAll();
            double tm2 = nowSec();
            while (nowSec() - tm2 < 120.0 && !app.seqLevelLegacy.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: one levelled stack alone -> %d series, "
                            "%d stack(s) open\n", (int)app.series.size(),
                    (int)app.seqs.size());
            check("a single levelled stack makes NO series", app.series.empty() &&
                                                            !app.seqs.empty());
            // ...and the same rule for TEXT. `ls >> lv` stores 0.0 when the
            // extraction fails, so every unreadable level used to migrate as a
            // hard 0 - which in linearity is the dark stack, and the migration
            // hands the series the "lx" prefill so it is fittable at once. This
            // branch reads hand-written and third-party files only, i.e. exactly
            // the input that carries text a parser cannot read.
            {
                std::ofstream lf(pathFromUtf8(legacy), std::ios::binary);
                lf << "viewer-session 1\n";
                const char* BAD[] = { "notanumber", "1e", "-", "12x", "" };
                for (size_t i = 0; i < lpaths.size(); i++) {
                    lf << "image 0 1 npy3 0 0 0 0 0 0 0 " << lpaths[i] << "\n";
                    lf << "imgbatch junkset\n" << "seqload 1\n";
                    lf << "seqlevel " << BAD[std::min<size_t>(i, 4)] << "\n";
                }
            }
            loadSession(legacy);
            loadAll();
            double tm3 = nowSec();
            while (nowSec() - tm3 < 120.0 && !app.seqLevelLegacy.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            int zeroed = 0;
            for (const auto& s : app.series)
                for (const auto& m : s.members)
                    if (std::isfinite(m.value) && fabs(m.value) < 1e-9) zeroed++;
            fprintf(stderr, "seriesselftest: %d unreadable seqlevel(s) -> %d series, "
                            "%d member(s) at 0, %d stack(s) open\n", (int)lpaths.size(),
                    (int)app.series.size(), zeroed, (int)app.seqs.size());
            check("an unreadable seqlevel is UNSET, never a dark stack at 0",
                  lpaths.size() >= 2 && app.series.empty() && zeroed == 0 &&
                  !app.seqs.empty());
        }

        // ---- the create/edit modal, pressed the way a human presses it --------
        // A number that nobody chose is the failure this whole layer exists to
        // prevent, and the modal is the shortest path to one: it is opened to
        // rename a series, and its Save rebuilds every member from the boxes.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int b = app.images.empty() ? 0 : app.images[0]->batchId;
            int sid = selftestMakeSeries(b, "lx");
            App::Series* M = seriesById(sid);
            if (!M || M->members.size() < 4) {
                fprintf(stderr, "seriesselftest: modal fixture failed\n");
                return 1;
            }
            // one member deliberately UNSET, one carrying more digits than the
            // box can show. Pick the unset one where the NAME does suggest a
            // number, so the guess has something to fabricate.
            int unsetIdx = -1;
            for (int i = 0; i < (int)M->members.size(); i++) {
                double g = extractLevelFromName(seqInfo(M->members[i].seqId)->name);
                if (std::isfinite(g) && fabs(g) > 1e-9) { unsetIdx = i; break; }
            }
            const double PRECISE = 1234567.89;
            int preciseIdx = unsetIdx == 0 ? 1 : 0;
            if (unsetIdx < 0) { fprintf(stderr, "seriesselftest: no named level\n"); return 1; }
            int unsetSeq = M->members[unsetIdx].seqId;
            std::string unsetName = seqInfo(unsetSeq)->name;
            double guess = extractLevelFromName(unsetName);
            M->members[unsetIdx].value = std::numeric_limits<double>::quiet_NaN();
            M->members[preciseIdx].value = PRECISE;
            int preciseSeq = M->members[preciseIdx].seqId;
            // "Edit series..." and then "Save", with NOTHING touched between.
            openSeriesModal(b, sid);
            const App::SeriesEdit::Row* r0 = nullptr;
            for (const auto& r : app.seriesEdit.rows) if (r.seqId == unsetSeq) r0 = &r;
            int nValued = 0;
            for (const auto& r : app.seriesEdit.rows)
                if (r.check && std::isfinite(seriesEditRowValue(r))) nValued++;
            fprintf(stderr, "seriesselftest: Edit... on '%s' (value unset, name suggests "
                            "%.6g): box \"%s\", %d of %d rows have a value\n",
                    unsetName.c_str(), guess, r0 ? r0->value : "?", nValued,
                    (int)app.seriesEdit.rows.size());
            check("Edit... leaves an unset member's box EMPTY (no re-guess)",
                  r0 && r0->value[0] == '\0' && std::isfinite(guess));
            check("...and the modal's counter does not claim it has a value",
                  nValued == (int)app.seriesEdit.rows.size() - 1);
            seriesModalAccept();
            M = seriesById(sid);
            double vu = 0, vp = 0;
            for (const auto& m : M->members) {
                if (m.seqId == unsetSeq) vu = m.value;
                if (m.seqId == preciseSeq) vp = m.value;
            }
            fprintf(stderr, "seriesselftest: after a Save that touched nothing: unset "
                            "member %s, %.11g -> %.11g\n",
                    std::isfinite(vu) ? "HAS A VALUE NOW" : "still unset", PRECISE, vp);
            check("a Save that edited nothing leaves the unset member UNSET",
                  !std::isfinite(vu));
            check("...and does not round the values it never touched", vp == PRECISE);
            check("invariant 1: audit after the no-op Save", audit());

            // The case the no-op Save cannot see: the box was TOUCHED and put
            // back. InputText reports a change on any accepted keystroke, so
            // from the first one the TEXT is the value - and a lossy prefill is
            // then authoritative. Type a digit into 1234567.89 and delete it
            // again, press Save, and the member became 1234570.
            openSeriesModal(b, sid);
            std::string shown;
            for (auto& r : app.seriesEdit.rows)
                if (r.seqId == preciseSeq) { r.touched = true; shown = r.value; }
            seriesModalAccept();
            M = seriesById(sid);
            double vt = 0;
            for (const auto& m : M->members) if (m.seqId == preciseSeq) vt = m.value;
            fprintf(stderr, "seriesselftest: box touched and reverted (showing \"%s\"): "
                            "%.11g -> %.11g\n", shown.c_str(), PRECISE, vt);
            check("a box touched and put back re-reads as the SAME double",
                  vt == PRECISE);

            // Text the program cannot read is UNSET, not 0. 0 is not "missing":
            // it is the dark stack the offset is anchored to and the read noise
            // is measured in (linRecompute), so a typo lands on the one value
            // that changes the answer most.
            openSeriesModal(b, sid);
            const char* JUNK[] = { "-", "1e", "12x", "" };
            std::vector<int> junkSeq;
            for (int i = 0; i < 4 && i < (int)app.seriesEdit.rows.size(); i++) {
                App::SeriesEdit::Row& r = app.seriesEdit.rows[i];
                snprintf(r.value, sizeof r.value, "%s", JUNK[i]);
                r.touched = true;
                junkSeq.push_back(r.seqId);
            }
            seriesModalAccept();
            M = seriesById(sid);
            int zeroed = 0, unset = 0;
            for (const auto& m : M->members)
                if (std::find(junkSeq.begin(), junkSeq.end(), m.seqId) != junkSeq.end()) {
                    if (!std::isfinite(m.value)) unset++;
                    else if (fabs(m.value) < 1e-9) zeroed++;
                }
            fprintf(stderr, "seriesselftest: value boxes \"-\" \"1e\" \"12x\" \"\" -> "
                            "%d unset, %d at 0\n", unset, zeroed);
            check("a value box that is not a number saves as UNSET, never 0",
                  unset == (int)junkSeq.size() && zeroed == 0);

            // Editing a series makes the fit on screen a measurement of a series
            // that no longer exists - and the panel reads its LABELS live, so a
            // fit left standing gets relabelled with the new unit.
            for (auto& m : M->members) m.value = extractLevelFromName(seqInfo(m.seqId)->name);
            linRecompute(sid);
            bool wasFit = app.lin.fitValid;
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "ms");
            seriesModalAccept();
            fprintf(stderr, "seriesselftest: fit before the unit edit %s, after %s\n",
                    wasFit ? "valid" : "invalid", app.lin.fitValid ? "STILL VALID" : "dropped");
            check("a fit exists before the edit", wasFit);
            check("editing the series drops the fit it no longer describes",
                  !app.lin.fitValid && app.lin.seriesId == 0);
            // ...and with the unit CLEARED there is no fit at all, which is the
            // one rule this layer was built to enforce.
            openSeriesModal(b, sid);
            app.seriesEdit.unit[0] = '\0';
            seriesModalAccept();
            linRecompute(sid);
            fprintf(stderr, "seriesselftest: unit cleared -> fit %s, %d point(s)\n",
                    app.lin.fitValid ? "PRINTED" : "refused", app.lin.nPts);
            check("a series whose unit was cleared cannot be fitted",
                  !app.lin.fitValid && !seriesCanFit(*seriesById(sid)));

            // The KIND decides the equation (docs/terminology.md). This panel
            // knows linearity and photon transfer; a temperature run pushed
            // through them prints a "sensitivity" in DN per degree.
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "degC");
            app.seriesEdit.kind = App::Series::KTemperature;
            seriesModalAccept();
            linRecompute(sid);
            fprintf(stderr, "seriesselftest: kind=temperature -> canFit %d, fit %s\n",
                    seriesCanFit(*seriesById(sid)) ? 1 : 0,
                    app.lin.fitValid ? "PRINTED" : "refused");
            check("a temperature series is not fitted as linearity",
                  !seriesCanFit(*seriesById(sid)) && !app.lin.fitValid);
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "lx");
            app.seriesEdit.kind = App::Series::KLinearity;
            seriesModalAccept();
            linRecompute(sid);
            check("...and putting the kind back brings the fit back",
                  app.lin.fitValid && seriesCanFit(*seriesById(sid)));

            // seqctx > Series > <name>: joining the SOLE member of one series
            // into another empties the first, and an empty series is the state
            // the audit rejects (plan §1 invariant 3).
            int lone = newSeries(b, "lone");
            if (App::Series* L2 = seriesById(lone))
                snprintf(L2->unit, sizeof L2->unit, "lx");
            int moved = seriesById(sid)->members.front().seqId;
            addToSeries(lone, moved, 1.0);
            std::string jmsg;
            bool joined = seriesJoinFromMenu(sid, moved, &jmsg);
            fprintf(stderr, "seriesselftest: join -> %d, \"%s\"; %d series left\n",
                    joined ? 1 : 0, jmsg.c_str(), (int)app.series.size());
            check("the menu join reports the value and the text it read it from",
                  joined && jmsg.find("read from") != std::string::npos);
            check("...and does not leave the emptied series behind",
                  seriesById(lone) == nullptr);
            check("invariant 1: audit after the menu join", audit());
        }

        // ---- sessions this program did not write ------------------------------
        // The writer honours "unset is not 0"; the reader is where a hand-made,
        // third-party or half-written file gets to claim otherwise.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            std::vector<std::string> paths;
            for (const auto& si : app.seqs) {
                std::vector<int> fr = framesOfSeq(si.id);
                if (!fr.empty()) paths.push_back(app.images[fr.front()]->src->path);
            }
            std::string bn = batchNameOf(app.images.empty() ? 0 : app.images[0]->batchId);
            std::string hostile = (std::filesystem::temp_directory_path(tec) /
                                   "viewer_serieshostile.vsession").u8string();
            {
                std::ofstream hf(pathFromUtf8(hostile), std::ios::binary);
                hf << "viewer-session 1\n";
                for (const auto& p : paths)
                    hf << "image 0 1 npy3 0 0 0 0 0 0 0 " << p << "\n"
                       << "imgbatch " << bn << "\n" << "seqload 1\n";
                hf << "series hostile\nseriesbatch " << bn << "\n"
                   << "seriesparam illuminance\nseriesunit lx\nserieskind 0\n";
                const char* VALS[] = { "notanumber", "-", "1e", "nan", "12x", "160", "320" };
                for (size_t i = 0; i < paths.size(); i++)
                    hf << "seriesmember " << VALS[std::min<size_t>(i, 6)] << " 1 "
                       << paths[i] << "\n";
                hf << "seriesend\n";
            }
            loadSession(hostile);
            loadAll();
            double th = nowSec();
            while (nowSec() - th < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* H = app.series.empty() ? nullptr : &app.series.front();
            std::string got;
            int atZero = 0;
            if (H)
                for (const auto& m : H->members) {
                    char bb[32];
                    snprintf(bb, sizeof bb, "%.6g", m.value);
                    got += (got.empty() ? "" : ",");
                    got += std::isfinite(m.value) ? bb : "unset";
                    if (std::isfinite(m.value) && fabs(m.value) < 1e-9) atZero++;
                }
            fprintf(stderr, "seriesselftest: hostile values -> [%s]\n", got.c_str());
            check("one series came back from the hostile file", app.series.size() == 1);
            check("no unreadable value became 0", atZero == 0);
            check("...they are UNSET, and the readable ones survived",
                  H && H->members.size() == paths.size() &&
                  std::isfinite(H->members.back().value));
            check("invariant 1: audit after the hostile file", audit());
            std::filesystem::remove(std::filesystem::u8path(hostile), tec);

            // The same folder open TWICE is two batches (the canon blesses it),
            // so one path names two stacks. A restore that takes "the first one"
            // rejects every member of the second copy's series on containment
            // and loses the whole series - permanently, at the next autosave.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int bA = app.images.empty() ? 0 : app.images[0]->batchId;
            openFolder(g_seriesSelftest);
            loadAll();
            int bB = 0;
            for (const auto& bt : app.batches) if (bt.id != bA) bB = bt.id;
            int sA = selftestMakeSeries(bA, "lx"), sB = selftestMakeSeries(bB, "ms");
            size_t nA = seriesById(sA) ? seriesById(sA)->members.size() : 0;
            size_t nB = seriesById(sB) ? seriesById(sB)->members.size() : 0;
            std::string dup = (std::filesystem::temp_directory_path(tec) /
                               "viewer_seriesdup.vsession").u8string();
            saveSession(dup, true);
            loadSession(dup);
            loadAll();
            double td = nowSec();
            while (nowSec() - td < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::vector<int> sbatch;
            for (const auto& s : app.series)
                if (std::find(sbatch.begin(), sbatch.end(), s.batchId) == sbatch.end())
                    sbatch.push_back(s.batchId);
            fprintf(stderr, "seriesselftest: same folder in two batches: saved %d + %d "
                            "member(s), restored %d series in %d batch(es)\n",
                    (int)nA, (int)nB, (int)app.series.size(), (int)sbatch.size());
            check("both series survive when one folder is open in two batches",
                  app.series.size() == 2 && nA > 0 && nB > 0 &&
                  app.series[0].members.size() == nA &&
                  app.series[1].members.size() == nB);
            check("...each in its own batch", app.series.size() == 2 &&
                  app.series[0].batchId != app.series[1].batchId);
            check("invariant 1: audit after the two-batch restore", audit());
            std::filesystem::remove(std::filesystem::u8path(dup), tec);

            // ...and the arrangement the canon RECOMMENDS for making one sweep
            // out of two opens: Move a stack from the second batch into the
            // first, so two stacks of the SAME first-frame path sit in ONE
            // batch. preferBatch cannot separate those - it only chooses
            // between batches - and the second member used to resolve onto the
            // stack the first had taken and be dropped by a bare `continue`,
            // uncounted, under a "restored 1 series" toast.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int bm = app.images.empty() ? 0 : app.images[0]->batchId;
            int keep = app.seqs.empty() ? 0 : app.seqs.front().id;
            std::string keepPath;
            {
                std::vector<int> fr = framesOfSeq(keep);
                if (!fr.empty()) keepPath = app.images[fr.front()]->src->path;
            }
            openFolder(g_seriesSelftest);
            loadAll();
            int mv = 0;
            for (const auto& si : app.seqs) {
                if (si.id == keep || batchOfStack(si.id) == bm) continue;
                std::vector<int> fr = framesOfSeq(si.id);
                if (!fr.empty() && app.images[fr.front()]->src->path == keepPath) mv = si.id;
            }
            if (mv) moveStackToBatch(mv, bm);
            int sm = newSeries(bm, "merged");
            if (App::Series* M2 = seriesById(sm)) {
                M2->paramName = "illuminance";
                snprintf(M2->unit, sizeof M2->unit, "lx");
            }
            addToSeries(sm, keep, 1.0);
            addToSeries(sm, mv, 2.0);
            size_t nMerged = seriesById(sm) ? seriesById(sm)->members.size() : 0;
            std::string same = (std::filesystem::temp_directory_path(tec) /
                                "viewer_seriessamepath.vsession").u8string();
            saveSession(same, true);
            loadSession(same);
            loadAll();
            double ts = nowSec();
            while (nowSec() - ts < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            {
                const App::Series* M3 = app.series.empty() ? nullptr : &app.series.front();
                std::string vals;
                bool distinct = M3 && M3->members.size() == 2 &&
                                M3->members[0].seqId != M3->members[1].seqId;
                if (M3)
                    for (const auto& m : M3->members) {
                        char bb[32];
                        snprintf(bb, sizeof bb, "%.6g", m.value);
                        vals += (vals.empty() ? "" : ",");
                        vals += std::isfinite(m.value) ? bb : "unset";
                    }
                fprintf(stderr, "seriesselftest: two same-path stacks in ONE batch: saved "
                                "%d member(s), restored %d [%s], toast \"%s\"\n",
                        (int)nMerged, M3 ? (int)M3->members.size() : -1, vals.c_str(),
                        app.toast.c_str());
                check("two same-path stacks in one batch both survive the round trip",
                      nMerged == 2 && M3 && M3->members.size() == 2);
                check("...bound to DIFFERENT stacks, each keeping its own value",
                      distinct && vals == "1,2");
                check("invariant 1: audit after the same-path restore", audit());
            }
            std::filesystem::remove(std::filesystem::u8path(same), tec);

            // A seriesmember line cut before its path (a power loss during the
            // autosave, the crash handler's short write, a hand-edit). The path
            // is the LAST field, so truncation takes it first: the line never
            // becomes a member and nothing downstream could count it.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            std::vector<std::string> tpaths;
            for (const auto& si : app.seqs) {
                std::vector<int> fr = framesOfSeq(si.id);
                if (!fr.empty()) tpaths.push_back(app.images[fr.front()]->src->path);
            }
            std::string tbn = batchNameOf(app.images.empty() ? 0 : app.images[0]->batchId);
            std::string cut = (std::filesystem::temp_directory_path(tec) /
                               "viewer_seriescut.vsession").u8string();
            {
                std::ofstream cf(pathFromUtf8(cut), std::ios::binary);
                cf << "viewer-session 1\n";
                for (const auto& p : tpaths)
                    cf << "image 0 1 npy3 0 0 0 0 0 0 0 " << p << "\n"
                       << "imgbatch " << tbn << "\n" << "seqload 1\n";
                cf << "series cut\nseriesbatch " << tbn << "\n"
                   << "seriesparam illuminance\nseriesunit lx\nserieskind 0\n";
                for (size_t i = 0; i + 1 < tpaths.size(); i++)
                    cf << "seriesmember " << (i * 10) << " 1 " << tpaths[i] << "\n";
                cf << "seriesmember 60 1\n";      // cut before the path
                cf << "seriesend\n";
            }
            closeAll();
            loadSession(cut);
            loadAll();
            double tc = nowSec();
            while (nowSec() - tc < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: truncated member line -> %d member(s), "
                            "toast \"%s\"\n",
                    app.series.empty() ? -1 : (int)app.series.front().members.size(),
                    app.toast.c_str());
            check("a truncated seriesmember line is REPORTED, not swallowed",
                  app.toast.find("could not be resolved") != std::string::npos &&
                  app.toast.find("1 member(s)") != std::string::npos);
            std::filesystem::remove(std::filesystem::u8path(cut), tec);

            // Ctrl+S (or the autosave, or the crash snapshot) while the file's
            // series are still waiting for their stacks. app.series is empty
            // for that whole window, so the save used to write them away and
            // toast "session saved" with nothing to report.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int bw = app.images.empty() ? 0 : app.images[0]->batchId;
            int sw = selftestMakeSeries(bw, "lx");
            size_t nWip = seriesById(sw) ? seriesById(sw)->members.size() : 0;
            std::string wip = (std::filesystem::temp_directory_path(tec) /
                               "viewer_serieswip.vsession").u8string();
            std::string mid = (std::filesystem::temp_directory_path(tec) /
                               "viewer_seriesmid.vsession").u8string();
            saveSession(wip, true);
            loadSession(wip);              // parsed; the stacks are queued
            check("the restore really is pending (app.series empty, nothing lost yet)",
                  app.series.empty() && !app.seriesRestore.empty());
            saveSession(mid, true);        // <- the save INSIDE the window
            loadAll();
            double tw = nowSec();
            while (nowSec() - tw < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            closeAll();
            loadSession(mid);
            loadAll();
            double tm = nowSec();
            while (nowSec() - tm < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: saved mid-restore -> %d series, %d member(s) "
                            "(had %d)\n", (int)app.series.size(),
                    app.series.empty() ? -1 : (int)app.series.front().members.size(),
                    (int)nWip);
            check("a save while the restore is pending keeps the series",
                  nWip > 0 && app.series.size() == 1 &&
                  app.series.front().members.size() == nWip);
            check("invariant 1: audit after the mid-restore save", audit());
            std::filesystem::remove(std::filesystem::u8path(wip), tec);
            std::filesystem::remove(std::filesystem::u8path(mid), tec);

            // Renaming two live batches to ONE name. Batch names must be unique
            // (docs/terminology.md) because a session restores batches by name,
            // so duplicates merge on the next load - and with the same folder
            // open twice, the merge defeats preferBatch and one series is
            // silently emptied and pruned under a "restored 2 series" toast.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int r1 = app.images.empty() ? 0 : app.images[0]->batchId;
            openFolder(g_seriesSelftest);
            loadAll();
            int r2 = 0;
            for (const auto& bt : app.batches) if (bt.id != r1 && bt.name != "preview") r2 = bt.id;
            int rs1 = selftestMakeSeries(r1, "lx"), rs2 = selftestMakeSeries(r2, "ms");
            size_t n1 = seriesById(rs1) ? seriesById(rs1)->members.size() : 0;
            size_t n2 = seriesById(rs2) ? seriesById(rs2)->members.size() : 0;
            renameBatch(r1, "set");
            renameBatch(r2, "set");
            fprintf(stderr, "seriesselftest: both batches renamed to \"set\" -> \"%s\" "
                            "and \"%s\"\n", batchNameOf(r1).c_str(), batchNameOf(r2).c_str());
            check("a rename cannot duplicate a live batch name",
                  batchNameOf(r1) != batchNameOf(r2) && batchNameOf(r1) == "set");
            std::string rn = (std::filesystem::temp_directory_path(tec) /
                              "viewer_seriesrename.vsession").u8string();
            saveSession(rn, true);
            loadSession(rn);
            loadAll();
            double tr = nowSec();
            while (nowSec() - tr < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: after the rename round trip: %d series in "
                            "%d batch(es) (saved %d + %d member(s))\n",
                    (int)app.series.size(),
                    app.series.size() == 2 &&
                            app.series[0].batchId == app.series[1].batchId ? 1 : 2,
                    (int)n1, (int)n2);
            check("...so neither series is lost on the round trip",
                  n1 > 0 && n2 > 0 && app.series.size() == 2 &&
                  app.series[0].members.size() == n1 &&
                  app.series[1].members.size() == n2);
            // The remaining damage a duplicate name does, now that the restore
            // claim list stops the cross-series theft: batchReuse MERGES the two
            // on load, so the user's two groupings silently come back as one.
            check("...and each comes back in its OWN batch, not merged into one",
                  app.series.size() == 2 &&
                  app.series[0].batchId != app.series[1].batchId);
            check("invariant 1: audit after the rename round trip", audit());
            std::filesystem::remove(std::filesystem::u8path(rn), tec);
        }

        // ---- phase 4: what the Files panel's series row actually does ---------
        // The rows themselves are ImGui, but every command behind them is a
        // function, and these are the three that can lose data if they are
        // wrong: a move that drops members, an ungroup that closes something,
        // a Close that leaves stacks behind.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int b = app.images.empty() ? 0 : app.images[0]->batchId;
            int sid = selftestMakeSeries(b, "lx");
            App::Series* P = seriesById(sid);
            int nStacks = (int)app.seqs.size();
            check("phase 4 fixture: one series over the whole batch",
                  P && (int)P->members.size() == nStacks && nStacks >= 4);
            if (!P) { fprintf(stderr, "seriesselftest: phase 4 fixture failed\n"); return 1; }
            // Move the SERIES: unlike a lone member, it takes everything with it
            // and stays whole (the row's "Move to batch (all members move)").
            {
                int dest = newBatch(uniqueBatchName("moved"));
                std::vector<int> was;
                for (const auto& m : P->members) was.push_back(m.seqId);
                app.toast.clear();
                moveSeriesToBatch(sid, dest);
                P = seriesById(sid);
                bool kept = P && P->members.size() == was.size() && P->batchId == dest;
                if (kept)
                    for (size_t i = 0; i < was.size(); i++)
                        if (P->members[i].seqId != was[i] || batchOfStack(was[i]) != dest)
                            kept = false;
                fprintf(stderr, "seriesselftest: moveSeriesToBatch -> batch '%s', "
                                "%d member(s), toast \"%s\"\n", batchNameOf(dest).c_str(),
                        P ? (int)P->members.size() : -1, app.toast.c_str());
                check("a series moved to another batch keeps every member", kept);
                check("...and nothing was told it 'left the series'",
                      app.toast.find("left series") == std::string::npos);
                check("invariant 1: audit after the series move", audit());
            }
            // UNGROUP: the fence goes, the data stays. This is the operation
            // that must NOT be confused with Close.
            {
                int stacksBefore = (int)app.seqs.size();
                size_t imagesBefore = app.images.size();
                app.toast.clear();
                ungroupSeries(sid);
                fprintf(stderr, "seriesselftest: ungroup -> %d series, %d stack(s), "
                                "%d frame(s), toast \"%s\"\n", (int)app.series.size(),
                        (int)app.seqs.size(), (int)app.images.size(), app.toast.c_str());
                check("ungroup removes the series", seriesById(sid) == nullptr);
                check("...and keeps every stack and frame",
                      (int)app.seqs.size() == stacksBefore &&
                      app.images.size() == imagesBefore);
                check("invariant 1: audit after ungroup", audit());
            }
            // Close series: the canon's Close, which discards the CONTENTS.
            {
                int b2 = app.images.empty() ? 0 : app.images[0]->batchId;
                int sid2 = selftestMakeSeries(b2, "lx");
                App::Series* Q = seriesById(sid2);
                int members = Q ? (int)Q->members.size() : 0;
                int stacksBefore = (int)app.seqs.size();
                app.toast.clear();
                closeSeries(sid2);
                fprintf(stderr, "seriesselftest: close series (%d member(s)) -> %d series, "
                                "%d stack(s) left, toast \"%s\"\n", members,
                        (int)app.series.size(), (int)app.seqs.size(), app.toast.c_str());
                check("Close series discards its stacks",
                      seriesById(sid2) == nullptr &&
                      (int)app.seqs.size() == stacksBefore - members);
                check("invariant 1: audit after Close series", audit());
            }
        }

        // ---- the modal's target dying while the modal is open -----------------
        // pumpSequenceAndQueue runs every frame regardless of the modal, so a
        // session restore or a picker sweep draining mid-edit can move the
        // edited series' stacks into ITS series and prune the emptied one. Save
        // then held a dead id: it ripped the ticked stacks out of the live
        // series that now held them and put them nowhere.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            std::vector<int> ids;
            for (const auto& si : app.seqs) ids.push_back(si.id);
            int bE = app.images.empty() ? 0 : app.images[0]->batchId;
            int A = newSeries(bE, "edited");
            if (App::Series* SA = seriesById(A)) {
                SA->paramName = "illuminance";
                snprintf(SA->unit, sizeof SA->unit, "lx");
            }
            for (size_t i = 0; i < 3 && i < ids.size(); i++)
                addToSeries(A, ids[i], (double)(i * 10));
            openSeriesModal(bE, A);
            // ...and now the resolver's half, under the open modal
            int B = newSeries(bE, "arrived");
            for (size_t i = 0; i < 3 && i < ids.size(); i++)
                addToSeries(B, ids[i], (double)(i * 10));
            pruneEmptySeries();
            check("the edited series really died under the open modal",
                  seriesById(A) == nullptr && seriesById(B) != nullptr);
            int got = seriesModalAccept();
            const App::Series* G = seriesById(got);
            fprintf(stderr, "seriesselftest: Save on a dead editId -> series %d, %d "
                            "series left, %d member(s)\n", got, (int)app.series.size(),
                    G ? (int)G->members.size() : -1);
            check("Save on a dead edit keeps the membership and the typed values",
                  got != 0 && G && G->members.size() == 3);
            check("...and does not leave the stacks in no series at all",
                  seriesOfStack(ids[0]) != nullptr);
            check("invariant 1: audit after the dead-edit Save", audit());
        }

        // ---- a sweep whose batch is closed before it resolves ------------------
        // A sweep is only a note keyed by batchId until every load drains.
        // closeBatch purged the two open queues by batchId and missed this one.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            check("the sweep is pending before the close", app.seriesPending.size() == 1);
            int bc = app.seriesPending.empty() ? 0 : app.seriesPending.front().batchId;
            app.toast.clear();
            closeBatch(bc);
            check("closeBatch takes the batch's pending sweep with it",
                  app.seriesPending.empty());
            loadAll();
            double tb = nowSec();
            while (nowSec() - tb < 1.0) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: sweep batch closed mid-load -> %d series, "
                            "toast \"%s\"\n", (int)app.series.size(), app.toast.c_str());
            check("...so nothing is created and nothing complains afterwards",
                  app.series.empty() &&
                  app.toast.find("could not be matched") == std::string::npos);
        }

        // ---- phase 5: the picker's "open as a sweep" --------------------------
        // Both halves. Ticked: one series, values off the group names, one
        // batch even though batch-per-top-folder was asked for. UNTICKED: the
        // same Open, and NOTHING is created - that half is the whole point.
        {
            closeAll();
            openFolder(g_seriesSelftest);          // local scan: synchronous
            check("phase 5: the picker opened", app.folderPickOpen);
            app.pickSweep = true;
            app.pickBatchMode = 1;                 // ...and gets overridden
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            check("the box is cleared on accept (a sweep is never sticky)",
                  !app.pickSweep);
            loadAll();
            double t5 = nowSec();
            while (nowSec() - t5 < 120.0 && !app.seriesPending.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* W = app.series.empty() ? nullptr : &app.series.front();
            std::vector<int> bids;                 // batches that actually hold data
            for (const auto& d : app.images)
                if (std::find(bids.begin(), bids.end(), d->batchId) == bids.end())
                    bids.push_back(d->batchId);
            bool valuesOk = W != nullptr && !W->members.empty();
            std::string vals;
            if (W)
                for (const auto& m : W->members) {
                    App::SeqInfo* si = seqInfo(m.seqId);
                    double want = si ? extractLevelFromName(si->name) : 0;
                    char b[32];
                    snprintf(b, sizeof b, "%.6g", m.value);
                    vals += (vals.empty() ? "" : ",");
                    vals += std::isfinite(m.value) ? b : "unset";
                    if (!si || !std::isfinite(m.value) || !std::isfinite(want) ||
                        fabs(m.value - want) > 1e-9 * std::max(1.0, fabs(want)))
                        valuesOk = false;
                }
            fprintf(stderr, "seriesselftest: sweep -> %d series '%s' param '%s' unit '%s' "
                            "kind %d, %d member(s) [%s], %d batch(es), %d stack(s)\n",
                    (int)app.series.size(), W ? W->name.c_str() : "",
                    W ? W->paramName.c_str() : "", W ? W->unit : "", W ? W->kind : -1,
                    W ? (int)W->members.size() : -1, vals.c_str(), (int)bids.size(),
                    (int)app.seqs.size());
            check("open as a sweep makes exactly ONE series", app.series.size() == 1);
            check("...over every stack the picker loaded",
                  W && W->members.size() == app.seqs.size() && app.seqs.size() >= 4);
            check("...with the parameter and unit typed in the picker",
                  W && W->paramName == "illuminance" && std::string(W->unit) == "lx" &&
                  W->kind == App::Series::KLinearity);
            check("...named \"<batch> 掃引\"",
                  W && W->name == batchNameOf(W->batchId) + " 掃引");
            check("...each member at the value its NAME gave", valuesOk);
            {   // a sweep is a parameter axis: it comes out in value order, not
                // in the folders' lexicographic order (0,10,160,20,320,40,80)
                bool asc = true;
                for (size_t i = 1; W && i < W->members.size(); i++)
                    if (W->members[i - 1].value > W->members[i].value) asc = false;
                check("...and the members are in VALUE order", W && asc);
            }
            check("a sweep forces ONE batch (series ⊂ batch beats the batch radio)",
                  bids.size() == 1 && W && W->batchId == bids.front());
            check("invariant 1: audit after the sweep", audit());
            // ...and the half that must NOT happen.
            closeAll();
            openFolder(g_seriesSelftest);
            check("phase 5: the picker opened again", app.folderPickOpen);
            check("the sweep box came up UNTICKED", !app.pickSweep);
            pickerAccept();
            loadAll();
            double t6 = nowSec();             // give a late resolve every chance
            while (nowSec() - t6 < 1.0) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: sweep unticked -> %d series, %d stack(s)\n",
                    (int)app.series.size(), (int)app.seqs.size());
            check("unticked: the same Open creates NO series",
                  app.series.empty() && !app.seqs.empty());
            app.pickBatchMode = 0;

            // A second Open before the first sweep has resolved. Resolution
            // waits for every load to drain, which is seconds locally and much
            // longer over the wire, and File > Open Folder is available the
            // whole time: a single pending slot threw the first sweep away -
            // its ticked box, its typed parameter and its unit - in silence.
            closeAll();
            openFolder(g_seriesSelftest);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            check("the first sweep is pending, not applied yet",
                  app.seriesPending.size() == 1);
            openFolder(g_seriesSelftest);       // ...while the first is still loading
            // the picker starts clean on every scan, the sweep boxes included:
            // a unit left over from the last Open would be applied to a folder
            // that has nothing to do with it
            fprintf(stderr, "seriesselftest: second scan's sweep row: box %d, param \"%s\", "
                            "unit \"%s\"\n", app.pickSweep ? 1 : 0, app.pickSweepParam,
                    app.pickSweepUnit);
            check("a new scan resets the sweep box, its parameter and its unit",
                  !app.pickSweep && !app.pickSweepParam[0] && !app.pickSweepUnit[0]);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "exposure");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "ms");
            pickerAccept();
            check("the second does not overwrite it - both are queued",
                  app.seriesPending.size() == 2);
            loadAll();
            double t7 = nowSec();
            while (nowSec() - t7 < 240.0 && !app.seriesPending.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string units;
            for (const auto& s : app.series)
                units += std::string(s.unit) + "x" + std::to_string(s.members.size()) + " ";
            fprintf(stderr, "seriesselftest: two sweeps opened back to back -> %d series "
                            "[%s], %d stack(s)\n", (int)app.series.size(), units.c_str(),
                    (int)app.seqs.size());
            check("a sweep opened while another is loading is NOT thrown away",
                  app.series.size() == 2);
            check("...and neither Open loses the stacks it had queued",
                  app.series.size() == 2 && app.series[0].members.size() == 7 &&
                  app.series[1].members.size() == 7);
            check("...and each keeps the parameter and unit it was given",
                  app.series.size() == 2 && app.series[0].paramName == "illuminance" &&
                  std::string(app.series[0].unit) == "lx" &&
                  app.series[1].paramName == "exposure" &&
                  std::string(app.series[1].unit) == "ms");
            check("invariant 1: audit after two queued sweeps", audit());
        }

        // ---- two members of ONE .npz: the cross-reference lines --------------
        // Every line that names a doc from ELSEWHERE in the session file spoke
        // bare path: `seriesmember`, `cmpslot`, `stackavg`. Two image members of
        // one .npz share a path and are told apart by nothing but src->member
        // (§6.2's identity tuple, the field PR #109 generalised), so on restore
        // the first same-path stack in list order claimed every want: the series
        // member at 0 lx landed on `bright`, the one at 100 lx on `dark`, and
        // both "resolved" - the toast read clean, the fit was anchored on
        // swapped axis values, and the next autosave froze the swap in. That is
        // the "silently misplaced point on the axis the fit is taken against"
        // resolveOnePendingSeries calls worse than a loss.
        //
        // The fixture opens `bright` FIRST so that list order and value order
        // disagree - matching on path alone cannot then be right by accident.
        {
            closeAll();
            std::error_code nec;
            std::filesystem::path ndir =
                std::filesystem::path(pathFromUtf8(g_seriesSelftest)) / "npzmember-scratch";
            std::filesystem::create_directories(ndir, nec);
            std::vector<uint16_t> pxD((size_t)3 * 64), pxB((size_t)3 * 64);
            for (int f = 0; f < 3; f++)
                for (int i = 0; i < 64; i++) {
                    pxD[(size_t)f * 64 + i] = (uint16_t)(100 + f);   // mean 101
                    pxB[(size_t)f * 64 + i] = (uint16_t)(900 + f);   // mean 901
                }
            const std::string npz = (ndir / "sweep.npz").u8string();
            selftestWriteNpz(npz, {
                { "dark",   selftestNpyBytes("<u2", { 3, 8, 8 }, pxD.data(), pxD.size() * 2) },
                { "bright", selftestNpyBytes("<u2", { 3, 8, 8 }, pxB.data(), pxB.size() * 2) },
            });
            std::string e1 = loadNpz(npz, "bright");    // opened first, ON PURPOSE
            std::string e2 = loadNpz(npz, "dark");
            loadAll();
            auto seqOfMember = [](const char* m) {
                for (const auto& d : app.images)
                    if (d->seqId != 0 && d->src->member == m) return d->seqId;
                return 0;
            };
            auto memberOfSeq = [](int sid) {
                std::vector<int> fr = framesOfSeq(sid);
                return fr.empty() ? std::string() : app.images[fr.front()]->src->member;
            };
            int sB = seqOfMember("bright"), sD = seqOfMember("dark");
            check("both .npz members opened, as two stacks of one file",
                  e1.empty() && e2.empty() && sB && sD && sB != sD &&
                  app.images.size() == 6);
            if (sB && sD) {
                int nb = newBatch(uniqueBatchName("npz sweep"));
                moveStackToBatch(sB, nb);
                moveStackToBatch(sD, nb);
                int sid = newSeries(nb, "two members");
                if (App::Series* Z = seriesById(sid)) {
                    Z->paramName = "illuminance";
                    snprintf(Z->unit, sizeof Z->unit, "lx");
                }
                addToSeries(sid, sD, 0.0);        // dark is the 0 lx point
                addToSeries(sid, sB, 100.0);
                // a compare slot pinned on frame 1 of DARK...
                std::vector<int> fD = framesOfSeq(sD);
                app.cmpExtra.clear();
                app.cmpExtra.push_back(app.images[fD[1]]->uid);
                // ...and a frame average of DARK (mean 101, so the picture
                // itself says which member was folded: bright's is 901)
                requestStackAverage(sD);
                loadAll();
                std::string ms = (ndir / "npzmember.vsession").u8string();
                saveSession(ms, true);
                std::string merr = loadSession(ms);
                check("the two-member session reloaded", merr.empty());
                loadAll();
                double tm = nowSec();
                while (nowSec() - tm < 120.0 &&
                       (!app.seriesRestore.empty() || !app.avgRestore.empty() ||
                        !app.pendingAvg.empty())) {
                    pumpSequenceAndQueue();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pumpCompareSlotRestore();
                const App::Series* R = app.series.empty() ? nullptr : &app.series.front();
                std::string at0, at100;
                if (R)
                    for (const auto& m : R->members) {
                        if (m.value == 0.0) at0 = memberOfSeq(m.seqId);
                        if (m.value == 100.0) at100 = memberOfSeq(m.seqId);
                    }
                std::string slotMember;
                int slotFrame = -1;
                for (uint64_t u : app.cmpExtra)
                    for (const auto& d : app.images)
                        if (d->uid == u) { slotMember = d->src->member; slotFrame = d->seqIndex; }
                const ImageDoc* avg = nullptr;
                for (const auto& d : app.images) if (!d->avgOfPath.empty()) avg = d.get();
                double avgMean = 0;
                if (avg) {
                    for (float v : avg->px()) avgMean += v;
                    avgMean /= (double)std::max<size_t>(1, avg->px().size());
                }
                fprintf(stderr, "seriesselftest: npz members: 0 lx -> '%s', 100 lx -> '%s'; "
                                "compare slot on '%s' frame %d; average of '%s' = %.6g\n",
                        at0.c_str(), at100.c_str(), slotMember.c_str(), slotFrame,
                        avg ? memberOfSeq(avg->avgSeqId).c_str() : "-", avgMean);
                check("a series member comes back on the .npz member it was on",
                      at0 == "dark" && at100 == "bright");
                check("a compare slot comes back on ITS member, at its frame",
                      slotMember == "dark" && slotFrame == 1);
                check("a frame average recomputes from the member it folded",
                      avg && fabs(avgMean - 101.0) < 1e-9);
                {   // ---- and the same file WITHOUT the new key ---------------
                    // Every session written before `refmember` existed, and
                    // every one an older viewer writes, is this file minus
                    // those lines. It must open, and open no worse than it does
                    // today: both members resolve, the slot lands, the recipe
                    // recomputes. It cannot be BETTER - path alone cannot tell
                    // the two members apart, which is the whole defect - so
                    // what is asserted is "nothing lost, nothing refused".
                    std::string body;
                    {
                        std::ifstream sf(pathFromUtf8(ms));
                        std::stringstream s2; s2 << sf.rdbuf(); body = s2.str();
                    }
                    int stripped = 0;
                    std::string old;
                    {
                        std::istringstream in(body);
                        std::string l;
                        while (std::getline(in, l)) {
                            if (l.rfind("refmember ", 0) == 0) { stripped++; continue; }
                            old += l;
                            old += "\n";
                        }
                    }
                    const std::string oldPath = (ndir / "npzmember-old.vsession").u8string();
                    { std::ofstream of(pathFromUtf8(oldPath), std::ios::binary); of << old; }
                    closeAll();
                    std::string oerr = loadSession(oldPath);
                    loadAll();
                    double to = nowSec();
                    while (nowSec() - to < 120.0 &&
                           (!app.seriesRestore.empty() || !app.avgRestore.empty() ||
                            !app.pendingAvg.empty())) {
                        pumpSequenceAndQueue();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    pumpCompareSlotRestore();
                    const App::Series* O = app.series.empty() ? nullptr : &app.series.front();
                    int oldAvgs = 0;
                    for (const auto& d : app.images) if (!d->avgOfPath.empty()) oldAvgs++;
                    // cmpExtra is uids and Close All does not clear it (dead
                    // ones are pruned when the letters resolve), so count the
                    // slots that actually point at an open doc
                    int liveSlots = 0;
                    for (uint64_t u : app.cmpExtra)
                        for (const auto& d : app.images) if (d->uid == u) liveSlots++;
                    fprintf(stderr, "seriesselftest: pre-refmember session (%d line(s) "
                                    "stripped): err=\"%s\" %zu series, %d member(s), "
                                    "%d live slot(s), %d average(s), %zu want(s) left\n",
                            stripped, oerr.c_str(), app.series.size(),
                            O ? (int)O->members.size() : -1, liveSlots, oldAvgs,
                            app.cmpSlotRestore.size());
                    check("a session written before refmember still opens",
                          stripped == 4 && oerr.empty() && app.series.size() == 1);
                    check("...losing no member, no slot and no recipe",
                          O && O->members.size() == 2 && liveSlots == 1 &&
                          oldAvgs == 1 && app.cmpSlotRestore.empty());
                }
            }
            closeAll();
            std::filesystem::remove_all(ndir, nec);
        }
        fprintf(stderr, "seriesselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        std::filesystem::remove(std::filesystem::u8path(sess), tec);
        std::filesystem::remove(std::filesystem::u8path(legacy), tec);
        return ok ? 0 : 1;
    }
