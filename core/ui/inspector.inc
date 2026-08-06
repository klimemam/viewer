// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// The A/B legend, on its own row BENEATH the plot. It used to be a filled box
// pinned inside the plot rect, top right, where it covered the data it was
// explaining: on a narrow panel it took roughly half the plot width, and any
// distribution with its peak on the right disappeared behind it. (The union
// range default makes that rarer; it does not make a box that hides data
// right.) One line, left to right, like everything else in these panels, at
// the cost of one text row - which the layouts reserve whether or not compare
// is on, so turning it on never moves the plot out from under the cursor.
//
// The swatches show a SAMPLE of each stroke - A's neutral ink, B's tint -
// because a text-only "A" and "B" leaves the reader matching words to lines.
// A is the neutral ink every plot draws in; B carries the blue tint it is
// actually drawn with (a mono series would otherwise give both sides the same
// grey). Names are elided from the FRONT: the tail is what tells two captures
// apart. They come from abDocLabel, so two stacks of one series do not both
// read "frame_000.npy".
// The ink B is drawn in when it shares a plot with A. Two curves of the same
// hue need one difference the eye can take in without tracing either of them;
// a dash pattern is not it, because a profile dense enough to matter turns
// dashes into a solid line at one zoom and into a broken curve at the next.
// Lifting toward white keeps the plane identity (R stays red) and separates
// the sides by weight, which survives decimation.
static const ImU32 AB_B_INK = IM_COL32(120, 190, 255, 230);   // the mono case, and the legend
static ImU32 abLiftInk(ImU32 c, float t) {
    int r = (int)( c        & 0xFF), g = (int)((c >> 8) & 0xFF);
    int b = (int)((c >> 16) & 0xFF), a = (int)((c >> 24) & 0xFF);
    auto up = [&](int v) { return v + (int)((255 - v) * t); };
    return IM_COL32(up(r), up(g), up(b), a);
}

static float abLegendSw()  { return 24 * app.uiScale; }
static float abLegendGap() { return 6 * app.uiScale; }
static std::string abLegendText(const char* side, const std::string& name) {
    return std::string(side) + ": " + elideFront(name, 26);
}
static float abLegendEntryW(const char* side, const std::string& name, bool stale) {
    float w = abLegendSw() + abLegendGap() +
              ImGui::CalcTextSize(abLegendText(side, name).c_str()).x;
    if (stale) w += abLegendGap() + ImGui::CalcTextSize(AB_STALE_TOKEN).x;
    return w;
}
// Too narrow for both entries side by side: stack them rather than clip one.
static bool abLegendOneLine(const std::string& aName, const std::string& bName, bool bStale) {
    return abLegendEntryW("A", aName, false) + 18 * app.uiScale +
           abLegendEntryW("B", bName, bStale) <= ImGui::GetContentRegionAvail().x;
}
static float abLegendH(const std::string& aName, const std::string& bName, bool bStale) {
    return ImGui::GetTextLineHeightWithSpacing() *
           (abLegendOneLine(aName, bName, bStale) ? 1.0f : 2.0f);
}
static void drawABLegendRow(const std::string& aName, const std::string& bName,
                            bool bStale = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = app.uiScale, fh = ImGui::GetFontSize();
    const float sw = abLegendSw(), gap = abLegendGap(), sep = 18 * s;
    const ImU32 ink = IM_COL32(215, 222, 228, 255);
    const ImU32 inkB = IM_COL32(120, 190, 255, 255);
    const bool one = abLegendOneLine(aName, bName, bStale);
    const float lineAdvance = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const std::string la = abLegendText("A", aName), lb = abLegendText("B", bName);
    dl->AddLine(ImVec2(p.x, p.y + fh * 0.5f), ImVec2(p.x + sw, p.y + fh * 0.5f), ink, 1.6f);
    dl->AddText(ImVec2(p.x + sw + gap, p.y), ink, la.c_str());
    const float bx = one ? p.x + abLegendEntryW("A", aName, false) + sep : p.x;
    const float by = one ? p.y : p.y + lineAdvance;
    dl->AddLine(ImVec2(bx, by + fh * 0.5f), ImVec2(bx + sw, by + fh * 0.5f), inkB, 1.6f);
    dl->AddText(ImVec2(bx + sw + gap, by), ink, lb.c_str());
    if (bStale)
        dl->AddText(ImVec2(bx + sw + gap + ImGui::CalcTextSize(lb.c_str()).x + gap, by),
                    AB_AMBER32, AB_STALE_TOKEN);
    // consume exactly what abLegendH promised (ImGui adds one ItemSpacing after)
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x,
                        one ? ImGui::GetTextLineHeight()
                            : ImGui::GetTextLineHeight() + lineAdvance));
}

