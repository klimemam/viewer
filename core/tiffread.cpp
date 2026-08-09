// TIFF, for core/imagefile.h. See tiffread.h for why this is ours and not a
// library.
//
// THE SHAPE OF THIS FILE IS "READ, OR SAY WHAT STOPPED YOU". TIFF is a
// container of tags, and a reader that meets a tag it does not understand has
// exactly two honest options: refuse, naming the tag and its value, or read the
// file correctly. There is no third option where it proceeds on an assumption,
// because every assumption available here - the byte order, the predictor, the
// photometric, the CFA pattern - changes the NUMBERS while leaving a picture
// that looks entirely plausible (core/imagefile.h, rules 1 and 3).
//
// So: every parse step below either establishes a fact from the file or writes
// a sentence into `err` that names the tag, its value and what this build does
// read instead. `err` is the reason alone; core/imagefile.cpp prefixes the
// format and the reader, and the caller prefixes the file name
// (docs/input-adapters.md §3.2).
//
// What is read: classic TIFF (magic 42), either byte order, strip layouts,
// 8- and 16-bit unsigned integer and 32-bit IEEE float samples, greyscale
// (WhiteIsZero or BlackIsZero) and RGB with or without alpha, compression none
// / PackBits / LZW / Deflate, predictor 1 and 2, one or many pages.
#include "tiffread.h"

#include <cstdio>
#include <cstring>
#include <set>

#include "miniz.h"          // Deflate. Already linked for .npz - no new dependency.

namespace imagefile {

const char* const TIFF_LIBRARY = "tiffread 1, core/tiffread.cpp";

namespace {

// A stack of more pages than this is not a measurement anyone is about to look
// at frame by frame, and the IFD walk should not be unbounded on a hostile
// file. Named in the refusal, so the number is not a mystery.
const uint32_t MAX_PAGES = 4096;
// ...and the pixels behind them. 2^30 float samples is 4 GiB resident; refusing
// BEFORE the allocation is the same discipline MAX_DIM exists for.
const uint64_t MAX_SAMPLES = 1u << 30;

// ---------------------------------------------------------------- the bytes
// Every read is bounds-checked against the file length and honours the byte
// order the header declared. "II" and "MM" are both ordinary; neither is the
// one the other is converted into.
struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool be = false;

