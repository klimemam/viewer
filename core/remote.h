// Client side of `viewer --serve`: talks to a peer process over its stdin/stdout.
//
// The peer is normally started by ssh:
//     ssh user@host viewer --serve
// so there is nothing to install beyond the same binary, nothing listening on the
// network, and no credentials of our own - ssh owns the authentication. Passing
// an empty host starts a local peer instead, which is how this is tested.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "remote_proto.h"            // rp::F32Loss - tile()'s narrowing census

namespace remote {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    // Protocol v3 listing extras. A v2 peer never sends them, so the defaults
    // double as the "unknown" values the UI renders as "-".
    int64_t mtime = 0;                 // unix seconds; 0 = unknown
    bool hasMeta = false;              // the server peeked this .npy's header
    std::string dtype;                 // "u16", "f32", ...; empty = unknown
    int ndim = 0;
    uint32_t dims[4] = { 0, 0, 0, 0 }; // numpy declaration order, 0-padded
    bool fortran = false;
    bool group = false;                // synthetic row for a numbered sequence
    uint32_t frames = 0;               // member count when group
    std::vector<std::string> members;  // member file names (no directory part)
};

// LIST reply payload -> entries, in the shape `peerVersion` promises. Split out
// of Session so the v2-compat path stays testable without a live v2 peer.
bool parseListPayload(const std::vector<uint8_t>& payload, int peerVersion,
                      std::vector<Entry>& out, std::string& err);

// One stack found by SCAN: where it lives (relative to the scanned root, "" =
// the root itself) and its group entry (pattern, members, meta).
struct ScanGroup { std::string dir; Entry entry; };

// One GLOB result: path relative to the searched root ('/' separators).
struct GlobHit { std::string rel; bool dir = false; };

struct Meta {
    int w = 0, h = 0, ch = 1, frames = 1;
    std::string dtype;
    // The shape the FILE declared, before any interpretation (protocol 9,
    // issue #124). EMPTY from a peer that predates it - which is the only
    // honest value, because "no shape" and "a shape I cannot tell you about"
    // must not read the same to the Inspector: the first is a document with no
    // §3.3 menu to offer, the second is a document whose menu exists and cannot
    // be acted on, and they get different lines on screen.
    std::vector<int64_t> shape;
    // The peer read this NATIVELY because the reading that was asked for does
    // not fit the shape - the same fallback core/app/loader_npz.inc's npyLayout
    // makes, reported so the caller can write the same note and record that no
    // declaration was applied. Reached by a session line pointing at a file
    // whose rank changed, never by the menu.
    bool readFellBack = false;
};

// MEASURE: what came back is exactly what the analyzer emitted, so the caller
// renders it through the same grid/plot code as a local run.
struct MeasureItem {
    int kind = 0;                 // 0 = number, 1 = text
    std::string key;
    double num = 0;
    std::string text;
};
struct MeasureSeries {
    std::string name, xLabel, yLabel;
    int col = 0;
    std::vector<float> xs, ys;    // xs empty = use 0..n-1
};
struct MeasureResult {
    int serverLoc = 0;            // 0 = CPU, 1 = CUDA (provenance)
    int framesUsed = 0;           // n
    // MOP_PLUGIN_ANALYZE and MOP_SET_FOLD (docs/abi-v3.md §10/§11): WHO
    // computed this, read off the PEER's ledger - its plugin row for the first,
    // its own viewer version for the second, since a built-in has no dll.
    // Empty for every other op: those never claimed a provenance trailer.
    // These are the peer's strings and are never filled in from a local dll of
    // the same name: a citation names the computer, and this machine did not
    // compute it.
    std::string provName, provVersion, provFile, provPath;
    int expected = 0;             // N; framesUsed < expected = a partial stack
    std::vector<std::vector<MeasureItem>> cols;
    std::vector<MeasureSeries> series;
};
struct MeasureReq {
    int op = 0;                   // rp::MeasureOp
    int frame0 = 0, frameCount = 0;
    int cfaType = 0, cfaPattern = 0;
    float black = 0, white = 1;
    std::vector<std::string> paths;   // >1 = one file per frame, in order
    std::string analyzer, params;     // empty for aggregate ops
    // MOP_PLUGIN_ANALYZE only: what OUR descriptor declares, verbatim from the
    // ledger, and "" when it declares nothing (a V1/V2 descriptor). Never a
    // guess and never the dll's file-version resource - the peer refuses an
    // undeclared version, and a fabricated one would turn that refusal into a
    // false pass.
    std::string analyzerVersion;
    int target = 0;                   // rp::MeasureTarget: frame or stack
    struct Roi { int x = 0, y = 0, w = 0, h = 0; };
    std::vector<Roi> rois;            // empty = whole frame
    // The declared geometry, when the frames are HEADERLESS (protocol 11). One
    // recipe for the whole request: every frame of a stack is the same picture
    // measured again (docs/terminology.md), so a per-frame geometry would not
    // be a stack. A SET names several stacks and could need one per role, which
    // this grammar cannot say - so it is refused rather than approximated
    // (docs/remote-headerless-design.md §7.3).
    bool hasRecipe = false;
    rp::RawWire recipe{};
    // MOP_SET_FOLD only. A set is {role: stack}, and the flat `paths` list
    // above cannot say where one stack stops - so the roles carry their own
    // paths and their own frame range, and `paths` is IGNORED for this op
    // rather than kept in step with them by hand. One source of truth: measure()
    // writes the flat list out of these, in declaration order.
    struct Role {
        std::string role;             // the schema's name, verbatim
        std::vector<std::string> paths;   // >1 = one file per frame, in order
        int frame0 = 0, frameCount = 0;   // range within a frame-axis file
    };
    std::vector<Role> roles;
    // What OUR fold declares (setfold::foldForm), and "" never - an undeclared
    // form is refused by the peer, exactly as an undeclared plugin version is.
    std::string foldForm;
    int join = 0;                     // rp::SetJoin
};

