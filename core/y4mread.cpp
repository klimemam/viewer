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

#include <cstring>

namespace imagefile {

const char* const Y4M_LIBRARY = "y4mread 1, core/y4mread.cpp";

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
    // every refusal in docs/input-adapters.md §3.2 has. The numbers are
    // MEASURED (docs/video-support.md §1), not asserted, because "lossy is bad"
    // is an opinion and "a sigma_t of 40 DN16 comes back 0.00" is a result.
    std::string m = std::string(k->name) +
        " needs a video codec this build does not link. Decoded 8-bit video is "
        "display-referred, not DN - a known sigma_t of 40 DN16 comes back as 0.00, "
        "and noise that IS representable at 8 bits is attenuated 11% with a "
        "GOP-periodic bias (docs/video-support.md §1).";
    if (k->mayBeLossless)
        m += " This container can also hold a LOSSLESS codec (FFV1, v210), whose values "
             "would be DN - reading that still needs libavcodec, which this build weighs "
             "at 95.6 MB against a 7.6 MB viewer (docs/video-support.md §4).";
    m += " Convert the frames you want to measure and open the .y4m:\n"
         "    ffmpeg -i \"" + path + "\" -pix_fmt gray16le -strict -1 out.y4m";
    return m;
}

}  // namespace imagefile
