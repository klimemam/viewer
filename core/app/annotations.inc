// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
// ---------------------------------------------------------------- annotations
// ANN_COLORS: annotations の配色。App 定義と app 定義の間に居たのを P6 でここへ
// (state.inc の「P 段階で core/app/annotations.inc へ」の履行)。
static const ImU32 ANN_COLORS[8] = {
    IM_COL32(77, 163, 255, 255), IM_COL32(105, 220, 130, 255), IM_COL32(255, 184, 77, 255),
    IM_COL32(255, 120, 120, 255), IM_COL32(200, 120, 255, 255), IM_COL32(90, 220, 220, 255),
    IM_COL32(255, 150, 200, 255), IM_COL32(180, 200, 90, 255),
};
static App::Ann* findAnn(int id) {
    for (auto& a : app.anns)
        if (a.id == id) return &a;
    return nullptr;
}
// Does an annotation fit inside a frame of W x H pixels? The one predicate
// behind both the ROIs panel's "outside this frame" and every write to a
// rectangle below - roiSideCovers() in the panel IS this function.
//
// A ROI that does not fit is NOT broken, and repairing it is not this
// predicate's job. Ann carries no document identifier and the list is shared by
// every open document on purpose (the shot-series workflow: open the next
// exposure, read the same rectangle), so opening a file from another sensor is
// the everyday way to get a ROI that hangs off the frame - and the panel SAYS
// "outside this frame" rather than moving it. What this gates is the other
// direction: a WRITE that would put a ROI there, or one that restores a
// rectangle remembered against a frame this one is not.
static bool annFits(const App::Ann& a, int W, int H) {
    if (a.x < 0 || a.y < 0) return false;
    if (a.type != 0) return a.x < W && a.y < H;              // POI: one pixel
    return a.w > 0 && a.h > 0 && a.x + a.w <= W && a.y + a.h <= H;
}

// Writing the ROIs panel's number fields onto the annotation: x/y/w/h for a
// ROI, x/y for a POI, in the editor's own order. Returns true when `a` changed;
// on a refusal NOTHING is written and `why` is the sentence to put on screen.
//
// Reported 2026-09-29: 「新しいファイルを開くと、ROI の原点がずれてあっちこっちいく」.
// One of the two roots was this commit, which used to be four clamps against
// the frame on screen:
//
//     sel->x = std::clamp(xywh[0], 0, im->w - 1);
//     sel->w = std::clamp(xywh[2], 1, im->w - sel->x);   // ...and y, h
//
// so editing ONE field rewrote all four whenever the ROI was not on this frame.
// Measured before the fix (--roistats-selftest W2): a (3000,2000) 200x200 ROI
// with the 640x480 file open, h 200 -> 100, commits as 1x1 @639,479 - and it
// PERSISTS, so going back to the 4096x3000 frame does not bring the rectangle
// back. The clamp did not protect the ROI; it destroyed it, quietly, from an
// edit to an unrelated component.
//
// Two rules, and neither of them clamps:
//
//   * only a component that CHANGED is written. The editor seeds its fields
//     from the annotation, so "changed" is "differs from the annotation" -
//     every other component is left exactly as it is, including one that does
//     not fit the frame on screen.
//   * a value is REFUSED and said, never pushed into range. Two things are
//     refused: an edited component that is not a rectangle at all (a negative
//     x, a w below 1), and an edit that takes an annotation this frame HOLDS
//     and puts it outside. An annotation that is already outside stays fully
//     editable - that is a normal state (see annFits), and a viewer that
//     tidied it onto the frame on screen would be the reported bug wearing a
//     helpful face.
static bool annWriteFields(App::Ann& a, const int* f, int W, int H, std::string* why) {
    static const char* NM[4] = { "x", "y", "w", "h" };
    const int n = a.type == 0 ? 4 : 2;
    const int had[4] = { a.x, a.y, a.w, a.h };
    App::Ann want = a;
    int* out[4] = { &want.x, &want.y, &want.w, &want.h };
    char b[224];
    bool any = false;
    for (int i = 0; i < n; i++) {
        if (f[i] == had[i]) continue;             // not edited: not written
        any = true;
        const int lo = i < 2 ? 0 : 1;             // x, y from 0; w, h from 1
        if (f[i] < lo) {
            snprintf(b, sizeof b, "%s = %d: %s is %d or more, so nothing was written",
                     NM[i], f[i], NM[i], lo);
            if (why) *why = b;
            return false;
        }
        *out[i] = f[i];
    }
    if (!any) return false;                       // a commit with no edit in it
    if (annFits(a, W, H) && !annFits(want, W, H)) {
        if (a.type == 0)
            snprintf(b, sizeof b, "%dx%d @%d,%d would be outside this frame (%dx%d) - "
                                  "not written, the ROI is as it was",
                     want.w, want.h, want.x, want.y, W, H);
        else
            snprintf(b, sizeof b, "@%d,%d would be outside this frame (%dx%d) - "
                                  "not written, the pin is as it was",
                     want.x, want.y, W, H);
        if (why) *why = b;
        return false;
    }
    a = want;
    return true;
}

