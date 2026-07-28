#include "remote.h"
#include "remote_proto.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "miniz.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX                  // windows.h's min/max macros break std::min/max
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace remote {

// ---------------------------------------------------------------- process pipe
struct Pipe {
#if defined(_WIN32)
    HANDLE inW = nullptr, outR = nullptr, proc = nullptr;
#else
    int inW = -1, outR = -1;
    pid_t pid = -1;
#endif
};

static bool spawn(Pipe& p, const std::vector<std::string>& argv, std::string& err) {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE childIn = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &p.inW, &sa, 0)) { err = "cannot create pipes"; return false; }
    if (!CreatePipe(&p.outR, &childOut, &sa, 0)) {
        CloseHandle(childIn); CloseHandle(p.inW); p.inW = nullptr;
        err = "cannot create pipes";
        return false;
    }
    SetHandleInformation(p.inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p.outR, HANDLE_FLAG_INHERIT, 0);
    std::string cmd;
    for (const auto& a : argv) {          // quote: paths have spaces
        if (!cmd.empty()) cmd += ' ';
        cmd += a.find(' ') != std::string::npos ? "\"" + a + "\"" : a;
    }
    // CreateProcessW, not A: this user's profile directory is Japanese, and the
    // ANSI entry point garbles any non-ASCII path to the binary or the data
    int wn = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wcmd((size_t)std::max(wn, 1));
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), wn);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = childIn;
    si.hStdOutput = childOut;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!ok) {
        CloseHandle(p.inW);  p.inW = nullptr;
        CloseHandle(p.outR); p.outR = nullptr;
        err = "cannot start: " + cmd;
        return false;
    }
    CloseHandle(pi.hThread);
    p.proc = pi.hProcess;
    return true;
#else
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0) { err = "cannot create pipes"; return false; }
    if (pipe(fromChild) != 0) {
        close(toChild[0]); close(toChild[1]);
        err = "cannot create pipes";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        err = "cannot fork";
        return false;
    }
    if (pid == 0) {
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    p.inW = toChild[1];
    p.outR = fromChild[0];
    p.pid = pid;
    return true;
#endif
}

static bool pipeWrite(Pipe& p, const void* buf, size_t n) {
    const uint8_t* q = (const uint8_t*)buf;
    while (n) {
#if defined(_WIN32)
        DWORD wrote = 0;
        if (!WriteFile(p.inW, q, (DWORD)std::min<size_t>(n, 1u << 20), &wrote, nullptr) || !wrote)
            return false;
#else
        ssize_t wrote = write(p.inW, q, std::min<size_t>(n, 1u << 20));
        if (wrote <= 0) return false;
#endif
        q += wrote; n -= (size_t)wrote;
    }
    return true;
}

static bool pipeRead(Pipe& p, void* buf, size_t n) {
    uint8_t* q = (uint8_t*)buf;
    while (n) {
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(p.outR, q, (DWORD)std::min<size_t>(n, 1u << 20), &got, nullptr) || !got)
            return false;
#else
        ssize_t got = read(p.outR, q, std::min<size_t>(n, 1u << 20));
        if (got <= 0) return false;
#endif
        q += got; n -= (size_t)got;
    }
    return true;
}

static void pipeClose(Pipe& p) {
#if defined(_WIN32)
    if (p.inW) { CloseHandle(p.inW); p.inW = nullptr; }
    if (p.outR) { CloseHandle(p.outR); p.outR = nullptr; }
    if (p.proc) {
        WaitForSingleObject(p.proc, 2000);
        TerminateProcess(p.proc, 0);
        CloseHandle(p.proc);
        p.proc = nullptr;
    }
#else
    if (p.inW >= 0) { close(p.inW); p.inW = -1; }
    if (p.outR >= 0) { close(p.outR); p.outR = -1; }
    if (p.pid > 0) {
        // terminate FIRST, then reap - the old order reaped a child that had not
        // exited yet, leaving a zombie for the life of the viewer
        kill(p.pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 20 && waitpid(p.pid, &st, WNOHANG) == 0; i++)
            usleep(50 * 1000);
        if (waitpid(p.pid, &st, WNOHANG) == 0) {
            kill(p.pid, SIGKILL);
            waitpid(p.pid, &st, 0);
        }
        p.pid = -1;
    }
#endif
}

