// PNG / JPEG / TIFF / OpenEXR, behind one interface (core/imagefile.h).
//
// This is the ONLY translation unit that knows a decoder library exists. The
// implementation of stb_image itself is in core/stb_image_impl.c, compiled with
// warnings off the way miniz.c is - third-party code is not policed here, and
// keeping it in its own file means this file IS held to -Wall -Wextra. The TIFF
// reader in core/tiffread.cpp is OURS, so it is held to it too, and
// core/exrread.cpp is our wrapper around a third-party library and is held to
// it as well - the library's own sources are compiled by its own project.
//
// To put a different library behind a format:
//   1. write `bool xxxDecode(const uint8_t*, size_t, std::vector<Image>&, std::string&)`;
//   2. point that format's row in backends() at it;
//   3. add the library to CMakeLists.txt and to THIRD-PARTY-NOTICES.
// Nothing else in the program changes, because nothing else in the program has
// ever seen the library's name outside the `library` field of that row. Step 3
// is what TIFF skips, and only because its reader is this repository's own
// code: there is no third party in it to notice.
#include "imagefile.h"

#include <cstdio>
#include <cstring>

#define STBI_NO_STDIO          // the caller has already read the bytes
#include "stb_image.h"
#include "tiffread.h"
#include "exrread.h"

namespace imagefile {

// ---------------------------------------------------------------- sniffing
// Magic bytes only. The extension is what a user typed or a camera chose; the
// first bytes are what the file IS, and when they disagree the bytes win.
static bool sniffPng(const uint8_t* p, size_t n) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return n >= 8 && memcmp(p, sig, 8) == 0;
}
static bool sniffJpeg(const uint8_t* p, size_t n) {
    return n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff;
}
static bool sniffTiff(const uint8_t* p, size_t n) {
    if (n < 4) return false;
    // "II" little-endian / "MM" big-endian, then 42 (TIFF) or 43 (BigTIFF)
    if (p[0] == 'I' && p[1] == 'I') return (p[2] == 42 || p[2] == 43) && p[3] == 0;
    if (p[0] == 'M' && p[1] == 'M') return p[2] == 0 && (p[3] == 42 || p[3] == 43);
    return false;
}
static bool sniffExr(const uint8_t* p, size_t n) {
    // 20000630 as a little-endian int32. It is the same four bytes for every
    // .exr there has ever been - scanline, tiled, deep or multi-part - so this
    // says "an EXR", and which KIND it is is the decoder's business to read and
    // (for three of those four) to refuse.
    static const uint8_t sig[4] = { 0x76, 0x2f, 0x31, 0x01 };
    return n >= 4 && memcmp(p, sig, 4) == 0;
}

// ---------------------------------------------------------------- PNG / JPEG
// stb_image gives back 8-bit or 16-bit samples with the file's own channel
// count (req_comp = 0), which is what the channel rule wants: a greyscale PNG
// is ONE frame of ONE channel, RGBA is one frame of four
// (docs/input-adapters.md §3.1, "the last axis, four or fewer, is channels").
// Asking stb for 4 channels always - the usual way to call it - would have made
// every mono capture a 4-channel image and every per-channel statistic a lie.

// What the FILE says it is, read from IHDR rather than from the decoder, so the
// note describes the file and survives a change of library. Bytes 24 and 25 of
// a PNG are the IHDR bit depth and colour type; the signature check above has
// already run, so the only thing left to test is that the chunk is there.
static bool pngIhdr(const uint8_t* p, size_t n, int& depth, int& colour) {
    if (n < 26 || memcmp(p + 12, "IHDR", 4) != 0) return false;
    depth = p[24];
    colour = p[25];
    return true;
}
static const char* pngColourName(int colour) {
    switch (colour) {
    case 0: return "greyscale";
    case 2: return "RGB";
    case 3: return "palette";
    case 4: return "greyscale+alpha";
    case 6: return "RGBA";
    }
    return "an unknown colour type";
}

