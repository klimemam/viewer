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

// ---- the compared sides ----------------------------------------------------
// One side of the comparison as THIS panel needs it. Built exactly the way the
// Statistics panel builds its own - same letters, same inks, same divergence -
// because a slot is one thing wearing one colour across the application, and a
// second construction here would be a second answer to the question "what is
// C" (docs/compare-n.md 10, "colour is the slot's property").
struct RoiSide {
    ImageDoc* doc = nullptr;
    std::string letter;          // "A", "B", "C", ... - the POSITION, not a name
    std::string label;           // the stack this side is looking at
    bool stale = false;          // follow-frame found no frame carrying A's number
    ImU32 ink = 0;
    std::string mute;            // empty = measurable; else WHY this side has no numbers
};

// The fold this side's frame stands for, as a column-name qualifier.
//
// docs/analysis-layers.md §3.2, amended 2026-08-09 for #57 judgment 2: the
// Stack ROIs sigma column and the Temporal panel's sigma_fpn come off the SAME
// accumulator (sum / sum^2 / cnt) and used to be the same number. They are not
// any more - sigma_fpn now subtracts the temporal residual mean(s_t,i^2/n_i)
// and declares the subtraction, while this column stays the plain spatial
// sigma of whatever frame is on screen. The amendment is explicit that what
// keeps them from being read as one number is the NAMES: this column wears the
// fold ("sigma [DN] (mean_t)"), that row wears its correction.
//
// A frame that came from a file gets no tag - there is no fold to declare, and
// a qualifier on an ordinary frame would claim one.
static std::string roiFoldTag(const ImageDoc* d) {
    if (!d || d->avgOfPath.empty()) return std::string();
    return d->avgFold == FOLD_SUM ? " (sum_t)" : " (mean_t)";
}

// The user's ruling on issue #76 (2026-08-09): 「roiは共通」- ONE set of
// rectangles serves every compared slot. The canvas has always drawn them that
// way (the same ROIs on every pane, in A's coordinates); this is the numbers
// agreeing with the picture.
//
// A shared rectangle measures the same PLACE on another side only if that
// side's frame CONTAINS it. roiBasicStatsUncached CLAMPS its rect to the image,
// so a ROI hanging off a smaller frame would quietly return the intersection -
// a different population, a different n, printed under the same ROI's name in
// the next column along. That is abProjOverlayable's refusal, one panel over:
// a correspondence the data does not support is declined and SAID, never
// invented (docs/ab-stats-plan.md 5).
static bool roiSideCovers(const ImageDoc& d, const App::Ann& a) {
    if (a.x < 0 || a.y < 0) return false;
    if (a.type != 0) return a.x < d.w && a.y < d.h;          // POI: one pixel
    return a.w > 0 && a.h > 0 && a.x + a.w <= d.w && a.y + a.h <= d.h;
}

// ...and the channel selector names a plane of A. Planes correspond BY NAME and
// never by index - abSeriesMatch's rule, and the canon's: "Gr" on a mosaic and
// "ch1" on a three-plane frame are not the same measurement, and index 2 of a
// one-channel side is no plane at all. That last one is not theoretical:
// roiBasicStatsUncached's chSel guard is `chSel < im.ch`, so an index past the
// end falls through to the "every channel" arm and POOLS the planes - a number
// that looks like a plane's and is the mosaic's. A side that cannot honour the
// selection prints nothing rather than something else.
static bool roiSidePlane(const ImageDoc& a, const ImageDoc& d) {
    if (app.roiChannel < 0) return true;             // "all" is askable anywhere
    const bool ca = a.ch == 1 && a.cfa != 0, cd = d.ch == 1 && d.cfa != 0;
    if (ca != cd) return false;
    return cd ? app.roiChannel < 4 : app.roiChannel < d.ch;
}

