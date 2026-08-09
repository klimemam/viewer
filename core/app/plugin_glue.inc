// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- plugin glue
static psFrame makeFrame(const ImageDoc& im) {
    psFrame f = {};
    f.w = (uint32_t)im.w; f.h = (uint32_t)im.h; f.ch = (uint32_t)im.ch;
    f.dtype = PS_DTYPE_F32; f.loc = PS_MEM_CPU;
    f.data = (void*)im.px().data();
    f.pitch_bytes = (size_t)im.w * im.ch * sizeof(float);
    f.black = effBlack(im); f.white = effWhite(im);
    f.cfa_type = im.cfa; f.cfa_pattern = im.cfaPattern;   // enums mirror psCfa* by construction
    f.pts_us = -1;
    f.name = im.name.c_str();                             // valid only during the call
    f.meta_json = nullptr;
    return f;
}
static void anaEmitNumber(void* ctx, const char* key, double v) {
    auto* rows = (std::vector<std::pair<std::string, std::string>>*)ctx;
    char b[64]; snprintf(b, 64, "%.6g", v);
    rows->emplace_back(key ? key : "", b);
}
static void anaEmitText(void* ctx, const char* key, const char* v) {
    auto* rows = (std::vector<std::pair<std::string, std::string>>*)ctx;
    rows->emplace_back(key ? key : "", v ? v : "");
}
// V2 sink context: rows go to the grid, curves into AnalysisState::series
struct AnaEmit2Ctx {
    std::vector<std::pair<std::string, std::string>>* rows;
    App::AnalysisState* ana;
    int col;
    int colorIdx;
};
static void ana2Number(void* ctx, const char* key, double v) {
    char b[64]; snprintf(b, 64, "%.6g", v);
    ((AnaEmit2Ctx*)ctx)->rows->emplace_back(key ? key : "", b);
}
static void ana2Text(void* ctx, const char* key, const char* v) {
    ((AnaEmit2Ctx*)ctx)->rows->emplace_back(key ? key : "", v ? v : "");
}
static void ana2Series(void* ctx, const char* name, const char* xl, const char* yl,
                       const float* x, const float* y, uint32_t n) {
    auto* c = (AnaEmit2Ctx*)ctx;
    if (!y || n == 0 || n > 1000000) return;
    App::AnalysisState::Series s;
    s.name = name ? name : "series";
    s.xLabel = xl ? xl : "";
    s.yLabel = yl ? yl : "";
    s.ys.assign(y, y + n);
    if (x) s.xs.assign(x, x + n);
    s.col = c->col;
    s.colorIdx = c->colorIdx;
    c->ana->series.push_back(std::move(s));
}

