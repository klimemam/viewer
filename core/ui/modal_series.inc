// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// → P 段階で core/selftest/util.inc へ
// SELFTEST ONLY - the one place in the program that creates a series without a
// human. The canon forbids auto-creation because a guessed sweep is a lie that
// fits; a headless test has nobody to press Create, so it presses it here, in
// the open: ONE series over every stack of one batch, values from
// extractLevelFromName (exactly the proposal the Create modal puts on screen),
// unit = the same prefill that modal starts from. Called only from --*-selftest
// branches in main(); returns 0 when the batch held no stacks.
static int selftestMakeSeries(int batchId, const char* unitPrefill) {
    if (!batchId) return 0;
    int id = newSeries(batchId, "");
    if (App::Series* S = seriesById(id)) {
        S->paramName = "level";
        snprintf(S->unit, sizeof S->unit, "%s",
                 unitPrefill && *unitPrefill ? unitPrefill : "lx");
    }
    for (const auto& si : app.seqs) {
        if (batchOfStack(si.id) != batchId) continue;
        addToSeries(id, si.id, extractLevelFromName(si.name));
    }
    App::Series* S = seriesById(id);
    if (!S || S->members.empty()) { pruneEmptySeries(); return 0; }
    return id;
}

static const char* SERIES_KINDS = "linearity\0PTC\0temperature\0other\0";

// What one row of the create/edit table means as a number. An untouched row of
// an existing member is its stored double, not a re-read of the six digits the
// box happens to be showing; anything else is the text, and text the program
// cannot read is UNSET, never 0 (parseSeriesValue).
static double seriesEditRowValue(const App::SeriesEdit::Row& r) {
    if (r.haveOrig && !r.touched) return r.orig;
    return parseSeriesValue(r.value);
}

// Fill the create/edit modal. Creating: every stack of the batch, all ticked,
// each with extractLevelFromName's proposal ALREADY in its value box and marked
// as a proposal. Editing: the members first, in their order, with their own
// values, then the batch's non-members, unticked.
//
// The proposal is the whole point of the old "Auto levels" button, moved to
// where it belongs: the user sees the numbers and the text they were read out
// of BEFORE pressing Create, so pressing it is a confirmation. Nothing here
// runs by itself - series are never auto-created (docs/terminology.md).
static void openSeriesModal(int batchId, int editId) {
    App::SeriesEdit& E = app.seriesEdit;
    E = App::SeriesEdit{};
    E.editId = editId;
    E.batchId = batchId;
    const App::Series* S = editId ? seriesById(editId) : nullptr;
    if (S) {
        E.batchId = S->batchId;
        snprintf(E.name, sizeof E.name, "%s", S->name.c_str());
        snprintf(E.param, sizeof E.param, "%s", S->paramName.c_str());
        snprintf(E.unit, sizeof E.unit, "%s", S->unit);
        E.kind = S->kind;
    } else {
        snprintf(E.name, sizeof E.name, "%s 掃引", batchNameOf(batchId).c_str());
        // the prefill, and only that: an empty box stays empty (prefs linunit)
        snprintf(E.unit, sizeof E.unit, "%s", app.lin.unit);
    }
    auto addRow = [&](int seqId, bool checked, double value, bool haveValue) {
        App::SeriesEdit::Row r;
        r.seqId = seqId;
        r.check = checked;
        App::SeqInfo* si = seqInfo(seqId);
        r.name = si ? si->name : std::string();
        double guess = extractLevelFromName(r.name, &r.from);
        if (haveValue) {
            // An EXISTING member. Its value is the user's - INCLUDING the
            // decision not to set one. A guess proposed over that is a proposal
            // on a row nobody came here to look at (Edit... is mostly opened to
            // rename), and Save would commit it: "value unset" becomes a fit
            // point with zero keystrokes. Creating is where a proposal belongs
            // (docs/series-plan.md §2: 見た上で Create を押させる); here it is
            // offered in the column beside the box instead of inside it.
            r.orig = value;
            r.haveOrig = true;
            // LOSSLESSLY. From the first keystroke in this box the TEXT is the
            // value (touched), so whatever is prefilled becomes authoritative
            // the moment the box is touched at all - and "click in, type a
            // digit, delete it again, Save" then committed the six digits the
            // box was showing: 1234567.89 saved as 1234570, the exact rounding
            // the orig/touched split exists to prevent, on the axis the fit is
            // taken against. fmtExact round-trips and fits (%.17g is ~24 of 32).
            if (std::isfinite(value))
                snprintf(r.value, sizeof r.value, "%s", fmtExact(value).c_str());
            else r.guess = guess;
        } else if (std::isfinite(guess)) {
            snprintf(r.value, sizeof r.value, "%.6g", guess);
            r.suggested = true;           // dim + "(guess)" until it is touched
        }
        E.rows.push_back(std::move(r));
    };
    if (S)
        for (const auto& m : S->members) addRow(m.seqId, true, m.value, true);
    for (const auto& si : app.seqs) {
        if (batchOfStack(si.id) != E.batchId) continue;
        bool already = false;
        for (const auto& r : E.rows) if (r.seqId == si.id) already = true;
        if (!already) addRow(si.id, S == nullptr, 0, false);
    }
    E.open = true;
}