// One side's numbers for one row: what the cells print, and - when they print
// nothing - the reason, in the words the footer will repeat.
struct RoiCell {
    const RoiSide* side = nullptr;
    RoiStat s{ 0, 0, 0, 0, 0, 0, false };
    double pr = 0;
    bool prOk = false;
    bool ok = false;             // false: this side has no numbers for this row
    std::string why;
};

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
//
// One block per SIDE once a comparison is on: the row is ONE ROI (they are
// shared), so the sides are the only thing that differs and the letter is what
// tells the blocks apart. The caveat underneath is about the MEASUREMENT and
// not about any one side, so it is said once - repeating six lines of prose per
// slot would bury the numbers the tooltip exists to hand over. With a single
// side the tooltip is exactly what it was.
static void roiStatTooltip(const std::vector<RoiCell>& cells, bool planesMixed) {
    bool any = false;
    for (const RoiCell& c : cells) if (c.ok && c.s.valid) any = true;
    if (!any || !ImGui::IsItemHovered()) return;
    const bool multi = cells.size() > 1;
    ImGui::BeginTooltip();
    for (const RoiCell& c : cells) {
        if (multi) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(c.side->ink), "%s",
                               c.side->letter.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", elideFront(c.side->label, 30).c_str());
            if (c.side->stale) {
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
            }
        }
        if (!c.ok)            { ImGui::TextDisabled("%s", c.why.c_str()); continue; }
        if (!c.s.valid)       { ImGui::TextDisabled("no samples here"); continue; }
        ImGui::Text("mean %.10g    sd %.10g   [DN]", c.s.mean, c.s.sd);
        ImGui::Text("min  %.10g    max %.10g   [DN]", c.s.mn, c.s.mx);
        if (c.prOk) ImGui::Text("PRNU %.10g   [σ %%]  = sd / mean * 100", c.pr);
        else ImGui::TextDisabled("PRNU: mean is not above 0, so sd / mean states nothing");
        if (c.s.step > 1) ImGui::Text("n = %zu (sampled 1 row in %zu)", c.s.n, c.s.step);
        else              ImGui::Text("n = %zu (every row)", c.s.n);
        if (c.s.n < 2)
            ImGui::TextDisabled("one sample: sd is 0 by definition, so the ratio is 0");
    }
    ImGui::TextDisabled("THIS ROI's sd over its own mean, as displayed:\n"
                        "one frame, no dark subtraction, no flat-field correction.\n"
                        "Temporal noise is inside it, so it bounds the fixed pattern\n"
                        "from above and is not a measurement of it. A PRNU that means\n"
                        "what EMVA means needs a layer above the frame - a flat stack\n"
                        "and a dark stack (docs/flat-field-stats.md).");
    // The one case where that bound has a named counterpart on screen: a frame
    // average IS the time-folded picture the Temporal panel's sigma_fpn is
    // measured on, off the same accumulator - and since #57 judgment 2 the two
    // numbers deliberately differ by the temporal residual. Say which is which
    // here, where someone is looking at both (docs/analysis-layers.md §3.2).
    for (const RoiCell& c : cells)
        if (c.ok && !roiFoldTag(c.side->doc).empty()) {
            ImGui::TextDisabled("%s stands for a time fold%s: this sd is the PLAIN spatial\n"
                                "sigma of the folded picture. The Temporal panel's sigma_fpn\n"
                                "comes off the same accumulator with the temporal residual\n"
                                "subtracted, so it is a SMALLER number on purpose - not a\n"
                                "second opinion about this one.",
                                multi ? c.side->letter.c_str() : "this frame",
                                roiFoldTag(c.side->doc).c_str());
            break;
        }
    if (planesMixed)
        ImGui::TextDisabled("channel = all: the planes' level differences are in this sd.");
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
// `side` is the LETTER whose numbers this entry carries, so a test can ask the
// question issue #76 is about - do the numbers on screen belong to the slot the
// blink is showing - of the cells rather than of the state that fed them. One
// entry per row with no comparison, one per row per column group with one.
struct RoiRowProbe { int id; char side; RoiStat s; ImVec2 del, lbl; double prnu; bool prnuOk; };
static std::vector<RoiRowProbe> g_roiRowProbe;
static bool g_roiRowProbeOn = false;