// ---- serving a stack to a plugin (docs/abi-v3.md §5.1, §9.1) ----------------
//
// The host end of the PULL mouth. In this build the answer to "where do the
// frames live" is the easy one - §9.1: the viewer already holds every loaded
// frame as resident float32, so get_frame returns a POINTER and costs nothing,
// and release_frame has nothing to recycle. That is the first dividend of pull
// over an array: today's shape went into the ABI unchanged, while the two
// transports §9.2/§9.3 keep open (a bounded shared-memory window, peer-side
// execution) stay reachable without another version.
//
// WHO OWNS WHAT DURING THE CALL:
//   - the psFrame vector is built BEFORE analyze_stack and never touched
//     again while it runs, so a pointer handed out stays valid for exactly as
//     long as §5.1 promises - until analyze_stack returns;
//   - the pixels stay the viewer's. A plugin gets const psFrame* and the ABI
//     has never had a way to hand pixel ownership the other way;
//   - psStack::name and psFrame::name are borrowed strings, valid for the call
//     only. `name` below is this struct's own copy, NOT SeqInfo::name: the
//     table reseats on every mutation (App::SeqTable, PR #128), so nothing
//     here holds a SeqInfo* for one line longer than it takes to copy out of
//     it - and certainly not across a plugin call;
//   - App::images is not touched by any of this, and an ImageDoc lives behind
//     a unique_ptr, so the docs themselves cannot move under the call either.
struct StackServe {
    std::vector<psFrame> fr;        // one per SERVED frame; index = stack order
    std::vector<uint8_t> pinned;    // 1 = handed out, not released yet
    std::string name;               // psStack::name points at THIS
    // What the call actually did with the mouth. Not bookkeeping for its own
    // sake: "did the plugin release what it took" is the difference between a
    // window transport being possible later (§9.2) and not, and a refusal that
    // was never even asked for is how §6's gate is proved to gate BEFORE the
    // call rather than inside it.
    uint64_t gets = 0, releases = 0, nulls = 0;
    uint32_t held = 0, peakHeld = 0;
    bool called = false;            // did analyze_stack run at all?
};
static const psFrame* stackServeGet(void* ctx, uint32_t index) {
    StackServe* s = (StackServe*)ctx;
    if (!s) return nullptr;
    s->gets++;
    // Out of range is a NULL, not a clamp and not a crash: §5.1 makes NULL the
    // one honest failure, and a clamp would answer a question about frame 9
    // with frame 2's pixels.
    if (index >= s->fr.size()) { s->nulls++; return nullptr; }
    if (!s->pinned[index]) { s->pinned[index] = 1; s->held++; }
    if (s->held > s->peakHeld) s->peakHeld = s->held;
    return &s->fr[index];
}
static void stackServeRelease(void* ctx, uint32_t index) {
    StackServe* s = (StackServe*)ctx;
    if (!s || index >= s->fr.size()) return;   // releasing nothing is not an error
    s->releases++;
    if (s->pinned[index]) { s->pinned[index] = 0; s->held--; }
    // In-process this is where the work would be, and there is none: the frame
    // was already resident before the plugin asked and stays resident after.
    // A shared-memory host recycles the slot here (§9.2), which is the whole
    // reason this mouth exists at all - and re-getting the same index is legal
    // in both, so nothing is invalidated by a release.
}

// Run ONE stack analyzer over ONE open stack. Returns false with `err` set, and
// `err` is always a sentence someone can act on - the two refusals below are
// the host's own and never reach the plugin.
//
// THE ORDER IS THE CONTRACT (docs/abi-v3.md §6, 判断3). min_frames is a
// DECLARATION and the host settles it BEFORE the call; frames/expected are
// FACTS and the plugin reads them after it has been called. Neither replaces
// the other: with the declaration alone, 8 of 24 is indistinguishable from 8
// of 8 and the provenance line becomes a lie; with the facts alone, every
// plugin reimplements the same refusal, differently.
static bool runStackAnalyzer(const StackAnalyzerPluginInfo& sa, int seqId,
                             const psRect* roi, const psAnalyzeSink3* sink,
                             std::string& err, StackServe* serveOut = nullptr) {
    err.clear();
    // Copy out and let go. Everything this call needs from the stack table is
    // two values, and holding the pointer for the length of a plugin call is
    // exactly the defect PR #128 made unconditional across allocators.
    std::string stackName;
    int declaredExpected = 0;
    if (const App::SeqInfo* si = seqInfo(seqId)) {
        stackName = si->name;
        declaredExpected = si->expectedFrames;
    } else {
        err = "that stack is not open";
        return false;
    }
    // The served set, selected exactly as computeStackStats selects its own:
    // a decimated remote preview is not this stack's pixels, and a frame of
    // another shape cannot be served under a psStack that promises one w/h/ch.
    // Excluded BEFORE `frames` is counted, which is what makes the header's
    // "every served frame has this geometry" true rather than hopeful.
    std::vector<int> fr = framesOfSeq(seqId);
    const ImageDoc* first = nullptr;
    std::vector<const ImageDoc*> use;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->src->remoteStep > 1 || d->px().empty()) continue;
        if (!first) first = d;
        if (d->w != first->w || d->h != first->h || d->ch != first->ch) continue;
        use.push_back(d);
    }
    const uint32_t n = (uint32_t)use.size();
    if (!n) {
        err = "no frame of \"" + stackName + "\" is loaded yet";
        return false;
    }
    // N: the declared size, never smaller than what is actually here. §5.1 -
    // "frames < expected = partial load; never the reverse".
    const uint32_t N = (uint32_t)std::max((int)n, declaredExpected);

    // ---- the gate. BEFORE the call, and it is the whole of §6's first half.
    if (n < sa.minFrames) {
        err = sa.name + " needs at least " + std::to_string(sa.minFrames) +
              " frames; \"" + stackName + "\" has " + std::to_string(n) +
              (n < N ? " of " + std::to_string(N) : "") + " loaded";
        return false;     // the plugin is not entered, and cannot be
    }

    StackServe local;
    StackServe& sv = serveOut ? *serveOut : local;
    sv.name = stackName;
    sv.fr.clear();
    sv.fr.reserve(n);
    for (const ImageDoc* d : use) sv.fr.push_back(makeFrame(*d));
    sv.pinned.assign(n, 0);

    psStack st = {};
    st.frames = n;
    st.expected = N;                 // the FACTS, passed in - §6's second half
    st.w = (uint32_t)first->w;
    st.h = (uint32_t)first->h;
    st.ch = (uint32_t)first->ch;
    st.dtype = PS_DTYPE_F32;
    st.name = sv.name.c_str();
    st.meta_json = nullptr;
    st.ctx = &sv;
    st.get_frame = stackServeGet;
    st.release_frame = stackServeRelease;

    char perr[256] = { 0 };
    sv.called = true;
    int32_t rc = sa.v3.analyze_stack(&st, roi, sink, perr, sizeof perr);
    if (rc != 0) {
        err = perr[0] ? perr : "the analyzer failed without saying why";
        return false;
    }
    return true;
}

