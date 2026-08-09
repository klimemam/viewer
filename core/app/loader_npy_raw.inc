// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- npy loader
// returns error string, empty = ok
// Pure decoder: no app state, no GL — safe to call from the sequence loader thread.
// framesOut/frameStrideOut report a frame axis (F,H,W[,C]); frameIdx selects one.
static std::unique_ptr<ImageDoc> decodeNpyBuffer(const std::vector<uint8_t>& buf,
                                                 const std::string& path,
                                                 const std::string& displayName,
                                                 std::string& errOut, int frameIdx,
                                                 int& framesOut, int64_t& frameStrideOut,
                                                 int npyRead) {
    auto fail = [&](const char* m) { errOut = m; return std::unique_ptr<ImageDoc>(); };
    framesOut = 1;
    frameStrideOut = 0;
    // Header parsing lives in npyPeekHeader, which the .npz classifier also
    // calls, so both read the SHAPE the same way. That is as far as the promise
    // goes: the scan also checks the declared payload against the size the zip
    // directory reports, but neither one verifies the bytes are decodable, so a
    // corrupt member can still be offered and then fail - and when it fails
    // part-way through a stack, loadNpyBuffer says n of N rather than pretending.
    NpyHead Hd;
    if (!npyPeekHeader(buf, Hd, errOut)) return {};
    const std::string& descr = Hd.descr;
    const std::string& code = Hd.code;
    const std::string& dtypeName = Hd.dtypeName;
    const bool fortran = Hd.fortran, be = Hd.be;
    const int esize = Hd.esize;
    if (esize == 0) { errOut = "unsupported dtype: " + descr; return {}; }
    // The shape the FILE declared, unedited. A 0-D scalar used to be padded to
    // (1,) here and then read as a 1x1 picture; §3.1 refuses it, like every
    // other rank that is not 2, 3 or 4, and npyLayout is the one place that says so.
    const std::vector<int64_t> shape = Hd.shape;

    size_t count = 1;
    for (int64_t d : shape) count *= (size_t)d;
    if (Hd.dataOff + count * esize > buf.size()) return fail("file too small for shape");
    // buf.data() + off, never &buf[off]. A shape carrying a zero-length axis -
    // (0,8,8), which numpy writes and this loader is handed - makes count 0, so
    // the check above PASSES with dataOff == buf.size(), and &buf[buf.size()] is
    // undefined behaviour. Nothing showed for it: getVal is never called at
    // count 0 and npyLayout refuses the shape a few lines below, so the only
    // consequence was a pointer nobody used. A hardened libstdc++
    // (-D_GLIBCXX_ASSERTIONS) aborts on the subscript itself, before any of
    // that reasoning applies - which is the point, because the reasoning is
    // about today's callers and the subscript is about the language. data() +
    // size() is the one spelling that is legal one past the end.
    const uint8_t* raw = buf.data() + Hd.dataOff;

    auto bswap = [&](uint64_t v, int n) -> uint64_t {
        uint64_t r = 0;
        for (int i = 0; i < n; i++) r = (r << 8) | ((v >> (8 * i)) & 0xff);
        return r;
    };
    auto getVal = [&](size_t i) -> float {
        const uint8_t* p = raw + i * esize;
        switch (esize) {
        case 1:
            if (code == "i1") return (float)*(int8_t*)p;
            return (float)*p;
        case 2: {
            uint16_t u; memcpy(&u, p, 2);
            if (be) u = (uint16_t)bswap(u, 2);
            return code == "i2" ? (float)(int16_t)u : (float)u;
        }
        case 4: {
            uint32_t u; memcpy(&u, p, 4);
            if (be) u = (uint32_t)bswap(u, 4);
            if (code == "f4") { float f; memcpy(&f, &u, 4); return f; }
            return code == "i4" ? (float)(int32_t)u : (float)u;
        }
        case 8: {
            uint64_t u; memcpy(&u, p, 8);
            if (be) u = bswap(u, 8);
            double d; memcpy(&d, &u, 8); return (float)d;
        }
        }
        return 0;
    };

    // strides in elements
    std::vector<int64_t> strides(shape.size());
    if (fortran) { int64_t s = 1; for (size_t i = 0; i < shape.size(); i++) { strides[i] = s; s *= shape[i]; } }
    else         { int64_t s = 1; for (int i = (int)shape.size() - 1; i >= 0; i--) { strides[i] = s; s *= shape[i]; } }

    // Layout decision, in npyLayout so the .npz picker predicts with the same
    // rule (§3.1): rank and the LAST axis decide, the leading axis is never
    // consulted, and every other rank is refused by name. npyRead carries the
    // user's re-read declaration when there is one (§3.3) - it is threaded in
    // rather than read from app state so the sequence loader thread and the
    // Inspector reach the same answer for the same file.
    std::string note, lerr;
    int64_t F = 1, sf = 0, H, W, C, sh, sw, sc;
    if (!npyLayout(shape, strides, npyRead, F, sf, H, W, C, sh, sw, sc, note, lerr)) {
        errOut = lerr;
        return {};
    }
    if (F > 1) {                        // multi-frame: caller turns this into a stack
        framesOut = (int)F;
        frameStrideOut = sf;
    }

    auto im = std::make_unique<ImageDoc>();
    im->name = displayName.empty() ? baseName(path) : displayName;
    FrameSource& S = *im->src;
    S.path = path;
    S.fileFrame = frameIdx;           // which frame of the FILE: registry identity (§6.2) and reload provenance
    S.w = (int)W; S.h = (int)H; S.ch = (int)C;
    S.dtype = dtypeName;
    S.npyShape = shape;                 // what the file said, for §3.3's menu
    S.npyRead = npyRead;                // and how it was read, for the session
    im->note = note;
    S.data.resize((size_t)W * H * C);
    size_t di = 0;
    for (int64_t y = 0; y < H; y++)
        for (int64_t x = 0; x < W; x++)
            for (int64_t c = 0; c < C; c++)
                S.data[di++] = getVal((size_t)(frameIdx * sf + y * sh + x * sw + c * sc));
    im->syncMirrors();
    return im;
}

