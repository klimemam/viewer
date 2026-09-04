// core/videowrite.cpp — the two lossless video sinks (issue #253).
// 担当: ImageView/export.  See core/videowrite.h for WHY there are two.
#include "videowrite.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace videowrite {
namespace {

std::filesystem::path fsPath(const std::string& utf8) {
    return std::filesystem::u8path(utf8);
}

void removeQuietly(const std::string& utf8) {
    std::error_code ec;
    std::filesystem::remove(fsPath(utf8), ec);
}

// ---- little-endian scalars, written by hand -------------------------------
// RIFF is little-endian on every platform that reads it, so these convert
// rather than memcpy: a big-endian host must produce the same file.
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);  p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
}
void putTag(uint8_t* p, const char* t) { memcpy(p, t, 4); }

// ---- the AVI 1.0 header, at FIXED offsets ---------------------------------
// Written once up front with placeholders and patched in finish(): the frame
// count is not known until the last frame has been written, and this export
// can be stopped at any frame. Laying the header out at compile-time constants
// rather than remembering seek positions means the patch sites and the
// selftest's RIFF parser are reading the same map.
//
//   0   RIFF <size*> 'AVI '
//   12  LIST 192 'hdrl'
//   24    avih 56  ............................ dwTotalFrames* at 48
//   88    LIST 116 'strl'
//   100     strh 56  .......................... dwLength* at 140
//   164     strf 40 (BITMAPINFOHEADER)
//   212  LIST <size*> 'movi'  ................. data starts at 224
//   ...  '00db' <size> <frame> [pad to even]
//   ...  idx1 <16*n> <entries>
// (*) patched by finish()
enum : uint32_t {
    OFF_RIFF_SIZE   = 4,
    OFF_AVIH        = 32,      // first field of the MainAVIHeader
    OFF_TOTALFRAMES = 48,
    OFF_STRH        = 108,     // first field of the AVIStreamHeader
    OFF_STRH_LENGTH = 140,
    OFF_STRF        = 172,     // first field of the BITMAPINFOHEADER
    OFF_MOVI_SIZE   = 216,
    OFF_MOVI_FOURCC = 220,     // idx1 offsets are measured from HERE
    HEADER_BYTES    = 224
};

// AVI's DIB rows are 4-byte aligned, like every other DIB. For a width whose
// w*3 is already a multiple of 4 (every multiple of 4 px) this is w*3 exactly.
uint32_t aviStride(int w) { return (uint32_t)(((w * 3) + 3) & ~3); }

class AviWriter final : public VideoSink {
public:
    AviWriter(const std::string& path, int w, int h, double fps)
        : path_(path), w_(w), h_(h), fps_(fps) {}
    ~AviWriter() override { if (f_.is_open()) f_.close(); }