static bool stbDecode(const uint8_t* p, size_t n, std::vector<Image>& images,
                      std::string& err) {
    // Exactly one picture: neither of these formats holds a second. The vector
    // is the seam's shape, not a claim that stb might fill it twice.
    images.clear();
    images.emplace_back();
    Image& out = images.back();
    int w = 0, h = 0, comp = 0;
    // This library takes the buffer length as an int. Truncating silently would
    // hand it a length that is not the file's, so the size is a refusal - one
    // more thing the seam checks so that a backend cannot be wrong quietly.
    if (n > (size_t)0x7fffffff) { err = "file is too large for this decoder (> 2 GiB)"; return false; }
    // Ask the header first. decode() checks the dimensions again afterwards -
    // that is the seam's guarantee and it stays - but a header claiming 60000
    // square would have been believed by the decoder long before the seam got
    // to see it, and 14 GB of float32 is not a thing to allocate on the way to
    // saying no.
    if (stbi_info_from_memory(p, (int)n, &w, &h, &comp) && (w > MAX_DIM || h > MAX_DIM)) {
        err = "unsupported image size (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        return false;
    }
    const bool png = sniffPng(p, n);
    // 16 bits is the case this audience actually has: a camera engineer's PNG
    // is far more likely to be 16-bit than 8, and reading it through the 8-bit
    // door would throw away half of every sample without saying so. So the bit
    // depth of the FILE decides which door is used, and neither depth is the
    // "normal" one that the other is converted into.
    const bool sixteen = stbi_is_16_bit_from_memory(p, (int)n) != 0;
    std::string note;
    if (png) {
        int depth = 0, colour = 0;
        if (pngIhdr(p, n, depth, colour)) {
            char buf[160];
            snprintf(buf, sizeof buf, "PNG %s, %d-bit", pngColourName(colour), depth);
            note = buf;
            // Depths below 8 are the one place the decoder MULTIPLIES: stb
            // expands 1/2/4-bit samples to fill 0..255 (x255, x85, x17). That
            // is a change of scale, so it is stated rather than left for
            // someone to discover from a histogram.
            if (depth < 8) {
                snprintf(buf, sizeof buf, "; expanded to 0..255 by the decoder (x%d)",
                         depth == 4 ? 17 : depth == 2 ? 85 : 255);
                note += buf;
            }
            if (colour == 3) note += "; palette expanded to colour samples";
        } else {
            note = "PNG";
        }
    }

    if (sixteen) {
        stbi_us* px = stbi_load_16_from_memory(p, (int)n, &w, &h, &comp, 0);
        if (!px) { err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; return false; }
        out.w = w; out.h = h; out.ch = comp;
        out.dtype = "u16";
        out.data.resize((size_t)w * h * comp);
        for (size_t i = 0; i < out.data.size(); i++) out.data[i] = (float)px[i];
        stbi_image_free(px);
    } else {
        stbi_uc* px = stbi_load_from_memory(p, (int)n, &w, &h, &comp, 0);
        if (!px) { err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; return false; }
        out.w = w; out.h = h; out.ch = comp;
        out.dtype = "u8";
        out.data.resize((size_t)w * h * comp);
        for (size_t i = 0; i < out.data.size(); i++) out.data[i] = (float)px[i];
        stbi_image_free(px);
    }

    if (!png) {
        // JPEG. The samples that come out went through the codec's own colour
        // transform, which is part of the format and not a choice this viewer
        // made - but it IS a thing that happened to the numbers, so it is said.
        note = "JPEG, 8-bit";
        if (out.ch >= 3) note += ", RGB after the codec's YCbCr->RGB transform";
        else note += ", greyscale";
    }
    out.note = note;
    return true;
}

// ---------------------------------------------------------------- the table
const std::vector<Backend>& backends() {
    static const std::vector<Backend> B = {
        { "PNG", ".png", "stb_image 2.30", sniffPng, stbDecode, nullptr, nullptr },
        { "JPEG", ".jpg .jpeg .jpe", "stb_image 2.30", sniffJpeg, stbDecode, nullptr, nullptr },
        // TIFF shipped for a while as a row with NO decoder, refusing by name.
        // What that row said is still the standard the reader behind it is held
        // to: TIFF is the only one of the three that carries measurements
        // (16-bit and float samples, a black level, sometimes a CFA pattern),
        // and reading it approximately is worse than not reading it. A reader
        // that assumed RGGB where the file said GBRG would produce plausible
        // pictures and wrong per-plane statistics with nothing on screen to say
        // so - so core/tiffread.cpp REFUSES a CFA TIFF, by name, rather than
        // open one. Everything else it will not read is refused the same way.
        { "TIFF", ".tif .tiff", TIFF_LIBRARY, sniffTiff, tiffDecode, nullptr, nullptr },
        // OpenEXR is the row that exercises BOTH first-class states of this
        // table from one build option: with -DVIEWER_WITH_EXR=ON it points at
        // core/exrread.cpp, and with OFF the same row still lists .exr, still
        // sniffs it, still dispatches to it, and refuses with EXR_ABSENT naming
        // the option. A user who turned the dependency off is told that; a user
        // who never had it is not left with "unknown file, try the raw dialog".
        //
        // It is also the only row whose parts are NAMED (partWord): an .exr
        // holds layers, which are documents, where a multi-page TIFF holds
        // pages, which are frames.
        { "OpenEXR", ".exr", EXR_LIBRARY, sniffExr, EXR_DECODE, EXR_ABSENT, "exr layer" },
    };
    return B;
}

static std::string lowerExt(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e;
}

// exts is a space-separated list: ".tif .tiff"
static bool extListHas(const char* exts, const std::string& e) {
    if (e.empty()) return false;
    std::string all = exts;
    size_t i = 0;
    while (i < all.size()) {
        size_t sp = all.find(' ', i);
        std::string one = all.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        if (one == e) return true;
        if (sp == std::string::npos) break;
        i = sp + 1;
    }
    return false;
}