static std::unique_ptr<ImageDoc> decodeNpyFrame(const std::string& path, std::string& errOut,
                                                int frameIdx, int& framesOut,
                                                int64_t& frameStrideOut, int npyRead) {
    // identity BEFORE the bytes: an overwrite completing during the read binds
    // the OLD tuple to the OLD pixels - the safe direction, the next open of
    // the NEW tuple misses the registry and re-decodes what is on disk
    FrameSource st;
    st.path = path;
    statSourceFile(st);
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) { errOut = "cannot read file"; return {}; }
    auto im = decodeNpyBuffer(buf, path, "", errOut, frameIdx, framesOut, frameStrideOut, npyRead);
    if (im) {                              // a local file: the Watch baseline
        im->src->mtime = st.mtime;
        im->src->fsize = st.fsize;
    }
    return im;
}
static std::unique_ptr<ImageDoc> decodeNpy(const std::string& path, std::string& errOut) {
    int frames = 1; int64_t fstride = 0;
    return decodeNpyFrame(path, errOut, 0, frames, fstride);
}

// ---------------------------------------------------------------- raw loader
// Two orthogonal axes:
//   dtype  = how ONE sample is stored in the file (u8/u16/f32/f64 + endian)
//   interp = what the samples MEAN (gray / RGB / BGR / RGBA / BGRA / Bayer / Quad Bayer)
// Any combination is valid (e.g. RGGB float32).
enum RawDtype  { RD_U8, RD_U16, RD_F32, RD_F64, RD_COUNT };
static const char* RAW_DTYPE_NAMES[] = { "u8", "u16", "f32", "f64" };
static const int   RAW_DTYPE_SIZE[]  = { 1, 2, 4, 8 };
enum RawInterp { RI_GRAY, RI_RGB, RI_BGR, RI_RGBA, RI_BGRA, RI_BAYER, RI_QUAD, RI_COUNT };
static const char* RAW_INTERP_NAMES[] =
    { "Gray (1ch)", "RGB (3ch)", "BGR (3ch)", "RGBA (4ch)", "BGRA (4ch)",
      "Bayer (1ch CFA)", "Quad Bayer (1ch CFA)" };
