// `viewer --serve`: the process ssh starts on the machine that holds the data.
//
// No window, no GL, no listening socket - it reads requests from stdin and writes
// replies to stdout. It never sends a whole image unless a whole image is what is
// on screen: it seeks to the requested rows and decimates while reading, so the
// cost of looking at a 12 Mpx frame over a link is the cost of the pane it is
// displayed in.
#include "remote_proto.h"
#include "plugin_host.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "miniz.h"

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

namespace rp {

static const char* DT_NAMES[DT_COUNT] = { "u8", "i8", "u16", "i16", "u32", "i32", "f32", "f64" };
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

// ---------------------------------------------------------------- npy reading
// A reader, not a loader: it parses the header and then seeks. Nothing here ever
// holds a whole frame, which is the entire point of serving remotely.
struct NpyFile {
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
};

static bool parseNpyHeader(NpyFile& n, const std::string& path, std::string& err) {
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
    static const struct { const char* np; uint32_t dt; } MAP[] = {
        { "u1", DT_U8 }, { "i1", DT_I8 }, { "u2", DT_U16 }, { "i2", DT_I16 },
        { "u4", DT_U32 }, { "i4", DT_I32 }, { "f4", DT_F32 }, { "f8", DT_F64 },
        { "b1", DT_U8 },
    };
    for (const auto& m : MAP) if (t == m.np) { n.dtype = m.dt; break; }
    if (n.dtype == DT_COUNT) { err = "unsupported dtype " + descr; return false; }
    n.elemSize = dtypeSize(n.dtype);

    std::vector<long long> dims;
    size_t open = hdr.find('(', sp), close = hdr.find(')', open);
    for (size_t i = open + 1; i < close; ) {
        while (i < close && (hdr[i] == ' ' || hdr[i] == ',')) i++;
        if (i >= close) break;
        dims.push_back(atoll(hdr.c_str() + i));
        while (i < close && hdr[i] != ',') i++;
    }
    n.ndim = (int)dims.size();
    for (size_t i = 0; i < dims.size() && i < 4; i++) n.odims[i] = (uint32_t)dims[i];
    if (dims.size() == 2)      { n.h = (int)dims[0]; n.w = (int)dims[1]; n.ch = 1; }
    else if (dims.size() == 3) {
        if (dims[2] <= 4)      { n.h = (int)dims[0]; n.w = (int)dims[1]; n.ch = (int)dims[2]; }
        else                   { n.frames = (int)dims[0]; n.h = (int)dims[1]; n.w = (int)dims[2]; n.ch = 1; }
    }
    else if (dims.size() == 4) { n.frames = (int)dims[0]; n.h = (int)dims[1];
                                 n.w = (int)dims[2]; n.ch = (int)dims[3]; }
    else { err = "unsupported .npy shape"; return false; }
    // A malformed header must produce an error message, not a std::length_error
    // that terminates the peer: negative dims flow into size arithmetic as 2^64.
    if (n.w <= 0 || n.h <= 0 || n.w > (1 << 20) || n.h > (1 << 20) ||
        n.ch < 1 || n.ch > 4 || n.frames < 1 || n.frames > (1 << 20)) {
        err = "unreasonable .npy shape";
        return false;
    }
    // Per-axis element strides. C order: the last dimension is fastest.
    // Fortran order: the first. Everything downstream indexes through these, so
    // the two layouts differ in four numbers and nowhere else.
    {
        std::vector<uint64_t> d;                    // dims in declaration order
        std::vector<uint64_t*> s;                   // stride slot for each
        if (dims.size() == 2)      { d = { (uint64_t)n.h, (uint64_t)n.w };
                                     s = { &n.sY, &n.sX }; }
        else if (dims.size() == 3 && n.frames == 1) { d = { (uint64_t)n.h, (uint64_t)n.w, (uint64_t)n.ch };
                                     s = { &n.sY, &n.sX, &n.sCh }; }
        else if (dims.size() == 3) { d = { (uint64_t)n.frames, (uint64_t)n.h, (uint64_t)n.w };
                                     s = { &n.sFrame, &n.sY, &n.sX }; }
        else                       { d = { (uint64_t)n.frames, (uint64_t)n.h,
                                           (uint64_t)n.w, (uint64_t)n.ch };
                                     s = { &n.sFrame, &n.sY, &n.sX, &n.sCh }; }
        for (auto* p : s) *p = 0;
        uint64_t acc = 1;
        if (n.fortran) {
            for (size_t i = 0; i < d.size(); i++) { *s[i] = acc; acc *= d[i]; }
        } else {
            for (size_t i = d.size(); i-- > 0; ) { *s[i] = acc; acc *= d[i]; }
        }
        if (!n.sCh) n.sCh = 1;                      // 2D / (F,H,W): single channel
        if (!n.sFrame) n.sFrame = (uint64_t)n.w * n.h * n.ch;   // single-frame file
    }
    n.ok = true;
    return true;
}

// Read one decimated region. Seeks per source row and takes every `step`th
// sample, so the bytes read scale with what is asked for, not with the frame.
static bool readRegion(NpyFile& n, const TileReq& r, std::vector<uint8_t>& out,
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

static bool isNpySuffix(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext == ".npy";
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
static void putListEntryV3(Buf& out, const std::filesystem::path& full,
                           const std::string& name, bool dir, uint64_t size,
                           int64_t mtime, int& peekBudget) {
    NpyFile n;
    bool meta = false;
    if (!dir && isNpySuffix(name) && peekBudget > 0) {
        peekBudget--;
        std::string e2;
        meta = parseNpyHeader(n, full.u8string(), e2);   // reads only the header
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

struct NpyGroup {
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
static bool npySegKey(const std::string& name, std::string& key, std::string& pattern) {
    if (!isNpySuffix(name)) return false;
    std::string stem = name.substr(0, name.size() - 4);
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
    key += ".npy"; pattern += ".npy";
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
// Two stages. Stage 1 buckets by npySegKey (every digit run collapsed) - cheap
// and unchanged. Stage 2 decides, PER BUCKET, which digit run is the frame
// axis: the LAST varying run (the client's findSequenceSiblings rule). The
// pattern then keeps every other
// digit run literal - "gain10_???.npy", never "gain??_???.npy": '?' means
// "varies along the stack", and a gain digit under a '?' claims two gains got
// averaged. When a SECOND run also varies, the bucket splits by the other
// runs' values instead of growing a second '?' - a condition-mixed stack has a
// meaningless sigma_t, and the client splits those sets too. One-member
// sub-buckets fall back to singles.
static void groupNumberedNpy(const std::vector<std::pair<std::string, std::filesystem::path>>& files,
                             std::vector<NpyGroup>& groups, std::vector<size_t>& singles) {
    struct Bucket { std::string key, pattern; std::vector<size_t> m; };
    std::vector<Bucket> buckets;
    std::vector<char> used(files.size(), 0);
    for (size_t i = 0; i < files.size(); i++) {
        std::string key, pat;
        if (!npySegKey(files[i].first, key, pat)) continue;
        Bucket* b = nullptr;
        for (auto& q : buckets) if (q.key == key) { b = &q; break; }
        if (!b) { buckets.push_back({ key, pat, {} }); b = &buckets.back(); }
        b->m.push_back(i);
    }
    // emit one group from members that are already in natural order; pattern
    // from the FIRST member: '?' over the frame-axis run only. Zero-padding
    // may be uneven (frame_9 / frame_10): the first member's digit count wins,
    // display-only - members always travel by real name (putGroupEntryV3).
    auto emit = [&](const std::vector<size_t>& mem, int frameAxis,
                    const std::string& fallbackPattern) {
        NpyGroup g;
        if (frameAxis >= 0) {
            const std::string& nm = files[mem.front()].first;
            std::vector<SegRun> sr = segRuns(nm.substr(0, nm.size() - 4));
            int run = 0;
            for (const auto& r : sr) {
                if (r.digit && run++ == frameAxis) g.pattern += std::string(r.s.size(), '?');
                else g.pattern += r.s;
            }
            g.pattern += ".npy";
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
            for (const auto& r : segRuns(nm.substr(0, nm.size() - 4)))
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
                std::vector<std::string> distinct;
                for (const auto& v : vals) {
                    bool dup = false;
                    for (const auto& d : distinct) if (d == v[r]) { dup = true; break; }
                    if (!dup) distinct.push_back(v[r]);
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
        for (size_t i = 0; i < b.m.size(); i++) {
            std::string ck;
            for (size_t r = 0; r < nRuns; r++)
                if ((int)r != frameAxis) { ck += vals[i][r]; ck += '\x01'; }
            std::vector<size_t>* sv = nullptr;
            for (auto& sp : subs) if (sp.first == ck) { sv = &sp.second; break; }
            if (!sv) { subs.push_back({ ck, {} }); sv = &subs.back().second; }
            sv->push_back(b.m[i]);
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
static void putGroupEntryV3(Buf& out, const NpyGroup& g, int& peekBudget) {
    NpyFile n;
    bool meta = false;
    if (peekBudget > 0) {
        peekBudget--;
        std::string e2;
        meta = parseNpyHeader(n, g.first.u8string(), e2);
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
    std::vector<NpyGroup> groups;
    std::vector<size_t> singles;
    groupNumberedNpy(files, groups, singles);
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
    std::vector<std::pair<std::filesystem::path, uint32_t>> todo{ { rootP, 0 } };
    while (!todo.empty() && !trunc) {
        auto cur = todo.back();
        todo.pop_back();
        std::error_code dec;
        std::filesystem::directory_iterator it(cur.first, dec), end;
        if (dec) { skipped++; continue; }
        for (; it != end && !trunc; it.increment(dec)) {
            if (dec) { dec.clear(); skipped++; break; }
            std::error_code fec;
            if (it->is_symlink(fec)) continue;
            bool isDir = it->is_directory(fec);
            if (isDir && cur.second < depth) todo.push_back({ it->path(), cur.second + 1 });
            std::string rel = it->path().lexically_relative(rootP).generic_u8string();
            if (!globPatternMatches(pattern, rel)) continue;
            if (hits.size() >= cap) { trunc = true; break; }
            hits.push_back({ std::move(rel), isDir });
        }
    }
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
    struct Found { std::string rel; NpyGroup g; };
    std::vector<Found> found;
    // Manual walk: recursive_directory_iterator aborts everything on one
    // unreadable entry, and symlinks are not followed at all - a cycle must
    // cost nothing, not hang a session.
    std::vector<std::pair<std::filesystem::path, uint32_t>> todo{ { rootP, 0 } };
    while (!todo.empty() && !trunc) {
        auto cur = todo.back();
        todo.pop_back();
        std::error_code dec;
        std::filesystem::directory_iterator it(cur.first, dec), end;
        if (dec) { skipped++; continue; }
        std::vector<std::pair<std::string, std::filesystem::path>> files;
        for (; it != end; it.increment(dec)) {
            if (dec) { dec.clear(); skipped++; break; }
            std::error_code fec;
            if (it->is_symlink(fec)) continue;
            if (it->is_directory(fec)) {
                if (cur.second < depth) todo.push_back({ it->path(), cur.second + 1 });
            } else if (it->is_regular_file(fec) &&
                       isNpySuffix(it->path().filename().u8string())) {
                files.push_back({ it->path().filename().u8string(), it->path() });
            }
        }
        if (files.empty()) continue;
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<NpyGroup> gs;
        std::vector<size_t> singles;
        groupNumberedNpy(files, gs, singles);
        // Two or more leftovers fold into ONE natural-order stack ("*.npy"):
        // capture sets are not always numbered (capture_a / capture_b / ...),
        // and N single-frame stacks from one folder is never what Open Folder
        // meant. A lone file still opens as itself.
        if (singles.size() >= 2) {
            NpyGroup g;
            g.pattern = "*.npy";
            std::sort(singles.begin(), singles.end(), [&](size_t x, size_t y) {
                return rp::naturalLess(files[x].first, files[y].first);
            });
            for (size_t i : singles) {
                g.names.push_back(files[i].first);
                std::error_code e2;
                g.bytes += (uint64_t)std::filesystem::file_size(files[i].second, e2);
                g.mtime = std::max(g.mtime, unixMtime(files[i].second));
            }
            g.first = files[singles.front()].second;
            gs.push_back(std::move(g));
        } else if (singles.size() == 1) {
            size_t i = singles[0];
            NpyGroup g;
            g.pattern = files[i].first;
            g.names = { files[i].first };
            std::error_code e2;
            g.bytes = (uint64_t)std::filesystem::file_size(files[i].second, e2);
            g.mtime = unixMtime(files[i].second);
            g.first = files[i].second;
            gs.push_back(std::move(g));
        }
        std::string rel = cur.first.lexically_relative(rootP).generic_u8string();
        if (rel == ".") rel.clear();
        for (auto& g : gs) {
            if (found.size() >= cap) { trunc = true; break; }
            found.push_back({ rel, std::move(g) });
        }
    }
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

static void handleMeta(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad META"); return; }
    NpyFile n;
    std::string err;
    if (!parseNpyHeader(n, path, err)) { sendErr(err); return; }
    MetaRep m{};
    m.w = (uint32_t)n.w; m.h = (uint32_t)n.h; m.ch = (uint32_t)n.ch;
    m.dtype = n.dtype; m.frames = (uint32_t)n.frames; m.flags = 0;
    Buf out;
    out.putBlob(&m, sizeof m);
    sendMsg(MSG_OK, out);
}

static void handleTile(Buf& in) {
    std::string path;
    if (!in.getStr(path)) { sendErr("bad TILE"); return; }
    TileReq r{};
    if (in.rd + sizeof r > in.b.size()) { sendErr("bad TILE"); return; }
    memcpy(&r, in.b.data() + in.rd, sizeof r);
    NpyFile n;
    std::string err;
    if (!parseNpyHeader(n, path, err)) { sendErr(err); return; }
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
bool g_pluginsLoaded = false;
void ensurePlugins() {
    if (g_pluginsLoaded) return;
    g_pluginsLoaded = true;
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins",
                           plugin_host::exeDir() + "/../plugins" },
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

// Frames from one frame-axis file, or one file per frame; ROI rows only, so the
// disk pays for the region, not the file.
struct FrameSource {
    const MeasureReqHead* head = nullptr;
    const std::vector<std::string>* paths = nullptr;
    NpyFile n;                        // in-file mode stays open across frames
    bool perFile = false;
    uint32_t count = 0;
    std::string err;

    bool init(const MeasureReqHead& h, const std::vector<std::string>& p) {
        head = &h;
        paths = &p;
        perFile = p.size() > 1;
        if (!parseNpyHeader(n, p[0], err)) return false;
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
            NpyFile f;
            if (!parseNpyHeader(f, (*paths)[i], err)) return false;
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

bool clampRoi(const NpyFile& n, const std::vector<RoiRect>& rois, uint32_t c,
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

static void sendMeasureReply(uint32_t framesUsed,
                             const std::vector<std::vector<MItem>>& cols,
                             const std::vector<MSeries>& series) {
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
    sendMsg(MSG_OK, out);
}

// Temporal noise vs fixed pattern: per-pixel mean/var over the frame range.
// sigma_t = sqrt(mean per-pixel temporal variance), sigma_fpn = spatial sigma of
// the per-pixel temporal means - the split every sensor evaluation starts from.
static void runTemporalStats(const MeasureReqHead& head,
                             const std::vector<std::string>& paths,
                             const std::vector<RoiRect>& rois) {
    FrameSource src;
    if (!src.init(head, paths)) { sendErr(src.err); return; }
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
        double plM[4] = {}, plM2[4] = {}, plV[4] = {};
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
                }
            }
        cols[c].push_back({ 0u, "frames", (double)src.count, {} });
        if (nonFinite)
            cols[c].push_back({ 0u, "non-finite samples (excluded)", (double)nonFinite, {} });
        for (int p = 0; p < nPl; p++) {
            if (!plC[p]) continue;
            double cnt = (double)plC[p];
            double mean = plM[p] / cnt;
            double st = sqrt(plV[p] / cnt);
            double fpn = sqrt(std::max(0.0, plM2[p] / cnt - mean * mean));
            cols[c].push_back({ 0u, planeKey("mean [DN]", head.cfaType, p), mean, {} });
            cols[c].push_back({ 0u, planeKey("sigma_t [DN]", head.cfaType, p), st, {} });
            cols[c].push_back({ 0u, planeKey("sigma_fpn [DN]", head.cfaType, p), fpn, {} });
            cols[c].push_back({ 0u, planeKey("sigma_tot [DN]", head.cfaType, p),
                                sqrt(st * st + fpn * fpn), {} });
        }
        cols[c].push_back({ 1u, "method", 0.0,
            "per-pixel mean/var over " + std::to_string(src.count) +
            " frames; sigma_t = sqrt(mean unbiased temporal var), "
            "sigma_fpn = spatial sigma of temporal means; non-finite excluded" +
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
                             const std::vector<RoiRect>& rois) {
    FrameSource src;
    if (!src.init(head, paths)) { sendErr(src.err); return; }
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

static void handleMeasure(Buf& in) {
    MeasureReqHead head{};
    if (in.rd + sizeof head > in.b.size()) { sendErr("bad MEASURE"); return; }
    memcpy(&head, in.b.data() + in.rd, sizeof head);
    in.rd += sizeof head;
    if (head.nPaths == 0 || head.nPaths > 100000 || head.nRois > 4096 ||
        head.cfaType > 2 || head.cfaPattern > 3) {
        sendErr("bad MEASURE header");
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

    if (head.op == MOP_TEMPORAL_STATS)  { runTemporalStats(head, paths, rois); return; }
    if (head.op == MOP_FRAME_ROI_STATS) { runFrameRoiStats(head, paths, rois); return; }
    if (head.op != MOP_ANALYZER) { sendErr("unknown measure op"); return; }

    ensurePlugins();
    const AnalyzerPluginInfo* ana = nullptr;
    std::string have;
    for (const auto& a : plugin_host::analyzers()) {
        if (a.name == analyzer) ana = &a;
        have += (have.empty() ? "" : ", ") + a.name;
    }
    if (!ana) { sendErr("analyzer not found: " + analyzer + " (server has: " + have + ")"); return; }

    // materialize the frame as f32, exactly as the local host would
    NpyFile n;
    std::string err;
    if (!parseNpyHeader(n, paths[0], err)) { sendErr(err); return; }
    TileReq full{};
    full.frame = head.frame0;
    full.x = 0; full.y = 0; full.w = (uint32_t)n.w; full.h = (uint32_t)n.h;
    full.step = 1;
    std::vector<uint8_t> raw;
    uint32_t ow = 0, oh = 0;
    if (!readRegion(n, full, raw, ow, oh, err)) { sendErr(err); return; }
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
    std::vector<std::vector<MItem>> cols(nCols);
    std::vector<MSeries> series;
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
        if (ana->isV2) {
            psAnalyzeSink2 sink{ &ctx, mNum, mTxt, mSer, {} };
            rc = ana->v2.analyze(&fr, rp, &sink, perr, sizeof perr);
        } else {
            psAnalyzeSink sink{ &ctx, mNum, mTxt };
            rc = ana->v1.analyze(&fr, rp, &sink, perr, sizeof perr);
        }
        if (rc != 0) {
            sendErr(analyzer + ": " + (perr[0] ? perr : "analyzer failed"));
            return;
        }
    }

    sendMeasureReply(1, cols, series);
}

// Transport-independent: give it a request, it answers through the current sink.
void handleRequest(uint32_t type, Buf& in) {
    switch (type) {
        case MSG_HELLO: {
            uint32_t cv = 0;
            if (in.getU32(cv) && cv) g_clientVersion = cv;   // gates the LIST shape
            Buf out;
            out.putU32(VERSION);
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