// naming convention "category/name": drives the 2-level Analysis picker + Measure menu
static void splitAnalyzerName(const std::string& full, std::string& cat, std::string& item) {
    size_t s = full.find('/');
    if (s == std::string::npos) { cat = "misc"; item = full; }
    else { cat = full.substr(0, s); item = full.substr(s + 1); }
}
// The Measure menu groups by the QUESTION being asked, not by the plugin file:
// nobody opens the menu thinking "iso12233", they think "is the resolution
// there?". The mapping is host knowledge (tiny, additive); unknown categories
// from third-party plugins fall through and keep their own name as a heading.
struct MeasureGroup { const char* cat; const char* group; };
static const MeasureGroup MEASURE_GROUPS[] = {
    { "noise",      "Noise / SNR" },
    { "iso12233",   "Resolution / Focus" },
    { "sharpness",  "Resolution / Focus" },
    { "uniformity", "Uniformity / Flat field" },
    { "stats",      "Statistics / Sanity" },
};
static const char* measureGroupOf(const std::string& cat) {
    for (const auto& g : MEASURE_GROUPS)
        if (cat == g.cat) return g.group;
    return nullptr;
}

// Machine-checkable preconditions only. The host must never guess about scene
// content - it cannot tell a flat field from a portrait, so "wants a flat
// field" stays a tooltip and the plugin's own error does the talking. What it
// CAN prove (no image at all, a mosaic the plugin is documented to reject) is
// worth a disabled item: the reason appears where the click would have been.
static const char* analyzerDisabledReason(const AnalyzerPluginInfo& a, const ImageDoc* im) {
    if (!im) return "open an image first";
    if (a.name == "iso12233/e-sfr" && im->ch == 1 && im->cfa != 0)
        return "CFA mosaic input - run Process > demosaic (bilinear) first";
    return nullptr;
}

// Running a measurement and showing its result are one gesture: the Measure
// menu (and the M key) must never leave the numbers in a hidden or buried
// panel. Reveal is half of it - a docked Analysis tab sitting behind
// Histogram would still swallow the result, hence the explicit focus.
static void revealAnalysis() {
    app.showAnalysis = true;
    ImGui::SetWindowFocus("Analysis");
}
static void requestMeasure(int sel) {
    app.anaSel = sel;
    app.anaRunRequest = true;   // consumed by the Analysis panel this frame
    revealAnalysis();
}

