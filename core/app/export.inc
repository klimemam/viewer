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
// - and "Auto per frame" stepping, which rewrites each frame's stored
// black/white as you go - broke the sharing silently. This is the one move
// that restores it: whatever `from` shows now, the rest of its stack shows.
// A standalone image is not a stack and shares with nobody: state.h spells the
// convention "0 = standalone, >0 = frame of that sequence", so ONLY a positive
// seqId names a stack. Written as `<= 0` rather than `== 0` because this
// decides who gets written to - a doc that somehow carried a negative id would
// otherwise be fused with every other one, and that is not a mistake worth
// leaving room for.
static void shareStackRange(const ImageDoc& from) {
    if (from.seqId <= 0) return;
    // ...and the stack REMEMBERS whose fit it is wearing (#254). The Range row
    // answers by comparison, so on every frame but this one the shared numbers
    // no longer match that frame's own min/max and the lit button went dark on
    // the first step - the range was still shared, the display was not.
    if (App::SeqInfo* si = seqInfo(from.seqId)) si->rangeRefUid = from.uid;
    for (auto& d : app.images)
        if (d.get() != &from && d->seqId == from.seqId) {
            d->black = from.black; d->white = from.white;
            d->texDirty = true;
        }
}
// The Range panel's one mode control (0 auto per frame / 1 per stack /
// 2 linked). It lives here, not inside the combo, because choosing a mode has
// to act on the frame ALREADY on screen: the per-frame policy is otherwise
// applied only by selectImage, on a frame change, and the mode read as dead
// until the first step (report 2026-08-03).
// `flavor` is what "Auto per frame" means from now on (AutoFlavor, #252), or
// -1 for "leave it as it is" - which is what the two modes that are not per
// frame pass, and what every caller that predates the flavours means. The
// combo's five entries are (mode, flavor) pairs; nothing else about the
// contract moved.
static void applyRangeMode(int mode, ImageDoc* on, int flavor = -1) {
    bool wasLinked = app.linkRange;
    if (flavor >= AF_MinMax && flavor <= AF_Median) app.autoFlavor = flavor;
    app.linkRange = mode == 2;
    app.rangeScope = mode == 0 ? 0 : 1;
    if (on) {
        if (app.linkRange && !wasLinked) {     // seed from what is on screen
            app.linkBlack = on->black; app.linkWhite = on->white;
        }
        // the frame's OWN fit, not defaultRange: same reason as in selectImage
        // (#245). WHICH fit is app.autoFlavor's answer, and refitByFlavor is
        // the one place that answers it (#252).
        if (mode == 0 && !app.linkRange) { refitByFlavor(*on); on->texDirty = true; }
        // ...and switching TO "per stack" has to RE-share, for the same reason
        // the mode has to act on the frame already on screen. Stepping in mode
        // 0 rewrote every frame's stored range to its own min..max (main.cpp
        // 1393 records that as known), so the stack arrives here holding
        // per-frame values and merely raising the scope flag changes nothing
        // you can see: the stored per-frame values simply reappear on the next
        // step, which is the report's 「Per stack にしても一枚毎に Auto」
        // (#248). The frame ON SCREEN is the reference - least surprise: the
        // picture in front of you is the one you meant to spread.
        if (mode == 1) shareStackRange(*on);
    }
    if (app.linkRange != wasLinked) markAllTexDirty();
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
// The black-white `im` is drawn at in view `v`: Link range, then the compare
// range mode 0..3, then the document's own numbers - one decision, which the
// screen (effBlack/effWhite, displayMapOf) and every export reach.
static void displayRangeFor(const ImageDoc& im, const ViewSides& v, float& lo, float& hi) {
    if (app.linkRange) { lo = app.linkBlack; hi = app.linkWhite; return; }
    lo = im.black; hi = im.white;
    if (app.compareRangeMode == 0) return;
    ImageDoc* self = sideOfView(im, v);
    if (!self) return;
    if (app.compareRangeMode == 1) { lo = v.side[0]->black; hi = v.side[0]->white; return; }
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
// The words for the range `im` is exported at in view `v`, in the words of the
// controls that set it: the Range combo ("Auto per frame (min-max)", "Per stack
// (default)", "Linked across all images") and the A/B combo ("A/B: auto over
// every side (union)" ...). Read off the same conditions displayRangeFor
// decides by, never off the numbers: a union that happens to equal one side's
// own range is still a union. `video` adds "per frame" wherever the range is
// worked out again for every frame, and drops the numbers, which then differ
// frame by frame; a single picture carries its numbers. A side drawn in a role
// other than A says which ("drawn as B").
static std::string exportRangeNote(const ImageDoc& im, const ViewSides& v, bool video) {
    static const char* SCOPE_FLAVOR[3] = { "min-max", "0.1-99.9%", "median->target" };
    static const char* AB_FIT[3] = { "min-max", "0.1-99.9%", "median" };
    const int fl = std::clamp(app.autoFlavor, (int)AF_MinMax, (int)AF_Median);
    const bool perFrameScope = !app.linkRange && app.rangeScope == 0;
    const std::string scope = app.linkRange ? std::string("Linked across all images")
                            : perFrameScope ? std::string("Auto per frame (") + SCOPE_FLAVOR[fl] + ")"
                                            : std::string("Per stack (default)");
    int self = -1;
    if (v.n >= 2) for (int i = 0; i < v.n && self < 0; i++) if (v.side[i] == &im) self = i;
    std::string rule;
    if (app.linkRange) rule = scope;
    else if (self >= 0 && app.compareRangeMode == 1)
        rule = "A/B: every side uses A's range - A's is " + scope +
               (video && perFrameScope ? ", per frame" : "");
    else if (self >= 0 && app.compareRangeMode == 2)
        rule = std::string("A/B: auto over every side (union)") + (video ? ", per frame" : "");
    else if (self >= 0 && app.compareRangeMode == 3)
        rule = std::string("A/B: each side fits itself (") + AB_FIT[fl] + ")" +
               (video ? ", per frame" : "");
    else if (self >= 0)
        rule = "A/B: each keeps its own - " + scope;
    else rule = scope;
    if (self > 0) rule += " - drawn as " + viewSideLetter(v, self);
    std::string out = "range: " + rule;
    if (!video) {
        float lo = 0, hi = 0;
        displayRangeFor(im, v, lo, hi);
        char num[64];
        snprintf(num, sizeof num, " = %.6g..%.6g", lo, hi);
        out += num;
        for (const std::string& h : viewHeldSides(v)) out += "; " + h;
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
// (#304). `stepTo` is the frame A would have to be stepped onto to show it -
// what "Auto per frame" re-fits on the way - or null when A stays put.
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
    s.side[r] = d;
    s.diverged[r] = false;
    return s;
}
// WHICH fit "each side fits itself" draws with (#301), set from the radio row
// under the A/B combo and from the View > Compare A/B menu. It is the SAME
// variable "Auto per frame" re-fits by - one answer to "which fit", not one per
// feature - so under Auto per frame the frame on screen re-fits exactly as it
// does when the flavour is picked in the Range combo (applyRangeMode). Under
// Per stack that variable has no other control at all, and without this the
// mode could only ever show min-max. The per-side cache is keyed on the
// flavour, so every compared side re-fits on its next draw; the dirty marks
// only make the textures follow on this frame.
static void setEachSideFlavor(int fl) {
    if (fl < AF_MinMax || fl > AF_Median) return;
    if (!app.linkRange && app.rangeScope == 0 && cur()) applyRangeMode(0, cur(), fl);
    else app.autoFlavor = fl;
    const ViewSides v = comparedSides();
    for (int i = 0; i < v.n; i++) v.side[i]->texDirty = true;
}

static void setRange(ImageDoc& im, float black, float white) {
    if (!(white > black)) return;
    if (app.linkRange) {
        app.linkBlack = black; app.linkWhite = white;
        markAllTexDirty();            // shared value changed: every image re-renders
    } else {
        im.black = black; im.white = white; im.texDirty = true;
        // ONE path for the number field and all five buttons - they all arrive
        // here - so "Per stack" holds for every one of them (#248). Without
        // this, pressing Auto in per-stack mode fitted the frame on screen and
        // left its siblings on their old numbers, i.e. the mode read as a
        // load-time accident rather than a policy.
        if (app.rangeScope == 1) shareStackRange(im);
    }
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
    if (app.linkRange || app.rangeScope != 1 || im.seqId <= 0) return &im;
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