    bool start(std::string& why) {
        f_.open(fsPath(path_), std::ios::binary | std::ios::trunc);
        if (!f_) { why = "cannot write " + path_; return false; }
        uint8_t hdr[HEADER_BYTES];
        memset(hdr, 0, sizeof hdr);
        const uint32_t frameBytes = aviFrameBytes(w_, h_);
        // rate/scale is a RATIO, so an integer fps is exact and 29.97 is
        // 29970/1000 rather than a rounded 30. The selftest reads it back and
        // divides, so this is the one place the number can be got wrong.
        uint32_t scale = 1, rate = 1;
        {
            double r = fps_;
            if (fabs(r - floor(r + 0.5)) < 1e-9 && r < 1e9) { scale = 1; rate = (uint32_t)(r + 0.5); }
            else { scale = 1000; rate = (uint32_t)(r * 1000.0 + 0.5); }
            if (rate == 0) { scale = 1; rate = 1; }
        }
        putTag(hdr + 0, "RIFF");  put32(hdr + 4, 0);            // patched
        putTag(hdr + 8, "AVI ");
        putTag(hdr + 12, "LIST"); put32(hdr + 16, 192);
        putTag(hdr + 20, "hdrl");
        putTag(hdr + 24, "avih"); put32(hdr + 28, 56);
        put32(hdr + OFF_AVIH + 0,  (uint32_t)(1000000.0 / fps_ + 0.5));  // dwMicroSecPerFrame
        {   // dwMaxBytesPerSec is advisory, but a double that does not fit a
            // uint32_t is undefined behaviour on conversion, not a big number.
            const double bps = (double)frameBytes * fps_;
            put32(hdr + OFF_AVIH + 4, bps >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)bps);
        }
        put32(hdr + OFF_AVIH + 8,  0);                                   // dwPaddingGranularity
        put32(hdr + OFF_AVIH + 12, 0x10);                                // AVIF_HASINDEX
        put32(hdr + OFF_AVIH + 16, 0);                                   // dwTotalFrames (patched)
        put32(hdr + OFF_AVIH + 20, 0);                                   // dwInitialFrames
        put32(hdr + OFF_AVIH + 24, 1);                                   // dwStreams
        put32(hdr + OFF_AVIH + 28, frameBytes);                          // dwSuggestedBufferSize
        put32(hdr + OFF_AVIH + 32, (uint32_t)w_);
        put32(hdr + OFF_AVIH + 36, (uint32_t)h_);
        putTag(hdr + 88, "LIST"); put32(hdr + 92, 116);
        putTag(hdr + 96, "strl");
        putTag(hdr + 100, "strh"); put32(hdr + 104, 56);
        putTag(hdr + OFF_STRH + 0, "vids");
        putTag(hdr + OFF_STRH + 4, "DIB ");                              // fccHandler
        put32(hdr + OFF_STRH + 8, 0);                                    // dwFlags
        put16(hdr + OFF_STRH + 12, 0);                                   // wPriority
        put16(hdr + OFF_STRH + 14, 0);                                   // wLanguage
        put32(hdr + OFF_STRH + 16, 0);                                   // dwInitialFrames
        put32(hdr + OFF_STRH + 20, scale);
        put32(hdr + OFF_STRH + 24, rate);
        put32(hdr + OFF_STRH + 28, 0);                                   // dwStart
        put32(hdr + OFF_STRH + 32, 0);                                   // dwLength (patched)
        put32(hdr + OFF_STRH + 36, frameBytes);                          // dwSuggestedBufferSize
        put32(hdr + OFF_STRH + 40, 0xFFFFFFFFu);                         // dwQuality: default
        put32(hdr + OFF_STRH + 44, 0);                                   // dwSampleSize
        put16(hdr + OFF_STRH + 48, 0); put16(hdr + OFF_STRH + 50, 0);    // rcFrame left/top
        // rcFrame is a 16-bit rectangle and the frame may be wider than that.
        // It is advisory (strf carries the real dimensions), so it saturates.
        put16(hdr + OFF_STRH + 52, (uint16_t)(w_ > 65535 ? 65535 : w_));
        put16(hdr + OFF_STRH + 54, (uint16_t)(h_ > 65535 ? 65535 : h_));
        putTag(hdr + 164, "strf"); put32(hdr + 168, 40);
        put32(hdr + OFF_STRF + 0,  40);                                  // biSize
        put32(hdr + OFF_STRF + 4,  (uint32_t)w_);                        // biWidth
        put32(hdr + OFF_STRF + 8,  (uint32_t)h_);                        // biHeight > 0 = bottom-up
        put16(hdr + OFF_STRF + 12, 1);                                   // biPlanes
        put16(hdr + OFF_STRF + 14, 24);                                  // biBitCount
        put32(hdr + OFF_STRF + 16, 0);                                   // biCompression = BI_RGB
        put32(hdr + OFF_STRF + 20, frameBytes);                          // biSizeImage
        putTag(hdr + 212, "LIST"); put32(hdr + OFF_MOVI_SIZE, 0);        // patched
        putTag(hdr + OFF_MOVI_FOURCC, "movi");
        f_.write((const char*)hdr, sizeof hdr);
        if (!f_) { why = "cannot write " + path_; return false; }
        row_.resize(aviStride(w_), 0);
        return true;
    }

    bool writeFrame(const uint8_t* rgb, std::string& why) override {
        if (failed_) { why = why_; return false; }
        const uint32_t bytes = aviFrameBytes(w_, h_);
        const uint32_t stride = aviStride(w_);
        uint8_t hdr[8];
        putTag(hdr, "00db"); put32(hdr + 4, bytes);
        f_.write((const char*)hdr, 8);
        // Bottom-up, and BGR: a DIB's first row is the image's LAST, and its
        // channel order is blue first. Doing it here rather than in the caller
        // is what keeps every sink's input the one thing renderDocRGBA made.
        for (int y = h_ - 1; y >= 0; y--) {
            const uint8_t* src = rgb + (size_t)y * w_ * 3;
            for (int x = 0; x < w_; x++) {
                row_[x * 3 + 0] = src[x * 3 + 2];
                row_[x * 3 + 1] = src[x * 3 + 1];
                row_[x * 3 + 2] = src[x * 3 + 0];
            }
            f_.write((const char*)row_.data(), (std::streamsize)stride);
        }
        if (bytes & 1) { const char pad = 0; f_.write(&pad, 1); }   // RIFF is WORD aligned
        if (!f_) return fail("writing " + path_ + " failed (disk full?)", why);
        // The index entry, whose offset is measured from the 'movi' fourcc -
        // so the first frame's is 4, not its absolute position in the file.
        idx_.push_back(HEADER_BYTES - OFF_MOVI_FOURCC +
                       (uint32_t)frames_ * (8 + bytes + (bytes & 1)));
        frames_++;
        return true;
    }

    bool finish(std::string& why) override {
        if (failed_) { why = why_; return false; }
        if (frames_ == 0) return fail("no frames were written", why);
        const uint32_t bytes = aviFrameBytes(w_, h_);
        {   // idx1: one 16-byte entry per frame, all key frames (uncompressed)
            std::vector<uint8_t> idx(8 + (size_t)idx_.size() * 16);
            putTag(idx.data(), "idx1");
            put32(idx.data() + 4, (uint32_t)idx_.size() * 16);
            for (size_t i = 0; i < idx_.size(); i++) {
                uint8_t* e = idx.data() + 8 + i * 16;
                putTag(e, "00db");
                put32(e + 4, 0x10);          // AVIIF_KEYFRAME
                put32(e + 8, idx_[i]);
                put32(e + 12, bytes);
            }
            f_.write((const char*)idx.data(), (std::streamsize)idx.size());
        }
        const uint64_t total = (uint64_t)f_.tellp();
        auto patch = [&](uint32_t at, uint32_t v) {
            uint8_t b[4]; put32(b, v);
            f_.seekp((std::streamoff)at, std::ios::beg);
            f_.write((const char*)b, 4);
        };
        const uint32_t movi = 4 + (uint32_t)frames_ * (8 + bytes + (bytes & 1));
        patch(OFF_RIFF_SIZE,   (uint32_t)(total - 8));
        patch(OFF_TOTALFRAMES, (uint32_t)frames_);
        patch(OFF_STRH_LENGTH, (uint32_t)frames_);
        patch(OFF_MOVI_SIZE,   movi);
        f_.flush();
        if (!f_) return fail("closing " + path_ + " failed", why);
        f_.close();
        return true;
    }

    void abort() override {
        if (f_.is_open()) f_.close();
        removeQuietly(path_);
    }
    int framesWritten() const override { return frames_; }

