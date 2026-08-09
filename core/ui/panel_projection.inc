// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// What the Projection panel SAID about the slots on the last frame it drew -
// the same grammar as g_histSideProbe ("rowX;" per table side, "curves=",
// "said-no-curve="), because it guards the same equation:
// rows ⊆ curves ∪ said-no-curve. The projection was the panel issue #60 caught
// giving slots table rows while the plots drew A and B and said NOTHING about
// the letters they left off - the exact silence the histogram had already been
// cured of, unguarded here because no probe existed to assert it.
static std::string g_projSideProbe;

static void drawPanelProjection() {
    g_projSideProbe.clear();
    ImageDoc* im = cur();
    if (!im || im->w < 1 || im->h < 1) { ImGui::TextDisabled("no image"); return; }
    const char* modes[3] = { "mean", "max", "min" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::BeginCombo("reduce", modes[std::clamp(app.projMode, 0, 2)])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(modes[i], app.projMode == i)) app.projMode = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::Checkbox("H", &app.showProjH);
    ImGui::SameLine(); ImGui::Checkbox("V", &app.showProjV);
    ImageDoc* Bim = abStatsB();
    pumpCompareSlotRestore();
    // The slots resolve exactly as B does - follow-frame included - and the
    // cache is per SLOT, indexed by the letter (= position in cmpExtra), so a
    // followed sibling recomputes into ITS slot and a slot A stands on keeps
    // its cache. A slot that is also B keeps its own column: see the histogram.
    std::vector<ResolvedSlot> xslots = resolveSlots();
    app.projExtra.resize(app.cmpExtra.size());
    recomputeProjectionIfNeeded(im, app.proj[0]);
    // The extras follow B's rule exactly: skipped while frames are being
    // stepped, so N slots cannot turn a held arrow key into N recomputes.
    if (!abStepBusy())
        for (const ResolvedSlot& rs : xslots)
            if (rs.idx < app.projExtra.size())
                recomputeProjectionIfNeeded(rs.doc, app.projExtra[rs.idx]);
    // skipped while frames are being stepped (see abStepBusy), except on the
    // first fill - an empty B plot would be worse than a stale one
    if (Bim && (!abStepBusy() || app.proj[1].uid == 0))
        recomputeProjectionIfNeeded(Bim, app.proj[1]);
    const App::ProjState& P = app.proj[0];
    const App::ProjState& PB = app.proj[1];
    const bool bStale = Bim && (PB.uid != Bim->uid || g_abFollowDiverged);
    const std::string bLabel = Bim ? abDocLabel(Bim) : std::string();
    ImGui::SameLine();
    if (Bim) ImGui::TextDisabled("A %dx%d  B %dx%d  (%s)", P.rw, P.rh, PB.rw, PB.rh,
                                 P.roiUsed ? "ROI" : "whole image");
    else     ImGui::TextDisabled("%s  %dx%d", P.roiUsed ? "ROI" : "whole image", P.rw, P.rh);

    // value axis: a rescaling y axis makes profiles impossible to compare
    const char* ymodes[3] = { "auto", "display range", "fixed" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("value axis", ymodes[std::clamp(app.projYMode, 0, 2)])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(ymodes[i], app.projYMode == i)) {
                if (i == 2 && app.projYMode != 2) {   // seed 'fixed' from what is shown
                    app.projYLo = std::min(P.hMin, P.vMin);
                    app.projYHi = std::max(P.hMax, P.vMax);
                }
                app.projYMode = i;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("auto: fits the data (H and V share one scale)\n"
                          "display range: the black/white points - link those across images\n"
                          "and every image's profile lands on the same axis\n"
                          "fixed: whatever you type below");
    float yLo, yHi;
    if (app.projYMode == 1) { yLo = effBlack(*im); yHi = effWhite(*im); }
    else if (app.projYMode == 2) { yLo = app.projYLo; yHi = app.projYHi; }
    else {                                   // shared across H, V - and A and B
        yLo = std::min(P.hMin, P.vMin); yHi = std::max(P.hMax, P.vMax);
        if (Bim) {
            yLo = std::min(yLo, std::min(PB.hMin, PB.vMin));
            yHi = std::max(yHi, std::max(PB.hMax, PB.vMax));
        }
    }
    if (app.projYMode == 2) {
        ImGui::SameLine();
        // shadow buffer: committing per keystroke would re-project the image on
        // every digit typed
        static float pv[2] = {};
        static bool pvEditing = false;
        if (!pvEditing) { pv[0] = app.projYLo; pv[1] = app.projYHi; }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        ImGui::InputFloat2("##projy", pv, "%.5g");
        pvEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && pv[1] > pv[0]) {
            app.projYLo = pv[0]; app.projYHi = pv[1];
        }
    }

    static const ImU32 CFA_COLS[4] = { IM_COL32(255, 92, 92, 220), IM_COL32(120, 230, 120, 220),
                                       IM_COL32(60, 180, 140, 220), IM_COL32(92, 155, 255, 220) };
    static const ImU32 RGB_COLS[3] = { IM_COL32(255, 92, 92, 210), IM_COL32(79, 221, 107, 210),
                                       IM_COL32(92, 155, 255, 210) };
    const ImU32 ODD_COL = IM_COL32(185, 192, 200, 210);   // a series only one side has
    bool cfa = im->ch == 1 && im->cfa != 0;
    int plots = (app.showProjH ? 1 : 0) + (app.showProjV ? 1 : 0);
    if (!plots) { ImGui::TextDisabled("enable H or V"); return; }

    // Overlay is only honest when the two profiles are the SAME axis: same
    // length and same origin. Different lengths cannot be aligned without
    // knowing the physical mapping between the two captures, and stretching one
    // to fit the other would invent a correspondence that does not exist. So:
    // fall back to side by side and say why.
    const bool canOverlay = !Bim || abProjOverlayable(P, PB);
    const float minSide = AB_MIN_SIDE * app.uiScale;
    bool side = Bim && (abSideBySide() || !canOverlay);
    // the narrow-panel fallback yields to correctness: mismatched profiles are
    // never overlaid, however little room there is
    bool tooNarrow = side && canOverlay && ImGui::GetContentRegionAvail().x < minSide;
    if (tooNarrow) side = false;

    // reserve the statistics table first: the plots must not push it off-panel
    int extraSeries = 0;
    for (const ResolvedSlot& rs : xslots)
        if (rs.idx < app.projExtra.size()) extraSeries += app.projExtra[rs.idx].nSeries;
    // ...plus one pooled "all" row per side when it is switched on
    int allRows = (P.allRow ? 1 : 0) + (Bim && PB.allRow ? 1 : 0);
    for (const ResolvedSlot& rs : xslots)
        if (rs.idx < app.projExtra.size() && app.projExtra[rs.idx].allRow) allRows++;
    int statRows = (P.nSeries + (Bim ? PB.nSeries : 0) + extraSeries + allRows) * plots;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    // wide draws one row per PLANE per side; per-axis draws one per axis too
    const bool wideGuess = app.projStatLayout == 1 ||
                           (app.projStatLayout == 0 &&
                            ImGui::GetContentRegionAvail().x >=
                                ImGui::GetFontSize() * (Bim ? 4.0f : 2.4f) + numColW() * 10 +
                                ImGui::GetStyle().CellPadding.x * 24);
    if (wideGuess) statRows = P.nSeries + (Bim ? PB.nSeries : 0) + extraSeries + allRows;
    float statsH = ImGui::GetFrameHeightWithSpacing()      // "profile statistics" separator
                 + ImGui::GetFrameHeightWithSpacing()      // the layout combo
                 + lineH * (statRows + 1)                  // header + one row per axis/channel
                 + lineH;                                  // footnote
    if (Bim && !canOverlay) statsH += lineH;               // the mismatch notice
    if (tooNarrow) statsH += abNarrowNoteH();
    // one legend row under each overlaid plot, reserved before they are sized
    const std::string aLabel = abDocLabel(im);
    float legendH = Bim && !side ? abLegendH(aLabel, bLabel, bStale) : 0.0f;
    float avail = ImGui::GetContentRegionAvail().y - statsH - legendH * plots;
    if (side) avail -= (ImGui::GetTextLineHeight() + 6 * app.uiScale);   // heading band
    float each = std::max((avail - lineH * plots) / plots
                          - (ImGui::GetFontSize() * 3 + 12 * app.uiScale), 60.0f * app.uiScale);

    // x range: the UNION of A's and B's sample ranges, so both plots (side by
    // side or overlaid) carry the same axis and neither is rescaled to fit.
    auto xRange = [&](bool horizontal, float& x0, float& x1) {
        int o = horizontal ? P.rx : P.ry, n = horizontal ? P.rw : P.rh;
        x0 = (float)o; x1 = (float)(o + std::max(n - 1, 1));
        if (Bim) {
            int ob = horizontal ? PB.rx : PB.ry, nb = horizontal ? PB.rw : PB.rh;
            x0 = std::min(x0, (float)ob);
            x1 = std::max(x1, (float)(ob + std::max(nb - 1, 1)));
        }
    };
    // Stroke one side's profiles. isB = this is the compare slot. tint = the
    // two are sharing ONE plot, so B needs its own ink; drawn side by side
    // there is nothing to tell apart within a plot, and B is stroked exactly
    // like A so the two panels can be read against each other. bars = the
    // min-max range of a decimated bucket - suppressed only when overlaid,
    // where two sets of range bars on one plot is noise.
    auto stroke = [&](const PlotRect& pr, const App::ProjState& S, bool horizontal,
                      bool isB, bool tint, bool bars) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::vector<float>* series = horizontal ? S.h : S.v;
        int origin = horizontal ? S.rx : S.ry;
        for (int s = 0; s < S.nSeries; s++) {
            // colour follows A's series OF THE SAME NAME (R is always R); a
            // series only one side has gets a neutral stroke, never a plane hue
            int a = isB ? abSeriesMatch(P.seriesNames, P.nSeries, S.seriesNames[s]) : s;
            const bool mono = !cfa && P.nSeries == 1;
            ImU32 col = a < 0 ? ODD_COL
                      : (cfa ? CFA_COLS[a]
                             : (mono ? IM_COL32(215, 220, 225, 230) : RGB_COLS[a]));
            // B on A's plot: keep the plane hue and lift it toward white, so R
            // still reads as R and the pair still separates at a glance. A dash
            // pattern did that job before and lost against a dense profile -
            // the curve simply looked broken. The mono case has no hue to keep
            // (A is already near-white), so B takes the blue the legend flies.
            if (tint && a >= 0) col = mono ? AB_B_INK : abLiftInk(col, 0.45f);
            const std::vector<float>& d = series[s];
            // decimate to the plot's pixel width: 4000 samples into 400 px was
            // 4000 line segments per series per frame
            int px = std::max(1, (int)(pr.p1.x - pr.p0.x));
            int stride = std::max(1, (int)d.size() / px);
            std::vector<ImVec2> run;
            auto flush = [&]() {
                if (run.size() >= 2)
                    dl->AddPolyline(run.data(), (int)run.size(), col, 0, 1.2f);
                run.clear();
            };
            for (int i = 0; i < (int)d.size(); i += stride) {
                float lo = FLT_MAX, hi = -FLT_MAX;
                for (int k = i; k < std::min(i + stride, (int)d.size()); k++)
                    if (std::isfinite(d[k])) { lo = std::min(lo, d[k]); hi = std::max(hi, d[k]); }
                if (lo > hi) { flush(); continue; }
                if (bars && stride > 1) {
                    ImVec2 a2 = pr.at((float)(origin + i), lo), b2 = pr.at((float)(origin + i), hi);
                    if (b2.y != a2.y) dl->AddLine(a2, b2, col, 1.2f);   // min-max bar
                }
                run.push_back(pr.at((float)(origin + i), (lo + hi) * 0.5f));
            }
            flush();
        }
    };
    // "There is a spike - WHICH column is it?" The readout answers with the
    // exact index and the values under the cursor, without decimation: the
    // marker snaps to the true sample, not to the plotted min-max bucket.
    auto hoverReadout = [&](const PlotRect& pr, bool horizontal, bool wantA, bool wantB) {
        if (!ImGui::IsMouseHoveringRect(pr.p0, pr.p1)) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float mx = ImGui::GetMousePos().x;
        float t = (mx - pr.p0.x) / std::max(pr.p1.x - pr.p0.x, 1.0f);
        int at = (int)(pr.xmin + t * (pr.xmax - pr.xmin) + 0.5f);
        dl->AddLine(pr.at((float)at, pr.ymin), pr.at((float)at, pr.ymax),
                    IM_COL32(230, 200, 90, 140), 1.0f);
        char tip[512];
        int off = snprintf(tip, sizeof tip, "%s %d", horizontal ? "column x" : "row y", at);
        auto one = [&](const App::ProjState& S, const char* side2, const std::string& dt) {
            const std::vector<float>* series = horizontal ? S.h : S.v;
            int origin = horizontal ? S.rx : S.ry;
            int i = at - origin;
            for (int s2 = 0; s2 < S.nSeries; s2++) {
                if (i < 0 || i >= (int)series[s2].size() || !std::isfinite(series[s2][i])) continue;
                const char* nm = S.nSeries == 1 ? "" : S.seriesNames[s2];
                off += snprintf(tip + off, sizeof tip - off, "\n%s%s%s%.6g %s",
                                side2, nm, *nm ? ": " : "", series[s2][i], dt.c_str());
                dl->AddCircleFilled(pr.at((float)at, series[s2][i]), 3.0f,
                                    IM_COL32(230, 200, 90, 230));
            }
        };
        if (wantA) one(P, Bim ? "A " : "", abValueUnit(im->dtype));
        if (wantB && Bim) one(PB, "B ", abValueUnit(Bim->dtype));
        ImGui::SetTooltip("%s", tip);
    };
    auto onePlot = [&](bool horizontal, bool wantA, bool wantB) {
        float x0, x1;
        xRange(horizontal, x0, x1);
        char yl[176];
        // "display range" is effBlack/effWhite, which with compare on is the A/B
        // range - so say which one, exactly as the histogram's x axis does
        char yr[80] = "";
        if (app.projYMode == 1)
            snprintf(yr, sizeof yr, ", %s", abRangeSaid(Bim != nullptr));
        if (Bim && Bim->dtype != im->dtype)
            snprintf(yl, sizeof yl, "%s value (%s%s)  -  A %s / B %s, DTYPE MISMATCH",
                     modes[std::clamp(app.projMode, 0, 2)], abValueUnit(im->dtype).c_str(),
                     yr, im->dtype.c_str(), Bim->dtype.c_str());
        else
            snprintf(yl, sizeof yl, "%s value (%s%s)", modes[std::clamp(app.projMode, 0, 2)],
                     abValueUnit(im->dtype).c_str(), yr);
        PlotRect pr = beginPlot(horizontal ? "column x (px)" : "row y (px)", yl,
                                x0, x1, yLo, yHi, true, false, each);
        if (!pr.ok) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pr.p0, pr.p1, true);
        // overlaid = both on this plot: B gets its own ink and gives up the
        // range bars. Alone in its own panel, B is stroked exactly like A.
        const bool overlaid = wantA && wantB && Bim;
        if (wantA) stroke(pr, P, horizontal, false, false, true);
        if (wantB && Bim) stroke(pr, PB, horizontal, true, overlaid, !overlaid);
        dl->PopClipRect();
        hoverReadout(pr, horizontal, wantA, wantB);
        if (wantA && wantB && Bim) drawABLegendRow(abDocLabel(im), bLabel, bStale);
    };

    if (!side) {
        if (app.showProjH) onePlot(true, true, true);
        if (app.showProjV) onePlot(false, true, true);
    } else {
        // always 50/50 and always A on the left (docs/ab-stats-plan.md 3)
        const ImGuiStyle& st = ImGui::GetStyle();
        float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
        float childH = ImGui::GetTextLineHeight() + 8 * app.uiScale
                     + plots * (each + ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        ImGui::BeginChild("##projA", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("A", abDocLabel(im));
        if (app.showProjH) onePlot(true, true, false);
        if (app.showProjV) onePlot(false, true, false);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##projB", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("B", bLabel, bStale);
        if (app.showProjH) onePlot(true, false, true);
        if (app.showProjV) onePlot(false, false, true);
        ImGui::EndChild();
    }
    if (Bim && !canOverlay)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "profiles do not share an axis (A x %d..%d, y %d..%d px; "
                           "B x %d..%d, y %d..%d px): shown side by side, never stretched",
                           P.rx, P.rx + P.rw - 1, P.ry, P.ry + P.rh - 1,
                           PB.rx, PB.rx + PB.rw - 1, PB.ry, PB.ry + PB.rh - 1);
    if (tooNarrow) abNarrowNote();
    {   // What the plots took and what they left, as the panel says it. The
        // profile plots still draw only A and B: the lettered slots are fed
        // (projExtra recomputes above) and the table below gives them rows,
        // and until the N overlay lands (docs/compare-n.md §12) the plots
        // cannot take them - which must be SAID, right here under the plots,
        // or a slot's absence reads as the slot having been lost. Same rule,
        // same wording shape and same amber as the histogram's off-plot note.
        g_projSideProbe += std::string("curves=A") + (Bim ? "B" : "") + ";";
        if (!xslots.empty()) {
            std::string off;
            for (const ResolvedSlot& rs : xslots) {
                if (!off.empty()) off += ", ";
                off += slotName(rs.idx);
            }
            std::string offLetters;
            for (const ResolvedSlot& rs : xslots) offLetters += slotName(rs.idx);
            g_projSideProbe += "said-no-curve=" + offLetters + ";";
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                               "slot %s: no curve here yet - the profile plots draw "
                               "only A and B. The table below has every slot's numbers.",
                               off.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // numbers to go with the curves
    ImGui::SeparatorText("profile statistics");
    // "side" exists as a column the moment there is more than one side, which
    // is B or any extra slot
    const bool anySide = Bim || !xslots.empty();
    // Every row of both tables comes from here, so the two layouts can never
    // disagree about which rows exist or what order they are in. Side major
    // keeps one capture's planes together; channel major puts R:A directly
    // above R:B, which is what comparing ONE plane across sides wants. The
    // pooled "all" row is always last of its group - it is not a plane, and
    // sorting it among them would invite reading it as one.
    // CFA interpretation is a property of an IMAGE, not of the sensor: A can be
    // set to Bayer in the Inspector while B, loaded another way, is still plain
    // 1-channel. Then A gets four rows and B gets one, and nothing on screen
    // says why - the reader concludes B has no planes rather than that B was
    // never told it is a mosaic. Say it.
    struct SideRef { const App::ProjState* S; std::string name; bool stale; };
    std::vector<SideRef> sides;
    sides.push_back({ &P, "A", false });
    // B is stale while the step throttle holds its recompute back, or when
    // follow-frame found no partner; a slot is stale when ITS follow diverged.
    // The user's ruling (A4): a lagging number may lag if it is VISIBLY not
    // current - so the stale side's cells draw dimmed, not in the same ink as
    // fresh ones.
    if (Bim) sides.push_back({ &PB, "B",
                               bStale || (abStepBusy() && app.proj[1].uid != 0) });
    for (const ResolvedSlot& rs : xslots)
        if (rs.idx < app.projExtra.size())
            sides.push_back({ &app.projExtra[rs.idx], slotName(rs.idx),
                              slotFollowDiverged(rs.idx) || abStepBusy() });
    // every side the table is about to give numbers to, for the equation
    for (const SideRef& sr : sides) g_projSideProbe += "row" + sr.name + ";";
    {   // a diverged slot's rows are the last frame its stack had - say so with
        // the same sentence the B side gets (g_abFollowDiverged, above)
        std::string div;
        for (const ResolvedSlot& rs : xslots)
            if (rs.diverged) { if (!div.empty()) div += ", "; div += slotName(rs.idx); }
        if (!div.empty())
            ImGui::TextColored(ImVec4(0.98f, 0.76f, 0.35f, 1),
                               "follow-frame: slot %s: no frame %d in that stack - its "
                               "rows show the last frame it had, not A's partner",
                               div.c_str(), im->seqIndex);
    }
    int maxSeries = 0, minSeries = 99;
    for (const auto& sr : sides) {
        maxSeries = std::max(maxSeries, sr.S->nSeries);
        minSeries = std::min(minSeries, sr.S->nSeries);
    }
    auto projSayPlaneMismatch = [&]() {
        if (sides.size() < 2 || minSeries == maxSeries) return;
        std::string few;
        for (const auto& sr : sides)
            if (sr.S->nSeries < maxSeries) { if (!few.empty()) few += ", "; few += sr.name; }
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "%s %s %d plane(s), not %d: its CFA interpretation differs",
                           few.c_str(), few.find(',') == std::string::npos ? "has" : "have",
                           minSeries, maxSeries);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("\"Bayer\" is set per IMAGE (Inspector > Interpret), not per\n"
                              "sensor. A side loaded without it stays one plane, so the\n"
                              "rows are not comparable until both are told the same thing.");
    };
    auto sideIsStale = [&](const char* name) {
        for (const auto& sr : sides) if (sr.name == name) return sr.stale;
        return false;
    };
    auto projForEachRow = [&](const std::function<void(const App::ProjState&, const char*, int)>& row) {
        if (app.projStatOrder == 0) {
            for (const auto& sr : sides) {
                for (int i = 0; i < sr.S->nSeries; i++) row(*sr.S, sr.name.c_str(), i);
                if (sr.S->allRow) row(*sr.S, sr.name.c_str(), 4);
            }
        } else {
            for (int i = 0; i < maxSeries; i++)
                for (const auto& sr : sides)
                    if (i < sr.S->nSeries) row(*sr.S, sr.name.c_str(), i);
            for (const auto& sr : sides)
                if (sr.S->allRow) row(*sr.S, sr.name.c_str(), 4);
        }
    };
    // WIDE puts a plane's three sigmas on one line (region, row means, column
    // means) and spends the row axis on the SIDES instead of the axes. That is
    // the axis that has to grow: A and B are two rows today, and more slots or
    // more frames are more rows - whereas the quantities per plane are a fixed
    // nine. Auto falls back to the per-axis rows when the nine number columns
    // would not fit, because a table that scrolls sideways is not a table.
    const float wideNeed = ImGui::GetFontSize() * (Bim ? 1.6f : 0.0f) +
                           ImGui::GetFontSize() * 2.4f + numColW() * 10 +
                           ImGui::GetStyle().CellPadding.x * 24;
    bool wide = app.projStatLayout == 1 ||
                (app.projStatLayout == 0 && ImGui::GetContentRegionAvail().x >= wideNeed);
    {
        const char* modes[3] = { "auto", "wide", "per axis" };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
        if (ImGui::BeginCombo("layout##projstat", modes[std::clamp(app.projStatLayout, 0, 2)])) {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(modes[i], app.projStatLayout == i)) {
                    app.projStatLayout = i;
                    app.prefsDirty = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("wide: one row per plane - region, row-mean and column-mean\n"
                              "sigma side by side, so a plane reads on one line.\n"
                              "per axis: one row per axis, as before.\n"
                              "auto: wide whenever the columns fit.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wide ? "wide" : "per axis");
        ImGui::SameLine();
        static const char* DP[8] = { "sig. digits", "0", "1", "2", "3", "4", "5", "6" };
        int dpIdx = app.projDecimals < 0 ? 0 : app.projDecimals + 1;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::BeginCombo("decimals##projstat", DP[std::clamp(dpIdx, 0, 7)])) {
            for (int i = 0; i < 8; i++)
                if (ImGui::Selectable(DP[i], dpIdx == i)) {
                    app.projDecimals = i == 0 ? -1 : i - 1;
                    app.prefsDirty = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("fixed decimals make a COLUMN scannable, and pasteable\n"
                              "into a report as a column. \"sig. digits\" is the old\n"
                              "behaviour: every cell picks its own length, so no two\n"
                              "rows line up.");
        ImGui::SameLine();
        if (ImGui::Checkbox("all", &app.projAllRow)) app.prefsDirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("adds one row over EVERY pixel of the ROI, planes mixed.\n"
                              "It is a different measurement from the per-plane rows,\n"
                              "not a fifth plane - mixing planes inside a per-plane\n"
                              "figure is the thing the plane split exists to prevent.");
        if (anySide) {
            ImGui::SameLine();
            static const char* ORD[2] = { "side, then ch", "ch, then side" };
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            if (ImGui::BeginCombo("order##projstat", ORD[std::clamp(app.projStatOrder, 0, 1)])) {
                for (int i = 0; i < 2; i++)
                    if (ImGui::Selectable(ORD[i], app.projStatOrder == i)) {
                        app.projStatOrder = i;
                        app.prefsDirty = true;
                    }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("\"ch, then side\" puts R:A directly above R:B, which is\n"
                                  "what you want when comparing ONE plane across sides.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy table (TSV)##projstat")) {
            // The SAME walk both layouts draw from, so the clipboard can never
            // disagree with the screen about which rows exist or their order.
            // Full precision regardless of the display decimals: the display
            // setting is about scanning columns, the clipboard is about
            // reproducing numbers.
            std::string tsv = "# projection profile statistics";
            if (P.roiUsed) {
                char r[96];
                snprintf(r, sizeof r, "; ROI (%d,%d) %dx%d", P.rx, P.ry, P.rw, P.rh);
                tsv += r;
            } else tsv += "; whole image";
            const char* pmodes[3] = { "mean", "max", "min" };
            tsv += std::string("; reduce=") + pmodes[std::clamp(app.projMode, 0, 2)] + "; DN\n";
            tsv += "side\tch\tmean\tsigma_frame\tsigma_row\tsigma_col\t"
                   "sigma_frame_pct\tsigma_row_pct\tsigma_col_pct\t"
                   "pp_frame\tpp_row\tpp_col\n";
            int nRows = 0;
            projForEachRow([&](const App::ProjState& S, const char* side, int i) {
                const App::ProjState::Stats& f = S.fStat[i];
                const App::ProjState::Stats& vv = S.vStat[i];
                const App::ProjState::Stats& hh = S.hStat[i];
                if (!f.valid && !hh.valid && !vv.valid) return;
                char b[512];
                auto n6 = [](const App::ProjState::Stats& st, double v, char* out, size_t cap) {
                    if (st.valid) snprintf(out, cap, "%.9g", v);
                    else          snprintf(out, cap, "-");
                };
                char m[40], sf[40], sv[40], sh[40], pf[40], pv2[40], ph[40], qf[40], qv[40], qh[40];
                n6(f, f.mean, m, sizeof m);
                n6(f, f.sd, sf, sizeof sf);  n6(vv, vv.sd, sv, sizeof sv);  n6(hh, hh.sd, sh, sizeof sh);
                n6(f, f.pct, qf, sizeof qf); n6(vv, vv.pct, qv, sizeof qv); n6(hh, hh.pct, qh, sizeof qh);
                n6(f, f.pp, pf, sizeof pf);  n6(vv, vv.pp, pv2, sizeof pv2); n6(hh, hh.pp, ph, sizeof ph);
                // a stale side's numbers carry the caveat INTO the paste - a
                // dimmed pixel does not survive the clipboard, the word does
                snprintf(b, sizeof b, "%s%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                         side, sideIsStale(side) ? " (stale)" : "",
                         S.seriesNames[i], m, sf, sv, sh, qf, qv, qh, pf, pv2, ph);
                tsv += b;
                nRows++;
            });
            ImGui::SetClipboardText(tsv.c_str());
            toast("copied " + std::to_string(nRows) + " row(s) (TSV)");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("the table as tab-separated text, one row per side/plane,\n"
                              "with a provenance line. Full precision - the decimals\n"
                              "setting only affects the display.");
    }
    if (wide) {
        // sigma_f = the REGION's own sigma (how much the pixels differ)
        // sigma_v = sigma of the row means    (horizontal banding / row FPN)
        // sigma_h = sigma of the column means (vertical striping / column FPN)
        // side? + ch + the ten value columns below - keep this equal to the
        // number of TableSetupColumn calls or ImGui asserts and the table is
        // drawn wrong. It went out of step when "mean" was added back.
        int wCols = (anySide ? 1 : 0) + 1 + 10;
        if (ImGui::BeginTable("projstatw", wCols, ImGuiTableFlags_SizingFixedFit |
                                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX)) {
            if (anySide)
                ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::GetFontSize() * 1.6f);
            ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
            // ONE mean, not three: over a rectangle the region mean, the mean
            // of the row means and the mean of the column means are the same
            // number. The old per-axis table printed it twice.
            // the literal is split after each escape: "\xcf\x83f" would be read
            // as \xcf followed by the 3-digit escape \x83f, which is not a byte
            // Spelled out rather than initialled. "sigma-f" needs the footnote
            // to be read at all; "sigma (frame)" is read on sight, and these
            // are numbers people copy into a report where the header travels
            // with them.
            static const char* WH[10] = { "mean",
                                          SIGMA " (frame)", SIGMA " (row)", SIGMA " (col)",
                                          SIGMA " (frame) %", SIGMA " (row) %", SIGMA " (col) %",
                                          "p-p frame", "p-p row", "p-p col" };
            for (int c = 0; c < 10; c++)
                ImGui::TableSetupColumn(WH[c], ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableHeadersRow();
            // the header abbreviations are not self-evident, and this table is
            // the one people will read numbers off into a report
            if (ImGui::TableGetHoveredColumn() >= (anySide ? 2 : 1))
                ImGui::SetTooltip("f = the whole region     v = the row means (horizontal banding)\n"
                                  "h = the column means (vertical striping)\n"
                                  "%% is sigma / mean.  p-p is max - min.  All values in DN.");
            auto wrow1 = [&](const App::ProjState& S, const char* sideName, int i) {
                {
                    const App::ProjState::Stats& f = S.fStat[i];
                    const App::ProjState::Stats& hh = S.hStat[i];
                    const App::ProjState::Stats& vv = S.vStat[i];
                    if (!f.valid && !hh.valid && !vv.valid) return;
                    ImGui::TableNextRow();
                    if (anySide) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sideName); }
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", S.seriesNames[i]);
                    ImGui::TableNextColumn();
                    if (f.valid)       textStat(f.mean);
                    else if (hh.valid) textStat(hh.mean);
                    else               ImGui::TextDisabled("-");
                    const App::ProjState::Stats* trio[3] = { &f, &vv, &hh };
                    for (int k = 0; k < 3; k++) {
                        ImGui::TableNextColumn();
                        if (trio[k]->valid) textStat(trio[k]->sd); else ImGui::TextDisabled("-");
                    }
                    for (int k = 0; k < 3; k++) {
                        ImGui::TableNextColumn();
                        if (trio[k]->valid) textStat(trio[k]->pct); else ImGui::TextDisabled("-");
                    }
                    for (int k = 0; k < 3; k++) {
                        ImGui::TableNextColumn();
                        if (trio[k]->valid) textStat(trio[k]->pp); else ImGui::TextDisabled("-");
                    }
                }
            };
            projForEachRow([&](const App::ProjState& S, const char* side, int i) {
                const bool st = sideIsStale(side);
                if (st) ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                wrow1(S, side, i);
                if (st) ImGui::PopStyleColor();
            });
            ImGui::EndTable();
        }
        ImGui::TextDisabled("frame = the whole region; row = the row means (banding); "
                            "col = the column means (striping). DN");
        projSayPlaneMismatch();
    } else
    {
    int nCols = (anySide ? 3 : 2) + 5;
    if (ImGui::BeginTable("projstats", nCols, ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX)) {
        // one row per (side, axis, plane). The table's axis is the QUANTITY, so
        // A and B cannot be a column pair here - they are rows, and the side
        // column says which is which (docs/ab-stats-plan.md 4).
        if (anySide)
            ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 1.6f);
        ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
        // sigma-f rides next to the mean on every row: the profile sigma beside
        // it is a spread of MEANS, and without the region's own sigma there is
        // nothing to judge it against. The axis column already says which
        // profile the bare sigma belongs to, so it needs no letter of its own -
        // naming it "sigma h,v" would say both when each row is only one.
        ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(SIGMA " (frame)", ImGuiTableColumnFlags_WidthFixed, numColW());
        // A bare sigma here means "of the means named in the axis column", and
        // nothing on the header said so. Each row IS one axis, so the header
        // cannot name a single one - it points at the axis column instead, and
        // matches the two columns after it rather than reading "this axis".
        ImGui::TableSetupColumn(SIGMA " (axis)", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(SIGMA " (axis) %", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("p-p (axis)", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        auto rows1 = [&](const App::ProjState& S, const char* sideName, int s, bool horizontal) {
            {
                const App::ProjState::Stats& st = horizontal ? S.hStat[s] : S.vStat[s];
                if (!st.valid) return;
                ImGui::TableNextRow();
                if (anySide) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sideName); }
                ImGui::TableNextColumn(); ImGui::TextDisabled(horizontal ? "H (col)" : "V (row)");
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", S.seriesNames[s]);
                ImGui::TableNextColumn(); textStat(st.mean);
                ImGui::TableNextColumn();
                if (S.fStat[s].valid) textStat(S.fStat[s].sd);
                else                  ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); textStat(st.sd);
                ImGui::TableNextColumn(); textStat(st.pct);
                ImGui::TableNextColumn(); textStat(st.pp);
            }
        };
        auto dimmed = [&](const App::ProjState& S, const char* side, int i, bool horiz) {
            const bool st = sideIsStale(side);
            if (st) ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            rows1(S, side, i, horiz);
            if (st) ImGui::PopStyleColor();
        };
        if (app.showProjH)
            projForEachRow([&](const App::ProjState& S, const char* side, int i) {
                dimmed(S, side, i, true);
            });
        if (app.showProjV)
            projForEachRow([&](const App::ProjState& S, const char* side, int i) {
                dimmed(S, side, i, false);
            });
        ImGui::EndTable();
    }
    ImGui::TextDisabled(SIGMA " (frame) = the whole region.  " SIGMA " (axis) = of the means "
                        "in the axis column:\n"
                        "column means = column FPN, row means = row FPN. DN");
    projSayPlaneMismatch();
    }
}

// ---- linearity ---------------------------------------------------------------
// The folder structure IS the experiment: each stack was captured at one
// exposure level, so level comes from the stack (auto-read from its name,
// editable), the response comes from the frames, and the fit answers the two
// questions a sensor evaluation starts with - is the response linear, and what
// is the conversion gain. CFA planes stay separate throughout: mixing them
// turns channel sensitivity differences into fake nonlinearity.

// "100lx/frame_###.npy" -> 100; "00/f_###" -> 0; "run42_200ms.npy [remote]" -> 42.
// Rule: the first number in the FOLDER part when there is one, else in the whole
// name. Deliberately simple - this is only a SUGGESTION a human confirms, and
// srcOut reports the text it was read out of so the modal can show its work.
// (the default for srcOut is on the forward declaration, with the model.)
static double extractLevelFromName(const std::string& name, std::string* srcOut) {
    std::string part = name;
    size_t slash = part.find_last_of('/');
    if (slash != std::string::npos && slash > 0) part = part.substr(0, slash);
    else {
        // No folder qualifier (a stack opened straight out of its own folder):
        // the FILE part is all there is, and its frame-axis extent is a frame
        // count, never an illuminance. Cut the extent run out before reading -
        // "0000..0003.npy" must stay "no level here", exactly as "????.npy"
        // did before the extent replaced the wildcard.
        size_t d = part.find("\xE2\x80\xA5");
        if (d != std::string::npos) {
            size_t a = d, b = d + 3;
            while (a > 0 && isdigit((unsigned char)part[a - 1])) a--;
            while (b < part.size() && isdigit((unsigned char)part[b])) b++;
            part.erase(a, b - a);
        }
    }
    if (srcOut) srcOut->clear();
    for (size_t i = 0; i < part.size(); i++) {
        if (isdigit((unsigned char)part[i])) {
            // avoid the "###" pattern placeholder and sizes like 640x480
            if (srcOut) *srcOut = part;
            return atof(part.c_str() + i);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

struct StackStats {
    bool valid = false;
    int frames = 0, nPl = 1;
    double mean[4] = {}, sigmaT[4] = {};
    uint64_t nonFinite = 0;       // samples EXCLUDED, reported not hidden
    size_t dropped = 0;           // pixels with fewer than 2 valid frames
    size_t unknown = 0;           // samples with NO valid frame: the mean does not exist
    // Per-sample valid-count extremes over the region. Equal = every sample
    // folded the same number of frames; unequal is exactly what the SUM fold
    // must declare (docs/analysis-layers.md §3.2: sums over different counts
    // are different quantities pixel to pixel).
    uint32_t cntMin = 0, cntMax = 0;
    std::string err;
};

// Per-plane mean and temporal sigma of one stack's resident frames, over the
// given ROI. The same statistic the server aggregate computes, evaluated on
// whatever is local - which covers NAS data and fully fetched remote stacks.
//
// foldOut (optional): the PER-PIXEL temporal fold over the ROI, interleaved
// exactly like a frame's pixels (rw*rh*C floats) - the mean by default, the
// sum when `fold` says so (docs/analysis-layers.md §3.2: the fold is a
// choice, and both come off ONE accumulator). This is the picture "Open frame
// average" / "Open frame sum" opens, and it comes out of THIS accumulator on
// purpose - the mean drawn on screen and the mean printed in the Temporal
// table are then the same number by construction, not by two functions
// agreeing today. (Doing it twice is the defect this codebase keeps finding:
// PRNU twice, naturalLess versus compare, the channel rule in three places.)
// A sample no frame had a finite value for is NaN there - unknown, and said
// so, rather than 0, which would be a measurement.
static StackStats computeStackStats(int seqId, int rx, int ry, int rw, int rh,
                                    std::vector<float>* foldOut = nullptr,
                                    int fold = FOLD_MEAN) {
    StackStats out;
    std::vector<int> fr = framesOfSeq(seqId);
    const ImageDoc* first = nullptr;
    std::vector<const ImageDoc*> use;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->src->remoteStep > 1 || d->px().empty()) continue;   // previews lie
        if (!first) first = d;
        if (d->w != first->w || d->h != first->h || d->ch != first->ch) continue;
        use.push_back(d);
    }
    if (use.size() < 2) { out.err = "needs >= 2 loaded frames"; return out; }
    int W = first->w, H = first->h, C = first->ch;
    rx = std::clamp(rx, 0, W - 1); ry = std::clamp(ry, 0, H - 1);
    rw = rw <= 0 ? W - rx : std::min(rw, W - rx);
    rh = rh <= 0 ? H - ry : std::min(rh, H - ry);
    size_t samples = (size_t)rw * rh * C;
    if (samples > (size_t)32 << 20) { out.err = "ROI too large (max 32M samples)"; return out; }
    // Per-sample valid counts. A NaN must be EXCLUDED, not folded in: skipping
    // it in the accumulator while keeping N as the divisor pulls the mean
    // toward 0 by (N-k)/N and corrupts the variance in the same direction -
    // understated noise that looks perfectly plausible. serve.cpp has done it
    // this way since it was written ("Silent optimism is the worst failure");
    // this is the same arithmetic, so the two sides now agree in VALUE and in
    // what they state.
    std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
    std::vector<uint32_t> cnt(samples, 0);
    for (const ImageDoc* d : use)
        for (int y = 0; y < rh; y++) {
            const float* row = &d->px()[((size_t)(ry + y) * W + rx) * C];
            double* s1 = &sum[(size_t)y * rw * C];
            double* s2 = &sum2[(size_t)y * rw * C];
            uint32_t* cn = &cnt[(size_t)y * rw * C];
            for (size_t i = 0; i < (size_t)rw * C; i++) {
                double v = row[i];
                if (!std::isfinite(v)) { out.nonFinite++; continue; }
                s1[i] += v; s2[i] += v * v; cn[i]++;
            }
        }
    if (foldOut) {
        // float32 out, DOUBLE all the way in. sum[] is double for exactly this
        // reason: adding hundreds of float32 samples in float32 loses low bits
        // at up to half a ULP per add, and a dark-current map IS its low bits -
        // the whole point of averaging N frames is to see what one frame's noise
        // buries. A pixel no frame had a finite value for comes out NaN, which
        // every panel already reads as "not measured"; 0 would be a measurement.
        // The sum is the same accumulator without the divide - which is why it
        // shares the mean's precision and the mean's NaN discipline for free.
        foldOut->resize(samples);
        for (size_t i = 0; i < samples; i++)
            (*foldOut)[i] = cnt[i]
                ? (float)(fold == FOLD_SUM ? sum[i] : sum[i] / (double)cnt[i])
                : std::numeric_limits<float>::quiet_NaN();
    }
    out.nPl = first->cfa ? 4 : 1;
    double plM[4] = {}, plV[4] = {};
    size_t plC[4] = {};
    uint32_t cmin = std::numeric_limits<uint32_t>::max(), cmax = 0;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int p = first->cfa ? cfaChannelAt(*first, rx + x, ry + y) : 0;
            for (int c = 0; c < C; c++) {
                size_t i = ((size_t)y * rw + x) * C + c;
                cmin = std::min(cmin, cnt[i]); cmax = std::max(cmax, cnt[i]);
                double nI = (double)cnt[i];
                if (nI < 1) out.unknown++;                 // and no mean either
                if (nI < 2) { out.dropped++; continue; }   // no variance to speak of
                double m = sum[i] / nI;
                double var = std::max(0.0, sum2[i] / nI - m * m) * (nI / (nI - 1.0));
                plM[p] += m; plV[p] += var; plC[p]++;
            }
        }
    for (int p = 0; p < out.nPl; p++) {
        if (!plC[p]) continue;
        out.mean[p] = plM[p] / (double)plC[p];
        out.sigmaT[p] = sqrt(plV[p] / (double)plC[p]);
    }
    out.cntMin = cmin == std::numeric_limits<uint32_t>::max() ? 0 : cmin;
    out.cntMax = cmax;
    out.frames = (int)use.size();
    out.valid = true;
    return out;
}

// The FRAME member's response (docs/analysis-layers.md §3.3): the spatial mean
// per plane of ONE standalone frame over the ROI, in the same StackStats
// vessel so a mixed member table has one shape. frames = 1, sigmaT stays 0 and
// the row that carries this says hasSigma = false - a single frame has no time
// axis, and the canon forbids dressing anything up as its sigma_t. The plane
// discipline (CFA by position, non-CFA pooled into plane 0) and the per-sample
// NaN exclusion (counted, never folded into a divisor) are computeStackStats'
// own, so a frame member's mean and a stack member's mean are the same
// quantity by construction.
static StackStats computeFrameStats(const ImageDoc* d, int rx, int ry, int rw, int rh) {
    StackStats out;
    if (!d || d->px().empty() || d->src->remoteStep > 1) {
        out.err = "frame not resident";
        return out;
    }
    int W = d->w, H = d->h, C = d->ch;
    rx = std::clamp(rx, 0, W - 1);
    ry = std::clamp(ry, 0, H - 1);
    rw = rw <= 0 ? W - rx : std::min(rw, W - rx);
    rh = rh <= 0 ? H - ry : std::min(rh, H - ry);
    out.nPl = d->cfa ? 4 : 1;
    double sum[4] = {};
    size_t cnt[4] = {};
    for (int y = 0; y < rh; y++) {
        const float* row = &d->px()[((size_t)(ry + y) * W + rx) * C];
        for (int x = 0; x < rw; x++) {
            int p = d->cfa ? cfaChannelAt(*d, rx + x, ry + y) : 0;
            for (int c = 0; c < C; c++) {
                double v = row[(size_t)x * C + c];
                if (!std::isfinite(v)) { out.nonFinite++; continue; }
                sum[p] += v;
                cnt[p]++;
            }
        }
    }
    size_t total = 0;
    for (int p = 0; p < out.nPl; p++) total += cnt[p];
    if (!total) { out.err = "no finite samples in the region"; return out; }
    for (int p = 0; p < out.nPl; p++)
        if (cnt[p]) out.mean[p] = sum[p] / (double)cnt[p];
    out.frames = 1;
    out.cntMin = out.cntMax = 1;
    out.valid = true;
    return out;
}

// ---- frame average / frame sum: a stack's time axis, folded to ONE frame ----
// One tooltip for every door into it: the five Browse entry points and the
// Files panel item must not describe the same operation five different ways.
// non-static since P7: the Browse ctx menus show the same tooltip (host.h)
const char* AVG_TIP =
    "Open the stack's PER-PIXEL MEAN across frames as one frame -\n"
    "a dark-current map, a flat field, the level sigma_t is the noise\n"
    "around. Accumulated in double; non-finite samples are excluded\n"
    "per pixel and counted. A mosaic stays a mosaic. The result is a\n"
    "computed frame, not a capture, and says so in its name and note;\n"
    "the stack itself stays open beside it.";
// ...and the sum fold beside it (docs/analysis-layers.md §3.2: the fold is a
// mean / sum CHOICE). Static, unlike AVG_TIP: Browse's doors say "average" and
// keep meaning it - the sum is selected where the open stack lives, in Files.
static const char* SUM_TIP =
    "Open the stack's PER-PIXEL SUM across frames as one frame -\n"
    "the same accumulator as the frame average, without the divide.\n"
    "Accumulated in double; non-finite samples are excluded per pixel\n"
    "and counted, so pixels with NaN frames sum FEWER terms - the\n"
    "note declares the per-pixel count range and a warning names the\n"
    "exclusions. For comparing pixels, prefer the average.";

// A STACK-level operation whose VALUE is a FRAME. docs/terminology.md makes that
// an ordinary frame hanging straight off the source's batch ("単発 frame は stack
// を経由せず batch に直接ぶら下がってよい"), seqId 0 - so Histogram, Projection
// and ROIs work on it because there is nothing here for them to special-case.
// The picture is the point: a dark-current map, a flat field, the thing sigma_t
// is the noise AROUND. Getting it today means opening N frames and leaving.
//
// THE FOLD IS A CHOICE - mean or sum, mean the default (docs/analysis-layers.md
// §3.2, 判断record 3). The original ruling draft banned the sum because NaN
// exclusion gives it a per-pixel population; the user's correction stands: the
// mean has the same varying population, normalization only hides it. So
// honesty is by DECLARATION, for both folds alike - NaN present raises a
// warning, the exclusion count and the valid frame count always travel with
// the result, and the sum additionally declares its per-pixel count range,
// because sums over different counts are different quantities pixel to pixel
// (the per-pixel count MAP has no vessel until #49; the range in the note is
// the declaration this build can already make).
//
// PROVENANCE IS THE HARD PART. This image looks exactly like a capture and will
// end up in a report as one if it does not keep saying otherwise. So it says it
//   - in its NAME, which is what the Files panel and every title bar show,
//   - in its NOTE, which is what the Inspector shows (and the Files tooltip,
//     which now falls back to the note when there is no path to print),
//   - and through a session, by avgOfPath below.
// src->path stays EMPTY - deliberately, and this is the load-bearing line. Give
// it the source file's path, the way materializeDerivedFrame does, and
// isSavedLine writes an ordinary `image` line; the next session then reloads
// frame 0 from disk and presents ONE CAPTURE as the mean, under this name, with
// no note left to contradict it. Empty path means the session cannot save the
// pixels, so it saves the RECIPE instead and takes the mean again.
//
// THE SOURCE STACK STAYS OPEN. Averaging reads every frame to yield one, so
// closing the source afterwards would look like a saving. It is not: the note
// names that stack, and provenance pointing at something the program silently
// destroyed is worth less than the memory. Close stack is one click away in the
// same menu, and there the user can see what they are closing.
// `expect` is a session's generation record (§10.2): compare what we fold
// against what the saved picture was folded from, and SAY when they differ.
// `into` is Recompute (§10.1): fold into this existing reduced frame instead
// of opening a new one - same uid, same batch, same display range; only the
// pixels and the record move, and the stale mark clears because the record is
// now the current key.
static bool stackReduceFrame(int seqId, int fold, std::string& err,
                             const App::PendingAvg* expect = nullptr,
                             ImageDoc* into = nullptr) {
    const bool sumF = fold == FOLD_SUM;
    const char* noun = sumF ? "sum" : "average";   // the fold IS the name
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) { err = "that stack is not open"; return false; }
    const std::string srcName = si->name;
    const int expected = si->expectedFrames;
    const int srcRev = si->stackRev;   // the §10.1 key, at compute time
    std::vector<int> fr = framesOfSeq(seqId);
    // The reference frame is the first RESIDENT full-resolution one - the same
    // frame computeStackStats measures against, since a remote preview is a
    // decimated stand-in and its pixels are not this stack's pixels.
    const ImageDoc* ref = nullptr;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->src->remoteStep > 1 || d->px().empty()) continue;
        ref = d;
        break;
    }
    if (!ref) { err = "no frame of \"" + srcName + "\" is loaded yet"; return false; }
    // Everything read off ref or si must be COPIED OUT before addImage: the
    // push_back below can reallocate app.images and both pointers dangle.
    const int W = ref->w, H = ref->h, C = ref->ch;
    const int refCfa = ref->cfa, refCfaPat = ref->cfaPattern, refLut = ref->displayLut;
    const bool refColorize = ref->cfaColorize;
    const float refBlack = ref->black, refWhite = ref->white;
    const int refBatch = ref->batchId;
    // The stack's FIRST frame names it for the session, resident or not - the
    // same key Series members travel by, for the same reason (paths round-trip;
    // ids do not survive a session and names are renameable and not unique).
    const std::string firstPath = app.images[fr.front()]->src->path;
    // ...and WHICH member of that file, when it has members: two 3-D members of
    // one .npz share this path and §6.2's tuple tells them apart by nothing
    // else, so a path-only recipe folded whichever of them came first.
    const std::string firstMember = app.images[fr.front()]->src->member;
    // The INPUT TUPLE record (§10.2 / §6.2): one digest over the identity
    // tuple of every member the fold will read - (path or url, member,
    // fileFrame, remoteFrame, mtime, fsize), each statted at decode time, so
    // the record describes the pixels that actually went in, not whatever the
    // disk holds at some later moment. The member walk is computeStackStats's
    // own `use` selection verbatim (previews and ragged frames excluded), so
    // the digest counts exactly the frames the numbers count.
    uint64_t srcDig = 1469598103934665603ull;      // FNV-1a 64 offset basis
    int srcDigN = 0;
    {
        auto digAdd = [&srcDig](const void* p, size_t n) {
            const uint8_t* b = (const uint8_t*)p;
            for (size_t i = 0; i < n; i++) { srcDig ^= b[i]; srcDig *= 1099511628211ull; }
        };
        const ImageDoc* first = nullptr;
        for (int idx : fr) {
            const ImageDoc* d = app.images[idx].get();
            if (d->src->remoteStep > 1 || d->px().empty()) continue;
            if (!first) first = d;
            if (d->w != first->w || d->h != first->h || d->ch != first->ch) continue;
            const FrameSource& s = *d->src;
            digAdd(s.path.data(), s.path.size());
            digAdd(s.remoteUrl.data(), s.remoteUrl.size());
            digAdd(s.member.data(), s.member.size());
            digAdd(&s.fileFrame, sizeof s.fileFrame);
            digAdd(&s.remoteFrame, sizeof s.remoteFrame);
            digAdd(&s.mtime, sizeof s.mtime);
            digAdd(&s.fsize, sizeof s.fsize);
            srcDigN++;
        }
    }
    char whenTs[32] = "";                          // "computed WHEN" - #82's 4th
    {                                              // question was exactly this
        time_t nowT = time(nullptr);
        if (struct tm* lt = localtime(&nowT))
            strftime(whenTs, sizeof whenTs, "%Y-%m-%d %H:%M:%S", lt);
    }
    std::vector<float> folded;
    // ONE accumulator in the program, for either fold. This is the one the
    // Temporal panel's mean and sigma_t come out of, so the number under the
    // picture and the number in the table cannot disagree - see
    // computeStackStats. The sum is the same pass without the divide.
    StackStats st = computeStackStats(seqId, 0, 0, W, H, &folded, fold);
    if (!st.valid) {
        err = "frame " + std::string(noun) + " of \"" + srcName + "\": " + st.err;
        return false;
    }
    const int n = st.frames;
    const int N = std::max(expected, n);
    const std::string ofN = n < N ? std::to_string(n) + " of " + std::to_string(N)
                                  : std::to_string(n);
    const std::string rName = srcName + "  frame " + noun + " (" + ofN + " frames)";
    // docs/terminology.md, 実装上の不変条件: "部分ロードされた stack の集計は
    // 「何枚中何枚か」を必ず併記する". The canon says STATE the ratio, not refuse
    // the aggregate - every other stack-level statistic in the program (montage,
    // Temporal, the linearity rows) already reads that way, and a mean over 8 of
    // 24 frames is a real measurement of 8 frames. What it must never do is call
    // itself "the average", so the count is in the name, in the note, and in a
    // warning toast; and the count is of frames actually AVERAGED (st.frames),
    // not of frames present, so a ragged or preview frame cannot inflate it.
    std::string note = "frame " + std::string(noun) + " of \"" + srcName +
                       "\": per-pixel " + (sumF ? "sum" : "mean") + " over " +
                       ofN + " frame(s), accumulated in double";
    if (st.nonFinite)
        note += "; " + std::to_string((unsigned long long)st.nonFinite) +
                (sumF ? " non-finite sample(s) EXCLUDED - each pixel sums only "
                        "its own finite frames"
                      : " non-finite sample(s) EXCLUDED - each pixel's own denominator");
    // §3.2's one caution that is the SUM's alone: per-pixel exclusion gives it
    // a per-pixel term count, and two sums over different counts are different
    // quantities. The range is the declaration (the count MAP waits on #49's
    // vessel); the mean needs none, its varying population is already priced
    // into each pixel's own denominator above.
    if (sumF && st.cntMin != st.cntMax)
        note += "; per-pixel valid frame counts vary (" + std::to_string(st.cntMin) +
                ".." + std::to_string(st.cntMax) + " of " + std::to_string(n) +
                ") - sums over different counts are different quantities pixel to pixel";
    if (st.unknown)
        note += "; " + std::to_string((unsigned long long)st.unknown) +
                " sample(s) had no finite frame at all and are NaN here";
    if (n < N)
        note += "; the stack is PARTLY LOADED - this is the " +
                std::string(sumF ? "sum" : "mean") + " of the " +
                std::to_string(n) + " frame(s) present, not of all " + std::to_string(N);
    if (firstPath.empty())
        note += "; no file path behind the source stack, so a session cannot "
                "recompute this frame";
    // WHEN this picture was read off its stack - #82's fourth question was
    // that the note said "N frames of <stack>" but never when. Part of the
    // note, not decoration: this is the date a report will cite.
    note += "; computed " + std::string(whenTs);
    // §10.2, the restore's one comparison: the recipe recomputes from whatever
    // the disk holds today (existing semantics, unchanged) - but if the input
    // tuples no longer match the record the session carried, this is a
    // DIFFERENT picture under the same name, and it says so instead of
    // silently standing in for the one the report cited.
    const bool genChanged = expect && expect->hasGen &&
        (expect->srcDigest != srcDig || expect->srcCount != srcDigN);
    if (genChanged)
        note += "; recomputed from newer files (source changed since " +
                (expect->when.empty() ? std::string("it was saved") : expect->when) + ")";

    if (into) {
        // Recompute (§10.1): the user's one move, into the SAME doc - uid,
        // batch, compare seats and the user's display range stay; the pixels,
        // the provenance and the generation record move. The source is private
        // by construction (path empty, outside the §6.2 registry), so writing
        // it in place cannot reach any other doc.
        FrameSource& S = *into->src;
        S.w = W; S.h = H; S.ch = C;
        S.dtype = "f32";
        S.data = std::move(folded);
        into->cfa = refCfa; into->cfaPattern = refCfaPat; into->cfaColorize = refColorize;
        computeMinMax(*into);
        into->syncMirrors();
        into->name = rName;
        into->note = note;
        into->avgOfPath = firstPath;
        into->avgOfMember = firstMember;
        into->avgFold = fold;
        into->avgSeqId = seqId;
        into->avgStackRev = srcRev;
        into->avgSrcDigest = srcDig;
        into->avgSrcCount = srcDigN;
        into->avgWhen = whenTs;
        into->avgStale = false;          // the record IS the current key again
        into->avgGenChanged = false;
        into->dataRev++;                 // hist/proj/ROI/diff keys unlatch
        forgetImage(into);
        into->texDirty = true;
        fprintf(stderr, "frame %s: %s - recomputed, %d of %d frame(s), %dx%d %dch\n",
                noun, srcName.c_str(), n, N, W, H, C);
        toast("frame " + std::string(noun) + " of \"" + srcName +
              "\": recomputed over " + ofN + " frame(s)");
        if (st.nonFinite)
            toast("frame " + std::string(noun) + " of \"" + srcName + "\": NaN present - " +
                  std::to_string((unsigned long long)st.nonFinite) +
                  " non-finite sample(s) excluded per pixel; " + (sumF ? "sum" : "mean") +
                  " over " + ofN + " frame(s)", true);
        return true;
    }

    auto out = std::make_unique<ImageDoc>();
    FrameSource& S = *out->src;
    S.w = W; S.h = H; S.ch = C;
    // NOT the source dtype. The mean of N integers is not an integer, and
    // labelling fractional DN "u16" would misdescribe every number printed off
    // this frame. Same call montageROI makes when its values stop being DN.
    S.dtype = "f32";
    S.data = std::move(folded);
    // The mean is PER PIXEL: nothing moves, nothing resamples, every pixel keeps
    // its coordinate and therefore its Bayer phase. So a Bayer stack's mean IS
    // still Bayer and the declaration travels verbatim - per-plane statistics on
    // the result are the per-plane statistics of the stack, which is the whole
    // reason a mosaic must never be silently dropped here.
    out->cfa = refCfa; out->cfaPattern = refCfaPat; out->cfaColorize = refColorize;
    out->displayLut = refLut;
    out->syncMirrors();
    out->batchId = refBatch;
    out->name = rName;
    out->note = note;
    out->avgOfPath = firstPath;
    out->avgOfMember = firstMember;
    out->avgFold = fold;             // the recipe must round-trip its QUANTITY
    out->avgSeqId = seqId;           // the §10.1 key this doc will be judged by
    out->avgStackRev = srcRev;
    out->avgSrcDigest = srcDig;      // ...and the §10.2 record a session carries
    out->avgSrcCount = srcDigN;
    out->avgWhen = whenTs;
    out->avgGenChanged = genChanged;
    out->texDirty = true;
    fprintf(stderr, "frame %s: %s - %d of %d frame(s), %dx%d %dch, cfa=%d, "
                    "%llu excluded, %llu unknown\n", noun, srcName.c_str(), n, N, W, H,
            C, refCfa, (unsigned long long)st.nonFinite, (unsigned long long)st.unknown);
    addImage(std::move(out));
    // addImage runs defaultRange, which is right for a file just opened and
    // wrong here. Averaging suppresses exactly the spread an auto-range keys
    // off, so the mean would come back stretched to full contrast over its own
    // residue and look nothing like the frames it summarises. It inherits the
    // source's display range instead, and is directly comparable with them.
    ImageDoc* made = app.images.back().get();
    // The sum is the mean's picture at n times the scale, so it inherits the
    // same range times the frames it folded - comparable with its sources the
    // way the mean is, instead of clipping solid white against theirs.
    const float rangeScale = sumF ? (float)std::max(n, 1) : 1.0f;
    made->black = refBlack * rangeScale; made->white = refWhite * rangeScale;
    if (n < N)
        toast("frame " + std::string(noun) + ": " + std::to_string(n) + " of " +
              std::to_string(N) + " frames are loaded - the result says so", true);
    else
        toast("frame " + std::string(noun) + " of \"" + srcName + "\": " +
              std::to_string(n) + " frames");
    // §3.2 (判断record 3): NaN 混入時は Warning. The note above is where the
    // count lives permanently; this is what makes it SEEN at reduction time -
    // exclusion count and valid frame count together, the same declaration the
    // frame average has always written, now impossible to not notice. Fired
    // last, so it is the toast left standing (both land in the Messages log).
    if (st.nonFinite)
        toast("frame " + std::string(noun) + " of \"" + srcName + "\": NaN present - " +
              std::to_string((unsigned long long)st.nonFinite) +
              " non-finite sample(s) excluded per pixel; " + (sumF ? "sum" : "mean") +
              " over " + ofN + " frame(s)", true);
    // The generation statement outranks everything above: a restored picture
    // that is NOT the one the session saved must be the toast left standing
    // (both land in the Messages log). The note carries it permanently.
    if (genChanged)
        toast("frame " + std::string(noun) + " of \"" + srcName +
              "\": recomputed from newer files - the source changed since " +
              (expect->when.empty() ? std::string("it was saved") : expect->when), true);
    return true;
}

