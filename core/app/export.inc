// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---------------------------------------------------------------- export
// Getting a picture OUT of the tool and into a report. Two things are wanted
// and they are not the same thing:
//   dot by dot  - one image pixel per output pixel, no resampling. What an
//                 image-quality reader needs: the noise is the subject, and a
//                 scaled screenshot invents and destroys exactly that.
//   as displayed - the canvas as it sits, zoom and overlays included. What a
//                 reader needs when the SUBJECT is the view: a ROI frame around
//                 the defect, a wipe at the boundary, the grid at 16x. Board 96,
//                 Fable 2026-09-27: a framebuffer readback, and the zoom is NOT
//                 normalised - the item above already covers the reproducible
//                 1:1 case, so normalising here would leave nobody served.
// The first two go through renderDocRGBA, so what leaves the app is what the
// screen showed - the same range, gamma, colormap and CFA colorize. The third
// cannot: renderDocRGBA maps ONE document and knows nothing about zoom, panes or
// overlays, and the only thing that holds the whole composition is the
// framebuffer. See copyViewAsDisplayed below for what that costs.

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

// "" when this document may be exported; otherwise the sentence saying why not.
// Same shape as peerRefusalFor / reloadUnavailable: one predicate, so the menu,
// the clipboard and the file cannot come to disagree about what is allowed.
//
// A DECIMATED PREVIEW is the case this exists for. `remoteStep > 1` means
// "still the preview" (state.h:130), and every analysis path in the tree
// already refuses one BY NAME - preprocess.inc, setanalysis.inc,
// panel_projection.inc and panel_temporal.inc all carry the same test under the
// comment `previews lie`. Export was the one surface outside that agreement,
// and it is also the one that makes an explicit CLAIM about the mapping: the
// menu item says "dot by dot", i.e. one output pixel per source pixel, which at
// step 4 is false by a factor of sixteen. The PNG is the worse half - a file
// outlives whatever sentence might have qualified it, and nothing in the file
// says it was a preview.
//
// The refusal is actionable rather than final: the full frame arrives on its
// own (pumpRemoteFetch swaps preview -> full), so the answer is "not yet".
static std::string exportRefusalFor(const ImageDoc* im) {
    if (!im || im->w < 1 || im->h < 1) return "nothing to export";
    if (im->src && im->src->remoteStep > 1)
        return "this is the decimated preview (every " +
               std::to_string(im->src->remoteStep) + "th pixel), not the frame - "
               "wait for the full frame to land, then export";
    return {};
}

// An export made while the value highlight is armed carries the magenta - it
// has to, because renderDocRGBA is the one mapping and the whole point of
// putting the highlight there is that the file and the screen agree. What the
// file does NOT carry is the badge that explains it: the badge is ImGui chrome
// over the canvas, not pixels. That is the same acceptance the display range,
// the gamma and a display LUT already have (all baked in, none explained by the
// image), so the sentence goes where the export is ANNOUNCED - the toast, the
// Messages log it lands in, and stderr. "" when nothing is armed.
static std::string exportHighlightNote(const ImageDoc* im) {
    if (!app.highlight.on) return {};
    return "exported with value " +
           histHlRangeSaid(im ? im->dtype : std::string());
}

// The display range an export was drawn at is baked in and, like the
// highlight, explained nowhere in the file - so it is named where the export
// is announced (#304). Defined with displayRangeFor below.
static std::string exportRangeNote(const ImageDoc& im, const ViewSides& v, bool video);
static std::string viewRefusalFor(const ViewSides& v, const ImageDoc* self);
static ImageDoc* sideOfView(const ImageDoc& im, const ViewSides& v);

// The ONE place an export is produced, so the file and the clipboard can never
// disagree about what the picture is. A single picture is drawn in the
// SCREEN's view: the image on screen is drawn as it is drawn there, a B or a
// slot keeps its role (and so A's range, or the union), and an image nobody
// compares is at its own range - which is also how it is drawn once it is on
// screen. (A stack video draws each frame in ITS view: export_video.inc.)
// `v` names another view - the one a save dialog was opened in, saveViewPng.
static bool exportDocRGBA(ImageDoc* im, std::vector<uint8_t>& rgba, int& w, int& h,
                          const ViewSides* v = nullptr) {
    if (!exportRefusalFor(im).empty()) return false;
    w = im->w; h = im->h;
    const ViewSides screen = v ? ViewSides() : comparedSides();
    renderDocRGBA(*im, rgba, v ? v : &screen);
    return true;
}

