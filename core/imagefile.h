#pragma once
// ---------------------------------------------------------------------------
// The picture formats that are not .npy: PNG, JPEG, TIFF, OpenEXR.
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
//     The float formats say the same rule from the other side: a 12.5 in an
//     .exr is a 12.5 in the document. Not tone mapped, not gamma-encoded, not
//     clamped to 0..1, and negatives kept - a viewer that display-encodes a
//     scene-linear file on load has destroyed the measurement it was opened
//     to make, and it has done so in a way that still draws a plausible
//     picture.
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

// The viewer's own ceiling on a dimension, the same number core/main.cpp's
// npyLayout enforces and refuses in the same words. It lives in the header
// because a backend has to be able to refuse an absurd header BEFORE it
// allocates for it: 32769 squared in f32 is 4 GB, and "refuse after decoding"
// would mean the machine swaps first and reads the message second.
enum { MAX_DIM = 32768 };

// One decoded picture: exactly the fields a FrameSource needs, and no others.
// There is deliberately no frame axis here - PNG and JPEG hold one picture, and
// a multi-page TIFF is a stack, which is the caller's word (App::SeqInfo) and
// not a decoder's. That backend has arrived: it returns several of these and
// the caller builds the stack the same way loadNpyBuffer does.
//
// SEVERAL PICTURES OUT OF ONE FILE ARE ONE OF TWO THINGS, and `member` is which:
//
//   * empty  - the file's pictures are UNNAMED, so they are ordered, and an
//              order over pictures of one shape is a frame axis. Three pages of
//              a TIFF are three frames of one stack.
//   * set    - the file NAMES its parts, so they are not ordered and they need
//              not share a shape. Two layers of an .exr are two documents, the
//              same as two arrays inside one .npz.
//
// The decoder says which; the caller (loadImageFile) builds the stack or the
// documents. Neither concept belongs to a decoder, and neither is guessable
// from a count - a two-layer .exr and a two-page TIFF are both "two pictures".
struct Image {
    int w = 0, h = 0, ch = 1;      // ch: 1 grey, 2 grey+alpha, 3 RGB, 4 RGBA
    std::string dtype;             // "u8" / "u16": what the FILE stored, not what we hold
    std::vector<float> data;       // w*h*ch, interleaved, VALUES AS STORED (rule 1)
    std::string note;              // what the decoder did, for the Inspector (rule 2)
    int cfa = 0;                   // 0 none, 1 Bayer, 2 Quad Bayer - only if READ (rule 3)
    int cfaPattern = 0;            // index into CFA_PATTERNS, meaningless when cfa == 0
    std::string member;            // this picture's name INSIDE the file; see above
};

// A format, and whatever reads it in THIS build.
//
// `decode == nullptr` is a first-class state, not a hole: the format is known,
// it is listed, it is dispatched to, and it refuses with `absent` - a sentence
// naming what is missing and why. That is how TIFF shipped until a reader was
// written for it, and it is what any future format costs to LIST rather than
// omit. The alternative (leave the format out of the table) turns its files
// into "unknown file, try the raw dialog", which tells the user nothing about
// the actual situation.
//
// `decode` fills a VECTOR because one file is not always one picture: a
// multi-page TIFF holds a frame per page. Formats that hold exactly one push
// exactly one, and the caller cannot tell the difference until it counts.
struct Backend {
    const char* format;            // "PNG"
    const char* exts;              // ".png" - space separated, lower case, dot included
    const char* library;           // what is linked for it, version included; "" when absent
    bool (*sniff)(const uint8_t* p, size_t n);                 // magic bytes, never the name
    bool (*decode)(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err);
    const char* absent;            // why there is no decoder; nullptr when there is one
    // What THIS format calls a named part, for the one line the Inspector
    // prints over `Image::member`. nullptr = the format has no named parts.
    // It is a column of this table because it is a fact about the format, and
    // because the alternative - the Inspector testing the extension itself -
    // is a second place that knows what an .exr is, which is precisely what
    // this seam exists to prevent.
    const char* partWord;
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
// One or more pictures, in file order. More than one means the file held more
// than one, and what to do with that is the caller's decision and vocabulary:
// several pictures out of one file are the frames of a stack.
//
// The error is the reason ALONE, in the register of docs/input-adapters.md
// §3.2 - the caller prefixes the file name, exactly as the .npy door does, so
// that one file's refusal is worded identically wherever it is raised:
//
//   holiday.png: not a PNG, JPEG or TIFF file (it starts 00 00 00 0c)
//     PNG, JPEG and TIFF are read natively
//     choose a reader to read it another way
bool decode(const std::string& path, const std::vector<uint8_t>& bytes,
            std::vector<Image>& out, std::string& err);

// "PNG, JPEG" - the formats with a decoder behind them, for messages and for
// the file dialog. Computed from the table, so it cannot go stale.
std::string decodableFormats();

// Every extension the table dispatches on, "*.png *.jpg ..." - for the OS file
// dialog's filter line. Includes formats that are listed but absent: their
// refusal is more useful than the raw-dialog fallback a filtered-out file gets.
std::string dialogPattern();

// The formats this build refuses OUTRIGHT, by name: the codec-bearing video
// containers. "" when `path` is not one of them.
//
// Deliberately not rows in the table above, and the difference is the reason
// rather than the mechanism. That table answers "what decodes this, and what
// would it take to" - its `absent` state is a format this build COULD read if
// something were linked, and its dispatch is by BYTES. These are formats this
// build has decided not to read even though a library could: an 8-bit lossy
// round trip destroys the measurement (docs/video-support.md §1 measured a
// sigma_t of 40 DN16 coming back as 0.00), so linking a codec would not change
// the answer. They are matched by NAME, they have no decoder to point at, and
// putting twenty-one of them in the table would also march *.mp4 into
// dialogPattern() and thus into the image file dialog - the opposite of what
// this says.
//
// Implemented in core/y4mread.cpp, beside the one video container that IS read,
// because the two halves of that decision only make sense together.
std::string videoRefusal(const std::string& path);

}  // namespace imagefile