// The modal's Save / Create button, as a plain function. Every other command in
// this layer already is one (that is what let the selftest drive them); this one
// was inline in the draw call, which is exactly why "open Edit... and press Save
// without touching anything" had to be found by hand instead of by a test.
// Returns the series' id, or 0 when nothing was accepted.
static int seriesModalAccept() {
    App::SeriesEdit& E = app.seriesEdit;
    // Build the member list in TABLE ORDER: the row order is the series
    // order, which is why the arrows exist.
    std::vector<App::Series::Member> ms;
    for (const auto& r : E.rows) {
        if (!r.check || !seqInfo(r.seqId)) continue;
        if (batchOfStack(r.seqId) != E.batchId) continue;   // strict containment
        App::Series::Member m;
        m.seqId = r.seqId;
        m.value = seriesEditRowValue(r);
        m.include = true;
        if (const App::Series* old = seriesById(E.editId))   // keep the fit flag
            for (const auto& om : old->members)
                if (om.seqId == r.seqId) m.include = om.include;
        ms.push_back(m);
    }
    if (ms.empty()) return 0;
    // E.editId was checked when the modal OPENED, never here. Meanwhile
    // pumpSequenceAndQueue runs every frame regardless of the modal (it even
    // forces frames while one is up), so resolveSeriesRestore or a picker sweep
    // draining mid-edit can move the edited series' stacks into ITS series and
    // pruneEmptySeries deletes what that empties. Keeping the dead id then made
    // Save destructive: the loop below ripped every ticked stack out of whatever
    // live series now held it, seriesById(id) found nothing to put them back
    // into, the rebuilt member list was dropped on the floor, and the return
    // value saying so was discarded by the caller - no toast, typed values gone.
    // A dead edit becomes a CREATE: the rip is followed by a real assignment, so
    // the membership and the values the user typed survive.
    int id = (E.editId && seriesById(E.editId)) ? E.editId
                                                : newSeries(E.batchId, E.name);
    // a stack belongs to AT MOST ONE series: taking one in takes it out of
    // wherever it was
    for (const auto& m : ms) {
        App::Series* other = seriesOfStack(m.seqId);
        if (other && other->id != id) removeFromSeries(m.seqId);
    }
    if (App::Series* S = seriesById(id)) {
        S->name = E.name[0] ? E.name : batchNameOf(E.batchId) + " 掃引";
        S->paramName = E.param;
        snprintf(S->unit, sizeof S->unit, "%s", E.unit);
        S->kind = std::clamp(E.kind, 0, 3);
        S->members = std::move(ms);
    }
    // the next new series starts from the unit this one ended with
    if (E.unit[0]) snprintf(app.lin.unit, sizeof app.lin.unit, "%s", E.unit);
    app.prefsDirty = true;
    pruneEmptySeries();
    if (seriesById(id)) app.curSeriesId = id;
    // Whatever was on screen measured the series as it WAS. The panel reads its
    // labels live off the series, so a fit left standing here does not merely go
    // stale - it gets relabelled with the new unit and the new parameter name.
    linInvalidate();
    app.imagesRev++;
    return seriesById(id) ? id : 0;
}

