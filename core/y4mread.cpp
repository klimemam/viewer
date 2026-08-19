// y4m (YUV4MPEG2), for core/imagefile.h. See y4mread.h for why a video
// container is in the picture-format table and why it is the only one.
//
// THE SHAPE OF THIS FILE IS "READ, OR SAY WHAT STOPPED YOU", the same as
// core/tiffread.cpp. A y4m header is untrusted text that decides an allocation
// and a stride, so every field is either established from the file or refused
// with a sentence naming the tag and its value. There is no third option where
// it proceeds on an assumption, because every assumption available here - the
// bit depth, the chroma layout, whether a frame is one instant - changes the
// NUMBERS while leaving a picture that looks entirely plausible.
//
// What is read: the luma plane of a progressive y4m, 8 to 16 bits, Cmono /
// Cmono<N> / C444 / C422 / C420 / C411 with the jpeg/mpeg2/paldv siting
// variants and the pNN depth suffix, with or without an alpha plane.
#include "y4mread.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace imagefile {

const char* const Y4M_LIBRARY = "y4mread 1, core/y4mread.cpp";

namespace {

// A header line longer than this is not a y4m header; the format's own line is
// a few dozen bytes. Bounded because the search for the newline runs over
// whatever bytes arrived.
const size_t MAX_HEADER = 1024;
// ...and the pixels behind it. The same ceiling core/imagefile.h applies, tested
// on the HEADER before anything is sized from it: W and H here are a claim made
// by the file, and a claim is not a reason to allocate.
const int64_t MAX_FRAMES = 100000;

struct Info {
    int w = 0, h = 0, bits = 8, bps = 1;
    size_t lumaBytes = 0, frameBytes = 0, headerLen = 0;
    char ilace = '?';
    bool hasChroma = false;
    std::string cspace = "420", range, hdr;
};

// Plane sizes from the C tag. Returns "" on success, else a reason that NAMES
// what it did not know - "unsupported" tells the operator nothing.
std::string geometry(Info& I) {
    const std::string s = I.cspace;
    int hs = 0, vs = 0;                       // 0 = no chroma planes at all
    bool alpha = false;
    if (s.compare(0, 4, "mono") == 0) {
        if (s.size() > 4) I.bits = atoi(s.c_str() + 4);
    } else if (s.size() >= 3 && isdigit((unsigned char)s[0])) {
        const std::string t = s.substr(0, 3), rest = s.substr(3);
        if      (t == "444") { hs = 1; vs = 1; }
        else if (t == "422") { hs = 2; vs = 1; }
        else if (t == "420") { hs = 2; vs = 2; }
        else if (t == "411") { hs = 4; vs = 1; }
        else return "colour space C" + s + " is not one this reader knows";
        // jpeg / paldv / mpeg2 differ only in chroma SITING - luma is
        // unaffected, so they are read rather than refused. The bit-depth
        // suffix is pNN, so the DIGIT test below is load-bearing: "C420paldv"
        // also begins with p, and reading its depth as atoi("aldv") = 0 would
        // refuse a perfectly ordinary 8-bit PAL-DV file as "bit depth 0".
        if (rest == "alpha") alpha = true;
        else if (rest.size() > 1 && rest[0] == 'p' && isdigit((unsigned char)rest[1]))
            I.bits = atoi(rest.c_str() + 1);
    } else {
        return "colour space C" + s + " is not one this reader knows";
    }
    if (I.bits < 8 || I.bits > 16)
        return "a luma bit depth of " + std::to_string(I.bits) + " is not one this reader knows";
    I.bps = I.bits > 8 ? 2 : 1;
    I.hasChroma = hs != 0;
    I.lumaBytes = (size_t)I.w * (size_t)I.h * (size_t)I.bps;
    size_t rest = 0;
    if (hs) rest = 2 * (size_t)((I.w + hs - 1) / hs) * (size_t)((I.h + vs - 1) / vs) * (size_t)I.bps;
    if (alpha) rest += I.lumaBytes;
    I.frameBytes = I.lumaBytes + rest;
    return {};
}

}  // namespace