// One connection to one machine. Not thread-safe: requests are serialised by the
// caller (the UI thread today, a prefetch thread later).
class Session {
public:
    ~Session();
    // host empty -> run `exe --serve` locally (testing); otherwise
    // `ssh [-p port] <host> <exe> --serve`.
    bool start(const std::string& host, const std::string& exe, std::string& err);
    bool startOn(const std::string& host, int port, const std::string& exe, std::string& err);
    bool alive() const { return alive_; }
    void stop();
    // A worker's stop flag. recv() blocks inside a raw pipe read, and the
    // workers poll their stop flags only BETWEEN queue items - so a worker
    // parked in a read never saw the flag, and main()'s four joins held the
    // dead window on screen for as long as the request took (up to ssh's
    // ~45-60 s keepalive teardown on a black-holed link, unbounded for a
    // local:// peer). With this set, a blocked read gives up within one 50 ms
    // slice. Deliberately NOT a wall-clock deadline: a legitimate MEASURE over
    // a 300-frame aggregate produces no bytes for minutes and is not a fault.
    // The pointer must outlive the Session.
    void setAbort(const std::atomic<bool>* flag) { abort_ = flag; }
    // Give up a read after `seconds` with NO bytes arriving at all (0 = never,
    // the default). For the UI thread's session, where the window is not being
    // repainted while the read blocks: a preview's META + TILE is a small
    // request, and "the link died" must become an error in seconds rather than
    // waiting out ssh's ~45-60 s keepalive - or, for a local:// peer, forever.
    // NOT set on the workers: a legitimate MEASURE over a 300-frame aggregate
    // produces no bytes for minutes and is not a fault.
    void setIdleTimeout(double seconds) { idleTimeout_ = seconds; }

    bool list(const std::string& path, std::vector<Entry>& out, std::string& err);
    // Walk the subtree under `root` server-side and return every stack below
    // it (the remote openFolder). depth/maxGroups bound the walk; truncated
    // reports the cap being hit, skippedDirs the unreadable directories.
    bool scan(const std::string& root, int depth, int maxGroups,
              std::vector<ScanGroup>& out, bool& truncated, int& skippedDirs,
              std::string& err);
    // Recursive find under `root`. Pattern: glob (* and ? cross '/'), or a
    // case-insensitive substring when it has no wildcard.
    bool glob(const std::string& root, const std::string& pattern, int depth,
              int maxResults, std::vector<GlobHit>& out, bool& truncated,
              int& skippedDirs, std::string& err);
    // `read` is rp::NpyRead: 0 = let the shape rule decide (what every caller
    // wanted before #124), anything else is the user's DECLARATION and travels
    // to the peer. Both calls take it, and both refuse from peerVersion() before
    // sending when the peer predates protocol 9 - see rp::npyReReadTooOldText
    // for why that refusal cannot be left to the peer.
    // `rw` (optional) is the DECLARED GEOMETRY of a headerless file (protocol
    // 11). It is not a hint and not a fallback: for .bin/.raw/... there is no
    // other source of a shape, so a request without one is refused rather than
    // guessed at - by this client when the peer is too old (rp::rawTooOldText),
    // by the peer otherwise. Passing one for a file that states its own shape
    // is refused too, at the peer, for the reason a dropped trailer always is.
    bool meta(const std::string& path, Meta& out, std::string& err, int read = 0,
              const rp::RawWire* rw = nullptr);
    // Region [x,y,w,h] of `frame`, decimated by `step`. Returns float samples
    // (converted from the source dtype) plus the decimated dimensions. The rect
    // and the frame index are coordinates INSIDE `read`, so it must be the same
    // reading the meta() that sized this request was asked for.
    // `loss` (optional) receives what the LAST step cost. The wire carries the
    // source dtype, so pixels arrive exact and are narrowed to float32 here and
    // nowhere else (remote_proto.h rp::F32Loss) - a caller that builds a
    // FrameSource must take this, or the same file read over a link would say
    // nothing where the local door says how many samples it could not hold.
    bool tile(const std::string& path, int frame, int x, int y, int w, int h, int step,
              std::vector<float>& out, int& outW, int& outH, int& outCh, std::string& dtype,
              std::string& err, int read = 0, rp::F32Loss* loss = nullptr,
              const rp::RawWire* rw = nullptr);
    // run analysis where the data is; only the emitted results travel
    bool measure(const MeasureReq& q, MeasureResult& out, std::string& err);