static void drawSeriesModal() {
    App::SeriesEdit& E = app.seriesEdit;
    // "###" fixes the popup's identity while the title changes: ImGui hashes
    // only what follows it, so create and edit really are one modal.
    const char* POPUP = "Series###seriesmodal";
    char title[64];
    snprintf(title, sizeof title, "%s###seriesmodal",
             E.editId ? "Edit series" : "Create series");
    if (E.open && !ImGui::IsPopupOpen(POPUP)) ImGui::OpenPopup(POPUP);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640 * app.uiScale, 480 * app.uiScale), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_None)) return;
    if (!E.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    ImGui::Text("batch: %s", batchNameOf(E.batchId).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(a series lives in exactly one batch)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::InputText("name", E.name, sizeof E.name);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("parameter", E.param, sizeof E.param);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("what was swept: illuminance, exposure, temperature ...");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputText("unit", E.unit, sizeof E.unit);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("lx, ms, lx*s, photons/px ... printed on every axis and\n"
                          "column. LEAVE IT EMPTY if you do not know it yet: an\n"
                          "unset unit means no fit, which is the honest answer.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::Combo("kind", &E.kind, SERIES_KINDS);
    if (!E.unit[0])
        ImGui::TextDisabled("unit not set - members and means still show, the fit will not.");

    ImGui::SeparatorText("members  (value = the parameter this stack was captured at)");
    if (ImGui::SmallButton("Sort by value")) {
        std::stable_sort(E.rows.begin(), E.rows.end(),
                         [](const App::SeriesEdit::Row& a, const App::SeriesEdit::Row& b) {
                             double av = seriesEditRowValue(a), bv = seriesEditRowValue(b);
                             if (!std::isfinite(av)) av = HUGE_VAL;   // unset goes last
                             if (!std::isfinite(bv)) bv = HUGE_VAL;
                             return av < bv;
                         });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) for (auto& r : E.rows) r.check = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) for (auto& r : E.rows) r.check = false;
    ImGui::SameLine();
    int nChecked = 0, nValued = 0;
    for (const auto& r : E.rows) {
        if (!r.check) continue;
        nChecked++;
        // what the row would BE, not whether the box has characters in it: a box
        // holding "-" or "1e" is an unset member, and the count has to say so
        if (std::isfinite(seriesEditRowValue(r))) nValued++;
    }
    ImGui::TextDisabled("| %d of %d stacks, %d with a value", nChecked,
                        (int)E.rows.size(), nValued);

    const ImGuiTableFlags TF = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    int moveFrom = -1, moveTo = -1;
    if (ImGui::BeginTable("sermembers", 5, TF, ImVec2(0, std::max(tableH, 80.0f * app.uiScale)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("stack", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("read from", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("order", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFrameHeight() * 2.4f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)E.rows.size(); i++) {
            App::SeriesEdit::Row& r = E.rows[i];
            ImGui::PushID(r.seqId);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Checkbox("##m", &r.check);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (r.suggested)      // a proposal reads as one until it is confirmed
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::InputText("##v", r.value, sizeof r.value,
                                 ImGuiInputTextFlags_CharsScientific)) {
                r.suggested = false;
                r.touched = true;         // from here the TEXT is the value
            }
            if (r.suggested) ImGui::PopStyleColor();
            if (r.value[0] && !std::isfinite(seriesEditRowValue(r)) &&
                ImGui::IsItemHovered())
                ImGui::SetTooltip("\"%s\" is not a number - this member saves as UNSET,\n"
                                  "which is left out of the fit. It is never read as 0.",
                                  r.value);
            ImGui::TableNextColumn();
            if (r.suggested) ImGui::TextDisabled("(guess) %s", r.from.c_str());
            else if (!r.value[0] && std::isfinite(r.guess)) {
                // offered, NOT filled in: this member's value is deliberately
                // unset and only a human may change that
                ImGui::TextDisabled("guess: %.6g", r.guess);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("\"%s\" suggests %.6g. The box is empty because this\n"
                                      "member has NO value set - type it in to use the\n"
                                      "suggestion; saving as it is keeps it unset.",
                                      r.from.c_str(), r.guess);
            }
            else if (r.from.empty() && !r.value[0]) ImGui::TextDisabled("-");
            else ImGui::TextDisabled("%s", r.from.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) { moveFrom = i; moveTo = i - 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i == (int)E.rows.size() - 1);
            if (ImGui::SmallButton("v")) { moveFrom = i; moveTo = i + 1; }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (moveFrom >= 0) std::swap(E.rows[moveFrom], E.rows[moveTo]);

    ImGui::Separator();
    bool canAccept = nChecked > 0;
    ImGui::BeginDisabled(!canAccept);
    if (ImGui::Button(E.editId ? "Save" : "Create", ImVec2(140 * app.uiScale, 0))) {
        seriesModalAccept();
        E.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * app.uiScale, 0))) {
        E.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!canAccept) {
        ImGui::SameLine();
        ImGui::TextDisabled("tick at least one stack");
    }
    ImGui::EndPopup();
}

