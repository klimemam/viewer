// The framed stream a reader returns (`VIEWERSTREAM 3` from current writers),
// in the part BOTH
// binaries need.
//
// docs/background/reviews/adapter-transport-review.md froze the format: a line-based header, then
// the pixel blobs raw and in the order their `pixels` lines named them. The
// viewer has always read it (core/app/loader_npz.inc). Since issue #180 the
// PEER writes one too - a reader runs where the file lives and its result stays
// there as a cache file - so the peer has to be able to say where a node's
// bytes begin and whether the tree it declares is a tree at all.
//
// WHAT IS SHARED AND WHAT IS NOT, deliberately:
//
//   shared      the TREE CHECK and its sentences. #71's rule: a refusal the
//               peer sends is read by a person looking at this viewer's window,
//               so it must be the sentence the local door would have used. Two
//               copies is how "3|4" outlived the rule it described.
//   shared      where a node's blob STARTS. The peer seeks to it and the client
//               never sees the file, so there is exactly one implementation and
//               it is this one.
//   not shared  the rest of the header - note, cfa, range, timestamps,
//               conditions, meta. Those describe a DOCUMENT and only the client
//               builds documents; the peer would carry a parser for fields it
//               can never act on, which is the kind of copy that drifts because
//               nothing on one side exercises it. The client parses the header
//               ONCE for both its carriers (a cache file it read itself, and
//               the header text a peer returned verbatim), which is where that
//               half's "one definition" lives.
#pragma once
#include <stdint.h>
#include <stdio.h>                   // sscanf, for the `pixels` line
#include <stdlib.h>                  // atoi, for the numbers on the others
                                     // (spelled out for remote_proto.h's reason:
                                     //  MSVC and libc++ pull these in through
                                     //  <string>, libstdc++ does not, and the
                                     //  build that finds out is the Linux one)
#include <istream>
#include <string>
#include <vector>
#include "remote_proto.h"             // shared shape/channel refusal wording