static void copyViewDotByDot() {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast(exportRefusalFor(im), true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    const std::string hlNote = exportHighlightNote(im);
    if (!hlNote.empty()) fprintf(stderr, "export: %s\n", hlNote.c_str());
    const std::string rNote = exportRangeNote(*im, comparedSides(), false);
    fprintf(stderr, "export: %s\n", rNote.c_str());
#ifdef _WIN32
    std::string err;
    if (clipboardPutImage(rgba, w, h, png, err))
        toast("copied " + std::to_string(w) + "x" + std::to_string(h) + " (dot by dot)  -  " +
              rNote + (hlNote.empty() ? "" : "  -  " + hlNote));
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
    toast("wrote " + path + " (path on the clipboard)  -  " + rNote +
          (hlNote.empty() ? "" : "  -  " + hlNote));
#endif
}

// ------------------------------------------------------- copy view AS DISPLAYED
// Board 96 (「項目18 表示どおりコピー」), ruled by Fable 2026-09-27. The pixels
// are the CANVAS RECTANGLE at screen resolution, at whatever zoom and
// interpolation are set, with every overlay that was drawn on it - ROI frames,
// pins, the wipe divider, the value highlight, the pixel grid. The zoom is baked
// in and that is the point: "Copy (dot by dot)" above already answers "give me
// the pixels", and a second item that also normalised would answer the same
// question twice while nobody could copy what they were looking at.
//
// It therefore CANNOT be produced where it is asked for. renderDocRGBA maps one
// document; the composition of two panes, a divider and four overlays exists
// nowhere but the framebuffer, and it exists there for exactly one moment - after
// ImGui_ImplOpenGL3_RenderDrawData has drawn the frame and before
// glfwSwapBuffers hands it to the driver. So the menu item cannot answer: it
// files a request, and the frame loop honours it at that moment (core/main.cpp,
// beside the swap).
//
// A COUNTDOWN, not a bool, and this is the subtle part. The frame the menu item
// is clicked on still has the open context menu in it: ImGui's MenuItem only
// marks the popup closed, and ClosePopupToLevel does not withdraw a window that
// has already been submitted (the same fact the comment above the annotation
// items in canvas.inc turns on - "the rest of this body still runs"). Capturing
// at the end of that frame would hand over a picture of the menu lying across
// the canvas. So the request skips one frame: 2 = "not this frame", 1 = "this
// one".
static int g_copyViewRequest = 0;

// The canvas rectangle drawn on the last frame, in ImGui coordinates, and the
// frame number it was drawn on. drawCanvas writes both (core/ui/canvas.inc,
// beside g_tileCanvasW): by readback time every other panel has drawn over the
// layout, so this is the only record of WHERE the canvas was, and the frame
// number is what distinguishes it from a stale rectangle left by a frame that
// drew no canvas at all.
static float g_canvasRect[4] = {};          // x, y, w, h  (ImGui coordinates)
static int g_canvasRectFrame = -1;
// Which panes that canvas held, as drawn: "A|B", "A|B|C|D", "A-B difference",
// "B held", or empty when compare was off and there was one picture. The copy
// takes the WHOLE canvas - both panes of a split ARE what is displayed - so the
// announcement has to say how many pictures are in it.
static std::string g_canvasPanes;

// What the last capture was. Same reasoning as g_paneBadgeProbe: a readback
// cannot be screenshotted on this machine either, so the frame writes down what
// it took and --tile-selftest reads it back. The PIXELS are kept only when
// asked - a canvas-sized RGBA buffer is megabytes and nothing but a test wants
// one after the clipboard has it.
static int g_copyViewW = 0, g_copyViewH = 0;
static std::string g_copyViewSaid;
static std::vector<uint8_t> g_copyViewPixels;
static bool g_copyViewProbeOnly = false;    // keep the pixels, leave the clipboard alone

// The readback. Every way it can fail is a sentence rather than a blank paste:
// "nothing happened" and "you pasted an empty rectangle" look the same at the
// other end, and only one of them is recoverable by the person holding it.
static bool captureCanvasAsDisplayed(std::vector<uint8_t>& rgba, int& outW, int& outH,
                                     std::string& why) {
    if (!ImGui::GetCurrentContext() || g_canvasRectFrame != ImGui::GetFrameCount()) {
        why = "the canvas was not drawn on this frame";
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // ImGui coordinates are LOGICAL; glReadPixels counts FRAMEBUFFER pixels. On a
    // 200% Windows display or a Retina panel the two differ by
    // DisplayFramebufferScale, and a readback that ignored it would copy the
    // top-left quarter of the canvas and call it the canvas. (This is the same
    // trap uiscale.inc is about, one layer down: there the app multiplies twice,
    // here it would fail to multiply at all.)
    const float sx = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
    const float sy = io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
    const int fbW = (int)lroundf(io.DisplaySize.x * sx);
    const int fbH = (int)lroundf(io.DisplaySize.y * sy);
    const ImVec2 vp = ImGui::GetMainViewport()->Pos;   // (0,0) without multi-viewport
    int px = (int)lroundf((g_canvasRect[0] - vp.x) * sx);
    int py = (int)lroundf((g_canvasRect[1] - vp.y) * sy);
    int cw = (int)lroundf(g_canvasRect[2] * sx);
    int ch = (int)lroundf(g_canvasRect[3] * sy);
    // Clamp into the framebuffer. A canvas can hang off a window being dragged
    // smaller, and glReadPixels does not error on that - it leaves the pixels
    // outside the buffer UNTOUCHED, i.e. it would hand over uninitialised heap.
    if (px < 0) { cw += px; px = 0; }
    if (py < 0) { ch += py; py = 0; }
    cw = std::min(cw, fbW - px);
    ch = std::min(ch, fbH - py);
    if (fbW < 1 || fbH < 1 || cw < 1 || ch < 1) {
        why = "the canvas is not on screen";
        return false;
    }
    rgba.assign((size_t)cw * ch * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // Y flip on the way in: the framebuffer's origin is its BOTTOM-left corner,
    // so the canvas's top edge is (fbH - bottom) rows up from it.
    glReadPixels(px, fbH - (py + ch), cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    // ...and again on the way out: the rows arrive bottom-up, and every consumer
    // from here on (encodePng, CF_DIB's own flip) is written for top-down RGBA.
    std::vector<uint8_t> row((size_t)cw * 4);
    for (int y = 0; y < ch / 2; y++) {
        uint8_t* a = &rgba[(size_t)y * cw * 4];
        uint8_t* b = &rgba[(size_t)(ch - 1 - y) * cw * 4];
        memcpy(row.data(), a, row.size());
        memcpy(a, b, row.size());
        memcpy(b, row.data(), row.size());
    }
    // The window is cleared opaque and everything is drawn over that, so the
    // alpha a readback carries is whatever the blends happened to leave - which
    // in a paste target that honours alpha shows up as holes in a frame that had
    // none. The screen was opaque; so is the copy.
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    outW = cw; outH = ch;
    return true;
}

// The facts that make a picture reproducible do NOT go into the pixels - they go
// into the SENTENCE, which is exactly the acceptance exportHighlightNote already
// made for the value highlight: a badge is chrome, not pixels, and a file
// outlives any badge anyway. So zoom, interpolation, panes and overlays are
// announced (toast + Messages log + stderr) and nothing is drawn into the frame.
//
// Every clause is what the frame ACTUALLY DREW, never what a preference permits.
// `grid` appears only when the grid is on AND the zoom is past the 8x threshold
// drawCanvas draws it at; an armed-but-invisible overlay in this sentence would
// be a lie of exactly the kind the sentence exists to prevent.
static std::string copyViewSaid(int w, int h) {
    const ImageDoc* im = cur();
    char head[128];
    snprintf(head, sizeof head, "copied view %dx%d · zoom %.2fx", w, h, app.view.zoom);
    std::string s = head;
    // The filter drawCanvas sets on every document it draws (setFilter, on the
    // zoom), read back off the document it drew rather than re-derived here.
    if (im) s += im->texNearest ? " · nearest" : " · linear";
    if (!g_canvasPanes.empty()) s += " · " + g_canvasPanes;
    // A DECIMATED PREVIEW is not refused here, unlike the dot-by-dot copy and
    // the PNG: those make a claim about the mapping ("one output pixel per source
    // pixel") that a step-4 preview breaks by a factor of sixteen. This item
    // claims only "what is on the screen", and the screen IS showing the preview.
    // So the preview is copied and SAID, which is the whole shape of this feature.
    if (im && im->src && im->src->remoteStep > 1)
        s += " · preview (every " + std::to_string(im->src->remoteStep) + "th pixel)";
    std::vector<std::string> ov;
    bool roi = false, pin = false;
    for (const auto& a : app.anns) {
        if (!a.visible) continue;
        if (a.type == 0) roi = true; else pin = true;
    }
    if (app.compareMode == App::CmpWipe && cmpB()) ov.push_back("wipe line");
    if (roi) ov.push_back("ROI");
    if (pin) ov.push_back("pins");
    if (app.highlight.on) ov.push_back("value highlight");
    if (app.showGrid && app.view.zoom >= 8.0f) ov.push_back("grid");
    s += " · overlays: ";
    if (ov.empty()) s += "none";
    for (size_t i = 0; i < ov.size(); i++) s += (i ? ", " : "") + ov[i];
    // ...and WHICH values are magenta, which only histHlRangeSaid can say.
    const std::string hl = exportHighlightNote(im);
    if (!hl.empty()) s += " · " + hl;
    return s;
}

static void copyViewAsDisplayed() {
    std::vector<uint8_t> rgba; int w = 0, h = 0; std::string why;
    g_copyViewW = g_copyViewH = 0;
    g_copyViewSaid.clear();
    g_copyViewPixels.clear();
    if (!captureCanvasAsDisplayed(rgba, w, h, why)) {
        toast("copy view failed: " + why, true);
        return;
    }
    g_copyViewW = w; g_copyViewH = h;
    g_copyViewSaid = copyViewSaid(w, h);
    fprintf(stderr, "export: %s\n", g_copyViewSaid.c_str());
    if (g_copyViewProbeOnly) {     // the selftest: assert the capture, own no clipboard
        g_copyViewPixels = rgba;
        logMsg(g_copyViewSaid);
        return;
    }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
#ifdef _WIN32
    std::string err;
    if (clipboardPutImage(rgba, w, h, png, err)) toast(g_copyViewSaid);
    else toast("copy failed: " + err, true);
#else
    // Same fallback as the dot-by-dot copy: no image clipboard without a toolkit,
    // so the PNG goes to a temp file and the PATH goes on the clipboard. One
    // fixed name rather than the document's uid - the subject here is the VIEW,
    // and a view is not a document.
    std::string path = (std::filesystem::temp_directory_path() / "viewer-view.png").u8string();
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    f.close();
    ImGui::SetClipboardText(path.c_str());
    toast("wrote " + path + " (path on the clipboard) · " + g_copyViewSaid);
#endif
}

// Called from the frame loop at the one moment the frame is readable. Here
// rather than inline in main.cpp so that the selftest, which drives its own
// frames, honours the request through the SAME code the loop does - the wiring
// is then tested, not merely written twice.
static void pumpCopyViewRequest() {
    if (g_copyViewRequest <= 0) return;
    if (--g_copyViewRequest > 0) return;   // the click frame still holds the menu
    copyViewAsDisplayed();
}

// `uid` is the document the SAVE DIALOG was opened for, not whatever is current
// when it closes. 0 means "the current one" (the CLI and the selftests, which
// have no dialog between the choice and the write). `viewAUid` is the A the
// dialog was opened under, by the same rule one level up (#304): the range a
// picture is drawn at belongs to the VIEW, and a step taken while the OS
// dialog is up moves the view - B follows A to another frame, the union moves
// with it - so the pixels come from viewSidesFor(that A). 0, or an A that has
// closed since, means the view on screen now.
static void saveViewPng(const std::string& path, uint64_t uid = 0, uint64_t viewAUid = 0) {
    ImageDoc* im = nullptr;
    if (uid) {
        for (const auto& d : app.images) if (d->uid == uid) { im = d.get(); break; }
        if (!im) {
            toast("the image this was going to save has been closed", true);
            return;
        }
    } else im = cur();
    ImageDoc* viewA = nullptr;
    if (viewAUid) for (const auto& d : app.images) if (d->uid == viewAUid) { viewA = d.get(); break; }
    const ViewSides view = viewA ? viewSidesFor(viewA) : comparedSides();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h, &view)) { toast(exportRefusalFor(im), true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    if (png.empty()) { toast("PNG encode failed", true); return; }
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write " + path, true); return; }
    f.write((const char*)png.data(), (std::streamsize)png.size());
    const std::string hlNote = exportHighlightNote(im);
    if (!hlNote.empty()) fprintf(stderr, "export: %s\n", hlNote.c_str());
    const std::string rNote = exportRangeNote(*im, view, false);
    fprintf(stderr, "export: %s\n", rNote.c_str());
    toast("saved " + baseName(path) + "  " + std::to_string(w) + "x" + std::to_string(h) +
          "  -  " + rNote + (hlNote.empty() ? "" : "  -  " + hlNote));
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
static App::SeqInfo* seqInfo(int id);   // fwd (defined with the stacks, in
                                        // sequence.inc): never held, only asked
// "Per stack" is a promise that has to be KEPT, not merely made once (#248).
// The frames of one stack sharing a reference range was implemented only where
// frames LAND (sequence.inc copies the reference frame's range onto each
// arriving frame); nothing afterwards held them together, so every later write
// - and "Auto per frame" stepping, which rewrote each frame's stored
// black/white as you went until #321 - broke the sharing silently. This is the one move
// that restores it: whatever `from` shows now, the rest of its stack shows.
// A standalone image is not a stack and shares with nobody: state.h spells the
// convention "0 = standalone, >0 = frame of that sequence", so ONLY a positive
// seqId names a stack. Written as `<= 0` rather than `== 0` because this
// decides who gets written to - a doc that somehow carried a negative id would
// otherwise be fused with every other one, and that is not a mistake worth
// leaving room for.
// A frame that keeps its own range (ImageDoc::rangeOwn, #321) is the stack's
// exception: it neither gives its numbers to the stack nor takes the stack's.
static void shareStackRange(const ImageDoc& from) {
    if (from.seqId <= 0 || from.rangeOwn) return;
    // ...and the stack REMEMBERS whose fit it is wearing (#254). The Range row
    // answers by comparison, so on every frame but this one the shared numbers
    // no longer match that frame's own min/max and the lit button went dark on
    // the first step - the range was still shared, the display was not.
    if (App::SeqInfo* si = seqInfo(from.seqId)) si->rangeRefUid = from.uid;
    for (auto& d : app.images)
        if (d.get() != &from && d->seqId == from.seqId && !d->rangeOwn) {
            d->black = from.black; d->white = from.white;
            d->texDirty = true;
        }
}
// The frame of `seqId` that holds the stack's range: any frame that does not
// keep its own, other than `but`. Null when there is none (a single image, or
// every other frame keeps its own).
static const ImageDoc* stackRangeHolder(int seqId, const ImageDoc* but) {
    if (seqId <= 0) return nullptr;
    for (const auto& d : app.images)
        if (d.get() != but && d->seqId == seqId && !d->rangeOwn) return d.get();
    return nullptr;
}
// Linking is an overlay, never a rewrite: each image keeps its own range, so
// unlinking restores exactly what every image had before.
// The compare display range - and "compare" means EVERY pane, not just the
// A/B pair: two images stretched differently cannot be compared, and with
// tiles that holds for slot C exactly as it holds for B (report 2026-07-30,
// 「Auto FitするのがA,BまででCが変わらない」). Mode 2 fits every compared
// side to the union of what the current frames actually contain, recomputed
// as you step. Mode 1 means "the reference side dictates": A's black-white
// for every other letter. Mode 3 shows each compared side at its OWN fit
// (#301) - the sides still have to be compared sides, so it shares the one
// definition of who they are (viewSidesFor, compare.inc) with the other two.
//
// WHOSE comparison is a parameter (#304): the range is a property of a VIEW,
// not of a document. comparedSides() is the screen's view and is nothing but
// viewSidesFor(cur()); an export hands displayRangeFor the view its pixels
// would be on screen in, so frame k of a stack video comes out at the range
// the screen gives frame k once A is stepped onto it - not at the range of a
// document that merely is not the one on screen right now.
static ViewSides comparedSides() { return viewSidesFor(cur()); }
// The compared side that IS `im`, as the writable document the comparison
// holds; null when nothing is compared or `im` is not one of the sides.
static ImageDoc* sideOfView(const ImageDoc& im, const ViewSides& v) {
    if (v.n < 2) return nullptr;
    for (int i = 0; i < v.n; i++) if (v.side[i] == &im) return v.side[i];
    return nullptr;
}
static ImageDoc* comparedSideOf(const ImageDoc& im) {
    return sideOfView(im, comparedSides());
}
// Does the range in force read the VIEW at all? Link range and "each keeps its
// own" answer from the document alone, so the screen's hot path (effBlack /
// effWhite / displayMapOf, several calls per document per UI frame) skips
// building one - it hands displayRangeFor an empty view, which then takes the
// same Link / own branch it would have taken anyway.
static bool rangeReadsView() { return !app.linkRange && app.compareRangeMode != 0; }
// The range `im` wears when nothing compared overrides it (#321): a frame that
// keeps its own range wears its numbers; under "each frame fits itself" every
// other frame is DRAWN at its own fit (I3 - display only, the stored numbers
// are not written, so leaving the rule gives nothing back to redistribute);
// otherwise the stack's one range, which is the stored black/white.
// The fit is ownFitCached's, the one cache "each side fits itself" draws from:
// the fit of a frame is one answer however it came to be asked. The cast is
// that cache's - it remembers an answer, it changes no pixel and no range.
static void ownRangeOf(const ImageDoc& im, float& lo, float& hi) {
    lo = im.black; hi = im.white;
    if (!im.rangeOwn && app.rangeRule == RR_EachFrame) ownFitCached(const_cast<ImageDoc&>(im), lo, hi);
}
// The black-white `im` is drawn at in view `v`: Link range, then the compare
// range mode 0..3, then the document's own range (ownRangeOf) - one decision,
// which the screen (effBlack/effWhite, displayMapOf) and every export reach.
static void displayRangeFor(const ImageDoc& im, const ViewSides& v, float& lo, float& hi) {
    if (app.linkRange) { lo = app.linkBlack; hi = app.linkWhite; return; }
    ownRangeOf(im, lo, hi);
    if (app.compareRangeMode == 0) return;
    ImageDoc* self = sideOfView(im, v);
    if (!self) return;
    // A's range is the range A is drawn at alone - its fit, under "each frame
    // fits itself", exactly as main drew B at A's re-fitted numbers then.
    if (app.compareRangeMode == 1) { ownRangeOf(*v.side[0], lo, hi); return; }
    float l = 0, h = 0;
    if (app.compareRangeMode == 3) {
        // Each side at its own fit (#301): display only, like the two modes
        // around it - the side's black/white stay as they are, and come back
        // when the mode changes or compare ends. Its own NEW frame is a
        // different doc with its own cache, so stepping (follow-frame
        // included) re-fits each side.
        ownFitCached(*self, l, h);
    } else {
        l = v.side[0]->vmin;                  // union of the CONTENT, not of the
        h = v.side[0]->vmax;                  // stretches
        for (int i = 1; i < v.n; i++) {
            l = std::min(l, v.side[i]->vmin);
            h = std::max(h, v.side[i]->vmax);
        }
    }
    if (h > l) { lo = l; hi = h; }
}
static float effBlack(const ImageDoc& im) {
    float lo, hi;
    displayRangeFor(im, rangeReadsView() ? comparedSides() : ViewSides(), lo, hi);
    return lo;
}
static float effWhite(const ImageDoc& im) {
    float lo, hi;
    displayRangeFor(im, rangeReadsView() ? comparedSides() : ViewSides(), lo, hi);
    return hi;
}
// The letter of side i of a view: "A", "B", or the slot's.
static std::string viewSideLetter(const ViewSides& v, int i) {
    return v.role[i] == 0 ? std::string("A") : v.role[i] == 1 ? std::string("B")
                                             : slotName((size_t)(v.role[i] - 2));
}
// "B held at frame 1" for every side of `v` whose follow found no frame at A's
// number and which the range is taken over (the union only): the screen draws
// such a side on its PINNED frame - the one chosen as B or as the slot - and
// marks it [stale]; the union includes it,
// and so does the export - which then has to SAY it, because the file cannot
// carry the mark. Empty when no such side.
static std::vector<std::string> viewHeldSides(const ViewSides& v) {
    std::vector<std::string> out;
    if (app.linkRange || app.compareRangeMode != 2 || v.n < 2) return out;
    for (int i = 1; i < v.n; i++)
        if (v.diverged[i])
            out.push_back(viewSideLetter(v, i) + " held at frame " +
                          std::to_string(v.side[i]->seqIndex + 1));
    return out;
}
// ---- the range in force, in words (#321 I4) ---------------------------------
// ONE function says what range a picture is at and by which rule, and every
// surface takes its words from it: the Inspector's `in force` line, the
// Histogram's x axis (abRangeSaid), the export notes (exportRangeNote) and the
// video toast. Read off the same conditions displayRangeFor decides by, never
// off the numbers: a union that happens to equal one side's own range is still
// a union.
//   said  - the rule, in the words of the Range controls. Its HEAD - the text
//           before the first " · " - is what the shorter surfaces quote.
//   held  - "B held at frame 1" for every side the union is taken over that
//           follow-frame could not move (viewHeldSides).
//   warn  - what the reader must not miss, drawn amber: the sides are on
//           different scales.
struct RangeWords { std::string said, warn; std::vector<std::string> held; };
static ImageDoc* rangeRefDoc(ImageDoc& im);                       // below
static bool rangeFitLit(ImageDoc& ref, float eb, float ew);       // below
static std::string fmtVal(float v, const std::string& dtype);    // main.cpp
static const char* flavorWords(int fl) {
    static const char* W[3] = { "min-max", "0.1-99.9%", "median->target" };
    return W[std::clamp(fl, (int)AF_MinMax, (int)AF_Median)];
}
static RangeWords rangeWords(const ImageDoc& im, const ViewSides& v) {
    RangeWords r;
    if (app.linkRange) { r.said = "one range over all open images"; return r; }
    int self = -1;
    if (v.n >= 2) for (int i = 0; i < v.n && self < 0; i++) if (v.side[i] == &im) self = i;
    if (self >= 0 && app.compareRangeMode == 1) { r.said = "A's range for every side"; return r; }
    if (self >= 0 && app.compareRangeMode == 2) {
        r.said = "union over ";
        for (int i = 0; i < v.n; i++) r.said += (i ? ", " : "") + viewSideLetter(v, i);
        r.said += " (min-max)";
        r.held = viewHeldSides(v);
        return r;
    }
    if (self >= 0 && app.compareRangeMode == 3) {
        r.said = std::string("each side fits itself (") + flavorWords(app.autoFlavor) + ")";
        r.warn = " · sides on different scales";
        return r;
    }
    if (im.rangeOwn && im.seqId > 0) r.said = "this frame only";
    else if (app.rangeRule == RR_EachFrame)
        r.said = std::string("each frame fits itself (") + flavorWords(app.autoFlavor) + ")";
    else {
        r.said = im.seqId > 0 ? "one range for the stack" : "one range for the image";
        // whose fit the stack is wearing (#254), said in words and not only by
        // the lamp: only while that frame's fit IS the range
        if (im.seqId > 0) {
            ImageDoc& m = const_cast<ImageDoc&>(im);
            if (ImageDoc* ref = rangeRefDoc(m))
                if (rangeFitLit(*ref, im.black, im.white))
                    r.said += " · fit of frame " + std::to_string(ref->seqIndex + 1);
        }
    }
    if (self >= 0) {                          // compare on, "each keeps its own"
        r.said += " · each side keeps its own";
        float al = 0, ah = 0, bl = 0, bh = 0;
        ownRangeOf(*v.side[0], al, ah);
        bool differ = false;
        std::string nums;
        for (int i = 0; i < v.n; i++) {
            ownRangeOf(*v.side[i], bl, bh);
            if (bl != al || bh != ah) differ = true;
            nums += std::string(i ? " / " : "") + viewSideLetter(v, i) + " " +
                    fmtVal(bl, v.side[i]->dtype) + "-" + fmtVal(bh, v.side[i]->dtype);
        }
        if (differ) r.warn = " · " + nums;
    }
    return r;
}
// The head of the words: what a surface with no room for the rest quotes.
static std::string rangeWordsHead(const RangeWords& w) {
    const size_t at = w.said.find(" · ");
    return at == std::string::npos ? w.said : w.said.substr(0, at);
}
// The note an export carries: "range: <the words> = lo..hi". `video` adds
// "per frame" wherever the range is worked out again for every frame, and
// drops the numbers, which then differ frame by frame; a single picture
// carries its numbers. A side drawn in a role other than A says which
// ("drawn as B").
static std::string exportRangeNote(const ImageDoc& im, const ViewSides& v, bool video) {
    const RangeWords w = rangeWords(im, v);
    int self = -1;
    if (v.n >= 2) for (int i = 0; i < v.n && self < 0; i++) if (v.side[i] == &im) self = i;
    std::string out = "range: " + w.said;
    const bool eachFrame = !app.linkRange && app.rangeRule == RR_EachFrame;
    if (video && !app.linkRange &&
        ((self >= 0 && (app.compareRangeMode == 2 || app.compareRangeMode == 3)) ||
         (self >= 0 && app.compareRangeMode == 1 && eachFrame)))
        out += ", per frame";
    if (self > 0) out += " - drawn as " + viewSideLetter(v, self);
    if (!video) {
        float lo = 0, hi = 0;
        displayRangeFor(im, v, lo, hi);
        char num[64];
        snprintf(num, sizeof num, " = %.6g..%.6g", lo, hi);
        out += num;
        for (const std::string& h : w.held) out += "; " + h;
    }
    return out;
}
// Does the range in force read the OTHER sides' pixels? Only "auto over every
// side (union)" does (A's range and each-fits-itself draw a side from A or
// from itself), so only it can be misled by a side that is not a real frame.
static bool rangeReadsOtherSides() {
    return !app.linkRange && app.compareRangeMode == 2;
}
// "" when every side the view's range is TAKEN OVER is a real frame; the
// refusal otherwise. A decimated preview's min/max is not the frame's, and a
// file cannot say so afterwards - the same "not yet" as exportRefusalFor,
// naming the side and (for a stack) its frame. A side whose stack simply has
// no frame at A's number is NOT refused: the screen draws it on its pinned
// frame and unions over that, the export does the same, and exportRangeNote
// says so. `self` is the picture being exported, which exportRefusalFor has
// already asked about; EVERY other side is checked - A included, because a
// B or slot stack is exported in a view whose side 0 is A's paired frame (or
// the A on screen), and the union starts from A's min/max.
static std::string viewRefusalFor(const ViewSides& v, const ImageDoc* self) {
    if (!rangeReadsOtherSides() || v.n < 2) return {};
    for (int i = 0; i < v.n; i++) {
        const ImageDoc* s = v.side[i];
        if (s == self || exportRefusalFor(s).empty()) continue;
        const std::string who = viewSideLetter(v, i);
        return "the A/B range is the union over every side, and " +
               (s->seqId > 0 ? who + "'s frame " + std::to_string(s->seqIndex + 1)
                             : who + " (a single image)") +
               " is still the decimated preview - wait for the full frame to land, "
               "then export";
    }
    return {};
}
// The view a FRAME OF A STACK is on screen in, for an export of that stack
// (#304). `stepTo` is the frame A would have to be stepped onto to show it, or
// null when A stays put.
//   - A's own stack (or no comparison): A on that frame, viewSidesFor(d).
//   - a stack held by B or a slot: the view in which follow-frame pairs A
//     with d - A on the frame of its own stack carrying d's number - so d is
//     drawn in its role there. With follow-frame off (or A a single image, or
//     no such frame of A's) the screen's view with d in that side's place: d
//     shown as that letter, A where it is.
//   - a stack nobody compares: its own range, an empty view.
static ViewSides exportFrameViewFor(ImageDoc* d, ImageDoc*& stepTo) {
    ImageDoc* a = cur();
    stepTo = d;
    if (!a || app.compareMode == App::CmpOff || d->seqId <= 0 || d->seqId == a->seqId)
        return viewSidesFor(d);
    ViewSides s = comparedSides();
    int r = -1;
    for (int i = 1; i < s.n && r < 0; i++) if (s.side[i]->seqId == d->seqId) r = i;
    if (r < 0) return ViewSides();
    if (app.compareFollowFrame && a->seqId > 0) {
        ImageDoc* pair = nullptr;
        for (const auto& q : app.images)
            if (q->seqId == a->seqId && q->seqIndex == d->seqIndex) { pair = q.get(); break; }
        if (pair) {
            ViewSides v = viewSidesFor(pair);
            if (sideOfView(*d, v)) { stepTo = pair; return v; }
        }
    }
    stepTo = nullptr;
    for (int p = 0; p < s.nPane; p++)       // ...and the pane holding that role
        if (s.paneRole[p] == s.role[r]) { s.pane[p] = d; s.paneStale[p] = false; }
    s.side[r] = d;
    s.diverged[r] = false;
    return s;
}

// ---------------- side-by-side tiles: the pane geometry --------------------
// More than two compare slots means side by side and nothing else. A wipe, a
// draggable split, a difference and a blink each mean exactly TWO images -
// there is no honest three-image version of any of them - so those four keep
// doing what they always did with A and B, and the extras get tiles.
//
// This lives out here, free of ImGui and of any frame state, because the
// geometry IS the feature: --tile-selftest has to be able to assert the
// rectangles without opening a window (this machine cannot screenshot GL).
struct TilePane { float x0 = 0, w = 0; };

// A pane narrower than this is not a comparison, it is a sliver: the badge that
// says WHICH image it holds no longer fits, and two 40 px strips of a sensor
// cannot be judged against each other. Say so rather than draw them.
static float tileMinPaneW(float uiScale) { return 110.0f * uiScale; }
// Half the band between two panes (the band is 2 * gutter wide).
static float tileGutter(float uiScale) { return 3.0f * uiScale; }

// n equal-width panes across [x0, x0+width), separated by a band of 2*gutter -
// the same band the A/B divider occupies, so the two layouts read as relatives.
//
// Equal width is the rule, not a simplification: app.splitFrac exists for A
// against B, and applied to five panes it would make one of them a sliver.
// Returns false, and leaves `out` empty, when the panes would be too narrow.
static bool tileLayout(float x0, float width, int n, float gutter, float minPaneW,
                       std::vector<TilePane>& out) {
    out.clear();
    if (n <= 0) return false;
    const float paneW = (width - (n - 1) * 2.0f * gutter) / (float)n;
    if (!(paneW >= minPaneW)) return false;      // written so a NaN fails too
    out.reserve(n);
    for (int i = 0; i < n; i++)
        out.push_back({ x0 + i * (paneW + 2.0f * gutter), paneW });
    return true;
}

// The ONE mapping every pane uses. Zoom and centre come from the view, never
// from the pane: two images of the same scene at different magnifications,
// side by side, is exactly how a reader compares sharpness or noise between
// them and reaches a wrong conclusion. A pane that cannot show its image at
// the shared zoom clips - it is never quietly rescaled.
static ImVec2 tileMap(ImVec2 p0, ImVec2 sz, ImVec2 center, float zoom, float ix, float iy) {
    return ImVec2(p0.x + sz.x * 0.5f + (ix - center.x) * zoom,
                  p0.y + sz.y * 0.5f + (iy - center.y) * zoom);
}
static ImVec2 tileUnmap(ImVec2 p0, ImVec2 sz, ImVec2 center, float zoom, ImVec2 sc) {
    return ImVec2(center.x + (sc.x - p0.x - sz.x * 0.5f) / zoom,
                  center.y + (sc.y - p0.y - sz.y * 0.5f) / zoom);
}

// The sentence for "these panes would be slivers" - the canvas's badge and
// the compare export's refusal (#308), in one spelling.
static std::string tileNarrowSaid(int n, float minW, float canvasW) {
    char m[224];
    snprintf(m, sizeof m,
             "%d compare panes need at least %.0f px each - this canvas is "
             "%.0f px. Widen the window, or drop a slot.",
             n, minW, canvasW);
    return m;
}

// ---------------------------------------- the COMPARE VIEW, at 1:1 (#308) ---
// The picture the canvas composes - A | B | C side by side, the wipe, the
// difference - built from the documents instead of read off the framebuffer,
// so it does not depend on the window, the zoom or the pan. Every side is
// drawn by renderDocRGBA(side, &v): the view `v` decides each side's range,
// exactly as for a single picture (#304), and the value highlight comes with
// it. composeCompareRGBA is the ONE place the composite is built; the PNG
// (saveCompareViewPng) and every frame of the compare video (pumpVideoExport)
// call it, so frame k of the video and the PNG of that view are the same
// bytes. Overlays (ROI frames, pins, grid) are not burnt in: they are chrome,
// and "Copy view (as displayed)" is the item that carries them.
//
// Layout, per mode (compareLayoutFor):
//   side by side / tiled - every side of the view, in the canvas's order (A,
//       B, then the slots in letter order), left to right in one row, each in
//       a cell the size of the LARGEST side, top-left aligned as the shared
//       view aligns them on screen; the rest of a smaller side's cell is
//       black. CMP_SEP_PX columns of neutral grey between cells.
//   wipe - A and B over one cell (the larger of the two, black where neither
//       reaches); A left of the image column the SCREEN's divider passes
//       through (tileUnmap of drawCanvas's wipeX, at the view's centre and
//       zoom), B from it, and that column is the 1 px divider line when it
//       falls inside the picture. A divider off the image's left edge is all
//       B, off its right edge all A - what the screen shows - with no line.
//   difference - renderDiffRGBA, the function the screen's difference texture
//       is drawn by, at the scale the screen uses (app.diffGain, or the auto
//       scale of THIS pair, diffAutoGainOf).
//   blink - refused: it is two pictures in time, not one.
// The panes are the view's PANES (ViewSides::pane) - the list tileSlots gives
// the canvas, one per letter, so a slot on B's document is a pane of its own
// here as it is there. One row because the canvas tiles in one row:
// tileLayout returns n panes left to right (TilePane is x0 and a width).
// Wipe and difference show A and B only, as on screen; slots are named as
// "not shown". The letters (app.cmpExportLetters) go top-left of each pane,
// in the canvas badge's ink (paneBadgeInk), on a black plate - drawn from a built-in 5x7 bitmap, because a
// windowless export has no font atlas to draw text with.

// The letter of a pane role: "A", "B", or the slot's.
static std::string paneRoleLetter(int role) {
    return role == 0 ? std::string("A") : role == 1 ? std::string("B") : slotName((size_t)(role - 2));
}
// The ink a pane's letter is drawn in on the CANVAS badge - and so in the
// compare export, which depicts the canvas. ONE definition: drawCanvas's
// badges call it, the export's letters call it. `role` 0 = A, 1 = B, 2+ = a
// slot; a slot's hue follows its PANE position past A (and B, when there is
// one: nAB = 2, else 1), as the badges always have. (slotInk, the plots' ink
// for the same letters, is a different palette - A green, B blue.)
static ImU32 paneBadgeInk(int role, int paneIndex, int nAB) {
    if (role == 0) return IM_COL32(120, 200, 255, 255);   // A: blue
    if (role == 1) return IM_COL32(255, 190, 120, 255);   // B: orange
    // C, D, E...: one hue each - "which pane is D" answered by the badge alone
    static const ImU32 TILE_COLS[6] = {
        IM_COL32(105, 220, 130, 255), IM_COL32(200, 120, 255, 255),
        IM_COL32(90, 220, 220, 255),  IM_COL32(255, 150, 200, 255),
        IM_COL32(180, 200, 90, 255),  IM_COL32(255, 120, 120, 255),
    };
    return TILE_COLS[std::max(0, paneIndex - nAB) % 6];
}

static const int CMP_SEP_PX = 2;
static const uint8_t CMP_SEP_RGB[3]  = { 128, 128, 128 };   // neutral grey
static const uint8_t CMP_WIPE_RGB[3] = { 255, 255, 255 };   // the canvas's wipe line

struct CompareLayout {
    int mode = 0;                       // App::CmpSplit / CmpWipe / CmpDiff
    int w = 0, h = 0;                   // the composite
    int cellW = 0, cellH = 0;           // split / wipe: the cell every side is placed in
    int n = 0;                          // sides drawn
    int at[CMP_SIDES_MAX] = {};         // ...as indices into v.pane, in drawing order
    int x0[CMP_SIDES_MAX] = {};         // split: each cell's left column
    int div = 0;                        // wipe: the divider column; B from it (0 = all B, w = all A)
    bool divFromScreen = false;         // ...taken off the screen's divider (else its fraction)
    bool padded = false;                // a side is smaller than its cell
    std::string unshown;                // wipe / difference: the letters not drawn
};

// What a compare export draws, as of NOW: the mode, the column the screen's
// wipe divider passes through, the letters, the difference scale, the canvas
// width tiling is decided for, and the pins the views resolve from. Taken when
// the export is ASKED for (the PNG dialog opens, the video's modal opens) and
// used from then on (App::CompareSpec).
static App::CompareSpec compareSpecNow() {
    App::CompareSpec s;
    s.mode = app.compareMode;
    s.wipeFrac = app.wipeFrac;
    s.letters = app.cmpExportLetters;
    s.diffGain = app.diffGain;
    s.diffAbs = app.diffAbs;
    s.uiScale = app.uiScale;
    if (g_canvasRectFrame >= 0) {          // a canvas has been drawn: its geometry
        const ImVec2 p0(g_canvasRect[0], g_canvasRect[1]), sz(g_canvasRect[2], g_canvasRect[3]);
        s.canvasW = sz.x;
        const float wipeX = p0.x + sz.x * app.wipeFrac;           // drawCanvas's divider
        const float ix = tileUnmap(p0, sz, app.view.center, app.view.zoom, ImVec2(wipeX, p0.y)).x;
        if (std::isfinite(ix)) s.wipeCol = (int)lroundf(std::clamp(ix, 0.0f, 1.0e9f));
    }
    if (cur()) s.aUid = cur()->uid;
    s.bUid = app.compareBUid;
    s.slots = app.cmpExtra;
    s.follow = app.compareFollowFrame;
    return s;
}

// "" and `L` filled when the view can be composed under `S`; the refusal
// otherwise. Draws nothing, so the menu, the modal and the video's up-front
// check can ask it for every frame.
static std::string compareLayoutFor(const ViewSides& v, CompareLayout& L, const App::CompareSpec& S) {
    L = CompareLayout();
    L.mode = S.mode;
    if (S.mode == App::CmpOff || app.compareMode == App::CmpOff)
        return "compare is off - there is no compare view to export";
    if (v.nPane < 2)
        return "nothing to compare with (no B, no slots) - pick a B (View > Compare A/B > "
               "B image) or add a compare slot";
    if (S.mode == App::CmpFlip)
        return "blink shows one side at a time, so it is not one picture - switch to "
               "side by side or wipe (View > Compare A/B), then export";
    int bAt = -1;
    for (int i = 1; i < v.nPane && bAt < 0; i++) if (v.paneRole[i] == 1) bAt = i;
    if (S.mode == App::CmpSplit) {
        for (int i = 0; i < v.nPane; i++) L.at[L.n++] = i;
        // Tiled, the canvas refuses panes narrower than tileMinPaneW and SAYS
        // so instead of drawing them; a file of the panes it declined to draw
        // would not be the screen, so the export refuses in the same words.
        bool slot = false;
        for (int k = 0; k < L.n; k++) if (v.paneRole[L.at[k]] >= 2) slot = true;
        if (slot && S.canvasW > 0) {
            std::vector<TilePane> tp;
            const float minW = tileMinPaneW(S.uiScale);
            if (!tileLayout(0.0f, S.canvasW, L.n, tileGutter(S.uiScale), minW, tp))
                return tileNarrowSaid(L.n, minW, S.canvasW);
        }
    } else {
        const char* mode = S.mode == App::CmpWipe ? "wipe" : "difference";
        if (bAt < 0) return std::string(mode) + " needs a B image - View > Compare A/B > B image";
        L.at[L.n++] = 0;
        L.at[L.n++] = bAt;
        for (int i = 1; i < v.nPane; i++)
            if (i != bAt) L.unshown += (L.unshown.empty() ? "" : ", ") + paneRoleLetter(v.paneRole[i]);
    }
    // Every side DRAWN is drawn dot by dot, so each is refused as a picture
    // would be; and every side the range is taken over (the union reads the
    // slots wipe does not draw), as viewRefusalFor says for one picture.
    for (int k = 0; k < L.n; k++) {
        const std::string why = exportRefusalFor(v.pane[L.at[k]]);
        if (!why.empty()) return paneRoleLetter(v.paneRole[L.at[k]]) + ": " + why;
    }
    {
        const std::string why = viewRefusalFor(v, nullptr);
        if (!why.empty()) return why;
    }
    auto dims = [](const ImageDoc* d) { return std::to_string(d->w) + "x" + std::to_string(d->h); };
    if (S.mode == App::CmpDiff) {
        const ImageDoc* A = v.pane[0];
        const ImageDoc* B = v.pane[bAt];
        if (A->w != B->w || A->h != B->h)
            return "difference needs A and B to be the same size (A " + dims(A) + ", B " +
                   dims(B) + ")";
        L.w = A->w; L.h = A->h;
    } else {
        for (int k = 0; k < L.n; k++) {
            L.cellW = std::max(L.cellW, v.pane[L.at[k]]->w);
            L.cellH = std::max(L.cellH, v.pane[L.at[k]]->h);
        }
        for (int k = 0; k < L.n; k++)
            if (v.pane[L.at[k]]->w != L.cellW || v.pane[L.at[k]]->h != L.cellH) L.padded = true;
        if (S.mode == App::CmpSplit) {
            for (int k = 0; k < L.n; k++) L.x0[k] = k * (L.cellW + CMP_SEP_PX);
            L.w = L.n * L.cellW + (L.n - 1) * CMP_SEP_PX;
        } else {
            L.w = L.cellW;
            L.divFromScreen = S.wipeCol >= 0;
            L.div = L.divFromScreen ? std::clamp(S.wipeCol, 0, L.w)
                                    : std::clamp((int)lroundf(S.wipeFrac * (float)L.w), 0, L.w);
        }
        L.h = L.cellH;
    }
    // 512 Mpx, i.e. a 2 GB RGBA buffer: past it the composite is not a picture
    // anyone opens, and the allocation alone can take the session down.
    if ((uint64_t)L.w * (uint64_t)L.h > (1ull << 29))
        return "the compare view would be " + std::to_string(L.w) + "x" + std::to_string(L.h) +
               " - over 512 Mpx; compare fewer sides";
    return {};
}

// The letters, as 5x7 bitmaps: A, B and the slot letters (SLOT_LETTERS ends
// at P). Row r of glyph g is CMP_GLYPH[g][r], bit 4 the leftmost column.
static const uint8_t CMP_GLYPH[16][7] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   // A
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },   // B
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },   // C
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },   // D
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   // E
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   // F
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E },   // G
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   // H
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },   // I
    { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C },   // J
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   // K
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },   // L
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   // M
    { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },   // N
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   // O
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },   // P
};
// Where the letter of a cell at (x0, 0), cellW x cellH, goes: a plate of
// 7s x 9s pixels (the 5x7 glyph and a border of s, s the scale) inset by s
// from the cell's corner. s grows with the cell so the letter stays legible on
// a large frame: one step per 128 px of the shorter side, at most 8. False
// when the plate does not fit in the cell - a letter over a neighbour is worse
// than none.
struct CmpLabelBox { int x = 0, y = 0, w = 0, h = 0, s = 1; };
static bool cmpLabelBox(int x0, int cellW, int cellH, CmpLabelBox& b) {
    b.s = std::clamp(std::min(cellW, cellH) / 128, 1, 8);
    b.x = x0 + b.s; b.y = b.s; b.w = 7 * b.s; b.h = 9 * b.s;
    return b.w + 2 * b.s <= cellW && b.h + 2 * b.s <= cellH;
}
static void cmpPaintLetter(std::vector<uint8_t>& rgba, int W, int x0, int cellW, int cellH,
                           const std::string& letter, ImU32 ink) {
    CmpLabelBox b;
    if (letter.size() != 1 || letter[0] < 'A' || letter[0] > 'P') return;
    if (!cmpLabelBox(x0, cellW, cellH, b)) return;
    const uint8_t* g = CMP_GLYPH[letter[0] - 'A'];
    const uint8_t ir = (uint8_t)((ink >> IM_COL32_R_SHIFT) & 0xFF);
    const uint8_t ig = (uint8_t)((ink >> IM_COL32_G_SHIFT) & 0xFF);
    const uint8_t ib = (uint8_t)((ink >> IM_COL32_B_SHIFT) & 0xFF);
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            const int gx = x / b.s - 1, gy = y / b.s - 1;     // the border is plate
            const bool on = gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && ((g[gy] >> (4 - gx)) & 1);
            uint8_t* p = &rgba[((size_t)(b.y + y) * W + (b.x + x)) * 4];
            p[0] = on ? ir : 0; p[1] = on ? ig : 0; p[2] = on ? ib : 0; p[3] = 255;
        }
}