    bool u16v(size_t o, uint32_t& v) const {
        if (o + 2 > n) return false;
        v = be ? (uint32_t)((p[o] << 8) | p[o + 1]) : (uint32_t)((p[o + 1] << 8) | p[o]);
        return true;
    }
    bool u32v(size_t o, uint32_t& v) const {
        if (o + 4 > n) return false;
        v = be ? ((uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | (uint32_t)p[o + 3])
               : ((uint32_t)p[o + 3] << 24 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o]);
        return true;
    }
};

enum {                                  // the tags this reader looks at
    T_NEWSUBFILE = 254, T_WIDTH = 256, T_HEIGHT = 257, T_BITS = 258,
    T_COMPRESSION = 259, T_PHOTOMETRIC = 262, T_STRIPOFFSETS = 273,
    T_SAMPLES = 277, T_ROWSPERSTRIP = 278, T_STRIPBYTES = 279, T_PLANAR = 284,
    T_PREDICTOR = 317, T_TILEWIDTH = 322, T_TILELENGTH = 323,
    T_TILEOFFSETS = 324, T_SAMPLEFORMAT = 339,
};

struct Field {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    size_t off = 0;                     // absolute offset of the VALUES
};

size_t typeSize(uint16_t t) {
    switch (t) {
    case 1: case 2: case 6: case 7: return 1;   // BYTE ASCII SBYTE UNDEFINED
    case 3: case 8:                  return 2;  // SHORT SSHORT
    case 4: case 9: case 11:         return 4;  // LONG SLONG FLOAT
    case 5: case 10: case 12:        return 8;  // RATIONAL SRATIONAL DOUBLE
    }
    return 0;
}

std::string num(uint64_t v) { return std::to_string((long long)v); }

// A tag's values as integers. Only the integer types are read: every tag this
// reader consults is one, and a RATIONAL where a LONG belongs is a file this
// build should refuse rather than reinterpret.
bool fieldValues(const Bytes& B, const Field& f, std::vector<uint64_t>& out) {
    const size_t ts = typeSize(f.type);
    if (ts == 0 || (ts != 1 && ts != 2 && ts != 4)) return false;
    const uint64_t bytes = (uint64_t)ts * f.count;
    if (bytes > B.n || f.off > B.n - bytes) return false;
    out.clear();
    out.reserve(f.count);
    for (uint32_t i = 0; i < f.count; i++) {
        uint32_t v = 0;
        const size_t o = f.off + (size_t)i * ts;
        if (ts == 1) v = B.p[o];
        else if (ts == 2) { if (!B.u16v(o, v)) return false; }
        else { if (!B.u32v(o, v)) return false; }
        out.push_back(v);
    }
    return true;
}

const Field* find(const std::vector<Field>& f, uint16_t tag) {
    for (const Field& e : f) if (e.tag == tag) return &e;
    return nullptr;
}

// One value, or the default. `got` says which, for the tags where "absent" and
// "present and equal to the default" are different sentences.
uint64_t one(const Bytes& B, const std::vector<Field>& f, uint16_t tag,
             uint64_t dflt, bool* got = nullptr) {
    if (got) *got = false;
    const Field* e = find(f, tag);
    if (!e || e->count == 0) return dflt;
    std::vector<uint64_t> v;
    if (!fieldValues(B, *e, v) || v.empty()) return dflt;
    if (got) *got = true;
    return v[0];
}

// ---------------------------------------------------------------- the names
// A refusal that says "compression 7" is already better than "unsupported"; one
// that says "compression 7 (JPEG)" is what a person can act on.
const char* compressionName(uint64_t c) {
    switch (c) {
    case 1:     return "none";
    case 2:     return "CCITT modified Huffman";
    case 3:     return "CCITT Group 3 fax";
    case 4:     return "CCITT Group 4 fax";
    case 5:     return "LZW";
    case 6:     return "JPEG, the withdrawn 1992 form";
    case 7:     return "JPEG";
    case 8:     return "Deflate";
    case 32773: return "PackBits";
    case 32946: return "Deflate, the old code";
    case 34712: return "JPEG 2000";
    case 34925: return "LZMA";
    case 50000: return "Zstandard";
    case 50001: return "WebP";
    }
    return "unknown to this build";
}

const char* photometricName(uint64_t p) {
    switch (p) {
    case 0:     return "WhiteIsZero";
    case 1:     return "BlackIsZero";
    case 2:     return "RGB";
    case 3:     return "palette colour";
    case 4:     return "transparency mask";
    case 5:     return "CMYK";
    case 6:     return "YCbCr";
    case 8:     return "CIE L*a*b*";
    case 32803: return "colour filter array";
    case 34892: return "linear raw";
    }
    return "unknown to this build";
}

const char* sampleFormatName(uint64_t s) {
    switch (s) {
    case 1: return "unsigned integer";
    case 2: return "signed integer";
    case 3: return "IEEE float";
    case 4: return "undefined";
    }
    return "unknown to this build";
}

// ---------------------------------------------------------- decompression
// Each of these writes EXACTLY dstN bytes or fails. That is deliberate: the
// destination is sized from the page's own geometry, so a stream that expands
// to more than its rows can hold cannot run anywhere, and one that expands to
// fewer is a truncated file rather than a page of zeros nobody mentioned.

bool inflateInto(const uint8_t* src, size_t srcN, uint8_t* dst, size_t dstN, std::string& err) {
    mz_ulong got = (mz_ulong)dstN;
    const int r = mz_uncompress(dst, &got, src, (mz_ulong)srcN);
    if (r != MZ_OK) {
        err = std::string("a Deflate strip did not decompress (") + mz_error(r) + ")";
        return false;
    }
    if ((size_t)got != dstN) {
        err = "a Deflate strip decompressed to " + num(got) + " bytes, not the " +
              num(dstN) + " its rows need";
        return false;
    }
    return true;
}

bool packbitsInto(const uint8_t* src, size_t srcN, uint8_t* dst, size_t dstN, std::string& err) {
    size_t i = 0, o = 0;
    while (o < dstN) {
        if (i >= srcN) break;
        const int8_t h = (int8_t)src[i++];
        if (h == -128) continue;                    // no-op, by the spec
        if (h >= 0) {
            const size_t cnt = (size_t)h + 1;
            if (i + cnt > srcN || o + cnt > dstN) break;
            memcpy(dst + o, src + i, cnt);
            i += cnt;
            o += cnt;
        } else {
            const size_t cnt = (size_t)(1 - h);
            if (i >= srcN || o + cnt > dstN) break;
            memset(dst + o, src[i++], cnt);
            o += cnt;
        }
    }
    if (o != dstN) {
        err = "a PackBits strip unpacked to " + num(o) + " bytes, not the " +
              num(dstN) + " its rows need";
        return false;
    }
    return true;
}

// TIFF LZW: codes are MSB-first, 9 to 12 bits wide, 256 clears the table, 257
// ends the strip, and the width steps up ONE CODE EARLY (at 511/1023/2047).
// That last detail is the difference between this and the LZW in a GIF, and
// getting it wrong desynchronises the bit stream partway through a strip -
// which produces noise in the bottom half of an image whose top half is right.
bool lzwInto(const uint8_t* src, size_t srcN, uint8_t* dst, size_t dstN, std::string& err) {
    struct Ent { int prev; uint8_t ch, first; uint32_t len; };
    std::vector<Ent> tab(4096);
    auto reset = [&](int& next, int& width, int& prev) {
        for (int i = 0; i < 256; i++)
            tab[(size_t)i] = { -1, (uint8_t)i, (uint8_t)i, 1 };
        next = 258;
        width = 9;
        prev = -1;
    };
    int next = 258, width = 9, prev = -1;
    reset(next, width, prev);

    size_t o = 0, bit = 0;
    const size_t bits = srcN * 8;
    auto emit = [&](int code) {
        const uint32_t len = tab[(size_t)code].len;
        if (o + len > dstN) return false;
        size_t w = o + len;
        for (int c = code; c >= 0; c = tab[(size_t)c].prev) dst[--w] = tab[(size_t)c].ch;
        o += len;
        return true;
    };
    while (bit + (size_t)width <= bits) {
        uint32_t code = 0;
        for (int i = 0; i < width; i++, bit++)
            code = (code << 1) | ((src[bit >> 3] >> (7 - (bit & 7))) & 1u);
        if (code == 257) break;                             // EndOfInformation
        if (code == 256) { reset(next, width, prev); continue; }
        if (prev < 0) {
            if (code > 255) { err = "an LZW strip starts with code " + num(code) +
                                    ", which is not a byte"; return false; }
            if (!emit((int)code)) { err = "an LZW strip expands past its rows"; return false; }
            prev = (int)code;
        } else if (code < (uint32_t)next) {
            if (next < 4096) {
                tab[(size_t)next] = { prev, tab[(size_t)code].first,
                                      tab[(size_t)prev].first, tab[(size_t)prev].len + 1 };
                next++;
            }
            if (!emit((int)code)) { err = "an LZW strip expands past its rows"; return false; }
            prev = (int)code;
        } else if (code == (uint32_t)next && next < 4096) {
            tab[(size_t)next] = { prev, tab[(size_t)prev].first,
                                  tab[(size_t)prev].first, tab[(size_t)prev].len + 1 };
            const int made = next++;
            if (!emit(made)) { err = "an LZW strip expands past its rows"; return false; }
            prev = made;
        } else {
            err = "an LZW strip uses code " + num(code) + ", which its table does not hold";
            return false;
        }
        width = next >= 2047 ? 12 : next >= 1023 ? 11 : next >= 511 ? 10 : 9;
    }
    if (o != dstN) {
        err = "an LZW strip expanded to " + num(o) + " bytes, not the " + num(dstN) +
              " its rows need";
        return false;
    }
    return true;
}

// ------------------------------------------------------------- one page ----
// Everything the reader needs about an IFD, established before a single pixel
// is allocated - so an impossible page is refused while it is still a header.
struct Page {
    uint32_t w = 0, h = 0, ch = 1, bits = 0;
    uint64_t sf = 1, compression = 1, photometric = 0, predictor = 1;
    uint64_t rowsPerStrip = 0;
    std::vector<uint64_t> offs, counts;
    bool reduced = false;               // NewSubfileType bit 0: a thumbnail
    int number = 0;                     // 1-based, the FILE's page number
};

bool parsePage(const Bytes& B, const std::vector<Field>& f, int pageNo, Page& P,
               std::string& err) {
    P.number = pageNo;
    const std::string at = "page " + num((uint64_t)pageNo);

    // A tag this reader CONSULTS, present but unreadable (a type it does not
    // read, a count that runs off the end), is refused here rather than
    // silently falling back to the tag's default. Those defaults are things
    // like "compression none" and "one strip", so falling back to them would
    // read the file as something it never claimed to be - which is the one
    // failure mode this reader has to make impossible rather than unlikely.
    static const uint16_t CONSULTED[] = {
        T_NEWSUBFILE, T_WIDTH, T_HEIGHT, T_BITS, T_COMPRESSION, T_PHOTOMETRIC,
        T_STRIPOFFSETS, T_SAMPLES, T_ROWSPERSTRIP, T_STRIPBYTES, T_PLANAR,
        T_PREDICTOR, T_TILEWIDTH, T_TILELENGTH, T_TILEOFFSETS, T_SAMPLEFORMAT };
    for (uint16_t t : CONSULTED) {
        const Field* e = find(f, t);
        std::vector<uint64_t> v;
        if (e && !fieldValues(B, *e, v)) {
            err = at + " carries tag " + num(t) + " as type " + num(e->type) +
                  " x" + num(e->count) + ", which this build cannot read - and it "
                  "is a tag that decides how the pixels are laid out";
            return false;
        }
    }

    // A page that declares itself a reduced-resolution copy of another one is
    // read as such and dropped by the caller. This is READ, not inferred from
    // its size - a small page is not automatically a thumbnail.
    P.reduced = (one(B, f, T_NEWSUBFILE, 0) & 1u) != 0;

    // Tiles first: the tag is what decides, before any strip tag is consulted,
    // because a tiled file HAS no strips and the strip refusal would be wrong.
    if (find(f, T_TILEWIDTH) || find(f, T_TILEOFFSETS)) {
        err = "tiled layout (TileWidth " + num(one(B, f, T_TILEWIDTH, 0)) +
              ", TileLength " + num(one(B, f, T_TILELENGTH, 0)) +
              "): this build reads strip layouts";
        return false;
    }

    bool gotW = false, gotH = false;
    P.w = (uint32_t)one(B, f, T_WIDTH, 0, &gotW);
    P.h = (uint32_t)one(B, f, T_HEIGHT, 0, &gotH);
    if (!gotW || !gotH || P.w == 0 || P.h == 0) {
        err = at + " does not say how big it is (ImageWidth/ImageLength)";
        return false;
    }
    if (P.w > (uint32_t)MAX_DIM || P.h > (uint32_t)MAX_DIM) {
        err = "unsupported image size (" + num(P.w) + "x" + num(P.h) + ")";
        return false;
    }

    P.ch = (uint32_t)one(B, f, T_SAMPLES, 1);
    if (P.ch < 1 || P.ch > 16) {
        err = at + " says " + num(P.ch) + " samples per pixel";
        return false;
    }

    // Every sample the same width and the same format. A file that mixes them
    // (8,8,4 is legal TIFF) is refused rather than read at one of the widths.
    std::vector<uint64_t> bps, sfv;
    const Field* fb = find(f, T_BITS);
    if (!fb || !fieldValues(B, *fb, bps) || bps.empty()) {
        err = at + " does not say how many bits a sample has (BitsPerSample)";
        return false;
    }
    for (uint64_t b : bps)
        if (b != bps[0]) {
            err = at + " gives its samples different widths (BitsPerSample " +
                  num(bps[0]) + " and " + num(b) + "): this build reads one width for all";
            return false;
        }
    P.bits = (uint32_t)bps[0];
    const Field* fs = find(f, T_SAMPLEFORMAT);
    if (fs && fieldValues(B, *fs, sfv) && !sfv.empty()) {
        for (uint64_t s : sfv)
            if (s != sfv[0]) {
                err = at + " gives its samples different formats (SampleFormat " +
                      num(sfv[0]) + " and " + num(s) + "): this build reads one format for all";
                return false;
            }
        P.sf = sfv[0];
    }
    const bool depthOk = (P.bits == 8 && P.sf == 1) || (P.bits == 16 && P.sf == 1) ||
                         (P.bits == 32 && P.sf == 3);
    if (!depthOk) {
        err = "BitsPerSample " + num(P.bits) + " with SampleFormat " + num(P.sf) +
              " (" + sampleFormatName(P.sf) + "): this build reads 8- and 16-bit "
              "unsigned integer and 32-bit IEEE float";
        return false;
    }

    bool gotPhoto = false;
    P.photometric = one(B, f, T_PHOTOMETRIC, 0, &gotPhoto);
    if (!gotPhoto) {
        err = at + " does not say what its samples mean (no PhotometricInterpretation)";
        return false;
    }
    if (P.photometric != 0 && P.photometric != 1 && P.photometric != 2) {
        err = "photometric " + num(P.photometric) + " (" + photometricName(P.photometric) + "): " +
              (P.photometric == 32803 || P.photometric == 34892
                   ? std::string("the colour filter pattern has to be read exactly or not at "
                                 "all, and this build does not read it")
                   : std::string("this build reads 0 (WhiteIsZero), 1 (BlackIsZero) and 2 (RGB)"));
        return false;
    }
    const uint32_t wantMin = P.photometric == 2 ? 3 : 1;
    if (P.ch < wantMin || P.ch > wantMin + 1) {
        err = "photometric " + num(P.photometric) + " (" + photometricName(P.photometric) +
              ") with " + num(P.ch) + " samples per pixel: this build reads " +
              num(wantMin) + " or " + num(wantMin + 1) + " there";
        return false;
    }

    const uint64_t planar = one(B, f, T_PLANAR, 1);
    if (planar != 1) {
        err = "PlanarConfiguration " + num(planar) +
              " (one plane per sample): this build reads 1 (samples interleaved)";
        return false;
    }

    P.compression = one(B, f, T_COMPRESSION, 1);
    if (P.compression != 1 && P.compression != 5 && P.compression != 8 &&
        P.compression != 32773 && P.compression != 32946) {
        err = "compression " + num(P.compression) + " (" + compressionName(P.compression) +
              "): this build reads 1 (none), 5 (LZW), 8 and 32946 (Deflate) and "
              "32773 (PackBits)";
        return false;
    }

    P.predictor = one(B, f, T_PREDICTOR, 1);
    if (P.predictor != 1 && P.predictor != 2) {
        err = "Predictor " + num(P.predictor) +
              (P.predictor == 3 ? " (floating point)" : "") +
              ": this build reads 1 (none) and 2 (horizontal differencing)";
        return false;
    }
    if (P.predictor == 2 && P.sf != 1) {
        err = "Predictor 2 (horizontal differencing) on " + num(P.bits) + "-bit " +
              sampleFormatName(P.sf) + " samples: differencing is defined for integers";
        return false;
    }

    // Strips. RowsPerStrip's default is "the whole image in one", which is what
    // a writer that omits it means.
    P.rowsPerStrip = one(B, f, T_ROWSPERSTRIP, P.h);
    if (P.rowsPerStrip == 0 || P.rowsPerStrip > P.h) P.rowsPerStrip = P.h;
    const uint64_t nstrips = (P.h + P.rowsPerStrip - 1) / P.rowsPerStrip;
    const Field* fo = find(f, T_STRIPOFFSETS);
    if (!fo || !fieldValues(B, *fo, P.offs) || P.offs.size() != nstrips) {
        err = at + " needs " + num(nstrips) + " StripOffsets and has " +
              num(fo ? P.offs.size() : 0);
        return false;
    }
    const Field* fc = find(f, T_STRIPBYTES);
    const uint64_t rowBytes = (uint64_t)P.w * P.ch * (P.bits / 8);
    if (fc && fieldValues(B, *fc, P.counts) && P.counts.size() == nstrips) {
        // as declared
    } else if (!fc && P.compression == 1) {
        P.counts.clear();                       // derivable, and only here
        for (uint64_t s = 0; s < nstrips; s++) {
            const uint64_t rows = P.rowsPerStrip * (s + 1) <= P.h
                                      ? P.rowsPerStrip : P.h - P.rowsPerStrip * s;
            P.counts.push_back(rows * rowBytes);
        }
    } else {
        err = at + " needs " + num(nstrips) + " StripByteCounts and has " +
              num(fc ? P.counts.size() : 0);
        return false;
    }
    for (uint64_t s = 0; s < nstrips; s++)
        if (P.counts[(size_t)s] > B.n || P.offs[(size_t)s] > B.n - P.counts[(size_t)s]) {
            err = at + " strip " + num(s + 1) + " runs past the end of the file";
            return false;
        }
    return true;
}

// One page's pixels. Only reached for a page parsePage accepted, so everything
// here is arithmetic on facts, and the only failures left are the compressed
// streams themselves.
bool decodePixels(const Bytes& B, const Page& P, Image& out, std::string& err) {
    const uint32_t sampPerRow = P.w * P.ch;
    const size_t rowBytes = (size_t)sampPerRow * (P.bits / 8);
    out.w = (int)P.w;
    out.h = (int)P.h;
    out.ch = (int)P.ch;
    out.dtype = P.bits == 8 ? "u8" : P.bits == 16 ? "u16" : "f32";
    out.data.assign((size_t)P.w * P.h * P.ch, 0.0f);

    std::vector<uint8_t> strip;
    std::vector<uint32_t> line(sampPerRow);            // predictor scratch
    const uint32_t mask = P.bits == 8 ? 0xffu : P.bits == 16 ? 0xffffu : 0xffffffffu;
    for (size_t s = 0; s < P.offs.size(); s++) {
        const uint64_t y0 = (uint64_t)s * P.rowsPerStrip;
        const uint64_t rows = y0 + P.rowsPerStrip <= P.h ? P.rowsPerStrip : P.h - y0;
        const size_t want = (size_t)rows * rowBytes;
        const uint8_t* src = B.p + P.offs[s];
        const size_t srcN = (size_t)P.counts[s];
        const uint8_t* raw = nullptr;
        if (P.compression == 1) {
            if (srcN < want) {
                err = "strip " + num(s + 1) + " holds " + num(srcN) + " bytes and its rows "
                      "need " + num(want);
                return false;
            }
            raw = src;
        } else {
            strip.assign(want, 0);
            bool ok = P.compression == 32773 ? packbitsInto(src, srcN, strip.data(), want, err)
                    : P.compression == 5     ? lzwInto(src, srcN, strip.data(), want, err)
                                             : inflateInto(src, srcN, strip.data(), want, err);
            if (!ok) return false;
            raw = strip.data();
        }
        for (uint64_t r = 0; r < rows; r++) {
            const uint8_t* rp = raw + (size_t)r * rowBytes;
            float* dp = &out.data[(size_t)(y0 + r) * sampPerRow];
            if (P.bits == 32) {                        // IEEE float, as stored
                for (uint32_t j = 0; j < sampPerRow; j++) {
                    const uint8_t* q = rp + (size_t)j * 4;
                    const uint32_t u = B.be
                        ? ((uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3])
                        : ((uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0]);
                    float v;
                    memcpy(&v, &u, 4);
                    dp[j] = v;
                }
                continue;
            }
            for (uint32_t j = 0; j < sampPerRow; j++)
                line[j] = P.bits == 8 ? rp[j]
                        : B.be ? (uint32_t)((rp[2 * j] << 8) | rp[2 * j + 1])
                               : (uint32_t)((rp[2 * j + 1] << 8) | rp[2 * j]);
            if (P.predictor == 2)                      // each sample is a step from
                for (uint32_t j = P.ch; j < sampPerRow; j++)   // the one CH back
                    line[j] = (line[j] + line[j - P.ch]) & mask;
            for (uint32_t j = 0; j < sampPerRow; j++) dp[j] = (float)line[j];
        }
    }
    return true;
}

// The sentence the Inspector prints. It names the file's own facts (depth,
// colour, byte order, compression, layout) and the ONE thing this reader did to
// the numbers that is not simply reading them (the predictor), because rule 2
// is that what was done is said.
std::string pageNote(const Page& P, const Bytes& B, int pageOf, int dropped) {
    std::string s = "TIFF";
    if (pageOf > 1) s += " page " + num((uint64_t)P.number) + " of " + num((uint64_t)pageOf);
    s += P.bits == 32 ? ", 32-bit float " : P.bits == 16 ? ", 16-bit " : ", 8-bit ";
    if (P.photometric == 2) s += P.ch == 4 ? "RGBA" : "RGB";
    else                    s += P.ch == 2 ? "greyscale+alpha" : "greyscale";
    if (B.be) s += ", big-endian (MM)";
    s += ", ";
    s += P.compression == 1 ? "no compression"
       : P.compression == 5 ? "LZW"
       : P.compression == 32773 ? "PackBits" : "Deflate";
    if (P.predictor == 2) s += ", Predictor 2 (horizontal differencing) undone";
    s += ", " + num(P.offs.size()) + (P.offs.size() == 1 ? " strip" : " strips");
    // Photometric 0 means 0 is WHITE. Inverting would make the picture look
    // right and every measurement wrong, so the samples are left alone and the
    // fact is carried here instead - the reader of a histogram needs it.
    if (P.photometric == 0)
        s += "; photometric 0 (WhiteIsZero): 0 is white, and the samples are NOT inverted";
    if (dropped > 0)
        s += "; " + num((uint64_t)dropped) + (dropped == 1 ? " reduced-resolution page"
                                                           : " reduced-resolution pages") +
             " (NewSubfileType bit 0) " + (dropped == 1 ? "is" : "are") +
             " not a frame of the stack";
    return s;
}

}  // namespace

