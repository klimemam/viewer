#include "remote.h"
#include "imagefile.h"    // peerServes: the one table the format gate reads
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
#include <poll.h>      // the timed pipe reads in runSshCommand
#include <time.h>      // clock_gettime for their deadline
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
    // Set by the owning worker so a blocked read can be given up on. See
    // Session::setAbort for why this is a flag and not a deadline.
    const std::atomic<bool>* abort = nullptr;
    double idleTimeout = 0;           // seconds with no bytes at all; 0 = never
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

// Blocking, but interruptible. runSshCommand has used the same
// PeekNamedPipe / poll slice loop since it was written ("ReadFile on a pipe
// blocks with no way out"); the protocol path did not, so a worker parked here
// could not be stopped and the quit path waited it out with the dead window
// still on screen.
static double nowSeconds();           // fwd (defined with runSshCommand)
static bool pipeRead(Pipe& p, void* buf, size_t n) {
    uint8_t* q = (uint8_t*)buf;
    const bool sliced = p.abort || p.idleTimeout > 0;
    double deadline = p.idleTimeout > 0 ? nowSeconds() + p.idleTimeout : 0;
    while (n) {
        if (p.abort && p.abort->load()) return false;
        if (deadline > 0 && nowSeconds() > deadline) return false;
#if defined(_WIN32)
        DWORD avail = 0;
        if (sliced) {                     // only pay for the poll when it can matter
            if (!PeekNamedPipe(p.outR, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (avail == 0) { Sleep(20); continue; }
        }
        DWORD got = 0;
        if (!ReadFile(p.outR, q, (DWORD)std::min<size_t>(n, 1u << 20), &got, nullptr) || !got)
            return false;
#else
        if (sliced) {
            struct pollfd pf { p.outR, POLLIN, 0 };
            int pr = poll(&pf, 1, 20);
            if (pr == 0) continue;
            if (pr < 0) return false;
        }
        ssize_t got = read(p.outR, q, std::min<size_t>(n, 1u << 20));
        if (got <= 0) return false;
#endif
        q += got; n -= (size_t)got;
        // progress resets the clock: a slow tile is not a dead link
        if (deadline > 0) deadline = nowSeconds() + p.idleTimeout;
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
// Wall clock, monotonic enough for a timeout.
static double nowSeconds() {
#if defined(_WIN32)
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

bool runSshCommand(const std::string& host, int port, const std::string& remoteCmd,
                   const std::string& stdinData, std::string& output, std::string& err,
                   double timeoutSec) {
    Pipe p;
    std::vector<std::string> argv;
    if (host.empty()) {
#if defined(_WIN32)
        err = "no local shell bootstrap on Windows";   // local:// needs no deploy
        return false;
#else
        argv = { "sh", "-c", remoteCmd };
#endif
    } else {
        argv = { "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10" };
        if (port > 0) { argv.push_back("-p"); argv.push_back(std::to_string(port)); }
        argv.push_back(host);
        argv.push_back(remoteCmd);
    }
    if (!spawn(p, argv, err)) return false;
    bool wroteOk = stdinData.empty() || pipeWrite(p, stdinData.data(), stdinData.size());
    closeWriteEnd(p);                     // EOF: the remote command runs
    output.clear();
    const double deadline = nowSeconds() + timeoutSec;
    char buf[4096];
    bool timedOut = false;
    for (;;) {
        if (nowSeconds() > deadline) { timedOut = true; break; }
#if defined(_WIN32)
        // PeekNamedPipe first: ReadFile on a pipe blocks with no way out, and a
        // hung git clone on the far side would otherwise wedge this thread until
        // the process exits.
        DWORD avail = 0;
        if (!PeekNamedPipe(p.outR, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) { Sleep(50); continue; }
        DWORD got = 0;
        if (!ReadFile(p.outR, buf, sizeof buf, &got, nullptr) || !got) break;
#else
        struct pollfd pf { p.outR, POLLIN, 0 };
        int pr = poll(&pf, 1, 50);
        if (pr == 0) continue;
        if (pr < 0) break;
        ssize_t got = read(p.outR, buf, sizeof buf);
        if (got <= 0) break;
#endif
        output.append(buf, (size_t)got);
        if (output.size() > (1u << 20)) break;   // no script needs a MB of output
    }
    pipeClose(p);
    if (timedOut) {
        err = "timed out after " + std::to_string((int)timeoutSec) + "s";
        return false;
    }
    if (!wroteOk) { err = "could not reach the host"; return false; }
    return true;
}

bool runSshScript(const std::string& host, int port, const std::string& script,
                  std::string& output, std::string& err, double timeoutSec) {
    return runSshCommand(host, port, "sh", script + "\n", output, err, timeoutSec);
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
    p->abort = abort_;                // interruptible reads for a worker's session
    p->idleTimeout = idleTimeout_;    // ...and a bounded one for the UI thread's
    impl_ = p;
    std::vector<std::string> argv;
    // --serve-readers: docs/features/remote/remote-reader-design.md §2. The peer's consent is
    // "whoever started this process said so", and over ssh the person who
    // started it is this user - who already has a shell there and could start
    // python by hand. So the client writes the flag: not a new permission, the
    // exercise of one already held. What the flag guards is the transport that
    // does NOT work that way (core/serve.cpp's handler note), where the argv
    // belongs to an operator and doing nothing leaves the gate shut.
    if (host.empty()) {
        argv = { exe, "--serve" };
        if (serveReaders_) argv.push_back("--serve-readers");
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
        if (serveReaders_) argv.push_back("--serve-readers");
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

// One v3 entry (shared by the LIST and SCAN replies).
static bool parseEntryV3(R& r, Entry& e) {
    uint32_t d = 0, lo = 0, hi = 0, mlo = 0, mhi = 0;
    if (!r.str(e.name) || !r.u32(d) || !r.u32(lo) || !r.u32(hi) ||
        !r.u32(mlo) || !r.u32(mhi))
        return false;
    e.size = ((uint64_t)hi << 32) | lo;
    e.dir = (d & rp::LE_DIR) != 0;
    e.mtime = (int64_t)(((uint64_t)mhi << 32) | mlo);
    if (d & rp::LE_META) {
        uint32_t dt = 0, nd = 0, fo = 0;
        if (!r.u32(dt) || !r.u32(nd)) return false;
        for (uint32_t& v : e.dims) if (!r.u32(v)) return false;   // always 4 on the wire
        if (!r.u32(fo)) return false;
        // The wire carries FOUR dims. A rank above that cannot be represented
        // in them, so it is not clamped to 4 - clamping printed a 5-D file into
        // the listing as a four-axis shape that the loader would then refuse by
        // name, a listing telling the user something the opener disagreed with
        // (issue #71 D5). Today no peer can send it: core/serve.cpp only sets
        // LE_META after a header parse that refuses any rank but 2, 3 or 4. So
        // this is the guard for a peer that is newer, older or wrong, and the
        // honest answer to a shape we cannot spell is to claim no shape at all
        // - Browse renders that as "-" (fmtEntryShape) and the file still
        // lists, with its name, size and mtime intact.
        e.hasMeta = nd >= 1 && nd <= 4;
        e.dtype = rp::dtypeName(dt);
        e.ndim = (int)nd;
        e.fortran = fo != 0;
    }
    if (d & rp::LE_GROUP) {
        uint32_t cnt = 0;
        if (!r.u32(cnt) || cnt > 1000000) return false;
        e.group = true;
        e.frames = cnt;
        e.members.resize(cnt);
        for (auto& m : e.members)
            if (!r.str(m)) return false;
    }
    return true;
}

bool parseListPayload(const std::vector<uint8_t>& payload, int peerVersion,
                      std::vector<Entry>& out, std::string& err) {
    R r(payload);
    uint32_t n = 0;
    if (!r.u32(n)) { err = "bad LIST reply"; return false; }
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        Entry e;
        if (peerVersion < 3) {              // v2: name/dir/size was the whole row
            uint32_t d = 0, lo = 0, hi = 0;
            if (!r.str(e.name) || !r.u32(d) || !r.u32(lo) || !r.u32(hi)) {
                err = "bad LIST reply"; return false;
            }
            e.dir = d != 0;
            e.size = ((uint64_t)hi << 32) | lo;
        } else if (!parseEntryV3(r, e)) {
            err = "bad LIST reply";
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

bool Session::list(const std::string& path, std::vector<Entry>& out, std::string& err) {
    W w; w.str(serverPath(path));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_LIST, w.b, err) || !recv(type, reply, err)) return false;
    if (type != rp::MSG_OK) { R r(reply); r.str(err); return false; }
    return parseListPayload(reply, peerVersion_, out, err);
}

bool Session::scan(const std::string& root, int depth, int maxGroups,
                   std::vector<ScanGroup>& out, bool& truncated, int& skippedDirs,
                   std::string& err) {
    if (peerVersion_ < 3) {
        err = "the remote peer is protocol " + std::to_string(peerVersion_) +
              " - File > Update remote peer enables folder scans";
        return false;
    }
    W w;
    w.str(serverPath(root));
    w.u32((uint32_t)std::max(0, depth));
    w.u32((uint32_t)std::max(0, maxGroups));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_SCAN, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t flags = 0, skipped = 0, n = 0;
    if (!r.u32(flags) || !r.u32(skipped) || !r.u32(n) || n > 1000000) {
        err = "bad SCAN reply";
        return false;
    }
    truncated = (flags & 1) != 0;
    skippedDirs = (int)skipped;
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        ScanGroup g;
        if (!r.str(g.dir) || !parseEntryV3(r, g.entry)) {
            err = "bad SCAN reply";
            return false;
        }
        out.push_back(std::move(g));
    }
    return true;
}

bool Session::glob(const std::string& root, const std::string& pattern, int depth,
                   int maxResults, std::vector<GlobHit>& out, bool& truncated,
                   int& skippedDirs, std::string& err) {
    if (peerVersion_ < 3) {
        err = "the remote peer is protocol " + std::to_string(peerVersion_) +
              " - File > Update remote peer enables recursive search";
        return false;
    }
    W w;
    w.str(serverPath(root));
    w.str(pattern);
    w.u32((uint32_t)std::max(0, depth));
    w.u32((uint32_t)std::max(0, maxResults));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_GLOB, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t flags = 0, skipped = 0, n = 0;
    if (!r.u32(flags) || !r.u32(skipped) || !r.u32(n) || n > 1000000) {
        err = "bad GLOB reply";
        return false;
    }
    truncated = (flags & 1) != 0;
    skippedDirs = (int)skipped;
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        GlobHit h;
        uint32_t d = 0;
        if (!r.str(h.rel) || !r.u32(d)) { err = "bad GLOB reply"; return false; }
        h.dir = d != 0;
        out.push_back(std::move(h));
    }
    return true;
}

// A DECLARED reading needs a peer that can serve one. Refused HERE, from the
// number the peer announced, and never by sending and hoping: the trailer a
// v8 peer does not read produces no error at all, only the native reading under
// the label of the one that was asked for (rp::npyReReadTooOldText, and the
// VERSION note in remote_proto.h at length). Shared by meta() and tile() so the
// two cannot drift into disagreeing about which peer is old enough.
bool Session::readServable(int read, std::string& err) const {
    if (read == rp::NR_NATIVE) return true;
    if (peerVersion_ >= 9) return true;
    err = rp::npyReReadTooOldText(peerVersion_);
    return false;
}

// Can THIS peer serve this file's FORMAT? Asked from the number the peer
// announced in HELLO, before anything is sent, and it is the answer to "should
// the gate be asked of the peer at connect time rather than hard-coded".
//
// It is, and this is where: peerServesName (core/ui/menus.inc) says what this
// build EXPECTS of a peer, because the Browse panel asks it while drawing rows
// with no session in hand; the peer's own protocol number says what the peer it
// actually reached can do. Only the second one can be wrong about a real
// machine, and it is only knowable here.
//
// The refusal a v9 peer would give on its own is "not a .npy file" - a sentence
// about the FILE, for a limit that belongs to the peer's build, and identical
// to what it says about a truncated .npy. That is the illegibility the VERSION
// note in remote_proto.h keeps paying for, so the client says it instead.
bool Session::formatServable(const std::string& path, std::string& err) const {
    // A file this build would not send anywhere is not this gate's business:
    // peerServesName refused it already and said why (#111).
    if (peerVersion_ >= 10 || !imagefile::peerServes(path)) return true;
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() >= 4) {
        std::string ext = name.substr(name.size() - 4);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".npy") return true;               // every peer ever served these
    }
    err = rp::pictureTooOldText(peerVersion_, name);
    return false;
}

// The protocol-11 gate. Two ways to be wrong and both are refused HERE:
//
//   a headerless file with no recipe   nothing on either end can supply a
//                                      shape, so the request is unanswerable -
//                                      and it must not reach a peer that would
//                                      say "not a .npy file" about it.
//   a headerless file, peer below 11   the peer will not read the trailer. It
//                                      would answer with a sentence about the
//                                      FILE for a limit that belongs to its
//                                      build - the illegibility this header
//                                      keeps paying to avoid.
//
// A recipe for a file that DOES state its shape is refused by the peer, not
// here: that one is a claim about the file, the peer is the side holding the
// file, and its refusal names what it actually found.
bool Session::recipeServable(const std::string& path, const rp::RawWire* rw,
                             std::string& err) const {
    if (!imagefile::isHeaderless(path)) return true;
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (peerVersion_ < 11) { err = rp::rawTooOldText(peerVersion_, name); return false; }
    if (!rw) { err = imagefile::peerRefusal(path); return false; }
    return true;
}

// An array inside a materialisation needs a peer that made one. Refused HERE,
// from the number, and for the reason at rp::readerTooOldText: an older peer
// does not refuse the trailer - it never reads it, opens the path it was given
// and answers with real pixels under the wrong name.
//
// WHICH number depends on what issued the key, and that is the whole of why
// KeyedRef::kind exists. The two are different builds and different fixes: a
// v12 peer runs readers and cannot list a container, so telling its user "a
// reader cannot run on this peer" about a native .npz would send them to fix
// something that is not broken.
bool Session::keyedServable(const KeyedRef& rd, const std::string& name,
                            std::string& err) const {
    const int need = rd.kind == KeyedRef::FromNpz ? 13 : 12;
    if (peerVersion_ >= need) return true;
    err = rd.kind == KeyedRef::FromNpz ? rp::npzTooOldText(peerVersion_, name)
                                       : rp::readerTooOldText(peerVersion_, name);
    return false;
}

// ...and the same question asked of the file itself, before a SCAN is sent
// (issue #217). A v12 peer answers MSG_NPZ_SCAN with "unknown request", the
// same sentence a typo gets, so the client says which mismatch it is.
bool Session::npzServable(const std::string& path, std::string& err) const {
    if (peerVersion_ >= 13) return true;
    const size_t slash = path.find_last_of("/\\");
    err = rp::npzTooOldText(peerVersion_,
                            slash == std::string::npos ? path : path.substr(slash + 1));
    return false;
}

// The optional blocks behind the reading, written the way the peer reads them
// (rp::ReqTrailer). One place, because meta() and tile() have to agree byte for
// byte about where a recipe stops and a key begins.
static void putTrailers(W& w, int peerVersion, const rp::RawWire* rw,
                        const remote::KeyedRef* rd) {
    if (peerVersion >= 12) {
        uint32_t f = 0;
        if (rw) f |= rp::RQ_RAW_RECIPE;
        if (rd) f |= rp::RQ_KEYED;
        w.u32(f);                          // ALWAYS, even 0: see rp::ReqTrailer
        if (rw) w.blob(rw, sizeof *rw);
        if (rd) { w.str(rd->key); w.u32((uint32_t)std::max(0, rd->node)); }
        return;
    }
    // Protocol 11 and below: one optional block, read by "if bytes remain".
    // A KeyedRef never reaches here - keyedServable refused first.
    if (rw && peerVersion >= 11) w.blob(rw, sizeof *rw);
}

// What a request is ABOUT, at the door. A reader's node is not a file, so the
// three file gates do not apply to it and the path is not sent at all: a key
// names one entry in the peer's own cache and must not become a second way to
// ask the peer to open a path.
bool Session::meta(const std::string& path, Meta& out, std::string& err, int read,
                   const rp::RawWire* rw, const KeyedRef* rd) {
    if (rd) {
        if (!keyedServable(*rd, path, err)) return false;
    } else if (!readServable(read, err) || !formatServable(path, err) ||
               !recipeServable(path, rw, err)) {
        return false;
    }
    W w; w.str(rd ? std::string() : serverPath(path));
    if (peerVersion_ >= 9) w.u32((uint32_t)read);
    putTrailers(w, peerVersion_, rw, rd);
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
    out.readFellBack = (m.flags & rp::MR_READ_FELL_BACK) != 0;
    out.shape.clear();
    // The declared shape, when the peer said it is there. A rank outside 2..4
    // is dropped rather than kept: the client's own §3.3 machinery reads no
    // other rank, and a shape it cannot compute a menu from would put a "read
    // as" line on screen with nothing behind it - the exact half-feature #124
    // says is worse than the blank.
    if (m.flags & rp::MR_SHAPE) {
        uint32_t nd = 0, d[4] = { 0, 0, 0, 0 };
        bool got = r.u32(nd);
        for (int i = 0; i < 4 && got; i++) got = r.u32(d[i]);
        if (!got) { err = "bad META reply"; return false; }
        if (nd >= 2 && nd <= 4)
            for (uint32_t i = 0; i < nd; i++) out.shape.push_back((int64_t)d[i]);
    }
    return true;
}

// Convert the served dtype to the float samples the viewer works in. This is the
// same normalisation the local loaders do, kept in one place.
//
// It is also where the LINK's exactness ends. remote_proto.h's opening
// paragraph says pixels cross "in the source dtype (so pixel values stay
// exact)" - true, and true right up to this function, which then throws the
// exactness away to satisfy FrameSource::data. So the census the local decoder
// keeps has to be kept here too, or a u32 file would name its unrepresentable
// samples when opened from disk and say nothing when opened from a peer.
static void toFloat(const uint8_t* src, uint32_t dtype, size_t n, std::vector<float>& out,
                    rp::F32Loss* loss = nullptr) {
    out.resize(n);
    switch (dtype) {
        case rp::DT_U8:  for (size_t i = 0; i < n; i++) out[i] = (float)src[i]; break;
        case rp::DT_I8:  for (size_t i = 0; i < n; i++) out[i] = (float)((const int8_t*)src)[i]; break;
        case rp::DT_U16: for (size_t i = 0; i < n; i++) out[i] = (float)((const uint16_t*)src)[i]; break;
        case rp::DT_I16: for (size_t i = 0; i < n; i++) out[i] = (float)((const int16_t*)src)[i]; break;
        // u4 / i4 / f8: the three that do not fit. (double) is exact for every
        // uint32_t and int32_t, so observe() compares against the peer's value
        // and not a second approximation of it.
        case rp::DT_U32: for (size_t i = 0; i < n; i++) {
                             double e = (double)((const uint32_t*)src)[i];
                             if (loss) loss->observe(e);
                             out[i] = (float)e;
                         } break;
        case rp::DT_I32: for (size_t i = 0; i < n; i++) {
                             double e = (double)((const int32_t*)src)[i];
                             if (loss) loss->observe(e);
                             out[i] = (float)e;
                         } break;
        case rp::DT_F32: memcpy(out.data(), src, n * 4); break;
        // f16 is the one narrow type that is NOT a loss: every half is exactly
        // a float, which is why it may cross the wire as itself (protocol 10)
        // instead of being widened on the peer and losing its NAME on the way.
        case rp::DT_F16: { for (size_t i = 0; i < n; i++)
                               out[i] = rp::halfToFloat(((const uint16_t*)src)[i]); } break;
        case rp::DT_F64: for (size_t i = 0; i < n; i++) {
                             double e = ((const double*)src)[i];
                             if (loss) loss->observe(e);
                             out[i] = (float)e;
                         } break;
        default: std::fill(out.begin(), out.end(), 0.0f); break;
    }
}

bool tileReplySane(uint32_t reqW, uint32_t reqH, uint32_t step,
                   uint32_t repW, uint32_t repH, uint32_t repCh,
                   uint32_t dtype, uint32_t rawBytes) {
    if (!step) return false;
    // FIRST against the REQUEST, which the caller holds: the peer cannot
    // return more samples than were asked for. That single invariant bounds
    // every factor below, which is what makes the self-consistency check
    // meaningful instead of defeatable by 64-bit overflow.
    uint32_t maxW = (uint32_t)(((uint64_t)reqW + step - 1) / step);
    uint32_t maxH = (uint32_t)(((uint64_t)reqH + step - 1) / step);
    if (repW > maxW || repH > maxH) return false;
    if (!repW || !repH || !repCh || repCh > 4 || dtype >= rp::DT_COUNT) return false;
    // ...then the dims and the byte count must agree BEFORE anything indexes
    // by dims, or toFloat reads far past the buffer.
    return (uint64_t)repW * repH * repCh * rp::dtypeSize(dtype) == (uint64_t)rawBytes;
}

bool Session::tileBytes(const std::string& path, int frame, int x, int y, int w, int h,
                        int step, std::vector<uint8_t>& raw, int& outW, int& outH,
                        int& outCh, uint32_t& dtype, std::string& err, int read,
                        const rp::RawWire* rw, const KeyedRef* rd) {
    if (rd) {
        if (!keyedServable(*rd, path, err)) return false;
    } else if (!readServable(read, err) || !formatServable(path, err) ||
               !recipeServable(path, rw, err)) {
        return false;
    }
    W wr;
    wr.str(rd ? std::string() : serverPath(path));
    rp::TileReq q{};
    q.frame = (uint32_t)std::max(0, frame);
    q.x = (uint32_t)std::max(0, x); q.y = (uint32_t)std::max(0, y);
    q.w = (uint32_t)std::max(1, w); q.h = (uint32_t)std::max(1, h);
    q.step = (uint32_t)std::max(1, step);
    q.flags = 1;                        // ask for deflate: the link is the cost
    wr.blob(&q, sizeof q);
    if (peerVersion_ >= 9) wr.u32((uint32_t)read);
    // Appended after the reading, in the order handleTile reads them. A peer
    // below 11 never gets a recipe and one below 12 never gets a reader key -
    // the gates above refused first, so the older wire does not move by a byte.
    putTrailers(wr, peerVersion_, rw, rd);
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_TILE, wr.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    rp::TileRep rep{};
    if (!r.blob(&rep, sizeof rep)) { err = "bad TILE reply"; return false; }
    // Validated against the REQUEST as well as against itself: see
    // tileReplySane. Reproduced as a process kill by the verification agent
    // (std::length_error out of toFloat, uncaught on every client path); now
    // it is an error message.
    if (!tileReplySane(q.w, q.h, q.step, rep.w, rep.h, rep.ch, rep.dtype, rep.rawBytes)) {
        err = "inconsistent tile from the peer";
        return false;
    }
    const uint8_t* blob = reply.data() + r.rd;
    size_t blobBytes = reply.size() - r.rd;
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
    dtype = rep.dtype;
    return true;
}

bool Session::tile(const std::string& path, int frame, int x, int y, int w, int h, int step,
                   std::vector<float>& out, int& outW, int& outH, int& outCh, std::string& dtype,
                   std::string& err, int read, rp::F32Loss* loss,
                   const rp::RawWire* rw, const KeyedRef* rd) {
    std::vector<uint8_t> raw;
    uint32_t dt = 0;
    if (!tileBytes(path, frame, x, y, w, h, step, raw, outW, outH, outCh, dt, err,
                   read, rw, rd))
        return false;
    dtype = rp::dtypeName(dt);
    toFloat(raw.data(), dt, (size_t)outW * outH * outCh, out, loss);
    return true;
}

bool Session::readerRun(const std::string& peerPath, const std::string& func,
                        const std::string files[3], ReaderRun& out, std::string& err) {
    out = ReaderRun{};
    {
        KeyedRef probe;                 // the reader family's number, asked once
        if (!keyedServable(probe, peerPath, err)) return false;
    }
    W w;
    w.str(serverPath(peerPath));
    w.str(func);
    w.u32(3);
    for (int i = 0; i < 3; i++) {
        w.str(rp::READER_RUN_FILES[i]);
        w.str(files[i]);
    }
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_READER_RUN, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t outcome = 0;
    if (!r.u32(outcome) || !r.str(out.err) || !r.str(out.stderrText) ||
        !r.str(out.provenance) || !r.str(out.key) || !r.str(out.header)) {
        err = "bad READER_RUN reply";
        return false;
    }
    out.outcome = (int)outcome;
    return true;
}

bool Session::npzScan(const std::string& path, NpzScan& out, std::string& err) {
    out = NpzScan{};
    if (!npzServable(path, err)) return false;
    W w;
    w.str(serverPath(path));
    std::vector<uint8_t> reply;
    uint32_t type = 0;
    if (!send(rp::MSG_NPZ_SCAN, w.b, err) || !recv(type, reply, err)) return false;
    R r(reply);
    if (type != rp::MSG_OK) { r.str(err); return false; }
    uint32_t kind = 0, cver = 0, n = 0;
    if (!r.str(out.key) || !r.u32(kind) || !r.u32(cver) || !r.u32(n)) {
        err = "bad NPZ_SCAN reply";
        return false;
    }
    out.kind = (int)kind;
    out.containerVersion = (int)cver;
    // A member costs at least a length word per field, so a count the payload
    // cannot possibly hold is refused before it reserves anything - the same
    // discipline tileReplySane keeps for a tile's dimensions.
    if ((uint64_t)n * 16 > reply.size()) { err = "bad NPZ_SCAN reply"; return false; }
    out.members.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        nz::Fact f;
        uint32_t lo = 0, hi = 0, node = 0, flags = 0, nb = 0;
        if (!r.str(f.name) || !r.u32(lo) || !r.u32(hi) || !r.u32(node) ||
            !r.str(f.err) || !r.u32(flags) || !r.u32(nb)) {
            err = "bad NPZ_SCAN reply";
            return false;
        }
        if (nb > reply.size() - r.rd) { err = "bad NPZ_SCAN reply"; return false; }
        f.usize = ((uint64_t)hi << 32) | lo;
        f.entry = (size_t)node;
        f.whole = (flags & 1) != 0;
        f.bytes.assign(reply.begin() + (ptrdiff_t)r.rd,
                       reply.begin() + (ptrdiff_t)(r.rd + nb));
        r.rd += nb;
        out.members.push_back(std::move(f));
    }
    return true;
}

bool Session::measure(const MeasureReq& q, MeasureResult& out, std::string& err) {
    if (peerVersion_ < 2) { err = "the remote peer is too old for MEASURE (update viewer-serve)"; return false; }
    // A subject that is one array inside a materialisation (protocol 14). The
    // gate is HERE and not at the peer for this header's sharpest reason: a v13
    // peer never reads the key, and a request that also carried an origin path
    // would come back as a real statistic over the whole container. So the
    // client refuses from the number, and sends no path when it does send.
    if (q.hasKeyed) {
        if (peerVersion_ < 14) { err = rp::measureKeyedTooOldText(peerVersion_); return false; }
        // Two subjects is not a request. A recipe declares the geometry of a
        // FILE, and what a reader or a container materialised declares its own;
        // the peer refuses the pair for META and TILE in the same words, and
        // refusing it here as well keeps the client from ever writing it.
        if (q.hasRecipe) {
            err = "a raw recipe does not apply to an array inside a materialisation";
            return false;
        }
        // ...and two subjects is not a request either. The wire cannot say it
        // (nPaths goes to 0 below), so a caller that filled both would have had
        // its file list SILENTLY DROPPED - which is the shape of bug that looks
        // like a correct answer to a question nobody asked. The peer refuses
        // this pair in these words; so does this end, before it is written.
        if (!q.paths.empty() || !q.roles.empty()) {
            err = "a keyed measurement addresses one array, not a list of files";
            return false;
        }
        // The ops whose grammar cannot say "this one array". A set names
        // several stacks and the plugin ops name a file in the sentences they
        // write about it; both are refused BY NAME rather than measured over
        // some other bytes. The measurement that IS carried is the stack
        // aggregate, which is what a document from a peer is opened to get.
        if (q.op != rp::MOP_TEMPORAL_STATS && q.op != rp::MOP_FRAME_ROI_STATS) {
            err = "this measurement addresses files, and these pixels are one array "
                  "inside something the peer materialised - so it cannot be run over "
                  "there yet. The temporal and per-frame statistics can; anything "
                  "else runs here, over the frames as they arrive.";
            return false;
        }
    }
    // MEASURE is the half #148 B exists for ("measure where the data is"), so
    // it gets the format gate too - and it gets it on EVERY path, because one
    // path per stack is what a set fold sends. A peer that could show a TIFF
    // and not measure it would be the same two-answers defect one level down.
    for (const auto& p : q.paths)
        if (!formatServable(p, err)) return false;
    // A headerless stack: the same two ways to be wrong the open door refuses,
    // asked once for the request rather than per path (one recipe, one stack).
    if (!q.paths.empty() && !recipeServable(q.paths.front(),
                                            q.hasRecipe ? &q.recipe : nullptr, err))
        return false;
    // ...and the ops this grammar cannot carry a recipe FOR. Refused before it
    // is sent, and by name, because the peer's own answer would be "not a .npy
    // file" about a file that is not the problem. The plugin ops put their own
    // block behind the rois and a set names several stacks - one recipe per
    // request cannot say either, and approximating it is how a measurement gets
    // made over the wrong bytes and looks perfectly normal.
    if (q.hasRecipe && q.op != rp::MOP_TEMPORAL_STATS && q.op != rp::MOP_FRAME_ROI_STATS) {
        err = "this measurement names several stacks or carries its own block, and "
              "this build sends ONE raw recipe per request - so a headerless file "
              "cannot be measured this way yet. Run it on copied files, or use the "
              "per-frame and temporal statistics, which do carry a recipe.";
        return false;
    }
    for (const auto& r : q.roles)                 // MOP_SET_FOLD ignores q.paths
        for (const auto& p : r.paths)
            if (!formatServable(p, err)) return false;
    // Refused HERE, from the number, and never by sending and reading back
    // "unknown measure op". §10's whole content is that a refusal must say
    // which mismatch it is: an old peer and a parity mismatch are different
    // problems with different fixes, and the one thing neither may become is a
    // quiet local run.
    if (q.op == rp::MOP_PLUGIN_ANALYZE && peerVersion_ < 7) {
        err = "the remote peer is too old for plugin analysis: it speaks protocol " +
              std::to_string(peerVersion_) + ", MOP_PLUGIN_ANALYZE needs 7 "
              "(update viewer-serve). Nothing was measured - a plugin result is not "
              "computed here and labelled as the peer's.";
        return false;
    }
    // ...and the same shape for the set fold. "unknown measure op" from a v7
    // peer is a refusal about a TYPO as much as about an age, and the one thing
    // it must never become is a quiet local fold over pixels this machine would
    // have had to fetch first.
    if (q.op == rp::MOP_SET_FOLD && peerVersion_ < 8) {
        err = "the remote peer is too old for set analysis: it speaks protocol " +
              std::to_string(peerVersion_) + ", MOP_SET_FOLD needs 8 "
              "(update viewer-serve). Nothing was folded - a set result is not "
              "computed here and labelled as the peer's.";
        return false;
    }
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
    // The set fold's paths come out of the ROLES, so that the flat list and the
    // per-role counts cannot drift apart: one loop writes both.
    if (q.op == rp::MOP_SET_FOLD) {
        size_t total = 0;
        for (const auto& r : q.roles) total += r.paths.size();
        head.nPaths = (uint32_t)total;
    }
    if (q.hasRecipe) head.flags |= rp::MRF_RAW_RECIPE;
    // ...and the subject that is not a path. nPaths goes to 0 and NO string is
    // written: the key names an entry in the peer's own cache, and a request
    // that also named a file would be asking two questions at once (and would
    // be answerable, wrongly, by a peer that reads only the first).
    if (q.hasKeyed) { head.flags |= rp::MRF_KEYED; head.nPaths = 0; }
    w.blob(&head, sizeof head);
    // A keyed request writes no path at all - its subject is the trailer.
    if (!q.hasKeyed) {
        if (q.op == rp::MOP_SET_FOLD) {
            for (const auto& r : q.roles)
                for (const auto& p : r.paths) w.str(serverPath(p));
        } else {
            for (const auto& p : q.paths) w.str(serverPath(p));
        }
    }
    w.str(q.analyzer);
    w.str(q.params);
    for (const auto& r : q.rois) {
        w.u32((uint32_t)std::max(0, r.x)); w.u32((uint32_t)std::max(0, r.y));
        w.u32((uint32_t)std::max(0, r.w)); w.u32((uint32_t)std::max(0, r.h));
    }
    // the recipe, right after the rois - the position handleMeasure reads it
    // from, and declared by the head's bit rather than by "bytes remain",
    // because two other blocks already live back here.
    if (q.hasRecipe) w.blob(&q.recipe, sizeof q.recipe);
    // ...and the key behind it, in BIT ORDER, which is the same rule the META /
    // TILE trailer keeps (rp::ReqTrailer). Two optional blocks and one "if
    // bytes remain" is how a key gets read as a geometry.
    if (q.hasKeyed) {
        w.str(q.keyed.key);
        w.u32((uint32_t)std::max(0, q.keyed.node));
    }
    // the parity block, last, so that the three older ops send the same bytes
    // they always sent (remote_proto.h)
    if (q.op == rp::MOP_PLUGIN_ANALYZE) {
        w.str(q.analyzerVersion);
        w.u32((uint32_t)q.target);
    }
    // the role block, in the same place and for the same reason
    if (q.op == rp::MOP_SET_FOLD) {
        w.str(q.foldForm);
        w.u32((uint32_t)q.join);
        w.u32((uint32_t)q.roles.size());
        for (const auto& r : q.roles) {
            w.str(r.role);
            w.u32((uint32_t)r.paths.size());
            w.u32((uint32_t)std::max(0, r.frame0));
            w.u32((uint32_t)std::max(0, r.frameCount));
        }
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
    if (q.op == rp::MOP_PLUGIN_ANALYZE || q.op == rp::MOP_SET_FOLD) {
        uint32_t expected = 0;
        if (!r.str(out.provName) || !r.str(out.provVersion) || !r.str(out.provFile) ||
            !r.str(out.provPath) || !r.u32(expected)) {
            // A result with no provenance is refused rather than shown: the op
            // exists to make "which build measured this" answerable, so a reply
            // that cannot answer it has failed at the thing it was for.
            err = q.op == rp::MOP_SET_FOLD
                      ? "the peer answered a set fold without provenance"
                      : "the peer answered a plugin analysis without provenance";
            return false;
        }
        out.expected = (int)expected;
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