// Everything settled: no local loader running, no decoded frame waiting to be
// integrated, no queued open and no remote fetch outstanding anywhere. This is
// the same predicate every selftest's drain loop waits on, and it is the gate a
// parked frame average fires behind - a stack that is still filling has a
// different mean every second, and taking one mid-flight would silently answer
// a question about however many frames happened to have landed.
static bool loadingSettled() {
    return !app.seqRunning && !seqReadyPending() && app.seqQueue.empty() &&
           app.seqRestore.empty() && app.rbOpenQueue.empty() && app.rfPending <= 0;
}

// The ONE door every menu item goes through, so that all of them mean the same
// thing. If everything has landed, the mean is taken now; if anything is still
// arriving, the request waits. That distinction is not about WHERE it was asked
// from - a stack half-open in the Files panel is as unfinished as one Browse has
// only just started fetching, and averaging either of them early would answer a
// question about "however many frames had arrived by the time you clicked".
// n of N therefore appears only when the load really did stop short (the memory
// budget did), which is a fact about the stack rather than about timing.
static void requestStackAverage(int seqId, int fold) {
    if (!seqId) return;
    if (!loadingSettled()) {
        app.pendingAvg.push_back({ seqId, fold, false, 0, 0, {} });
        const App::SeqInfo* si = seqInfo(seqId);
        toast("frame " + std::string(fold == FOLD_SUM ? "sum" : "average") + ": \"" +
              (si ? si->name : std::string("stack")) + "\" will be " +
              (fold == FOLD_SUM ? "summed" : "averaged") + " once its frames are here");
        return;
    }
    std::string err;
    if (!stackReduceFrame(seqId, fold, err)) toast(err, true);
}

