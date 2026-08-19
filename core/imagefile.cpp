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
#include "rawread.h"
#include "tiffread.h"
#include "exrread.h"
#include "y4mread.h"

namespace imagefile {

// VIEWER_NO_LIBRAW: the build viewer-serve gets. The RAW row stays in the
// table, keeps its extensions and keeps its SNIFF (core/rawread.cpp's sniff is
// header inspection and needs no library), and loses only its decoder - which
// is the `absent` state this table was designed to have. A .NEF therefore still
// arrives AS a .NEF on the peer and is refused by name, instead of falling
// through to the TIFF row underneath it and being decoded as whatever IFD sits
// at the front of it.
#ifdef VIEWER_NO_LIBRAW
static const char* const RAW_ABSENT =
    "LibRaw is CDDL-1.0 and viewer-serve installs itself onto another machine "
    "over ssh, so the peer does not carry it";
#define VIEWER_RAW_LIBRARY ""
#define VIEWER_RAW_DECODE  nullptr
#define VIEWER_RAW_ABSENT  RAW_ABSENT
#else
#define VIEWER_RAW_LIBRARY RAW_LIBRARY
#define VIEWER_RAW_DECODE  rawDecode
#define VIEWER_RAW_ABSENT  nullptr
#endif

// THERE IS NO VIEWER_NO_OPENEXR, and its absence is a decision rather than an
// omission. One existed for exactly one build - the glibc-compatibility rebuild
// of viewer-serve in .github/workflows/build.yml, which is compiled by one g++
// line, and OpenEXR is the only reader here that is a LIBRARY rather than a
// file of ours. That build now builds OpenEXR too (statically, from the version
// CMakeLists.txt pins), because the flag bought its minute of CI with the very
// defect #148 was opened to remove: the Linux client read .exr and the peer in
// the same release tarball refused it, so one file answered two ways depending
// on which end opened it. A format this viewer reads is not a thing a build may
// elect - that is #53's ruling ("no off switch") and it does not stop being
// true when the reader is the peer.
//
// So the RAW row above is now the ONLY reader any build of this project leaves
// out, and the difference between the two is worth keeping legible: LibRaw is
// refused for a LICENCE, on every peer, by decision, and its `overLink` column
// says so on both ends. Nothing here is refused for a BUILD any more, and no
// two peers of the same protocol number differ in what they serve - which is
// what kept a capability list out of the HELLO reply, and still does.

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
static bool sniffY4m(const uint8_t* p, size_t n) {
    // A text signature, which is unusual here and is the format's own: the file
    // begins with the literal "YUV4MPEG2" and a space.
    return n >= 10 && memcmp(p, "YUV4MPEG2", 9) == 0 && (p[9] == ' ' || p[9] == '\n');
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
// (docs/features/adapters/input-adapters.md §3.1, "the last axis, four or fewer, is channels").
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
        { "PNG", ".png", "stb_image 2.30", sniffPng, stbDecode, nullptr, nullptr, true },
        { "JPEG", ".jpg .jpeg .jpe", "stb_image 2.30", sniffJpeg, stbDecode, nullptr, nullptr, true },
        // Vendor RAW sits BEFORE TIFF, and the order is the decision rather
        // than an accident of when it was written.
        //
        // A .NEF, a .ARW, a .PEF and a .DNG ARE TIFF files - the same first
        // four bytes a scanner's .tif has - so whichever of these two rows is
        // asked first gets all of them. In this order rawSniff answers only for
        // a file that says it is a camera's: a DNG version tag, a maker name
        // beside a sub-image chain, or one of the containers that carries its
        // own signature (core/rawread.h). Every TIFF the TIFF reader reads
        // today therefore still reaches it, and the other order would send
        // photographs off a camera to a reader that refuses them under the
        // wrong format's name.
        //
        // ".raw" is deliberately NOT among these extensions. It belongs to this
        // viewer's own headerless dialog, where the USER states the shape and
        // the depth because the file carries no header to state them; handing
        // it to a vendor library would be that library claiming to know
        // something about a file that says nothing.
        { "vendor RAW",
          ".dng .cr2 .cr3 .crw .nef .nrw .arw .srf .sr2 .orf .rw2 .rwl .raf "
          ".pef .ptx .srw .3fr .fff .iiq .kdc .dcr .mrw .x3f .erf .mef .mos "
          ".mdc .cap",
          // The one row whose `overLink` is false, and the only one whose
          // reason is a licence rather than a reader (see imagefile.h). The
          // peer's own build additionally has no decoder for it at all - the
          // VIEWER_NO_LIBRAW block above - so the refusal there is the table's
          // own `absent` sentence and not a special case anywhere.
          VIEWER_RAW_LIBRARY, rawSniff, VIEWER_RAW_DECODE, VIEWER_RAW_ABSENT,
          nullptr, false },
        // TIFF shipped for a while as a row with NO decoder, refusing by name.
        // What that row said is still the standard the reader behind it is held
        // to: TIFF is the only one of the three that carries measurements
        // (16-bit and float samples, a black level, sometimes a CFA pattern),
        // and reading it approximately is worse than not reading it. A reader
        // that assumed RGGB where the file said GBRG would produce plausible
        // pictures and wrong per-plane statistics with nothing on screen to say
        // so - so core/tiffread.cpp REFUSES a CFA TIFF, by name, rather than
        // open one. Everything else it will not read is refused the same way.
        { "TIFF", ".tif .tiff", TIFF_LIBRARY, sniffTiff, tiffDecode, nullptr, nullptr, true },
        // OpenEXR used to be the row that could be in either of this table's
        // two states, because a build option could take its library away. That
        // option is gone (#53, 2026-08-09: 「既定ONで。OFFにするパスは不要
        // です」), so the row is an ordinary one - a format with a decoder -
        // and `absent` is nullptr here for the same reason it is on the four
        // rows around it. It stayed that way through #148: the peer builds
        // OpenEXR too, including the glibc-compat rebuild, so there is no
        // build of this project in which this row loses its decoder. The
        // `absent` state belongs to the row above, whose reason is a licence.
        //
        // It is also the only row whose parts are NAMED (partWord): an .exr
        // holds layers, which are documents, where a multi-page TIFF holds
        // pages, which are frames.
        { "OpenEXR", ".exr", EXR_LIBRARY, sniffExr, exrDecode,
          nullptr, "exr layer", true },
        // y4m is a VIDEO container in a picture-format table, and it belongs
        // here because after docs/features/media/video-support.md's question it is one: a text
        // header plus raw planes, no compression, no inter-frame prediction -
        // so a file is N pictures of one shape in order, which is precisely
        // what a multi-page TIFF is and what this table's `member` rule already
        // covers. Everything that makes video a different KIND of problem (a
        // codec, a GOP, a seek that can land on the wrong picture) is absent
        // from this one format, which is also exactly why it is the only one:
        // the containers a codec library would open are the containers whose
        // numbers did not survive (core/y4mread.h). Those are refused by NAME,
        // in videoRefusal, and deliberately NOT as rows here.
        { "y4m", ".y4m", Y4M_LIBRARY, sniffY4m, y4mDecode, nullptr, nullptr, true },
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
// way out is offered (docs/features/adapters/input-adapters.md §3.2 / §4.13).
static const char* CHOOSE_A_READER = "\n  choose a reader to read it another way";

static std::string whatIsRead() {
    return "\n  " + listFormats(" and ", true) + " are read natively";
}

// ---------------------------------------------------------------- the link
// See imagefile.h: the ONE place that answers "will the peer serve this".
bool peerServes(const std::string& path) {
    const std::string e = lowerExt(path);
    // .npy is not a row of the table and never was - core/main.cpp opens it and
    // core/serve.cpp parses its header inline - so it is named here rather than
    // given a row with a decoder that does not exist.
    if (e == ".npy") return true;
    const Backend* b = forPath(path);
    return b && b->overLink;
}

// What the peer WILL serve, as a sentence fragment, computed from the table so
// a row added there cannot leave a refusal quoting yesterday's list.
static std::string servedList() {
    std::vector<const char*> names;
    for (const Backend& b : backends()) if (b.overLink) names.push_back(b.format);
    std::string s = ".npy";
    for (size_t i = 0; i < names.size(); i++) {
        s += (i + 1 == names.size()) ? " and " : ", ";
        s += names[i];
    }
    return s;
}

// ---- headerless RAW: the files that state nothing about themselves ---------
// See imagefile.h. One array, in the home both binaries compile.
const std::vector<std::string>& headerlessExts() {
    static const std::vector<std::string> E = { ".bin", ".raw", ".yuv", ".dat", ".rggb" };
    return E;
}
bool isHeaderless(const std::string& path) {
    const std::string e = lowerExt(path);
    for (const std::string& x : headerlessExts()) if (e == x) return true;
    return false;
}

bool isNpz(const std::string& path) { return lowerExt(path) == ".npz"; }

bool peerServesDeclared(const std::string& path) {
    return peerServes(path) || isHeaderless(path) || isNpz(path);
}

std::string peerRefusal(const std::string& path) {
    if (peerServes(path)) return {};
    // Named, reasoned, way out attached - docs/features/adapters/input-adapters.md §3.2's three
    // parts. The way out is real for the first two cases: the file IS readable
    // here, just not over this link.
    static const char* const WAY_OUT =
        "\n  browse it locally (File > Browse Folder), or copy it here first";
    if (const Backend* b = forPath(path)) {
        // A row this build reads and the peer does not: say WHY the peer does
        // not, which is never "we have no reader" (we do, one call away).
        std::string why = std::string(b->format) + " is read on this machine, but the "
                          "peer does not serve it";
        // The peer's own build has the reason in the table's `absent` field -
        // but only the peer's build does, so the sentence cannot be read out of
        // it here. The one row this applies to states it in imagefile.h.
        if (!b->overLink)
            why += ": LibRaw is CDDL-1.0 and viewer-serve installs itself onto "
                   "another machine over ssh";
        return why + WAY_OUT;
    }
    // A CONTAINER, and since protocol 13 the peer lists what is inside one
    // (issue #217, docs/features/remote/remote-reader-design.md §10.4). This sentence used to
    // be "the peer serves one array per file, not a container" - true when
    // nothing could address a member, and FALSE the day MSG_NPZ_SCAN shipped.
    // A refusal that denies a door which exists is verify-matrix G11 all over
    // again, so what is left is the part that is still true: this request named
    // the whole file as if it were one array, and a container has no single
    // geometry to answer with. The way out is the door that works, named.
    //
    // Reached by the ONE-CLICK PREVIEW and by anything else that addresses the
    // file as an array - deliberately: peerServesDeclared lets the Browse row
    // live and open, peerServes keeps the preview gate where it was, and this
    // is the sentence in between (F4g asserts both halves together, the way
    // F4d does for a headerless file).
    if (isNpz(path))
        return ".npz is a container and opens from its member list - this request "
               "addressed the whole file as one array"
               "\n  open it from Browse (the peer lists the members), or copy it here";
    // A headerless RAW. It reaches the fall-through below today and gets the
    // generic list, which is the defect verify-matrix G1 named: nobody DECIDED
    // that headerless files cannot cross the link, they fell off the end of a
    // table they were correctly never in.
    //
    // What is true right now is narrower and more useful than "the peer serves
    // these formats": the shape of this file is a DECLARATION that lives on
    // this machine, and this protocol has no field to carry it. Say that, and
    // name the door that does work. This is one step more specific than the
    // fall-through, which is the whole reason this branch exists - and it is
    // still the LINK's limit rather than a claim about the file, which is the
    // rule G9 (PR #176) settled.
    //
    // The wording changes the day the wire can carry a recipe
    // (docs/features/remote/remote-headerless-design.md §4.4, stage 2): then the fact is "this
    // request carried no recipe", not "this link cannot carry one".
    if (isHeaderless(path))
        return "a headerless " + lowerExt(path) + " states its shape in a recipe on "
               "this machine, and this link cannot yet carry that declaration"
               "\n  copy the file here and open it with File > Open, or update both "
               "ends when a recipe-carrying build ships";
    // A container this build refuses BY NAME already has a measured sentence,
    // and it answers the question the operator actually has. Asked first,
    // because the fall-through below can only say what the peer serves - and
    // for an .mp4 that is a list the operator can already see this file is not
    // in. `selftest.fmtgate` F3 asserts that a LOCAL unreadable row names its
    // own reason rather than the peer's; a remote row losing the same reason is
    // the same defect facing the other way (verify-matrix G7).
    {
        std::string vid = videoRefusal(path);
        if (!vid.empty()) return vid;
    }
    // The fall-through: a name with no row, no decoder here, and no measured
    // refusal of its own - headerless RAW today, whatever is unrecognised
    // tomorrow. It got the first two parts of docs/features/adapters/input-adapters.md §3.2 and
    // not the third, so it was the one refusal in this function that left the
    // operator with nowhere to go (verify-matrix G9).
    //
    // NOT the WAY_OUT above. That one says "browse it locally", which is true
    // of a file this build can read and false of the ones that land here - a
    // headerless .raw does open locally, an unrecognised name does not, and
    // this branch cannot tell them apart (core/main.cpp's own doors are not
    // compiled into the peer). So the way out states the LINK's limit, which
    // is true either way and is the fact that actually moves the file.
    //
    // ...and the second half of the way out, which is the one that actually
    // applies here: nothing that reaches this line is read natively on EITHER
    // machine, so the door is a reader (docs/features/adapters/input-adapters.md §4.13), and what
    // this refusal owes the operator is the state of that door. It was DECIDED
    // on 2026-08-03 (§4.13.1: "adapter は peer 側で走る" - the file is where the
    // data is, and shipping raw bytes here to convert them defeats the tool),
    // and until issue #180 stage 1-2 it was not built. Saying nothing made
    // "decided and unbuilt" read exactly like "never considered" - which was
    // verify-matrix G11, and what stage 0 fixed by naming the decision.
    //
    // STAGE 2 CHANGED THIS SENTENCE, exactly as stage 0 said it would. The door
    // is open: MSG_READER_RUN carries the reader there, it runs in the peer's
    // python, and META / TILE bring back the pixels a screen needs. So the line
    // is now the local door's own offer - `choose a reader` - and the Browse
    // row that shows it leads to the Reader panel (core/app/open_dispatch.inc
    // openRemote), which is the difference between describing a mechanism and
    // having one.
    //
    // The --serve-readers clause is not a hedge, it is the second half of the
    // truth (docs/features/remote/remote-reader-design.md §2): the peer's own launcher decides
    // whether code sent from here may run there, and a person told to choose a
    // reader without being told that would meet the gate's refusal with no idea
    // what it was about. It NAMES the flag rather than promising an outcome -
    // §7's rule that a dim row must not imply readers waiting on the peer still
    // holds: none are looked for by name over there, and none ever will be.
    return "the peer serves " + servedList() +
           "\n  this link carries those formats only - copy the file to this "
           "machine to open it with everything else this viewer reads"
           "\n  choose a reader: it is sent to the peer and runs where the file "
           "lives (docs/features/adapters/input-adapters.md §4.13.1), and only the pixels a screen "
           "needs come back - if that peer was started with --serve-readers"
           "\n  copy the file here to use a reader on this machine instead";
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
