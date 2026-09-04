// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---------------------------------------------------------------- export
// Getting a picture OUT of the tool and into a report. Two things are wanted
// and they are not the same thing:
//   dot by dot  - one image pixel per output pixel, no resampling. What an
//                 image-quality reader needs: the noise is the subject, and a
//                 scaled screenshot invents and destroys exactly that.
//   as displayed - the canvas as it sits, zoom and overlays included. Not here
//                 yet (see docs/background/project/todo-open.md): it needs a framebuffer readback
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

// The ONE place an export is produced, so the file and the clipboard can never
// disagree about what the picture is.
static bool exportDocRGBA(ImageDoc* im, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!exportRefusalFor(im).empty()) return false;
    w = im->w; h = im->h;
    renderDocRGBA(*im, rgba);
    return true;
}

static void copyViewDotByDot() {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast(exportRefusalFor(im), true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    const std::string hlNote = exportHighlightNote(im);
    if (!hlNote.empty()) fprintf(stderr, "export: %s\n", hlNote.c_str());
#ifdef _WIN32
    std::string err;
    if (clipboardPutImage(rgba, w, h, png, err))
        toast("copied " + std::to_string(w) + "x" + std::to_string(h) + " (dot by dot)" +
              (hlNote.empty() ? "" : "  -  " + hlNote));
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
    toast("wrote " + path + " (path on the clipboard)" +
          (hlNote.empty() ? "" : "  -  " + hlNote));
#endif
}

// `uid` is the document the SAVE DIALOG was opened for, not whatever is current
// when it closes. 0 means "the current one" (the CLI and the selftests, which
// have no dialog between the choice and the write).
static void saveViewPng(const std::string& path, uint64_t uid = 0) {
    ImageDoc* im = nullptr;
    if (uid) {
        for (const auto& d : app.images) if (d->uid == uid) { im = d.get(); break; }
        if (!im) {
            toast("the image this was going to save has been closed", true);
            return;
        }
    } else im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast(exportRefusalFor(im), true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    if (png.empty()) { toast("PNG encode failed", true); return; }
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write " + path, true); return; }
    f.write((const char*)png.data(), (std::streamsize)png.size());
    const std::string hlNote = exportHighlightNote(im);
    if (!hlNote.empty()) fprintf(stderr, "export: %s\n", hlNote.c_str());
    toast("saved " + baseName(path) + "  " + std::to_string(w) + "x" + std::to_string(h) +
          (hlNote.empty() ? "" : "  -  " + hlNote));
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
// "Per stack" is a promise that has to be KEPT, not merely made once (#248).
// The frames of one stack sharing a reference range was implemented only where
// frames LAND (sequence.inc copies the reference frame's range onto each
// arriving frame); nothing afterwards held them together, so every later write
// - and "Auto per frame" stepping, which rewrites each frame's stored
// black/white as you go - broke the sharing silently. This is the one move
// that restores it: whatever `from` shows now, the rest of its stack shows.
// A standalone image (seqId == 0) is not a stack and shares with nobody.
static void shareStackRange(const ImageDoc& from) {
    if (from.seqId == 0) return;
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
static void applyRangeMode(int mode, ImageDoc* on) {
    bool wasLinked = app.linkRange;
    app.linkRange = mode == 2;
    app.rangeScope = mode == 0 ? 0 : 1;
    if (on) {
        if (app.linkRange && !wasLinked) {     // seed from what is on screen
            app.linkBlack = on->black; app.linkWhite = on->white;
        }
        // min..max, not defaultRange: same reason as in selectImage (#245).
        if (mode == 0 && !app.linkRange) { autoRange(*on); on->texDirty = true; }
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
enum RangeButton { RB_Auto = 0, RB_AutoPct, RB_0_1, RB_0_255, RB_0_65535 };

static bool rangeButtonMatches(ImageDoc& im, int btn, float eb, float ew) {
    switch (btn) {
        case RB_0_1:     return eb == 0.0f && ew == 1.0f;
        case RB_0_255:   return eb == 0.0f && ew == 255.0f;
        case RB_0_65535: return eb == 0.0f && ew == 65535.0f;
        case RB_Auto: {
            // the button's own arithmetic, repeated: linked Auto fits the
            // min/max over every open image, and those are cached numbers -
            // no pixel is read here.
            float lo = im.vmin, hi = im.vmax;
            if (app.linkRange)
                for (const auto& d : app.images) { lo = std::min(lo, d->vmin); hi = std::max(hi, d->vmax); }
            return eb == lo && ew == hi;
        }
        case RB_AutoPct: {
            // Linked "Auto %" pools EVERY open pixel onto one histogram
            // (autoPercentileFit says so in as many words). That is a second's
            // work on a 200-frame stack and this question is asked once per UI
            // frame, so the linked row simply does not light this button: the
            // fixed presets and Auto still answer, and no answer is a smaller
            // wrong than a stall. Unlinked, the answer is per-image and cached.
            if (app.linkRange) return false;
            // A quantile of these pixels cannot fall outside their own
            // min..max, so a range wider than the image - which is the normal
            // case in per-stack mode, where the shared range comes from a
            // sibling - is refused before the first pass is ever paid for.
            if (eb < im.vmin || ew > im.vmax) return false;
            if (!im.pctValid) {
                // percentileRange leaves lo/hi usable (min..max) even when it
                // returns false, and that fallback is exactly what the button
                // would have set - so the return value is not consulted here
                // any more than autoPercentileFit consults it.
                percentileRange(im, 0.001f, 0.999f, im.pctLo, im.pctHi);
                im.pctValid = true;
            }
            return eb == im.pctLo && ew == im.pctHi;
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
