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
    // effBlack/effWhite return the linked pair and never reach the A/B mode, so the
    // axis spans the range shared by every open image - widened by stacks that
    // have nothing to do with the comparison. The Inspector already knows this
    // (it HIDES the A/B combo while the scope is "everything"), but the View >
    // Compare menu still sets the mode and the default is 2 regardless, so all
    // three combinations came out labelled with a range that was not in force.
    if (app.linkRange) return "linked range, all open images";
    // Slots past B widen the union (displayRangeFor spans every compared pane), so
    // "A and B combined" undersells what the axis covers the moment C joins.
    // The letters count only while compare is on - a seat is not a comparison.
    const bool slots = app.compareMode != App::CmpOff && !compareExtras().empty();
    if (!haveB && !slots) return "black-white range";
    if (app.compareRangeMode == 2)
        return slots ? "combined range, all compared images"
                     : "A and B combined range";
    // Each side is DRAWN at its own fit (#301), but the curves still share one
    // bin grid - A's range in force, which in this mode is A's own fit.
    if (app.compareRangeMode == 3) return "A's own fit, each side fitted to itself";
    return "A's black-white range";
}

// The histogram's x axis label, built in one place so --range-selftest can
// print the exact string the panel draws.
static std::string abHistXLabel(const ImageDoc* a, const ImageDoc* b,
                                const App::HistState& H) {
    char xl[288];
    const std::string xu = abValueUnit(a->dtype);
    const char* xr = abRangeSaid(b != nullptr);
    // Zoomed (#309), the bins are NOT on the range named above, so naming it
    // would be untrue; the axis names the zoom instead, in the plot's unit.
    char zr[96];
    if (H.zoomed) {
        snprintf(zr, sizeof zr, "zoomed %.6g..%.6g %s, not the display range",
                 (double)H.zoomLo, (double)H.zoomHi, xu.c_str());
        xr = zr;
    }
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
                 (double)H.binOrigin + (double)H.nBins * (double)H.binW,
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
    // ...and a side binned under another zoom (#309) is on another grid, the
    // same way a side binned on another range is.
    const App::HistZoom& Z = app.histZoom;
    const bool zoomDiffers = HB.zoomed != Z.on ||
                             (Z.on && (HB.zoomLo != Z.lo || HB.zoomHi != Z.hi));
    return HB.uid != b->uid || HB.black != effBlack(*a) || HB.white != effWhite(*a) ||
           zoomDiffers;
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
//   "emph=B;"           the side drawn heavy because the IMAGE is showing it
//   "plane=R;"          which plane the curves are of ("all" = every one)
//   "layout=side;"      one pane per side, or "overlay" - one shared axis
// A letter in `rows` but in neither of the other two would be the old defect
// back again, which is what the N group of --abstats-selftest asserts.
// `emph` and `plane` are the 2026-08-14 ruling on #60 made checkable: the
// first is a STRUCTURAL record of the blink emphasis (which letter got the
// weight, never how many pixels wide it was), the second is how a test sees
// that the projection's plane selector and this one are one setting.
static std::string g_histSideProbe;

// ---- the histogram zoom (#309) ----------------------------------------------
// A drag on the plot re-lays the 256 bins on the dragged values - the bins get
// FINER, which is what a zoom of a histogram is worth; stretching the old bars
// would only magnify their width. The state is app.histZoom (state.h says what
// it is keyed on and why); these are its only writers.
//
// What the panel SAID about the zoom on its last frame, for --histzoom-selftest,
// on g_histSideProbe's pattern: "mode=zoom;" (what a plain drag does),
// "zoomed=100..356;" while zoomed, "note=..." the sentence on the plot,
// "marks=black,white;" which display-range lines were drawn, "hl=left|right;"
// when the highlight lies off the zoomed axis, and "out=A:...;" per side.
static std::string g_histZoomProbe;
static void histZoomProbeAdd(const std::string& k, const std::string& v) {
    g_histZoomProbe += k + "=" + v + ";";
}

// The range a linked zoom reads and writes: the one setRange writes for `im`.
static void histZoomRawRange(const ImageDoc& im, float& b, float& w) {
    if (app.linkRange) { b = app.linkBlack; w = app.linkWhite; }
    else               { b = im.black;      w = im.white; }
}

// Is the zoom still about the A on screen? Same stack (a step keeps it), or the
// same document when A stands alone; same plane; same dtype.
static bool histZoomKeyHolds(const ImageDoc* im) {
    const App::HistZoom& Z = app.histZoom;
    if (!im) return false;
    const bool sameDoc = im->seqId > 0 ? im->seqId == Z.seqId : (Z.seqId == 0 && im->uid == Z.uid);
    return sameDoc && app.histPlane == Z.plane && im->dtype == Z.dtype;
}

// Drop a zoom whose key no longer holds. Not a reset: the range is not given
// back, because the document it was taken from is no longer the one on screen.
static void histZoomSync(const ImageDoc* im) {
    if (app.histZoom.on && !histZoomKeyHolds(im)) app.histZoom = App::HistZoom{};
}

// NaN NaN when there is no zoom - recomputeHistogramIfNeeded's "none".
static void histZoomRange(float& lo, float& hi) {
    lo = hi = std::numeric_limits<float>::quiet_NaN();
    if (app.histZoom.on) { lo = app.histZoom.lo; hi = app.histZoom.hi; }
}

// Commit a zoom onto the dragged interval [lo, hi) (already snapped onto the bin
// grid the drag was made on: curW wide, curN bins). Returns false - and says
// why - in the two cases a zoom has nothing to show:
//   * an integer range narrower than 2 DN: one bin of 1 DN is not a histogram;
//   * a grid no narrower than the axis already on screen - dragging (nearly)
//     the whole axis, where the "zoom" would only shift the window.
static bool histZoomApply(ImageDoc* im, float lo, float hi, float curW, int curN = 256) {
    if (!im || !(hi > lo)) return false;
    float o = 0, w = 0;
    int n = 256;
    histZoomGrid(im->dtype, lo, hi, o, w, n);
    const char* no = nullptr;
    if (n < 2) no = "zoom: drag at least 2 DN - 1 DN per bin is as fine as an "
                    "integer histogram gets";
    else if (!((double)n * (double)w < (double)curN * (double)curW))
        no = "zoom: that range is no narrower than the axis already on screen";
    if (no) {
        toast(no, false);
        return false;
    }
    App::HistZoom& Z = app.histZoom;
    Z.on = true;
    Z.lo = o;
    Z.hi = (float)((double)o + (double)n * (double)w);
    Z.seqId = im->seqId; Z.uid = im->uid;
    Z.plane = app.histPlane; Z.dtype = im->dtype;
    if (app.histZoomSetsRange) {
        float rb = 0, rw = 0;
        histZoomRawRange(*im, rb, rw);
        // The step back is taken from the range in force before the FIRST zoom
        // of a run - unless the user moved the range since the last one wrote
        // it, and then from where they put it.
        if (!Z.restore || Z.restoreLink != app.linkRange || rb != Z.setBlack || rw != Z.setWhite) {
            Z.prevBlack = rb; Z.prevWhite = rw;
            Z.restoreLink = app.linkRange;
            Z.restore = true;
        }
        setRange(*im, Z.lo, Z.hi);
        histZoomRawRange(*im, Z.setBlack, Z.setWhite);
    }
    return true;
}

// Reset the zoom (double-click in zoom mode, or `reset zoom`). With the linked
// option on, the range before the first zoom comes back - but only if the range
// in force is still the one the zoom wrote; an Auto or a typed range in between
// is the user's later word and is left alone.
static void histZoomReset(ImageDoc* im) {
    App::HistZoom& Z = app.histZoom;
    if (Z.on && Z.restore && app.histZoomSetsRange && im && Z.restoreLink == app.linkRange) {
        float rb = 0, rw = 0;
        histZoomRawRange(*im, rb, rw);
        if (rb == Z.setBlack && rw == Z.setWhite) setRange(*im, Z.prevBlack, Z.prevWhite);
    }
    Z = App::HistZoom{};
}

// The zoom's drag band: the Temporal box-zoom's blue, so the two zooms in the
// app look like one gesture, and never the highlight's magenta.
static ImU32 histZoomInk(int alpha) { return IM_COL32(120, 190, 255, alpha); }

// What a drag does with Shift held or not: the heading row's switch names the
// plain drag, Shift is the other one.
static bool histDragZooms(bool shift) { return (app.histDragMode == 0) != shift; }

// ---- value highlight, the plot side (#68, docs/features/histogram/histogram-select-design.md) --
// The band and the gesture. Both derive their colour from HL_INK rather than
// spelling magenta again, so §11's "one constant" really is one constant.
static ImU32 histHlBandInk(int alpha) {
    return (HL_INK & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// The selected interval, drawn where it was MADE: same hue as the paint on the
// pixels, low alpha, UNDER the curves (it is the ground the distribution
// stands on, not a mark over it). Drawn on every half of a side-by-side,
// because there is one axis and the interval belongs to the axis - asking
// "is this A's selection or B's" is a question the design deletes rather than
// answers.
static void histHlBand(const PlotRect& pr) {
    if (!pr.ok || !app.highlight.on) return;
    const App::HistHighlight& H = app.highlight;
    // A zoomed axis can leave the whole interval off the plot (#309). A band
    // pinned 1 px wide at the edge would claim the values are there; say on
    // which side they are instead.
    const bool offLeft = H.hi <= pr.xmin, offRight = H.lo >= pr.xmax;
    if (offLeft || offRight) {
        ImFont* f = ImGui::GetFont();
        const bool tri = f && f->FindGlyphNoFallback((ImWchar)(offLeft ? 0x25C0 : 0x25B6));
        const char* t = offLeft ? (tri ? "\xE2\x97\x80 highlight outside" : "< highlight outside")
                                : (tri ? "highlight outside \xE2\x96\xB6" : "highlight outside >");
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const float y = (pr.p0.y + pr.p1.y - ts.y) * 0.5f;
        const float x = offLeft ? pr.p0.x + 4 * app.uiScale : pr.p1.x - ts.x - 4 * app.uiScale;
        ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), histHlBandInk(230), t);
        histZoomProbeAdd("hl", offLeft ? "left" : "right");
        return;
    }
    // An OPEN side runs to the edge of the plot, because that is exactly what
    // the end bar it touched means; a closed one lands on its own value.
    float x0 = H.loOpen ? pr.p0.x : pr.at(H.lo, pr.ymin).x;
    float x1 = H.hiOpen ? pr.p1.x : pr.at(H.hi, pr.ymin).x;
    x0 = std::clamp(x0, pr.p0.x, pr.p1.x);
    x1 = std::clamp(x1, pr.p0.x, pr.p1.x);
    if (x1 < x0) std::swap(x0, x1);
    // A band narrower than a pixel would leave the plot blank while the image
    // is painted magenta. Armed is never invisible.
    if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, pr.p0.y), ImVec2(x1, pr.p1.y), histHlBandInk(40));
    dl->AddRect(ImVec2(x0, pr.p0.y), ImVec2(x1, pr.p1.y), histHlBandInk(150));
}

