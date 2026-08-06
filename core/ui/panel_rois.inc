// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// Basic per-ROI statistics (host-computed, always on). Detailed measurements
// live in the Analysis window; this is the "at a glance" layer.
// step: the row stride the sampler used (1 = every row). It rides along in the
// result because a sigma whose sample count nobody can see is not a measurement
// - the panel has to be able to say what n it rests on, and why n < the rect.
struct RoiStat { double mean, sd, mn, mx; size_t n, step; bool valid; };
static RoiStat roiBasicStatsUncached(const ImageDoc& im, int rx, int ry, int rw, int rh, int chSel) {
    RoiStat s{ 0, 0, 0, 0, 0, 0, false };
    rx = std::clamp(rx, 0, im.w); ry = std::clamp(ry, 0, im.h);
    rw = std::clamp(rw, 0, im.w - rx); rh = std::clamp(rh, 0, im.h - ry);
    if (rw < 1 || rh < 1) return s;
    bool cfa = im.ch == 1 && im.cfa != 0;
    size_t total = (size_t)rw * rh;
    // Whole mosaic cells, never a strided linear index (the 47eec0c shape,
    // same as recomputeHistogramIfNeeded): with x = rx + p % rw, an even step
    // on an even-width ROI never landed on an odd column, so from 0.4 Mpx
    // (800x500) the per-plane rows for Gr and B (RGGB) went silently empty
    // while R and Gb kept printing normal numbers.
    const int cell = cfa ? (im.cfa == 2 ? 4 : 2) : 1;       // quad Bayer: 4
    // each landing covers `cell` rows, so the budget has to know that
    size_t step = std::max<size_t>(1, total * cell / 200000);   // cap the cost per ROI
    if (cell > 1) step = ((step + cell - 1) / cell) * cell;     // whole cells only
    double sum = 0, sum2 = 0, mn = DBL_MAX, mx = -DBL_MAX;
    size_t n = 0;
    for (int y0 = 0; y0 < rh; y0 += (int)step)
        for (int y = y0; y < y0 + cell && y < rh; y++)
            for (int x = 0; x < rw; x++) {
                const int ax = rx + x, ay = ry + y;
                const float* src = &im.px()[((size_t)ay * im.w + ax) * im.ch];
                if (cfa) {
                    if (chSel >= 0 && cfaChannelAt(im, ax, ay) != chSel) continue;
                    float v = src[0];
                    if (!std::isfinite(v)) continue;
                    sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
                } else if (chSel >= 0 && chSel < im.ch) {
                    float v = src[chSel];
                    if (!std::isfinite(v)) continue;
                    sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
                } else {
                    for (int c = 0; c < im.ch; c++) {
                        float v = src[c];
                        if (!std::isfinite(v)) continue;
                        sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
                    }
                }
            }
    if (!n) return s;
    s.mean = sum / n;
    double var = sum2 / n - s.mean * s.mean;
    s.sd = sqrt(var > 0 ? var : 0);
    s.mn = mn; s.mx = mx; s.n = n; s.step = step; s.valid = true;
    return s;
}

// The "std / mean [%]" column: this row's own sd over this row's own mean.
// Born "PRNU [σ %]", renamed by the 2026-08-06 ruling: a famous measurement
// name may not sit on a plain ratio. PRNU now belongs only to analyses whose
// inputs declare a dark (docs/analysis-layers.md). The number is unchanged.
// DERIVED, not measured, and deliberately not a field of RoiStat - it is the
// two numbers the row already prints, divided, so it cannot drift from them
// (which also settles the decimated case for free: the sample the sd describes
// is the sample the ratio describes) and it cannot be pooled by averaging the
// planes' values, because the pooled row divides the POOLED sd by the POOLED
// mean and that is a different quantity.
// mean <= 0 has no answer: a ratio to a level needs a level, and zero is not
// one. So the column prints "-" rather than a nan or an inf - these numbers get
// pasted into reports, and a blank that means "not stated" is recoverable where
// an inf that means "we divided by zero" is not. It is also the same guard
// plugins/analyzer_prnu.c already applies before it divides.
static bool roiPrnuPct(const RoiStat& s, double* out) {
    if (!s.valid || !(s.mean > 0)) return false;
    double v = s.sd / s.mean * 100.0;
    if (!std::isfinite(v)) return false;         // sd finite, mean denormal
    *out = v;
    return true;
}