static const char* RAW_INTERP_CLI[] = { "gray", "rgb", "bgr", "rgba", "bgra", "bayer", "quad-bayer" };
static const int   RAW_INTERP_CH[]  = { 1, 3, 3, 4, 4, 1, 1 };

// legacy combined names (--raw-format, session v1 "raw" lines) -> (dtype, interp)
static const int LEGACY_RAW_MAP[][2] = {
    { RD_U8,  RI_GRAY }, { RD_U16, RI_GRAY }, { RD_F32, RI_GRAY }, { RD_F64, RI_GRAY },
    { RD_U8,  RI_RGB  }, { RD_U8,  RI_BGR  }, { RD_U8,  RI_RGBA }, { RD_U8,  RI_BGRA },
    { RD_U16, RI_RGB  }, { RD_F32, RI_RGB  }, { RD_U8,  RI_BAYER }, { RD_U16, RI_BAYER },
};
static const char* LEGACY_RAW_NAMES[] = { "gray8", "gray16", "grayf32", "grayf64", "rgb8", "bgr8",
                                          "rgba8", "bgra8", "rgb16", "rgbf32", "bayer8", "bayer16" };

struct RawDialog {
    bool open = false;
    bool forQueue = false;            // settings will be applied to every queued stack
    int queueCount = 0;               // stacks waiting on these settings (for the prompt)
    std::string path;
    size_t fileSize = 0;
    int w = 1920, h = 1080, offset = 0;
    int dtype = RD_U8, interp = RI_GRAY;
    bool littleEndian = true;
    int cfaPattern = 0;               // RGGB/BGGR/GRBG/GBRG
    int replaceIdx = -1;              // >=0: reload INTO this image slot (reinterpret)
    bool cropOn = false;              // decode only a window of the source frame
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    std::vector<std::pair<int,int>> guesses;
} rawDlg;
// raw settings captured once and reused for every stack queued by "Open Folder"
static RawDialog g_folderRecipe;
// published by drawRawModal each frame: ImGui::IsPopupOpen(name) needs a live
// frame, and --browse-keys-selftest's action list runs between frames
static bool g_rawPopupAlive = false;
static void openRawDialogFor(const std::string& path);

static void rawGuessDims(RawDialog& d) {
    d.guesses.clear();
    size_t bpp = (size_t)RAW_DTYPE_SIZE[d.dtype] * RAW_INTERP_CH[d.interp];
    size_t nbytes = d.fileSize > (size_t)d.offset ? d.fileSize - d.offset : 0;
    if (!bpp || nbytes % bpp != 0) return;   // no exact pixel count -> no candidates
    int64_t n = (int64_t)(nbytes / bpp);
    static const int commons[][2] = { {3840,2160},{1920,1080},{1280,720},{640,480},{512,512},{1024,1024},
        {2048,2048},{4096,4096},{256,256},{2560,1440},{720,480},{640,360},{320,240},{128,128} };
    for (auto& c : commons)
        if ((int64_t)c[0] * c[1] == n)
            d.guesses.push_back({ c[0], c[1] });
    for (int64_t w = 16; w <= 8192 && (int)d.guesses.size() < 30; w++) {
        if (n % w == 0) {
            int64_t h = n / w;
            if (h >= 16 && h <= 8192 && w <= h * 8 && h <= w * 8) {
                bool dup = false;
                for (auto& g : d.guesses) if (g.first == w && g.second == h) dup = true;
                if (!dup) d.guesses.push_back({ (int)w, (int)h });
            }
        }
    }
}