// The ink each compare slot is stroked in when they share one plot - Temporal's
// chart and the Histogram overlay both. A, B and
// the extras are SLOTS, not CFA planes, so hue is free here - the iron rule is
// about mixing planes inside a statistic, and the per-frame mean is a pooled
// level that says so on its axis. A keeps the neutral green every plot draws
// in and B keeps the blue the shared legend flies, so nothing that already
// reads as A or B changes colour when a C appears.
static ImU32 slotInk(size_t i) {
    static const ImU32 INK[] = {
        IM_COL32(105, 220, 130, 255),   // A
        IM_COL32(120, 190, 255, 230),   // B - AB_B_INK
        IM_COL32(245, 170,  90, 255),   // C
        IM_COL32(205, 140, 245, 255),   // D
        IM_COL32(235, 225, 110, 255),   // E
        IM_COL32(120, 225, 225, 255),   // F
        IM_COL32(245, 130, 160, 255),   // G
    };
    return INK[i % (sizeof INK / sizeof INK[0])];
}
// How many slots the palette can tell APART. The modulo above wraps, so slot H
// comes back in A's green: two curves the same colour on one chart is the panel
// omitting a slot again, this time behind a colour instead of an array bound.
// A caller that overlays slots must cap at this and SAY which letters it left
// off - never draw past it and let the wrap do the lying.
static size_t slotInkCount() { return 7; }

// The Temporal chart's legend. drawABLegendRow knows exactly two slots; this
// one takes as many as the compare slots hold and WRAPS, because C/D/E push a
// single row off the panel. Measuring and drawing are the same code (draw=false
// only skips the ink), so the height reserved for the plot above is the height
// the legend actually takes and adding a slot never moves the plot out from
// under the cursor.
struct SlotLegendEntry { std::string text; ImU32 ink; };
static float slotLegendRow(const std::vector<SlotLegendEntry>& e, bool draw) {
    if (e.empty()) return 0.0f;
    const float sw = abLegendSw(), gap = abLegendGap(), sep = 18 * app.uiScale;
    const float fh = ImGui::GetFontSize(), adv = ImGui::GetTextLineHeightWithSpacing();
    const float availW = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    ImDrawList* dl = draw ? ImGui::GetWindowDrawList() : nullptr;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 txt = IM_COL32(215, 222, 228, 255);
    float x = 0;
    int row = 0;
    for (size_t i = 0; i < e.size(); i++) {
        float w = sw + gap + ImGui::CalcTextSize(e[i].text.c_str()).x;
        if (i && x + sep + w > availW) { row++; x = 0; }
        else if (i) x += sep;
        if (draw) {
            float ex = p.x + x, ey = p.y + row * adv;
            dl->AddLine(ImVec2(ex, ey + fh * 0.5f), ImVec2(ex + sw, ey + fh * 0.5f),
                        e[i].ink, 1.6f);
            dl->AddText(ImVec2(ex + sw + gap, ey), txt, e[i].text.c_str());
        }
        x += w;
    }
    float h = (row + 1) * adv;
    // consume exactly h (ImGui adds one ItemSpacing after the Dummy)
    if (draw) ImGui::Dummy(ImVec2(availW, std::max(h - ImGui::GetStyle().ItemSpacing.y, 1.0f)));
    return h;
}

// The heading strip over one half of a side-by-side pair. Neutral colour: which
// side you are looking at is the message, not which channel.
static void drawABBand(const char* side, const std::string& name, bool stale = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = app.uiScale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeight() + 4 * s;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(52, 58, 66, 255), 3 * s);
    std::string t = std::string(side) + "   " + elideFront(name, 34);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddText(ImVec2(p.x + 6 * s, p.y + 2 * s), IM_COL32(225, 230, 236, 255), t.c_str());
    if (stale)
        dl->AddText(ImVec2(p.x + 6 * s + ImGui::CalcTextSize(t.c_str()).x + 8 * s,
                           p.y + 2 * s), AB_AMBER32, AB_STALE_TOKEN);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h + 2 * s));
}

// Why the side-by-side layout fell back to an overlay. Two things it has to get
// right and did not: it must WRAP (Text does not, and at exactly the width that
// triggers it the line was cut mid-word - "panel narrower than 480 px: overlaid
// ins..."), and it must quote the LOGICAL threshold. It used to print the
// DPI-scaled minSide, so a 150% display was told to widen past 480 px while the
// code compares against 320 logical ones - a number the user cannot act on.
static const char* AB_NARROW_MSG =
    "panel narrower than 320 px (logical width, before display scaling): "
    "overlaid instead of side by side";
static const float AB_MIN_SIDE = 320.0f;
// The height it will take at the current width, so the plot layout can reserve
// exactly what the wrapped text uses. A note that appears and pushes the plot
// down is the same defect as the browser's scrub bar and the preview row.
static float abNarrowNoteH() {
    return ImGui::CalcTextSize(AB_NARROW_MSG, nullptr, false,
                               ImGui::GetContentRegionAvail().x).y +
           ImGui::GetStyle().ItemSpacing.y;
}
static void abNarrowNote() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(AB_NARROW_MSG);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A and B series correspond BY NAME (R <-> R), never by index: the canon says
// CFA planes are never mixed, and ch0 is not R. -1 = this series exists on one
// side only, and is drawn as such.
static int abSeriesMatch(const char* const* names, int n, const char* want) {
    if (!want) return -1;
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], want) == 0) return i;
    return -1;
}

