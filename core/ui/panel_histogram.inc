// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// The unit a PIXEL VALUE is quoted in, anywhere one is labelled: DN, always.
// A float .npy is still a digital number - f32 is how the value is STORED, not
// what it measures, and an axis reading "pixel value (f32)" states a storage
// class where a unit belongs. The per-frame TSV has always written [DN]
// unconditionally. Lives here, above the first plot, because every axis label
// below goes through it; the dtype argument is kept so the rule has one place
// to change if a file ever carries a real unit.
// (Statements ABOUT the file - the Inspector's "1920x1080 1ch f32", the browse
// listing's shape column, a dtype-mismatch warning - are not this: they say how
// the data is stored, which is exactly what they mean.)
static std::string abValueUnit(const std::string&) {
    return "DN";
}

// ...and WHOSE range that value is plotted against. It has to name the range
// actually in force: with the union default the x axis spans A and B together,
// and an axis that still says "A's black-white range" is simply telling the
// reader something untrue about the picture in front of them.
// Mode 0 is not an exception. Each side keeps its own STRETCH, but the two
// histograms are still binned on A's black/white - one bin grid, or the curves
// are not comparable at all (see recomputeHistogramIfNeeded) - so naming A's
// range there stays true.
static const char* abRangeSaid(bool haveB) {
    // linkRange wins over all of it, and does so BEFORE any of this is consulted:
    // effBlack/effWhite return the linked pair and never reach abRange, so the
    // axis spans the range shared by every open image - widened by stacks that
    // have nothing to do with the comparison. The Inspector already knows this
    // (it HIDES the A/B combo while the scope is "everything"), but the View >
    // Compare menu still sets the mode and the default is 2 regardless, so all
    // three combinations came out labelled with a range that was not in force.
    if (app.linkRange) return "linked range, all open images";
    // Slots past B widen the union (abRange spans every compared pane), so
    // "A and B combined" undersells what the axis covers the moment C joins.
    // The letters count only while compare is on - a seat is not a comparison.
    const bool slots = app.compareMode != App::CmpOff && !compareExtras().empty();
    if (!haveB && !slots) return "black-white range";
    if (app.compareRangeMode == 2)
        return slots ? "combined range, all compared images"
                     : "A and B combined range";
    return "A's black-white range";
}

// The histogram's x axis label, built in one place so --range-selftest can
// print the exact string the panel draws.
static std::string abHistXLabel(const ImageDoc* a, const ImageDoc* b,
                                const App::HistState& H) {
    char xl[288];
    const std::string xu = abValueUnit(a->dtype);
    const char* xr = abRangeSaid(b != nullptr);
    // The bins are snapped OUTWARD onto a round grid (histBinGrid), so the range
    // the plot actually spans is not the black-white range named above, and the
    // bin width is a quantity the reader needs in order to judge what the shape
    // of a distribution means. Both go on the axis, with the unit, by the same
    // standing rule that puts a unit on every axis in this app:
    //   "... | bins 100..1124 DN, 4 DN/bin"
    char gr[112] = "";
    if (H.binW > 0)
        snprintf(gr, sizeof gr, "  |  bins %.6g..%.6g %s, %.6g %s/bin",
                 (double)H.binOrigin,
                 (double)H.binOrigin + 256.0 * (double)H.binW,
                 xu.c_str(), (double)H.binW, xu.c_str());
    if (b && b->dtype != a->dtype)
        snprintf(xl, sizeof xl, "pixel value (%s, %s - bins every side)"
                                "  -  A %s / B %s, DTYPE MISMATCH%s",
                 xu.c_str(), xr, a->dtype.c_str(), b->dtype.c_str(), gr);
    else
        snprintf(xl, sizeof xl, "pixel value (%s, %s)%s", xu.c_str(), xr, gr);
    return xl;
}

// Is a histogram slot describing something other than the picture it is named
// against? TWO keys, not one. The image is the obvious half. The other is
// the RANGE the bins were built on: in the union default the bin range is the
// min/max of the two CURRENT frames, so it moves every time A steps, and A's
// histogram re-bins each step because its cache keys include black/white while
// B's recompute is skipped for as long as the stepping throttle is armed.
// plotSeries then places B's bins POSITIONALLY across the current plot rect, so
// a slot binned for the old union is drawn shifted and squeezed on the new axis.
// Keyed on uid alone, the amber [stale] tag fired only when B itself changed -
// so it never fired for the case that needs it most, a B pinned with Shift+B
// (an advertised workflow: its uid never moves). Once the stepping stops the
// recompute runs and both comparisons go back to equal.
// It asks the question for ANY side, not only B - every lettered slot is
// binned on A's black/white and skipped by the same stepping throttle, so it
// goes stale for the same two reasons and has to say so in the same words.
static bool abHistSideStale(const ImageDoc* a, const ImageDoc* b, const App::HistState& HB) {
    if (!a || !b) return false;
    return HB.uid != b->uid || HB.black != effBlack(*a) || HB.white != effWhite(*a);
}

