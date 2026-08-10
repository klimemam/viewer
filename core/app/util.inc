// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---------------------------------------------------------------- utilities
// (pathFromUtf8 は P6 で app/state.h へ: state.h の自己完結が要求する。この断片は
//  state.h の後に include されるので、以下の関数からは従来どおり見える。)
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
// Whole-file reads, counted so a selftest can assert that opening one file
// reads it ONCE. loadNpy used to read a 252 MB stack 120 times and nothing
// noticed, because "it works, slowly" looks exactly like "it works".
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
