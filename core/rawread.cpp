// Vendor RAW through LibRaw, for core/imagefile.h. See rawread.h for why this
// one is a library, what is taken out of it and what is deliberately not.
//
// WHAT THIS FILE IS FOR, in one line: the sensor's own counts arrive in the
// document unchanged, and everything this build will not read is refused BY
// NAME with a reason (docs/input-adapters.md §3.2).
//
// The first half is the whole reason vendor RAW is worth having behind this
// seam. A RAW file is the only format here that STATES what its numbers are: a
// CFA pattern, a black level, a white level and a bit depth, all declared by
// the file rather than assumed by the reader. So the load path does nothing to
// them - no demosaic, no white balance, no colour matrix, no tone curve, and
// above all no black subtraction - and the declared numbers travel in the note
// where the Inspector prints them.
//
// The second half is where the code actually is. "RAW" is not one format: it is
// every sensor container thirty vendors have shipped, and several of them are
// things this viewer cannot represent (a 6x6 X-Trans mosaic, a Foveon stack, a
// three-plane sRAW) or that LibRaw itself does not decode (Nikon HE, JPEG-XL
// DNG, GoPro VC-5). Each of those is a file that could be opened into something
// plausible and wrong, so each is refused by the name the format itself uses.
#include "rawread.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

// VIEWER_NO_LIBRAW is viewer-serve's build (CMakeLists.txt). The peer does not
// carry LibRaw - it is CDDL-1.0 and viewer-serve installs ITSELF onto another
// machine over ssh, which makes it a distribution question and not a technical
// one (issue #148). What survives that build is rawSniff and nothing else, and
// it survives on purpose: it is header inspection with no library behind it, so
// a .NEF reaching the peer is still RECOGNISED as a camera's file and refused
// by name through the table's `absent` state, instead of falling through to the
// TIFF row beneath it and being decoded as whichever IFD sits at its front.
#ifndef VIEWER_NO_LIBRAW
#include <libraw/libraw.h>
#endif

