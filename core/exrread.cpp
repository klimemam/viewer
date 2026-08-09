// OpenEXR, for core/imagefile.h. See exrread.h for why this one is a library
// and TIFF is not.
//
// WHAT THIS FILE IS FOR, in one line: the numbers in the file arrive in the
// document unchanged, and everything this build will not read is refused BY
// NAME with a reason (docs/input-adapters.md §3.2).
//
// The first half of that is the whole reason .exr is worth having. It is the
// only format behind this seam whose samples are floating point and scene
// linear: 12.5 means 12.5, negatives are real, and there is no full scale. So
// the load path does nothing to them - no tone map, no gamma, no clamp, no RGBA
// repacking - and half widens to float losslessly, because every half IS a
// float exactly. A viewer that display-encoded these on the way in would still
// draw a plausible picture and would have destroyed the measurement.
//
// The second half is where the code actually is. EXR is a container: multi-part,
// deep, chroma-subsampled, mip-mapped, UINT id channels. Each of those is a file
// this reader could open into something plausible and wrong, so each is refused
// by the name the format itself uses for it.
#include "exrread.h"

#ifdef VIEWER_WITH_EXR

#include <cstring>

#include <IexBaseExc.h>
#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfMultiPartInputFile.h>
#include <ImfPartType.h>
#include <ImfTileDescription.h>
#include <ImfThreading.h>
#include <OpenEXRConfig.h>
#include <half.h>

#endif  // VIEWER_WITH_EXR

