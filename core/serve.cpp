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
#include <cstdio>
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
    uint64_t dataOffset = 0;
    size_t elemSize = 0;
    bool ok = false;
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
    std::string hdr(hlen, '\0');
    n.f.read(&hdr[0], hlen);
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
    if (n.fortran) { err = "fortran-order .npy is not served (open it locally)"; return false; }
    if (dims.size() == 2)      { n.h = (int)dims[0]; n.w = (int)dims[1]; n.ch = 1; }
    else if (dims.size() == 3) {
        if (dims[2] <= 4)      { n.h = (int)dims[0]; n.w = (int)dims[1]; n.ch = (int)dims[2]; }
        else                   { n.frames = (int)dims[0]; n.h = (int)dims[1]; n.w = (int)dims[2]; n.ch = 1; }
    }
    else if (dims.size() == 4) { n.frames = (int)dims[0]; n.h = (int)dims[1];
                                 n.w = (int)dims[2]; n.ch = (int)dims[3]; }
    else { err = "unsupported .npy shape"; return false; }
    if (n.w <= 0 || n.h <= 0) { err = "empty .npy"; return false; }
    n.ok = true;
    return true;
}

// Read one decimated region. Seeks per source row and takes every `step`th
// sample, so the bytes read scale with what is asked for, not with the frame.
static bool readRegion(NpyFile& n, const TileReq& r, std::vector<uint8_t>& out,
                       uint32_t& outW, uint32_t& outH, std::string& err) {
    uint32_t step = std::max(1u, r.step);
    uint32_t x0 = std::min(r.x, (uint32_t)n.w), y0 = std::min(r.y, (uint32_t)n.h);
    uint32_t x1 = std::min(r.x + r.w, (uint32_t)n.w), y1 = std::min(r.y + r.h, (uint32_t)n.h);
    if (x1 <= x0 || y1 <= y0) { err = "empty region"; return false; }
    outW = (x1 - x0 + step - 1) / step;
    outH = (y1 - y0 + step - 1) / step;
    size_t px = n.elemSize * n.ch;
    out.resize((size_t)outW * outH * px);

    uint64_t frameBytes = (uint64_t)n.w * n.h * px;
    uint64_t base = n.dataOffset + (uint64_t)std::min<uint32_t>(r.frame, n.frames - 1) * frameBytes;
    std::vector<uint8_t> row((size_t)(x1 - x0) * px);
    uint8_t* dst = out.data();
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

static void handleMeasure(Buf& in) {
    MeasureReqHead head{};
    if (in.rd + sizeof head > in.b.size()) { sendErr("bad MEASURE"); return; }
    memcpy(&head, in.b.data() + in.rd, sizeof head);
    in.rd += sizeof head;
    if (head.nPaths == 0 || head.nPaths > 100000 || head.nRois > 4096) {
        sendErr("bad MEASURE header");
        return;
    }
    std::vector<std::string> paths(head.nPaths);
    for (auto& p : paths) if (!in.getStr(p)) { sendErr("bad MEASURE paths"); return; }
    std::string analyzer, params;
    if (!in.getStr(analyzer) || !in.getStr(params)) { sendErr("bad MEASURE"); return; }
    struct RoiRect { uint32_t x, y, w, h; };
    std::vector<RoiRect> rois(head.nRois);
    for (auto& r : rois) {
        uint32_t v[4];
        for (uint32_t& x : v) if (!in.getU32(x)) { sendErr("bad MEASURE rois"); return; }
        r.x = v[0]; r.y = v[1]; r.w = v[2]; r.h = v[3];
    }

    if (head.op != MOP_ANALYZER) { sendErr("measure op not implemented yet"); return; }

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

    Buf out;
    out.putU32(0);                              // serverLoc: CPU (CUDA slots in here)
    out.putU32(1);                              // framesUsed
    out.putU32(nCols);
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

// Transport-independent: give it a request, it answers through the current sink.
void handleRequest(uint32_t type, Buf& in) {
    switch (type) {
        case MSG_HELLO: {
            Buf out;
            out.putU32(VERSION);
            out.putStr("viewer --serve");
            sendMsg(MSG_OK, out);
            break;
        }
        case MSG_LIST: handleList(in); break;
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
    g_sink = sendStdio;
    for (;;) {
        Header h{};
        if (!readExact(&h, sizeof h)) return 0;       // peer closed: normal exit
        if (h.magic != MAGIC) return 1;
        Buf in;
        in.b.resize(h.len);
        if (h.len && !readExact(in.b.data(), h.len)) return 1;
        handleRequest(h.type, in);
    }
}

}  // namespace rp