// The parked "Open as frame average" / "Open frame sum" requests, from Browse
// and from a session - each carrying its fold, because a parked sum that fired
// as a mean would be the quiet 1/n error the recipe key exists to prevent.
static void pumpStackAverages() {
    if (app.pendingAvg.empty() && app.avgRestore.empty()) return;
    if (!loadingSettled()) return;
    // A session's recipes name their stack by (path, member); resolve them the
    // moment everything is open, exactly as resolvePendingSeries does with
    // members. An empty member is a session written before that key: it matches
    // any member, i.e. resolves exactly as it did then.
    for (const auto& p : app.avgRestore) {
        int found = 0;
        for (const auto& d : app.images)
            if (d->seqId != 0 && d->src->path == p.path &&
                (p.member.empty() || d->src->member == p.member)) { found = d->seqId; break; }
        // the generation record rides to the reduction: the recompute is
        // where the fresh tuples exist to compare against it (§10.2)
        if (found) app.pendingAvg.push_back({ found, p.fold, p.hasGen,
                                              p.srcCount, p.srcDigest, p.when });
        else fprintf(stderr, "session: a frame %s named \"%s\", which is not "
                             "open - not recomputed\n",
                     p.fold == FOLD_SUM ? "sum" : "average", p.path.c_str());
    }
    app.avgRestore.clear();
    std::vector<App::PendingAvg> todo;
    todo.swap(app.pendingAvg);
    for (const auto& id : todo) {
        if (!seqInfo(id.seqId)) continue;        // closed while it was waiting
        std::string err;
        if (!stackReduceFrame(id.seqId, id.fold, err, id.hasGen ? &id : nullptr))
            toast(err, true);
    }
}