bool tiffDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err) {
    Bytes B;
    B.p = p;
    B.n = n;
    if (n < 8) { err = "the file is too short to hold a TIFF header"; return false; }
    if (p[0] == 'M' && p[1] == 'M') B.be = true;
    else if (!(p[0] == 'I' && p[1] == 'I')) {
        err = "the byte order mark is neither II nor MM";
        return false;
    }
    uint32_t magic = 0;
    B.u16v(2, magic);
    if (magic == 43) {
        err = "BigTIFF (magic 43): this build reads classic TIFF (magic 42)";
        return false;
    }
    if (magic != 42) { err = "magic " + num(magic) + " is not TIFF's 42"; return false; }
    uint32_t first = 0;
    B.u32v(4, first);

    // ---- the pages, as a chain -------------------------------------------
    // Walked before anything is decoded, with the offsets already seen kept, so
    // a file whose IFDs point at each other is refused instead of read forever.
    std::vector<size_t> ifds;
    std::set<uint32_t> seen;
    for (uint32_t off = first; off; ) {
        if (!seen.insert(off).second) {
            err = "the pages loop: the IFD at offset " + num(off) + " is reached twice";
            return false;
        }
        if (ifds.size() >= MAX_PAGES) {
            err = "more than " + num(MAX_PAGES) + " pages";
            return false;
        }
        uint32_t cnt = 0;
        if (!B.u16v(off, cnt) || (uint64_t)off + 2 + (uint64_t)cnt * 12 + 4 > n) {
            err = "the page directory at offset " + num(off) + " runs past the end of the file";
            return false;
        }
        ifds.push_back(off);
        uint32_t nxt = 0;
        B.u32v((size_t)off + 2 + (size_t)cnt * 12, nxt);
        off = nxt;
    }
    if (ifds.empty()) { err = "the file holds no pages"; return false; }

    // ---- every page's header, before any page's pixels --------------------
    std::vector<Page> pages;
    for (size_t i = 0; i < ifds.size(); i++) {
        uint32_t cnt = 0;
        B.u16v(ifds[i], cnt);
        std::vector<Field> fields;
        fields.reserve(cnt);
        for (uint32_t k = 0; k < cnt; k++) {
            const size_t e = ifds[i] + 2 + (size_t)k * 12;
            uint32_t tag = 0, type = 0, count = 0, val = 0;
            B.u16v(e, tag);
            B.u16v(e + 2, type);
            B.u32v(e + 4, count);
            Field f;
            f.tag = (uint16_t)tag;
            f.type = (uint16_t)type;
            f.count = count;
            const uint64_t bytes = (uint64_t)typeSize(f.type) * count;
            if (bytes <= 4) f.off = e + 8;
            else { B.u32v(e + 8, val); f.off = val; }
            fields.push_back(f);
        }
        Page P;
        if (!parsePage(B, fields, (int)i + 1, P, err)) return false;
        pages.push_back(std::move(P));
    }

    // A thumbnail is not a frame of a measurement. Which pages say so is read
    // from NewSubfileType and never inferred, and how many were left out is
    // carried into the note of the frame that is kept.
    std::vector<const Page*> keep;
    int dropped = 0;
    for (const Page& P : pages) {
        if (P.reduced) dropped++;
        else keep.push_back(&P);
    }
    if (keep.empty()) {
        err = "every one of its " + num(pages.size()) +
              " pages declares itself a reduced-resolution copy (NewSubfileType bit 0)";
        return false;
    }

    // A multi-page TIFF is a stack, and a stack's frames are one grid: pages
    // that disagree are named rather than stacked into something ragged.
    for (size_t i = 1; i < keep.size(); i++) {
        const Page& a = *keep[0];
        const Page& b = *keep[i];
        if (b.w != a.w || b.h != a.h || b.ch != a.ch || b.bits != a.bits || b.sf != a.sf) {
            err = "page " + num((uint64_t)b.number) + " is " + num(b.w) + "x" + num(b.h) +
                  " " + num(b.bits) + "-bit x" + num(b.ch) + " and page " +
                  num((uint64_t)a.number) + " is " + num(a.w) + "x" + num(a.h) + " " +
                  num(a.bits) + "-bit x" + num(a.ch) +
                  ": a multi-page TIFF is a stack, and a stack's frames are one shape";
            return false;
        }
    }

    const uint64_t samples = (uint64_t)keep[0]->w * keep[0]->h * keep[0]->ch * keep.size();
    if (samples > MAX_SAMPLES) {
        err = "its " + num(keep.size()) + " pages hold " + num(samples) +
              " samples: more than this build will hold at once (" + num(MAX_SAMPLES) + ")";
        return false;
    }

    out.clear();
    out.reserve(keep.size());
    for (size_t i = 0; i < keep.size(); i++) {
        Image img;
        if (!decodePixels(B, *keep[i], img, err)) {
            err = "page " + num((uint64_t)keep[i]->number) + ": " + err;
            return false;
        }
        img.note = pageNote(*keep[i], B, (int)keep.size() > 1 ? (int)pages.size() : 1,
                            i == 0 ? dropped : 0);
        out.push_back(std::move(img));
    }
    return true;
}

}  // namespace imagefile