// The sentence a compare export is announced with: the mode and the layout,
// what was padded or not shown, and the range in the controls' words
// (exportRangeNote, for A - every side is drawn under the same rule). `video`:
// the range and an auto difference scale are worked out again for every
// frame, so no numbers. `gain` is the difference's full scale (one picture).
static std::string compareExportNote(const ViewSides& v, const CompareLayout& L, bool video,
                                     float gain, const App::CompareSpec& S) {
    std::string s = "compare: ";
    char num[128];
    if (L.mode == App::CmpDiff) {
        s += S.diffAbs ? "difference |A-B|" : "difference A-B";
        if (S.diffGain > 0) snprintf(num, sizeof num, ", full scale +-%g (set)", S.diffGain);
        else if (video) snprintf(num, sizeof num, ", full scale auto (99.9th percentile), per frame");
        else snprintf(num, sizeof num, ", full scale +-%g (auto)", gain);
        s += num;
    } else {
        std::string order;
        for (int k = 0; k < L.n; k++) order += (k ? " | " : "") + paneRoleLetter(v.paneRole[L.at[k]]);
        if (L.mode == App::CmpSplit) {
            snprintf(num, sizeof num, ", 1:1 in %dx%d cells, %d px grey separators",
                     L.cellW, L.cellH, CMP_SEP_PX);
            s += (L.n > 2 ? "tiled " : "side by side ") + order + num;
        } else {
            if (L.divFromScreen)
                snprintf(num, sizeof num, "wipe: B from column %d of %d, where the screen's divider "
                         "is (%s)", L.div, L.w,
                         L.div <= 0 ? "all B" : L.div >= L.w ? "all A" : "1 px line");
            else
                snprintf(num, sizeof num, "wipe: B from column %d of %d, %.0f%% of the width - no "
                         "canvas drawn (1 px line)", L.div, L.w, S.wipeFrac * 100.0f);
            s += num;
        }
        if (L.padded) {
            std::string undersized;
            for (int k = 0; k < L.n; k++) {
                const ImageDoc* d = v.pane[L.at[k]];
                if (d->w != L.cellW || d->h != L.cellH)
                    undersized += (undersized.empty() ? "" : ", ") + paneRoleLetter(v.paneRole[L.at[k]]) + " " +
                             std::to_string(d->w) + "x" + std::to_string(d->h);
            }
            snprintf(num, sizeof num, "%dx%d", L.cellW, L.cellH);
            s += "; sizes differ - padded with black to " + std::string(num) + " (" + undersized + ")";
        }
    }
    if (!L.unshown.empty())
        s += "; " + L.unshown + " not shown (" +
             (L.mode == App::CmpWipe ? "wipe" : "difference") + " compares A and B)";
    if (L.mode != App::CmpDiff) {
        s += "  -  " + exportRangeNote(*v.side[0], v, video);
        // the numbers a single picture carries are A's; under the two modes
        // where each side has its own, that has to be said
        if (!video && !app.linkRange && (app.compareRangeMode == 0 || app.compareRangeMode == 3))
            s += " (A's; each side at its own)";
    }
    // the difference carries no letters (it is one picture of a pair), so it
    // claims none
    if (L.mode != App::CmpDiff) s += S.letters ? "  -  letters on" : "  -  no letters";
    return s;
}