// The plot's one drag gesture, which is TWO (#309): zoom and highlight. Which
// one a plain left-drag is comes from the heading row's `drag: [zoom]
// [highlight]` switch (app.histDragMode); Shift, held when the button goes
// down, makes it the other. Decided at the PRESS, so letting go of Shift
// mid-drag does not turn a highlight into a zoom under the cursor.
//
// Both commit on RELEASE. Nothing is re-binned or repainted while the mouse is
// down - the band follows live and the work happens once, which is the Temporal
// box-zoom's gesture economy and the reason a 12 Mpx frame is not re-rendered
// per mouse-move. The vertical extent carries no meaning (y is frequency), so
// the band is always full height, in blue for a zoom and magenta for a
// highlight.
//
// Double-click undoes what a PLAIN drag does: the zoom in zoom mode, the
// highlight in highlight mode. The other one has its button on screen
// (`reset zoom` on the heading row, `clear` on the highlight's footer line).
//
// Both are taken against the bin grid on the axis, i.e. the zoomed one while
// zoomed - so a highlight dragged on a zoomed plot snaps to the fine bins it
// was drawn on, and its end bins are closed (nothing folds into them there).
static void histDragGesture(const PlotRect& pr, ImageDoc* im, const App::HistState& H) {
    if (!pr.ok || !im) return;
    static ImVec2 dragA;
    static bool dragging = false;
    static bool dragZoom = false;
    // WHICH plot the drag began on. Side by side calls this twice per frame
    // with two different rectangles, and a gesture is one gesture: without
    // this, a drag started in the B half was completed by the A half's call
    // (A is drawn first), which unmapped B's screen x through A's rectangle
    // and armed an interval nobody drew - pinned to bin 255, because every
    // point of the right-hand plot is past the left-hand plot's xmax.
    static float dragOn = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hov = ImGui::IsMouseHoveringRect(pr.p0, pr.p1) &&
                     ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (app.histDragMode == 0) histZoomReset(im);
        else                       histHlClear();
        dragging = false;
        return;
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragA = ImGui::GetMousePos();
        dragOn = pr.p0.x;
        dragging = true;
        dragZoom = histDragZooms(ImGui::GetIO().KeyShift);
    }
    if (dragging && dragOn != pr.p0.x) return;      // the other half's plot
    if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetMousePos();
        float x0 = std::clamp(std::min(dragA.x, m.x), pr.p0.x, pr.p1.x);
        float x1 = std::clamp(std::max(dragA.x, m.x), pr.p0.x, pr.p1.x);
        dl->AddRectFilled(ImVec2(x0, pr.p0.y), ImVec2(x1, pr.p1.y),
                          dragZoom ? histZoomInk(30) : histHlBandInk(40));
        dl->AddRect(ImVec2(x0, pr.p0.y), ImVec2(x1, pr.p1.y),
                    dragZoom ? histZoomInk(160) : histHlBandInk(150));
    }
    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging = false;
        const HistHlRange r = histHlFromDrag(pr, dragA, ImGui::GetMousePos(),
                                             H.binOrigin, H.binW, !H.zoomed, H.nBins);
        if (!r.ok) return;                    // a click with jitter is not a drag
        if (dragZoom) {
            histZoomApply(im, r.lo, r.hi, H.binW, H.nBins);
            return;
        }
        // The binding is decided HERE, once, at the moment of the drag - and a
        // document on which "in range" would be ambiguous is declined out loud
        // rather than painted on a guess.
        const std::string no = histHlRefusalFor(im, app.histPlane);
        if (!no.empty()) {
            toast(no, true);
            histHlProbeSet("refused", no);
            return;
        }
        histHlArm(r.lo, r.hi, r.loOpen, r.hiOpen, app.histPlane,
                  app.histPlane >= 0 ? H.seriesNames[app.histPlane] : nullptr);
    }
}