bool y4mDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err) {
    const size_t look = n < MAX_HEADER ? n : MAX_HEADER;
    const void* nlp = memchr(p, '\n', look);
    if (!nlp) { err = "there is no header line in the first 1024 bytes"; return false; }
    const size_t nl = (size_t)((const uint8_t*)nlp - p);

    Info I;
    I.hdr.assign((const char*)p, nl);
    I.headerLen = nl + 1;
    if (I.hdr.compare(0, 9, "YUV4MPEG2") != 0) {
        err = "the YUV4MPEG2 signature is missing";
        return false;
    }
    {
        std::istringstream ss(I.hdr);
        std::string tok;
        ss >> tok;                                   // YUV4MPEG2
        while (ss >> tok) {
            if (tok.size() < 2) continue;
            const std::string v = tok.substr(1);
            switch (tok[0]) {
                case 'W': I.w = atoi(v.c_str()); break;
                case 'H': I.h = atoi(v.c_str()); break;
                case 'I': I.ilace = v[0]; break;
                case 'C': I.cspace = v; break;
                case 'X': if (v.compare(0, 11, "COLORRANGE=") == 0) I.range = v.substr(11); break;
                default: break;                      // F (rate), A (aspect): not pixels
            }
        }
    }
    if (I.w <= 0 || I.h <= 0) { err = "the header does not give a usable W and H"; return false; }
    // The header is untrusted input and W/H feed an allocation below, so they
    // are bounded BEFORE anything is sized from them - the same discipline
    // core/imagefile.h's MAX_DIM exists for.
    if (I.w > MAX_DIM || I.h > MAX_DIM) {
        err = "the header claims " + std::to_string(I.w) + "x" + std::to_string(I.h) +
              ", which is past this viewer's " + std::to_string((int)MAX_DIM) + " limit";
        return false;
    }
    // A frame that is two fields is two INSTANTS, and a stack's sigma_t assumes
    // one instant per frame (docs/terminology.md). Averaging them would produce
    // a temporal noise that is really a motion artefact, silently.
    if (I.ilace != 'p' && I.ilace != '?') {
        err = std::string("interlaced (I") + I.ilace + "): one frame is two fields, so it is "
              "two instants - a sigma_t over such a stack is not a temporal noise. "
              "Deinterlace to progressive frames first";
        return false;
    }
    std::string ge = geometry(I);
    if (!ge.empty()) { err = ge; return false; }

    // Frames are FIXED SIZE, so N is arithmetic from the byte count. This is the
    // one format here where the tool knows exactly what it is missing, and
    // docs/terminology.md forbids not saying so - hence "n of N" below rather
    // than a stack that is quietly short.
    const size_t stride = 6 + I.frameBytes;          // "FRAME\n" + planes
    const uint64_t body = n > I.headerLen ? (uint64_t)(n - I.headerLen) : 0;
    if (I.frameBytes == 0 || (uint64_t)I.frameBytes > body) {
        err = "the header claims " + std::to_string(I.w) + "x" + std::to_string(I.h) + " " +
              std::to_string(I.bits) + "-bit C" + I.cspace + " (" +
              std::to_string(I.frameBytes) + " bytes a frame) but only " +
              std::to_string(body) + " bytes follow it";
        return false;
    }
    int64_t declared = (int64_t)(body / stride);
    if (body % stride) declared++;                   // a partial frame was intended
    if (declared <= 0) { err = "the header is there but no complete frame follows it"; return false; }
    if (declared > MAX_FRAMES) {
        err = "the bytes imply " + std::to_string(declared) + " frames, past this reader's " +
              std::to_string((long long)MAX_FRAMES) + " limit";
        return false;
    }

    // The note carries the two claims APART: bit-exactness is what earns the
    // [DN] label, and the transfer characteristic is a separate claim that this
    // container almost never makes. Saying "not assumed linear" out loud is what
    // keeps a linearity fit over these frames honest.
    const std::string baseNote =
        "y4m C" + I.cspace + " " + std::to_string(I.bits) + "-bit luma" +
        (I.hasChroma ? " (chroma subsampled: present, NOT read - an interpolated colour is "
                       "not a measurement)" : "") +
        "; uncompressed, bit-exact - values are DN as stored. Range: " +
        (I.range.empty() ? "not declared" : I.range) +
        ". Transfer/primaries: not declared by the file, not assumed linear";

    size_t off = I.headerLen;
    for (int64_t fr = 0; fr < declared; fr++) {
        // "FRAME" + optional per-frame parameters + '\n'
        if (off + 6 > n || memcmp(p + off, "FRAME", 5) != 0) break;
        size_t q = off + 5;
        while (q < n && p[q] != '\n') q++;
        if (q >= n) break;
        q++;
        if (q + I.frameBytes > n) break;             // ends inside this frame
        const uint8_t* luma = p + q;                 // the luma plane leads the frame
        Image img;
        img.w = I.w; img.h = I.h; img.ch = 1;
        img.dtype = I.bps == 2 ? "u16" : "u8";       // how it is STORED
        img.data.resize((size_t)I.w * (size_t)I.h);
        if (I.bps == 1)
            for (size_t i = 0; i < img.data.size(); i++) img.data[i] = (float)luma[i];
        else
            for (size_t i = 0; i < img.data.size(); i++)   // y4m is little-endian
                img.data[i] = (float)((uint32_t)luma[2 * i] | ((uint32_t)luma[2 * i + 1] << 8));
        img.note = baseNote;
        out.push_back(std::move(img));
        off = q + I.frameBytes;
    }
    if (out.empty()) { err = "the file ends inside its first frame: no complete frame to open"; return false; }
    // n of N, on every frame, because a partial stack that does not say so is
    // the failure docs/terminology.md names. The count is trustworthy here in a
    // way it is in no other video format: it is a division, not a claim.
    if ((int64_t)out.size() < declared) {
        const std::string tail = "; the file ends inside frame " + std::to_string(out.size() + 1) +
                                 " - " + std::to_string(out.size()) + " of " +
                                 std::to_string(declared) + " frames were read";
        for (Image& img : out) img.note += tail;
    }
    return true;
}