// What this panel SAID about the compare slots on the last frame it drew - the
// g_histSideProbe discipline, and for the same reason: this machine cannot
// screenshot GL, so the panel writes down the letters it named rather than a
// test trusting that it did. The bug being guarded is SILENCE, so every field
// records what the panel SAYS and never what a cache holds:
//   "sides=ABC;"           every side the panel resolved
//   "shown=B;"             collapsed form: the ONE side whose numbers are printed
//   "cols=ABC;"            laid-out form: the sides that got a column group
//   "said-not-shown=AC;"   collapsed: the sides NOT printed, named on screen
//   "said-no-numbers=C;"   sides with no numbers for some row, named on screen
// The invariant a test can hold against it is the N group's, transposed: every
// letter in `sides` is in `shown`, in `cols`, or in `said-not-shown`. A letter
// in none of them is the panel dropping a slot without saying so.
static std::string g_roiSideProbe;

static void drawPanelRois() {
    if (g_roiRowProbeOn) g_roiRowProbe.clear();
    g_roiSideProbe.clear();
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

    // ---- who is being compared ---------------------------------------------
    // The letters resolve exactly as the Statistics panel's do, follow-frame
    // included, and rs.idx IS the letter - it travels with the resolved
    // document, because after a follow the document is no longer the pinned uid
    // and a uid lookup would lose the letter. A slot that resolves onto A's own
    // document keeps its own side, for the reason --abeq-selftest pins: two
    // letters on one image is a COINCIDENCE to be shown, not folded away.
    pumpCompareSlotRestore();
    std::vector<RoiSide> sides;
    sides.push_back({ im, "A", abDocLabel(im), false, slotInk(0), std::string() });
    if (ImageDoc* Bim = abStatsB())
        sides.push_back({ Bim, "B", abDocLabel(Bim), g_abFollowDiverged, slotInk(1),
                          std::string() });
    for (const ResolvedSlot& rs : resolveSlots()) {
        if (!rs.doc) continue;
        sides.push_back({ rs.doc, slotName(rs.idx), abDocLabel(rs.doc), rs.diverged,
                          slotInk(rs.idx + 2),
                          rs.doc->w < 1 || rs.doc->h < 1 || rs.doc->px().empty()
                              ? std::string("no pixels loaded yet") : std::string() });
    }
    // The channel selection is A's, and it has to NAME a plane on a side before
    // that side may print one. Muted here, once, rather than per row.
    for (RoiSide& sd : sides)
        if (sd.mute.empty() && !roiSidePlane(*im, *sd.doc))
            sd.mute = std::string("channel \"") + curSel + "\" is not a plane of this frame";
    {
        std::string all;
        for (const RoiSide& sd : sides) all += sd.letter;
        g_roiSideProbe += "sides=" + all + ";";
    }
    const bool multi = sides.size() > 1;

    // The rows a side cannot answer, counted over EVERY annotation and not over
    // the ones the clipper happens to submit: a slot that is silent on a row you
    // have to scroll to is exactly as silent as one at the top.
    std::vector<std::string> mutedWhy(sides.size());
    for (size_t i = 0; i < sides.size(); i++) {
        if (!sides[i].mute.empty()) { mutedWhy[i] = sides[i].mute; continue; }
        int out = 0;
        for (const App::Ann& a : app.anns) if (!roiSideCovers(*sides[i].doc, a)) out++;
        if (out)
            mutedWhy[i] = std::to_string(out) + (out == 1 ? " ROI is" : " ROIs are") +
                          " outside this frame";
    }

    // ---- the two forms the ruling names -------------------------------------
    // 2026-08-09 on issue #76: 「blinkのときは表示切り替え、sidebysideのときは
    // 横に並べる(ヒストグラムとかと同様)」.
    //
    // LAID OUT: the slots' numbers next to each other, a column group per slot.
    // The question "side by side or not" is abSideBySide() - ONE answer for
    // every panel (App::abStatsLayout), so a comparison laid out on the image is
    // laid out here too, and the Histogram and Temporal panels are asking the
    // same thing on the same frame.
    //
    // COLLAPSED: one slot's numbers, in the columns they have always been in.
    // This is the table's only way to "overlay", and it is why the ruling names
    // blink: rows cannot be superimposed the way curves can, so the way to put
    // two of them in one place is to put them there one at a time. The eye stays
    // on a cell and the number under it changes, which is the whole reason the
    // issue argued blink reads BETTER on a table than on an image.
    const bool laid = multi && abSideBySide();
    // Which slot the collapsed form is showing: the one the image is blinking,
    // so the table and the canvas never disagree about whose frame this is.
    // Outside blink nothing is alternating, so it is A - the cursor - and the
    // band over the table says so rather than leaving it to be assumed.
    int shown = 0;
    if (!laid) {
        const int b = blinkSideIndex();
        if (b >= 0 && b < (int)sides.size()) shown = b;
    }
    // The sides that get cells. One list drives both forms, so a row is drawn
    // once and the form is only how many groups it has.
    std::vector<const RoiSide*> cols;
    if (laid) for (const RoiSide& sd : sides) cols.push_back(&sd);
    else      cols.push_back(&sides[shown]);
    {
        std::string got;
        for (const RoiSide* sd : cols) got += sd->letter;
        g_roiSideProbe += laid ? "cols=" + got + ";" : "shown=" + got + ";";
    }

    // ---- what the panel will have to say afterwards -------------------------
    // Measured before the table is sized, so arming a slot never pushes the rows
    // out from under the cursor - the reason the histogram measures its legend
    // ahead of its plot.
    // The probe field is a letter SET, the way the histogram's are, and the
    // sentence is the sentence: a test asks which letters, a reader reads
    // English, and neither should have to parse the other's punctuation.
    std::string notShown, notShownSaid;
    if (multi && !laid)
        for (size_t i = 0; i < sides.size(); i++)
            if ((int)i != shown) {
                notShown += sides[i].letter;
                notShownSaid += (notShownSaid.empty() ? "" : ", ") + sides[i].letter;
            }
    std::string notShownMsg;
    if (!notShown.empty()) {
        notShownMsg = notShownSaid + ": not on this table - blink switches it between "
                      "slots, side by side puts them next to each other.";
        g_roiSideProbe += "said-not-shown=" + notShown + ";";
    }
    {
        std::string quiet;
        for (size_t i = 0; i < sides.size(); i++) if (!mutedWhy[i].empty()) quiet += sides[i].letter;
        if (!quiet.empty()) g_roiSideProbe += "said-no-numbers=" + quiet + ";";
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    // fill the window instead of a fixed 9-text-line box: rows are frame-height,
    // so the old constant showed 4 ROIs no matter how large the window was
    const float editH = ImGui::GetFrameHeight() * 2 + st.ItemSpacing.y;
    float footH = multi ? ImGui::GetTextLineHeightWithSpacing() * (float)sides.size() : 0.0f;
    if (!notShownMsg.empty())
        footH += ImGui::CalcTextSize(notShownMsg.c_str(), nullptr, false,
                                     ImGui::GetContentRegionAvail().x).y + st.ItemSpacing.y;
    // channel = "all" over more than one plane pools them: the plane levels are
    // then part of every sd this table prints, and the tooltip has to say so.
    const bool planesMixed = app.roiChannel < 0 && (cfa || im->ch > 1);

    // The band naming the ONE side on the table - the same strip the Histogram
    // and Temporal panels put over each half of a side-by-side pair, because it
    // is the same statement. Absent with nothing to compare: a lone image needs
    // no letter, and the panel is then exactly what it always was.
    if (multi && !laid)
        drawABBand(sides[shown].letter.c_str(), sides[shown].label, sides[shown].stale);

    // ---- the table ----------------------------------------------------------
    // One side's numbers for one row. `a` null = the "All (whole image)" row,
    // whose target is each side's OWN frame: "all" is a per-image target and not
    // a rectangle, so it is the one row every side can always answer.
    auto cellOf = [&](const RoiSide& sd, const App::Ann* a) {
        RoiCell c;
        c.side = &sd;
        if (!sd.mute.empty()) { c.why = sd.mute; return c; }
        if (a && !roiSideCovers(*sd.doc, *a)) {
            char b[64];
            snprintf(b, sizeof b, "outside this frame (%dx%d)", sd.doc->w, sd.doc->h);
            c.why = b;
            return c;
        }
        c.ok = true;
        if (!a)                c.s = roiBasicStats(*sd.doc, 0, 0, sd.doc->w, sd.doc->h,
                                                   app.roiChannel);
        else if (a->type == 0) c.s = roiBasicStats(*sd.doc, a->x, a->y, a->w, a->h,
                                                   app.roiChannel);
        c.prOk = roiPrnuPct(c.s, &c.pr);
        return c;
    };
    // the five numeric columns of one side, in the order the header names them
    auto areaCells = [&](const RoiCell& c) {
        const bool v = c.ok && c.s.valid;
        ImGui::TableNextColumn(); v ? ImGui::Text("%.6g", c.s.mean) : ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); v ? ImGui::Text("%.6g", c.s.sd)   : ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); c.prOk ? ImGui::Text("%.6g", c.pr) : ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); v ? ImGui::Text("%.6g", c.s.mn)   : ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); v ? ImGui::Text("%.6g", c.s.mx)   : ImGui::TextDisabled("-");
    };
    // ...and of a POI, on every side: one pixel has no spread, so std, the ratio,
    // min and max are all "-" here - a POI row is a VALUE, not a population.
    auto poiCells = [&](const RoiCell& c, const App::Ann& a) {
        const ImageDoc* d = c.side->doc;
        const int c0 = app.roiChannel < 0 ? 0 : std::min(app.roiChannel, d->ch - 1);
        ImGui::TableNextColumn();
        if (c.ok) ImGui::TextUnformatted(fmtVal(d->sample(a.x, a.y, c0), d->dtype).c_str());
        else      ImGui::TextDisabled("-");
        for (int k = 0; k < 4; k++) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
    };

    ImGuiTableFlags TF = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    // Laid out: fixed widths and a horizontal scroll, with the ROI's identity
    // columns FROZEN at the left. Stretch weights have no total to take a share
    // of once the groups can outrun the panel, and columns that reflowed as the
    // slots came into view would make two of them impossible to line up by eye
    // (the reason every side-by-side plot in this application is drawn on
    // identical axes). Freezing name and region is what makes the scroll
    // readable rather than a way to lose which row you are on - and it is why
    // no slot is ever dropped for want of width: nothing here has a palette's
    // limit, so nothing has to be left off and named.
    if (laid) TF |= ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX |
                    ImGuiTableFlags_BordersInnerV;
    else      TF |= ImGuiTableFlags_SizingStretchProp;
    const int nCol = 3 + 5 * (int)cols.size() + 1;
    if (ImGui::BeginTable("roitable", nCol, TF,
                          ImVec2(0, -(editH + footH + st.ItemSpacing.y * 2 + 1.0f)))) {
        ImGui::TableSetupScrollFreeze(laid ? 3 : 0, laid ? 2 : 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        if (laid) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 9);
            ImGui::TableSetupColumn("region", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("8888x8888 @8888,8888").x);
        } else {
            // stretch weights, not fixed widths: four fixed numeric columns would eat
            // the whole table and collapse name/region to a few pixels
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("region", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        }
        for (size_t i = 0; i < cols.size(); i++) {
            // Next to the two numbers it is made of, and wider than them: the unit
            // is part of the name, the way the Analysis grid carries a unit column
            // rather than leaving a reader to guess what a figure is measured in -
            // and a header clipped to "std / m..." would be the one column nobody
            // could identify from a screenshot. That holds per GROUP: the letter
            // above it says whose number it is, never what the number is.
            // ...and the fold rides in the two spread columns' names, per SIDE:
            // A can be a frame average while B is an ordinary frame, and a
            // qualifier written over the whole table would say so of both.
            const std::string ft = roiFoldTag(cols[i]->doc);
            const std::string hSd = "std" + ft;
            const std::string hPct = "std / mean [%]" + ft;
            if (laid) {
                ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
                ImGui::TableSetupColumn(hSd.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                        std::max(numColW(), ImGui::CalcTextSize(hSd.c_str()).x));
                ImGui::TableSetupColumn(hPct.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                        ImGui::CalcTextSize(hPct.c_str()).x);
                ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, numColW());
                ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, numColW());
            } else {
                ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(hSd.c_str(), ImGuiTableColumnFlags_WidthStretch,
                                        ft.empty() ? 1.0f : 1.4f);
                ImGui::TableSetupColumn(hPct.c_str(), ImGuiTableColumnFlags_WidthStretch,
                                        ft.empty() ? 1.5f : 2.0f);
                ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            }
        }
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("x").x + st.FramePadding.x * 2);
        // The slot band: the letter, in the slot's OWN ink, over the five columns
        // carrying its numbers - the table's spelling of the name band the
        // Histogram and Temporal panels put over each half of a side-by-side
        // pair. It has no palette cap, and that is not an oversight: on a shared
        // plot the ink IS the identifier, so two slots wearing one colour is the
        // panel lying; in a table the LETTER identifies and the ink only seconds
        // it, so slot H coming back in A's green costs nothing.
        if (laid) {
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (size_t i = 0; i < cols.size(); i++) {
                ImGui::TableSetColumnIndex(3 + 5 * (int)i);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[i]->ink), "%s",
                                   cols[i]->letter.c_str());
                if (cols[i]->stale) {
                    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.4f);
                    ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
                }
            }
        }
        ImGui::TableHeadersRow();

        // "All" is a real, selectable entry so the table is never empty and the
        // measurement target is always explicit. The region column stays A's
        // size: the ROIs are A's rectangles, and each side's own frame size is
        // on its footer line, where a side that differs is worth reading twice.
        {
            std::vector<RoiCell> cs;
            for (const RoiSide* sd : cols) cs.push_back(cellOf(*sd, nullptr));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("All (whole image)", app.selectedAnn <= 0,
                                  ImGuiSelectableFlags_SpanAllColumns))
                app.selectedAnn = 0;
            roiStatTooltip(cs, planesMixed);
            ImGui::TableNextColumn(); ImGui::Text("%dx%d", im->w, im->h);
            for (const RoiCell& c : cs) areaCells(c);
            ImGui::TableNextColumn();
            if (g_roiRowProbeOn)
                for (const RoiCell& c : cs)
                    g_roiRowProbe.push_back({ 0, c.side->letter[0], c.s, ImVec2(-1, -1),
                                              ImVec2(-1, -1), c.pr, c.prOk });
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
            std::vector<RoiCell> cs;
            for (const RoiSide* sd : cols) cs.push_back(cellOf(*sd, &a));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // ONE checkbox, ONE name, ONE delete for the row: the ruling's first
            // clause is that the ROIs are shared, and a set of controls per slot
            // would say the opposite - that C has a rectangle of its own to hide
            // or throw away. What varies down the row is the numbers.
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
            roiStatTooltip(cs, planesMixed);
            ImGui::TableNextColumn();
            // A's coordinates, because that is what a shared rectangle IS
            if (a.type == 0) ImGui::Text("%dx%d @%d,%d", a.w, a.h, a.x, a.y);
            else if (im->cfa && a.x < im->w && a.y < im->h)
                ImGui::Text("%d,%d [%s]", a.x, a.y, CFA_CH_NAMES[cfaChannelAt(*im, a.x, a.y)]);
            else ImGui::Text("%d,%d", a.x, a.y);
            for (const RoiCell& c : cs) {
                if (a.type == 0) areaCells(c);          // ROI: area statistics
                else             poiCells(c, a);        // POI: the pixel value itself
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeId = a.id;
            if (g_roiRowProbeOn) {
                const ImVec2 delC((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                  (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
                for (const RoiCell& c : cs)
                    g_roiRowProbe.push_back({ a.id, c.side->letter[0], c.s, delC, lblC,
                                              c.pr, c.prOk });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removeId >= 0) deleteAnn(removeId);
    }

    // ---- and what it could not put in the table -----------------------------
    // One line per side, its own frame's size included, because "the same ROI"
    // on two frames of different sizes is the question this footer exists to
    // let a reader ask. A slot with no numbers for some row says so HERE, next
    // to the dashes it is explaining: a column of "-" with nothing accounting
    // for it is the silence this whole panel's compare support is judged on.
    if (multi) {
        for (size_t i = 0; i < sides.size(); i++) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sides[i].ink), "%s",
                               sides[i].letter.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s  %dx%d", elideFront(sides[i].label, 24).c_str(),
                                sides[i].doc->w, sides[i].doc->h);
            if (sides[i].stale) {
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
            }
            if (!mutedWhy[i].empty()) {
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", mutedWhy[i].c_str());
            }
        }
    }
    if (!notShownMsg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, AB_AMBER);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(notShownMsg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
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