const Backend* forPath(const std::string& path) {
    const std::string e = lowerExt(path);
    for (const Backend& b : backends())
        if (extListHas(b.exts, e)) return &b;
    return nullptr;
}

const Backend* forBytes(const uint8_t* p, size_t n) {
    for (const Backend& b : backends())
        if (b.sniff && b.sniff(p, n)) return &b;
    return nullptr;
}

std::string decodableFormats() {
    std::string s;
    for (const Backend& b : backends()) {
        if (!b.decode) continue;
        if (!s.empty()) s += ", ";
        s += b.format;
    }
    return s;
}

std::string dialogPattern() {
    std::string s;
    for (const Backend& b : backends()) {
        std::string all = b.exts;
        size_t i = 0;
        while (i < all.size()) {
            size_t sp = all.find(' ', i);
            std::string one = all.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            if (!one.empty()) {
                if (!s.empty()) s += " ";
                s += "*" + one;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
    }
    return s;
}

// The format names as a sentence fragment: "PNG, JPEG or TIFF" / "PNG and
// JPEG". `only` skips the rows with no decoder. A refusal is read by a person,
// so it is punctuated for one.
static std::string listFormats(const char* conjunction, bool decodableOnly) {
    std::vector<const char*> names;
    for (const Backend& b : backends())
        if (!decodableOnly || b.decode) names.push_back(b.format);
    std::string s;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) s += (i + 1 == names.size()) ? conjunction : ", ";
        s += names[i];
    }
    return s;
}

// The last line of every refusal here, and the same words the .npy door uses
// (core/main.cpp npyNotNative): the message that says no is also the place the
// way out is offered (docs/input-adapters.md §3.2 / §4.13).
static const char* CHOOSE_A_READER = "\n  choose a reader to read it another way";

static std::string whatIsRead() {
    return "\n  " + listFormats(" and ", true) + " are read natively";
}

bool decode(const std::string& path, const std::vector<uint8_t>& bytes,
            std::vector<Image>& out, std::string& err) {
    const uint8_t* p = bytes.data();
    const size_t n = bytes.size();
    const Backend* byName = forPath(path);
    const Backend* b = forBytes(p, n);
    if (!b) {
        char head[64] = "it is empty";
        if (n >= 4)
            snprintf(head, sizeof head, "it starts %02x %02x %02x %02x", p[0], p[1], p[2], p[3]);
        else if (n > 0)
            snprintf(head, sizeof head, "it is %d byte(s) long", (int)n);
        err = "not a " + listFormats(" or ", false) + " file (" + head + ")" +
              whatIsRead() + CHOOSE_A_READER;
        return false;
    }
    if (!b->decode) {
        err = std::string(b->format) + " is not read by this build\n  " + b->absent +
              whatIsRead() + CHOOSE_A_READER;
        return false;
    }
    std::vector<Image> got;
    std::string why;
    if (!b->decode(p, n, got, why) || got.empty()) {
        if (why.empty()) why = "nothing came back from the decoder";
        err = std::string(b->format) + ": " + why + " (" + b->library + ")" + CHOOSE_A_READER;
        return false;
    }
    for (Image& img : got) {
        // The same ceilings the .npy door applies, in the same words, because they
        // are the viewer's limits and not the format's (core/main.cpp npyLayout).
        if (img.w < 1 || img.h < 1) {
            err = std::string(b->format) + ": no pixels in it" + CHOOSE_A_READER;
            return false;
        }
        if (img.w > MAX_DIM || img.h > MAX_DIM) {
            err = "unsupported image size";
            return false;
        }
        if (img.ch < 1 || img.ch > 4) {
            err = std::string(b->format) + " would be " + std::to_string(img.ch) +
                  " channels: this viewer shows up to 4";
            return false;
        }
        // The sentence every one of these files needs and none of them carries.
        // A PNG, a JPEG or a TIFF does not declare a transfer characteristic,
        // an exposure or a black level: the numbers are whatever the writer put
        // there. Saying what was NOT done is the only honest thing available -
        // and it is what stops "0..255 of something" from silently becoming
        // "0..1 of radiance".
        //
        // It is appended HERE and not by each backend, because it is a promise
        // the SEAM makes (rule 1) and not a fact about a library. A new backend
        // therefore cannot forget it, and cannot word it slightly differently.
        if (!img.note.empty()) img.note += "; ";
        img.note += "values as stored, no scaling or transfer curve applied";
        // A name that promised something else. Not an error - the bytes decoded -
        // but a fact about this file that belongs on screen, because the next
        // person to open it will believe the extension too.
        if (byName && byName != b)
            img.note = std::string("the name says ") + byName->format + " but the bytes are " +
                       b->format + "; read as " + b->format + ". " + img.note;
    }
    out = std::move(got);
    return true;
}

}  // namespace imagefile