static void drawPanelHistogram() {
    g_histSideProbe.clear();
    g_histZoomProbe.clear();
    ImageDoc* im = cur();
    histZoomSync(im);
    if (im && im->w > 0 && im->h > 0) {
        float zLo, zHi;
        histZoomRange(zLo, zHi);
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
        recomputeHistogramIfNeeded(im, app.hist[0], std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::quiet_NaN(), zLo, zHi);
        // Every other side is binned on A's black/white: one x axis, one bin
        // grid, or the curves are not comparable. Sizes may differ freely - the
        // y axis below normalises that away. Skipped while frames are being
        // stepped: an empty slot is always filled, a filled one waits for the
        // stepping to stop and is labelled stale until then. The slots take
        // B's rule unchanged, so N of them cannot turn a held arrow key into N
        // full passes over N images. A zoom is A's grid too, so it reaches
        // every side the same way.
        if (Bim && (!abStepBusy() || app.hist[1].uid == 0))
            recomputeHistogramIfNeeded(Bim, app.hist[1], effBlack(*im), effWhite(*im),
                                       zLo, zHi);
        for (const ResolvedSlot& rs : xslots)
            if (rs.idx < app.histExtra.size() &&
                (!abStepBusy() || app.histExtra[rs.idx].uid == 0))
                recomputeHistogramIfNeeded(rs.doc, app.histExtra[rs.idx],
                                           effBlack(*im), effWhite(*im), zLo, zHi);
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
        // (docs/features/compare/ab-stats-plan.md 4); the rows are the axis that grows.
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
        // ---- what a drag on the plot does (#309), on the heading row of the
        // plot it acts on: a modifier nobody can see is not a way to find the
        // second gesture. The lit button is the PLAIN drag; Shift gives the
        // other, and the dim note after them says which that is.
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("drag:");
            auto modeButton = [&](const char* label, int mode) {
                ImGui::SameLine();
                const bool lit = app.histDragMode == mode;
                if (lit) {
                    const ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                    ImGui::PushStyleColor(ImGuiCol_Button, a);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a);
                }
                if (ImGui::SmallButton(label) && app.histDragMode != mode) {
                    app.histDragMode = mode;
                    savePrefs();
                }
                if (lit) ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(mode == 0
                        ? "Drag on the plot zooms: the bins are laid again on the values\n"
                          "you dragged, so they get finer (down to 1 DN per bin for\n"
                          "integer data). Double-click resets.\n"
                          "Shift+drag highlights a value range instead."
                        : "Drag on the plot highlights a value range on the image.\n"
                          "Double-click clears it. Shift+drag zooms instead.");
            };
            modeButton("zoom##histdrag", 0);
            modeButton("highlight##histdrag", 1);
            ImGui::SameLine();
            ImGui::TextDisabled(app.histDragMode == 0 ? "(Shift: highlight)" : "(Shift: zoom)");
            ImGui::SameLine();
            if (ImGui::Checkbox("zoom sets display range##hist", &app.histZoomSetsRange))
                savePrefs();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On: a zoom also sets the display range to it, exactly as\n"
                                  "typing it into Range would (Per stack / Linked / A/B rules).\n"
                                  "Resetting the zoom gives back the range from before the\n"
                                  "first zoom - unless the range was changed in between.\n"
                                  "Off: the zoom never touches the display range.");
            if (app.histZoom.on) {
                ImGui::SameLine();
                if (ImGui::SmallButton("reset zoom##hist")) histZoomReset(im);
            }
            histZoomProbeAdd("mode", app.histDragMode == 0 ? "zoom" : "highlight");
        }
        static const ImU32 CFA_COLS[4] = { IM_COL32(255, 92, 92, 170), IM_COL32(120, 230, 120, 170),
                                           IM_COL32(60, 180, 140, 170), IM_COL32(92, 155, 255, 170) };
        static const ImU32 RGB_COLS[3] = { IM_COL32(255, 92, 92, 150), IM_COL32(79, 221, 107, 150),
                                           IM_COL32(92, 155, 255, 150) };
        const ImU32 ODD_COL = IM_COL32(185, 192, 200, 190);   // a series only one side has
        bool cfaHist = im->ch == 1 && im->cfa != 0;

        // ---- plane selector: four CFA planes times two sides is eight curves
        // on one axis. Same control as the ROI table's, and it filters EVERY
        // side, so a comparison is always plane against the same plane.
        //
        // `app.histPlane` is SHARED with the Projection panel (ruling 2 on #60,
        // 2026-08-14): narrowing to one plane is a statement about what is
        // being compared, not about which panel you happen to be reading, and
        // two panels quietly holding two different planes puts one comparison
        // on screen twice with two different subjects. Changing it here moves
        // the profiles too, and the profile panel's own combo moves this one.
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
        // has to leave room for.
        //
        // Side by side holds ONE PANE PER SIDE, at any N (ruling 1 on #60,
        // 2026-08-14: 「splitの場合は横並び(A, B, Cなら3つならぶ)」). Two
        // halves were the two-image spelling of the same thing, and while they
        // were nailed in, split - the layout that tiles every armed slot on the
        // image - was answering with a plot of A and B and a sentence about the
        // rest. That is half of "histogram が3個以上にならん".
        //
        // At large N the panes SHRINK rather than scroll (ruling 3), and when a
        // pane would fall under the minimum the panel says so and retreats to
        // the overlay - the threshold is per PANE, exactly as the Temporal
        // chart already measures it.
        const size_t nSides = sides.size();
        const float minSide = AB_MIN_SIDE * app.uiScale;
        // nSides, not Bim: a B with no pixels to measure (a remote preview
        // placeholder) is not a side, but the letters behind it still are, and
        // the image tiles them.
        bool side = nSides > 1 && abSideBySide();
        const float paneW = (ImGui::GetContentRegionAvail().x -
                             ImGui::GetStyle().ItemSpacing.x * (float)(nSides - 1))
                          / (float)std::max<size_t>(nSides, 1);
        bool tooNarrow = side && paneW < minSide;
        if (tooNarrow) side = false;

        // ---- how many sides the PLOT can take. The table above always takes
        // all of them; a SHARED axis has two hard limits and both of them have
        // to be SPOKEN, never applied in silence:
        //   - PLANES. Four CFA planes times five slots is twenty curves, which
        //     is not a comparison, it is a texture. So slots overlay when ONE
        //     plane is on the axis (docs/features/compare/compare-n.md 4 - the general form of
        //     ab-stats-plan 2's "eight curves are unreadable"), and the plane
        //     selector above is how you get there.
        //   - INK. slotInk can tell slotInkCount() slots apart and then wraps;
        //     drawing past that would put two slots in one colour, which is
        //     this panel omitting a slot again with a different mechanism.
        // Both are properties of sharing one axis, so NEITHER applies side by
        // side: a pane holds one side, wears its letter in a band, and colours
        // by plane the way a single-image histogram always has.
        // Whatever is left off is named on screen, right under the plot.
        const bool onePlane = nDrawn <= 1;
        size_t nPlot;
        const char* offWhy = nullptr;
        if (side) {
            nPlot = nSides;                // one pane each: nothing is left off
        } else if (!xslots.empty() && !onePlane) {
            nPlot = Bim ? 2 : 1;           // the A|B pair the two-image modes mean
            offWhy = "pick ONE plane above and every slot overlays on it";
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
        // places bin b positionally, at b/nBins of the rect, so the rect has to
        // span exactly what the bins span - and since the grid snaps outward
        // (histBinGrid) those two are no longer the same numbers. Drawn against
        // black..white the whole staircase would be stretched onto a narrower
        // axis and every bar would sit at the wrong pixel value. Every side is
        // binned on A's grid, so one pair of limits serves all of them.
        const float xLo = H.binW > 0 ? H.binOrigin : effBlack(*im);
        const float xHi = H.binW > 0
                            ? (float)((double)H.binOrigin + (double)H.nBins * (double)H.binW)
                            : effWhite(*im);

        // one curve, one way to draw it: filled bars, solid outline, dashed
        // outline. The staircase is built once and reused by all three.
        auto plotSeries = [&](const PlotRect& pr, const uint32_t bins[256], int nBins,
                              double sc, ImU32 col, int style,  // 0 fill, 1 solid, 2 dashed
                              float w = 1.4f) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float pw = pr.p1.x - pr.p0.x, ph = pr.p1.y - pr.p0.y;
            auto yOf = [&](uint32_t v) {
                double u = v * sc;
                double f = app.histLog ? (logTop > 0 ? log1p(u) / logTop : 0.0) : u / yTop;
                return pr.p1.y - (float)std::clamp(f, 0.0, 1.0) * ph;
            };
            // the side's own nBins, not 256: a zoomed integer grid can use
            // fewer slots, and the rect spans exactly the slots in use.
            const int nb = std::clamp(nBins, 1, 256);
            const float fn = (float)nb;
            if (style == 0) {
                for (int b = 0; b < nb; b++) {
                    if (!bins[b]) continue;
                    float bx0 = pr.p0.x + (float)b / fn * pw;
                    dl->AddRectFilled(ImVec2(bx0, yOf(bins[b])),
                                      ImVec2(bx0 + pw / fn + 0.5f, pr.p1.y), col);
                }
                return;
            }
            std::vector<ImVec2> pts;
            pts.reserve(512);
            for (int b = 0; b < nb; b++) {
                float bx0 = pr.p0.x + (float)b / fn * pw;
                float bx1 = pr.p0.x + (float)(b + 1) / fn * pw;
                float y = yOf(bins[b]);
                pts.push_back(ImVec2(bx0, y));
                pts.push_back(ImVec2(bx1, y));
            }
            if (style == 1) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, w);
            else addDashedPolyline(dl, pts.data(), (int)pts.size(), col, w,
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
        // ...and side by side does NOT use it: a pane holds one side, its band
        // says which, and the plane colours are then free to mean planes again.
        const bool bySlotInk = !side && nPlot > 2;
        // Which side the image is showing this instant, drawn HEAVY so that the
        // overlay says it too (ruling 1 on #60: 「重なって表示される場合は
        // 重ねる - ただし表示中のスロットが分かる状態にする(太線等)」).
        // Only on a shared axis: side by side already gives every side its own
        // pane and its own name band, so weight there would repeat the band.
        // ...and only while the list really is A, B, ...: blinkSideIndex()
        // counts in the canvas's seats, so with no measurable B its "1" would
        // put the weight on C.
        const int emphIdx = side || !Bim ? -1 : abEmphSide();
        const bool emphOn = emphIdx >= 0 && (size_t)emphIdx < nPlot && nSides > 1;
        // Four CFA planes (or three RGB ones) as eight filled areas is
        // unreadable, so at three drawn curves or more BOTH sides become
        // outlines. Below that A keeps its fill and only B is an outline.
        // Slot ink means outlines throughout: N filled areas hide each other,
        // and the fill was only ever how "this one is A" got said when there
        // were two. With a legend and one hue per letter, the hue says it.
        // Emphasis means outlines throughout for a different reason: weight is
        // the message, and weight cannot be compared between a filled area and
        // a line - a filled A would read as the lead whether or not it is.
        const bool outlineA = (nSides > 1 && nDrawn >= 3) || bySlotInk || emphOn;
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
                plotSeries(pr, sd.H->bins[s], sd.H->nBins, norm ? sd.sc : 1.0, col,
                           i == 0 && !outlineA ? 0 : 1,
                           abCurveW(1.4f, emphOn && (int)i == emphIdx));
            }
        };
        // Every side the shared axis took, in one clip rect.
        auto drawAll = [&](const PlotRect& pr) {
            if (!pr.ok) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            for (size_t i = 0; i < nPlot; i++) drawSide(pr, i);
            dl->PopClipRect();
        };
        // ...and ONE side, in its own pane.
        auto drawPane = [&](const PlotRect& pr, size_t i) {
            if (!pr.ok) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            drawSide(pr, i);
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
        // ...and the value highlight's own lines, one per side that answers,
        // COUNTED HERE with the rest rather than discovered when they are
        // drawn: everything below this point is sized against footerH, and a
        // line that appears after the layout pushes the whole footer out of the
        // bottom of the panel - which is exactly the state where the highlight
        // must not be the thing that scrolls away.
        // ONE ROW PER SLOT while the highlight is armed, whether or not that
        // slot is on screen (板 275 R2). Counting the DRAWN panes here is what
        // made the footer breathe: hold-B removed A's row for as long as the key
        // was down (§7 says a temporary Hold B does not change the pane
        // composition, and this contradicted it), and blink swapped two rows
        // every half second, so the plot above resized under the cursor. A row
        // that is not on screen carries its reason and no number, which is also
        // what keeps the older defect closed - the number is what must not
        // survive, not the letter.
        const int hlLines = app.highlight.on ? (int)nSides : 0;
        float footerH = ImGui::GetTextLineHeightWithSpacing() *
                            (float)(nSides + hlLines)
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
            if (emphOn) g_histSideProbe += "emph=" + sides[emphIdx].letter + ";";
            g_histSideProbe += std::string("plane=") +
                               (app.histPlane < 0 ? "all" : H.seriesNames[app.histPlane]) + ";";
            // ...and WHICH arrangement drew them, because "every side got a
            // curve" is true of one pane each and of one shared axis, and the
            // ruling is about which of the two the image asked for.
            g_histSideProbe += std::string("layout=") + (side ? "side" : "overlay") + ";";
        }

        // ---- the zoom, declared on the plot (#309). A magnified chart that
        // does not announce itself reads as the whole distribution, so while
        // zoomed every pane carries the Temporal chart's sentence, top right -
        // naming the gesture that undoes it in THIS drag mode, because in
        // highlight mode a double-click clears the highlight instead. And the
        // display range, where it falls inside the zoom, is marked with a
        // dashed line at black and at white: with the linked option off the
        // picture is still drawn on that range, and this is where it sits on
        // the axis being looked at.
        bool zoomSaid = false;
        auto zoomOverlay = [&](const PlotRect& pr) {
            if (!pr.ok || !H.zoomed) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float eb = effBlack(*im), ew = effWhite(*im);
            std::string marks;
            const float dash = 4 * app.uiScale;
            auto mark = [&](float v, const char* name) {
                if (!(v >= pr.xmin && v <= pr.xmax)) return;
                const float x = pr.at(v, pr.ymin).x;
                ImVec2 seg[2] = { ImVec2(x, pr.p0.y), ImVec2(x, pr.p1.y) };
                addDashedPolyline(dl, seg, 2, IM_COL32(225, 225, 225, 170), 1.0f, dash, dash);
                const ImVec2 ts = ImGui::CalcTextSize(name);
                dl->AddText(ImVec2(std::min(x + 3 * app.uiScale, pr.p1.x - ts.x),
                                   pr.p1.y - ts.y - 2 * app.uiScale),
                            IM_COL32(225, 225, 225, 200), name);
                marks += std::string(marks.empty() ? "" : ",") + name;
            };
            mark(eb, "black");
            mark(ew, "white");
            char zt[128];
            snprintf(zt, sizeof zt, app.histDragMode == 0
                                        ? "zoomed %.6g..%.6g - double-click to reset"
                                        : "zoomed %.6g..%.6g - reset zoom above",
                     (double)H.zoomLo, (double)H.zoomHi);
            const ImVec2 ts = ImGui::CalcTextSize(zt);
            dl->AddText(ImVec2(pr.p1.x - ts.x - 4 * app.uiScale, pr.p0.y + 2 * app.uiScale),
                        AB_AMBER32, zt);
            if (!zoomSaid) {
                zoomSaid = true;
                char zr[64];
                snprintf(zr, sizeof zr, "%.6g..%.6g", (double)H.zoomLo, (double)H.zoomHi);
                histZoomProbeAdd("zoomed", zr);
                histZoomProbeAdd("note", zt);
                histZoomProbeAdd("marks", marks);
            }
        };

        if (!side) {
            PlotRect hp = beginPlot(xl, yl, xLo, xHi, 0.0f, 1.0f,
                                    false, false, plotH);
            histHlBand(hp);                 // under the curves
            drawAll(hp);
            zoomOverlay(hp);
            histDragGesture(hp, im, H);
            if (!leg.empty())  slotLegendRow(leg, true);
            else if (Bim)      drawABLegendRow(abDocLabel(im), bLabel, bStale);
        } else {
            // Equal widths, never splitFrac: comparing shapes needs plots of the
            // SAME width, whether there are two of them or five. What the layout
            // copies from the image is the ORDER (A first), not the divider
            // position - which is also why the panes stay in slot order as
            // letters are armed and released.
            float childH = plotH + ImGui::GetFontSize() * 3 + 12 * app.uiScale
                         + ImGui::GetTextLineHeight() + 8 * app.uiScale;
            for (size_t i = 0; i < nSides; i++) {
                if (i) ImGui::SameLine();
                ImGui::BeginChild(("##histSide" + sides[i].letter).c_str(),
                                  ImVec2(paneW, childH), false,
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
                drawABBand(sides[i].letter.c_str(), sides[i].label, sides[i].stale);
                {   // The band and the gesture live on EVERY pane (#68), not on
                    // two halves: one axis, one interval, and each pane answers
                    // with its own pixels. Both are asked against A's bin grid
                    // (im, H) whichever pane they are drawn in - every side is
                    // binned on it, which is what makes the panes comparable at
                    // all, and a per-pane grid would make the same drag select
                    // a different range in each one.
                    // identical axis ranges by construction: same xl/yl, same limits
                    PlotRect hp = beginPlot(xl, yl, xLo, xHi, 0.0f, 1.0f,
                                            false, false, plotH);
                    histHlBand(hp);
                    drawPane(hp, i);
                    zoomOverlay(hp);
                    histDragGesture(hp, im, H);
                }
                ImGui::EndChild();
            }
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
            // Zoomed (#309), what fell off the grid, counted the way the clip
            // rates are - a share of this side's finite sampled values - and
            // beside them, because it is the same kind of statement about the
            // plot: these pixels exist and no bar shows them.
            std::string out;
            if (sd.H->zoomed) {
                const double fin = (double)std::max<size_t>(sd.H->finite, 1);
                char ob[160];
                snprintf(ob, sizeof ob, " | outside zoom <%.6g %.2f%%  >=%.6g %.2f%%",
                         (double)sd.H->zoomLo, (double)sd.H->outLoN / fin * 100.0,
                         (double)sd.H->zoomHi, (double)sd.H->outHiN / fin * 100.0);
                out = ob;
                histZoomProbeAdd("out", sd.letter + ":" + out.substr(3));
            }
            ImGui::TextDisabled("%s  %zu px | <black %.2f%%  >white %.2f%%%s%s",
                                sd.letter.c_str(), sd.H->sampled,
                                sd.H->clipLo * 100.0, sd.H->clipHi * 100.0, out.c_str(),
                                tail.c_str());
            if (sd.stale) {   // never inside the disabled string: it must stand out
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
            }
        }
        // ---- the value highlight: what is armed, and what it found ----------
        // Printed on EVERY frame it is armed. This sentence is the whole reason
        // "the highlight is not saved in the session" is a safe decision (#68
        // §1): a lens left on cannot become an invisible state, because the
        // screen showing the paint always carries the line explaining it. It is
        // also where the measurement earns the right to be quoted - the count,
        // its denominator and the REGION it was taken over, every time, because
        // the header above may be saying "(selected ROI)" about the curves
        // while this is always the whole frame.
        if (app.highlight.on) {
            std::string joined;
            const std::string head = histHlRangeSaid(im->dtype);
            bool first = true;
            auto emit = [&](const std::string& line, bool dim) {
                if (dim) ImGui::TextDisabled("%s", line.c_str());
                else     ImGui::TextUnformatted(line.c_str());
                if (first) {          // one clear button, on the first line
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear##hl")) histHlClear();
                    first = false;
                }
                // The probe carries the dim marker too: "the row is still there,
                // quietly" is the claim R2 makes, and a claim about what the
                // screen looks like has to be readable by the selftest.
                joined += std::string(joined.empty() ? "" : "|") + (dim ? "[dim] " : "") + line;
            };
            // ONE ROW PER SLOT, in slot order. A row whose pane is on the screen
            // in this pass carries the count the PAINT measured; a row whose
            // pane is not carries the reason and NO NUMBER.
            //   The number is the thing that must not survive: printing a Split
            //   count for a pane Wipe had dropped was 板 275 DEFECT(3), and
            //   deriving one from the bins would be worse (a sampled, possibly
            //   ROI-limited population wearing this count's face).
            //   The ROW must survive, because this panel's own discipline is to
            //   name what it could not show rather than go quiet, and because a
            //   row that appears and disappears moves the plot above it.
            // ONE reading of the screen for the whole footer: every row is
            // answered against the same stamped set the canvas badge read.
            const bool anyShown = !histHlShownPanes().empty();
            for (const HistSide& sd : sides) {
                const ImageDoc* d = sd.H->img;
                const std::string said = histHlSideSaid(d, sd.letter);
                if (!said.empty()) emit(head + "  -  " + said, false);
                else emit(head + "  -  " + sd.letter + ": " +
                          histHlNotShownWhy(d, anyShown), true);
            }
            histHlProbeSet("panel", joined);
        } else {
            histHlProbeSet("panel", "");
        }
    }

}