// Set while the background queue loads: newly arrived images must never steal
// the selection (or the zoom) from what the user is working on.
static bool g_quietLoad = false;

// measured = true: the loader already ran computeMinMax (or adopted a measured
// resident) BEFORE the source became discoverable in the registry. Do not
// write vmin/vmax again here - the seq loader thread may be reading them
// through an adoption, and shareOrRegisterSource's contract says adopters
// never rewrite shared fields.
static void addImage(std::unique_ptr<ImageDoc> im, bool measured = false) {
    if (im->batchId == 0) {
        if (app.loadBatchId) im->batchId = app.loadBatchId;
        else {
            size_t sl = im->src->path.find_last_of("/\\");
            std::string dir = sl == std::string::npos ? std::string() : im->src->path.substr(0, sl);
            size_t s2 = dir.find_last_of("/\\");
            std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
            im->batchId = batchReuse(leaf.empty() ? "generated" : leaf, dir);
        }
    }
    // --cfa says "this dump is mosaiced" for formats that carry no such flag (an
    // .npy of a sensor read is exactly that). The Inspector can still change it
    // per image; this only sets what a file arrives as.
    if (app.forceCfa >= 0 && im->ch == 1 && im->cfa == 0) {
        im->cfa = app.forceCfa;
        im->cfaPattern = app.forceCfaPattern & 3;
    }
    im->uid = app.nextUid++;
    app.imagesRev++;
    if (!measured) computeMinMax(*im);
    defaultRange(*im);            // own range is always meaningful; link only overlays it
    im->texDirty = true;
    app.images.push_back(std::move(im));
    if (!g_quietLoad || app.current < 0) {      // first image still gets shown
        app.current = (int)app.images.size() - 1;
        // the view is shared: only frame the very first image, keep it afterwards
        if (app.images.size() == 1 || app.fitOnSwitch) app.fitRequested = true;
    }
}