// Pure decoder: no app state, no GL — safe on the sequence loader thread.
static std::unique_ptr<ImageDoc> decodeRawFrame(const RawDialog& d, std::string& errOut) {
    auto fail = [&](const char* m) { errOut = m; return std::unique_ptr<ImageDoc>(); };
    if (d.dtype < 0 || d.dtype >= RD_COUNT || d.interp < 0 || d.interp >= RI_COUNT)
        return fail("invalid raw format");
    if (d.w < 1 || d.h < 1 || d.w > 32768 || d.h > 32768)
        return fail("unsupported image size");   // sessions can carry unclamped values
    // identity BEFORE the bytes: see decodeNpyFrame
    FrameSource st;
    st.path = d.path;
    statSourceFile(st);
    std::vector<uint8_t> buf;
    if (!readFileBytes(d.path, buf)) return fail("cannot read file");
    const int elem = RAW_DTYPE_SIZE[d.dtype];
    const int ch = RAW_INTERP_CH[d.interp];
    size_t px = (size_t)d.w * d.h;
    size_t count = px * ch;
    if (count * elem + d.offset > buf.size()) return fail("file too small for this size/format");
    const uint8_t* p = buf.data() + d.offset;
    bool le = d.littleEndian;

    auto rd = [&](size_t i) -> float {   // one sample, any dtype/endian -> float
        const uint8_t* q = p + i * elem;
        switch (d.dtype) {
        case RD_U8:  return q[0];
        case RD_U16: return le ? (float)(q[0] | q[1] << 8) : (float)(q[0] << 8 | q[1]);
        case RD_F32: {
            uint32_t u = le ? (uint32_t)q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24
                            : (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
            float f; memcpy(&f, &u, 4); return f;
        }
        case RD_F64: {
            uint64_t u = 0;
            for (int b = 0; b < 8; b++) u |= (uint64_t)q[le ? b : 7 - b] << (8 * b);
            double v; memcpy(&v, &u, 8); return (float)v;
        }
        }
        return 0;
    };

    // optional crop window (decode only the window; source layout is full-frame)
    int cx = 0, cy = 0, outW = d.w, outH = d.h;
    if (d.cropOn && d.cropW > 0 && d.cropH > 0) {
        cx = std::clamp(d.cropX, 0, d.w - 1);
        cy = std::clamp(d.cropY, 0, d.h - 1);
        if (d.interp == RI_BAYER) { cx &= ~1; cy &= ~1; }        // keep CFA phase
        if (d.interp == RI_QUAD)  { cx &= ~3; cy &= ~3; }
        outW = std::clamp(d.cropW, 1, d.w - cx);
        outH = std::clamp(d.cropH, 1, d.h - cy);
    }

    auto im = std::make_unique<ImageDoc>();
    im->name = baseName(d.path);
    FrameSource& S = *im->src;
    S.path = d.path;
    S.w = outW; S.h = outH; S.ch = ch;
    S.dtype = RAW_DTYPE_NAMES[d.dtype];
    S.data.resize((size_t)outW * outH * ch);
    float* out = S.data.data();
    bool sw = d.interp == RI_BGR || d.interp == RI_BGRA;   // channel 0/2 swap
    for (int y = 0; y < outH; y++)
        for (int x = 0; x < outW; x++) {
            size_t src = ((size_t)(cy + y) * d.w + (cx + x)) * ch;
            size_t dst = ((size_t)y * outW + x) * ch;
            for (int c = 0; c < ch; c++) {
                int oc = (sw && c == 0) ? 2 : (sw && c == 2) ? 0 : c;
                out[dst + oc] = rd(src + c);
            }
        }
    S.srcW = d.w; S.srcH = d.h;
    S.cropX = cx; S.cropY = cy;
    im->note = std::string(RAW_INTERP_NAMES[d.interp]) + " " + RAW_DTYPE_NAMES[d.dtype];
    if (d.interp == RI_BAYER || d.interp == RI_QUAD) {
        im->cfa = d.interp == RI_QUAD ? 2 : 1;
        im->cfaPattern = d.cfaPattern & 3;
        im->note = std::string(d.interp == RI_QUAD ? "Quad Bayer " : "Bayer ")
                 + CFA_PATTERNS[im->cfaPattern] + " " + RAW_DTYPE_NAMES[d.dtype];
    }
    S.rawDtype = d.dtype;             // remember raw params: sessions + reinterpret
    S.rawInterp = d.interp;
    S.rawOffset = d.offset;
    S.rawLE = d.littleEndian;
    S.mtime = st.mtime;               // the PRE-read Watch baseline: identity
    S.fsize = st.fsize;               // binds the bytes that were actually read
    im->syncMirrors();
    return im;
}

static std::string loadRaw(const RawDialog& d) {
    std::string err;
    auto im = decodeRawFrame(d, err);
    if (!im) return err.empty() ? "decode failed" : err;
    computeMinMax(*im);               // measure BEFORE publishing: no source in
                                      // the registry ever shows the default 0/1
    shareOrRegisterSource(*im);       // §6.2: same file + same recipe -> share
                                      // (adoption = an already-measured resident;
                                      // a shared source is never remeasured here)

    if (d.replaceIdx >= 0 && d.replaceIdx < (int)app.images.size()) {
        // reinterpret in place: keep list position, selection and view
        ImageDoc* old = app.images[d.replaceIdx].get();
        if (app.ana.img == old) app.ana.img = nullptr;
        if (old->tex) glDeleteTextures(1, &old->tex);
        defaultRange(*im);
        im->texDirty = true;
        forgetImage(old);
        im->uid = app.nextUid++;
        app.images[d.replaceIdx] = std::move(im);
        app.current = d.replaceIdx;
    } else {
        addImage(std::move(im), true);
    }
    return {};
}

// ---------------------------------------------------------------- dynamic crop
// Crop the in-memory data (no file IO); origin snaps to the CFA period so the
// pattern stays valid. appliedX/Y report the snapped origin.
// Returns false when the crop is REFUSED (nothing changed). A decimated remote
// frame (remoteStep > 1) refuses: the full-resolution fetch is still in flight
// and its landing replaces data/w/h wholesale, knowing nothing of crop
// bookkeeping - a crop recorded against the decimated grid would be silently
// reverted by the swap and leave srcW stamped with preview dimensions: a
// phantom crop (isCropped forever true), an unshareable source, and a session
// line carrying garbage rectangles. Crop after the full frame lands.
static bool cropInPlace(ImageDoc& im, int x, int y, int w, int h,
                        int* appliedX = nullptr, int* appliedY = nullptr) {
    if (im.src->remoteStep > 1) return false;
    // CoW (§2.2): a crop re-scopes THIS stack's measurement subject, so pixels
    // shared with another stack must not change under it. Under g_srcRegMtx for
    // the whole mutation: the registry hands sources to the sequence loader
    // thread under that lock, so a probe either adopts BEFORE this (then
    // use_count > 1 forces the clone) or looks up after and is refused
    // (srcShareable sees the crop) - it can never adopt pixels mid-crop.
    std::lock_guard<std::mutex> regLk(g_srcRegMtx);
    if (im.src.use_count() > 1) {
        im.src = cloneSource(*im.src);
        app.imagesRev++;              // the Files share marks change with the split
    } else {
        // sole owner, mutated IN PLACE: this source stops being the full frame,
        // so it leaves the registry NOW - a live occupant that fails
        // srcShareable would otherwise squat the tuple's slot, and the same
        // file could neither be adopted nor re-registered until this doc
        // closed. Erased by regKey - the key the slot was CREATED under -
        // which also keeps filesystem IO (key recomputation) out of this lock.
        if (!im.src->regKey.empty()) {
            auto it = g_srcRegistry.find(im.src->regKey);
            if (it != g_srcRegistry.end() && it->second.lock() == im.src)
                g_srcRegistry.erase(it);
            im.src->regKey.clear();
        }
    }
    FrameSource& S = *im.src;
    if (S.srcW == 0) { S.srcW = S.w; S.srcH = S.h; }
    x = std::clamp(x, 0, im.w - 1);
    y = std::clamp(y, 0, im.h - 1);
    if (im.cfa == 1) { x &= ~1; y &= ~1; }
    if (im.cfa == 2) { x &= ~3; y &= ~3; }
    w = std::clamp(w, 1, im.w - x);
    h = std::clamp(h, 1, im.h - y);
    std::vector<float> nd((size_t)w * h * im.ch);
    for (int yy = 0; yy < h; yy++)
        memcpy(&nd[(size_t)yy * w * im.ch],
               &S.data[((size_t)(y + yy) * im.w + x) * im.ch],
               (size_t)w * im.ch * sizeof(float));
    S.data = std::move(nd);
    S.w = w; S.h = h;
    S.cropX += x; S.cropY += y;
    im.syncMirrors();
    im.dataRev++;
    computeMinMax(im);
    im.texDirty = true;
    if (appliedX) *appliedX = x;
    if (appliedY) *appliedY = y;
    return true;
}

static void shiftAnnotations(int dx, int dy) {
    if (app.anns.empty() || (dx == 0 && dy == 0)) return;
    for (auto& a : app.anns) { a.x += dx; a.y += dy; }
    app.annRev++;
}

static bool isCropped(const ImageDoc& im) {
    return im.src->srcW > 0 && (im.w != im.src->srcW || im.h != im.src->srcH ||
                                im.src->cropX != 0 || im.src->cropY != 0);
}

static void cropCurrentToSelectedRoi() {
    ImageDoc* im = cur();
    App::Ann* sel = findAnn(app.selectedAnn);
    if (!im || !sel || sel->type != 0) return;
    int ax = 0, ay = 0;
    if (!cropInPlace(*im, sel->x, sel->y, sel->w, sel->h, &ax, &ay)) {
        toast("still fetching the full frame - crop after it lands", true);
        return;
    }
    shiftAnnotations(-ax, -ay);        // annotations follow into the cropped frame
    if (app.ana.img == im) app.ana.img = nullptr;
    app.fitRequested = true;
    toast("cropped to " + sel->label);
}

static void restoreFull() {
    ImageDoc* im = cur();
    if (!im || !isCropped(*im)) return;
    int sx = im->src->cropX, sy = im->src->cropY;
    int idx = app.current;
    float ob = im->black, ow = im->white;
    if (im->src->rawDtype >= 0) {
        RawDialog d;
        d.path = im->src->path;
        d.dtype = im->src->rawDtype;
        d.interp = im->src->rawInterp;
        if (RAW_INTERP_CH[d.interp] == 1)
            d.interp = im->cfa == 2 ? RI_QUAD : im->cfa == 1 ? RI_BAYER : RI_GRAY;
        d.w = im->src->srcW; d.h = im->src->srcH;
        d.offset = im->src->rawOffset; d.littleEndian = im->src->rawLE;
        d.cfaPattern = im->cfaPattern & 3;
        d.replaceIdx = idx;
        std::string err = loadRaw(d);
        if (!err.empty()) { toast("restore failed: " + err, true); return; }
    } else if (!im->src->path.empty()) {
        int before = (int)app.images.size();
        // the reading is part of what "the same file" means: restoring the full
        // frame must not quietly hand back the native reading of a file the user
        // has already said is read some other way (§3.3)
        std::string err = imagefile::forPath(im->src->path)
                        ? loadImageFile(im->src->path)
                        : loadNpy(im->src->path, im->src->npyRead);
        if (!err.empty() || (int)app.images.size() == before) {
            toast("restore failed: " + (err.empty() ? std::string("reload error") : err), true);
            return;
        }
        auto doc = std::move(app.images.back());   // loadNpy appended; move into old slot
        app.images.pop_back();
        ImageDoc* old = app.images[idx].get();
        doc->cfa = old->cfa; doc->cfaPattern = old->cfaPattern;
        doc->cfaColorize = old->cfaColorize;
        doc->texDirty = true;
        if (app.ana.img == old) app.ana.img = nullptr;
        if (old->tex) glDeleteTextures(1, &old->tex);
        forgetImage(old);
        doc->uid = app.nextUid++;
        app.images[idx] = std::move(doc);
        app.current = idx;
    } else {
        toast("no source file to restore from (processed image)", true);
        return;
    }
    cur()->black = ob; cur()->white = ow; cur()->texDirty = true;   // keep user range
    shiftAnnotations(sx, sy);
    app.fitRequested = true;
    toast("restored full frame");
}