// One side of the Statistics panel: A, B, or a lettered slot. It is built ONCE,
// at the top, and the table, the plot, the legend and the footer all walk this
// same list - so the table can never name a slot the footer forgets. That is
// the defect this change exists to remove (issue #60): the cache held two
// sides, so the panel drew the two it held and said nothing about C onwards.
struct HistSide {
    const App::HistState* H;
    std::string letter;               // "A", "B", "C", ... - the POSITION
    std::string label;                // the stack this side is looking at
    bool stale = false;
    ImU32 ink = 0;
    double sc = 1;                    // share of THIS side's own sampled pixels
};

// What the Statistics/Histogram panel ACTUALLY said about the slots on the last
// frame it drew - the same reasoning as g_filesBadgeProbe and g_paneBadgeProbe:
// this machine cannot screenshot GL, so the panel writes down the letters it
// named instead of a test trusting that it did. The bug being guarded is
// SILENCE, so every field records what the panel SAYS, never what a cache
// holds:
//   "rows=ABCD;"        the sides the statistics table gave rows to
//   "curves=ABC;"       the sides that got a curve on the plot
//   "said-no-curve=D;"  the sides left OFF the plot AND named on screen for it
// A letter in `rows` but in neither of the other two would be the old defect
// back again, which is what the N group of --abstats-selftest asserts.
static std::string g_histSideProbe;