// L2 host service: line plot for analyzer curve output (the only curve UI —
// plugins never draw; they emit_series and this renders every curve the same way)
static void drawAnalysisPlots() {
    const auto& S = app.ana.series;
    if (S.empty()) return;
    std::vector<std::string> names;
    for (const auto& s : S) {
        bool seen = false;
        for (const auto& n : names) if (n == s.name) { seen = true; break; }
        if (!seen) names.push_back(s.name);
    }
    for (const auto& nm : names) {
        float xmin = FLT_MAX, xmax = -FLT_MAX, ymin = FLT_MAX, ymax = -FLT_MAX;
        const App::AnalysisState::Series* first = nullptr;
        for (const auto& s : S) {
            if (s.name != nm || s.ys.empty()) continue;
            if (!first) first = &s;
            for (size_t i = 0; i < s.ys.size(); i++) {
                float xv = s.xs.empty() ? (float)i : s.xs[i];
                float yv = s.ys[i];
                if (!std::isfinite(xv) || !std::isfinite(yv)) continue;
                xmin = std::min(xmin, xv); xmax = std::max(xmax, xv);
                ymin = std::min(ymin, yv); ymax = std::max(ymax, yv);
            }
        }
        if (!first || xmin > xmax || ymin > ymax) continue;
        // the user's reference line is part of the picture: keep it in range
        // instead of silently clipping the one line they asked to see
        if (app.anaRefOn && std::isfinite(app.anaRef)) {
            ymin = std::min(ymin, app.anaRef);
            ymax = std::max(ymax, app.anaRef);
        }
        ImGui::TextDisabled("%s", nm.c_str());
        bool xInt = true;                     // integer axis when the plugin sends no x
        for (const auto& s : S)
            if (s.name == nm && !s.xs.empty()) { xInt = false; break; }
        PlotRect pr = beginPlot(first->xLabel.empty() ? (xInt ? "sample index" : "x")
                                                     : first->xLabel.c_str(),
                                first->yLabel.empty() ? nm.c_str() : first->yLabel.c_str(),
                                xmin, xmax, ymin, ymax, xInt, false, 140.0f * app.uiScale);
        if (!pr.ok) continue;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pr.p0, pr.p1, true);
        for (const auto& s : S) {
            if (s.name != nm || s.ys.size() < 2) continue;
            ImU32 col = s.colorIdx >= 0 ? ANN_COLORS[s.colorIdx & 7]
                                        : IM_COL32(200, 205, 210, 255);
            ImVec2 prev(0, 0); bool has = false;
            for (size_t i = 0; i < s.ys.size(); i++) {
                float xv = s.xs.empty() ? (float)i : s.xs[i];
                float yv = s.ys[i];
                if (!std::isfinite(xv) || !std::isfinite(yv)) { has = false; continue; }
                ImVec2 pt = pr.at(xv, yv);
                if (has) dl->AddLine(prev, pt, col, 1.5f);
                prev = pt; has = true;
            }
        }
        // reference line, dashed: data and reference must never read as one
        // curve. Amber, like the frame markers on the temporal plots.
        if (app.anaRefOn && std::isfinite(app.anaRef) &&
            app.anaRef >= pr.ymin && app.anaRef <= pr.ymax) {
            float y = pr.at(pr.xmin, app.anaRef).y;
            ImU32 rc = IM_COL32(255, 184, 77, 220);
            float dash = 6.0f * app.uiScale;
            ImVec2 seg[2] = { ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y) };
            addDashedPolyline(dl, seg, 2, rc, 1.0f, dash, dash);
            char rb[32];
            snprintf(rb, sizeof rb, "ref %.4g", app.anaRef);
            ImVec2 rts = ImGui::CalcTextSize(rb);
            dl->AddText(ImVec2(pr.p1.x - rts.x - 4 * app.uiScale,
                               y - rts.y - 2 * app.uiScale), rc, rb);
        }
        // hover readout: crosshair snapped to the nearest sample, values for
        // every curve in a tooltip. Nobody should eyeball numbers off a plot
        // in a measurement tool - the curve is the shape, the tooltip is the
        // number. (A tooltip, so the layout never moves.)
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (ImGui::IsWindowHovered() && mp.x >= pr.p0.x && mp.x <= pr.p1.x &&
            mp.y >= pr.p0.y && mp.y <= pr.p1.y) {
            float fx = pr.xmin + (mp.x - pr.p0.x) / (pr.p1.x - pr.p0.x) * (pr.xmax - pr.xmin);
            int best = -1;
            float bd = FLT_MAX;
            for (size_t i = 0; i < first->ys.size(); i++) {
                float xv = first->xs.empty() ? (float)i : first->xs[i];
                float d = fabsf(xv - fx);
                if (d < bd) { bd = d; best = (int)i; }
            }
            if (best >= 0) {
                float sx = first->xs.empty() ? (float)best : first->xs[best];
                float lx = pr.at(sx, pr.ymin).x;
                dl->AddLine(ImVec2(lx, pr.p0.y), ImVec2(lx, pr.p1.y),
                            IM_COL32(150, 160, 170, 120));
                ImGui::BeginTooltip();
                // quantity + unit on the readout too: the tooltip may end up
                // in a screenshot without the axes that explain it
                ImGui::TextDisabled("%s = %.5g   (y: %s)",
                                    first->xLabel.empty() ? "x" : first->xLabel.c_str(), sx,
                                    first->yLabel.empty() ? nm.c_str() : first->yLabel.c_str());
                for (const auto& s : S) {
                    if (s.name != nm || (size_t)best >= s.ys.size()) continue;
                    ImU32 col = s.colorIdx >= 0 ? ANN_COLORS[s.colorIdx & 7]
                                                : IM_COL32(200, 205, 210, 255);
                    const char* lbl = s.col >= 0 && s.col < (int)app.ana.cols.size()
                                    ? app.ana.cols[s.col].c_str() : "?";
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col),
                                       "%s  %.5g", lbl, s.ys[best]);
                }
                ImGui::EndTooltip();
            }
        }
        dl->PopClipRect();
    }
}

