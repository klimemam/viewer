// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// The AnalysisSet layer's home (docs/analysis-layers.md §2, "住処" = set の操作):
// the SetAnalyzers of docs/flat-field-stats.md (b) over ONE bound set. The
// arithmetic is core/app/setanalysis.inc; this file only reads it out.
//
// It follows its sibling panels rather than inventing a shape:
//   - Series Analysis picks its subject with a combo and says so when there is
//     nothing to pick;
//   - Temporal recomputes on a KEY (subject + stackRev + frame count) instead of
//     every frame, and never on its own initiative beyond that;
//   - the refusal of an analysis whose signature is unmet is a greyed row with
//     one line of reason, never a hidden row (measure-ux.md).
// What is NOT borrowed is a detrend option: docs/flat-field-stats.md 判断6 puts
// detrend in the PreProcessor stage and nowhere else, so there is no switch here
// that could change what these numbers mean.
struct SetPanelCache {
    int setId = 0;                // which set the panel shows
    int cachedId = 0;             // ...and which one r[] was computed for
    uint64_t key = 0;
    SetResult r[SA_COUNT];
    // The separation fit is NOT on that key. It folds every level of a sweep,
    // which is a measurement action over potentially hundreds of frames, so it
    // runs when it is asked to - the Series Analysis panel's Compute, for the
    // Series Analysis panel's reason. What the key still does is say when the
    // printed fit stopped describing what is on screen: a fit that outlives its
    // own inputs is worse than no fit, so it goes STALE and is dropped rather
    // than left standing under a membership it never saw (linFitStale's rule).
    SepFit sep;
    int sepSetId = 0;             // 0 = nothing computed yet
    uint64_t sepKey = 0;
    int tablePlane = 0;           // which plane the per-level table shows
};
static SetPanelCache g_setPanel;

// The subject's revision: which stacks are bound, how many frames each has
// landed, and each one's stackRev (a reload swaps pixels under an unchanged
// count - docs/reference-design.md §3.2). Bindings themselves ride along
// through app.imagesRev, which every model mutation bumps.
static uint64_t setPanelKey(const App::ASet& S) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; }
    };
    mix((uint64_t)app.imagesRev);
    for (const auto& r : S.roles) {
        mix((uint64_t)r.seqId);
        mix((uint64_t)r.frameUid);
        mix((uint64_t)r.seriesId);
        if (r.seqId != 0) {
            const App::SeqInfo* si = seqInfo(r.seqId);
            mix((uint64_t)(si ? si->stackRev : -1));
            mix((uint64_t)framesOfSeq(r.seqId).size());
        }
        // A Series() role's revision is its MEMBERSHIP: the include switch and
        // the fold combo are written straight onto the member by the Series
        // Analysis panel, which stales its own fit and bumps nothing global -
        // so a key built only from app.imagesRev would let this panel print a
        // fit over members the user had just taken out.
        if (r.seriesId != 0) {
            const App::Series* Ser = seriesById(r.seriesId);
            if (!Ser) { mix(~0ull); continue; }
            mix((uint64_t)Ser->members.size());
            for (const auto& m : Ser->members) {
                mix((uint64_t)m.seqId);
                mix(m.frameUid);
                mix((uint64_t)(m.include ? 1 : 0));
                mix((uint64_t)m.fold);
                uint64_t vb;                       // the VALUE, bit for bit:
                memcpy(&vb, &m.value, sizeof vb);  // unset (NaN) is a state too
                mix(vb);
                if (m.seqId != 0) {
                    const App::SeqInfo* si = seqInfo(m.seqId);
                    mix((uint64_t)(si ? si->stackRev : -1));
                    mix((uint64_t)framesOfSeq(m.seqId).size());
                }
            }
        }
    }
    return h;
}

