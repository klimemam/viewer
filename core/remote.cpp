#include "remote.h"
#include "remote_proto.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "miniz.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
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
    if (!CreatePipe(&childIn, &p.inW, &sa, 0) || !CreatePipe(&p.outR, &childOut, &sa, 0)) {
        err = "cannot create pipes"; return false;
    }
    SetHandleInformation(p.inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p.outR, HANDLE_FLAG_INHERIT, 0);
    std::string cmd;
    for (const auto& a : argv) {          // quote: paths have spaces
        if (!cmd.empty()) cmd += ' ';
        cmd += a.find(' ') != std::string::npos ? "\"" + a + "\"" : a;
    }
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = childIn;
    si.hStdOutput = childOut;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!ok) { err = "cannot start: " + cmd; return false; }
    CloseHandle(pi.hThread);
    p.proc = pi.hProcess;
    return true;
#else
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) { err = "cannot create pipes"; return false; }
    pid_t pid = fork();
    if (pid < 0) { err = "cannot fork"; return false; }
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
        int st = 0;
        waitpid(p.pid, &st, WNOHANG);
        kill(p.pid, SIGTERM);
        p.pid = -1;
    }
#endif
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
    bool str(std::string& s) {
        uint32_t n; if (!u32(n) || rd + n > b.size()) return false;
        s.assign((const char*)b.data() + rd, n); rd += n; return true;
    }
    bool blob(void* p, size_t n) { if (rd + n > b.size()) return false; memcpy(p, b.data() + rd, n); rd += n; return true; }
};

// ---------------------------------------------------------------- session
Session::~Session() { stop(); }

void Session::stop() {
    if (impl_) { pipeClose(*(Pipe*)impl_); delete (Pipe*)impl_; impl_ = nullptr; }
    alive_ = false;
}

bool Session::start(const std::string& host, const std::string& exe, std::string& err) {
    stop();
    host_ = host;
    Pipe* p = new Pipe();
    impl_ = p;
    std::vector<std::string> argv;
    if (host.empty()) {
        argv = { exe, "--serve" };
    } else {
        // -o BatchMode: fail fast instead of hanging on a password prompt with no
        // terminal to type it into
        argv = { "ssh", "-o", "BatchMode=yes", host, exe, "--serve" };
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
    payload.resize(h.len);
    if (h.len && !pipeRead(p, payload.data(), h.len)) {
        err = "connection lost"; alive_ = false; return false;
    }
    type = h.type;
    rx_ += sizeof h + h.len;
    return true;
}

bool Session::list(const std::string& path, std::vector<Entry>& out, std::string& err) {
    W w; w.str(path);
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_LIST, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t n = 0;
    if (!r.u32(n)) { err = "bad LIST reply"; return false; }
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        Entry e; uint32_t d = 0, sz = 0;
        if (!r.str(e.name) || !r.u32(d) || !r.u32(sz)) { err = "bad LIST reply"; return false; }
        e.dir = d != 0; e.size = sz;
        out.push_back(std::move(e));
    }
    return true;
}

bool Session::meta(const std::string& path, Meta& out, std::string& err) {
    W w; w.str(path);
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
    wr.str(path);
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

bool parseUrl(const std::string& url, std::string& host, std::string& path) {
    const std::string pre = "ssh://";
    if (url.compare(0, pre.size(), pre) != 0) return false;
    size_t slash = url.find('/', pre.size());
    if (slash == std::string::npos) return false;
    host = url.substr(pre.size(), slash - pre.size());
    path = url.substr(slash);
    return !host.empty() && path.size() > 1;
}

}  // namespace remote