namespace imagefile {

#ifndef VIEWER_WITH_EXR

const char* const EXR_LIBRARY = "";
const char* const EXR_ABSENT =
    "this build was configured with -DVIEWER_WITH_EXR=OFF, so no OpenEXR is linked";
bool (*const EXR_DECODE)(const uint8_t*, size_t, std::vector<Image>&, std::string&) = nullptr;

#else

const char* const EXR_LIBRARY = "OpenEXR " OPENEXR_VERSION_STRING;
const char* const EXR_ABSENT = nullptr;

namespace {

// What OpenEXR calls this stream in its own exception messages, which are
// passed straight through to the user. It must not look like a path: the caller
// has already prefixed the real file name, and a second, fake one next to it
// ("(memory)") reads as a bug rather than as a truncated file.
const char* const STREAM_NAME = "these bytes";

// The .exr is read into memory first and handed to OpenEXR as a stream, so the
// library never touches the filesystem: its default stream opens a narrow-char
// path, which on Windows cannot express the Japanese paths this tool is used
// with. Every other loader here reads bytes first (readFileBytes) for the same
// reason.
class MemStream : public Imf::IStream {
public:
    MemStream(const char* name, const uint8_t* d, size_t n)
        : Imf::IStream(name), d_(d), n_(n), p_(0) {}
    bool isMemoryMapped() const override { return true; }
    char* readMemoryMapped(int n) override {
        if (n < 0 || p_ + (size_t)n > n_)
            throw IEX_NAMESPACE::InputExc("unexpected end of EXR data");
        char* r = (char*)d_ + p_;
        p_ += (size_t)n;
        return r;
    }
    bool read(char c[], int n) override {
        if (n < 0 || p_ + (size_t)n > n_)
            throw IEX_NAMESPACE::InputExc("unexpected end of EXR data");
        memcpy(c, d_ + p_, (size_t)n);
        p_ += (size_t)n;
        return p_ < n_;
    }
    uint64_t tellg() override { return (uint64_t)p_; }
    void seekg(uint64_t p) override { p_ = (size_t)p; }
    void clear() override {}
private:
    const uint8_t* d_;
    size_t n_, p_;
};

// OpenEXR would otherwise start its own thread pool. The sequence loader
// already runs decodes on ITS thread against a memory budget, so a second,
// invisible pool underneath it is not what anyone asked for.
void initThreads() {
    static const bool once = [] { Imf::setGlobalThreadCount(0); return true; }();
    (void)once;
}

// EXR names channels "layer.LEAF": the layer is everything before the LAST dot,
// so "diffuse.R" is leaf R of layer "diffuse" and a bare "R" is leaf R of the
// base (unnamed) layer.
void splitChannel(const std::string& full, std::string& layer, std::string& leaf) {
    size_t dot = full.rfind('.');
    if (dot == std::string::npos) { layer.clear(); leaf = full; }
    else { layer = full.substr(0, dot); leaf = full.substr(dot + 1); }
}

const char* compressionName(Imf::Compression c) {
    switch (c) {
        case Imf::NO_COMPRESSION:   return "uncompressed";
        case Imf::RLE_COMPRESSION:  return "RLE";
        case Imf::ZIPS_COMPRESSION: return "ZIPS (one scanline per block)";
        case Imf::ZIP_COMPRESSION:  return "ZIP";
        case Imf::PIZ_COMPRESSION:  return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24 (lossy for float)";
        case Imf::B44_COMPRESSION:  return "B44 (lossy)";
        case Imf::B44A_COMPRESSION: return "B44A (lossy)";
        case Imf::DWAA_COMPRESSION: return "DWAA (lossy)";
        case Imf::DWAB_COMPRESSION: return "DWAB (lossy)";
        default: return "an unnamed compression";
    }
}

// One group of channels that becomes one Image.
struct Layer {
    std::string name;                  // "" = the base layer, unnamed in the file
    std::vector<std::string> chans;    // full channel names, in document order
    std::string dtype;                 // "f16" / "f32"
};

// Decide which layers this file offers, or say why it offers none. Returns ""
// and fills `out` on success; otherwise returns the refusal, which is the
// reason ALONE - core/imagefile.cpp prefixes the format and the library and the
// caller prefixes the file name.
std::string scanLayers(const Imf::Header& hdr, int parts, std::vector<Layer>& out) {
    // ---- what is not one flat picture ------------------------------------
    if (parts > 1)
        return "multi-part EXR (" + std::to_string(parts) +
               " parts): each part is a separate image with its own header, and "
               "which one was meant is not something a loader may decide";
    if (hdr.hasType()) {
        const std::string& t = hdr.type();
        if (t == Imf::DEEPSCANLINE || t == Imf::DEEPTILE)
            return "deep EXR: a deep pixel is a LIST of samples at different "
                   "depths, not a value, and there is no flattening of it that "
                   "would still be the measurement";
    }
    // Tiling is a STORAGE layout and the linked library undoes it losslessly,
    // so a one-level tiled file is read (see exrDecode). A mip or rip pyramid
    // is a different thing: it holds the same picture at several resolutions,
    // and picking one silently would answer a question the file asked.
    if (hdr.hasTileDescription()) {
        const Imf::TileDescription& td = hdr.tileDescription();
        if (td.mode != Imf::ONE_LEVEL)
            return std::string("multi-resolution EXR (") +
                   (td.mode == Imf::MIPMAP_LEVELS ? "mipmap" : "ripmap") +
                   " levels): it holds one picture at several resolutions, and "
                   "choosing a level is the user's decision, not a loader's";
    }

    // ---- group the channels by layer -------------------------------------
    struct Group { std::string layer; std::vector<std::pair<std::string, std::string>> leaves; };
    std::vector<Group> groups;
    const Imf::ChannelList& cl = hdr.channels();
    for (auto it = cl.begin(); it != cl.end(); ++it) {
        const std::string full = it.name();
        const Imf::Channel& c = it.channel();
        if (c.xSampling != 1 || c.ySampling != 1)
            return "chroma-subsampled channel '" + full + "' (sampling " +
                   std::to_string(c.xSampling) + "x" + std::to_string(c.ySampling) +
                   "): only full-resolution channels are read, there is no chroma "
                   "reconstruction here";
        if (c.type == Imf::UINT)
            return "channel '" + full +
                   "' is UINT: it is an id or an index, not a measurement, and "
                   "widening it into float would lose values above 2^24 silently - "
                   "only half and float channels are read";
        std::string layer, leaf;
        splitChannel(full, layer, leaf);
        // Y/RY/BY is the luminance-chroma layout. Even at 1x1 sampling we do not
        // reconstruct RGB from it, and handing back three planes named Y, RY and
        // BY as if they were pictures would be three lies.
        if (leaf == "RY" || leaf == "BY")
            return "luminance-chroma channel '" + full +
                   "': RGB is not reconstructed from Y/RY/BY here, and the planes "
                   "themselves are not pictures";
        Group* g = nullptr;
        for (auto& q : groups) if (q.layer == layer) { g = &q; break; }
        if (!g) { groups.push_back({ layer, {} }); g = &groups.back(); }
        g->leaves.push_back({ leaf, full });
    }
    if (groups.empty()) return "there are no channels in this file";

    auto findLeaf = [](const Group& g, const char* want, std::string& full) {
        for (const auto& p : g.leaves) if (p.first == want) { full = p.second; return true; }
        return false;
    };
    auto dtypeOf = [&](const std::vector<std::string>& chans) {
        for (const auto& c : chans) {
            const Imf::Channel* ch = cl.findChannel(c.c_str());
            if (ch && ch->type == Imf::FLOAT) return std::string("f32");
        }
        return std::string("f16");
    };

    for (const auto& g : groups) {
        std::string r, gg, b, a;
        std::vector<std::string> taken;
        if (findLeaf(g, "R", r) && findLeaf(g, "G", gg) && findLeaf(g, "B", b)) {
            Layer L;
            L.name = g.layer;
            L.chans = { r, gg, b };
            if (findLeaf(g, "A", a)) L.chans.push_back(a);
            L.dtype = dtypeOf(L.chans);
            taken = L.chans;
            out.push_back(std::move(L));
        }
        // Anything in this layer the RGB(A) image did not take becomes its own
        // 1-channel image. Orders other than R,G,B,A have no meaning defined by
        // the format, so packing e.g. X,Y,Z as if it were colour would be a
        // guess; one plane per channel states exactly what is in the file.
        for (const auto& p : g.leaves) {
            bool used = false;
            for (const auto& t : taken) if (t == p.second) used = true;
            if (used) continue;
            Layer L;
            L.name = p.second;             // the channel's own full name
            L.chans = { p.second };
            L.dtype = dtypeOf(L.chans);
            out.push_back(std::move(L));
        }
    }
    if (out.empty()) return "there are no readable channels in this file";
    return {};
}

// Read ONE layer. half widens to float without loss (every half is exactly
// representable as a float) and float is copied; the slice type is FLOAT
// because that is what the IMAGE holds, and InputFile does the widening.
bool readLayer(Imf::InputFile& in, const Layer& L, const Imath::Box2i& dw,
               int layersInFile, Image& out, std::string& err) {
    const int64_t w = (int64_t)dw.max.x - (int64_t)dw.min.x + 1;
    const int64_t h = (int64_t)dw.max.y - (int64_t)dw.min.y + 1;
    if (w < 1 || h < 1) {
        err = "the data window is empty (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        return false;
    }
    if (w > MAX_DIM || h > MAX_DIM) {
        err = "the data window is " + std::to_string(w) + "x" + std::to_string(h) +
              ", past this viewer's " + std::to_string((int)MAX_DIM) + " limit";
        return false;
    }
    const int ch = (int)L.chans.size();
    out.w = (int)w; out.h = (int)h; out.ch = ch;
    out.dtype = L.dtype;
    out.member = L.name;
    out.data.assign((size_t)w * (size_t)h * (size_t)ch, 0.0f);

    Imf::FrameBuffer fb;
    const size_t xs = sizeof(float) * (size_t)ch;
    const size_t ys = sizeof(float) * (size_t)ch * (size_t)w;
    for (int c = 0; c < ch; c++) {
        // OpenEXR addresses pixels in IMAGE coordinates, so a slice base is
        // where pixel (0,0) WOULD sit. For a data window that does not start at
        // the origin that address is outside the buffer, which is the idiom the
        // format's own headers document - and getting it wrong shifts every
        // pixel while still producing a picture, which is why there is a
        // fixture whose window starts at (5,3).
        char* base = (char*)(out.data.data() + c)
                   - ((size_t)dw.min.x * xs + (size_t)dw.min.y * ys);
        fb.insert(L.chans[c].c_str(), Imf::Slice(Imf::FLOAT, base, xs, ys, 1, 1, 0.0));
    }
    in.setFrameBuffer(fb);
    in.readPixels(dw.min.y, dw.max.y);

    // Rule 2: what was done is said. None of this is guessable from the pixels,
    // and the layer/channel names in particular are the only thing that
    // distinguishes "this is the beauty pass" from "this is a depth buffer".
    const Imf::Header& hdr = in.header();
    std::string note = "OpenEXR, ";
    note += L.dtype == "f32" ? "32-bit float" : "16-bit float (half), widened to float without loss";
    note += ", ";
    note += hdr.hasTileDescription() ? "tiled" : "scanline";
    note += ", ";
    note += compressionName(hdr.compression());
    note += "; channel";
    if (ch != 1) note += "s";
    note += " ";
    for (int c = 0; c < ch; c++) { if (c) note += ", "; note += L.chans[c]; }
    if (layersInFile > 1)
        note += "; layer " + (L.name.empty() ? std::string("(base)") : L.name) + " of " +
                std::to_string(layersInFile);
    const Imath::Box2i& disp = hdr.displayWindow();
    if (dw != disp)
        note += "; the data window is " + std::to_string(w) + "x" + std::to_string(h) +
                " at (" + std::to_string(dw.min.x) + "," + std::to_string(dw.min.y) +
                "), inside a display window of " +
                std::to_string(disp.max.x - disp.min.x + 1) + "x" +
                std::to_string(disp.max.y - disp.min.y + 1) +
                " - these pixels are the DATA window, not the frame around it";
    out.note = note;
    return true;
}

// (not named `decode`: imagefile::decode is the seam's own entry point, and an
// overload of it one namespace down is a trap for the next reader)
bool exrDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err) {
    initThreads();
    std::vector<Layer> layers;
    try {
        // MultiPartInputFile is how the part COUNT is asked for; a single-part
        // file is then re-opened as an InputFile, which is the one class that
        // reads both scanline and one-level tiled files without the caller
        // knowing which it got.
        {
            MemStream st(STREAM_NAME, p, n);
            Imf::MultiPartInputFile mp(st);
            std::string r = scanLayers(mp.header(0), mp.parts(), layers);
            if (!r.empty()) { err = r; return false; }
        }
        MemStream st(STREAM_NAME, p, n);
        Imf::InputFile in(st);
        const Imath::Box2i dw = in.header().dataWindow();
        for (const Layer& L : layers) {
            Image img;
            if (!readLayer(in, L, dw, (int)layers.size(), img, err)) return false;
            out.push_back(std::move(img));
        }
    } catch (const std::exception& e) {
        // Everything OpenEXR reports, it reports by throwing. Its messages name
        // the file it was handed - "(memory)" here - and the fault it found,
        // which is exactly the reason this layer of the sentence is for.
        err = e.what();
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

bool (*const EXR_DECODE)(const uint8_t*, size_t, std::vector<Image>&, std::string&) = &exrDecode;

#endif  // VIEWER_WITH_EXR

}  // namespace imagefile
