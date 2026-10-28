// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---------------------------------------------------------------- utilities
// (pathFromUtf8 は P6 で app/state.h へ: state.h の自己完結が要求する。この断片は
//  state.h の後に include されるので、以下の関数からは従来どおり見える。)

// ---- WINDOWS: THE COMMAND LINE AND THE ENVIRONMENT ARE UTF-16 (board 333) --
// A narrow `main(int, char**)` on Windows is handed its arguments in the
// process's ACTIVE CODE PAGE, and so is getenv(). Everything downstream of
// them in this program treats a std::string path as UTF-8 - pathFromUtf8 is
// literally u8path - so on any machine whose ACP is not UTF-8 the bytes are
// reinterpreted as a different encoding the moment they are used.
//
// THE CHARACTER THAT PROVES IT is 表 (U+8868). In cp932 - the ACP of a
// Japanese Windows - it is the two bytes 0x95 0x5C, and 0x5C is the byte for a
// backslash. So `C:\data\表\a.npy` does not merely come out looking wrong: the
// second byte of 表 becomes A PATH SEPARATOR, the name splits in two, and what
// the program then looks for is a directory that was never there. That is why
// the report is "the file does not open" rather than "the name looks odd", and
// it is why this is a correctness bug and not a cosmetic one.
//
// WHAT WAS DELIBERATELY NOT DONE (board 333's own ruling):
//  - NOT `wmain`. MinGW needs -municode for it, so the link line changes and
//    the entry point differs between the two Windows toolchains this repo
//    actually uses. One entry point on three platforms is worth more.
//  - NOT an `activeCodePage` manifest. It puts a floor at Windows 10 1903 and
//    changes the meaning of every narrow API in the process at once.
//  - CommandLineToArgvW sets no OS floor, changes neither the link line nor
//    the entry point, and puts the conversion in ONE place.
#if defined(_WIN32)
// UTF-16 -> UTF-8. `wlen` is -1 for a NUL-terminated string, or an explicit
// length in wchar_t. No WC_ERR_INVALID_CHARS: an unpaired surrogate becomes
// U+FFFD rather than failing the whole conversion, because what is being
// converted is a NAME and a name that cannot be fully represented is still
// worth carrying (and reporting) rather than dropping.
static std::string wideToUtf8(const wchar_t* w, int wlen = -1) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    // With wlen == -1 the count includes the terminating NUL; with an explicit
    // length it does not.
    std::string out((size_t)(wlen < 0 ? n - 1 : n), '\0');
    if (out.empty()) return out;
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, &out[0], n, nullptr, nullptr);
    return out;
}

// THE SEAM (board 333 §5): a UTF-16 command line in, UTF-8 arguments out.
// Named, and pure apart from the OS call, because a machine with a Japanese
// ACP cannot be put in CI - so what the selftest exercises is THE CONVERSION,
// with a command line it wrote itself, rather than the operating system.
static bool winArgvFromCommandLine(const wchar_t* cmdline,
                                   std::vector<std::string>& out) {
    out.clear();
    if (!cmdline) return false;
    int n = 0;
    wchar_t** wargv = CommandLineToArgvW(cmdline, &n);
    if (!wargv) return false;
    out.reserve((size_t)n);
    for (int i = 0; i < n; i++) out.push_back(wideToUtf8(wargv[i]));
    LocalFree(wargv);
    return true;
}

// ...and the environment, from the wide API for the same reason (§2).
// false = not set. An empty value also answers false, which is exactly what
// getenv()'s callers here already did with it: `if (const char* v = getenv(..))`
// followed by a path that comes out empty and is then rejected as empty. The
// point of this function is the ENCODING; which directory gets chosen must not
// move, so the chain each call site walks is left alone, character for
// character.
static bool winEnvUtf8(const wchar_t* name, std::string& out) {
    out.clear();
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return false;                  // not set
    std::vector<wchar_t> buf((size_t)need);
    const DWORD got = GetEnvironmentVariableW(name, buf.data(), need);
    if (got == 0 || got >= need) return false;    // vanished, or raced
    out = wideToUtf8(buf.data(), (int)got);
    return !out.empty();
}