// Recompute (§10.1): the user's one move against a stale reduced frame - and
// only the user's. Resolves the source stack by the live seqId first, then by
// the recipe path (a stack reopened after a session has a new id), and folds
// into the SAME doc through the same accumulator every reduction uses.
static bool recomputeStackAverage(ImageDoc* m, std::string& err) {
    if (!m || m->avgOfPath.empty()) { err = "not a computed frame"; return false; }
    int seq = 0;
    if (m->avgSeqId && seqInfo(m->avgSeqId)) seq = m->avgSeqId;
    else
        for (const auto& d : app.images)
            if (d->seqId != 0 && d->src->path == m->avgOfPath &&
                d->src->member == m->avgOfMember) { seq = d->seqId; break; }
    if (!seq) {
        err = "the stack this frame was computed from is not open";
        return false;
    }
    return stackReduceFrame(seq, m->avgFold, err, nullptr, m);
}

// least squares y = a*x + b; returns false with fewer than 2 distinct points
static bool linFit(const std::vector<double>& xs, const std::vector<double>& ys,
                   double& a, double& b, double& r2) {
    size_t n = xs.size();
    if (n < 2) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (size_t i = 0; i < n; i++) {
        sx += xs[i]; sy += ys[i];
        sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; syy += ys[i] * ys[i];
    }
    double d = n * sxx - sx * sx;
    if (fabs(d) < 1e-12) return false;
    a = (n * sxy - sx * sy) / d;
    b = (sy - a * sx) / n;
    double sst = syy - sy * sy / n;
    double ssr = 0;
    for (size_t i = 0; i < n; i++) {
        double e = ys[i] - (a * xs[i] + b);
        ssr += e * e;
    }
    r2 = sst > 1e-12 ? 1.0 - ssr / sst : 1.0;
    return true;
}

