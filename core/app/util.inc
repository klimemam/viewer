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
    // At least one path was passed over because the breadcrumb said the last
    // start died baking it. The user has to be told: their setting is being
    // ignored, and only they can decide whether to fix or remove it.
    bool skippedAfterCrash = false;
    // ...and whether the thing skipped was the USER'S setting or one of this
    // build's own candidates. The two need different advice: only one of them
    // has a line the user can delete (review P3).
    bool skippedWasOverride = false;
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

// The size the CJK faces are baked at. Named because it is used in three
// places - the primary, the merged face, and the retry that drops the merge -
// and three copies of a literal is how they stop agreeing.
static constexpr float CJK_FONT_PX = 17.0f;

// ---- ONE LIST, WITH THE LANGUAGE ON EACH ROW (review P2) -------------------
// There used to be two lists and a font's language was inferred from which one
// it sat in. That inference was wrong on macOS, where Hiragino Sans GB was in
// BOTH - as a primary candidate and as a merge candidate - and a duplicated
// truth produced exactly what one does: the "a Chinese face is being used for
// everything" message could not fire on the one platform where that state is
// reachable (the path came back tagged "built-in candidate", because the
// primary list found it first), and a refusal could print the same path twice.
//
// So the language is a FIELD. The merge partner and the fourth message are both
// read off it, which leaves one row as the single place either answer can move.
//
// HIRAGINO SANS GB IS TAGGED zh, by the criterion the review set: not "does it
// have kana" - it does - but "does it draw JAPANESE forms". GB is 国标, the
// PRC standard, and its ideographs follow Chinese conventions. That is the
// whole reason it is useful as a second face and the whole reason it is wrong
// as the first. PingFang SC is the same case.
enum FontLang { LangJa, LangZh };
struct FontCandidate { const char* path; FontLang lang; };

