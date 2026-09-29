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
        // The second root of the 2026-09-29 report. prevX/prevW (and
        // prevY/prevH; -1 = nothing saved) hold the rectangle the band was made
        // from, in the pixels of whatever frame was open at the time - and Ann
        // has no room to record WHICH frame that was. The old code decided
        // between "put the rect back" and "save the rect and widen" by asking
        // whether the ROI is the full width of the frame on screen:
        //
        //     bool full = a->x == 0 && a->w == im->w;
        //
        // A band made on 4096x3000 is not "full" on 2048x1500, so the toggle
        // took the SAVE arm there and overwrote the memory with 0 / 4096.
        // Measured before the fix (--roistats-selftest W7): the 200-wide
        // rectangle the band came from was gone for good, and the next press
        // restored a 4096-wide ROI onto a 2048-wide frame.
        //
        // X / Y means "the row / column of the frame I am looking at", so the
        // frame has to be able to hold what is being toggled. Which arm we are
        // on is the PRESENCE of a memory - never a width compared against this
        // frame - and both arms are gated on annFits, so nothing is clamped and
        // nothing is overwritten:
        //   * a ROI this frame does not hold: refused, the memory kept. It is
        //     still the way back, once a frame that holds it is open.
        //   * a memory this frame does not hold: refused for the same reason,
        //     and kept for the same reason.
        // A ROI off the frame is NOT repaired by this key: it is left alone and
        // the refusal says which frame it is being measured against.
        //
        // Still open, and deliberately not touched here: the two "Widen ROI to
        // full width / height" items in core/ui/canvas.inc write prevX/prevW
        // themselves, so widening from the context menu still overwrites the
        // memory the way this function no longer does. That file belongs to
        // another change in flight; the seam for it is right here, and the
        // items should come through this arm rather than keep their own copy.
        char b[224];
        // one wording for "the rect I am holding does not belong to this
        // frame", said the same way on both axes
        auto refuseBack = [&](const App::Ann& back) {
            char m[224];
            snprintf(m, sizeof m, "the rect this band was made from (%dx%d @%d,%d) is "
                                  "outside this frame (%dx%d) - kept as it is, not "
                                  "resized", back.w, back.h, back.x, back.y,
                     im->w, im->h);
            toast(m, true);
        };
        if (!annFits(*a, im->w, im->h)) {
            snprintf(b, sizeof b, "%s (%dx%d @%d,%d) is outside this frame (%dx%d) - "
                                  "X / Y bands a ROI against the frame it is on; "
                                  "the rect it remembers is kept",
                     a->label.c_str(), a->w, a->h, a->x, a->y, im->w, im->h);
            toast(b, true);
            return;
        }
        if (horizontal) {
            if (a->prevW >= 1) {                 // a memory: put that rect back
                App::Ann back = *a;
                back.x = a->prevX; back.w = a->prevW;
                if (!annFits(back, im->w, im->h)) { refuseBack(back); return; }
                a->x = back.x; a->w = back.w; a->prevW = -1;
            } else {                             // no memory: save it and widen
                a->prevX = a->x; a->prevW = a->w;
                a->x = 0; a->w = im->w;
            }
        } else {
            if (a->prevH >= 1) {
                App::Ann back = *a;
                back.y = a->prevY; back.h = a->prevH;
                if (!annFits(back, im->w, im->h)) { refuseBack(back); return; }
                a->y = back.y; a->h = back.h; a->prevH = -1;
            } else {
                a->prevY = a->y; a->prevH = a->h;
                a->y = 0; a->h = im->h;
            }
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