private:
    bool fail(const std::string& m, std::string& why) {
        failed_ = true; why_ = m; why = m; return false;
    }
    std::string path_, why_;
    int w_ = 0, h_ = 0;
    double fps_ = 30;
    int frames_ = 0;
    bool failed_ = false;
    std::ofstream f_;
    std::vector<uint8_t> row_;
    std::vector<uint32_t> idx_;
};

// ---- ffmpeg on the other end of a pipe ------------------------------------
// NOT _popen on Windows. The CRT's own documentation says _popen returns an
// invalid stream when the calling program has no console, and this one gives
// its console away at startup (dropOwnConsole in core/main.cpp) - so the pipe
// would either fail or hang, and a GUI process would flash a console window at
// the user besides. CreateProcess with an inherited pipe handle and
// CREATE_NO_WINDOW is the same three steps done in the API that supports them.
// POSIX has no such problem and popen() is exactly right there.
class FfmpegPipe final : public VideoSink {
public:
    FfmpegPipe(const std::string& path, int w, int h, double fps)
        : path_(path), w_(w), h_(h), fps_(fps) {}
    ~FfmpegPipe() override { closePipe(); }

    bool start(const std::string& exe, std::string& why) {
        char fps[64];
        snprintf(fps, sizeof fps, "%.6g", fps_);
        char size[64];
        snprintf(size, sizeof size, "%dx%d", w_, h_);
        // -level 3 is FFV1's stable, seekable level; gbrp is the lossless
        // packing of rgb24 that ffv1 actually encodes (rgb24 itself is not one
        // of its pixel formats, and letting ffmpeg pick would leave "lossless"
        // to a negotiation rather than to this line).
        std::vector<std::string> argv = {
            exe, "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pixel_format", "rgb24",
            "-video_size", size, "-framerate", fps,
            "-i", "-", "-c:v", "ffv1", "-level", "3", "-pix_fmt", "gbrp",
            path_
        };
        return spawn(argv, why);
    }

