// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---------------------------------------------------------------- export
// Getting a picture OUT of the tool and into a report. Two things are wanted
// and they are not the same thing:
//   dot by dot  - one image pixel per output pixel, no resampling. What an
//                 image-quality reader needs: the noise is the subject, and a
//                 scaled screenshot invents and destroys exactly that.
//   as displayed - the canvas as it sits, zoom and overlays included. Not here
//                 yet (see docs/todo-open.md): it needs a framebuffer readback
//                 after the frame is rendered, and this machine cannot verify
//                 the result today.
// Both go through renderDocRGBA, so what leaves the app is what the screen
// showed - the same range, gamma, colormap and CFA colorize.

// PNG via miniz, which is already linked for .npz. No new dependency.
static std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> out;
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6, MZ_FALSE);
    if (!png) return out;
    out.assign((const uint8_t*)png, (const uint8_t*)png + len);
    mz_free(png);
    return out;
}

#ifdef _WIN32
// Two formats, because "paste" means different things to different programs:
// CF_DIB is what Word, PowerPoint and Excel read; the registered "PNG" format
// is what most browsers and chat clients prefer, and it keeps the alpha.
static bool clipboardPutImage(const std::vector<uint8_t>& rgba, int w, int h,
                              const std::vector<uint8_t>& png, std::string& err) {
    if (!OpenClipboard(nullptr)) { err = "the clipboard is held by another program"; return false; }
    EmptyClipboard();
    bool any = false;
    {   // CF_DIB: BITMAPINFOHEADER + BGRA, bottom-up
        const size_t px = (size_t)w * h * 4;
        HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + px);
        if (hm) {
            uint8_t* dst = (uint8_t*)GlobalLock(hm);
            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h;   // + = bottom-up
            bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
            bi.biSizeImage = (DWORD)px;
            memcpy(dst, &bi, sizeof bi);
            uint8_t* q = dst + sizeof bi;
            for (int y = 0; y < h; y++) {
                const uint8_t* src = &rgba[(size_t)(h - 1 - y) * w * 4];
                for (int x = 0; x < w; x++) {
                    q[x * 4 + 0] = src[x * 4 + 2];   // B
                    q[x * 4 + 1] = src[x * 4 + 1];   // G
                    q[x * 4 + 2] = src[x * 4 + 0];   // R
                    q[x * 4 + 3] = src[x * 4 + 3];
                }
                q += (size_t)w * 4;
            }
            GlobalUnlock(hm);
            if (SetClipboardData(CF_DIB, hm)) any = true; else GlobalFree(hm);
        }
    }
    if (!png.empty()) {
        UINT cfPng = RegisterClipboardFormatA("PNG");
        HGLOBAL hp = cfPng ? GlobalAlloc(GMEM_MOVEABLE, png.size()) : nullptr;
        if (hp) {
            memcpy(GlobalLock(hp), png.data(), png.size());
            GlobalUnlock(hp);
            if (SetClipboardData(cfPng, hp)) any = true; else GlobalFree(hp);
        }
    }
    CloseClipboard();
    if (!any) err = "the clipboard refused the image";
    return any;
}
#endif

// The ONE place an export is produced, so the file and the clipboard can never
// disagree about what the picture is.
static bool exportDocRGBA(ImageDoc* im, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!im || im->w < 1 || im->h < 1) return false;
    w = im->w; h = im->h;
    renderDocRGBA(*im, rgba);
    return true;
}

