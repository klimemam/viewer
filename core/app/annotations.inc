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
