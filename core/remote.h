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

struct Entry { std::string name; bool dir = false; uint64_t size = 0; };

struct Meta {
    int w = 0, h = 0, ch = 1, frames = 1;
    std::string dtype;
};

// One connection to one machine. Not thread-safe: requests are serialised by the
// caller (the UI thread today, a prefetch thread later).
class Session {
public:
    ~Session();
    // host empty -> run `exe --serve` locally (testing); otherwise
    // `ssh <host> <exe> --serve`.
    bool start(const std::string& host, const std::string& exe, std::string& err);
    bool alive() const { return alive_; }
    void stop();

    bool list(const std::string& path, std::vector<Entry>& out, std::string& err);
    bool meta(const std::string& path, Meta& out, std::string& err);
    // Region [x,y,w,h] of `frame`, decimated by `step`. Returns float samples
    // (converted from the source dtype) plus the decimated dimensions.
    bool tile(const std::string& path, int frame, int x, int y, int w, int h, int step,
              std::vector<float>& out, int& outW, int& outH, int& outCh, std::string& dtype,
              std::string& err);

    const std::string& host() const { return host_; }
    uint64_t bytesReceived() const { return rx_; }

private:
    bool send(uint32_t type, const std::vector<uint8_t>& payload, std::string& err);
    bool recv(uint32_t& type, std::vector<uint8_t>& payload, std::string& err);

    std::string host_;
    bool alive_ = false;
    uint64_t rx_ = 0;
    void* impl_ = nullptr;      // platform pipe/process handles
};

// "ssh://user@host/path/to/file" -> host="user@host", path="/path/to/file".
bool parseUrl(const std::string& url, std::string& host, std::string& path);

}  // namespace remote