// least squares y = a*x + b with per-point weights; r2 is the weighted R^2.
// A sibling of linFit, not a change to it. Born in the Temporal panel's
// interim frame-linearity section and moved here with the analysis (§3.3
// migration): the series fit's rev4-style method is this function, weights
// w_i = 1/y_i^2.
static bool linFitWeighted(const std::vector<double>& xs, const std::vector<double>& ys,
                           const std::vector<double>& ws, double& a, double& b, double& r2) {
    size_t n = xs.size();
    if (n < 2) return false;
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        sw += ws[i]; sx += ws[i] * xs[i]; sy += ws[i] * ys[i];
        sxx += ws[i] * xs[i] * xs[i]; sxy += ws[i] * xs[i] * ys[i];
    }
    if (!(sw > 0)) return false;
    double d = sw * sxx - sx * sx;
    // relative degeneracy test: 1/y^2 weights make the ABSOLUTE determinant
    // tiny on perfectly good data
    if (fabs(d) <= 1e-12 * std::max(sw * sxx, sx * sx)) return false;
    a = (sw * sxy - sx * sy) / d;
    b = (sy - a * sx) / sw;
    double ym = sy / sw, sst = 0, ssr = 0;
    for (size_t i = 0; i < n; i++) {
        double e = ys[i] - (a * xs[i] + b);
        ssr += ws[i] * e * e;
        sst += ws[i] * (ys[i] - ym) * (ys[i] - ym);
    }
    r2 = sst > 1e-12 ? 1.0 - ssr / sst : 1.0;
    return true;
}