// ...and this process's own image path (§3), which fixes an encoding bug and a
// correctness bug in the same line. argv[0] is what the shell happened to type,
// so it is ACP-encoded AND relative - and selfExePath()'s own comment already
// admitted the second half ("argv[0]: right unless the cwd changed"). What sits
// next to this path is `viewer-serve` (open_dispatch.inc), so a wrong answer
// here is the same family as the "viewer-serve does not get replaced" report.
//
// The buffer GROWS. GetModuleFileNameW truncates and reports
// ERROR_INSUFFICIENT_BUFFER rather than telling us the size, and MAX_PATH is
// not the limit on a modern Windows - a long path under a deep profile
// directory really does exceed it.
static std::string winModulePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (int tries = 0; tries < 8; tries++) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};                            // no answer at all
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return wideToUtf8(buf.data(), (int)n);
        buf.resize(buf.size() * 2);
    }
    return {};
}
#endif  // _WIN32

// Is this byte string well-formed UTF-8? (board 333 §4.) Needed because a
// machine that ran an EARLIER build has already written ACP bytes into its
// prefs.txt and its .vsession, and those files outlive this fix. Such a line
// cannot be repaired - working out which code page it was is guesswork, and
// guessing at a path is how you open the wrong file - so it is REFUSED and
// named. This is the test for "is this one of those".
//
// Overlong forms, encoded surrogates and values above U+10FFFF are ill-formed
// even though their byte patterns look correct, so the check is on the VALUE
// and not only on the shape.
static bool utf8IsValid(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        int need = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF)      { need = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07u; }
        else return false;               // 0x80..0xC1 and 0xF5..0xFF start nothing
        if (i + (size_t)need >= n) return false;          // truncated
        for (int k = 1; k <= need; k++) {
            const unsigned char cc = p[i + k];
            if (cc < 0x80 || cc > 0xBF) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false;
        i += (size_t)need + 1;
    }
    return true;
}
static std::string jpFontPath() {
    static const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-JP-Regular.otf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
#endif
    };
    for (const char* c : candidates)
        if (std::filesystem::exists(pathFromUtf8(c))) return c;
    return {};
}
// Seconds since the first call, monotonic - the app's clock.
//
// It used to be GLFW's own timer, which is only a clock once glfwInit() has run:
// before that it answers 0.0 forever and reports GLFW_NOT_INITIALIZED. The
// windowless startup path (--no-window) deliberately never calls glfwInit, and
// under a frozen 0.0 every "give up after 120 s" budget in the selftests -
// `while (t() - t0 < 120)` - becomes a loop with no way out, so a load that
// never finishes would hang until ctest's 900 s timeout instead of failing.
// steady_clock needs nothing initialised, and anchoring on first use gives it
// the same epoch glfwGetTime had, so the two are interchangeable. There is
// exactly one of them now: mixing the epochs would be far worse than either.
static double nowSec() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
// ---- is a RELOAD of these pixels IN FLIGHT? (the 2026-09-28 report) --------
// nullptr = no. A sentence = yes, and the sentence is what to refuse with.
//
// THE RULING (review, 2026-09-29): while a reload of a stack is in flight, an
// in-place pixel operation on that stack is REFUSED. Crop and "restore full"
// rewrite a source's pixels and its crop bookkeeping; a detrend reads every
// member to compute a product. Run either one beside a reload and the answer is
// built from two generations of the file, mixed frame by frame according to how
// far the worker happened to have got - a number that would look perfectly
// ordinary afterwards and could never be traced.
//
// REFUSING AT THE GESTURE, not at the landing. The alternative - accept the
// crop and drop it when the reload lands - throws away work the user asked for,
// invisibly, which is the worse failure of the two. cropInPlace already refuses
// by name for the same shape of reason (a decimated remote tile whose
// full-resolution fetch is still coming), and this is that precedent with a
// different thing in flight.
//
// The sentence names the Files row's Stop, because that is the control that
// makes the wait end, and it is the same vocabulary the row itself uses.
static const char* reloadInFlightFor(int seqId, uint64_t uid) {
    if (!app.reloadJob || app.reloadJob->cancel.load()) return nullptr;
    const App::ReloadJob& j = *app.reloadJob;
    if (j.oneUid != 0) {
        if (uid != 0 && j.oneUid == uid)
            return "this frame is being re-read - wait for it, or press Stop on "
                   "the Files row";
        // ...and a STACK is asked about too, because a one-frame reload of one
        // of its members is still a reload of pixels a detrend would read
        // (review): the job names a document, so the stack it belongs to is
        // found through that document rather than carried on the job.
        if (seqId != 0)
            for (const auto& q : app.images)
                if (q->uid == j.oneUid && q->seqId == seqId)
                    return "a frame of this stack is being re-read - wait for it, "
                           "or press Stop on the Files row";
        return nullptr;
    }
    if (seqId != 0 && j.seqId == seqId)
        return "this stack is being re-read - wait for it, or press Stop on the "
               "Files row";
    return nullptr;
}