static void drawPanelHistogram() {
    g_histSideProbe.clear();
    ImageDoc* im = cur();
    if (im && im->w > 0 && im->h > 0) {
        ImageDoc* Bim = abStatsB();
        pumpCompareSlotRestore();
        // The lettered slots resolve exactly as B does, follow-frame included,
        // and rs.idx IS the letter - it travels with the resolved document
        // because after a follow the document is no longer the pinned uid and a
        // uid lookup would lose the letter. A slot that resolves to the same
        // document as B keeps its own side: two letters landing on one document
        // is a COINCIDENCE, and folding it away leaves no way to tell "C agrees
        // with B" from "C was dropped".
        std::vector<ResolvedSlot> xslots = resolveSlots();
        // resolveSlots() prunes closed slots first, so this is the pruned size.
        // NOTHING below resizes histExtra again: the sides hold pointers into
        // it, and a growth under them would dangle exactly the way seqInfo()'s
        // pointers into app.seqs did (cc1ee8b) - on MSVC and not here.
        app.histExtra.resize(app.cmpExtra.size());
        recomputeHistogramIfNeeded(im, app.hist[0]);
        // Every other side is binned on A's black/white: one x axis, one bin
        // grid, or the curves are not comparable. Sizes may differ freely - the
        // y axis below normalises that away. Skipped while frames are being
        // stepped: an empty slot is always filled, a filled one waits for the
        // stepping to stop and is labelled stale until then. The slots take
        // B's rule unchanged, so N of them cannot turn a held arrow key into N
        // full passes over N images.
        if (Bim && (!abStepBusy() || app.hist[1].uid == 0))
            recomputeHistogramIfNeeded(Bim, app.hist[1], effBlack(*im), effWhite(*im));
        for (const ResolvedSlot& rs : xslots)
            if (rs.idx < app.histExtra.size() &&
                (!abStepBusy() || app.histExtra[rs.idx].uid == 0))
                recomputeHistogramIfNeeded(rs.doc, app.histExtra[rs.idx],
                                           effBlack(*im), effWhite(*im));
        const App::HistState& H = app.hist[0];
        const App::HistState& HB = app.hist[1];
        // ...or B is not A's partner at all: follow-frame found no frame of
        // B's stack carrying A's number, so what is drawn is the last latched
        // one. Same amber token, because it is the same statement.
        const bool bStale = abHistSideStale(im, Bim, HB) || (Bim && g_abFollowDiverged);
        const std::string bLabel = Bim ? abDocLabel(Bim) : std::string();
        auto shareScale = [](const App::HistState& S) {
            return 100.0 / (double)std::max<size_t>(S.sampled, 1);
        };
        std::vector<HistSide> sides;
        sides.push_back({ &H, "A", abDocLabel(im), false, slotInk(0), shareScale(H) });
        if (Bim) sides.push_back({ &HB, "B", bLabel, bStale, slotInk(1), shareScale(HB) });
        for (const ResolvedSlot& rs : xslots) {
            if (rs.idx >= app.histExtra.size()) continue;
            const App::HistState& HX = app.histExtra[rs.idx];
            // slotInk counts A and B first, so a slot's ink is its letter's
            // ink on the Temporal chart too - one colour per slot across the
            // application, not one per panel.
            sides.push_back({ &HX, slotName(rs.idx), abDocLabel(rs.doc),
                              rs.diverged || abHistSideStale(im, rs.doc, HX),
                              slotInk(rs.idx + 2), shareScale(HX) });
        }
        ImGui::Text("Statistics");
        ImGui::SameLine();
        // with a B on screen, an unlabelled table of numbers is ambiguous:
        // say whose numbers these are
        if (Bim) ImGui::TextDisabled("A: %s   (%s)", elideFront(abDocLabel(im), 26).c_str(),
                                     H.roiUsed ? "selected ROI" : "whole image");
        else     ImGui::TextDisabled("(%s)", H.roiUsed ? "selected ROI" : "whole image");
        ImGui::Separator();
        // fixed widths + right-aligned numbers: columns must not reflow while
        // stepping through frames, otherwise values are impossible to compare.
        // The SIDE is a row, never a column pair: a column pair has exactly two
        // operands - which is what a delta needs and what A|B|delta is - and
        // there are N sides. The Projection table settled this axis already
        // (docs/ab-stats-plan.md 4); the rows are the axis that grows.
        const bool anySide = sides.size() > 1;
        if (ImGui::BeginTable("quickstats", anySide ? 5 : 4,
                              ImGuiTableFlags_SizingFixedFit |
                              (anySide ? ImGuiTableFlags_RowBg : 0))) {
            if (anySide)
                ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::GetFontSize() * 1.8f);
            ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
            ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("std", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("var", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableHeadersRow();
            // EVERY side, always - the table has no readability limit a plot
            // has, so whatever the chart below cannot draw is still numbers
            // here. Side major: one capture's planes stay together.
            for (const HistSide& sd : sides) {
                g_histSideProbe += "row" + sd.letter + ";";
                // A lagging side may lag if it is VISIBLY not current (the A4
                // ruling): its cells dim, the same way the Projection table
                // dims a stale side's.
                if (sd.stale)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                for (int s = 0; s < sd.H->nSeries; s++) {
                    ImGui::TableNextRow();
                    if (anySide) {
                        // the letter in the slot's OWN ink, so the row and the
                        // curve carrying the same numbers are the same colour
                        ImGui::TableNextColumn();
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sd.ink),
                                           "%s", sd.letter.c_str());
                    }
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sd.H->seriesNames[s]);
                    ImGui::TableNextColumn(); textNum("%.6g", sd.H->mean[s]);
                    ImGui::TableNextColumn(); textNum("%.6g", sd.H->sd[s]);
                    ImGui::TableNextColumn(); textNum("%.6g", sd.H->sd[s] * sd.H->sd[s]);
                }
                if (sd.stale) ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }

        // SeparatorText spans to the work rect edge, so a plain SameLine() lands
        // outside it and the widget is neither drawn nor clickable: use an
        // absolute offset instead.
        ImGui::SeparatorText("Histogram");
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            float cbW = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("log").x;
            ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - cbW, 0.0f));
            ImGui::Checkbox("log##hist", &app.histLog);
        }
        static const ImU32 CFA_COLS[4] = { IM_COL32(255, 92, 92, 170), IM_COL32(120, 230, 120, 170),
                                           IM_COL32(60, 180, 140, 170), IM_COL32(92, 155, 255, 170) };
        static const ImU32 RGB_COLS[3] = { IM_COL32(255, 92, 92, 150), IM_COL32(79, 221, 107, 150),
                                           IM_COL32(92, 155, 255, 150) };
        const ImU32 ODD_COL = IM_COL32(185, 192, 200, 190);   // a series only one side has
        bool cfaHist = im->ch == 1 && im->cfa != 0;

        // ---- plane selector: four CFA planes times two sides is eight curves
        // on one axis. Same control as the ROI table's, and it filters BOTH
        // sides, so a comparison is always plane against the same plane.
        app.histPlane = std::clamp(app.histPlane, -1, std::max(H.nSeries - 1, -1));
        if (H.nSeries > 1) {
            const char* selName = app.histPlane < 0 ? "all" : H.seriesNames[app.histPlane];
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            if (ImGui::BeginCombo("plane##hist", selName)) {
                if (ImGui::Selectable("all", app.histPlane < 0)) app.histPlane = -1;
                for (int s = 0; s < H.nSeries; s++)
                    if (ImGui::Selectable(H.seriesNames[s], app.histPlane == s)) app.histPlane = s;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Which plane the curves show. The bins are computed for\n"
                                  "all of them either way - this only chooses what is drawn.");
        }
        auto drawn = [&](int s) { return app.histPlane < 0 || app.histPlane == s; };
        int nDrawn = 0;
        for (int s = 0; s < H.nSeries; s++) if (drawn(s)) nDrawn++;

        // ---- the layout question, asked BEFORE anything is sized, because the
        // answer decides how many footer lines and how tall a legend the plot
        // has to leave room for. Side by side stays strictly the A|B pair: two
        // halves hold two sides, and a grid that holds N is the image-side
        // layout job (docs/compare-n.md 10 「表示レイアウト」), not something
        // to fake here.
        const float minSide = AB_MIN_SIDE * app.uiScale;
        bool side = Bim && abSideBySide();
        bool tooNarrow = side && ImGui::GetContentRegionAvail().x < minSide;
        if (tooNarrow) side = false;

        // ---- how many sides the PLOT can take. The table above always takes
        // all of them; a chart has two hard limits and both of them have to be
        // SPOKEN, never applied in silence:
        //   - PLANES. Four CFA planes times five slots is twenty curves, which
        //     is not a comparison, it is a texture. So slots overlay when ONE
        //     plane is on the axis (docs/compare-n.md 4 - the general form of
        //     ab-stats-plan 2's "eight curves are unreadable"), and the plane
        //     selector above is how you get there.
        //   - INK. slotInk can tell slotInkCount() slots apart and then wraps;
        //     drawing past that would put two slots in one colour, which is
        //     this panel omitting a slot again with a different mechanism.
        // Whatever is left off is named on screen, right under the plot.
        const size_t nSides = sides.size();
        const bool onePlane = nDrawn <= 1;
        size_t nPlot;
        const char* offWhy = nullptr;
        if (side || (!xslots.empty() && !onePlane)) {
            nPlot = Bim ? 2 : 1;           // the A|B pair the two-image modes mean
            offWhy = side ? "side by side has two halves - set the A/B layout to "
                            "overlay and every slot goes on one plot"
                          : "pick ONE plane above and every slot overlays on it";
        } else if (nSides > slotInkCount()) {
            nPlot = slotInkCount();
            offWhy = "the palette runs out - these would repeat a colour already "
                     "on the plot, and two slots in one ink is not a comparison";
        } else {
            nPlot = nSides;
        }
        std::string offPlot;              // the letters that get no curve
        for (size_t i = nPlot; i < nSides; i++) {
            if (!offPlot.empty()) offPlot += ", ";
            offPlot += sides[i].letter;
        }
        std::string offMsg;
        if (!offPlot.empty())
            offMsg = "slot " + offPlot + ": no curve - " + offWhy +
                     ". Their numbers are in the table above.";

        // ---- y axis. One side: pixel counts, exactly as before. More than
        // one: a share of each side's OWN sampled pixels, because images of
        // different size put incomparable bar heights on one axis.
        const bool norm = nSides > 1;
        double yTop = 0;
        for (size_t i = 0; i < nPlot; i++) {
            const HistSide& sd = sides[i];
            for (int s = 0; s < sd.H->nSeries; s++) {
                // every side's planes are matched to A's BY NAME (R <-> R):
                // the canon forbids mixing planes, and ch0 is not R
                int a = i == 0 ? s : abSeriesMatch(H.seriesNames, H.nSeries,
                                                   sd.H->seriesNames[s]);
                if (a >= 0 ? !drawn(a) : app.histPlane >= 0) continue;
                for (int b = 0; b < 256; b++)
                    yTop = std::max(yTop, sd.H->bins[s][b] * (norm ? sd.sc : 1.0));
            }
        }
        if (!(yTop > 0)) yTop = 1;
        const double logTop = log1p(yTop);
        char yl[96];
        if (norm)
            snprintf(yl, sizeof yl, app.histLog ? "share of sampled px [%%] (log, max %.3g %%)"
                                                : "share of sampled px [%%] (max %.3g %%)", yTop);
        else
            snprintf(yl, sizeof yl, app.histLog ? "pixel count (log, max %u)"
                                                : "pixel count (max %u)", H.maxBin);
        const std::string xlBuf = abHistXLabel(im, Bim, H);
        const char* xl = xlBuf.c_str();
        // The x axis is the BIN GRID, not the black-white range. plotSeries
        // places bin b positionally, at b/256 of the rect, so the rect has to
        // span exactly what the bins span - and since the grid snaps outward
        // (histBinGrid) those two are no longer the same numbers. Drawn against
        // black..white the whole staircase would be stretched onto a narrower
        // axis and every bar would sit at the wrong pixel value. Every side is
        // binned on A's grid, so one pair of limits serves all of them.
        const float xLo = H.binW > 0 ? H.binOrigin : effBlack(*im);
        const float xHi = H.binW > 0
                            ? (float)((double)H.binOrigin + 256.0 * (double)H.binW)
                            : effWhite(*im);

        // one curve, one way to draw it: filled bars, solid outline, dashed
        // outline. The staircase is built once and reused by all three.
        auto plotSeries = [&](const PlotRect& pr, const uint32_t bins[256], double sc,
                              ImU32 col, int style) {          // 0 fill, 1 solid, 2 dashed
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float pw = pr.p1.x - pr.p0.x, ph = pr.p1.y - pr.p0.y;
            auto yOf = [&](uint32_t v) {
                double u = v * sc;
                double f = app.histLog ? (logTop > 0 ? log1p(u) / logTop : 0.0) : u / yTop;
                return pr.p1.y - (float)std::clamp(f, 0.0, 1.0) * ph;
            };
            if (style == 0) {
                for (int b = 0; b < 256; b++) {
                    if (!bins[b]) continue;
                    float bx0 = pr.p0.x + (float)b / 256.0f * pw;
                    dl->AddRectFilled(ImVec2(bx0, yOf(bins[b])),
                                      ImVec2(bx0 + pw / 256.0f + 0.5f, pr.p1.y), col);
                }
                return;
            }
            std::vector<ImVec2> pts;
            pts.reserve(512);
            for (int b = 0; b < 256; b++) {
                float bx0 = pr.p0.x + (float)b / 256.0f * pw;
                float bx1 = pr.p0.x + (float)(b + 1) / 256.0f * pw;
                float y = yOf(bins[b]);
                pts.push_back(ImVec2(bx0, y));
                pts.push_back(ImVec2(bx1, y));
            }
            if (style == 1) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.4f);
            else addDashedPolyline(dl, pts.data(), (int)pts.size(), col, 1.4f,
                                   5 * app.uiScale, 4 * app.uiScale);
        };
        // Once SLOTS share the plot, the colour has to say which SLOT. With one
        // plane on the axis (the condition for putting them there at all) the
        // plane is fixed and named on the axis, so hue is free to carry the
        // letter - and it carries the SAME letter the Temporal chart, the
        // statistics rows and the tile badges use, because slotInk is asked
        // once per slot and every panel asks it. A palette invented here would
        // be a second, unrelated naming of a thing that already has a name.
        //
        // A and B alone keep exactly the colours they had: A's planes in the
        // plane colours, B lifted from the same hue so R stays R.
        const bool bySlotInk = nPlot > 2;
        // Four CFA planes (or three RGB ones) as eight filled areas is
        // unreadable, so at three drawn curves or more BOTH sides become
        // outlines. Below that A keeps its fill and only B is an outline.
        // Slot ink means outlines throughout: N filled areas hide each other,
        // and the fill was only ever how "this one is A" got said when there
        // were two. With a legend and one hue per letter, the hue says it.
        const bool outlineA = (nSides > 1 && nDrawn >= 3) || bySlotInk;
        auto drawSide = [&](const PlotRect& pr, size_t i) {
            const HistSide& sd = sides[i];
            for (int s = 0; s < sd.H->nSeries; s++) {
                // matched by NAME; a series only this side has is drawn neutral,
                // and never coloured as if it were one of A's planes
                int a = i == 0 ? s : abSeriesMatch(H.seriesNames, H.nSeries,
                                                   sd.H->seriesNames[s]);
                if (a >= 0 ? !drawn(a) : app.histPlane >= 0) continue;
                // A single grey series gave B the SAME colour as A's fill,
                // leaving a 1.4 px dashed line at ~30/255 contrast wherever
                // it crossed that fill - the dash pattern was B's only cue.
                // Mono B gets a cool tint so it reads against A's grey; the
                // coloured cases already differ by hue per plane.
                //
                // ...and every side past A is drawn SOLID, like the profile and
                // temporal curves. A dash reads as a broken curve rather than
                // as "this one is B", and a histogram is dense enough that the
                // dashes close up at one zoom and open at the next.
                ImU32 col;
                if (bySlotInk)           col = sd.ink;
                else if (a < 0)          col = ODD_COL;
                else if (cfaHist)        col = CFA_COLS[a];
                else if (H.nSeries == 1) col = i == 0 ? IM_COL32(200, 205, 210, 200) : AB_B_INK;
                else                     col = i == 0 ? RGB_COLS[a]
                                                      : abLiftInk(RGB_COLS[a], 0.45f);
                plotSeries(pr, sd.H->bins[s], norm ? sd.sc : 1.0, col,
                           i == 0 && !outlineA ? 0 : 1);
            }
        };
        auto drawAll = [&](const PlotRect& pr, bool wantA, bool wantRest) {
            if (!pr.ok) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            for (size_t i = 0; i < nPlot; i++) {
                if (i == 0 ? !wantA : !wantRest) continue;
                drawSide(pr, i);
            }
            dl->PopClipRect();
        };

        // The legend names every side that got a curve. Two sides keep the
        // fixed A/B row; three or more fly the wrapping N-slot legend, because
        // C/D/E push a single row off the panel. Both are MEASURED before the
        // plot is sized, so adding a slot never moves the plot out from under
        // the cursor.
        std::vector<SlotLegendEntry> leg;
        if (!side && bySlotInk)
            for (size_t i = 0; i < nPlot; i++)
                leg.push_back({ abLegendText(sides[i].letter.c_str(), sides[i].label) +
                                (sides[i].stale ? std::string("  ") + AB_STALE_TOKEN
                                                : std::string()),
                                sides[i].ink });
        // fill the rest of the panel: a fixed height overflowed the bottom dock
        float offMsgH = offMsg.empty() ? 0.0f
                      : ImGui::CalcTextSize(offMsg.c_str(), nullptr, false,
                                            ImGui::GetContentRegionAvail().x).y +
                        ImGui::GetStyle().ItemSpacing.y;
        float footerH = ImGui::GetTextLineHeightWithSpacing() * (float)nSides
                      + (tooNarrow ? abNarrowNoteH() : 0.0f) + offMsgH;
        // the legend row lives under the overlaid plot, and its height comes off
        // the plot BEFORE the plot is laid out
        float legendH = !leg.empty() ? slotLegendRow(leg, false)
                      : (Bim && !side ? abLegendH(abDocLabel(im), bLabel, bStale) : 0.0f);
        float hAvail = ImGui::GetContentRegionAvail().y
                     - (ImGui::GetFontSize() * 3 + 12 * app.uiScale)   // axes + footer
                     - footerH - legendH;
        if (side) hAvail -= ImGui::GetTextLineHeight() + 6 * app.uiScale;   // heading band
        float plotH = std::max(hAvail, 70.0f * app.uiScale);

        {   // what the plot took, and what it left - as the panel says it, so a
            // letter that is in neither list is a silent omission by definition
            std::string got, off;
            for (size_t i = 0; i < nSides; i++)
                (i < nPlot ? got : off) += sides[i].letter;
            g_histSideProbe += "curves=" + got + ";";
            if (!off.empty()) g_histSideProbe += "said-no-curve=" + off + ";";
        }

        if (!side) {
            PlotRect hp = beginPlot(xl, yl, xLo, xHi, 0.0f, 1.0f,
                                    false, false, plotH);
            drawAll(hp, true, true);
            if (!leg.empty())  slotLegendRow(leg, true);
            else if (Bim)      drawABLegendRow(abDocLabel(im), bLabel, bStale);
        } else {
            // Always 50/50, never splitFrac: comparing two shapes needs two
            // plots of the SAME width. What the layout copies from the image is
            // the ORDER (A on the left), not the divider position.
            const ImGuiStyle& st = ImGui::GetStyle();
            float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
            float childH = plotH + ImGui::GetFontSize() * 3 + 12 * app.uiScale
                         + ImGui::GetTextLineHeight() + 8 * app.uiScale;
            ImGui::BeginChild("##histA", ImVec2(half, childH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            drawABBand("A", abDocLabel(im));
            drawAll(beginPlot(xl, yl, xLo, xHi, 0.0f, 1.0f,
                              false, false, plotH), true, false);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##histB", ImVec2(half, childH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            drawABBand("B", bLabel, bStale);
            // identical axis ranges by construction: same xl/yl, same limits
            drawAll(beginPlot(xl, yl, xLo, xHi, 0.0f, 1.0f,
                              false, false, plotH), false, true);
            ImGui::EndChild();
        }
        if (tooNarrow) abNarrowNote();
        // A slot the plot could not take is named HERE, in the panel, next to
        // the plot that does not have it. This is the whole point of the
        // change: the failure being removed is a panel that shows a subset and
        // says nothing, and a limit that is spoken is not that failure.
        if (!offMsg.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, AB_AMBER);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(offMsg.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        // the real pixel counts survive the normalised axis - one line per
        // side, including the ones with no curve: the counts and the clipping
        // are what the table cannot show, and they are per side too
        for (size_t i = 0; i < nSides; i++) {
            const HistSide& sd = sides[i];
            std::string tail;
            if (i > 0) {
                tail = "  | " + sd.label;
                // a side stored differently from A is worth saying so where the
                // numbers are: the x-axis label only ever names A against B
                if (i >= 2 && sd.H->img && sd.H->img->dtype != im->dtype)
                    tail += "  [" + sd.H->img->dtype + " vs A " + im->dtype + "]";
            } else if (cfaHist) tail = " | R/Gr/Gb/B";
            ImGui::TextDisabled("%s  %zu px | <black %.2f%%  >white %.2f%%%s",
                                sd.letter.c_str(), sd.H->sampled,
                                sd.H->clipLo * 100.0, sd.H->clipHi * 100.0, tail.c_str());
            if (sd.stale) {   // never inside the disabled string: it must stand out
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
            }
        }
    }

}

// H/V projections: column means (horizontal profile) and row means (vertical
// profile) over the selected ROI. Column FPN, banding and shading show up here
// far more clearly than in the image itself.
// Slot 0 = A, slot 1 = compare B; the body is unchanged apart from the slot.
static void recomputeProjectionIfNeeded(ImageDoc* im, App::ProjState& P) {
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), chh = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && chh > 0) { rx = cx; ry = cy; rw = cw; rh = chh; roiUsed = true; }
        }
    }
    if (P.uid == im->uid && P.dataRev == im->dataRev && P.mode == app.projMode &&
        P.cfa == im->cfa && P.cfaPattern == im->cfaPattern &&
        P.rx == rx && P.ry == ry && P.rw == rw && P.rh == rh)
        return;
    if (app.annBusy && P.uid == im->uid) return;   // mid-drag: keep the last profile
    P.img = im; P.uid = im->uid; P.dataRev = im->dataRev;
    P.mode = app.projMode; P.cfa = im->cfa; P.cfaPattern = im->cfaPattern;
    P.rx = rx; P.ry = ry; P.rw = rw; P.rh = rh; P.roiUsed = roiUsed;

    bool cfa = im->ch == 1 && im->cfa != 0;
    static const char* CFA_N[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_N[4] = { "ch0", "ch1", "ch2", "ch3" };
    P.nSeries = cfa ? 4 : std::min(im->ch, 3);
    for (int s = 0; s < 4; s++) P.seriesNames[s] = cfa ? CFA_N[s] : CH_N[s];
    // slot 4 is "all" - only worth having when there is more than one plane to
    // mix, and only when asked for
    P.allRow = app.projAllRow && P.nSeries > 1;
    P.seriesNames[4] = "all";
    // WHICH slots the passes fill, not how many. The pooled row is slot 4 by
    // NAME whatever nSeries is - that is the whole point of it not being a
    // fifth plane - so a count is only ever right when nSeries happens to be 4.
    // `nSeries + allRow` was that count: for a 3-plane RGB image with "all"
    // ticked it read 4, sized slots 0..3, and then the pass wrote slot 4, whose
    // vectors had never been sized and whose counter row did not exist. An
    // out-of-range write, on a checkbox, on every non-CFA image with more than
    // one plane - the viewer died the moment it was ticked. CFA hid it because
    // there nSeries IS 4 and the count is accidentally the right answer.
    int fillSlot[5], nAcc = 0;
    for (int s = 0; s < P.nSeries; s++) fillSlot[nAcc++] = s;
    if (P.allRow) fillSlot[nAcc++] = 4;

    // ...and everything below is indexed BY SLOT, never by position, so the
    // five-slot arrays and the loops cannot drift apart again.
    //
    // hAcc/vAcc are where the profile is SUMMED; P.h/P.v are where it is
    // STORED. They are different types on purpose. P.h is float because the
    // plot draws it and a curve is drawn from floats, but a sum of a few
    // hundred to a few thousand samples has no business being one: at a few
    // thousand DN a float sum is already spending its last bits on the
    // running total, and what it drops is the tail of every addend after it.
    // The region moments below are double for exactly this reason and over
    // exactly these samples - one accumulator of each type over one set of
    // pixels is the arrangement that cannot be right.
    std::vector<double> hAcc[5], vAcc[5];
    std::vector<std::vector<int>> hN(5), vN(5);
    for (int i = 0; i < nAcc; i++) {
        const int s = fillSlot[i];
        P.h[s].assign(rw, 0.0f);
        P.v[s].assign(rh, 0.0f);
        hAcc[s].assign(rw, 0.0);
        vAcc[s].assign(rh, 0.0);
        hN[s].assign(rw, 0);
        vN[s].assign(rh, 0);
    }
    if (!P.allRow) { P.h[4].clear(); P.v[4].clear(); P.fStat[4] = {}; P.hStat[4] = {}; P.vStat[4] = {}; }
    // Region moments, gathered from the SAME samples the H pass visits (every
    // column, every step-th row). So the region statistics and the H profile
    // describe one and the same set of pixels, and - every column of a
    // rectangle carrying the same sample count - the region mean and the mean
    // of the column means are ONE NUMBER, computed twice. The panel prints
    // both: fStat[].mean in the wide table, hStat[].mean in the per-axis one.
    // They are asserted equal to within one float ulp (--precision-selftest
    // P7c); they were 0.015 DN apart while the sum above was a float.
    //
    // NOT the V profile. That pass visits every row of every step-th COLUMN,
    // which is a different set of pixels, and its mean differs from the region
    // mean by ~0.04 DN on a 4000x3000 frame for that reason alone. The comment
    // that used to stand here said "the profile sigmas", plural, and was
    // therefore false for half of what it named.
    double fSum[5] = {}, fSum2[5] = {};
    size_t fN[5] = {};
    double fMn[5] = { DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX };
    double fMx[5] = { -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
    bool useMax = app.projMode == 1, useMin = app.projMode == 2;
    if (useMax || useMin)
        for (int i = 0; i < nAcc; i++) {
            const int s = fillSlot[i];
            std::fill(hAcc[s].begin(), hAcc[s].end(), useMax ? -DBL_MAX : DBL_MAX);
            std::fill(vAcc[s].begin(), vAcc[s].end(), useMax ? -DBL_MAX : DBL_MAX);
        }
    // Sample cap (this was the only uncapped full-image pass). Each profile keeps
    // full resolution along its OWN axis and strides the orthogonal one, so no
    // column or row is ever left without data.
    const size_t PROJ_MAX_SAMPLES = 2000000;
    // A CFA stride must move by WHOLE mosaic cells AND visit every line of the
    // cell it lands on. Rounding the stride up to an even number and then
    // stepping by it did the opposite: ((1+1)/2)*2 is 2, so EVERY mosaiced
    // image strided, the H pass saw only even rows and the V pass only even
    // columns - and for RGGB that means Gb and B are never sampled by H, Gr
    // and B never by V, so the B plane had no samples at all and its row
    // simply disappeared from the table. Two of four planes, silently.
    const int cell = im->cfa == 2 ? 4 : im->cfa ? 2 : 1;   // quad Bayer repeats every 4
    // ...and the budget has to know that each step now covers `cell` lines
    int step = (int)std::max<size_t>(1, ((size_t)rw * rh * cell) / PROJ_MAX_SAMPLES);
    if (cell > 1) step = ((step + cell - 1) / cell) * cell;   // whole cells only
    auto accumulate = [&](int x, int y, bool toH) {
        const float* src = &im->px()[((size_t)(ry + y) * im->w + (rx + x)) * im->ch];
        int lo = 0, hi = P.nSeries;
        if (cfa) { lo = cfaChannelAt(*im, rx + x, ry + y); hi = lo + 1; }
        // every value goes into its own plane's row, and - when asked for -
        // into the pooled "all" row as well. Pooling here rather than in a
        // second pass keeps the two rows over EXACTLY the same samples.
        auto put = [&](int slot, float val) {
            double& acc = toH ? hAcc[slot][x] : vAcc[slot][y];
            if (useMax) acc = std::max(acc, (double)val);
            else if (useMin) acc = std::min(acc, (double)val);
            else acc += val;
            (toH ? hN[slot][x] : vN[slot][y])++;
            if (toH) {           // one pass only, or every pixel counts twice
                fSum[slot] += val; fSum2[slot] += (double)val * val; fN[slot]++;
                fMn[slot] = std::min(fMn[slot], (double)val);
                fMx[slot] = std::max(fMx[slot], (double)val);
            }
        };
        for (int s = lo; s < hi; s++) {
            float val = cfa ? src[0] : src[std::min(s, im->ch - 1)];
            if (!std::isfinite(val)) continue;
            put(s, val);
            if (P.allRow) put(4, val);
        }
    };
    // Each step lands on a mosaic cell and takes ALL of its lines, so every
    // plane is sampled the same number of times whatever the stride.
    for (int y = 0; y < rh; y += step)          // H profile: every column
        for (int dy = 0; dy < cell && y + dy < rh; dy++)
            for (int x = 0; x < rw; x++) accumulate(x, y + dy, true);
    for (int y = 0; y < rh; y++)                // V profile: every row
        for (int x = 0; x < rw; x += step)
            for (int dx = 0; dx < cell && x + dx < rw; dx++) accumulate(x + dx, y, false);
    P.hMin = P.vMin = FLT_MAX; P.hMax = P.vMax = -FLT_MAX;
    // The one place the profile narrows to float: the mean is DIVIDED in double
    // and rounded once, on its way into the vector the plot draws. One rounding
    // per column instead of one per sample.
    for (int i = 0; i < nAcc; i++) {
        const int s = fillSlot[i];
        for (int x = 0; x < rw; x++) {
            if (!hN[s][x]) { P.h[s][x] = std::numeric_limits<float>::quiet_NaN(); continue; }
            double m = hAcc[s][x];
            if (!useMax && !useMin) m /= hN[s][x];
            P.h[s][x] = (float)m;
            P.hMin = std::min(P.hMin, P.h[s][x]); P.hMax = std::max(P.hMax, P.h[s][x]);
        }
        for (int y = 0; y < rh; y++) {
            if (!vN[s][y]) { P.v[s][y] = std::numeric_limits<float>::quiet_NaN(); continue; }
            double m = vAcc[s][y];
            if (!useMax && !useMin) m /= vN[s][y];
            P.v[s][y] = (float)m;
            P.vMin = std::min(P.vMin, P.v[s][y]); P.vMax = std::max(P.vMax, P.v[s][y]);
        }
    }
    if (P.hMin > P.hMax) { P.hMin = 0; P.hMax = 1; }
    if (P.vMin > P.vMax) { P.vMin = 0; P.vMax = 1; }

    for (int i = 0; i < nAcc; i++) {
        const int s = fillSlot[i];
        App::ProjState::Stats& f = P.fStat[s];
        f = {};
        if (!fN[s]) continue;
        f.mean = fSum[s] / fN[s];
        double var = fSum2[s] / fN[s] - f.mean * f.mean;
        f.sd = sqrt(var > 0 ? var : 0);
        f.mn = fMn[s]; f.mx = fMx[s]; f.pp = f.mx - f.mn;
        f.pct = f.mean != 0 ? f.sd / fabs(f.mean) * 100.0 : 0.0;
        f.valid = true;
    }
    // statistics OF THE PROFILE itself: the spread of column means is column FPN,
    // the spread of row means is row FPN / banding - that is the number people
    // actually want out of a projection, and eyeballing a curve cannot give it.
    for (int i = 0; i < nAcc; i++) {
        const int s = fillSlot[i];
        for (int axis = 0; axis < 2; axis++) {
            const std::vector<float>& d = axis == 0 ? P.h[s] : P.v[s];
            App::ProjState::Stats& st = axis == 0 ? P.hStat[s] : P.vStat[s];
            st = {};
            double sum = 0, sum2 = 0;
            double mn = DBL_MAX, mx = -DBL_MAX;
            size_t n = 0;
            for (float v : d) {
                if (!std::isfinite(v)) continue;
                sum += v; sum2 += (double)v * v;
                mn = std::min(mn, (double)v); mx = std::max(mx, (double)v);
                n++;
            }
            if (!n) continue;
            st.mean = sum / n;
            // ddof=1 (stats-taxonomy.md §8 判断2). These are the row and column
            // MEANS, not the pixels: sigma of them is A and B of the flat-field
            // decomposition T = sigma_p^2 + A + B, and flat-field-stats.md fixes
            // A and B at ddof=1 with a reason - taking T at ddof=0 puts an
            // O(1/W) inconsistency back. Leaving these at ddof=0 would mean the
            // decomposition, when it lands, either does not add up or silently
            // moves numbers that have already shipped.
            //
            // n here is the PROFILE LENGTH - the number of columns or rows, two
            // orders below the pixel count - so unlike the region sigma this is
            // a difference you can see: +2.4e-2 % over a full frame, +6.9 % at
            // n=8, +41 % at n=2 (a 3x3 ROI on a Bayer plane).
            double var = n > 1 ? (sum2 / n - st.mean * st.mean) * ((double)n / (n - 1.0))
                               : 0.0;
            st.sd = sqrt(var > 0 ? var : 0);
            st.mn = mn; st.mx = mx;
            st.pp = mx - mn;
            st.pct = st.mean != 0 ? st.sd / fabs(st.mean) * 100.0 : 0.0;
            st.valid = true;
        }
    }
}