static const char* LIN_PLANES[4] = { "R", "Gr", "Gb", "B" };
static const ImU32 LIN_COLS[4] = {
    IM_COL32(255, 92, 92, 255), IM_COL32(79, 221, 107, 255),
    IM_COL32(90, 200, 190, 255), IM_COL32(96, 156, 255, 255),
};

// Everything the panel shows, recomputed only when Compute is pressed: this is
// a measurement action over potentially hundreds of frames, not a per-frame UI.
//
// ONE SERIES, not "every open stack". The old walk over app.seqs meant two
// sweeps open at the same time were silently fitted as one curve, and the
// parameter values had nowhere to live but the stacks themselves. The rows are
// the series' members, in the series' order; include and value come straight
// off Series::Member, so a table row IS members[i] and there is nothing to
// carry across a recompute.
static void linRecompute(int seriesId) {
    App::LinState& L = app.lin;
    L.seriesId = seriesId;
    // the ROI is shared with everything else: the selected rect, or whole frame
    int rx = 0, ry = 0, rw = 0, rh = 0;
    L.roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn))
        if (a->type == 0) { rx = a->x; ry = a->y; rw = a->w; rh = a->h; L.roiUsed = true; }
    L.rows.clear();
    L.refusal.clear();
    L.nZero = 0;
    for (int p = 0; p < 4; p++) {
        L.winPlaneLo[p] = L.winPlaneHi[p] = 0;
        L.nFit[p] = 0;
        L.planeFit[p] = false;
    }
    App::Series* S = seriesById(seriesId);
    if (!S) {
        L.fitValid = false;
        L.nPl = 1; L.nPts = 0;
        L.computedRev = ++L.rev;
        return;
    }
    for (const auto& m : S->members) {
        App::LinState::Row r;
        StackStats st;
        if (m.seqId != 0) {             // a stack member: §3.2's mean statistics
            App::SeqInfo* si = seqInfo(m.seqId);
            if (!si) continue;          // seriesAudit says this cannot happen
            r.seqId = m.seqId;
            r.name = si->name;
            r.fold = m.fold;
            st = computeStackStats(m.seqId, rx, ry, rw, rh);
            r.hasSigma = st.valid && st.frames >= 2;
        } else {                        // a frame member IS its own point (§3.3)
            ImageDoc* fd = frameMemberDoc(m.frameUid);
            if (!fd) continue;          // seriesAudit says this cannot happen
            r.uid = m.frameUid;
            r.isFrame = true;
            r.name = fd->name;
            st = computeFrameStats(fd, rx, ry, rw, rh);
        }
        r.level = m.value;
        r.include = m.include;
        r.valid = st.valid;
        r.err = st.err;
        r.frames = st.frames;
        r.nPl = st.nPl;
        r.nonFinite = st.nonFinite;
        for (int p = 0; p < 4; p++) { r.mean[p] = st.mean[p]; r.sigmaT[p] = st.sigmaT[p]; }
        L.rows.push_back(std::move(r));
    }
    // fits, per plane, over rows that have both a value and a measurement
    L.fitValid = false;
    L.nPl = 1;
    for (const auto& r : L.rows) if (r.valid) L.nPl = std::max(L.nPl, r.nPl);
    int fitted = 0;
    // §3.3: the fit takes MEAN statistics only. A sum-standing member is
    // refused BEFORE anything fits - a sum is n x the mean's scale, so with
    // differing n the points are different quantities point to point - and it
    // is refused OUT LOUD, never quietly read as the mean it was not declared
    // to be. The rows above are still measured: mean statistics are honest
    // for both folds, it is only the fit's axis they must not share.
    for (const auto& r : L.rows) {
        if (r.isFrame || r.fold != FOLD_SUM) continue;
        if (!r.include || !std::isfinite(r.level)) continue;
        L.refusal = "fit refused: \"" + r.name + "\" stands as sum - a sum is n x the "
                    "mean's scale, so with differing n the points are different "
                    "quantities. Set it to mean (or exclude it) to fit.";
        break;
    }
    // No unit, no fit - and no fit for a kind these equations do not describe.
    // The per-member MEANS above are DN either way and stay on screen; a
    // sensitivity is DN per SOMETHING measured by a KNOWN law, and inventing
    // either is exactly the assumption the series layer exists to stop.
    if (S->unit[0] == '\0' || !seriesFitKind(*S) || !L.refusal.empty()) {
        L.nPts = 0;
        L.computedRev = ++L.rev;
        return;
    }
    for (int p = 0; p < L.nPl; p++) {
        // candidates: the fit's own inclusion rule; the response is the mean
        int nCand = 0;
        double ymax = -DBL_MAX;
        for (const auto& r : L.rows) {
            if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
            nCand++;
            ymax = std::max(ymax, r.mean[p]);
        }
        L.nPts = nCand;
        // The fit WINDOW (§3.3 migration of the interim frame-linearity):
        // none / auto = 5..95% of THIS plane's max member mean / manual [DN].
        // The applied bounds are recorded per plane so the screen and the
        // export state what was DONE, never only what was set.
        double wlo = -DBL_MAX, whi = DBL_MAX;
        if (L.winMode == 1 && ymax > -DBL_MAX) {
            wlo = 0.05 * ymax;
            whi = 0.95 * ymax;
        } else if (L.winMode == 2) {
            wlo = std::min(L.winLo, L.winHi);
            whi = std::max(L.winLo, L.winHi);
        }
        L.winPlaneLo[p] = L.winMode ? wlo : 0;
        L.winPlaneHi[p] = L.winMode ? whi : 0;
        std::vector<double> lx, ly, lw, pm, pv;
        for (const auto& r : L.rows) {
            if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
            if (L.winMode && (r.mean[p] < wlo || r.mean[p] > whi)) continue;
            // rev4-style weights are 1/y^2: a point at |mean| ~ 0 has no
            // weight to give and is dropped, COUNTED (the interim tool's rule)
            if (L.fitMethod == 1 && fabs(r.mean[p]) < 1e-12) { L.nZero++; continue; }
            lx.push_back(r.level);
            ly.push_back(r.mean[p]);
            lw.push_back(L.fitMethod == 1 ? 1.0 / (r.mean[p] * r.mean[p]) : 1.0);
            // PTC wants sigma_t^2 vs mean - only rows that HAVE a sigma_t
            // (stack members with >= 2 frames; a frame member carries none)
            if (r.hasSigma) {
                pm.push_back(r.mean[p]);
                pv.push_back(r.sigmaT[p] * r.sigmaT[p]);
            }
        }
        L.nFit[p] = (int)lx.size();
        // A WINDOWED fit needs 3 points - 2 fit any line exactly and would
        // report a flawless LE for arbitrarily bent data (the interim tool's
        // rule, migrated). Windowless keeps the panel's historic 2, so no
        // existing series measures differently.
        if (L.winMode != 0 && L.nFit[p] < 3) continue;
        bool fitOk = L.fitMethod == 1
                         ? linFitWeighted(lx, ly, lw, L.slope[p], L.offs[p], L.r2[p])
                         : linFit(lx, ly, L.slope[p], L.offs[p], L.r2[p]);
        if (!fitOk) continue;
        fitted++;
        L.planeFit[p] = true;
        // linearity error: worst deviation from the fit, relative to the fit,
        // over the points the fit USED (out-of-window points stay on the
        // chart, dimmed - they are not this number's business)
        double worst = 0;
        for (size_t i = 0; i < lx.size(); i++) {
            double fit = L.slope[p] * lx[i] + L.offs[p];
            if (fabs(fit) > 1e-9)
                worst = std::max(worst, fabs((ly[i] - fit) / fit) * 100.0);
        }
        L.leMax[p] = worst;
        // photon transfer: sigma_t^2 vs mean; the slope is the system gain K
        double kr2;
        if (!linFit(pm, pv, L.ptcK[p], L.ptcRead2[p], kr2)) { L.ptcK[p] = 0; L.ptcRead2[p] = 0; }
        // Read noise is the noise left at ZERO SIGNAL, and zero signal sits at
        // the black level - not at mean = 0 DN. Extrapolating the PTC to 0 DN
        // lands K*offset below that, which for any sensor with an offset is a
        // large negative number and reads out as "0".
        //
        // Even corrected, the extrapolation is a difference of two large numbers
        // (verified: injecting 3.0 DN of read noise comes back as 0-3.4 DN when
        // the lowest level is already 80 DN above black). So it is only reported
        // as a measurement when there is a DARK stack to measure it in; from a
        // lit-only series it is marked as an extrapolation.
        L.readDN[p] = sqrt(std::max(0.0, L.ptcRead2[p] + L.ptcK[p] * L.offs[p]));
        L.readFromDark = false;
        for (const auto& r : L.rows) {
            if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
            if (fabs(r.level) > 1e-9) continue;             // not a dark member
            // read noise is MEASURED only where there is a sigma_t to measure:
            // a dark FRAME member anchors the black level (its mean is real)
            // but has no time axis, so the read noise stays an extrapolation
            if (!r.hasSigma) continue;
            L.readDN[p] = r.sigmaT[p];                      // measured, not fitted
            L.readFromDark = true;
            break;
        }
    }
    L.fitValid = fitted > 0;
    L.computedRev = ++L.rev;
}