// ---- the frame statistics: EVERY pixel of the region (issue #283) -----------
// fStat used to be gathered inside the H pass below, which visits every column
// of every step-th ROW. While step == cell that is every pixel; once the H
// pass strides (step > cell: mono from 4 Mpx, Bayer from 3 Mpx, quad Bayer from 2.5 Mpx) - a 4096x3000
// frame is step 6 - it was one row in six, while the panel said "the
// whole region". The p-p of a subset can only be smaller than the region's,
// and a row-periodic pattern aliases with the stride, so sigma (frame) and
// p-p frame were biased exactly where FPN work looks. They are a pass of their
// own now, over every row and every column, in the same order the H pass used
// to visit them - so wherever the H pass did cover every pixel, the sums are
// the same additions in the same order and the numbers do not move by a bit.
//
// Pure: it reads the buffer and the layout it is given and nothing in App, so
// it runs unchanged on the UI thread or on a worker. Returns false when
// `cancel` stopped it (checked once a row), leaving `out` unspecified.
static bool projFrameMoments(const float* px, int imgW, int ch, int cfaType, int cfaPattern,
                             int nSeries, bool allRow, int rx, int ry, int rw, int rh,
                             App::ProjState::Stats out[5],
                             const std::atomic<bool>* cancel) {
    const bool cfa = cfaType != 0;
    double fSum[5] = {}, fSum2[5] = {};
    size_t fN[5] = {};
    double fMn[5] = { DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX };
    double fMx[5] = { -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
    auto put = [&](int slot, float val) {
        fSum[slot] += val; fSum2[slot] += (double)val * val; fN[slot]++;
        fMn[slot] = std::min(fMn[slot], (double)val);
        fMx[slot] = std::max(fMx[slot], (double)val);
    };
    for (int y = 0; y < rh; y++) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        const float* row = &px[((size_t)(ry + y) * imgW + rx) * ch];
        for (int x = 0; x < rw; x++) {
            const float* src = row + (size_t)x * ch;
            int lo = 0, hi = nSeries;
            if (cfa) { lo = cfaChannelAtRaw(cfaType, cfaPattern, rx + x, ry + y); hi = lo + 1; }
            for (int s = lo; s < hi; s++) {
                float val = cfa ? src[0] : src[std::min(s, ch - 1)];
                if (!std::isfinite(val)) continue;
                put(s, val);
                if (allRow) put(4, val);
            }
        }
    }
    for (int s = 0; s < 5; s++) {
        App::ProjState::Stats& f = out[s];
        f = {};
        if (!fN[s]) continue;
        f.n = fN[s];                   // the pooling weight, published
        f.mean = fSum[s] / fN[s];
        double var = fSum2[s] / fN[s] - f.mean * f.mean;
        f.sd = sqrt(var > 0 ? var : 0);
        f.mn = fMn[s]; f.mx = fMx[s]; f.pp = f.mx - f.mn;
        f.pct = f.mean != 0 ? f.sd / fabs(f.mean) * 100.0 : 0.0;
        f.valid = true;
    }
    return true;
}