// Whole-file reads, counted so a selftest can assert that opening one file
// reads it ONCE. loadNpy used to read a 252 MB stack 120 times and nothing
// noticed, because "it works, slowly" looks exactly like "it works".
// (--asyncopen-selftest R2 is the same assertion for a RELOAD: three
// memberships of one file are one read of it.)
static long long g_fileReads = 0;

static bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out) {
    g_fileReads++;
    std::ifstream f(pathFromUtf8(utf8Path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize sz = f.tellg();
    f.seekg(0);
    out.resize((size_t)sz);
    return sz == 0 || (bool)f.read((char*)out.data(), sz);
}
std::string baseName(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}
// A path on its way to the SCREEN. The bundled CJK font draws U+005C as the yen
// sign - the JIS legacy - so a Windows path renders "C:(yen)Users(yen)hish".
// Every candidate font does it (Meiryo, Yu Gothic, MS Gothic, Noto CJK JP), so
// this is not one font's bug to route around, and there is no second font in
// the atlas to fall back to: the CJK face IS the app's font, one base, no
// merge. Dropping U+005C from its ranges would leave the glyph missing, not
// borrowed - a '?' where the separator should be.
//
// So the separator is shown, not the codepoint. The app already normalises to
// '/' everywhere it compares or joins paths (openPickerWith does it per file,
// browseLocalFolder does it on the way in, and every remote path is '/'), the
// Windows API takes '/' as readily as '\\', and a path copied out of here
// pastes into Python, CMake and a shell unescaped. Storage is untouched: this
// is display only.
static std::string dispPath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// The shortest path that still names a volume - where "up" stops and what the
// first breadcrumb points at. A UNC share is ONE root: \\nas\share names the
// volume, and neither \\nas nor a bare \\ can be listed, so it must not be cut
// into clickable "nas" / "share" pieces. A bare "C:" is not the drive's root
// either - on Windows it means the process's current directory ON that drive.
std::string pathRootOf(const std::string& d) {
    if (d.size() > 1 && d[0] == '/' && d[1] == '/') {          // //server/share
        size_t srv = d.find('/', 2);
        size_t shr = srv == std::string::npos ? std::string::npos : d.find('/', srv + 1);
        return shr == std::string::npos ? d : d.substr(0, shr);
    }
    if (!d.empty() && d[0] == '~') return "~";
    if (d.size() >= 2 && d[1] == ':') return d.substr(0, 2) + "/";
    return "/";
}

// The path as clickable pieces: {what it reads as, where it goes}. Splitting a
// path is not splitting on '/' - the root can be "/", "~", "C:/" or a whole
// UNC share, and rebuilding a prefix from the pieces has to give back a path
// that exists (a NAS reported 2026-08-03: clicking the path errored, while ".."
// kept working, because "//nas/share/x" was being rebuilt as "/nas").
// (struct PathSeg lives in core/browse/host.h since P7: this declaration-
// carrying header is where the Browse TUs see the return type.)
std::vector<PathSeg> pathSegments(const std::string& d) {
    std::vector<PathSeg> segs;
    size_t i = 0;
    std::string root = pathRootOf(d);
    if (d.size() > 1 && d[0] == '/' && d[1] == '/') {
        segs.push_back({ root, root });
        i = root.size() < d.size() ? root.size() + 1 : d.size();
    } else if (!d.empty() && d[0] == '/') {
        segs.push_back({ "/", "/" });
        i = 1;
    } else if (!d.empty() && d[0] == '~') {
        segs.push_back({ "~", "~" });
        i = d.size() > 1 && d[1] == '/' ? 2 : 1;
    } else if (d.size() >= 2 && d[1] == ':') {
        segs.push_back({ d.substr(0, 2), root });               // "C:" -> "C:/"
        i = d.size() > 2 && d[2] == '/' ? 3 : 2;
    }
    while (i < d.size()) {
        size_t s = d.find('/', i);
        std::string part = d.substr(i, s == std::string::npos ? std::string::npos : s - i);
        if (!part.empty()) {
            const std::string base = segs.empty() ? std::string() : segs.back().path;
            segs.push_back({ part, base.empty()            ? part
                                 : base.back() == '/'      ? base + part
                                                           : base + "/" + part });
        }
        if (s == std::string::npos) break;
        i = s + 1;
    }
    return segs;
}
// ---- a stack's frames are ONE shape -----------------------------------------
// "8x8 1ch" - the same three numbers, in the same order, that every stack line
// already prints ("%dx%d %dch"), so a refusal and the log above it describe a
// frame the same way.
static std::string frameShapeText(int w, int h, int ch) {
    return std::to_string(w) + "x" + std::to_string(h) + " " + std::to_string(ch) + "ch";
}
// Does this frame belong in that stack? Empty = yes; otherwise the reason, in
// the words core/tiffread.cpp refuses a disagreeing PAGE in.
//
// It lives HERE, above both transports, because there are two of them and the
// rule is one rule. The local sequence loader learned it first (PR #116) and
// the remote fetcher did not, so a folder of disagreeing shapes was refused
// when it sat on this disk and stacked anyway when the same folder was reached
// through a peer - docs/terminology.md gives a stack a TIME axis either way,
// and sigma_t / FPN / the per-frame table are quantities only if every frame is
// the same picture measured again. One definition means the two halves cannot
// drift apart into two different promises about the same word.
static std::string stackShapeRefusal(int w, int h, int ch,
                                     int refW, int refH, int refCh,
                                     const std::string& refName) {
    if (w == refW && h == refH && ch == refCh) return std::string();
    return frameShapeText(w, h, ch) + " and " + refName + " is " +
           frameShapeText(refW, refH, refCh) +
           ": a run of numbered files is a stack, and a stack's frames are "
           "one shape - open it on its own";
}

static float niceStep(float raw) {   // smallest 1/2/5*10^k >= raw
    float mag = powf(10.0f, floorf(log10f(std::max(raw, 1e-9f))));
    for (float m : {1.0f, 2.0f, 5.0f, 10.0f})
        if (m * mag >= raw) return m * mag;
    return 10.0f * mag;
}

// Set or clear an environment variable for a CHILD this process is about to
// start. The only callers are selftests that need a peer built differently from
// the one they already have - VIEWER_SERVE_PROTOCOL, which makes `viewer --serve`
// BE an older peer (announcing the lower number in HELLO and refusing everything
// that number predates), so a "your peer is too old" refusal is exercised in the
// gate instead of by keeping an old binary on somebody's disk.
//
// Here, in the file everything includes, because two selftests need it now
// (--rset-selftest for MOP_SET_FOLD's v7 gate, --remote-selftest for the
// declared reading's v8 one) and a second copy of a #ifdef is a second thing to
// get wrong on one of the three platforms.
static void selftestSetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else       unsetenv(name);
#endif
}
