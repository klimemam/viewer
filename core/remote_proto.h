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

namespace rp {

static const uint32_t MAGIC = 0x56525031;   // "VRP1"
static const uint32_t VERSION = 1;

enum MsgType : uint32_t {
    MSG_HELLO      = 1,   // -> (version)                  <- (version, server id)
    MSG_LIST       = 2,   // -> (path)                     <- entries
    MSG_META       = 3,   // -> (path)                     <- shape/dtype/frames
    MSG_TILE       = 4,   // -> (path, frame, rect, step)  <- pixels
    MSG_MEASURE    = 5,   // reserved: run an analyzer where the data is
    MSG_OK         = 128,
    MSG_ERR        = 129,
};

// Every message: [magic][type][payload bytes][payload]. Payloads are packed
// little-endian scalars followed by any blob; both ends are ours, and the format
// is versioned by HELLO, so there is no need for anything more elaborate.
struct Header {
    uint32_t magic;
    uint32_t type;
    uint32_t len;
};

// TILE request. The client asks for the region it is about to draw, decimated to
// roughly the pixels it can actually show: `step` is the sample stride, so a
// 4000x3000 image shown in a 1000px pane travels as 1000x750, not 12 Mpx.
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

// `viewer --serve`: answer requests on stdin/stdout until the peer closes.
int runServeMode();

// META reply: what the client needs to lay out the image before any pixel moves.
struct MetaRep {
    uint32_t w, h, ch;
    uint32_t dtype;
    uint32_t frames;            // 1 for a plain image, N for a frame axis
    uint32_t flags;
};

}  // namespace rp