// What fStat must answer for THIS document and region right now. nSeries and
// allRow are the recompute's own rule (one plane per CFA phase, otherwise up
// to three channels; "all" only with more than one plane), restated here
// because the key has to be known BEFORE the recompute decides whether to run.
static App::ProjState::FrameKey projFrameKeyOf(const ImageDoc* im, int rx, int ry,
                                              int rw, int rh) {
    App::ProjState::FrameKey k;
    const bool cfa = im->ch == 1 && im->cfa != 0;
    const int nSeries = cfa ? 4 : std::min(im->ch, 3);
    k.uid = im->uid; k.dataRev = im->dataRev;
    k.cfa = im->cfa; k.cfaPattern = im->cfaPattern;
    k.rx = rx; k.ry = ry; k.rw = rw; k.rh = rh;
    k.allRow = app.projAllRow && nSeries > 1;
    k.pixels = im->px().data();
    k.count = im->px().size();
    return k;
}

// WHICH slots get frame statistics: every plane, and the pooled row when it is
// on - and none at all on a refused side (the recompute's momSlot list, as a
// mask, from the state it has already written).
static void projFrameMask(const App::ProjState& P, bool mask[5]) {
    for (int s = 0; s < 5; s++)
        mask[s] = P.unavail.empty() && ((s < 4 && s < P.nSeries) || (s == 4 && P.allRow));
}