namespace vns {

// v1 already carried layer/layout and v2 added AnalysisSet. v3 is the safety
// generation requiring readers to enforce those existing typed semantics at
// the materialisation boundary. Readers accept all three under the corrected
// semantics; writers name v3 unconditionally so a pre-v3 reader refuses the
// whole result instead of silently applying its native-axis heuristic.
inline constexpr int STREAM_VERSION = 3;

inline std::string versionError(int version, int maxVersion = STREAM_VERSION) {
    if (version <= maxVersion) return {};
    return "this stream is version " + std::to_string(version) +
           " and this viewer reads " + std::to_string(maxVersion);
}

// The two fields a TREE is made of. Everything else a node declares is about
// what it holds, not about where it sits.
struct TreeNode {
    std::string layer;      // "frame" "stack" "series" "batch" "analysisset"
    int parent = -1;
    // Empty is the layer's canonical order.  The only v1 transposed forms are
    // frame/CHW and stack/FCHW.  Kept in the shared projection because a
    // layout is a claim about the pixel axes, not client-only presentation.
    std::string layout;
};

// One typed pixel node's axes in its DECLARATION order.  Both the local
// container door and the peer's stream/cache door use this exact assignment;
// otherwise (C,H,W) can silently become F frames on one side of the link.
struct PixelAxes {
    int iF = -1, iH = -1, iW = -1, iC = -1;
};

inline std::string shapeText(const std::vector<int64_t>& shape) {
    return rp::npyShapeText(shape);
}

// Protocol 15 evidence carried by a typed Reader result. A named layout always
// needs the typed wire contract. With an empty layout, only Stack(F,H,W) whose
// W <= 4 differs from the old native rank heuristic (which calls it HWC).
inline bool requiresTypedAxes15(const TreeNode& node,
                                const std::vector<int64_t>& shape) {
    if (!node.layout.empty()) return true;
    return node.layer == "stack" && rp::npyNativeRead(shape) != rp::NR_STACK;
}

inline std::string typedAxesKind(const TreeNode& node) {
    return !node.layout.empty() ? "Reader " + node.layout : "Reader Stack/FHW";
}

// Frame -> HWC (or CHW when declared), Stack -> FHWC (or FCHW when
// declared).  Series has members and therefore deliberately has no raw tensor
// layout.  A declaration that belongs to another layer is refused by name;
// duck-typed adapters must not be allowed to turn it into a plausible but
// different image.
inline std::string pixelAxes(const TreeNode& v, const std::vector<int64_t>& shape,
                             PixelAxes& a) {
    a = PixelAxes{};
    const std::string sh = shapeText(shape);
    if (v.layer == "frame") {
        if (v.layout.empty()) {
            if (shape.size() == 2) { a.iH = 0; a.iW = 1; }
            else if (shape.size() == 3) { a.iH = 0; a.iW = 1; a.iC = 2; }
            else return rp::npyNotNativeText(shape);
        } else if (v.layout == "CHW") {
            if (shape.size() != 3)
                return "frame layout CHW needs rank 3, not shape " + sh;
            a.iC = 0; a.iH = 1; a.iW = 2;
        } else if (v.layout == "FCHW") {
            return "frame layout FCHW belongs to a stack, not a frame";
        } else {
            return "frame layout \"" + v.layout +
                   "\" is not known (empty or CHW)";
        }
    } else if (v.layer == "stack") {
        if (v.layout.empty()) {
            if (shape.size() == 3) { a.iF = 0; a.iH = 1; a.iW = 2; }
            else if (shape.size() == 4) {
                a.iF = 0; a.iH = 1; a.iW = 2; a.iC = 3;
            } else return rp::npyNotNativeText(shape);
        } else if (v.layout == "FCHW") {
            if (shape.size() != 4)
                return "stack layout FCHW needs rank 4, not shape " + sh;
            a.iF = 0; a.iC = 1; a.iH = 2; a.iW = 3;
        } else if (v.layout == "CHW") {
            return "stack layout CHW belongs to a frame, not a stack";
        } else {
            return "stack layout \"" + v.layout +
                   "\" is not known (empty or FCHW)";
        }
    } else {
        return v.layer + " has no raw pixel tensor layout; give it frame or stack members";
    }
    for (int64_t d : shape)
        if (d < 1) return "a zero-length axis in shape " + sh + ": no pixels in it";
    const int64_t c = a.iC >= 0 ? shape[(size_t)a.iC] : 1;
    if (c > 4) return rp::npyChannelCeilingText(shape, c);
    return {};
}

// docs/features/adapters/reader-analysisset.md's A3 gate, and the ONE spelling of its refusals.
// Called by the local carrier (core/app/loader_npz.inc vnzCheckTree, which
// projects its richer node onto this one) and by the peer before it accepts
// what a reader wrote. Same sentence from both, by construction rather than by
// promise.
inline std::string checkTree(const std::vector<TreeNode>& nodes, int n) {
    if (n < 0 || (size_t)n > nodes.size()) return "the node count does not match the nodes";
    for (int i = 0; i < n; i++) {
        const TreeNode& v = nodes[(size_t)i];
        const std::string si = std::to_string(i);
        if (v.layer != "frame" && v.layer != "stack" && v.layer != "series" &&
            v.layer != "batch" && v.layer != "analysisset")
            return "node " + si + " is a \"" + v.layer + "\": not a layer "
                   "(frame / stack / series / batch / analysisset)";
        if (i == 0 && v.parent != -1)
            return "node 0's parent is " + std::to_string(v.parent) +
                   ": node 0 is the root (-1)";
        if (i != 0 && (v.parent < 0 || v.parent >= i))
            return "node " + si + "'s parent is " + std::to_string(v.parent) +
                   ": a parent must be an earlier node (depth first, root 0)";
        if (v.layer == "series" && !v.layout.empty())
            return "node " + si + " is a series with layout \"" + v.layout +
                   "\": a series has no raw tensor layout; declare layout on its members";
        if (v.layer != "frame" && v.layer != "stack" && v.layer != "series" &&
            !v.layout.empty())
            return "node " + si + " is a " + v.layer + " with layout \"" +
                   v.layout + "\": only frame and stack pixels have a layout";
    }
    return {};
}

// One `pixels` line: which node it belongs to, what the array is, and how many
// bytes of it follow. `at` is the absolute offset of the first of those bytes
// in the stream, which is the number the peer seeks to and the only reason this
// scanner exists.
struct Blob {
    int node = 0;
    std::string dtype;              // numpy descr without the byte order: u2, f4, ...
    std::vector<int64_t> shape;     // declaration order
    uint64_t nbytes = 0;
    uint64_t at = 0;
};

// Pixel ownership is a tree fact, not a decoding detail. Every concrete image
// node owns exactly one blob; structural nodes own none. Checking this only
// while vnzBuild walks would be too late: a missing later node can leave an
// earlier document behind, while duplicate lines make the client select the
// last blob and the peer select the first. Both stream readers therefore feed
// their per-node counts through this one gate before any materialisation.
inline std::string checkPixelCounts(const std::vector<TreeNode>& nodes, int n,
                                    const std::vector<int>& counts) {
    if (n < 0 || (size_t)n > nodes.size() || (size_t)n > counts.size())
        return "the node count does not match the pixel declaration counts";
    for (int i = 0; i < n; i++) {
        const TreeNode& v = nodes[(size_t)i];
        const int count = counts[(size_t)i];
        const bool pixelLayer = v.layer == "frame" || v.layer == "stack";
        if (pixelLayer && count != 1)
            return "node " + std::to_string(i) + " is a " + v.layer + " with " +
                   std::to_string(count) + " pixels declaration(s): exactly one is required";
        if (!pixelLayer && count != 0)
            return "node " + std::to_string(i) + " is a " + v.layer + " with " +
                   std::to_string(count) + " pixels declaration(s): only frame and stack "
                   "nodes have pixels";
    }
    return {};
}

// What the peer reads out of a stream: the version, the tree, and where each
// blob sits. Everything else is skipped by the rule the format already has -
// "a key this build does not know is skipped rather than refused".
struct Scan {
    int version = 0;
    int n = 0;
    std::vector<TreeNode> nodes;
    std::vector<Blob> blobs;
    uint64_t headerEnd = 0;         // first byte after the "end" line
};

// Read the header of a stream open at its first byte. Returns "" on success.
// The stream is left positioned wherever the reads left it; every offset the
// caller needs is absolute and in `out`.
//
// The refusals here are the local carrier's, word for word, for checkTree's
// reason: loadViewerStream is the door most readers come through and a person
// who sees one of these sentences must not be able to tell which end wrote it.
inline std::string scanHeader(std::istream& f, Scan& out,
                              int maxVersion = STREAM_VERSION) {
    out = Scan{};
    std::string line;
    if (!std::getline(f, line)) return "the reader produced nothing";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, 13, "VIEWERSTREAM ") != 0)
        return "not a viewer stream: first line is \"" + line.substr(0, 60) + "\"";
    out.version = atoi(line.c_str() + 13);
    const std::string versionErr = versionError(out.version, maxVersion);
    if (!versionErr.empty()) return versionErr;
    int n = -1;
    bool sawEnd = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "end") { sawEnd = true; break; }
        const size_t sp = line.find(' ');
        const std::string key = sp == std::string::npos ? line : line.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        if (key == "n") {
            n = atoi(rest.c_str());
            if (n <= 0 || n > 100000) return "n is " + rest + ": not a node count";
            out.nodes.assign((size_t)n, TreeNode{});
            continue;
        }
        if (n < 0) return "the stream declares its nodes before it describes them (n is missing)";
        const size_t sp2 = rest.find(' ');
        const int idx = atoi(rest.c_str());
        const std::string val = sp2 == std::string::npos ? std::string() : rest.substr(sp2 + 1);
        if (idx < 0 || idx >= n) continue;              // a node this build does not have
        TreeNode& v = out.nodes[(size_t)idx];
        if (key == "layer") { v.layer = val; continue; }
        if (key == "parent") { v.parent = atoi(val.c_str()); continue; }
        if (key == "layout") { v.layout = val; continue; }
        if (key == "pixels") {
            Blob b;
            b.node = idx;
            const char* p = val.c_str();
            char dt[32] = { 0 };
            int ndim = 0, used = 0;
            if (sscanf(p, "%31s %d%n", dt, &ndim, &used) != 2) continue;
            b.dtype = dt;
            p += used;
            if (ndim < 0 || ndim > 8) continue;
            for (int k = 0; k < ndim; k++) {
                long long d = 0;
                if (sscanf(p, " %lld%n", &d, &used) != 1) { ndim = -1; break; }
                p += used;
                b.shape.push_back((int64_t)d);
            }
            if (ndim < 0) continue;
            unsigned long long nb = 0;
            if (sscanf(p, " %llu", &nb) != 1) continue;
            b.nbytes = (uint64_t)nb;
            out.blobs.push_back(b);
        }
        // any other key: skipped on purpose, see the format note above
    }
    if (!sawEnd) return "the stream stopped before its header ended - the reader was cut short";
    if (n < 0) return "the stream never said how many nodes it has";
    out.n = n;
    const std::streampos here = f.tellg();
    if (here < 0) return "cannot seek in what the reader wrote";
    out.headerEnd = (uint64_t)here;
    // The blobs follow IN ORDER, back to back, starting where the header
    // stopped. That is the format's own rule (the client reads them
    // sequentially and refuses one that arrives out of order), which is what
    // makes an offset computable at all rather than needing a directory.
    uint64_t at = out.headerEnd;
    for (Blob& b : out.blobs) { b.at = at; at += b.nbytes; }
    const std::string treeErr = checkTree(out.nodes, out.n);
    if (!treeErr.empty()) return treeErr;
    std::vector<int> pixelCounts((size_t)out.n, 0);
    for (const Blob& b : out.blobs) {
        if (b.node < 0 || b.node >= out.n)
            return "a pixels blob names node " + std::to_string(b.node) +
                   ", outside this tree";
        pixelCounts[(size_t)b.node]++;
    }
    const std::string countErr = checkPixelCounts(out.nodes, out.n, pixelCounts);
    if (!countErr.empty()) return countErr;
    for (const Blob& b : out.blobs) {
        PixelAxes a;
        const std::string axesErr = pixelAxes(out.nodes[(size_t)b.node], b.shape, a);
        if (!axesErr.empty())
            return "node " + std::to_string(b.node) + ": " + axesErr;
    }
    return {};
}

}  // namespace vns
