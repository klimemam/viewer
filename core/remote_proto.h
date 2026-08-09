// Wire format shared by the client and `viewer --serve`.
//
// The transport is deliberately the ssh stdio pipe: the client runs
//     ssh user@host viewer --serve
// and talks to its stdin/stdout, exactly the way rsync and git do it. No port to
// open, no daemon to keep alive, no listening socket to secure - ssh already did
// the authentication, and a process that owns no socket cannot be reached by
// anyone who is not already through ssh.
//
// What crosses the link is only what the screen needs: a decimated view of the
// region being looked at, in the source dtype (so pixel values stay exact), and
// measurement RESULTS rather than the pixels they were measured from.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace rp {

static const uint32_t MAGIC = 0x56525031;   // "VRP1"
// 1 = LIST/META/TILE; 2 adds MEASURE; 3 extends the LIST reply (mtime, npy
// header peek, synthetic stack-group entries) and adds GLOB/SCAN. The server
// answers LIST in the v2 shape when the client's HELLO said 2, so either end
// may lag the other by one protocol without breaking the session.
//
// 4 changes no wire format at all: the sequence-GROUPING rules changed ('?'
// only on the varying digit runs; the frame axis is the LAST varying run).
// The number is what makes an already-installed peer update itself on connect
// - versioning the MEANING, not just the framing. The user hit the gap this
// closes: a peer updated mid-way through those changes grouped a linearity
// folder differently than the same folder opened locally, with nothing
// anywhere saying why. If the answers can differ, the versions must.
//
// 5: same story again - the DISPLAYED pattern of a sequence group now carries
// its extent ("frame_000..023.npy", not "frame_???.npy"). Wire format
// unchanged, but a stale peer sends the old text while the local scanner
// produces the new one, so the same folder reads differently depending on
// which side listed it. Versioning the meaning is what makes the peer update
// itself on connect.
//
// 6: the MEANING again, and this time of a number people paste into reports.
// MOP_TEMPORAL_STATS' `sigma_fpn [DN]` is now the CORRECTED quantity - the
// temporal residual mean(s_t,i^2/n_i) subtracted, ddof=1 spatial variance,
// clamped at 0 and saying so (#57 judgment 2, docs/flat-field-stats.md (b)) -
// and the reply carries `fpn_corr [DN]` / `fpn_clamped` beside it. A v5 peer
// answers the same key with the UNCORRECTED upper bound, which the panel would
// then print under a label declaring a correction it never had: the same folder
// measured here and there would differ by sigma_t^2/N with nothing saying why.
// Wire format is unchanged (MEASURE carries named items), so this number exists
// solely to make an installed peer update itself on connect.
static const uint32_t VERSION = 6;

enum MsgType : uint32_t {
    MSG_HELLO      = 1,   // -> (version)                  <- (version, server id)
    MSG_LIST       = 2,   // -> (path)                     <- entries
    MSG_META       = 3,   // -> (path)                     <- shape/dtype/frames
    MSG_TILE       = 4,   // -> (path, frame, rect, step)  <- pixels
    MSG_MEASURE    = 5,   // -> (op, frames, rois, name)   <- emitted results only
    MSG_GLOB       = 6,   // -> (root, pattern, depth, cap) <- matching rel paths
    MSG_SCAN       = 7,   // -> (root, depth, cap)         <- stack groups per subdir
    MSG_OK         = 128,
    MSG_ERR        = 129,
};