// Copy a side's rows into the composite at column dx, only the composite
// columns [c0, c1) - the wipe draws A left of the divider and B right of it.
static void cmpBlit(std::vector<uint8_t>& dst, int W, const std::vector<uint8_t>& src,
                    int sw, int sh, int dx, int c0, int c1) {
    const int x0 = std::max(dx, c0), x1 = std::min(dx + sw, c1);
    if (x1 <= x0) return;
    for (int y = 0; y < sh; y++)
        memcpy(&dst[((size_t)y * W + x0) * 4], &src[((size_t)y * sw + (x0 - dx)) * 4],
               (size_t)(x1 - x0) * 4);
}

// The compare view `v` as one RGBA picture, w x h, drawn under `spec` (null =
// compareSpecNow(), the screen as it is). False, with the refusal in `note`,
// when it cannot be composed; otherwise `note` is the announcement.
static bool composeCompareRGBA(const ViewSides& v, std::vector<uint8_t>& rgba, int& w, int& h,
                               std::string& note, const App::CompareSpec* spec = nullptr) {
    const App::CompareSpec S = spec ? *spec : compareSpecNow();
    CompareLayout L;
    note = compareLayoutFor(v, L, S);
    // renderDiffRGBA reads app.diffAbs itself (one flag for the screen and the
    // bar), so a spec taken under the other one cannot be drawn honestly
    if (note.empty() && S.mode == App::CmpDiff && S.diffAbs != app.diffAbs)
        note = "the difference changed between A-B and |A-B| since this export was asked "
               "for - export again";
    if (!note.empty()) { w = h = 0; rgba.clear(); return false; }
    w = L.w; h = L.h;
    if (L.mode == App::CmpDiff) {
        ImageDoc* A = v.pane[L.at[0]];
        ImageDoc* B = v.pane[L.at[1]];
        const float gain = S.diffGain > 0 ? S.diffGain : diffAutoGainOf(*A, *B);
        renderDiffRGBA(*A, *B, w, h, std::min(A->ch, B->ch), gain, rgba);
        note = compareExportNote(v, L, false, gain, S);
        return true;
    }
    rgba.assign((size_t)w * h * 4, 0);
    for (size_t p = 0; p < (size_t)w * h; p++) rgba[p * 4 + 3] = 255;   // opaque black
    static std::vector<uint8_t> side;    // one scratch buffer, as rebuildTexture keeps
    for (int k = 0; k < L.n; k++) {
        ImageDoc* d = v.pane[L.at[k]];
        renderDocRGBA(*d, side, &v);
        if (L.mode == App::CmpSplit) cmpBlit(rgba, w, side, d->w, d->h, L.x0[k], 0, w);
        else if (k == 0)             cmpBlit(rgba, w, side, d->w, d->h, 0, 0, L.div);
        else                         cmpBlit(rgba, w, side, d->w, d->h, 0, L.div, w);
    }
    auto column = [&](int x, const uint8_t* c) {
        for (int y = 0; y < h; y++) {
            uint8_t* p = &rgba[((size_t)y * w + x) * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = 255;
        }
    };
    if (L.mode == App::CmpSplit) {
        for (int k = 1; k < L.n; k++)
            for (int x = L.x0[k] - CMP_SEP_PX; x < L.x0[k]; x++) column(x, CMP_SEP_RGB);
    } else if (L.div > 0 && L.div < w) {      // a divider off the picture draws no line
        column(L.div, CMP_WIPE_RGB);
    }
    if (S.letters) {
        int nAB = 1;
        for (int k = 0; k < L.n; k++) if (v.paneRole[L.at[k]] == 1) nAB = 2;
        for (int k = 0; k < L.n; k++) {
            const int role = v.paneRole[L.at[k]];
            const ImU32 ink = paneBadgeInk(role, k, nAB);
            const std::string letter = paneRoleLetter(role);
            if (L.mode == App::CmpSplit)
                cmpPaintLetter(rgba, w, L.x0[k], L.cellW, L.cellH, letter, ink);
            else if (k == 0)
                cmpPaintLetter(rgba, w, 0, L.div, L.cellH, letter, ink);
            else if (L.div == 0)
                cmpPaintLetter(rgba, w, 0, w, L.cellH, letter, ink);
            else
                cmpPaintLetter(rgba, w, L.div + 1, w - L.div - 1, L.cellH, letter, ink);
        }
    }
    note = compareExportNote(v, L, false, 0.0f, S);
    return true;
}

// The refusal for the compare view ON SCREEN ("" = it can be exported): the
// menu items' enabled state and tooltip.
static std::string compareViewRefusal() {
    CompareLayout L;
    return compareLayoutFor(comparedSides(), L, compareSpecNow());
}

// File > Save compare view as PNG..., the picture half of #308: the compare
// view the dialog was opened under (viewAUid, saveViewPng's rule), drawn by the
// spec taken then (null = now), composed by composeCompareRGBA - the same bytes
// a compare video writes for that view.
static void saveCompareViewPng(const std::string& path, uint64_t viewAUid = 0,
                               const App::CompareSpec* spec = nullptr) {
    ImageDoc* viewA = nullptr;
    if (viewAUid) for (const auto& d : app.images) if (d->uid == viewAUid) { viewA = d.get(); break; }
    const ViewSides view = viewA ? viewSidesFor(viewA) : comparedSides();
    std::vector<uint8_t> rgba; int w = 0, h = 0; std::string note;
    if (!composeCompareRGBA(view, rgba, w, h, note, spec)) { toast(note, true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    if (png.empty()) { toast("PNG encode failed", true); return; }
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write " + path, true); return; }
    f.write((const char*)png.data(), (std::streamsize)png.size());
    const std::string hlNote = exportHighlightNote(view.side[0]);
    fprintf(stderr, "export: %s\n", note.c_str());
    if (!hlNote.empty()) fprintf(stderr, "export: %s\n", hlNote.c_str());
    toast("saved " + baseName(path) + "  " + std::to_string(w) + "x" + std::to_string(h) +
          "  -  " + note + (hlNote.empty() ? "" : "  -  " + hlNote));
}
static void askCompareViewPng() {
    const std::string why = compareViewRefusal();
    if (!why.empty()) { toast(why, true); return; }
    if (!pfd::settings::available()) {
        toast("no file-dialog backend found (install zenity or kdialog)", true);
        return;
    }
    app.pngDlgCompare = true;
    app.pngDlgCmpSpec = compareSpecNow();   // the picture is the one asked for
    app.pngDlgUid = 0;
    app.pngDlgViewAUid = cur()->uid;
    app.pngDlg = std::make_unique<pfd::save_file>("Save compare view as PNG",
        baseName(cur()->name) + "_compare.png",
        std::vector<std::string>{ "PNG (*.png)", "*.png" });
}
// ---- the Range setters (#321) ----------------------------------------------
// The scope (who shares one range) and the rule (how it is decided) are chosen
// through these and nothing else - the Inspector, the View menu and the tests
// all call them, so a mode is never a side effect of a combo.

// Is a comparison on screen at all - the `A+B` scope exists only then.
static bool comparing() { return comparedSides().n >= 2; }

// The scope the `applies to` row lights for `im` (the doc on screen).
static int rangeScopeOf(const ImageDoc* im) {
    if (app.linkRange) return RS_All;
    if (app.compareRangeMode != 0 && comparing()) return RS_Compared;
    if (im && im->rangeOwn && im->seqId > 0) return RS_Frame;
    return RS_Stack;
}

// Choose who shares one range. `on` is the doc on screen.
//   frame    - `on` alone keeps the range it is drawn at now
//   stack    - `on` rejoins its stack's range; compare goes to "each keeps its own"
//   compared - every compared side, at the compare rule last chosen
//   all      - one range overlays every open image, seeded from what is on screen
static void setRangeScope(int s, ImageDoc* on) {
    float seenLo = 0, seenHi = 0;
    if (on) { seenLo = effBlack(*on); seenHi = effWhite(*on); }
    const bool wasLinked = app.linkRange;
    const bool cmp = comparing();
    if (s != RS_All) app.linkRange = false;
    if ((s == RS_Frame || s == RS_Stack) && cmp && app.compareRangeMode != 0) {
        app.compareRuleLast = app.compareRangeMode;
        app.compareRangeMode = 0;
    }
    if (s == RS_Frame && on && on->seqId > 0) {
        on->rangeOwn = true;
        on->black = seenLo; on->white = seenHi;
    } else if (s == RS_Stack && on && on->rangeOwn) {
        on->rangeOwn = false;
        if (const ImageDoc* h = stackRangeHolder(on->seqId, on)) {
            on->black = h->black; on->white = h->white;
        }
    } else if (s == RS_Compared && cmp) {
        app.compareRangeMode = std::clamp(app.compareRuleLast, 1, 3);
    } else if (s == RS_All) {
        if (!wasLinked) { app.linkBlack = seenLo; app.linkWhite = seenHi; }
        app.linkRange = true;
    }
    markAllTexDirty();
}

// Choose the stack's rule. "Each frame fits itself" writes nothing (I3).
// Going back to "one range" adopts the fit of the frame on screen as the
// stack's range: the picture in front of you is the one you meant to spread
// (#248's P8 ruling). A frame that keeps its own range is not the stack, so it
// spreads nothing.
static void setRangeRule(int r, ImageDoc* on) {
    if (r != RR_EachFrame && r != RR_OneRange) return;
    if (r == app.rangeRule) return;
    if (r == RR_OneRange && on && !on->rangeOwn && !app.linkRange) {
        float lo = 0, hi = 0;
        ownRangeOf(*on, lo, hi);              // its fit: the rule is still EachFrame here
        on->black = lo; on->white = hi;
        shareStackRange(*on);
    }
    app.rangeRule = r;
    markAllTexDirty();
}

// Choose the compare rule: 0 "each keeps its own" (the scope is then the
// stack), 1 A's range, 2 union, 3 each side fits itself.
static void setCompareRule(int m) {
    if (m < 0 || m > 3) return;
    if (m >= 1) app.compareRuleLast = m;
    app.compareRangeMode = m;
    markAllTexDirty();
}

// WHICH fit "each frame fits itself" and "each side fits itself" draw with
// (#252, #301): one variable for both, chosen by the fit buttons under those
// rules, the "fit:" items of View > Compare A/B, and the session.
static void setFlavor(int fl) {
    if (fl < AF_MinMax || fl > AF_Median) return;
    app.autoFlavor = fl;
    markAllTexDirty();                // the fits are cached per flavour: draw them anew
}

// Every write of a display range - the number field, all six buttons, the
// Histogram's zoom link, the tests - arrives here, and goes to the scope in
// force (I1): all open -> the link range; frame -> that frame alone; A+B ->
// A's stack; stack -> the stack, minus its frames that keep their own.
// A fixed number is a declaration that ONE range is wanted (I2): under "each
// frame fits itself" the rule moves to "one range", under the union or "each
// side fits itself" to A's range - otherwise the number is written and the
// screen goes on drawing the fit, which is the trap #321 closes. The move is
// said, because the rule row changing under you would otherwise be silent.
static void setRange(ImageDoc& im, float black, float white) {
    if (!(white > black)) return;
    if (app.linkRange) {
        app.linkBlack = black; app.linkWhite = white;
        markAllTexDirty();            // shared value changed: every image re-renders
        return;
    }
    ImageDoc* t = &im;
    const ViewSides v = comparedSides();
    if (app.compareRangeMode != 0 && sideOfView(im, v)) {
        if (app.compareRangeMode != 1) {
            toast(std::string("A/B range: ") +
                  (app.compareRangeMode == 2 ? "union" : "each side fits itself") +
                  " -> A's range (a typed or fixed range is one range)", false);
            app.compareRangeMode = 1;
            app.compareRuleLast = 1;
        }
        t = v.side[0];
        for (int i = 0; i < v.n; i++) v.side[i]->texDirty = true;
    }
    t->black = black; t->white = white; t->texDirty = true;
    if (t->rangeOwn) return;          // this frame only
    if (app.rangeRule == RR_EachFrame) {
        toast("range rule: each frame fits itself -> one range "
              "(a typed or fixed range is one range for the stack)", false);
        app.rangeRule = RR_OneRange;
        markAllTexDirty();
    }
    shareStackRange(*t);
}

// ---- which Range button is the range currently in force? (#248) ------------
// The row of five presets used to give no sign of which one you are looking at.
// It is answered by COMPARISON and carries no state of its own: nothing has to
// remember that Auto was pressed, and a range restored from a session or typed
// by hand is judged by exactly the same rule as one that came from a click.
//
// It is therefore not a radio group. Two rules can be true of the same numbers
// - data that happens to run exactly 0..1 makes Auto and 0-1 the same answer -
// and both buttons light: the row states facts about the range, and hiding one
// of two true facts to keep the look of a selector would be a lie about the
// data. Nothing matching lights nothing, which is what a hand-typed range is.
//
// The row is TWO rows on the panel - the three fits, then the three fixed
// conventions - but one rule and one enum: which row a button is drawn on is
// a reading-order decision, not a difference in what lighting means.
enum RangeButton { RB_Auto = 0, RB_AutoPct, RB_Median, RB_0_1, RB_0_255, RB_0_65535 };

// WHOSE pixels the three fits are compared against (#254). Normally the frame
// on screen. In "Per stack" it is the frame whose fit was SPREAD over the
// stack: the shared range is that frame's answer, so asking any other frame
// "is this your min..max?" gets a truthful no, and the button went dark on the
// first step - 「Stack で Auto の設定が外れる」. Null = there is nothing to
// compare against (nothing ever spread this stack's range, or the frame that
// did has been closed), and the row then says nothing at all.
// Not consulted by the fixed presets: 0-255 is 0-255 whoever is looking.
// ...except where the range in force is not the stack's at all: "each side
// fits itself" (#301) shows every compared side at ITS OWN fit, so that side's
// own pixels are what the row has to compare against.
static ImageDoc* rangeRefDoc(ImageDoc& im) {
    if (app.linkRange || app.rangeRule != RR_OneRange || im.seqId <= 0 || im.rangeOwn) return &im;
    if (app.compareRangeMode == 3 && comparedSideOf(im)) return &im;
    App::SeqInfo* si = seqInfo(im.seqId);
    if (!si || si->rangeRefUid == 0) return nullptr;
    if (si->rangeRefUid == im.uid) return &im;
    for (auto& d : app.images) if (d->uid == si->rangeRefUid) return d.get();
    return nullptr;
}

static bool rangeButtonMatches(ImageDoc& im, int btn, float eb, float ew) {
    switch (btn) {
        case RB_0_1:     return eb == 0.0f && ew == 1.0f;
        case RB_0_255:   return eb == 0.0f && ew == 255.0f;
        case RB_0_65535: return eb == 0.0f && ew == 65535.0f;
        case RB_Auto: {
            // the button's own arithmetic, repeated: linked Auto fits the
            // min/max over every open image, and those are cached numbers -
            // no pixel is read here.
            if (app.linkRange) {
                float lo = im.vmin, hi = im.vmax;
                for (const auto& d : app.images) { lo = std::min(lo, d->vmin); hi = std::max(hi, d->vmax); }
                return eb == lo && ew == hi;
            }
            ImageDoc* ref = rangeRefDoc(im);
            return ref && eb == ref->vmin && ew == ref->vmax;
        }
        case RB_AutoPct: {
            // Linked "Auto %" pools EVERY open pixel onto one histogram
            // (autoPercentileFit says so in as many words). That is a second's
            // work on a 200-frame stack and this question is asked once per UI
            // frame, so the linked row simply does not light this button: the
            // fixed presets and Auto still answer, and no answer is a smaller
            // wrong than a stall. Unlinked, the answer is per-image and cached.
            if (app.linkRange) return false;
            ImageDoc* ref = rangeRefDoc(im);
            if (!ref) return false;
            // A quantile of the reference frame's pixels cannot fall outside
            // their own min..max, so a wider range is refused before the first
            // 65536-bin pass is ever paid for.
            if (eb < ref->vmin || ew > ref->vmax) return false;
            float lo = 0, hi = 0;
            percentileRangeCached(*ref, lo, hi);
            return eb == lo && ew == hi;
        }
        case RB_Median: {
            // Linked "Med" pools every open pixel exactly as linked "Auto %"
            // does, and is dark here for exactly that reason.
            if (app.linkRange) return false;
            ImageDoc* ref = rangeRefDoc(im);
            if (!ref) return false;
            // The black point is known without reading a pixel unless it is
            // the 0.1% cut (cached), so a range whose black is not the chosen
            // one is refused before the median pass is paid for.
            float p1 = ref->vmin;
            if (app.medBlackMode == MB_MinPct) { float b = 0; percentileRangeCached(*ref, p1, b); }
            const float black = medBlackOf(ref->vmin, p1);
            if (eb != black) return false;
            float med = 0, white = 0;
            if (!medianValue(*ref, med)) return false;
            if (!medianWhite(black, med, app.medTarget, white)) return false;
            return ew == white;
        }
    }
    return false;
}
// Is the range eb..ew one of `ref`'s three fits? The in-force line's "fit of
// frame k" asks this of the stack's reference frame (rangeWords).
static bool rangeFitLit(ImageDoc& ref, float eb, float ew) {
    return rangeButtonMatches(ref, RB_Auto, eb, ew) || rangeButtonMatches(ref, RB_AutoPct, eb, ew) ||
           rangeButtonMatches(ref, RB_Median, eb, ew);
}
// ---- what the Inspector's Range rows say (#321 §3) ---------------------------
// Pure answers, so --range-selftest reads exactly what the panel draws.

// The `applies to` row: the segments offered for `im`, in reading order. A
// single image has no `frame` (it IS its stack); `A+B` exists only while a
// comparison is on, spelled with the letters compared ("A+B+C", "A+B+C+..."
// past four).
struct RangeSeg { int scope; std::string label; };
static std::vector<RangeSeg> rangeScopeSegs(const ImageDoc* im) {
    std::vector<RangeSeg> out;
    if (im && im->seqId > 0) out.push_back({ RS_Frame, "frame" });
    out.push_back({ RS_Stack, "stack" });
    const ViewSides v = comparedSides();
    if (v.n >= 2) {
        std::string l;
        for (int i = 0; i < v.n && i < 4; i++) l += (i ? "+" : "") + viewSideLetter(v, i);
        if (v.n > 4) l += "+…";
        out.push_back({ RS_Compared, l });
    }
    out.push_back({ RS_All, "all open" });
    return out;
}
// The `rule` row for a scope: (id, label). Stack: RangeRule ids; A+B: the
// compare rule 1..3; frame and all open: one range only (id -1, not a choice).
static std::vector<std::pair<int, std::string>> rangeRuleSegs(int scope) {
    if (scope == RS_Stack) return { { RR_OneRange, "one range" }, { RR_EachFrame, "each frame fits itself" } };
    if (scope == RS_Compared) return { { 1, "A's range" }, { 2, "union" }, { 3, "each side fits itself" } };
    return { { -1, "one range" } };
}
static int rangeRuleLit(int scope) {
    return scope == RS_Stack ? app.rangeRule : scope == RS_Compared ? app.compareRangeMode : -1;
}
static std::vector<int> framesOfSeq(int seqId);   // fwd (sequence.inc)
// The dim line under `applies to`: WHO that is, by name.
static std::string rangeScopeName(const ImageDoc* im, int scope) {
    if (!im) return {};
    auto stackName = [](const ImageDoc* d) {
        if (d->seqId > 0) if (const App::SeqInfo* si = seqInfo(d->seqId)) if (!si->name.empty()) return si->name;
        return d->name;
    };
    if (scope == RS_All) {
        const size_t n = app.images.size();
        return std::to_string(n) + (n == 1 ? " open image" : " open images");
    }
    if (scope == RS_Compared) {
        const ViewSides v = comparedSides();
        std::string s;
        for (int i = 0; i < v.n; i++) s += (i ? " · " : "") + viewSideLetter(v, i) + " " + stackName(v.side[i]);
        return s;
    }
    if (scope == RS_Frame)
        return "frame " + std::to_string(im->seqIndex + 1) + " of " + stackName(im) + " — its own range";
    const size_t n = im->seqId > 0 ? framesOfSeq(im->seqId).size() : 1;
    return stackName(im) + "  (" + std::to_string(n) + (n == 1 ? " frame)" : " frames)");
}
// Frames of `im`'s stack, other than `im`, that keep their own range.
static int ownFramesElsewhere(const ImageDoc* im) {
    if (!im || im->seqId <= 0) return 0;
    int n = 0;
    for (const auto& d : app.images) if (d.get() != im && d->seqId == im->seqId && d->rangeOwn) n++;
    return n;
}
static std::string ownFramesLine(int n) {
    return n == 1 ? std::string("1 frame of this stack keeps its own range")
                  : std::to_string(n) + " frames of this stack keep their own range";
}
// [rejoin all]: every frame of `im`'s stack wears the stack's range again.
static void rejoinAllOwn(const ImageDoc* im) {
    if (!im || im->seqId <= 0) return;
    const ImageDoc* h = stackRangeHolder(im->seqId, nullptr);
    for (auto& d : app.images)
        if (d->seqId == im->seqId && d->rangeOwn) {
            d->rangeOwn = false;
            if (h) { d->black = h->black; d->white = h->white; }
            d->texDirty = true;
        }
}
// The `in force` row: "<lo> .. <hi>   <words>", the amber part apart.
struct InForce { std::string text, warn; bool amber = false; };
static InForce inForceOf(const ImageDoc& im) {
    InForce f;
    const ViewSides v = comparedSides();
    float lo = 0, hi = 0;
    displayRangeFor(im, v, lo, hi);
    const RangeWords w = rangeWords(im, v);
    f.text = fmtVal(lo, im.dtype) + " .. " + fmtVal(hi, im.dtype) + "   " + w.said;
    for (const std::string& h : w.held) f.text += "; " + h;
    f.warn = w.warn;
    return f;
}
// What a fit button reads under the rule in force (#321 §3.2): under "each
// frame / each side fits itself" the three fits are the CHOICE of fit, and the
// chosen one is lit; under the union Auto is lit as a fact (the union is
// min-max over every side) and the other two are not offered; under the rest
// they are lit by comparison, as before (#248). The fixed presets are dark
// under a fitting rule - pressing one moves the rule to one range (I2).
static bool rangeEachRule(const ImageDoc* im) {
    const int s = rangeScopeOf(im);
    return (s == RS_Stack && app.rangeRule == RR_EachFrame) || (s == RS_Compared && app.compareRangeMode == 3);
}
static bool rangeUnionRule(const ImageDoc* im) {
    return rangeScopeOf(im) == RS_Compared && app.compareRangeMode == 2;
}
static bool rangeButtonEnabled(const ImageDoc& im, int btn) {
    return !(rangeUnionRule(&im) && (btn == RB_AutoPct || btn == RB_Median));
}
static bool rangeButtonLit(ImageDoc& im, int btn, float eb, float ew) {
    const bool fit = btn == RB_Auto || btn == RB_AutoPct || btn == RB_Median;
    if (rangeEachRule(&im))
        return fit && btn == (app.autoFlavor == AF_Pct ? RB_AutoPct : app.autoFlavor == AF_Median ? RB_Median : RB_Auto);
    if (rangeUnionRule(&im)) return btn == RB_Auto;
    return rangeButtonMatches(im, btn, eb, ew);
}
// The words a Range control's tooltip ends on: who a press reaches.
static std::string rangeScopeTail(const ImageDoc* im) {
    switch (rangeScopeOf(im)) {
        case RS_Frame:    return "this frame only";
        case RS_Compared: return "A's stack (A's range for every side)";
        case RS_All:      return "all open images";
        default:          return im && im->seqId > 0 ? "the stack" : "this image";
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