// The SNR curve: the rows linRecompute already measured, presented against the
// noise they carry (docs/stats-taxonomy.md gap #2 - no new measurement, a new
// presentation of the fit's own rows). One point per lit member per CFA plane,
// planes never mixed. x = dark-corrected mean signal [DN], y = x / sigma_t,
// dimensionless. The dark convention is linRecompute's own: a level-0 member is
// a MEASUREMENT and wins (the same rule that decides readFromDark); without one
// the FITTED offset is subtracted and *fromDark tells the caller to say
// "extrapolated". Inclusion is the fit's rule verbatim (include && valid &&
// finite level) - not re-derived - so a point excluded from the fit is excluded
// here by the same test. The dark row itself is no point: zero signal has no
// ratio, and the log axis could not place it anyway. *belowDark counts lit rows
// whose corrected mean came out <= 0 (or whose sigma_t was 0) - they are
// dropped, and the caller must SAY so, not hide them. *noSigma counts lit
// FRAME members (§3.3): no time axis, no sigma_t, no ratio - a different
// reason from belowDark, and the caller states it as its own sentence.
struct LinSnrPt { int seqId; int plane; double x, snr; };
static void linSnrPoints(const App::LinState& L, std::vector<LinSnrPt>& out,
                         bool* fromDark, int* belowDark, int* noSigma = nullptr) {
    out.clear();
    if (noSigma) *noSigma = 0;
    const App::LinState::Row* darkRow = nullptr;
    for (const auto& r : L.rows) {
        if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
        if (fabs(r.level) <= 1e-9) { darkRow = &r; break; }   // linRecompute's dark rule
    }
    if (fromDark) *fromDark = darkRow != nullptr;
    if (belowDark) *belowDark = 0;
    for (const auto& r : L.rows) {
        if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
        if (&r == darkRow) continue;
        // a frame member carries no sigma_t (§3.3), so it has no SNR point -
        // COUNTED, so the caller says "N member(s) without sigma_t" instead
        // of a curve that silently holds fewer points than the table
        if (!r.hasSigma) { if (noSigma) (*noSigma)++; continue; }
        for (int p = 0; p < r.nPl && p < 4; p++) {
            double dark = darkRow ? darkRow->mean[p] : L.offs[p];
            double x = r.mean[p] - dark;
            if (!(x > 0) || !(r.sigmaT[p] > 0)) { if (belowDark) (*belowDark)++; continue; }
            out.push_back({ r.seqId, p, x, x / r.sigmaT[p] });
        }
    }
}