// Start the pass for P.fKey on a worker. fStat is cleared and stays cleared
// until projFramePump installs the answer for the SAME key.
static void projFrameStart(ImageDoc* im, App::ProjState& P) {
    auto j = std::make_shared<App::ProjState::FrameJob>();
    j->key = P.fKey;
    j->src = im->src;
    j->px = P.fKey.pixels;
    j->imgW = im->w; j->ch = im->ch;
    j->cfa = (im->ch == 1) ? im->cfa : 0;
    j->cfaPattern = im->cfaPattern;
    j->nSeries = P.nSeries;
    j->allRow = P.fKey.allRow;
    projFrameMask(P, j->mom);
    for (int s = 0; s < 5; s++) P.fStat[s] = {};
    P.fPending = true;
    App::ProjState::FrameJob* raw = j.get();
    j->th = std::thread([raw]() {
        const App::ProjState::FrameKey& k = raw->key;
        projFrameMoments(raw->px, raw->imgW, raw->ch, raw->cfa, raw->cfaPattern,
                         raw->nSeries, raw->allRow, k.rx, k.ry, k.rw, k.rh,
                         raw->out, &raw->cancel);
        raw->done.store(true);
    });
    P.fJob = j;
    // the registry is pruned by whoever walks it; a run with no frame loop
    // (projFrameBusy) still starts jobs, so starting one prunes too
    for (size_t i = 0; i < g_projFrameJobs.size();)
        if (g_projFrameJobs[i].expired()) g_projFrameJobs.erase(g_projFrameJobs.begin() + (long)i);
        else i++;
    g_projFrameJobs.push_back(j);
    g_projFrameJobStarts++;
}

