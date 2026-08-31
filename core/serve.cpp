// `viewer --serve`: the process ssh starts on the machine that holds the data.
//
// No window, no GL, no listening socket - it reads requests from stdin and writes
// replies to stdout. It never sends a whole image unless a whole image is what is
// on screen: it seeks to the requested rows and decimates while reading, so the
// cost of looking at a 12 Mpx frame over a link is the cost of the pane it is
// displayed in.
#include "remote_proto.h"
#include "plugin_host.h"
#include "setfold.h"                 // the fold half of a set analysis, shared with the viewer
#include "version.h"                 // the commit this peer was built from (provenance)
#include "adapter.h"                 // running the reader harness here (issue #180)
#include "vstream.h"                 // what it writes, and the tree check both ends use
#include "npzfile.h"                 // the zip walk and the header peek both ends use

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <csignal>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>             // groupNumbered's buckets, by key and not by scan
#include <unordered_set>             // ...and "does this digit run vary at all"
#include <vector>

#include "imagefile.h"           // #148 B: the peer decodes what the viewer decodes
#include "miniz.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>                 // _getpid: the scratch directory a run owns
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>                  // getpid, same
#endif

namespace rp {

static const char* DT_NAMES[DT_COUNT] = { "u8", "i8", "u16", "i16", "u32", "i32",
                                         "f32", "f64", "f16" };
const char* dtypeName(uint32_t t) { return t < DT_COUNT ? DT_NAMES[t] : "?"; }
uint32_t dtypeFromName(const char* s) {
    for (uint32_t i = 0; i < DT_COUNT; i++) if (!strcmp(s, DT_NAMES[i])) return i;
    return DT_COUNT;
}

// ---------------------------------------------------------------- payload codec
struct Buf {
    std::vector<uint8_t> b;
    size_t rd = 0;
    void putU32(uint32_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
    void putF64(double v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 8); }
    void putStr(const std::string& s) {
        putU32((uint32_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    void putBlob(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    bool getU32(uint32_t& v) {
        if (rd + 4 > b.size()) return false;
        memcpy(&v, b.data() + rd, 4); rd += 4; return true;
    }
    bool getStr(std::string& s) {
        uint32_t n;
        if (!getU32(n) || rd + n > b.size()) return false;
        s.assign((const char*)b.data() + rd, n); rd += n; return true;
    }
};

// ---------------------------------------------------------------- transport
// Replies are handed to whatever transport is in use. Today that is the ssh
// stdio pipe; a WebSocket server would set a different sink and reuse every
// handler unchanged.
using ReplySink = bool (*)(uint32_t type, const Buf& payload);
static ReplySink g_sink = nullptr;

static bool readExact(void* p, size_t n) {
    uint8_t* q = (uint8_t*)p;
    while (n) {
        size_t got = fread(q, 1, n, stdin);
        if (got == 0) return false;
        q += got; n -= got;
    }
    return true;
}
static bool writeExact(const void* p, size_t n) {
    return n == 0 || fwrite(p, 1, n, stdout) == n;
}
static bool sendStdio(uint32_t type, const Buf& payload) {
    Header h{ MAGIC, type, (uint32_t)payload.b.size() };
    if (!writeExact(&h, sizeof h)) return false;
    if (!writeExact(payload.b.data(), payload.b.size())) return false;
    return fflush(stdout) == 0;
}
static bool sendMsg(uint32_t type, const Buf& payload) {
    return g_sink ? g_sink(type, payload) : false;
}
static bool sendErr(const std::string& msg) {
    Buf p; p.putStr(msg);
    return sendMsg(MSG_ERR, p);
}

// ---------------------------------------------------------------- one file
// TWO BRANCHES, AND THEY COST DIFFERENT THINGS (issue #148, judgment B).
//
// .npy is a READER, not a loader: openNpy parses the header and readRegion
// seeks, so nothing here ever holds a whole frame. That is the entire point of
// serving remotely and it does not change.
//
// The picture formats cannot be read that way. A PNG is one deflate stream, a
// JPEG is entropy-coded, a strip TIFF is compressed per strip and an .exr is
// compressed per scanline block: there is no offset to compute for "row y,
// column x" without decoding what came before it. So they are DECODED WHOLE and
// the requested rect is cropped and strided out of the decoded frames. The peer
// holds one file's pictures for the lifetime of one request, which is the trade
// - and the thing that does NOT change is what LEAVES the peer: the rect that
// was asked for, at the step that was asked for, in the file's own dtype.
//
// One consequence is worth stating where it will be read: a request that walks
// N frames of ONE file (MEASURE over a multi-page TIFF) decodes that file ONCE,
// because the decode lives on this struct and the struct outlives the loop. A
// request that walks N SEPARATE files decodes each once, which is the floor.
struct ServedFile {
    // ---- the picture branch: empty for .npy ---------------------------------
    // The frames of ONE stack. imagefile hands back several Images when a file
    // holds several pictures; core/app/loader_npz.inc's loadImageFile turns the
    // UNNAMED ones into a stack's frames and the NAMED ones (an .exr's layers)
    // into separate documents. The wire addresses a FILE, so only the first of
    // those two is servable and the second is refused by name - see openPicture.
    std::vector<imagefile::Image> pics;
    std::ifstream f;
    uint32_t dtype = DT_COUNT;
    bool bigEndian = false, fortran = false;
    int w = 0, h = 0, ch = 1, frames = 1;
    // shape as declared in the file, before the w/h/ch/frames interpretation:
    // the LIST metadata shows what the file says, not what the viewer made of it
    int ndim = 0;
    uint32_t odims[4] = { 0, 0, 0, 0 };
    uint64_t dataOffset = 0;
    size_t elemSize = 0;
    bool ok = false;
    // element strides for each logical axis, so a Fortran-order file reads
    // through the same code as a C-order one (the local loader has always
    // handled Fortran; refusing it only over the link was an inconsistency)
    uint64_t sFrame = 0, sY = 0, sX = 0, sCh = 1;
    // the DECLARED reading did not fit this rank, so native was used instead
    // (issue #124; the local npyLayout's "read natively" note is the same fact)
    bool readFellBack = false;
};

// The layout decision, under a DECLARED reading (issue #124, docs/
// docs/features/adapters/input-adapters.md §3.3). This is core/app/loader_npz.inc's npyLayout, which
// the peer had no equivalent of: openNpy hard-coded the native
// interpretation, so the escape hatch the Inspector offers for a file opened
// through File > Open did not exist for the same file opened through a peer.
//
// It is written as ONE assignment through rp::npyReadAxes rather than as the
// four hand-rolled cases that were here, because there are now twelve
// (rank 2/3/4 x four readings) and hand-rolling twelve is how the two doors
// disagreed the first time. The axis rule, the native rule and both refusal
// sentences all come from remote_proto.h, so what this function contributes is
// only the STRIDES - which is the one thing the local npyLayout cannot lend,
// since a peer computes them from the file's own fortran flag and reads bytes
// through them, while the client's are into a decoded buffer.
static bool serveLayout(ServedFile& n, const std::vector<int64_t>& dims, int read,
                        bool& fellBack, std::string& err) {
    fellBack = false;
    const size_t rank = dims.size();
    if (rank < 2 || rank > 4) { err = npyNotNativeText(dims); return false; }
    int r = read;
    // A declaration that does not fit this rank falls back to native and SAYS
    // so - word for word what the local npyLayout does, including for rank 2,
    // which has no choice to make. Matching matters more than the refusal I
    // first wrote here: a session restored against a file that was rewritten
    // from (H,W,3) to (F,H,W,C) must open the same way through a peer as
    // through File > Open, and "this door refuses, that one opens with a note"
    // is exactly the disagreement #71 was about. The client turns fellBack into
    // the same note the local decoder writes.
    //
    // It is never reached from the menu on either side: npyReadOptions only
    // offers readings the shape permits. What reaches it is a stale session
    // line, which is precisely the case that must not silently lie.
    if (r != NR_NATIVE) {
        int a, b, c, d;
        if (!npyReadAxes(rank, r, a, b, c, d)) { fellBack = true; r = NR_NATIVE; }
    }
    int iF = -1, iH = -1, iW = -1, iC = -1;
    if (rank == 2) {                          // (H,W): one frame, one channel
        iH = 0; iW = 1;
    } else {
        if (r == NR_NATIVE) r = npyNativeRead(dims);
        npyReadAxes(rank, r, iF, iH, iW, iC);
    }
    n.frames = iF >= 0 ? (int)dims[iF] : 1;
    n.h      = (int)dims[iH];
    n.w      = (int)dims[iW];
    n.ch     = iC >= 0 ? (int)dims[iC] : 1;
    // The ceiling is tested on the DECLARED 64-bit extent, not on the truncated
    // int, so that the sentence quotes the number the file actually says and is
    // the local door's sentence character for character (the refusal selftest
    // asserts that equality, and a (2^32+3) channel axis must not pass as 3).
    const int64_t c64 = iC >= 0 ? dims[iC] : 1;
    if (c64 > 4) { err = npyChannelCeilingText(dims, c64); return false; }
    // A malformed header must produce an error message, not a std::length_error
    // that terminates the peer: negative dims flow into size arithmetic as 2^64.
    // Still one guard over every axis, and it NAMES the shape.
    if (n.w <= 0 || n.h <= 0 || n.w > (1 << 20) || n.h > (1 << 20) ||
        n.ch < 1 || n.frames < 1 || n.frames > (1 << 20)) {
        err = npyShapeText(dims) + " has an axis this cannot read: "
              "every axis must be at least 1, and H and W at most " +
              std::to_string(1 << 20);
        return false;
    }
    // Per-axis element strides, computed for the DECLARED axes and then picked
    // off by role. C order: the last dimension is fastest. Fortran order: the
    // first. Everything downstream indexes through these four numbers, so the
    // two layouts - and now the four readings - differ nowhere else.
    std::vector<uint64_t> ax(rank, 0);
    uint64_t acc = 1;
    if (n.fortran) { for (size_t i = 0; i < rank; i++)  { ax[i] = acc; acc *= (uint64_t)dims[i]; } }
    else           { for (size_t i = rank; i-- > 0; )   { ax[i] = acc; acc *= (uint64_t)dims[i]; } }
    n.sFrame = iF >= 0 ? ax[iF] : 0;
    n.sY     = ax[iH];
    n.sX     = ax[iW];
    n.sCh    = iC >= 0 ? ax[iC] : 1;
    if (!n.sCh) n.sCh = 1;                    // 2D / (F,H,W): single channel
    if (!n.sFrame) n.sFrame = (uint64_t)n.w * n.h * n.ch;   // single-frame file
    return true;
}

// A numpy descr code, byte order already stripped, as an rp::DType. DT_COUNT =
// this peer does not serve that type.
//
// Its own function since issue #180: a reader's framed stream declares its
// blobs with the SAME two-letter codes a .npy header uses, and the peer now
// reads both. `b1 -> DT_U8` is kept exactly where it was rather than being
// widened into a wire type of its own - one bool per byte IS a u8 on the wire,
// and the client's own decoder makes the same call.
static uint32_t npyTypeCode(const std::string& t) {
    static const struct { const char* np; uint32_t dt; } MAP[] = {
        { "u1", DT_U8 }, { "i1", DT_I8 }, { "u2", DT_U16 }, { "i2", DT_I16 },
        { "u4", DT_U32 }, { "i4", DT_I32 }, { "f4", DT_F32 }, { "f8", DT_F64 },
        { "b1", DT_U8 },
    };
    for (const auto& m : MAP) if (t == m.np) return m.dt;
    return DT_COUNT;
}

static bool openNpy(ServedFile& n, const std::string& path, std::string& err, int read) {
    n.f.open(std::filesystem::u8path(path), std::ios::binary);
    if (!n.f) { err = "cannot open " + path; return false; }
    char magic[6];
    n.f.read(magic, 6);
    if (memcmp(magic, "\x93NUMPY", 6) != 0) { err = "not a .npy file"; return false; }
    uint8_t major = 0, minor = 0;
    n.f.read((char*)&major, 1); n.f.read((char*)&minor, 1);
    uint32_t hlen = 0;
    if (major == 1) { uint16_t l = 0; n.f.read((char*)&l, 2); hlen = l; }
    else            { n.f.read((char*)&hlen, 4); }
    // The non-v1 branch takes a RAW u32 straight off the file and used to size
    // a std::string with it, unbounded, with no read check. A 12-byte file
    // whose header length reads 0xffffffff therefore cost 4 GB of commit and
    // ~1.8 s on EVERY LIST of its directory (measured against the real peer),
    // up to 256 such files per LIST and 512 per SCAN - and on a memory-capped
    // box the peer is OOM-killed and the session dies for everyone using it.
    // The client's two .npy header readers have always bounded this; the peer
    // did not. 64 KB is why numpy minted the 2.0 format at all, so the bound
    // rejects nothing legitimate.
    if (hlen == 0 || hlen > 65536) { err = "bad .npy header length"; return false; }
    std::string hdr(hlen, '\0');
    n.f.read(&hdr[0], hlen);
    if (n.f.gcount() != (std::streamsize)hlen) { err = "truncated .npy header"; return false; }
    n.dataOffset = (uint64_t)n.f.tellg();

    size_t dp = hdr.find("'descr'");
    size_t fp = hdr.find("'fortran_order'");
    size_t sp = hdr.find("'shape'");
    if (dp == std::string::npos || sp == std::string::npos) { err = "bad .npy header"; return false; }
    size_t q1 = hdr.find('\'', hdr.find(':', dp)), q2 = hdr.find('\'', q1 + 1);
    std::string descr = hdr.substr(q1 + 1, q2 - q1 - 1);
    n.fortran = fp != std::string::npos && hdr.find("True", fp) != std::string::npos &&
                hdr.find("True", fp) < hdr.find(',', fp);
    n.bigEndian = !descr.empty() && descr[0] == '>';
    std::string t = descr.substr(descr.find_first_of("<>|=") == std::string::npos ? 0 : 1);
    n.dtype = npyTypeCode(t);
    if (n.dtype == DT_COUNT) { err = "unsupported dtype " + descr; return false; }
    n.elemSize = dtypeSize(n.dtype);

    // int64_t rather than long long so this vector is the type
    // rp::npyShapeText takes: on Linux int64_t is `long`, so the two spellings
    // are DIFFERENT types there even though they are the same on Windows.
    std::vector<int64_t> dims;
    size_t open = hdr.find('(', sp), close = hdr.find(')', open);
    for (size_t i = open + 1; i < close; ) {
        while (i < close && (hdr[i] == ' ' || hdr[i] == ',')) i++;
        if (i >= close) break;
        dims.push_back(atoll(hdr.c_str() + i));
        while (i < close && hdr[i] != ',') i++;
    }
    n.ndim = (int)dims.size();
    for (size_t i = 0; i < dims.size() && i < 4; i++) n.odims[i] = (uint32_t)dims[i];
    // The interpretation, the ceiling and the axis guard now all live in
    // serveLayout, which takes the DECLARED reading (issue #124). What used to
    // be here was the native case written out by hand; §3.2's refusal
    // (rp::npyNotNativeText, the same sentence the local door uses - #71 D4,
    // asserted by R3 below and by V22c on the local side) and the C > 4 ceiling
    // are the same sentences, now said in one place because the peer has to say
    // them about a declared reading too.
    if (!serveLayout(n, dims, read, n.readFellBack, err)) return false;
    n.ok = true;
    return true;
}

// Read one decimated region of a .npy. Seeks per source row and takes every
// `step`th sample, so the bytes read scale with what is asked for, not with the
// frame.
static bool readNpyRegion(ServedFile& n, const TileReq& r, std::vector<uint8_t>& out,
                          uint32_t& outW, uint32_t& outH, std::string& err) {
    uint32_t step = std::max(1u, r.step);
    uint32_t x0 = std::min(r.x, (uint32_t)n.w), y0 = std::min(r.y, (uint32_t)n.h);
    // 64-bit sums: x + w wrapping at 2^32 must clamp to the edge, not go empty
    uint32_t x1 = (uint32_t)std::min<uint64_t>((uint64_t)r.x + r.w, (uint64_t)n.w);
    uint32_t y1 = (uint32_t)std::min<uint64_t>((uint64_t)r.y + r.h, (uint64_t)n.h);
    if (x1 <= x0 || y1 <= y0) { err = "empty region"; return false; }
    outW = (x1 - x0 + step - 1) / step;
    outH = (y1 - y0 + step - 1) / step;
    size_t px = n.elemSize * n.ch;
    out.resize((size_t)outW * outH * px);

    uint32_t frame = std::min<uint32_t>(r.frame, (uint32_t)n.frames - 1);
    const size_t es = n.elemSize;
    uint8_t* dst = out.data();
    const bool rowContiguous = n.sCh == 1 && n.sX == (uint64_t)n.ch &&
                               n.sY == (uint64_t)n.w * n.ch;      // plain C order
    if (rowContiguous) {
        uint64_t base = n.dataOffset + (uint64_t)frame * n.sFrame * es;
        std::vector<uint8_t> row((size_t)(x1 - x0) * px);
        for (uint32_t y = y0, oy = 0; oy < outH; y += step, oy++) {
            n.f.seekg((std::streamoff)(base + ((uint64_t)y * n.w + x0) * px));
            n.f.read((char*)row.data(), (std::streamsize)row.size());
            if (!n.f) { err = "short read"; return false; }
            if (step == 1) {
                memcpy(dst, row.data(), row.size());
                dst += row.size();
            } else {
                for (uint32_t ox = 0; ox < outW; ox++) {
                    memcpy(dst, row.data() + (size_t)ox * step * px, px);
                    dst += px;
                }
            }
        }
    } else {
        // Fortran order (or any exotic axis order): read the byte extent the
        // region spans once, then gather through the strides. One read instead
        // of a seek per element, which is what makes this usable at all.
        uint64_t lo = UINT64_MAX, hi = 0;
        auto off = [&](uint64_t y, uint64_t x, uint64_t c) {
            return (uint64_t)frame * n.sFrame + y * n.sY + x * n.sX + c * n.sCh;
        };
        const uint64_t corners[4][2] = { { y0, x0 }, { y0, (uint64_t)x1 - 1 },
                                         { (uint64_t)y1 - 1, x0 },
                                         { (uint64_t)y1 - 1, (uint64_t)x1 - 1 } };
        for (const auto& cn : corners)
            for (uint64_t c = 0; c < (uint64_t)n.ch; c++) {
                uint64_t o = off(cn[0], cn[1], c);
                lo = std::min(lo, o);
                hi = std::max(hi, o);
            }
        uint64_t spanElems = hi - lo + 1;
        if (spanElems * es > ((uint64_t)1 << 30)) {
            err = "region spans too much of a non-contiguous (fortran-order) file";
            return false;
        }
        std::vector<uint8_t> blk((size_t)(spanElems * es));
        n.f.seekg((std::streamoff)(n.dataOffset + lo * es));
        n.f.read((char*)blk.data(), (std::streamsize)blk.size());
        if (!n.f) { err = "short read"; return false; }
        for (uint32_t y = y0, oy = 0; oy < outH; y += step, oy++)
            for (uint32_t x = x0, ox = 0; ox < outW; x += step, ox++)
                for (uint32_t c = 0; c < (uint32_t)n.ch; c++) {
                    uint64_t o = off(y, x, c) - lo;
                    memcpy(dst, blk.data() + (size_t)(o * es), es);
                    dst += es;
                }
    }
    if (n.bigEndian && n.elemSize > 1) {          // normalise on the server side
        for (size_t i = 0; i + n.elemSize <= out.size(); i += n.elemSize)
            std::reverse(out.begin() + i, out.begin() + i + n.elemSize);
    }
    return true;
}

// FRAME_001.NPY is still a .npy. The client lower-cases the same way
// (core/imagefile.cpp peerServes, through core/ui/menus.inc peerServesName) and
// the two must agree, or the client offers an open the peer then refuses.
static bool isNpySuffix(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".npy";
}

// What this peer will walk, group and decode. ONE predicate, and it is not this
// file's: core/imagefile.cpp owns it so that the client's Browse gate and the
// peer's own limit cannot become two lists (core/imagefile.h, issue #148).
static bool isServedSuffix(const std::string& name) { return imagefile::peerServes(name); }

// ------------------------------------------------------------ picture reading
//
// WHY float32 MAY BE INVERTED HERE AND NOWHERE ELSE.
//
// core/imagefile.h hands every decode back as float32 "values as stored", and
// remote_proto.h's opening paragraph forbids float32 on the wire for a measured
// reason: a 24-bit significand cannot hold u4/i4 above 2^24 or f8 at all, so a
// 16777217 would arrive as 16777216 and a 1e300 as inf.
//
// The inverse is exact for these formats, and that is arithmetic rather than
// convention. Every dtype they store is exactly representable in float32:
//
//   u8   0..255                 - 8 bits of significand needed
//   u16  0..65535               - 16 bits; 2^24 is 65536 times larger
//   f16  every half is a float  - core/exrread.cpp says so where it widens
//   f32  it IS the wire type
//
// So decode -> float32 -> the file's own dtype is a round trip that changes no
// bit, and the peer sends u16 for a 16-bit TIFF exactly as it sends u16 for a
// u2 .npy. The alternative - send f32 for everything - would also have made the
// Inspector print "f32" for a file the local door calls "u16", which is the
// same folder giving two answers that issue #148 is about.
static bool dtypeOfPicture(const std::string& name, uint32_t& dt) {
    // The names core/imagefile.h's backends produce are exactly the names on
    // the wire, DT_F16 included since protocol 10 - so no mapping table sits
    // between the two vocabularies and neither can drift.
    dt = dtypeFromName(name.c_str());
    return dt < DT_COUNT;
}

// One sample, float32 -> the file's dtype. The clamp can only fire on a value
// that was never in an integer file (nothing decodes a u8 to 300), and it is
// here so that a future backend cannot turn a bad decode into an out-of-range
// store; the bit-identical local-vs-peer assertion in --fmtgate-selftest is
// what proves it never fires on the formats that are actually served.
static inline void putSample(uint8_t* dst, uint32_t dtype, float v) {
    switch (dtype) {
        case DT_U8: {
            uint8_t q = (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
            memcpy(dst, &q, 1);
            break;
        }
        case DT_U16: {
            uint16_t q = (uint16_t)(v < 0.0f ? 0.0f : v > 65535.0f ? 65535.0f : v);
            memcpy(dst, &q, 2);
            break;
        }
        case DT_F16: {
            uint16_t q = floatToHalf(v);
            memcpy(dst, &q, 2);
            break;
        }
        default: memcpy(dst, &v, 4); break;            // f32, bit for bit
    }
}

static bool openPicture(ServedFile& n, const std::string& path, std::string& err, int read) {
    // A DECLARED READING (§3.3 / issue #124) is a .npy concept: it reinterprets
    // an ARRAY's axes, and a picture format declares its own geometry - a TIFF
    // page is a page and an .exr channel is a channel. Refused rather than
    // ignored, for the reason protocol 9 exists at all: a request whose trailer
    // is silently dropped comes back as a successful answer to a question that
    // was not asked.
    if (read != NR_NATIVE) {
        err = "a declared .npy reading does not apply to a picture format";
        return false;
    }
    if (!imagefile::peerServes(path)) { err = imagefile::peerRefusal(path); return false; }
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    if (sz < 0) { err = "cannot read " + path; return false; }
    // The same ceiling MSG framing carries, applied before the allocation: a
    // peer that OOMs on a listing takes the session down for everyone using it
    // (the .npy header-length bound above is the same lesson).
    if ((uint64_t)sz > (2ull << 30)) { err = "file is too large to decode (> 2 GiB)"; return false; }
    std::vector<uint8_t> bytes((size_t)sz);
    in.seekg(0);
    in.read((char*)bytes.data(), sz);
    if (in.gcount() != sz) { err = "short read on " + path; return false; }
    in.close();
    if (!imagefile::decode(path, bytes, n.pics, err)) return false;
    bytes.clear();
    bytes.shrink_to_fit();

    // NAMED parts are documents, not frames (core/imagefile.h). The wire
    // addresses a file and has no word for "the Z layer of it", so an .exr that
    // holds more than one is refused BY NAME rather than silently served as its
    // first layer - which would draw a plausible picture of the wrong channel.
    // One picture with a name is still one document, so it passes.
    if (n.pics.size() > 1) {
        std::string named;
        for (const auto& im : n.pics)
            if (!im.member.empty()) named += (named.empty() ? "" : ", ") + im.member;
        if (!named.empty()) {
            err = "this file holds named parts (" + named + "), which are separate "
                  "documents rather than frames - the link addresses a file, not a "
                  "part of one\n  open it on the machine that holds it, or copy it here";
            return false;
        }
    }
    const imagefile::Image& head = n.pics.front();
    for (const auto& im : n.pics)
        if (im.w != head.w || im.h != head.h || im.ch != head.ch || im.dtype != head.dtype) {
            err = "the pictures in this file differ in shape or dtype, so they are "
                  "not the frames of one stack";
            return false;
        }
    if (!dtypeOfPicture(head.dtype, n.dtype)) {
        err = "unsupported sample type " + head.dtype;
        return false;
    }
    n.w = head.w; n.h = head.h; n.ch = head.ch;
    n.frames = (int)n.pics.size();
    n.elemSize = dtypeSize(n.dtype);
    // ndim stays 0: MR_SHAPE is the DECLARED .npy shape, and a picture declares
    // none. Sending one would put a "read as (F,H,W)" menu on screen for a file
    // that has no other reading.
    n.ndim = 0;
    n.ok = true;
    return true;
}

// Crop and stride out of an ALREADY DECODED frame - the picture half of
// readNpyRegion, and the arithmetic clamping the rect is deliberately the same
// so a request that lands on the edge lands the same way on both branches.
static bool readPictureRegion(ServedFile& n, const TileReq& r, std::vector<uint8_t>& out,
                              uint32_t& outW, uint32_t& outH, std::string& err) {
    const uint32_t step = std::max(1u, r.step);
    const uint32_t x0 = std::min(r.x, (uint32_t)n.w), y0 = std::min(r.y, (uint32_t)n.h);
    const uint32_t x1 = (uint32_t)std::min<uint64_t>((uint64_t)r.x + r.w, (uint64_t)n.w);
    const uint32_t y1 = (uint32_t)std::min<uint64_t>((uint64_t)r.y + r.h, (uint64_t)n.h);
    if (x1 <= x0 || y1 <= y0) { err = "empty region"; return false; }
    outW = (x1 - x0 + step - 1) / step;
    outH = (y1 - y0 + step - 1) / step;
    const uint32_t frame = std::min<uint32_t>(r.frame, (uint32_t)n.frames - 1);
    const imagefile::Image& im = n.pics[frame];
    const size_t es = n.elemSize;
    const size_t px = es * (size_t)n.ch;
    out.resize((size_t)outW * outH * px);
    uint8_t* dst = out.data();
    for (uint32_t y = y0, oy = 0; oy < outH; y += step, oy++) {
        const float* row = im.data.data() + (size_t)y * im.w * im.ch;
        for (uint32_t x = x0, ox = 0; ox < outW; x += step, ox++)
            for (int c = 0; c < n.ch; c++, dst += es)
                putSample(dst, n.dtype, row[(size_t)x * im.ch + c]);
    }
    return true;
}

// ------------------------------------------------------------- the two doors
//
// EVERY call site goes through these, which is the point: the peer had exactly
// two functions that touched pixels, so teaching those two a second format
// teaches LIST, SCAN, META, TILE, MEASURE and the plugin mouth at once. TILE
// without MEASURE would be a peer that can show a TIFF and cannot measure it -
// two answers for one file, one level down from the issue that asked for this.
//
// Dispatch is by NAME, and it is the same order the local door uses
// (core/app/loader_npz.inc: .npy first, then imagefile::forPath). A name
// neither of them claims falls to the .npy branch, which opens it and says
// "not a .npy file" - unchanged, and still the right sentence for a file whose
// bytes nothing here recognises.
// A HEADERLESS file, read under a geometry the request carried (protocol 11).
//
// Simpler than openNpy, and that is the shape of the whole feature: there is no
// header to parse, because the client already parsed a human. Every field comes
// from the RawWire, the strides are plain C-order, and readNpyRegion therefore
// reads it with no raw-specific loop at all - the row-contiguous fast path, the
// endian normalisation and the step decimation are the code that was already
// there.
//
// The checks below REFUSE rather than clamp, for getRead's reason: a value this
// peer does not recognise means the two ends disagree about what the numbers
// mean, and the one answer that must not come back from that is a picture.
static bool openRaw(ServedFile& n, const std::string& path, std::string& err,
                    const RawWire& rw) {
    const uint32_t esz = rawDtypeSize(rw.dtype), ch = rawInterpCh(rw.interp);
    if (!esz) { err = "unknown raw dtype " + std::to_string(rw.dtype); return false; }
    if (!ch)  { err = "unknown raw interpretation " + std::to_string(rw.interp); return false; }
    if (rw.w < 1 || rw.h < 1 || rw.w > RAW_MAX_DIM || rw.h > RAW_MAX_DIM) {
        err = "raw size " + std::to_string(rw.w) + "x" + std::to_string(rw.h) +
              " is outside 1..32768";
        return false;
    }
    n.f.open(std::filesystem::u8path(path), std::ios::binary);
    if (!n.f) { err = "cannot open " + path; return false; }
    n.f.seekg(0, std::ios::end);
    const std::streamoff sz = n.f.tellg();
    if (sz < 0) { err = "cannot read " + path; return false; }
    // Both numbers, and the arithmetic that produced the second. The local
    // door's "file too small for this size/format" is enough when the operator
    // is looking at the file; over a link the LIST size can be stale, so the
    // figures are the diagnosis.
    const uint64_t need = (uint64_t)rw.offset + (uint64_t)rw.w * rw.h * ch * esz;
    if (need > (uint64_t)sz) {
        err = "file is " + std::to_string((uint64_t)sz) + " bytes: this recipe needs " +
              std::to_string(need) + " (offset " + std::to_string(rw.offset) + " + " +
              std::to_string(rw.w) + "x" + std::to_string(rw.h) + " x " +
              std::to_string(ch) + " ch x " + std::to_string(esz) + " B/sample)";
        return false;
    }
    static const uint32_t DT[] = { DT_U8, DT_U16, DT_F32, DT_F64 };
    n.dtype = DT[rw.dtype];
    n.elemSize = esz;
    n.w = (int)rw.w; n.h = (int)rw.h; n.ch = (int)ch;
    n.frames = 1;
    n.fortran = false;
    n.bigEndian = !(rw.flags & RW_LITTLE_ENDIAN);
    n.dataOffset = rw.offset;
    n.sCh = 1; n.sX = ch; n.sY = (uint64_t)rw.w * ch; n.sFrame = 0;
    // ndim stays 0: this file declares no shape of its own, so META carries no
    // MR_SHAPE and the Inspector offers no "read as" menu - the same reason a
    // picture keeps ndim 0. Restating a recipe is the recipe panel's job (#166),
    // not the .npy reading menu's.
    n.ndim = 0;
    n.ok = true;
    return true;
}

// ---------------------------------------------------------------- readers here
//
// docs/features/remote/remote-reader-design.md, issue #180 judgment B. An adapter runs where
// the file lives; what it produced stays here as a cache file, and the client
// reaches its pixels through the SAME two requests every other file uses.
//
// §2: the consent belongs to whoever started this process. Not an environment
// variable (ssh carries none), not a directory of blessed files (there is no
// file to bless - the reader is carried), and closed unless said.
static bool g_serveReaders = false;
void setServeReaders(bool on) { g_serveReaders = on; }
bool serveReadersOpen() { return g_serveReaders; }

// The same 64-bit FNV-1a-style recurrence the local adapter cache uses in
// core/app/session.inc (`adapterHash`). These are separate helpers because the
// two caches have different owners and inputs; keeping the algorithm aligned
// does not pretend their key formulas are interchangeable. The design named
// sha256; the implementation hashes the reader's whole TEXT, not its mtime, so
// "the same bytes" is what this key means and a module VERSION line is already
// inside the thing being hashed.
// This process's id, which several places below need in a file name so that
// two peers on one disk cannot write into one another's scratch.
static unsigned long servePid() {
#if defined(_WIN32)
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static uint64_t readerHash(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ~/.viewer-serve/reader-cache, or VIEWER_SERVE_CACHE. The peer has no settings
// window, so its SETTINGS are environment variables - the same call
// VIEWER_SERVE_PLUGINS made. Consent is the flag; this is not consent.
static std::string readerCacheDir() {
    std::error_code ec;
    std::filesystem::path d;
    if (const char* e = getenv("VIEWER_SERVE_CACHE")) {
        if (*e) d = std::filesystem::u8path(e);
    }
    if (d.empty()) {
        const char* home = getenv("HOME");
#if defined(_WIN32)
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home) d = std::filesystem::temp_directory_path(ec) / "viewer-serve";
        else                 d = std::filesystem::u8path(home) / ".viewer-serve";
        d /= "reader-cache";
    }
    std::filesystem::create_directories(d, ec);
    return d.u8string();
}

// A key names ONE file inside the cache directory and nothing else. It is hex
// because it is issued here and quoted back by the client (§4.2): a key that
// could carry a path would be a second way for a client to name a file on this
// disk, and there is no reason to build one.
static bool cacheFileFor(const std::string& key, const std::string& leaf, std::string& out) {
    if (key.empty() || key.size() > 64) return false;
    for (char c : key)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    const std::string dir = readerCacheDir();
    if (dir.empty()) return false;
    out = (std::filesystem::u8path(dir) / (key + leaf)).u8string();
    return true;
}
static bool readerCachePath(const std::string& key, std::string& out) {
    return cacheFileFor(key, ".vstream", out);
}

// The persistent rendezvous file for every mutation of one canonical cache
// entry. The file stays behind: unlinking a lock file after unlock creates two
// different inodes when a waiter already has the old one open and a third
// process opens the newly-created path. Keeping one inode per key is what makes
// this a lock rather than a best-effort marker.
static bool readerLockPath(const std::string& key, std::string& out) {
    return cacheFileFor(key, ".lock", out);
}

// An OS-owned, interprocess lock. The kernel releases it if viewer-serve exits
// or crashes, unlike an O_EXCL marker which can strand a key forever. It is
// deliberately held only while the canonical name is inspected or changed;
// Python writes to a per-run .part without it, so two expensive producers may
// overlap and the first complete result still wins.
class ReaderKeyLock {
public:
    ReaderKeyLock() = default;
    ReaderKeyLock(const ReaderKeyLock&) = delete;
    ReaderKeyLock& operator=(const ReaderKeyLock&) = delete;
    ~ReaderKeyLock() { release(); }

    bool acquire(const std::string& key, std::string& why) {
        std::string path;
        if (!readerLockPath(key, path)) {
            why = "cannot form the per-key cache lock path";
            return false;
        }
#if defined(_WIN32)
        handle_ = CreateFileW(std::filesystem::u8path(path).wstring().c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            why = "cannot open the per-key cache lock (Windows error " +
                  std::to_string((unsigned long)GetLastError()) + ")";
            return false;
        }
        OVERLAPPED ov{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
            why = "cannot take the per-key cache lock (Windows error " +
                  std::to_string((unsigned long)GetLastError()) + ")";
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
#else
        fd_ = open(std::filesystem::u8path(path).c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) {
            why = "cannot open the per-key cache lock: " +
                  std::string(std::strerror(errno));
            return false;
        }
        int rc;
        do { rc = flock(fd_, LOCK_EX); } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            why = "cannot take the per-key cache lock: " +
                  std::string(std::strerror(errno));
            close(fd_);
            fd_ = -1;
            return false;
        }
#endif
        return true;
    }

private:
    void release() {
#if defined(_WIN32)
        if (handle_ == INVALID_HANDLE_VALUE) return;
        OVERLAPPED ov{};
        UnlockFileEx(handle_, 0, 1, 0, &ov);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ < 0) return;
        flock(fd_, LOCK_UN);
        close(fd_);
        fd_ = -1;
#endif
    }

#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// ...and where a run WRITES before it is anything (#218 review r3794154621).
//
// Two viewer sessions asking the same uncached key at the same time are two
// viewer-serve processes, and both used to point their harness straight at the
// cache file. One would then validate, delete or overwrite what the other was
// still writing; when the two outputs happened to have compatible lengths the
// mixture even passed the structural check, and a peer answered with pixels
// that were half of one run and half of another.
//
// The leaf is UNIQUE to this process and this run and sits in the cache
// DIRECTORY, which is the point: a rename within one filesystem is atomic, so
// what appears under the key is a file that was already complete and already
// checked. Nothing is ever published half-written, and no contender can touch
// anything but its own temp.
static bool readerTempPath(const std::string& key, std::string& out) {
    static std::atomic<uint64_t> seq{ 0 };
    char leaf[64];
    snprintf(leaf, sizeof leaf, ".%lu.%llu.part", servePid(),
             (unsigned long long)(seq.fetch_add(1) + 1));
    return cacheFileFor(key, leaf, out);
}

// VIEWER_SERVE_CACHE_JUDGE_HOLD: a seam that stops this process between judging
// an existing cache entry unusable and taking the per-key lock that makes it
// safe to act on that judgement.
//
// The race it exists to test is a window of a few microseconds between two
// processes on one disk. A selftest that started two peers and hoped would be a
// test that passes because the machine was busy, so this is a HANDSHAKE rather
// than a sleep: the peer writes `<path>.at` to say it has judged and is
// waiting, and waits for `<path>` to appear before it goes on. The test then
// knows exactly when the stale observation exists and closes it deliberately.
// The handler MUST recheck after acquiring the lock. A minute,
// and then it proceeds anyway - a seam may not be a way to hang a peer.
//
// Absent everywhere else, read once, exactly as VIEWER_SERVE_LAG_MS and
// VIEWER_SERVE_PROTOCOL are.
static void serveCacheJudgeHold() {
    static const std::string path = [] {
        const char* e = getenv("VIEWER_SERVE_CACHE_JUDGE_HOLD");
        return std::string(e ? e : "");
    }();
    if (path.empty()) return;
    { std::ofstream f(std::filesystem::u8path(path + ".at"), std::ios::binary);
      f << "held\n"; }
    std::error_code e;
    for (int i = 0; i < 3000; i++) {
        if (std::filesystem::exists(std::filesystem::u8path(path), e)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// One node of a reader's output, laid out for readNpyRegion.
//
// This is openRaw's shape exactly and for openRaw's reason: the blob is C-order
// contiguous, so an offset and four strides are the whole of it and the
// decimating reader that already exists reads it with no reader-specific pixel
// loop anywhere. The GEOMETRY comes out of serveLayout - the same function, the
// same axis rule and the same two refusal sentences a loose .npy gets - so a
// reader's (F,H,W) is read here the way the client's own decoder reads it.
static bool openReaderCache(ServedFile& n, const std::string& key, uint32_t node,
                            std::string& err) {
    std::string path;
    if (!readerCachePath(key, path)) {
        err = "that is not a key this peer could have issued";
        return false;
    }
    n.f.open(std::filesystem::u8path(path), std::ios::binary);
    if (!n.f) {
        // Not "run the reader again": a key that resolves to nothing is a key
        // whose ISSUER is unknowable (rp::keyGoneText), and half of them came
        // from a container listing.
        err = rp::keyGoneText();
        return false;
    }
    vns::Scan sc;
    const std::string serr = vns::scanHeader(n.f, sc);
    if (!serr.empty()) { err = serr; return false; }
    const vns::Blob* b = nullptr;
    for (const vns::Blob& q : sc.blobs) if ((uint32_t)q.node == node) { b = &q; break; }
    if (!b) {
        err = "node " + std::to_string(node) + " of what the reader returned has no "
              "pixels (" + std::to_string(sc.blobs.size()) + " node(s) do)";
        return false;
    }
    n.dtype = npyTypeCode(b->dtype);
    if (n.dtype == DT_COUNT) { err = "unsupported dtype " + b->dtype; return false; }
    n.elemSize = dtypeSize(n.dtype);
    n.fortran = false;                 // the harness writes C order, LE, always
    n.bigEndian = false;               // (adapter-transport-review A2/A4)
    bool fellBack = false;
    if (!serveLayout(n, b->shape, NR_NATIVE, fellBack, err)) return false;
    uint64_t want = n.elemSize;
    for (int64_t d : b->shape) want *= (uint64_t)(d > 0 ? d : 0);
    if (want != b->nbytes) {
        err = "node " + std::to_string(node) + " declares " + std::to_string(b->nbytes) +
              " bytes and its shape needs " + std::to_string(want);
        return false;
    }
    n.f.clear();                       // scanHeader may have stopped at a limit
    n.dataOffset = b->at;
    // ndim stays 0 for openRaw's reason: what the reader RETURNED is not a file
    // that declares a shape a user may re-read another way. The §3.3 menu
    // belongs to the origin, and offering one over a materialisation would let
    // a person re-cut an array the reader already decided the meaning of.
    n.ndim = 0;
    n.ok = true;
    return true;
}

// ---------------------------------------------------------------- containers
//
// issue #217, docs/features/remote/remote-reader-design.md §10. A .npz is a zip of .npy
// members, so the peer's job is exactly two things it can do and the client
// cannot: LIST what is in the file, and turn ONE named member into bytes that
// readNpyRegion can decimate. Everything else - which member is pixels, which
// is an axis, what a picker row says, what tree a viewer container declares -
// stays on the client, where the one implementation of it already lives.
//
// The materialisation is a real .npy FILE in the same cache the reader uses,
// because that is what makes the pixel path free: a member written out is a
// .npy, and openNpy already reads a .npy with the right strides, the right
// fortran handling and the right byte order.

// What a key was issued FOR, written beside the members it names. A key is
// opaque and carries no path (§4.2), so the peer needs its own note of which
// file it listed - and it has to be on DISK rather than in this process,
// because the client fetches through more than one session: the UI thread's
// peer answers the open and the prefetch worker's peer, a different process
// entirely, answers the full-resolution swap. A key that only one of them could
// resolve would make refinement fail on exactly the frames it exists for.
struct NpzSrc {
    std::string path;
    uint64_t mtime = 0, fsize = 0;
};
static bool npzSrcWrite(const std::string& key, const NpzSrc& s) {
    std::string p;
    if (!cacheFileFor(key, ".npzsrc", p)) return false;
    std::ofstream f(std::filesystem::u8path(p), std::ios::binary);
    if (!f) return false;
    f << s.mtime << "\n" << s.fsize << "\n" << s.path << "\n";
    return (bool)f;
}
static bool npzSrcRead(const std::string& key, NpzSrc& s) {
    std::string p;
    if (!cacheFileFor(key, ".npzsrc", p)) return false;
    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    if (!f) return false;
    std::string a, b;
    if (!std::getline(f, a) || !std::getline(f, b) || !std::getline(f, s.path)) return false;
    s.mtime = strtoull(a.c_str(), nullptr, 10);
    s.fsize = strtoull(b.c_str(), nullptr, 10);
    return !s.path.empty();
}

// The peer's own stat, which is the only one that means anything: a client's
// stat of a NAS mounted twice makes identity a lie (§5.3, and §10.6 keeps it).
static void statPeerFile(const std::string& path, uint64_t& mtime, uint64_t& fsize) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(path);
    auto t = std::filesystem::last_write_time(p, ec);
    mtime = ec ? 0 : (uint64_t)t.time_since_epoch().count();
    ec.clear();
    auto s = std::filesystem::file_size(p, ec);
    fsize = ec ? 0 : (uint64_t)s;
}

// Whole-file read, with the ceiling openPicture already applies for the reason
// it applies it: a peer that OOMs takes the session down for everyone using it.
// A .npz is read whole here exactly as the local door reads it whole - the zip
// directory is at the END, so there is no streaming form of this - and the cost
// is paid ONCE per member because the member is then a cache file.
static bool readWholeInto(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    if (sz < 0) { err = "cannot read " + path; return false; }
    if ((uint64_t)sz > (2ull << 30)) { err = "file is too large to open (> 2 GiB)"; return false; }
    out.resize((size_t)sz);
    in.seekg(0);
    in.read((char*)out.data(), sz);
    if (in.gcount() != sz) { err = "short read on " + path; return false; }
    return true;
}

// ONE member of a listed .npz, as a ServedFile. Lazy and per member (§10.6):
// the first request for (key, node) inflates that one array and nothing else,
// so opening one member of a forty-member file costs one inflate.
static bool openNpzMember(ServedFile& n, const std::string& key, uint32_t node,
                          std::string& err) {
    NpzSrc src;
    if (!npzSrcRead(key, src)) {
        err = "this peer has not listed a container under that key - reopen the "
              "file to rescan it";
        return false;
    }
    std::string memberPath;
    if (!cacheFileFor(key, "-" + std::to_string(node) + ".npy", memberPath)) {
        err = "that is not a container key";
        return false;
    }
    std::error_code ec;
    // Already materialised: those bytes ARE the ones the key was issued for, so
    // they are served without touching the container again. Identity is bound
    // at materialisation, which is the local door's rule (a .npz is read whole
    // and the members opened from that copy) reached one member at a time.
    if (!std::filesystem::exists(std::filesystem::u8path(memberPath), ec))
    {
        // identity BEFORE the bytes, over a link (§10.6). Locally the whole zip
        // is in hand before a member is decoded, so an overwrite cannot land
        // between the two; here the read happens later than the listing did, so
        // the tuple is checked again and a change is REFUSED rather than
        // silently re-bound to whatever is on the disk now.
        uint64_t mtime = 0, fsize = 0;
        statPeerFile(src.path, mtime, fsize);
        if (mtime != src.mtime || fsize != src.fsize) {
            err = "the file changed on the peer since its members were listed - "
                  "reopen to rescan";
            return false;
        }
        std::vector<uint8_t> zip;
        if (!readWholeInto(src.path, zip, err)) return false;
        std::vector<nz::Entry> entries;
        if (!nz::list(zip, entries, err)) return false;
        if (node >= entries.size()) {
            err = "member " + std::to_string(node) + " is past the end of this "
                  "container (" + std::to_string(entries.size()) + " member(s))";
            return false;
        }
        std::vector<uint8_t> member;
        if (!nz::extract(zip, entries[node], member, err)) return false;
        // Written through a temporary and renamed: two sessions may materialise
        // the same member at the same moment, and a reader that met a half
        // written file would refuse a member that is perfectly good. The bytes
        // are identical either way - the key covers the path and the stat - so
        // the loser of the race simply overwrites with the same content.
        const std::string tmp = memberPath + ".part" +
                                std::to_string((unsigned long long)(uintptr_t)&n);
        {
            std::ofstream f(std::filesystem::u8path(tmp), std::ios::binary);
            if (!f) { err = "cannot write into this peer's cache"; return false; }
            f.write((const char*)member.data(), (std::streamsize)member.size());
            if (!f) { err = "cannot write into this peer's cache"; return false; }
        }
        std::filesystem::rename(std::filesystem::u8path(tmp),
                                std::filesystem::u8path(memberPath), ec);
        if (ec) {
            std::filesystem::remove(std::filesystem::u8path(tmp), ec);
            err = "cannot place the member in this peer's cache";
            return false;
        }
    }
    // From here it is an ordinary .npy, which is the point of materialising:
    // the axis rule, the ceiling, both refusal sentences, the fortran strides
    // and the endian normalisation are the ones every other .npy gets.
    return openNpy(n, memberPath, err, NR_NATIVE);
}

// Which family a key belongs to. The wire does not say (§10.5: a key names a
// materialisation and the trailer does not care what produced it), so the peer
// resolves it from its own cache - the note it wrote when it issued the key.
static bool keyIsNpz(const std::string& key) {
    NpzSrc s;
    return npzSrcRead(key, s);
}

static bool openServed(ServedFile& n, const std::string& path, std::string& err,
                       int read = NR_NATIVE, const RawWire* rw = nullptr) {
    // A recipe is a claim about a file that states nothing. Applying one to a
    // file that DOES state its shape would be answering a question nobody
    // asked, so it is refused - the mirror of openPicture's declared-reading
    // refusal, and for the identical reason.
    if (!isNpySuffix(path) && imagefile::isHeaderless(path)) {
        if (!rw) { err = imagefile::peerRefusal(path); return false; }
        if (read != NR_NATIVE) {
            err = "a declared .npy reading does not apply to a headerless file";
            return false;
        }
        return openRaw(n, path, err, *rw);
    }
    if (rw) {
        err = "a raw recipe does not apply to a file that states its own shape";
        return false;
    }
    if (!isNpySuffix(path) && imagefile::forPath(path)) return openPicture(n, path, err, read);
    return openNpy(n, path, err, read);
}

static bool readRegion(ServedFile& n, const TileReq& r, std::vector<uint8_t>& out,
                       uint32_t& outW, uint32_t& outH, std::string& err) {
    return n.pics.empty() ? readNpyRegion(n, r, out, outW, outH, err)
                          : readPictureRegion(n, r, out, outW, outH, err);
}

// ---------------------------------------------------------------- handlers
//
// The handlers below answer a request and produce a reply; they do not know how
// either travelled. That separation is the point: the ssh stdio framing at the
// bottom of this file is one transport, and a WebSocket front end (the browser
// client this project has always wanted) is another - same requests, same
// replies, same code here. Only the framing differs.
// Version the CLIENT announced in HELLO. A v2 viewer talking to this server
// must get the LIST shape it knows how to parse; defaulting to 2 keeps a client
// that never said hello (none of ours) on the safe format.
static uint32_t g_clientVersion = 2;

// ...and the set a SCAN may fold into stacks, which is wider since protocol 11:
// a headerless file has an answer here as soon as the request that opens it
// carries a recipe, so a listing that hid it would say "there is nothing here"
// about a folder the very next double-click opens (verify-matrix G1 / #148).
//
// GATED ON THE CLIENT'S OWN NUMBER, not on this peer's. A v10 client cannot
// send a recipe, so a group row of .raw files would arrive somewhere it cannot
// be opened - a listing offering an open that is refused is the defect this
// gate exists to avoid, pointed the other way. g_clientVersion is what HELLO
// left behind, so no new plumbing is needed: the fact was already here.
static bool isScannableSuffix(const std::string& name) {
    if (isServedSuffix(name)) return true;
    return g_clientVersion >= 11 && imagefile::isHeaderless(name);
}


// VIEWER_SERVE_PROTOCOL: serve as an OLDER peer. The same kind of seam
// VIEWER_SERVE_LAG_MS already is - "a real link's latency, on demand" - and it
// is not a claim but a behaviour: this peer announces the lower number in HELLO
// AND refuses every op that number predates, which is byte for byte what the
// build before those ops does. Without it the client's "your peer is too old"
// gate can only be exercised by keeping an old binary on the disk, and a
// refusal nobody runs is a refusal that rots.
static uint32_t servedVersion() {
    static uint32_t v = [] {
        const char* e = getenv("VIEWER_SERVE_PROTOCOL");
        const uint32_t n = e ? (uint32_t)atoi(e) : VERSION;
        return (n == 0 || n > VERSION) ? VERSION : n;
    }();
    return v;
}

// C++17 has no portable file_clock -> system_clock conversion (that is C++20);
// anchoring the difference against "now" on both clocks is exact to well under
// a second, which a directory listing does not care about.
static int64_t unixMtime(const std::filesystem::path& p) {
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(
                   ft - std::filesystem::file_time_type::clock::now());
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
               sys.time_since_epoch()).count();
}

// One v3 listing entry. `peekBudget` bounds how many .npy headers one LIST may
// open: a directory of thousands of files must not turn a listing into
// thousands of file opens (the header itself is a few hundred bytes to read).
//
// The peek stays .npy-ONLY even though the peer now serves pictures, and the
// budget is the reason it has to. Peeking a .npy costs a few hundred bytes;
// peeking a PNG costs a full decode, because a picture format states its shape
// in a header the seam does not expose separately - so a folder of 256 photos
// would decode 256 photos to draw a listing. A picture row therefore carries no
// LE_META, and the Browse listing shows no dtype for it. That is the same
// answer here as over ssh and the same answer as a local browse (which goes
// through this very code), so no folder gains a second reading from it.
static void putListEntryV3(Buf& out, const std::filesystem::path& full,
                           const std::string& name, bool dir, uint64_t size,
                           int64_t mtime, int& peekBudget) {
    ServedFile n;
    bool meta = false;
    if (!dir && isNpySuffix(name) && peekBudget > 0) {
        peekBudget--;
        std::string e2;
        meta = openServed(n, full.u8string(), e2);   // reads only the header
    }
    out.putStr(name);
    out.putU32((dir ? LE_DIR : 0u) | (meta ? LE_META : 0u));
    out.putU32((uint32_t)(size & 0xFFFFFFFFu));
    out.putU32((uint32_t)(size >> 32));
    out.putU32((uint32_t)((uint64_t)mtime & 0xFFFFFFFFu));
    out.putU32((uint32_t)((uint64_t)mtime >> 32));
    if (meta) {
        out.putU32(n.dtype);
        out.putU32((uint32_t)n.ndim);
        for (int i = 0; i < 4; i++) out.putU32(n.odims[i]);
        out.putU32(n.fortran ? 1u : 0u);
    }
}

// ---- numbered-sequence grouping ------------------------------------------

struct SeqGroup {
    std::string pattern;                // frame_###.npy - display name
    std::vector<std::string> names;     // member file names, numeric order
    uint64_t bytes = 0;                 // sum over members
    int64_t mtime = 0;                  // newest member
    std::filesystem::path first;        // header peek target
};

// The grouping key: the stem with every digit RUN collapsed to one marker.
// "00_A" and "01_A" share a key, "00_B" does not - the digits may sit
// ANYWHERE in the name, which is what real capture scripts produce. The
// trailing-digits-only version filed every 00_A/01_A set under "no sequence",
// and the *.npy fold then swallowed the lot into one stack (verbatim
// complaint). Matches the client's segment-based sibling scan in spirit;
// '?' in the pattern because the client renders it in ImGui labels.
static bool seqSegKey(const std::string& name, std::string& key, std::string& pattern) {
    if (!isScannableSuffix(name)) return false;
    // The EXTENSION rides in the key, so two formats in one folder never land
    // in one group: dark_001.png and dark_001.npy are different files of the
    // same scene, and a stack that mixed them would average two readings of it.
    // The client's own scanner splits the same way (scanFolderGroups folds its
    // leftovers per extension), and the two have to agree or one folder reads
    // differently depending on which end listed it.
    const size_t dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    std::string stem = name.substr(0, name.size() - ext.size());
    bool anyDigit = false;
    key.clear(); pattern.clear();
    for (size_t i = 0; i < stem.size();) {
        if (isdigit((unsigned char)stem[i])) {
            size_t j = i;
            while (j < stem.size() && isdigit((unsigned char)stem[j])) j++;
            key += '\x01';                       // one marker per digit run
            pattern += std::string(j - i, '?');
            anyDigit = true;
            i = j;
        } else { key += stem[i]; pattern += stem[i]; i++; }
    }
    key += ext; pattern += ext;
    return anyDigit;
}

// One stem split into digit / non-digit runs, digit runs keeping their text.
// Second stage of the grouping needs the VALUES, not just the positions.
struct SegRun { bool digit; std::string s; };
static std::vector<SegRun> segRuns(const std::string& stem) {
    std::vector<SegRun> out;
    for (size_t i = 0; i < stem.size();) {
        bool d = isdigit((unsigned char)stem[i]) != 0;
        size_t j = i;
        while (j < stem.size() && (isdigit((unsigned char)stem[j]) != 0) == d) j++;
        out.push_back({ d, stem.substr(i, j - i) });
        i = j;
    }
    return out;
}

// Partition one directory's files into numbered groups (>= 2 members) and the
// indices of everything else. `files` are (name, path) of regular files only.
//
// Two stages. Stage 1 buckets by seqSegKey (every digit run collapsed) - cheap
// and unchanged. Stage 2 decides, PER BUCKET, which digit run is the frame
// axis: the LAST varying run (the client's findSequenceSiblings rule). The
// pattern then keeps every other
// digit run literal - "gain10_???.npy", never "gain??_???.npy": '?' means
// "varies along the stack", and a gain digit under a '?' claims two gains got
// averaged. When a SECOND run also varies, the bucket splits by the other
// runs' values instead of growing a second '?' - a condition-mixed stack has a
// meaningless sigma_t, and the client splits those sets too. One-member
// sub-buckets fall back to singles.
static void groupNumbered(const std::vector<std::pair<std::string, std::filesystem::path>>& files,
                             std::vector<SeqGroup>& groups, std::vector<size_t>& singles) {
    struct Bucket { std::string key, pattern; std::vector<size_t> m; };
    std::vector<Bucket> buckets;
    // The bucket a name belongs to is found BY KEY. Scanning the bucket list
    // for it cost one string compare per bucket per file, and a folder whose
    // files mostly do not share a key has as many buckets as files: 100000
    // loose names took the peer 11 seconds in that loop alone (#236). The
    // buckets stay a vector - the emit order below is their creation order,
    // and a hash map has no order to give back.
    std::unordered_map<std::string, size_t> bucketIdx;
    std::vector<char> used(files.size(), 0);
    for (size_t i = 0; i < files.size(); i++) {
        std::string key, pat;
        if (!seqSegKey(files[i].first, key, pat)) continue;
        auto it = bucketIdx.find(key);
        if (it == bucketIdx.end()) {
            bucketIdx.emplace(key, buckets.size());
            buckets.push_back({ key, pat, {} });
            buckets.back().m.push_back(i);
        } else {
            buckets[it->second].m.push_back(i);
        }
    }
    // emit one group from members that are already in natural order; pattern
    // from the FIRST member: '?' over the frame-axis run only. Zero-padding
    // may be uneven (frame_9 / frame_10): the first member's digit count wins,
    // display-only - members always travel by real name (putGroupEntryV3).
    auto emit = [&](const std::vector<size_t>& mem, int frameAxis,
                    const std::string& fallbackPattern) {
        SeqGroup g;
        if (frameAxis >= 0) {
            const std::string& nm = files[mem.front()].first;
            const size_t dot = nm.find_last_of('.');
            const std::string ext = dot == std::string::npos ? std::string() : nm.substr(dot);
            std::vector<SegRun> sr = segRuns(nm.substr(0, nm.size() - ext.size()));
            int run = 0;
            for (const auto& r : sr) {
                if (r.digit && run++ == frameAxis) g.pattern += std::string(r.s.size(), '?');
                else g.pattern += r.s;
            }
            g.pattern += ext;
        } else {
            g.pattern = fallbackPattern;      // degenerate bucket: stage-1 view
        }
        for (size_t i : mem) {
            g.names.push_back(files[i].first);
            used[i] = 1;
            std::error_code ec;
            g.bytes += (uint64_t)std::filesystem::file_size(files[i].second, ec);
            g.mtime = std::max(g.mtime, unixMtime(files[i].second));
        }
        // "????.npy" says nothing; "0000..0003.npy" says what the stack is. The
        // client applies the SAME function to the patterns it builds locally -
        // see rp::patternWithExtent for why it has to be the same one. Which is
        // exactly why it is gated on the CLIENT's version: a v4 client cannot
        // produce this text locally, so serving it would make the same folder
        // read one way over ssh and another way opened from disk. The version
        // rule was enforced only downward (a newer client updates an older
        // peer); this is the mirror case, and the peer is the only end that
        // can act on it.
        if (frameAxis >= 0 && g_clientVersion >= 5)
            g.pattern = rp::patternWithExtent(g.pattern, g.names);
        g.first = files[mem.front()].second;
        groups.push_back(std::move(g));
    };
    for (auto& b : buckets) {
        if (b.m.size() < 2) continue;
        std::sort(b.m.begin(), b.m.end(), [&](size_t x, size_t y) {
            return rp::naturalLess(files[x].first, files[y].first);
        });
        // digit-run values per member (same key -> same run structure)
        std::vector<std::vector<std::string>> vals(b.m.size());
        size_t nRuns = 0;
        bool segOk = true;
        for (size_t i = 0; i < b.m.size() && segOk; i++) {
            const std::string& nm = files[b.m[i]].first;
            const size_t dot = nm.find_last_of('.');
            for (const auto& r : segRuns(nm.substr(0, dot == std::string::npos ? nm.size() : dot)))
                if (r.digit) vals[i].push_back(r.s);
            if (i == 0) nRuns = vals[i].size();
            else if (vals[i].size() != nRuns) segOk = false;
        }
        int frameAxis = -1, varying = 0;
        if (segOk) {
            // The frame axis is the LAST varying run, not the one with the most
            // distinct values. Capture software puts the counter last -
            // frame_001, IMG_0001, lv000_f02 - and a condition run can easily
            // outnumber the frames: 10 illuminances x 3 frames grouped by
            // illuminance, producing three "stacks" each spanning 10 levels,
            // which is exactly the condition-mixed stack the canon forbids.
            for (size_t r = 0; r < nRuns; r++) {
                // "Does this run take two different values" - never how many,
                // so the values go in a set and the walk stops at the second.
                // Collecting them in a list and re-comparing the list for every
                // member made this quadratic in the bucket size, which on a
                // single 100000-file group is the whole answer time (#236).
                std::unordered_set<std::string> distinct;
                for (const auto& v : vals) {
                    distinct.insert(v[r]);
                    if (distinct.size() >= 2) break;
                }
                if (distinct.size() < 2) continue;
                varying++;
                frameAxis = (int)r;           // keep overwriting: the last one wins
            }
        }
        if (!segOk || frameAxis < 0) {        // cannot analyze: stage-1 grouping
            emit(b.m, -1, b.pattern);
            continue;
        }
        if (varying < 2) {                    // one varying run: the whole bucket
            emit(b.m, frameAxis, b.pattern);
            continue;
        }
        // >= 2 varying runs: sub-bucket by every run EXCEPT the frame axis
        std::vector<std::pair<std::string, std::vector<size_t>>> subs;
        // ...found by key, for the bucket loop's reason. The vector stays: the
        // emit order below is creation order, and that is what the client sees.
        std::unordered_map<std::string, size_t> subIdx;
        for (size_t i = 0; i < b.m.size(); i++) {
            std::string ck;
            for (size_t r = 0; r < nRuns; r++)
                if ((int)r != frameAxis) { ck += vals[i][r]; ck += '\x01'; }
            auto it = subIdx.find(ck);
            if (it == subIdx.end()) {
                subIdx.emplace(ck, subs.size());
                subs.push_back({ ck, {} });
                subs.back().second.push_back(b.m[i]);
            } else {
                subs[it->second].second.push_back(b.m[i]);
            }
        }
        for (const auto& sp : subs)
            if (sp.second.size() >= 2) emit(sp.second, frameAxis, b.pattern);
            // < 2: stays unused -> falls through to singles below
    }
    for (size_t i = 0; i < files.size(); i++)
        if (!used[i]) singles.push_back(i);
}

// A group crosses the wire as its explicit member names (no directory part).
// A (prefix, start, count, width) encoding would be smaller, but real capture
// dumps have gaps and uneven zero-padding, and reconstructing names that do
// not exist is exactly the failure this tool must not have. Names only: even
// a 1000-frame group is ~20 KB, noise next to one tile.
static void putGroupEntryV3(Buf& out, const SeqGroup& g, int& peekBudget) {
    ServedFile n;
    bool meta = false;
    // .npy only, for putListEntryV3's reason: a group's meta is its FIRST
    // frame's, and peeking a picture group would decode a picture per group.
    if (peekBudget > 0 && isNpySuffix(g.first.filename().u8string())) {
        peekBudget--;
        std::string e2;
        meta = openServed(n, g.first.u8string(), e2);
    }
    out.putStr(g.pattern);
    out.putU32(LE_GROUP | (meta ? LE_META : 0u));
    out.putU32((uint32_t)(g.bytes & 0xFFFFFFFFu));
    out.putU32((uint32_t)(g.bytes >> 32));
    out.putU32((uint32_t)((uint64_t)g.mtime & 0xFFFFFFFFu));
    out.putU32((uint32_t)((uint64_t)g.mtime >> 32));
    if (meta) {
        out.putU32(n.dtype);
        out.putU32((uint32_t)n.ndim);
        for (int i = 0; i < 4; i++) out.putU32(n.odims[i]);
        out.putU32(n.fortran ? 1u : 0u);
    }
    out.putU32((uint32_t)g.names.size());
    for (const auto& nm : g.names) out.putStr(nm);
}

static void handleList(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad LIST"); return; }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(path);
    if (!std::filesystem::exists(p, ec)) { sendErr("no such path: " + path); return; }
    Buf out;
    std::vector<std::filesystem::directory_entry> entries;
    if (std::filesystem::is_directory(p, ec)) {
        for (auto& e : std::filesystem::directory_iterator(p, ec)) entries.push_back(e);
    } else {
        entries.push_back(std::filesystem::directory_entry(p, ec));
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b) {
                  return a.path().filename().u8string() < b.path().filename().u8string();
              });
    if (g_clientVersion < 3) {
        out.putU32((uint32_t)entries.size());
        for (auto& e : entries) {
            std::error_code e2;
            bool dir = e.is_directory(e2);
            out.putStr(e.path().filename().u8string());
            out.putU32(dir ? 1u : 0u);
            // 64-bit size as lo/hi: a 300-frame 12-bit 4K stack file passes 4 GB
            // routinely, and a silently clamped size is the failure mode this tool
            // exists to avoid
            uint64_t sz = dir ? 0 : (uint64_t)e.file_size(e2);
            out.putU32((uint32_t)(sz & 0xFFFFFFFFu));
            out.putU32((uint32_t)(sz >> 32));
        }
        sendMsg(MSG_OK, out);
        return;
    }
    // v3: numbered .npy siblings collapse into one synthetic stack entry;
    // directories, non-npy files and loose .npy stay plain rows.
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::vector<const std::filesystem::directory_entry*> dirs;
    for (auto& e : entries) {
        std::error_code e2;
        if (e.is_directory(e2)) dirs.push_back(&e);
        else files.push_back({ e.path().filename().u8string(), e.path() });
    }
    std::vector<SeqGroup> groups;
    std::vector<size_t> singles;
    groupNumbered(files, groups, singles);
    // one merged, name-sorted row list, so the reply reads like a directory
    struct Row { std::string name; int kind; size_t idx; };   // 0 dir, 1 single, 2 group
    std::vector<Row> rows;
    for (size_t i = 0; i < dirs.size(); i++)
        rows.push_back({ dirs[i]->path().filename().u8string(), 0, i });
    for (size_t i : singles) rows.push_back({ files[i].first, 1, i });
    for (size_t i = 0; i < groups.size(); i++)
        rows.push_back({ groups[i].pattern, 2, i });
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.name < b.name; });
    int peekBudget = 256;
    out.putU32((uint32_t)rows.size());
    for (const Row& r : rows) {
        std::error_code e2;
        if (r.kind == 0) {
            putListEntryV3(out, dirs[r.idx]->path(), r.name, true, 0,
                           unixMtime(dirs[r.idx]->path()), peekBudget);
        } else if (r.kind == 1) {
            const auto& f = files[r.idx];
            putListEntryV3(out, f.second, r.name, false,
                           (uint64_t)std::filesystem::file_size(f.second, e2),
                           unixMtime(f.second), peekBudget);
        } else {
            putGroupEntryV3(out, groups[r.idx], peekBudget);
        }
    }
    sendMsg(MSG_OK, out);
}

// GLOB: recursive find under a root. The pattern addresses a subtree, so *
// and ? deliberately cross '/' (unlike a shell); "**/" additionally matches
// zero directories, bash-globstar-style; a pattern with no wildcard is a
// case-insensitive substring. Matching is case-insensitive throughout -
// FRAME_001.NPY is the same capture dump.
static bool globCross(const char* pat, const char* str) {
    const char *star = nullptr, *ss = str;
    while (*str) {
        char p = *pat, s = *str;
        if (p == '?' || (p && tolower((unsigned char)p) == tolower((unsigned char)s))) {
            pat++; str++; continue;
        }
        if (p == '*') { star = pat++; ss = str; continue; }
        if (star) { pat = star + 1; str = ++ss; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}
static bool globPatternMatches(const std::string& pat, const std::string& rel) {
    if (pat.find('*') == std::string::npos && pat.find('?') == std::string::npos) {
        std::string a = pat, b = rel;
        for (char& c : a) c = (char)tolower((unsigned char)c);
        for (char& c : b) c = (char)tolower((unsigned char)c);
        return b.find(a) != std::string::npos;
    }
    if (globCross(pat.c_str(), rel.c_str())) return true;
    return pat.compare(0, 3, "**/") == 0 && globCross(pat.c_str() + 3, rel.c_str());
}

// The walk discipline GLOB and SCAN share: depth-limited, symlinks never
// followed (a cycle must cost nothing rather than hang a session), and one
// unreadable directory counted and stepped over rather than aborting the whole
// request - which is what std::filesystem::recursive_directory_iterator would
// do. Both requests had their own copy of this; the copies were byte-identical
// for six of their seven lines.
//
// `State` is what the two did NOT share. SCAN accumulates the .npy files of one
// directory before it can group them, GLOB accumulates nothing. Passing the
// accumulator's type in gives SCAN a fresh one per directory - the scope its
// own loop gave it, kept by the compiler rather than by remembering to clear -
// and lets GLOB pass an empty struct that costs nothing. `onDirEnd` is the
// other half of that: SCAN's per-directory epilogue, and a no-op for GLOB.
//
// `trunc` stops the walk from the OUTSIDE, because in both callers the result
// cap is the caller's to enforce and the reply it belongs to is the caller's to
// build. Setting it from a callback ends the walk at the next check.
template <typename State, typename EntryCb, typename DirEndCb>
static void walkDirectory(const std::filesystem::path& rootP, uint32_t depth,
                          uint32_t& skipped, bool& trunc, State initialState,
                          EntryCb&& onEntry, DirEndCb&& onDirEnd) {
    std::vector<std::pair<std::filesystem::path, uint32_t>> todo{ { rootP, 0 } };
    while (!todo.empty() && !trunc) {
        auto cur = todo.back();
        todo.pop_back();
        std::error_code dec;
        std::filesystem::directory_iterator it(cur.first, dec), end;
        if (dec) { skipped++; continue; }
        State state = initialState;
        for (; it != end && !trunc; it.increment(dec)) {
            if (dec) { dec.clear(); skipped++; break; }
            std::error_code fec;
            if (it->is_symlink(fec)) continue;
            bool isDir = it->is_directory(fec);
            if (isDir && cur.second < depth) todo.push_back({ it->path(), cur.second + 1 });
            onEntry(state, *it, isDir);
        }
        if (!trunc) onDirEnd(cur.first, state);
    }
}

//   -> [str root][str pattern][u32 depthLimit][u32 maxResults]
//   <- [u32 flags bit0=truncated][u32 skippedDirs][u32 n]
//      n * ([str relPath][u32 isDir])   relPath uses '/' on every platform
static void handleGlob(Buf& in) {
    std::string root, pattern;
    uint32_t depth = 6, cap = 2000;
    if (!in.getStr(root) || !in.getStr(pattern) || !in.getU32(depth) || !in.getU32(cap)) {
        sendErr("bad GLOB");
        return;
    }
    if (pattern.empty()) { sendErr("empty GLOB pattern"); return; }
    depth = std::min(depth, 32u);
    cap = std::min(cap ? cap : 2000u, 100000u);
    std::error_code ec;
    std::filesystem::path rootP = std::filesystem::u8path(root);
    if (!std::filesystem::is_directory(rootP, ec)) {
        sendErr("not a directory: " + root);
        return;
    }
    uint32_t skipped = 0;
    bool trunc = false;
    struct Hit { std::string rel; bool dir; };
    std::vector<Hit> hits;
    // same walk discipline as SCAN: no symlinks, unreadable = count and go on
    struct EmptyState {};
    walkDirectory(rootP, depth, skipped, trunc, EmptyState{},
    [&](EmptyState&, const std::filesystem::directory_entry& it, bool isDir) {
        std::string rel = it.path().lexically_relative(rootP).generic_u8string();
        if (!globPatternMatches(pattern, rel)) return;
        if (hits.size() >= cap) { trunc = true; return; }
        hits.push_back({ std::move(rel), isDir });
    }, [](const std::filesystem::path&, EmptyState&) {});

    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.rel < b.rel; });
    Buf out;
    out.putU32(trunc ? 1u : 0u);
    out.putU32(skipped);
    out.putU32((uint32_t)hits.size());
    for (const auto& h : hits) {
        out.putStr(h.rel);
        out.putU32(h.dir ? 1u : 0u);
    }
    sendMsg(MSG_OK, out);
}

// SCAN: the server-side half of "open this folder as stacks". Walks the
// subtree (depth- and count-limited), groups numbered .npy per directory, and
// returns ONE reply the client turns into open-as-stack jobs - the remote
// mirror of the local openFolder() scan. Loose .npy become single-member
// groups so a folder of unnumbered files still opens, exactly like locally.
//   -> [str root][u32 depthLimit][u32 maxGroups]
//   <- [u32 flags bit0=truncated][u32 skippedDirs][u32 n]
//      n * ([str dirRel]  [group entry, v3 LIST encoding])
static void handleScan(Buf& in) {
    std::string root;
    uint32_t depth = 6, cap = 256;
    if (!in.getStr(root) || !in.getU32(depth) || !in.getU32(cap)) {
        sendErr("bad SCAN");
        return;
    }
    depth = std::min(depth, 32u);
    cap = std::min(cap ? cap : 256u, 10000u);
    std::error_code ec;
    std::filesystem::path rootP = std::filesystem::u8path(root);
    if (!std::filesystem::is_directory(rootP, ec)) {
        sendErr("not a directory: " + root);
        return;
    }
    uint32_t skipped = 0;
    bool trunc = false;
    struct Found { std::string rel; SeqGroup g; };
    std::vector<Found> found;
    // Manual walk: recursive_directory_iterator aborts everything on one
    // unreadable entry, and symlinks are not followed at all - a cycle must
    // cost nothing, not hang a session.
    using ScanState = std::vector<std::pair<std::string, std::filesystem::path>>;
    walkDirectory(rootP, depth, skipped, trunc, ScanState{},
    [&](ScanState& files, const std::filesystem::directory_entry& it, bool isDir) {
        std::error_code fec;
        if (!isDir && it.is_regular_file(fec) &&
            isScannableSuffix(it.path().filename().u8string())) {
            files.push_back({ it.path().filename().u8string(), it.path() });
        }
    }, [&](const std::filesystem::path& curDir, ScanState& files) {
        if (files.empty()) return;
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<SeqGroup> gs;
        std::vector<size_t> singles;
        groupNumbered(files, gs, singles);
        // Two or more leftovers fold into ONE natural-order stack ("*.npy"):
        // capture sets are not always numbered (capture_a / capture_b / ...),
        // and N single-frame stacks from one folder is never what Open Folder
        // meant. A lone file still opens as itself.
        //
        // PER EXTENSION, because the peer no longer walks one format: a folder
        // holding capture_a.png beside notes.npy is two stacks and not one, and
        // "*.npy" over a set of PNGs would be a label that names the wrong
        // format. The client's scanFolderGroups folds its own leftovers per
        // extension for the same reason (core/app/sequence.inc), and these two
        // have to answer the same or one folder reads two ways.
        std::vector<std::pair<std::string, std::vector<size_t>>> byExt;
        for (size_t i : singles) {
            const std::string& nm = files[i].first;
            const size_t dot = nm.find_last_of('.');
            std::string ext = dot == std::string::npos ? std::string() : nm.substr(dot);
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            std::vector<size_t>* v = nullptr;
            for (auto& q : byExt) if (q.first == ext) { v = &q.second; break; }
            if (!v) { byExt.push_back({ ext, {} }); v = &byExt.back().second; }
            v->push_back(i);
        }
        std::sort(byExt.begin(), byExt.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& q : byExt) {
            std::vector<size_t>& mem = q.second;
            SeqGroup g;
            if (mem.size() >= 2) {
                g.pattern = "*" + q.first;
                std::sort(mem.begin(), mem.end(), [&](size_t x, size_t y) {
                    return rp::naturalLess(files[x].first, files[y].first);
                });
            } else {
                g.pattern = files[mem[0]].first;
            }
            for (size_t i : mem) {
                g.names.push_back(files[i].first);
                std::error_code e2;
                g.bytes += (uint64_t)std::filesystem::file_size(files[i].second, e2);
                g.mtime = std::max(g.mtime, unixMtime(files[i].second));
            }
            g.first = files[mem.front()].second;
            gs.push_back(std::move(g));
        }
        std::string rel = curDir.lexically_relative(rootP).generic_u8string();
        if (rel == ".") rel.clear();
        for (auto& g : gs) {
            if (found.size() >= cap) { trunc = true; break; }
            found.push_back({ rel, std::move(g) });
        }
    });

    Buf out;
    out.putU32(trunc ? 1u : 0u);
    out.putU32(skipped);
    out.putU32((uint32_t)found.size());
    int peekBudget = 512;
    for (auto& f : found) {
        out.putStr(f.rel);
        putGroupEntryV3(out, f.g, peekBudget);
    }
    sendMsg(MSG_OK, out);
}

// The DECLARED reading trailing a META or TILE request (protocol 9). Absent =
// NR_NATIVE, which is what every v8 and older client sends and what this peer
// answered before there was anything else to answer.
//
// Refused rather than clamped when it is not one of the four: a reading is a
// user's declaration and the client computed it from a menu, so a value this
// peer does not recognise means the two ends disagree about what the numbers
// mean - and the one answer that must not come back from that is a picture.
static bool getRead(Buf& in, int& read, std::string& err) {
    read = NR_NATIVE;
    uint32_t v = 0;
    if (!in.getU32(v)) return true;               // an older client: native
    if (v > NR_CHW) { err = "unknown .npy reading " + std::to_string(v); return false; }
    read = (int)v;
    return true;
}

// What may follow the reading on a META or TILE request.
//
// Protocol 11 had ONE optional block and read it by "if bytes remain, they were
// meant". Twelve has two, and that rule cannot carry two: a reader key parsed
// as a geometry is a picture returned for a request nobody made. So a v12
// client writes a FLAGS WORD always - 0 when it has nothing to add - and the
// blocks follow it in bit order.
//
// Gated on the SERVED version and the client's own, both. servedVersion() is
// the number this peer announced in HELLO, which is exactly what the client
// gated on when it decided whether to write the word - so the
// VIEWER_SERVE_PROTOCOL seam keeps testing a peer that could exist: pretend to
// be 11 and the v11 bytes are what arrive.
struct ReqTrailers {
    bool haveRw = false;
    RawWire rw{};
    bool haveKey = false;             // [str key][u32 node]: one array inside
    std::string key;                  // something this peer materialised - a
    uint32_t node = 0;                // reader's output, or a listed .npz
};
static bool getTrailers(Buf& in, ReqTrailers& t, std::string& err) {
    t = ReqTrailers{};
    if (!(servedVersion() >= 12 && g_clientVersion >= 12)) {
        if (in.rd >= in.b.size()) return true;             // an older client
        if (in.rd + sizeof t.rw > in.b.size()) { err = "truncated raw recipe"; return false; }
        memcpy(&t.rw, in.b.data() + in.rd, sizeof t.rw);
        in.rd += sizeof t.rw;
        t.haveRw = true;
        return true;
    }
    uint32_t flags = 0;
    if (!in.getU32(flags)) return true;                    // nothing at all
    if (flags & ~(uint32_t)(RQ_RAW_RECIPE | RQ_KEYED)) {
        err = "unknown request trailer " + std::to_string(flags);
        return false;
    }
    if (flags & RQ_RAW_RECIPE) {
        if (in.rd + sizeof t.rw > in.b.size()) { err = "truncated raw recipe"; return false; }
        memcpy(&t.rw, in.b.data() + in.rd, sizeof t.rw);
        in.rd += sizeof t.rw;
        t.haveRw = true;
    }
    if (flags & RQ_KEYED) {
        if (!in.getStr(t.key) || !in.getU32(t.node)) {
            err = "truncated keyed trailer";
            return false;
        }
        t.haveKey = true;
    }
    return true;
}

// ONE ARRAY INSIDE SOMETHING THIS PEER MATERIALISED, whatever asked for it.
//
// WHICH materialisation is asked of the cache, not of the wire (§10.5). A
// reader's output and a listed .npz are addressed identically on purpose - the
// trailer says "key, node" and means it - so this is the one place that has to
// know there are two kinds, and it learns it from the note the peer wrote when
// it issued the key.
//
// A function rather than three lines inside openRequested, because MEASURE
// reaches it too since protocol 14: the subject of a keyed measurement is
// resolved by exactly this, so a statistic and a tile of the same [key][node]
// cannot come from two different arrays.
static bool openKeyed(ServedFile& n, const std::string& key, uint32_t node,
                      std::string& err) {
    if (keyIsNpz(key)) return openNpzMember(n, key, node, err);
    return openReaderCache(n, key, node, err);
}

// One request's subject: a path on this disk, or one array inside something
// this peer materialised. Both end as a ServedFile, which is why META and TILE
// below each grew one line and nothing else.
static bool openRequested(ServedFile& n, const std::string& path, const ReqTrailers& t,
                          int read, std::string& err) {
    if (!t.haveKey) return openServed(n, path, err, read, t.haveRw ? &t.rw : nullptr);
    if (t.haveRw) {
        err = "a raw recipe does not apply to an array inside a materialisation";
        return false;
    }
    if (read != NR_NATIVE) {
        err = "a declared .npy reading does not apply to an array inside a "
              "materialisation";
        return false;
    }
    return openKeyed(n, t.key, t.node, err);
}

static void handleMeta(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad META"); return; }
    int read = NR_NATIVE;
    std::string err;
    if (!getRead(in, read, err)) { sendErr(err); return; }
    ReqTrailers tr;
    if (!getTrailers(in, tr, err)) { sendErr(err); return; }
    ServedFile n;
    if (!openRequested(n, path, tr, read, err)) { sendErr(err); return; }
    MetaRep m{};
    m.w = (uint32_t)n.w; m.h = (uint32_t)n.h; m.ch = (uint32_t)n.ch;
    m.dtype = n.dtype; m.frames = (uint32_t)n.frames; m.flags = 0;
    // The shape the FILE declared, so the client can print §3.3's "read as" line
    // and compute which OTHER readings this array permits (issue #124). Only
    // when the version this peer is SERVING has it: VIEWER_SERVE_PROTOCOL=8 has
    // to be a v8 peer in the reply as well as in the handshake, or the seam
    // tests a peer that does not exist.
    const bool sendShape = servedVersion() >= 9 && n.ndim >= 2 && n.ndim <= 4;
    if (sendShape) m.flags |= MR_SHAPE;
    if (servedVersion() >= 9 && n.readFellBack) m.flags |= MR_READ_FELL_BACK;
    Buf out;
    out.putBlob(&m, sizeof m);
    if (sendShape) {
        out.putU32((uint32_t)n.ndim);
        for (int i = 0; i < 4; i++) out.putU32(n.odims[i]);
    }
    sendMsg(MSG_OK, out);
}

static void handleTile(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad TILE"); return; }
    TileReq r{};
    if (in.rd + sizeof r > in.b.size()) { sendErr("bad TILE"); return; }
    memcpy(&r, in.b.data() + in.rd, sizeof r);
    in.rd += sizeof r;
    int read = NR_NATIVE;
    std::string err;
    if (!getRead(in, read, err)) { sendErr(err); return; }
    ReqTrailers tr;
    if (!getTrailers(in, tr, err)) { sendErr(err); return; }
    ServedFile n;
    if (!openRequested(n, path, tr, read, err)) { sendErr(err); return; }
    std::vector<uint8_t> pix;
    uint32_t ow = 0, oh = 0;
    if (!readRegion(n, r, pix, ow, oh, err)) { sendErr(err); return; }

    TileRep rep{};
    rep.w = ow; rep.h = oh; rep.ch = (uint32_t)n.ch; rep.dtype = n.dtype;
    if (pix.size() > 0xFFFFFFFFull) {   // u32 on the wire: refuse, never truncate
        sendErr("tile exceeds 4 GB - request a decimated tile (step > 1)");
        return;
    }
    rep.rawBytes = (uint32_t)pix.size();
    rep.flags = 0;
    std::vector<uint8_t> packed;
    if (r.flags & 1) {         // deflate: sensor data compresses well enough to pay
        mz_ulong cap = mz_compressBound((mz_ulong)pix.size());
        packed.resize(cap);
        mz_ulong got = cap;
        if (mz_compress2(packed.data(), &got, pix.data(), (mz_ulong)pix.size(), 6) == MZ_OK &&
            got < pix.size()) {
            packed.resize(got);
            rep.flags |= 1;
        } else {
            packed.clear();
        }
    }
    Buf out;
    out.putBlob(&rep, sizeof rep);
    if (rep.flags & 1) out.putBlob(packed.data(), packed.size());
    else               out.putBlob(pix.data(), pix.size());
    sendMsg(MSG_OK, out);
}

// ---------------------------------------------------------------- measure
// Run analysis where the data lives. The reply carries only what was emitted:
// the whole point is that a statistic over gigabytes of frames crosses the wire
// as a few hundred bytes, immediately, while any pixel transfer runs in parallel.

// The peer's own narrowing, and the one of the four that does NOT yet report
// what it cost. u4/i4/f8 lose here exactly as they do in remote.cpp's toFloat
// (rp::F32Loss says how, and the local decoders, the raw decoder and the remote
// client all measure it) - but a census raised here has no way home: a MEASURE
// reply carries emitted results, and saying "3 of 4000000 samples are not the
// file's" would need a field in that reply and therefore rp::VERSION 8. What
// covers the user today is that the DOCUMENT is opened through TILE, whose
// census does travel, so the Inspector already declares the loss for the very
// pixels these statistics were taken from. Left as a named gap rather than a
// silent one; it is judgment item 3 of the precision report.
void toFloatSamples(const uint8_t* src, uint32_t dtype, size_t n, float* out) {
    switch (dtype) {
        case DT_U8:  for (size_t i = 0; i < n; i++) out[i] = (float)src[i]; break;
        case DT_I8:  for (size_t i = 0; i < n; i++) out[i] = (float)((const int8_t*)src)[i]; break;
        case DT_U16: for (size_t i = 0; i < n; i++) out[i] = (float)((const uint16_t*)src)[i]; break;
        case DT_I16: for (size_t i = 0; i < n; i++) out[i] = (float)((const int16_t*)src)[i]; break;
        case DT_U32: for (size_t i = 0; i < n; i++) out[i] = (float)((const uint32_t*)src)[i]; break;
        case DT_I32: for (size_t i = 0; i < n; i++) out[i] = (float)((const int32_t*)src)[i]; break;
        case DT_F32: memcpy(out, src, n * 4); break;
        case DT_F64: for (size_t i = 0; i < n; i++) out[i] = (float)((const double*)src)[i]; break;
        // every half is exactly a float, so this one loses nothing at all
        // (braced: the `break` belongs to the switch, not to the loop, and
        //  gcc's -Wmisleading-indentation is right to say the layout implied
        //  otherwise)
        case DT_F16: { for (size_t i = 0; i < n; i++)
                           out[i] = halfToFloat(((const uint16_t*)src)[i]); } break;
        default:     for (size_t i = 0; i < n; i++) out[i] = 0.0f; break;
    }
}

namespace {
struct MItem { uint32_t kind; std::string key; double num; std::string text; };
struct MSeries {
    std::string name, xl, yl;
    uint32_t col = 0;
    bool hasX = false;
    std::vector<float> xs, ys;
};
struct MSink {
    std::vector<std::vector<MItem>>* cols;
    std::vector<MSeries>* series;
    uint32_t col = 0;
};
void mNum(void* c, const char* k, double v) {
    auto* s = (MSink*)c;
    (*s->cols)[s->col].push_back({ 0u, k ? k : "", v, {} });
}
void mTxt(void* c, const char* k, const char* v) {
    auto* s = (MSink*)c;
    (*s->cols)[s->col].push_back({ 1u, k ? k : "", 0.0, v ? v : "" });
}
void mSer(void* c, const char* name, const char* xl, const char* yl,
          const float* x, const float* y, uint32_t n) {
    auto* s = (MSink*)c;
    MSeries so;
    so.name = name ? name : "";
    so.xl = xl ? xl : ""; so.yl = yl ? yl : "";
    so.col = s->col;
    so.hasX = x != nullptr;
    if (x) so.xs.assign(x, x + n);
    so.ys.assign(y, y + n);
    s->series->push_back(std::move(so));
}

// Plugins load on the FIRST measure, not at startup: a session that only lists
// and ships tiles should not pay for (or depend on) the plugin directory.
//
// VIEWER_SERVE_PLUGINS: extra directories, PATH-separated, searched as well as
// the two beside the binary. A compute node is not a workstation - the peer is
// often a single file copied into ~/bin while the plugins the lab actually runs
// live on a shared mount - and until ABI v3 §10 that was invisible, because
// MOP_ANALYZER's only failure was "analyzer not found" and nobody could tell
// "wrong directory" from "wrong machine". It is also how a fixture directory is
// reached without shipping fixtures, the same affordance VIEWER_SERVE_LAG_MS is.
bool g_pluginsLoaded = false;
void ensurePlugins() {
    if (g_pluginsLoaded) return;
    g_pluginsLoaded = true;
    std::vector<std::string> dirs{ plugin_host::exeDir() + "/plugins",
                                   plugin_host::exeDir() + "/../plugins" };
    if (const char* extra = getenv("VIEWER_SERVE_PLUGINS")) {
#ifdef _WIN32
        const char sep = ';';         // ':' is a drive letter here, never a separator
#else
        const char sep = ':';
#endif
        std::string s = extra;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(sep, i);
            if (j == std::string::npos) j = s.size();
            if (j > i) dirs.push_back(s.substr(i, j - i));
            i = j + 1;
        }
    }
    plugin_host::loadAll(dirs,
                         [](const std::string& m, bool) { fprintf(stderr, "%s\n", m.c_str()); });
}
}  // namespace

// ---- aggregate ops -------------------------------------------------------
// The statistics an IQ engineer wants from a stack, computed in ONE streaming
// pass where the frames live: nothing but the results crosses the wire, and
// nothing waits for a transfer. f64 accumulation; the contract is statistical,
// not bit-exact across backends (a GPU implementation may reassociate sums).
namespace {
struct RoiRect { uint32_t x, y, w, h; };

const int CFA_MAP_S[4][4] = { {0,1,2,3}, {3,2,1,0}, {1,0,3,2}, {2,3,0,1} };
const char* PLANE_NAMES[4] = { "R", "Gr", "Gb", "B" };
inline int planeOf(uint32_t cfaType, uint32_t pat, uint32_t x, uint32_t y) {
    if (!cfaType) return 0;
    uint32_t cx = cfaType == 2 ? (x >> 1) & 1 : x & 1;
    uint32_t cy = cfaType == 2 ? (y >> 1) & 1 : y & 1;
    return CFA_MAP_S[pat & 3][cy * 2 + cx];
}
std::string planeKey(const char* base, uint32_t cfaType, int plane) {
    return cfaType ? std::string(base) + " " + PLANE_NAMES[plane] : std::string(base);
}

// The subject of a MEASURE when it is not a file (protocol 14, MRF_KEYED): one
// array inside something this peer materialised. The pair travels with the
// SAME meaning it has on META and TILE, and it is resolved by the same function
// (openKeyed), so a stack's sigma_t and its pixels cannot come from two
// different arrays.
struct MeasureKey {
    std::string key;
    uint32_t node = 0;
};

// Frames from one frame-axis file, or one file per frame; ROI rows only, so the
// disk pays for the region, not the file.
struct FrameSource {
    const MeasureReqHead* head = nullptr;
    const std::vector<std::string>* paths = nullptr;
    ServedFile n;                        // in-file mode stays open across frames
    bool perFile = false;
    uint32_t count = 0;
    std::string err;
    // The declared geometry, when the frames are headerless (protocol 11). One
    // recipe for the whole request: every frame of a stack is the same picture
    // measured again (docs/terminology.md), so a per-frame geometry would not
    // be a stack at all. A SET names several stacks and is refused for exactly
    // that reason, on the client, before it is sent.
    const RawWire* recipe = nullptr;

    // `kd` (optional) makes the subject one array inside a materialisation
    // instead of `p` - which is then EMPTY, because a keyed request carries no
    // path at all. The frame axis, the range check and read() below are
    // untouched by which of the two opened the ServedFile: that is the point of
    // the shape, and it is why MOP_TEMPORAL_STATS and MOP_FRAME_ROI_STATS
    // needed no arithmetic of their own for stage 5.
    bool init(const MeasureReqHead& h, const std::vector<std::string>& p,
              const RawWire* rec, const MeasureKey* kd = nullptr) {
        head = &h;
        paths = &p;
        recipe = rec;
        perFile = !kd && p.size() > 1;
        if (kd) {
            if (!openKeyed(n, kd->key, kd->node, err)) return false;
        } else if (!openServed(n, p[0], err, NR_NATIVE, recipe)) {
            return false;
        }
        if (perFile) {
            count = (uint32_t)p.size();
        } else {
            uint32_t total = (uint32_t)n.frames;
            // out of range is an ERROR: readRegion would clamp to the last frame
            // and a statistic over the wrong frames would look perfectly normal
            if (h.frame0 >= total) {
                err = "frame range outside the file (frame0 " + std::to_string(h.frame0) +
                      " of " + std::to_string(total) + ")";
                return false;
            }
            uint32_t avail = total - h.frame0;
            count = h.frameCount ? std::min(h.frameCount, avail) : avail;
        }
        if (count == 0) { err = "no frames in range"; return false; }
        return true;
    }
    // series x values are real frame numbers, not 0-based positions: two ROIs
    // measured with different frame0 must not silently misalign on one plot
    uint32_t xBase() const { return perFile ? 0 : head->frame0; }
    bool read(uint32_t i, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh,
              std::vector<uint8_t>& raw, uint32_t& ow, uint32_t& oh) {
        TileReq r{};
        r.x = rx; r.y = ry; r.w = rw; r.h = rh; r.step = 1;
        if (perFile) {
            ServedFile f;
            if (!openServed(f, (*paths)[i], err, NR_NATIVE, recipe)) return false;
            if (f.w != n.w || f.h != n.h || f.ch != n.ch || f.dtype != n.dtype) {
                err = "frame " + std::to_string(i) + " differs in shape/dtype";
                return false;
            }
            r.frame = 0;
            return readRegion(f, r, raw, ow, oh, err);
        }
        r.frame = head->frame0 + i;
        return readRegion(n, r, raw, ow, oh, err);
    }
};

bool clampRoi(const ServedFile& n, const std::vector<RoiRect>& rois, uint32_t c,
              uint32_t& rx, uint32_t& ry, uint32_t& rw, uint32_t& rh) {
    rx = 0; ry = 0; rw = (uint32_t)n.w; rh = (uint32_t)n.h;
    if (!rois.empty()) {
        rx = std::min(rois[c].x, (uint32_t)n.w);
        ry = std::min(rois[c].y, (uint32_t)n.h);
        rw = std::min(rois[c].w, (uint32_t)n.w - rx);
        rh = std::min(rois[c].h, (uint32_t)n.h - ry);
    }
    return rw > 0 && rh > 0;
}
}  // namespace

// Who computed it, from the peer's OWN ledger (docs/reference/abi-v3.md §10/§11). Empty
// name = not a plugin op, and then not one byte of trailer is written: the
// three ops that came before this one send the reply they always sent.
struct MeasureProv {
    std::string name, version, file, path;
    uint32_t expected = 0;              // N; framesUsed is n
};

static void sendMeasureReply(uint32_t framesUsed,
                             const std::vector<std::vector<MItem>>& cols,
                             const std::vector<MSeries>& series,
                             const MeasureProv* prov = nullptr) {
    Buf out;
    out.putU32(0);                              // serverLoc: CPU (CUDA slots in here)
    out.putU32(framesUsed);
    out.putU32((uint32_t)cols.size());
    for (const auto& col : cols) {
        out.putU32((uint32_t)col.size());
        for (const auto& it : col) {
            out.putU32(it.kind);
            out.putStr(it.key);
            if (it.kind == 0) out.putF64(it.num);
            else              out.putStr(it.text);
        }
    }
    out.putU32((uint32_t)series.size());
    for (const auto& s : series) {
        out.putStr(s.name); out.putStr(s.xl); out.putStr(s.yl);
        out.putU32(s.col);
        out.putU32(s.hasX ? 1u : 0u);
        out.putU32((uint32_t)s.ys.size());
        if (s.hasX) out.putBlob(s.xs.data(), s.xs.size() * 4);
        out.putBlob(s.ys.data(), s.ys.size() * 4);
    }
    if (prov) {
        out.putStr(prov->name);
        out.putStr(prov->version);
        out.putStr(prov->file);
        out.putStr(prov->path);
        out.putU32(prov->expected);
    }
    sendMsg(MSG_OK, out);
}

// Temporal noise vs fixed pattern: per-pixel mean/var over the frame range.
// sigma_t = sqrt(mean per-pixel temporal variance); sigma_fpn = the spatial
// sigma of the per-pixel temporal means with the temporal residual STILL IN
// that mean subtracted (#57 judgment 2, docs/features/analysis/flat-field-stats.md (b)):
//
//     C        = mean_i(s_t,i^2 / n_i)              ... per pixel, per its own n_i
//     sigma_fpn = sqrt(max(0, var_spatial(M) - C))  ... var ddof=1, clamp declared
//
// This function and the viewer's own recomputeTemporalIfNeeded are ONE
// estimator with two transports (docs/analysis-layers.md §3.5), so the
// arithmetic below is written in the same order as the local one - clamp before
// scaling, correction accumulated per pixel - and rtemporal's P1..P4 compare
// the two on the same stack rather than trusting that they look alike.
static void runTemporalStats(const MeasureReqHead& head,
                             const std::vector<std::string>& paths,
                             const std::vector<RoiRect>& rois, const RawWire* rec,
                             const MeasureKey* kd) {
    FrameSource src;
    if (!src.init(head, paths, rec, kd)) { sendErr(src.err); return; }
    if (src.count < 2) { sendErr("temporal stats needs at least 2 frames"); return; }
    if (head.cfaType && src.n.ch != 1) {
        sendErr("CFA planes need a 1-channel frame");   // planes over RGB = nonsense
        return;
    }
    uint32_t nCols = rois.empty() ? 1 : (uint32_t)rois.size();
    std::vector<std::vector<MItem>> cols(nCols);
    std::vector<MSeries> series;
    std::vector<uint8_t> raw;
    std::vector<float> pix;
    for (uint32_t c = 0; c < nCols; c++) {
        uint32_t rx, ry, rw, rh;
        if (!clampRoi(src.n, rois, c, rx, ry, rw, rh)) { sendErr("empty ROI"); return; }
        size_t samples = (size_t)rw * rh * src.n.ch;
        if (samples > (size_t)32 << 20) {   // 32M samples ~ 640 MB of accumulators
            sendErr("ROI too large for temporal stats (max 32M samples)");
            return;
        }
        std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
        // Per-pixel valid counts: a NaN must be EXCLUDED, not folded in. Folding
        // gives max(0, NaN) = 0, i.e. zero variance - an understated noise number
        // that looks perfectly plausible. Silent optimism is the worst failure.
        std::vector<uint32_t> cnt(samples, 0);
        uint64_t nonFinite = 0;
        MSeries fm, fs;
        fm.name = "frame mean"; fm.xl = "frame number"; fm.yl = "mean [DN]";
        fm.col = c; fm.hasX = true;
        fs.name = "frame std";  fs.xl = "frame number"; fs.yl = "std [DN]";
        fs.col = c; fs.hasX = true;
        for (uint32_t f = 0; f < src.count; f++) {
            uint32_t ow = 0, oh = 0;
            if (!src.read(f, rx, ry, rw, rh, raw, ow, oh)) { sendErr(src.err); return; }
            pix.resize(samples);
            toFloatSamples(raw.data(), src.n.dtype, samples, pix.data());
            double s1 = 0, s2 = 0;
            size_t sn = 0;
            for (size_t i = 0; i < samples; i++) {
                double v = pix[i];
                if (!std::isfinite(v)) { nonFinite++; continue; }
                sum[i] += v; sum2[i] += v * v; cnt[i]++;
                s1 += v; s2 += v * v; sn++;
            }
            double m = sn ? s1 / (double)sn : 0.0;
            fm.xs.push_back((float)(src.xBase() + f));
            fm.ys.push_back((float)m);
            fs.xs.push_back((float)(src.xBase() + f));
            fs.ys.push_back((float)(sn ? sqrt(std::max(0.0, s2 / (double)sn - m * m)) : 0.0));
        }
        const int nPl = head.cfaType ? 4 : 1;
        double plM[4] = {}, plM2[4] = {}, plV[4] = {}, plCorr[4] = {};
        size_t plC[4] = {};
        for (uint32_t y = 0; y < rh; y++)
            for (uint32_t x = 0; x < rw; x++) {
                int p = planeOf(head.cfaType, head.cfaPattern, rx + x, ry + y);
                for (uint32_t k = 0; k < (uint32_t)src.n.ch; k++) {
                    size_t i = ((size_t)y * rw + x) * src.n.ch + k;
                    double nI = (double)cnt[i];
                    if (nI < 2) continue;              // not enough valid frames
                    double m = sum[i] / nI;
                    double var = std::max(0.0, sum2[i] / nI - m * m) * (nI / (nI - 1.0));
                    plM[p] += m; plM2[p] += m * m; plV[p] += var; plC[p]++;
                    plCorr[p] += var / nI;      // s_t,i^2 / n_i, this pixel's own n_i

                }
            }
        cols[c].push_back({ 0u, "frames", (double)src.count, {} });
        if (nonFinite)
            cols[c].push_back({ 0u, "non-finite samples (excluded)", (double)nonFinite, {} });
        bool anyClamp = false;
        for (int p = 0; p < nPl; p++) {
            if (!plC[p]) continue;
            double cnt = (double)plC[p];
            double mean = plM[p] / cnt;
            double st = sqrt(plV[p] / cnt);
            double corr = plCorr[p] / cnt;              // C = mean_i(s_t,i^2 / n_i)
            // ddof=1 on the spatial variance, as the settled estimator writes it:
            // subtracting an unbiased C from a biased variance leaves
            // (sigma_fpn^2 + C)/n behind. See recomputeTemporalIfNeeded for the
            // full note - the two sides carry the same reasoning because they
            // have to carry the same arithmetic.
            double pvar = cnt > 1.0
                ? std::max(0.0, plM2[p] / cnt - mean * mean) * (cnt / (cnt - 1.0)) : 0.0;
            double fvar = pvar - corr;
            bool clamped = fvar < 0.0;
            if (clamped) fvar = 0.0;
            anyClamp |= clamped;
            double fpn = sqrt(fvar);
            cols[c].push_back({ 0u, planeKey("mean [DN]", head.cfaType, p), mean, {} });
            cols[c].push_back({ 0u, planeKey("sigma_t [DN]", head.cfaType, p), st, {} });
            cols[c].push_back({ 0u, planeKey("sigma_fpn [DN]", head.cfaType, p), fpn, {} });
            // sqrt(plV/cnt + fvar), NOT sqrt(st*st + fvar): st is already a
            // sqrt, and squaring it back is not the identity in binary64 - so
            // the old form could differ from the viewer's sqrt(tvar + fvar) in
            // the last ulp, on a quantity the two sides are supposed to agree on
            // exactly. Nothing had ever compared them, which is how it survived.
            cols[c].push_back({ 0u, planeKey("sigma_tot [DN]", head.cfaType, p),
                                sqrt(plV[p] / cnt + fvar), {} });
            // The correction is REPORTED, not merely applied: a reader has to be
            // able to get back to the uncorrected upper bound
            // (sqrt(sigma_fpn^2 + fpn_corr^2)) without re-measuring, and a
            // clamped 0 has to say it is a clamp rather than a flat sensor.
            cols[c].push_back({ 0u, planeKey("fpn_corr [DN]", head.cfaType, p),
                                sqrt(corr), {} });
            cols[c].push_back({ 0u, planeKey("fpn_clamped", head.cfaType, p),
                                clamped ? 1.0 : 0.0, {} });
        }
        cols[c].push_back({ 1u, "method", 0.0,
            "per-pixel mean/var over " + std::to_string(src.count) +
            " frames; sigma_t = sqrt(mean unbiased temporal var), "
            "sigma_fpn = sqrt(max(0, var_spatial(temporal means, ddof=1) - "
            "fpn_corr^2)) with fpn_corr = sqrt(mean(s_t,i^2/n_i)) - temporal "
            "residual C subtracted (#57 item 2); non-finite excluded" +
            std::string(anyClamp ? "; sigma_fpn CLAMPED at 0 on at least one "
                                   "plane (see fpn_clamped): the temporal "
                                   "residual is not below the spatial spread, "
                                   "so no fixed pattern is resolved here"
                                 : "; no clamp") +
            std::string(head.cfaType ? "; CFA planes" : "") + "; backend=cpu" });
        series.push_back(std::move(fm));
        series.push_back(std::move(fs));
    }
    sendMeasureReply(src.count, cols, series);
}

// Per-frame, per-ROI mean and variance: the raw material of photon-transfer and
// linearity curves, ~50 KB for 300 frames x 5 ROIs instead of gigabytes.
static void runFrameRoiStats(const MeasureReqHead& head,
                             const std::vector<std::string>& paths,
                             const std::vector<RoiRect>& rois, const RawWire* rec,
                             const MeasureKey* kd) {
    FrameSource src;
    if (!src.init(head, paths, rec, kd)) { sendErr(src.err); return; }
    if (head.cfaType && src.n.ch != 1) {
        sendErr("CFA planes need a 1-channel frame");
        return;
    }
    uint32_t nCols = rois.empty() ? 1 : (uint32_t)rois.size();
    std::vector<std::vector<MItem>> cols(nCols);
    std::vector<MSeries> series;
    std::vector<uint8_t> raw;
    std::vector<float> pix;
    const int nPl = head.cfaType ? 4 : 1;
    uint64_t nonFinite = 0;
    for (uint32_t c = 0; c < nCols; c++) {
        uint32_t rx, ry, rw, rh;
        if (!clampRoi(src.n, rois, c, rx, ry, rw, rh)) { sendErr("empty ROI"); return; }
        size_t samples = (size_t)rw * rh * src.n.ch;
        if (samples > (size_t)64 << 20) {   // one frame's ROI buffer, not unbounded
            sendErr("ROI too large for frame stats (max 64M samples)");
            return;
        }
        std::vector<std::vector<float>> mnY((size_t)nPl), vrY((size_t)nPl);
        std::vector<float> xsAll;
        for (uint32_t f = 0; f < src.count; f++) {
            uint32_t ow = 0, oh = 0;
            if (!src.read(f, rx, ry, rw, rh, raw, ow, oh)) { sendErr(src.err); return; }
            pix.resize(samples);
            toFloatSamples(raw.data(), src.n.dtype, samples, pix.data());
            double s1[4] = {}, s2[4] = {};
            size_t cn[4] = {};
            for (uint32_t y = 0; y < rh; y++)
                for (uint32_t x = 0; x < rw; x++) {
                    int p = planeOf(head.cfaType, head.cfaPattern, rx + x, ry + y);
                    for (uint32_t k = 0; k < (uint32_t)src.n.ch; k++) {
                        double v = pix[((size_t)y * rw + x) * src.n.ch + k];
                        if (!std::isfinite(v)) { nonFinite++; continue; }
                        s1[p] += v; s2[p] += v * v; cn[p]++;
                    }
                }
            xsAll.push_back((float)(src.xBase() + f));
            for (int p = 0; p < nPl; p++) {
                double m = cn[p] ? s1[p] / (double)cn[p] : 0.0;
                double var = cn[p] ? std::max(0.0, s2[p] / (double)cn[p] - m * m) : 0.0;
                mnY[p].push_back((float)m);
                vrY[p].push_back((float)var);
            }
        }
        cols[c].push_back({ 0u, "frames", (double)src.count, {} });
        if (nonFinite)
            cols[c].push_back({ 0u, "non-finite samples (excluded)", (double)nonFinite, {} });
        cols[c].push_back({ 1u, "method", 0.0,
            "per-frame spatial mean/var of the ROI, f64 accum, non-finite excluded" +
            std::string(head.cfaType ? "; CFA planes" : "") + "; backend=cpu" });
        for (int p = 0; p < nPl; p++) {
            MSeries sm, sv;
            sm.name = planeKey("roi mean", head.cfaType, p);
            sm.xl = "frame number (index)"; sm.yl = "mean [DN]";
            sm.col = c; sm.hasX = true; sm.xs = xsAll; sm.ys = std::move(mnY[p]);
            sv.name = planeKey("roi var", head.cfaType, p);
            sv.xl = "frame number (index)"; sv.yl = "variance [DN^2]";
            sv.col = c; sv.hasX = true; sv.xs = xsAll; sv.ys = std::move(vrY[p]);
            series.push_back(std::move(sm));
            series.push_back(std::move(sv));
        }
    }
    sendMeasureReply(src.count, cols, series);
}

// ---- set folding on the peer (docs/analysis-layers.md §3.5 / §6) -----------
//
// A 480-frame dark on a compute node should be folded HERE, not pulled across
// ssh to be folded there. What this op does is exactly the fold - the per-pixel
// temporal mean and the temporal residual still in it, reduced to per-plane
// sums - and it computes nothing that has a name. DSNU, PRNU, the separation
// fit's OLS: every one of those is composed by the client, on the client's own
// code, out of the handful of scalars below. core/setfold.h says why the split
// falls there and why the ONE thing that could not be composed from per-role
// scalars (the pixel-wise difference of two mean images) is reduced on this
// side instead.
//
// Everything large is a std::vector: PR #133's 768 KB local overflowed Windows'
// 1 MB main-thread stack, and a fold's accumulators are megabytes.
struct SetRoleReq {
    std::string role;
    uint32_t nPaths = 0, frame0 = 0, frameCount = 0;
};

struct RoleFold {
    std::string role;
    bool folded = false;               // false = fewer than 2 frames; the CLIENT
                                       // words that refusal, see below
    uint32_t frames = 0, expected = 0;
    int w = 0, h = 0, ch = 1, nPl = 1;
    uint64_t nonFinite = 0, dropped = 0;
    std::vector<double> M, corr;       // kept only when a join needs them
    std::vector<uint8_t> plane;
    setfold::PlaneAcc pl[4];
    DetrendReport shade;
};

// One role: read its frames, fold, reduce, probe. `keep` decides whether the
// per-pixel pictures survive the call - a join needs two of them resident at
// once, and the separation fit's M+1 roles deliberately do not, so a sweep of
// twenty levels costs this peer one fold's memory rather than twenty.
static bool foldOneRole(const MeasureReqHead& head, const std::vector<std::string>& paths,
                        const SetRoleReq& rq, bool keep, RoleFold& F, std::string& err) {
    MeasureReqHead h = head;           // FrameSource reads the range off the head,
    h.frame0 = rq.frame0;              // and a set's roles each have their own
    h.frameCount = rq.frameCount;
    FrameSource src;
    if (!src.init(h, paths, nullptr)) { err = "\"" + rq.role + "\": " + src.err; return false; }
    F.role = rq.role;
    F.frames = src.count;
    F.w = (int)src.n.w; F.h = (int)src.n.h; F.ch = (int)src.n.ch;
    F.expected = src.perFile ? (uint32_t)paths.size() : (uint32_t)src.n.frames;
    if (F.expected < F.frames) F.expected = F.frames;
    // The plane rule, mirrored from the local fold rather than from
    // runTemporalStats: a mosaic is four planes only when the frame really has
    // one channel, and an RGB frame with a CFA declaration is one plane. Same
    // condition, same table (CFA_MAP_S == state.h's CFA_MAP).
    F.nPl = (head.cfaType != 0 && F.ch == 1) ? 4 : 1;
    if (F.frames < (uint32_t)setfold::kMinFrames) return true;   // a FACT, not an error
    const size_t samples = (size_t)F.w * F.h * F.ch;
    if (samples > setfold::kMaxSamples) {
        err = "\"" + rq.role + "\": the frame is larger than 32M samples";
        return false;
    }
    F.M.assign(samples, std::numeric_limits<double>::quiet_NaN());
    F.corr.assign(samples, std::numeric_limits<double>::quiet_NaN());
    F.plane.assign(samples, 0);
    {
        std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
        std::vector<uint32_t> cnt(samples, 0);
        std::vector<uint8_t> raw;
        std::vector<float> pix;
        for (uint32_t f = 0; f < src.count; f++) {
            uint32_t ow = 0, oh = 0;
            if (!src.read(f, 0, 0, (uint32_t)F.w, (uint32_t)F.h, raw, ow, oh)) {
                err = "\"" + rq.role + "\": " + src.err;
                return false;
            }
            pix.resize(samples);
            toFloatSamples(raw.data(), src.n.dtype, samples, pix.data());
            for (size_t i = 0; i < samples; i++) {
                const double v = pix[i];
                if (!std::isfinite(v)) { F.nonFinite++; continue; }
                sum[i] += v; sum2[i] += v * v; cnt[i]++;
            }
        }
        for (int y = 0; y < F.h; y++)
            for (int x = 0; x < F.w; x++) {
                const uint8_t p = F.nPl == 4
                    ? (uint8_t)planeOf(head.cfaType, head.cfaPattern, (uint32_t)x, (uint32_t)y)
                    : (uint8_t)0;
                for (int c = 0; c < F.ch; c++) {
                    const size_t i = ((size_t)y * F.w + x) * F.ch + c;
                    F.plane[i] = p;
                    double m = 0, cr = 0;
                    if (!setfold::pixelMeanCorr(sum[i], sum2[i], cnt[i], m, cr)) {
                        F.dropped++;
                        continue;
                    }
                    F.M[i] = m; F.corr[i] = cr;
                }
            }
    }
    setfold::reduceOne(F.M.data(), F.corr.data(), F.plane.data(), samples, F.pl);
    F.shade = detrendProbeT(F.M.data(), F.w, F.h, F.ch, F.plane.data(), F.nPl);
    F.folded = true;
    if (!keep) {
        F.M.clear(); F.M.shrink_to_fit();
        F.corr.clear(); F.corr.shrink_to_fit();
        F.plane.clear(); F.plane.shrink_to_fit();
    }
    return true;
}

static std::string setPlaneKey(int p, const char* leaf) {
    char b[48];
    snprintf(b, sizeof b, "p%d.%s", p, leaf);
    return b;
}

static void putShade(std::vector<MItem>& col, int nPl, const DetrendReport& S) {
    for (int p = 0; p < nPl; p++) {
        col.push_back({ 0u, setPlaneKey(p, "shade_pp"), S.pl[p].ppDn, {} });
        col.push_back({ 0u, setPlaneKey(p, "shade_pct"),
                        S.pl[p].pctOk ? S.pl[p].pct : 0.0, {} });
        col.push_back({ 0u, setPlaneKey(p, "shade_pct_ok"), S.pl[p].pctOk ? 1.0 : 0.0, {} });
    }
}

// Parity, for a BUILT-IN. §10 matches a plugin's name and version; there is no
// dll here and no descriptor to version, so what is matched is the FORM the
// fold declares - see core/setfold.h for why that, and not the viewer version,
// is the thing whose equality makes the answer mean one thing.
//
// NOT ONE BUILD-DERIVED STRING APPEARS IN THIS REFUSAL, and that is the point
// PR #135 paid for on the plugin side: an equality check on a build-derived
// string makes two peers that agree about every line refuse each other, which
// is the accident parity exists to prevent, arriving from the other side. The
// version this peer was built from is real provenance and it travels in the
// reply trailer, where it is never compared - naming it here would invite the
// reader to think the BUILD is what mismatched, when what mismatched is
// printed in full immediately above.
static bool setFoldParityOk(const std::string& clientForm, uint32_t join, std::string& why) {
    const std::string mine = setfold::foldForm(join);
    if (!clientForm.empty() && clientForm == mine) return true;
    const std::string head = "set fold parity refused: the fold ";
    if (clientForm.empty()) {
        why = head + "declares nothing on the client and is\n  <" + mine + ">\n  on the peer\n"
              "  A set analyzer is a built-in, so there is no descriptor version to "
              "match: what is matched is the FORM the fold declares, and a form that "
              "was never declared cannot be matched.";
        return false;
    }
    why = head + "is\n  <" + clientForm + ">\n  on the client and\n  <" + mine +
          ">\n  on the peer\n"
          "  Neither form wins: this is not folded with the peer's build, and it is "
          "not handed back to the client to fold here.\n"
          "  A set analyzer's name is worn by more than one estimator, so a fold "
          "that is not the one the row was named for is the failure this check "
          "exists to make impossible.\n"
          "  The two builds' VERSIONS are not what differ here and are deliberately "
          "not quoted: a version is provenance, it rides in the reply, and it is "
          "never matched - two peers one commit apart fold identically.";
    return false;
}

static void runSetFold(const MeasureReqHead& head,
                       const std::vector<std::string>& paths,
                       const std::vector<RoiRect>& rois,
                       const std::string& clientForm, uint32_t join,
                       const std::vector<SetRoleReq>& roles) {
    if (!rois.empty()) {
        // A set analyzer reads the frame. Measuring a rectangle here would
        // answer a different population than the local path and call it the
        // same number, which is the one thing this op must not do.
        sendErr("set fold refused: a set analyzer reads the whole frame, so this "
                "op takes no ROI (it was sent " + std::to_string(rois.size()) + ")");
        return;
    }
    std::string why;
    if (!setFoldParityOk(clientForm, join, why)) { sendErr(why); return; }
    if (join == SJ_DIFF && roles.size() != 2) {
        std::string have;
        for (const auto& r : roles) have += (have.empty() ? "" : ", ") + ("\"" + r.role + "\"");
        sendErr("set fold refused: the difference join is between two roles, and "
                "this request names " + std::to_string(roles.size()) + " (" +
                (have.empty() ? "none" : have) + ")");
        return;
    }
    std::vector<RoleFold> folds(roles.size());
    std::vector<std::vector<MItem>> cols;
    std::string err;
    size_t at = 0;
    uint32_t usedTotal = 0, expectedTotal = 0;
    for (size_t r = 0; r < roles.size(); r++) {
        std::vector<std::string> mine(paths.begin() + at, paths.begin() + at + roles[r].nPaths);
        at += roles[r].nPaths;
        if (!foldOneRole(head, mine, roles[r], join != SJ_NONE, folds[r], err)) {
            sendErr("set fold refused: " + err);
            return;
        }
        usedTotal += folds[r].frames;
        expectedTotal += folds[r].expected;
    }
    for (const RoleFold& F : folds) {
        cols.emplace_back();
        std::vector<MItem>& col = cols.back();
        col.push_back({ 1u, "role", 0.0, F.role });
        col.push_back({ 0u, "frames", (double)F.frames, {} });
        col.push_back({ 0u, "expected", (double)F.expected, {} });
        col.push_back({ 0u, "w", (double)F.w, {} });
        col.push_back({ 0u, "h", (double)F.h, {} });
        col.push_back({ 0u, "ch", (double)F.ch, {} });
        col.push_back({ 0u, "planes", (double)F.nPl, {} });
        col.push_back({ 0u, "cfa", (double)head.cfaType, {} });
        col.push_back({ 0u, "cfa_pattern", (double)head.cfaPattern, {} });
        col.push_back({ 0u, "folded", F.folded ? 1.0 : 0.0, {} });
        if (!F.folded) continue;       // frames < 2: the facts, and no sums
        col.push_back({ 0u, "nonfinite", (double)F.nonFinite, {} });
        col.push_back({ 0u, "dropped", (double)F.dropped, {} });
        for (int p = 0; p < F.nPl; p++) {
            col.push_back({ 0u, setPlaneKey(p, "s1"), F.pl[p].s1, {} });
            col.push_back({ 0u, setPlaneKey(p, "s2"), F.pl[p].s2, {} });
            col.push_back({ 0u, setPlaneKey(p, "cs"), F.pl[p].cs, {} });
            col.push_back({ 0u, setPlaneKey(p, "n"),  F.pl[p].n,  {} });
        }
        putShade(col, F.nPl, F.shade);
    }
    // ---- the join -----------------------------------------------------------
    // Omitted rather than refused when the two roles do not line up: the
    // sentence for "these two are not one shape" is settled in
    // core/app/setanalysis.inc, in the words of the ROLES it is about, and the
    // client can only write it if the role columns above arrive. So the peer
    // sends the facts and says join_ok = 0; the refusal is written where its
    // words live.
    if (join == SJ_DIFF) {
        cols.emplace_back();
        std::vector<MItem>& col = cols.back();
        col.push_back({ 1u, "join", 0.0, "difference" });
        const RoleFold& A = folds[0];
        const RoleFold& B = folds[1];
        const bool lined = A.folded && B.folded && A.w == B.w && A.h == B.h &&
                           A.ch == B.ch && A.nPl == B.nPl;
        col.push_back({ 0u, "join_ok", lined ? 1.0 : 0.0, {} });
        if (lined) {
            const size_t samples = (size_t)A.w * A.h * A.ch;
            setfold::PairAcc pl[4];
            setfold::reducePair(A.M.data(), A.corr.data(), B.M.data(), B.corr.data(),
                                A.plane.data(), samples, pl);
            col.push_back({ 0u, "planes", (double)A.nPl, {} });
            for (int p = 0; p < A.nPl; p++) {
                col.push_back({ 0u, setPlaneKey(p, "s1a"), pl[p].s1a, {} });
                col.push_back({ 0u, setPlaneKey(p, "s1b"), pl[p].s1b, {} });
                col.push_back({ 0u, setPlaneKey(p, "dq"),  pl[p].dq,  {} });
                col.push_back({ 0u, setPlaneKey(p, "cs"),  pl[p].cs,  {} });
                col.push_back({ 0u, setPlaneKey(p, "n"),   pl[p].n,   {} });
            }
            // the shading of D, because D is the picture the client's ratio is
            // taken over and a figure has to describe the picture beside it
            std::vector<double> D(samples);
            setfold::differenceImage(A.M.data(), B.M.data(), samples, D.data());
            putShade(col, A.nPl,
                     detrendProbeT(D.data(), A.w, A.h, A.ch, A.plane.data(), A.nPl));
        }
    }
    // #46's 計算者欄 for a built-in: the viewer version, and no dll, because
    // there is none and that absence is the statement.
    //
    // This is the ONE place the peer's build string is used, and it is a
    // provenance field: it is written into the reply, printed beside the row,
    // and NEVER compared. That separation is the whole reason it is safe to
    // put a build-derived string on this wire at all (PR #135).
    MeasureProv prov;
    prov.name = "set fold (built-in)";
    prov.version = viewerVersion();
    prov.file = "";
    prov.path = plugin_host::exeDir();
    prov.expected = expectedTotal;
    sendMeasureReply(usedTotal, cols, {}, &prov);
}

// ---- plugin analysis on the peer (docs/reference/abi-v3.md §10) ---------------------
//
// One frame, materialized as f32 exactly as the local host would, handed to
// whichever descriptor generation registered the analyzer. Shared by
// MOP_ANALYZER (name only, unchanged since it shipped) and MOP_PLUGIN_ANALYZE
// (name AND version): the two ops differ in what they ADMIT and never in what
// they compute, which is the only way "the same plugin measured it" survives
// having two doors.
static bool runFrameAnalyzerOn(const AnalyzerPluginInfo& ana,
                               const MeasureReqHead& head,
                               const std::vector<std::string>& paths,
                               const std::vector<RoiRect>& rois,
                               std::vector<std::vector<MItem>>& cols,
                               std::vector<MSeries>& series,
                               std::string& err) {
    ServedFile n;
    if (!openServed(n, paths[0], err)) return false;
    TileReq full{};
    full.frame = head.frame0;
    full.x = 0; full.y = 0; full.w = (uint32_t)n.w; full.h = (uint32_t)n.h;
    full.step = 1;
    std::vector<uint8_t> raw;
    uint32_t ow = 0, oh = 0;
    if (!readRegion(n, full, raw, ow, oh, err)) return false;
    std::vector<float> pix((size_t)ow * oh * n.ch);
    toFloatSamples(raw.data(), n.dtype, pix.size(), pix.data());
    raw.clear();
    raw.shrink_to_fit();

    psFrame fr{};
    fr.w = ow; fr.h = oh; fr.ch = (uint32_t)n.ch;
    fr.dtype = PS_DTYPE_F32;
    fr.loc = PS_MEM_CPU;
    fr.data = pix.data();
    fr.pitch_bytes = (size_t)ow * n.ch * sizeof(float);
    fr.black = head.black; fr.white = head.white;
    fr.cfa_type = (int32_t)head.cfaType;
    fr.cfa_pattern = (int32_t)head.cfaPattern;
    fr.pts_us = -1;
    fr.name = paths[0].c_str();

    uint32_t nCols = head.nRois ? head.nRois : 1;
    cols.assign(nCols, {});
    series.clear();
    MSink ctx{ &cols, &series, 0 };
    char perr[512];
    for (uint32_t c = 0; c < nCols; c++) {
        ctx.col = c;
        psRect rr{};
        const psRect* rp = nullptr;
        if (head.nRois) {
            rr.x = std::min(rois[c].x, fr.w); rr.y = std::min(rois[c].y, fr.h);
            rr.w = std::min(rois[c].w, fr.w - rr.x); rr.h = std::min(rois[c].h, fr.h - rr.y);
            rp = &rr;
        }
        perr[0] = 0;
        int32_t rc;
        // Three descriptor generations, one measurement path. The peer runs the
        // SAME dlls the local host runs, so a v3 analyzer that only the local
        // side could call would turn "MEASURE runs the same plugin on both
        // sides" into a claim that quietly stopped being true.
        if (ana.abi == 3) {
            psAnalyzeSink3 sink{ &ctx, mNum, mTxt, mSer, {} };
            rc = ana.v3.analyze(&fr, rp, &sink, perr, sizeof perr);
        } else if (ana.abi == 2) {
            psAnalyzeSink2 sink{ &ctx, mNum, mTxt, mSer, {} };
            rc = ana.v2.analyze(&fr, rp, &sink, perr, sizeof perr);
        } else {
            psAnalyzeSink sink{ &ctx, mNum, mTxt };
            rc = ana.v1.analyze(&fr, rp, &sink, perr, sizeof perr);
        }
        if (rc != 0) {
            err = ana.name + ": " + (perr[0] ? perr : "analyzer failed");
            return false;
        }
    }
    return true;
}

namespace {
// A psStack served from the peer's own streaming reader (docs/reference/abi-v3.md §9.3 +
// §5.1). This is what pull was FOR: the local host returns a pointer because
// every frame is already resident, and the peer - which holds nothing resident
// and may be asked about 300 frames of 48 MB - reads frame i on demand and
// gives the slot back on release_frame. §9.2's bounded window, on the peer,
// with no new transport: the same FrameSource the aggregate ops stream through.
//
// Everything large is heap: the peer's main thread is the only thread it has,
// and PR #133 is what a megabyte-class local costs on Windows.
struct PeerStack {
    FrameSource* src = nullptr;
    uint32_t w = 0, h = 0, ch = 0, dtype = 0;
    std::vector<std::vector<float>> pix;   // one per index; freed on release
    std::vector<uint8_t> resident;         // 1 = pix[i] holds this frame's samples
    std::vector<psFrame> fr;
    std::vector<std::string> names;        // psFrame::name, stable for the call
    std::vector<uint8_t> raw;              // scratch for one read, reused
    size_t held = 0, budget = 0;
    uint64_t gets = 0, releases = 0, peakHeld = 0;
    std::string err;                       // why a NULL was a NULL, for the log
};
const psFrame* peerStackGet(void* ctx, uint32_t index) {
    PeerStack* s = (PeerStack*)ctx;
    if (!s) return nullptr;
    s->gets++;
    // Out of range is NULL, never a clamp: §5.1 makes NULL the one honest
    // failure, and a clamp answers a question about frame 9 with frame 2.
    if (index >= s->fr.size()) { s->err = "frame " + std::to_string(index) +
                                          " is past the end of the stack"; return nullptr; }
    if (s->resident[index]) return &s->fr[index];
    const size_t samples = (size_t)s->w * s->h * s->ch;
    const size_t bytes = samples * sizeof(float);
    // The pin budget the ABI already anticipated ("frame lost, transport
    // failure, PIN BUDGET EXCEEDED"). A plugin that never releases is correct
    // and greedy (§5.1); on a workstation that costs nothing because the frames
    // were resident anyway, and on a compute node it is the difference between
    // a refusal and the OOM killer taking the ssh session with it.
    if (s->held + bytes > s->budget) {
        s->err = "pin budget exceeded at frame " + std::to_string(index) + " (" +
                 std::to_string(s->budget >> 20) + " MB; release frames, or raise "
                 "VIEWER_SERVE_PIN_BUDGET_MB on the peer)";
        return nullptr;
    }
    uint32_t ow = 0, oh = 0;
    if (!s->src->read(index, 0, 0, s->w, s->h, s->raw, ow, oh)) {
        s->err = s->src->err;
        return nullptr;
    }
    s->pix[index].resize(samples);
    toFloatSamples(s->raw.data(), s->dtype, samples, s->pix[index].data());
    s->resident[index] = 1;
    s->held += bytes;
    if (s->held > s->peakHeld) s->peakHeld = s->held;
    s->fr[index].data = s->pix[index].data();
    return &s->fr[index];
}
void peerStackRelease(void* ctx, uint32_t index) {
    PeerStack* s = (PeerStack*)ctx;
    if (!s || index >= s->fr.size()) return;   // releasing nothing is not an error
    s->releases++;
    if (!s->resident[index]) return;
    // Here the mouth earns its keep. In-process this is a no-op; on the peer it
    // is the recycle that lets a 300-frame stack be measured inside a window,
    // and re-getting the same index afterwards is legal - it reads again.
    s->held -= s->pix[index].size() * sizeof(float);
    std::vector<float>().swap(s->pix[index]);
    s->resident[index] = 0;
    s->fr[index].data = nullptr;
}
size_t pinBudgetBytes() {
    static size_t b = [] {
        const char* e = getenv("VIEWER_SERVE_PIN_BUDGET_MB");
        long mb = e ? atol(e) : 0;
        if (mb < 1) mb = 1024;               // a stack's worth of a workstation
        return (size_t)mb << 20;
    }();
    return b;
}
}  // namespace

// The peer half of §5/§6 for a stack analyzer, in the order the contract fixes:
// the gate is settled BEFORE the call, from the peer's own count of the frames
// it will serve, and the facts (n, N) are passed in for the plugin to read
// after it has been let in.
static bool runStackAnalyzerOn(const StackAnalyzerPluginInfo& sa,
                               const MeasureReqHead& head,
                               const std::vector<std::string>& paths,
                               const std::vector<RoiRect>& rois,
                               std::vector<std::vector<MItem>>& cols,
                               std::vector<MSeries>& series,
                               uint32_t& framesUsed, uint32_t& expected,
                               std::string& err) {
    FrameSource src;
    if (!src.init(head, paths, nullptr)) { err = src.err; return false; }
    const uint32_t n = src.count;
    // N is what the PEER can count, and nothing more: the frames the file
    // declares it holds (or the files it was handed). §10 says the partial-load
    // facts are the peer's, not a number the client asserted about itself.
    const uint32_t N = src.perFile ? (uint32_t)paths.size()
                                   : std::max(n, (uint32_t)src.n.frames);
    std::string what = paths[0];
    { size_t sl = what.find_last_of("/\\"); if (sl != std::string::npos) what = what.substr(sl + 1); }

    // ---- the gate, before the call, exactly as the local host words it
    if (n < sa.minFrames) {
        err = sa.name + " needs at least " + std::to_string(sa.minFrames) +
              " frames; \"" + what + "\" has " + std::to_string(n) +
              (n < N ? " of " + std::to_string(N) : "") + " on the peer";
        return false;
    }

    PeerStack ps;
    ps.src = &src;
    ps.w = (uint32_t)src.n.w; ps.h = (uint32_t)src.n.h; ps.ch = (uint32_t)src.n.ch;
    ps.dtype = src.n.dtype;
    ps.budget = pinBudgetBytes();
    ps.pix.resize(n);
    ps.resident.assign(n, 0);
    ps.names.resize(n);
    ps.fr.assign(n, psFrame{});
    for (uint32_t i = 0; i < n; i++) {
        ps.names[i] = src.perFile ? paths[i]
                                  : what + " #" + std::to_string(src.xBase() + i);
        psFrame& f = ps.fr[i];
        f.w = ps.w; f.h = ps.h; f.ch = ps.ch;
        f.dtype = PS_DTYPE_F32;
        f.loc = PS_MEM_CPU;
        f.data = nullptr;                 // filled by get_frame, freed by release
        f.pitch_bytes = (size_t)ps.w * ps.ch * sizeof(float);
        f.black = head.black; f.white = head.white;
        f.cfa_type = (int32_t)head.cfaType;
        f.cfa_pattern = (int32_t)head.cfaPattern;
        f.pts_us = -1;
        f.name = ps.names[i].c_str();
    }

    psStack st = {};
    st.frames = n;
    st.expected = N;
    st.w = ps.w; st.h = ps.h; st.ch = ps.ch;
    st.dtype = PS_DTYPE_F32;
    st.name = what.c_str();
    st.meta_json = nullptr;
    st.ctx = &ps;
    st.get_frame = peerStackGet;
    st.release_frame = peerStackRelease;

    uint32_t nCols = head.nRois ? head.nRois : 1;
    cols.assign(nCols, {});
    series.clear();
    MSink ctx{ &cols, &series, 0 };
    char perr[512];
    for (uint32_t c = 0; c < nCols; c++) {
        ctx.col = c;
        psRect rr{};
        const psRect* rp = nullptr;
        if (head.nRois) {
            rr.x = std::min(rois[c].x, ps.w); rr.y = std::min(rois[c].y, ps.h);
            rr.w = std::min(rois[c].w, ps.w - rr.x); rr.h = std::min(rois[c].h, ps.h - rr.y);
            rp = &rr;
        }
        perr[0] = 0;
        psAnalyzeSink3 sink{ &ctx, mNum, mTxt, mSer, {} };
        int32_t rc = sa.v3.analyze_stack(&st, rp, &sink, perr, sizeof perr);
        if (rc != 0) {
            // The plugin's own sentence first; the peer's reason for a NULL is
            // appended because from inside the plugin "get_frame returned NULL"
            // is all there was to say, and which of the three NULLs it was is
            // knowledge only this side has.
            err = sa.name + ": " + (perr[0] ? perr : "the analyzer failed without saying why");
            if (!ps.err.empty()) err += " [peer: " + ps.err + "]";
            return false;
        }
    }
    framesUsed = n;
    expected = N;
    return true;
}

// ---- parity (docs/reference/abi-v3.md §10, #104 judgment 8) -------------------------
//
// name + version equality, and a mismatch is refused with BOTH versions on the
// line and no fallback of any kind. The reasoning the spec records: a version
// field (#46 stage 2) is what first made "is the peer the same instrument as
// me?" a question that can be ASKED over a link. Before it, parity was
// unmeasurable and the honest thing was to not claim it; now it is a
// declaration that can be compared, so refusing to compare it would be a
// choice.
//
// What it is NOT is a proof of identical arithmetic - two builds can declare
// one version and differ. Neither side can measure ULP agreement over a pipe,
// and the spec says so: this is the discipline of DECLARATION, the same one
// the provenance line runs on. A wrong declaration is the plugin author's
// fault and is visible; a missing one is nobody's fault and is not, which is
// why the missing case is refused rather than waved through.
static bool parityOk(const std::string& name, const std::string& clientVer,
                     const AnalyzerPluginInfo* fa, const StackAnalyzerPluginInfo* sa,
                     std::string& why) {
    const std::string& peerVer  = fa ? fa->version : sa->version;
    const std::string& peerFile = fa ? fa->file    : sa->file;
    if (!clientVer.empty() && clientVer == peerVer) return true;
    const std::string head = "plugin parity refused: \"" + name + "\" ";
    if (clientVer.empty() || peerVer.empty()) {
        why = head + (clientVer.empty() ? "declares no version on the client"
                                        : "is <" + clientVer + "> on the client") +
              " and " +
              (peerVer.empty() ? "none on the peer" : "<" + peerVer + "> on the peer") +
              " (" + peerFile + ")\n"
              "  ABI v3 §10 matches name AND version, and a version that was never "
              "declared cannot be matched.\n"
              "  V1/V2 descriptors carry no version field - those reach the peer through "
              "MOP_ANALYZER, which checks the name only and says so.";
        return false;
    }
    why = head + "is <" + clientVer + "> on the client and <" + peerVer +
          "> on the peer (" + peerFile + ")\n"
          "  Neither version wins: this is not run with the peer's build, and it is not "
          "handed back to the client to run here.\n"
          "  A number from a plugin other than the one asked for is the failure this "
          "check exists to make impossible.";
    return false;
}

static void runPluginAnalyze(const MeasureReqHead& head,
                             const std::vector<std::string>& paths,
                             const std::vector<RoiRect>& rois,
                             const std::string& analyzer,
                             const std::string& clientVersion,
                             uint32_t target) {
    ensurePlugins();
    const AnalyzerPluginInfo* fa = nullptr;
    const StackAnalyzerPluginInfo* sa = nullptr;
    std::string have;
    if (target == MT_STACK) {
        for (const auto& a : plugin_host::stackAnalyzers()) {
            if (a.name == analyzer) sa = &a;
            have += (have.empty() ? "" : ", ") + a.name;
        }
    } else {
        for (const auto& a : plugin_host::analyzers()) {
            if (a.name == analyzer) fa = &a;
            have += (have.empty() ? "" : ", ") + a.name;
        }
    }
    if (!fa && !sa) {
        sendErr(std::string("plugin analysis refused: no ") +
                (target == MT_STACK ? "stack" : "frame") + " analyzer named \"" +
                analyzer + "\" on the peer (peer has: " +
                (have.empty() ? "none" : have) + ")");
        return;
    }
    std::string why;
    if (!parityOk(analyzer, clientVersion, fa, sa, why)) { sendErr(why); return; }

    std::vector<std::vector<MItem>> cols;
    std::vector<MSeries> series;
    std::string err;
    MeasureProv prov;
    uint32_t used = 1, expected = 1;
    if (sa) {
        if (!runStackAnalyzerOn(*sa, head, paths, rois, cols, series, used, expected, err)) {
            sendErr(err); return;
        }
        prov.name = sa->name; prov.version = sa->version;
        prov.file = sa->file; prov.path = sa->path;
    } else {
        if (!runFrameAnalyzerOn(*fa, head, paths, rois, cols, series, err)) {
            sendErr(err); return;
        }
        prov.name = fa->name; prov.version = fa->version;
        prov.file = fa->file; prov.path = fa->path;
    }
    prov.expected = expected;
    sendMeasureReply(used, cols, series, &prov);
}

static void handleMeasure(Buf& in) {
    MeasureReqHead head{};
    if (in.rd + sizeof head > in.b.size()) { sendErr("bad MEASURE"); return; }
    memcpy(&head, in.b.data() + in.rd, sizeof head);
    in.rd += sizeof head;
    // The subject is ONE ARRAY INSIDE A MATERIALISATION and there is no path in
    // this request (protocol 14). Gated on the SERVED version, so the
    // VIEWER_SERVE_PROTOCOL seam keeps testing a peer that could exist: told to
    // be a 13, this answers "bad MEASURE header" below - which is exactly what
    // a real v13 answers a request with no paths in it, and exactly why the
    // client refuses from the number instead of sending and reading that back.
    const bool keyed = (head.flags & MRF_KEYED) && servedVersion() >= 14;
    if ((head.nPaths == 0 && !keyed) || head.nPaths > 100000 || head.nRois > 4096 ||
        head.cfaType > 2 || head.cfaPattern > 3) {
        sendErr("bad MEASURE header");
        return;
    }
    // ...and a key WITH paths is two subjects. Refused rather than resolved by
    // precedence: whichever this peer picked, the other end meant the other one.
    if (keyed && head.nPaths != 0) {
        sendErr("a keyed measurement addresses one array, not a list of files");
        return;
    }
    std::vector<std::string> paths(head.nPaths);
    for (auto& p : paths) if (!in.getStr(p)) { sendErr("bad MEASURE paths"); return; }
    std::string analyzer, params;
    if (!in.getStr(analyzer) || !in.getStr(params)) { sendErr("bad MEASURE"); return; }
    std::vector<RoiRect> rois(head.nRois);
    for (auto& r : rois) {
        uint32_t v[4];
        for (uint32_t& x : v) if (!in.getU32(x)) { sendErr("bad MEASURE rois"); return; }
        r.x = v[0]; r.y = v[1]; r.w = v[2]; r.h = v[3];
    }

    // The recipe, read AFTER the rois so nothing an older op parses moved, and
    // announced by a BIT rather than by "bytes remain" - MEASURE already has
    // op-dependent blocks back here (the parity block, the role block), so the
    // position is fixed and the presence is declared. `flags` was reserved-0,
    // which is what every v10 and older client writes.
    RawWire mrw{}; bool haveMrw = false;
    if (head.flags & MRF_RAW_RECIPE) {
        if (in.rd + sizeof mrw > in.b.size()) { sendErr("bad MEASURE raw recipe"); return; }
        memcpy(&mrw, in.b.data() + in.rd, sizeof mrw);
        in.rd += sizeof mrw;
        haveMrw = true;
    }
    const RawWire* mrwp = haveMrw ? &mrw : nullptr;
    // ...and the key behind the recipe, in BIT ORDER - the same rule the META /
    // TILE trailer keeps, for the same reason: two optional blocks cannot share
    // one "if bytes remain".
    MeasureKey mkey;
    if (keyed) {
        if (!in.getStr(mkey.key) || !in.getU32(mkey.node)) {
            sendErr("truncated keyed trailer");
            return;
        }
        // Two declarations of what the pixels ARE. The words are META's, so a
        // person meets one sentence for one mistake wherever they meet it.
        if (haveMrw) {
            sendErr("a raw recipe does not apply to an array inside a materialisation");
            return;
        }
        // The ops whose own blocks name FILES. The client refuses these before
        // sending; this end refuses them too, because a peer that measured the
        // wrong bytes on a request it half-understood is the failure the whole
        // trailer discipline exists to prevent.
        if (head.op != MOP_TEMPORAL_STATS && head.op != MOP_FRAME_ROI_STATS) {
            sendErr("this measure op addresses files, and this request addresses one "
                    "array inside something this peer materialised");
            return;
        }
    }
    const MeasureKey* mkp = keyed ? &mkey : nullptr;
    if (head.op == MOP_TEMPORAL_STATS)  { runTemporalStats(head, paths, rois, mrwp, mkp); return; }
    if (head.op == MOP_FRAME_ROI_STATS) { runFrameRoiStats(head, paths, rois, mrwp, mkp); return; }
    if (head.op == MOP_PLUGIN_ANALYZE) {
        // The parity block, read AFTER the rois so that nothing an older op
        // parses moved. A truncated one is a bad request, not a parity failure:
        // "no version arrived" and "no version was declared" are different
        // claims and only the second one is the plugin author's.
        std::string clientVersion;
        uint32_t target = MT_FRAME;
        if (!in.getStr(clientVersion) || !in.getU32(target)) {
            sendErr("bad MEASURE parity block");
            return;
        }
        if (target > MT_STACK) { sendErr("bad MEASURE target"); return; }
        if (servedVersion() < 7) { sendErr("unknown measure op"); return; }
        runPluginAnalyze(head, paths, rois, analyzer, clientVersion, target);
        return;
    }
    if (head.op == MOP_SET_FOLD) {
        // On the op NUMBER, before a byte of the role block is read - which is
        // what a peer that predates this op does, because it has nothing to
        // read the block with. VIEWER_SERVE_PROTOCOL makes this peer be that
        // peer rather than imitate it.
        if (servedVersion() < 8) { sendErr("unknown measure op"); return; }
        // The role block, read AFTER the rois for the reason the parity block
        // is: an op an older peer knows is parsed by it identically, and the op
        // it does not know it never reaches.
        std::string foldForm;
        uint32_t join = SJ_NONE, nRoles = 0;
        if (!in.getStr(foldForm) || !in.getU32(join) || !in.getU32(nRoles)) {
            sendErr("bad MEASURE role block");
            return;
        }
        if (join > SJ_DIFF) { sendErr("bad MEASURE join"); return; }
        if (nRoles == 0 || nRoles > 64) { sendErr("bad MEASURE role count"); return; }
        std::vector<SetRoleReq> roles(nRoles);
        uint64_t sum = 0;
        for (auto& r : roles) {
            if (!in.getStr(r.role) || !in.getU32(r.nPaths) || !in.getU32(r.frame0) ||
                !in.getU32(r.frameCount)) {
                sendErr("bad MEASURE role block");
                return;
            }
            if (r.nPaths == 0) {
                sendErr("bad MEASURE role block: \"" + r.role + "\" owns no path");
                return;
            }
            sum += r.nPaths;
        }
        // The companion count array has ONE invariant and it is checked rather
        // than trusted: a mis-summed block would silently re-slice one role's
        // frames into another's, and the answer would look perfectly normal.
        if (sum != (uint64_t)head.nPaths) {
            sendErr("bad MEASURE role block: the roles claim " + std::to_string(sum) +
                    " path(s) and the head declared " + std::to_string(head.nPaths));
            return;
        }
        runSetFold(head, paths, rois, foldForm, join, roles);
        return;
    }
    if (head.op != MOP_ANALYZER) { sendErr("unknown measure op"); return; }

    ensurePlugins();
    const AnalyzerPluginInfo* ana = nullptr;
    std::string have;
    for (const auto& a : plugin_host::analyzers()) {
        if (a.name == analyzer) ana = &a;
        have += (have.empty() ? "" : ", ") + a.name;
    }
    if (!ana) { sendErr("analyzer not found: " + analyzer + " (server has: " + have + ")"); return; }

    std::vector<std::vector<MItem>> cols;
    std::vector<MSeries> series;
    std::string err;
    if (!runFrameAnalyzerOn(*ana, head, paths, rois, cols, series, err)) { sendErr(err); return; }
    sendMeasureReply(1, cols, series);
}

// ---------------------------------------------------------------- reader run
//
// The whole of MSG_READER_RUN. It answers with MSG_OK for every outcome
// including the failures, because the four facts adapter::Run separates
// (started / timedOut / exit / stderr) are FACTS about a run that happened on
// this machine, and an MSG_ERR string would fold them back into one sentence
// the client would have to parse. docs/features/remote/remote-reader-design.md §6 maps each
// outcome to the words a person reads; this end supplies the facts and never
// the words - except where the words are settled HERE, which is the gate (§2.1)
// and the check of what the reader returned (§5.2, shared with the local door).

// "Python 3.11.4 (/usr/bin/python3), numpy 1.26.4". ASKED ONCE PER RUN.
//
// It was asked once per interpreter per process and kept (#180 codex review),
// which is a claim about a peer's lifetime that a peer does not get to make.
// viewer-serve outlives the session it answers - hours, on a machine where
// somebody else may `pip install -U numpy` in the environment it was pointed at
// - and this string is BOTH what the client prints beside the pixels AND part
// of the cache key (§5.3). A stale one breaks the key in both directions: the
// same origin and reader keep hitting a materialisation the old numpy produced,
// and if anything else in the key does move, the NEW numpy's pixels are
// published under the OLD environment's name. The second is the exact thing
// putting `prov` in the key was for.
//
// The cost is one authoritative `python -c` provenance probe per READER_RUN -
// a few hundred milliseconds, once per Load. A cache hit pays it too, and that
// is the point: the hit is only a hit if the environment is still the one the
// key was computed under. The configured interpreter used to be started once
// merely to import numpy and then a second time to ask this question. Besides
// doing twice the work, that admitted two different answers across an upgrade;
// this one command both proves numpy is importable and supplies the key fact.
// Failure or silence is RO_NO_PYTHON, never a made-up provenance equal to `py`.
static bool pythonProvenance(const std::string& py, std::string& provenance,
                             std::string& why, std::string& detail) {
    adapter::Run r = adapter::run({ py, "-c",
        "import sys,numpy;print('Python %d.%d.%d (%s), numpy %s' % "
        "(sys.version_info[0],sys.version_info[1],sys.version_info[2],"
        "sys.executable,numpy.__version__))" }, 30000);
    detail = r.err;
    if (!r.started) {
        why = r.fail.empty() ? "the provenance probe could not start" : r.fail;
        return false;
    }
    if (r.timedOut) {
        why = "the provenance probe did not finish within 30 seconds";
        return false;
    }
    if (r.exit != 0) {
        why = r.err.find("numpy") != std::string::npos
                ? "the provenance probe could not import or identify numpy"
                : "the provenance probe exited with status " + std::to_string(r.exit);
        return false;
    }
    provenance = r.out;
    while (!provenance.empty() &&
           (provenance.back() == '\n' || provenance.back() == '\r' ||
            provenance.back() == ' ' || provenance.back() == '\t'))
        provenance.pop_back();
    const size_t first = provenance.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        provenance.clear();
        why = "the provenance probe returned no Python/numpy identity";
        return false;
    }
    if (first) provenance.erase(0, first);
    return true;
}

static bool writeWholeFile(const std::filesystem::path& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(body.data(), (std::streamsize)body.size());
    return (bool)f;
}

// The first `n` bytes of a file, verbatim. The client is handed the header text
// EXACTLY as the harness wrote it (§5.2): it parses that text with the same
// parser it uses on a cache file of its own, so the layer, the axes, the unit,
// the note and the cfa cannot come out differently depending on which machine
// the reader ran on.
static std::string headOfFile(const std::string& path, uint64_t n) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return {};
    std::string s((size_t)n, '\0');
    f.read(&s[0], (std::streamsize)n);
    s.resize((size_t)f.gcount());
    return s;
}

// Everything the peer can say about what a reader left behind, before it hands
// out a key for it. The tree check is vns::checkTree (inside scanHeader) and
// the geometry check is serveLayout - BOTH shared with the client's own door,
// so a refusal reads the same wherever the reader ran. That sharing is the
// whole reason those two moved: #71's rule, applied to a new pair of doors.
static std::string checkReaderOutput(const std::string& cachePath, std::string& headerText) {
    std::ifstream f(std::filesystem::u8path(cachePath), std::ios::binary);
    if (!f) return "cannot open what the reader wrote";
    vns::Scan sc;
    const std::string serr = vns::scanHeader(f, sc);
    if (!serr.empty()) return serr;
    if (sc.blobs.empty()) return "the container declares no pixels";
    std::error_code ec;
    const uint64_t fsize = (uint64_t)std::filesystem::file_size(
                               std::filesystem::u8path(cachePath), ec);
    for (const vns::Blob& b : sc.blobs) {
        ServedFile probe;
        probe.dtype = npyTypeCode(b.dtype);
        if (probe.dtype == DT_COUNT) return "unsupported dtype " + b.dtype;
        probe.elemSize = dtypeSize(probe.dtype);
        probe.fortran = false;
        bool fellBack = false;
        std::string err;
        if (!serveLayout(probe, b.shape, NR_NATIVE, fellBack, err)) return err;
        if (!ec && b.at + b.nbytes > fsize)
            return "the stream ended " + std::to_string(b.at + b.nbytes - fsize) +
                   " byte(s) short of what it declared";
    }
    headerText = headOfFile(cachePath, sc.headerEnd);
    return {};
}

static void handleReaderRun(Buf& in) {
    auto reply = [](uint32_t outcome, const std::string& err, const std::string& errText,
                    const std::string& prov, const std::string& key,
                    const std::string& header) {
        Buf out;
        out.putU32(outcome);
        out.putStr(err);
        out.putStr(errText);
        out.putStr(prov);
        out.putStr(key);
        out.putStr(header);
        sendMsg(MSG_OK, out);
    };
    // FIRST, before the request is even read out: the gate is about this
    // MACHINE, not about this request, so a closed one must not be reportable
    // as "your third file was named wrong". Three states, three sentences
    // (§4.1) - and this is the one the peer owns.
    if (!serveReadersOpen()) {
        reply(RO_GATE_CLOSED, readerGateClosedText(), "", "", "", "");
        return;
    }
    std::string peerPath, func;
    uint32_t nFiles = 0;
    if (!in.getStr(peerPath) || !in.getStr(func) || !in.getU32(nFiles)) {
        sendErr("bad READER_RUN");
        return;
    }
    if (nFiles != 3) { sendErr("READER_RUN carries three files"); return; }
    std::string body[3];
    for (uint32_t i = 0; i < 3; i++) {
        std::string name, text;
        if (!in.getStr(name) || !in.getStr(text)) { sendErr("bad READER_RUN"); return; }
        // The name is checked against a CLOSED SET rather than sanitised. A
        // client that can name the file it writes here is a client that can
        // write anywhere on this disk, and the cheapest way not to have that
        // path is not to build it (§4.2).
        if (name != READER_RUN_FILES[i]) {
            sendErr("READER_RUN file " + std::to_string(i) + " is \"" + name +
                    "\": this op carries " + READER_RUN_FILES[0] + ", " +
                    READER_RUN_FILES[1] + " and " + READER_RUN_FILES[2] + ", in that order");
            return;
        }
        if (text.size() > READER_RUN_MAX_BYTES) {
            sendErr(name + " is " + std::to_string(text.size()) +
                    " bytes: a reader is text, and this op carries up to " +
                    std::to_string(READER_RUN_MAX_BYTES));
            return;
        }
        body[i] = std::move(text);
    }
    if (func.empty() || func.size() > 128) { sendErr("READER_RUN needs a function name"); return; }

    // VIEWER_SERVE_PYTHON is STRICT here, and that is a deliberate difference
    // from the local door. The viewer falls back to PATH when a configured
    // interpreter fails, because the person who configured it is sitting in
    // front of a Settings window that shows what happened. On a peer nobody is
    // looking: a run under an interpreter the operator did not choose is a
    // result nobody can reproduce, and "it worked, with the wrong numpy" is
    // exactly the silence this project spends its refusals on.
    std::string why, py;
    const char* cfgPy = getenv("VIEWER_SERVE_PYTHON");
    if (cfgPy && *cfgPy) {
        // Do not run a preliminary `import numpy`: the provenance command below
        // is the one authoritative observation for this RUN.
        py = cfgPy;
    } else {
        py = adapter::findPython("", why);
        if (py.empty()) { reply(RO_NO_PYTHON, why, "", "", "", ""); return; }
    }
    std::string prov, probeDetail;
    if (!pythonProvenance(py, prov, why, probeDetail)) {
        if (cfgPy && *cfgPy)
            why = std::string("VIEWER_SERVE_PYTHON is ") + cfgPy + ": " + why;
        reply(RO_NO_PYTHON, why, probeDetail, "", "", "");
        return;
    }

    // §5.3's key. The peer's OWN stat of the origin, because a client's stat of
    // a NAS mounted twice makes identity a lie; the reader's whole text, so
    // "the same reader" means the same bytes and not the same name; the
    // function; the ENVIRONMENT that produced the pixels; and the version the
    // key RULE was fixed at, so a peer that changes what a key covers cannot
    // hand back a hit computed under the old one.
    //
    // The current number is 14. Rule 13 first added `py` and `prov`; rule 14
    // invalidates entries a long-lived peer could have issued from its former
    // process-cached provenance. There is no fact in such an entry that can
    // prove whether the environment was still the one printed beside it, so a
    // current peer must never call it a hit.
    //
    // WHY THE ENVIRONMENT IS IN IT. The key covered the origin and the reader's
    // text and stopped there, so the answer to "which python ran this" was not
    // part of what made a hit a hit - while `prov` was reported beside the
    // pixels on every reply, cached or not. Point VIEWER_SERVE_PYTHON at a
    // different interpreter, or upgrade numpy under the one it names, and the
    // peer handed back the OLD environment's pixels with the NEW environment's
    // provenance printed next to them. That is not a stale cache; it is a
    // provenance line that says something untrue about the numbers it labels,
    // which is the one failure this project spends its refusals on.
    //
    // Both halves are here on purpose. `py` is the interpreter this peer was
    // told to use - two paths are two environments even when they report the
    // same versions. `prov` is what that interpreter SAYS it is (§4.3.1's
    // sentence: "Python 3.11.4 (/usr/bin/python3), numpy 1.26.4"), which is
    // what moves when numpy is upgraded in place and the path does not. It is
    // asked ON THIS RUN, not remembered from an earlier one (#180 codex
    // review): a peer process lives for hours, an upgrade under it is an
    // ordinary event, and a remembered answer would hand back the old
    // environment's pixels - or publish the new environment's pixels under the
    // old environment's name - for as long as that process lasted.
    //
    // A FOLDER ORIGIN is the third thing in it (#218 review). Its own mtime
    // moves when a child is added or removed and not when one is REWRITTEN,
    // which is the ordinary way a capture directory changes - so this key,
    // which recorded that mtime and left oSize at 0, called a folder whose
    // every frame had been replaced "the same input". adapter::folderIdentity
    // is the answer, and it is the LOCAL door's answer too: adapterCacheKey
    // had the identical hole, and one helper is what keeps the two ends from
    // disagreeing about whether a directory changed.
    //
    // No identity, no cache. The reply still has to carry a KEY - that is how
    // the client addresses the materialisation it is about to read - so the key
    // is made UNIQUE to this run instead of being made up out of the directory
    // mtime. It is written, it is addressable, and no later run can ever hit
    // it, which is what "caching must be disabled" means for a peer that
    // answers by key.
    std::error_code ec;
    const std::filesystem::path origin = std::filesystem::u8path(peerPath);
    uint64_t oMtime = 0, oSize = 0;
    std::string folderId;
    bool folder = false, uncacheable = false;
    {
        auto t = std::filesystem::last_write_time(origin, ec);
        if (!ec) oMtime = (uint64_t)t.time_since_epoch().count();
        std::error_code de;
        folder = std::filesystem::is_directory(origin, de) && !de;
        if (!folder) {
            auto s = std::filesystem::file_size(origin, ec);
            if (!ec) oSize = (uint64_t)s;
        } else {
            oMtime = 0;                  // the very number that was the defect
            uncacheable = !adapter::folderIdentity(peerPath, folderId);
        }
    }
    uint64_t h = readerHash("viewer-serve reader 14");
    h = readerHash(peerPath, h);
    h = readerHash(std::to_string(oMtime) + "|" + std::to_string(oSize), h);
    if (folder) h = readerHash("dir|" + folderId, h);
    for (int i = 0; i < 3; i++) h = readerHash(body[i], h);
    h = readerHash(func, h);
    h = readerHash(py, h);
    h = readerHash(prov, h);
    if (uncacheable) {
        static std::atomic<uint64_t> nonce{ 0 };
        const uint64_t n = nonce.fetch_add(1) + 1;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        h = readerHash("uncacheable|" + std::to_string((unsigned long long)servePid()) +
                       "|" + std::to_string(n) + "|" + std::to_string((long long)now), h);
    }
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    const std::string key = hex;
    std::string cachePath, tmpPath;
    if (!readerCachePath(key, cachePath) || !readerTempPath(key, tmpPath)) {
        sendErr("this peer has nowhere to keep what a reader produces "
                "(set VIEWER_SERVE_CACHE)");
        return;
    }
    // Whatever happens below, THIS process's temp goes away and nothing else
    // does. Every failure path used to delete `cachePath` - which is to say,
    // a contender that timed out or whose Python raised deleted the file a
    // DIFFERENT session had just published, and that session's next open then
    // re-ran a reader that had already succeeded.
    struct TempFile {
        std::string p;
        bool keep = false;
        ~TempFile() {
            if (keep || p.empty()) return;
            std::error_code e;
            std::filesystem::remove(std::filesystem::u8path(p), e);
        }
    } temp{ tmpPath };

    // The cache answer, and the ONE sentence that says exactly what did and did
    // not run. The reader and harness are skipped; the Python provenance probe
    // above still runs, because its answer is part of this run's key.
    auto replyFromCache = [&](const std::string& header) {
        reply(RO_OK, "",
              "read from cache - the reader and harness were not re-run; Python "
              "ran only the environment provenance probe.\n"
              "Edit the reader and press Load again to re-run it.",
              prov, key, header);
    };
    bool observedInvalid = false;
    ec.clear();
    if (std::filesystem::exists(std::filesystem::u8path(cachePath), ec)) {
        std::string header;
        const std::string cerr = checkReaderOutput(cachePath, header);
        if (cerr.empty()) {
            replyFromCache(header);
            return;
        }
        observedInvalid = true;
    }
    if (observedInvalid) {
        serveCacheJudgeHold();

        // Every change to `<key>.vstream` is made while this lock is held. Most
        // importantly, an INVALID observation made before the lock is never
        // acted on directly: recheck after acquisition. Another producer may
        // have replaced the bad bytes and already issued this key while we
        // waited. An absent key needs no mutation here; its producer takes this
        // same lock at publish time.
        ReaderKeyLock lock;
        std::string lockWhy;
        if (!lock.acquire(key, lockWhy)) {
            sendErr(lockWhy);
            return;
        }
        ec.clear();
        if (std::filesystem::exists(std::filesystem::u8path(cachePath), ec)) {
            std::string header;
            if (checkReaderOutput(cachePath, header).empty()) {
                replyFromCache(header);
                return;
            }
            // Still invalid while exclusively owned. Removal is safe now: no
            // conforming producer can publish between this verdict and act.
            std::error_code de;
            if (!std::filesystem::remove(std::filesystem::u8path(cachePath), de) || de) {
                sendErr("cannot remove an unusable reader cache entry: " + de.message());
                return;
            }
        }
    }

    // The three texts land in a directory of their own and leave with it. What
    // stays on this disk is the .vstream plus its empty lock rendezvous, never
    // other people's code: that is the part §3's form requires.
    std::filesystem::path td = std::filesystem::temp_directory_path(ec);
    char nm[64];
    const unsigned long pid = (unsigned long)
#if defined(_WIN32)
        _getpid();
#else
        getpid();
#endif
    snprintf(nm, sizeof nm, "viewer_serve_reader_%lu_%s", pid, key.c_str());
    td /= nm;
    std::filesystem::remove_all(td, ec);
    std::filesystem::create_directories(td, ec);
    struct Sweep {
        std::filesystem::path d;
        ~Sweep() { std::error_code e; std::filesystem::remove_all(d, e); }
    } sweep{ td };
    bool wrote = true;
    for (int i = 0; i < 3; i++) wrote &= writeWholeFile(td / READER_RUN_FILES[i], body[i]);
    if (!wrote) {
        reply(RO_NOT_STARTED, "cannot write the reader into " + td.u8string(), "", prov, "", "");
        return;
    }
    const std::string spec = (td / READER_RUN_FILES[0]).u8string() + ":" + func;
    const std::vector<std::string> argv = {
        py, "-u", (td / READER_RUN_FILES[2]).u8string(), spec, peerPath, "--stream"
    };
    const std::string errFile = (td / "reader.err").u8string();
    // 300 s, the local limit (core/app/session.inc). The policy lives with the
    // code that runs the process and never on the wire: one side deciding how
    // long the other side's patience is would be a setting with two owners.
    adapter::Run r = adapter::run(argv, 300000, nullptr, tmpPath, errFile, false);
    // BEFORE the reply, not on the way out of this function. The client is
    // entitled to treat the answer as proof that the code it sent is no longer
    // on this disk, and a destructor that runs after the bytes are on the wire
    // makes that a race it can lose. Nothing below needs the directory: what
    // the reader printed is already in `r`.
    std::filesystem::remove_all(td, ec);
    // Each of the three failures below leaves ONLY its own temp behind, which
    // `temp` then removes. None of them touches `cachePath`: this run never
    // published anything, so there is nothing of its own there to withdraw.
    if (!r.started) {
        reply(RO_NOT_STARTED, r.fail, r.err, prov, "", "");
        return;
    }
    if (r.timedOut) {
        reply(RO_TIMED_OUT, adapter::showCommand(argv), r.err, prov, "", "");
        return;
    }
    if (r.exit != 0) {
        // The traceback travels WHOLE (§6). The line numbers in it are the
        // client's own file's, because the file is the one the client sent -
        // which is what carrying the reader buys that a peer-side copy could
        // never promise.
        reply(RO_EXITED, "the reader exited with status " + std::to_string(r.exit),
              r.err, prov, "", "");
        return;
    }
    // VALIDATED FIRST, then published under the SAME per-key exclusion used by
    // invalid-entry removal. The shared lock is the promise: after any process
    // has issued `key`, every later producer rechecks its valid canonical and
    // leaves it in place. META/TILE can therefore use that key immediately,
    // even while older producers are still finishing their own .part files.
    std::string header;
    const std::string cerr = checkReaderOutput(tmpPath, header);
    if (!cerr.empty()) {
        reply(RO_UNREADABLE, cerr, r.err, prov, "", "");
        return;
    }
    ReaderKeyLock publishLock;
    std::string lockWhy;
    if (!publishLock.acquire(key, lockWhy)) {
        sendErr(lockWhy);
        return;
    }
    ec.clear();
    if (std::filesystem::exists(std::filesystem::u8path(cachePath), ec)) {
        std::string winnerHeader;
        if (checkReaderOutput(cachePath, winnerHeader).empty()) {
            reply(RO_OK, "",
                  r.err.empty() ? std::string("another session published this key first.")
                                : r.err,
                  prov, key, winnerHeader);
            return;
        }
        // An external writer, an old peer, or a crash may have left rubbish
        // while this producer ran. It is owned, rechecked and removed under the
        // same exclusion as every current producer's publish.
        std::error_code de;
        if (!std::filesystem::remove(std::filesystem::u8path(cachePath), de) || de) {
            reply(RO_UNREADABLE,
                  "the reader ran, but an unusable cache entry could not be removed: " +
                      de.message(),
                  r.err, prov, "", "");
            return;
        }
    }
    std::error_code re;
    std::filesystem::rename(std::filesystem::u8path(tmpPath),
                            std::filesystem::u8path(cachePath), re);
    if (!re) {
        temp.keep = true;                // it is the cache file now, not a temp
        reply(RO_OK, "", r.err, prov, key, header);
        return;
    }
    // This can now only be an I/O failure or a writer that ignores the lock.
    // Recheck before reporting it: if a valid canonical nevertheless appeared,
    // it answers this key and our own temp remains ours to remove.
    std::string wheader;
    const std::string werr = checkReaderOutput(cachePath, wheader);
    if (werr.empty()) {
        reply(RO_OK, "",
              r.err.empty() ? std::string("another session published this key first.")
                            : r.err,
              prov, key, wheader);
        return;
    }
    reply(RO_UNREADABLE,
          "the reader ran, but its result could not be put where this peer keeps them: " +
              re.message(),
          r.err, prov, "", "");
}

// ---------------------------------------------------------------- npz scan
//
// issue #217, docs/features/remote/remote-reader-design.md §10.2. What is in this container -
// facts only. The peer classifies nothing, builds no tree and inflates no
// pixels: it walks the zip directory, peeks each member's .npy header, and
// carries the VALUES of the small members (a scalar, a string, a short 1-D
// vector, and a viewer container's reserved members) because those are what a
// picker row and a declared tree are made of and they are kilobytes.
//
// --serve-readers has nothing to do with this. The gate (§2) is about OTHER
// PEOPLE'S CODE running here; what runs for a .npz is this binary, on a format
// it already reads. A peer with the flag closed answers SCAN.
// The ceiling on ONE NPZ_SCAN REPLY - the whole of it, all members together
// (#221 review, corrected by #180's). The per-member rule cannot bound a reply:
// nz::INLINE_MAX_BYTES lets each member through at 8 MiB and a perfectly valid
// container may hold hundreds of them, so 68 axes of 2^20 f8 elements are
// 544 MiB of a file nothing is wrong with - past the 512 MiB core/remote.cpp
// will accept, which arrives as "oversized reply from the peer": a sentence
// about the transport, for a file, that the person cannot act on.
//
// AND IT IS THE WHOLE REPLY, not the values in it. The first version of this
// budgeted `f.bytes` alone, which left the NAMES unbudgeted - a zip member name
// is a 16-bit length, so 5,000 members of 60,000 characters carry 300 MB of
// name, and a file whose values summed to 238.4 MiB passed a 256 MiB ceiling
// and put 524.5 MiB on the wire. So what is spent here is rp::
// NPZ_SCAN_REPLY_FIXED plus, per member, rp::NPZ_SCAN_FACT_FIXED + name + err +
// bytes: exactly the bytes handleNpzScan will append below, counted before one
// of them is built.
//
// 256 MiB, which is half of the client's limit. A file above it is refused
// WHOLE and by name - never trimmed to fit, because a picker missing rows is a
// listing that lied - and the refusal is an MSG_ERR for the FILE, so the person
// is told about the file rather than about the transport.
//
// VIEWER_SERVE_NPZ_SCAN_MAX may LOWER it, in bytes. That exists so the rule can
// be tested without writing a quarter-gigabyte fixture; it must not raise the
// ceiling past the transport bound the default was chosen to respect. The
// strict parse and clamp live beside the protocol constant and are read once,
// at the first scan, so the peer answers one number for its whole life.
static uint64_t npzScanInlineMax() {
    static const uint64_t n =
        rp::npzScanCeilingFor(getenv("VIEWER_SERVE_NPZ_SCAN_MAX"));
    return n;
}

static void handleNpzScan(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad NPZ_SCAN"); return; }
    if (!imagefile::isNpz(path)) {
        sendErr("this is not a .npz: " + path);
        return;
    }
    uint64_t mtime = 0, fsize = 0;
    statPeerFile(path, mtime, fsize);
    std::vector<uint8_t> zip;
    std::string err;
    if (!readWholeInto(path, zip, err)) { sendErr(err); return; }
    std::vector<nz::Entry> entries;
    if (!nz::list(zip, entries, err)) { sendErr(err); return; }
    // The budget is spent HERE - before a member is inflated, before the key is
    // published and before one byte is copied into a reply. The order is the
    // point: an aggregate ceiling enforced while filling the message would have
    // already paid for everything it then refuses.
    nz::InlineBudget budget;
    budget.max = npzScanInlineMax();
    // The message's own fields, charged before the first member is looked at,
    // and what each member costs beyond its name, its error and its bytes. Both
    // are rp::'s, next to the wire they describe, so this cannot drift from what
    // the Buf below actually appends.
    budget.used = rp::NPZ_SCAN_REPLY_FIXED;
    budget.perFact = rp::NPZ_SCAN_FACT_FIXED;
    const std::vector<nz::Fact> facts = nz::readFacts(zip, entries, &budget);
    if (!budget.over.empty()) {
        sendErr("this .npz needs more than one reply can hold: \"" +
                budget.over + "\" needs " + std::to_string(budget.need) +
                " bytes on top of " + std::to_string(budget.used) +
                " already listed, over this peer's ceiling of " +
                std::to_string(budget.max) + " bytes for one scan. Open " + path +
                " on the machine it lives on.");
        return;
    }
    if (facts.empty()) { sendErr("no arrays in npz"); return; }

    // §10.6's key. The kind word is what keeps the two families apart in one
    // cache root: a reader key and a container key over the same origin must
    // not collide, and they cannot, because "npz" is inside the hash.
    uint64_t h = readerHash("viewer-serve npz 13");
    h = readerHash(path, h);
    h = readerHash(std::to_string(mtime) + "|" + std::to_string(fsize), h);
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    const std::string key = hex;
    NpzSrc src;
    src.path = path;
    src.mtime = mtime;
    src.fsize = fsize;
    if (!npzSrcWrite(key, src)) {
        sendErr("this peer has nowhere to keep what it lists (set VIEWER_SERVE_CACHE)");
        return;
    }

    uint32_t cver = 0;
    const bool container = nz::hasContainerMark(entries);
    if (container) {
        // The version the file declares, read where the bytes are. The client
        // refuses a version it does not know with its own sentence - the check
        // stays at ONE gate (§5.2) - so this is carried, never judged here.
        //
        // Read with nz::elem, which is what the local loader reads its own
        // members with. This used to be a switch of host-order memcpys written
        // out here, and it ignored the byte order the descr states: `>i4`
        // version 1 arrived as 16777216, the client refused a "version it does
        // not know", and the same file opened locally was fine. Carrying a
        // number is only harmless if it is the number the file says (#221
        // review); bounds and byte order both belong to the decoder.
        for (const nz::Fact& f : facts) {
            if (f.name != "__viewer" || !f.whole) continue;
            nz::Head H;
            std::string e2;
            if (!nz::peekHeader(f.bytes, H, e2) || H.esize <= 0) break;
            const double v = nz::elem(f.bytes, H, 0);
            if (v > 0 && v < 1e9) cver = (uint32_t)v;
            break;
        }
    }

    Buf out;
    out.putStr(key);
    out.putU32(container ? (uint32_t)NK_CONTAINER : (uint32_t)NK_ORDINARY);
    out.putU32(cver);
    out.putU32((uint32_t)facts.size());
    for (const nz::Fact& f : facts) {
        out.putStr(f.name);
        out.putU32((uint32_t)(f.usize & 0xffffffffu));
        out.putU32((uint32_t)(f.usize >> 32));
        out.putU32((uint32_t)f.entry);
        out.putStr(f.err);
        out.putU32(f.whole ? 1u : 0u);
        out.putU32((uint32_t)f.bytes.size());
        out.putBlob(f.bytes.data(), f.bytes.size());
    }
    // THE BUDGET WAS A PREDICTION; this is the message. They must be the same
    // number, and if they are not then something appended a byte the ceiling
    // never saw - so the reply is refused rather than sent, and the peer says
    // which of the two it trusts. A reply's length is also a u32 on the wire
    // (rp::Header), and 256 MiB is far under that, but the bound is stated
    // rather than assumed because the consequence of it being wrong is a
    // session that ends instead of a file that is refused.
    if (out.b.size() != (size_t)budget.used || (uint64_t)out.b.size() > budget.max ||
        out.b.size() > 0xffffffffull) {
        sendErr("this peer built a reply of " + std::to_string(out.b.size()) +
                " bytes for " + path + " after budgeting " +
                std::to_string(budget.used) + " against a ceiling of " +
                std::to_string(budget.max) + ". It will not send it. Open the file "
                "on the machine it lives on.");
        return;
    }
    sendMsg(MSG_OK, out);
}

// VIEWER_SERVE_LAG_MS: hold a META or TILE answer back by that many
// milliseconds. A real link's latency, on demand - the peer the selftests run
// against is a pipe on the same disk, and "the UI thread blocks while a file
// is fetched" is a fact of openRemote that a zero-latency peer can never make
// visible. Only the two OPEN requests are held: LIST/SCAN keep their speed, so
// the listing a test navigates through still arrives at once.
static int serveLagMs() {
    static int ms = [] {
        const char* e = getenv("VIEWER_SERVE_LAG_MS");
        return e ? atoi(e) : 0;
    }();
    return ms;
}

// Transport-independent: give it a request, it answers through the current sink.
void handleRequest(uint32_t type, Buf& in) {
    if (serveLagMs() > 0 && (type == MSG_META || type == MSG_TILE))
        std::this_thread::sleep_for(std::chrono::milliseconds(serveLagMs()));
    switch (type) {
        case MSG_HELLO: {
            uint32_t cv = 0;
            if (in.getU32(cv) && cv) g_clientVersion = cv;   // gates the LIST shape
            Buf out;
            out.putU32(servedVersion());
            out.putStr("viewer --serve");
            sendMsg(MSG_OK, out);
            break;
        }
        case MSG_LIST: handleList(in); break;
        case MSG_GLOB: handleGlob(in); break;
        case MSG_SCAN: handleScan(in); break;
        case MSG_META: handleMeta(in); break;
        case MSG_TILE: handleTile(in); break;
        case MSG_MEASURE: handleMeasure(in); break;
        // Refused by NUMBER when this peer is pretending to be older, and with
        // the sentence a build that predates the op would give: the seam has to
        // produce the refusal it exists to test, not a newer one wearing an old
        // number (VIEWER_SERVE_PROTOCOL, above).
        case MSG_READER_RUN:
            if (servedVersion() < 12) sendErr("unknown request");
            else                      handleReaderRun(in);
            break;
        case MSG_NPZ_SCAN:
            if (servedVersion() < 13) sendErr("unknown request");
            else                      handleNpzScan(in);
            break;
        default: sendErr("unknown request"); break;
    }
}

// ---------------------------------------------------------------- stdio loop
int runServeMode() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);      // or the first 0x1A ends the session
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);   // a vanished ssh must not kill the peer mid-write
#endif
    g_sink = sendStdio;
    for (;;) {
        Header h{};
        if (!readExact(&h, sizeof h)) return 0;       // peer closed: normal exit
        if (h.magic != MAGIC) return 1;
        // No legitimate request is anywhere near this size: without the cap, a
        // 12-byte message claiming a 4 GB payload allocates 4 GB before a single
        // byte is read (reproduced by the verification agent).
        if (h.len > (64u << 20)) return 1;
        Buf in;
        in.b.resize(h.len);
        if (h.len && !readExact(in.b.data(), h.len)) return 1;
        // A malformed request may throw (length_error, bad_alloc): answer with an
        // error and keep serving - one bad file must not end the session.
        try {
            handleRequest(h.type, in);
        } catch (const std::exception& e) {
            sendErr(std::string("internal error: ") + e.what());
        } catch (...) {
            sendErr("internal error");
        }
    }
}

}  // namespace rp
