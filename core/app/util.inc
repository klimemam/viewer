// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---------------------------------------------------------------- utilities
// (pathFromUtf8 は P6 で app/state.h へ: state.h の自己完結が要求する。この断片は
//  state.h の後に include されるので、以下の関数からは従来どおり見える。)
// ---- WHICH font file the CJK glyphs are baked from (板306 C) ----------------
// The candidate list used to be the whole story, and on three kinds of machine
// it is the wrong story:
//  - The Windows candidates are ALL Japanese supplemental fonts (a Feature on
//    Demand). On an English Windows 11 with no Japanese language pack, meiryo,
//    YuGothic and MS Gothic can all three be absent, and there was nothing
//    after them.
//  - The non-Windows candidates are five hard-coded paths. Noto CJK ships under
//    a growing number of names - NotoSansCJK-VF.otf.ttc is the current
//    variable-font spelling - so a perfectly well equipped Linux box could miss
//    every one of them.
//  - And there was no way for the user to SAY where their font is.
// So: an override first, then the candidates, then one call to fontconfig. And
// whichever way it ends, the paths that were LOOKED AT come back with the
// answer, because "CJK font not found" on its own is a message nobody can act
// on - what they need is the list, so they can point the override at a font
// they do have.
struct FontChoice {
    std::string path;                    // "" = nothing was found
    const char* how = "none";            // who decided: the override, the list, fontconfig
    std::vector<std::string> tried;      // in order, including the override
};
// The override, as settings.jsonc / prefs.txt left it. Declared here rather
// than in settings.inc (where g_settingsEditorCmd and g_settingsRepoUrl live)
// because THIS file is the one that reads it, and settings.inc is later in the
// spine so its writer sees it either way. Empty = no override, use the list.
static std::string g_settingsFontPath;
// ...and what the start-up actually settled on, kept so that the refusal, the
// Preferences row and --cjkfont-selftest all read one answer instead of each
// re-deriving it. `g_fontMergePath` is the second face, "" where none was
// merged (see cjkMergeFontPath).
static FontChoice g_fontChoice;
static std::string g_fontMergePath;

// The paths this build knows about, in order. Named so the refusal can print
// them and so a test can assert that it did.
static std::vector<std::string> jpFontCandidates() {
    static const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        // Bundled with Windows 11 itself rather than with the Japanese language
        // pack, so on the English install where the three above are all missing
        // this is the one that is there.
        "C:\\Windows\\Fonts\\NotoSansJP-VF.ttf",
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
        // The VARIABLE-font spellings, which is how Noto CJK is packaged now.
        // A machine with a perfectly good font and a newer file name used to
        // fall through the whole list.
        "/usr/share/fonts/opentype/noto/NotoSansCJK-VF.otf.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-VF.otf.ttc",
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-VF.otf.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-JP-VF.otf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-JP-VF.otf",
#endif
    };
    return std::vector<std::string>(candidates, candidates + (sizeof candidates / sizeof candidates[0]));
}

// The last way out: ask fontconfig, which on a Linux box is the only thing that
// actually KNOWS. Through `fc-match` and popen rather than by linking
// libfontconfig, because a new link-time dependency taken on for a fallback
// makes every build need a library in order to cope with one machine that is
// unusual. Called ONCE, and only after the candidates have all missed, so
// every ordinary failure here - no popen, no fc-match, an answer that is not a
// file - costs nothing: the answer was going to be "nothing found" anyway.
// Never on Windows: there is no fontconfig there, and the shell popen would
// open is a visible flash of a console window.
static std::string fcMatchJpFont() {
#if defined(_WIN32)
    return {};
#else
    FILE* p = popen("fc-match -f %{file} sans:lang=ja 2>/dev/null", "r");
    if (!p) return {};
    char buf[1024] = { 0 };
    const size_t got = fread(buf, 1, sizeof buf - 1, p);
    pclose(p);
    std::string out(buf, got);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    std::error_code ec;
    if (out.empty() || !std::filesystem::is_regular_file(pathFromUtf8(out), ec)) return {};
    return out;
#endif
}