static void setPanelRow(const SetResult& R) {
    const SetAnalyzerDef& D = kSetAnalyzers[R.kind];
    ImGui::PushID(D.key);
    if (!R.ok) {
        // Unmet signature: shown, named, and refused out loud. The famous name
        // is on the row because the row IS the role table's row - what it does
        // not have is a number (docs/analysis-layers.md §4's refusal rule).
        ImGui::TextDisabled("%s", setResultTitle(R).c_str());
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::TextWrapped("%s", R.refusal.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::PopID();
        return;
    }
    ImGui::TextUnformatted(setResultTitle(R).c_str());
    // The value column's header comes off the SAME table the title does. Not a
    // convenience: a second literal is exactly how the ROIs column and its own
    // tooltip drifted apart for eleven weeks, and the scan in
    // --setanalysis-selftest would fail this file for writing one.
    const std::string colName = std::string(D.famous) + " [" + D.unit + "]";
    if (ImGui::BeginTable("v", R.kind == SA_PRNU ? 8 : 7,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("plane");
        ImGui::TableSetupColumn(colName.c_str());
        if (R.kind == SA_PRNU) ImGui::TableSetupColumn("sigma_d [DN]");
        ImGui::TableSetupColumn(R.kind == SA_PRNU ? "mu [DN]" : "mean n");
        ImGui::TableSetupColumn("fpn_corr [DN]");
        // 判断7's other half, and the reason its "default = raw" is not a
        // silent choice: the shading this value was measured THROUGH stands
        // next to it, whether or not anything was removed. The reader decides
        // whether raw was right, every time, from the same table.
        ImGui::TableSetupColumn("shading [%]");
        ImGui::TableSetupColumn("shading p-p [DN]");
        ImGui::TableSetupColumn("samples");
        ImGui::TableHeadersRow();
        for (int p = 0; p < R.nPl; p++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(R.nPl == 4 ? CFA_CH_NAMES[p] : "all");
            ImGui::TableNextColumn();
            if (R.clamped[p]) {
                // never a bare 0: a clamp means "this stack cannot see a fixed
                // pattern under its own temporal floor", not "there is none"
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
                ImGui::Text("%.10g  (clamped)", R.value[p]);
                ImGui::PopStyleColor();
            } else {
                ImGui::Text("%.10g", R.value[p]);
            }
            if (R.kind == SA_PRNU) {
                ImGui::TableNextColumn(); ImGui::Text("%.10g", R.sigmaD[p]);
                ImGui::TableNextColumn(); ImGui::Text("%.10g", R.mu[p]);
            } else {
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn(); ImGui::Text("%.10g", R.corrDn[p]);
            ImGui::TableNextColumn();
            // never a ratio without a level: a dark has no centre level to be
            // a percentage OF, and the p-p beside it is the figure that always
            // exists (docs/flat-field-stats.md (a)'s "低周波成分の p-p / 中心比")
            if (R.shadePctOk[p]) ImGui::Text("%.4g", R.shadePct[p]);
            else                 ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.4g", R.shadePp[p]);
            ImGui::TableNextColumn(); ImGui::Text("%zu", R.samples[p]);
        }
        ImGui::EndTable();
    }
    // Printed, not tooltipped: the figure is only load-bearing if the reader
    // knows what band it is and that it did not move with the option.
    ImGui::TextDisabled("shading = p-p of a degree-2 surface fitted to the picture this row "
                        "measured, per plane, and\nthe same degree whether or not shading was "
                        "removed. Removing it is a PRODUCT (Files > a stack >\nDetrend), never "
                        "a switch here - a switch would make this column name a different "
                        "quantity.");
    // What came off the number, said beside it. A corrected value that does not
    // state its correction cannot be checked - the same reason PR #127 put
    // fpn_corr next to sigma_fpn rather than in a release note.
    ImGui::TextDisabled("fpn_corr = sqrt(C), the temporal residual taken OFF this value. "
                        "Uncorrected it would read %.10g %s.",
                        R.uncorr[0], kSetAnalyzers[R.kind].unit);
    ImGui::Spacing();
    ImGui::PopID();
}

// ---- the separation fit's section ------------------------------------------
// It borrows the Series Analysis panel's idiom on purpose - Compute, a member
// table with the per-point residual beside the measurement, a fit table per
// plane, and a residual plot under it - because a second fit UI would be a
// second thing to learn for the same act. What it does NOT borrow is the
// AXES, and that is deliberate too: the two fits are different quantities on
// different layers and the labels are what say so.
//
//   Series Analysis   series layer, General:  <param> [unit]  ->  ROI mean [DN]
//   here              set layer, SetAnalyzer: mu^2 [DN^2]     ->  sigma_fpn^2 [DN^2]
//
// A reader who cannot see the panel title can still read which one this is off
// either axis, off the section caption (which states the model), and off the
// tag on every row (which names the sweep and the number of levels).
static void setPanelSeparation(const App::ASet& S, uint64_t key) {
    char cap[160];
    snprintf(cap, sizeof cap, "separation fit  (%s, unweighted OLS on mu^2)", kSepModel);
    ImGui::SeparatorText(cap);
    const bool have = g_setPanel.sepSetId == S.id;
    const bool stale = have && g_setPanel.sepKey != key;
    if (ImGui::Button("Compute##sep")) {
        g_setPanel.sep = runSeparationFit(S);
        g_setPanel.sepSetId = S.id;
        g_setPanel.sepKey = key;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Folds every level of the sweep: a measurement action over\n"
                          "potentially hundreds of frames, not something a panel does\n"
                          "on its own while you click around (the Series Analysis rule).");
    ImGui::SameLine();
    ImGui::TextDisabled("| a sweep separates what scales with the level from what does not - "
                        "one level cannot");
    if (!have) {
        ImGui::TextDisabled("press Compute to fit this set's sweep.");
        return;
    }
    if (stale) {
        // the membership, the values or the pixels moved under the printed fit
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "the sweep changed since this fit ran - press Compute. "
                           "(The numbers are not shown against a set of levels they "
                           "were not measured over.)");
        return;
    }
    const SepFit& F = g_setPanel.sep;
    ImGui::TextDisabled("%s", setResultTitle(F.dsnu).c_str());
    ImGui::TextDisabled("%s", setResultTitle(F.prnu).c_str());
    if (!F.ok) {
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::TextWrapped("%s", F.refusal.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
        return;
    }
    // ---- the answers, per plane. BOTH column headers are built from the same
    // table the titles are, for the reason --setanalysis-selftest enforces.
    const std::string dCol = std::string(kSetAnalyzers[SA_SEP_DSNU].famous) + " [" +
                             kSetAnalyzers[SA_SEP_DSNU].unit + "]";
    const std::string pCol = std::string(kSetAnalyzers[SA_SEP_PRNU].famous) + " [" +
                             kSetAnalyzers[SA_SEP_PRNU].unit + "]";
    if (ImGui::BeginTable("sepfit", 7,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("plane");
        ImGui::TableSetupColumn(dCol.c_str());
        ImGui::TableSetupColumn(pCol.c_str());
        ImGui::TableSetupColumn("levels");
        ImGui::TableSetupColumn("R^2");
        ImGui::TableSetupColumn("RMS resid [DN^2]");
        ImGui::TableSetupColumn("intercept / slope");
        ImGui::TableHeadersRow();
        for (int p = 0; p < F.nPl; p++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(F.nPl == 4 ? CFA_CH_NAMES[p] : "all");
            if (!F.planeFit[p]) {
                for (int c = 0; c < 5; c++) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F.planeWhy[p].c_str());
                continue;
            }
            for (int i = 0; i < 2; i++) {
                const SetResult& R = i ? F.prnu : F.dsnu;
                ImGui::TableNextColumn();
                if (R.clamped[p]) {
                    // a coefficient that came out negative is a statement about
                    // this SWEEP, never about the sensor
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
                    ImGui::Text("%.10g  (clamped)", R.value[p]);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("the fitted %s came out below 0 (%.6g) - the "
                                          "levels\ncannot see this term over their own "
                                          "scatter. Clamped\nat 0, and said so: it is not "
                                          "a measurement of zero.",
                                          i ? "slope" : "intercept",
                                          i ? F.slope[p] : F.inter[p]);
                } else {
                    ImGui::Text("%.10g", R.value[p]);
                }
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", F.M);
            ImGui::TableNextColumn();
            if (std::isfinite(F.r2[p])) ImGui::Text("%.5f", F.r2[p]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.6g", F.rms[p]);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.10g / %.10g", F.inter[p], F.slope[p]);
        }
        ImGui::EndTable();
    }
    // What the -C did to THIS fit, beside it. The failure mode the spec
    // measured is not subtle and it is not symmetric: the omitted correction is
    // a term in mu that the model cannot represent, so it lands in the
    // intercept and the additive term alone reads high.
    ImGui::TextDisabled("with the -C skipped at every level this fit would read %s %.10g %s / "
                        "%s %.10g %s - the correction's absence goes into the INTERCEPT.",
                        kSetAnalyzers[SA_SEP_DSNU].famous, F.dsnu.uncorr[0],
                        kSetAnalyzers[SA_SEP_DSNU].unit,
                        kSetAnalyzers[SA_SEP_PRNU].famous, F.prnu.uncorr[0],
                        kSetAnalyzers[SA_SEP_PRNU].unit);

    // ---- the levels, and the residual beside each one ----------------------
    if (F.nPl > 1) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::Combo("table plane", &g_setPanel.tablePlane, "R\0Gr\0Gb\0B\0");
        g_setPanel.tablePlane = std::clamp(g_setPanel.tablePlane, 0, F.nPl - 1);
    }
    const int tp = std::clamp(g_setPanel.tablePlane, 0, F.nPl - 1);
    char valHdr[64];
    snprintf(valHdr, sizeof valHdr, "value [%s]", F.unit.empty() ? "unit not set" : F.unit.c_str());
    if (ImGui::BeginTable("seplev", 8,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("level");
        ImGui::TableSetupColumn(valHdr);
        ImGui::TableSetupColumn("frames");
        ImGui::TableSetupColumn("mu [DN]");
        ImGui::TableSetupColumn("sigma_fpn [DN]");
        ImGui::TableSetupColumn("sigma_fpn^2 [DN^2]");
        ImGui::TableSetupColumn("fit [DN^2]");
        ImGui::TableSetupColumn("residual [DN^2]");
        ImGui::TableHeadersRow();
        for (const auto& L : F.levels) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(L.member.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.6g", L.value);
            ImGui::TableNextColumn();
            if (L.N > L.n) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%d/%d", L.n, L.N);
            else ImGui::Text("%d", L.n);
            ImGui::TableNextColumn(); ImGui::Text("%.6g", L.mu[tp]);
            ImGui::TableNextColumn();
            if (L.clamped[tp]) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
                ImGui::Text("%.6g (clamped)", sqrt(std::max(0.0, L.fpnVar[tp])));
                ImGui::PopStyleColor();
            } else {
                ImGui::Text("%.6g", sqrt(std::max(0.0, L.fpnVar[tp])));
            }
            ImGui::TableNextColumn(); ImGui::Text("%.6g", L.fpnVar[tp]);
            ImGui::TableNextColumn();
            if (F.planeFit[tp]) ImGui::Text("%.6g", L.fit[tp]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (F.planeFit[tp]) ImGui::Text("%.6g", L.resid[tp]);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    // The census, always - even when nothing was excluded. "4 of 4 used" and a
    // silent table are not the same statement.
    ImGui::TextDisabled("%s", sepLevelCensus(F).c_str());
    for (const auto& sk : F.skipped)
        ImGui::TextDisabled("  excluded \"%s\": %s", sk.member.c_str(), sk.why.c_str());

    // ---- the residual plot: the model check, on the fit's own axis ---------
    if (F.planeFit[tp] && F.levels.size() >= 3) {
        double x0 = DBL_MAX, x1 = -DBL_MAX, e = 0;
        for (const auto& L : F.levels) {
            const double x = L.mu[tp] * L.mu[tp];
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            e = std::max(e, fabs(L.resid[tp]));
        }
        const float lim = (float)std::max(e * 1.25, 1e-9);
        float h = std::max(90.0f * app.uiScale,
                           std::min(ImGui::GetContentRegionAvail().y * 0.5f,
                                    ImGui::GetFontSize() * 12));
        PlotRect pr = beginPlot("mu^2 [DN^2]", "residual [DN^2]",
                                (float)x0, (float)x1, -lim, lim, false, false, h);
        if (pr.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            dl->AddLine(pr.at((float)x0, 0), pr.at((float)x1, 0), IM_COL32(140, 150, 160, 160));
            for (const auto& L : F.levels)
                dl->AddCircleFilled(pr.at((float)(L.mu[tp] * L.mu[tp]), (float)L.resid[tp]),
                                    3.5f, LIN_COLS[tp]);
            dl->PopClipRect();
        }
    }
    ImGui::TextWrapped("%s", F.note.c_str());
    ImGui::TextDisabled("%s", F.sweepWarn.c_str());
    // ...and the one sentence that keeps this fit and the Series Analysis panel's
    // apart for a reader who arrived at a screenshot.
    ImGui::TextDisabled("Not the Series Analysis fit: that one is on the SERIES layer and fits "
                        "the RESPONSE (member mean against the swept value, DN per unit). This "
                        "one is a SetAnalyzer over a bound set and fits the FIXED PATTERN "
                        "against level - the axes say which is which.");
}

static void drawPanelSetAnalysis() {
    if (app.analysisSets.empty()) {
        ImGui::TextWrapped("No analysis set is open. These measurements need a DARK, and "
                           "which data plays that part is DECLARED - it is never guessed "
                           "from a folder name or from level 0.");
        ImGui::Spacing();
        ImGui::TextDisabled("A set binds roles to data. Today they are made by a reader");
        ImGui::TextDisabled("that returns AnalysisSet(...) - docs/reader-analysisset.md.");
        return;
    }
    if (!aSetById(g_setPanel.setId)) g_setPanel.setId = app.analysisSets.front().id;
    // The Files selection leads, as it does for every other panel (§3.4): click
    // a set node and this panel is about that set.
    if (g_aSetSelId != 0 && aSetById(g_aSetSelId)) g_setPanel.setId = g_aSetSelId;
    App::ASet* Sp = aSetById(g_setPanel.setId);
    if (!Sp) return;
    {
        char label[384];
        snprintf(label, sizeof label, "%s / %s", batchNameOf(Sp->batchId).c_str(),
                 Sp->name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
        if (ImGui::BeginCombo("##set", label)) {
            for (const auto& s : app.analysisSets) {
                char it[448];
                snprintf(it, sizeof it, "%s / %s##set%d", batchNameOf(s.batchId).c_str(),
                         s.name.c_str(), s.id);
                if (ImGui::Selectable(it, s.id == g_setPanel.setId)) {
                    g_setPanel.setId = s.id;
                    g_aSetSelId = s.id;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", aSetRowText(*Sp).c_str());
    ImGui::Separator();
    const uint64_t key = setPanelKey(*Sp);
    if (g_setPanel.cachedId != Sp->id || key != g_setPanel.key) {
        for (int k = 0; k < kSetDirectCount; k++) g_setPanel.r[k] = runSetAnalyzer(*Sp, k);
        g_setPanel.cachedId = Sp->id;
        g_setPanel.key = key;
    }
    for (int k = 0; k < kSetDirectCount; k++) setPanelRow(g_setPanel.r[k]);
    setPanelSeparation(*Sp, key);
    // ---- provenance: what a number pasted into a report has to carry ---------
    ImGui::Separator();
    if (ImGui::TreeNodeEx("provenance", ImGuiTreeNodeFlags_DefaultOpen)) {
        // WHO computed it. Every row this panel can produce today is folded
        // here, so this reads "[local]" - but the field is the result's own
        // (SetResult::computedBy) rather than a literal, because a row folded
        // on a peer carries the peer's answer to the same question and a panel
        // that printed "[local]" over it would be citing the wrong machine.
        // Routing a run to a peer from this panel is 残課題【Fable】.
        std::string who;
        for (int k = 0; k < SA_COUNT && who.empty(); k++)
            if (g_setPanel.r[k].ok) who = g_setPanel.r[k].computedBy;
        ImGui::TextDisabled("%s   set \"%s\" in batch \"%s\"",
                            who.empty() ? "[local]" : who.c_str(), Sp->name.c_str(),
                            batchNameOf(Sp->batchId).c_str());
        for (const auto& r : Sp->roles) {
            if (!r.bound()) {
                ImGui::TextDisabled("  %s", aSetRoleText(r).c_str());
                continue;
            }
            std::string extra;
            if (r.seqId != 0) {
                std::vector<int> fr = framesOfSeq(r.seqId);
                if (!fr.empty())
                    extra = "  <- " + dispPath(app.images[fr.front()]->src->path);
            }
            ImGui::TextDisabled("  %s%s", aSetRoleText(r).c_str(), extra.c_str());
        }
        // the two required declarations: the spatial band the numbers were
        // measured over, and WHICH estimator wrote them. The cutoff is now
        // per RESULT, because it is a fact about that result's inputs - a role
        // bound to a detrend product carries its method and window here
        // without anything on this panel having to ask.
        bool anyOk = false;
        for (int k = 0; k < SA_COUNT; k++)
            if (g_setPanel.r[k].ok) {
                anyOk = true;
                ImGui::TextDisabled("  cutoff (%s): %s", kSetAnalyzers[k].key,
                                    g_setPanel.r[k].cutoff.c_str());
            }
        if (!anyOk) ImGui::TextDisabled("  cutoff: %s", kSetCutoffRaw.c_str());
        for (int k = 0; k < SA_COUNT; k++)
            ImGui::TextDisabled("  %s: %s", kSetAnalyzers[k].key, kSetAnalyzers[k].form);
        ImGui::TextWrapped("These are tool-defined quantities. Nothing here has been "
                           "checked against the text of any standard, so no standard is "
                           "named on this screen.");
        ImGui::TreePop();
    }
}