// beginPlot for the SNR curve's LOG axes: x is always log10 (the levels span
// decades by design), y is log10 for the ratio view and stays linear for the dB
// view (a dB is already a log). Bounds arrive LINEAR (x > 0; y > 0 when yLog);
// the returned PlotRect lives in log10-x space, so callers map points with
// at(log10(x), yLog ? log10(y) : ydB). Ticks sit on the 1-2-5 sequence per
// decade with unlabeled minor lines at the remaining integers - log-paper, so
// a slope-1/2 reference line reads as one.
static PlotRect beginPlotLog(const char* xLabel, const char* yLabel,
                             double xlo, double xhi, double ylo, double yhi,
                             bool yLog, float height) {
    xlo = log10(std::max(xlo, 1e-12)); xhi = log10(std::max(xhi, 1e-12));
    if (yLog) { ylo = log10(std::max(ylo, 1e-12)); yhi = log10(std::max(yhi, 1e-12)); }
    // breathing room so the end points sit off the frame, and a span floor so a
    // single level still draws as a plot instead of a degenerate axis
    auto pad = [](double& lo, double& hi, double minSpan, double frac) {
        if (hi < lo) std::swap(lo, hi);
        if (hi - lo < minSpan) { double c = (lo + hi) * 0.5; lo = c - minSpan * 0.5; hi = c + minSpan * 0.5; }
        double span = hi - lo;
        lo -= span * frac; hi += span * frac;
    };
    pad(xlo, xhi, 0.3, 0.05);
    pad(ylo, yhi, yLog ? 0.3 : 1.0, 0.07);
    // 1-2-5 majors (labels + grid) and 3,4,6..9 minors (grid only), in log space
    auto logMajors = [](double lo, double hi, std::vector<double>& out) {
        bool dense = hi - lo <= 3.5;              // decades only once it gets busy
        static const double M[3] = { 1, 2, 5 };
        for (int e = (int)floor(lo) - 1; e <= (int)ceil(hi); e++)
            for (int i = 0; i < (dense ? 3 : 1); i++) {
                double v = e + log10(M[i]);
                if (v >= lo - 1e-9 && v <= hi + 1e-9) out.push_back(v);
            }
    };
    auto logMinors = [](double lo, double hi, std::vector<double>& out) {
        static const double M[6] = { 3, 4, 6, 7, 8, 9 };
        for (int e = (int)floor(lo) - 1; e <= (int)ceil(hi); e++)
            for (double m : M) {
                double v = e + log10(m);
                if (v >= lo - 1e-9 && v <= hi + 1e-9) out.push_back(v);
            }
    };
    std::vector<double> xMaj, xMin, yMaj, yMin;
    logMajors(xlo, xhi, xMaj); logMinors(xlo, xhi, xMin);
    double ystep = 1;
    if (yLog) { logMajors(ylo, yhi, yMaj); logMinors(ylo, yhi, yMin); }
    else {
        ystep = niceStep((float)((yhi - ylo) / 4.0));
        for (double v = ceil(ylo / ystep) * ystep; v <= yhi + 1e-9; v += ystep)
            yMaj.push_back(v);
    }
    PlotRect pr;
    pr.xmin = (float)xlo; pr.xmax = (float)xhi;
    pr.ymin = (float)ylo; pr.ymax = (float)yhi;

    const float s = app.uiScale;
    const float fh = ImGui::GetFontSize();
    char tb[48];
    float wy = 0.0f;                     // measure EVERY y label (see beginPlot)
    for (double v : yMaj) {
        fmtTick(tb, sizeof tb, yLog ? pow(10.0, v) : v, false);
        wy = std::max(wy, ImGui::CalcTextSize(tb).x);
    }
    const float marginL = wy + 9 * s;
    const float marginB = fh * 2 + 6 * s;
    const float marginT = fh + 4 * s;

    float wid = ImGui::GetContentRegionAvail().x;
    ImVec2 org = ImGui::GetCursorScreenPos();
    pr.p0 = ImVec2(org.x + marginL, org.y + marginT);
    pr.p1 = ImVec2(org.x + wid, org.y + marginT + height);
    if (pr.p1.x - pr.p0.x < 20 || height < 20) { ImGui::Dummy(ImVec2(wid, height)); return pr; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = IM_COL32(12, 14, 16, 255), grid = IM_COL32(46, 52, 58, 255),
          gridMin = IM_COL32(30, 34, 38, 255), txt = IM_COL32(150, 160, 170, 255),
          axis = IM_COL32(90, 100, 110, 255);
    dl->AddRectFilled(pr.p0, pr.p1, bg);
    for (double v : yMin) {              // minors under majors under labels
        float y = pr.at(pr.xmin, (float)v).y;
        dl->AddLine(ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y), gridMin);
    }
    for (double v : xMin) {
        float x = pr.at((float)v, pr.ymin).x;
        dl->AddLine(ImVec2(x, pr.p0.y), ImVec2(x, pr.p1.y), gridMin);
    }
    for (double v : yMaj) {
        float y = pr.at(pr.xmin, (float)v).y;
        dl->AddLine(ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y), grid);
        fmtTick(tb, sizeof tb, yLog ? pow(10.0, v) : v, false);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(pr.p0.x - 5 * s - ts.x, y - fh * 0.5f), txt, tb);
    }
    for (double v : xMaj) {
        float x = pr.at((float)v, pr.ymin).x;
        dl->AddLine(ImVec2(x, pr.p0.y), ImVec2(x, pr.p1.y), grid);
        fmtTick(tb, sizeof tb, pow(10.0, v), false);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, pr.p0.x, pr.p1.x - ts.x),
                           pr.p1.y + 2 * s), txt, tb);
    }
    dl->AddRect(pr.p0, pr.p1, axis);
    dl->AddText(ImVec2(org.x, org.y), txt, yLabel ? yLabel : "y");
    {
        const char* xl = xLabel ? xLabel : "x";
        ImVec2 ts = ImGui::CalcTextSize(xl);
        dl->AddText(ImVec2((pr.p0.x + pr.p1.x - ts.x) * 0.5f, pr.p1.y + 2 * s + fh), txt, xl);
    }
    ImGui::Dummy(ImVec2(wid, height + marginT + marginB));
    pr.ok = true;
    return pr;
}