static void copyViewDotByDot() {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast("nothing to copy", true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
#ifdef _WIN32
    std::string err;
    if (clipboardPutImage(rgba, w, h, png, err))
        toast("copied " + std::to_string(w) + "x" + std::to_string(h) + " (dot by dot)");
    else
        toast("copy failed: " + err, true);
#else
    // No image clipboard without a toolkit here: write the PNG next to nothing
    // and hand over the PATH, which every file dialog and chat client accepts.
    std::string path = (std::filesystem::temp_directory_path() /
                        ("viewer-" + std::to_string(im->uid) + ".png")).u8string();
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    f.close();
    ImGui::SetClipboardText(path.c_str());
    toast("wrote " + path + " (path on the clipboard)");
#endif
}

static void saveViewPng(const std::string& path) {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast("nothing to save", true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    if (png.empty()) { toast("PNG encode failed", true); return; }
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write " + path, true); return; }
    f.write((const char*)png.data(), (std::streamsize)png.size());
    toast("saved " + baseName(path) + "  " + std::to_string(w) + "x" + std::to_string(h));
}

// ここから forgetImage までは表示レンジ/テクスチャ/キャッシュ掃除の補助で export ではない(§2 の ~240 はこの並び込みの勘定)。住所のまま移動(§1) → P 段階で分けるなら compare 側へ
static void setFilter(ImageDoc& im, bool nearest) {
    if (im.texNearest == nearest || !im.tex) { im.texNearest = nearest; return; }
    im.texNearest = nearest;
    glBindTexture(GL_TEXTURE_2D, im.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}

static void markAllTexDirty() {
    for (auto& d : app.images) d->texDirty = true;
}
// The Range panel's one mode control (0 auto per frame / 1 per stack /
// 2 linked). It lives here, not inside the combo, because choosing a mode has
// to act on the frame ALREADY on screen: the per-frame policy is otherwise
// applied only by selectImage, on a frame change, and the mode read as dead
// until the first step (report 2026-08-03).
static void applyRangeMode(int mode, ImageDoc* on) {
    bool wasLinked = app.linkRange;
    app.linkRange = mode == 2;
    app.rangeScope = mode == 0 ? 0 : 1;
    if (on) {
        if (app.linkRange && !wasLinked) {     // seed from what is on screen
            app.linkBlack = on->black; app.linkWhite = on->white;
        }
        if (mode == 0 && !app.linkRange) { defaultRange(*on); on->texDirty = true; }
    }
    if (app.linkRange != wasLinked) markAllTexDirty();
}
// Linking is an overlay, never a rewrite: each image keeps its own range, so
// unlinking restores exactly what every image had before.
static ImageDoc* cmpB();          // fwd: the B side, or null when compare is off
// The compare display range - and "compare" means EVERY pane, not just the
// A/B pair: two images stretched differently cannot be compared, and with
// tiles that holds for slot C exactly as it holds for B (report 2026-07-30,
// 「Auto FitするのがA,BまででCが変わらない」). Mode 2 fits every compared
// side to the union of what the current frames actually contain, recomputed
// as you step. Mode 1 means "the reference side dictates": A's black-white
// for every other letter. The slots join only while compare is ON - the
// letters survive compare-off as seats, but a seat is not a comparison.
// A, B and the slots may now name the SAME document (resolveB no longer goes
// quiet there), which this handles by construction: a union of min/max does
// not care how many times a side appears, and the `rs.doc != b` below is kept
// only so a duplicate cannot eat one of the fixed 16 places. It changes no
// number - unlike the panels, where dropping a duplicate side dropped a curve.
static bool abRange(const ImageDoc& im, float& lo, float& hi) {
    if (app.compareRangeMode == 0) return false;
    ImageDoc* a = cur();
    if (!a) return false;
    ImageDoc* b = cmpB();
    // who is being compared: A, B if it resolves, every resolved slot. A tiled
    // canvas can hold A + C + D with no B at all, so B is optional - what is
    // required is that there be someone besides A to compare WITH.
    ImageDoc* parts[2 + 14];    // A + B + every slot letter (SLOT_LETTERS)
    int np = 0;
    parts[np++] = a;
    if (b) parts[np++] = b;
    if (app.compareMode != App::CmpOff && !app.cmpExtra.empty())
        for (const ResolvedSlot& rs : resolveSlots())
            if (rs.doc != b && np < (int)(sizeof parts / sizeof *parts))
                parts[np++] = rs.doc;
    if (np < 2) return false;
    bool member = false;
    for (int i = 0; i < np; i++) if (parts[i] == &im) member = true;
    if (!member) return false;
    if (app.compareRangeMode == 1) { lo = a->black; hi = a->white; return true; }
    lo = parts[0]->vmin;                      // union of the CONTENT, not of the
    hi = parts[0]->vmax;                      // stretches
    for (int i = 1; i < np; i++) {
        lo = std::min(lo, parts[i]->vmin);
        hi = std::max(hi, parts[i]->vmax);
    }
    return hi > lo;
}
static float effBlack(const ImageDoc& im) {
    if (app.linkRange) return app.linkBlack;
    float lo, hi;
    if (abRange(im, lo, hi)) return lo;
    return im.black;
}
static float effWhite(const ImageDoc& im) {
    if (app.linkRange) return app.linkWhite;
    float lo, hi;
    if (abRange(im, lo, hi)) return hi;
    return im.white;
}

static void setRange(ImageDoc& im, float black, float white) {
    if (!(white > black)) return;
    if (app.linkRange) {
        app.linkBlack = black; app.linkWhite = white;
        markAllTexDirty();            // shared value changed: every image re-renders
    } else {
        im.black = black; im.white = white; im.texDirty = true;
    }
}
static void forgetTexture(ImageDoc* im) {
    app.texLru.erase(std::remove(app.texLru.begin(), app.texLru.end(), im), app.texLru.end());
}
// One place that drops an image from every cache that can name it.
static void forgetImage(ImageDoc* im) {
    if (app.ana.img == im) app.ana.img = nullptr;
    // BOTH slots: the image being dropped may be the B side, and a cache still
    // naming it is a dangling pointer the next draw would follow
    for (int k = 0; k < 2; k++) {
        if (app.hist[k].img == im) { app.hist[k].img = nullptr; app.hist[k].uid = 0; }
        if (app.proj[k].img == im) { app.proj[k].img = nullptr; app.proj[k].uid = 0; }
        app.temporal[k].seqId = -1;
    }
    // ...and EVERY lettered slot, for the same reason. Closing C shifts D down
    // into C's cache entry, and the entry still held C's ImageDoc*: the key
    // check (uid/dataRev) fails and it is recomputed before it is read, so the
    // stale pointer was never followed - but "never followed" is a property of
    // the read path, not of the cache, and the next reader is one edit away
    // from following it. The cache says what it holds or it says nothing.
    // (Reachable through a slot stack's preview->full swap even before reload
    // existed - docs/reference-design.md §3.2 - hence temporalExtra too.)
    for (auto& H : app.histExtra) if (H.img == im) { H.img = nullptr; H.uid = 0; }
    for (auto& P : app.projExtra) if (P.img == im) { P.img = nullptr; P.uid = 0; }
    for (auto& T : app.temporalExtra) T.seqId = -1;
    forgetTexture(im);
    app.imagesRev++;
}