    bool writeFrame(const uint8_t* rgb, std::string& why) override {
        if (failed_) { why = why_; return false; }
        const size_t n = (size_t)w_ * h_ * 3;
        if (!writeAll(rgb, n)) {
            // A dead child is the usual cause, and its stderr already said why
            return fail("ffmpeg stopped reading after " + std::to_string(frames_) +
                        " frame(s) - see the console for what it printed", why);
        }
        frames_++;
        return true;
    }

    bool finish(std::string& why) override {
        if (failed_) { why = why_; return false; }
        if (frames_ == 0) return fail("no frames were written", why);
        int code = closePipe();      // closes stdin, then waits for the child
        if (code != 0)
            return fail("ffmpeg exited with status " + std::to_string(code) +
                        " - see the console for what it printed", why);
        return true;
    }

    void abort() override {
        closePipe();
        removeQuietly(path_);
    }
    int framesWritten() const override { return frames_; }

private:
    bool fail(const std::string& m, std::string& why) {
        failed_ = true; why_ = m; why = m; return false;
    }

#if defined(_WIN32)
    static std::wstring wide(const std::string& utf8) {
        if (utf8.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        std::wstring out((size_t)(n > 0 ? n : 0), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], n);
        return out;
    }
    // The CommandLineToArgvW rules, in reverse: quote the argument, double the
    // backslashes that precede a quote, escape the quotes.
    static std::wstring quoteArg(const std::wstring& a) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring q = L"\"";
        size_t slashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { slashes++; q += c; continue; }
            if (c == L'"') { q.append(slashes + 1, L'\\'); }
            slashes = 0;
            q += c;
        }
        q.append(slashes, L'\\');
        return q + L"\"";
    }
    bool spawn(const std::vector<std::string>& argv, std::string& why) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20))
            return fail("cannot create a pipe to ffmpeg", why);
        // Only the READ end travels to the child; a write end the child also
        // held would keep the pipe open forever and ffmpeg would never see EOF.
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        std::wstring cmd;
        for (size_t i = 0; i < argv.size(); i++) {
            if (i) cmd += L' ';
            cmd += quoteArg(wide(argv[i]));
        }
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = rd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(rd);
        if (!ok) {
            CloseHandle(wr);
            return fail("cannot start ffmpeg", why);
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        pipe_ = wr;
        return true;
    }
    bool writeAll(const uint8_t* p, size_t n) {
        while (n) {
            DWORD wrote = 0;
            DWORD chunk = (DWORD)(n > 1u << 20 ? 1u << 20 : n);
            if (!WriteFile(pipe_, p, chunk, &wrote, nullptr) || wrote == 0) return false;
            p += wrote; n -= wrote;
        }
        return true;
    }
    int closePipe() {
        if (pipe_) { CloseHandle(pipe_); pipe_ = nullptr; }
        if (!proc_) return 0;
        WaitForSingleObject(proc_, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc_, &code);
        CloseHandle(proc_);
        proc_ = nullptr;
        return (int)code;
    }
    HANDLE proc_ = nullptr;
    HANDLE pipe_ = nullptr;
#else
    static std::string quoteArg(const std::string& a) {
        std::string q = "'";
        for (char c : a) { if (c == '\'') q += "'\\''"; else q += c; }
        return q + "'";
    }
    bool spawn(const std::vector<std::string>& argv, std::string& why) {
        std::string cmd;
        for (size_t i = 0; i < argv.size(); i++) { if (i) cmd += ' '; cmd += quoteArg(argv[i]); }
        pipe_ = popen(cmd.c_str(), "w");
        if (!pipe_) return fail("cannot start ffmpeg", why);
        return true;
    }
    bool writeAll(const uint8_t* p, size_t n) {
        return fwrite(p, 1, n, pipe_) == n;
    }
    int closePipe() {
        if (!pipe_) return 0;
        int st = pclose(pipe_);
        pipe_ = nullptr;
        if (st == -1) return 1;
#ifdef WEXITSTATUS
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        return 1;
#else
        return st;
#endif
    }
    FILE* pipe_ = nullptr;