// Numeric cell: fixed column width + right alignment, so digits keep their
// position while flipping through frames (values must be comparable at a glance).
// U+03C3, as UTF-8 bytes. A macro rather than a literal at each use, because
// "\xcf\x83f" is read as \xcf followed by the three-digit escape \x83f - the
// letter after the escape has to be a separate string, and getting that wrong
// is a warning, not an error.
#define SIGMA "\xcf\x83"
static void textNum(const char* fmt, double v) {
    char buf[64];
    snprintf(buf, sizeof buf, fmt, v);
    float w = ImGui::CalcTextSize(buf).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    ImGui::TextUnformatted(buf);
}
// One value cell of the profile-statistics table. The whole point of the
// setting is that a COLUMN is uniform, so the format is decided in one place
// rather than at each of the twenty call sites.
static void textStat(double v) {
    char fmt[8];
    if (app.projDecimals < 0) snprintf(fmt, sizeof fmt, "%%.6g");
    else                      snprintf(fmt, sizeof fmt, "%%.%df", app.projDecimals);
    textNum(fmt, v);
}
static void textNumStr(const std::string& s) {
    float w = ImGui::CalcTextSize(s.c_str()).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    ImGui::TextUnformatted(s.c_str());
}
// content width for a full-precision number; the table adds CellPadding itself
static float numColW() { return ImGui::CalcTextSize("-0.00000e+00").x; }

// What the channels of a ch-channel document are CALLED, at least four entries
// long whatever ch is, so a caller that indexes 0..3 cannot walk off it.
//
// 2ch is usually UV, or a complex pair, and almost never RG - so it is C0/C1
// rather than the first two of R/G/B/A. That branch was written long ago and
// was UNREACHABLE for a local .npy until issue #71: (H,W,2) was read as a
// stack of 1-channel frames, so nothing ever arrived here with ch == 2. Lifted
// out of drawInspector() so the selftest can assert it is reached now, instead
// of the rule change quietly landing on a branch nobody has ever seen run.
static const char* const* channelLabels(int ch) {
    static const char* const LB1[] = { "V",  "?",  "?", "?" };
    static const char* const LB2[] = { "C0", "C1", "?", "?" };
    static const char* const LB3[] = { "R",  "G",  "B", "A" };
    return ch == 1 ? LB1 : ch == 2 ? LB2 : LB3;
}