static std::vector<FontCandidate> cjkFontCandidates() {
    static const FontCandidate all[] = {
#if defined(_WIN32)
        { "C:\\Windows\\Fonts\\meiryo.ttc",        LangJa },
        { "C:\\Windows\\Fonts\\YuGothM.ttc",       LangJa },
        { "C:\\Windows\\Fonts\\msgothic.ttc",      LangJa },
        // Bundled with Windows 11 itself rather than with the Japanese language
        // pack, so on the English install where the three above are all missing
        // this is the one that is there.
        { "C:\\Windows\\Fonts\\NotoSansJP-VF.ttf", LangJa },
        { "C:\\Windows\\Fonts\\msyh.ttc",          LangZh },   // Microsoft YaHei
        { "C:\\Windows\\Fonts\\msyhl.ttc",         LangZh },   // ...Light
        { "C:\\Windows\\Fonts\\simsun.ttc",        LangZh },   // the pre-Vista face
#elif defined(__APPLE__)
        { "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", LangJa },
        { "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", LangJa },
        { "/System/Library/Fonts/Hiragino Sans GB.ttc",      LangZh },
        { "/System/Library/Fonts/PingFang.ttc",              LangZh },
#else
        // Linux needs no second file: Noto Sans CJK is one pan-CJK font, and
        // even the language-specific NotoSansCJK-JP-Regular.otf carries the
        // whole ideograph set (the JP/SC/TC builds differ in which VARIANT is
        // default, not in coverage). So every row here is ja and the merge list
        // comes out EMPTY - which is the right answer, not a missing one.
        { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", LangJa },
        { "/usr/share/fonts/opentype/noto/NotoSansCJK-JP-Regular.otf", LangJa },
        { "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", LangJa },
        { "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc", LangJa },
        { "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf", LangJa },
        // The VARIABLE-font spellings, which is how Noto CJK is packaged now.
        // A machine with a perfectly good font and a newer file name used to
        // fall through the whole list.
        { "/usr/share/fonts/opentype/noto/NotoSansCJK-VF.otf.ttc", LangJa },
        { "/usr/share/fonts/noto-cjk/NotoSansCJK-VF.otf.ttc", LangJa },
        { "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-VF.otf.ttc", LangJa },
        { "/usr/share/fonts/opentype/noto/NotoSansCJK-JP-VF.otf", LangJa },
        { "/usr/share/fonts/noto-cjk/NotoSansCJK-JP-VF.otf", LangJa },
#endif
    };
    return std::vector<FontCandidate>(all, all + (sizeof all / sizeof all[0]));
}

// The Japanese faces, in order: what the primary search prefers.
static std::vector<std::string> jpFontCandidates() {
    std::vector<std::string> out;
    for (const FontCandidate& c : cjkFontCandidates())
        if (c.lang == LangJa) out.push_back(c.path);
    return out;
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
    // TIMED OUT (review P3-5). On a machine whose fontconfig cache is cold,
    // fc-match scans every font on the disk and can take many seconds - and
    // this runs while the window is being created, so the whole of that is a
    // start-up with nothing on screen. `timeout` is coreutils and is not on
    // every machine (macOS has none by default), so it is used only when it is
    // there; without it the behaviour is exactly what it was.
    // `if/else`, not `timeout ... || fc-match ...`: with the `||` a run that
    // actually TIMED OUT (exit 124) would fall through to the untimed call and
    // wait all over again, which is the opposite of the point.
    FILE* p = popen("if command -v timeout >/dev/null 2>&1; then "
                    "timeout 5 fc-match -f %{file} sans:lang=ja; else "
                    "fc-match -f %{file} sans:lang=ja; fi 2>/dev/null", "r");
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

// ---- IS THIS FILE A FONT? (review P1) --------------------------------------
// "It exists and is a regular file" was the whole test, and it is not enough in
// a way that takes the program out before it can say anything.
//
// MEASURED: point appearance.fontPath at a 175-byte text file and this build
// exits 139 - SIGSEGV - having printed nothing after the "ui scale:" line.
// AddFontFromFileTTF does not parse anything, it only reads the bytes, so it
// returns NON-NULL and the `if (!jp)` fallback never runs. The break is inside
// Build(): stbtt_GetFontOffsetForIndex answers -1, and with NDEBUG (a default
// cmake build of this project sets no CMAKE_BUILD_TYPE, so the release
// packages are where this bites) that -1 reaches stbtt_InitFont, whose
// stbtt__find_table takes a stbtt_uint32 offset - so it reads at data + 4 GB.
// All of this happens BEFORE openCrashFile() and the SIGSEGV handler are
// installed, so there is no crash record either.
//
// And the value is reachable by hand: the Preferences box commits on every
// keystroke and prefs.txt keeps it, so a user could put this machine into a
// state where the application does not start and the only way back is to edit
// a settings file - with the application that does not start.
//
// So the signature is checked. Four bytes, which is all an sfnt wrapper needs
// to identify itself. This does NOT prove the font can be parsed - see the
// Build() check in fontAtlasBake(), which is the other half of the same hole.
// THE SIGNATURE WAS NOT ENOUGH, measured. A file holding the FIRST 4096 BYTES
// of a real meiryo.ttc has a correct signature, a correct collection header and
// a correct table directory - and every table it names points past the end of
// the file. stb_truetype follows those offsets, and `viewer --no-window` exits
// 139 with nothing printed. That is not a contrived input: an interrupted
// download, a half-finished copy and a partial read off a network drive all
// produce exactly it, and because `appearance.fontPath` PERSISTS, the machine
// then fails to start again at every subsequent launch.
//
// So the directory is walked. Everything here is cheap - one file size and at
// most a few kilobytes of header - and every check is a bound, not a parse:
// the goal is to refuse what would make stb_truetype read outside the file.
//
// THIS DOES NOT MAKE stb_truetype SAFE, and it cannot: past the directory it
// goes on to follow offsets inside cmap subtables and inside loca/glyf. Each
// check narrows the hole; none closes it. What closes it is the breadcrumb in
// session.inc - "if the last start died while baking this file, do not bake it
// again" - which is the only mechanism that survives a crash we did not predict.
namespace sfnt {
inline uint16_t be16(const unsigned char* p) {
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
inline uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline bool isWrapperTag(uint32_t tag) {
    return tag == 0x00010000u ||     // TrueType outlines
           tag == 0x74727565u ||     // 'true' - Apple's TrueType
           tag == 0x4F54544Fu ||     // 'OTTO' - CFF outlines
           tag == 0x74797031u;       // 'typ1' - a Type 1 housed in an sfnt
}
// One font's table directory, at `off` inside a file of `size` bytes.
inline bool directoryOk(std::ifstream& f, uint64_t off, uint64_t size,
                        std::string& why) {
    unsigned char h[12];
    f.clear();
    f.seekg((std::streamoff)off, std::ios::beg);
    f.read((char*)h, 12);
    if (f.gcount() != 12) { why = "the table directory is past the end of the file"; return false; }
    if (!isWrapperTag(be32(h))) { why = "a font inside it has no sfnt signature"; return false; }
    const uint32_t numTables = be16(h + 4);
    if (numTables == 0) { why = "it declares no tables at all"; return false; }
    if (off + 12 + 16ull * numTables > size) {
        why = "its table directory (" + std::to_string(numTables) +
              " entries) does not fit in the file";
        return false;
    }
    std::vector<unsigned char> dir((size_t)16 * numTables);
    f.read((char*)dir.data(), (std::streamsize)dir.size());
    if ((size_t)f.gcount() != dir.size()) { why = "the table directory is truncated"; return false; }
    bool haveCmap = false, haveHead = false, haveHhea = false, haveHmtx = false;
    bool haveGlyf = false, haveLoca = false, haveCff = false;
    for (uint32_t k = 0; k < numTables; k++) {
        const unsigned char* r = dir.data() + (size_t)16 * k;
        const uint32_t tag = be32(r);
        // uint64 on purpose: offset and length are each uint32 and their sum
        // overflows in 32 bits, which is how a crafted or corrupt file slips a
        // bounds check that adds them in the wrong type.
        const uint64_t tOff = be32(r + 8), tLen = be32(r + 12);
        if (tOff + tLen > size) {
            char t[5] = { (char)r[0], (char)r[1], (char)r[2], (char)r[3], 0 };
            for (int i = 0; i < 4; i++) if (t[i] < 0x20 || t[i] > 0x7E) t[i] = '?';
            why = std::string("its '") + t + "' table points outside the file "
                  "(the file is truncated)";
            return false;
        }
        switch (tag) {
            case 0x636D6170u: haveCmap = true; break;   // cmap
            case 0x68656164u: haveHead = true; break;   // head
            case 0x68686561u: haveHhea = true; break;   // hhea
            case 0x686D7478u: haveHmtx = true; break;   // hmtx
            case 0x676C7966u: haveGlyf = true; break;   // glyf
            case 0x6C6F6361u: haveLoca = true; break;   // loca
            case 0x43464620u: haveCff  = true; break;   // 'CFF '
            default: break;
        }
    }
    // Exactly what stbtt_InitFont requires, in its own order - so "this build
    // cannot interpret it" is decided HERE, with a name, instead of by an
    // assert that release builds compile out. CFF2 lands on the last line:
    // its outlines are in a `CFF2` table, so neither glyf nor 'CFF ' is found.
    if (!haveCmap || !haveHead || !haveHhea || !haveHmtx) {
        why = "it is missing a table this build needs (cmap/head/hhea/hmtx)";
        return false;
    }
    if (haveGlyf && !haveLoca) { why = "it has glyf but no loca"; return false; }
    if (!haveGlyf && !haveCff) {
        why = "its outlines are in a format this build cannot read (no glyf and "
              "no 'CFF ' - a CFF2 variable font looks like this)";
        return false;
    }
    return true;
}
}  // namespace sfnt

static bool sfntLooksSane(const std::string& p, std::string& why) {
    why.clear();
    std::ifstream f(pathFromUtf8(p), std::ios::binary | std::ios::ate);
    if (!f) { why = "it could not be opened"; return false; }
    const std::streamoff end = f.tellg();
    if (end < 12) { why = "it is too small to be a font"; return false; }
    const uint64_t size = (uint64_t)end;
    unsigned char h[12];
    f.seekg(0, std::ios::beg);
    f.read((char*)h, 12);
    if (f.gcount() != 12) { why = "it could not be read"; return false; }
    const uint32_t tag = sfnt::be32(h);
    if (tag == 0x74746366u) {          // 'ttcf' - a collection of fonts
        const uint32_t numFonts = sfnt::be32(h + 8);
        if (numFonts == 0) { why = "it is a font collection holding no fonts"; return false; }
        if (12 + 4ull * numFonts > size) {
            why = "its collection header (" + std::to_string(numFonts) +
                  " fonts) does not fit in the file";
            return false;
        }
        // ...and a CEILING, because "it fits in the file" is not a bound worth
        // having here: a 40 MB file can claim ten million fonts and each one
        // costs a seek and a directory walk, so a start-up could be made to
        // spend minutes inside this check (review P3). Real collections hold a
        // handful - meiryo.ttc has 2, the largest shipped CJK collections are
        // in the tens - so 64 refuses nothing anyone has and bounds the work at
        // a few hundred reads.
        if (numFonts > 64) {
            why = "it claims " + std::to_string(numFonts) +
                  " fonts in one collection, which no real font file does";
            return false;
        }
        std::vector<unsigned char> offs((size_t)4 * numFonts);
        f.read((char*)offs.data(), (std::streamsize)offs.size());
        if ((size_t)f.gcount() != offs.size()) { why = "its collection header is truncated"; return false; }
        // Only the FIRST font is baked (ImFontConfig::FontNo defaults to 0), but
        // all of them are checked: stb_truetype is handed the whole buffer and
        // a collection whose other members are broken is a broken file.
        for (uint32_t k = 0; k < numFonts; k++) {
            const uint64_t off = sfnt::be32(offs.data() + (size_t)4 * k);
            if (off + 12 > size) { why = "one of its fonts starts past the end of the file"; return false; }
            if (!sfnt::directoryOk(f, off, size, why)) return false;
        }
        return true;
    }
    if (!sfnt::isWrapperTag(tag)) { why = "it has no sfnt signature"; return false; }
    return sfnt::directoryOk(f, 0, size, why);
}
// The old name, kept for the one caller that only wants a yes/no.
static bool looksLikeSfnt(const std::string& p) {
    std::string why;
    return sfntLooksSane(p, why);
}

// `want` is the user's override (appearance.fontPath / prefs.txt `fontPath`),
// empty when they have not set one. It is tried FIRST and it is not silently
// dropped when it misses: it lands in `tried` like everything else, so the
// refusal says "you asked for this and it is not there" rather than listing
// four paths the user never mentioned - and a path that is there but is not a
// font says THAT, which is a different sentence and a different mistake.
static std::vector<std::string> cjkMergeCandidates();   // fwd: the Chinese faces
// `skip` is the breadcrumb's list: paths the LAST start died while baking, so
// this start must not bake them again whatever else recommends them. Empty in
// the ordinary case. See fontBakeCrumb* in session.inc for why this exists.
static FontChoice jpFontChoice(const std::string& want,
                               const std::vector<std::string>& skip = {}) {
    FontChoice c;
    std::error_code ec;
    // Each attempt records WHY it failed, because "not found", "found, not a
    // font", "found, truncated" and "found, a format we cannot read" send the
    // user to four different places.
    auto tryPath = [&](const std::string& p, const char* how) {
        if (p.empty()) return false;
        if (std::find(skip.begin(), skip.end(), p) != skip.end()) {
            c.tried.push_back(p + " (skipped: the last start did not survive "
                                  "baking it)");
            c.skippedAfterCrash = true;
            if (strcmp(how, "appearance.fontPath") == 0) c.skippedWasOverride = true;
            return false;
        }
        if (!std::filesystem::is_regular_file(pathFromUtf8(p), ec)) {
            c.tried.push_back(p);
            return false;
        }
        std::string why;
        if (!sfntLooksSane(p, why)) {
            c.tried.push_back(p + " (unusable: " + why + ")");
            return false;
        }
        c.tried.push_back(p);
        c.path = p; c.how = how;
        return true;
    };
    if (tryPath(want, "appearance.fontPath")) return c;
    for (const std::string& p : jpFontCandidates())
        if (tryPath(p, "built-in candidate")) return c;
    // ---- the CHINESE faces, as a PRIMARY (review P2-8) ---------------------
    // They were only ever reachable as the second face, so the machine this PR
    // itself describes - an English Windows with no Japanese language pack -
    // had Microsoft YaHei sitting there and drew nothing at all, and the
    // refusal did not even list it. A Chinese face draws kana and the shared
    // ideographs perfectly well; what it gets wrong is the VARIANT of some
    // Japanese glyphs. That is much better than '?' for everything and much
    // worse than being told about it - so it is taken, and it is NAMED
    // (cjkFontChineseFallbackText, raised at start-up).
    for (const std::string& p : cjkMergeCandidates())
        if (tryPath(p, "Chinese face (no Japanese face on this machine)")) return c;
    // ...and fontconfig's answer goes through THE SAME GATE. It used to assign
    // c.path directly, which left the hole this whole change is about wide open
    // on the one arm that Linux and macOS actually reach: fc-match returns a
    // file that EXISTS, and says nothing about whether it is intact. A machine
    // with a half-copied font installed, or a stale fontconfig cache pointing
    // at a file that has since been replaced, would hand it straight to
    // stb_truetype. There is ONE gate, and every arm goes through it.
    if (std::string fc = fcMatchJpFont(); !fc.empty()) {
        const size_t before = c.tried.size();
        if (tryPath(fc, "fc-match")) {
            // Keep fontconfig's name for it in the record as well, so a reader
            // can tell "the list found this" from "fontconfig chose this".
            c.tried.back() = "fc-match sans:lang=ja -> " + fc;
            return c;
        }
        // IT ANSWERED AND THE ANSWER WAS REFUSED, which is not the same as "no
        // answer" (review P3). The record used to say the latter, which sent a
        // reader looking for a fontconfig problem when the problem was the file
        // fontconfig named. tryPath already appended the path and the reason;
        // this only marks whose suggestion it was.
        if (c.tried.size() > before)
            c.tried.back() = "fc-match sans:lang=ja -> " + c.tried.back();
        return c;
    }
#if !defined(_WIN32)
    c.tried.push_back("fc-match sans:lang=ja (no answer)");
#endif
    return c;
}

// The CHINESE faces this platform is expected to have (板306 B). Named as a
// list of its own because it has two uses now: the second face of the merge,
// and - when no Japanese face is on the machine at all - the primary
// (jpFontChoice, review P2-8).
//
// The reason a second file is needed on Windows is a measurement rather than a
// preference: the cmap tables in C:\Windows\Fonts say Meiryo holds 1819 of the
// 2500 common simplified-Chinese characters (72.8%), and 测 (U+6D4B) and 试
// (U+8BD5) - the two characters in the word "test" - are in NONE of Meiryo, Yu
// Gothic, MS Gothic or NotoSansJP-VF. Of the fonts on a stock Windows, the ones
// that carry the whole 2500 are the CHINESE faces listed here; the Japanese
// ones do not, which is the whole point.
//
// macOS has the same shape of gap for the same reason - Hiragino Kaku Gothic is
// a Japanese face - and the same answer: Hiragino Sans GB (or PingFang SC) is
// the Chinese face that ships with the system.
//
// Linux needs no second file: Noto Sans CJK is one pan-CJK font, and even the
// language-specific NotoSansCJK-JP-Regular.otf carries the whole ideograph set
// (the JP/SC/TC builds differ in which VARIANT is default, not in coverage).
//
// READ OFF THE LANGUAGE FIELD, not a second hand-written list - see
// cjkFontCandidates() for what that duplication cost.
static std::vector<std::string> cjkMergeCandidates() {
    std::vector<std::string> out;
    for (const FontCandidate& c : cjkFontCandidates())
        if (c.lang == LangZh) out.push_back(c.path);
    return out;
}

// ...and which one to MERGE on top of `primary`. `primary` is the face already
// added, so a machine that had no Japanese face and fell through to a Chinese
// one (above) does not then merge it onto itself.
//
// `skip` IS THE BREADCRUMB, and it belongs here as much as in jpFontChoice.
// Found by sweeping the shape "a path is chosen for baking without being
// checked against the crumb": the primary route honoured it and THIS one did
// not, so a machine whose SECOND face was the one that killed the process
// would have written a breadcrumb naming it, skipped nothing, and crashed
// again at every subsequent start - the exact failure the breadcrumb exists to
// end, surviving in the arm next to the one that was fixed.
static std::string cjkMergeFontPath(const std::string& primary,
                                    const std::vector<std::string>& skip = {}) {
    std::error_code ec;
    for (const std::string& m : cjkMergeCandidates()) {
        if (primary == m) continue;
        if (std::find(skip.begin(), skip.end(), m) != skip.end()) continue;
        if (std::filesystem::is_regular_file(pathFromUtf8(m), ec) && looksLikeSfnt(m))
            return m;
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
// this is not one font's bug to route around. There IS a second face in the
// atlas now (板306 B merges a Chinese font on Windows and macOS), and it does
// not help here for two reasons: the merge is first-come-first-served, so
// U+005C is claimed by the Japanese face before the second one is consulted,
// and the Chinese faces draw the same JIS-legacy yen sign anyway. Dropping
// U+005C from the ranges would leave the glyph missing rather than borrowed -
// a '?' where the separator should be.
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
// THE RANGES THIS START ACTUALLY ASKED FOR, kept so that "the font does not
// have it" and "this build never asked for it" can be told apart downstream
// (review P3). Filled by cjkFontRanges(); empty before start-up has run, which
// makes cjkRangeHas() answer false and the tally fall back to "not asked" -
// the conservative side, since it offers no cure that cannot work.
static ImVector<ImWchar> g_cjkRangesAsked;

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
// WHAT IS DELIBERATELY NOT HERE: Hangul, and the measurement is worth stating
// exactly because the obvious version of it is wrong. Adding
// GetGlyphRangesKorean takes the REQUESTED set from 5075 to 16298 code points -
// 3.2x - while on this machine the glyphs baked do not move at all, staying at
// 4963: neither Meiryo nor Microsoft YaHei has a Hangul glyph, so Windows would
// pay the whole request and get nothing back. The atlas that really grows is
// the one on a machine whose font HAS the syllables - Noto Sans CJK on Linux -
// where 11k+ more glyphs have to be rasterised at start-up and the texture
// grows with them. Either way it is cost for a script nobody has reported, so
// it is a separate row on the board and not a line here.
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
    g_cjkRangesAsked = out;      // see g_cjkRangesAsked for why this is kept
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
    // review P1: the chosen font had a correct sfnt signature and stb_truetype
    // still could not parse it (CFF2 is the case with a name). The numbers
    // above are then the BUILT-IN font's, because that is what got baked.
    bool buildFailed = false;
    // ...and the second face that had to be dropped for the first one to bake
    // at all. Empty in every ordinary case.
    std::string mergeDropped;
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

// WHAT IS ACTUALLY IN THE ATLAS, as the summary line says it (review P2).
// Pure, and named, because it is the one sentence a bug report is most likely
// to quote and it used to be wrong in the case that matters: after a failed
// build the atlas is made entirely of the BUILT-IN font, and the line still
// ended "from <path> [appearance.fontPath]" - so a reader who saw only this
// line concluded that file was in use. The failed file is still named, as the
// file that failed. It is the same mistake the merge path already avoided, and
// it needs the same answer.
static std::string fontAtlasFromText(const FontAtlasCost& c, const FontChoice& ch,
                                     const std::string& mergePath) {
    if (c.buildFailed) {
        std::string s = "(built-in font)";
        if (!ch.path.empty()) s += " after failing to read " + ch.path;
        return s;
    }
    if (ch.path.empty()) return "(built-in font)";
    std::string s = ch.path + " [" + ch.how + "]";
    if (!mergePath.empty()) s += " + " + mergePath;
    return s;
}

// `fontLoadFailed` = the chosen file did not even LOAD (AddFontFromFileTTF
// returned null), so the caller has already put the built-in font in its place.
// It is reported through the same field as a failed Build, because from the
// user's side the two are one fact: the font they asked for is not the one on
// screen (review P2).
static void fontAtlasBake(const ImVector<ImWchar>& ranges, float fontScale,
                          bool fontLoadFailed = false) {
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
    // ---- THE RETURN VALUE IS THE OTHER HALF OF THE HOLE (review P1) --------
    // Build() can FAIL, and it used to be discarded. It returns false whenever
    // stb_truetype cannot parse a file that nevertheless has a correct sfnt
    // signature, and the signature check in looksLikeSfnt() cannot see that:
    // the case with a name is CFF2, the outline format of the current variable
    // Noto CJK builds. An 'OTTO' file whose outlines live in a `CFF2` table has
    // neither `glyf` nor `CFF `, stbtt_InitFont returns 0, and ImGui's own
    // IM_ASSERT for it is compiled out of a release build - so the atlas came
    // back EMPTY and every character in the program drew as nothing, with the
    // failure reported nowhere.
    //
    // A failed Build also leaves the atlas half-initialised, so recovery is
    // Clear() and start again from the built-in font - which has no CJK but
    // does have ASCII, and an application that draws its own menus is one the
    // user can use to fix this. The font that failed is NAMED: see
    // cjkFontUnreadableText(), which is what start-up puts on screen.
    bool built = a->Build();
    // ---- IF THERE WERE TWO FACES, THE SECOND ONE MAY BE THE BAD ONE --------
    // (review, reference finding 2.) A failed Build used to throw away BOTH
    // faces and fall straight to the built-in font, so a broken Chinese face
    // cost the user their perfectly good Japanese one - and the message then
    // named the primary, which was innocent, and offered a fix for it that
    // could not help. So the primary is given one more chance on its own. If
    // that works the merge is simply gone, which is the ordinary state on
    // Linux and a survivable one everywhere.
    if (!built && !g_fontMergePath.empty() && !g_fontChoice.path.empty()) {
        fprintf(stderr, "font atlas: build failed with two faces - retrying with "
                        "%s alone (without %s)\n",
                g_fontChoice.path.c_str(), g_fontMergePath.c_str());
        a->Clear();
        if (a->AddFontFromFileTTF(g_fontChoice.path.c_str(),
                                  CJK_FONT_PX * fontScale, nullptr, ranges.Data) &&
            a->Build()) {
            built = true;
            c.mergeDropped = g_fontMergePath;   // what the message has to name
            g_fontMergePath.clear();
        }
    }
    c.buildFailed = !built || fontLoadFailed;
    if (!built) {
        fprintf(stderr, "font atlas: FAILED to build from %s - falling back to "
                        "the built-in font (no CJK)\n",
                g_fontChoice.path.empty() ? "(no font)" : g_fontChoice.path.c_str());
        a->Clear();
        // Clear() took the merged face with it, so the record has to stop
        // claiming one - the line below prints this, and a log that said
        // "+ msyh.ttc" after falling back to the built-in font would send the
        // next reader looking for a merge that is not there.
        g_fontMergePath.clear();
        ImFontConfig cfg; cfg.SizePixels = 13.0f * fontScale;
        a->AddFontDefault(&cfg);
        if (!a->Build()) {
            // The built-in font is compiled into the binary, so this cannot be
            // a missing file. Saying so beats an empty atlas nobody can explain.
            fprintf(stderr, "font atlas: the BUILT-IN font did not build either "
                            "- text will not draw\n");
        }
    }
    c.buildMs = (nowSec() - t0) * 1000.0;
    for (ImFont* f : a->Fonts) c.bakedGlyphs += f->Glyphs.Size;
    c.texW = a->TexWidth; c.texH = a->TexHeight;
    c.faces = a->ConfigData.Size;
    g_fontAtlasCost = c;
    // ...and WHICH FONT, on the same line (review P3-7). The merge has exactly
    // one silent failure mode - the wrong face first - and this is the only
    // place a log from a user's machine could ever show which files were in it.
    //
    // NAMING WHAT WAS ACTUALLY BAKED (review P2). When the build failed, this
    // line used to end "from <path> [appearance.fontPath]" for an atlas made
    // entirely out of the built-in font - so a reader who saw only this line
    // concluded that file was in use. It is the same mistake the merge path
    // already avoided two paragraphs up, and it needs the same answer: say
    // what is in the atlas, and name the file that failed as the file that
    // failed. `c.buildFailed` also covers the case where the font could not be
    // LOADED at all (AddFontFromFileTTF returned null), because main() records
    // that through the same field.
    const std::string from = fontAtlasFromText(c, g_fontChoice, g_fontMergePath);
    fprintf(stderr, "font atlas: %d glyph(s) of %d requested codepoint(s), "
                    "%dx%d px (%.1f MB RGBA), %d face(s), built in %.0f ms "
                    "(font scale %.2f, kana %s) from %s\n",
            c.bakedGlyphs, c.wantedCodepoints, c.texW, c.texH,
            c.texW * (double)c.texH * 4.0 / 1048576.0, c.faces, c.buildMs,
            fontScale, fontAtlasHasCjk() ? "present" : "MISSING", from.c_str());
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

// ...and the THIRD failure (review P1 / P2-9), which is not "no font" and is
// not "a font with no CJK": the file IS a font by its signature and
// stb_truetype could not parse it. CFF2 - the outline format of the current
// variable Noto CJK builds - is the case with a name. The whole atlas fails in
// that case, so what the user is looking at is the built-in font: ASCII only,
// no CJK, and every non-Latin character gone. Naming the file is the entire
// value of this message, because nothing else on screen can point at it.
static std::string cjkFontUnreadableText(const FontChoice& c) {
    return "this font could not be read (the file is a font, but not in a "
           "format this build can parse - a CFF2 variable font is the usual "
           "case): " + (c.path.empty() ? std::string("(none)") : c.path) +
           " (chosen by " + c.how + "). The built-in font is in use instead, so "
           "only Latin text will draw. Point \"appearance.fontPath\" in "
           "settings.jsonc (or `fontPath` in prefs.txt) at a .ttc/.ttf and "
           "restart.";
}

// ...and the FIFTH (review P1-A layer 2): the last start did not survive baking
// this file, so this start did not try it. The whole value of the sentence is
// the last clause - the way OUT - because the user's own setting is what is
// being overruled and only they can decide what to do about it.
static std::string cjkFontCrashSkipText(const FontChoice& c) {
    // Deduplicated: one path can be reached twice in a single search - as the
    // override and again as a built-in candidate - and naming the same file
    // twice in one sentence reads like two different problems.
    std::vector<std::string> names;
    for (const std::string& t : c.tried) {
        if (t.find("(skipped: the last start did not survive baking it)") ==
            std::string::npos) continue;
        std::string one = t.substr(0, t.find(" (skipped:"));
        // The fc-match arm records its answer as "fc-match ... -> /the/path",
        // so the same file reached twice - once as a candidate and once as
        // fontconfig's suggestion - escaped a dedupe that compared whole
        // strings. Measured on the Ubuntu runner, where the message named
        // DejaVu Sans twice. Compare the PATH.
        if (const size_t arrow = one.rfind(" -> "); arrow != std::string::npos)
            one = one.substr(arrow + 4);
        if (std::find(names.begin(), names.end(), one) == names.end())
            names.push_back(one);
    }
    std::string skipped;
    for (const std::string& n : names) {
        if (!skipped.empty()) skipped += "; ";
        skipped += n;
    }
    std::string s = "the last start did not survive reading this font, so it "
                    "was NOT used this time: " +
                    (skipped.empty() ? std::string("(unknown)") : skipped) +
                    ". This start is using " +
                    (c.path.empty() ? std::string("the built-in font") : c.path) +
                    " instead.";
    // THE WAY OUT HAS TO EXIST. "delete that line" is an instruction, and it is
    // false when what was skipped is a BUILT-IN CANDIDATE rather than the
    // user's own setting - there is no line to delete, and a reader who goes
    // looking for one learns nothing except that the message lies. Which case
    // this is, is known: `skippedWasOverride` is set where the skip happened.
    // (Same type as the Files-panel defect this round: never print an
    // instruction that cannot be carried out.)
    if (c.skippedWasOverride)
        s += " To fix it, point \"appearance.fontPath\" in settings.jsonc (or "
             "`fontPath` in prefs.txt) at a different file, or delete that line "
             "to go back to the built-in search.";
    else
        s += " That file is one this build looks for on its own, not something "
             "you set - if it is broken, replace it, or name a different font "
             "with \"appearance.fontPath\" in settings.jsonc (or `fontPath` in "
             "prefs.txt).";
    return s;
}

// ...and the SIXTH: the two faces would not bake together, the primary alone
// did, so the simplified-Chinese characters the second face was there to supply
// are gone. Not a refusal - Japanese names draw exactly as they should - but it
// must not be silent, because the ONE thing the merge existed for has stopped
// happening and nothing else on screen would say so.
static std::string cjkFontMergeDroppedText(const FontAtlasCost& c,
                                           const FontChoice& ch) {
    return "a second font could not be read, so it was left out: " +
           c.mergeDropped + ". " + ch.path + " is in use on its own, which draws "
           "Japanese correctly but has fewer simplified-Chinese characters - "
           "some Chinese filenames will show '?'. Replacing that file, or "
           "pointing \"appearance.fontPath\" at a font that covers both, fixes "
           "it at the next start.";
}

// ...and the sentence for a machine with NO Japanese face, where a Chinese one
// was taken as the primary (review P2-8). Not a refusal - the names will draw -
// but it must not be silent: 板306 B exists precisely to keep Japanese glyphs
// in a Japanese face, and this is the one path where that cannot be done.
// Saying it is the difference between a known compromise and a typeface that
// changed for reasons nobody can find.
static std::string cjkFontChineseFallbackText(const FontChoice& c) {
    return "no Japanese font was found on this machine, so " + c.path +
           " - a Chinese face - is being used for everything. Japanese text "
           "will draw in Chinese glyph shapes (the characters are right; some "
           "of the forms are not). Point \"appearance.fontPath\" in "
           "settings.jsonc (or `fontPath` in prefs.txt) at a Japanese font and "
           "restart.";
}