// The landing, on the UI thread. A finished job is installed only if it was
// not stopped AND its key is still the one `want` names; anything else is
// dropped unread. A pending state left without a job (a fence stopped it) is
// restarted over the CURRENT pixels - never the stopped job's, whose buffer
// the writer behind the fence has just replaced.
static void projFramePump(ImageDoc* im, App::ProjState& P,
                          const App::ProjState::FrameKey& want) {
    if (P.fJob && P.fJob->done.load()) {
        std::shared_ptr<App::ProjState::FrameJob> j = std::move(P.fJob);
        if (j->th.joinable()) j->th.join();
        if (!j->cancel.load() && P.fPending && j->key == P.fKey && j->key == want) {
            for (int s = 0; s < 5; s++)
                P.fStat[s] = j->mom[s] ? j->out[s] : App::ProjState::Stats{};
            P.fPending = false;
        }
    }
    if (P.fPending && !P.fJob && P.fKey == want) projFrameStart(im, P);
}

// Drive P's pending frame statistics to the end on THIS thread: join the
// worker, then land it through the same pump the frame loop uses. For a run
// with no frame loop (a selftest) and for a file export, which must write
// numbers rather than "computing...". waitForOpenJob's twin.
static void recomputeProjectionIfNeeded(ImageDoc* im, App::ProjState& P);
static void projFrameWait(ImageDoc* im, App::ProjState& P) {
    for (int guard = 0; P.fPending && guard < 8; guard++) {
        if (P.fJob && P.fJob->th.joinable()) P.fJob->th.join();
        recomputeProjectionIfNeeded(im, P);
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
    // the frame statistics land (or restart) BEFORE the cache answers: a
    // region that has not moved still has a pass to collect
    const App::ProjState::FrameKey fWant = projFrameKeyOf(im, rx, ry, rw, rh);
    projFramePump(im, P, fWant);
    if (P.uid == im->uid && P.dataRev == im->dataRev && P.mode == app.projMode &&
        P.cfa == im->cfa && P.cfaPattern == im->cfaPattern &&
        P.allReq == app.projAllRow &&
        P.rx == rx && P.ry == ry && P.rw == rw && P.rh == rh)
        return;
    if (app.annBusy && P.uid == im->uid) {         // mid-drag: keep the last profile
        // ...but NOT the last frame statistics. The curves lag the rectangle
        // visibly; a number in a table does not, and the frame cells would be
        // the previous region's numbers under the region being drawn - the
        // one thing #283's ruling says they never are. Comparing the key costs
        // nothing; the pass itself waits for the drag to end, like the profile.
        if (fWant != P.fKey && !P.fPending) {
            P.fJob.reset();
            for (int s = 0; s < 5; s++) P.fStat[s] = {};
            P.fPending = true;
        }
        return;
    }
    P.img = im; P.uid = im->uid; P.dataRev = im->dataRev;
    P.mode = app.projMode; P.cfa = im->cfa; P.cfaPattern = im->cfaPattern;
    P.allReq = app.projAllRow;     // the ASK, keyed on above; allRow is the answer
    P.rx = rx; P.ry = ry; P.rw = rw; P.rh = rh; P.roiUsed = roiUsed;
    // Always a mean-based decomposition of this frame/ROI, independent of the
    // plot's reduce mode. Do not combine the orthogonally strided plot samples.
    P.noise = computeProfileNoise(*im, rx, ry, rw, rh);

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
    // ...and whether there is a profile to compute AT ALL. A decimated remote
    // preview of a mosaic carries every Nth row and column of the sensor, so
    // the mosaic phase is gone: cfaChannelAt over preview coordinates would
    // name a plane the pixel is not from, and every plane's curve would be
    // another plane's values under this plane's name. Refused in the wording
    // computeProfileNoise already uses for the same cause, rather than pooled
    // into one dense series - a curve whose phase has been destroyed is not
    // something to read shading off, and an empty plot with a reason is the
    // better of the two. The real planes arrive with the full-resolution frame.
    P.unavail.clear();
    if (cfa && im->src->remoteStep > 1)
        P.unavail = "full-resolution remote pixels required: this is a decimated "
                    "preview, and it does not keep the mosaic phase - no plane's "
                    "profile can be told from the others'. The planes appear when "
                    "the full-resolution frame lands.";
    //
    // TWO LISTS, because the pooled row does not get the same treatment as a
    // plane on a mosaic. `fillSlot` is the slots that get a PROFILE (P.h/P.v);
    // `momSlot` is the slots that get REGION moments (fStat).
    //
    // ON A MOSAIC THE POOLED ROW GETS NO PROFILE AT ALL. Pooling the planes
    // makes a DENSE column mean out of their level difference, so the profile
    // exists, is drawable, and means the inter-plane offset rather than
    // anything about the columns: `all in view [0 .. 15]: p-p 415` on a 16x16
    // RGGB frame, of which 400 is the level step between the planes and 15 is
    // the only column structure there is. Nothing may draw it, its sigma and
    // p-p are refused, and the per-axis mean it would carry is the pooled
    // REGION mean computed a second time - so there is nothing to keep and a
    // dense array no reader is allowed to touch is a trap. The region moments
    // are the whole of what "all" means on a mosaic.
    int fillSlot[5], nAcc = 0;
    int momSlot[5], nMom = 0;
    if (P.unavail.empty()) {
        for (int s = 0; s < P.nSeries; s++) { fillSlot[nAcc++] = s; momSlot[nMom++] = s; }
        if (P.allRow) {
            momSlot[nMom++] = 4;
            if (!cfa) fillSlot[nAcc++] = 4;
        }
    }
    bool hasProfile[5] = {};
    for (int i = 0; i < nAcc; i++) hasProfile[fillSlot[i]] = true;
    // ...and the list itself goes into the state, so the panel's readers walk
    // the slots the pass filled instead of counting them (ProjState::fill).
    for (int i = 0; i < nAcc; i++) P.fill[i] = fillSlot[i];
    P.nFill = nAcc;

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
    {   // EVERY slot the pass did not fill is cleared, not just slot 4. An
        // unfilled slot keeps the PREVIOUS document's profile and statistics
        // under this document's names: open a Bayer frame, then a 3-channel
        // one, and slot 3 still held the mosaic's B plane while the readers
        // that counted rather than walked went looking for it as "ch3".
        bool moments[5] = {};
        for (int i = 0; i < nMom; i++) moments[momSlot[i]] = true;
        for (int s = 0; s < 5; s++) {
            if (!hasProfile[s]) {
                P.h[s].clear(); P.v[s].clear();
                P.hStat[s] = {}; P.vStat[s] = {};
            }
            if (!moments[s]) P.fStat[s] = {};
        }
    }
    // The region moments (fStat) are NOT gathered here any more: they are
    // projFrameMoments' pass over every pixel (issue #283), started below
    // once the stride is known. Where the H pass below covers every pixel -
    // step == cell - the H profile and the region share one sample set and
    // the region mean and the mean of the column means are ONE NUMBER,
    // computed twice; they are asserted equal to within one float ulp
    // (--precision-selftest P7c), and were 0.015 DN apart while the column
    // sum was a float. Once the H pass strides (step > cell: mono from 4 Mpx, Bayer from 3 Mpx, quad Bayer from 2.5 Mpx)
    // the H profile is over every step-th row
    // and the region is over all of them, so the two means are different
    // measurements - which is what the per-axis table's "mean (axis)" says.
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
    // WHERE each plane's samples land, recorded as the pass visits them
    // (ProjState::hPhase, core/app/state.h). The reader of a mosaic profile has
    // to tell a hole in the lattice from a hole in the data, and this is the
    // pass's own answer to the first half - taken from cfaChannelAt below, so
    // the panel never spells the phase a second time.
    P.cell = cell;
    for (int s = 0; s < 5; s++) { P.hPhase[s] = 0; P.vPhase[s] = 0; }
    auto accumulate = [&](int x, int y, bool toH) {
        const float* src = &im->px()[((size_t)(ry + y) * im->w + (rx + x)) * im->ch];
        int lo = 0, hi = P.nSeries;
        if (cfa) { lo = cfaChannelAt(*im, rx + x, ry + y); hi = lo + 1; }
        // every value goes into its own plane's row, and - when asked for -
        // into the pooled "all" row as well. Pooling here rather than in a
        // second pass keeps the two rows over EXACTLY the same samples.
        auto put = [&](int slot, float val) {
            if (hasProfile[slot]) {         // a slot with no profile - the pooled
                                            // row on a mosaic - has moments only
                double& acc = toH ? hAcc[slot][x] : vAcc[slot][y];
                if (useMax) acc = std::max(acc, (double)val);
                else if (useMin) acc = std::min(acc, (double)val);
                else acc += val;
                (toH ? hN[slot][x] : vN[slot][y])++;
            }
        };
        // THE LATTICE IS WHERE THE PASS LOOKED, NOT WHERE IT FOUND SOMETHING.
        // Recorded here, before the finiteness test, from the cfaChannelAt call
        // above (ProjState::hPhase, core/app/state.h). Recording it inside
        // put() - after the test - made an offset whose samples are ALL
        // non-finite look like an offset the plane does not have, and then the
        // stroke walked straight past it and JOINED THE TWO SIDES OF A REAL
        // GAP. That is exactly the distinction the lattice exists to draw, lost
        // in the one case it matters: a quad-Bayer frame with one phase dead,
        // or any plane whose every sample at one offset is NaN.
        for (int s = lo; s < hi; s++) {
            if (hasProfile[s]) {
                if (toH) P.hPhase[s] |= 1u << (x % cell);
                else     P.vPhase[s] |= 1u << (y % cell);
            }
            if (P.allRow && hasProfile[4]) {
                if (toH) P.hPhase[4] |= 1u << (x % cell);
                else     P.vPhase[4] |= 1u << (y % cell);
            }
            float val = cfa ? src[0] : src[std::min(s, im->ch - 1)];
            if (!std::isfinite(val)) continue;
            put(s, val);
            if (P.allRow) put(4, val);
        }
    };
    // Each step lands on a mosaic cell and takes ALL of its lines, so every
    // plane is sampled the same number of times whatever the stride.
    if (nMom) {           // ...and nothing to gather means no pass at all: the
                          // refused preview, where accumulate would write into
                          // vectors that were never sized
        for (int y = 0; y < rh; y += step)          // H profile: every column
            for (int dy = 0; dy < cell && y + dy < rh; dy++)
                for (int x = 0; x < rw; x++) accumulate(x, y + dy, true);
        for (int y = 0; y < rh; y++)                // V profile: every row
            for (int x = 0; x < rw; x += step)
                for (int dx = 0; dx < cell && x + dx < rw; dx++) accumulate(x + dx, y, false);
    }
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

    // ---- the frame statistics (issue #283), keyed ------------------------
    // ...including the pooled row on a mosaic, which has ONLY these. Over a
    // region the H pass covers whole (step == cell) the pass is no bigger than
    // the one just made, so it runs here and nothing is ever pending; above
    // the cap it goes to a worker and fStat reads "computing..." until
    // projFramePump lands the answer for this same key. A job still running
    // for the PREVIOUS key is simply dropped: its result could only be
    // installed under a key that is no longer wanted.
    //   ...and a recompute that did not move the KEY - the reduce mode
    // changed, or forgetImage dropped the cache pointer - keeps the answer it
    // has or the pass it is waiting for: the pixels and the region are the
    // same, so "computing..." again would be a flash with nothing behind it.
    const bool keepFrame = fWant.uid != 0 && P.fKey == fWant && (P.fJob || !P.fPending);
    if (!keepFrame) {
        P.fJob.reset();
        P.fKey = fWant;
        P.fPending = false;
    }
    if (nMom && !keepFrame) {
        const bool async = step > cell && (!g_scriptedRun || g_forceAsyncProjFrame);
        if (async) {
            projFrameStart(im, P);
        } else {
            App::ProjState::Stats out[5];
            projFrameMoments(im->px().data(), im->w, im->ch, cfa ? im->cfa : 0,
                             im->cfaPattern, P.nSeries, P.allRow, rx, ry, rw, rh,
                             out, nullptr);
            bool mom[5];
            projFrameMask(P, mom);
            for (int s = 0; s < 5; s++) P.fStat[s] = mom[s] ? out[s] : App::ProjState::Stats{};
        }
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
            // (The pooled row on a mosaic never reaches here: it is not in
            // fillSlot, because it has no profile at all. See the two lists.)
            double sum = 0, sum2 = 0;
            double mn = DBL_MAX, mx = -DBL_MAX;
            size_t n = 0;
            for (float v : d) {
                if (!std::isfinite(v)) continue;
                sum += v; sum2 += (double)v * v;
                mn = std::min(mn, (double)v); mx = std::max(mx, (double)v);
                n++;
            }
            // n < 2, not n == 0: with one sample the variance below is 0 by
            // construction and `valid` would publish that exact zero as a
            // measurement. One column of a Bayer plane - a 2-wide ROI, or a
            // 3x3 one - then reported sigma_col = 0, which reads as a perfectly
            // uniform sensor. A single sample has a mean and no spread, and
            // this structure carries both together, so neither is published.
            if (n < 2) continue;
            st.n = n;
            st.mean = sum / n;
            // ddof=1 (docs/features/analysis/stats-taxonomy.md §8 判断2). These are the row and column
            // MEANS, not the pixels: sigma of them is A and B of the flat-field
            // decomposition T = sigma_p^2 + A + B, and docs/features/analysis/flat-field-stats.md fixes
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