namespace imagefile {

// LIBRAW_VERSION_STR is "0.22.2-Release": the tail is a build flavour, not part
// of the version anyone pins, and it would ride into every refusal sentence.
#define RAW_STR2(x) #x
#define RAW_STR(x) RAW_STR2(x)
#ifdef VIEWER_NO_LIBRAW
// "" is what core/imagefile.h says a row with no decoder carries here, and this
// build is that row: naming a library the binary does not link would be a claim
// about a decoder that is not there.
const char* const RAW_LIBRARY = "";
#else
const char* const RAW_LIBRARY = "LibRaw " RAW_STR(LIBRAW_MAJOR_VERSION) "."
                               RAW_STR(LIBRAW_MINOR_VERSION) "."
                               RAW_STR(LIBRAW_PATCH_VERSION);
#endif

namespace {

// ---------------------------------------------------------------- sniffing
// Read rawread.h first for WHY this is not four magic bytes. What follows is
// which of the file's own words are read.

static uint16_t rd16(const uint8_t* p, bool le) {
    return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t* p, bool le) {
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : ((uint32_t)p[3] | ((uint32_t)p[2] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24));
}

// The makers whose RAW container is a PLAIN TIFF file - the ones that cannot be
// told from a scanner's .tif by the first four bytes, and so have to be told by
// what IFD0 says its Make is.
//
// This is a list of NAMES A FILE CAN CONTAIN, not a list of extensions, and
// that difference is the point: it is matched against tag 271 of the file
// itself, so a .tif that a camera wrote is recognised and a .nef that something
// else wrote is not. It is compared case-insensitively and as a PREFIX, because
// makers write "NIKON CORPORATION", "SEIKO EPSON CORP." and "Phase One A/S".
const char* const TIFF_HEADED_MAKERS[] = {
    "NIKON", "SONY", "PENTAX", "RICOH", "SAMSUNG", "Hasselblad", "Phase One",
    "Leaf", "KODAK", "EASTMAN KODAK", "Mamiya", "Leica", "Epson", "SEIKO EPSON",
    "AgfaPhoto", "Canon", "CASIO", "Sinar", "KONICA MINOLTA", "Minolta", "Asahi",
    "OLYMPUS", "Panasonic", "SIGMA", "FUJIFILM", "Apple", "Google", "Motorola",
    "Xiaomi", "OnePlus", "HUAWEI", "LGE", "HMD Global", "ZTE",
};

bool makerIsCamera(const char* s, size_t len) {
    for (const char* want : TIFF_HEADED_MAKERS) {
        const size_t wl = strlen(want);
        if (len < wl) continue;
        size_t i = 0;
        for (; i < wl; i++) {
            char a = s[i], b = want[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) break;
        }
        if (i == wl) return true;
    }
    return false;
}

// Walk IFD0 of a classic (not Big-) TIFF and answer the two questions that
// decide this: does the file declare itself a DNG, and does it name a camera
// maker beside a sub-image chain. Bounded: it reads the one directory and
// follows no pointer it has not range-checked.
bool tiffHeadedRaw(const uint8_t* p, size_t n) {
    const bool le = (p[0] == 'I');
    const uint32_t ifd = rd32(p + 4, le);
    if (ifd < 8 || (size_t)ifd + 2 > n) return false;
    const uint16_t cnt = rd16(p + ifd, le);
    if (cnt == 0 || (size_t)ifd + 2 + 12 * (size_t)cnt > n) return false;
    bool dng = false, subIfds = false, camera = false;
    for (uint16_t i = 0; i < cnt; i++) {
        const uint8_t* e = p + ifd + 2 + 12 * (size_t)i;
        const uint16_t tag = rd16(e, le), type = rd16(e + 2, le);
        const uint32_t num = rd32(e + 4, le);
        if (tag == 50706) dng = true;                      // DNGVersion
        if (tag == 330) subIfds = true;                    // SubIFDs
        if (tag == 271 && type == 2 && num >= 2) {         // Make, ASCII
            const uint8_t* v = e + 8;
            if (num > 4) {
                const uint32_t off = rd32(e + 8, le);
                if ((size_t)off + num > n) continue;
                v = p + off;
            }
            if (makerIsCamera((const char*)v, num - 1)) camera = true;
        }
    }
    // A DNG says so in one tag and needs no other evidence. Everything else
    // needs BOTH halves: the maker AND the sub-image chain a raw container has
    // and a flat picture TIFF does not - so that a 16-bit measurement .tif that
    // happens to carry a camera's name in its EXIF still goes to the TIFF
    // reader, which is the one that reads it.
    return dng || (camera && subIfds);
}

}  // namespace

bool rawSniff(const uint8_t* p, size_t n) {
    if (n < 16) return false;
    // ---- containers that are not TIFF at all, and say so in their first bytes
    // Canon CR3 is ISO base media: [size]"ftyp" then the brand.
    if (memcmp(p + 4, "ftypcrx ", 8) == 0) return true;
    // Fujifilm RAF, Sigma/Foveon X3F, Minolta MRW: their own signatures.
    if (memcmp(p, "FUJIFILM", 8) == 0) return true;
    if (memcmp(p, "FOVb", 4) == 0) return true;
    if (memcmp(p, "\0MRM", 4) == 0) return true;
    // Canon CRW is CIFF, not TIFF at all: "II", a header offset, then this.
    if (memcmp(p + 6, "HEAPCCDR", 8) == 0) return true;
    // Olympus ORF and Panasonic RW2 are TIFF-SHAPED but put their own number
    // where TIFF's 42 goes, which is why sniffTiff never sees them.
    if (memcmp(p, "IIRO", 4) == 0 || memcmp(p, "IIRS", 4) == 0 ||
        memcmp(p, "MMOR", 4) == 0) return true;
    if (p[0] == 'I' && p[1] == 'I' && p[2] == 0x55 && p[3] == 0) return true;
    // ---- and the ones that ARE a TIFF file --------------------------------
    const bool le = p[0] == 'I' && p[1] == 'I';
    const bool be = p[0] == 'M' && p[1] == 'M';
    if (!le && !be) return false;
    if (rd16(p + 2, le) != 42) return false;   // 43 is BigTIFF: TIFF's to refuse
    // Canon CR2 stamps "CR" and its version straight after the header.
    if (le && p[8] == 'C' && p[9] == 'R') return true;
    return tiffHeadedRaw(p, n);
}

#ifndef VIEWER_NO_LIBRAW

namespace {

// The four Bayer orders this viewer can name (core/app/state.h CFA_PATTERNS).
// A mosaic that is not one of them is refused rather than approximated: the
// per-plane statistics this tool exists for are wrong in silence otherwise.
const char* const BAYER[4] = { "RGGB", "BGGR", "GRBG", "GBRG" };

std::string ver(unsigned v) {
    char b[32];
    snprintf(b, sizeof b, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return b;
}

// Why a codec that LibRaw KNOWS but does not decode cannot be decoded, named.
// The flag is raised by get_decoder_info() BEFORE a pixel is touched, so this
// is answerable without unpacking (#52's survey, §4.8.3).
std::string unsupportedCodec(const char* decoder, const LibRaw& R) {
    const std::string d = decoder ? decoder : "";
    std::string s;
    if (d.find("nikon_he") != std::string::npos) {
        const unsigned c = R.imgdata.makernotes.nikon.NEFCompression;
        s = "Nikon High Efficiency (" + std::string(c == 14 ? "HE*" : "HE") +
            ", NEFCompression " + std::to_string(c) + "): " + RAW_LIBRARY +
            " does not decode it - upstream's own camera list says \"not supported yet\"";
    } else if (d.find("jxl") != std::string::npos) {
        s = "JPEG-XL compression (DNG 1.7): decoding it needs the Adobe DNG SDK, "
            "which is not built into this viewer";
    } else if (d.find("vc5") != std::string::npos) {
        s = "GoPro VC-5 compression: decoding it needs the GPR SDK, which is not "
            "built into this viewer";
    } else {
        s = std::string("the codec behind ") + (decoder ? decoder : "this file") +
            " is one " + RAW_LIBRARY + " knows by name and does not decode";
    }
    return s;
}

}  // namespace

// The body of rawDecode. Wrapped by the entry point below, because the one
// thing here that can throw is OURS: a 10388x7792 mosaic is 324 MB of float32,
// and std::bad_alloc escaping a backend would take the process down on a file
// the seam is supposed to be able to refuse.
static bool rawDecodeBody(const uint8_t* p, size_t n, std::vector<Image>& out,
                          std::string& err) {
    // ON THE HEAP, and that is the whole of it: sizeof(LibRaw) is 768,512 bytes.
    //
    // A LibRaw is not an ordinary handle. Its context is the decoder's entire
    // working state - imgdata.color alone carries a 65536-entry tone curve and
    // a 4102-entry per-plane black level - and the constructor memsets all of
    // it, so an instance as a LOCAL is three quarters of a megabyte of stack
    // written on the way in. Windows gives the main thread ONE megabyte
    // (SizeOfStackReserve 0x100000, which is what MSVC links and therefore what
    // every shipped Windows build has); Linux and macOS give eight. So on
    // Windows this frame alone was 75% of the thread's stack, and whether it
    // fit depended on how much of that megabyte the caller had already spent -
    // which for a selftest binary is `main`, whose frame carries every
    // #include'd selftest body. That made the overflow a property of the
    // BINARY'S LAYOUT rather than of this file: main survived, and two branches
    // that touched nothing near this path (the Set Analysis separation fit, the
    // detrend preprocessor) each died with STATUS_STACK_OVERFLOW (0xC00000FD)
    // on the first .dng the media selftest opens, reported as `selftest.media
    // ***Exception: SegFault` on windows-latest and nowhere else.
    //
    // Reproduced on unmodified main by linking the MinGW build with a 768 KB
    // stack (-Wl,--stack,0xC0000): it dies at exactly that file, after M28's
    // last assert and before M29's first. On the heap this frame measures 1,304
    // bytes instead of 769,816, and what keeps it there is not this comment but
    // -Werror=frame-larger-than=65536 on the five backend sources
    // (CMakeLists.txt): the next context that lands on the stack does not
    // compile on Linux or macOS, which is the check Windows cannot make.
    //
    // std::unique_ptr and not a `new` this function has to remember to delete:
    // there are eleven `return false` paths below, and one of them is reached
    // by a throw from the resize() further down.
    std::unique_ptr<LibRaw> Rp(new LibRaw);
    LibRaw& R = *Rp;
    // Nothing in imgdata.params is set, and that is deliberate: every one of
    // them steers dcraw_process(), which is not linked into this binary. The
    // only call made here is unpack(), whose output is the file's own samples.
    int rc = R.open_buffer(const_cast<uint8_t*>(p), n);
    if (rc != LIBRAW_SUCCESS) {
        // LibRaw's own sentence. It is the honest one here: the bytes reached
        // this backend because they LOOK like a vendor RAW, and what the
        // library found wrong with them is a better answer than a rephrasing.
        err = std::string(libraw_strerror(rc)) + " (LibRaw read the header and refused)";
        return false;
    }

    // ---- refuse BEFORE decoding, where the file lets us --------------------
    libraw_decoder_info_t di;
    memset(&di, 0, sizeof di);
    if (R.get_decoder_info(&di) == LIBRAW_SUCCESS &&
        (di.decoder_flags & LIBRAW_DECODER_UNSUPPORTED_FORMAT)) {
        err = unsupportedCodec(di.decoder_name, R);
        return false;
    }

    const libraw_iparams_t& I = R.imgdata.idata;
    if (I.is_foveon) {
        err = "a Foveon X3 sensor: it stacks three measurements at every pixel "
              "rather than mosaicing one, so it has no CFA pattern and is not "
              "the one-plane mosaic this reader hands over";
        return false;
    }

    rc = R.unpack();
    if (rc != LIBRAW_SUCCESS) {
        err = std::string(libraw_strerror(rc)) + " (unpacking the sensor data, " +
              (di.decoder_name ? di.decoder_name : "unnamed decoder") + ")";
        return false;
    }

    const libraw_rawdata_t& D = R.imgdata.rawdata;
    if (!D.raw_image) {
        // The three shapes that are not a one-plane mosaic, each named as the
        // thing it is. A viewer that flattened any of them would produce a
        // picture and destroy the measurement it was opened to make.
        const char* what = D.float_image  ? "floating-point samples (a linear DNG)"
                         : D.color4_image ? "four colour planes per pixel (a 4-shot or sRAW capture)"
                         : D.color3_image ? "three colour planes per pixel (already demosaiced in the file)"
                                          : "no sensor mosaic at all";
        err = std::string("this file holds ") + what +
              ", and this reader hands over the CFA mosaic only";
        return false;
    }
    if (I.filters == 0) {
        err = "the file declares no CFA pattern (filters = 0): its samples are "
              "not a mosaic, and a pattern is never guessed here";
        return false;
    }
    if (I.filters < 1000) {
        err = std::string("a non-Bayer mosaic (LibRaw filters code ") +
              std::to_string(I.filters) +
              (I.filters == 9 ? ", an X-Trans 6x6 pattern" : "") +
              "): this viewer names four 2x2 Bayer orders and cannot describe it, "
              "and describing it wrongly would make every per-plane statistic wrong "
              "in silence";
        return false;
    }

    const libraw_image_sizes_t& S = R.imgdata.sizes;
    const int w = (int)S.width, h = (int)S.height;
    if (w < 1 || h < 1) {
        err = "the visible frame is " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    if (w > MAX_DIM || h > MAX_DIM) {
        err = "the visible frame is " + std::to_string(w) + "x" + std::to_string(h) +
              ", past this viewer's " + std::to_string((int)MAX_DIM) + " limit";
        return false;
    }
    const size_t stride = S.raw_pitch ? (size_t)S.raw_pitch / 2 : (size_t)S.raw_width;
    if ((size_t)S.top_margin + (size_t)h > (size_t)S.raw_height ||
        (size_t)S.left_margin + (size_t)w > stride) {
        err = "the visible frame the file declares (" + std::to_string(w) + "x" +
              std::to_string(h) + " at " + std::to_string(S.left_margin) + "," +
              std::to_string(S.top_margin) + ") does not fit its own raw frame (" +
              std::to_string(S.raw_width) + "x" + std::to_string(S.raw_height) + ")";
        return false;
    }

    // ---- the CFA pattern, READ ---------------------------------------------
    // COLOR(row,col) is LibRaw's own answer for the VISIBLE image (its
    // raw2image() indexes the mosaic exactly this way), which is the window
    // handed over below. Deriving a pattern for some other window would be
    // arithmetic on a fact rather than the fact, and rule 3 of
    // core/imagefile.h forbids exactly that.
    char q[5];
    for (int i = 0; i < 4; i++) {
        const int c = R.COLOR(i >> 1, i & 1);
        q[i] = (c >= 0 && c < 4) ? I.cdesc[c] : '?';
    }
    q[4] = 0;
    int pattern = -1;
    for (int i = 0; i < 4; i++) if (strcmp(q, BAYER[i]) == 0) pattern = i;
    if (pattern < 0) {
        err = std::string("the CFA reads ") + q + " (the file's colour description is \"" +
              I.cdesc + "\"), which is not one of the four Bayer orders this viewer "
              "names - RGGB, BGGR, GRBG, GBRG";
        return false;
    }

    // ---- the pixels, as counted --------------------------------------------
    out.clear();
    out.emplace_back();
    Image& img = out.back();
    img.w = w; img.h = h; img.ch = 1;
    img.dtype = "u16";                 // what raw_image IS: unsigned 16-bit counts
    img.cfa = 1;                       // Bayer, and READ (rule 3)
    img.cfaPattern = pattern;
    img.data.resize((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) {
        const uint16_t* row = D.raw_image + ((size_t)y + S.top_margin) * stride + S.left_margin;
        float* dst = img.data.data() + (size_t)y * w;
        for (int x = 0; x < w; x++) dst[x] = (float)row[x];
    }

    // ---- and what the FILE declared about them -----------------------------
    // Every number below is the file's, printed rather than applied. The black
    // level in particular: #52's survey §4.7 measured a NEF declaring 0 with
    // samples at 80, so a reader that subtracted the declared value would be
    // wrong there and unnoticeable everywhere else.
    const libraw_colordata_t& C = R.imgdata.color;
    char buf[512];
    std::string note = std::string(RAW_LIBRARY) + " (" +
                       (di.decoder_name ? di.decoder_name : "unnamed decoder") + "), " +
                       I.make + " " + I.model + "; the CFA mosaic, one plane";
    snprintf(buf, sizeof buf, "; CFA %s, read from the file", BAYER[pattern]);
    note += buf;
    snprintf(buf, sizeof buf, "; %u-bit samples as the file declares them", C.raw_bps);
    note += buf;
    if (I.dng_version) note += "; DNG " + ver(I.dng_version);
    snprintf(buf, sizeof buf, "; black level %d, SHOWN and not subtracted", C.black);
    note += buf;
    if (C.cblack[0] || C.cblack[1] || C.cblack[2] || C.cblack[3]) {
        snprintf(buf, sizeof buf, " (per plane %u/%u/%u/%u on top of it)",
                 C.black + C.cblack[0], C.black + C.cblack[1],
                 C.black + C.cblack[2], C.black + C.cblack[3]);
        note += buf;
    }
    if (C.cblack[4] > 1 || C.cblack[5] > 1) {
        snprintf(buf, sizeof buf, " (the file also declares a %ux%u black-level block)",
                 C.cblack[4], C.cblack[5]);
        note += buf;
    }
    snprintf(buf, sizeof buf, "; white level %d", C.maximum);
    note += buf;
    if (S.raw_width != (unsigned)w || S.raw_height != (unsigned)h) {
        snprintf(buf, sizeof buf,
                 "; %dx%d read out of a %ux%u raw frame at (%u,%u) - the masked "
                 "border the file excludes is not read",
                 w, h, S.raw_width, S.raw_height, S.left_margin, S.top_margin);
        note += buf;
    }
    if (I.raw_count > 1) {
        snprintf(buf, sizeof buf, "; the file holds %u shots and this is the first",
                 I.raw_count);
        note += buf;
    }
    // The one sentence a RAW needs and no other format here does: naming what
    // was NOT done is what stops a mosaic in [DN] from being read as a picture.
    note += "; no demosaic, no white balance, no colour matrix, no tone curve";
    img.note = note;
    return true;
}

bool rawDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err) {
    try {
        return rawDecodeBody(p, n, out, err);
    } catch (const std::exception& e) {
        err = std::string("this file could not be decoded: ") + e.what();
        out.clear();
        return false;
    }
}

#endif  // VIEWER_NO_LIBRAW

}  // namespace imagefile