static void closeWriteEnd(Pipe& p) {
#if defined(_WIN32)
    if (p.inW) { CloseHandle(p.inW); p.inW = nullptr; }
#else
    if (p.inW >= 0) { close(p.inW); p.inW = -1; }
#endif
}

// One shell command on the host, script over stdin (immune to every quoting
// layer between here and the remote sh), combined output back. This is how the
// peer gets bootstrapped into ~/.viewer without the user copying anything.
bool runSshCommand(const std::string& host, int port, const std::string& script,
                   std::string& output, std::string& err) {
    Pipe p;
    std::vector<std::string> argv;
    if (host.empty()) {
#if defined(_WIN32)
        err = "no local shell bootstrap on Windows";   // local:// needs no deploy
        return false;
#else
        argv = { "sh" };
#endif
    } else {
        argv = { "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10" };
        if (port > 0) { argv.push_back("-p"); argv.push_back(std::to_string(port)); }
        argv.push_back(host);
        argv.push_back("sh");
    }
    if (!spawn(p, argv, err)) return false;
    std::string body = script + "\n";
    bool wroteOk = pipeWrite(p, body.data(), body.size());
    closeWriteEnd(p);                     // EOF: sh runs what it has
    output.clear();
    char buf[4096];
    for (;;) {
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(p.outR, buf, sizeof buf, &got, nullptr) || !got) break;
#else
        ssize_t got = read(p.outR, buf, sizeof buf);
        if (got <= 0) break;
#endif
        output.append(buf, (size_t)got);
        if (output.size() > (1u << 20)) break;   // no script needs a MB of output
    }
    pipeClose(p);
    if (!wroteOk) { err = "could not reach the host"; return false; }
    return true;
}