// ---------------------------------------------------------- the refusals ----
std::string videoRefusal(const std::string& path) {
    struct Known { const char* ext; const char* name; bool mayBeLossless; };
    static const Known K[] = {
        { ".mp4",  "MP4 (H.264/HEVC)",                  false },
        { ".m4v",  "M4V (H.264)",                       false },
        { ".mov",  "QuickTime MOV (H.264/HEVC/ProRes)",  true  },
        { ".mkv",  "Matroska MKV",                       true  },
        { ".webm", "WebM (VP8/VP9)",                    false },
        { ".avi",  "AVI",                                true  },
        { ".wmv",  "WMV (VC-1)",                        false },
        { ".flv",  "FLV",                               false },
        { ".mpg",  "MPEG program stream",               false },
        { ".mpeg", "MPEG program stream",               false },
        { ".m2v",  "MPEG-2 elementary stream",          false },
        { ".ts",   "MPEG transport stream",             false },
        { ".m2ts", "MPEG transport stream",             false },
        { ".mts",  "MPEG transport stream",             false },
        { ".3gp",  "3GP (H.263/H.264)",                 false },
        { ".ogv",  "Ogg Theora",                        false },
        { ".264",  "raw H.264 elementary stream",       false },
        { ".h264", "raw H.264 elementary stream",       false },
        { ".avc",  "raw H.264 elementary stream",       false },
        { ".265",  "raw HEVC elementary stream",        false },
        { ".hevc", "raw HEVC elementary stream",        false },
    };
    std::string low = path;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    const Known* k = nullptr;
    for (const auto& e : K) {
        size_t len = strlen(e.ext);
        if (low.size() >= len && low.compare(low.size() - len, len, e.ext) == 0) { k = &e; break; }
    }
    if (!k) return {};
    // Named, reasoned, and with the way out attached - the same three parts
    // every refusal in docs/features/adapters/input-adapters.md §3.2 has. The numbers are
    // MEASURED (docs/features/media/video-support.md §1), not asserted, because "lossy is bad"
    // is an opinion and "a sigma_t of 40 DN16 comes back 0.00" is a result.
    std::string m = std::string(k->name) +
        " needs a video codec this build does not link. Decoded 8-bit video is "
        "display-referred, not DN - a known sigma_t of 40 DN16 comes back as 0.00, "
        "and noise that IS representable at 8 bits is attenuated 11% with a "
        "GOP-periodic bias (docs/features/media/video-support.md §1).";
    if (k->mayBeLossless)
        m += " This container can also hold a LOSSLESS codec (FFV1, v210), whose values "
             "would be DN - reading that still needs libavcodec, which this build weighs "
             "at 95.6 MB against a 7.6 MB viewer (docs/features/media/video-support.md §4).";
    m += " Convert the frames you want to measure and open the .y4m:\n"
         "    ffmpeg -i \"" + path + "\" -pix_fmt gray16le -strict -1 out.y4m";
    return m;
}

}  // namespace imagefile
