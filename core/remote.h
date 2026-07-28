// Client side of `viewer --serve`: talks to a peer process over its stdin/stdout.
//
// The peer is normally started by ssh:
//     ssh user@host viewer --serve
// so there is nothing to install beyond the same binary, nothing listening on the
// network, and no credentials of our own - ssh owns the authentication. Passing
// an empty host starts a local peer instead, which is how this is tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
    int framesUsed = 0;
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
    struct Roi { int x = 0, y = 0, w = 0, h = 0; };
    std::vector<Roi> rois;            // empty = whole frame
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
    bool meta(const std::string& path, Meta& out, std::string& err);
    // Region [x,y,w,h] of `frame`, decimated by `step`. Returns float samples
    // (converted from the source dtype) plus the decimated dimensions.
    bool tile(const std::string& path, int frame, int x, int y, int w, int h, int step,
              std::vector<float>& out, int& outW, int& outH, int& outCh, std::string& dtype,
              std::string& err);
    // run analysis where the data is; only the emitted results travel
    bool measure(const MeasureReq& q, MeasureResult& out, std::string& err);

    const std::string& host() const { return host_; }
    int port() const { return port_; }
    uint64_t bytesReceived() const { return rx_; }
    int peerVersion() const { return peerVersion_; }   // from HELLO; gates MEASURE

private:
    bool send(uint32_t type, const std::vector<uint8_t>& payload, std::string& err);
    bool recv(uint32_t& type, std::vector<uint8_t>& payload, std::string& err);

    std::string host_;
    bool alive_ = false;
    uint64_t rx_ = 0;
    int peerVersion_ = 0;
    int port_ = 0;
    void* impl_ = nullptr;      // platform pipe/process handles
};

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