// ---------------------------------------------------------------- payload codec
struct W {
    std::vector<uint8_t> b;
    void u32(uint32_t v) { b.insert(b.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
    void str(const std::string& s) { u32((uint32_t)s.size()); b.insert(b.end(), s.begin(), s.end()); }
    void blob(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};
struct R {
    const std::vector<uint8_t>& b;
    size_t rd = 0;
    explicit R(const std::vector<uint8_t>& v) : b(v) {}
    bool u32(uint32_t& v) { if (rd + 4 > b.size()) return false; memcpy(&v, b.data() + rd, 4); rd += 4; return true; }
    bool f64(double& v) { if (rd + 8 > b.size()) return false; memcpy(&v, b.data() + rd, 8); rd += 8; return true; }
    bool f32v(std::vector<float>& v, size_t n) {
        if (n > (b.size() - rd) / 4) return false;
        v.resize(n);
        memcpy(v.data(), b.data() + rd, n * 4); rd += n * 4; return true;
    }
    bool str(std::string& s) {
        uint32_t n; if (!u32(n) || rd + n > b.size()) return false;
        s.assign((const char*)b.data() + rd, n); rd += n; return true;
    }
    bool blob(void* p, size_t n) { if (rd + n > b.size()) return false; memcpy(p, b.data() + rd, n); rd += n; return true; }
};

// The peer's working directory is the login home (that is where ssh starts an
// exec command), so "~" becomes "." and "~/x" becomes "x" - the server never
// sees a literal tilde, which std::filesystem would treat as a real name.
static std::string serverPath(const std::string& p) {
    if (p == "~" || p == "~/") return ".";
    if (p.compare(0, 2, "~/") == 0) return p.substr(2);
    return p;
}

// ---------------------------------------------------------------- session
Session::~Session() { stop(); }

void Session::stop() {
    if (impl_) { pipeClose(*(Pipe*)impl_); delete (Pipe*)impl_; impl_ = nullptr; }
    alive_ = false;
}

bool Session::start(const std::string& host, const std::string& exe, std::string& err) {
    return startOn(host, 0, exe, err);
}

bool Session::startOn(const std::string& host, int port, const std::string& exe, std::string& err) {
    stop();
    host_ = host;
    port_ = port;
    Pipe* p = new Pipe();
    impl_ = p;
    std::vector<std::string> argv;
    if (host.empty()) {
        argv = { exe, "--serve" };
    } else {
        // BatchMode: fail fast instead of hanging on a password prompt with no
        // terminal. ConnectTimeout/ServerAlive: a black-holed route or a dead
        // sshd must become an error in seconds, not a UI frozen forever.
        argv = { "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                 "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3" };
        if (port > 0) { argv.push_back("-p"); argv.push_back(std::to_string(port)); }
        argv.push_back(host);
        argv.push_back(exe);
        argv.push_back("--serve");
    }
    if (!spawn(*p, argv, err)) { stop(); return false; }
    alive_ = true;
    W w; w.u32(rp::VERSION);
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_HELLO, w.b, err) || !recv(type, reply, err)) {
        err = err.empty() ? "no answer from the remote viewer (is it installed there?)" : err;
        stop();
        return false;
    }
    if (type != rp::MSG_OK) {
        R r(reply); std::string m; r.str(m);
        err = m.empty() ? "handshake rejected" : m;
        stop();
        return false;
    }
    {   // the version was always on the wire; MEASURE is gated on it
        R r(reply);
        uint32_t v = 0;
        if (r.u32(v)) peerVersion_ = (int)v;
    }
    return true;
}

bool Session::send(uint32_t type, const std::vector<uint8_t>& payload, std::string& err) {
    if (!alive_) { err = "not connected"; return false; }
    rp::Header h{ rp::MAGIC, type, (uint32_t)payload.size() };
    Pipe& p = *(Pipe*)impl_;
    if (!pipeWrite(p, &h, sizeof h) ||
        (!payload.empty() && !pipeWrite(p, payload.data(), payload.size()))) {
        err = "connection lost"; alive_ = false; return false;
    }
    return true;
}

bool Session::recv(uint32_t& type, std::vector<uint8_t>& payload, std::string& err) {
    if (!alive_) { err = "not connected"; return false; }
    Pipe& p = *(Pipe*)impl_;
    rp::Header h{};
    if (!pipeRead(p, &h, sizeof h) || h.magic != rp::MAGIC) {
        err = "connection lost"; alive_ = false; return false;
    }
    if (h.len > (512u << 20)) {   // do not allocate gigabytes on a peer's say-so
        err = "oversized reply from the peer";
        alive_ = false;
        return false;
    }
    payload.resize(h.len);
    if (h.len && !pipeRead(p, payload.data(), h.len)) {
        err = "connection lost"; alive_ = false; return false;
    }
    type = h.type;
    rx_ += sizeof h + h.len;
    return true;
}

bool Session::list(const std::string& path, std::vector<Entry>& out, std::string& err) {
    W w; w.str(serverPath(path));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_LIST, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t n = 0;
    if (!r.u32(n)) { err = "bad LIST reply"; return false; }
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        Entry e; uint32_t d = 0, lo = 0, hi = 0;
        if (!r.str(e.name) || !r.u32(d) || !r.u32(lo) || !r.u32(hi)) {
            err = "bad LIST reply"; return false;
        }
        e.dir = d != 0;
        e.size = ((uint64_t)hi << 32) | lo;
        out.push_back(std::move(e));
    }
    return true;
}

bool Session::meta(const std::string& path, Meta& out, std::string& err) {
    W w; w.str(serverPath(path));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_META, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    rp::MetaRep m{};
    if (!r.blob(&m, sizeof m)) { err = "bad META reply"; return false; }
    out.w = (int)m.w; out.h = (int)m.h; out.ch = (int)m.ch;
    out.frames = (int)m.frames;
    out.dtype = rp::dtypeName(m.dtype);
    return true;
}

// Convert the served dtype to the float samples the viewer works in. This is the
// same normalisation the local loaders do, kept in one place.
static void toFloat(const uint8_t* src, uint32_t dtype, size_t n, std::vector<float>& out) {
    out.resize(n);
    switch (dtype) {
        case rp::DT_U8:  for (size_t i = 0; i < n; i++) out[i] = (float)src[i]; break;
        case rp::DT_I8:  for (size_t i = 0; i < n; i++) out[i] = (float)((const int8_t*)src)[i]; break;
        case rp::DT_U16: for (size_t i = 0; i < n; i++) out[i] = (float)((const uint16_t*)src)[i]; break;
        case rp::DT_I16: for (size_t i = 0; i < n; i++) out[i] = (float)((const int16_t*)src)[i]; break;
        case rp::DT_U32: for (size_t i = 0; i < n; i++) out[i] = (float)((const uint32_t*)src)[i]; break;
        case rp::DT_I32: for (size_t i = 0; i < n; i++) out[i] = (float)((const int32_t*)src)[i]; break;
        case rp::DT_F32: memcpy(out.data(), src, n * 4); break;
        case rp::DT_F64: for (size_t i = 0; i < n; i++) out[i] = (float)((const double*)src)[i]; break;
        default: std::fill(out.begin(), out.end(), 0.0f); break;
    }
}

bool Session::tile(const std::string& path, int frame, int x, int y, int w, int h, int step,
                   std::vector<float>& out, int& outW, int& outH, int& outCh, std::string& dtype,
                   std::string& err) {
    W wr;
    wr.str(serverPath(path));
    rp::TileReq q{};
    q.frame = (uint32_t)std::max(0, frame);
    q.x = (uint32_t)std::max(0, x); q.y = (uint32_t)std::max(0, y);
    q.w = (uint32_t)std::max(1, w); q.h = (uint32_t)std::max(1, h);
    q.step = (uint32_t)std::max(1, step);
    q.flags = 1;                        // ask for deflate: the link is the cost
    wr.blob(&q, sizeof q);
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_TILE, wr.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    rp::TileRep rep{};
    if (!r.blob(&rep, sizeof rep)) { err = "bad TILE reply"; return false; }
    // The dims and the byte count must agree BEFORE anything indexes by dims: a
    // peer whose reply disagrees with itself (or a truncated 4 GB tile) would
    // otherwise send toFloat reading far past the buffer. Reproduced as a crash
    // by the verification agent; now it is an error message.
    size_t need = (size_t)rep.w * rep.h * rep.ch * rp::dtypeSize(rep.dtype);
    if (!rep.w || !rep.h || !rep.ch || rep.ch > 4 || rep.dtype >= rp::DT_COUNT ||
        need != rep.rawBytes) {
        err = "inconsistent tile from the peer";
        return false;
    }
    const uint8_t* blob = reply.data() + r.rd;
    size_t blobBytes = reply.size() - r.rd;
    std::vector<uint8_t> raw;
    if (rep.flags & 1) {
        raw.resize(rep.rawBytes);
        mz_ulong got = rep.rawBytes;
        if (mz_uncompress(raw.data(), &got, blob, (mz_ulong)blobBytes) != MZ_OK ||
            got != rep.rawBytes) {
            err = "tile decompression failed";
            return false;
        }
    } else {
        if (blobBytes < rep.rawBytes) { err = "short tile"; return false; }
        raw.assign(blob, blob + rep.rawBytes);
    }
    outW = (int)rep.w; outH = (int)rep.h; outCh = (int)rep.ch;
    dtype = rp::dtypeName(rep.dtype);
    toFloat(raw.data(), rep.dtype, (size_t)rep.w * rep.h * rep.ch, out);
    return true;
}

bool Session::measure(const MeasureReq& q, MeasureResult& out, std::string& err) {
    if (peerVersion_ < 2) { err = "the remote peer is too old for MEASURE (update viewer-serve)"; return false; }
    W w;
    rp::MeasureReqHead head{};
    head.op = (uint32_t)q.op;
    head.frame0 = (uint32_t)std::max(0, q.frame0);
    head.frameCount = (uint32_t)std::max(0, q.frameCount);
    head.cfaType = (uint32_t)q.cfaType;
    head.cfaPattern = (uint32_t)q.cfaPattern;
    head.black = q.black; head.white = q.white;
    head.nPaths = (uint32_t)q.paths.size();
    head.nRois = (uint32_t)q.rois.size();
    w.blob(&head, sizeof head);
    for (const auto& p : q.paths) w.str(serverPath(p));
    w.str(q.analyzer);
    w.str(q.params);
    for (const auto& r : q.rois) {
        w.u32((uint32_t)std::max(0, r.x)); w.u32((uint32_t)std::max(0, r.y));
        w.u32((uint32_t)std::max(0, r.w)); w.u32((uint32_t)std::max(0, r.h));
    }
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_MEASURE, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t loc = 0, frames = 0, nCols = 0;
    if (!r.u32(loc) || !r.u32(frames) || !r.u32(nCols) || nCols > 4096) {
        err = "bad MEASURE reply"; return false;
    }
    out.serverLoc = (int)loc;
    out.framesUsed = (int)frames;
    out.cols.assign(nCols, {});
    for (auto& col : out.cols) {
        uint32_t nItems = 0;
        if (!r.u32(nItems) || nItems > 100000) { err = "bad MEASURE reply"; return false; }
        col.resize(nItems);
        for (auto& it : col) {
            uint32_t kind = 0;
            if (!r.u32(kind) || !r.str(it.key)) { err = "bad MEASURE reply"; return false; }
            it.kind = (int)kind;
            if (kind == 0) { if (!r.f64(it.num)) { err = "bad MEASURE reply"; return false; } }
            else           { if (!r.str(it.text)) { err = "bad MEASURE reply"; return false; } }
        }
    }
    uint32_t nSeries = 0;
    if (!r.u32(nSeries) || nSeries > 100000) { err = "bad MEASURE reply"; return false; }
    out.series.resize(nSeries);
    for (auto& s : out.series) {
        uint32_t col = 0, hasX = 0, n = 0;
        if (!r.str(s.name) || !r.str(s.xLabel) || !r.str(s.yLabel) ||
            !r.u32(col) || !r.u32(hasX) || !r.u32(n)) {
            err = "bad MEASURE reply"; return false;
        }
        s.col = (int)col;
        if (hasX && !r.f32v(s.xs, n)) { err = "bad MEASURE reply"; return false; }
        if (!r.f32v(s.ys, n)) { err = "bad MEASURE reply"; return false; }
    }
    return true;
}

bool parseUrl(const std::string& url, std::string& host, std::string& path, int* port) {
    if (port) *port = 0;
    // local://<path> runs the peer on this machine: the whole remote path exercised
    // end to end, minus ssh. That is how it is tested, and how someone can check
    // the protocol without a second machine.
    const std::string loc = "local://";
    if (url.compare(0, loc.size(), loc) == 0) {
        host.clear();
        path = url.substr(loc.size());
        return !path.empty();
    }
    const std::string pre = "ssh://";
    if (url.compare(0, pre.size(), pre) == 0) {
        std::string rest = url.substr(pre.size());
        size_t cut = rest.find_first_of("/:");
        if (cut == std::string::npos) { host = rest; path = "~"; return !host.empty(); }
        host = rest.substr(0, cut);
        if (rest[cut] == ':') {
            // RFC 3986: what follows the colon is a PORT. A non-standard ssh port
            // is common enough that mis-parsing it as a path is a real bug.
            size_t slash = rest.find('/', cut + 1);
            std::string p = rest.substr(cut + 1, slash == std::string::npos
                                                     ? std::string::npos : slash - cut - 1);
            bool numeric = !p.empty() &&
                           p.find_first_not_of("0123456789") == std::string::npos;
            if (numeric) {
                if (port) *port = atoi(p.c_str());
                path = slash == std::string::npos ? "~" : rest.substr(slash);
            } else {
                path = rest.substr(cut + 1);   // tolerate the scp-ish ssh://host:~/x
            }
        } else {
            path = rest.substr(cut);
        }
        if (path.empty()) return false;
        // git's extension: /~/rel and /~user/rel mean home-relative, not a
        // directory literally called "~" at the root
        if (path.compare(0, 2, "/~") == 0) path = path.substr(1);
        return !host.empty();
    }
    // Bare scp form: host:path or user@host:path. Not a URL, but it is what the
    // muscle memory produces, and there is no ambiguity with a local path here
    // because those are handled before this is ever called.
    size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 0 && url.compare(0, 8, "local://") != 0 &&
        url.find("://") == std::string::npos &&
        !(colon == 1 && isalpha((unsigned char)url[0]))) {   // not "C:\..."
        host = url.substr(0, colon);
        path = url.substr(colon + 1);
        return !host.empty() && !path.empty();
    }
    return false;
}

}  // namespace remote