// LIST reply, v3. v2 was: [u32 n] then per entry [str name][u32 dir][u32 szLo]
// [u32 szHi]. v3 is:      [u32 n] then per entry
//   [str name][u32 flags][u32 szLo][u32 szHi][u32 mtimeLo][u32 mtimeHi]
//   flags & LE_META : [u32 dtype][u32 ndim][u32 dims[4]] declaration order,
//                     0-padded  [u32 fortran]           (.npy header peek)
//   flags & LE_GROUP: [u32 frameCount][frameCount * str memberName]
// mtime is unix seconds (64-bit as lo/hi like the size: 2038 is within the
// service life of a lab tool). A group entry's size is the sum over members,
// its mtime the newest member, its META fields those of the first frame.
enum ListEntryFlags : uint32_t {
    LE_DIR   = 1,   // directory
    LE_META  = 2,   // npy header fields follow
    LE_GROUP = 4,   // synthetic entry for a numbered .npy sequence
};

// MEASURE: run analysis where the data lives. A 300-frame statistic crosses the
// wire as a few hundred bytes of results instead of gigabytes of pixels - and
// it runs immediately, while any pixel transfer proceeds in parallel.
enum MeasureOp : uint32_t {
    MOP_ANALYZER        = 0,   // run a named plugin analyzer on ONE frame
    MOP_TEMPORAL_STATS  = 1,   // per-pixel mean/var over N frames (noise vs FPN)
    MOP_FRAME_ROI_STATS = 2,   // per-frame per-ROI mean/var (PTC / linearity)
};

// Fixed head of the request; the variable part follows as
//   [str path * nPaths][str analyzer][str paramsJson][{u32 x,y,w,h} * nRois]
// nPaths > 1 means one file per frame, in sequence order. analyzer/params are
// empty for the aggregate ops. nRois == 0 means whole frame.
struct MeasureReqHead {
    uint32_t op;                 // MeasureOp
    uint32_t frame0, frameCount; // range in a frame-axis file; frameCount 0 = all
    uint32_t cfaType, cfaPattern;// psCfaType / psCfaPattern values
    float    black, white;       // display-range hint handed to the analyzer
    uint32_t nPaths, nRois;
    uint32_t flags;              // reserved, 0
};

// Reply (MSG_OK): a serialization of exactly what the plugin sink emitted, so
// the host renders it through the same grid/plot code as a local run.
//   [u32 serverLoc]  0 = CPU, 1 = CUDA (provenance)
//   [u32 framesUsed]
//   [u32 nCols] per col: [u32 nItems]
//       per item: [u32 kind] [str key] (kind 0 ? [f64 value] : [str value])
//   [u32 nSeries] per series: [str name][str xLabel][str yLabel]
//       [u32 col][u32 hasX][u32 n] (hasX ? [f32*n xs]) [f32*n ys]

// shared sample-to-float conversion (defined in serve.cpp, linked everywhere)
void toFloatSamples(const uint8_t* src, uint32_t dtype, size_t n, float* out);

// Every message: [magic][type][payload bytes][payload]. Payloads are packed
// little-endian scalars followed by any blob; both ends are ours, and the format
// is versioned by HELLO, so there is no need for anything more elaborate.
struct Header {
    uint32_t magic;
    uint32_t type;
    uint32_t len;
};

// TILE request. The client asks for the region it is about to draw, decimated to
// roughly the pixels it can actually show: `step` is the sample stride, chosen so
// that one returned sample is about one screen pixel (step ~ 1/zoom). A 4000x3000
// frame fitted into a 1000px-tall pane travels as 1334x1000, not as 12 Mpx.
struct TileReq {
    uint32_t frame;             // frame index within the file (0 for a plain image)
    uint32_t x, y, w, h;        // region in source pixels
    uint32_t step;              // 1 = full resolution, n = every nth sample
    uint32_t flags;             // bit0: compress payload (deflate)
};

// TILE reply header; the pixel blob follows, possibly deflate-compressed.
struct TileRep {
    uint32_t w, h, ch;          // dimensions AFTER decimation
    uint32_t dtype;             // rp::DType
    uint32_t rawBytes;          // uncompressed size of the blob
    uint32_t flags;             // bit0: blob is deflate-compressed
};

