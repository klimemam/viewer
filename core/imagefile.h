#pragma once
// ---------------------------------------------------------------------------
// The picture formats that are not .npy: PNG, JPEG, TIFF.
//
// THE SEAM IS THE DELIVERABLE. Which library decodes a PNG is a build-time
// detail that this repository has already changed its mind about once (the
// board carried three candidates: stb_image, the three official libraries, and
// OpenImageIO), and it will change its mind again the day a 16-bit TIFF has to
// be read exactly. So the rest of the program is not allowed to know: nothing
// outside core/imagefile.cpp includes a decoder's header, and swapping one out
// is editing ONE row of the table below plus the file that row points at.
//
// What comes back out is what `.npy` already produces - w/h/ch, a dtype name, a
// float32 buffer and a note - so no panel, no analyzer and no exporter learns a
// second vocabulary for "an image the viewer opened".
//
//   loadNpy(path)       -> decodeNpyBuffer -> ImageDoc          (core/main.cpp)
//   loadImageFile(path) -> imagefile::decode -> Image -> ImageDoc
//
// Three rules this interface exists to enforce:
//
//  1. VALUES ARE AS STORED. An 8-bit PNG arrives as 0..255 and a 16-bit PNG as
//     0..65535 - never divided by anything. docs/input-adapters.md §8-9 fixed
//     the pixel unit at [DN] and §4.8 says an integer rides into f32 with its
//     value intact; a decoder that normalised to 0..1 would make the same file
//     measure differently depending on which library was linked, which is the
//     one thing a seam like this must never allow.
//
//  2. WHAT WAS DONE IS SAID. Every decode fills `note`, and the Inspector
//     prints it. "8-bit" is a fact about the file; "no transfer curve was
//     applied" is a fact about what we did NOT do, and both belong on screen -
//     a PNG does not declare what its numbers mean, so the viewer must at least
//     declare what it did with them.
//
//  3. CFA IS READ OR ABSENT, NEVER GUESSED. `cfa` may only be set by a decoder
//     that actually READ a pattern out of the file. Assuming RGGB because a
//     file has one channel produces the wrong colour and wrong per-plane
//     statistics, silently, which is the failure mode this tool exists to
//     avoid. A format whose pattern cannot be read reliably is refused, not
//     guessed at.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace imagefile {

// One decoded picture: exactly the fields a FrameSource needs, and no others.
// There is deliberately no frame axis here - PNG and JPEG hold one picture, and
// a multi-page TIFF is a stack, which is the caller's word (App::SeqInfo) and
// not a decoder's. When a backend that can do that arrives, it returns several
// of these and the caller builds the stack the same way loadNpyBuffer does.
struct Image {
    int w = 0, h = 0, ch = 1;      // ch: 1 grey, 2 grey+alpha, 3 RGB, 4 RGBA
    std::string dtype;             // "u8" / "u16": what the FILE stored, not what we hold
    std::vector<float> data;       // w*h*ch, interleaved, VALUES AS STORED (rule 1)
    std::string note;              // what the decoder did, for the Inspector (rule 2)
    int cfa = 0;                   // 0 none, 1 Bayer, 2 Quad Bayer - only if READ (rule 3)
    int cfaPattern = 0;            // index into CFA_PATTERNS, meaningless when cfa == 0
};

// A format, and whatever reads it in THIS build.
//
// `decode == nullptr` is a first-class state, not a hole: the format is known,
// it is listed, it is dispatched to, and it refuses with `absent` - a sentence
// naming what is missing and why. That is how TIFF ships today. The alternative
// (leave TIFF out of the table) turns a .tif into "unknown file, try the raw
// dialog", which tells the user nothing about the actual situation.
struct Backend {
    const char* format;            // "PNG"
    const char* exts;              // ".png" - space separated, lower case, dot included
    const char* library;           // what is linked for it, version included; "" when absent
    bool (*sniff)(const uint8_t* p, size_t n);                 // magic bytes, never the name
    bool (*decode)(const uint8_t* p, size_t n, Image& out, std::string& err);
    const char* absent;            // why there is no decoder; nullptr when there is one
};

// The table, in dispatch order. THE ONLY PLACE A LIBRARY IS NAMED.
const std::vector<Backend>& backends();

// Which format the NAME claims (extension, lower-cased). Null = not one of ours,
// and the caller falls through to whatever it did before. This is dispatch, not
// identification: what actually decodes is decided by the bytes.
const Backend* forPath(const std::string& path);

// Which format the BYTES are. Null = none of them. A file whose name and
// content disagree is decoded as its content and says so in the note - the
// bytes are evidence and the extension is a claim.
const Backend* forBytes(const uint8_t* p, size_t n);

// Decode, or explain. `path` is used for the extension and for nothing else.
//
// The error is the reason ALONE, in the register of docs/input-adapters.md
// §3.2 - the caller prefixes the file name, exactly as the .npy door does, so
// that one file's refusal is worded identically wherever it is raised:
//
//   holiday.png: not a PNG, JPEG or TIFF file (it starts 00 00 00 0c)
//     PNG and JPEG are read natively; TIFF is not in this build
//     choose a reader to read it another way
bool decode(const std::string& path, const std::vector<uint8_t>& bytes,
            Image& out, std::string& err);

// "PNG, JPEG" - the formats with a decoder behind them, for messages and for
// the file dialog. Computed from the table, so it cannot go stale.
std::string decodableFormats();

// Every extension the table dispatches on, "*.png *.jpg ..." - for the OS file
// dialog's filter line. Includes formats that are listed but absent: their
// refusal is more useful than the raw-dialog fallback a filtered-out file gets.
std::string dialogPattern();

}  // namespace imagefile