    const std::string& host() const { return host_; }
    int port() const { return port_; }
    uint64_t bytesReceived() const { return rx_; }
    int peerVersion() const { return peerVersion_; }   // from HELLO; gates MEASURE

private:
    // "can this peer serve a DECLARED reading at all" - the protocol-9 gate,
    // in one place because meta() and tile() must answer it identically.
    bool readServable(int read, std::string& err) const;
    // "can THIS peer serve this file's FORMAT" - the protocol-10 gate (#148 B),
    // in one place for the same reason: meta(), tile() and measure() must all
    // refuse a .png on a v9 peer with the same sentence, and none of them may
    // let it through to come back as "not a .npy file".
    bool formatServable(const std::string& path, std::string& err) const;
    // "can this peer carry a recipe at all" - the protocol-11 gate, in one
    // place for the reason the two above are: meta() and tile() must refuse a
    // headerless file on a v10 peer with the same sentence, and neither may let
    // it through to come back as "not a .npy file".
    bool recipeServable(const std::string& path, const rp::RawWire* rw,
                        std::string& err) const;
    bool send(uint32_t type, const std::vector<uint8_t>& payload, std::string& err);
    bool recv(uint32_t& type, std::vector<uint8_t>& payload, std::string& err);

    std::string host_;
    bool alive_ = false;
    uint64_t rx_ = 0;
    int peerVersion_ = 0;
    int port_ = 0;
    const std::atomic<bool>* abort_ = nullptr;
    double idleTimeout_ = 0;
    void* impl_ = nullptr;      // platform pipe/process handles
};

// Is a TILE reply consistent with the REQUEST that produced it? A peer cannot
// legitimately return more samples than were asked for, and that invariant is
// what bounds the allocation the reply sizes. Exposed (rather than buried in
// Session::tile) so the hostile cases are testable without a hostile peer:
// without it, w=h=2^30 / ch=4 / f32 overflows the 64-bit product to exactly 0
// and matches rawBytes=0, and a self-consistent 32768x32767 u32 reply makes a
// ~4 MB deflate payload allocate 4 GB.
bool tileReplySane(uint32_t reqW, uint32_t reqH, uint32_t step,
                   uint32_t repW, uint32_t repH, uint32_t repCh,
                   uint32_t dtype, uint32_t rawBytes);

// Run one shell command on the host (or locally when host is empty) and collect
// its combined output. `stdinData` is fed to it verbatim - a script for `sh`, or
// the bytes of a file for `cat > path`. Returns false on spawn failure or when
// timeoutSec elapses (a hung git clone must not wedge the caller forever).
bool runSshCommand(const std::string& host, int port, const std::string& remoteCmd,
                   const std::string& stdinData, std::string& output, std::string& err,
                   double timeoutSec = 60.0);
// convenience: feed a script to `sh`
bool runSshScript(const std::string& host, int port, const std::string& script,
                  std::string& output, std::string& err, double timeoutSec = 60.0);

// Accepted forms, in order of standards-conformance:
//   ssh://user@host[:port]/abs/path   RFC 3986: after the colon comes a PORT,
//                                     and the path is always absolute
//   ssh://user@host[:port]/~/rel      git's extension for a home-relative path
//   user@host:~/rel , user@host:/abs  scp/rsync convention (what fingers type)
//   local://path                      run the peer here; testing
// port is 0 when unspecified.
bool parseUrl(const std::string& url, std::string& host, std::string& path, int* port = nullptr);

}  // namespace remote