enum DType : uint32_t {
    DT_U8 = 0, DT_I8, DT_U16, DT_I16, DT_U32, DT_I32, DT_F32, DT_F64, DT_COUNT
};
static inline size_t dtypeSize(uint32_t t) {
    switch (t) {
        case DT_U8: case DT_I8:   return 1;
        case DT_U16: case DT_I16: return 2;
        case DT_U32: case DT_I32: case DT_F32: return 4;
        case DT_F64: return 8;
        default: return 0;
    }
}
const char* dtypeName(uint32_t t);
uint32_t dtypeFromName(const char* s);

// ---- how a .npy shape is SAID, spelled once for both doors -----------------
//
// This viewer has two ways in - the local decoder (core/app/loader_npz.inc) and
// the peer (core/serve.cpp) - and issue #71 was what happens when they answer
// the same file differently. The RULE is one rule now (the last axis is
// channels when it is 4 or fewer, on both sides). These are the SENTENCES that
// describe it, and they belong in the one header both sides already include for
// the same reason the rule does: a refusal the peer sends is read by a person
// looking at this viewer's window, so it has to be the sentence the local door
// would have used. A second copy in serve.cpp is a copy that drifts, which is
// precisely how the "3|4" spelling outlived the rule it was describing.
//
// ASCII "C<=4" rather than U+2264: this string is printed to consoles whose
// codepage may be cp932 (which cannot encode that character at all) and is
// quoted back verbatim through a pipe by tools/import/run_adapter.py.
// docs/input-adapters.md §4.13.0 draws the same forms with the real character.
static const char* const NPY_NATIVE_FORMS =
    "(H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)";

// "(24, 480, 640)" / "scalar" - the shape a human recognises from the script
// that wrote it, printed from the header and never from what we made of it.
inline std::string npyShapeText(const std::vector<int64_t>& shape) {
    if (shape.empty()) return "scalar";
    std::string s = "(";
    for (size_t i = 0; i < shape.size(); i++) {
        s += std::to_string(shape[i]);
        if (i + 1 < shape.size()) s += ", ";
        else if (shape.size() == 1) s += ",";
    }
    return s + ")";
}

// docs/input-adapters.md §3.2: name the shape that ARRIVED, and name what
// native DOES read. A refusal that says only "cannot open" sends the reader
// nowhere - which is how a 1-D exposure vector stayed a one-pixel-tall image
// for as long as it did.
//
// Two lines, not three. The local door adds a third ("choose a reader to read
// it another way") because openReaderPicker is there to receive it; the peer
// does not, because a document opened through a peer has no reader picker yet
// (issue #71 D3 - FrameSource::npyShape is only ever set by the local decoder).
// Promising a way out that the remote half does not have would be worse than
// the dead end it replaces, so the peer stops at the two lines it can keep.
inline std::string npyNotNativeText(const std::vector<int64_t>& shape) {
    return (shape.empty() ? std::string("a scalar")
                          : "shape " + npyShapeText(shape)) +
           " is not a native form\n  native reads " + std::string(NPY_NATIVE_FORMS);
}

// `viewer --serve`: answer requests on stdin/stdout until the peer closes.
int runServeMode();

// Natural order over the WHOLE name: every digit run compares as a number
// ("img2_gain10" < "img10_gain2"), case-insensitive elsewhere. Lives here
// because BOTH ends must agree on frame order - the client sorts what it
// opens, the server sorts what a scan folds into an unnumbered group.
inline bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            size_t i0 = i, j0 = j;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') i++;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') j++;
            size_t ia = i0, jb = j0;                 // strip leading zeros
            while (ia < i - 1 && a[ia] == '0') ia++;
            while (jb < j - 1 && b[jb] == '0') jb++;
            size_t la = i - ia, lb = j - jb;
            if (la != lb) return la < lb;
            int c = a.compare(ia, la, b, jb, lb);
            if (c != 0) return c < 0;
        } else {
            int la = ca >= 'A' && ca <= 'Z' ? ca + 32 : ca;
            int lb2 = cb >= 'A' && cb <= 'Z' ? cb + 32 : cb;
            if (la != lb2) return la < lb2;
            i++; j++;
        }
    }
    return a.size() - i < b.size() - j;
}