static void addAnn(int type, int x, int y, int w, int h) {
    App::Ann a;
    a.id = app.nextAnnId++;
    a.type = type; a.x = x; a.y = y; a.w = w; a.h = h;
    a.color = (a.id - 1) & 7;
    int seq = type == 0 ? ++app.roiSeq : ++app.poiSeq;
    a.label = (type == 0 ? "ROI " : "P") + std::to_string(seq);
    app.anns.push_back(std::move(a));
    app.selectedAnn = app.anns.back().id;
    app.annRev++;
}
// X / Y: row and column selection. A band is a ROI one pixel thick, which is why
// it cannot be drawn by dragging - so it has to come from a key:
//   ROI selected     -> toggle band <-> the rect it had before
//   POI selected     -> the row/column through that pin (toggle back = the pixel)
//   nothing selected -> the row/column through the pixel under the cursor
static void toggleBand(bool horizontal) {
    ImageDoc* im = cur();
    if (!im) return;
    App::Ann* a = findAnn(app.selectedAnn);
    if (a && a->type == 0) {                     // ROI: the existing toggle
        if (horizontal) {
            bool full = a->x == 0 && a->w == im->w;
            if (full && a->prevW >= 1) { a->x = a->prevX; a->w = a->prevW; a->prevW = -1; }
            else if (!full)            { a->prevX = a->x; a->prevW = a->w; a->x = 0; a->w = im->w; }
        } else {
            bool full = a->y == 0 && a->h == im->h;
            if (full && a->prevH >= 1) { a->y = a->prevY; a->h = a->prevH; a->prevH = -1; }
            else if (!full)            { a->prevY = a->y; a->prevH = a->h; a->y = 0; a->h = im->h; }
        }
        app.annRev++;
        return;
    }
    int px = -1, py = -1;
    if (a && a->type == 1) { px = a->x; py = a->y; }          // the selected pin
    else if (app.hoverX >= 0) { px = app.hoverX; py = app.hoverY; }
    if (px < 0 || px >= im->w || py < 0 || py >= im->h) {
        toast("select a pin, or hover a pixel, then press X or Y");
        return;
    }
    if (horizontal) addAnn(0, 0, py, im->w, 1);               // whole row
    else            addAnn(0, px, 0, 1, im->h);               // whole column
    App::Ann* n = findAnn(app.selectedAnn);
    if (n) {   // pressing X/Y again collapses the band back onto that one pixel
        if (horizontal) { n->prevX = px; n->prevW = 1; }
        else            { n->prevY = py; n->prevH = 1; }
        n->label = (horizontal ? "row " + std::to_string(py) : "col " + std::to_string(px));
    }
}

// A ROI drawn on a 1/3 preview is in PREVIEW coordinates: after the full-res
// swap it would silently cover 1/9 of the intended area and every panel would
// measure the wrong region. Placing annotations waits for the real pixels.
static bool annBlockedOnPreview() {
    if (cur() && cur()->src->remoteStep > 1) {
        toast("preview: wait for full resolution before placing ROIs/pins", true);
        return true;
    }
    return false;
}

static void deleteAnn(int id) {
    for (size_t i = 0; i < app.anns.size(); i++)
        if (app.anns[i].id == id) { app.anns.erase(app.anns.begin() + i); break; }
    if (app.selectedAnn == id) app.selectedAnn = 0;
    app.annRev++;
}