// What the table cannot show in six characters. Full precision lives here; the
// columns format at the edge. n both directions: past ~200k samples the sampler
// reads whole rows at a stride, and n = 1 (a 1x1 ROI) makes sd exactly 0 - the
// single most confusing number this panel can print, so it says why.
// This ratio keeps the longest note in this panel because its OLD name claimed more than
// one frame can deliver: what is on screen is the spatial spread of one region
// as displayed, so the tooltip has to hand back the conditions the name implies
// and this layer does not have (docs/flat-field-stats.md).
// planesMixed: channel = "all" on a multi-plane frame pools planes into one
// population, so the plane LEVELS are inside that sd - the ratio then measures
// the mosaic more than the sensor, and it must say so where it is read.
static void roiStatTooltip(const RoiStat& s, bool planesMixed) {
    if (!s.valid || !ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::Text("mean %.10g    sd %.10g   [DN]", s.mean, s.sd);
    ImGui::Text("min  %.10g    max %.10g   [DN]", s.mn, s.mx);
    double pr = 0;
    if (roiPrnuPct(s, &pr)) ImGui::Text("PRNU %.10g   [σ %%]  = sd / mean * 100", pr);
    else ImGui::TextDisabled("PRNU: mean is not above 0, so sd / mean states nothing");
    ImGui::TextDisabled("THIS ROI's sd over its own mean, as displayed:\n"
                        "one frame, no dark subtraction, no flat-field correction.\n"
                        "Temporal noise is inside it, so it bounds the fixed pattern\n"
                        "from above and is not a measurement of it. A PRNU that means\n"
                        "what EMVA means needs a layer above the frame - a flat stack\n"
                        "and a dark stack (docs/flat-field-stats.md).");
    if (planesMixed)
        ImGui::TextDisabled("channel = all: the planes' level differences are in this sd.");
    if (s.step > 1) ImGui::Text("n = %zu (sampled 1 row in %zu)", s.n, s.step);
    else            ImGui::Text("n = %zu (every row)", s.n);
    if (s.n < 2) ImGui::TextDisabled("one sample: sd is 0 by definition, so the ratio is 0");
    ImGui::EndTooltip();
}

// The ROI table is drawn every frame; recomputing hundreds of thousands of
// samples per row each time made the whole UI (typing included) crawl.
struct RoiStatCacheEntry {
    uint64_t uid; int dataRev, x, y, w, h, ch, cfa, cfaPat;
    RoiStat s;
};
static RoiStat roiBasicStats(const ImageDoc& im, int rx, int ry, int rw, int rh, int chSel) {
    // Hashed, not scanned: the table asks once per row, so a linear cache made the
    // whole panel O(ROIs^2) - measured 1.37 ms/frame at 400 ROIs against 0.34 ms
    // at none. The key still carries every input that changes the numbers.
    static std::unordered_map<uint64_t, RoiStatCacheEntry> cache;
    uint64_t k = im.uid * 1000003ull;
    k = k * 31 + (uint64_t)(uint32_t)im.dataRev;
    k = k * 31 + (uint64_t)(uint32_t)rx;   k = k * 31 + (uint64_t)(uint32_t)ry;
    k = k * 31 + (uint64_t)(uint32_t)rw;   k = k * 31 + (uint64_t)(uint32_t)rh;
    k = k * 31 + (uint64_t)(uint32_t)chSel;
    k = k * 31 + (uint64_t)(uint32_t)im.cfa;
    k = k * 31 + (uint64_t)(uint32_t)im.cfaPattern;
    auto it = cache.find(k);
    if (it != cache.end()) {
        const RoiStatCacheEntry& e = it->second;
        if (e.uid == im.uid && e.dataRev == im.dataRev && e.x == rx && e.y == ry &&
            e.w == rw && e.h == rh && e.ch == chSel && e.cfa == im.cfa && e.cfaPat == im.cfaPattern)
            return e.s;                    // hash collisions must not return wrong numbers
    }
    RoiStat s = roiBasicStatsUncached(im, rx, ry, rw, rh, chSel);
    // room for every ROI of a few frames, so scrubbing does not thrash
    size_t cap = std::max<size_t>(64, (app.anns.size() + 1) * 8);
    if (cache.size() > cap) cache.clear();
    cache[k] = { im.uid, im.dataRev, rx, ry, rw, rh, chSel, im.cfa, im.cfaPattern, s };
    return s;
}

// Selftest probe: the numbers this table actually printed, and where each row's
// delete button landed on screen. The panel's own call path - roiBasicStats
// through the cache, inside the clipper, with the button under a span-all-columns
// Selectable - had no coverage; asserts that call the stats function directly
// cannot see any of it. id 0 = the "All (whole image)" row (no delete button).
// prnu/prnuOk are what the PRNU cell PRINTED, taken at the cell: asserting on
// roiPrnuPct() from the test would only re-run the test's own arithmetic.
struct RoiRowProbe { int id; RoiStat s; ImVec2 del, lbl; double prnu; bool prnuOk; };
static std::vector<RoiRowProbe> g_roiRowProbe;
static bool g_roiRowProbeOn = false;

static void drawPanelRois() {
    if (g_roiRowProbeOn) g_roiRowProbe.clear();
    ImageDoc* im = cur();
    if (!im) { ImGui::TextDisabled("no image"); return; }

    // channel selector keeps the table to a fixed, readable width
    bool cfa = im->ch == 1 && im->cfa != 0;
    static const char* CFA_SEL[5] = { "all", "R", "Gr", "Gb", "B" };
    static const char* CH_SEL[5] = { "all", "ch0", "ch1", "ch2", "ch3" };
    int maxSel = cfa ? 4 : std::min(im->ch, 4);
    // maxSel - 1 on BOTH sides: the CFA arm used to clamp to maxSel, so a
    // restored roichannel of 4 stuck at 4 - one past the last plane. That reads
    // CFA_SEL[5] off the end of the array and matches no plane, so every row in
    // the table would print "-" with nothing saying why.
    app.roiChannel = std::clamp(app.roiChannel, -1, maxSel - 1);
    const char* curSel = app.roiChannel < 0 ? "all"
                                            : (cfa ? CFA_SEL[app.roiChannel + 1] : CH_SEL[app.roiChannel + 1]);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::BeginCombo("channel", curSel)) {
        if (ImGui::Selectable("all", app.roiChannel < 0)) app.roiChannel = -1;
        for (int c = 0; c < maxSel; c++)
            if (ImGui::Selectable(cfa ? CFA_SEL[c + 1] : CH_SEL[c + 1], app.roiChannel == c))
                app.roiChannel = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear all")) { app.anns.clear(); app.selectedAnn = 0; app.annRev++; }
    ImGui::SameLine();
    ImGui::TextDisabled("(R: drag = ROI, P: click = pin)");
    ImGui::Separator();

    const ImGuiTableFlags TF = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable;
    // fill the window instead of a fixed 9-text-line box: rows are frame-height,
    // so the old constant showed 4 ROIs no matter how large the window was
    const float editH = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.y;
    // channel = "all" over more than one plane pools them: the plane levels are
    // then part of every sd this table prints, and the tooltip has to say so.
    const bool planesMixed = app.roiChannel < 0 && (cfa || im->ch > 1);
    if (ImGui::BeginTable("roitable", 9, TF,
                          ImVec2(0, -(editH + ImGui::GetStyle().ItemSpacing.y * 2 + 1.0f)))) {
        // stretch weights, not fixed widths: four fixed numeric columns would eat
        // the whole table and collapse name/region to a few pixels
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("region", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("std", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        // Next to the two numbers it is made of, and wider than them: the unit
        // is part of the name, the way the Analysis grid carries a unit column
        // rather than leaving a reader to guess what a figure is measured in -
        // and a header clipped to "std / m..." would be the one column nobody
        // could identify from a screenshot.
        ImGui::TableSetupColumn("std / mean [%]", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::TableHeadersRow();

        // "All" is a real, selectable entry so the table is never empty and the
        // measurement target is always explicit
        {
            RoiStat s = roiBasicStats(*im, 0, 0, im->w, im->h, app.roiChannel);
            double pr = 0;
            const bool prOk = roiPrnuPct(s, &pr);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("All (whole image)", app.selectedAnn <= 0,
                                  ImGuiSelectableFlags_SpanAllColumns))
                app.selectedAnn = 0;
            roiStatTooltip(s, planesMixed);
            ImGui::TableNextColumn(); ImGui::Text("%dx%d", im->w, im->h);
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mean) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.sd) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); prOk ? ImGui::Text("%.6g", pr) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mn) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mx) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (g_roiRowProbeOn)
                g_roiRowProbe.push_back({ 0, s, ImVec2(-1, -1), ImVec2(-1, -1), pr, prOk });
        }
        int removeId = -1;
        // Only the rows you can see. Submitting all of them cost 1.35 ms/frame at
        // 400 ROIs (0.33 ms at none) - and each row also asked for its statistics,
        // so scrolled-away ROIs were being measured for nobody.
        ImGuiListClipper clipper;
        clipper.Begin((int)app.anns.size());
        while (clipper.Step())
        for (int ai = clipper.DisplayStart; ai < clipper.DisplayEnd; ai++) {
            App::Ann& a = app.anns[ai];
            ImGui::PushID(a.id);
            // measured before the row is drawn, so the row label can carry the
            // tooltip that says what n these numbers rest on
            RoiStat s{ 0, 0, 0, 0, 0, 0, false };
            if (a.type == 0) s = roiBasicStats(*im, a.x, a.y, a.w, a.h, app.roiChannel);
            double pr = 0;
            const bool prOk = roiPrnuPct(s, &pr);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##vis", &a.visible)) app.annRev++;
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ANN_COLORS[a.color & 7]));
            bool sel = app.selectedAnn == a.id;
            // AllowOverlap: this Selectable is submitted first and its rect covers
            // the whole row, the delete button in the last column included. Without
            // it ImGui hands the row's hover to the Selectable and every later item
            // in the row is unreachable - the x button never saw a click.
            if (ImGui::Selectable(a.label.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap))
                app.selectedAnn = a.id;
            ImVec2 lblC((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                        (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            ImGui::PopStyleColor();
            roiStatTooltip(s, planesMixed);
            ImGui::TableNextColumn();
            if (a.type == 0) ImGui::Text("%dx%d @%d,%d", a.w, a.h, a.x, a.y);
            else if (im->cfa && a.x < im->w && a.y < im->h)
                ImGui::Text("%d,%d [%s]", a.x, a.y, CFA_CH_NAMES[cfaChannelAt(*im, a.x, a.y)]);
            else ImGui::Text("%d,%d", a.x, a.y);
            if (a.type == 0) {                          // ROI: area statistics
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mean) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.sd) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); prOk ? ImGui::Text("%.6g", pr) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mn) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mx) : ImGui::TextDisabled("-");
            } else {                                     // POI: the pixel value itself
                // one pixel has no spread, so std, PRNU, min and max are all
                // "-" here: a POI row is a VALUE, not a population
                bool inside = a.x >= 0 && a.y >= 0 && a.x < im->w && a.y < im->h;
                int c0 = app.roiChannel < 0 ? 0 : std::min(app.roiChannel, im->ch - 1);
                ImGui::TableNextColumn();
                if (inside) ImGui::TextUnformatted(fmtVal(im->sample(a.x, a.y, c0), im->dtype).c_str());
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeId = a.id;
            if (g_roiRowProbeOn)
                g_roiRowProbe.push_back({ a.id, s,
                    ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f),
                    lblC, pr, prOk });
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removeId >= 0) deleteAnn(removeId);
    }

    // editor block: fixed height so the window never shrinks with the selection
    ImGui::Separator();
    ImGui::BeginChild("roiedit", ImVec2(0, editH));
    if (App::Ann* sel = findAnn(app.selectedAnn)) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", sel->label.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rename", buf, sizeof buf))
            sel->label = buf;                       // live update is cheap...
        if (ImGui::IsItemDeactivatedAfterEdit())
            app.annRev++;                           // ...but re-analyze only on commit
        if (sel->type == 0) {
            // Commit on Enter/focus-loss, not per keystroke. The caches key on the
            // resolved rect, so applying "5", "51", "512" as you type would rescan
            // the histogram, the ROI stats and every analyzer three times - that is
            // what made typing in these fields feel like the app had died.
            static int xywh[4] = {};
            static bool xywhEditing = false;
            static int xywhId = -1;
            if (!xywhEditing || xywhId != sel->id) {   // follow the ROI until edited
                xywh[0] = sel->x; xywh[1] = sel->y; xywh[2] = sel->w; xywh[3] = sel->h;
                xywhId = sel->id;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt4("##xywh", xywh);      // full width: a visible label would clip
            xywhEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                sel->x = std::clamp(xywh[0], 0, im->w - 1);
                sel->y = std::clamp(xywh[1], 0, im->h - 1);
                sel->w = std::clamp(xywh[2], 1, im->w - sel->x);
                sel->h = std::clamp(xywh[3], 1, im->h - sel->y);
                app.annRev++;
            }
        } else {
            int v[2] = { sel->x, sel->y };
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt2("##pinxy", v)) {
                sel->x = std::clamp(v[0], 0, im->w - 1);
                sel->y = std::clamp(v[1], 0, im->h - 1);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) app.annRev++;
            ImGui::TextDisabled("X / Y: select this pin's row / column");
        }
    } else {
        ImGui::TextDisabled("target: whole image (%dx%d, %s)", im->w, im->h, im->dtype.c_str());
        ImGui::TextDisabled("drag a ROI, or X / Y for the row / column under the cursor");
    }
    ImGui::EndChild();
}