// The DISPLAYED form of a sequence pattern: the frame-axis '?' run replaced by
// the range it actually covers.
//
//   "????.npy"      over 0000.npy .. 0003.npy    -> "0000..0003.npy"
//   "frame_???.npy" over frame_000 .. frame_023  -> "frame_000..023.npy"
//   "f_?.npy"       over f_9, f_10, f_11         -> "f_9..11.npy"
//
// (with U+2025 TWO DOT LEADER, not two periods, where this comment writes "..")
//
// A folder of 0000.npy .. 0003.npy groups as "????.npy": correct by the rule -
// every digit varies - and it says nothing at all. The extent says what the
// stack IS, and it is exactly as cheap: the member names are already in hand
// wherever a pattern is built.
//
// Lives HERE, inline, because both ends produce patterns and both ends must
// agree character for character: the peer names the LIST / SCAN group row, the
// client names the stack it opens from a local folder, and a capture opened
// both ways has to read the same in the Files panel and in the session file.
// Only the frame-axis run is touched; every other digit stays literal, so
// "gain10_???.npy" becomes "gain10_000..007.npy" and never loses the gain.
inline std::string patternWithExtent(const std::string& pattern,
                                     const std::vector<std::string>& members) {
    size_t qs = pattern.find('?');
    if (qs == std::string::npos || members.size() < 2) return pattern;
    size_t qe = qs;
    while (qe < pattern.size() && pattern[qe] == '?') qe++;
    // more than one '?' run means a degenerate grouping the caller could not
    // analyse; rewriting one of the two runs would claim a frame axis it never
    // decided on, so the pattern is left exactly as it came.
    if (pattern.find('?', qe) != std::string::npos) return pattern;
    auto isDig = [](char c) { return c >= '0' && c <= '9'; };
    // which digit run of a member name the '?' run stands for
    size_t runIdx = 0;
    for (size_t i = 0; i < qs; i++)
        if (isDig(pattern[i]) && (i == 0 || !isDig(pattern[i - 1]))) runIdx++;
    std::vector<std::string> vals;
    vals.reserve(members.size());
    for (const std::string& m : members) {
        size_t run = 0, i = 0;
        bool found = false;
        while (i < m.size()) {
            if (!isDig(m[i])) { i++; continue; }
            size_t j = i;
            while (j < m.size() && isDig(m[j])) j++;
            if (run++ == runIdx) { vals.push_back(m.substr(i, j - i)); found = true; break; }
            i = j;
        }
        if (!found) return pattern;          // member structure differs: give up
    }
    // by VALUE, not by string: frame_9 and frame_10 are not lexicographic, and
    // the zero padding of the winning member is kept as it is on disk
    size_t loI = 0, hiI = 0;
    unsigned long long loV = ~0ull, hiV = 0;
    for (size_t k = 0; k < vals.size(); k++) {
        unsigned long long n = 0;
        for (char c : vals[k]) {
            if (n > 1000000000000000ull) break;      // absurd run: stop accumulating
            n = n * 10 + (unsigned long long)(c - '0');
        }
        if (n < loV) { loV = n; loI = k; }
        if (n > hiV) { hiV = n; hiI = k; }
    }
    if (vals[loI] == vals[hiI]) return pattern;
    return pattern.substr(0, qs) + vals[loI] + "\xE2\x80\xA5" + vals[hiI] +
           pattern.substr(qe);
}

// META reply: what the client needs to lay out the image before any pixel moves.
struct MetaRep {
    uint32_t w, h, ch;
    uint32_t dtype;
    uint32_t frames;            // 1 for a plain image, N for a frame axis
    uint32_t flags;
};

}  // namespace rp
