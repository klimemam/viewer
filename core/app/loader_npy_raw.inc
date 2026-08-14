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
    // What this decode costs by being float32, measured on the way past
    // (rp::F32Loss). Only u4 / i4 / f8 can lose anything - u1/i1/b1/u2/i2/f4
    // are exact in float32 by construction - so only they pay the branch, and
    // the census `total` is then the count of samples that COULD have lost
    // rather than of every sample read, which is what the note wants to say.
    rp::F32Loss loss;
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
            // (double) is exact for every uint32_t and int32_t, so the census
            // is comparing against the file's own value and not a second
            // approximation of it.
            const double exact = code == "i4" ? (double)(int32_t)u : (double)u;
            loss.observe(exact);
            return (float)exact;
        }
        case 8: {
            uint64_t u; memcpy(&u, p, 8);
            if (be) u = bswap(u, 8);
            double d; memcpy(&d, &u, 8);
            loss.observe(d);
            return (float)d;
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
    // ...and what that cost, recorded on the SOURCE rather than appended to the
    // note. Two reasons, and both are about it being a fact per array:
    //   * the note is stack-constant by construction here (it comes from the
    //     SHAPE) and open_dispatch.inc requires it to stay that way, while this
    //     census is genuinely per frame - frame 3 of a u32 stack can hold a
    //     value frame 2 does not;
    //   * the remote door cannot use the note at all for the same reason, and a
    //     fact that is worded one way locally and another over a link is the
    //     shape of issue #71.
    // So it rides on FrameSource - shared with the registry, copied by
    // cloneSource, and printed by the Inspector from rp::f32LossNote, which is
    // the ONE wording all four narrowing sites answer with.
    S.f32loss = loss;
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
static void openRawDialogFor(const std::string& path, uint64_t knownSize = 0);

// The whole recipe, as the sentence that travels with the numbers it produced.
//
// docs/settings-inventory.md §4.1 M1: a measuring-class setting has to appear
// in its own results, and a headerless read is nothing BUT measuring-class
// settings - the file states none of it. The note used to say "Bayer RGGB u16"
// and stop, which named the two fields already visible on screen and left out
// the three that are dangerous precisely because they are not:
//
//   the SIZE          4096x3000 and 3000x4096 are the same byte count. A
//                     transposed frame looks perfectly normal.
//   the HEADER offset  a wrong one shifts every pixel by a constant and the
//                     picture still looks like a picture.
//   the BYTE ORDER    swapped u16 is noise on some sensors and a plausible
//                     gradient on others.
//
// Each changes the numbers. None of them was anywhere a report could cite, so
// a figure taken off a .raw could not be checked against the read that made it.
// This is the precondition §10.7 names for putting `loading.raw` into
// settings.jsonc at all: defaults that do not declare themselves break M1 on
// the first day.
static std::string rawRecipeNote(const RawDialog& d, int cfa, int cfaPattern) {
    const int dt = std::clamp(d.dtype, 0, RD_COUNT - 1);
    const int in = std::clamp(d.interp, 0, RI_COUNT - 1);
    std::string s;
    if (in == RI_BAYER || in == RI_QUAD)
        s = std::string(cfa == 2 ? "Quad Bayer " : "Bayer ") + CFA_PATTERNS[cfaPattern & 3];
    else
        s = RAW_INTERP_NAMES[in];
    s += std::string(" ") + RAW_DTYPE_NAMES[dt];
    s += ", " + std::to_string(d.w) + "x" + std::to_string(d.h);
    // Written even when it is zero and even when it is the common one: "no
    // header" is a claim about the file, and a field that disappears when it
    // holds the ordinary value cannot be read as a claim at all - the reader
    // cannot tell it from a note written before the field existed.
    s += ", header " + std::to_string(d.offset) + " B";
    s += d.littleEndian ? ", little-endian" : ", big-endian";
    if (d.cropOn && d.cropW > 0 && d.cropH > 0)
        s += ", read window " + std::to_string(d.cropW) + "x" + std::to_string(d.cropH) +
             " at (" + std::to_string(d.cropX) + "," + std::to_string(d.cropY) + ")";
    // ...and what all of it IS. The fields above are a declaration, not a
    // measurement, and the difference is the reason this line exists.
    s += " - headerless: this shape was declared, not read from the file";
    return s;
}

// The same declaration, in the shape the wire carries (protocol 11). One
// converter, because every door that can open a headerless file over a link -
// the CLI here, the Browse dialog in stage 4, a session restore after it -
// has to put the SAME six numbers on the wire that the local decode used, or
// one file reads two ways depending on which end opened it.
//
// crop is deliberately absent: it re-scopes a frame rather than being one, and
// the local registry already refuses to share a cropped raw source
// (docs/remote-headerless-design.md §3.2). A crop asked for on the command line
// therefore applies to what came back, not to what was fetched.
static rp::RawWire rawWireFrom(const RawDialog& d) {
    rp::RawWire w{};
    w.dtype = (uint32_t)std::clamp(d.dtype, 0, RD_COUNT - 1);
    w.interp = (uint32_t)std::clamp(d.interp, 0, RI_COUNT - 1);
    w.w = (uint32_t)std::max(1, d.w);
    w.h = (uint32_t)std::max(1, d.h);
    w.offset = (uint32_t)std::max(0, d.offset);
    w.flags = d.littleEndian ? rp::RW_LITTLE_ENDIAN : 0u;
    return w;
}

// ---------------------------------------------------------------- raw recipes
// Issue #166. A NAMED RECIPE is exactly the fields the dialog above already
// carries, minus everything that is about ONE open (the path, the file size,
// the candidate list, which slot to reinterpret). It is deliberately NOT a
// second vocabulary for the same things: the words a recipe is written in are
// RAW_DTYPE_NAMES / RAW_INTERP_CLI / CFA_PATTERNS - the same three tables
// --raw-dtype, --raw-interp and --bayer-pattern already spell, and the same
// ones docs/settings-inventory.md §5.5 sketched for `loading.raw`.
//
// TWO THINGS LIVE HERE AND THEIR LIFETIMES ARE OPPOSITE. That split is the
// whole feature (the user's ruling, 2026-08-11):
//
//   THE LIBRARY       named recipes, written BY HAND in settings.jsonc.
//                     Persistent, human-editable, and it NEVER applies by
//                     itself - a recipe is used because somebody picked it.
//   THE SIZE BINDING  "the recipe this session was told to use for a file of
//                     exactly N bytes". PROCESS-ONLY: it is in neither
//                     settings.jsonc, prefs.txt nor the session file, and it
//                     ends with the process.
//
// Why the binding may not be written down anywhere. A headerless file is the
// one shape that does not defend itself: 4096x3000 and 3000x4096 are the SAME
// byte count, nothing in the bytes says which, and a transposed frame looks
// perfectly normal. So the only thing allowed to apply without being picked is
// a choice THE USER MADE MINUTES AGO AND STILL REMEMBERS - if it is wrong they
// can tell, because they remember making it. A binding that survived a restart
// would apply days later to a file it was never chosen for, draw a plausible
// picture, and give nobody anything to notice. Making it unwritable is what
// makes that impossible, rather than a rule someone has to keep.
struct RawRecipe {
    std::string name;                 // the library's name for it, or the derived label
    bool fromLibrary = false;         // true = named in settings.jsonc
    int w = 1920, h = 1080, offset = 0;
    int dtype = RD_U8, interp = RI_GRAY;
    bool littleEndian = true;
    int cfaPattern = 0;
    bool cropOn = false;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
};

// The library. Filled ONLY by core/app/settings.inc's reader, cleared by it on
// every load, and read by the panel. Nothing else writes it: a recipe exists
// because it is in the user's file.
static std::vector<RawRecipe> g_rawLibrary;

// The size binding, and the reason it is a plain process global with no writer
// anywhere near a file. std::map so the panel lists sizes in a stable order.
static std::map<size_t, RawRecipe> g_rawSizeRecipe;

static int rawClampIdx(int v, int n) { return v < 0 || v >= n ? 0 : v; }

// What a recipe IS, in one phrase, in the vocabulary of the three tables.
// Used as the panel's second column, as the derived name of an unnamed recipe,
// and inside the line that announces an applied one - one spelling, three
// places, so a recipe cannot be described two ways in one session.
static std::string rawRecipeSummary(const RawRecipe& r) {
    const int dt = rawClampIdx(r.dtype, RD_COUNT), in = rawClampIdx(r.interp, RI_COUNT);
    std::string s = std::to_string(r.w) + "x" + std::to_string(r.h) + " " + RAW_DTYPE_NAMES[dt];
    if (RAW_DTYPE_SIZE[dt] > 1 && !r.littleEndian) s += " BE";
    s += " ";
    if (in == RI_BAYER)      s += std::string("Bayer ") + CFA_PATTERNS[r.cfaPattern & 3];
    else if (in == RI_QUAD)  s += std::string("Quad Bayer ") + CFA_PATTERNS[r.cfaPattern & 3];
    else                     s += RAW_INTERP_CLI[in];
    if (r.offset > 0) s += " +" + std::to_string(r.offset) + "B";
    if (r.cropOn && r.cropW > 0 && r.cropH > 0)
        s += " crop " + std::to_string(r.cropX) + "," + std::to_string(r.cropY) + " " +
             std::to_string(r.cropW) + "x" + std::to_string(r.cropH);
    return s;
}

static void rawRecipeIntoDialog(const RawRecipe& r, RawDialog& d) {
    d.w = r.w; d.h = r.h; d.offset = r.offset;
    d.dtype = rawClampIdx(r.dtype, RD_COUNT);
    d.interp = rawClampIdx(r.interp, RI_COUNT);
    d.littleEndian = r.littleEndian;
    d.cfaPattern = r.cfaPattern & 3;
    d.cropOn = r.cropOn;
    d.cropX = r.cropX; d.cropY = r.cropY; d.cropW = r.cropW; d.cropH = r.cropH;
}

// Two recipes that would decode the same bytes the same way. The NAME is
// deliberately not part of it: this is what "the user is holding that recipe"
// means, and a recipe is what its fields are.
static bool rawRecipeSameFields(const RawRecipe& a, const RawRecipe& b) {
    return a.w == b.w && a.h == b.h && a.offset == b.offset &&
           rawClampIdx(a.dtype, RD_COUNT) == rawClampIdx(b.dtype, RD_COUNT) &&
           rawClampIdx(a.interp, RI_COUNT) == rawClampIdx(b.interp, RI_COUNT) &&
           a.littleEndian == b.littleEndian && (a.cfaPattern & 3) == (b.cfaPattern & 3) &&
           a.cropOn == b.cropOn &&
           (!a.cropOn || (a.cropX == b.cropX && a.cropY == b.cropY &&
                          a.cropW == b.cropW && a.cropH == b.cropH));
}

// The reverse, and the NAME is decided HERE, from the values, rather than
// remembered as UI state. If the fields the user is about to load with are
// field-for-field a library recipe, that recipe's name is what gets recorded;
// otherwise the recipe is anonymous and names itself after what it is.
// Deciding it from the values is what makes "picked MyCam, then nudged the
// height" stop being MyCam - which is what the applied line would otherwise
// claim, three files later, about a geometry MyCam does not have.
static RawRecipe rawRecipeFromDialog(const RawDialog& d) {
    RawRecipe r;
    r.w = d.w; r.h = d.h; r.offset = d.offset;
    r.dtype = d.dtype; r.interp = d.interp;
    r.littleEndian = d.littleEndian;
    r.cfaPattern = d.cfaPattern & 3;
    r.cropOn = d.cropOn;
    r.cropX = d.cropX; r.cropY = d.cropY; r.cropW = d.cropW; r.cropH = d.cropH;
    for (const RawRecipe& lib : g_rawLibrary)
        if (rawRecipeSameFields(r, lib)) { r.name = lib.name; r.fromLibrary = true; return r; }
    r.name = rawRecipeSummary(r);
    return r;
}

static const RawRecipe* rawRecipeForSize(size_t n) {
    auto it = g_rawSizeRecipe.find(n);
    return it == g_rawSizeRecipe.end() ? nullptr : &it->second;
}

// Last explicit choice wins: a size already answered for is REPLACED, never
// kept. The user's word for this is "読み方変えたかったらレシピをかえる", and
// an entry that refused to move would make that sentence false.
static void rawRecipeBindSize(size_t n, const RawRecipe& r) {
    if (n == 0) return;                // a zero-length file has no geometry to bind
    g_rawSizeRecipe[n] = r;
}

// The one line, and it is one line on purpose. Something applied that the user
// did not pick THIS TIME, so it says which recipe, what that recipe is, and
// why it was in a position to apply at all - the byte count that matched.
static std::string rawRecipeAppliedLine(const RawRecipe& r, const std::string& path, size_t sz) {
    const std::string sum = rawRecipeSummary(r);
    const std::string who = r.fromLibrary && r.name != sum ? "\"" + r.name + "\" (" + sum + ")" : sum;
    return baseName(path) + ": opened with " + who +
           " - this session's recipe for " + std::to_string(sz) + "-byte files";
}

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

    // Same census as the .npy door (rp::F32Loss): the raw dialog's f64 is the
    // same narrowing arriving through a different mouth, and a fact that is
    // stated in one door and silent in the other is worse than either.
    rp::F32Loss loss;
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
            double v; memcpy(&v, &u, 8);
            loss.observe(v);
            return (float)v;
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
    if (d.interp == RI_BAYER || d.interp == RI_QUAD) {
        im->cfa = d.interp == RI_QUAD ? 2 : 1;
        im->cfaPattern = d.cfaPattern & 3;
    }
    im->note = rawRecipeNote(d, im->cfa, im->cfaPattern);
    S.f32loss = loss;                 // the f64 raw dtype's census; see decodeNpyBuffer
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

// The RAW dialog's Load button, with no ImGui in it (#166). Two callers: the
// button in core/ui/canvas.inc and --rawrecipe-selftest, which drives THIS
// rather than a copy of it - the same reason npzPickAccept() exists.
//
// The BINDING IS MADE HERE and only on success. "This session's recipe for a
// file of N bytes" is a claim that a file of N bytes decodes with it, and a
// recipe that has never decoded anything has not earned that claim; recording
// a failed attempt would arm the next open of that size with settings already
// known not to work.
// Is this dialog about a file on a PEER? The path carries a url, which is the
// only thing that distinguishes the two landings - everything above this line
// (the guesses, the fields, the binding's key) is byte-count arithmetic and
// does not care where the bytes are.
static bool rawDialogIsRemote(const RawDialog& d) {
    return hasUrlScheme(d.path);
}

static std::string rawDialogAccept(RawDialog& d) {
    // ONE branch, at the landing. The dialog itself is not taught about peers:
    // rawGuessDims works from a byte count, and a byte count is a byte count on
    // either disk. A second dialog for remote files would be a second place for
    // the same six fields to be asked for, and the two would drift.
    std::string err;
    if (rawDialogIsRemote(d)) {
        const rp::RawWire rw = rawWireFrom(d);
        // openRemote SAYS what it did (toasts on refusal), so a bare false is
        // not silence - but this call site still needs a non-empty string to
        // mean "the binding does not happen", which is the rule below.
        err = openRemote(d.path, false, 0, 0, &rw) ? std::string()
                                                   : std::string("could not open " + d.path);
    } else {
        err = loadRaw(d);
    }
    // A SUCCESSFUL read is what binds the size, on either side. #166's rule
    // ("only a decode that worked makes a binding") is about the operator
    // having seen the result, and that is as true of a peer's pixels.
    if (err.empty()) rawRecipeBindSize(d.fileSize, rawRecipeFromDialog(d));
    return err;
}

// The recipe panel's "Apply": re-read the document that is already open with
// this recipe's geometry, into the same list slot. It is how a user changes
// their mind about a file they are looking at, and it is an EXPLICIT choice -
// so the size binding moves with it. Last word wins, which is the user's own
// wording of the rule ("読み方変えたかったらレシピをかえる").
static std::string rawRecipeReinterpretCurrent(const RawRecipe& r) {
    ImageDoc* im = cur();
    if (!im) return "no image is open";
    if (im->src->rawDtype < 0 || im->src->path.empty())
        return "this document did not come in through the headerless dialog";
    RawDialog d;
    rawRecipeIntoDialog(r, d);
    d.path = im->src->path;
    d.replaceIdx = app.current;
    FrameSource st;                    // the byte count is the binding's key
    st.path = d.path;
    statSourceFile(st);
    d.fileSize = (size_t)st.fsize;
    std::string err = loadRaw(d);
    if (err.empty()) rawRecipeBindSize(d.fileSize, rawRecipeFromDialog(d));
    return err;
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
    // dataRev is a MEMBERSHIP counter, and it unlatches every cache keyed on
    // uid+dataRev. The stack's measurements are not keyed that way: sigma_t is
    // computed over every member of the stack and keyed on
    // (seqId, frames, stackRev, ROI, CFA), where the ROI carries the REFERENCE
    // doc's dims - so a crop of any OTHER member moves nothing in that key
    // while silently dropping that member out of the numbers (it no longer has
    // the stack's shape). SeqInfo::stackRev is the stack's DATA revision
    // (docs/reference-design.md §3.2) and a crop is a change to the stack's
    // data, so it is bumped here - the same revision the reload walk bumps and
    // the same one a reduced frame's (seqId, stackRev) provenance reads (§10.1),
    // rather than a second answer to "have these pixels changed".
    if (im.seqId != 0)
        if (App::SeqInfo* si = seqInfo(im.seqId)) si->stackRev++;
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
