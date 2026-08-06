// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
static void drawPanelLinearity() {
    App::LinState& L = app.lin;
    // ---- which series? Nothing here creates one on its own -------------------
    if (app.series.empty()) {
        ImGui::TextWrapped("No series yet. Linearity is measured over a SERIES: the stacks "
                           "of one swept parameter, each with its value, and the parameter's "
                           "name and unit belong to that series - not to the application.");
        ImGui::Spacing();
        ImGui::TextDisabled("A sweep is never guessed from the folder tree. One wrong");
        ImGui::TextDisabled("guess would quietly fit data that was never a sweep.");
        ImGui::Spacing();
        int b = cur() ? cur()->batchId : 0;
        ImGui::BeginDisabled(b == 0);
        if (ImGui::Button("Create a series from this batch's stacks..."))
            openSeriesModal(b, 0);
        ImGui::EndDisabled();
        if (b == 0) ImGui::TextDisabled("(nothing is open yet)");
        else ImGui::TextDisabled("batch: %s", batchNameOf(b).c_str());
        return;
    }
    if (!seriesById(app.curSeriesId)) app.curSeriesId = app.series.front().id;
    App::Series* S = seriesById(app.curSeriesId);
    {
        char label[320];
        snprintf(label, sizeof label, "%s / %s", batchNameOf(S->batchId).c_str(),
                 S->name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 17);
        if (ImGui::BeginCombo("##series", label)) {
            for (const auto& s : app.series) {
                char it[384];
                // "##" would cut a batch or series name in half: give every row
                // an explicit id and let the visible part be whatever it is.
                // The KIND is on the row because this panel cannot measure them
                // all: picking a temperature run here has to be a visible choice.
                snprintf(it, sizeof it, "%s / %s%s%s##ser%d", batchNameOf(s.batchId).c_str(),
                         s.name.c_str(), seriesFitKind(s) ? "" : "   ",
                         seriesFitKind(s) ? "" : seriesKindName(s.kind), s.id);
                if (ImGui::Selectable(it, s.id == app.curSeriesId)) app.curSeriesId = s.id;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Edit...")) openSeriesModal(S->batchId, S->id);
        ImGui::SameLine();
        if (ImGui::Button("New...")) openSeriesModal(cur() ? cur()->batchId : S->batchId, 0);
        S = seriesById(app.curSeriesId);
    }
    const bool haveUnit = S->unit[0] != '\0';
    const bool fitKind = seriesFitKind(*S);
    if (S->paramName.empty()) ImGui::TextDisabled("(parameter unnamed)");
    else ImGui::TextUnformatted(S->paramName.c_str());
    ImGui::SameLine();
    if (haveUnit) ImGui::Text("[%s]", S->unit);
    else ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "[unit not set]");
    if (!haveUnit && ImGui::IsItemHovered())
        ImGui::SetTooltip("A sensitivity is DN per SOMETHING. Set the unit in\n"
                          "Edit... - it is not assumed for you.");
    if (!fitKind) {   // the kind picks the equation; this panel knows two of them
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "[%s series]",
                           seriesKindName(S->kind));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This panel fits a linearity / PTC sweep. A %s series\n"
                              "is a different measurement with a different equation,\n"
                              "so no fit is printed for it - the per-stack means are\n"
                              "measured either way.\n"
                              "Change the kind in Edit... if this really is a sweep.",
                              seriesKindName(S->kind));
    }
    ImGui::SameLine();
    if (ImGui::Button("Compute")) linRecompute(S->id);
    ImGui::SameLine();
    ImGui::TextDisabled(L.roiUsed ? "| selected ROI" : "| whole frame");
    // The per-stack table has room for ONE response column. Averaging the CFA
    // planes into it would be a different measurement, so it shows the plane you
    // pick and says which one; the fits and the plots stay per-plane.
    if (L.nPl > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::Combo("table plane", &L.tablePlane, "R\0Gr\0Gb\0B\0");
        L.tablePlane = std::clamp(L.tablePlane, 0, L.nPl - 1);
    }
    const bool fresh = L.seriesId == S->id;

    // one row per MEMBER, in the series' order: value in, response out
    const ImGuiTableFlags TF = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    char lvlHdr[64], meanHdr[32], sigHdr[32];
    snprintf(lvlHdr, sizeof lvlHdr, "value [%s]", haveUnit ? S->unit : "unit not set");
    const char* pl = L.nPl > 1 ? LIN_PLANES[std::clamp(L.tablePlane, 0, 3)] : "";
    snprintf(meanHdr, sizeof meanHdr, "mean %s [DN]", pl);
    snprintf(sigHdr, sizeof sigHdr, "sigma_t %s [DN]", pl);
    float tableH = ImGui::GetFontSize() * std::min<int>(10, (int)S->members.size() + 2) * 1.6f;
    if (ImGui::BeginTable("lintab", 7, TF, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("stack", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
        ImGui::TableSetupColumn(lvlHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.2f);
        ImGui::TableSetupColumn(meanHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(sigHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2);
        ImGui::TableHeadersRow();
        for (auto& m : S->members) {
            App::SeqInfo* si = seqInfo(m.seqId);
            if (!si) continue;
            ImGui::PushID(m.seqId);
            const App::LinState::Row* row = nullptr;
            if (fresh)
                for (const auto& r : L.rows) if (r.seqId == m.seqId) { row = &r; break; }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##inc", &m.include)) {
                for (auto& r : L.rows) if (r.seqId == m.seqId) r.include = m.include;
                // the fit below was measured WITH this point (or without it):
                // it is now describing a set of points that is not on screen
                linFitStale();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("include this point in the fit");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(si->name.c_str());
            ImGui::TableNextColumn();
            // NEVER 0 for "not set": the whole reason values live on the member
            if (std::isfinite(m.value)) textNum("%.6g", m.value);
            else ImGui::TextDisabled("unset");
            ImGui::TableNextColumn();
            // used/expected whenever the total is known: Compute derives
            // sigma_t, K, LEmax and read noise from whatever is RESIDENT, and a
            // bare "60" beside a [server, 300 frames] figure for the same stack
            // is the difference the canon requires to be stated
            if (row && row->valid && si->expectedFrames > row->frames)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%d/%d",
                                   row->frames, si->expectedFrames);
            else if (row && row->valid) ImGui::Text("%d", row->frames);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            int tp = row ? std::clamp(L.tablePlane, 0, std::max(0, row->nPl - 1)) : 0;
            if (row && row->valid) textNum("%.6g", row->mean[tp]);
            else ImGui::TextDisabled(row ? row->err.c_str() : "-");
            ImGui::TableNextColumn();
            if (row && row->valid) textNum("%.6g", row->sigmaT[tp]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("values are edited in Edit... - a blank one is UNSET, not zero");

    if (!L.fitValid || !fresh) {
        if (!haveUnit)
            ImGui::TextDisabled("no unit: set one in Edit... and the fit follows.");
        else if (!fitKind)
            ImGui::TextDisabled("kind is \"%s\": the linearity / PTC equations are not the "
                                "ones this series asked for.", seriesKindName(S->kind));
        else if (seriesFitPoints(*S) < 2)
            ImGui::TextDisabled("a fit needs 2+ members with a value (3+ to see linearity).");
        else if (!fresh)
            ImGui::TextDisabled("press Compute to measure this series.");
        else
            ImGui::TextDisabled("press Compute; CFA planes stay separate throughout.");
        return;
    }

    // ---- the answers ----
    ImGui::SeparatorText("fit  (response = sensitivity * value + offset)");
    if (ImGui::BeginTable("linfit", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        char sensHdr[64];
        snprintf(sensHdr, sizeof sensHdr, "sensitivity [DN/%s]", S->unit);
        ImGui::TableSetupColumn("plane", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn(sensHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("offset [DN]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("R^2", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("LE max [%]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("K [DN/e-]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(L.readFromDark ? "read noise [DN]" : "read noise [DN] (extrap.)",
                                ImGuiTableColumnFlags_WidthFixed, numColW() * 1.3f);
        ImGui::TableHeadersRow();
        for (int p = 0; p < L.nPl; p++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LIN_COLS[p]));
            ImGui::TextUnformatted(L.nPl > 1 ? LIN_PLANES[p] : "all");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); textNum("%.6g", L.slope[p]);
            ImGui::TableNextColumn(); textNum("%.6g", L.offs[p]);
            ImGui::TableNextColumn(); textNum("%.5f", L.r2[p]);
            ImGui::TableNextColumn(); textNum("%.3f", L.leMax[p]);
            ImGui::TableNextColumn(); textNum("%.5g", L.ptcK[p]);
            ImGui::TableNextColumn();
            if (L.readFromDark) textNum("%.4g", L.readDN[p]);
            else {                        // an extrapolation is not a measurement
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                textNum("%.4g", L.readDN[p]);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Extrapolated from the photon transfer fit to the black\n"
                                      "level, so it is a difference of two large numbers and\n"
                                      "can be off by several DN.\n"
                                      "Add a dark stack (level 0) to MEASURE it instead.");
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%d points | K from photon transfer (sigma_t^2 vs mean); "
                        "LE = worst deviation from the fit | read noise %s", L.nPts,
                        L.readFromDark ? "measured in the level-0 stack"
                                       : "extrapolated (no dark stack)");

    // ---- SNR curve: the same rows, the mean set against the noise it carries.
    // A SECTION of this panel, not a new one: the material is the fit's own rows
    // (docs/stats-taxonomy.md gap #2, and its §4 sends LM curves here first).
    {
        std::vector<LinSnrPt> sp;
        bool snrDark = false;
        int dropped = 0;
        linSnrPoints(L, sp, &snrDark, &dropped);
        if (ImGui::CollapsingHeader("SNR curve", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("dB##snrdb", &L.snrDb);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("y axis as 20*log10(SNR) instead of the plain ratio");
            ImGui::SameLine();
            ImGui::BeginDisabled(sp.empty());
            if (ImGui::SmallButton("Copy curve (TSV)")) {
                // provenance rides along as a # comment, like every other copy
                std::string tsv = "# SNR curve: series '" + S->name + "' (batch '" +
                                  batchNameOf(S->batchId) + "') | " +
                                  (L.roiUsed ? "selected ROI" : "whole frame") +
                                  " | SNR = (mean - dark) / sigma_t per CFA plane | dark = " +
                                  (snrDark ? "measured level-0 stack"
                                           : "fitted offset (extrapolated)") + "\n";
                char b[320];
                snprintf(b, sizeof b, "value [%s]\tstack\tplane\tmean [DN]\tsignal [DN]\t"
                                      "sigma_t [DN]\tSNR [-]\tSNR [dB]\tframes\n", S->unit);
                tsv += b;
                for (const auto& q : sp) {
                    const App::LinState::Row* r = nullptr;
                    for (const auto& rr : L.rows) if (rr.seqId == q.seqId) { r = &rr; break; }
                    if (!r) continue;
                    const App::SeqInfo* si = seqInfo(q.seqId);
                    char fb[32];
                    if (si && si->expectedFrames > r->frames)
                        snprintf(fb, sizeof fb, "%d/%d", r->frames, si->expectedFrames);
                    else snprintf(fb, sizeof fb, "%d", r->frames);
                    snprintf(b, sizeof b, "%.6g\t%s\t%s\t%.6g\t%.6g\t%.6g\t%.6g\t%.6g\t%s\n",
                             r->level, r->name.c_str(),
                             L.nPl > 1 ? LIN_PLANES[q.plane] : "all", r->mean[q.plane],
                             q.x, r->sigmaT[q.plane], q.snr, 20.0 * log10(q.snr), fb);
                    tsv += b;
                }
                ImGui::SetClipboardText(tsv.c_str());
                toast("SNR curve copied as TSV (provenance included)");
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            // the same measured-vs-extrapolated honesty the read noise column has
            ImGui::TextDisabled(snrDark ? "| dark = measured level-0 stack"
                                        : "| dark = fitted offset (extrapolated)");
            if (!snrDark && ImGui::IsItemHovered())
                ImGui::SetTooltip("No dark (level-0) stack in the series, so the signal is\n"
                                  "the mean minus the FITTED offset - an extrapolation.\n"
                                  "Add a dark stack to measure the black level instead.");
            if (sp.empty()) {
                ImGui::TextDisabled("no lit points: SNR needs a level with signal above dark.");
            } else {
                double x0 = DBL_MAX, x1 = 0, y0 = DBL_MAX, y1 = 0;
                for (const auto& q : sp) {
                    x0 = std::min(x0, q.x); x1 = std::max(x1, q.x);
                    y0 = std::min(y0, q.snr); y1 = std::max(y1, q.snr);
                }
                double Kavg = 0;                  // for the shot-noise guide line
                int nK = 0;
                for (int p = 0; p < L.nPl; p++) if (L.ptcK[p] > 0) { Kavg += L.ptcK[p]; nK++; }
                Kavg = nK ? Kavg / nK : 0;
                const bool db = L.snrDb;
                float snrH = std::max(110.0f * app.uiScale,
                                      std::min(ImGui::GetContentRegionAvail().y * 0.40f,
                                               ImGui::GetFontSize() * 18));
                PlotRect pr = beginPlotLog("mean signal [DN] (dark-corrected)",
                                           db ? "SNR [dB]" : "SNR [-]", x0, x1,
                                           db ? 20.0 * log10(y0) : y0,
                                           db ? 20.0 * log10(y1) : y1, !db, snrH);
                if (pr.ok) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->PushClipRect(pr.p0, pr.p1, true);
                    // the ideal shot-noise limit sqrt(signal/K): slope 1/2 in
                    // log-log (10 dB/decade in dB). DASHED because it is a
                    // REFERENCE - the data curves stay solid.
                    if (Kavg > 0) {
                        auto refY = [&](float t) {   // t = log10(signal)
                            return db ? (float)(10.0 * (t - log10(Kavg)))
                                      : (float)(0.5 * (t - log10(Kavg)));
                        };
                        ImVec2 seg[2] = { pr.at(pr.xmin, refY(pr.xmin)),
                                          pr.at(pr.xmax, refY(pr.xmax)) };
                        addDashedPolyline(dl, seg, 2, IM_COL32(170, 178, 186, 190), 1.4f,
                                          7 * app.uiScale, 5 * app.uiScale);
                    }
                    for (int p = 0; p < L.nPl; p++) {   // per plane: line under points
                        std::vector<const LinSnrPt*> ps;
                        for (const auto& q : sp) if (q.plane == p) ps.push_back(&q);
                        std::sort(ps.begin(), ps.end(),
                                  [](const LinSnrPt* a, const LinSnrPt* b) { return a->x < b->x; });
                        std::vector<ImVec2> line;
                        for (const LinSnrPt* q : ps)
                            line.push_back(pr.at((float)log10(q->x),
                                                 db ? (float)(20.0 * log10(q->snr))
                                                    : (float)log10(q->snr)));
                        for (size_t i = 1; i < line.size(); i++)
                            dl->AddLine(line[i - 1], line[i],
                                        (LIN_COLS[p] & 0x00FFFFFF) | 0x60000000, 1.5f);
                        for (const ImVec2& v : line) dl->AddCircleFilled(v, 3.5f, LIN_COLS[p]);
                    }
                    dl->PopClipRect();
                }
                if (L.nPl > 1)
                    for (int p = 0; p < L.nPl; p++) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImGui::ColorConvertU32ToFloat4(LIN_COLS[p]));
                        ImGui::TextUnformatted(LIN_PLANES[p]);
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                    }
                ImGui::TextDisabled("%s- - ideal shot noise sqrt(signal/K), slope 1/2",
                                    L.nPl > 1 ? "| " : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("SNR if photon shot noise were the only noise, from the\n"
                                      "PTC gain K (averaged over the fitted planes). The gap\n"
                                      "below it at low signal is read noise.");
                ImGui::TextDisabled("%d point(s) | SNR = (mean - dark) / sigma_t, planes "
                                    "never mixed", (int)sp.size());
                // n of N, exactly where the table above says it: a sigma_t over a
                // partial stack is a real measurement over fewer frames, and the
                // curve must say so too
                std::string part;
                for (const auto& r : L.rows) {
                    if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
                    const App::SeqInfo* si = seqInfo(r.seqId);
                    if (si && si->expectedFrames > r.frames) {
                        char pb[96];
                        snprintf(pb, sizeof pb, "%s%s %d/%d", part.empty() ? "" : ", ",
                                 r.name.c_str(), r.frames, si->expectedFrames);
                        part += pb;
                    }
                }
                if (!part.empty())
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                                       "partial stacks (measured over resident frames): %s",
                                       part.c_str());
                if (dropped)
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                                       "%d plane point(s) at or below dark level left out "
                                       "(nothing positive to plot)", dropped);
            }
        }
    }

    // ---- plots: the response, and the part of it that is NOT linear ----
    struct Pt { double x, y; int p; };
    std::vector<Pt> pts;
    double x0 = DBL_MAX, x1 = -DBL_MAX, y0 = DBL_MAX, y1 = -DBL_MAX, e1 = 0;
    for (const auto& r : L.rows) {
        if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
        for (int p = 0; p < r.nPl; p++) {
            pts.push_back({ r.level, r.mean[p], p });
            x0 = std::min(x0, r.level); x1 = std::max(x1, r.level);
            y0 = std::min(y0, r.mean[p]); y1 = std::max(y1, r.mean[p]);
            double fit = L.slope[p] * r.level + L.offs[p];
            if (fabs(fit) > 1e-9)
                e1 = std::max(e1, fabs((r.mean[p] - fit) / fit) * 100.0);
        }
    }
    if (pts.empty()) return;
    // A plotted axis states the quantity AND the unit, and the unit is the
    // series' - there is no fit without one, and editing the series drops the
    // fit (linInvalidate), so it cannot have been emptied out from under this.
    char xl[96];
    snprintf(xl, sizeof xl, "%s [%s]",
             S->paramName.empty() ? "value" : S->paramName.c_str(), S->unit);
    float half = std::max((ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 5) * 0.5f,
                          70.0f * app.uiScale);
    {
        PlotRect pr = beginPlot(xl, "ROI mean [DN]", (float)x0, (float)x1,
                                (float)y0, (float)y1, false, false, half);
        if (pr.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            for (int p = 0; p < L.nPl; p++) {   // the fit first, under the points
                ImVec2 a = pr.at((float)x0, (float)(L.slope[p] * x0 + L.offs[p]));
                ImVec2 b = pr.at((float)x1, (float)(L.slope[p] * x1 + L.offs[p]));
                dl->AddLine(a, b, (LIN_COLS[p] & 0x00FFFFFF) | 0x60000000, 1.5f);
            }
            for (const auto& q : pts)
                dl->AddCircleFilled(pr.at((float)q.x, (float)q.y), 3.5f, LIN_COLS[q.p]);
            dl->PopClipRect();
        }
    }
    {
        float lim = (float)std::max(e1 * 1.2, 0.1);
        PlotRect pr = beginPlot(xl, "deviation from fit [%]", (float)x0, (float)x1,
                                -lim, lim, false, false, half);
        if (pr.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            ImVec2 z0 = pr.at((float)x0, 0), z1 = pr.at((float)x1, 0);
            dl->AddLine(z0, z1, IM_COL32(140, 150, 160, 160));
            for (const auto& q : pts) {
                double fit = L.slope[q.p] * q.x + L.offs[q.p];
                if (fabs(fit) < 1e-9) continue;
                double res = (q.y - fit) / fit * 100.0;
                dl->AddCircleFilled(pr.at((float)q.x, (float)res), 3.5f, LIN_COLS[q.p]);
            }
            dl->PopClipRect();
        }
    }
}

// The x axis one stack's per-frame data is plotted (and exported) against:
// the custom axis when it is set AND still covers every frame, else the frame
// index. ONE resolver for the chart, the export and the selftest, so they can
// never disagree about which axis is in force. `why` names the reason a SET
// axis is not usable (frame count changed since it was applied).
struct FrameAxis {
    bool custom = false;
    std::string name, unit;
    const std::vector<double>* vals = nullptr;   // by seqIndex
    std::string label;                           // ready for beginPlot's x label
    std::string why;
    double at(double seqIndex) const {           // mapping: value i <- seqIndex i
        int i = (int)(seqIndex + 0.5);
        return custom && i >= 0 && i < (int)vals->size() ? (*vals)[i] : seqIndex;
    }
};
static FrameAxis frameAxisOf(int seqId) {
    FrameAxis a;
    a.label = "frame number (index in stack)";
    App::SeqInfo* si = seqInfo(seqId);
    if (!si || si->axisVals.empty()) return a;
    int n = (int)framesOfSeq(seqId).size();
    int N = std::max(si->expectedFrames, n);
    if ((int)si->axisVals.size() != N) {
        char b[128];
        snprintf(b, sizeof b, "custom x axis ignored: %d value(s) for %d frame(s) - "
                              "plotted on frame index",
                 (int)si->axisVals.size(), N);
        a.why = b;
        return a;
    }
    a.custom = true;
    a.name = si->axisName;
    a.unit = si->axisUnit;
    a.vals = &si->axisVals;
    a.label = si->axisName + " (" + si->axisUnit + ")";
    return a;
}