static void drawInspector() {
    ImageDoc* im = cur();
    ImGui::SeparatorText("Pixel");
    // Layout must not depend on hover state: the section always occupies the same
    // number of rows, otherwise everything below jumps as the cursor enters/leaves.
    {
        // hover may sit outside A when the cursor is over a larger B pane
        bool live = im && app.hoverX >= 0 && app.hoverX < im->w && app.hoverY < im->h;
        if (live && im->cfa)
            ImGui::Text("(%d, %d)  [%s]", app.hoverX, app.hoverY,
                        CFA_CH_NAMES[cfaChannelAt(*im, app.hoverX, app.hoverY)]);
        else if (live)
            ImGui::Text("(%d, %d)", app.hoverX, app.hoverY);
        else
            ImGui::TextDisabled("(-, -)  hover the image");
        int nch = im ? im->ch : 1;
        const char* const* lb = channelLabels(nch);
        float inv = im ? 1.0f / std::max(effWhite(*im) - effBlack(*im), 1e-20f) : 1.0f;
        // With compare on, the same pixel in B (and A-B) is the number the eye
        // cannot read off a wipe: two extra columns, present whenever compare is.
        ImageDoc* b = cmpB();
        bool bHere = b && app.hoverX >= 0 && app.hoverX < b->w && app.hoverY < b->h;
        // "DN" is only true for integer sensor data; a float .npy may hold
        // reflectance, e-, or anything else, so do not assert a unit for it.
        auto isInt = [](const std::string& t) {
            return !t.empty() && (t[0] == 'u' || t[0] == 'i' || t == "bool");
        };
        auto unitOf = [&](const std::string& t) { return isInt(t) ? " [DN]" : ""; };
        std::string tA = im ? im->dtype : "f32";
        // A-B in the wider of the two types: u16 minus f32 rounded to whole DN
        // would hide exactly the sub-DN residual you opened the compare for
        std::string tDiff = (b && !isInt(b->dtype)) ? b->dtype : tA;
        std::string hRaw = std::string(b ? "A" : "raw") + unitOf(tA);
        std::string hB = std::string("B") + (b ? unitOf(b->dtype) : "");
        std::string hDiff = std::string("A-B") + unitOf(tDiff);
        // fixed widths: the numbers must not shift as the cursor moves
        if (ImGui::BeginTable("px", b ? 5 : 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
            ImGui::TableSetupColumn(hRaw.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("norm [-]", ImGuiTableColumnFlags_WidthFixed, numColW());
            if (b) {
                ImGui::TableSetupColumn(hB.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
                ImGui::TableSetupColumn(hDiff.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
            }
            ImGui::TableHeadersRow();
            for (int c = 0; c < nch; c++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c < 4 ? lb[std::min(c, 3)] : "?");
                float v = 0;
                if (live) {
                    v = im->sample(app.hoverX, app.hoverY, c);
                    ImGui::TableNextColumn(); textNumStr(fmtVal(v, im->dtype));
                    ImGui::TableNextColumn(); textNum("%.4f", (v - effBlack(*im)) * inv);
                } else {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                }
                if (b) {
                    if (bHere && c < b->ch) {
                        float bv = b->sample(app.hoverX, app.hoverY, c);
                        ImGui::TableNextColumn(); textNumStr(fmtVal(bv, b->dtype));
                        ImGui::TableNextColumn(); textNumStr(fmtVal(v - bv, tDiff));
                    } else {
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    }
                }
            }
            ImGui::EndTable();
        }
        if (b && im) {
            ImGui::TextDisabled("A: %s", abDocLabel(im).c_str());
            ImGui::TextDisabled("B: %s", abDocLabel(b).c_str());
            if (b->w != im->w || b->h != im->h)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "size differs: B is %dx%d",
                                   b->w, b->h);
            // Two images auto-ranged to their own min/max look different even when
            // the pixels are identical - the fastest way to a wrong conclusion.
            if (!app.linkRange) {
                // Sharing is the default because two images stretched
                // differently cannot be compared: the difference you see would
                // be the stretch. When it is off, SAY the numbers differ - that
                // is the case where a reading can mislead.
                ImGui::SetNextItemWidth(-1);
                // "both" and "B uses" were true when the comparison was a
                // pair. The RULE was always N-wide (abRange spans every
                // compared pane), so at three slots the labels lied about
                // arity - issue #60 named this one by name. "every side" is
                // the noun the panels already use, and it is true at two.
                static const char* AB_RANGE_ITEMS[3] = {
                    "A/B: each keeps its own", "A/B: every side uses A's range",
                    "A/B: auto over every side (union)" };
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##abrange", &app.compareRangeMode,
                                 AB_RANGE_ITEMS, 3)) {
                    b->texDirty = true; im->texDirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "How the compared sides are STRETCHED - every pane, slots\n"
                        "included, not just A and B. Display only - each keeps its own\n"
                        "numbers and gets them back when compare ends.\n"
                        "  auto over every side (union), the default: every compared\n"
                        "    side re-fits to min/max across A, B and the slots, so no\n"
                        "    side clips and none is favoured. Two stacks at different\n"
                        "    exposures need this - B against A's range alone saturates\n"
                        "    into a single bin.\n"
                        "  every side uses A's range: one range, A's, for every other\n"
                        "    letter, and it does not move while you step A. Right when\n"
                        "    the others really are measured against A.\n"
                        "  each keeps its own: comparing SHAPES at wildly different\n"
                        "    exposures - the one case where the sides are not directly\n"
                        "    comparable, and it says so.");
                if (app.compareRangeMode == 0 &&
                    (b->black != im->black || b->white != im->white)) {
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "display range differs");
                    ImGui::TextDisabled("A %s-%s / B %s-%s", fmtVal(im->black, im->dtype).c_str(),
                                        fmtVal(im->white, im->dtype).c_str(),
                                        fmtVal(b->black, b->dtype).c_str(),
                                        fmtVal(b->white, b->dtype).c_str());
                }
            }
            if (b->dtype != im->dtype)
                ImGui::TextDisabled("dtype differs: A %s / B %s", im->dtype.c_str(), b->dtype.c_str());
            if (app.compareMode == App::CmpFlip) {
                ImGui::Checkbox("auto blink", &app.flipAuto);
                ImGui::SetNextItemWidth(numColW() * 2);
                // round numbers: 0.5 s per side, not 0.4736
                if (ImGui::InputFloat("seconds per side", &app.flipPeriod, 0.1f, 0.5f, "%.2f"))
                    app.flipPeriod = std::clamp(app.flipPeriod, 0.05f, 10.0f);
                ImGui::TextDisabled("showing %s   (Space or B toggles)",
                                    app.flipShowB ? "B" : "A");
            }
            if (app.compareMode == App::CmpDiff) {
                bool intType = !im->dtype.empty() && (im->dtype[0] == 'u' || im->dtype[0] == 'i');
                bool autoGain = app.diffGain <= 0;
                if (ImGui::Checkbox("auto scale", &autoGain))
                    app.diffGain = autoGain ? 0 : app.diff.gain;
                if (!autoGain) {
                    // shadow buffer: each keystroke would otherwise rebuild the
                    // whole difference image (107 ms at 12 Mpx)
                    static float g = 0;
                    static bool gEditing = false;
                    if (!gEditing) g = app.diffGain > 0 ? app.diffGain : app.diff.gain;
                    ImGui::SetNextItemWidth(numColW() * 2);
                    // step on round numbers, as everywhere else
                    ImGui::InputFloat(intType ? "full scale [DN]" : "full scale", &g,
                                      niceCeil(g) * 0.1f, niceCeil(g), "%g");
                    gEditing = ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivatedAfterEdit()) app.diffGain = std::max(g, 1e-9f);
                } else {
                    ImGui::TextDisabled("full scale +-%g%s (99.9%% of |A-B|)",
                                        app.diff.gain, intType ? " DN" : "");
                }
                ImGui::Checkbox("magnitude only |A-B|", &app.diffAbs);
            }
        }
    }

    ImGui::SeparatorText("Image");
    if (im) {
        // two short lines instead of one long one: the merged line was clipped in
        // a ~300px dock (Text never wraps and there is no horizontal scrollbar)
        if (im->seqId != 0) {
            std::vector<int> fr = framesOfSeq(im->seqId);
            int pos = 0;
            for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
            ImGui::Text("%dx%d  %dch  %s   frame %d/%d", im->w, im->h, im->ch, im->dtype.c_str(),
                        pos + 1, (int)fr.size());
        } else {
            ImGui::Text("%dx%d  %dch  %s", im->w, im->h, im->ch, im->dtype.c_str());
        }
        ImGui::TextDisabled("min %s / max %s", fmtVal(im->vmin, im->dtype).c_str(),
                            fmtVal(im->vmax, im->dtype).c_str());
        if (!im->note.empty()) ImGui::TextWrapped("%s", im->note.c_str());
        // ---- how this file was read, and how to read it differently ----------
        // docs/input-adapters.md §4.13. Every file answers this, including the
        // ones read natively: "reader native" is a fact about this file, and a
        // row that only appeared for adapter-read files would leave the user
        // guessing whether the question even applies to the others.
        if (!im->src->path.empty() && im->src->remoteUrl.empty()) {
            std::string spec = readerFor(im->src->path);
            ImGui::TextDisabled("reader    %s", spec.empty() ? "native" : spec.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("change...")) openReaderPicker(im->src->path, "");
            if (!spec.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("edit")) {
                    std::string how;
                    if (adapter::openInEditor(readerFileOf(spec), how))
                        toast("opened in " + how + ": " + baseName(readerFileOf(spec)));
                    else toast(how, true);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("native")) {
                    // Back to native for this file, and said out loud: the
                    // memory is a record of a choice, so unmaking it is a choice
                    // too and must be as reachable as making it was.
                    forgetReader(im->src->path);
                    toast(baseName(im->src->path) + ": reader forgotten - opens natively next time");
                }
            }
        }
        // Where these pixels came from inside their container, and what else the
        // container held. An .npz member's provenance is the file PLUS the array
        // name, and the members that are not pixels (exposure, gain, a note) are
        // the reason the file was written that way - so they belong beside it
        // rather than in a panel of their own (docs/npz-design.md §4 item 3).
        if (!im->src->npzMember.empty()) {
            ImGui::TextDisabled("npz member  %s", im->src->npzMember.c_str());
            for (const auto& e : app.npzMeta) {
                if (e.path != im->src->path) continue;
                if (e.items.empty()) break;
                if (ImGui::TreeNodeEx("##npzmeta", ImGuiTreeNodeFlags_SpanAvailWidth,
                                      "other members (%d)", (int)e.items.size())) {
                    for (const auto& kv : e.items)
                        ImGui::TextDisabled("%s   %s", kv.first.c_str(), kv.second.c_str());
                    ImGui::TreePop();
                }
                break;
            }
        }
        // ---- how this array was read, and how else this SHAPE could be read --
        // docs/input-adapters.md §3.3. The successor to --npy-axis: what the
        // viewer decided is on screen, and the ways out are computed from the
        // actual shape, so a reading this array cannot support is never offered.
        if (!im->src->npyShape.empty()) {
            const std::vector<int64_t>& shp = im->src->npyShape;
            const int nowRead = im->src->npyRead != NR_NATIVE ? im->src->npyRead
                                                             : npyNativeRead(shp);
            ImGui::TextDisabled("read as   %s", npyReadLabel(shp, nowRead).c_str());
            std::vector<int> opts = npyReadOptions(shp);
            if (opts.size() > 1) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.75f);
                if (ImGui::BeginCombo("##reread", "re-read as...",
                                      ImGuiComboFlags_NoArrowButton)) {
                    for (int r : opts) {
                        if (r == nowRead) continue;
                        if (ImGui::Selectable(npyReadLabel(shp, r).c_str())) {
                            g_reReadAsk = im->uid;      // confirm first: §3.3 says
                            g_reReadWant = r;           // what is lost, BEFORE
                        }
                    }
                    ImGui::EndCombo();
                }
                if (g_reReadAsk == im->uid && !ImGui::IsPopupOpen("Re-read"))
                    ImGui::OpenPopup("Re-read");
                if (ImGui::BeginPopupModal("Re-read", nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("%s  %s", baseName(im->src->path).c_str(),
                                npyShapeText(shp).c_str());
                    ImGui::Text("read as   %s", npyReadLabel(shp, nowRead).c_str());
                    ImGui::Text("re-read as   %s", npyReadLabel(shp, g_reReadWant).c_str());
                    std::string loss = npyReReadLoss(im);
                    if (!loss.empty()) {
                        ImGui::Separator();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                        ImGui::TextWrapped("%s", loss.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Re-read")) {
                        g_reReadGo = im->uid;           // pumpReRead runs it next frame
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) {
                        g_reReadAsk = 0; g_reReadWant = NR_NATIVE;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
        }
        if (im->ch == 1) {
            // interpretation axis: change what the 1ch data means AFTER opening
            const char* modes[3] = { "Gray", "Bayer", "Quad Bayer" };
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            if (ImGui::BeginCombo("Interpret", modes[std::clamp(im->cfa, 0, 2)])) {
                for (int i = 0; i < 3; i++)
                    if (ImGui::Selectable(modes[i], i == im->cfa) && i != im->cfa) {
                        im->cfa = i;
                        im->texDirty = true;
                        seqMosaicChanged(im);
                    }
                ImGui::EndCombo();
            }
            if (im->cfa) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##cfapat", CFA_PATTERNS[im->cfaPattern & 3])) {
                    for (int i = 0; i < 4; i++)
                        if (ImGui::Selectable(CFA_PATTERNS[i], i == im->cfaPattern)) {
                            im->cfaPattern = i;
                            im->texDirty = true;
                            seqMosaicChanged(im);
                        }
                    ImGui::EndCombo();
                }
            }
        }
        if (im->cfa) {
            if (ImGui::Checkbox("Colorize CFA pattern", &im->cfaColorize)) im->texDirty = true;
        }
        if (im->src->rawDtype >= 0 && ImGui::Button("Reinterpret raw...")) {
            openRawDialogFor(im->src->path);            // prefill size guesses from the file
            if (rawDlg.open) {                          // only if the file was readable
            rawDlg.dtype = im->src->rawDtype;
            rawDlg.interp = im->src->rawInterp;
            if (RAW_INTERP_CH[rawDlg.interp] == 1)      // 1ch family: honor current interpretation
                rawDlg.interp = im->cfa == 2 ? RI_QUAD : im->cfa == 1 ? RI_BAYER : RI_GRAY;
            rawDlg.w = im->src->srcW > 0 ? im->src->srcW : im->w;
            rawDlg.h = im->src->srcH > 0 ? im->src->srcH : im->h;
            rawDlg.offset = im->src->rawOffset;
            rawDlg.littleEndian = im->src->rawLE;
            rawDlg.cfaPattern = im->cfaPattern & 3;
            rawDlg.cropOn = isCropped(*im);
            rawDlg.cropX = im->src->cropX; rawDlg.cropY = im->src->cropY;
            rawDlg.cropW = im->w; rawDlg.cropH = im->h;
            rawDlg.replaceIdx = app.current;
            rawGuessDims(rawDlg);
            }
        }
        {
            App::Ann* selAnn = findAnn(app.selectedAnn);
            bool roiSel = selAnn && selAnn->type == 0;
            if (isCropped(*im))
                ImGui::TextDisabled("crop %dx%d @ (%d,%d) of %dx%d",
                                    im->w, im->h, im->src->cropX, im->src->cropY,
                                    im->src->srcW, im->src->srcH);
            if (roiSel && ImGui::Button("Crop to selected ROI")) cropCurrentToSelectedRoi();
            if (isCropped(*im)) {
                if (roiSel) ImGui::SameLine();
                if (ImGui::Button("Restore full")) restoreFull();
            }
        }
        if (im->ch == 1 && !plugin_host::displays().empty()) {
            if (im->cfa && im->cfaColorize) {
                ImGui::TextDisabled("colormap disabled while CFA colorize is on");
            } else {
                const char* curName = (im->displayLut >= 0 &&
                                       im->displayLut < (int)plugin_host::displays().size())
                    ? plugin_host::displays()[im->displayLut].name.c_str() : "Gray";
                if (ImGui::BeginCombo("Colormap", curName)) {
                    if (ImGui::Selectable("Gray", im->displayLut < 0)) { im->displayLut = -1; im->texDirty = true; }
                    for (int i = 0; i < (int)plugin_host::displays().size(); i++)
                        if (ImGui::Selectable(plugin_host::displays()[i].name.c_str(), im->displayLut == i)) {
                            im->displayLut = i; im->texDirty = true;
                        }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::SeparatorText("Range (black / white)");
        {   // ONE mode control. It used to be a "link across all images"
            // checkbox PLUS an on-switch combo whose third entry re-implemented
            // linking destructively (copying the range into each image instead
            // of overlaying one) - two mechanisms for the same intent, spotted
            // as such immediately. Modes:
            //   0 auto per frame - every frame re-fits to its own min..max
            //   1 per stack      - frames of a stack share the reference range
            //   2 linked         - one display range overlays every open image
            int mode = app.linkRange ? 2 : (app.rangeScope == 0 ? 0 : 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##rangemode", &mode,
                             "Auto per frame\0Per stack (default)\0Linked across all images\0")) {
                applyRangeMode(mode, im);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto per frame: every frame re-fits to its own min..max\n"
                                  "Per stack: frames of one stack share the reference\n"
                                  "  frame's range, so they compare directly\n"
                                  "Linked: one display range for every open image - display\n"
                                  "  only, each image keeps its own and gets it back on unlink");
        }
        // EnterReturnsTrue is not allowed on InputScalar-family widgets (asserts in
        // debug builds); edit a shadow buffer and commit on deactivate-after-edit.
        static float bwEdit[2];
        static bool bwEditing = false;
        if (!bwEditing) { bwEdit[0] = effBlack(*im); bwEdit[1] = effWhite(*im); }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputFloat2("##bw", bwEdit, "%.5g");
        bwEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && bwEdit[1] > bwEdit[0])
            setRange(*im, bwEdit[0], bwEdit[1]);
        {   // four equal buttons measured from the panel, never wider than it
            float bw = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
            bw = std::max(bw, ImGui::GetFontSize() * 2.0f);
            if (ImGui::Button("Auto", ImVec2(bw, 0))) {
                float lo = im->vmin, hi = im->vmax;
                if (app.linkRange)                       // fit every image, not just this one
                    for (const auto& d : app.images) { lo = std::min(lo, d->vmin); hi = std::max(hi, d->vmax); }
                setRange(*im, lo, hi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(app.linkRange ? "min/max over all open images" : "min/max of this image");
            ImGui::SameLine();
            if (ImGui::Button("0-1", ImVec2(bw, 0))) setRange(*im, 0, 1);
            ImGui::SameLine();
            if (ImGui::Button("0-255", ImVec2(bw, 0))) setRange(*im, 0, 255);
            ImGui::SameLine();
            if (ImGui::Button("0-65535", ImVec2(bw, 0))) setRange(*im, 0, 65535);
        }

        int g = app.dispGamma > 1.5f ? 1 : 0;
        ImGui::TextUnformatted("gamma"); ImGui::SameLine();
        if (ImGui::RadioButton("1.0", g == 0)) { app.dispGamma = 1.0f; markAllTexDirty(); } ImGui::SameLine();
        if (ImGui::RadioButton("2.2", g == 1)) { app.dispGamma = 2.2f; markAllTexDirty(); }
        {   // only share the row if the checkbox actually fits (fails at 200% DPI)
            const ImGuiStyle& st = ImGui::GetStyle();
            float need = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                         ImGui::CalcTextSize("grid (G)").x + st.ItemSpacing.x;
            float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x -
                         ImGui::GetCursorStartPos().x;
            if (ImGui::GetContentRegionAvail().x - used >= need) ImGui::SameLine();
            ImGui::Checkbox("grid (G)", &app.showGrid);
        }
    } else {
        ImGui::TextDisabled("no image");
    }

}