// 住所注記: ROI montage は plugin glue ではない(§2 の ~250 はこの節込みの勘定)。住所のまま移動(§1) → P 段階で分けるなら core/app/roi_montage.inc へ
// ---- ROI montage: the same ROI from every frame, laid side by side ---------
// A contact sheet of one region across a stack - the thing you actually paste
// into a report to show drift, flicker or a defect appearing.
//
// The result is a real ImageDoc, not a picture: it can be measured, ranged and
// exported like anything else. So it carries DATA rules, not poster rules -
//   - no separator lines: a drawn line is a pixel value that was never captured
//   - CFA phase is preserved by snapping the ROI to even coordinates, or the
//     tiles would each start on a different Bayer phase and every per-plane
//     statistic over the montage would be wrong
//   - it says n of N: a partially loaded stack must not look complete
// perFrameAuto: each tile is normalised to ITS OWN finite min..max, mapped to
// 0..1. That makes an exposure sweep readable - under one shared range the dark
// frames are black squares - but it also means the pixels STOP BEING DN: a
// normalised montage is a picture for looking, not a measurement, and it says
// so in its name, its note and its unit-less display range. A constant tile
// (max == min) maps to 0.5 - the middle, not a lie about being dark or bright.
static bool montageROI(bool horizontal, std::string& err, bool perFrameAuto = false) {
    ImageDoc* im = cur();
    if (!im) { err = "no image"; return false; }
    if (im->seqId == 0) { err = "not a stack: montage lays out the frames of one stack"; return false; }
    App::Ann* a = findAnn(app.selectedAnn);
    if (!a || a->type != 0) { err = "select a rectangle ROI first"; return false; }
    int rx = std::clamp(a->x, 0, im->w), ry = std::clamp(a->y, 0, im->h);
    int rw = std::clamp(a->w, 0, im->w - rx), rh = std::clamp(a->h, 0, im->h - ry);
    if (rw < 1 || rh < 1) { err = "the ROI is empty"; return false; }
    bool snapped = false;
    if (im->cfa) {                       // keep every tile on the same Bayer phase
        if (rx & 1) { rx--; rw++; snapped = true; }
        if (ry & 1) { ry--; rh++; snapped = true; }
        if (rw & 1) { rw--; snapped = true; }
        if (rh & 1) { rh--; snapped = true; }
        if (rw < 2 || rh < 2) { err = "the ROI is too small for a CFA montage"; return false; }
    }
    std::vector<int> fr = framesOfSeq(im->seqId);
    if (fr.size() < 2) { err = "the stack has one frame"; return false; }
    const int ch = im->ch, n = (int)fr.size();
    const int ow = horizontal ? rw * n : rw;
    const int oh = horizontal ? rh : rh * n;
    auto out = std::make_unique<ImageDoc>();
    FrameSource& S = *out->src;          // composed NEW pixels: a copy by design (§2.2)
    S.w = ow; S.h = oh; S.ch = ch;
    S.dtype = im->dtype;
    S.data.assign((size_t)ow * oh * ch, 0.0f);
    for (int k = 0; k < n; k++) {
        const ImageDoc& src = *app.images[fr[k]];
        if (src.w != im->w || src.h != im->h || src.ch != ch) continue;   // ragged stack
        const int ox = horizontal ? rw * k : 0;
        const int oy = horizontal ? 0 : rh * k;
        for (int y = 0; y < rh; y++) {
            const float* sp = &src.px()[((size_t)(ry + y) * src.w + rx) * ch];
            float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
            memcpy(dp, sp, (size_t)rw * ch * sizeof(float));
        }
        if (perFrameAuto) {
            // this tile's own finite range - the whole tile, planes included:
            // per-plane normalisation would silently rebalance the CFA mosaic
            float lo = FLT_MAX, hi = -FLT_MAX;
            for (int y = 0; y < rh; y++) {
                const float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
                for (int x = 0; x < rw * ch; x++)
                    if (std::isfinite(dp[x])) { lo = std::min(lo, dp[x]); hi = std::max(hi, dp[x]); }
            }
            const float span = hi - lo;
            for (int y = 0; y < rh; y++) {
                float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
                for (int x = 0; x < rw * ch; x++) {
                    if (!std::isfinite(dp[x])) continue;
                    dp[x] = span > 0 ? (dp[x] - lo) / span : 0.5f;
                }
            }
        }
    }
    // the montage inherits the CFA description only if the phase survived, which
    // the even-snap above guarantees; the pattern itself is unchanged
    out->cfa = im->cfa; out->cfaPattern = im->cfaPattern;
    if (perFrameAuto) {
        S.dtype = "f32";                 // whatever the source was, this is not it
        out->black = 0.0f; out->white = 1.0f;
        S.vmin = 0.0f;  S.vmax = 1.0f;
    } else {
        out->black = im->black; out->white = im->white;
        S.vmin = im->vmin; S.vmax = im->vmax;
    }
    out->syncMirrors();
    out->batchId = im->batchId;
    const App::SeqInfo* si = seqInfo(im->seqId);
    int expected = si && si->expectedFrames > 0 ? si->expectedFrames : n;
    char nm[512];
    snprintf(nm, sizeof nm, "%s  ROI %dx%d x%d%s %s", si ? si->name.c_str() : im->name.c_str(),
             rw, rh, n, n < expected ? (" of " + std::to_string(expected)).c_str() : "",
             perFrameAuto ? (horizontal ? "(montage H, per-frame range)"
                                        : "(montage V, per-frame range)")
                          : (horizontal ? "(montage H)" : "(montage V)"));
    out->name = nm;
    out->note = std::string("ROI (") + std::to_string(rx) + "," + std::to_string(ry) + ") " +
                std::to_string(rw) + "x" + std::to_string(rh) + " from " +
                std::to_string(n) + (n < expected ? " of " + std::to_string(expected) : "") +
                " frame(s)" + (snapped ? ", snapped to the CFA phase" : "") +
                (perFrameAuto ? "; per-frame auto range: each tile mapped to 0..1 - "
                                "values are NOT DN, view only" : "");
    out->texDirty = true;
    addImage(std::move(out));
    if (n < expected)
        toast("montage: " + std::to_string(n) + " of " + std::to_string(expected) +
              " frames are loaded", true);
    return true;
}