// `want` is the user's override (appearance.fontPath / prefs.txt `fontPath`),
// empty when they have not set one. It is tried FIRST and it is not silently
// dropped when it misses: it lands in `tried` like everything else, so the
// refusal says "you asked for this and it is not there" rather than listing
// four paths the user never mentioned.
static FontChoice jpFontChoice(const std::string& want) {
    FontChoice c;
    std::error_code ec;
    auto usable = [&](const std::string& p) {
        return !p.empty() && std::filesystem::is_regular_file(pathFromUtf8(p), ec);
    };
    if (!want.empty()) {
        c.tried.push_back(want);
        if (usable(want)) { c.path = want; c.how = "appearance.fontPath"; return c; }
    }
    for (const std::string& p : jpFontCandidates()) {
        c.tried.push_back(p);
        if (usable(p)) { c.path = p; c.how = "built-in candidate"; return c; }
    }
    if (std::string fc = fcMatchJpFont(); !fc.empty()) {
        c.tried.push_back("fc-match sans:lang=ja -> " + fc);
        c.path = fc; c.how = "fc-match";
        return c;
    }
#if !defined(_WIN32)
    c.tried.push_back("fc-match sans:lang=ja (no answer)");
#endif
    return c;
}

// The SECOND face, merged on top of the first (板306 B). Windows only, and the
// reason is a measurement rather than a preference: the cmap tables in
// C:\Windows\Fonts say Meiryo holds 1819 of the 2500 common simplified-Chinese
// characters (72.8%), and 测 (U+6D4B) and 试 (U+8BD5) - the two characters in
// the word "test" - are in NONE of Meiryo, Yu Gothic, MS Gothic or
// NotoSansJP-VF. Microsoft YaHei is the only font on a stock Windows that has
// all 2500, so on Windows the simplified characters can only come from a second
// file.
//
// macOS has the same shape of gap for the same reason - Hiragino Kaku Gothic is
// a Japanese face - and the same answer: Hiragino Sans GB (or PingFang SC) is
// the Chinese face that ships with the system. Note that Hiragino Sans GB is
// ALSO the third candidate in the primary list, which is what `primary` guards
// against: a machine that had no Japanese face and fell through to it must not
// then merge it onto itself.
//
// Linux needs no second file: Noto Sans CJK is one pan-CJK font, and even the
// language-specific NotoSansCJK-JP-Regular.otf carries the whole ideograph set
// (the JP/SC/TC builds differ in which VARIANT is default, not in coverage).
static std::string cjkMergeFontPath(const std::string& primary) {
    static const char* merges[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\msyh.ttc",     // Microsoft YaHei
        "C:\\Windows\\Fonts\\msyhl.ttc",    // ...Light, if only that one is there
        "C:\\Windows\\Fonts\\simsun.ttc",   // the pre-Vista face, still installed on some
#elif defined(__APPLE__)
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/PingFang.ttc",
#else
        nullptr,
#endif
    };
    std::error_code ec;
    for (const char* m : merges) {
        if (!m) continue;
        if (primary == m) continue;
        if (std::filesystem::is_regular_file(pathFromUtf8(m), ec)) return m;
    }
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
// ---- WHICH CODE POINTS ARE ASKED FOR (板306 A) ------------------------------
// The user report was 「日本語とか中国語のファイル名が文字化けする」, and
// widening this set is the first half of the answer. Measured, by expanding
// ImGui's two tables and intersecting them: 1144 of the 2500 common simplified
// Chinese characters - 45.8% - are OUTSIDE GetGlyphRangesJapanese, so a Chinese
// filename lost nearly half its characters. And the punctuation was worse,
// because it hit JAPANESE names too: General Punctuation and Letterlike Symbols
// were in no range at all, so — – … " " ' № and ℃ drew as '?' although every
// candidate font has them - and `25℃.npy` / `60℃_dark.npy` are ordinary names
// in camera IQ work, which is most of what the report was about.
//
// A range rather than a handful of AddChar calls: the builder is a bitset over
// the BMP, the two blocks are 112 + 80 code points, and enumerating the ones
// somebody happened to have hit is how U+2025 came to be listed on its own
// while U+2014 next to it was missing. (U+29C9 stays an AddChar - it is in
// Miscellaneous Technical, outside both blocks.)
//
// WHAT IS DELIBERATELY NOT HERE: Hangul. Measured on this machine, adding
// GetGlyphRangesKorean takes the atlas from 3612 to 16185 glyphs - 4.5x - and
// the texture from 1024x1024 to 2048x4096, i.e. 33.6 MB of RGBA and a startup
// several times longer, for a script nobody has reported. It is a separate row
// on the board, not a line here.
static void cjkFontRanges(ImFontAtlas* atlas, ImVector<ImWchar>& out) {
    ImFontGlyphRangesBuilder b;
    b.AddRanges(atlas->GetGlyphRangesJapanese());
    b.AddRanges(atlas->GetGlyphRangesChineseSimplifiedCommon());
    // AddRanges wants a {lo, hi, ..., 0}-terminated static array.
    static const ImWchar kTypography[] = {
        // General Punctuation: the dashes, the quotation marks, the ellipsis,
        // U+2025 TWO DOT LEADER (stack names carry it - frame_000‥023.npy, see
        // rp::patternWithExtent - and an extent that draws as '?' is exactly
        // the uninformative character the extent exists to remove), and U+2030.
        0x2000, 0x206F,
        // Letterlike Symbols: ℃ ℉ № ™ Ω ℓ. A temperature in a filename is the
        // single most common non-ASCII character in this project's own data.
        0x2100, 0x214F,
        0,
    };
    b.AddRanges(kTypography);
    // U+29C9 TWO JOINED SQUARES, the Files panel's share mark (file_list §4).
    // Not every CJK font carries it: shareGlyph() checks the built atlas and
    // falls back to the word "shared" rather than showing '?'.
    b.AddChar((ImWchar)0x29C9);
    // Neither table covers Greek, so a table of noise figures could only write
    // "sigma_f" where it means one symbol. (Unicode has no subscript f or v, so
    // the axis letter rides alongside the symbol rather than under it.)
    b.AddChar((ImWchar)0x03C3);        // sigma
    b.AddChar((ImWchar)0x03BC);        // mu
    b.BuildRanges(&out);
}

// Is this code point one the ranges ASKED for? A different question from "did
// a glyph get baked", which is what FindGlyphNoFallback answers, and the two
// have to be asked separately: the first is this build's decision and the
// second is a fact about the font file on the machine.
static bool cjkRangeHas(const ImVector<ImWchar>& ranges, unsigned int cp) {
    for (int k = 0; k + 1 < ranges.Size && ranges[k]; k += 2)
        if (cp >= (unsigned)ranges[k] && cp <= (unsigned)ranges[k + 1]) return true;
    return false;
}

// ---- WHAT THE GLYPH ATLAS COST, MEASURED (板306 A) --------------------------
// The ranges are baked ONCE, while the window is being created, and widening
// them is the whole fix for a Chinese filename drawing as '?'. It is also the
// whole of the price: more code points means more glyphs rasterised at startup
// and a bigger texture. 板306's acceptance condition was that the price be
// MEASURED rather than estimated, and a number nobody can read after the fact
// is not measured - so it is one line on stderr, on every start, beside the
// "ui scale:" line that exists for the same reason.
//
// Build() is called HERE rather than left to the renderer backend, and that is
// three things at once: it is what makes the figure timeable, it is what makes
// the line print on a machine with no GL at all, and it is what gives a
// --no-window run an atlas to interrogate - ImGui builds the atlas lazily, from
// GetTexDataAs*, which only a renderer backend calls. Calling it now is free
// later: GetTexDataAsAlpha8 only builds when TexPixelsAlpha8 is null.
struct FontAtlasCost {
    int wantedCodepoints = 0;   // what the ranges ASKED for
    int bakedGlyphs = 0;        // what the font(s) actually had
    int texW = 0, texH = 0;
    int faces = 0;              // 1, or 2 where a second face was merged on
    double buildMs = 0;
    float fontScale = 0;
};
static FontAtlasCost g_fontAtlasCost;

// A code point every CJK font has (HIRAGANA LETTER A). Asked of the BUILT
// atlas, which is the only place the answer is honest: the candidate list can
// hand back a font with no CJK in it at all - fc-match answers
// sans:lang=ja with DejaVu Sans on a machine that has no Japanese font - and
// "a font was found" would then be a true sentence that told the user nothing.
static bool fontAtlasHasCjk() {
    if (!ImGui::GetCurrentContext()) return false;
    ImFontAtlas* a = ImGui::GetIO().Fonts;
    if (!a || a->Fonts.Size == 0) return false;
    return a->Fonts[0]->FindGlyphNoFallback((ImWchar)0x3042) != nullptr;
}

static void fontAtlasBake(const ImVector<ImWchar>& ranges, float fontScale) {
    ImFontAtlas* a = ImGui::GetIO().Fonts;
    FontAtlasCost c;
    c.fontScale = fontScale;
    // BuildRanges emits {lo,hi} pairs and a 0 terminator, so the count is the
    // sum of the closed intervals - the number of code points that were ASKED
    // for, which is not the number that get baked: a font is entitled not to
    // have a glyph, and the gap between these two numbers is exactly what the
    // "still draws as '?'" half of this change is about.
    for (int k = 0; k + 1 < ranges.Size && ranges[k]; k += 2)
        c.wantedCodepoints += (int)ranges[k + 1] - (int)ranges[k] + 1;
    const double t0 = nowSec();
    a->Build();
    c.buildMs = (nowSec() - t0) * 1000.0;
    for (ImFont* f : a->Fonts) c.bakedGlyphs += f->Glyphs.Size;
    c.texW = a->TexWidth; c.texH = a->TexHeight;
    c.faces = a->ConfigData.Size;
    g_fontAtlasCost = c;
    fprintf(stderr, "font atlas: %d glyph(s) of %d requested codepoint(s), "
                    "%dx%d px (%.1f MB RGBA), %d face(s), built in %.0f ms "
                    "(font scale %.2f, kana %s)\n",
            c.bakedGlyphs, c.wantedCodepoints, c.texW, c.texH,
            c.texW * (double)c.texH * 4.0 / 1048576.0, c.faces, c.buildMs,
            fontScale, fontAtlasHasCjk() ? "present" : "MISSING");
}

// The refusal, with the paths in it (板306 C). "CJK font not found" was the
// whole message before, which names no action: the machine that produces it is
// precisely the machine whose font is somewhere this build did not look, and
// the list is the only thing that lets the user point the override at it.
static std::string cjkFontMissingText(const FontChoice& c) {
    std::string s = "no CJK font found - Japanese and Chinese filenames will "
                    "draw as '?'. Looked at: ";
    for (size_t k = 0; k < c.tried.size(); k++) {
        if (k) s += "; ";
        s += c.tried[k];
    }
    if (c.tried.empty()) s += "(nothing - this build has no candidates)";
    s += ". Set \"appearance.fontPath\" in settings.jsonc (or `fontPath` in "
         "prefs.txt) to a font file and restart.";
    return s;
}

// ...and the OTHER failure, which used to be silent: a font was found and it has
// no CJK in it. Same shape of sentence, because it is the same problem from the
// user's side - the names will not draw - and the same way out.
static std::string cjkFontNoGlyphsText(const FontChoice& c) {
    return "the font this build is using has no Japanese or Chinese glyphs, so "
           "those filenames will draw as '?': " + c.path + " (chosen by " +
           c.how + "). Set \"appearance.fontPath\" in settings.jsonc (or "
           "`fontPath` in prefs.txt) to a CJK font and restart.";
}
