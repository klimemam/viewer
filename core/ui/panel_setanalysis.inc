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
    if (ImGui::BeginTable("v", R.kind == SA_PRNU ? 6 : 5,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("plane");
        ImGui::TableSetupColumn(colName.c_str());
        if (R.kind == SA_PRNU) ImGui::TableSetupColumn("sigma_d [DN]");
        ImGui::TableSetupColumn(R.kind == SA_PRNU ? "mu [DN]" : "mean n");
        ImGui::TableSetupColumn("fpn_corr [DN]");
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
            ImGui::TableNextColumn(); ImGui::Text("%zu", R.samples[p]);
        }
        ImGui::EndTable();
    }
    // What came off the number, said beside it. A corrected value that does not
    // state its correction cannot be checked - the same reason PR #127 put
    // fpn_corr next to sigma_fpn rather than in a release note.
    ImGui::TextDisabled("fpn_corr = sqrt(C), the temporal residual taken OFF this value. "
                        "Uncorrected it would read %.10g %s.",
                        R.uncorr[0], kSetAnalyzers[R.kind].unit);
    ImGui::Spacing();
    ImGui::PopID();
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
        for (int k = 0; k < SA_COUNT; k++) g_setPanel.r[k] = runSetAnalyzer(*Sp, k);
        g_setPanel.cachedId = Sp->id;
        g_setPanel.key = key;
    }
    for (int k = 0; k < SA_COUNT; k++) setPanelRow(g_setPanel.r[k]);
    // ---- provenance: what a number pasted into a report has to carry ---------
    ImGui::Separator();
    if (ImGui::TreeNodeEx("provenance", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("[local]   set \"%s\" in batch \"%s\"", Sp->name.c_str(),
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
        // measured over, and WHICH estimator wrote them
        ImGui::TextDisabled("  cutoff: %s", kSetCutoffRaw);
        for (int k = 0; k < SA_COUNT; k++)
            ImGui::TextDisabled("  %s: %s", kSetAnalyzers[k].key, kSetAnalyzers[k].form);
        ImGui::TextWrapped("These are tool-defined quantities. Nothing here has been "
                           "checked against the text of any standard, so no standard is "
                           "named on this screen.");
        ImGui::TreePop();
    }
}