#endif
    std::string path_, why_;
    int w_ = 0, h_ = 0;
    double fps_ = 30;
    int frames_ = 0;
    bool failed_ = false;
};

}  // namespace

uint32_t aviFrameBytes(int w, int h) { return aviStride(w) * (uint32_t)h; }

std::string ffmpegPath() {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv || !*pathEnv) return {};
#if defined(_WIN32)
    const char sep = ';';
    static const char* const EXTS[] = { ".exe", ".com", "" };
#else
    const char sep = ':';
    static const char* const EXTS[] = { "" };
#endif
    std::string all(pathEnv);
    size_t at = 0;
    while (at <= all.size()) {
        size_t end = all.find(sep, at);
        if (end == std::string::npos) end = all.size();
        std::string dir = all.substr(at, end - at);
        at = end + 1;
        if (dir.empty()) continue;
        // A quoted PATH entry is legal on Windows and common in the wild.
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
            dir = dir.substr(1, dir.size() - 2);
        for (const char* ext : EXTS) {
            std::error_code ec;
            std::filesystem::path cand = fsPath(dir) / ("ffmpeg" + std::string(ext));
            if (std::filesystem::is_regular_file(cand, ec)) return cand.u8string();
        }
    }
    return {};
}

std::string aviSizeRefusal(int w, int h, int n) {
    if (w < 1 || h < 1 || n < 1) return {};
    const uint64_t frame = (uint64_t)aviFrameBytes(w, h);
    // header + n*(chunk header + frame [+ pad]) + idx1 header + n*16
    const uint64_t total = (uint64_t)HEADER_BYTES +
                           (uint64_t)n * (8 + frame + (frame & 1)) +
                           8 + (uint64_t)n * 16;
    if (total < 0xFFFFFFFFull) return {};
    char gb[64];
    snprintf(gb, sizeof gb, "%.1f", (double)total / 1073741824.0);
    return std::string("this stack would make a ") + gb + " GB uncompressed AVI, and "
           "AVI 1.0 addresses at most 4 GB - its chunk offsets are 32-bit, so "
           "everything past the 4 GB mark would be unreachable to any player. "
           "FFV1/MKV has no such limit and is lossless too (the same pixels, "
           "typically a third of the bytes): install ffmpeg, then choose MKV in "
           "the export dialog. Exporting fewer frames, or a smaller stack, is "
           "the other way.";
}

std::string ffmpegMissingRefusal() {
    return "FFV1/MKV is written by ffmpeg, and no `ffmpeg` was found on PATH. "
           "FFV1 is lossless compression - the same pixels the AVI would carry, "
           "in far fewer bytes - so it is what this export prefers when it is "
           "there. Two ways on: save as .avi instead (uncompressed RGB24, "
           "written by this build itself, no dependency), or install ffmpeg and "
           "export again.";
}

std::unique_ptr<VideoSink> openAvi(const std::string& utf8Path, int w, int h,
                                   double fps, std::string& why) {
    if (w < 1 || h < 1) { why = "nothing to write"; return nullptr; }
    if (!(fps > 0.0) || !std::isfinite(fps)) { why = "frame rate must be positive"; return nullptr; }
    auto a = std::unique_ptr<AviWriter>(new AviWriter(utf8Path, w, h, fps));
    if (!a->start(why)) { a->abort(); return nullptr; }
    return a;
}

std::unique_ptr<VideoSink> openFfv1(const std::string& utf8Path, int w, int h,
                                    double fps, std::string& why) {
    if (w < 1 || h < 1) { why = "nothing to write"; return nullptr; }
    if (!(fps > 0.0) || !std::isfinite(fps)) { why = "frame rate must be positive"; return nullptr; }
    const std::string exe = ffmpegPath();
    if (exe.empty()) { why = ffmpegMissingRefusal(); return nullptr; }
    auto p = std::unique_ptr<FfmpegPipe>(new FfmpegPipe(utf8Path, w, h, fps));
    if (!p->start(exe, why)) return nullptr;
    return p;
}

}  // namespace videowrite
