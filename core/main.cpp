// viewer v0.1 — native image viewer for engineering data
// Features: .npy / .bin/.raw / .png/.jpg loading, hover pixel inspection, rulers,
//           zoom/pan, black/white point normalization.
#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION
  #include <OpenGL/gl3.h>            // 3.2+ core declarations
#elif defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>               // GL/gl.h needs APIENTRY/WINGDIAPI
  #include <GL/gl.h>
#else
  #include <GL/gl.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"          // DockBuilder for the default layout
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"
#include "plugin_host.h"
#include "miniz.h"                   // deflate for compressed .npz members
#include "ui_theme.h"
#include "version.h"                 // the commit this binary was built from
#include "remote.h"
#include "adapter.h"                 // running an input adapter (docs/input-adapters.md §4)
#include "imagefile.h"               // PNG / JPEG / TIFF, behind one seam
#include "remote_proto.h"
#include "app_icon.h"
#include "window_frame.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <csignal>
#include <fstream>
// low-level IO for the crash handler: it must not go through iostreams
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/wait.h>              // the detached double-fork in spawnDetached
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>           // sysctlbyname for the physical-memory budget
#endif
#include <memory>
#include <unordered_map>
#include <unordered_set>              // residentImageBytes: dedupe by srcId
#include <mutex>
#include <condition_variable>
#include <limits>
#include <iomanip>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>                      // wall-clock stamp on measurement results
#include <thread>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// ---------------------------------------------------------------- utilities
static std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::u8path(s);   // UTF-8 -> native (wide on Windows, bytes on POSIX)
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
static std::string baseName(const std::string& p) {
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
static std::string pathRootOf(const std::string& d) {
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
struct PathSeg { std::string label, path; };
static std::vector<PathSeg> pathSegments(const std::string& d) {
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
static float niceStep(float raw) {   // smallest 1/2/5*10^k >= raw
    float mag = powf(10.0f, floorf(log10f(std::max(raw, 1e-9f))));
    for (float m : {1.0f, 2.0f, 5.0f, 10.0f})
        if (m * mag >= raw) return m * mag;
    return 10.0f * mag;
}

// ---------------------------------------------------------------- image model
// Per-FRAME state: the pixels and their provenance (docs/reference-design.md
// §2.1). Held by ImageDoc through a shared_ptr so one frame can belong to
// several stacks from stage 2 on; in stage 1 every source has exactly one
// owner (use_count == 1) and behaviour is identical to the pre-split code.
// The platform's shortcut modifier, spelled two ways: SC_MOD for the label a
// menu shows, MODK for the chord a key test compares. They are declared
// together because a build where they disagree advertises one key and answers
// to another, and Browse needs MODK long before the menu bar is written.
#if defined(__APPLE__)
  #define SC_MOD "Cmd"
  static const ImGuiKeyChord MODK = ImGuiMod_Super;
#else
  #define SC_MOD "Ctrl"
  static const ImGuiKeyChord MODK = ImGuiMod_Ctrl;
#endif

static std::atomic<uint64_t> g_nextSrcId{ 1 };
struct FrameSource {
    std::vector<float> data;          // raw values, size w*h*ch
    int w = 0, h = 0, ch = 1;
    std::string dtype;
    float vmin = 0, vmax = 1;         // data min/max
    std::string path;
    std::string npzMember;            // array name when this came from a .npz
    int fileFrame = 0;                // frame index within a multi-frame LOCAL file
                                      // (npy frame axis; npz members too). With path +
                                      // npzMember it completes the provenance a reload
                                      // re-decodes - remoteFrame is the remote twin
    // How this array was READ (docs/input-adapters.md §3.1/§3.3). npyShape is
    // the shape the FILE declared, kept so the Inspector can compute which
    // other readings that shape permits instead of offering a fixed list; empty
    // = these pixels did not come from a .npy. npyRead is NR_NATIVE until a
    // user says otherwise, and then it is a DECLARATION, not a guess: it
    // survives reload and session, and it is what --npy-axis stopped being.
    // It is also registry identity (srcIdentityKey embeds the EFFECTIVE
    // reading), so two readings of one file are two tuples and never
    // silently share pixels.
    std::vector<int64_t> npyShape;
    int npyRead = 0;                  // NpyRead; 0 = NR_NATIVE
    // remote frames: opened as a decimated preview, replaced in place by the full
    // frame when the background fetch lands - after that, indistinguishable from
    // a local image. remoteStep > 1 means "still the preview".
    std::string remoteUrl;
    int remoteFrame = 0;
    int remoteStep = 1;
    std::string remoteErr;            // background fetch failed; preview is all we have
    // raw reload parameters (sessions + post-open reinterpretation; -1 = not raw)
    int rawDtype = -1, rawInterp = 0, rawOffset = 0;
    bool rawLE = true;
    // crop bookkeeping: srcW/srcH = full source dims (0 = unknown), cropX/Y = origin in source
    int srcW = 0, srcH = 0, cropX = 0, cropY = 0;
    uint64_t srcId;                   // frame identity: accounting dedupe (§7)
    int rev = 0;                      // +1 when the pixels are replaced wholesale (preview→full swap; later, reload)
    int64_t mtime = 0;                // Watch baseline: disk state at decode time; 0 = unknown
    uint64_t fsize = 0;
    // The key this source is REGISTERED under, "" = not registered. Registry
    // bookkeeping, not identity: srcKeyPath canonicalizes through the live
    // filesystem, so a key recomputed later (symlink retargeted, share
    // remounted) can differ from the one the slot was created under - erasing
    // by a recomputed key would then miss, leaving a stale row whose pixels a
    // later open could adopt. Erase by what was WRITTEN, not by what would be
    // written today. Maintained under g_srcRegMtx by the *Locked helpers.
    std::string regKey;
    FrameSource() : srcId(g_nextSrcId.fetch_add(1)) {}
};
// Watch baseline (§2.1): what was on disk when these pixels were decoded.
// Any failure (missing file, remote path) leaves 0 = unknown.
static void statSourceFile(FrameSource& s) {
    std::error_code ec;
    auto p = pathFromUtf8(s.path);
    auto t = std::filesystem::last_write_time(p, ec);
    s.mtime = ec ? 0 : (int64_t)t.time_since_epoch().count();
    auto sz = std::filesystem::file_size(p, ec);
    s.fsize = ec ? 0 : (uint64_t)sz;
}
// The local:// twin of statSourceFile: the url EMBEDS a path on this disk, so
// unlike a true remote the disk baseline is knowable - and it has to be known,
// because a local:// tuple with mtime/fsize 0 never moves: an overwrite on
// disk followed by a re-open of the same url would ADOPT the stale resident
// and silently show the old pixels. ssh:// urls stay 0/0 on purpose (a peer's
// disk cannot be statted from here; change detection there is Watch's).
static void statLocalUrl(FrameSource& s) {
    if (s.remoteUrl.rfind("local://", 0) != 0) return;
    std::error_code ec;
    auto p = pathFromUtf8(s.remoteUrl.substr(8));
    auto t = std::filesystem::last_write_time(p, ec);
    s.mtime = ec ? 0 : (int64_t)t.time_since_epoch().count();
    auto sz = std::filesystem::file_size(p, ec);
    s.fsize = ec ? 0 : (uint64_t)sz;
}
// A deep copy of s with its own identity: same content, fresh srcId, rev 0.
// The two callers are the crop CoW (§2.2) and any ImageDoc copy that must not
// alias a live source.
static std::shared_ptr<FrameSource> cloneSource(const FrameSource& s) {
    auto ns = std::make_shared<FrameSource>(s);   // same content...
    ns->srcId = g_nextSrcId.fetch_add(1);         // ...own identity
    ns->rev = 0;
    ns->regKey.clear();               // the clone is NOT the registered occupant
    return ns;
}

// ------------------------------------------------------------ source registry
// §6.2: one identity tuple -> at most one resident FrameSource. Loaders consult
// it before decoding; the same tuple already resident means share, not decode.
// Entries are weak: a source whose last membership closes disappears from here
// by itself, so the registry never keeps pixels alive.
//
// The tuple is (path-or-url, npzMember, frame-within-file, raw recipe, npy
// reading, mtime, fsize). mtime+fsize are the point: a file that changed on
// disk is a DIFFERENT tuple, so re-opening still reads the new bytes - sharing
// never overrides "re-open reads the disk". The npy reading (§3.3, normalized
// by npyKeyRead) keeps two READINGS of one file apart the same way. Remote
// sources carry mtime/fsize 0 and rely on the url; server-side change
// detection is Watch's (stage 6).
// Lock order: g_srcRegMtx is a LEAF lock - nothing else is ever acquired while
// it is held. It serializes (a) the map itself and (b) every cross-thread read
// or write of a REGISTERED source's shared fields: writers hold it across the
// whole mutation (reloadSource's field swap + rekey, cropInPlace), and the seq
// loader thread holds ONE continuous section from lookup through syncMirrors,
// so no reader can ever see a half-swapped source. Compound steps use the
// *Locked helpers to take it exactly once; never call a function that takes it
// itself (cropInPlace!) while holding it - reloadSource re-crops its PRIVATE
// fresh decode BEFORE locking for the swap.
// And NO IO under it: srcIdentityKey canonicalizes the path (srcKeyPath =
// weakly_canonical = filesystem round-trips - seconds on a UNC path with a
// slow or dead server), so every key is computed BEFORE the lock is taken;
// while it is held, only map and field operations happen. Identity fields are
// only ever written by the UI thread (load, reload swap, crop) or on a source
// no other thread can see yet, so a pre-lock read of them is never torn.
static std::mutex g_srcRegMtx;              // lookups also run on the seq loader thread
static std::unordered_map<std::string, std::weak_ptr<FrameSource>> g_srcRegistry;

// The same file reached as "C:\data\F.npy" and "c:/data/f.npy" is ONE tuple:
// canonicalize exactly once, HERE, where the key is built. The stored path is
// provenance (reload and sessions read it verbatim) and is never rewritten.
// URLs (ssh://, local://) pass through - they are not filesystem paths.
static std::string srcKeyPath(const std::string& p) {
    if (p.empty() || p.find("://") != std::string::npos) return p;
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(pathFromUtf8(p), ec);
    if (ec) return p;
    std::string s = c.make_preferred().u8string();
#ifdef _WIN32
    for (char& ch : s)      // Windows filesystems compare case-insensitively.
                            // ASCII fold only: it covers the drive letter and
                            // spelling divergences real callers produce, and
                            // identical non-ASCII bytes still fold identically.
        ch = (char)tolower((unsigned char)ch);
#endif
    return s;
}

// The reading that names this source's tuple (defined by npyLayout's §3.1
// machinery, below): NR_NATIVE stays 0 whatever the shape - a pre-decode
// probe knows no shape, and a native open must still find a native resident -
// and a DECLARATION that merely spells out what native would do anyway
// collapses onto 0 too, so "read as (H,W,C)" on a natively-HWC file and a
// plain open are ONE tuple. Only a declaration that actually changes the
// reading keys its own tuple.
static int npyKeyRead(const std::vector<int64_t>& shape, int npyRead);

static std::string srcIdentityKey(const FrameSource& s) {
    char t[160];
    if (s.rawDtype >= 0)    // the raw recipe (incl. its dims) decides the pixels
        snprintf(t, sizeof t, "\n%d|%d\nraw%d,%d,%d,%d,%dx%d\n%lld,%llu",
                 s.fileFrame, s.remoteFrame, s.rawDtype, s.rawInterp, s.rawOffset,
                 (int)s.rawLE, s.srcW, s.srcH,
                 (long long)s.mtime, (unsigned long long)s.fsize);
    else                    // npy/npz: the file bytes decide, mtime+fsize name
                            // them; npyRead (§3.3) keeps two READINGS of one
                            // file from collapsing onto one key - two readings
                            // are two different sets of pixels
        snprintf(t, sizeof t, "\n%d|%d\nnr%d\n%lld,%llu",
                 s.fileFrame, s.remoteFrame, npyKeyRead(s.npyShape, s.npyRead),
                 (long long)s.mtime, (unsigned long long)s.fsize);
    // npzMember is LENGTH-PREFIXED: a zip may name a member anything, newlines
    // included, and an unescaped name could forge the field boundaries of this
    // key - two different tuples reading back as one string
    return srcKeyPath(s.path.empty() ? s.remoteUrl : s.path) + "\n" +
           std::to_string(s.npzMember.size()) + ":" + s.npzMember + t;
}
// What may satisfy (or seed) a lookup: full-frame pixels that still mirror
// their origin. A crop re-scoped them; a decimated remote preview and a failed
// fetch are not the frame; no identity or no disk baseline means no tuple.
static bool srcShareable(const FrameSource& s) {
    if (s.path.empty() && s.remoteUrl.empty()) return false;   // montage/processed
    if (s.remoteStep > 1 || !s.remoteErr.empty()) return false;
    // no disk baseline = no tuple, and local:// counts as disk: its embedded
    // path is statLocalUrl-able, so a 0/0 local:// source is one that skipped
    // the stat - refusing it here fails SAFE (no sharing) instead of letting
    // an overwrite-then-reopen adopt stale pixels under a key that never moves
    const bool localDisk = s.remoteUrl.empty() ||
                           s.remoteUrl.rfind("local://", 0) == 0;
    if (localDisk && s.mtime == 0 && s.fsize == 0) return false;
    if (s.rawDtype >= 0)              // raw decodes always record srcW/srcH
        return s.w == s.srcW && s.h == s.srcH && s.cropX == 0 && s.cropY == 0;
    return s.srcW == 0;               // npy: srcW only appears once cropped
}
static std::shared_ptr<FrameSource> srcRegistryFindLocked(const std::string& key) {
    auto it = g_srcRegistry.find(key);
    if (it == g_srcRegistry.end()) return nullptr;
    std::shared_ptr<FrameSource> sp = it->second.lock();
    if (!sp) { g_srcRegistry.erase(it); return nullptr; }   // died with its last stack
    if (!srcShareable(*sp)) {         // e.g. cropped in place while sole-owned:
        if (sp->regKey == key) sp->regKey.clear();
        g_srcRegistry.erase(it);      // EVICT - a live occupant that stopped being
        return nullptr;               // the frame must not squat the tuple's slot
    }
    return sp;
}
// (no unlocked find wrapper on purpose: every reader needs more than the bare
// lookup - mirrors, or an add - inside the SAME hold, per the note above)
// Returns the live occupant when the tuple's slot is already taken - the
// caller ADOPTS it (the seq worker can finish a decode after the UI thread
// registered the same file: both stacks must end up sharing, not keep twin
// residents) - and nullptr when s was registered or is not shareable.
// `key` is srcIdentityKey(*s), computed by the caller OUTSIDE the lock
// (srcKeyPath does filesystem IO - see the lock-order note above).
static std::shared_ptr<FrameSource> srcRegistryAddLocked(const std::shared_ptr<FrameSource>& s,
                                                         const std::string& key) {
    if (!s || !srcShareable(*s)) { if (s) s->regKey.clear(); return nullptr; }
    std::weak_ptr<FrameSource>& slot = g_srcRegistry[key];
    if (std::shared_ptr<FrameSource> live = slot.lock()) {
        if (live == s) { s->regKey = key; return nullptr; }   // already the registered one
        if (srcShareable(*live)) {                 // first resident wins: adopt it
            s->regKey.clear();                     // s holds no slot
            return live;
        }
        // live but no longer the frame (cropped in place): reclaim the slot
        if (live->regKey == key) live->regKey.clear();
    }
    slot = s;
    s->regKey = key;
    // dead rows hold no pixels but would pile up over many open/close cycles;
    // sweep amortised, whenever the map has doubled since the last sweep
    static size_t sweepAt = 256;
    if (g_srcRegistry.size() >= sweepAt) {
        for (auto q = g_srcRegistry.begin(); q != g_srcRegistry.end();)
            q = q->second.expired() ? g_srcRegistry.erase(q) : std::next(q);
        sweepAt = std::max<size_t>(256, g_srcRegistry.size() * 2);
    }
    return nullptr;
}
static std::shared_ptr<FrameSource> srcRegistryAdd(const std::shared_ptr<FrameSource>& s) {
    if (!s || !srcShareable(*s)) return nullptr;
    const std::string key = srcIdentityKey(*s);   // IO: before the lock
    std::lock_guard<std::mutex> lk(g_srcRegMtx);
    return srcRegistryAddLocked(s, key);
}
// One membership: "this frame as seen by this stack". Pixels and provenance
// live in *src; what stays here is per-membership (identity, position, display
// range, interpretation) and per-view (texture) state.
struct ImageDoc {
    std::string name, dtype, note;
    int w = 0, h = 0, ch = 1;
    float vmin = 0, vmax = 1;         // data min/max
    float black = 0, white = 255;     // display range
    GLuint tex = 0;
    bool texDirty = true;
    float texBlack = 0, texWhite = 0;   // the range this texture was built with
    bool texNearest = true;
    // CFA (Bayer) metadata
    int batchId = 0;                  // which 塊 (Files header) this belongs to
    bool preview = false;             // transient: not yet a registered open
    int remoteFrames = 1;             // frame-axis count, kept for promotion
    int cfa = 0;                      // 0 none, 1 Bayer, 2 Quad Bayer
    int cfaPattern = 0;               // index into CFA_PATTERNS
    bool cfaColorize = false;
    int displayLut = -1;              // index into plugin_host::displays(), -1 = gray
    int dataRev = 0;                  // bumped on in-place pixel changes (crop)
    uint64_t uid = 0;                 // stable identity for caches (pointers ABA on reopen)
    int seqId = 0;                    // 0 = standalone, >0 = frame of that sequence
    int seqIndex = 0;                 // position within the sequence (file number order)
    float pendingViewScale = 1;       // full-res swap while NOT current: applied on select
    // A COMPUTED frame - today only a stack's frame average - has no file behind
    // it, so the session cannot round-trip it the way it round-trips an open.
    // What it CAN round-trip is the RECIPE: this holds the first-frame path of
    // the stack that was averaged (the key Series members already travel by,
    // for the same reason - paths round-trip, ids and names do not), and the
    // restore recomputes the mean once that stack is back. Empty on every frame
    // that came from a file, which is every other frame in the program.
    std::string avgOfPath;

    // w/h/ch/dtype/vmin/vmax above are per-doc MIRRORS of *src, kept as plain
    // fields because they are read on 240+ lines (docs/reference-design.md
    // §2.1). Only code that creates or mutates a source may write them, and it
    // must set both sides - normally by filling the source and calling this.
    // NB: copying an ImageDoc copies this POINTER - the copy ALIASES the same
    // source. A copy that must own its pixels takes cloneSource() (§2.2).
    std::shared_ptr<FrameSource> src = std::make_shared<FrameSource>();
    void syncMirrors() {
        w = src->w; h = src->h; ch = src->ch;
        dtype = src->dtype; vmin = src->vmin; vmax = src->vmax;
    }
    std::vector<float>&       px()       { return src->data; }
    const std::vector<float>& px() const { return src->data; }
    float sample(int x, int y, int c) const { return src->data[((size_t)y * w + x) * ch + c]; }
};

// §6.2, the UI-thread half: a freshly decoded doc either surrenders its pixels
// for the already-resident source with the same identity, or registers its own
// for the next open to find. Returns true when it adopted - callers then skip
// their computeMinMax (the resident source is already measured, and rewriting
// shared fields from here would race a loader thread reading them).
static bool shareOrRegisterSource(ImageDoc& d) {
    if (!srcShareable(*d.src)) return false;
    const std::string key = srcIdentityKey(*d.src);   // IO: before the lock
    // ONE hold of g_srcRegMtx across find + add + mirror sync: the worker's
    // probe and reloadSource's swap take the same lock, so an adoption can
    // neither interleave with a half-registered tuple nor capture a mid-swap
    // source (see the lock-order note at g_srcRegMtx)
    std::lock_guard<std::mutex> lk(g_srcRegMtx);
    if (std::shared_ptr<FrameSource> hit = srcRegistryFindLocked(key)) {
        if (hit != d.src) { d.src = std::move(hit); d.syncMirrors(); return true; }
        return false;                 // already the registered one (re-share path)
    }
    srcRegistryAddLocked(d.src, key);
    return false;
}

// CFA pattern tables: channel of each 2x2 cell position (cy*2+cx); 0=R 1=Gr 2=Gb 3=B
static const char* CFA_PATTERNS[] = { "RGGB", "BGGR", "GRBG", "GBRG" };
static const char* CFA_CH_NAMES[] = { "R", "Gr", "Gb", "B" };
static const int CFA_MAP[4][4] = {
    { 0, 1, 2, 3 },   // RGGB
    { 3, 2, 1, 0 },   // BGGR
    { 1, 0, 3, 2 },   // GRBG
    { 2, 3, 0, 1 },   // GBRG
};
static int cfaChannelAt(const ImageDoc& im, int x, int y) {
    if (im.cfa == 0) return -1;
    int cx = im.cfa == 2 ? (x >> 1) & 1 : x & 1;   // Quad Bayer: 2x2 blocks share a color
    int cy = im.cfa == 2 ? (y >> 1) & 1 : y & 1;
    return CFA_MAP[im.cfaPattern & 3][cy * 2 + cx];
}

struct ViewState {
    float zoom = 1.0f;
    ImVec2 center = ImVec2(0, 0);     // image px at canvas center
};

// Where the Browse toolbar's two width-critical controls ended up, in screen x,
// against the panel's own content edges. Recorded every frame so a selftest can
// assert the thing a human reads off a screenshot: "the filter is on screen".
// (Defined up here because every Browse INSTANCE carries one - see
// App::BrowseInstance below.)
struct RbToolbarGeom {
    // menuR was moreR until the drawer went away: the row's LAST permanent item
    // is the "..." panel menu now, and the contract it stands for is unchanged -
    // whatever ends the toolbar row must still be inside the panel at any width.
    float x0 = 0, x1 = 0, filterL = 0, filterR = 0, menuR = 0;
    float dateCellW = 0, dateTextW = 0;    // the listing's "modified" column
    int   emptyLocalBtn = 0;               // the not-connected state's local entry
    float rowX = 0, rowY = 0;              // centre of the first row submitted
    // ---- the bottom status line, and the star that ends the path line -------
    // The line is built as one string and then elided to fit, so the selftest
    // reads the LITERAL text the user sees rather than re-deriving it.
    std::string statusText;                // after elision: what is on screen
    std::string statusFull;                // before elision: the whole sentence
    int   statusShown = 0, statusTotal = 0, statusSel = 0;
    // what the drawn line MEASURES against the room it had. The line elides
    // middle-out and must never wrap into a second row, so textW <= availW is
    // the whole contract, checked at every width the geometry sweep uses.
    float statusTextW = 0, statusAvailW = 0;
    float listTopY = 0;                    // where the listing starts: a band
                                           // above it would push this down
    ImVec2 starCentre = ImVec2(-1, -1);    // the path line's bookmark star
    int   starLit = 0;                     // 1 = this place is bookmarked
    // ---- the ancestors held at the top of a scrolled tree -------------------
    // What the reader can still see of where they are. Recorded rather than
    // inferred because the whole claim this feature makes is about the SCREEN:
    // "the folder you are inside is laid out, at the top", and a flag saying
    // the feature is enabled would not have caught a band that drew nothing.
    int   pinnedRows = 0;                  // how many levels are held
    std::string pinnedNames;               // which, outermost first, ";"-joined
    // ...and where the outermost one landed, so an injected click can be aimed
    // at it. A pinned row that cannot be clicked would be a picture of a row.
    ImVec2 pinCentre = ImVec2(-1, -1);
};

// One array inside a .npz, CLASSIFIED BY SHAPE before anything is opened
// (docs/npz-design.md §2.1). An .npz is a container: the file is a batch, a
// member is a stack or a frame - and a member that is not pixels must never
// become pixels. (Defined up here because App carries the member picker; the
// scanner that fills these lives with the zip reader.)
struct NpzMember {
    enum Role {
        RImage,    // 2-D+ and interpretable: opens as a stack or a frame
        RAxis,     // 1-D numeric: the per-frame x axis, or metadata. NOT pixels
        RMeta,     // scalar / string / object dtype: provenance, never opened
        RAmbig,    // deeper than (F,H,W,C): which axis is frames? we do not guess
        RBad       // this viewer cannot read it, and says why
    };
    std::string name;         // array name, without the ".npy" the zip member has
    std::string shapeText;    // "(24, 480, 640)" / "scalar" - as WRITTEN
    std::string dtype;        // "u16" ... or the raw descr when unreadable
    std::string becomes;      // what it will become, in the picker's own words
    std::string why;          // why it is not an image (RAxis/RMeta/RAmbig/RBad)
    int role = RMeta;
    int frames = 1;           // RImage: frames of the stack it makes (1 = a frame)
    int w = 0, h = 0, ch = 1;
    std::vector<double> vals; // RAxis: every value, at FULL precision (never float)
    std::string text;         // RMeta: the value as text (fmtExact / the string)
    bool selected = false;
    size_t entry = 0;         // index into the zip listing
};

struct App {
    std::vector<std::unique_ptr<ImageDoc>> images;
    int current = -1;
    ViewState view;
    bool showGrid = false;
    float dispGamma = 1.0f;           // 1.0 or 2.2
    float uiScale = 1.0f;
    int themeVariant = ui_theme::VariantDark;   // View > Theme
    int themeAccent = 0;                        // index into ui_theme::accents()
    std::string toast; double toastUntil = 0; bool toastErr = false;
    bool fitRequested = false;
    std::unique_ptr<pfd::open_file> openDlg;   // polled each frame; never blocks render
    std::unique_ptr<pfd::save_file> saveDlg;
    std::unique_ptr<pfd::save_file> csvDlg;    // Analysis > Export curves (CSV)
    std::unique_ptr<pfd::save_file> texportDlg; // Temporal > Save (CSV)...
    std::unique_ptr<pfd::save_file> pngDlg;    // Image > Save view as PNG
    // One predicate for "an OS file dialog is pending". pollFileDialog only
    // runs inside a drawn frame, so a dialog missing from the idle loop's busy
    // set is polled only when some unrelated event happens to wake the loop -
    // which is how Export curves' CSV came to be written on the viewer's next
    // input rather than when the dialog closed. Defined out of line below,
    // next to folderDlg.
    std::string pendingCsv;                    // built at click time: the results may
                                               // change while the OS dialog is open
    std::string pendingTexport;                // Temporal export, same rule: built at
                                               // click time, written when the dialog lands
    bool showHelp = false, showAbout = false;
    // hover state (image coords, -1 = none)
    int hoverX = -1, hoverY = -1;
    bool hoverInB = false;            // cursor is over the B pane of a split compare
    // ---- unified annotations: ROIs (rect) and POIs (point), multiple of each ----
    struct Ann {
        int id = 0;
        int type = 0;                 // 0 = rect (ROI), 1 = point (POI)
        int x = 0, y = 0, w = 0, h = 0;
        std::string label;
        int color = 0;                // palette index
        bool visible = true;
        // pre-expansion rect memory for the X/Y band toggles (-1 = nothing saved)
        int prevX = 0, prevW = -1, prevY = 0, prevH = -1;
    };
    std::vector<Ann> anns;
    int nextAnnId = 1;
    int roiSeq = 0, poiSeq = 0;       // monotonic label counters (no dupes after deletes)
    int selectedAnn = 0;              // Ann::id; 0 = the built-in All (whole image) entry
    uint64_t annRev = 0;              // bumped on any annotation change
    bool annBusy = false;             // true while an annotation drag is in progress
    bool dragPans = false;            // left-drag on empty canvas: pan instead of new ROI
    int ctxX = -1, ctxY = -1;         // image coords the context menu was opened at
    // A/B compare: A is always the current image; B is a second open image shown
    // beside it (split) or under a draggable divider (wipe). Both panes use the
    // one shared view, so a pixel is at the same place in both.
    enum { CmpOff = 0, CmpWipe = 1, CmpSplit = 2, CmpDiff = 3, CmpFlip = 4 };
    // Blink comparator: same view, whole frame, alternating between A and B. The
    // eye finds a small difference far better from a flicker in place than from a
    // seam or from two images side by side.
    bool flipShowB = false;
    bool flipAuto = false;
    float flipPeriod = 0.5f;          // seconds per side
    double flipNext = 0;
    // Difference view: A-B as a signed map. Sign is hue, magnitude is brightness,
    // and anything beyond the scale is flagged, because a difference image with a
    // silently clipped scale is worse than no difference image.
    struct DiffTex {
        GLuint tex = 0;
        uint64_t uidA = 0, uidB = 0;
        int revA = -1, revB = -1;
        float gain = 0;
        bool absMode = false;
        int w = 0, h = 0;
        double clipped = 0;           // fraction of pixels outside +-gain
    } diff;
    float diffGain = 0;               // 0 = auto (99.9th percentile, rounded)
    float diffAutoGain = 1;
    bool diffAbs = false;
    int compareMode = CmpOff;
    // B is identified by uid: every frame of an in-file stack shares one name, so
    // a name would silently re-point B at another frame as A walks the stack.
    // The name (+ frame) is only the session fallback, resolved once on load.
    uint64_t compareBUid = 0;
    std::string compareB;             // B's name, for the session file
    int compareBSeq = -1;             // ...and its frame index, when B is in a stack
    // Stack-vs-stack compare: step A and B follows to the same frame NUMBER.
    // Without this, comparing two 300-frame captures pins B to whatever frame
    // it was on and every step compares against a stale image. Off when B is a
    // single reference image (a dark, a golden sample) - then B must not move.
    bool compareFollowFrame = true;
    // How the two sides' DISPLAY range relates while comparing. Non-destructive
    // throughout: B keeps its own numbers and gets them back when compare ends,
    // exactly the contract linkRange has.
    //   0 each own   - every side keeps its own stretch (shapes at wildly
    //                  different exposures; the only case where it is honest)
    //   1 B uses A's - one range, A's. Stable while stepping A.
    //   2 union auto - both sides re-fit to min/max ACROSS A and B at the
    //                  current frame pair, so neither clips and neither is
    //                  favoured. This is "auto, but the same auto for both".
    // The DEFAULT is 2. It used to be 1, and on two stacks captured at
    // different levels that renders B at >white 99.69%: B's whole histogram
    // collapses into one dashed line against the right edge of the plot, and
    // its half of a side-by-side pair is empty. A mode whose purpose is "see B
    // against A's reference" is not doing that when B saturates. The union
    // shows both distributions on identical bins, and clipping drops to 0.02%
    // on the same data. The cmprange pref persists a CHOSEN value, so only the
    // out-of-the-box one moves.
    int compareRangeMode = 2;
    // A/B statistics panels: how the two sides are laid out. ONE global setting,
    // not one per panel - "the same arrangement as the image" is a property of
    // the comparison, not of the histogram, and per-panel state would multiply
    // by five with no way to tell which panel is showing what.
    // Auto mirrors the image: CmpSplit puts the images left/right, so the plots
    // go left/right; wipe / blink / diff have ONE image area, so the plots overlay.
    enum { AbAuto = 0, AbOverlay = 1, AbSide = 2 };
    int abStatsLayout = AbAuto;
    // Slot 1 of every statistics cache is filled ONLY while compare is on. This
    // records whether it still holds anything, so compare-off invalidates it
    // exactly once and then costs nothing at all.
    bool abSlot1Live = false;
    // While frames are being stepped faster than a person reads them, the B
    // caches are NOT recomputed (see selectImage). nowSec() deadline.
    double abStepBusyUntil = 0;
    // Compare slots BEYOND B: C, D, E... An interim shape, deliberately bolted
    // ON TOP of A/B rather than replacing it - every existing mode (wipe,
    // split, diff, flip) still means exactly two images and still works
    // untouched. The extras are for the numeric side, which extends to N
    // without argument, and for side-by-side, which is the one image layout
    // that does. docs/todo-open.md item 19 holds the shape this should
    // eventually take.
    std::vector<uint64_t> cmpExtra;          // uids, in slot order (C, D, ...)
    uint64_t lastCompareBUid = 0;            // the B last CHOSEN, kept across compare being off
    struct SlotWant { int frame; std::string path; };
    std::vector<SlotWant> cmpSlotRestore;    // parsed from a session, not yet resolved
    int pendingCompare = -1;          // --compare, applied once two images exist
    uint64_t prevImageUid = 0;        // the doc looked at before this one (B default)
    bool prefsDirty = false;          // a preference actually changed in this run
    // The main window's geometry, remembered across runs (prefs.txt "window").
    // winW/winH are LOGICAL pixels and winX/winY desktop coordinates - the two
    // units are deliberate and are argued at fitSavedWindow. winW == 0 means
    // nothing has been saved yet. While the window is MAXIMIZED these hold the
    // rectangle to restore DOWN to, not the maximized one, which is why sampling
    // them stops the moment winMax goes true.
    int winX = 0, winY = 0, winW = 0, winH = 0;
    bool winMax = false;
    float inputLagMs = 0, inputLagMaxMs = 0;   // input event -> the frame answering it
    float cpuMs = 0, swapMs = 0;               // our drawing vs presenting it
    float wipeFrac = 0.5f;            // divider position, fraction of canvas width
    float splitFrac = 0.5f;
    bool wheelZoomPlain = false;// false: Ctrl+wheel zooms, plain wheel pans
    // analyzer plugin state: cached result grid (rows = keys, cols = ROIs)
    struct AnalysisState {
        const ImageDoc* img = nullptr;
        uint64_t uid = 0;
        int plugin = -1;
        int dataRev = -1, cfa = -1, cfaPattern = -1;
        float black = 0, white = 0;      // effective range is a plugin input
        uint64_t rev = (uint64_t)-1;
        std::vector<std::string> cols;                 // "whole" or ROI labels
        std::vector<std::string> keys;                 // row keys, first-seen order
        std::vector<std::vector<std::string>> vals;    // [row][col]
        struct Series {                                // curves from V2 analyzers
            std::string name, xLabel, yLabel;
            std::vector<float> xs, ys;                 // xs empty = use index
            int col = 0;                               // which ROI column produced it
            int colorIdx = -1;                         // annotation palette; -1 = neutral
        };
        std::vector<Series> series;
        std::string err;
        // provenance, built once per run: where measured, on what, over which
        // target, when, and how long it took. A screenshot of the panel must
        // answer all of that without the surrounding session.
        std::string prov;
        float runMs = 0;
        // presentation metadata, derived once per run (never in the draw loop):
        std::vector<int> colColor;             // ANN palette per column; -1 = neutral
        std::vector<std::string> units;        // per row; "" for text rows
        std::vector<char> headline;            // per row: the number the user came for
    } ana;
    bool anaAuto = false;
    int anaSel = 0;
    bool anaRunRequest = false;       // set by the Measure menu; consumed by the panel
    // user-set reference line on the curve plots (a spec limit, a pass line):
    // one horizontal y = value across every analysis plot, session-persisted
    bool anaRefOn = false;
    float anaRef = 0.5f;              // 0.5 = the MTF50 line on an SFR plot
    // host-built-in histogram cache (ROI-aware, CFA-split)
    struct HistState {
        const ImageDoc* img = nullptr;
        uint64_t uid = 0;
        int dataRev = -1;
        float black = 0, white = 0;
        int rx = -1, ry = -1, rw = -1, rh = -1;   // resolved ROI, not annRev
        int cfa = -1, cfaPattern = -1;
        int nSeries = 0;
        uint32_t bins[4][256] = {};
        uint32_t maxBin = 1;
        double clipLo = 0, clipHi = 0;
        size_t sampled = 0;
        bool roiUsed = false;
        double mean[4] = {}, sd[4] = {};      // always-on quick stats (same ROI/sampling)
        const char* seriesNames[4] = {};
    } hist[2];                        // 0 = A (the current frame), 1 = B (compare)
    // ...and one per extra compare slot (C, D, E...), indexed by cmpExtra
    // position - which IS the letter, and travels with the resolved document
    // (ResolvedSlot::idx), because after a follow-frame the document is no
    // longer the pinned uid and a uid lookup would lose the letter. Exactly the
    // shape projExtra and temporalExtra already have; the histogram was the one
    // statistics cache still stopping at two, so the Statistics panel could
    // only ever name A and B however many slots were armed.
    std::vector<HistState> histExtra;
    bool histLog = true;
    // Which plane the histogram draws: -1 = all, else the series index. Four
    // CFA planes times two sides is eight curves on one axis; the selector is
    // what makes a CFA overlay readable at all. Presentation only - the bins
    // are computed for every plane either way.
    int histPlane = -1;

    // ---- sequences (連番): a stack of frames that supports temporal analysis ----
    struct SeqInfo {
        int id = 0;
        std::string name;             // display name (pattern)
        int lastImageIdx = -1;        // last viewed frame, for stack switching
        // remote origin: lets the temporal panel measure server-side without all
        // frames being resident. remoteUrl for a frame-axis file; remoteFiles for
        // a folder-of-frames stack. expectedFrames = the true total from META.
        std::string remoteUrl, remoteHost;
        int remotePort = 0;           // non-default ssh port, or 0
        std::vector<std::string> remoteFiles;
        int expectedFrames = 0;
        int cfaType = 0, cfaPattern = 0;
        // Data revision of the stack AS A SET of pixels: the reload walk (§3.2)
        // bumps it whenever a member's source is swapped in place. Part of the
        // temporal cache key - (seqId, frames, ROI, CFA) alone cannot see a
        // reload that changes no shape (docs/reference-design.md §3.2).
        int stackRev = 0;
        // Per-frame X axis for the Temporal chart: what frame i physically IS
        // (elapsed time, exposure, temperature). NAME + UNIT + one value per
        // frame - a bare list of numbers cannot label an axis, so all three
        // are required, exactly as a series carries name/unit/value for its
        // stack-level parameter one layer up (docs/series-plan.md). This is
        // legitimately per-STACK (unlike `level`): the axis maps THIS stack's
        // frame numbers and nothing else. Empty axisVals = not set - and the
        // unit is never defaulted (unset is unset, never assumed).
        std::string axisName, axisUnit;
        std::vector<double> axisVals;     // axisVals[i] belongs to seqIndex i
        // NO level here. The value a stack was captured at is meaningless
        // without the parameter's NAME and UNIT, and both of those belong to
        // the series - so the value does too (Series::Member::value). Keeping
        // it on the stack is what forced "one unit for the whole application".
    };
    std::vector<SeqInfo> seqs;
    // A batch is the unit the Files panel groups by: created per OPEN ACTION
    // (not per folder - reopening the same folder makes a NEW batch), named
    // after the folder only as a starting value, renameable, session-saved.
    // It is the user's analysis grouping, deliberately decoupled from disk.
    // srcDir: the DIRECTORY loose opens reuse a batch for. Keying that reuse
    // on the leaf NAME made ~/runA/10lx and ~/runB/10lx one "10lx" batch -
    // two different measurements in two different directories, so Close batch
    // took files from a directory the user never named, and "Create a series
    // from this batch's stacks" offered two unrelated runs as one sweep with
    // every row pre-ticked. Empty for batches made by the pickers, which are
    // per OPEN ACTION by design.
    struct Batch { int id; std::string name; std::string srcDir; };
    std::vector<Batch> batches;
    int nextBatchId = 1;
    // ---- series (系列): the stacks of ONE swept parameter (docs/terminology.md) --
    // A batch makes no structural claim; a series does. Its members carry a
    // PARAMETER VALUE and an ORDER, and the whole run is one measurement - so
    // the parameter's name, its unit and the kind of fit live HERE, not on the
    // stack and not once per application.
    //
    // Series::members is the ONLY truth about membership. There is deliberately
    // no SeqInfo::seriesId: two places holding the same fact drift apart, and
    // the reverse lookup (seriesOfStack) is a walk over a handful of series.
    struct Series {
        enum { KLinearity = 0, KPtc = 1, KTemperature = 2, KOther = 3 };
        int id = 0, batchId = 0;      // the batch it lives in - strictly ONE
        std::string name;             // "<batch> 掃引" until a human names it
        std::string paramName;        // "illuminance" / "exposure" / ...
        // Empty = NOT SET, and that is the default: assuming "lx" would put a
        // unit on an axis nobody chose. No unit, no fit (see seriesCanFit).
        char unit[16] = "";
        int kind = KLinearity;
        // value NaN = not set. NEVER treated as 0 - it is left out of the fit
        // and reads as "unset" on screen.
        struct Member {
            int seqId = 0;
            double value = std::numeric_limits<double>::quiet_NaN();
            bool include = true;
        };
        std::vector<Member> members;  // order = display order (sorting is a button)
    };
    std::vector<Series> series;
    int nextSeriesId = 1;
    int curSeriesId = 0;              // which series the Linearity panel shows
    int loadBatchId = 0;              // batch newly opened images join; 0 = derive
    uint64_t previewUid = 0;          // the ONE reusable preview slot (0 = none)
    // Where that preview came from, so the browser can step through the
    // sequence without opening it: the group's member paths (one file per
    // frame), or the frame count of the ONE previewed file (frame axis), and
    // the index currently shown. Cleared with the preview.
    std::vector<std::string> previewFiles;
    int previewFrames = 0;
    int previewIndex = 0;
    std::string previewLabel;
    // ...and which machine those paths live on. The preview slot is GLOBAL -
    // one across every Browse instance (the app's one throwaway; two instances
    // previewing = last click wins, exactly like clicking twice in one panel) -
    // so the stepper cannot ask "the" browse panel for its host any more.
    std::string previewHost;
    int previewPort = 0;
    int nextSeqId = 1;
    uint64_t nextUid = 1;
    uint64_t imagesRev = 1;           // bumped whenever the image list changes
    int seqLoadMode = 0;              // 0 = ask, 1 = always, 2 = never
    int rangeScope = 1;               // value range: 0 frame, 1 stack, 2 all
    float memBudgetGB = 0;            // 0 = auto (60% of physical RAM)
    // remote viewing: one peer process per host, reached over ssh.
    // OWNERSHIP - remote::Session is documented not-thread-safe (remote.h), so
    // every Session in this program has exactly ONE thread that touches it:
    //   BrowseInstance::session -> that instance's browse worker (rbWorker),
    //                 and nobody else. Not even a read: peerVersion /
    //                 bytesReceived / alive are PUBLISHED into that instance's
    //                 RemoteBrowse by the worker (RbResult) and the UI reads
    //                 those plain values. One session per instance, one worker
    //                 per instance - the singleton rule "one Session, one
    //                 owning thread" generalises to "per place being viewed".
    //   uiSession  -> the UI thread (openRemote's meta+tile), and nobody else.
    //   rfWorker / mWorker each hold their own function-local Session.
    // A mutex only one of two racing parties takes protects nothing, which is
    // what the old shared app.remoteSession + app.sesMtx amounted to.
    remote::Session uiSession;                     // the UI thread's, exclusively
    std::string remoteExe;            // empty = ~/.viewer/viewer-serve (self-installed)
    std::string exePath;              // argv[0], for the local:// test peer
    bool remoteDlgOpen = false;       // File > Start Remote (ssh)...
    std::string lastRemoteUrl;        // last host, prefilled next time (prefs)
    // A connected server, browsable in the Files panel. Connect first, then look
    // around - which is why nobody has to know the path shape up front.
    struct RemoteBrowse {
        bool connected = false;
        bool autoUpdateTried = false; // one shot per connect: no update loops
        std::string host, dir = "~", err;
        int port = 0;
        std::vector<remote::Entry> entries;
        // Published by the worker with every result (RbResult), so no draw-path
        // code ever names the Session. peerVersion gates the metadata columns
        // and the MEASURE button; rxBytes feeds the status-bar tooltip.
        int peerVersion = 0;
        uint64_t rxBytes = 0;
        // "Search under here" stores an ABSOLUTE path on THIS machine. It was a
        // function-local static of drawPanelRemote, and every state-replacing
        // path (the disconnect button, goToPlace) assigns `App::RemoteBrowse{}`
        // - so entries, dir, host and the tree cache were forgotten on a host
        // change and this string was not: Search on machine B stayed rooted at
        // machine A's folder. Living here makes "reset on host change" true by
        // construction, for this field and for the next one.
        std::string searchRoot;
        // Bumped on every listing REPLACEMENT. The row-selection and cursor
        // caches keyed on host|dir|entry-COUNT, so a refresh that added one
        // file and removed another kept row-indexed selection flags pointing
        // at different files - and "Open N selected as stack" then acted on
        // files the user never picked.
        uint64_t rev = 1;
        // The cancel token for SCAN, as rbSearch.gen is for GLOB. A cancelled
        // scan's late reply used to reach openPickerWith, which does not merely
        // raise a modal - it CLEARS app.folderPick and resets the root, filter,
        // merge and sweep fields, destroying a local Open Folder selection the
        // user was in the middle of filtering.
        uint32_t scanGen = 0;
    };
    // The connect / install / list sequence runs on the INSTANCE's worker, never
    // on the UI thread: a git clone on the far side takes seconds, and "Connect
    // froze the app" is precisely the bug class this tool exists to avoid.
    // RbTreeList is an ordinary LIST whose answer lands in the tree cache
    // instead of replacing the listing: expanding a node must not navigate.
    // RbDisconnect stops the session on the thread that OWNS it: the status bar
    // must never call stop() on the worker's Session from the UI thread.
    enum RbKind { RbConnect = 0, RbList = 1, RbUpdatePeer = 2, RbScan = 3, RbGlob = 4,
                  RbTreeList = 5, RbDisconnect = 6 };
    struct RbJob {
        int kind = RbConnect;
        std::string host, dir;
        int port = 0;
        std::string pattern;          // RbGlob only
        uint32_t gen = 0;             // RbGlob: Stop bumps it, stale results drop
        std::string exe;              // frozen at enqueue: see MJob
    };
    struct RbResult {
        int kind = RbConnect;
        bool ok = false;
        std::string err, host, dir, info;
        int port = 0;
        std::vector<remote::Entry> entries;
        // RbScan payload: every stack under dir, plus how the walk ended
        std::vector<remote::ScanGroup> scanGroups;
        bool truncated = false;
        int skippedDirs = 0;
        // RbGlob payload
        std::vector<remote::GlobHit> hits;
        uint32_t gen = 0;
        // Everything the UI would otherwise have to ask the Session for, read
        // by the thread that OWNS it and carried home as plain values.
        int peerVersion = 0;              // what the peer answered in HELLO
        bool alive = false;               // is the session still up AFTER this job?
        uint64_t rx = 0;                  // bytes received so far
    };
    // Stacks a remote folder scan decided to open, one at a time: the next
    // stack starts only when the fetcher is idle, so the memory budget each
    // openRemoteStack computes reflects what the previous one actually loaded.
    // name: folder-qualified stack name ("10lx/frame_###.npy") - seven stacks
    // all called frame_000.npy would be indistinguishable, and the linearity
    // Auto-levels reads the level from exactly this folder prefix.
    struct RemoteOpen { std::string host; std::vector<std::string> files;
                        std::string name; int batchId = 0; int port = 0;
                        int token = 0; };
    std::vector<RemoteOpen> rbOpenQueue;
    // Places: starred host+path urls, and the last ~10 visited (most recent
    // first). Both persist in prefs - a lab machine's data layout outlives any
    // one session.
    std::vector<std::string> rbBookmarks;
    std::vector<std::string> rbRecents;
    // Server-side recursive find (GLOB). gen is the cancel token: Stop bumps
    // it and the worker's result is dropped on arrival (the walk itself is
    // bounded by depth and result caps, so there is nothing to interrupt).
    struct RemoteSearch {
        bool active = false;          // results view instead of the listing
        bool running = false;
        uint32_t gen = 0;
        std::string root, pattern;
        std::vector<remote::GlobHit> hits;
        bool truncated = false;
        int skippedDirs = 0;
    };
    // ==== ONE Browse instance: a view onto ONE place =========================
    // Decision record: docs/todo-open.md item 17 ("instance-able views").
    // The Browse panel stopped being a singleton: every instance holds a place
    // (RemoteBrowse), the listing state that used to be ~21 function-local
    // statics of drawPanelRemote and five g_rb* globals, and its OWN worker
    // thread with its OWN Session. Local vs remote is where an instance
    // STANDS, never a panel type - the place vocabulary of
    // docs/browse-as-file-manager.md survives, only the panel count changed.
    struct BrowseInstance {
        // identity. num 1 is the primordial instance: it wears the original
        // ImGui id "Browse###Remote" (existing layouts and the dock builder
        // keep working) and is never destroyed, only hidden via app.showRemote
        // - the last Browse closing behaves like the panel being hidden, so
        // there is always a Browse to reopen. num >= 2 wear "Browse N###BrowseN"
        // (stable across sessions: the number is saved) and are destroyed when
        // their window closes.
        int num = 1;
        std::string wtitle;           // the ImGui window name, fixed at creation
        bool open = true;             // window shown (num 1 mirrors app.showRemote)
        bool focusReq = false;        // bring the window forward next frame
        // Where "+" wants this panel to appear: the dock node of the panel whose
        // "+" was clicked, so a new Browse is a TAB beside the one you were
        // looking at rather than a floating window somewhere else. Applied once,
        // then cleared - after that the panel is wherever the user put it.
        unsigned dockInto = 0;
        // the place, and everything listed there
        RemoteBrowse b;
        RemoteSearch search;
        std::string pendingOpen;      // a pasted url, opened once the session is up
        // ---- the worker: one thread, one Session, per instance --------------
        // The Session is touched by THIS thread only; the UI reads the plain
        // values the worker publishes (RbResult). Destroying an instance joins
        // the thread first, so the Session's destruction is single-threaded too.
        std::unique_ptr<remote::Session> session;   // the worker's, exclusively
        std::thread thread;
        std::mutex mtx;               // guards queue / done / phase
        std::condition_variable cv;
        std::vector<RbJob> queue;
        std::vector<RbResult> done;
        std::string phase;            // what the worker is doing, for the UI
        std::atomic<bool> busy{ false };
        std::atomic<bool> stop{ false };
        // ---- tree mode: this instance's expanded nodes and their children ---
        std::map<std::string, std::vector<remote::Entry>> treeCache;
        std::vector<std::string> expanded;     // absolute dirs currently open
        std::vector<std::string> treePending;  // ...and those still being listed
        int treeLists = 0;                     // node LISTs issued (selftest)
        // ---- navigation history (mouse back/forward, Alt+Left / Alt+Right) --
        std::vector<std::string> histBack, histFwd;
        std::string histKey;          // host:port the history belongs to
        bool histNav = false;         // set while back/forward itself navigates
        // ---- the shape of the listing. Per instance; app.rbFlat / rbTree /
        // rbNatural remain the persisted DEFAULTS a new instance starts from,
        // and a toggle writes through to them (last toggle wins next start).
        // (`advanced` - was the "more" drawer open - went with the drawer.)
        bool flat = false, tree = false;
        // ...and the order the NAME column puts rows in. Natural by default
        // (rp::naturalLess): see rbNameCmp for why the listing gets a choice
        // here and a stack never does.
        bool nameNatural = true;
        // ---- panel state, formerly function-local statics of drawPanelRemote.
        // keyboard cursor (row index into the built view, -1 = none)
        int cursor = -1;
        bool cursorScroll = false;    // bring it into view this frame
        // A click GESTURE that navigated: how many clicks of it have landed so
        // far (0 = no such gesture in flight). Everything after that click
        // belongs to the navigation and not to the listing that replaced the
        // one it was aimed at - see rbNavGesture in drawPanelRemote.
        int navChain = 0;
        std::string curSig;           // host|dir|rev the cursor was built for
        bool curFlat = false, curTree = false;
        // sort spec, stashed from the table one frame late (see RB_COL_NAME)
        int sortCol = 0;              // RB_COL_NAME
        bool sortDesc = false;
        // multi-select, by row index; sig says which listing it was built for
        std::vector<char> sel;
        int selAnchor = -1;
        bool selFlat = false, selTree = false;
        std::string selSig;
        // filter / search / path editing. ONE buffer: the filter box is also the
        // search box, and Enter is the difference between "narrow these rows"
        // and "walk below this folder". searchBuf/searchFocus fed a second,
        // identical-looking box in a popup and went with it.
        char filter[256] = "";
        bool searchOpen = false;      // put the caret in the filter box next frame
        char pathEdit[1024] = "";
        bool pathEditing = false, pathFocus = false;
        // Properties popup: a snapshot, because the row may scroll out of the
        // clipper (or the listing may refresh) while the popup is up
        remote::Entry propsEntry;
        std::string propsPath;
        bool propsOpen = false;
        bool propsNoSize = false;     // an expanded frame: no size/mtime of its own
        // deferred panel actions - see rbDefer. Per instance: the queue fills
        // only while THIS instance's rows are alive and drains when they die.
        std::vector<std::function<void()>> deferred;
        // selftest probes: where the toolbar and the cursor row landed on screen
        RbToolbarGeom toolbar;
        ImVec2 cursorRect[2] = { ImVec2(0, 0), ImVec2(0, 0) };
        std::string cursorName;
        // ...and its FULL path, which is the key the tree's `expanded` set is
        // written in: "is the cursor row expanded" cannot be asked with a bare
        // name, and counting the whole set answers a different question.
        std::string cursorFull;
        // ...and the centre of the cursor row's chevron hit zone (tree dir
        // rows only; x < 0 = the row has no chevron), for "chevclick"
        ImVec2 cursorChev = ImVec2(-1, -1);
        // The listing's row PITCH, measured from the rows the clipper actually
        // laid out. The pinned-ancestor band has to know which row is at the
        // top of the viewport BEFORE it submits anything (ImGui wants the
        // frozen-row count before the first row), so it divides the scroll
        // offset by this instead of asking the clipper - which has not run yet.
        // Measured, not derived: a row is a Selectable inside a table cell, and
        // reconstructing that height from font size and two paddings is a
        // second copy of ImGui's arithmetic that would drift from it.
        float listRowH = 0;
    };
    // Never empty once rbMain() ran; [0] is always the primordial instance.
    std::vector<std::unique_ptr<BrowseInstance>> browsePanels;
    bool showRemoteError = false;     // the full failure text, on demand
    // (there is deliberately no session mutex here any more: see the ownership
    // note on session/uiSession above. Nothing is shared, so nothing locks.)
    // where analysis runs. auto: remote data -> server, local -> local. server:
    // even a resident frame is measured server-side (one engine for a whole
    // batch). local-fetch: never use the server for compute (today's behavior).
    // PolServer (1) is RETIRED. It promised "even a resident frame is measured
    // server-side", and nothing implemented it: serverComputes - the only
    // functional read of procPolicy in the program - is
    // `procPolicy != PolLocalFetch`, so PolServer and PolAuto took the same
    // branch everywhere, and the Analysis panel (where a single-frame
    // measurement would be routed) never reads procPolicy at all. Nor is it
    // implementable as promised with today's protocol: the peer reads files by
    // PATH on the server, so "measure a LOCAL frame on the server" has nothing
    // to measure there. A persisted, CLI-settable setting labelled "measure on
    // the server" that is byte-for-byte equivalent to auto is worse than a
    // missing one, so the menu item is gone and the value folds into auto
    // wherever it is read back.
    enum { PolAuto = 0, PolServerRetired = 1, PolLocalFetch = 2 };
    int procPolicy = PolAuto;
    // background MEASURE worker (own ssh connection: a 300-frame aggregate must
    // not stall the tile fetches). Results carry provenance for the panel.
    // `exe` is the same snapshot RFetchJob carries and for the same reason
    // (main.cpp: "how the peer is invoked, frozen NOW: the UI thread edits
    // remoteExe freely"). A worker reading app.remoteExe while the Start Remote
    // dialog assigns it is a data race on a std::string.
    struct MJob { std::string url; int op; uint64_t token; std::vector<std::string> files;
                  int cfaType = 0, cfaPattern = 0; float black = 0, white = 1;
                  int rx = 0, ry = 0, rw = 0, rh = 0; std::string exe; };
    struct MDone { uint64_t token; bool ok = false; std::string err, host;
                   remote::MeasureResult res; };
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<int> mPending{ 0 };
    std::mutex mMtx;
    std::condition_variable mCv;
    std::vector<MJob> mQueue;
    std::vector<MDone> mDone;
    // last server temporal result, keyed to the stack it describes
    // Linearity: one row per MEMBER of one series (value in, response out), fits
    // per CFA plane. Recomputed only on demand - this walks hundreds of frames.
    struct LinState {
        struct Row {
            int seqId = 0;
            std::string name;
            double level = std::numeric_limits<double>::quiet_NaN();
            bool include = true, valid = false, pending = false;
            int frames = 0, nPl = 1;
            double mean[4] = {}, sigmaT[4] = {};
            std::string err;
        };
        std::vector<Row> rows;
        int seriesId = 0;                 // the series these rows describe
        // NOT the unit of any measurement: the PREFILL a newly created series
        // starts from (prefs key linunit, unchanged). What gets printed on an
        // axis comes from Series::unit, and empty there means "not set" - it
        // never silently becomes "lx".
        char unit[16] = "lx";
        int tablePlane = 0;               // which CFA plane the per-stack table shows
        bool snrDb = false;               // SNR curve y axis: ratio (default) or dB
        bool fitValid = false, roiUsed = false;
        bool readFromDark = false;        // read noise measured, not extrapolated
        int nPl = 1, nPts = 0;
        double slope[4] = {}, offs[4] = {}, r2[4] = {}, leMax[4] = {};
        double ptcK[4] = {}, ptcRead2[4] = {}, readDN[4] = {};
        uint64_t rev = 0, computedRev = 0;
    } lin;
    bool showLinearity = false;
    // Create / edit a series. ONE modal for both: the fields are identical and
    // "edit" is only "create with the boxes already ticked". The value column
    // is a SUGGESTION the user confirms - see drawSeriesModal.
    struct SeriesEdit {
        bool open = false;
        int editId = 0;                   // 0 = creating a new one
        int batchId = 0;
        char name[128] = "";
        char param[64] = "";
        char unit[16] = "";
        int kind = Series::KLinearity;
        struct Row {
            int seqId = 0;
            bool check = true;
            // TEXT, not a double: "" has to stay distinguishable from 0, and a
            // numeric box cannot be empty. Parsed on accept; empty = NaN = unset.
            char value[32] = "";
            bool suggested = false;       // still extractLevelFromName's proposal
            // An EXISTING member's value, kept as the double it is. The box shows
            // it as "%.6g" and a Save that never touched the box must give the
            // member back UNCHANGED - re-parsing the display text would quietly
            // round 1234567.89 to 1234570 on every visit to the dialog.
            double orig = std::numeric_limits<double>::quiet_NaN();
            bool haveOrig = false;        // this row is already a member
            bool touched = false;         // a human typed in the box
            // extractLevelFromName's proposal when it is NOT in the box: offered
            // in the "read from" column, where it cannot be committed by accident.
            double guess = std::numeric_limits<double>::quiet_NaN();
            std::string name, from;       // from = the text the number was read out of
        };
        std::vector<Row> rows;
    } seriesEdit;
    int forceCfa = -1, forceCfaPattern = 0;   // --cfa: how 1ch files arrive
    // Open by default (2026-08-03, user): Browse is how a file is found, and a
    // tool you have to summon through the OS dialog it replaces is the wrong
    // way round. The saved layout wins after the first run - close it and it
    // stays closed, because layout.ini remembers.
    bool showRemote = true;           // the server browser, its own panel
    // Browse listing view: false = the collapsed group rows the peer sends,
    // true = every frame of every sequence as its own row. Expansion is a pure
    // CLIENT-SIDE view over the same reply (the peer always sends `.members`),
    // so the toggle costs no round trip. Persisted: it is a way of working.
    bool rbFlat = false;
    // (rbAdvanced - whether the Browse header's "more" drawer started open -
    // is gone with the drawer. Its contents each moved to the place they are
    // about: docs/browse-topbar-design.md 10.2.)
    // Tree mode: a directory expands IN PLACE instead of replacing the listing,
    // so a folder of folders can be compared without losing your place. LAZY -
    // expanding a node costs exactly one LIST, issued on the browse worker and
    // never on the UI thread, and collapsing KEEPS the answer: re-expanding is
    // free. Cache keyed by absolute path, so it survives navigation; dropped
    // when the machine changes or on an explicit refresh.
    bool rbTree = false;                                        // persisted
    // (the tree cache, expanded set and pending list live per Browse instance
    // now - BrowseInstance::treeCache / expanded / treePending)
    // Name order in the listing: true = natural (frame_2 before frame_10, the
    // order a stack is built in and the order the peer folds a scan in),
    // false = plain lexicographic. Persisted, and the DEFAULT a new panel
    // starts from - the panel that changes it changes only itself, exactly as
    // rbFlat / rbTree do. There is no Preferences panel to put it in (#50).
    bool rbNatural = true;                                      // persisted
    bool focusRemote = false;         // bring the ACTIVE Browse instance forward
    bool focusTemporal = false;       // ditto for Temporal (browser-fired stats)
    struct Msg { std::string text; bool err; };
    std::vector<Msg> msgLog;          // every toast, kept so it can be copied
    // msgLog is capped, so its SIZE stops changing once the cap is reached and
    // cannot be used to notice that it changed. This counter only ever goes up.
    size_t msgSeq = 0;
    bool showMessages = false, msgUnreadErr = false;
    struct ServerTemporal {
        uint64_t token = 0;           // matches the MJob that produced it
        int seqId = -1;               // owning stack; -2 = detached (browser-fired
                                      // aggregate over files nobody opened)
        bool valid = false, pending = false;
        bool detached = false;        // browser origin: no SeqInfo backs this
        std::string host, err;
        std::string label;            // what was measured ("10lx/frame_???.npy")
        std::vector<std::string> files;   // detached only: for "Open as stack"
        int frames = 0;
        // The REGION the server actually measured. requestServerTemporal sends
        // no ROI, so this is the whole frame - and it has to be recorded rather
        // than assumed, because the panel used to label these numbers with the
        // LOCAL side's roiUsed flag and so claimed a region they never had.
        int rx = 0, ry = 0, rw = 0, rh = 0;   // rw/rh 0 = whole frame
        bool roiUsed = false;
        // Per CFA plane, because the peer answers per plane when it was told
        // the mosaic (serve.cpp planeKey). nPl == 1 means the reply was pooled
        // - which the panel then has to SAY, not hide.
        int nPl = 1;
        double tempNoise[4] = {}, fixedPattern[4] = {}, totalNoise[4] = {}, mean[4] = {};
        std::vector<float> idx, frameMean, frameStd;
    } srvTemporal;
    // The same, for the compare B side. NEVER fired automatically: a server
    // aggregate is a real job on a real machine, and B following A around
    // would double them silently. The Temporal panel's "Measure B" button is
    // the only thing that fills this.
    ServerTemporal srvTemporalB;
    // background loader
    std::thread seqThread;
    std::atomic<bool> seqCancel{ false };
    std::atomic<bool> seqRunning{ false };
    std::atomic<int> seqDone{ 0 }, seqTotal{ 0 };
    std::mutex seqMtx;
    std::vector<std::pair<int, std::unique_ptr<ImageDoc>>> seqReady;   // (seqIndex, frame)
    std::string seqErr;               // guarded by seqMtx
    std::string seqNote;              // last loader message, kept on screen (UI thread)
    int seqLoadingId = 0;
    size_t seqBytes = 0;
    // background full-resolution fetch for remote frames. Frame granularity on
    // purpose: once the full frame lands, a remote image IS a local image - no
    // tile bookkeeping, no partial state, no coordinate mapping to maintain.
    // uid != 0: replace that doc's pixels (preview -> full swap).
    // uid == 0: a NEW frame of stack seqId - the remote prefetch, which is how a
    // server-side folder becomes an ordinary local stack, one frame at a time.
    struct RFetchJob {
        std::string url, name;
        std::string note;             // the head frame's note, copied verbatim onto
                                      // this one: a stack-constant Inspector row
                                      // (see openRemote, materializeDerivedFrame)
        std::string exe;              // snapshot at enqueue: the UI may edit
                                      // remoteExe while the worker connects
        int frame = 0;
        uint64_t uid = 0;
        int seqId = 0, seqIndex = 0;
        uint32_t gen = 0;             // closeAll bumps the generation; stale
                                      // results must not graft onto a new list
        bool low = false;             // preview buffering: yields to registered opens
        size_t bytes = 0;             // what this frame will occupy once resident
        int64_t mtime = 0;            // local:// only: the disk baseline statted at
        uint64_t fsize = 0;           // ENQUEUE (identity before bytes) - the minted
                                      // sibling binds to the OPEN's stat epoch
    };
    struct RFetchDone {
        uint64_t uid = 0;
        int w = 0, h = 0, ch = 0;
        float vmin = 0, vmax = 1;     // computed on the worker: 12M floats is a
                                      // visible hitch on the UI thread
        std::string dtype, err;
        std::vector<float> data;
        std::string url, name, note;
        int frame = 0, seqId = 0, seqIndex = 0;
        uint32_t gen = 0;
        size_t bytes = 0;             // what was committed for it, to give back
        int64_t mtime = 0;            // the job's enqueue-time disk baseline
        uint64_t fsize = 0;           // (local:// only) - see RFetchJob
    };
    std::atomic<uint32_t> rfGen{ 0 };
    std::thread rfThread;
    std::atomic<bool> rfStop{ false };
    std::atomic<int> rfPending{ 0 };
    // Bytes COMMITTED but not yet resident. residentImageBytes() counts only
    // app.images, so two opens a second apart each sized their prefetch against
    // a budget the other had already spent - each enqueueing a full budget's
    // worth. rbOpenQueue serialises the scan-open path for exactly this reason
    // ("the next stack starts only when the fetcher is idle"), but every
    // browser open and every session-restore line bypasses that queue.
    std::atomic<size_t> rfBytesInFlight{ 0 };
    std::atomic<int> rfTotal{ 0 }, rfFetched{ 0 };   // progress for the Files panel
    std::mutex rfMtx;
    std::condition_variable rfCv;
    std::vector<RFetchJob> rfQueue;   // guarded by rfMtx
    std::atomic<uint64_t> rfBusyUid{ 0 };   // the uid the worker is fetching NOW:
                                      // requestFullRemote's dedupe must see the
                                      // job that left the queue, or a promote
                                      // mid-flight enqueues the fetch twice
    std::vector<RFetchDone> rfDone;   // guarded by rfMtx
    // pending "load the rest of the folder?" question
    int seqAskImage = -1;
    std::vector<std::string> seqAskFiles;
    std::string seqAskPattern;
    // queued stacks from "Open Folder" (loaded one after another).
    // shape: "24x1200x1600 u16" when known (npy header peek locally, v3 listing
    // metadata remotely); the picker shows it and the merge warning compares it.
    // token: the identity of the stack this group is GOING to create, stamped
    // when the group is queued and recorded against the real seqId the moment
    // the stack appears (see groupStacks). A pending sweep resolves against
    // that, never against a display name.
    // openId: which Open Folder queued this group. The queue spans Opens (a
    // second Open while the first drains appends), and the raw format recipe is
    // answered once per OPEN - it must not reach the other one's files.
    struct PendingGroup { std::string name; std::vector<std::string> files;
                          bool isRaw = false; int batchId = 0; std::string shape;
                          int token = 0; int openId = 0; };
    std::vector<PendingGroup> seqQueue;
    // Sibling loads a session asked for, drained one at a time.
    // startSequenceLoad calls stopSequenceLoader, so restoring N stacks by
    // calling it N times in the parse loop cancelled every load but the last:
    // a 3-stack session came back with 7 of its 15 frames.
    // `name` is the user-given stack name from the session's seqname line.
    // seqload only QUEUES the rescan, so when seqname is parsed the SeqInfo
    // does not exist yet and the reader's `cur()->seqId != 0` guard dropped the
    // rename with no message - the stack came back under its folder-derived
    // pattern. It travels with the restore entry and is applied when the stack
    // is actually created.
    struct SeqRestore { uint64_t uid; std::vector<std::string> files; std::string pattern;
                        std::string name;
                        // the session's per-frame x axis, applied with the name
                        // once the stack exists (same window, same fix)
                        std::string axisName, axisUnit;
                        std::vector<double> axisVals; };
    std::vector<SeqRestore> seqRestore;
    // "Open as frame average" on a stack that is not here yet. Browse opens are
    // asynchronous - the first frame shows and the rest stream in - so the mean
    // cannot be taken at click time; the request is parked on the seqId it is
    // waiting for and fired by pumpStackAverages() once nothing more is coming.
    // A session restore pushes into the SAME list (through avgRestore below), so
    // there is one place where a stack becomes an average and one set of rules
    // about when it is allowed to happen.
    std::vector<int> pendingAvg;
    // ...and the session's side of it, by PATH, resolved lazily exactly like
    // seriesRestore: at parse time a folder stack is one loose image plus a
    // queued rescan, so the stack this names does not exist yet.
    std::vector<std::string> avgRestore;
    // Series a session asked for, resolved LAZILY for the same reason: at parse
    // time the stacks do not exist yet (a folder stack is one loose image plus a
    // queued rescan), so a member cannot be looked up. Members are named by the
    // PATH OF THEIR FIRST FRAME - a stack NAME would not do, because a name is
    // renameable and not unique (two folders can hold the same stack name, and
    // the user may change it between save and load), while a path is what the
    // image lines themselves already round-trip.
    struct SeriesRestore {
        std::string name, batchName, paramName, unit;
        int kind = 0;
        int badValues = 0;                // value fields the parser could not read
        int truncated = 0;                // member lines cut before their path
        struct M { double value; bool include; std::string path; };
        std::vector<M> members;
    };
    std::vector<SeriesRestore> seriesRestore;
    // MIGRATION of the old per-stack level, also by path. saveSession has never
    // written "seqlevel" (verified over the whole history), so this is normally
    // empty - it exists for hand-written and third-party files, and it creates
    // a series only where the file actually says there was a sweep.
    std::vector<std::pair<std::string, double>> seqLevelLegacy;
    // A sweep the PICKER was told to make ("open as a sweep"), resolved once
    // its stacks are open - the same lateness as seriesRestore and for the same
    // reason. Each member carries the TOKEN of the group it was ticked on, and
    // that is what it resolves by: the stack the group actually created is
    // recorded under that token at creation (App::groupStacks). The group name
    // is kept for the message and as a fallback, but it is not identity - it is
    // a display string, renameable with F2 the instant the head frame lands.
    struct SeriesPending {
        int batchId = 0;
        std::string name, paramName, unit;
        int kind = Series::KLinearity;
        struct Want { int token = 0; std::string name; double value = 0; };
        std::vector<Want> members;
    };
    // A QUEUE, not a slot. Resolution waits for every load to drain, which for a
    // folder-of-folders is seconds and for a remote sweep much longer, and File >
    // Open Folder is available throughout - a single slot meant the second sweep
    // silently threw the first one's ticked box, typed parameter and unit away.
    std::vector<SeriesPending> seriesPending;
    // Which stack each queued group produced, by token. Filled while a sweep is
    // waiting and cleared when it resolves, so it never outlives the Open that
    // stamped it.
    int nextGroupToken = 1;
    std::vector<std::pair<int, int>> groupStacks;   // group token -> seqId
    bool folderRecipeValid = false;   // raw recipe shared by the queue (see g_folderRecipe)
    int folderRecipeOpenId = 0;       // ...and WHOSE Open answered for it
    int nextOpenId = 1;               // one per Open Folder, stamped on its groups
    // "which stacks do you want?" picker shown after scanning a folder.
    // rel: each file's "folder/name" relative to the scanned root - the live
    // filter matches against exactly these strings, so folder names and file
    // names both narrow the tree. match/nMatch: the filter's current cut;
    // filtered-out files are NOT loaded when the group is accepted.
    struct FolderPick {
        PendingGroup g;
        bool selected = true;
        std::vector<std::string> rel;     // parallel to g.files
        std::vector<uint8_t> match;       // parallel to g.files; 1 = passes filter
        int nMatch = 0;
    };
    std::vector<FolderPick> folderPick;
    bool folderPickOpen = false;
    std::string folderPickRoot;
    // remote variant: the same picker, but accepted groups go through
    // rbOpenQueue / openRemoteStack instead of the local sequence loader
    bool folderPickRemote = false;
    std::string folderPickHost;
    int folderPickPort = 0;                // the port is part of the locator
    // one live filter box: space-separated terms AND together, '!' excludes,
    // * and ? wildcards, matched anywhere in FolderPick::rel (see applyPickFilter)
    char pickFilter[256] = "";
    int pickMerge = 0;                // 0 = one stack per group, 1 = ONE merged stack
    // batch assignment for the accepted selection: 0 = the whole Open is ONE
    // batch (the canon's default), 1 = one batch per top-level folder of the
    // scanned root. Ignored under pickMerge (a merged stack is one batch).
    int pickBatchMode = 0;
    // "open as a sweep": the accepted groups also become ONE series. Not
    // persisted and cleared on accept - a sticky box would create a sweep on
    // some later Open that nobody asked for, which is the one thing series are
    // never allowed to do. Exclusive with pickBatchMode (a series lives in one
    // batch); the parameter name and unit are the series', typed here.
    bool pickSweep = false;
    char pickSweepParam[64] = "";
    char pickSweepUnit[16] = "";
    std::string folderPickBatchBase;  // leaf of the scanned root: batch name stem
    // ---- .npz member picker (docs/npz-design.md §2.3) ----------------------
    // The SAME dialog as the folder picker, one layer down: an .npz is a batch
    // and its members are the stacks and frames inside it, so the vocabulary,
    // the All/None/Invert row and the accept path are the folder picker's.
    // Raised only when there is a choice to make - a file holding one image and
    // nothing else opens with no dialog at all, exactly as it does today.
    std::vector<NpzMember> npzPick;
    bool npzPickOpen = false;
    std::string npzPickPath;          // the .npz these members came from
    // Dropping several .npz files calls openPath in a LOOP, and one picker can
    // be up at a time: the rest WAIT here and are named, rather than the second
    // silently overwriting the first one's pending choice.
    std::vector<std::string> npzPickQueue;
    // The axis' NAME and UNIT are the user's to confirm. The key name is offered
    // as the default label because it is the only honest starting point; the
    // unit starts EMPTY and is never read out of the key name ("exposure_ms"
    // does not prove milliseconds - docs/terminology.md: 単位を仮定しない).
    char npzAxisName[64] = "";
    char npzAxisUnit[16] = "";
    // Members that are NOT pixels, kept per .npz file so the Inspector can show
    // the file's provenance next to the frame it opened
    // (docs/npz-design.md §4 item 3 「実装で答えたこと (v1)」).
    struct NpzMeta { std::string path; std::vector<std::pair<std::string, std::string>> items; };
    std::vector<NpzMeta> npzMeta;
    // ---- input adapters (docs/input-adapters.md §4.12 / §4.13) ---------------
    // v1 remembers ONE thing: which reader read which file. Folder and glob
    // RULES are deliberately absent (§8 item 7) - they need a trust rule first,
    // because a rule living in a shared data folder would run someone else's
    // Python without anyone choosing it. This list is a record of choices the
    // user made, which is why it is visible and removable rather than magic.
    std::string pythonExe;                  // configured interpreter; "" = probe PATH
    struct ReaderMemo { std::string path, spec; };
    std::vector<ReaderMemo> readerMemo;     // most recent first; bounded, see §4.12
    std::vector<std::string> readerShown;   // specs whose command was shown once (§4.13)
    // §4.13.0: ONE panel, three entrances, and it does not close. Writing an
    // adapter is write -> load -> read the failure -> fix -> load again, and a
    // modal cuts that loop every time the author leaves for their editor.
    std::string readerPickPath;             // the file or folder the panel is aimed at
    bool readerPanelOpen = false;
    bool readerPanelRaise = false;          // an entrance asked for focus
    bool readerListOpen = false;            // §4.12's visible, removable list
    char readerPickFile[512] = "";
    char readerPickFunc[128] = "load";
    char readerLocalDir[512] = "";          // a folder of the user's own readers
    std::string readerPickWhy;              // why we are asking (native's own refusal)
    // The last run, kept WHOLE. A traceback is the only debugging surface an
    // adapter author has; folding it into "could not open" makes the feature
    // useless to exactly the people it exists for (§4.13.0).
    std::string readerLastOut;
    std::string readerLastSpec;
    bool readerLastOk = false;
    bool readerRan = false;
    std::unique_ptr<pfd::select_folder> folderDlg;
    int folderDlgMode = 0;            // 0 = Open Folder (load stacks), 1 = Browse
    // The reader panel's own dialogs. These went in as pfd::open_file(...).
    // result() - blocking, inside the frame - which broke the rule the openDlg
    // comment states, skipped the one-at-a-time guard every other dialog has,
    // and left anyFileDialog() saying "no dialog" while one was on screen.
    std::unique_ptr<pfd::open_file>     rdOpenDlg;
    int rdOpenMode = 0;               // 0 = pick a reader .py, 1 = open a file with one
    std::unique_ptr<pfd::select_folder> rdFolderDlg;
    int rdFolderMode = 0;             // 0 = the folder of readers, 1 = open a folder with one
    std::unique_ptr<pfd::save_file>     rdNewDlg;   // where to write the template
    // A reader runs for as long as the user's data takes. Waiting for it on the
    // UI thread made the whole window stop answering - up to the five-minute
    // timeout - with nothing on screen to say why or to stop it. Only the wait
    // moves off-thread: reading what it produced touches images and GL.
    struct ReaderJob {
        std::string src, spec, out;        // what it is reading, with what, to where
        std::vector<std::string> argv;
        std::string outFile, errFile;      // the child's streams, readable AS it writes
        std::thread th;
        std::atomic<bool> done{ false };
        std::atomic<bool> cancel{ false };
        adapter::Run r;
        double startedAt = 0;
        size_t shown = 0;                  // how much of the live output is on screen
        int64_t srcMtime = 0;              // the origin's stat, taken BEFORE the
        uint64_t srcFsize = 0;             // reader ran (identity before bytes)
    };
    std::unique_ptr<ReaderJob> rdJob;
    bool anyFileDialog() const {      // see the note on csvDlg
        return openDlg || saveDlg || csvDlg || texportDlg || pngDlg || folderDlg ||
               rdOpenDlg || rdFolderDlg || rdNewDlg;
    }
                                      // Folder (local peer in the Browse panel)
    // temporal analysis cache (built-in, follows the selected ROI)
    struct TemporalState {
        int seqId = -1;
        int frames = 0;
        int rx = -1, ry = -1, rw = -1, rh = -1;   // resolved ROI, not annRev
        // The mosaic is part of the KEY: toggling CFA in the Inspector changes
        // what these numbers mean, and a cache that ignored it served the old
        // plane-mixed answer back.
        int cfa = -1, cfaPattern = -1;
        // SeqInfo::stackRev at compute time - the DATA revision. Without it the
        // key is (seqId, frames, ROI, CFA) and a reload that changes no shape
        // hands back the dead pixels' sigma_t (docs/reference-design.md §3.2).
        int stackRev = -1;
        int nPl = 1;                              // 4 when the frame is mosaiced
        uint64_t nonFinite = 0;                   // samples EXCLUDED, and said so
        size_t dropped = 0;                       // samples with < 2 valid frames
        std::vector<float> idx, frameMean, frameStd;
        double tempNoise[4] = {}, fixedPattern[4] = {}, totalNoise[4] = {};
        bool valid = false;
        bool roiUsed = false;
    } temporal[2];                    // 0 = A, 1 = B (compare)
    // ...and one per extra compare slot (C, D, E...), in cmpExtra order. Same
    // cache, same key: a slot only pays for a recompute when its STACK or the
    // ROI changes, so N slots cost N cached lookups per frame, not N passes.
    std::vector<TemporalState> temporalExtra;
    // H/V projections (line profiles) of the selected ROI / whole image
    struct ProjState {
        const ImageDoc* img = nullptr;
        uint64_t uid = 0;
        int dataRev = -1;
        int mode = -1, cfa = -1, cfaPattern = -1;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        int nSeries = 0;
        // 5, not 4: slot 4 is the optional "all" row - every pixel of the ROI
        // regardless of plane. The canon forbids MIXING planes inside a
        // per-plane statistic, so this is a DIFFERENT measurement and is
        // labelled "all", never drawn as a fifth plane. nSeries stays the plane
        // count; allRow says whether slot 4 is filled.
        const char* seriesNames[5] = {};
        std::vector<float> h[5], v[5];    // per series: mean along columns / rows
        float hMin = 0, hMax = 1, vMin = 0, vMax = 1;
        // statistics of the profiles themselves (sigma of column means = column FPN)
        struct Stats { double mean = 0, sd = 0, mn = 0, mx = 0, pp = 0, pct = 0; bool valid = false; };
        Stats hStat[5], vStat[5];
        // ...and of the REGION itself. sigma of the column means says how much
        // the columns differ; this says how much the pixels do. Reading the two
        // profile sigmas without it is reading a ratio with no denominator -
        // and it is the third quantity the row/column noise split needs.
        Stats fStat[5];
        bool allRow = false;              // slot 4 holds the plane-mixed row
        bool roiUsed = false;
    } proj[2];                        // 0 = A, 1 = B (compare)
    std::vector<ProjState> projExtra;  // one per cmpExtra slot, same order
    // profile statistics table: 0 auto (wide when it fits), 1 wide, 2 per-axis rows.
    // Wide = one row per plane with sigma_f / sigma_v / sigma_h across it, which
    // keeps a plane's three numbers on ONE line - and leaves the row axis free
    // for the sides, which is where extra frames or slots will have to go.
    int projStatLayout = 0;
    // Decimals in the profile-statistics table. -1 = significant digits (%g,
    // the old behaviour), 0..6 = fixed places. Interim, and the reason is
    // alignment rather than precision: %g gives every cell its own length, so
    // a column of numbers can be neither scanned down nor pasted into a report
    // as a column. What the app should do about number formats in GENERAL is
    // docs/todo-open.md.
    int projDecimals = -1;
    // an extra row over every pixel of the ROI, planes mixed. Off by default:
    // it is a different measurement from the per-plane rows and reading it as
    // a fifth plane is exactly the mistake the canon's iron rule exists to stop.
    bool projAllRow = false;
    // 0 = side major (A's planes, then B's), 1 = channel major (R: A then B,
    // then Gr: ...). Comparing one plane across sides wants the second.
    int projStatOrder = 0;
    int projMode = 0;                 // 0 = mean, 1 = max, 2 = min
    bool showProjH = true, showProjV = true;
    int projYMode = 0;                // value axis: 0 auto (H/V shared), 1 display range, 2 fixed
    float projYLo = 0, projYHi = 1;   // used by mode 2
    std::vector<ImageDoc*> texLru;    // GPU textures kept for the N most recent frames
    int roiChannel = -1;              // channel shown in the ROI table (-1 = all)
    // (npyAxis lived here: one global override for every 3-D .npy in a run.
    //  docs/input-adapters.md §3.4 replaced it with FrameSource::npyRead, which
    //  is per file, visible in the Inspector, and saved with that image.)
    // shared display range: every open image (and every newly loaded one) uses it
    bool linkRange = false;
    float linkBlack = 0, linkWhite = 1;
    // panel visibility (persisted with the ImGui layout)
    bool showFiles = true, showInspector = true, showRois = true, showAnalysis = true;
    // Projection is on by default, and its tab in FRONT of Histogram's: the
    // row/column profiles are what this user looks at first. A saved session
    // still restores whatever arrangement it recorded.
    bool showHistogram = true, showTemporal = true, showProjection = true;
    bool resetLayout = false;
    std::string pendingLayout;        // dock settings from a session, applied next frame
    bool compactUi = true;            // dense spacing: this tool is table-heavy
    int wakeFrames = 3;               // frames still to draw after the last input
    bool lowBandwidth = false;        // remote/ssh: draw the minimum, not a tail
    bool showFps = false;
    bool fitOnSwitch = false;         // view (zoom/pan) is shared; switching keeps it
    // window frame: 1 = the title bar lives in our menu bar (see window_frame.h),
    // 0 = the desktop draws one above us. Persisted; --frame overrides for one run.
    int frameMode = 1;
};
static const ImU32 ANN_COLORS[8] = {
    IM_COL32(77, 163, 255, 255), IM_COL32(105, 220, 130, 255), IM_COL32(255, 184, 77, 255),
    IM_COL32(255, 120, 120, 255), IM_COL32(200, 120, 255, 255), IM_COL32(90, 220, 220, 255),
    IM_COL32(255, 150, 200, 255), IM_COL32(180, 200, 90, 255),
};
static App app;

// ---- remote viewing: declared here, defined further down ----
static const char* REMOTE_HOME = "~/.viewer";
// Plain ASCII: the bundled font has no icon set. TWO tags, because there are
// two states. An empty host means the peer runs HERE - the same protocol over a
// pipe instead of ssh - and a browse of this disk that flies an [ssh] flag,
// says "local peer" in the title bar and prints "local://" in a dialog reads as
// a connection to a machine called "local". Nothing is open. Say so.
#define ICON_SSH   "[ssh]"
#define ICON_LOCAL "[local]"
// what an empty host is CALLED on screen (the url form stays "local://" - it is
// storage, not language)
#define PEER_HERE  "this machine"
static const char* peerTag(const std::string& host) {
    return host.empty() ? ICON_LOCAL : ICON_SSH;
}
static std::string peerLabel(const std::string& host) {
    return host.empty() ? std::string(PEER_HERE) : host;
}
static bool openRemote(const std::string& url, bool asPreview = false, int frame = 0);
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name = std::string(), int port = 0,
                            int token = 0);
static std::string makeRemoteUrl(const std::string& host, const std::string& path, int port = 0);
static bool ensureUiSession(const std::string& host, std::string& err, int port = 0);
static void remoteBrowseTo(App::BrowseInstance& I, const std::string& dir);
static void rbTreeForget(App::BrowseInstance& I);   // drop the tree's cached children
static std::string placeUrl(const std::string& host, int port, const std::string& path);
static void rbEnqueue(App::BrowseInstance& I, App::RbJob job);
static void pumpRemoteBrowse();
static void stopRbWorker();
static std::string bootstrapScript();
static std::string updateScript();
static void startRemote(App::BrowseInstance& I, const std::string& hostSpec);
static void sessionRestoreBrowsePlace(int num, const std::string& url);
static void drawPanelRemote(App::BrowseInstance& I);
static bool deployPeer(const std::string& host, int port, bool force, std::string& log);
static bool isNpyName(const std::string& n);
static void sortFramesNumerically(std::vector<std::string>& files);

#include "browse/instances.inc"

static ImageDoc* cur() { return app.current >= 0 && app.current < (int)app.images.size() ? app.images[app.current].get() : nullptr; }

static void promotePreview(ImageDoc* d);   // fwd: preview -> registered open

// Set by resolveB when follow-frame is on, both sides are stacks, and B has no
// frame carrying A's number - B is then the LAST latched frame, which is not
// A's partner any more. The A/B surfaces render it with the [stale] vocabulary
// they already have; silently returning a mismatched pair is the part that had
// to go. (docs/terminology.md: stack-to-stack comparison holds the frame
// NUMBER, and abDocLabel names the STACK, so nothing on screen said otherwise.)
static bool g_abFollowDiverged = false;

// ONE follow-frame rule for every letter past A. The pin (a member uid) names
// the STACK; when both sides are stacks, follow answers the sibling carrying
// A's frame number. A single-image side (a dark frame, a golden sample) has no
// frame axis and stays put; so does a pin into A's OWN stack - that is how one
// frame is compared against another of the same capture. A stack with no frame
// at A's number hands back the pinned frame with `diverged` set: the caller
// must SAY it (the [stale] vocabulary), never show it as a matched pair.
static ImageDoc* followFrame(ImageDoc* d, bool& diverged) {
    diverged = false;
    ImageDoc* a = cur();
    if (!d || !a || !app.compareFollowFrame) return d;
    if (d->seqId == 0 || a->seqId == 0 || d->seqId == a->seqId ||
        d->seqIndex == a->seqIndex)
        return d;
    for (const auto& q : app.images)
        if (q->seqId == d->seqId && q->seqIndex == a->seqIndex) return q.get();
    // The stack is shorter, or its remote frames have not arrived. Falling
    // through SILENTLY here is what used to hand the difference image, the
    // statistics and the wipe a pair that was no longer a pair.
    diverged = true;
    return d;
}

// The B side of an A/B compare, or null when compare is off / B is gone.
//
// B RESOLVING TO THE SAME DOCUMENT AS A IS NOT A SPECIAL CASE. This used to
// answer null there ("paused"), and every surface downstream - the histogram,
// the projection and temporal panels, the Inspector's B column, the canvas -
// then rendered as if there were no comparison at all. The kindness was
// counterproductive (user report, 2026-08-04: 「A/B比較で，A=Bの時，ヒストグラム
// の表示を切り替えているけど，このケアはかえってみずらい．比較の際に同じもの
// を選んだ時に，同じことを確認できる方がよいので，こういうの全般不要です」):
// putting the same document on both sides is something people do ON PURPOSE,
// to confirm the two sides agree, and a panel that hides B at that moment makes
// the confirmation impossible - "B is identical to A" and "B is not being
// drawn" look the same. So B is B, and two coinciding sides are DRAWN
// coinciding: two curves in their own slotInk, two rows, two labels.
static ImageDoc* resolveB() {
    g_abFollowDiverged = false;
    if (app.compareBUid) {
        ImageDoc* b = nullptr;
        for (const auto& d : app.images)
            if (d->uid == app.compareBUid) { b = d.get(); break; }
        if (!b) { app.compareBUid = 0; return nullptr; }      // B was closed
        return followFrame(b, g_abFollowDiverged);
    }
    if (app.compareB.empty()) return nullptr;
    // session fallback: match the saved name (and frame, for a stack), then latch
    // the uid so navigation cannot re-point B at a different frame
    for (const auto& d : app.images) {
        if (d->name != app.compareB) continue;
        if (app.compareBSeq >= 0 && d->seqId != 0 && d->seqIndex != app.compareBSeq) continue;
        app.compareBUid = d->uid;
        return d.get();
    }
    return nullptr;
}
static ImageDoc* cmpB() { return app.compareMode == App::CmpOff ? nullptr : resolveB(); }

// fwd: A and B are named by their STACK wherever the two are set against each
// other - two stacks of a series hold identically named frames
static std::string abDocLabel(const ImageDoc* d);
// fwd: the extra compare slots (defined with compareExtras below)
static std::string slotOf(const ImageDoc* d);
static void removeCompareSlot(const ImageDoc* d);
static void toast(const std::string& msg, bool err);

// remember B as an identity, not as a name
static void setCompareB(const ImageDoc* d) {
    // making a preview the B side IS using it: promote before it can vanish
    if (d && d->preview) promotePreview(const_cast<ImageDoc*>(d));
    // One image, one letter: choosing a lettered image as B is a PROMOTION -
    // the slot is released, or the same pixels would hold two panes and two
    // table rows. (The default-B paths in ensureCompareB never reach here for
    // a lettered image; an explicit choice outranks the letter.)
    if (d) {
        std::string held = slotOf(d);
        if (!held.empty()) {
            removeCompareSlot(d);
            toast("slot " + held + " -> B  (" + abDocLabel(d) + ")", false);
        }
    }
    // Remember the last B that was CHOSEN, and keep remembering it after
    // compare is switched off. Turning compare back on should return to the
    // pair you were working with, not to whatever happens to be adjacent.
    if (d) app.lastCompareBUid = d->uid;
    app.compareBUid = d ? d->uid : 0;
    app.compareB = d ? d->name : std::string();
    app.compareBSeq = d && d->seqId != 0 ? d->seqIndex : -1;
}

// The uid holding the B SEAT: the live pin, or the remembered one after an
// exit path cleared it (A9: compare off and on returns to the same B). The
// Files letters are seat assignments, so they key on this - never on cmpB(),
// which answers null while compare is off, and a letter that vanishes there
// reads as the assignment being lost.
static uint64_t bSeatUid() {
    return app.compareBUid ? app.compareBUid : app.lastCompareBUid;
}

// The B side FOR THE STATISTICS PANELS: the compare partner, but only when it
// has pixels to measure - a remote preview placeholder has none.
static ImageDoc* abStatsB() {
    ImageDoc* b = cmpB();
    if (!b || b->w < 1 || b->h < 1 || b->px().empty()) return nullptr;
    return b;
}

// Side by side, or overlaid? One answer for every panel (App::abStatsLayout).
// Auto mirrors the IMAGE: CmpSplit puts A and B left/right, so the plots go
// left/right too; wipe / blink / difference have one image area, so they overlay.
static bool abSideBySide() {
    if (app.abStatsLayout == App::AbOverlay) return false;
    if (app.abStatsLayout == App::AbSide) return true;
    return app.compareMode == App::CmpSplit;
}

// Can A's and B's profiles be drawn on ONE axis? Only when both sampled the
// same range - same origin and same length. Anything else would have to be
// stretched to line up, and stretching invents a correspondence between two
// captures that nothing in the data supports. False means: side by side, and
// say why (docs/ab-stats-plan.md 5).
static bool abProjOverlayable(const App::ProjState& A, const App::ProjState& B) {
    return A.rx == B.rx && A.rw == B.rw && A.ry == B.ry && A.rh == B.rh;
}

// --no-ab-throttle: developer flag, so the cost the throttle removes can be
// measured on the same binary instead of quoted from an older one.
static bool g_abNoThrottle = false;
// True while frames are being stepped continuously. The B slots skip their
// recompute then; whoever draws B must say "stale" (docs/ab-stats-plan.md 1).
static bool abStepBusy() {
    return !g_abNoThrottle && app.abStepBusyUntil > nowSec();
}

// Slot 1 of every statistics cache belongs to the B side and is filled only
// while a B exists. Called once per frame BEFORE the panels draw: the first
// frame after compare goes off clears it, and every frame after that does
// nothing at all - so compare-off costs exactly what it did before A/B stats.
static void abStatsFrame() {
    if (cmpB()) { app.abSlot1Live = true; return; }
    if (!app.abSlot1Live) return;
    app.hist[1] = App::HistState{};
    app.proj[1] = App::ProjState{};
    app.temporal[1] = App::TemporalState{};
    app.abSlot1Live = false;
}

// Put a line in the Messages panel WITHOUT showing a toast. Everything that
// writes to msgLog goes through here: the cap and the de-duplication are the
// log's rules, and three call sites that pushed straight onto the vector were
// keeping neither.
static void logMsg(const std::string& msg, bool err = false) {
    if (!app.msgLog.empty() && app.msgLog.back().text == msg) return;
    app.msgLog.push_back({ msg, err });
    if (app.msgLog.size() > 300) app.msgLog.erase(app.msgLog.begin());
    app.msgSeq++;
    if (err) app.msgUnreadErr = true;
}

static void toast(const std::string& msg, bool err = false) {
    app.toast = msg; app.toastErr = err;
    app.toastUntil = ImGui::GetTime() + (err ? 6.0 : 2.5);
    // A toast fades after six seconds, which is not long enough to paste it into
    // a bug report. Every message is kept here and shown, selectable, in the
    // Messages panel.
    logMsg(msg, err);
}

// One batch per OPEN ACTION: reopening a folder deliberately makes a fresh
// one - "calling the same folder twice" must give two groupings you can name
// apart, or the second load is invisible.
static std::string uniqueBatchName(std::string base);   // fwd
static int newBatch(const std::string& name, const std::string& srcDir = std::string()) {
    app.batches.push_back({ app.nextBatchId, name.empty() ? "opened" : name, srcDir });
    app.imagesRev++;                      // the Files grouping caches on this
    return app.nextBatchId++;
}
// Loose single-file opens share a batch instead of making one each - the right
// behaviour for "clicked three files in a row". The identity is the source
// DIRECTORY, not its leaf name: two directories that happen to end in the same
// component are different measurements. `dir` empty = match by name, which is
// what session restore does (the session persists batches BY NAME, by design).
static int batchReuse(const std::string& name, const std::string& dir = std::string()) {
    if (!dir.empty()) {
        for (const auto& b : app.batches) if (b.srcDir == dir) return b.id;
        return newBatch(uniqueBatchName(name), dir);
    }
    for (const auto& b : app.batches) if (b.name == name) return b.id;
    return newBatch(name);
}
// Batch names must be UNIQUE (docs/terminology.md): sessions restore batches BY
// NAME (imgbatch -> batchReuse), so two batches sharing one name silently merge
// on the next load. Collisions get " (2)", " (3)", ...
static std::string uniqueBatchName(std::string base) {
    if (base.empty()) base = "opened";
    auto taken = [](const std::string& n) {
        for (const auto& b : app.batches) if (b.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int k = 2;; k++) {
        std::string n = base + " (" + std::to_string(k) + ")";
        if (!taken(n)) return n;
    }
}
// The Files header's rename, as a function so it can be tested and so it goes
// through the uniquifier every creation path already uses. It was the ONE
// name-write that did not, and the canon says why that matters: a session
// restores batches by name, so two live batches sharing one MERGE on the next
// load. That used to be cosmetic. With series persisted it is not: two copies
// of the same folder (which the canon blesses) inside the merged batch defeat
// preferBatch, so the second restored series resolves onto the stacks the first
// already holds, takes them, and pruneEmptySeries deletes the emptied first one
// - while the toast still counts it as restored, because `made` was incremented
// before the theft. The next autosave makes it permanent.
static void renameBatch(int batchId, const std::string& want) {
    for (auto& b : app.batches) {
        if (b.id != batchId) continue;
        if (want.empty() || want == b.name) return;
        std::string uniq = uniqueBatchName(want);   // skips names nobody holds
        if (uniq != want)
            toast("batch renamed to \"" + uniq + "\": \"" + want + "\" is taken, and a "
                  "session restores batches by name");
        b.name = uniq;
        app.imagesRev++;                  // the Files grouping caches on this
        return;
    }
}

static void selectImage(int idx);   // fwd (defined with the sequence helpers)

// The extra slots, resolved and pruned in one place. A slot whose image was
// closed simply stops existing - the same rule B follows - and A can never
// occupy one, or a column would compare a thing with itself. The slots resolve
// EXACTLY as B does (resolveB): the uid pins the stack, follow-frame picks the
// sibling carrying A's number, and a stack with no such frame is `diverged`.
// `idx` is the position in cmpExtra - the LETTER - and it travels with the
// resolved document, because after a follow the document is no longer the
// pinned uid and a uid lookup would lose the letter.
struct ResolvedSlot { ImageDoc* doc; size_t idx; bool diverged; };
static std::vector<uint8_t> g_slotDiverged;   // by cmpExtra index, last resolve
// A slot standing on the same document as A resolves like any other: the same
// rule resolveB now follows, for the same reason. Dropping it here made a
// letter's row, curve and pane vanish the moment A walked onto it, which is
// indistinguishable from the slot having been lost.
static std::vector<ResolvedSlot> resolveSlots() {
    std::vector<ResolvedSlot> out;
    g_slotDiverged.assign(app.cmpExtra.size(), 0);
    for (size_t i = 0; i < app.cmpExtra.size();) {
        ImageDoc* d = nullptr;
        for (const auto& q : app.images) if (q->uid == app.cmpExtra[i]) { d = q.get(); break; }
        if (!d) {   // closed
            app.cmpExtra.erase(app.cmpExtra.begin() + i);
            g_slotDiverged.erase(g_slotDiverged.begin() + i);
            continue;
        }
        bool div = false;
        ImageDoc* r = followFrame(d, div);
        g_slotDiverged[i] = div ? 1 : 0;
        out.push_back({ r, i, div });
        i++;
    }
    return out;
}
static std::vector<ImageDoc*> compareExtras() {
    std::vector<ImageDoc*> out;
    for (const ResolvedSlot& rs : resolveSlots()) out.push_back(rs.doc);
    return out;
}
static const char* SLOT_LETTERS = "CDEFGHIJKLMNOP";
static std::string slotName(size_t i) {
    return std::string(1, i < strlen(SLOT_LETTERS) ? SLOT_LETTERS[i] : '?');
}
// Which letter this image already holds, or "" - so a row can say "remove"
// instead of silently adding the same image twice.
static std::string slotOf(const ImageDoc* d) {
    if (!d) return "";
    for (size_t i = 0; i < app.cmpExtra.size(); i++)
        if (app.cmpExtra[i] == d->uid) return slotName(i);
    return "";
}
static void addCompareSlot(ImageDoc* d) {
    if (!d) return;
    // A is the CURSOR, not a seat, so the image under it may hold a letter like
    // any other - that is how you park the frame you are on as C and go looking
    // for what to set against it. (It used to be refused outright, which is the
    // same "you cannot compare a thing with itself" care resolveB carried.)
    if (d->uid == app.compareBUid) {   // one image, one letter: it is B already
        toast("that image is B - wipe / split / difference compare it already", true);
        return;
    }
    for (uint64_t u : app.cmpExtra) if (u == d->uid) return;
    if (app.cmpExtra.size() >= strlen(SLOT_LETTERS)) {
        toast("no free compare slot", true);
        return;
    }
    app.cmpExtra.push_back(d->uid);
    toast("slot " + slotName(app.cmpExtra.size() - 1) + " = " + abDocLabel(d));
}
static void removeCompareSlot(const ImageDoc* d) {
    if (!d) return;
    for (size_t i = 0; i < app.cmpExtra.size(); i++)
        if (app.cmpExtra[i] == d->uid) { app.cmpExtra.erase(app.cmpExtra.begin() + i); return; }
}

// What the slot item on a Files row OFFERS for this document - and `letter` is
// the letter the offer would name ("Add as compare slot D" / "Remove from
// compare slot C"). Out here rather than in the menu for the same reason
// abRowItem is: so it can be checked without a frame (--abstats-selftest, N5).
// Issue #60 established what that costs when it is skipped: every panel could
// HANDLE letters past B, the selftests proved it - by arming slots with a
// direct call - and whether the one path a user has to arm them still offered
// anything was checked by nobody, over an equation that holds on the empty set.
enum SlotRowItem {
    SlotRowNone,       // no document, or the pinned B: its row's B items say so,
                       // and a slot on top would put one image in two panes
    SlotRowAdd,        // "Add as compare slot <letter>" - the next free letter
    SlotRowRemove,     // "Remove from compare slot <letter>" - the letter held
};
static SlotRowItem slotRowItem(const ImageDoc* pick, std::string& letter) {
    letter.clear();
    if (!pick) return SlotRowNone;
    letter = slotOf(pick);
    if (!letter.empty()) return SlotRowRemove;
    if (pick->uid == app.compareBUid) return SlotRowNone;
    letter = slotName(app.cmpExtra.size());
    return SlotRowAdd;
}

// Did slot i's follow-frame land on nothing the last time the slots resolved?
// The per-slot g_abFollowDiverged (--abstats-selftest S3).
static bool slotFollowDiverged(size_t i) {
    return i < g_slotDiverged.size() && g_slotDiverged[i];
}

// A session names its slots by path (+ frame, for a stack member); the images
// arrive later and asynchronously, so this is retried until it lands or the
// load settles. Same shape as the B fallback below it.
static void pumpCompareSlotRestore() {
    if (app.cmpSlotRestore.empty()) return;
    for (size_t i = 0; i < app.cmpSlotRestore.size();) {
        const auto& w = app.cmpSlotRestore[i];
        ImageDoc* hit = nullptr;
        for (const auto& d : app.images) {
            if (d->src->path != w.path) continue;
            if (w.frame >= 0 && d->seqId != 0 && d->seqIndex != w.frame) continue;
            hit = d.get(); break;
        }
        if (!hit) { i++; continue; }
        bool dup = false;
        for (uint64_t u : app.cmpExtra) if (u == hit->uid) dup = true;
        if (!dup) app.cmpExtra.push_back(hit->uid);
        app.cmpSlotRestore.erase(app.cmpSlotRestore.begin() + i);
    }
}

// Default B = the image next to A in the list: with two images open, "compare"
// should just work without first picking a partner.
static void ensureCompareB() {
    if (resolveB()) return;       // mode-independent: called before the mode changes
    setCompareB(nullptr);
    if (app.images.empty()) return;
    // A DEFAULT B never lands on a lettered image: wipe / difference / blink
    // are strictly A against B, and a B that doubled as C both tiled the same
    // pixels twice and read as an "A,C compare" (defect, 2026-07-30). An
    // explicit choice still may - setCompareB then releases the letter.
    auto free = [](const ImageDoc* d) { return d != cur() && slotOf(d).empty(); };
    // 1. the B you last chose, if it is still open. Coming back to a comparison
    //    should come back to the SAME comparison.
    if (app.lastCompareBUid)
        for (const auto& d : app.images)
            if (d->uid == app.lastCompareBUid && free(d.get())) { setCompareB(d.get()); return; }
    // 2. otherwise the image you were just looking at
    // (after Process > demosaic that is the source, not images[0])
    if (app.prevImageUid)
        for (const auto& d : app.images)
            if (d->uid == app.prevImageUid && free(d.get())) { setCompareB(d.get()); return; }
    for (int i = 0; i < (int)app.images.size(); i++) {
        int j = (app.current + 1 + i) % (int)app.images.size();
        if (free(app.images[j].get())) { setCompareB(app.images[j].get()); break; }
    }
}

// Shift+C: bring up the comparison BEYOND B. The slots already show in the
// statistics tables; what this does is put the images on screen next to each
// other, which is the one image layout that means more than two. Wipe,
// difference and blink are left alone - they mean exactly two, and cycling
// into them with four slots open would quietly show two of them.
static void showCompareSlots() {
    if (app.cmpExtra.empty()) {
        toast("no slots past B yet - Files: right-click a row, \"Add as compare slot C\"", true);
        return;
    }
    if (app.compareMode == App::CmpOff) ensureCompareB();
    app.compareMode = App::CmpSplit;      // the side-by-side family
    toast("side by side: A, B and " + std::to_string(app.cmpExtra.size()) + " more");
}

static void cycleCompare() {
    if (app.images.size() < 2) { toast("compare needs a second open image", true); return; }
    ensureCompareB();
    app.compareMode = (app.compareMode + 1) % 5;
    toast(app.compareMode == App::CmpOff   ? "compare off"
        : app.compareMode == App::CmpWipe  ? "compare: wipe  (drag the divider)"
        : app.compareMode == App::CmpSplit ? "compare: side by side"
        : app.compareMode == App::CmpDiff  ? "compare: difference A-B"
                                           : "compare: blink  (B or Space toggles, "
                                             "'auto blink' in Inspector)");
}

// ESC, one step OUTWARD per press, innermost claim first (user report,
// 2026-07-30: 「抜ける方法が自明ではない．ESCボタンでぬける，でよいのは？」):
//   1. an open popup / menu takes it (the ClosePopupsExceptModals pass)
//   2. an active text item takes it (reverts the edit)
//   3. a selected annotation / ROI is deselected, and ONLY that
//   4. an active comparison is switched off, and ONLY that
// Steps 1 and 2 are the gates on the shortcut block that calls this
// (!popupOpen, !io.WantTextInput) - by the time it runs, ESC belongs to the
// canvas state. Steps 3 and 4 live HERE, as one dispatch, so a single press
// can never do two of them at once; the return value is what the press
// consumed, which is how --abstats-selftest proves that ordering.
enum class EscTook { Nothing, RoiDeselected, CompareOff };
// Which layer ACTUALLY took each Escape, one name per press in the order the
// presses arrived: "popup;textedit;roi;compare;nothing;". The four steps above
// are decided in four different places - two of them inside ImGui, where this
// app's state never hears about them - so "one press, one step outward" was
// only ever checkable for the two that end in escapePressed(). A14/A14b assert
// the ROI/compare pair and, separately, that a popup went away; neither can
// say that ONE press did not do two of them, which is the whole rule.
// (docs/verify-ui.md E7.) Recorded at the point each layer acts, so a press
// consumed twice appears as two entries rather than being reasoned away.
static std::string g_escProbe;
static int g_escProbeN = 0;              // entries, so a caller can take a delta
static int g_escProbeFrame = -1;         // the frame the last entry was made in
static void escProbeNote(const char* layer) {
    g_escProbe += std::string(layer) + ";";
    g_escProbeN++;
    if (ImGui::GetCurrentContext()) g_escProbeFrame = ImGui::GetFrameCount();
}
static EscTook escapePressed() {
    if (app.selectedAnn != 0) {          // back to All - the old ESC meaning
        app.selectedAnn = 0;
        escProbeNote("roi");
        return EscTook::RoiDeselected;
    }
    if (app.compareMode != App::CmpOff) {
        // OUT of the comparison, keeping the seats: the B pin survives
        // (compareBUid / lastCompareBUid) and the slot letters stay armed
        // (cmpExtra) - settled A9 behavior, compare off and on returns to
        // the same pair. The toast says so, or leaving reads as losing the
        // setup; the Files letters stay visible too, dimmed (see the badge
        // rule in drawFileList).
        app.compareMode = App::CmpOff;
        toast("comparison off - B/C assignments kept (C to resume)");
        escProbeNote("compare");
        return EscTook::CompareOff;
    }
    escProbeNote("nothing");
    return EscTook::Nothing;
}

// Pin the frame you are looking at as B, then walk A somewhere else: this is how
// you compare frame 12 against frame 13 of one stack, or a source against its
// processed result. Staying put is a comparison too - A and B on one document,
// both sides drawn coinciding - so the toast invites the walk rather than
// instructing it ("move A somewhere else" read as a precondition, back when
// the pair really did stay silent until you did).
static void pinCurrentAsB() {
    if (!cur()) return;
    setCompareB(cur());
    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
    toast("B = " + abDocLabel(cur()) + "  (A and B agree - move A to compare)");
}

static void swapCompare() {
    ImageDoc* b = cmpB();
    if (!cur()) return;
    if (!b) {   // silence here reads as "the feature does not exist"
        toast(app.compareMode == App::CmpOff ? "compare is off  (\\ or C turns it on)"
                                             : "no B image  -  View > Compare A/B > B image", true);
        return;
    }
    const ImageDoc* a = cur();
    // Both sides on one document is a legal comparison now, and swapping it
    // moves nothing. Saying "swapped" over an unchanged screen is how a no-op
    // becomes a bug report, so say what actually happened instead.
    if (b == a) {
        toast("A and B are the same image - nothing to swap  (" + abDocLabel(a) + ")");
        return;
    }
    std::string an = abDocLabel(a), bn = abDocLabel(b);
    uint64_t bUid = b->uid;
    setCompareB(a);                       // set B before moving A: cur() changes
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->uid == bUid) { selectImage(i); break; }
    toast("A / B swapped:  A = " + bn + "   B = " + an);
}

// The status-bar compare chip, as one sentence --abstats-selftest can read.
// Returns "" while compare is off - the bar then says nothing about comparing.
static std::string abStatusChipText() {
    if (app.compareMode == App::CmpOff) return "";
    ImageDoc* b = cmpB();
    char buf[320];
    // One silence left, and it means one thing: there is no B. The second
    // sentence here used to be "A = B (paused)" - the chip explaining why the
    // panels had gone quiet while A stood on the pinned B. Nothing goes quiet
    // any more (resolveB), so the chip names the pair like any other.
    if (!b) return "A/B: no B image";
    // §3.1: A and B mapping the SAME source is a true comparison whose
    // difference is all zero - the chip says why. A slot sharing A's source
    // is said the same way. (When A IS B - allowed now - the sentence is
    // still true, and stops pretending otherwise.)
    std::string share;
    if (const ImageDoc* a = cur()) {
        // EVERY side sharing A's pixels, by the letters the panes use: B and
        // the slots, follow-frame-resolved exactly as the panes are
        // (resolveSlots IS that resolution). Testing the pinned uid instead
        // claimed or withheld the share against a frame nobody was comparing -
        // and stopping at the first match hid every sharer past it.
        std::string letters;
        if (a->src == b->src) letters = "B";
        for (const ResolvedSlot& rs : resolveSlots())
            if (rs.doc->src == a->src)
                letters += (letters.empty() ? "" : ", ") + slotName(rs.idx);
        if (!letters.empty())
            share = "  -  A and " + letters + " share the same pixels";
    }
    if (app.compareMode == App::CmpDiff) {
        snprintf(buf, sizeof buf, "%s +-%g  B: %s", app.diffAbs ? "|A-B|" : "A-B",
                 app.diff.gain, abDocLabel(b).c_str());
        return buf + share;
    }
    float fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
    snprintf(buf, sizeof buf, "A/B %s %.0f%%  B: %s",
             app.compareMode == App::CmpSplit ? "split" : "wipe", fr * 100,
             abDocLabel(b).c_str());
    return buf + share;
}

// What the A/B item on a Files row offers, for the frame or stack that row
// would hand to B.
//
// EVERY row offers it, the one you are looking at included. That row used to
// offer nothing at all, on the reasoning that it IS A and a thing cannot be
// compared with itself - which is the very care the user threw out (2026-08-04,
// 「比較の際に同じものを選んだ時に，同じことを確認できる方がよい」). Setting
// this row as B is now a legitimate and useful move: both sides show the same
// document, the panels draw two curves and two rows on top of each other, and
// that coincidence is the confirmation you were after.
//
// (It also used to offer "Swap A and B" here, and THAT line really was care
// nobody needed - user report 2026-07-30: 「swapはショートカットキーで対応され
// ているから右クリックメニューでのケアは不要では」. The swap lives on Shift+\ ,
// the View menu and the status-bar button; it is not coming back.)
//
// Out here rather than in the menu so it can be checked without a frame
// (--abstats-selftest, A7).
enum AbRowItem {
    AbSetB,        // this row can become B - and every row can
    AbNone         // reserved: nothing on this row (no image at all)
};
static AbRowItem abRowItem(const ImageDoc* pick) {
    return pick ? AbSetB : AbNone;
}

static void computeMinMax(ImageDoc& im) {
    float mn = FLT_MAX, mx = -FLT_MAX;
    for (float v : im.px()) {
        if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
    }
    if (mn > mx) { mn = 0; mx = 1; }
    if (mn == mx) mx = mn + 1;
    im.src->vmin = mn; im.src->vmax = mx;   // the source's truth...
    im.vmin = mn; im.vmax = mx;             // ...and the doc's mirror of it
}
// Effective display range: the shared value when linking is on, the image's own
// otherwise. Linking never overwrites an image's range (defined below with App).
static float effBlack(const ImageDoc& im);
static float effWhite(const ImageDoc& im);

static void defaultRange(ImageDoc& im) {
    if (im.dtype == "u8" || im.dtype == "i8")       { im.black = 0; im.white = 255; }
    else if (im.dtype == "u16")                     { im.black = 0; im.white = 65535; }
    else if (im.dtype == "i16")                     { im.black = 0; im.white = 32767; }
    else if (im.vmin >= -0.001f && im.vmax <= 1.2f && im.vmax > 0.005f) { im.black = 0; im.white = 1; }
    else                                            { im.black = im.vmin; im.white = im.vmax; }
}

// Keep only the most recent textures resident: a 200-frame sequence would
// otherwise pin gigabytes of VRAM. Ordered by USE, not by creation - with A/B
// compare, B is often an old doc that is needed every frame, and an insertion-
// ordered list would evict and re-upload it constantly.
static void touchTex(ImageDoc& im) {
    auto it = std::find(app.texLru.begin(), app.texLru.end(), &im);
    if (it != app.texLru.end()) app.texLru.erase(it);
    app.texLru.push_back(&im);
    const size_t TEX_KEEP = 12;
    for (size_t i = 0; i < app.texLru.size() && app.texLru.size() > TEX_KEEP; ) {
        ImageDoc* old = app.texLru[i];
        if (old == &im || old == cur() || old == cmpB()) { i++; continue; }  // on screen
        if (old->tex) { glDeleteTextures(1, &old->tex); old->tex = 0; old->texDirty = true; }
        app.texLru.erase(app.texLru.begin() + i);
    }
}

// Round up to 1/2/5 x 10^n: a difference scale of "+-2000 DN" is readable, one of
// "+-1873.4 DN" is not (and the axis rule applies to legends too).
static float niceCeil(float v) {
    if (!(v > 0) || !std::isfinite(v)) return 1.0f;
    float e = powf(10.0f, floorf(log10f(v)));
    float m = v / e;
    return (m <= 1.0f ? 1.0f : m <= 2.0f ? 2.0f : m <= 5.0f ? 5.0f : 10.0f) * e;
}

// A-B as a signed image. Rebuilt only when an input, the scale or the mode
// changes: at 12 Mpx this is a ~60 ms pass, not something to do per frame.
static void ensureDiffTexture(const ImageDoc& a, const ImageDoc& b) {
    App::DiffTex& D = app.diff;
    int w = std::min(a.w, b.w), h = std::min(a.h, b.h);
    int ch = std::min(a.ch, b.ch);
    if (w <= 0 || h <= 0) return;

    if (app.diffGain <= 0) {   // auto scale: the 99.9th percentile of |A-B|
        static uint64_t autoKeyA = 0, autoKeyB = 0; static int autoRevA = -1, autoRevB = -1;
        if (autoKeyA != a.uid || autoKeyB != b.uid || autoRevA != a.dataRev || autoRevB != b.dataRev) {
            autoKeyA = a.uid; autoKeyB = b.uid; autoRevA = a.dataRev; autoRevB = b.dataRev;
            std::vector<float> mags;
            size_t total = (size_t)w * h;
            size_t step = std::max<size_t>(1, total / 200000);   // bounded sample
            // ...and the step is forced ODD, which is the f35a79e defect in its
            // last hiding place. Striding a one-dimensional index and wrapping
            // it (x = p % w) samples only even columns the moment step and w are
            // both even - and a sensor width essentially always is. On an RGGB
            // frame that leaves R and Gb with samples and Gr and B with none, so
            // this percentile - and with it the whole A/B difference display's
            // scaling - would be decided by half the sensor. An odd step walks
            // every column parity, and every row parity with it, because
            // consecutive samples advance x by an odd amount and wrap.
            if (step % 2 == 0) step++;
            mags.reserve(total / step + 1);
            for (size_t p = 0; p < total; p += step) {
                int x = (int)(p % (size_t)w), y = (int)(p / (size_t)w);
                float m = 0;
                for (int c = 0; c < ch; c++)
                    m = std::max(m, fabsf(a.sample(x, y, c) - b.sample(x, y, c)));
                if (std::isfinite(m)) mags.push_back(m);
            }
            float g = 1.0f;
            if (!mags.empty()) {
                size_t k = (size_t)(mags.size() * 0.999);
                if (k >= mags.size()) k = mags.size() - 1;
                std::nth_element(mags.begin(), mags.begin() + k, mags.end());
                g = mags[k];
            }
            app.diffAutoGain = niceCeil(g > 0 ? g : 1.0f);
        }
    }
    float gain = app.diffGain > 0 ? app.diffGain : app.diffAutoGain;

    if (D.tex && D.uidA == a.uid && D.uidB == b.uid && D.revA == a.dataRev &&
        D.revB == b.dataRev && D.gain == gain && D.absMode == app.diffAbs &&
        D.w == w && D.h == h)
        return;                                     // cache hit: nothing changed

    double t0 = nowSec();
    static std::vector<uint8_t> rgba;
    rgba.resize((size_t)w * h * 4);
    const float inv = 1.0f / std::max(gain, 1e-20f);
    size_t clipped = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = 0;
            for (int c = 0; c < ch; c++) {          // the channel that differs most
                float v = a.sample(x, y, c) - b.sample(x, y, c);
                if (fabsf(v) > fabsf(d)) d = v;
            }
            if (!std::isfinite(d)) d = 0;
            float t = d * inv;
            size_t o = ((size_t)y * w + x) * 4;
            float r, g2, bl;
            if (fabsf(t) > 1.0f) {                  // out of scale: unmistakable
                clipped++;
                r = 1.0f; g2 = 1.0f; bl = 0.2f;     // yellow
            } else if (app.diffAbs) {
                float m = fabsf(t);
                r = m; g2 = m; bl = m;
            } else if (t >= 0) {                    // A brighter: warm
                r = t; g2 = 0.35f * t; bl = 0.10f * t;
            } else {                                // B brighter: cool
                float m = -t;
                r = 0.10f * m; g2 = 0.45f * m; bl = m;
            }
            rgba[o + 0] = (uint8_t)(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 1] = (uint8_t)(std::clamp(g2, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 2] = (uint8_t)(std::clamp(bl, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 3] = 255;
        }
    }
    if (!D.tex) glGenTextures(1, &D.tex);
    glBindTexture(GL_TEXTURE_2D, D.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    D.uidA = a.uid; D.uidB = b.uid; D.revA = a.dataRev; D.revB = b.dataRev;
    D.gain = gain; D.absMode = app.diffAbs; D.w = w; D.h = h;
    D.clipped = (double)clipped / ((double)w * h);
    fprintf(stderr, "diff: %s - %s  full scale +-%g  (%.3f%% off scale, %.0f ms)\n",
            a.name.c_str(), b.name.c_str(), gain, D.clipped * 100,
            (nowSec() - t0) * 1000);
}

// upload/normalize into RGBA8 texture
// float pixels -> display RGBA, exactly as the screen shows them (range, gamma,
// colormap, CFA colorize). Split out of rebuildTexture so that EXPORTING a view
// runs the same code: a picture pasted into a report that does not match the
// screen is worse than no picture.
static void renderDocRGBA(ImageDoc& im, std::vector<uint8_t>& rgba) {
    rgba.resize((size_t)im.w * im.h * 4);
    const float ib = effBlack(im), iw = effWhite(im);
    float inv = 1.0f / std::max(iw - ib, 1e-20f);
    float invGamma = 1.0f / app.dispGamma;
    bool doGamma = fabsf(app.dispGamma - 1.0f) > 1e-3f;
    // Gamma through a table instead of 36 million powf() calls at 12 Mpx (235 ms
    // -> 76 ms measured), which is what makes dragging the range slider usable at
    // gamma 2.2. The table is in the SQRT domain: x^(1/g) is vertical at x=0, so
    // interpolating it there is worth 3 output codes of error, while the same
    // table over sqrt(x) is nearly straight and lands within 0.001 of exact.
    static float gammaLut[1025];
    static float gammaLutFor = -1;
    if (doGamma && gammaLutFor != app.dispGamma) {
        for (int i = 0; i <= 1024; i++) gammaLut[i] = powf(i / 1024.0f, 2.0f * invGamma);
        gammaLutFor = app.dispGamma;
    }
    auto applyGamma = [&](float x) {
        float f = sqrtf(x) * 1024.0f;
        int i = std::min((int)f, 1023);
        return gammaLut[i] + (gammaLut[i + 1] - gammaLut[i]) * (f - i);
    };
    bool cfaColor = im.ch == 1 && im.cfa != 0 && im.cfaColorize;
    const uint8_t* lut = nullptr;   // display plugin colormap (1ch only; CFA colorize wins)
    if (im.ch == 1 && !cfaColor && im.displayLut >= 0 &&
        im.displayLut < (int)plugin_host::displays().size())
        lut = plugin_host::displays()[im.displayLut].lut.data();
    int px = 0, py = 0;
    for (size_t p = 0; p < (size_t)im.w * im.h; p++, px++) {
        if (px == im.w) { px = 0; py++; }
        const float* src = &im.px()[p * im.ch];
        float r, g, b;
        if (lut) {
            float x = std::clamp((src[0] - ib) * inv, 0.0f, 1.0f);
            if (doGamma) x = applyGamma(x);
            int idx = (int)(x * 255.0f + 0.5f);
            rgba[p * 4 + 0] = lut[idx * 3];
            rgba[p * 4 + 1] = lut[idx * 3 + 1];
            rgba[p * 4 + 2] = lut[idx * 3 + 2];
            rgba[p * 4 + 3] = 255;
            continue;
        }
        if (cfaColor) {
            // running x/y instead of p%w and p/w: a 64-bit div+mod per pixel is
            // ~40% of this loop at 12 Mpx
            int c = cfaChannelAt(im, px, py);
            r = c == 0 ? src[0] : ib;
            g = (c == 1 || c == 2) ? src[0] : ib;
            b = c == 3 ? src[0] : ib;
        }
        else if (im.ch == 1) { r = g = b = src[0]; }
        else if (im.ch == 2) { r = src[0]; g = src[1]; b = ib; }
        else                 { r = src[0]; g = src[1]; b = src[2]; }
        float v[3] = { (r - ib) * inv, (g - ib) * inv, (b - ib) * inv };
        for (int c = 0; c < 3; c++) {
            float x = std::clamp(v[c], 0.0f, 1.0f);
            if (doGamma) x = applyGamma(x);
            rgba[p * 4 + c] = (uint8_t)(x * 255.0f + 0.5f);
        }
        rgba[p * 4 + 3] = 255;
    }
}

static void rebuildTexture(ImageDoc& im) {
    // One scratch buffer for the whole app: a 12 Mpx image is a 48 MB allocation,
    // and this runs on every range/gamma/LUT change.
    static std::vector<uint8_t> rgba;
    im.texBlack = effBlack(im); im.texWhite = effWhite(im);
    renderDocRGBA(im, rgba);
    if (!im.tex) glGenTextures(1, &im.tex);
    touchTex(im);
    glBindTexture(GL_TEXTURE_2D, im.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, im.texNearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, im.texNearest ? GL_NEAREST : GL_LINEAR);
    im.texDirty = false;
}
// ---------------------------------------------------------------- export
// Getting a picture OUT of the tool and into a report. Two things are wanted
// and they are not the same thing:
//   dot by dot  - one image pixel per output pixel, no resampling. What an
//                 image-quality reader needs: the noise is the subject, and a
//                 scaled screenshot invents and destroys exactly that.
//   as displayed - the canvas as it sits, zoom and overlays included. Not here
//                 yet (see docs/todo-open.md): it needs a framebuffer readback
//                 after the frame is rendered, and this machine cannot verify
//                 the result today.
// Both go through renderDocRGBA, so what leaves the app is what the screen
// showed - the same range, gamma, colormap and CFA colorize.

// PNG via miniz, which is already linked for .npz. No new dependency.
static std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> out;
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6, MZ_FALSE);
    if (!png) return out;
    out.assign((const uint8_t*)png, (const uint8_t*)png + len);
    mz_free(png);
    return out;
}

#ifdef _WIN32
// Two formats, because "paste" means different things to different programs:
// CF_DIB is what Word, PowerPoint and Excel read; the registered "PNG" format
// is what most browsers and chat clients prefer, and it keeps the alpha.
static bool clipboardPutImage(const std::vector<uint8_t>& rgba, int w, int h,
                              const std::vector<uint8_t>& png, std::string& err) {
    if (!OpenClipboard(nullptr)) { err = "the clipboard is held by another program"; return false; }
    EmptyClipboard();
    bool any = false;
    {   // CF_DIB: BITMAPINFOHEADER + BGRA, bottom-up
        const size_t px = (size_t)w * h * 4;
        HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + px);
        if (hm) {
            uint8_t* dst = (uint8_t*)GlobalLock(hm);
            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h;   // + = bottom-up
            bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
            bi.biSizeImage = (DWORD)px;
            memcpy(dst, &bi, sizeof bi);
            uint8_t* q = dst + sizeof bi;
            for (int y = 0; y < h; y++) {
                const uint8_t* src = &rgba[(size_t)(h - 1 - y) * w * 4];
                for (int x = 0; x < w; x++) {
                    q[x * 4 + 0] = src[x * 4 + 2];   // B
                    q[x * 4 + 1] = src[x * 4 + 1];   // G
                    q[x * 4 + 2] = src[x * 4 + 0];   // R
                    q[x * 4 + 3] = src[x * 4 + 3];
                }
                q += (size_t)w * 4;
            }
            GlobalUnlock(hm);
            if (SetClipboardData(CF_DIB, hm)) any = true; else GlobalFree(hm);
        }
    }
    if (!png.empty()) {
        UINT cfPng = RegisterClipboardFormatA("PNG");
        HGLOBAL hp = cfPng ? GlobalAlloc(GMEM_MOVEABLE, png.size()) : nullptr;
        if (hp) {
            memcpy(GlobalLock(hp), png.data(), png.size());
            GlobalUnlock(hp);
            if (SetClipboardData(cfPng, hp)) any = true; else GlobalFree(hp);
        }
    }
    CloseClipboard();
    if (!any) err = "the clipboard refused the image";
    return any;
}
#endif

// The ONE place an export is produced, so the file and the clipboard can never
// disagree about what the picture is.
static bool exportDocRGBA(ImageDoc* im, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!im || im->w < 1 || im->h < 1) return false;
    w = im->w; h = im->h;
    renderDocRGBA(*im, rgba);
    return true;
}

static void copyViewDotByDot() {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast("nothing to copy", true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
#ifdef _WIN32
    std::string err;
    if (clipboardPutImage(rgba, w, h, png, err))
        toast("copied " + std::to_string(w) + "x" + std::to_string(h) + " (dot by dot)");
    else
        toast("copy failed: " + err, true);
#else
    // No image clipboard without a toolkit here: write the PNG next to nothing
    // and hand over the PATH, which every file dialog and chat client accepts.
    std::string path = (std::filesystem::temp_directory_path() /
                        ("viewer-" + std::to_string(im->uid) + ".png")).u8string();
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    f.close();
    ImGui::SetClipboardText(path.c_str());
    toast("wrote " + path + " (path on the clipboard)");
#endif
}

static void saveViewPng(const std::string& path) {
    ImageDoc* im = cur();
    std::vector<uint8_t> rgba; int w = 0, h = 0;
    if (!exportDocRGBA(im, rgba, w, h)) { toast("nothing to save", true); return; }
    std::vector<uint8_t> png = encodePng(rgba, w, h);
    if (png.empty()) { toast("PNG encode failed", true); return; }
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write " + path, true); return; }
    f.write((const char*)png.data(), (std::streamsize)png.size());
    toast("saved " + baseName(path) + "  " + std::to_string(w) + "x" + std::to_string(h));
}

static void setFilter(ImageDoc& im, bool nearest) {
    if (im.texNearest == nearest || !im.tex) { im.texNearest = nearest; return; }
    im.texNearest = nearest;
    glBindTexture(GL_TEXTURE_2D, im.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}

static void markAllTexDirty() {
    for (auto& d : app.images) d->texDirty = true;
}
// The Range panel's one mode control (0 auto per frame / 1 per stack /
// 2 linked). It lives here, not inside the combo, because choosing a mode has
// to act on the frame ALREADY on screen: the per-frame policy is otherwise
// applied only by selectImage, on a frame change, and the mode read as dead
// until the first step (report 2026-08-03).
static void applyRangeMode(int mode, ImageDoc* on) {
    bool wasLinked = app.linkRange;
    app.linkRange = mode == 2;
    app.rangeScope = mode == 0 ? 0 : 1;
    if (on) {
        if (app.linkRange && !wasLinked) {     // seed from what is on screen
            app.linkBlack = on->black; app.linkWhite = on->white;
        }
        if (mode == 0 && !app.linkRange) { defaultRange(*on); on->texDirty = true; }
    }
    if (app.linkRange != wasLinked) markAllTexDirty();
}
// Linking is an overlay, never a rewrite: each image keeps its own range, so
// unlinking restores exactly what every image had before.
static ImageDoc* cmpB();          // fwd: the B side, or null when compare is off
// The compare display range - and "compare" means EVERY pane, not just the
// A/B pair: two images stretched differently cannot be compared, and with
// tiles that holds for slot C exactly as it holds for B (report 2026-07-30,
// 「Auto FitするのがA,BまででCが変わらない」). Mode 2 fits every compared
// side to the union of what the current frames actually contain, recomputed
// as you step. Mode 1 means "the reference side dictates": A's black-white
// for every other letter. The slots join only while compare is ON - the
// letters survive compare-off as seats, but a seat is not a comparison.
// A, B and the slots may now name the SAME document (resolveB no longer goes
// quiet there), which this handles by construction: a union of min/max does
// not care how many times a side appears, and the `rs.doc != b` below is kept
// only so a duplicate cannot eat one of the fixed 16 places. It changes no
// number - unlike the panels, where dropping a duplicate side dropped a curve.
static bool abRange(const ImageDoc& im, float& lo, float& hi) {
    if (app.compareRangeMode == 0) return false;
    ImageDoc* a = cur();
    if (!a) return false;
    ImageDoc* b = cmpB();
    // who is being compared: A, B if it resolves, every resolved slot. A tiled
    // canvas can hold A + C + D with no B at all, so B is optional - what is
    // required is that there be someone besides A to compare WITH.
    ImageDoc* parts[2 + 14];    // A + B + every slot letter (SLOT_LETTERS)
    int np = 0;
    parts[np++] = a;
    if (b) parts[np++] = b;
    if (app.compareMode != App::CmpOff && !app.cmpExtra.empty())
        for (const ResolvedSlot& rs : resolveSlots())
            if (rs.doc != b && np < (int)(sizeof parts / sizeof *parts))
                parts[np++] = rs.doc;
    if (np < 2) return false;
    bool member = false;
    for (int i = 0; i < np; i++) if (parts[i] == &im) member = true;
    if (!member) return false;
    if (app.compareRangeMode == 1) { lo = a->black; hi = a->white; return true; }
    lo = parts[0]->vmin;                      // union of the CONTENT, not of the
    hi = parts[0]->vmax;                      // stretches
    for (int i = 1; i < np; i++) {
        lo = std::min(lo, parts[i]->vmin);
        hi = std::max(hi, parts[i]->vmax);
    }
    return hi > lo;
}
static float effBlack(const ImageDoc& im) {
    if (app.linkRange) return app.linkBlack;
    float lo, hi;
    if (abRange(im, lo, hi)) return lo;
    return im.black;
}
static float effWhite(const ImageDoc& im) {
    if (app.linkRange) return app.linkWhite;
    float lo, hi;
    if (abRange(im, lo, hi)) return hi;
    return im.white;
}

static void setRange(ImageDoc& im, float black, float white) {
    if (!(white > black)) return;
    if (app.linkRange) {
        app.linkBlack = black; app.linkWhite = white;
        markAllTexDirty();            // shared value changed: every image re-renders
    } else {
        im.black = black; im.white = white; im.texDirty = true;
    }
}
static void forgetTexture(ImageDoc* im) {
    app.texLru.erase(std::remove(app.texLru.begin(), app.texLru.end(), im), app.texLru.end());
}
// One place that drops an image from every cache that can name it.
static void forgetImage(ImageDoc* im) {
    if (app.ana.img == im) app.ana.img = nullptr;
    // BOTH slots: the image being dropped may be the B side, and a cache still
    // naming it is a dangling pointer the next draw would follow
    for (int k = 0; k < 2; k++) {
        if (app.hist[k].img == im) { app.hist[k].img = nullptr; app.hist[k].uid = 0; }
        if (app.proj[k].img == im) { app.proj[k].img = nullptr; app.proj[k].uid = 0; }
        app.temporal[k].seqId = -1;
    }
    // ...and EVERY lettered slot, for the same reason. Closing C shifts D down
    // into C's cache entry, and the entry still held C's ImageDoc*: the key
    // check (uid/dataRev) fails and it is recomputed before it is read, so the
    // stale pointer was never followed - but "never followed" is a property of
    // the read path, not of the cache, and the next reader is one edit away
    // from following it. The cache says what it holds or it says nothing.
    // (Reachable through a slot stack's preview->full swap even before reload
    // existed - docs/reference-design.md §3.2 - hence temporalExtra too.)
    for (auto& H : app.histExtra) if (H.img == im) { H.img = nullptr; H.uid = 0; }
    for (auto& P : app.projExtra) if (P.img == im) { P.img = nullptr; P.uid = 0; }
    for (auto& T : app.temporalExtra) T.seqId = -1;
    forgetTexture(im);
    app.imagesRev++;
}

static void ensureSession(remote::Session& ses, std::string& sesHost, int& sesPort,
                          const std::string& host, int port, const std::string& exe,
                          std::string& errOut) {
    std::string err;
    // startOn, not start(): start() hardwires port 0, i.e. plain
    // `ssh host` - a DIFFERENT machine when the user named a port
    if (!ses.alive() || sesHost != host || sesPort != port) {
        if (!ses.startOn(host, port, exe, err)) {
            errOut = err;
        } else {
            sesHost = host;
            sesPort = port;
        }
    }
}

// ---- background full-resolution fetch (remote frames) -------------------------
// The preview is already on screen; the real pixels arrive here on their own ssh
// connection and are swapped in by the UI thread.
static void rfWorker() {
    remote::Session ses;
    ses.setAbort(&app.rfStop);        // a blocked read gives up when Quit asks
    std::string sesHost = "\n";                  // impossible: force first connect
    int sesPort = -1;
    while (!app.rfStop) {
        App::RFetchJob job;
        {
            std::unique_lock<std::mutex> lk(app.rfMtx);
            app.rfCv.wait(lk, []{ return app.rfStop || !app.rfQueue.empty(); });
            // Stop wins over pending work, as it did when this was a poll: the
            // old `while (!app.rfStop)` was checked BEFORE any dequeue. Asking
            // for the queue to be empty as well lets a worker that was notified
            // with a job and stopped before it could reacquire the mutex run one
            // more fetch INSIDE stopRemoteFetcher's join(), with the UI thread
            // waiting on it. The predicate above already guarantees the queue is
            // non-empty whenever the flag is false, so there is nothing to lose.
            if (app.rfStop) break;
            job = std::move(app.rfQueue.front());
            app.rfQueue.erase(app.rfQueue.begin());
            app.rfBusyUid = job.uid;   // inside the lock: the dedupe reads the
                                       // queue and this uid under the same mtx
        }
        App::RFetchDone d;
        d.uid = job.uid;
        d.url = job.url; d.name = job.name; d.note = job.note;
        d.frame = job.frame; d.seqId = job.seqId; d.seqIndex = job.seqIndex;
        d.gen = job.gen;
        d.bytes = job.bytes;
        d.mtime = job.mtime; d.fsize = job.fsize;
        std::string host, rpath, err;
        int port = 0;
        if (!remote::parseUrl(job.url, host, rpath, &port)) {
            d.err = "bad remote url";
        } else {
            ensureSession(ses, sesHost, sesPort, host, port, job.exe, d.err);
            if (d.err.empty()) {
                int w = 0, h = 0, ch = 0;
                // the server clamps the rect, so "huge" means "the whole frame"
                if (!ses.tile(rpath, job.frame, 0, 0, 1 << 30, 1 << 30, 1,
                              d.data, w, h, ch, d.dtype, err)) {
                    d.err = err;
                } else {
                    d.w = w; d.h = h; d.ch = ch;
                    float mn = FLT_MAX, mx = -FLT_MAX;
                    for (float v : d.data)
                        if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
                    if (mn > mx) { mn = 0; mx = 1; }
                    if (mn == mx) mx = mn + 1;
                    d.vmin = mn; d.vmax = mx;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(app.rfMtx);
            app.rfDone.push_back(std::move(d));
            app.rfBusyUid = 0;         // done is QUEUED before busy clears, so a
                                       // dedupe that misses both still lands
                                       // behind the result it would duplicate
        }
        glfwPostEmptyEvent();                    // wake the UI to swap it in
    }
}

static void rfEnqueue(App::RFetchJob job) {
    job.gen = app.rfGen;
    // how the peer is invoked, frozen NOW: the UI thread edits remoteExe freely
    std::string host, rpath;
    remote::parseUrl(job.url, host, rpath);
    job.exe = app.remoteExe.empty() ? (host.empty() ? app.exePath
                                                : std::string(REMOTE_HOME) + "/viewer-serve")
                               : app.remoteExe;
    {
        // counters BEFORE the push, under the lock: the worker could finish job 1
        // while jobs 2..N are still being enqueued, and the pending==0 reset would
        // restart the progress display mid-stack
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfPending++;
        app.rfTotal++;
        app.rfBytesInFlight += job.bytes;
        // an unpromoted preview is only ALLOWED the link when nothing a user
        // registered is waiting: normal jobs enter ahead of every low one
        if (job.low) {
            app.rfQueue.push_back(std::move(job));
        } else {
            auto it = app.rfQueue.begin();
            while (it != app.rfQueue.end() && !it->low) ++it;
            app.rfQueue.insert(it, std::move(job));
        }
    }
    app.rfCv.notify_one();
    if (!app.rfThread.joinable()) app.rfThread = std::thread(rfWorker);
}

static void requestFullRemote(const ImageDoc* d, bool low = false) {
    if (d->src->remoteUrl.empty() || d->src->remoteStep <= 1) return;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        // the job the worker is running RIGHT NOW left the queue but is still
        // this uid's fetch: enqueueing another would transfer the frame twice
        // and land a second in-place swap on a by-then-registered source (the
        // landing guard drops it, but the transfer is real bytes)
        if (app.rfBusyUid == d->uid) return;
        for (auto it = app.rfQueue.begin(); it != app.rfQueue.end(); ++it)
            if (it->uid == d->uid) {
                if (it->low && !low) {         // promotion: same job, real priority
                    App::RFetchJob j = std::move(*it);
                    j.low = false;
                    app.rfQueue.erase(it);
                    auto at = app.rfQueue.begin();
                    while (at != app.rfQueue.end() && !at->low) ++at;
                    app.rfQueue.insert(at, std::move(j));
                }
                return;
            }
    }
    App::RFetchJob j;
    j.url = d->src->remoteUrl; j.frame = d->src->remoteFrame; j.uid = d->uid;
    j.low = low;
    // the full frame REPLACES the decimated one, so only the growth is new
    {
        size_t have = d->px().size() * sizeof(float);
        size_t full = have * (size_t)d->src->remoteStep * d->src->remoteStep;
        j.bytes = full > have ? full - have : 0;
    }
    rfEnqueue(std::move(j));
}

// UI thread: swap arrived frames in. After this an ex-preview is a local image.
static App::SeqInfo* seqInfo(int id);   // fwd: closed-stack results are dropped
static void pumpRemoteFetch() {
    std::vector<App::RFetchDone> batch;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        batch.swap(app.rfDone);
    }
    for (auto& d : batch) {
        if (d.gen != app.rfGen) continue;   // outlived a Close All: drop entirely
        app.rfPending--;
        app.rfBytesInFlight -= std::min(app.rfBytesInFlight.load(), d.bytes);
        app.rfFetched++;
        if (app.rfPending <= 0) {
            app.rfBytesInFlight = 0;        // nothing outstanding: no drift
            if (app.rfFetched > 1)
                fprintf(stderr, "remote: fetch complete (%d items)\n", app.rfFetched.load());
            app.rfTotal = 0;
            app.rfFetched = 0;
        }
        if (d.uid == 0) {                        // a new frame of a remote stack
            // Closed while this fetch was in flight: closeStack swept the QUEUE,
            // but the one job the worker was already running could not be
            // removed there - its result lands here and must be dropped, or a
            // closed stack regrows one orphan frame at a time.
            if (d.seqId != 0 && !seqInfo(d.seqId)) continue;
            if (!d.err.empty()) {
                if (app.seqNote.empty())         // first error explains the gap
                    app.seqNote = "remote: " + d.err;
                continue;
            }
            auto doc = std::make_unique<ImageDoc>();
            doc->name = d.name;
            doc->note = d.note;                  // the head frame's note, verbatim: the
                                                 // Inspector's source row is a property
                                                 // of the STACK, so it never appears or
                                                 // vanishes as the user steps frames
            FrameSource& S = *doc->src;
            S.mtime = d.mtime;                   // the enqueue-time baseline: every
            S.fsize = d.fsize;                   // frame of one open, one stat epoch
                                                 // (0/0 for ssh://)
            S.path = d.url;
            S.remoteUrl = d.url;
            S.remoteFrame = d.frame;
            S.dtype = d.dtype;
            S.w = d.w; S.h = d.h; S.ch = d.ch;
            S.data = std::move(d.data);
            S.vmin = d.vmin; S.vmax = d.vmax;
            doc->syncMirrors();
            // §6.2: the same url+frame may already be resident (the same remote
            // folder opened twice) - the arrival grafts as a second membership
            // on the resident source instead of keeping a second copy. The
            // fetch itself already happened; what is deduplicated is residency.
            shareOrRegisterSource(*doc);
            doc->seqId = d.seqId; doc->seqIndex = d.seqIndex;
            doc->uid = app.nextUid++;
            doc->texDirty = true;
            // frames of one stack share the display range (like the local
            // loader) and its batch (frame ⊂ stack ⊂ batch)
            bool ranged = false;
            for (const auto& q : app.images)
                if (q->seqId == d.seqId) {
                    doc->black = q->black; doc->white = q->white;
                    doc->batchId = q->batchId;
                    ranged = true;
                    break;
                }
            if (!ranged) defaultRange(*doc);
            app.images.push_back(std::move(doc));  // quiet: never steals selection
            app.imagesRev++;
            continue;
        }
        ImageDoc* im = nullptr;
        for (auto& q : app.images) if (q->uid == d.uid) { im = q.get(); break; }
        if (!im) continue;                       // closed while fetching
        // A DUPLICATE full-res landing (a promote raced the in-flight fetch:
        // requestFullRemote's dedupe reads the queue, and a job the worker had
        // already dequeued was invisible to it). The first landing set step=1
        // and REGISTERED the source - it may be shared by now - so a second
        // in-place swap here would mutate a registered source outside the
        // g_srcRegMtx contract and outside any walk. Drop it, error included
        // (remoteErr on a registered source would un-share it silently). If
        // the peer's file changed between the two fetches, that is Watch's to
        // notice, the same answer as everywhere else.
        if (im->src->remoteStep <= 1) continue;
        if (!d.err.empty()) {
            im->src->remoteErr = d.err;
            toast("remote: " + d.err, true);
            continue;
        }
        int stepBefore = im->src->remoteStep;
        FrameSource& S = *im->src;
        S.data = std::move(d.data);
        S.w = d.w; S.h = d.h; S.ch = d.ch;
        S.dtype = d.dtype;
        S.remoteStep = 1;
        S.remoteErr.clear();
        S.vmin = d.vmin; S.vmax = d.vmax;        // measured on the worker
        S.rev++;      // in-place pixel replacement (§2.2's "reload" family):
                      // stage 1's only rev writer; the invalidation walk is stage 5
        // §6.2: remoteStep=1 just made this source shareable, and it was
        // guaranteed unregistered until now (step>1 sources never pass
        // srcShareable, previews never register at all). Register it - or
        // ADOPT a resident of the same url+frame that appeared meanwhile:
        // this doc is the SOLE owner of an ex-preview source, the resident
        // holds exactly the full-resolution bytes this fetch delivered, and
        // the invalidation just below (mirrors, dataRev, forgetImage) is the
        // same walk an adoption needs - so adopting changes no pixels and
        // drops "same url opened twice" back to 1x resident bytes. Previews
        // stay out of the registry entirely; promotion re-runs this.
        if (!im->preview) shareOrRegisterSource(*im);
        im->syncMirrors();
        size_t p = im->name.find("  (1/");       // drop the preview marker
        if (p != std::string::npos) im->name.erase(p);
        im->dataRev++;
        im->texDirty = true;
        forgetImage(im);                         // caches hold the preview's numbers
        if (stepBefore > 1) {
            if (im == cur()) {
                // same pixels on screen, now through finer data: rescale the
                // shared view so nothing appears to move at the swap
                app.view.zoom = std::max(app.view.zoom / stepBefore, 1.0f / 512);
                app.view.center.x *= stepBefore;
                app.view.center.y *= stepBefore;
            } else {
                // not on screen: remember, and rescale when it is selected
                im->pendingViewScale = (float)stepBefore;
            }
        }
        app.imagesRev++;
        fprintf(stderr, "remote: full resolution %dx%d %dch for %s\n",
                d.w, d.h, d.ch, im->name.c_str());
    }
}

static void stopRemoteFetcher() {
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfStop = true;
    }
    app.rfCv.notify_all();
    if (app.rfThread.joinable()) app.rfThread.join();
}

// ---- background MEASURE (server-side analysis) --------------------------------
// Its own ssh connection: a 300-frame aggregate can hold the pipe for seconds,
// and that must not stall the tile fetches on the other worker.
static void mWorker() {
    remote::Session ses;
    ses.setAbort(&app.mStop);         // a blocked read gives up when Quit asks
    std::string sesHost = "\n";
    int sesPort = -1;
    while (!app.mStop) {
        App::MJob job;
        {
            std::unique_lock<std::mutex> lk(app.mMtx);
            app.mCv.wait(lk, []{ return app.mStop || !app.mQueue.empty(); });
            // Stop wins over pending work - the third time this exact line has
            // arrived, after rbWorker and rfWorker (3a5f724). The loop it
            // replaces checked the flag BEFORE dequeuing, so a stop abandoned
            // the queue; asking for the queue to be empty as well lets a worker
            // that was notified with a job and stopped before it could reacquire
            // the mutex run one more measurement inside stopMeasureWorker's
            // join(), with the UI thread waiting on it. The predicate already
            // guarantees the queue is non-empty whenever the flag is false.
            if (app.mStop) break;
            job = std::move(app.mQueue.front());
            app.mQueue.erase(app.mQueue.begin());
        }
        App::MDone d;
        d.token = job.token;
        std::string host, rpath, err;
        int port = 0;
        if (!remote::parseUrl(job.url, host, rpath, &port)) { d.err = "bad remote url"; }
        else {
            d.host = host;
            ensureSession(ses, sesHost, sesPort, host, port, job.exe, d.err);
            if (d.err.empty()) {
                remote::MeasureReq q;
                q.op = job.op;
                q.cfaType = job.cfaType; q.cfaPattern = job.cfaPattern;
                q.black = job.black; q.white = job.white;
                if (job.files.empty()) q.paths = { rpath };
                else                   q.paths = job.files;   // one file per frame
                if (job.rw > 0 && job.rh > 0)
                    q.rois.push_back({ job.rx, job.ry, job.rw, job.rh });
                if (!ses.measure(q, d.res, err)) d.err = err;
                else d.ok = true;
            }
        }
        {
            std::lock_guard<std::mutex> lk(app.mMtx);
            app.mDone.push_back(std::move(d));
        }
        glfwPostEmptyEvent();
    }
}

static void mEnqueue(App::MJob job) {
    if (job.exe.empty()) {
        std::string h, rp2;
        remote::parseUrl(job.url, h, rp2);
        job.exe = app.remoteExe.empty()
            ? (h.empty() ? app.exePath : std::string(REMOTE_HOME) + "/viewer-serve")
            : app.remoteExe;
    }
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        app.mPending++;
        app.mQueue.push_back(std::move(job));
    }
    app.mCv.notify_one();
    if (!app.mThread.joinable()) app.mThread = std::thread(mWorker);
}

static void stopMeasureWorker() {
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        app.mStop = true;
    }
    app.mCv.notify_all();
    if (app.mThread.joinable()) app.mThread.join();
}

// Pull one scalar / series out of a server MeasureResult column 0 by key.
static double mFindNum(const remote::MeasureResult& r, const char* key, double dflt = 0) {
    if (r.cols.empty()) return dflt;
    for (const auto& it : r.cols[0]) if (it.kind == 0 && it.key == key) return it.num;
    return dflt;
}
static const remote::MeasureSeries* mFindSeries(const remote::MeasureResult& r, const char* name) {
    for (const auto& s : r.series) if (s.name == name) return &s;
    return nullptr;
}

static void pumpMeasure() {
    std::vector<App::MDone> batch;
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        batch.swap(app.mDone);
    }
    for (auto& d : batch) {
        app.mPending--;
        // the A slot or the explicitly-measured B slot, by token
        App::ServerTemporal* Sp = nullptr;
        if (app.srvTemporal.token && d.token == app.srvTemporal.token) Sp = &app.srvTemporal;
        else if (app.srvTemporalB.token && d.token == app.srvTemporalB.token)
            Sp = &app.srvTemporalB;
        if (!Sp) continue;                                // superseded request
        App::ServerTemporal& S = *Sp;
        S.pending = false;
        S.host = d.host;
        if (!d.ok) { S.valid = false; S.err = d.err; continue; }
        S.err.clear();
        S.valid = true;
        S.frames = d.res.framesUsed;
        // The peer answers per CFA plane when it was told the mosaic (keys
        // suffixed " R" / " Gr" / " Gb" / " B", serve.cpp planeKey) and with
        // un-suffixed keys when it was not. Take whichever shape arrived and
        // record which it was - collapsing a four-plane reply onto its R plane
        // and calling it "the overall figure" is worse than the pooled number.
        auto has = [&](const char* k) {
            if (d.res.cols.empty()) return false;
            for (const auto& it : d.res.cols[0]) if (it.kind == 0 && it.key == k) return true;
            return false;
        };
        S.nPl = has("sigma_t [DN] R") ? 4 : 1;
        for (int p = 0; p < S.nPl; p++) {
            std::string sfx = S.nPl > 1 ? std::string(" ") + CFA_CH_NAMES[p] : std::string();
            S.mean[p] = mFindNum(d.res, ("mean [DN]" + sfx).c_str());
            S.tempNoise[p] = mFindNum(d.res, ("sigma_t [DN]" + sfx).c_str());
            S.fixedPattern[p] = mFindNum(d.res, ("sigma_fpn [DN]" + sfx).c_str());
            S.totalNoise[p] = mFindNum(d.res, ("sigma_tot [DN]" + sfx).c_str());
        }
        if (const auto* fm = mFindSeries(d.res, "frame mean")) { S.idx = fm->xs; S.frameMean = fm->ys; }
        if (const auto* fs = mFindSeries(d.res, "frame std"))  { S.frameStd = fs->ys; }
        fprintf(stderr, "remote: server temporal over %d frames, %d plane(s) - sigma_t "
                        "%.4g, sigma_fpn %.4g [%s]\n", S.frames, S.nPl, S.tempNoise[0],
                S.fixedPattern[0], d.res.serverLoc ? "gpu" : "cpu");
    }
}

// ---- close, per layer (docs/terminology.md) -----------------------------------
// The canon: Close on a stack member closes the STACK - frames vanishing one at
// a time from a measurement set was the reported bug. Ctrl+Alt+W stays as the
// one-frame escape hatch.
static std::vector<int> framesOfSeq(int seqId);   // fwd (sequence helpers below)
static App::SeqInfo* seqInfo(int id);             // fwd
static void stopSequenceLoader();                 // fwd
static void pruneEmptyBatches();                  // fwd (defined with the moves)
// The value a stack's NAME suggests, and the text it was read out of. Declared
// here because the picker proposes one per group long before the linearity
// code that defines it (the default argument lives on this declaration only).
static double extractLevelFromName(const std::string& name, std::string* srcOut = nullptr);

// ---- series (系列), the model (docs/terminology.md, docs/series-plan.md) -------
// No UI here on purpose: the invariants have to hold before anything can draw
// them. Series are few and members are tens, so every lookup is a plain walk.
static App::Series* seriesById(int id) {
    for (auto& s : app.series) if (s.id == id) return &s;
    return nullptr;
}
// Reverse membership. The canon: a stack belongs to AT MOST ONE series, so the
// first hit is the only hit (seriesAudit proves it, addToSeries maintains it).
static App::Series* seriesOfStack(int seqId) {
    if (seqId == 0) return nullptr;
    for (auto& s : app.series)
        for (const auto& m : s.members)
            if (m.seqId == seqId) return &s;
    return nullptr;
}
// The batch a stack is in, read off its frames (the frames own batchId).
// 0 = the stack has no frames resident, so it claims no batch.
static int batchOfStack(int seqId) {
    for (const auto& d : app.images) if (d->seqId == seqId) return d->batchId;
    return 0;
}
static std::string batchNameOf(int batchId) {
    for (const auto& b : app.batches) if (b.id == batchId) return b.name;
    return {};
}
// Default name until a human gives it one (docs/terminology.md).
static int newSeries(int batchId, const std::string& name) {
    App::Series s;
    s.id = app.nextSeriesId++;
    s.batchId = batchId;
    s.name = name.empty() ? batchNameOf(batchId) + " 掃引" : name;
    app.series.push_back(std::move(s));
    app.imagesRev++;                  // the Files grouping caches on this
    return app.series.back().id;
}
// Drop one stack from whatever series holds it. Returns the series' id, or 0.
static int removeFromSeries(int seqId) {
    for (auto& s : app.series)
        for (auto it = s.members.begin(); it != s.members.end(); ++it)
            if (it->seqId == seqId) {
                s.members.erase(it);
                app.imagesRev++;
                return s.id;
            }
    return 0;
}
// Add a stack to a series. Fails (false) when the stack is not in the series'
// batch - the canon's strict containment, enforced rather than papered over:
// the caller's answer is "Move to batch first", never "we widened the series".
// Adding a stack that already sits in another series MOVES it (at most one).
static bool addToSeries(int seriesId, int seqId, double value, bool include = true) {
    App::Series* S = seriesById(seriesId);
    if (!S || seqId == 0 || !seqInfo(seqId)) return false;
    int b = batchOfStack(seqId);
    if (b != S->batchId) return false;
    for (auto& m : S->members)
        if (m.seqId == seqId) { m.value = value; m.include = include; return true; }
    removeFromSeries(seqId);
    S->members.push_back({ seqId, value, include });
    app.imagesRev++;
    return true;
}
// A series with nothing in it is not a work in progress, it is litter: the
// creation path leaves one member behind, closes take the rest away.
static void pruneEmptySeries() {
    for (auto it = app.series.begin(); it != app.series.end();)
        if (it->members.empty()) {
            if (app.curSeriesId == it->id) app.curSeriesId = 0;
            it = app.series.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
}
// Text -> a parameter value, and NOTHING ELSE to a number. atof() answers 0 for
// every string it cannot read, and 0 in a series is not a missing point: it is
// the dark stack the offset is anchored to and the read noise is MEASURED in
// (linRecompute). "-", "", "1e", "notanumber" and a truncated write all mean the
// same thing here - UNSET, which the whole layer already knows how to show and
// how to keep out of a fit.
static double parseSeriesValue(const char* s) {
    const double NOTSET = std::numeric_limits<double>::quiet_NaN();
    if (!s) return NOTSET;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || (s[0] == '-' && s[1] == '\0')) return NOTSET;   // "-" = unset
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return NOTSET;                     // nothing numeric at all
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return NOTSET;                         // trailing junk: "1e", "12x"
    return std::isfinite(v) ? v : NOTSET;            // inf/nan are not measurements
}
// A double as text that reads back as the SAME double. The session file is
// written at setprecision(9), which is right for a view offset and wrong for the
// axis a fit is taken against: 1234567.89 comes back as 1234570. Nine digits
// first, so every value that already round-tripped keeps its old spelling.
static std::string fmtExact(double v) {
    char b[48];
    for (int p = 9; p < 17; p++) {
        snprintf(b, sizeof b, "%.*g", p, v);
        if (strtod(b, nullptr) == v) return b;
    }
    snprintf(b, sizeof b, "%.17g", v);
    return b;
}
// The Linearity panel's numbers describe ONE set of members with ONE set of
// values on the axis, and every label around them is read live off the series.
// Change the series and a fit left standing is not merely old, it is relabelled:
// lux measurements headed "DN/ms". linFitStale keeps the per-stack MEASUREMENTS
// (they are DN whatever the axis says) and drops only the fit; linInvalidate
// drops the rows too, for when the membership itself moved.
static void linFitStale() { app.lin.fitValid = false; app.lin.rev++; }
static void linInvalidate() {
    app.lin.rows.clear();
    app.lin.seriesId = 0;
    app.lin.nPts = 0;
    linFitStale();
}
// Points a fit would actually use: included, value SET (NaN is "unset", never
// 0), and the stack still there.
static int seriesFitPoints(const App::Series& S) {
    int n = 0;
    for (const auto& m : S.members)
        if (m.include && std::isfinite(m.value) && seqInfo(m.seqId)) n++;
    return n;
}
static const char* seriesKindName(int k) {
    switch (k) {
        case App::Series::KPtc:         return "PTC";
        case App::Series::KTemperature: return "temperature";
        case App::Series::KOther:       return "other";
        default:                        return "linearity";
    }
}
// Whether the Linearity panel's EQUATIONS are the ones this series asked for.
// The canon (docs/terminology.md: 測定の種類 ... fit の式と出す量が決まる) makes
// the kind decide that, and this panel knows exactly two: a straight response
// against the swept parameter, and the photon transfer taken alongside it. A
// temperature run pushed through them comes out as a "sensitivity" in DN per
// degree, a system gain K read off stacks that differ in temperature rather
// than in signal, and a read noise "measured in the level-0 stack" - meaning
// whichever stack happened to sit at 0 degrees.
static bool seriesFitKind(const App::Series& S) {
    return S.kind == App::Series::KLinearity || S.kind == App::Series::KPtc;
}
// Two points, a named unit, and a kind whose equation this is. A series of one
// is perfectly legal (it is being built); it just cannot be fitted yet. An unset
// unit is the same kind of "not yet" - fitting without one would print numbers
// per nothing.
static bool seriesCanFit(const App::Series& S) {
    return seriesFitKind(S) && S.unit[0] != '\0' && seriesFitPoints(S) >= 2;
}
// Every invariant the canon states, checked exhaustively. Used by
// --series-selftest after every mutation; cheap enough to call from anywhere.
static bool seriesAudit(std::string& why) {
    std::vector<int> seen;
    for (const auto& S : app.series) {
        if (S.members.empty()) { why = "series '" + S.name + "' has no members"; return false; }
        bool haveBatch = false;
        for (const auto& b : app.batches) if (b.id == S.batchId) haveBatch = true;
        if (!haveBatch) { why = "series '" + S.name + "' points at a dead batch"; return false; }
        for (const auto& m : S.members) {
            if (!seqInfo(m.seqId)) {
                why = "series '" + S.name + "' member seq " + std::to_string(m.seqId) +
                      " does not exist";
                return false;
            }
            for (const auto& d : app.images)
                if (d->seqId == m.seqId && d->batchId != S.batchId) {
                    why = "series '" + S.name + "' member seq " + std::to_string(m.seqId) +
                          " has a frame in batch " + std::to_string(d->batchId) +
                          " (series batch " + std::to_string(S.batchId) + ")";
                    return false;
                }
            for (int s : seen)
                if (s == m.seqId) {
                    why = "stack " + std::to_string(m.seqId) + " is in two series";
                    return false;
                }
            seen.push_back(m.seqId);
        }
    }
    why.clear();
    return true;
}

// Close a set of images by index: erase in descending order so the indices stay
// valid, one forget per cache that can name them, current re-picked at the end.
static void closeImages(std::vector<int> idxs) {
    if (idxs.empty()) return;
    std::sort(idxs.begin(), idxs.end());
    idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
    uint64_t curUid = cur() ? cur()->uid : 0;
    bool closedCur = false;
    for (int i = (int)idxs.size() - 1; i >= 0; i--) {
        int idx = idxs[i];
        if (idx < 0 || idx >= (int)app.images.size()) continue;
        ImageDoc* im = app.images[idx].get();
        if (im->uid == curUid) closedCur = true;
        if (im->uid == app.previewUid) app.previewUid = 0;
        forgetImage(im);
        if (im->tex) glDeleteTextures(1, &im->tex);
        app.images.erase(app.images.begin() + idx);
    }
    if (curUid) {                     // re-point current: same doc if it survived
        if (!closedCur) {
            app.current = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->uid == curUid) { app.current = i; break; }
        }
        if (closedCur || app.current < 0) {
            app.current = app.images.empty() ? -1
                        : std::min(idxs.front(), (int)app.images.size() - 1);
            app.fitRequested = true;
        }
    } else if (app.current >= (int)app.images.size()) {
        app.current = (int)app.images.size() - 1;
    }
    app.imagesRev++;
    if (!resolveB()) {   // B went with them: a later same-name file must not
        app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
    }
    pruneEmptyBatches();
}

// What the last close SAID about pixels that survived it (§4's survival
// notice), as state a selftest can read - the panel is OpenGL this machine
// cannot screenshot, same pattern as g_filesBadgeProbe. Empty = the last
// close freed everything it held (and said nothing, as before sharing).
static std::string g_lastCloseNote;

// The survival scan behind that notice, over the WHOLE closing set of ONE
// user action: closeBatch/closeSeries hand every doc they are about to close
// here at once, so the sentence can never name a "survivor" the same action
// then frees (a per-member scan did exactly that for a batch holding a source
// stack and its derived stack). Counts are UNIQUE PIXELS, not memberships -
// source+derived closing together is N frames, not 2N. Sources adopted by an
// in-flight load but still queued in app.seqReady count as ALIVE (the loader
// holds the shared_ptr and lands them moments later), named by the stack they
// are loading into - unless that stack is itself being closed, in which case
// closeStack sweeps the queue. Speaks (stderr + toast + g_lastCloseNote) only
// when pixels survive; a close that frees everything stays silent, as before
// sharing.
static void sayCloseNotice(const std::vector<int>& closing,
                           const std::vector<int>& closingSeqIds,
                           const std::string& what) {
    g_lastCloseNote.clear();
    if (closing.empty()) return;
    std::vector<char> inSet(app.images.size(), 0);
    for (int idx : closing)
        if (idx >= 0 && idx < (int)app.images.size()) inSet[idx] = 1;
    std::unordered_map<uint64_t, const ImageDoc*> outside;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (!inSet[i]) outside.emplace(app.images[i]->src->srcId, app.images[i].get());
    std::unordered_set<uint64_t> queued;   // decoded and adopted, not landed yet
    const int loadingSeq = app.seqLoadingId;
    if (loadingSeq != 0 &&
        std::find(closingSeqIds.begin(), closingSeqIds.end(), loadingSeq) ==
            closingSeqIds.end()) {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        for (const auto& r : app.seqReady)
            if (r.second && r.second->src) queued.insert(r.second->src->srcId);
    }
    std::unordered_set<uint64_t> seen;
    int total = 0, kept = 0;
    std::vector<std::string> keepers;      // distinct counterpart names
    auto keeper = [&](const std::string& nm) {
        if (std::find(keepers.begin(), keepers.end(), nm) == keepers.end())
            keepers.push_back(nm);
    };
    for (int idx : closing) {
        if (idx < 0 || idx >= (int)app.images.size()) continue;
        const ImageDoc& d = *app.images[idx];
        if (!seen.insert(d.src->srcId).second) continue;
        total++;
        auto it = outside.find(d.src->srcId);
        if (it != outside.end()) {
            kept++;
            const ImageDoc* o = it->second;
            const App::SeqInfo* osi = o->seqId ? seqInfo(o->seqId) : nullptr;
            keeper(osi ? osi->name : o->name);
        } else if (queued.count(d.src->srcId)) {
            kept++;
            const App::SeqInfo* lsi = seqInfo(loadingSeq);
            keeper((lsi ? lsi->name : std::string("a stack")) + " (loading)");
        }
    }
    if (kept == 0) return;                 // freed everything: silence, as ever
    std::string who = "\"" + keepers[0] + "\"";
    if (keepers.size() > 1)
        who += " (+" + std::to_string(keepers.size() - 1) + " more)";
    char m[512];
    snprintf(m, sizeof m, "closed %s - %d frame(s) freed, %d still referenced by %s",
             what.c_str(), total - kept, kept, who.c_str());
    fprintf(stderr, "%s\n", m);
    toast(m);
    g_lastCloseNote = m;
}

// Close every frame of a stack plus its SeqInfo, and stop everything that
// would quietly regrow it: the sequence loader (pumpSequence stamps
// seqLoadingId on frames as they land), the remote prefetch queue, the
// linearity row, the server temporal result. announce=false is for
// closeBatch/closeSeries, which speak ONCE for their whole set instead.
static void closeStack(int seqId, bool announce = true) {
    if (seqId == 0) return;
    if (app.seqLoadingId == seqId) { stopSequenceLoader(); app.seqLoadingId = 0; }
    {   // queued remote prefetches. The ONE job the worker may be running right
        // now cannot be removed here - pumpRemoteFetch drops its result on
        // arrival instead (seqInfo(d.seqId) == nullptr). Both are needed.
        std::lock_guard<std::mutex> lk(app.rfMtx);
        int removed = 0;
        size_t freed = 0;
        for (auto it = app.rfQueue.begin(); it != app.rfQueue.end();)
            if (it->seqId == seqId) { freed += it->bytes; it = app.rfQueue.erase(it); removed++; }
            else ++it;
        app.rfPending -= removed;
        app.rfBytesInFlight -= std::min(app.rfBytesInFlight.load(), freed);
        if (app.rfPending <= 0) {
            app.rfPending = 0; app.rfTotal = 0; app.rfFetched = 0; app.rfBytesInFlight = 0;
        }
    }
    // §4: when another doc still references some of these pixels, the close
    // says so BEFORE the memberships go - counted in both directions (how many
    // die, how many live on), the same shape as derive's count promise. A
    // silent close would read as "freed" while a sharer keeps the pixels
    // alive. When this close is ONE MEMBER of a larger action, the caller has
    // already scanned and spoken over its whole set (announce=false):
    // announcing per member here named survivors the same action then freed.
    if (announce) {
        const App::SeqInfo* si = seqInfo(seqId);
        sayCloseNotice(framesOfSeq(seqId), { seqId },
                       "\"" + (si ? si->name : std::string("stack")) + "\"");
    }
    // rbOpenQueue entries carry no seqId yet, so a stack whose FOLDER is still
    // queued may open later regardless - accepted; closeBatch removes those by
    // batchId, which the queue does know.
    closeImages(framesOfSeq(seqId));
    for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
        if (it->id == seqId) { app.seqs.erase(it); break; }
    // the stack is gone, so it is not a member of anything anymore; a series
    // that held nothing else goes with it (docs/series-plan.md invariant 3)
    removeFromSeries(seqId);
    pruneEmptySeries();
    for (auto it = app.lin.rows.begin(); it != app.lin.rows.end();)
        if (it->seqId == seqId) it = app.lin.rows.erase(it);
        else ++it;
    // ...and the fit that row was part of goes with it: dropping the dot while
    // the line and the "%d points" count still include it is worse than saying
    // "press Compute".
    linFitStale();
    app.temporal[0].seqId = app.temporal[1].seqId = -1;
    if (app.srvTemporal.seqId == seqId) app.srvTemporal = App::ServerTemporal{};
    if (app.srvTemporalB.seqId == seqId) app.srvTemporalB = App::ServerTemporal{};
}

// Close a batch: every stack and loose frame in it, plus the queued opens that
// would resurrect it the moment the fetcher goes idle. A series lives in
// exactly one batch, so the batch's series go too - Close on a batch is the
// canon's "discard the contents", not "ungroup".
static void closeBatch(int batchId) {
    for (auto it = app.series.begin(); it != app.series.end();)
        if (it->batchId == batchId) {
            if (app.curSeriesId == it->id) app.curSeriesId = 0;
            it = app.series.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
    std::vector<int> seqIds;
    for (const auto& d : app.images)
        if (d->batchId == batchId && d->seqId != 0 &&
            std::find(seqIds.begin(), seqIds.end(), d->seqId) == seqIds.end())
            seqIds.push_back(d->seqId);
    // §4, scoped to THIS user action: ONE survival scan over everything the
    // batch close takes - member stacks AND loose frames - so the notice can
    // never name a survivor this same close then frees, and a loose member's
    // survivors are said too (closeImages itself carries no notice).
    {
        std::vector<int> closing;
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->batchId == batchId) closing.push_back(i);
        sayCloseNotice(closing, seqIds, "batch \"" + batchNameOf(batchId) + "\"");
    }
    for (int s : seqIds) closeStack(s, false);
    std::vector<int> loose;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->batchId == batchId) loose.push_back(i);
    closeImages(loose);
    for (auto it = app.rbOpenQueue.begin(); it != app.rbOpenQueue.end();)
        if (it->batchId == batchId) it = app.rbOpenQueue.erase(it);
        else ++it;
    for (auto it = app.seqQueue.begin(); it != app.seqQueue.end();)
        if (it->batchId == batchId) it = app.seqQueue.erase(it);
        else ++it;
    // ...and the sweeps the picker accepted INTO this batch, which are only a
    // NOTE until every load drains. closeAll purges them for exactly this
    // reason ("they named stacks being thrown away") and this function already
    // purges two queues by batchId - it just missed the third container. Batch
    // ids are never reused, so a surviving note resolves against nothing: a red
    // "sweep: 0 stack(s) ... N could not be matched" about an Open the user
    // deliberately discarded, and if the folder was reopened meanwhile the note
    // still points at the dead batch, so the new one gets no series either.
    for (auto it = app.seriesPending.begin(); it != app.seriesPending.end();)
        if (it->batchId == batchId) it = app.seriesPending.erase(it);
        else ++it;
    if (app.seriesPending.empty()) app.groupStacks.clear();
    if (app.loadBatchId == batchId) app.loadBatchId = 0;
    pruneEmptyBatches();       // the queue purge above may have freed this batch
}

// Ctrl+W: the stack when the frame is in one (the canon), the image otherwise.
// frameOnly = the Ctrl+Alt+W escape hatch: just this one frame.
static void closeCurrent(bool frameOnly = false) {
    ImageDoc* im = cur();
    if (!im) return;
    if (im->seqId != 0 && !frameOnly) { closeStack(im->seqId); return; }
    int seqId = im->seqId;
    // §4 for the two one-frame closes (Ctrl+W on a loose frame, Ctrl+Alt+W):
    // one membership goes here without passing closeStack, and pixels that
    // live on elsewhere must still be said to - a silent close reads as freed.
    // Said BEFORE closeImages: the notice reads the doc it is about.
    sayCloseNotice({ app.current }, {}, "\"" + im->name + "\"");
    closeImages({ app.current });
    // the escape hatch emptied the stack: drop the SeqInfo and its bookkeeping
    // too, or a zero-frame stack haunts the linearity table
    if (seqId != 0 && framesOfSeq(seqId).empty()) closeStack(seqId);
}

// Drop batches nothing references anymore: no image, no queued group, no
// pending remote open, not the load target. Called after every close and after
// every move - an empty Files heading is a lie about what is open. The preview
// pseudo-batch stays: its emptiness is its normal state between previews.
static void pruneEmptyBatches() {
    for (auto it = app.batches.begin(); it != app.batches.end();) {
        bool used = it->name == "preview" || app.loadBatchId == it->id;
        if (!used)
            for (const auto& d : app.images)
                if (d->batchId == it->id) { used = true; break; }
        if (!used)
            for (const auto& q : app.seqQueue)
                if (q.batchId == it->id) { used = true; break; }
        if (!used)
            for (const auto& q : app.rbOpenQueue)
                if (q.batchId == it->id) { used = true; break; }
        if (!used) {
            it = app.batches.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
    }
}

// Move a STACK between batches - the whole stack, per the canon's containment
// (frame ⊂ stack ⊂ batch): frames never move between batches one by one.
// A member moved out ALONE leaves its series: series ⊂ batch is strict, and the
// canon says say so rather than forbid it. Moving the series itself is a
// different operation (it carries every member and its own batchId along).
static void moveStackToBatch(int seqId, int batchId) {
    if (App::Series* S = seriesOfStack(seqId))
        if (S->batchId != batchId) {
            std::string sn = S->name;
            App::SeqInfo* si = seqInfo(seqId);
            removeFromSeries(seqId);
            pruneEmptySeries();
            // The membership just changed, so whatever was computed measured a
            // different set (docs/terminology.md: editing members / values /
            // unit / parameter discards the computed fit). The Linearity panel's
            // only freshness keys are the series id and fitValid, and a move
            // leaves the series alive - so both stayed true, the fit table and
            // the "%d points" count went on describing the old membership, and
            // both plots went on DRAWING the departed stack's point under a
            // member table that no longer listed it. Every sibling edit already
            // does this: closeStack, the seqctx join, the Files "leave series",
            // the modal's Save.
            //
            // moveSeriesToBatch sets S->batchId FIRST, so its per-member calls
            // never reach this branch: a series moved WHOLE keeps its fit, which
            // is right - nothing about its membership changed.
            linFitStale();
            toast((si ? si->name : std::string("stack")) + " left series \"" + sn +
                  "\" (moved to another batch)");
        }
    for (int idx : framesOfSeq(seqId)) app.images[idx]->batchId = batchId;
    app.imagesRev++;
    pruneEmptyBatches();
}
// ---- what the Files panel's series row does (docs/series-plan.md §4) --------
// Move a SERIES between batches: every member goes with it and so does the
// series itself, so strict containment holds at every instant. The series'
// batchId is set FIRST on purpose - moveStackToBatch drops a member whose
// series stays behind, which is the right rule for a lone stack and exactly
// the wrong one here.
static void moveSeriesToBatch(int seriesId, int batchId) {
    App::Series* S = seriesById(seriesId);
    if (!S || batchId == 0 || S->batchId == batchId) return;
    std::vector<int> ids;
    for (const auto& m : S->members) ids.push_back(m.seqId);
    S->batchId = batchId;
    for (int s : ids) moveStackToBatch(s, batchId);
    app.imagesRev++;
}
// UNGROUP (解散): take the fence away and leave everything standing. This is
// NOT Close - nothing is discarded, every stack stays open exactly where it is
// and merely stops being part of a sweep. The canon lists the two side by side
// because they are different operations, not two words for one.
static void ungroupSeries(int seriesId) {
    for (auto it = app.series.begin(); it != app.series.end(); ++it)
        if (it->id == seriesId) {
            std::string n = it->name;
            int members = (int)it->members.size();
            if (app.curSeriesId == seriesId) app.curSeriesId = 0;
            app.series.erase(it);
            app.imagesRev++;
            toast("ungrouped \"" + n + "\": " + std::to_string(members) +
                  " stack(s) stay open");
            break;
        }
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
}
// Close a series: the canon's Close, which discards the CONTENTS. Each member's
// closeStack removes it from the series, and the last one takes the series with
// it (pruneEmptySeries) - the erase below is only for a series whose members
// were all gone already.
static void closeSeries(int seriesId) {
    App::Series* S = seriesById(seriesId);
    if (!S) return;
    std::string n = S->name;
    std::vector<int> ids;
    for (const auto& m : S->members) ids.push_back(m.seqId);
    {   // §4, scoped to THIS user action: one scan across every member stack,
        // so no member is called a survivor of a close that takes it too
        std::vector<int> closing;
        for (int s : ids)
            for (int idx : framesOfSeq(s)) closing.push_back(idx);
        sayCloseNotice(closing, ids, "series \"" + n + "\"");
    }
    for (int s : ids) closeStack(s, false);
    for (auto it = app.series.begin(); it != app.series.end(); ++it)
        if (it->id == seriesId) {
            if (app.curSeriesId == seriesId) app.curSeriesId = 0;
            app.series.erase(it);
            app.imagesRev++;
            break;
        }
    toast("closed series \"" + n + "\": " + std::to_string(ids.size()) + " stack(s)");
}
// "seqctx > Series > <name>": the one join that does not go through the modal.
// A function like every other command behind a Files row, so the selftest can
// press it. Returns false (and a reason) when strict containment refuses.
//
// The value comes from the stack's NAME and is said out loud - the menu item
// showed it before the click, and the toast repeats both the number and the text
// it was read out of. A name with nothing numeric in it joins UNSET, never at 0.
static bool seriesJoinFromMenu(int seriesId, int seqId, std::string* msgOut) {
    App::Series* S = seriesById(seriesId);
    App::SeqInfo* si = seqInfo(seqId);
    if (!S || !si) return false;
    std::string from;
    double v = extractLevelFromName(si->name, &from);
    // the series it is LEAVING: taking the last member out of one empties it
    const App::Series* was = seriesOfStack(seqId);
    int wasId = was && was->id != S->id ? was->id : 0;
    std::string wasName = wasId ? was->name : std::string();
    if (!addToSeries(S->id, seqId, v)) {
        if (msgOut)
            *msgOut = si->name + " is not in batch \"" + batchNameOf(S->batchId) +
                      "\" - Move to batch first";
        return false;
    }
    std::string msg = si->name + " joined series \"" + S->name + "\"";
    if (std::isfinite(v)) {
        char b[96];
        snprintf(b, sizeof b, " at %.6g%s%s", v, S->unit[0] ? " " : "", S->unit);
        msg += b;
        if (!from.empty()) msg += " (read from \"" + from + "\")";
    } else {
        msg += " with NO value yet - set it in Edit...";
    }
    // Every other caller of addToSeries prunes; this one did not, so joining the
    // sole member of one series into another left the first behind with zero
    // members - the state plan §1 invariant 3 forbids and seriesAudit rejects.
    pruneEmptySeries();
    if (wasId && !seriesById(wasId)) msg += "; series \"" + wasName + "\" is now empty and gone";
    linFitStale();          // the fit below was measured over the old membership
    if (msgOut) *msgOut = msg;
    return true;
}
// A standalone frame (no stack) hangs off the batch directly and moves alone.
static void moveImageToBatch(int imageIdx, int batchId) {
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    app.images[imageIdx]->batchId = batchId;
    app.imagesRev++;
    pruneEmptyBatches();
}
static void closeAll() {
    // FIRST: stop everything that produces images, or Close All does not close.
    // closeStack and closeBatch already know this - a queue the close leaves
    // behind reopens the closed content the moment the loader/fetcher goes
    // idle, stamped with a batchId app.batches no longer contains, so the
    // frames are resident, unnamed and unreachable by pruneEmptyBatches.
    // Measured before this line existed: Close All over 30 queued stacks left
    // running=1 / queued=28, and draining brought back 86 frames in 28 stacks
    // with batches=0. This is the one place that knows what a Close has to
    // stop; loadSession used to open-code half of it two lines above its own
    // closeAll() and inherits the whole rule now.
    stopSequenceLoader();
    app.seqLoadingId = 0;
    app.seqQueue.clear();
    app.seqRestore.clear();
    app.pendingAvg.clear();           // they name stacks being thrown away...
    app.avgRestore.clear();           // ...and paths that will not be open either
    app.rbOpenQueue.clear();
    for (auto& bp : app.browsePanels) bp->pendingOpen.clear();
    app.ana.img = nullptr;
    for (int k = 0; k < 2; k++) {     // both A and B slots
        app.hist[k] = App::HistState{};
        app.proj[k] = App::ProjState{};
        app.temporal[k] = App::TemporalState{};
    }
    app.histExtra.clear();            // ...and every lettered one
    app.projExtra.clear();
    app.temporalExtra.clear();
    app.abSlot1Live = false;
    app.texLru.clear();
    app.imagesRev++;
    for (auto& d : app.images)
        if (d->tex) glDeleteTextures(1, &d->tex);
    app.images.clear();
    app.seqs.clear();
    // Series name stacks that no longer exist; they go with them (and with the
    // batches below - a series cannot outlive its batch). Pending restores name
    // stacks of the list being thrown away, so they go too.
    app.series.clear();
    app.curSeriesId = 0;
    app.seriesRestore.clear();
    app.seqLevelLegacy.clear();
    app.seriesPending.clear();                  // they named stacks being thrown away
    app.groupStacks.clear();                    // ...and those stacks are gone too
    // The batches go with their contents: an empty batch that survives Close
    // All keeps its NAME reserved, so reopening the same folder came back as
    // "multi (2)" - the uniquifier colliding with a ghost.
    app.batches.clear();
    app.loadBatchId = 0;
    app.previewUid = 0;
    app.previewFiles.clear();
    app.previewFrames = 0;
    app.previewLabel.clear();
    // ...and the three this used to leave standing. dropPreview() clears all
    // seven; Close All cleared four, so the machine and port of a preview that
    // no longer exists survived a Close All and were handed to the next step.
    app.previewIndex = 0;
    app.previewHost.clear();
    app.previewPort = 0;
    app.current = -1;
    // compare state refers to docs that no longer exist; leaving it would let a
    // later file with the same name silently become B again
    app.compareMode = App::CmpOff;
    app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
    app.cmpSlotRestore.clear();       // the same rule for the SLOT wants: a
                                      // stale want from a dead session would
                                      // graft a letter onto whichever open
                                      // next matches its path (every sibling
                                      // restore queue is cleared above)
    // in-flight remote fetches belong to the OLD list: bump the generation so
    // their results are dropped instead of grafting orphan frames onto the new one
    app.rfGen++;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfQueue.clear();
        app.rfDone.clear();
        app.rfPending = 0;
        app.rfTotal = 0;
        app.rfFetched = 0;
        app.rfBytesInFlight = 0;
    }
}

// ---------------------------------------------------------------- annotations
static App::Ann* findAnn(int id) {
    for (auto& a : app.anns)
        if (a.id == id) return &a;
    return nullptr;
}
static void addAnn(int type, int x, int y, int w, int h) {
    App::Ann a;
    a.id = app.nextAnnId++;
    a.type = type; a.x = x; a.y = y; a.w = w; a.h = h;
    a.color = (a.id - 1) & 7;
    int seq = type == 0 ? ++app.roiSeq : ++app.poiSeq;
    a.label = (type == 0 ? "ROI " : "P") + std::to_string(seq);
    app.anns.push_back(std::move(a));
    app.selectedAnn = app.anns.back().id;
    app.annRev++;
}
// X / Y: row and column selection. A band is a ROI one pixel thick, which is why
// it cannot be drawn by dragging - so it has to come from a key:
//   ROI selected     -> toggle band <-> the rect it had before
//   POI selected     -> the row/column through that pin (toggle back = the pixel)
//   nothing selected -> the row/column through the pixel under the cursor
static void toggleBand(bool horizontal) {
    ImageDoc* im = cur();
    if (!im) return;
    App::Ann* a = findAnn(app.selectedAnn);
    if (a && a->type == 0) {                     // ROI: the existing toggle
        if (horizontal) {
            bool full = a->x == 0 && a->w == im->w;
            if (full && a->prevW >= 1) { a->x = a->prevX; a->w = a->prevW; a->prevW = -1; }
            else if (!full)            { a->prevX = a->x; a->prevW = a->w; a->x = 0; a->w = im->w; }
        } else {
            bool full = a->y == 0 && a->h == im->h;
            if (full && a->prevH >= 1) { a->y = a->prevY; a->h = a->prevH; a->prevH = -1; }
            else if (!full)            { a->prevY = a->y; a->prevH = a->h; a->y = 0; a->h = im->h; }
        }
        app.annRev++;
        return;
    }
    int px = -1, py = -1;
    if (a && a->type == 1) { px = a->x; py = a->y; }          // the selected pin
    else if (app.hoverX >= 0) { px = app.hoverX; py = app.hoverY; }
    if (px < 0 || px >= im->w || py < 0 || py >= im->h) {
        toast("select a pin, or hover a pixel, then press X or Y");
        return;
    }
    if (horizontal) addAnn(0, 0, py, im->w, 1);               // whole row
    else            addAnn(0, px, 0, 1, im->h);               // whole column
    App::Ann* n = findAnn(app.selectedAnn);
    if (n) {   // pressing X/Y again collapses the band back onto that one pixel
        if (horizontal) { n->prevX = px; n->prevW = 1; }
        else            { n->prevY = py; n->prevH = 1; }
        n->label = (horizontal ? "row " + std::to_string(py) : "col " + std::to_string(px));
    }
}

// A ROI drawn on a 1/3 preview is in PREVIEW coordinates: after the full-res
// swap it would silently cover 1/9 of the intended area and every panel would
// measure the wrong region. Placing annotations waits for the real pixels.
static bool annBlockedOnPreview() {
    if (cur() && cur()->src->remoteStep > 1) {
        toast("preview: wait for full resolution before placing ROIs/pins", true);
        return true;
    }
    return false;
}

static void deleteAnn(int id) {
    for (size_t i = 0; i < app.anns.size(); i++)
        if (app.anns[i].id == id) { app.anns.erase(app.anns.begin() + i); break; }
    if (app.selectedAnn == id) app.selectedAnn = 0;
    app.annRev++;
}

// ---------------------------------------------------------------- plugin glue
static psFrame makeFrame(const ImageDoc& im) {
    psFrame f = {};
    f.w = (uint32_t)im.w; f.h = (uint32_t)im.h; f.ch = (uint32_t)im.ch;
    f.dtype = PS_DTYPE_F32; f.loc = PS_MEM_CPU;
    f.data = (void*)im.px().data();
    f.pitch_bytes = (size_t)im.w * im.ch * sizeof(float);
    f.black = effBlack(im); f.white = effWhite(im);
    f.cfa_type = im.cfa; f.cfa_pattern = im.cfaPattern;   // enums mirror psCfa* by construction
    f.pts_us = -1;
    f.name = im.name.c_str();                             // valid only during the call
    f.meta_json = nullptr;
    return f;
}
static void anaEmitNumber(void* ctx, const char* key, double v) {
    auto* rows = (std::vector<std::pair<std::string, std::string>>*)ctx;
    char b[64]; snprintf(b, 64, "%.6g", v);
    rows->emplace_back(key ? key : "", b);
}
static void anaEmitText(void* ctx, const char* key, const char* v) {
    auto* rows = (std::vector<std::pair<std::string, std::string>>*)ctx;
    rows->emplace_back(key ? key : "", v ? v : "");
}
// V2 sink context: rows go to the grid, curves into AnalysisState::series
struct AnaEmit2Ctx {
    std::vector<std::pair<std::string, std::string>>* rows;
    App::AnalysisState* ana;
    int col;
    int colorIdx;
};
static void ana2Number(void* ctx, const char* key, double v) {
    char b[64]; snprintf(b, 64, "%.6g", v);
    ((AnaEmit2Ctx*)ctx)->rows->emplace_back(key ? key : "", b);
}
static void ana2Text(void* ctx, const char* key, const char* v) {
    ((AnaEmit2Ctx*)ctx)->rows->emplace_back(key ? key : "", v ? v : "");
}
static void ana2Series(void* ctx, const char* name, const char* xl, const char* yl,
                       const float* x, const float* y, uint32_t n) {
    auto* c = (AnaEmit2Ctx*)ctx;
    if (!y || n == 0 || n > 1000000) return;
    App::AnalysisState::Series s;
    s.name = name ? name : "series";
    s.xLabel = xl ? xl : "";
    s.yLabel = yl ? yl : "";
    s.ys.assign(y, y + n);
    if (x) s.xs.assign(x, x + n);
    s.col = c->col;
    s.colorIdx = c->colorIdx;
    c->ana->series.push_back(std::move(s));
}

// naming convention "category/name": drives the 2-level Analysis picker + Measure menu
static void splitAnalyzerName(const std::string& full, std::string& cat, std::string& item) {
    size_t s = full.find('/');
    if (s == std::string::npos) { cat = "misc"; item = full; }
    else { cat = full.substr(0, s); item = full.substr(s + 1); }
}
// The Measure menu groups by the QUESTION being asked, not by the plugin file:
// nobody opens the menu thinking "iso12233", they think "is the resolution
// there?". The mapping is host knowledge (tiny, additive); unknown categories
// from third-party plugins fall through and keep their own name as a heading.
struct MeasureGroup { const char* cat; const char* group; };
static const MeasureGroup MEASURE_GROUPS[] = {
    { "noise",      "Noise / SNR" },
    { "iso12233",   "Resolution / Focus" },
    { "sharpness",  "Resolution / Focus" },
    { "uniformity", "Uniformity / Flat field" },
    { "stats",      "Statistics / Sanity" },
};
static const char* measureGroupOf(const std::string& cat) {
    for (const auto& g : MEASURE_GROUPS)
        if (cat == g.cat) return g.group;
    return nullptr;
}

// Machine-checkable preconditions only. The host must never guess about scene
// content - it cannot tell a flat field from a portrait, so "wants a flat
// field" stays a tooltip and the plugin's own error does the talking. What it
// CAN prove (no image at all, a mosaic the plugin is documented to reject) is
// worth a disabled item: the reason appears where the click would have been.
static const char* analyzerDisabledReason(const AnalyzerPluginInfo& a, const ImageDoc* im) {
    if (!im) return "open an image first";
    if (a.name == "iso12233/e-sfr" && im->ch == 1 && im->cfa != 0)
        return "CFA mosaic input - run Process > demosaic (bilinear) first";
    return nullptr;
}

// Running a measurement and showing its result are one gesture: the Measure
// menu (and the M key) must never leave the numbers in a hidden or buried
// panel. Reveal is half of it - a docked Analysis tab sitting behind
// Histogram would still swallow the result, hence the explicit focus.
static void revealAnalysis() {
    app.showAnalysis = true;
    ImGui::SetWindowFocus("Analysis");
}
static void requestMeasure(int sel) {
    app.anaSel = sel;
    app.anaRunRequest = true;   // consumed by the Analysis panel this frame
    revealAnalysis();
}

// Set while the background queue loads: newly arrived images must never steal
// the selection (or the zoom) from what the user is working on.
static bool g_quietLoad = false;

// measured = true: the loader already ran computeMinMax (or adopted a measured
// resident) BEFORE the source became discoverable in the registry. Do not
// write vmin/vmax again here - the seq loader thread may be reading them
// through an adoption, and shareOrRegisterSource's contract says adopters
// never rewrite shared fields.
static void addImage(std::unique_ptr<ImageDoc> im, bool measured = false) {
    if (im->batchId == 0) {
        if (app.loadBatchId) im->batchId = app.loadBatchId;
        else {
            size_t sl = im->src->path.find_last_of("/\\");
            std::string dir = sl == std::string::npos ? std::string() : im->src->path.substr(0, sl);
            size_t s2 = dir.find_last_of("/\\");
            std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
            im->batchId = batchReuse(leaf.empty() ? "generated" : leaf, dir);
        }
    }
    // --cfa says "this dump is mosaiced" for formats that carry no such flag (an
    // .npy of a sensor read is exactly that). The Inspector can still change it
    // per image; this only sets what a file arrives as.
    if (app.forceCfa >= 0 && im->ch == 1 && im->cfa == 0) {
        im->cfa = app.forceCfa;
        im->cfaPattern = app.forceCfaPattern & 3;
    }
    im->uid = app.nextUid++;
    app.imagesRev++;
    if (!measured) computeMinMax(*im);
    defaultRange(*im);            // own range is always meaningful; link only overlays it
    im->texDirty = true;
    app.images.push_back(std::move(im));
    if (!g_quietLoad || app.current < 0) {      // first image still gets shown
        app.current = (int)app.images.size() - 1;
        // the view is shared: only frame the very first image, keep it afterwards
        if (app.images.size() == 1 || app.fitOnSwitch) app.fitRequested = true;
    }
}

// ---- ROI montage: the same ROI from every frame, laid side by side ---------
// A contact sheet of one region across a stack - the thing you actually paste
// into a report to show drift, flicker or a defect appearing.
//
// The result is a real ImageDoc, not a picture: it can be measured, ranged and
// exported like anything else. So it carries DATA rules, not poster rules -
//   - no separator lines: a drawn line is a pixel value that was never captured
//   - CFA phase is preserved by snapping the ROI to even coordinates, or the
//     tiles would each start on a different Bayer phase and every per-plane
//     statistic over the montage would be wrong
//   - it says n of N: a partially loaded stack must not look complete
// perFrameAuto: each tile is normalised to ITS OWN finite min..max, mapped to
// 0..1. That makes an exposure sweep readable - under one shared range the dark
// frames are black squares - but it also means the pixels STOP BEING DN: a
// normalised montage is a picture for looking, not a measurement, and it says
// so in its name, its note and its unit-less display range. A constant tile
// (max == min) maps to 0.5 - the middle, not a lie about being dark or bright.
static bool montageROI(bool horizontal, std::string& err, bool perFrameAuto = false) {
    ImageDoc* im = cur();
    if (!im) { err = "no image"; return false; }
    if (im->seqId == 0) { err = "not a stack: montage lays out the frames of one stack"; return false; }
    App::Ann* a = findAnn(app.selectedAnn);
    if (!a || a->type != 0) { err = "select a rectangle ROI first"; return false; }
    int rx = std::clamp(a->x, 0, im->w), ry = std::clamp(a->y, 0, im->h);
    int rw = std::clamp(a->w, 0, im->w - rx), rh = std::clamp(a->h, 0, im->h - ry);
    if (rw < 1 || rh < 1) { err = "the ROI is empty"; return false; }
    bool snapped = false;
    if (im->cfa) {                       // keep every tile on the same Bayer phase
        if (rx & 1) { rx--; rw++; snapped = true; }
        if (ry & 1) { ry--; rh++; snapped = true; }
        if (rw & 1) { rw--; snapped = true; }
        if (rh & 1) { rh--; snapped = true; }
        if (rw < 2 || rh < 2) { err = "the ROI is too small for a CFA montage"; return false; }
    }
    std::vector<int> fr = framesOfSeq(im->seqId);
    if (fr.size() < 2) { err = "the stack has one frame"; return false; }
    const int ch = im->ch, n = (int)fr.size();
    const int ow = horizontal ? rw * n : rw;
    const int oh = horizontal ? rh : rh * n;
    auto out = std::make_unique<ImageDoc>();
    FrameSource& S = *out->src;          // composed NEW pixels: a copy by design (§2.2)
    S.w = ow; S.h = oh; S.ch = ch;
    S.dtype = im->dtype;
    S.data.assign((size_t)ow * oh * ch, 0.0f);
    for (int k = 0; k < n; k++) {
        const ImageDoc& src = *app.images[fr[k]];
        if (src.w != im->w || src.h != im->h || src.ch != ch) continue;   // ragged stack
        const int ox = horizontal ? rw * k : 0;
        const int oy = horizontal ? 0 : rh * k;
        for (int y = 0; y < rh; y++) {
            const float* sp = &src.px()[((size_t)(ry + y) * src.w + rx) * ch];
            float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
            memcpy(dp, sp, (size_t)rw * ch * sizeof(float));
        }
        if (perFrameAuto) {
            // this tile's own finite range - the whole tile, planes included:
            // per-plane normalisation would silently rebalance the CFA mosaic
            float lo = FLT_MAX, hi = -FLT_MAX;
            for (int y = 0; y < rh; y++) {
                const float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
                for (int x = 0; x < rw * ch; x++)
                    if (std::isfinite(dp[x])) { lo = std::min(lo, dp[x]); hi = std::max(hi, dp[x]); }
            }
            const float span = hi - lo;
            for (int y = 0; y < rh; y++) {
                float* dp = &S.data[((size_t)(oy + y) * ow + ox) * ch];
                for (int x = 0; x < rw * ch; x++) {
                    if (!std::isfinite(dp[x])) continue;
                    dp[x] = span > 0 ? (dp[x] - lo) / span : 0.5f;
                }
            }
        }
    }
    // the montage inherits the CFA description only if the phase survived, which
    // the even-snap above guarantees; the pattern itself is unchanged
    out->cfa = im->cfa; out->cfaPattern = im->cfaPattern;
    if (perFrameAuto) {
        S.dtype = "f32";                 // whatever the source was, this is not it
        out->black = 0.0f; out->white = 1.0f;
        S.vmin = 0.0f;  S.vmax = 1.0f;
    } else {
        out->black = im->black; out->white = im->white;
        S.vmin = im->vmin; S.vmax = im->vmax;
    }
    out->syncMirrors();
    out->batchId = im->batchId;
    const App::SeqInfo* si = seqInfo(im->seqId);
    int expected = si && si->expectedFrames > 0 ? si->expectedFrames : n;
    char nm[512];
    snprintf(nm, sizeof nm, "%s  ROI %dx%d x%d%s %s", si ? si->name.c_str() : im->name.c_str(),
             rw, rh, n, n < expected ? (" of " + std::to_string(expected)).c_str() : "",
             perFrameAuto ? (horizontal ? "(montage H, per-frame range)"
                                        : "(montage V, per-frame range)")
                          : (horizontal ? "(montage H)" : "(montage V)"));
    out->name = nm;
    out->note = std::string("ROI (") + std::to_string(rx) + "," + std::to_string(ry) + ") " +
                std::to_string(rw) + "x" + std::to_string(rh) + " from " +
                std::to_string(n) + (n < expected ? " of " + std::to_string(expected) : "") +
                " frame(s)" + (snapped ? ", snapped to the CFA phase" : "") +
                (perFrameAuto ? "; per-frame auto range: each tile mapped to 0..1 - "
                                "values are NOT DN, view only" : "");
    out->texDirty = true;
    addImage(std::move(out));
    if (n < expected)
        toast("montage: " + std::to_string(n) + " of " + std::to_string(expected) +
              " frames are loaded", true);
    return true;
}

#include "app/loader_npz.inc"

#include "app/loader_npy_raw.inc"

#include "app/session.inc"

#include "app/sequence.inc"

#include "app/temporal_model.inc"

#include "app/open_dispatch.inc"

// ---------------------------------------------------------------- view helpers
static bool tileEngaged();   // fwd: defined with the tile geometry below
static void fitToCanvas(ImVec2 canvasSize) {
    ImageDoc* im = cur();
    if (!im || canvasSize.x < 10 || canvasSize.y < 10) return;
    // Fit spans the LARGER dimensions of what is on screen (user decision,
    // 2026-07-30): every side shares one view anchored at image (0,0), so
    // fitting A alone crops a bigger B. The extras join the union exactly when
    // the tiles have them on screen. Compare off: A alone, as always.
    float w = (float)im->w, h = (float)im->h;
    if (ImageDoc* b = cmpB()) {
        w = std::max(w, (float)b->w);
        h = std::max(h, (float)b->h);
    }
    if (tileEngaged())
        for (const ResolvedSlot& rs : resolveSlots()) {
            w = std::max(w, (float)rs.doc->w);
            h = std::max(h, (float)rs.doc->h);
        }
    app.view.zoom = std::min(canvasSize.x / w, canvasSize.y / h) * 0.97f;
    app.view.center = ImVec2(w * 0.5f, h * 0.5f);
}

static std::string fmtVal(float v, const std::string& dtype) {
    char b[64];
    if (dtype == "u8" || dtype == "u16" || dtype == "i8" || dtype == "i16" ||
        dtype == "u32" || dtype == "i32" || dtype == "bool")
        snprintf(b, 64, "%.0f", v);
    else
        snprintf(b, 64, "%.5g", v);
    return b;
}

// ---------------------------------------------------------------- UI
#include "ui/canvas.inc"
#include "ui/inspector.inc"
#include "ui/panel_histogram.inc"
#include "ui/panel_projection.inc"
#include "ui/modal_series.inc"
#include "ui/panel_linearity.inc"
#include "ui/panel_temporal.inc"
#include "ui/panel_rois.inc"
#include "ui/panel_analysis.inc"
#include "browse/panel.inc"

#include "ui/modal_derive.inc"
#include "ui/file_list.inc"
#include "ui/menus.inc"
// ---------------------------------------------------------------- CLI
static bool g_linSelftest = false;      // --lin-selftest: fit, print, exit
static bool g_fstatSelftest = false;    // --framestats-selftest: TSV to stdout, exit
static std::string g_rangeSelftest;      // --range-selftest <dir with 2 stacks>
static std::string g_scanSelftest;      // --scan-selftest <dir>: remote Open Folder, print, exit
static std::string g_pickerSelftest;    // --picker-selftest <dir>: filter cut + merge, print, exit
static std::string g_closeSelftest;     // --close-selftest <dir>: close per stack, print, exit
static std::string g_batchSelftest;     // --batch-selftest <dir>: move-to-batch + session, exit
static std::string g_rtemporalSelftest; // --rtemporal-selftest <dir>: browser temporal, exit
static std::string g_localbrowseSelftest; // --localbrowse-selftest <dir>: Browse (local), exit
static std::string g_browseSelftest;    // --browse-selftest <dir>: Browse panel behaviour, exit
static std::string g_verifySelftest;    // --verify-selftest <dir>: close/batch corners, exit
static std::string g_deriveSelftest;    // --derive-selftest <dir>: derive stack from stack, exit
static std::string g_stackAvgSelftest;  // --stackavg-selftest <dir>: stack mean as a frame, exit
static std::string g_abstatsSelftest;   // --abstats-selftest <dir>: A/B stats caches, exit
static std::string g_tileSelftest;      // --tile-selftest <dir>: side-by-side pane geometry, exit
static std::string g_seriesSelftest;    // --series-selftest <dir>: series invariants, exit
static std::string g_exportSelftest;    // --export-selftest <dir>: PNG + montage, exit
static std::string g_exportTsvSelftest; // --export-tsv-selftest <dir>: Temporal export document, exit
static bool g_frameLinSelftest = false; // --frame-lin-selftest: frame-wise linearity, synthetic, exit
static std::string g_sweepFileSelftest; // --sweepfile-selftest <dir>: one .npy per level, exit
static std::string g_newwinSelftest;    // --newwin-selftest <dir>: instance slots + spawn line, exit
static std::string g_reloadSelftest;    // --reload-selftest <dir>: reload walk + temporal key, exit
static std::string g_srcmapSelftest;    // --srcmap-selftest <dir>: who holds which pixels, exit
static std::string g_mediaSelftest;     // --media-selftest <dir>: PNG/JPEG/TIFF through the seam, exit
// --roistats-selftest: the ROI table's NUMBERS, through the real panel, with no
// window. It takes no directory because it needs no file: every fixture it
// measures is analytic and built in memory (see roiStatsSelftest()).
static bool g_roiStatsSelftest = false;

// --abeq-selftest: A and B (and a letter past B) on ONE document - the
// comparison people set up on purpose, to confirm the two sides agree. Takes no
// directory for the same reason as --roistats-selftest, and is windowless for
// the same reason: see abEqSelftest().
static bool g_abEqSelftest = false;

// --browse-keys-selftest <dir>: the Browse panel's KEYBOARD, driven through real
// frames. The other browse selftests call the panel's helpers directly, which
// is enough for what the rows contain but blind to everything that only exists
// inside a frame - and a single Down arrow segfaulted the process on an ImGui
// clipper call that a frameless test can never reach. So this one connects a
// LOCAL peer to <dir> in a hidden window and replays real UI actions into the
// real input queue, one per script slot: the panel cannot tell them from a
// human. Actions (--browse-keys overrides the canned list): focus, down, up,
// left, right, enter, home, end, back, flat, tree, disc, fmenu, rctx,
// esc, w<px>; comma / period (the preview scrub); altleft / altright (history);
// img0 (select the first image); natorder (flip the NAME column between
// natural and text order - relative, like flat / tree, and pinned back by
// viewreset); click / ctrlclick / dbl (real mouse clicks on
// the cursor's row - a double-click only exists as clicks); clickoff:N /
// dbloff:N (the same two, N rows BELOW the cursor - the only aim the keyboard
// has not already previewed, and so the only one where click one pays for the
// fetch, as a human's first click on a row always does); idle:N (hold N frames
// - a delay knob, so "two clicks a second apart" can be said); chevclick (a click
// on the cursor row's tree chevron, asserting the toggle landed at once);
// starclick (a click on the bookmark star at the end of the path line);
// pinclick (a click on the OUTERMOST row the pinned-ancestor band is holding,
// with chkpin:SPEC reading that band back as text - ";"-terminated names,
// outermost first, "-" for nothing pinned - and h<px> forcing the panel's
// height so the listing is short enough to have scrolled at all);
// marklist / starmark / seterr / clrerr (baselines and a fake failure for the
// drawer-removal checks below); mback / mfwd
// (mouse buttons 4 / 5); svtemp (the group row's server-temporal request);
// viewreset (absolute grouped+list+folded+natural-name state - the toggles are
// relative);
// waitimg:N / waitdir:LEAF (hold until N images are open / the browsed dir's
// leaf is LEAF); and the assertions chkimg:N (count + NAMES), chkpv:N,
// chkidx:K, chkopen:S, chkcur, chkcurn:NAME, chknames:SPEC, chkdir:LEAF,
// chkcursor:N (view index), chkatrow:NAME (the cursor row's name), chkback:N,
// chkfwd:N, chkexp:N, exparm / chkexpn:0 (a per-frame watch: the armed path
// was never expanded on ANY frame in between), chkfocus:0|1 - any FAIL fails
// the run.
//
// The "more" drawer's removal (docs/browse-topbar-design.md 10.2/10.3) is
// asserted by chktitle (the panel is named after its machine, both directions,
// with the ### id untouched), chkstat:N (the bottom status line's item and
// selected counts, both directions, and one line only), chkstar:0|1 (the star
// flipped from starmark and agrees with the bookmark list), chkerr:0|1 (a
// failure is IN the status line and the listing did not move - marklist is the
// baseline) and setpv:N / pvback / chkproto:0|1 (the same two claims for the
// protocol-mismatch notice, the other orange row that is gone). "more" is gone
// as an action along with the drawer it folded.
//
// INSTANCES (item 17): every stateful action and check drives the TARGET
// instance (target:N switches it; 1 = the primordial panel). newpanel creates
// a second Browse aimed at <dir>/scanroot and targets it; reconnect
// re-connects the target to <dir> post-"disc"; closep closes the target's
// window (extras are destroyed, the last one hides); hidep / showp drive
// app.showRemote (the View menu's flag); filt:S types S into the target's
// filter; chkfilt:S ("-" = empty), chkpanels:N (instance count), chkshown:N
// (windows visible), chksel:N (rows selected in the target).
//
// w<px> floats the panel at an exact width and then asserts what a human would
// otherwise have to read off a screenshot: that the filter box and the item
// that ENDS the toolbar row are still INSIDE the panel, and that the filter is
// wide enough to type a glob into. 271 is the panel's own default docked width
// on a 1600 px screen, which is where the fixed SameLine chain used to push
// both of them off-screen. (That last item was the "more" fold button and is
// the "..." panel menu now - same contract, different button.)
static int g_abRangeDefault = -1;      // App's compareRangeMode before loadPrefs
static std::string g_browseKeys;        // <dir>, empty = not running
// Key-routing evidence for --browse-keys-selftest: how many times the MAIN
// view's arrow/Home/End handler ran, and how many times it stood down because
// the Browse panel owned focus. The rule is one line of arithmetic - while the
// panel has focus the first must not move and the second must.
static int g_navKeyGlobal = 0;
static int g_navKeyYielded = 0;
static int g_navKeyAtBlur = -1, g_navKeyYieldAtBlur = -1;   // snapshot at "blur"
// Root-level popup collision evidence: "popupcheck" records whether the RAW
// dialog is still on the popup stack, once before and once after a competing
// root-level modal asks to open.
static int g_popupCheck[2] = { -1, -1 };
static int g_popupChecks = 0;
// Tree double-click purity evidence: "exparm" arms a PER-FRAME watch on the
// cursor row's path; until "chkexpn:0" every frame probes
// rbHas(expanded, path). An end-state check (chkexp) cannot see a flash -
// an expand executed on click one and cancelled on click two is state-
// correct at the end but was RENDERED between the clicks, and the eye sees
// every frame. hits counts the frames the path stood expanded.
static std::string g_expNeverPath;      // armed path; empty = not watching
static int g_expNeverHits = 0;          // frames rbHas(expanded, path) was true
static int g_expNeverFrames = 0;        // frames watched
static int g_chevPreExp = -1;           // "chevclick": expanded count pre-press
static bool g_browseKeysBlur = false;   // "blur" action: drop panel focus
// "imgmark" / "chkimgmark": how many documents were open before a gesture, and
// the assertion that the gesture opened none. An absolute chkimg:N says the
// same thing only as long as every count before it holds, which is a
// maintenance tax on a 258-action list; this pair says "this gesture opened
// nothing" in the two actions it takes to say it.
static int g_rbImgMark = -1;
static std::string g_browseKeysActs =
    "down,down,down,enter,flat,down,down,down,flat,down,tree,down,right,down,"
    "left,end,home,up,down,down,back,"
    // ---- what PREVIEW means for a stack row (docs/todo-open.md items 1/26).
    // Floated first (w400): the click actions aim real mouse events at the
    // cursor row's rectangle, and the user's saved layout must not decide
    // where that is. A grouped row previews as a poster + a 24-frame scrub,
    // ONE slot throughout; , / . step it; the server-temporal request may
    // surface the Temporal panel but must NOT steal the keys (chkfocus - the
    // scrub going dead right after asking for stats was defect 1).
    // (waitdir first: the prefix ends on "back", and keys fired before that
    // listing lands would walk the OLD rows and be reset mid-sequence.)
    "waitdir:rb,viewreset,w400,home,down,down,down,down,down,down,down,down,down,"
    "chkimg:1,chkpv:24,chkidx:0,period,chkidx:1,chknames:frame_001.npy[pv],"
    "comma,chkidx:0,svtemp,chkfocus:1,period,chkidx:1,comma,chkidx:0,"
    // Double-click = the stack opens exactly ONCE, poster dropped, nothing
    // else registered. The release half of the double-click used to re-run
    // the single-click path and park the poster NEXT TO the stack
    // (「一枚目とStackが2つFilesに登録される」) - the NAMES prove it stays gone.
    "dbl,waitimg:24,chkopen:1,chkimg:24,chknames:frame_###.npy*24,"
    // ...same overlap on a single-file row: promote once, no duplicate
    // preview, and Enter afterwards re-shows the open file instead of doing
    // nothing (the old final branch) or opening a second copy.
    "home,down,down,down,down,down,down,down,chkpv:0,dbl,chkimg:25,"
    "chknames:frame_###.npy*24+dark.npy,enter,chkimg:25,chkcurn:dark.npy,"
    // ...and a group whose numbering starts at 9 (padset): grouping, preview
    // scrub and the double-click open all work by POSITION, not by the
    // parsed number (the user's "連番の開始が0じゃないから?" hypothesis).
    "home,down,down,down,down,enter,waitdir:padset,down,down,chkpv:3,chkidx:0,"
    "period,chkidx:1,dbl,waitimg:28,chkopen:2,chkimg:28,"
    "chknames:frame_###.npy*24+dark.npy+f_9.npy+f_10.npy+f_11.npy,"
    "back,waitdir:rb,"
    // ---- folder rows in a LIST: one click ENTERS. A list has no "open it
    // where it is", so the single click is the folder's whole vocabulary and
    // there is no double-click meaning left to give it. Mouse back/forward and
    // Alt+Left/Right walk the history; a fresh navigation truncates forward.
    "home,down,chkatrow:digitset,click,waitdir:digitset,chkback:5,"
    "chkfwd:0,mback,waitdir:rb,chkfwd:1,mfwd,waitdir:digitset,chkfwd:0,"
    "altleft,waitdir:rb,chkfwd:1,altright,waitdir:digitset,altleft,waitdir:rb,"
    // ---- THE ORDER THE ROWS ARE IN (user decision 2026-08-05: natural).
    // rb/unpadded is the one fixture whose names are not zero-padded, so it is
    // the only one where the two candidate orders differ at all: read as TEXT,
    // lv10 falls between lv1 and lv2. Walking down with the arrow keys asserts
    // the order the panel actually laid out, through the real sort - and the
    // group row below the folders makes the same claim about the file names,
    // because the peer's extent spans frame_1 to frame_10 only if the run was
    // read by value.
    //
    // Placed after the history checks and before the next chkfwd: entering a
    // folder and backing out costs two history entries (chkback above counts
    // them) and truncates the forward stack (the double-click below sets it
    // back to 0 itself).
    //
    // This is the on-screen half. --browse-selftest owns the other half: that
    // this is also the order the stack is BUILT in.
    "home,down,down,down,down,down,down,enter,waitdir:unpadded,"
    "home,down,chkatrow:lv1,down,chkatrow:lv2,down,chkatrow:lv10,"
    "down,chkatrow:frame_1\xE2\x80\xA5" "10.npy,"
    "back,waitdir:rb,"
    // ...and a folder that is DOUBLE-clicked is entered ONCE, and opens
    // NOTHING. The habit is older than this panel, so the second click will
    // arrive whatever the list mode means by one click; it lands a frame or
    // two after rbGoTo, by which time the row it was aimed at has been
    // replaced under a pointer that never moved. This is the assertion the
    // one-click design lives or dies on, and it is the one CI produced the
    // counter-example for: imgs 44 -> 52 between "dbl" and the listing
    // landing - eight frames of the folder just entered, opened by a click
    // aimed at the folder. Checked twice, because an open is asynchronous and
    // one check right after the navigation could beat it.
    "home,down,chkatrow:digitset,imgmark,dbl,waitdir:digitset,chkimgmark,"
    "chkfwd:0,back,waitdir:rb,chkimgmark,"
    "home,down,down,click,waitdir:expset,chkfwd:0,back,waitdir:rb,"
    // ...in a TREE a folder has TWO verbs, so both gestures are spoken for:
    // the name toggles it in place, a double-click goes to it. That is not the
    // ce02f12 latch this file used to forbid - THAT expanded on click one and
    // CANCELLED on click two, so the expand was rendered and then undone. Here
    // click one expands and it STAYS expanded; the double-click adds a
    // navigation on top. Nothing is undone, so there is nothing to flash.
    //
    // The toggles are asserted on THIS ROW now (chkrowexp), which is what the
    // gap named here used to be: chkexp counts every expanded folder in the
    // panel, so "the name-click toggled this row" was only expressible when
    // the panel started fully collapsed - and the keyboard section far above
    // leaves one open with Right, so it was not expressible at all. cursorFull
    // is the exact key `expanded` is written in, so the question is asked
    // directly.
    //
    // The two toggles ALTERNATE name / chevron on purpose: two clicks at the
    // same pixel inside ImGui's double-click window are one gesture with a
    // count of two, not two gestures, and the second would be read as a
    // double-click. The name (row centre + 40 px) and the chevron (the left
    // gutter) are far enough apart that each press starts a fresh chain, so
    // this reads the same at any frame rate.
    "tree,home,down,chkatrow:digitset,chkrowexp:0,"
    "click,chkrowexp:1,chevclick,chkrowexp:0,"
    "click,chkrowexp:1,chevclick,chkrowexp:0,"
    // ...and the OTHER verb, on the same row: a double-click navigates, once,
    // and opens nothing on the way (the tree's navigation happens on the
    // second PRESS, so its release is one frame behind - the same window the
    // list's is two frames behind).
    "imgmark,dbl,waitdir:digitset,chkimgmark,back,waitdir:rb,tree,"
    // ---- Enter on a multi-selection opens EVERY selected row, each group
    // as its own stack (the action-row button is the MERGE; this is the other
    // one - only the cursor's row used to open).
    "home,down,down,down,enter,waitdir:gainset,down,down,ctrlclick,down,ctrlclick,"
    "enter,waitimg:44,chkopen:4,"
    "chknames:frame_###.npy*24+dark.npy+f_9.npy+f_10.npy+f_11.npy"
    "+gain10_00#.npy*8+gain20_00#.npy*8,"
    "back,waitdir:rb,"
    // ...then hand focus back to nobody and repeat four of the same keys: the
    // main view must own them again, or the gate would be "the arrows are dead"
    // rather than "the panel owns them while it has focus"
    "blur,down,up,end,home,"
    // ...then the panel geometry sweep: the toolbar must stay inside the panel
    // at every docked width, and Escape must close a menu and a context popup.
    // Swept with EVERY chip lit (viewreset pins the absolute state first, then
    // one toggle each): a chip only appears when its setting is off the
    // default, so the widest the toolbar can ever be is the case no default
    // run would ever draw - which is exactly the one where the filter box gets
    // pushed off the right edge.
    "viewreset,flat,tree,natorder,"
    "w271,w200,w180,w420,w700,w1150,w271,w700,w180,w0,"
    "viewreset,rctx,esc,fmenu,esc,disc,"
    // ---- INSTANCES (docs/todo-open.md item 17: the panel stopped being a
    // singleton). Reconnect the primordial panel, then open a SECOND Browse
    // standing in a DIFFERENT place at the same moment - the one sentence a
    // singleton can never make true (one App::rbrowse.dir = one place, ever).
    "reconnect,waitdir:rb,chkpanels:1,"
    "newpanel,chkpanels:2,waitdir:scanroot,chkdir:scanroot,"
    "target:1,chkdir:rb,"
    // independence: a filter typed into one panel does not narrow the other
    "filt:frame,chkfilt:frame,target:2,chkfilt:-,"
    // the keyboard belongs to the FOCUSED instance: panel 2 holds focus (it
    // was just created), so Down moves ITS cursor and leaves panel 1's alone
    "down,down,chkatrow:10lx,chkfocus:1,target:1,chkcursor:-1,target:2,"
    // selection is per instance too
    "ctrlclick,chksel:1,target:1,chksel:0,target:2,"
    // ...and so is the history: navigating panel 2 records nothing in panel 1
    "imgmark,dbl,waitdir:10lx,chkimgmark,chkback:1,target:1,chkback:0,"
    // focus back on panel 1: the same keys now move ITS cursor, and panel 2's
    // (reset to -1 by its own navigation) stays put
    "focus,chkfocus:1,down,chkcursor:0,target:2,chkcursor:-1,"
    // a session round-trip (item 10, decision A5): both places are written as
    // rbplace lines, every instance is torn down, and the restore reconnects
    // BOTH - each through its own worker - to where they stood
    "sessrt,chkpanels:2,target:1,waitdir:rb,target:2,waitdir:10lx,"
    // closing the second leaves the first fully working...
    "closep,chkpanels:1,target:1,chkdir:rb,focus,down,down,"
    "chkatrow:frame_000\xE2\x80\xA5" "023.npy,"
    // ...and the LAST one closing only hides: View > Panels > Browse reopens
    // it with its place, listing and cursor intact
    "hidep,chkshown:0,showp,chkshown:1,chkdir:rb,focus,up,chkatrow:..,"
    // ---- THE DRAWER IS GONE (docs/browse-topbar-design.md 10.2/10.3). Each
    // of these asserts one of its contents in the place it moved to, which is
    // the only way to tell "rehomed" from "deleted". Floated first so the path
    // line has room for the star and the click can be aimed at it.
    // The title names the machine; the status line counts what is in front of
    // you in both directions; the star is lit iff the place is bookmarked and
    // survives a real click at the end of the path line; and a failure speaks
    // ON the status line without opening a band that moves the listing.
    // (the filter from the instance segment is still on here, so the first
    // chkstat reads the narrowed form "N of M items"; clearing it reads the
    // plain "M items" - the count in both directions)
    "w400,chktitle,chkstat:0,"
    // ...a GROUPED row: one row, twenty-four frames, and the line says both
    // (the filter from the instance segment leaves exactly that row showing)
    "home,down,chkatrow:frame_000\xE2\x80\xA5" "023.npy,"
    "ctrlclick,chkstat:1,chkframes:24,ctrlclick,chkstat:0,"
    "filt:,marklist,chkstat:0,"
    "starmark,starclick,chkstar:1,starclick,chkstar:0,"
    "seterr,chkerr:1,clrerr,chkerr:0,"
    "setpv:2,chkproto:1,setpv:99,chkproto:1,pvback,chkproto:0,"
    // ...and a plain row: one row, one thing, no second count. Placed from
    // "home" because clearing the filter above rebuilt the shown set and reset
    // the cursor - a bare "down" would land on ".." , which selects nothing.
    "home,down,chkatrow:digitset,ctrlclick,chkstat:1,chkframes:0,w0,"
    // ---- the levels above the reader do not scroll away -------------------
    // 「階層表示で下にスクロールすると上層の階層が上に行って消える」. Every
    // ancestor of the row at the top of the viewport is held above it now, at
    // any depth, with no setting - so this asserts the two things a setting
    // would otherwise have to be trusted for.
    //
    // FIRST, that they are on screen. chkpin reads the band back as text,
    // outermost first, which is the same order and the same words a human
    // reads off the panel - not a flag saying the feature is on.
    // rb/expset/zdeep exists for exactly this shape of question: its last
    // sixty rows are all three levels down, so scrolling to the end is
    // guaranteed to leave the reader inside a/b/c. The panel is floated SHORT
    // (h360) as well, because "the fixture was longer than this screen" is not
    // a thing a gate should rest on - on a taller monitor the same script
    // would pass by never scrolling at all.
    //
    // SECOND, that a pinned row is the row and not a picture of it. pinclick
    // aims a real mouse click at the outermost pinned rectangle; what answers
    // is the verb a folder row has always had in a tree - collapse it - and
    // the proof is that one action later its whole subtree, and the band with
    // it, is gone. imgmark/chkimgmark says the same click opened nothing,
    // which is also what clicking a folder has always not done.
    "viewreset,w400,h360,waitdir:rb,home,"
    "down,down,chkatrow:expset,enter,waitdir:expset,"
    "home,down,chkatrow:zdeep,enter,waitdir:zdeep,"
    "tree,home,down,chkatrow:a,chkpin:-,right,idle:10,"
    "down,chkatrow:b,right,idle:10,"
    "down,chkatrow:c,right,idle:20,"
    "end,chkpin:a;b;c;,"
    "imgmark,pinclick,chkimgmark,chkpin:-,"
    "h0,w0,viewreset,back,waitdir:expset,back,waitdir:rb,"
    // ...and LAST the root-level popup collision: open the RAW dialog for a
    // QUEUE, then ask for the sequence prompt, which is the other root-level
    // modal. The RAW dialog must survive - which is why this runs last: it
    // deliberately ENDS with a modal up, and Escape leaves modals alone
    // (theirs is a decision), so anything checking Escape after it would be
    // reading the surviving modal rather than its own popup.
    "rawopen,popupcheck,seqask,popupcheck";

static void printUsage() {
    printf(
        "usage: viewer [options] [files or folders...]\n"
        "  files:  .npy, .npz, .png, .jpg, .vsession (saved session),\n"
        "          or raw binaries (.bin/.raw/...)\n"
        "  folder: loads the numbered files below it, one stack per group\n"
        "options:\n"
        "  --session <file.vsession>   restore a saved session\n"
        "  --raw-dtype <t>             storage of one sample: u8|u16|f32|f64\n"
        "  --raw-interp <i>            meaning of samples: gray|rgb|bgr|rgba|bgra|bayer|quad-bayer\n"
        "  --raw-format <fmt>          legacy combined names (gray8|...|rgbf32|bayer8|bayer16)\n"
        "  --raw-size <WxH>            raw dimensions, e.g. 1920x1080\n"
        "  --raw-crop <x,y,WxH>        decode only a window, e.g. 100,200,640x480\n"
        "  --raw-offset <bytes>        raw header offset (default 0)\n"
        "  --big-endian                raw byte order (default little endian)\n"
        "  --bayer-pattern <p>         RGGB|BGGR|GRBG|GBRG (default RGGB)\n"
        "  --quad-bayer                treat the CFA as Quad Bayer\n"
        "  --cfa <none|bayer|quad>     1ch files (.npy included) arrive mosaiced\n"
        "  --stack <mode>              numbered siblings: ask (default) | always | never\n"
        "  --mem-budget <GB>           what the stack loader may hold (default: auto,\n"
        "                              60%% of physical RAM)\n"
        // %s, not a copy: this line said "(H,W,3|4)" while the refusal it is
        // describing said something else, which is exactly the drift issue #71
        // was. There is now one spelling and it lives in one place.
        "  .npy shapes read natively:  %s\n"
        "                              any other shape is refused by name; to read one\n"
        "                              differently, use \"re-read as...\" in the Inspector\n"
        "  ssh://user@host/path.npy    view a file on another machine: the UI stays\n"
        "                              here, only the visible region is fetched\n"
        "  ssh://user@host/~/dir       connect and browse there instead (a host on\n"
        "                              its own works too) - what a desktop shortcut\n"
        "                              made by tools/install_shortcut.* passes\n"
        "  --remote-exe <path>         how to start the peer there (default:\n"
        "                              ~/.viewer/viewer-serve, self-installed on the\n"
        "                              first connection)\n"
        "  --remote-policy <p>         where a measurement runs: auto (default, on the\n"
        "                              server) | local-fetch (pull tiles, measure here)\n"
        "  --serve                     BE that peer: answer requests on stdin/stdout\n"
        "  --compare <off|wipe|split|diff>  A/B compare the first two images given\n"
        "  --bench <frames>            render N frames offscreen, print frame-time stats, exit\n"
        "  --bench-tiles               ...with A, B and two extra slots tiled side by\n"
        "                              side, and report the panes the canvas drew\n"
        "  --bench-step                ...and step A one frame per bench frame (A/B\n"
        "                              follow-frame cost: both sides recompute)\n"
        "  --bench-panels              ...with Projection, Histogram and Temporal side\n"
        "                              by side rather than tabbed, so all three draw\n"
        "  --no-ab-throttle            do NOT hold the B statistics while stepping\n"
        "  --gl-probe                  can this machine make the GL context the viewer\n"
        "                              needs? name it and exit (0 yes, 3 no + why)\n"
        "  --no-window                 start WITHOUT a window or a GL context, for the\n"
        "                              selftests that never draw (nothing else runs)\n"
        "  --frame <system|integrated> title bar: the desktop's, or the one drawn\n"
        "                              in the menu bar (default: last used)\n"
        "  --window-offset <dx,dy>     shift the window off its default position (how\n"
        "                              \"Open in new window\" cascades the child ~40 px)\n"
        "  --secondary                 a spawned extra window: prefs are read-only\n"
        "  --zoom <z>                  initial zoom (1 = 100%%)\n"
        "  --center <x,y>              initial view center in image pixels\n"
        "  -h, --help                  show this help\n"
        // All of them, always. docs/manual.md 9 promises this list is the canon,
        // and it said 8 of 22 for long enough that --verify-selftest could be
        // believed not to exist. Adding a flag means adding its line here.
        "self-tests (load, check, print, exit; 0 = pass). ctest runs all 22:\n"
        "  --lin-selftest              linearity fit over the stacks given\n"
        "  --frame-lin-selftest        frame-wise linearity (Temporal section):\n"
        "                              synthetic stacks with exact means, both methods\n"
        "  --sweepfile-selftest <dir>  a sweep of ONE .npy per level: names + fit\n"
        "  --series-selftest <dir>     series (系列) model + invariants\n"
        "  --range-selftest <dir>      every A/B display-range combination (2 stacks)\n"
        "  --framestats-selftest       per-frame statistics, TSV to stdout\n"
        "  --abstats-selftest <dir>    A/B statistics caches: two slots\n"
        "  --tile-selftest <dir>       side-by-side compare panes: geometry\n"
        "  --export-selftest <dir>     PNG export + montage\n"
        "  --export-tsv-selftest <dir> the Temporal export document\n"
        "  --picker-selftest <dir>     stack picker: filter cut + merge\n"
        "  --scan-selftest <dir>       Open Folder: every stack below a root\n"
        "  --close-selftest <dir>      closing, per stack\n"
        "  --batch-selftest <dir>      move-to-batch + session round trip\n"
        "  --verify-selftest <dir>     the corners the others miss (V1-V18)\n"
        "  --roistats-selftest         the ROI table's numbers and its std/mean column,\n"
        "                              through the real panel, on analytic fixtures\n"
        "  --abeq-selftest             A and B on ONE document: both sides drawn,\n"
        "                              coinciding, through the real panel\n"
        "  --derive-selftest <dir>     derive a stack from a stack: counts, copy, follow\n"
        "  --stackavg-selftest <dir>   a stack's per-pixel mean opened as one frame:\n"
        "                              value, NaN exclusion, CFA planes, provenance\n"
        "  --srcmap-selftest <dir>     which document holds which pixels: shared sources,\n"
        "                              refcounts, Watch baselines\n"
        "  --media-selftest <dir>      PNG/JPEG bit depth, channels and notes; TIFF's refusal\n"
        "  --newwin-selftest <dir>     instance autosave slots + spawn line\n"
        "  --browse-selftest <dir>     Browse panel behaviour\n"
        "  --localbrowse-selftest <dir>  Browse against the local filesystem\n"
        "  --browse-keys-selftest <dir>  the Browse panel's KEYBOARD, replayed as real\n"
        "                              input into real frames (--browse-keys <script>\n"
        "                              overrides the canned action list)\n"
        "  --rtemporal-selftest <dir>  temporal analysis driven from the browser\n"
        "  --remote-selftest <file>    remote round trip against a peer started here;\n"
        "                              pass --remote-exe to test viewer-serve itself\n",
        NPY_NATIVE_FORMS);
}

static void parseCli(int argc, char** argv) {
    RawDialog d;                       // accumulates --raw-* options for positional raw files
    bool rawReady = false, cliQuad = false;
    bool haveZoom = false, haveCenter = false;
    float zoom = 1, cx = 0, cy = 0;
    int cliCompare = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--session") {
            std::string p = next();
            if (!p.empty()) openPath(p);
        } else if (a == "--raw-format") {          // legacy combined names
            std::string v = next();
            bool found = false;
            for (int f = 0; f < 12; f++)
                if (v == LEGACY_RAW_NAMES[f]) {
                    d.dtype = LEGACY_RAW_MAP[f][0];
                    d.interp = LEGACY_RAW_MAP[f][1];
                    rawReady = found = true;
                }
            if (!found) fprintf(stderr, "unknown raw format: %s (see --help)\n", v.c_str());
        } else if (a == "--raw-dtype") {
            std::string v = next();
            for (int f = 0; f < RD_COUNT; f++)
                if (v == RAW_DTYPE_NAMES[f]) { d.dtype = f; rawReady = true; }
        } else if (a == "--raw-interp") {
            std::string v = next();
            for (int f = 0; f < RI_COUNT; f++)
                if (v == RAW_INTERP_CLI[f]) { d.interp = f; rawReady = true; }
        } else if (a == "--raw-size") {
            std::string v = next();
            size_t x = v.find_first_of("xX*");
            if (x != std::string::npos) {
                d.w = std::clamp(atoi(v.substr(0, x).c_str()), 1, 32768);
                d.h = std::clamp(atoi(v.substr(x + 1).c_str()), 1, 32768);
            }
        } else if (a == "--raw-crop") {            // x,y,WxH
            std::string v = next();
            int x = 0, y = 0, w = 0, h = 0;
            if (sscanf(v.c_str(), "%d,%d,%dx%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
                d.cropOn = true;
                d.cropX = std::max(0, x); d.cropY = std::max(0, y);
                d.cropW = w; d.cropH = h;
            } else {
                fprintf(stderr, "bad --raw-crop (expected x,y,WxH)\n");
            }
        } else if (a == "--raw-offset") {
            d.offset = std::max(0, atoi(next().c_str()));
        } else if (a == "--big-endian") {
            d.littleEndian = false;
        } else if (a == "--bayer-pattern") {
            std::string v = next();
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            for (int p = 0; p < 4; p++)
                if (v == CFA_PATTERNS[p]) d.cfaPattern = p;
            // ...and forward it to --cfa NOW, so the two can be given in either
            // order. Only --cfa used to publish the pattern, which made
            // "--cfa bayer --bayer-pattern GRBG" demosaic as RGGB without a word
            // - the CMake tests write exactly that order and only escape it
            // because RGGB is index 0. Nothing reads forceCfaPattern unless
            // --cfa was given (addImage tests forceCfa >= 0 first).
            app.forceCfaPattern = d.cfaPattern;
        } else if (a == "--quad-bayer") {
            cliQuad = true;                        // applied at load; order-independent
            rawReady = true;
        } else if (a == "--npy-axis") {
            // docs/input-adapters.md §3.4. One global flag decided the meaning
            // of every 3-D array in a run, so a session holding one file to read
            // as a stack and another to read as colour could not be expressed at
            // all. Say so and carry on - silence would leave the user believing
            // the old meaning still applied.
            next();                                // swallow its value, not a path
            fprintf(stderr, "--npy-axis is gone: (F,H,W) is F frames and (H,W,C<=4) is one "
                            "frame of C channels, always. To read one file differently, open "
                            "it and use \"re-read as...\" in the Inspector - it is per file "
                            "and it is remembered.\n");
        } else if (a == "--stack") {               // ask | always | never
            std::string v = next();
            if (v == "always") app.seqLoadMode = 1;
            else if (v == "never") app.seqLoadMode = 2;
            else if (v == "ask") app.seqLoadMode = 0;
            else fprintf(stderr, "--stack expects ask|always|never\n");
        } else if (a == "--bench" || a == "--crash-test" || a == "--frame" ||
                   a == "--window-offset") {
            next();                                // consumed in main(), not an error
        } else if (a == "--bench-step" || a == "--bench-tiles" || a == "--bench-panels" ||
                   a == "--secondary" || a == "--no-window") {
            /* consumed in main(): no value */
        } else if (a == "--no-ab-throttle") {
            g_abNoThrottle = true;     // measure what the B-slot throttle saves
        } else if (a == "--cfa") {                 // none | bayer | quad
            std::string v = next();
            app.forceCfa = v == "bayer" ? 1 : v == "quad" || v == "quad-bayer" ? 2
                         : v == "none" ? 0 : -1;
            if (app.forceCfa < 0) fprintf(stderr, "--cfa expects none|bayer|quad\n");
            app.forceCfaPattern = d.cfaPattern;    // --bayer-pattern seen so far, if any
        } else if (a == "--lin-selftest") {
            g_linSelftest = true;                  // handled in main() after loading
        } else if (a == "--range-selftest") {
            g_rangeSelftest = next();
        } else if (a == "--framestats-selftest") {
            g_fstatSelftest = true;                // handled in main() after loading
        } else if (a == "--export-tsv-selftest") {
            g_exportTsvSelftest = next();          // handled in main()
        } else if (a == "--frame-lin-selftest") {
            g_frameLinSelftest = true;             // handled in main()
        } else if (a == "--roistats-selftest") {
            g_roiStatsSelftest = true;             // handled in main()
        } else if (a == "--abeq-selftest") {
            g_abEqSelftest = true;                 // handled in main()
        } else if (a == "--scan-selftest") {
            g_scanSelftest = next();               // handled in main()
        } else if (a == "--picker-selftest") {
            g_pickerSelftest = next();             // handled in main()
        } else if (a == "--close-selftest") {
            g_closeSelftest = next();              // handled in main()
        } else if (a == "--batch-selftest") {
            g_batchSelftest = next();              // handled in main()
        } else if (a == "--rtemporal-selftest") {
            g_rtemporalSelftest = next();          // handled in main()
        } else if (a == "--localbrowse-selftest") {
            g_localbrowseSelftest = next();        // handled in main()
        } else if (a == "--browse-selftest") {
            g_browseSelftest = next();             // handled in main()
        } else if (a == "--browse-keys-selftest") {
            g_browseKeys = next();                 // handled in main()'s frame loop
        } else if (a == "--browse-keys") {
            g_browseKeysActs = next();             // override the canned action list
        } else if (a == "--verify-selftest") {
            g_verifySelftest = next();             // handled in main()
        } else if (a == "--derive-selftest") {
            g_deriveSelftest = next();             // handled in main()
        } else if (a == "--stackavg-selftest") {
            g_stackAvgSelftest = next();           // handled in main()
        } else if (a == "--abstats-selftest") {
            g_abstatsSelftest = next();            // handled in main()
        } else if (a == "--tile-selftest") {
            g_tileSelftest = next();               // handled in main()
        } else if (a == "--export-selftest") {
            g_exportSelftest = next();             // handled in main()
        } else if (a == "--series-selftest") {
            g_seriesSelftest = next();             // handled in main()
        } else if (a == "--sweepfile-selftest") {
            g_sweepFileSelftest = next();          // handled in main()
        } else if (a == "--newwin-selftest") {
            g_newwinSelftest = next();             // handled in main()
        } else if (a == "--reload-selftest") {
            g_reloadSelftest = next();             // handled in main()
        } else if (a == "--srcmap-selftest") {
            g_srcmapSelftest = next();             // handled in main()
        } else if (a == "--media-selftest") {
            g_mediaSelftest = next();              // handled in main()
        } else if (a == "--remote-selftest") {
            next();
        } else if (a == "--remote-policy") {        // auto | local-fetch
            std::string v = next();
            if (v == "auto") app.procPolicy = App::PolAuto;
            else if (v == "server") {          // accepted, so scripts keep working
                app.procPolicy = App::PolAuto;
                fprintf(stderr, "--remote-policy server is retired (it was identical to "
                                "auto): using auto\n");
            }
            else if (v == "local-fetch" || v == "local") app.procPolicy = App::PolLocalFetch;
            else fprintf(stderr, "--remote-policy expects auto|local-fetch\n");
        } else if (a == "--remote-exe") {          // how to invoke the peer over ssh
            app.remoteExe = next();
        } else if (a == "--mem-budget") {          // GB the stack loader may use
            app.memBudgetGB = std::clamp((float)atof(next().c_str()), 0.5f, 4096.0f);
        } else if (a == "--compare") {             // off | wipe | split
            std::string v = next();
            if (v == "wipe") cliCompare = App::CmpWipe;
            else if (v == "split" || v == "side") cliCompare = App::CmpSplit;
            else if (v == "diff") cliCompare = App::CmpDiff;
            else if (v == "off") cliCompare = App::CmpOff;
            else fprintf(stderr, "--compare expects off|wipe|split|diff\n");
        } else if (a == "--zoom") {
            zoom = (float)atof(next().c_str()); haveZoom = true;
        } else if (a == "--center") {
            std::string v = next();
            size_t c = v.find(',');
            if (c != std::string::npos) {
                cx = (float)atof(v.substr(0, c).c_str());
                cy = (float)atof(v.substr(c + 1).c_str());
                haveCenter = true;
            }
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "unknown option: %s (see --help)\n", a.c_str());
        } else if (std::filesystem::is_directory(pathFromUtf8(a))) {
            openFolder(a);                     // a folder = every stack below it
        } else {
            std::string low = a;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            bool special = (low.size() > 4 && low.compare(low.size() - 4, 4, ".npy") == 0) ||
                           (low.size() > 4 && low.compare(low.size() - 4, 4, ".npz") == 0) ||
                           (low.size() > 9 && low.compare(low.size() - 9, 9, ".vsession") == 0) ||
                           // ...and a picture format, which carries its own
                           // dimensions and dtype: --raw-* on the same command
                           // line is for the files that carry none
                           imagefile::forPath(a) != nullptr;
            if (!special && rawReady) {   // raw params given: load directly, no dialog
                if (cliQuad && RAW_INTERP_CH[d.interp] == 1) d.interp = RI_QUAD;
                d.path = a;
                std::string err = loadRaw(d);
                if (!err.empty()) toast(baseName(a) + ": " + err, true);
                else maybeOfferSequence(app.current);
            } else {
                openPath(a);
            }
        }
    }
    // Applied later, not here: with --stack always the sibling frames are still
    // arriving on the loader thread, so at this point there may be only one image.
    if (cliCompare >= 0) app.pendingCompare = cliCompare;
    if (haveZoom || haveCenter) {
        if (haveZoom) app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        if (haveCenter) app.view.center = ImVec2(cx, cy);
        else if (cur()) app.view.center = ImVec2(cur()->w * 0.5f, cur()->h * 0.5f);
        app.fitRequested = false;      // explicit view from CLI wins over fit-on-load
    }
}

// MEASURE selftest glue: capture what an analyzer emits, formatted the same way
// on both sides so the comparison is string-exact.
static std::vector<std::pair<std::string, std::string>>* g_mstRows;
static void mstNum(void*, const char* k, double v) {
    char b[64];
    snprintf(b, 64, "%.9g", v);
    g_mstRows->emplace_back(k ? k : "", b);
}
static void mstTxt(void*, const char* k, const char* v) {
    g_mstRows->emplace_back(k ? k : "", v ? v : "");
}
static void mstSer(void*, const char*, const char*, const char*,
                   const float*, const float*, uint32_t) {}

// Round-trip check for the remote path, without needing an ssh host: start a
// local peer, ask for the same pixels two ways, and compare. A viewer that shows
// subtly wrong pixels over a link would be worse than one that shows none.
static int remoteSelfTest(const char* exe, const char* path) {
    remote::Session s;
    std::string err;
    if (!s.start("", exe, err)) { fprintf(stderr, "selftest: %s\n", err.c_str()); return 1; }
    remote::Meta m;
    if (!s.meta(path, m, err)) { fprintf(stderr, "selftest META: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "selftest: meta %dx%d %dch %s frames=%d\n", m.w, m.h, m.ch,
            m.dtype.c_str(), m.frames);

    std::string localErr = loadNpy(path);          // the local decoder, for reference
    if (!localErr.empty() || !cur()) {
        fprintf(stderr, "selftest: local load failed: %s\n", localErr.c_str());
        return 1;
    }
    const ImageDoc& ref = *cur();
    int bad = 0;
    std::string dir = path, base = path;       // the test file's folder and name
    {
        size_t sl = dir.find_last_of("/\\");
        if (sl == std::string::npos) dir = ".";
        else { dir = dir.substr(0, sl); base = base.substr(sl + 1); }
    }
    {   // LIST v3: the listing's metadata must agree with the local loader
        std::vector<remote::Entry> ents;
        if (!s.list(dir, ents, err)) {
            fprintf(stderr, "selftest LIST: %s\n", err.c_str());
            return 1;
        }
        const remote::Entry* me = nullptr;
        for (const auto& e : ents) if (e.name == base) me = &e;
        for (const auto& e : ents)                 // grouping may have folded it
            if (!me && e.group)
                for (const auto& mm : e.members) if (mm == base) me = &e;
        size_t bad0 = 0;
        if (!me) {
            fprintf(stderr, "selftest LIST: %s not in the listing of %s\n",
                    base.c_str(), dir.c_str());
            bad0++;
        } else {
            uint64_t px = 1;
            for (int i = 0; i < me->ndim; i++) px *= me->dims[i];
            uint64_t refPx = (uint64_t)m.frames * ref.w * ref.h * ref.ch;
            if (!me->hasMeta || me->dtype != m.dtype || px != refPx) {
                fprintf(stderr, "selftest LIST: meta mismatch (dtype %s vs %s, "
                                "%llu vs %llu samples)\n",
                        me->dtype.c_str(), m.dtype.c_str(),
                        (unsigned long long)px, (unsigned long long)refPx);
                bad0++;
            }
            if (me->mtime <= 0) {
                fprintf(stderr, "selftest LIST: mtime missing (%lld unix s)\n",
                        (long long)me->mtime);
                bad0++;
            }
        }
        fprintf(stderr, "selftest: LIST v3 %s: %s  %s  mtime %lld unix s : %s\n",
                base.c_str(), me ? fmtEntryShape(*me).c_str() : "?",
                me ? fmtBytesHuman(me->size).c_str() : "?",
                me ? (long long)me->mtime : 0, bad0 ? "FAIL" : "ok");
        bad += bad0 ? 1 : 0;
    }
    {   // A v2 peer's LIST reply must parse into "unknown" metadata, not into an
        // error - this is the compatibility contract, tested without a v2 binary.
        std::vector<uint8_t> pl;
        auto pu32 = [&](uint32_t v) { pl.insert(pl.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
        auto pstr = [&](const std::string& t) { pu32((uint32_t)t.size());
                                                pl.insert(pl.end(), t.begin(), t.end()); };
        pu32(2);
        pstr("a.npy"); pu32(0); pu32(5); pu32(0);      // file, 5 bytes
        pstr("sub");   pu32(1); pu32(0); pu32(0);      // directory
        std::vector<remote::Entry> v2;
        std::string perr;
        bool ok = remote::parseListPayload(pl, 2, v2, perr) && v2.size() == 2 &&
                  !v2[0].dir && v2[0].size == 5 && !v2[0].hasMeta &&
                  v2[0].mtime == 0 && !v2[0].group && v2[1].dir;
        fprintf(stderr, "selftest: LIST v2-compat parse (missing fields -> unknown): %s\n",
                ok ? "ok" : "FAIL");
        bad += ok ? 0 : 1;
    }
    {   // Grouping: 24 numbered frames fold into ONE synthetic row; the loose
        // .npy and the .txt stay plain. Fixture from tools/gen_testdata.py.
        std::string rb = dir + "/rb";
        std::vector<remote::Entry> ents;
        if (!s.list(rb, ents, err)) {
            fprintf(stderr, "selftest: LIST grouping: skipped (%s: %s)\n",
                    rb.c_str(), err.c_str());
        } else {
            int nGroups = 0, nPlain = 0;
            const remote::Entry* g = nullptr;
            for (const auto& e : ents) {
                if (e.dir) continue;                   // scanroot/ lives here too
                if (e.group) { nGroups++; g = &e; }
                else nPlain++;
            }
            // the pattern SHOWS ITS EXTENT: no bare '?' run survives into the
            // name a user reads (rp::patternWithExtent)
            bool ok = nGroups == 1 && nPlain == 3 && g && g->frames == 24 &&
                      g->members.size() == 24 && g->hasMeta && g->dtype == "f32" &&
                      g->name == "frame_000\xE2\x80\xA5" "023.npy";
            // the member names must lead back to real files, or the row is a lie
            remote::Meta gm;
            if (ok && !s.meta(rb + "/" + g->members[0], gm, err)) {
                fprintf(stderr, "selftest LIST grouping: member META: %s\n", err.c_str());
                ok = false;
            }
            fprintf(stderr, "selftest: LIST grouping %s: %d group(s), %d single(s), "
                            "%u frames %s: %s\n",
                    g ? g->name.c_str() : "?", nGroups, nPlain,
                    g ? g->frames : 0, g ? fmtEntryShape(*g).c_str() : "?",
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Grouping stage 2: the extent covers ONLY the varying digit run.
        // gainset has a constant gain digit next to the frame counter - the
        // pattern must keep it LITERAL (gain10_000..007.npy, never
        // gain10..20_...), or two gains read as one stack.
        std::string gd = dir + "/rb/gainset";
        std::vector<remote::Entry> ents;
        if (!s.list(gd, ents, err)) {
            fprintf(stderr, "selftest: LIST gainset: skipped (%s: %s)\n",
                    gd.c_str(), err.c_str());
        } else {
            std::vector<std::string> pats;
            bool frames8 = true;
            int nPlain = 0;
            for (const auto& e : ents) {
                if (e.dir) continue;
                if (e.group) { pats.push_back(e.name); frames8 &= e.frames == 8; }
                else nPlain++;
            }
            std::sort(pats.begin(), pats.end());
            bool ok = pats.size() == 2 && nPlain == 0 && frames8 &&
                      pats[0] == "gain10_000\xE2\x80\xA5" "007.npy" &&
                      pats[1] == "gain20_000\xE2\x80\xA5" "007.npy";
            fprintf(stderr, "selftest: LIST grouping gainset -> %d group(s) [%s], "
                            "8 frames each=%d: %s\n",
                    (int)pats.size(),
                    (pats.empty() ? std::string("?")
                                  : pats.size() < 2 ? pats[0] : pats[0] + "," + pats[1]).c_str(),
                    frames8 ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Grouping stage 2, split: expset varies TWO digit runs. One stack per
        // condition (g00_e??.npy / g01_e??.npy), never a second '?' run - a
        // condition-mixed stack has a meaningless sigma_t.
        std::string ed = dir + "/rb/expset";
        std::vector<remote::Entry> ents;
        if (!s.list(ed, ents, err)) {
            fprintf(stderr, "selftest: LIST expset: skipped (%s: %s)\n",
                    ed.c_str(), err.c_str());
        } else {
            std::vector<std::string> pats;
            bool frames4 = true;
            int nPlain = 0;
            for (const auto& e : ents) {
                if (e.dir) continue;
                if (e.group) { pats.push_back(e.name); frames4 &= e.frames == 4; }
                else nPlain++;
            }
            std::sort(pats.begin(), pats.end());
            bool ok = pats.size() == 2 && nPlain == 0 && frames4 &&
                      pats[0] == "g00_e00\xE2\x80\xA5" "03.npy" &&
                      pats[1] == "g01_e00\xE2\x80\xA5" "03.npy";
            fprintf(stderr, "selftest: LIST grouping expset (2 varying runs) -> "
                            "%d group(s) [%s], 4 frames each=%d: %s\n",
                    (int)pats.size(),
                    (pats.empty() ? std::string("?")
                                  : pats.size() < 2 ? pats[0] : pats[0] + "," + pats[1]).c_str(),
                    frames4 ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // Pattern extent, and the agreement between the two ends. digitset is
        // all digits ("????.npy" by the rule, which says nothing); padset has
        // uneven padding, so the extent has to be read by VALUE. For each, the
        // peer's LIST pattern and the pattern the CLIENT builds for the same
        // folder (findSequenceSiblings) must be the identical string - a
        // capture opened locally and listed over ssh names one stack, not two.
        struct PatCase { const char* sub; const char* first; const char* want; };
        const PatCase pcs[] = {
            { "digitset", "0000.npy", "0000\xE2\x80\xA5" "0003.npy" },
            { "padset",   "f_9.npy",  "f_9\xE2\x80\xA5" "11.npy" },
            { "gainset",  "gain10_000.npy", "gain10_000\xE2\x80\xA5" "007.npy" },
        };
        for (const PatCase& pc : pcs) {
            std::string pd = dir + "/rb/" + pc.sub;
            std::vector<remote::Entry> ents;
            if (!s.list(pd, ents, err)) {
                fprintf(stderr, "selftest: pattern %s: skipped (%s)\n", pc.sub, err.c_str());
                continue;
            }
            std::string peerPat;
            for (const auto& e : ents) if (e.group && peerPat.empty()) peerPat = e.name;
            std::string clientPat;
            findSequenceSiblings(pd + "/" + pc.first, clientPat);
            bool ok = peerPat == pc.want && clientPat == pc.want;
            fprintf(stderr, "selftest: pattern %s -> peer '%s', client '%s' "
                            "(want '%s'): %s\n",
                    pc.sub, peerPat.c_str(), clientPat.c_str(), pc.want,
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
        {   // The pattern is ALSO the stack name, and linearity's Auto levels
            // parses stack names. It reads the FOLDER part - verified here, not
            // assumed - so a changed file part is safe; and where there is no
            // folder part the extent run is cut out first, so an all-digit
            // stack still reports "no level" exactly as "????.npy" did.
            double folder = extractLevelFromName("10lx/0000\xE2\x80\xA5" "0003.npy");
            double bare   = extractLevelFromName("0000\xE2\x80\xA5" "0003.npy");
            double gain   = extractLevelFromName("gain10_000\xE2\x80\xA5" "007.npy");
            bool ok = folder == 10.0 && !std::isfinite(bare) && gain == 10.0;
            fprintf(stderr, "selftest: auto-level from '10lx/...' -> %g, from a bare "
                            "extent -> %s, from 'gain10_...' -> %g: %s\n",
                    folder, std::isfinite(bare) ? "a number (WRONG)" : "none", gain,
                    ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // SCAN (the remote openFolder): 3 illumination folders -> 3 stacks
        std::string sroot = dir + "/rb/scanroot";
        std::vector<remote::ScanGroup> gs;
        bool trunc = false;
        int skipped = 0;
        if (!s.scan(sroot, 6, 256, gs, trunc, skipped, err)) {
            fprintf(stderr, "selftest: SCAN: skipped (%s: %s)\n", sroot.c_str(), err.c_str());
        } else {
            bool ok = gs.size() == 3 && !trunc;
            std::string dirsSeen;
            for (const auto& g : gs) {
                dirsSeen += (dirsSeen.empty() ? "" : ",") + g.dir;
                if (g.entry.frames != 8 || g.entry.members.size() != 8 ||
                    !g.entry.hasMeta || g.dir.empty())
                    ok = false;
            }
            fprintf(stderr, "selftest: SCAN %s -> %d stack(s) [%s], 8 frames each, "
                            "truncated=%d, %d dir(s) skipped: %s\n",
                    sroot.c_str(), (int)gs.size(), dirsSeen.c_str(),
                    trunc ? 1 : 0, skipped, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;
        }
    }
    {   // GLOB: recursive find - full count, cap-5 truncation, bare substring
        std::string groot = dir + "/rb";
        std::vector<remote::GlobHit> hits;
        bool trunc = false;
        int skipped = 0;
        if (!s.glob(groot, "**/frame_*.npy", 6, 2000, hits, trunc, skipped, err)) {
            fprintf(stderr, "selftest: GLOB: skipped (%s: %s)\n", groot.c_str(), err.c_str());
        } else {
            // 24 in rb/ itself + 3 x 8 under scanroot/ + 3 under unpadded/
            // (frame_1, frame_2, frame_10 - the unpadded names, which is the
            //  whole of what that fixture contributes here)
            bool ok = hits.size() == 51 && !trunc;
            for (const auto& h : hits) if (h.dir) ok = false;
            fprintf(stderr, "selftest: GLOB **/frame_*.npy under %s -> %d hit(s), "
                            "truncated=%d: %s\n",
                    groot.c_str(), (int)hits.size(), trunc ? 1 : 0, ok ? "ok" : "FAIL");
            bad += ok ? 0 : 1;

            if (!s.glob(groot, "**/frame_*.npy", 6, 5, hits, trunc, skipped, err)) {
                fprintf(stderr, "selftest GLOB cap: %s\n", err.c_str());
                bad++;
            } else {
                bool ok2 = hits.size() == 5 && trunc;
                fprintf(stderr, "selftest: GLOB cap 5 -> %d hit(s), truncated=%d: %s\n",
                        (int)hits.size(), trunc ? 1 : 0, ok2 ? "ok" : "FAIL");
                bad += ok2 ? 0 : 1;
            }
            if (!s.glob(groot, "dark", 6, 2000, hits, trunc, skipped, err)) {
                fprintf(stderr, "selftest GLOB substring: %s\n", err.c_str());
                bad++;
            } else {
                bool ok3 = hits.size() == 1 && hits[0].rel == "dark.npy";
                fprintf(stderr, "selftest: GLOB substring 'dark' -> %d hit(s) (%s): %s\n",
                        (int)hits.size(), hits.empty() ? "?" : hits[0].rel.c_str(),
                        ok3 ? "ok" : "FAIL");
                bad += ok3 ? 0 : 1;
            }
        }
    }
    struct Case { int x, y, w, h, step; };
    const Case cases[] = {
        { 0, 0, ref.w, ref.h, 1 },                              // whole frame
        { 0, 0, ref.w, ref.h, 3 },                              // decimated overview
        { ref.w / 4, ref.h / 4, ref.w / 2, ref.h / 2, 1 },      // zoomed-in crop
        { 1, 1, 17, 13, 2 },                                    // odd rect, odd stride
    };
    for (const Case& c : cases) {
        std::vector<float> got;
        int gw = 0, gh = 0, gch = 0;
        std::string dt;
        if (!s.tile(path, 0, c.x, c.y, c.w, c.h, c.step, got, gw, gh, gch, dt, err)) {
            fprintf(stderr, "selftest TILE: %s\n", err.c_str());
            return 1;
        }
        size_t mismatch = 0;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                for (int ci = 0; ci < gch; ci++) {
                    float a = got[((size_t)y * gw + x) * gch + ci];
                    float b = ref.sample(c.x + x * c.step, c.y + y * c.step, ci);
                    if (a != b && !(std::isnan(a) && std::isnan(b))) mismatch++;
                }
        static uint64_t prevRx = 0;
        uint64_t wire = s.bytesReceived() - prevRx;
        prevRx = s.bytesReceived();
        fprintf(stderr, "selftest: rect %dx%d@%d,%d step %d -> %dx%d %dch %s : %s "
                        "(%zu mismatches, %.2f MB on the wire for %.2f MB of samples)\n",
                c.w, c.h, c.x, c.y, c.step, gw, gh, gch, dt.c_str(),
                mismatch ? "FAIL" : "ok", mismatch,
                wire / 1048576.0, (double)got.size() * 4 / 1048576.0);
        bad += mismatch ? 1 : 0;
    }
    {   // A blocked protocol read must be interruptible, or Quit waits it out
        // with a dead window on screen: the workers poll their stop flags only
        // BETWEEN queue items, so one parked inside recv() never saw them.
        std::atomic<bool> stopFlag{ false };
        remote::Session s2;
        s2.setAbort(&stopFlag);
        std::string e2;
        std::vector<remote::Entry> ents2;
        bool started = s2.startOn("", 0, exe, e2);
        bool listedOk = started && s2.list(dir, ents2, e2);
        stopFlag = true;                       // exactly what stop*() sets
        double at0 = nowSec();
        bool listedAfter = s2.list(dir, ents2, e2);
        double asecs = nowSec() - at0;
        bool abortOk = started && listedOk && !listedAfter && asecs < 1.0;
        fprintf(stderr, "selftest: abortable read: LIST before the flag %s, after it %s "
                        "in %.3f s: %s\n",
                listedOk ? "ok" : "FAILED", listedAfter ? "STILL SUCCEEDED" : "gave up",
                asecs, abortOk ? "ok" : "FAIL");
        bad += abortOk ? 0 : 1;
    }
    {   // A TILE reply sizes an allocation, so it is validated against the
        // REQUEST and not only against itself. Both hostile shapes below pass
        // every self-consistency test the client used to apply.
        struct C { const char* what; uint32_t rw, rh, st, w, h, ch, dt, raw; bool want; };
        const C cases[] = {
            // 64-bit overflow: 2^30 x 2^30 x 4 x 4 bytes is exactly 2^64, which
            // wraps to 0 and matches rawBytes = 0. toFloat then resized to 2^62
            // floats and std::length_error killed the process.
            { "overflow to 0", 512, 512, 1, 0x40000000u, 0x40000000u, 4, rp::DT_F32, 0, false },
            // no overflow at all: fully self-consistent, ~4 MB of deflate on the
            // wire (under recv's 512 MB cap), 4 GB of allocation on arrival
            { "deflate bomb", 512, 512, 1, 32768, 32767, 1, rp::DT_U32, 4294836224u, false },
            { "more rows than asked", 512, 512, 1, 512, 513, 1, rp::DT_U16, 512u*513*2, false },
            { "bad dtype", 512, 512, 1, 8, 8, 1, rp::DT_COUNT, 64, false },
            { "byte count disagrees", 512, 512, 1, 8, 8, 1, rp::DT_U16, 64, false },
            { "legitimate full tile", 512, 512, 1, 512, 512, 1, rp::DT_U16, 512u*512*2, true },
            { "legitimate decimated", 513, 513, 3, 171, 171, 3, rp::DT_F32, 171u*171*3*4, true },
        };
        int badT = 0;
        for (const C& c : cases) {
            bool got = remote::tileReplySane(c.rw, c.rh, c.st, c.w, c.h, c.ch, c.dt, c.raw);
            if (got != c.want) {
                fprintf(stderr, "selftest TILE guard: %s -> %s, wanted %s\n",
                        c.what, got ? "accepted" : "rejected", c.want ? "accepted" : "rejected");
                badT++;
            }
        }
        fprintf(stderr, "selftest: TILE reply guard over %d shape(s): %s\n",
                (int)(sizeof cases / sizeof cases[0]), badT ? "FAIL" : "ok");
        bad += badT ? 1 : 0;
    }
    // MEASURE: the same plugin must produce the same numbers on both sides.
    // Same code, same f32 input - the comparison is string-exact on %.9g.
    plugin_host::loadAll({ plugin_host::exeDir() + "/plugins" },
                         [](const std::string&, bool) {});
    const auto& anas = plugin_host::analyzers();
    int pick = -1;
    for (int i = 0; i < (int)anas.size(); i++)
        if (anas[i].name.find("stats") != std::string::npos) { pick = i; break; }
    if (pick < 0 && !anas.empty()) pick = 0;
    if (pick >= 0) {
        const AnalyzerPluginInfo& a = anas[pick];
        std::vector<std::pair<std::string, std::string>> local;
        g_mstRows = &local;
        psFrame fr = makeFrame(ref);
        psRect roi{ (uint32_t)(ref.w / 4), (uint32_t)(ref.h / 4),
                    (uint32_t)(ref.w / 2), (uint32_t)(ref.h / 2) };
        char perr[256];
        perr[0] = 0;
        int rc;
        if (a.isV2) {
            psAnalyzeSink2 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
            rc = a.v2.analyze(&fr, &roi, &sk, perr, sizeof perr);
        } else {
            psAnalyzeSink sk{ nullptr, mstNum, mstTxt };
            rc = a.v1.analyze(&fr, &roi, &sk, perr, sizeof perr);
        }
        if (rc != 0) {
            fprintf(stderr, "selftest MEASURE: local run failed: %s\n", perr);
            return 1;
        }
        remote::MeasureReq q;
        q.op = rp::MOP_ANALYZER;
        q.paths = { path };
        q.analyzer = a.name;
        q.cfaType = ref.cfa;
        q.cfaPattern = ref.cfaPattern;
        q.black = effBlack(ref);
        q.white = effWhite(ref);
        q.rois.push_back({ ref.w / 4, ref.h / 4, ref.w / 2, ref.h / 2 });
        remote::MeasureResult mr;
        if (!s.measure(q, mr, err)) {
            fprintf(stderr, "selftest MEASURE: %s\n", err.c_str());
            return 1;
        }
        size_t bad2 = 0;
        if (mr.cols.size() != 1 || mr.cols[0].size() != local.size()) {
            fprintf(stderr, "selftest MEASURE: shape mismatch (%zu cols, %zu vs %zu items)\n",
                    mr.cols.size(), mr.cols.empty() ? 0 : mr.cols[0].size(), local.size());
            bad2++;
        } else {
            for (size_t i = 0; i < local.size(); i++) {
                const remote::MeasureItem& it = mr.cols[0][i];
                char b[64];
                std::string got = it.kind == 0
                    ? (snprintf(b, 64, "%.9g", it.num), std::string(b)) : it.text;
                if (it.key != local[i].first || got != local[i].second) {
                    fprintf(stderr, "selftest MEASURE mismatch: %s: '%s' vs local '%s'\n",
                            it.key.c_str(), got.c_str(), local[i].second.c_str());
                    bad2++;
                }
            }
        }
        fprintf(stderr, "selftest: MEASURE %s on the peer [loc=%s]: %s (%zu keys)\n",
                a.name.c_str(), mr.serverLoc ? "gpu" : "cpu",
                bad2 ? "FAIL" : "ok", local.size());
        bad += bad2 ? 1 : 0;
        // ...and the same again with the frame declared MOSAICED. CFA planes
        // are never mixed (docs/stats-taxonomy.md), so stats/moments must
        // bucket by plane (R/Gr/Gb/B) instead of pooling into one "ch0" - and
        // both sides must do it identically, or a remote measurement and a
        // local one of the same pixels disagree.
        if (ref.ch == 1) {
            ImageDoc mosaic = ref;                  // same pixels, declared Bayer
            // the implicit copy ALIASES ref's source (one shared_ptr, two docs)
            // - stage 1's invariant is one owner per source, and makeFrame's
            // const-cast pointer must aim at a throwaway, not the live doc
            mosaic.src = cloneSource(*mosaic.src);
            mosaic.cfa = 1;
            mosaic.cfaPattern = 0;                  // RGGB
            std::vector<std::pair<std::string, std::string>> local2;
            g_mstRows = &local2;
            psFrame fr2 = makeFrame(mosaic);
            psRect roi2{ (uint32_t)(ref.w / 4), (uint32_t)(ref.h / 4),
                         (uint32_t)(ref.w / 2), (uint32_t)(ref.h / 2) };
            perr[0] = 0;
            if (a.isV2) {
                psAnalyzeSink2 sk{ nullptr, mstNum, mstTxt, mstSer, {} };
                rc = a.v2.analyze(&fr2, &roi2, &sk, perr, sizeof perr);
            } else {
                psAnalyzeSink sk{ nullptr, mstNum, mstTxt };
                rc = a.v1.analyze(&fr2, &roi2, &sk, perr, sizeof perr);
            }
            int planeKeys = 0;
            for (const auto& kv : local2)
                if (kv.first.rfind("R.", 0) == 0 || kv.first.rfind("Gr.", 0) == 0 ||
                    kv.first.rfind("Gb.", 0) == 0 || kv.first.rfind("B.", 0) == 0)
                    planeKeys++;
            int pooledKeys = 0;
            for (const auto& kv : local2) if (kv.first.rfind("ch0.", 0) == 0) pooledKeys++;
            remote::MeasureReq q2 = q;
            q2.cfaType = 1; q2.cfaPattern = 0;
            remote::MeasureResult mr2;
            size_t bad3 = 0;
            if (rc != 0 || !s.measure(q2, mr2, err)) {
                fprintf(stderr, "selftest MEASURE cfa: %s\n", rc ? perr : err.c_str());
                bad3++;
            } else if (mr2.cols.size() != 1 || mr2.cols[0].size() != local2.size()) {
                fprintf(stderr, "selftest MEASURE cfa: shape mismatch (%zu vs %zu items)\n",
                        mr2.cols.empty() ? 0 : mr2.cols[0].size(), local2.size());
                bad3++;
            } else {
                for (size_t i = 0; i < local2.size(); i++) {
                    const remote::MeasureItem& it = mr2.cols[0][i];
                    char b2[64];
                    std::string got = it.kind == 0
                        ? (snprintf(b2, 64, "%.9g", it.num), std::string(b2)) : it.text;
                    if (it.key != local2[i].first || got != local2[i].second) {
                        fprintf(stderr, "selftest MEASURE cfa mismatch: %s: '%s' vs "
                                        "local '%s'\n", it.key.c_str(), got.c_str(),
                                local2[i].second.c_str());
                        bad3++;
                    }
                }
            }
            bool split = planeKeys >= 4 * 8 && pooledKeys == 0;
            fprintf(stderr, "selftest: MEASURE %s on a Bayer frame: %d per-plane key(s), "
                            "%d pooled ch0 key(s), local==peer: %s\n",
                    a.name.c_str(), planeKeys, pooledKeys,
                    (split && !bad3) ? "ok" : "FAIL");
            bad += (split && !bad3) ? 0 : 1;
            g_mstRows = nullptr;
        }
    }
    // Aggregates: recompute the same statistics client-side from tiles the tile
    // path already proved correct, and compare. Frame-axis files only.
    if (m.frames >= 2) {
        remote::MeasureReq q;
        q.op = rp::MOP_TEMPORAL_STATS;
        q.paths = { path };
        remote::MeasureResult mr;
        if (!s.measure(q, mr, err)) {
            fprintf(stderr, "selftest TEMPORAL: %s\n", err.c_str());
            return 1;
        }
        // reference: per-pixel sums in f64 over all frames
        size_t samples = (size_t)m.w * m.h * m.ch;
        std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
        for (int f = 0; f < m.frames; f++) {
            std::vector<float> px;
            int tw, th, tch;
            std::string dt2;
            if (!s.tile(path, f, 0, 0, m.w, m.h, 1, px, tw, th, tch, dt2, err)) {
                fprintf(stderr, "selftest TEMPORAL tile: %s\n", err.c_str());
                return 1;
            }
            for (size_t i = 0; i < samples; i++) {
                double v = px[i];
                sum[i] += v; sum2[i] += v * v;
            }
        }
        double N = m.frames, aM = 0, aM2 = 0, aV = 0;
        for (size_t i = 0; i < samples; i++) {
            double mm = sum[i] / N;
            aM += mm; aM2 += mm * mm;
            aV += std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
        }
        double refMean = aM / samples;
        double refSt = sqrt(aV / samples);
        double refFpn = sqrt(std::max(0.0, aM2 / samples - refMean * refMean));
        auto findNum = [&](const char* k) {
            for (const auto& it : mr.cols[0])
                if (it.kind == 0 && it.key == k) return it.num;
            return -1.0;
        };
        auto rel = [](double a, double b) {
            return fabs(a - b) / std::max({ fabs(a), fabs(b), 1e-12 });
        };
        size_t bad3 = 0;
        struct { const char* k; double ref; } checks[] = {
            { "mean [DN]", refMean }, { "sigma_t [DN]", refSt }, { "sigma_fpn [DN]", refFpn },
        };
        for (const auto& c : checks)
            if (rel(findNum(c.k), c.ref) > 1e-9) {
                fprintf(stderr, "selftest TEMPORAL mismatch: %s server %.12g vs ref %.12g\n",
                        c.k, findNum(c.k), c.ref);
                bad3++;
            }
        bool seriesOk = mr.series.size() == 2 &&
                        (int)mr.series[0].ys.size() == m.frames &&
                        (int)mr.series[1].ys.size() == m.frames;
        if (!seriesOk) { fprintf(stderr, "selftest TEMPORAL: bad series shape\n"); bad3++; }
        fprintf(stderr, "selftest: TEMPORAL_STATS over %d frames: %s\n",
                mr.framesUsed, bad3 ? "FAIL" : "ok");
        bad += bad3 ? 1 : 0;

        // FRAME_ROI_STATS on an off-center ROI
        remote::MeasureReq q2;
        q2.op = rp::MOP_FRAME_ROI_STATS;
        q2.paths = { path };
        q2.rois.push_back({ 3, 5, m.w / 2, m.h / 2 });
        remote::MeasureResult r2;
        if (!s.measure(q2, r2, err)) {
            fprintf(stderr, "selftest FRAME_ROI: %s\n", err.c_str());
            return 1;
        }
        size_t bad4 = 0;
        const remote::MeasureSeries* mrs = nullptr;
        for (const auto& se : r2.series) if (se.name == "roi mean") mrs = &se;
        if (!mrs || (int)mrs->ys.size() != m.frames) {
            fprintf(stderr, "selftest FRAME_ROI: bad series\n");
            bad4++;
        } else {
            for (int f = 0; f < m.frames; f++) {          // reference per frame
                std::vector<float> px;
                int tw, th, tch;
                std::string dt2;
                if (!s.tile(path, f, 3, 5, m.w / 2, m.h / 2, 1, px, tw, th, tch, dt2, err))
                    { bad4++; break; }
                double s1 = 0;
                for (float v : px) s1 += v;
                double refM = s1 / (double)px.size();
                if (rel((double)mrs->ys[f], refM) > 1e-6) {   // series is f32
                    fprintf(stderr, "selftest FRAME_ROI mismatch at frame %d: %.9g vs %.9g\n",
                            f, mrs->ys[f], refM);
                    bad4++;
                }
            }
        }
        fprintf(stderr, "selftest: FRAME_ROI_STATS over %d frames: %s\n",
                r2.framesUsed, bad4 ? "FAIL" : "ok");
        bad += bad4 ? 1 : 0;
    }
    {   // ---- the channel rule, through BOTH doors (issue #71) ---------------
        //
        // This whole harness exists to compare what the LOCAL decoder makes of
        // a file against what the PEER makes of it, so it was always the thing
        // that would catch core/main.cpp and core/serve.cpp disagreeing. It had
        // simply never been aimed at a shape where they did. main.cpp read the
        // last axis as channels only when it was exactly 3 or 4; serve.cpp read
        // it as channels whenever it was 4 or fewer. So (H,W,1) - what
        // img[:, :, None] produces, and an ordinary way to save one mono
        // capture - opened here as H frames of 1xW and there as one WxH frame.
        // Pointed at a (48,640,1) fixture before the rule was unified, this
        // printed exactly that:
        //
        //   selftest: meta 640x48 1ch u16 frames=1        <- the peer: 1 frame
        //   npy stack: ... 48 of 48 frames (1x640 1ch)    <- here: 48 frames
        //   selftest: rect 1x640@0,0 step 1 -> 1x48 1ch u16 : FAIL (47 mismatches)
        //
        // Aiming an existing test is worth more than writing a new one: this
        // one was built to catch this class of fault and had been sitting
        // unused against it.
        //
        // The sweep walks the last axis ACROSS the ceiling - 1,2,3,4 are one
        // frame of C channels, 5 is a stack - because the fault was never
        // "channels or not", it was the two sides putting the boundary in
        // different places. A rule that agrees at 3 and 4 and nowhere else is
        // the bug that was here.
        //
        // Fixtures are written here rather than read from tools/testdata for
        // two reasons: that tree is gitignored and generated, so a machine
        // without it would silently skip the check; and a fixture that lives
        // apart from the rule it pins can drift away from it.
        std::error_code cec;
        std::filesystem::path cdir =
            std::filesystem::temp_directory_path(cec) / "viewer_chanrule";
        std::filesystem::remove_all(cdir, cec);
        std::filesystem::create_directories(cdir, cec);
        std::vector<uint16_t> ramp(48 * 40 * 5);
        for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (uint16_t)(i * 11 + 5);
        struct Want { int64_t lastAxis; int frames, w, h, ch; const char* why; };
        const Want SWEEP[] = {
            { 1, 1, 40, 48, 1, "(H,W,1) is ONE mono frame" },
            { 2, 1, 40, 48, 2, "(H,W,2) is ONE two-channel frame" },
            { 3, 1, 40, 48, 3, "(H,W,3) is ONE colour frame" },
            { 4, 1, 40, 48, 4, "(H,W,4) is ONE RGBA frame" },
            { 5, 48, 5, 40, 1, "(H,W,5) is past the ceiling: a stack" },
        };
        size_t badC = 0;
        for (const Want& t : SWEEP) {
            const std::vector<int64_t> shape = { 48, 40, t.lastAxis };
            std::string p = npyWriteFile(
                (cdir / ("chan_48x40x" + std::to_string(t.lastAxis) + ".npy")).u8string(),
                "<u2", shape, ramp.data(), (size_t)48 * 40 * t.lastAxis * 2);
            remote::Meta cm;
            std::string cerr;
            if (!s.meta(p, cm, cerr)) {
                fprintf(stderr, "selftest CHANNEL META %s: %s\n",
                        npyShapeText(shape).c_str(), cerr.c_str());
                badC++;
                continue;
            }
            closeAll();
            std::string lerr = loadNpy(p);
            const ImageDoc* lref = cur();
            if (!lerr.empty() || !lref) {
                fprintf(stderr, "selftest CHANNEL local %s: %s\n",
                        npyShapeText(shape).c_str(),
                        lerr.empty() ? "nothing opened" : lerr.c_str());
                badC++;
                continue;
            }
            int lframes = (int)app.images.size();
            // The geometry first: the two doors must MAKE the same thing of the
            // file before comparing pixels is even a meaningful question.
            bool geom = lframes == cm.frames && lref->w == cm.w &&
                        lref->h == cm.h && lref->ch == cm.ch;
            bool wanted = lframes == t.frames && lref->w == t.w &&
                          lref->h == t.h && lref->ch == t.ch;
            // ...then the pixels, which is where a geometry agreement that is
            // only a coincidence would still come apart.
            size_t mism = 0;
            std::vector<float> got;
            int gw = 0, gh = 0, gch = 0;
            std::string gdt;
            if (!s.tile(p, 0, 0, 0, lref->w, lref->h, 1, got, gw, gh, gch, gdt, cerr)) {
                fprintf(stderr, "selftest CHANNEL TILE %s: %s\n",
                        npyShapeText(shape).c_str(), cerr.c_str());
                badC++;
                continue;
            }
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++)
                    for (int ci = 0; ci < gch; ci++)
                        if (got[((size_t)y * gw + x) * gch + ci] != lref->sample(x, y, ci))
                            mism++;
            bool ok = geom && wanted && mism == 0;
            fprintf(stderr, "selftest: channel rule %-13s local %d frame(s) %dx%d %dch"
                            " == peer %d frame(s) %dx%d %dch, %zu pixel mismatch(es)"
                            " : %s  (%s)\n",
                    npyShapeText(shape).c_str(), lframes, lref->w, lref->h, lref->ch,
                    cm.frames, cm.w, cm.h, cm.ch, mism, ok ? "ok" : "FAIL", t.why);
            badC += ok ? 0 : 1;
        }
        closeAll();
        std::filesystem::remove_all(cdir, cec);          // ours: ours to remove
        fprintf(stderr, "selftest: channel rule over %d shape(s), both doors: %s\n",
                (int)(sizeof SWEEP / sizeof SWEEP[0]), badC ? "FAIL" : "ok");
        bad += badC ? 1 : 0;
    }
    fprintf(stderr, "selftest: %llu bytes received from the peer\n",
            (unsigned long long)s.bytesReceived());
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- main
// Idle throttling: an immediate-mode UI normally redraws 60x per second, which
// over ssh X11 forwarding means pushing the whole window across the network
// continuously. Input wakes the loop; when nothing happens we block in
// glfwWaitEventsTimeout and draw nothing at all.
//
// The wake budget is a DEADLINE, not a frame count. ImGui's own timers run on
// io.DeltaTime - a submenu needs 300 ms of hovering, a modal backdrop 167 ms to
// fade - and three frames after an input is ~1 ms of wall clock on a fast
// machine, so those timers used to advance by almost nothing and fire only on
// the next idle timeout. Low-bandwidth mode restores the old frame-count
// behaviour for remote sessions.
static double g_wakeUntil = 0;
static uint64_t g_inputSeq = 0;      // bumped by every real input: idle can tell
                                     // a genuine event from a spurious wake-up
// Set once in main(); lets the window callbacks draw a frame themselves while the
// OS holds the thread in a modal loop (resizing / moving the window).
static std::function<void()> g_drawFrame;
static bool g_inFrame = false;
static void redrawNow() {
    if (!g_drawFrame || g_inFrame) return;   // never reenter a frame
    g_inFrame = true;
    g_drawFrame();
    g_inFrame = false;
}
static double g_lastInputAt = 0;      // for the input-latency readout
static void wakeUi(int frames = 3) {
    app.wakeFrames = std::max(app.wakeFrames, frames);
    g_inputSeq++;
    if (g_lastInputAt == 0) g_lastInputAt = nowSec();   // first of a burst
    if (!app.lowBandwidth) g_wakeUntil = nowSec() + 0.25;
}

// ---- window / taskbar icon ---------------------------------------------------
// Drawn, not loaded (core/app_icon.cpp), for one reason beyond having no asset
// file to ship: it can be redrawn while the app runs. A window looking at a
// server's pixels gets the green frame the status bar uses for a live peer, so
// two viewer buttons in the taskbar say which is which before you hover them.
// macOS is excluded: there glfwSetWindowIcon is documented to fail (the dock
// icon comes from the .app bundle, which tools/install_shortcut.sh writes).
static void applyWindowIcon(GLFWwindow* w, bool remote) {
#if !defined(__APPLE__)
    static const int SIZES[] = {16, 24, 32, 48, 64, 128};
    constexpr int N = (int)(sizeof(SIZES) / sizeof(SIZES[0]));
    // rendered once per variant and kept: the pixels must outlive the call, and
    // a reconnect must not re-rasterize six bitmaps
    static std::vector<unsigned char> pix[2][N];
    const int v = remote ? 1 : 0;
    GLFWimage imgs[N];
    for (int i = 0; i < N; i++) {
        if (pix[v][i].empty())
            pix[v][i] = app_icon::render(SIZES[i], remote ? app_icon::Remote : app_icon::Local);
        imgs[i].width = imgs[i].height = SIZES[i];
        imgs[i].pixels = pix[v][i].data();
    }
    glfwSetWindowIcon(w, N, imgs);
#else
    (void)w; (void)remote;
#endif
}

static void dropCallback(GLFWwindow*, int count, const char** paths) {
    wakeUi(4);
    for (int i = 0; i < count; i++) openPath(paths[i]);
}
static void installWakeCallbacks(GLFWwindow* win) {
    // installed BEFORE ImGui's backend, which chains to these
    glfwSetCursorPosCallback(win, [](GLFWwindow*, double, double) { wakeUi(); });
    glfwSetMouseButtonCallback(win, [](GLFWwindow*, int, int, int) { wakeUi(4); });
    glfwSetScrollCallback(win, [](GLFWwindow*, double, double) { wakeUi(4); });
    glfwSetKeyCallback(win, [](GLFWwindow*, int, int, int, int) { wakeUi(4); });
    glfwSetCharCallback(win, [](GLFWwindow*, unsigned int) { wakeUi(4); });
    glfwSetCursorEnterCallback(win, [](GLFWwindow*, int) { wakeUi(); });
    glfwSetWindowFocusCallback(win, [](GLFWwindow*, int) { wakeUi(4); });
    // these two fire from inside the OS resize/move modal loop: draw right here,
    // or the window stays frozen for as long as the user holds the edge
    glfwSetWindowSizeCallback(win, [](GLFWwindow*, int, int) { wakeUi(4); redrawNow(); });
    // ... and so does a move: with an integrated frame the OS runs the drag in
    // its own modal loop, and without this the window content freezes mid-drag
    glfwSetWindowPosCallback(win, [](GLFWwindow*, int, int) { wakeUi(2); redrawNow(); });
    glfwSetWindowRefreshCallback(win, [](GLFWwindow*) { wakeUi(2); redrawNow(); });
}

#if defined(_WIN32)
// The black window a double-click leaves behind, and why this is not solved by
// building for the WINDOWS subsystem.
//
// viewer.exe stays a CONSOLE-subsystem binary on purpose: cmd does not wait for
// a GUI-subsystem process, so `viewer --help`, `--bench N`, `--lin-selftest`
// and `--remote-selftest` would all return the prompt before printing anything,
// and their exit codes would stop reaching the shell (the VSCode tasks read
// both). The console is the cost of keeping the command line honest.
//
// It is only unwanted in one case, and that case is detectable: when nothing
// else is attached to the console, it was created FOR this process - Explorer,
// a desktop shortcut, a file association - rather than inherited from a shell,
// and closing it immediately is exactly right. Started from a terminal, that
// terminal is in the list too and this does nothing.
// (install_shortcut.ps1 also marks its shortcuts "start minimized", so the
// console does not even flash in the moment before we get here.)
static void dropOwnConsole() {
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();
}
#endif

// ---- the GL context this binary asks for, and whether the machine has one ---
//
// The hints live in one function because the probe below and the real window
// must ask for exactly the SAME context. A machine that will hand out a context
// but not a 3.0 (3.2 core on macOS) one is precisely the case the probe exists
// to catch, and it would miss it the moment the two lists drifted apart.
// glslVersion travels with them: it is one decision, not two.
static const char* applyGlContextHints() {
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    return "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    return "#version 130";
#endif
}

// GLFW's own account of the last thing that went wrong. It is only STORED here,
// never printed on arrival: the failure sites decide what is worth saying, and
// say it once. Until this existed GLFW's reason was simply discarded, so every
// failure to make a window said "window creation failed" and nothing more -
// which is exactly what the Windows and macOS runners printed, once per test,
// with no way to tell a dead runner from a broken viewer.
static std::string g_glfwLastError;
static void glfwErrorSink(int code, const char* desc) {
    g_glfwLastError = "GLFW error " + std::to_string(code) + ": " +
                      (desc && *desc ? desc : "(no description)");
}
static std::string glfwReasonOr(const char* fallback) {
    return g_glfwLastError.empty() ? std::string(fallback) : g_glfwLastError;
}

// --gl-probe: "this machine cannot make a GL context" and "an assert failed"
// are different events, and until this existed nothing could tell them apart.
// Startup opens a real 1600x1000 window unconditionally, so every selftest but
// one dies inside glfwCreateWindow on a runner that has no context, and each of
// them reads as a broken test - which is how CI stayed red for hours with
// nothing in the output that said why. Answering it once, out loud, is the fix.
//
// The probe does what those tests do in their first moments - glfwInit, the
// same hints, one window - and when it cannot, it says WHY in GLFW's own words,
// taken from the error callback rather than guessed at from the failure site.
// Exit 0: there is a context, run everything. Exit 3: there is not, and
// tools/run_selftests.sh turns that into a named skip instead of a red matrix.
// It answers for the harness today; the app can ask it the same question later.
static int glProbe() {
    // The no-GL branch of run_selftests.sh has to be provable on a machine that
    // HAS GL, or nobody watches it work until CI does - and CI is unreadable.
    if (const char* f = getenv("VIEWER_FORCE_NO_GL"); f && *f && strcmp(f, "0")) {
        printf("no OpenGL context on this machine: forced by VIEWER_FORCE_NO_GL=%s\n", f);
        return 3;
    }
    glfwSetErrorCallback(glfwErrorSink);
    if (!glfwInit()) {
        printf("no OpenGL context on this machine: %s\n",
               glfwReasonOr("glfwInit failed, no GLFW error reported").c_str());
        return 3;
    }
    applyGlContextHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // a probe must never flash a window
    // This is the call the runners die in - 0.08-0.20 s per test, 21 times a run.
    GLFWwindow* w = glfwCreateWindow(64, 64, "viewer gl probe", nullptr, nullptr);
    if (!w) {
        printf("no OpenGL context on this machine: %s\n",
               glfwReasonOr("window creation failed, no GLFW error reported").c_str());
        glfwTerminate();
        return 3;
    }
    glfwMakeContextCurrent(w);
    // Named, because "which GL did that machine actually get" is the next
    // question every time a render result differs between a runner and a desk.
    const char* rend = (const char*)glGetString(GL_RENDERER);
    const char* ver  = (const char*)glGetString(GL_VERSION);
    printf("OpenGL context OK: %s / %s\n", rend ? rend : "(no renderer)",
                                           ver  ? ver  : "(no version)");
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}

// ---- restoring and remembering the window -----------------------------------
// The three things fitSavedWindow cannot know on its own, all of them questions
// about the machine at THIS moment rather than about the saved numbers. They sit
// here because they need glfwInit() to have happened and nothing else.

// The monitors as they are right now, work areas only. GLFW promises the primary
// monitor is first, which is what makes work[0] the sane fallback.
static std::vector<WinRect> monitorWorkAreas() {
    std::vector<WinRect> out;
    int n = 0;
    GLFWmonitor** m = glfwGetMonitors(&n);
    for (int i = 0; i < n && m; i++) {
        WinRect r;
        glfwGetMonitorWorkarea(m[i], &r.x, &r.y, &r.w, &r.h);
        if (r.w > 0 && r.h > 0) out.push_back(r);
    }
    return out;
}

// The scale to turn the SAVED logical size back into screen coordinates: the
// content scale of the monitor the saved position falls on, expressed the way
// startup expresses uiScale so the two can never drift apart. Asked before the
// window exists, which is the whole reason it goes by position rather than by
// glfwGetWindowContentScale.
static float scaleAtPos(int x, int y) {
#if defined(__APPLE__)
    (void)x; (void)y;
    return 1.0f;                     // Cocoa coordinates are points; the backend does the rest
#else
    int n = 0;
    GLFWmonitor** m = glfwGetMonitors(&n);
    GLFWmonitor* hit = (n > 0 && m) ? m[0] : nullptr;
    for (int i = 0; i < n && m; i++) {
        WinRect r;
        glfwGetMonitorWorkarea(m[i], &r.x, &r.y, &r.w, &r.h);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { hit = m[i]; break; }
    }
    if (!hit) return 1.0f;
    float sx = 1, sy = 1;
    glfwGetMonitorContentScale(hit, &sx, &sy);
    (void)sy;
    return sx > 0.1f ? std::max(sx, 1.0f) : 1.0f;
#endif
}

// What to remember, read off the live window.
//
// Maximized is a STATE, not a size. A maximized window reports the maximized
// rectangle, and writing that back would mean quitting maximized and coming back
// to a window that fills the screen but no longer knows how to restore DOWN. So
// the flag is always sampled and the rectangle only while the window is not
// maximized: together they say "come up maximized, and this is what the restore
// button gives back". Fullscreen is NOT in scope - this viewer has no fullscreen
// mode to be in; window_frame's two modes are System and Integrated, and both
// are ordinary windows.
//
// The divisor is app.uiScale, fixed at startup from the monitor the window was
// born on. It does not follow the window to a differently scaled screen, and it
// should not: it is the scale the app is actually DRAWING at, so it is the scale
// the size on screen actually means.
// A geometry change that has been seen but not yet written, waiting out the
// rate limit below. It is at file scope because the IDLE path has to know the
// loop still owes prefs.txt a write: the frame body is skipped entirely while
// nothing is happening, and "nothing is happening" is the precise state a
// window sits in for the two seconds after it was resized. Without this, a
// resize followed by a kill loses the resize.
static bool g_geomWriteDue = false;

static void sampleWindowGeometry(GLFWwindow* w) {
    app.winMax = glfwGetWindowAttrib(w, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (app.winMax) return;
    int x = 0, y = 0, ww = 0, hh = 0;
    glfwGetWindowPos(w, &x, &y);
    glfwGetWindowSize(w, &ww, &hh);
    if (ww <= 0 || hh <= 0) return;
    float s = app.uiScale > 0.1f ? app.uiScale : 1.0f;
    app.winX = x;
    app.winY = y;
    app.winW = (int)lround(ww / s);
    app.winH = (int)lround(hh / s);
}

#include "selftest/util.inc"

int main(int argc, char** argv) {
#if defined(_WIN32)
    {
        // --serve is the exception: its stdin/stdout are the protocol, and it is
        // started by ssh, never by a person clicking something.
        bool serve = false;
        for (int i = 1; i < argc; i++) serve |= !strcmp(argv[i], "--serve");
        if (!serve) dropOwnConsole();
    }
#endif
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printUsage(); return 0; }
    // Serve mode is what ssh starts on the machine holding the data: no window,
    // no GL, no socket - it answers pixel requests on stdin/stdout. It must be
    // handled before anything touches GLFW.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--serve")) return rp::runServeMode();
    {   // --remote-exe must reach the selftest too, or "test the standalone peer"
        // silently tests this binary against itself
        const char* rexe = nullptr;
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--remote-exe")) rexe = argv[i + 1];
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--remote-selftest"))
                return remoteSelfTest(rexe ? rexe : argv[0], argv[i + 1]);
    }
    // Asked before prefs are read and before an autosave slot is claimed: a
    // question ABOUT the machine must not alter the machine it is asking about.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--gl-probe")) return glProbe();
    // --no-window: the startup path that makes no window and touches no GL.
    //
    // Startup created a window unconditionally, so every selftest but
    // --remote-selftest died inside glfwCreateWindow on a runner with no GL
    // context - even though most of them never draw anything. They load data,
    // call the panels' own helpers and assert on the state that comes back;
    // the window they were given was pure ceremony. This flag skips the whole
    // ceremony: no glfwInit, no window, no ImGui backends, no OpenGL symbol
    // called at any point. Everything else about startup is unchanged - prefs,
    // plugins, the ImGui context itself and the CLI are all still there,
    // because those are what the tests are made of.
    //
    // Which selftests can take it is declared ONCE, in CMakeLists.txt: the word
    // NOGL on a viewer_selftest() line both labels the test for
    // tools/run_selftests.sh and passes this flag. A test that DOES draw and is
    // marked NOGL by mistake stops at needWindow() below with its name and the
    // line to fix, rather than dying inside a backend with no device.
    bool noWindow = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--no-window")) noWindow = true;
    // Is this run SCRIPTED - a selftest, a bench, or the windowless path? Then
    // its window is a fixed 1600x1000 and nothing about it is written back.
    //
    // ba40ba8 stopped scripted runs reading the user's layout.ini for exactly
    // this reason, and tonight's benchmark that was silently measuring one panel
    // of three is what it costs when a measurement inherits geometry instead of
    // naming it. A remembered WINDOW size is the same hazard with the same
    // answer: a suite whose window is whatever the developer last dragged is a
    // suite that measures the developer. Read once, here, from argv, because
    // that is the only thing that exists this early - the layout.ini guard
    // below asks parseCli's g_browseKeys instead and is dead for it, which is a
    // mistake this must not copy.
    //
    // --window-offset and g_rbForceW stay exactly as they are: those are
    // overrides a run asked for BY NAME, which is the opposite of inheriting one.
    bool scriptedRun = noWindow;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n >= 9 && !strcmp(argv[i] + n - 9, "-selftest")) scriptedRun = true;
        if (!strncmp(argv[i], "--bench", 7)) scriptedRun = true;
    }
    // --bench N: render N frames in a hidden window and report frame times, so
    // performance can be measured (and regressions caught) instead of guessed.
    int benchFrames = 0, crashAfter = 0, cliFrame = -1;
    int winOffX = 0, winOffY = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--bench")) benchFrames = std::max(1, atoi(argv[i + 1]));
        // developer flag: verify the crash safety net actually writes a session
        if (!strcmp(argv[i], "--crash-test")) crashAfter = std::max(1, atoi(argv[i + 1]));
        // the window frame has to be decided before the window exists
        if (!strcmp(argv[i], "--frame")) cliFrame = !strcmp(argv[i + 1], "system") ? 0 : 1;
        // "Open in new window" cascade: applied right after the window exists
        if (!strcmp(argv[i], "--window-offset"))
            sscanf(argv[i + 1], "%d,%d", &winOffX, &winOffY);
    }
    // --secondary: spawned by "Open in new window". Known before loadPrefs so
    // nothing between here and there can write prefs back (see savePrefs).
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secondary")) g_secondary = true;
    // --bench-step: advance A by one frame before every benched frame. --bench
    // alone holds one image still, so every cache key stays hit and the loop
    // measures drawing only. The A/B question is the opposite one - what a
    // FRAME STEP costs when compareFollowFrame moves B too and both sides'
    // histogram / projection caches miss - and it cannot be measured without
    // actually stepping.
    bool benchStep = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-step")) benchStep = true;
    // --bench-tiles: arm two EXTRA compare slots for the bench, so the frames it
    // renders go through the tiled side-by-side path, and report the pane
    // rectangles drawCanvas actually produced. --tile-selftest proves the
    // geometry is right; this proves the canvas is the thing using it.
    bool benchTiles = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-tiles")) benchTiles = true;
    // --bench-panels: dock Projection, Histogram and Temporal side by side for
    // the bench instead of tabbing them, so all three DRAW. The default layout
    // puts them in one node, and ImGui::Begin returns false for a tab that is
    // not selected - so a bench on the default layout pays for exactly one of
    // the three, whichever happens to lead. docs/ab-stats-plan.md's A/B
    // measurement is specified with Histogram AND Projection showing, and until
    // ba40ba8 it got that by accident, from whatever layout.ini the machine
    // running the bench happened to have. That is why f424dae's numbers could
    // not be re-taken: a scripted run is now correctly isolated from the user's
    // layout, which also isolated it from the arrangement being measured. This
    // flag names the arrangement instead of inheriting it.
    bool benchPanels = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-panels")) benchPanels = true;
    app.exePath = argv[0];
    // the out-of-the-box A/B range mode, read before anything can override it:
    // --range-selftest checks it, and prefs on the machine running the test
    // would otherwise decide what "the default" is
    g_abRangeDefault = app.compareRangeMode;
    {   // Which autosave is OURS: claim the lowest free slot before anything
        // reads or writes one - two windows sharing autosave.vsession is how
        // the second one eats the first one's session (todo-open item 28/A7).
        std::string cfgDir = viewerConfigDir();
        if (!cfgDir.empty()) g_instanceSlot = claimAutosaveSlot(cfgDir, g_instanceLockPath);
        // Selftests return from main() early and a normal quit has one exit
        // path already; atexit covers them all. A CRASH skips it on purpose:
        // the stale lock left behind is what marks that autosave reclaimable.
        atexit(releaseAutosaveLock);
    }
    loadPrefs();       // before the theme is applied and before the CLI is parsed
    if (cliFrame >= 0) app.frameMode = cliFrame;   // for this run only: not saved
    // Installed before anything can fail, so the two messages below can quote
    // GLFW instead of shrugging. Storing only - it prints nothing by itself.
    GLFWwindow* win = nullptr;
    const char* glslVersion = nullptr;
    if (!noWindow) {
        glfwSetErrorCallback(glfwErrorSink);
        if (!glfwInit()) {
            fprintf(stderr, "glfwInit failed: %s\n",
                    glfwReasonOr("no GLFW error reported").c_str());
            return 1;
        }
        glslVersion = applyGlContextHints();   // the same context --gl-probe asks for
        if (benchFrames || !g_browseKeys.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        // How a Linux desktop matches a window to its launcher: without these the
        // WM_CLASS is GLFW's own "GLFW-Application", the .desktop file written by
        // tools/install_shortcut.sh cannot claim the window, and GNOME/KDE show a
        // generic icon in a second, ungrouped dock entry. Ignored elsewhere.
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "viewer");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "viewer");
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "viewer");
        // How big, and where. Restored from prefs unless this run has a reason
        // not to, and there are three:
        //   * a SCRIPTED run has to be reproducible on any machine (above);
        //   * a SECONDARY window is placed by the --window-offset cascade it was
        //     spawned with, and restoring the main window's geometry on top of
        //     that offset would undo the one thing the offset exists to do -
        //     it would also come up maximized over the parent that spawned it,
        //     which is the parent it was meant to sit beside;
        //   * nothing has been saved yet, which is every first run.
        // The size is created directly rather than set afterwards, so the window
        // never flashes at 1600x1000 on its way to the size it is supposed to be.
        int startW = WIN_DEF_W, startH = WIN_DEF_H;
        WinRect want;
        bool wantPos = false, wantMax = false;
        if (!scriptedRun && !g_secondary && !winOffX && !winOffY && app.winW > 0) {
            float s = scaleAtPos(app.winX, app.winY);
            want.x = app.winX;
            want.y = app.winY;
            want.w = (int)lround(app.winW * s);
            want.h = (int)lround(app.winH * s);
            wantPos = fitSavedWindow(want, monitorWorkAreas());
            startW = want.w;
            startH = want.h;
            wantMax = app.winMax;
        }
        win = glfwCreateWindow(startW, startH,
                               (std::string("viewer  ") + viewerVersion()).c_str(),
                               nullptr, nullptr);
        if (!win) {
            // THE line the Windows and macOS CI runners printed once per selftest,
            // with no reason attached - which is what made a machine with no GL
            // indistinguishable from 21 broken tests. GLFW knew why the whole time.
            fprintf(stderr, "window creation failed: %s\n",
                    glfwReasonOr("no GLFW error reported").c_str());
            fprintf(stderr, "  is it this machine or is it this test? "
                            "`viewer --gl-probe` answers that alone.\n");
            return 1;
        }
        applyWindowIcon(win, false);
        // The frame comes up before the first frame is drawn, so the window never
        // flashes a system title bar it is about to lose. --frame on the command
        // line wins over the preference for this run and is not written back: it is
        // the way out if a window manager makes a mess of the integrated one.
        window_frame::init(win);
        window_frame::setMode(app.frameMode ? window_frame::Integrated : window_frame::System);
        // After setMode, never before: switching the frame off changes the
        // decoration, and a window manager is entitled to move the window when
        // it does. Maximizing last, so the rectangle above stays the one the
        // restore button gives back.
        if (wantPos) glfwSetWindowPos(win, want.x, want.y);
        if (wantMax) glfwMaximizeWindow(win);
        if (winOffX || winOffY) {
            // "Open in new window" passes --window-offset 40,40: the child must not
            // land exactly on top of its parent, or nobody can tell it opened
            int wx = 0, wy = 0;
            glfwGetWindowPos(win, &wx, &wy);
            glfwSetWindowPos(win, wx + winOffX, wy + winOffY);
        }
        glfwMakeContextCurrent(win);
        glfwSwapInterval(benchFrames ? 0 : 1);   // benchmark must not be vsync-limited
        installWakeCallbacks(win);        // before ImGui's backend: it chains to these
        glfwSetDropCallback(win, dropCallback);
        g_haveWindow = true;
    }   // if (!noWindow)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // a blinking caret would force a redraw several times per second, which over
    // ssh X11 means retransmitting the window just to animate a cursor
    io.ConfigInputTextCursorBlink = false;
    // remember the panel layout between runs (user preference, not session data)
    static std::string iniPath;
    {
        std::error_code ec;
        std::filesystem::path cfg;
#if defined(_WIN32)
        if (const char* ad = getenv("APPDATA")) cfg = std::filesystem::u8path(ad);
#else
        if (const char* hm = getenv("HOME")) cfg = std::filesystem::u8path(hm) / ".config";
#endif
        // scripted runs (bench, browse-keys, --no-window) must neither read nor
        // write the user's layout - same rule the exit path applies to
        // autosave/prefs. A run with no window has no layout to remember either.
        //
        // NOT switched to scriptedRun, though the window geometry above is, and
        // it is worth writing down why rather than leaving it to look like an
        // oversight. `!g_browseKeys.empty()` is set by parseCli - several
        // hundred lines BELOW this point - so for the keys selftests it is
        // always false here, and only the APPDATA that CMakeLists.txt pins
        // keeps ctest off the developer's real layout.ini (docs/verify-ui.md
        // measured the md5 changing on a run by hand). Closing that hole makes
        // selftest.browse-dbl fail every time: its scripted double-click is
        // aimed at a panel whose place currently comes from the layout.ini an
        // EARLIER test in the same pinned home left behind. That is a real
        // defect and it is not this change's - a test that only lands its
        // clicks because of another test's leftovers needs its own geometry
        // pinned first, the way g_rbForceW already pins the width.
        if (benchFrames || !g_browseKeys.empty() || noWindow) cfg.clear();
        if (!cfg.empty()) {
            cfg /= "viewer";
            std::filesystem::create_directories(cfg, ec);
            if (!ec) {
                iniPath = (cfg / "layout.ini").u8string();
                io.IniFilename = iniPath.c_str();
                app.resetLayout = !std::filesystem::exists(cfg / "layout.ini", ec);
            }
        }
        if (iniPath.empty()) { io.IniFilename = nullptr; app.resetLayout = true; }
    }
    // One-time migration: the browser window was renamed "Remote" ->
    // "Browse###Remote". ImHashStr treats the ### part as the whole identity,
    // so a saved [Window][Remote] entry no longer matches and the panel would
    // silently lose its docked place. Rewrite the ini before ImGui reads it.
    if (!iniPath.empty()) {
        std::string txt;
        if (readWholeFile(iniPath, txt) &&
            txt.find("[Window][Remote]") != std::string::npos) {
            size_t p;
            while ((p = txt.find("[Window][Remote]")) != std::string::npos)
                txt.replace(p, strlen("[Window][Remote]"), "[Window][Browse###Remote]");
            std::ofstream mf(pathFromUtf8(iniPath), std::ios::binary);
            if (mf) mf << txt;
        }
    }

    float xs = 1, ys = 1;
    // 1.0 with no window: the scale is a property of the monitor the window
    // landed on, and there is neither. Everything downstream (uiScale, the font
    // size, ui_theme) then reads exactly what a 100% display would give.
    if (win) glfwGetWindowContentScale(win, &xs, &ys);
#if defined(__APPLE__)
    float uiScale = 1.0f;                    // Cocoa coords are points; backend handles px
    float fontScale = std::max(xs, 1.0f);    // rasterize glyphs at retina resolution
#else
    float uiScale = std::max(xs, 1.0f);
    float fontScale = uiScale;
#endif
    app.uiScale = uiScale;
    ui_theme::apply(app.themeVariant, app.themeAccent, uiScale, app.compactUi);
    std::string fontPath = jpFontPath();
    // The Japanese ranges plus U+2025 TWO DOT LEADER: stack names carry it
    // (frame_000‥023.npy - see rp::patternWithExtent), and a glyph the atlas
    // does not hold renders as a fallback '?', which is precisely the
    // uninformative character the extent exists to remove.
    static ImVector<ImWchar> fontRanges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddChar((ImWchar)0x2025);
        // ...and U+29C9 TWO JOINED SQUARES, the Files panel's share mark (§4).
        // Not every CJK font carries it: shareGlyph() checks the built atlas
        // and falls back to the word "shared" rather than showing '?'.
        b.AddChar((ImWchar)0x29C9);
        // GetGlyphRangesJapanese covers Latin-1 and the kana/kanji but NOT
        // Greek, so a table of noise figures could only write "sigma_f" where
        // it means one symbol. (Unicode has no subscript f or v, so the axis
        // letter rides alongside the symbol rather than under it.)
        b.AddChar((ImWchar)0x03C3);        // sigma
        b.AddChar((ImWchar)0x03BC);        // mu
        b.BuildRanges(&fontRanges);
    }
    ImFont* jp = fontPath.empty() ? nullptr
        : io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f * fontScale, nullptr,
                                       fontRanges.Data);
    if (!jp) {
        ImFontConfig cfg; cfg.SizePixels = 13.0f * fontScale;
        io.Fonts->AddFontDefault(&cfg);
        toast("CJK font not found - Japanese filenames may not display correctly", true);
    }
#if defined(__APPLE__)
    io.FontGlobalScale = 1.0f / fontScale;
#endif

    // The two BACKENDS - platform and renderer - are the only part of ImGui that
    // needs the window and the GL context. The context itself, the style, the
    // font atlas and every panel helper that computes rather than draws are
    // pure CPU and are set up above regardless, which is what lets a selftest
    // run with --no-window. Drawing a frame still needs these: see needWindow().
    if (win) {
        ImGui_ImplGlfw_InitForOpenGL(win, true);
        ImGui_ImplOpenGL3_Init(glslVersion);
    }

    // write the session on the way out of any crash, then let it crash normally.
    // The file is opened now, while opening files still works.
    openCrashFile();
    for (int sig : { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM }) signal(sig, crashHandler);
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);   // a dead ssh peer must not kill the viewer mid-write
#endif

    plugin_host::loadAll(
        { plugin_host::exeDir() + "/plugins", plugin_host::exeDir() + "/../plugins" },
        [](const std::string& m, bool err) {
            if (err) toast(m, true);
            fprintf(stderr, "%s\n", m.c_str());
        });

    parseCli(argc, argv);

    #include "selftest/picker.inc"

    #include "selftest/localbrowse.inc"

    #include "selftest/browse.inc"

    #include "selftest/rtemporal.inc"

    #include "selftest/batch.inc"

    #include "selftest/export.inc"

    #include "selftest/series.inc"

    #include "selftest/sweepfile.inc"

    #include "selftest/close.inc"

    #include "selftest/newwin.inc"

    #include "selftest/srcmap.inc"

    #include "selftest/media.inc"

    // The ROI table's numbers. Windowless, so it runs on every runner in the
    // matrix rather than only on the one that has a GL context - see
    // roiStatsSelftest(), and V19 below for the half of it that does draw.
    if (g_roiStatsSelftest) return roiStatsSelftest();

    // A and B on one document. Windowless for the same reason - what it asserts
    // is what the Statistics panel PRINTS, and that is CPU on both sides of the
    // window branch.
    if (g_abEqSelftest) return abEqSelftest();

    #include "selftest/verify.inc"

    #include "selftest/derive.inc"

    #include "selftest/reload.inc"

    #include "selftest/stackavg.inc"

    #include "selftest/abstats.inc"

    #include "selftest/tile.inc"

    #include "selftest/scan.inc"

    #include "selftest/range.inc"

    #include "selftest/export-tsv.inc"

    #include "selftest/frame-lin.inc"

    #include "selftest/framestats.inc"

    #include "selftest/lin.inc"

    // Everything past this point draws. Every selftest that can run windowless
    // has returned above, so reaching here with no window means either
    // --no-window on a test that needs one (say so, by name) or --no-window on
    // a normal start, which is a window the user asked for and cannot have.
    if (!g_haveWindow) {
        if (!g_browseKeys.empty()) needWindow("--browse-keys-selftest");
        else if (benchFrames)      needWindow("--bench");
        else fprintf(stderr, "--no-window: there is nothing to run without a "
                             "window - it exists for the selftests that never "
                             "draw, and none was asked for.\n");
        return 1;
    }

    #include "selftest/browse-keys.inc"

    std::vector<double> benchMs;
    int benchLeft = benchFrames;
    if (benchFrames) {                 // exercise every panel, not just the defaults
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = true;
        app.showHistogram = app.showTemporal = app.showProjection = true;
        app.showLinearity = true;      // ...which now includes the series selector
        benchMs.reserve(benchFrames);
    }
    if (benchTiles) {
        // ...except here. Every panel open squeezes the canvas to its 50 px
        // floor, where four panes are correctly REFUSED - which is the one
        // thing this run is not trying to measure.
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = false;
        app.showHistogram = app.showTemporal = app.showProjection = false;
        app.showLinearity = false;
    }
    double lastFrameEnd = nowSec();
    // The frame body lives in a callable so it can also run from the window
    // refresh/size callbacks. Win32 runs a MODAL message loop while the user
    // drags a window edge: DispatchMessage never returns until the drag ends, so
    // a main loop that only draws between events shows a frozen window for the
    // whole resize. GLFW 3.4 sets no timer there, so the repaint has to come from
    // inside the callback.
    g_drawFrame = [&]() {
        double frameBodyT0 = nowSec();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (g_injMouse.x >= 0) {            // see g_injMouse: after the backend
            io.AddMousePosEvent(g_injMouse.x, g_injMouse.y);
            static int injHeld = -1;
            if (injHeld != g_injMouseBtn) {
                if (injHeld >= 0) io.AddMouseButtonEvent(injHeld, false);
                if (g_injMouseBtn >= 0) io.AddMouseButtonEvent(g_injMouseBtn, true);
                injHeld = g_injMouseBtn;
            }
        }
        ImGui::NewFrame();
        // --browse-keys "blur": give focus to nothing, so the next keys prove
        // the main view still owns them when the Browse panel does not
        if (g_browseKeysBlur) { g_browseKeysBlur = false; ImGui::SetWindowFocus(nullptr); }
        // ImGui hands input out over several frames (ConfigInputTrickleEventQueue):
        // NewFrame stops mid-queue on a wheel-after-move or a second action on one
        // button and KEEPS the rest for later. An event-driven loop that sleeps
        // there replays stale input at the idle timeout instead - one wheel notch
        // every 250 ms, for seconds, after the user has stopped. Measured: a
        // 30-event burst took ~15 s to drain; with this line, ~7 ms.
        if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) wakeUi(1);

        // shortcuts
        pollFileDialog();
        pollReader();          // a running reader, and what it has printed so far
        // modals (RAW dialog etc.) own the keyboard: no global shortcuts underneath —
        // Ctrl+W during reinterpret would shift the replaceIdx target image
        bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // The Browse panel binds the arrows, Home/End and Ctrl+F for its own
        // row navigation, gated on ITS focus. This block runs before any panel
        // draws and knows nothing about focus, so every key both of them bind
        // fires TWICE - once here on the main view, once there on the listing.
        // One rule, computed once, applied to every key the panel claims.
        ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow;
        bool remoteFocused = nav && nav->RootWindow &&
                             rbIsBrowseWindowName(nav->RootWindow->Name);
        if (!io.WantTextInput && !popupOpen) {
            // Cmd/Ctrl+O means "open what is selected" (2026-08-03, user), the
            // way it does on macOS - not "show me a file dialog". Inside Browse
            // it rides with Enter, where the selection is. Outside it, there is
            // no selection to open, so it goes to the place where there would
            // be one rather than doing nothing at all.
            if (!remoteFocused && ImGui::IsKeyChordPressed(MODK | ImGuiKey_O))
                rbShowInstance(rbActive());
            // Open Folder keeps its chord: nothing else claims it.
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Shift | ImGuiKey_O)) openFolderDialog();
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_W)) closeCurrent();
            // the layer variants (docs/terminology.md): frame-only escape hatch
            // and whole-batch close
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Alt | ImGuiKey_W)) closeCurrent(true);
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Shift | ImGuiKey_W) && cur())
                closeBatch(cur()->batchId);
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_S)) saveSessionDialog();
            // Emacs-style navigation: time axis = C-f/C-b, stack axis = C-n/C-p,
            // sequence start/end = C-a/C-e (always Ctrl, also on macOS).
            // Ctrl+F is "find" while the Remote browser owns focus (it jumps to
            // the filter box there), so frame-stepping must yield to it.
            if (!remoteFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) gotoFrame(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) gotoFrame(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) gotoStack(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) gotoStack(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) gotoFrame(0, true, true);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) gotoFrame(0, true, false);
            // Shift+C: the comparison PAST B. It has to be a chord - the
            // plain-key block below is gated on KeyMods == None, so a Shift
            // test inside it can never be true. (It was written there first,
            // and the item was simply dead.) Shift+Backslash is NOT an alias:
            // that is the swap, further down, and 5419de1 briefly binding both
            // keys to both actions made one press do the two at once.
            if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_C))
                showCompareSlots();
        }
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_None) {   // plain keys
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) app.fitRequested = true;
            if (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad1, false))
                app.view.zoom = 1.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) app.showGrid = !app.showGrid;
            // C is an alias: ImGuiKey_Backslash follows the US scancode, which is
            // not where "\" sits on a JIS keyboard
            if (ImGui::IsKeyPressed(ImGuiKey_Backslash, false) ||
                ImGui::IsKeyPressed(ImGuiKey_C, false)) cycleCompare();
            // B is hold-to-see-B (handled in drawCanvas); Shift+B pins A as B.
            // In blink mode it is a toggle instead - "ぱちぱち" needs a tap, not
            // a held key. Space doubles as the toggle there (it only pans while
            // a drag is in progress, which a blink comparison never is).
            if (app.compareMode == App::CmpFlip && cmpB()) {
                if (ImGui::IsKeyPressed(ImGuiKey_B, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                    app.flipShowB = !app.flipShowB;
                    app.flipNext = ImGui::GetTime() + std::max(app.flipPeriod, 0.05f);
                }
            }
            // nudge the divider: small differences show up when you step it, not
            // when you sweep it with the mouse
            if (app.compareMode != App::CmpOff) {
                float& fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) fr = std::clamp(fr - 0.01f, 0.03f, 0.97f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) fr = std::clamp(fr + 0.01f, 0.03f, 0.97f);
            }
            // bare O is not bound either - same reasoning as the chord above
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) app.showHelp = !app.showHelp;
            // M = measure again: rerun the selected analyzer on the current
            // image and ROI set, and bring the Analysis panel forward. This is
            // the whole multi-image comparison loop: arrow key, M, read.
            if (ImGui::IsKeyPressed(ImGuiKey_M, false) && cur() &&
                !plugin_host::analyzers().empty())
                requestMeasure(app.anaSel);
            // P drops a pin at the pixel under the cursor - no click, no modifier
            if (ImGui::IsKeyPressed(ImGuiKey_P, false) && app.hoverX >= 0 &&
                !annBlockedOnPreview())
                addAnn(1, app.hoverX, app.hoverY, 0, 0);
            // X/Y: TOGGLE the selected ROI between its rect and full width / height.
            // Press once = row/column band, press again = restore the remembered rect.
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) toggleBand(true);
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) toggleBand(false);
            // arrows: horizontal = time (frames), vertical = stacks. These six
            // are the Browse panel's row navigation too, so they yield to it
            // exactly as Ctrl+F does - otherwise one Down both steps the browse
            // cursor AND moves the main view to the next STACK, and with
            // rangeScope = per frame that second effect REWRITES the stored
            // black/white of an image the user never navigated to.
            bool navKey = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_Home, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_End, false);
            if (remoteFocused) {
                if (navKey) g_navKeyYielded++;
            } else {
                if (navKey) g_navKeyGlobal++;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) gotoFrame(1);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) gotoFrame(-1);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) gotoStack(1);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) gotoStack(-1);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) gotoFrame(0, true, true);
                if (ImGui::IsKeyPressed(ImGuiKey_End, false)) gotoFrame(0, true, false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && app.selectedAnn >= 0)
                deleteAnn(app.selectedAnn);
            // ESC steps outward: deselect the ROI, or - nothing selected -
            // leave the comparison, seats kept. ONE dispatch (escapePressed),
            // never two handlers racing on the same press.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) escapePressed();
            if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        }
        // Shift+\ : flip which one is on top. C is NOT an alias here the way it
        // is for the plain key: Shift+C belongs to the slots (showCompareSlots,
        // above), and both bindings firing on one press swapped A and B at the
        // very moment you asked for the tiles. JIS keyboards keep the swap on
        // the status-bar button and the Files row menu.
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            ImGui::IsKeyPressed(ImGuiKey_Backslash, false))
            swapCompare();
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            ImGui::IsKeyPressed(ImGuiKey_B, false))
            pinCurrentAsB();
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            app.compareMode != App::CmpOff) {   // Shift+[ ] : coarse divider steps
            float& fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) fr = std::clamp(fr - 0.10f, 0.03f, 0.97f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) fr = std::clamp(fr + 0.10f, 0.03f, 0.97f);
        }
        // the caption / exclusion rectangles are rebuilt every frame: menus grow
        // and shrink with the plugins that are loaded
        window_frame::beginFrame(uiScale);
        drawMenuBar(win);
        // before any panel draws: the B cache slots exist only while compare does
        abStatsFrame();
        // ...and before any panel draws, because it closes documents (§3.3)
        pumpReRead();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);

        const float STATUS_H = 26 * uiScale;
        // dock space fills everything above the status bar; panels dock into it
        ImGuiID dockId = ImGui::GetID("MainDock");
        ImGui::DockSpace(dockId, ImVec2(0, ImGui::GetContentRegionAvail().y - STATUS_H),
                         ImGuiDockNodeFlags_PassthruCentralNode);
        if (app.resetLayout) {        // first run, or View > Reset layout
            app.resetLayout = false;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, ImVec2(vp->WorkSize.x, vp->WorkSize.y - STATUS_H));
            ImGuiID center = dockId, left, right, bottom;
            left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
            right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
            bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
            ImGui::DockBuilderDockWindow("Files", left);
            ImGui::DockBuilderDockWindow("Browse###Remote", left);   // tabbed with Files
            ImGui::DockBuilderDockWindow("Image View", center);
            ImGui::DockBuilderDockWindow("Inspector", right);
            // tab order = dock order; Projection leads, and the explicit
            // focus below makes it the SELECTED tab, not merely the leftmost
            if (benchPanels) {          // --bench-panels: all three at once
                ImGuiID bp1 = bottom, bp2, bp3;
                bp2 = ImGui::DockBuilderSplitNode(bp1, ImGuiDir_Right, 0.66f, nullptr, &bp1);
                bp3 = ImGui::DockBuilderSplitNode(bp2, ImGuiDir_Right, 0.50f, nullptr, &bp2);
                ImGui::DockBuilderDockWindow("Projection", bp1);
                ImGui::DockBuilderDockWindow("Histogram", bp2);
                ImGui::DockBuilderDockWindow("Temporal", bp3);
            } else {
                ImGui::DockBuilderDockWindow("Projection", bottom);
                ImGui::DockBuilderDockWindow("Histogram", bottom);
                ImGui::DockBuilderDockWindow("Temporal", bottom);
            }
            ImGui::DockBuilderFinish(dockId);
            ImGui::SetWindowFocus("Projection");
            // ROIs and Analysis stay floating (they follow the work, not the frame)
            ImGui::SetWindowPos("ROIs", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                               vp->WorkPos.y + vp->WorkSize.y * 0.08f));
            ImGui::SetWindowSize("ROIs", ImVec2(620 * uiScale, 360 * uiScale));
            ImGui::SetWindowPos("Analysis", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                                   vp->WorkPos.y + vp->WorkSize.y * 0.42f));
            ImGui::SetWindowSize("Analysis", ImVec2(560 * uiScale, 420 * uiScale));
        }

        if (app.showFiles) { if (ImGui::Begin("Files", &app.showFiles)) drawFileList(); ImGui::End(); }
        // Browse: N windows, one per instance. Instance 1 is "Browse###Remote"
        // (the singleton's id, so layouts and the dock builder keep working)
        // and only ever HIDES - View > Panels > Browse and the session's
        // "panels" line speak app.showRemote for it, exactly as before. The
        // others are "Browse N###BrowseN" and closing one destroys it (worker
        // joined first). Destruction is collected and done AFTER the loop:
        // the loop is iterating the vector it would mutate.
        {
            rbMain();                       // the primordial instance exists
            int destroyNum = 0;
            for (size_t bi = 0; bi < app.browsePanels.size(); bi++) {
                App::BrowseInstance& I = *app.browsePanels[bi];
                bool primordial = I.num == 1;
                bool& show = primordial ? app.showRemote : I.open;
                // an extra whose flag is already down (the window's X last
                // frame, or a scripted close) is destroyed here too - any
                // path that clears `open` closes the instance
                if (!show) { if (!primordial) destroyNum = I.num; continue; }
                // app.focusRemote = "bring the ACTIVE browse forward" (the
                // Temporal panel hands focus back through it); focusReq is an
                // instance asking for itself
                if (I.focusReq || (app.focusRemote && I.num == g_rbActiveNum)) {
                    ImGui::SetNextWindowFocus();
                    I.focusReq = false;
                    if (I.num == g_rbActiveNum) app.focusRemote = false;
                }
                // --browse-keys-selftest's "w<px>" action: float the panel at
                // an exact width so the toolbar's flow layout can be asserted
                // at the widths a human would drag it to. Off (0) otherwise.
                if (g_rbForceW > 0 && primordial) {
                    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
                    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8),
                                            ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(g_rbForceW,
                                                    g_rbForceH > 0 ? g_rbForceH
                                                                   : vp->WorkSize.y * 0.8f),
                                             ImGuiCond_Always);
                }
                if (!primordial)
                    ImGui::SetNextWindowSize(ImVec2(420 * uiScale, 520 * uiScale),
                                             ImGuiCond_FirstUseEver);
                // The title names the machine (10.3). Recomputed here, every
                // frame, because connecting and disconnecting change it - and
                // only the part BEFORE ### changes, so ImGui's identity (and
                // with it the docking, the layout file and the session) is
                // exactly what it was when the title was a fixed string.
                I.wtitle = rbPanelTitle(I.num, I.b.connected ? I.b.host : std::string());
                // "+" asked for this one to land beside the panel it came from.
                // Once only: after the first frame the panel is wherever the
                // user dragged it, and the layout file owns that.
                if (I.dockInto) {
                    ImGui::SetNextWindowDockID((ImGuiID)I.dockInto, ImGuiCond_Always);
                    I.dockInto = 0;
                }
                if (ImGui::Begin(I.wtitle.c_str(), &show)) drawPanelRemote(I);
                ImGui::End();
                if (!show && !primordial) destroyNum = I.num;
            }
            if (destroyNum) rbDestroyInstance(destroyNum);
        }
        if (app.showMessages) {
            ImGui::SetNextWindowSize(ImVec2(720 * uiScale, 300 * uiScale), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Messages", &app.showMessages)) drawMessagesPanel();
            ImGui::End();
        }
        if (ImGui::Begin("Image View", nullptr, ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse))
            drawCanvas(ImGui::GetContentRegionAvail());
        ImGui::End();
        if (app.showInspector) {
            if (ImGui::Begin("Inspector", &app.showInspector)) drawInspector();
            ImGui::End();
        }
        if (app.showHistogram) {
            if (ImGui::Begin("Histogram", &app.showHistogram)) drawPanelHistogram();
            ImGui::End();
        }
        if (app.showTemporal) {
            if (app.focusTemporal) {
                // Surface the tab so the browser-fired result is visible - but
                // give the keyboard straight back to the Browse panel (next
                // frame: its Begin ran earlier this one). Leaving focus here is
                // what killed the , / . preview scrub and the arrow keys right
                // after asking for server stats: the panel the user was
                // browsing in silently stopped being the panel that hears them.
                ImGui::SetNextWindowFocus();
                app.focusTemporal = false;
                app.focusRemote = true;
            }
            if (ImGui::Begin("Temporal", &app.showTemporal)) drawPanelTemporal();
            ImGui::End();
        }
        if (app.showProjection) {
            if (ImGui::Begin("Projection", &app.showProjection)) drawPanelProjection();
            ImGui::End();
        }
        if (app.showLinearity) {
            ImGui::SetNextWindowSize(ImVec2(760 * uiScale, 520 * uiScale), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Linearity", &app.showLinearity)) drawPanelLinearity();
            ImGui::End();
        }
        if (app.showRois) {   // min size: the table stays readable even if dragged small
            ImGui::SetNextWindowSizeConstraints(ImVec2(460 * uiScale, 300 * uiScale),
                                                ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin("ROIs", &app.showRois)) drawPanelRois();
            ImGui::End();
        }
        if (app.showAnalysis) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(420 * uiScale, 260 * uiScale),
                                                ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin("Analysis", &app.showAnalysis)) drawPanelAnalysis();
            ImGui::End();
        }

        // status bar
        {
            ImageDoc* im = cur();
            char st[512];
            snprintf(st, 512, "no image");
            if (im) {
                std::string hover;
                // the readout follows the pane: in a split compare, values under
                // the cursor in pane B are B's, and must say so
                ImageDoc* hv = app.hoverInB && cmpB() ? cmpB() : im;
                if (app.hoverX >= 0 && app.hoverX < hv->w && app.hoverY < hv->h) {
                    hover = "  |  ";
                    if (cmpB()) hover += app.hoverInB ? "B " : "A ";
                    hover += "(" + std::to_string(app.hoverX) + ", " + std::to_string(app.hoverY) + ")";
                    if (hv->cfa)
                        hover += std::string(" [") + CFA_CH_NAMES[cfaChannelAt(*hv, app.hoverX, app.hoverY)] + "]";
                    hover += " =";
                    for (int c = 0; c < hv->ch; c++)
                        hover += " " + fmtVal(hv->sample(app.hoverX, app.hoverY, c), hv->dtype);
                }
                char seqInfoStr[64] = "";
                if (im->seqId != 0) {
                    std::vector<int> fr = framesOfSeq(im->seqId);
                    int pos = 0;
                    for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
                    snprintf(seqInfoStr, sizeof seqInfoStr, "  frame %d/%d", pos + 1, (int)fr.size());
                }
                // zoom moved into the Image View footer (C3): magnification is
                // a property of the pixels being looked at, so it sits beside
                // them, not at the far edge of the screen
                snprintf(st, 512, "%s%s   %dx%d %dch %s%s",
                         im->name.c_str(), seqInfoStr, im->w, im->h, im->ch,
                         im->dtype.c_str(), hover.c_str());
            }
            // Remote link indicator, VSCode-style: the leftmost thing on the
            // status bar, always present while a server is involved. A toast
            // expires; "am I connected?" must be answerable at any moment.
            // One segment per Browse INSTANCE that has anything to say (a
            // phase, a connection or a failure) - with one panel open this is
            // byte-for-byte the old bar.
            for (size_t sbi = 0; sbi < app.browsePanels.size(); sbi++) {
                App::BrowseInstance& SI = *app.browsePanels[sbi];
                ImGui::PushID((int)(9100 + SI.num));
                std::string phase;
                { std::lock_guard<std::mutex> lk(SI.mtx); phase = SI.phase; }
                const App::RemoteBrowse& B = SI.b;
                if (!phase.empty()) {
                    // The spinner and the phase text both live in cells of a
                    // FIXED width. The font is proportional, so "|" and "/" are
                    // not the same width: spinning re-laid out the row eight
                    // times a second and walked the cancel button out from
                    // under the cursor - a button that dodges the pointer while
                    // you aim at it. Same for the phase, which changes length
                    // as it goes ("connecting to..." -> "listing...").
                    const char* spin[4] = { "|", "/", "-", "\\" };
                    const int fi = (int)(ImGui::GetTime() * 8) & 3;
                    float sw = 0;
                    for (const char* g : spin) sw = std::max(sw, ImGui::CalcTextSize(g).x);
                    const float pad = ImGui::GetStyle().ItemSpacing.x;
                    const float phaseW = ImGui::CalcTextSize("installing the viewer peer").x;
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    ImDrawList* sdl = ImGui::GetWindowDrawList();
                    const ImU32 amber = ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.4f, 1));
                    // centred in its cell, so it turns on the spot
                    float gw = ImGui::CalcTextSize(spin[fi]).x;
                    sdl->AddText(ImVec2(at.x + (sw - gw) * 0.5f, at.y), amber, spin[fi]);
                    sdl->PushClipRect(ImVec2(at.x + sw + pad, at.y),
                                      ImVec2(at.x + sw + pad + phaseW,
                                             at.y + ImGui::GetTextLineHeight()), true);
                    sdl->AddText(ImVec2(at.x + sw + pad, at.y), amber, phase.c_str());
                    sdl->PopClipRect();
                    ImGui::Dummy(ImVec2(sw + pad + phaseW, ImGui::GetTextLineHeight()));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", phase.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel##rb")) {
                        // the in-flight ssh still finishes - but its result is
                        // now stale on arrival instead of destructive
                        SI.b.scanGen++;
                        SI.search.gen++;
                        SI.search.running = false;
                        std::lock_guard<std::mutex> lk(SI.mtx);
                        SI.queue.clear();
                        // The click must land NOW. The worker is still inside
                        // its blocking call and cannot notice the bump until
                        // the server answers, so the spinner kept saying
                        // "scanning..." long after the cancel - which read as
                        // the button doing nothing. "cancelling" is the honest
                        // phase: the remote side IS still working, only its
                        // answer is already condemned.
                        if (!SI.phase.empty())
                            SI.phase = "cancelling (the server is still finishing)...";
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                } else if (B.connected) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s  %s",
                                       peerTag(B.host), peerLabel(B.host).c_str());
                    if (ImGui::IsItemHovered()) {
                        // published counters plus this thread's own session: no
                        // draw-path code touches the worker's Session
                        uint64_t rx = B.rxBytes + app.uiSession.bytesReceived();
                        ImGui::SetTooltip(B.host.empty()
                            ? "browsing this machine  -  %s\nthe peer runs here, over a pipe: "
                              "nothing is connected and nothing is on the network\n"
                              "peer protocol v%d, %.1f MB read\nclick to close the browse"
                            : "connected  -  %s\npeer protocol v%d, %.1f MB received\n"
                              "click to disconnect",
                                          B.dir.c_str(), B.peerVersion, rx / 1048576.0);
                    }
                    if (ImGui::IsItemClicked()) {
                        app.uiSession.stop();     // ours to stop
                        App::RbJob j;             // the worker's is the worker's
                        j.kind = App::RbDisconnect;
                        rbEnqueue(SI, std::move(j));
                        SI.b = App::RemoteBrowse{};
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                } else if (!B.err.empty()) {
                    // The reason is the useful part: a tooltip hides it behind a
                    // hover nobody thinks to try. Show the first line inline and
                    // keep the full text one click away.
                    std::string first = B.err.substr(0, B.err.find('\n'));
                    if (first.size() > 90) first = first.substr(0, 87) + "...";
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s  %s",
                                       peerTag(B.host), first.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n\n(click for details)", B.err.c_str());
                    if (ImGui::IsItemClicked()) app.showRemoteError = true;
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                }
                ImGui::PopID();
            }
            ImGui::TextUnformatted(st);
            // compare is a persistent state: a toast that expires is not enough
            if (app.compareMode != App::CmpOff) {
                ImageDoc* b = cmpB();
                ImGui::SameLine();
                // amber for the states that need attention (no B, or paused on
                // it), the compare blue for a live pair - one text source, so
                // the selftest reads the exact sentence the bar shows
                ImGui::TextColored(b ? ImVec4(0.55f, 0.78f, 1.0f, 1)
                                     : ImVec4(0.95f, 0.72f, 0.35f, 1),
                                   "   |  %s", abStatusChipText().c_str());
                // swapping A and B is a thing you reach for constantly, so it needs
                // a visible control and not only a keyboard shortcut
                if (b) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("swap A/B")) swapCompare();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Put %s on the A side and %s on the B side"
                                          "   (Shift+\\)",
                                          b->name.c_str(), cur() ? cur()->name.c_str() : "");
                    // how the STATISTICS panels show the same pair, next to the
                    // divider setting they belong with (also in View > Compare A/B)
                    static const char* ABL[3] = { "stats: auto", "stats: overlay",
                                                  "stats: side by side" };
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::CalcTextSize(ABL[2]).x +
                                            ImGui::GetFrameHeight() * 1.4f);
                    int alv = std::clamp(app.abStatsLayout, 0, 2);
                    if (ImGui::BeginCombo("##abstatslayout", ABL[alv])) {
                        for (int i = 0; i < 3; i++)
                            if (ImGui::Selectable(ABL[i], alv == i)) app.abStatsLayout = i;
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Histogram / Projection / Temporal layout for this\n"
                                          "comparison. Auto follows the image: split -> side by\n"
                                          "side, wipe / blink / difference -> overlay.");
                }
            }
            if (app.showFps) {
                // Frame time alone cannot tell you why the UI feels slow: this app
                // is event-driven, so the interesting number is how long an input
                // waited for its frame. Both are shown, plus the redraw mode,
                // because "low bandwidth" is the one setting that makes typing
                // feel like a teletype.
                ImGui::SameLine();
                ImGui::TextDisabled("   %.1f ms/frame (draw %.1f + present %.1f)   input %.1f ms (max %.0f)%s",
                                    1000.0f / ImGui::GetIO().Framerate, app.cpuMs, app.swapMs,
                                    app.inputLagMs, app.inputLagMaxMs,
                                    app.lowBandwidth ? "   [low bandwidth]" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("draw = building and rendering the frame here.\n"
                                      "present = handing it to the display (SwapBuffers):\n"
                                      "  large = the display path, not this app - a remote\n"
                                      "  X11 link ships the whole window every frame.\n"
                                      "input = event -> the frame answering it.\n"
                                      "Click to reset the maximum.");
                if (ImGui::IsItemClicked()) app.inputLagMaxMs = 0;
            }
            // A toast fades; the place to go read it afterwards has to be visible,
            // or "what did that error say?" has no answer.
            if (!app.msgLog.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("   |");
                ImGui::SameLine();
                if (app.msgUnreadErr) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.4f, 1));
                if (ImGui::SmallButton(app.msgUnreadErr ? "messages (!)" : "messages")) {
                    app.showMessages = true;
                    app.msgUnreadErr = false;
                }
                if (app.msgUnreadErr) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n\n(everything the app has said, copyable)",
                                      app.msgLog.back().text.c_str());
            }
            // Title bar and taskbar icon both carry "this window is looking at
            // another machine": the title for the window list and the tooltip,
            // the icon (green frame instead of blue) for the button itself.
            // With an integrated frame there is no system title bar to read the
            // title off - it is drawn in the menu bar instead - but it is still
            // what the taskbar button and the window list show.
            // ...and "another machine" is the same test the title uses: a local
            // browse is not a connection, so it gets no green frame either.
            const bool remoteNow = !viewingHost().empty();
            static std::string lastTitle;
            std::string title = windowTitleText();
            if (title != lastTitle) { glfwSetWindowTitle(win, title.c_str()); lastTitle = title; }
            static int lastIconVariant = -1;
            if ((int)remoteNow != lastIconVariant) {
                applyWindowIcon(win, remoteNow);
                lastIconVariant = (int)remoteNow;
            }
        }

        drawRawModal();
        drawSequenceModal();
        drawSeriesModal();
        drawDeriveModal();
        drawFolderPickModal();
        drawNpzPickModal();
        drawReaderPanel();
        drawReaderList();
        drawRemoteOpenModal();
        drawRemoteErrorWindow();
        drawHelpAbout();

        // toast
        if (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImVec2 ts = ImGui::CalcTextSize(app.toast.c_str());
            ImVec2 p(vp->WorkPos.x + std::max((vp->WorkSize.x - ts.x) * 0.5f, 12.0f * uiScale),
                     vp->WorkPos.y + vp->WorkSize.y - 60 * uiScale);
            fg->AddRectFilled(ImVec2(p.x - 12 * uiScale, p.y - 6 * uiScale),
                              ImVec2(p.x + ts.x + 12 * uiScale, p.y + ts.y + 6 * uiScale),
                              app.toastErr ? IM_COL32(90, 30, 30, 235) : IM_COL32(35, 42, 48, 235), 6);
            fg->AddText(p, IM_COL32(230, 235, 240, 255), app.toast.c_str());
        }

        ImGui::End();
        // Escape closes a menu or a context popup. ImGui does that itself only
        // with keyboard NAV enabled, and this app cannot enable it - the Browse
        // panel owns the arrow keys, and nav would take them - so a menu opened
        // by accident stayed open until it was clicked away. That is not just
        // untidy: an open menu OWNS the keyboard, and every accelerator typed
        // underneath it went nowhere (a Ctrl+W with the File menu up closed
        // nothing at all). Runs here, after every popup this frame has been
        // submitted, so the stack it reads is complete.
        //   - modals are left alone: theirs is a decision, and their own
        //     Cancel / Escape already owns the key.
        //   - an ACTIVE item gets the key first, so Escape in the "New batch"
        //     field inside Move to batch reverts the text instead of throwing
        //     the whole menu away.
        {
            bool escNow = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool anyPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                        ImGuiPopupFlags_AnyPopupLevel);
            if (escNow && !ImGui::IsAnyItemActive() && anyPopup) {
                ImGui::ClosePopupsExceptModals();
                escProbeNote("popup");                  // step 1
            } else if (escNow && io.WantTextInput) {
                // Step 2, and it is ImGui that performs it: an active text item
                // reverts its own edit. Both steps below are gated on this same
                // flag, so nothing else has run - which is exactly the claim
                // that had nowhere to be written down (E7).
                escProbeNote("textedit");
            } else if (escNow && g_escProbeFrame != ImGui::GetFrameCount()) {
                // The press reached no layer at all (a modifier held, say). It
                // still gets an entry: a chain that silently drops presses
                // cannot be read as a chain.
                escProbeNote("unclaimed");
            }
        }
        // after every panel has had its say about the mouse: drag, resize and
        // the cursor shape along the window edges
        window_frame::endFrame();
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        ImVec4 cc = ui_theme::clearColor(app.themeVariant);
        glClearColor(cc.x, cc.y, cc.z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Split the frame: our drawing versus getting it onto the screen. Over a
        // remote display the second number is the whole story (a full window per
        // frame across the link), and no amount of work on our side moves it.
        double swapT0 = nowSec();
        glfwSwapBuffers(win);
        app.swapMs = (float)((nowSec() - swapT0) * 1000.0);
        app.cpuMs = (float)((swapT0 - frameBodyT0) * 1000.0);
        if (g_lastInputAt != 0) {      // this frame answered an input: how late was it?
            float ms = (float)((nowSec() - g_lastInputAt) * 1000.0);
            app.inputLagMs = ms;
            app.inputLagMaxMs = std::max(app.inputLagMaxMs, ms);
            g_lastInputAt = 0;
        }
    };

    // Browse is open when the app comes up, standing where it stood last time
    // (2026-08-03, user). Until now the ONLY way to get a Browse panel was
    // File > Browse Folder (Local)..., which opens the OS folder dialog first -
    // so reaching the tool that replaces the OS dialog required going through
    // it. The place comes from rbRecents, which prefs.txt already carries: the
    // session's rbplace cannot serve here, because the autosave is offered by
    // a menu item and never restored on its own at startup.
    //
    // Not for scripted runs: --bench and the keys selftests drive their own
    // panels, and starting a local peer under them would change what they
    // measure. A CLI path was an explicit request to look at THAT, so the
    // panel stays where it is rather than jumping somewhere else.
    if (!benchFrames && g_browseKeys.empty() && !g_secondary && app.showRemote) {
        App::BrowseInstance& I = rbMain();
        if (!I.b.connected && I.b.host.empty() && I.b.dir == "~") {
            std::string place = app.rbRecents.empty() ? std::string() : app.rbRecents.front();
            if (place.empty()) {
#if defined(_WIN32)
                const char* h = getenv("USERPROFILE");
#else
                const char* h = getenv("HOME");
#endif
                if (h && *h) {
                    std::string hp = h;
                    std::replace(hp.begin(), hp.end(), '\\', '/');
                    place = "local://" + hp;
                }
            }
            if (!place.empty()) goToPlace(I, place);
        }
    }

    while (!glfwWindowShouldClose(win)) {
        double frameT0 = nowSec();
        // work that must keep animating even without input
        // rbBusy / mPending: a connect, a peer install or a server measurement is
        // in flight. Without these the idle path draws NOTHING while they run -
        // the window stops repainting and Windows paints it white and calls it
        // "not responding", which is exactly what a spinner is supposed to deny.
        bool busy = app.seqRunning || !app.seqQueue.empty() || app.rfPending > 0 ||
                    rbAnyBusy() || app.mPending > 0 ||
                    app.rdJob != nullptr ||          // a reader is running
                    app.anyFileDialog() ||
                    (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) ||
                    // the A/B step throttle is a DEADLINE, not an event: without
                    // a frame after it expires, B's statistics stay stale until
                    // the user happens to move the mouse (the input wake tail is
                    // 0.25 s, the throttle 0.30, and low-bandwidth has no tail)
                    frameT0 < app.abStepBusyUntil ||
                    // auto blink alternates on a timer, so it needs frames with
                    // no input at all - same case as a background load
                    (app.compareMode == App::CmpFlip && app.flipAuto);
        // --bench on a FOLDER: accept the picker the way the Load button does,
        // and do not start counting until the loaders are idle. Without the
        // first, a folder given to --bench never loads at all; without the
        // second, the numbers are the decode, not the frame.
        static bool benchWarm = false;
        if (!g_browseKeys.empty()) { glfwPollEvents(); app.wakeFrames = 1; busy = true; }
        if (benchFrames) {
            glfwPollEvents(); app.wakeFrames = 1; busy = true;
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
            benchWarm = (app.seqRunning || !app.seqQueue.empty() || seqReadyPending() ||
                         app.folderPickOpen || app.rfPending > 0 ||
                         app.pendingCompare >= 0) &&
                        nowSec() < 600.0;
            // ...and only once the folder HAS loaded can the slots be armed: at
            // setup time app.images is still empty for a folder argument
            if (benchTiles && !benchWarm && app.cmpExtra.empty() && app.images.size() >= 4) {
                selectImage(0);
                setCompareB(app.images[1].get());
                addCompareSlot(app.images[2].get());
                addCompareSlot(app.images[3].get());
                app.compareMode = App::CmpSplit;
                fprintf(stderr, "benchtiles: armed %d extra slot(s), %d image(s), engaged=%d\n",
                        (int)app.cmpExtra.size(), (int)app.images.size(),
                        tileEngaged() ? 1 : 0);
            }
        }
        bool active = app.wakeFrames > 0 || frameT0 < g_wakeUntil;
        if (active || busy) {
            // Frame pacing lives here, not in the swap: the driver may ignore
            // vsync entirely (measured ~4000 fps with SwapInterval(1)), and this
            // wait returns the instant an event arrives, so it costs no latency.
            // (~100 fps in practice: the OS wait rounds up to its timer tick.)
            double budget = app.lowBandwidth ? 1.0 / 30.0 : 1.0 / 120.0;
            if (!active) budget = 0.08;        // background progress only
            double left = budget - (frameT0 - lastFrameEnd);
            // Never sleep on a backlog: ImGui hands out one trickled event per
            // frame, so pacing would show a 100-event fling one notch at a time.
            if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) left = 0;
            if (benchFrames || left <= 0.0005) {
                glfwPollEvents();
            } else if (app.lowBandwidth && !ImGui::GetIO().WantTextInput) {
                // Remote: the budget is a real cap, so keep waiting out the rest
                // of it even when events arrive - bandwidth beats latency here,
                // and it costs latency: ~32 ms on a streamed drag against 0.4 ms
                // in the default mode. The OS wait rounds up to its timer quantum
                // (15.6 ms on Windows), so the rate lands near 20-25 fps.
                //
                // TYPING IS EXEMPT. The bandwidth argument is about redraw floods
                // from mouse drags, not about 20 characters a second - and with
                // the cap applied, typing measured 32 ms per character (49 ms
                // p90) against 1.7 ms without it. That is the difference between
                // a text field and a teletype.
                do { glfwWaitEventsTimeout(left);
                     left = budget - (nowSec() - lastFrameEnd);
                } while (left > 0.0005);
            } else {
                glfwWaitEventsTimeout(left);   // returns at once on input
            }
            if (app.wakeFrames > 0) app.wakeFrames--;
        } else {
            // Idle: block until something happens. The caret does not blink
            // (ConfigInputTextCursorBlink=false), so text fields need no heartbeat.
            // The wait also returns on messages GLFW handles without calling any
            // callback; drawing a frame for those (let alone arming the 250 ms
            // wake tail) is what turned a 1 fps idle into a 40 fps one.
            uint64_t before = g_inputSeq;
            // While a text field owns the keyboard, a wake-up with no callback is
            // not proof that nothing happened: GLFW 3.4 has no IME support, so an
            // entire Japanese composition arrives as WM_IME_* and VK_PROCESSKEY
            // messages that it swallows without calling anything (win32_window.c
            // returns early on VK_PROCESSKEY). Skipping the frame there means the
            // window does not repaint for as long as the user is composing.
            bool typing = ImGui::GetIO().WantTextInput;
            // A worker that starts between the busy check above and this wait
            // must not leave the window unpainted: re-check before skipping.
            glfwWaitEventsTimeout(rbAnyBusy() || app.mPending > 0 ? 0.05 : 1.0);
            bool working = rbAnyBusy() || app.mPending > 0 || app.rfPending > 0 || app.seqRunning;
            // Results a worker has FINISHED but no frame has integrated yet, and
            // modals waiting for a frame to open them. Without these the remote
            // scan's picker never appeared: the worker went idle (working=false),
            // no input arrived, every frame was skipped - and the pumps live
            // inside the frame, so the result that would have opened the dialog
            // sat in rbDone until the user happened to move the mouse. Twice.
            if (!working) {
                working |= rbAnyDonePending();
                { std::lock_guard<std::mutex> lk(app.rfMtx); working |= !app.rfDone.empty(); }
                { std::lock_guard<std::mutex> lk(app.mMtx);  working |= !app.mDone.empty(); }
                working |= seqReadyPending();
                working |= app.folderPickOpen || app.seqAskImage >= 0 || app.remoteDlgOpen;
                working |= app.anyFileDialog();   // pollFileDialog lives in the frame
                working |= app.rdJob != nullptr;  // and so does pollReader
                working |= app.seriesEdit.open;   // the create/edit modal wants a frame
                working |= !app.rbOpenQueue.empty() || !app.seqQueue.empty();
                working |= !app.seqRestore.empty();
                // a session's series wait for their stacks, then need one frame
                working |= !app.seriesRestore.empty() || !app.seqLevelLegacy.empty();
                working |= !app.seriesPending.empty();   // ...and so do the picker's
                working |= nowSec() < app.abStepBusyUntil;   // B refresh pending
                // a tree node whose LIST is still out: its "(listing...)" row
                // has to become children without waiting for the mouse to move
                working |= rbAnyTreePending();
                // A window that was just resized owes prefs.txt a write, and it
                // sits perfectly still until the user does something else. The
                // write lives in the frame body, so the frame has to happen.
                working |= g_geomWriteDue;
            }
            // (--crash-test counts frames, so it must not be skipped)
            if (g_inputSeq == before && !typing && !working && !crashAfter) continue;
            app.wakeFrames = std::max(app.wakeFrames, 1);   // not wakeUi: no tail
        }
        lastFrameEnd = nowSec();
        if (crashAfter && --crashAfter == 0) raise(SIGSEGV);   // --crash-test
        if (!app.pendingLayout.empty()) {   // between frames: safe point to re-dock
            ImGui::LoadIniSettingsFromMemory(app.pendingLayout.c_str(), app.pendingLayout.size());
            app.pendingLayout.clear();
        }
        if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {   // minimised: draw nothing
            glfwWaitEvents();
            continue;
        }
        pumpSequenceAndQueue();       // integrate decoded frames, chain queued stacks
        pumpRemoteFetch();            // swap in full-resolution remote frames
        pumpMeasure();                // integrate server-side measurement results
        pumpRemoteBrowse();           // connect/list results from the browse worker
        pumpRemoteOpenQueue();        // folder-scan stacks, opened one at a time
        // --compare, deferred until the files (and their background-loaded frames)
        // are actually here. B is the first doc from a DIFFERENT source file, so a
        // stack on the command line does not end up compared against itself.
        if (app.pendingCompare >= 0 && !app.seqRunning) {
            if (app.pendingCompare == App::CmpOff || app.images.size() < 2) {
                if (app.images.size() < 2 && app.pendingCompare != App::CmpOff)
                    fprintf(stderr, "--compare needs two images\n");
            } else {
                selectImage(0);
                const ImageDoc* a0 = app.images[0].get();
                for (const auto& d : app.images) {
                    // A different STACK when A is in one. "A different source
                    // file" was the old rule and it picks frame_001.npy of A's
                    // OWN stack for a folder-of-frames capture - which is the
                    // self-comparison this loop exists to avoid, and it also
                    // pins compareFollowFrame (it only steps B across stacks).
                    bool other = a0->seqId != 0
                               ? d->seqId != a0->seqId
                               : (d->src->path != a0->src->path ||
                                  d->src->npzMember != a0->src->npzMember);
                    if (other) { setCompareB(d.get()); break; }
                }
                if (!resolveB()) setCompareB(app.images[1].get());   // same file twice
                app.compareMode = app.pendingCompare;
            }
            app.pendingCompare = -1;
        }
        {   // Preferences are tiny: write them the moment they change, so a crash
            // or a kill never costs the user their setup.
            static uint64_t lastPrefs = 0;
            uint64_t h = (uint64_t)app.themeVariant * 31 + (uint64_t)app.themeAccent * 131 +
                         (app.compactUi ? 1u : 0u) * 7 + (app.dragPans ? 1u : 0u) * 13 +
                         (app.wheelZoomPlain ? 1u : 0u) * 17 + (app.fitOnSwitch ? 1u : 0u) * 19 +
                         (uint64_t)app.seqLoadMode * 23 + (app.showFps ? 1u : 0u) * 29 +
                         (app.lowBandwidth ? 1u : 0u) * 43 +
                         (uint64_t)(app.dispGamma * 10) * 37 + (app.showGrid ? 1u : 0u) * 41 + 1;
            if (lastPrefs && h != lastPrefs) { app.prefsDirty = true; savePrefs(); }
            lastPrefs = h;
        }
        {   // The window's own geometry, on the same cadence but not in the hash
            // above: a checkbox changes once and is worth a write, whereas a
            // window changes on every frame of a drag and rewriting prefs.txt
            // sixty times a second to record a resize in progress is not saving
            // a preference, it is following a mouse. So the sample is taken
            // every frame and the WRITE is rate-limited to one every two
            // seconds, which bounds what a kill can cost to the last two
            // seconds of dragging. The exit path takes whatever the last change
            // left dirty, so a clean quit always records the final rectangle.
            //
            // Scripted runs and secondary windows never get here at all - the
            // first must not inherit or leave a geometry, the second must not
            // write prefs.txt at all (savePrefs says why).
            static uint64_t lastGeom = 0;
            static double lastGeomWrite = -1e9;
            static bool geomPrimed = false;
            if (win && !g_secondary && !scriptedRun &&
                !glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
                sampleWindowGeometry(win);
                uint64_t g = (uint64_t)(uint32_t)app.winX * 2654435761ull +
                             (uint64_t)(uint32_t)app.winY * 40503ull +
                             (uint64_t)(uint32_t)app.winW * 2246822519ull +
                             (uint64_t)(uint32_t)app.winH * 3266489917ull +
                             (app.winMax ? 1u : 0u);
                double nowG = nowSec();
                // The first sample is what the restore produced, not a change
                // the user made, so priming it keeps a launch-and-quit from
                // rewriting prefs.txt for nothing. It does NOT make a refused
                // position sticky: once the window settles anywhere the
                // fallback becomes the new saved position, and the geometry
                // from the screen that went away is gone. That is the choice
                // between two imperfect things - the alternative is freezing
                // the position for the whole run, which would also throw away
                // a move the user made deliberately on that same run. This one
                // always converges on where they last put it.
                if (!geomPrimed) { geomPrimed = true; lastGeom = g; lastGeomWrite = nowG; }
                else if (g != lastGeom) { lastGeom = g; g_geomWriteDue = true; app.prefsDirty = true; }
                if (g_geomWriteDue && nowG - lastGeomWrite > 2.0) {
                    g_geomWriteDue = false;
                    lastGeomWrite = nowG;
                    savePrefs();
                }
            }
        }
        {   // Autosave on change, debounced. A hard kill cannot run any handler,
            // so the safety net has to be written while things still work.
            static uint64_t lastState = 0;
            static double dirtySince = -1, lastAutosave = 0;
            uint64_t state = app.imagesRev * 1000003ull + app.annRev * 31ull +
                             (uint64_t)(app.current + 1) + (app.linkRange ? 7ull : 0) +
                             (uint64_t)(app.view.zoom * 1000) + (uint64_t)app.view.center.x +
                             (uint64_t)app.view.center.y + (uint64_t)(app.dispGamma * 10) +
                             (uint64_t)(app.abStatsLayout * 8192 + (app.histPlane + 1) * 65536) +
                             (uint64_t)(app.showGrid + app.histLog * 2 + app.anaSel * 4 +
                                        app.projMode * 64 + app.projYMode * 256 +
                                        app.roiChannel * 1024 + app.selectedAnn * 4096) +
                             (uint64_t)(app.showFiles + app.showInspector * 2 + app.showRois * 4 +
                                        app.showAnalysis * 8 + app.showHistogram * 16 +
                                        app.showTemporal * 32 + app.showProjection * 64) * 131ull;
            double nowA = nowSec();
            static double lastSnap = -1;
            if (state != lastState) { lastState = state; dirtySince = nowA; }
            // Re-render the crash copy far more often than the autosave debounce
            // (a crash 100 ms after a change should lose nothing) but not every
            // frame: dragging a ROI bumps annRev continuously.
            if (dirtySince >= 0 && nowA - lastSnap > 0.4) {
                lastSnap = nowA;
                refreshCrashSnapshot();
            }
            bool due = dirtySince >= 0 && nowA - dirtySince > 3.0 && nowA - lastAutosave > 5.0;
            if (!benchFrames && g_browseKeys.empty() && due && !app.images.empty()) {
                dirtySince = -1;
                lastAutosave = nowA;
                autosaveSession();
            }
        }
        // through the guard, never g_drawFrame() directly: the frame body pumps
        // Win32 messages while a file dialog is open (pfd's ready() calls
        // PeekMessage/DispatchMessage), which delivers WM_PAINT/WM_SIZE to our
        // own callbacks and would start a second ImGui frame inside this one.
        // IM_ASSERT is compiled out in release, so that corrupts silently.
        // --bench-step: one frame step per benched frame, bouncing off the ends
        // so every step is a real cache miss and never a no-op clamp at the last
        // frame. Stepped BEFORE the draw, so the frame we time is the one that
        // has to recompute (and, with compare on, recompute B as well).
        if (benchStep && !benchWarm && cur() && cur()->seqId != 0) {
            static int benchDir = 1;
            std::vector<int> bf = framesOfSeq(cur()->seqId);
            int bpos = 0;
            for (int i = 0; i < (int)bf.size(); i++) if (bf[i] == app.current) bpos = i;
            if (bpos >= (int)bf.size() - 1) benchDir = -1;
            else if (bpos <= 0) benchDir = 1;
            gotoFrame(benchDir);
        }
        // --browse-keys-selftest: one scripted UI action per 8 frames, replayed
        // into the REAL input queue so the panel cannot tell it from a human.
        if (!g_browseKeys.empty()) {
            static double reproT0 = nowSec();
            static int reproIdle = 0;
            static bool reproReady = false;
            static double reproActT0 = 0;      // the ACTION phase's own clock
            // A wall clock on the action phase, the same shape as the listing
            // phase's 60 s above. It had none: waitdir/waitimg bound each WAIT
            // at 60 s, and nothing bounds the run. Measured on this build, a
            // list with three impossible waits sits for 3m03s (60 s apiece);
            // the stock list carries fifteen waits, so an abnormal run - A-16's
            // shape, where a stale layout puts every scripted click somewhere
            // else and every wait therefore expires - waits about a quarter of
            // an hour and is then killed by ctest's own 900 s TIMEOUT, which
            // says only "Timeout" and names no action. This is eight times a
            // healthy run (~35 s) and comfortably inside that TIMEOUT, so the
            // test dies by its own hand, names the action it died on, and
            // FAILS. A suite that waits is a suite nobody runs.
            const double reproActBudget = 300.0;
            ImGuiIO& rio = ImGui::GetIO();
            auto reproKey = [](const std::string& a) -> ImGuiKey {
                if (a == "down")  return ImGuiKey_DownArrow;
                if (a == "up")    return ImGuiKey_UpArrow;
                if (a == "left")  return ImGuiKey_LeftArrow;
                if (a == "right") return ImGuiKey_RightArrow;
                if (a == "enter") return ImGuiKey_Enter;
                if (a == "home")  return ImGuiKey_Home;
                if (a == "end")   return ImGuiKey_End;
                if (a == "back")  return ImGuiKey_Backspace;
                if (a == "esc")   return ImGuiKey_Escape;
                if (a == "comma")  return ImGuiKey_Comma;    // preview scrub
                if (a == "period") return ImGuiKey_Period;
                return ImGuiKey_None;
            };
            if (!reproReady) {
                if (rbKeysT().b.connected && !rbKeysT().b.entries.empty()) {
                    reproReady = true;
                    reproActT0 = glfwGetTime();  // the actions start HERE, not
                                                 // at startup: the listing has
                                                 // its own 60 s below
                    rbMain().focusReq = true;    // = clicking the panel
                    fprintf(stderr, "browsekeys: listing ready, %d entr(ies)\n",
                            (int)rbKeysT().b.entries.size());
                } else if (nowSec() - reproT0 > 60.0) {
                    fprintf(stderr, "browsekeys: no listing for %s (%s)\n",
                            g_browseKeys.c_str(), rbKeysT().b.err.c_str());
                    break;
                }
            } else if (keyAct < keyActs.size() &&
                       glfwGetTime() - reproActT0 > reproActBudget) {
                fprintf(stderr, "browsekeys: action phase gave up after %.0f s at "
                                "action %d/%d '%s' (phase %d), dir=%s imgs=%d: FAILED\n",
                        glfwGetTime() - reproActT0, (int)keyAct, (int)keyActs.size(),
                        keyActs[keyAct].c_str(), keyPhase, rbKeysT().b.dir.c_str(),
                        (int)app.images.size());
                fflush(stderr);
                break;                          // keysOk stays false: rc = 1
            } else if (keyAct < keyActs.size()) {
                // the armed never-expanded watch (see g_expNeverPath): probed
                // HERE, once per frame, whatever the current action or phase -
                // this block runs before the frame's draw, so each probe reads
                // exactly the state the previous frame rendered
                if (!g_expNeverPath.empty()) {
                    g_expNeverFrames++;
                    if (rbHas(rbKeysT().expanded, g_expNeverPath)) g_expNeverHits++;
                }
                const std::string& a = keyActs[keyAct];
                // "op:arg" actions (checks, waits); op == a when there is no ':'
                size_t kColon = a.find(':');
                std::string op = a.substr(0, kColon == std::string::npos ? a.size() : kColon);
                int arg = kColon == std::string::npos ? -1 : atoi(a.c_str() + kColon + 1);
                std::string sarg = kColon == std::string::npos ? std::string()
                                                               : a.substr(kColon + 1);
                // what the preview / double-click checks assert against: the
                // live preview doc (if any), the scrub length, and the NAMES of
                // everything open - "2 images appeared" cannot say which one
                // was the stray, the names can
                auto pvDoc = []() -> ImageDoc* {
                    for (const auto& d : app.images)
                        if (d->uid == app.previewUid && d->preview) return d.get();
                    return nullptr;
                };
                auto scrubLen = []() {   // mirrors the scrub bar: files, or frame axis
                    return app.previewFiles.size() >= 2 ? (int)app.previewFiles.size()
                                                        : app.previewFrames;
                };
                auto imgNames = []() {
                    std::string s;
                    for (const auto& d : app.images) {
                        if (!s.empty()) s += ",";
                        s += d->name;
                        if (d->preview) s += "[pv]";
                    }
                    return s;
                };
                // a chk* line prints the state it judged, ok or FAIL - and a
                // FAIL fails the run (keysCheckBad feeds the summary gate)
                auto chk = [&](bool cond, const std::string& extra = std::string()) {
                    fprintf(stderr, "browsekeys: %-18s -> imgs=%d seqs=%d pv=%d scrub=%d "
                                    "idx=%d cur=%s%s%s: %s\n",
                            a.c_str(), (int)app.images.size(), (int)app.seqs.size(),
                            pvDoc() ? 1 : 0, scrubLen(), app.previewIndex,
                            cur() ? cur()->name.c_str() : "-",
                            extra.empty() ? "" : "  ", extra.c_str(),
                            cond ? "ok" : "FAIL");
                    fflush(stderr);
                    if (!cond) keysCheckBad++;
                };
                bool hold = false;                 // waitimg: stay on this action
                static size_t klogged = (size_t)-1;
                static int escProbeAtPress = 0;
                if (keyPhase == 0 && klogged != keyAct) {
                    klogged = keyAct;
                    // one press, one layer: the count is taken BEFORE the key
                    // goes into the queue, and read back at phase 7
                    if (a == "esc") escProbeAtPress = g_escProbeN;
                    // every action names what the panel is showing when it runs,
                    // so a crash log ends on the action that caused it
                    fprintf(stderr, "browsekeys: %2d %-6s dir=%s rows=%d imgs=%d preview=%s\n",
                            (int)keyAct, a.c_str(), rbKeysT().b.dir.c_str(),
                            (int)rbKeysT().b.entries.size(), (int)app.images.size(),
                            app.previewLabel.empty() ? "-" : app.previewLabel.c_str());
                    fflush(stderr);
                }
                if (op == "dbl" || op == "dbloff" || op == "click" || op == "clickoff" ||
                    op == "ctrlclick" ||
                    op == "chevclick" || op == "starclick" || op == "pinclick" ||
                    op == "mback" || op == "mfwd") {
                    // A real gesture into the real queue, aimed at the row the
                    // keyboard cursor is on - a double-click only exists as real
                    // clicks. "click" lands 40 px right of "dbl" (same row: rows
                    // span the table) so a click that follows a double-click can
                    // never chain into a triple. "ctrlclick" holds Ctrl around
                    // the click (multi-select). "chevclick" lands on the cursor
                    // row's CHEVRON (the tree's expand hit zone) and asserts the
                    // toggle landed within two frames of the press - a toggle
                    // deferred past the double-click window would still be
                    // unexpanded here. mback / mfwd press mouse
                    // buttons 4 / 5 (ImGui 3 / 4) over the listing: the history
                    // handler is gated on the panel being hovered or focused.
                    if (keyPhase == 0) {
                        if (op == "chevclick" && rbKeysT().cursorChev.x < 0)
                            chk(false, "cursor row has no chevron");
                        // "starclick" is aimed at the bookmark star at the right
                        // end of the PATH line - not at a row. It is a real
                        // click because the point of the move is that the star
                        // is reachable there, which only a real click can show.
                        if (op == "starclick" && rbKeysT().toolbar.starCentre.x < 0)
                            chk(false, "no bookmark star on the path line");
                        // "pinclick" is aimed at the OUTERMOST row the pinned
                        // band is holding - the folder that has scrolled off
                        // the top. The whole claim being tested is that it is
                        // still a row: the click goes to real pixels, and the
                        // verb that answers is the one that row has always had.
                        if (op == "pinclick" && rbKeysT().toolbar.pinCentre.x < 0)
                            chk(false, "nothing is pinned above the listing");
                        // "dbloff:N" lands N rows BELOW the cursor row - the one
                        // aim the keyboard cannot pre-warm. Every other click
                        // action is aimed at the cursor, and the keyboard put
                        // the cursor there, which on a file row has ALREADY
                        // previewed it: click one of such a double-click finds
                        // its preview live and costs nothing. A human's first
                        // contact with a row is the mouse, and click one then
                        // pays for the whole fetch - see rbActivateRow.
                        float rowH = rbKeysT().cursorRect[1].y - rbKeysT().cursorRect[0].y;
                        g_injMouse = op == "chevclick" ? rbKeysT().cursorChev
                                   : op == "starclick" ? rbKeysT().toolbar.starCentre
                                   : op == "pinclick"  ? rbKeysT().toolbar.pinCentre
                            : ImVec2((rbKeysT().cursorRect[0].x + rbKeysT().cursorRect[1].x) * 0.5f +
                                     (op == "click" ? 40.0f : 0.0f),
                                     (rbKeysT().cursorRect[0].y + rbKeysT().cursorRect[1].y) * 0.5f +
                                     ((op == "dbloff" || op == "clickoff") ? rowH * arg : 0.0f));
                    }
                    else if (keyPhase == 1 && op == "ctrlclick")
                        rio.AddKeyEvent(ImGuiMod_Ctrl, true);
                    else if (keyPhase == 1 && op == "chevclick")
                        g_chevPreExp = (int)rbKeysT().expanded.size();
                    else if (keyPhase == 2)
                        g_injMouseBtn = op == "mback" ? 3 : op == "mfwd" ? 4 : 0;
                    else if (keyPhase == 3) g_injMouseBtn = -1;
                    else if (keyPhase == 4 && (op == "dbl" || op == "dbloff")) g_injMouseBtn = 0;
                    else if (keyPhase == 4 && op == "chevclick")
                        chk((int)rbKeysT().expanded.size() != g_chevPreExp,
                            "expanded " + std::to_string(g_chevPreExp) + " -> " +
                            std::to_string((int)rbKeysT().expanded.size()) +
                            " by two frames after the press");
                    else if (keyPhase == 5 && (op == "dbl" || op == "dbloff")) g_injMouseBtn = -1;
                    else if (keyPhase == 5 && op == "ctrlclick")
                        rio.AddKeyEvent(ImGuiMod_Ctrl, false);
                } else if (op == "altleft" || op == "altright") {
                    // the keyboard mirror of mouse back/forward
                    ImGuiKey k2 = op == "altleft" ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow;
                    if (keyPhase == 0) { rio.AddKeyEvent(ImGuiMod_Alt, true);
                                         rio.AddKeyEvent(k2, true); }
                    else if (keyPhase == 1) { rio.AddKeyEvent(k2, false);
                                              rio.AddKeyEvent(ImGuiMod_Alt, false); }
                } else if (keyPhase == 0) {
                    // ("more" - fold the drawer open - is gone with the drawer.)
                    if (a == "flat")       rbKeysT().flat = !rbKeysT().flat;
                    else if (a == "tree")  rbKeysT().tree = !rbKeysT().tree;
                    else if (a == "natorder")
                        rbKeysT().nameNatural = !rbKeysT().nameNatural;
                    else if (a == "viewreset") {
                        // an ABSOLUTE state pin: the toggles above are relative,
                        // and a segment that assumes grouped+list must not
                        // depend on the parity of every toggle before it
                        rbKeysT().flat = false;
                        rbKeysT().tree = false;
                        rbKeysT().nameNatural = true;
                        rbTreeForget(rbKeysT());
                    }
                    else if (a == "focus") rbShowInstance(rbKeysT());
                    // ---- Browse INSTANCES (item 17) ------------------------
                    else if (a == "reconnect") {
                        // reconnect the target to the test dir (post-"disc"),
                        // with a clean history - the checks below assert what
                        // the OTHER panel's navigation adds to it (nothing)
                        std::string kd = g_browseKeys;
                        std::replace(kd.begin(), kd.end(), '\\', '/');
                        while (kd.size() > 1 && kd.back() == '/') kd.pop_back();
                        rbKeysT().histBack.clear();
                        rbKeysT().histFwd.clear();
                        startRemote(rbKeysT(), "local://" + kd);
                    }
                    else if (a == "newpanel") {
                        // exactly what "+" / View > New Browse Panel do, then
                        // aim it at a DIFFERENT place than instance 1's
                        std::string kd = g_browseKeys;
                        std::replace(kd.begin(), kd.end(), '\\', '/');
                        while (kd.size() > 1 && kd.back() == '/') kd.pop_back();
                        App::BrowseInstance& NI = rbNewInstance();
                        g_rbKeysTarget = NI.num;
                        startRemote(NI, "local://" + kd + "/scanroot");
                    }
                    else if (op == "target") g_rbKeysTarget = arg;
                    else if (a == "closep") {
                        // the window's X: extras are destroyed by the frame loop
                        if (rbKeysT().num != 1) rbKeysT().open = false;
                        else app.showRemote = false;
                    }
                    else if (a == "hidep") app.showRemote = false;
                    else if (a == "showp") rbShowInstance(rbMain());
                    else if (op == "filt")
                        snprintf(rbKeysT().filter, sizeof rbKeysT().filter,
                                 "%s", sarg.c_str());
                    else if (op == "chkfilt")
                        chk(std::string(rbKeysT().filter) ==
                            (sarg == "-" ? std::string() : sarg),
                            std::string("filter=\"") + rbKeysT().filter + "\"");
                    else if (op == "chkpanels")
                        chk((int)app.browsePanels.size() == arg,
                            "panels=" + std::to_string((int)app.browsePanels.size()));
                    else if (op == "chkshown") {
                        int shown2 = 0;
                        for (auto& bp : app.browsePanels)
                            if (rbInstanceShown(*bp)) shown2++;
                        chk(shown2 == arg, "shown=" + std::to_string(shown2));
                    }
                    else if (op == "chksel") {
                        int nsel = 0;
                        for (char c2 : rbKeysT().sel) if (c2) nsel++;
                        chk(nsel == arg, "sel=" + std::to_string(nsel));
                    }
                    else if (a == "sessrt") {
                        // item 10 round-trip: write the session, tear every
                        // instance down, then replay the rbplace lines through
                        // the SAME helper the session reader uses. The checks
                        // that follow prove both instances reconnect - through
                        // their own workers - to the places they stood at.
                        std::ostringstream ss2;
                        writeSessionTo(ss2);
                        std::vector<std::pair<int, std::string>> places;
                        std::istringstream is2(ss2.str());
                        std::string l2;
                        while (std::getline(is2, l2))
                            if (l2.rfind("rbplace ", 0) == 0) {
                                std::istringstream pl(l2.substr(8));
                                int n2 = 0;
                                pl >> n2;
                                std::string u2;
                                std::getline(pl, u2);
                                while (!u2.empty() && u2.front() == ' ') u2.erase(0, 1);
                                places.emplace_back(n2, u2);
                            }
                        while (app.browsePanels.size() > 1)
                            rbDestroyInstance(app.browsePanels.back()->num);
                        rbMain().b = App::RemoteBrowse{};
                        rbTreeForget(rbMain());
                        fprintf(stderr, "browsekeys: sessrt saved %d rbplace "
                                        "line(s), all instances torn down\n",
                                (int)places.size());
                        for (auto& pr : places)
                            sessionRestoreBrowsePlace(pr.first, pr.second);
                        g_rbKeysTarget = 1;
                    }
                    // --------------------------------------------------------
                    else if (a == "rawopen") {
                        App::PendingGroup pg;
                        pg.files = { "a.raw", "b.raw" };
                        pg.isRaw = true;
                        app.seqQueue.push_back(pg);
                        rawDlg = RawDialog{};
                        rawDlg.path = "a.raw";
                        rawDlg.fileSize = 1024;
                        rawDlg.open = true;
                        rawDlg.forQueue = true;
                        rawDlg.queueCount = 1;
                    }
                    else if (a == "seqask") {          // the competing root modal
                        app.seqAskImage = 0;
                        app.seqAskFiles = { "a.npy", "b.npy" };
                        app.seqAskPattern = "?.npy";
                    }
                    else if (a == "popupcheck") {
                        if (g_popupChecks < 2)
                            g_popupCheck[g_popupChecks++] = g_rawPopupAlive ? 1 : 0;
                    }
                    else if (a == "disc") { rbKeysT().b = App::RemoteBrowse{}; rbTreeForget(rbKeysT()); }
                    else if (a == "fmenu") {
                        // A real click on the menu bar's "File". The move, the
                        // press and the release get a phase each: ImGui trickles
                        // a queued burst over several frames, and a click whose
                        // hover is still settling opens nothing.
                        if (ImGuiWindow* mb = ImGui::FindWindowByName("##MainMenuBar")) {
                            const ImGuiStyle& mst = ImGui::GetStyle();
                            g_injMouse = ImVec2(mb->Pos.x + mst.WindowPadding.x +
                                                mst.ItemSpacing.x +
                                                ImGui::CalcTextSize("File").x * 0.5f,
                                                mb->Pos.y + mb->Size.y * 0.5f);
                        }
                    } else if (a == "rctx" && rbKeysT().toolbar.rowY > 0) {
                        // ...and the other half of the defect: a right-click
                        // context popup on a listing row.
                        g_injMouse = ImVec2(rbKeysT().toolbar.rowX, rbKeysT().toolbar.rowY);
                    }
                    else if (a == "blur")  { g_browseKeysBlur = true;
                                             g_navKeyAtBlur = g_navKeyGlobal;
                                             g_navKeyYieldAtBlur = g_navKeyYielded; }
                    else if (a == "img0")  selectImage(0);
                    else if (a == "svtemp") {
                        // what the group row's "Temporal stats (server)" context
                        // item does - fired directly because a menu item cannot
                        // be scripted, the focus consequences are identical
                        if (!app.previewFiles.empty())
                            requestBrowseTemporal(app.previewHost, app.previewFiles,
                                                  "svtemp", app.previewPort);
                    }
                    else if (op == "chkfocus") {   // the TARGET instance owns the keys
                        ImGuiWindow* nw = ImGui::GetCurrentContext()->NavWindow;
                        bool f2 = nw && rbKeysT().wtitle == nw->Name;
                        chk(f2 == (arg != 0), nw ? nw->Name : "(no nav window)");
                    }
                    else if (op == "waitdir") {    // navigation lands async
                        static double waitD0 = 0;
                        std::string d3 = rbKeysT().b.dir;
                        size_t sl3 = d3.find_last_of('/');
                        bool at = (sl3 == std::string::npos ? d3 : d3.substr(sl3 + 1)) == sarg;
                        if (at) { chk(true, "dir=" + d3); waitD0 = 0; }
                        else if (waitD0 == 0) { waitD0 = nowSec(); hold = true; }
                        else if (nowSec() - waitD0 < 60.0) hold = true;
                        else { chk(false, "dir=" + d3); waitD0 = 0; }
                    }
                    else if (op == "idle") {       // burn N frames: a delay knob
                        static int idleLeft = -1;
                        if (idleLeft < 0) idleLeft = arg;
                        if (idleLeft > 0) { idleLeft--; hold = true; }
                        else idleLeft = -1;
                    }
                    else if (op == "waitimg") {    // fetches land between frames
                        static double waitT0 = 0;
                        if ((int)app.images.size() >= arg) { chk(true); waitT0 = 0; }
                        else if (waitT0 == 0) { waitT0 = nowSec(); hold = true; }
                        else if (nowSec() - waitT0 < 60.0) hold = true;
                        else { chk(false, imgNames()); waitT0 = 0; }   // timed out
                    }
                    else if (op == "chkimg")  chk((int)app.images.size() == arg, imgNames());
                    else if (op == "chkpv")                  // a live preview, LOOKED AT
                        chk(pvDoc() && cur() == pvDoc() && scrubLen() == arg);
                    else if (op == "chkidx") {               // the scrub sits at K...
                        ImageDoc* pv = pvDoc();
                        bool at = pv && app.previewIndex == arg && arg < scrubLen();
                        // ...and the slot really holds frame K, not just the label
                        if (at && app.previewFiles.size() >= 2)
                            at = pv->src->remoteUrl ==
                                 makeRemoteUrl(app.previewHost,
                                               app.previewFiles[arg], app.previewPort);
                        else if (at) at = pv->src->remoteFrame == arg;   // frame axis
                        chk(at, pv ? pv->src->remoteUrl : "-");
                    } else if (op == "chkopen") {            // registered, nothing else
                        bool okc = !pvDoc() && app.previewUid == 0 &&
                                   (int)app.seqs.size() == arg && app.previewFiles.empty();
                        for (const auto& d : app.images)     // no stray poster frame
                            if (d->preview) okc = false;
                        chk(okc, imgNames());
                    }
                    else if (op == "chkcur")  chk(pvDoc() && cur() == pvDoc());
                    else if (op == "chkcurn") chk(cur() && cur()->name == sarg);
                    else if (op == "chknames") {
                        // spec: pat[*N](+pat[*N])...; '#' matches one digit; a
                        // doc that is still a preview wears a [pv] suffix. The
                        // open images must match the multiset EXACTLY - the
                        // point is to NAME the surplus image, not to count it.
                        auto patMatch = [](const std::string& p, const std::string& s) {
                            if (p.size() != s.size()) return false;
                            for (size_t x = 0; x < p.size(); x++)
                                if (p[x] == '#' ? !isdigit((unsigned char)s[x])
                                                : p[x] != s[x]) return false;
                            return true;
                        };
                        std::vector<std::string> have;
                        for (const auto& d : app.images)
                            have.push_back(d->name + (d->preview ? "[pv]" : ""));
                        std::string missing;
                        for (size_t x = 0, y; x <= sarg.size(); x = y + 1) {
                            y = sarg.find('+', x);
                            if (y == std::string::npos) y = sarg.size();
                            if (y == x) { if (y == sarg.size()) break; continue; }
                            std::string item = sarg.substr(x, y - x);
                            int n = 1;
                            size_t st = item.rfind('*');
                            if (st != std::string::npos && st + 1 < item.size() &&
                                isdigit((unsigned char)item[st + 1])) {
                                n = atoi(item.c_str() + st + 1);
                                item.erase(st);
                            }
                            for (int q = 0; q < n; q++) {
                                bool found = false;
                                for (size_t h = 0; h < have.size(); h++)
                                    if (patMatch(item, have[h])) {
                                        have.erase(have.begin() + h);
                                        found = true;
                                        break;
                                    }
                                if (!found)
                                    missing += (missing.empty() ? "" : ",") + item;
                            }
                            if (y == sarg.size()) break;
                        }
                        std::string extra;
                        for (const auto& h : have)
                            extra += (extra.empty() ? "" : ",") + h;
                        chk(missing.empty() && extra.empty(),
                            "missing=[" + missing + "] surplus=[" + extra + "]");
                    }
                    else if (op == "chkdir") {     // the leaf of the browsed dir
                        std::string d3 = rbKeysT().b.dir;
                        size_t sl3 = d3.find_last_of('/');
                        chk((sl3 == std::string::npos ? d3 : d3.substr(sl3 + 1)) == sarg,
                            "dir=" + d3);
                    }
                    else if (op == "chkcursor") chk(rbKeysT().cursor == arg,
                                                    "cursor=" + std::to_string(rbKeysT().cursor));
                    else if (op == "chkatrow") chk(rbKeysT().cursorName == sarg,
                                                   "cursor row=" + rbKeysT().cursorName);
                    else if (op == "chkback") chk((int)rbKeysT().histBack.size() == arg,
                                                  "back=" + std::to_string((int)rbKeysT().histBack.size()) +
                                                  " fwd=" + std::to_string((int)rbKeysT().histFwd.size()));
                    else if (op == "chkfwd")  chk((int)rbKeysT().histFwd.size() == arg,
                                                  "back=" + std::to_string((int)rbKeysT().histBack.size()) +
                                                  " fwd=" + std::to_string((int)rbKeysT().histFwd.size()));
                    else if (op == "chkexp")  chk((int)rbKeysT().expanded.size() == arg);
                    // The band, read as text: every level the listing has
                    // scrolled past, outermost first, ";"-terminated - "-" for
                    // "nothing is pinned". This is the whole feature stated the
                    // way a human would read it off the screen, and it is a
                    // FRAME check rather than an arithmetic one on purpose: the
                    // rows have to have been laid out for the string to fill in.
                    else if (op == "chkpin")
                        chk(rbKeysT().toolbar.pinnedNames == (sarg == "-" ? std::string() : sarg),
                            "pinned=\"" + rbKeysT().toolbar.pinnedNames + "\"");
                    // ...and the one chkexp cannot say: is the row the cursor is
                    // ON expanded? chkexp counts the whole panel, so "this row
                    // toggled" was only expressible when nothing else in the
                    // panel was open - and the keyboard section far above leaves
                    // one open with Right, so it was not expressible at all.
                    // cursorFull is the exact key `expanded` is written in, at
                    // any depth, so this asks the question directly.
                    else if (op == "chkrowexp")
                        chk(rbHas(rbKeysT().expanded, rbKeysT().cursorFull) == (arg != 0),
                            "row \"" + rbKeysT().cursorFull + "\" expanded=" +
                            std::to_string(rbHas(rbKeysT().expanded, rbKeysT().cursorFull) ? 1 : 0) +
                            " of " + std::to_string((int)rbKeysT().expanded.size()) +
                            " expanded in the panel");
                    else if (a == "imgmark") g_rbImgMark = (int)app.images.size();
                    else if (a == "chkimgmark")
                        chk((int)app.images.size() == g_rbImgMark,
                            "opened " + std::to_string((int)app.images.size() - g_rbImgMark) +
                            " document(s) since the mark (" +
                            std::to_string(g_rbImgMark) + ")");
                    else if (a == "exparm") {
                        // arm the per-frame watch on the CURSOR row's path -
                        // the row the next gesture will be aimed at
                        g_expNeverPath = rbKeysT().cursorFull;
                        g_expNeverHits = g_expNeverFrames = 0;
                    }
                    else if (op == "chkexpn") {
                        chk(g_expNeverHits == arg && g_expNeverFrames > 0,
                            g_expNeverPath + " expanded on " +
                            std::to_string(g_expNeverHits) + "/" +
                            std::to_string(g_expNeverFrames) + " watched frame(s)");
                        g_expNeverPath.clear();
                    }
                    // ---- the "more" drawer is gone, and each thing it held is
                    // asserted in its NEW home (docs/browse-topbar-design.md
                    // 10.3). These are the only checks that can tell "moved"
                    // from "deleted".
                    else if (a == "chktitle") {
                        // The panel is named after the machine it stands on -
                        // the fact that used to be invisible until you unfolded
                        // the drawer, so that two panels were pixel-identical.
                        // BOTH directions, because the wrong one is a lie: a
                        // local panel must not claim ssh, and a remote one must
                        // not keep the plain name. And the ### id is untouched
                        // either way, or saved layouts stop docking the panel.
                        const App::BrowseInstance& T = rbKeysT();
                        bool ok = rbPanelTitle(1, "") == "Browse###Remote" &&
                                  rbPanelTitle(1, "trc2") == "Browse [ssh: trc2]###Remote" &&
                                  rbPanelTitle(2, "") == "Browse 2###Browse2" &&
                                  rbPanelTitle(2, "trc2") == "Browse 2 [ssh: trc2]###Browse2" &&
                                  // ...and the LIVE title of a local panel says
                                  // nothing about ssh (local:// is not remote)
                                  T.b.connected && T.b.host.empty() &&
                                  T.wtitle == rbPanelTitle(T.num, "") &&
                                  T.wtitle.find("[ssh:") == std::string::npos;
                        chk(ok, "live=\"" + T.wtitle + "\" ssh form=\"" +
                                rbPanelTitle(1, "trc2") + "\"");
                    }
                    else if (op == "chkstat") {
                        // The bottom status line, which is where every fact the
                        // drawer and the two orange bands used to carry now
                        // lives. Counts BOTH directions: the numbers it prints
                        // are the numbers the panel holds, and the "selected"
                        // clause is present exactly when a selection exists.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        int realSel = 0;
                        for (char c : rbKeysT().sel) if (c) realSel++;
                        char want[64], wantSel[64];
                        if (g.statusShown != g.statusTotal)
                            snprintf(want, sizeof want, "%d of %d items",
                                     g.statusShown, g.statusTotal);
                        else
                            snprintf(want, sizeof want, "%d item%s", g.statusTotal,
                                     g.statusTotal == 1 ? "" : "s");
                        snprintf(wantSel, sizeof wantSel, "%d of %d selected",
                                 g.statusSel, g.statusTotal);
                        bool says = g.statusFull.find(" selected") != std::string::npos;
                        bool ok = g.statusSel == arg && realSel == arg &&
                                  g.statusTotal > 0 &&
                                  g.statusFull.find(want) != std::string::npos &&
                                  says == (arg > 0) &&
                                  (arg == 0 ||
                                   g.statusFull.find(wantSel) != std::string::npos) &&
                                  // The host is NOT on this line: the panel
                                  // title names the machine, and the bottom row
                                  // says only what nothing else on screen says.
                                  // Kept as an assert pointing the other way
                                  // rather than deleted with the text it used
                                  // to check, or nothing would notice the host
                                  // coming back.
                                  g.statusFull.find(peerTag(rbKeysT().b.host)) ==
                                      std::string::npos &&
                                  // it is one line
                                  g.statusText.find('\n') == std::string::npos;
                        chk(ok, "\"" + g.statusFull + "\"");
                    }
                    else if (op == "chkframes") {
                        // What the selected ROWS stand for. One grouped row is
                        // one row and N frames, and the row count alone is the
                        // wrong answer to "am I about to open 24 files or 480".
                        // Both directions: the clause is absent when the two
                        // numbers are equal, or every ordinary selection would
                        // carry a redundant second count.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        char want[48];
                        snprintf(want, sizeof want, "%d frames", arg);
                        bool says = g.statusFull.find(" frames") != std::string::npos;
                        chk(arg == 0 ? !says
                                     : (says && g.statusFull.find(want) != std::string::npos),
                            "\"" + g.statusFull + "\"");
                    }
                    else if (a == "starmark") g_rbStar0 = rbKeysT().toolbar.starLit;
                    else if (op == "chkstar") {
                        // The star is a STATE before it is a verb: lit means
                        // this place is in the bookmark list. Asserted as a flip
                        // from the mark, because a scripted run inherits the
                        // user's real bookmarks and must not assume the test
                        // folder is absent from them.
                        const App::BrowseInstance& T = rbKeysT();
                        std::string u = placeUrl(T.b.host, T.b.port, T.b.dir);
                        bool listed = std::find(app.rbBookmarks.begin(),
                                                app.rbBookmarks.end(), u) !=
                                      app.rbBookmarks.end();
                        int wantLit = arg ? !g_rbStar0 : g_rbStar0;
                        bool ok = g_rbStar0 >= 0 &&
                                  T.toolbar.starLit == wantLit &&
                                  (T.toolbar.starLit != 0) == listed &&
                                  T.toolbar.starCentre.x > 0;   // and it is ON the path line
                        chk(ok, "star lit=" + std::to_string(T.toolbar.starLit) +
                                " bookmarked=" + std::to_string(listed ? 1 : 0) +
                                " " + u);
                    }
                    else if (a == "marklist") g_rbListTopY0 = rbKeysT().toolbar.listTopY;
                    else if (a == "seterr")
                        rbKeysT().b.err = "listing failed: no such file or directory";
                    else if (a == "clrerr") rbKeysT().b.err.clear();
                    else if (op == "chkerr") {
                        // A failure speaks on the status line and NOWHERE else.
                        // The old shape was a wrapped orange band above the
                        // listing: it said the failure in the one place the
                        // failure did not happen, and it pushed every file row
                        // down by one to three lines on its way in and out.
                        // listTopY is the proof - the listing does not move.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        bool inLine = g.statusFull.find("failed: ") != std::string::npos;
                        float moved = g.listTopY - g_rbListTopY0;
                        if (moved < 0) moved = -moved;
                        chk(inLine == (arg != 0) && moved <= 1.0f,
                            "list top " + std::to_string((int)g_rbListTopY0) + " -> " +
                            std::to_string((int)g.listTopY) + "  line=\"" +
                            g.statusFull + "\"");
                    }
                    else if (op == "setpv") {
                        if (g_rbPeerV0 < 0) g_rbPeerV0 = rbKeysT().b.peerVersion;
                        rbKeysT().b.peerVersion = arg;
                    }
                    else if (a == "pvback") {
                        if (g_rbPeerV0 >= 0) rbKeysT().b.peerVersion = g_rbPeerV0;
                        g_rbPeerV0 = -1;
                    }
                    else if (op == "chkproto") {
                        // The protocol-mismatch notice was the OTHER orange row
                        // above the listing. Same two claims as chkerr: it is on
                        // the status line, and it does not take a row from the
                        // files. The sentence names BOTH versions - a warning
                        // that says only the peer's leaves the reader guessing
                        // what it is being compared against.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        char peer[32], mine[32];
                        snprintf(peer, sizeof peer, "protocol %d", rbKeysT().b.peerVersion);
                        snprintf(mine, sizeof mine, "speaks %d", (int)rp::VERSION);
                        bool said = g.statusFull.find("protocol") != std::string::npos;
                        bool both = g.statusFull.find(peer) != std::string::npos &&
                                    g.statusFull.find(mine) != std::string::npos;
                        float moved = g.listTopY - g_rbListTopY0;
                        if (moved < 0) moved = -moved;
                        chk(said == (arg != 0) && (arg == 0 || both) && moved <= 1.0f &&
                            // ...and the agreeing case is SILENT, which is the
                            // rule the whole redesign rests on
                            rbProtocolNote((int)rp::VERSION).empty(),
                            "peer v" + std::to_string(rbKeysT().b.peerVersion) +
                            " list top " + std::to_string((int)g.listTopY) +
                            "  line=\"" + g.statusFull + "\"");
                    }
                    else if (a[0] == 'w')  g_rbForceW = (float)atof(a.c_str() + 1);
                    // "h<px>" - a digit is required, or "home" and "hidep" would
                    // both be read as a height. It only bites while w<px> is on,
                    // which is where every check that uses it lives.
                    else if (a[0] == 'h' && a.size() > 1 && a[1] >= '0' && a[1] <= '9')
                        g_rbForceH = (float)atof(a.c_str() + 1);
                    else if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), true);
                } else if (keyPhase == 1) {
                    if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), false);
                } else if (keyPhase == 2) {
                    if (a == "fmenu") g_injMouseBtn = 0;
                    if (a == "rctx")  g_injMouseBtn = 1;
                } else if (keyPhase == 4) {
                    g_injMouseBtn = -1;
                } else if (keyPhase == 7 && (a == "fmenu" || a == "rctx" || a == "esc")) {
                    // Escape must close a menu. ImGui only does that with
                    // keyboard nav on, which this app cannot have, so the File
                    // menu stayed up - and an open menu owns the keyboard, which
                    // is how a Ctrl+W underneath it went nowhere. "fmenu" and
                    // "rctx" assert the click landed (without that, "esc"
                    // proves nothing); "esc" asserts the popup is gone.
                    bool up = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                          ImGuiPopupFlags_AnyPopupLevel);
                    bool want = (a != "esc");
                    if (up != want) keysCheckBad++;
                    fprintf(stderr, "browsekeys: after %-5s a popup is %s "
                                    "(cursor %.0f,%.0f): %s\n",
                            a.c_str(), up ? "open" : "closed",
                            rio.MousePos.x, rio.MousePos.y,
                            up == want ? "ok" : "FAIL");
                    fflush(stderr);
                    if (a == "esc") {
                        // ...and it was the POPUP layer that took it, and only
                        // that layer. "the popup went away" is compatible with
                        // the same press also reaching escapePressed() and
                        // dropping an ROI or leaving a comparison underneath -
                        // the exact failure the one-step-outward rule forbids,
                        // and the one thing this test could not see before
                        // g_escProbe existed (docs/verify-ui.md E7).
                        int took = g_escProbeN - escProbeAtPress;
                        std::string chain = g_escProbe.size() > 60
                            ? g_escProbe.substr(g_escProbe.size() - 60) : g_escProbe;
                        bool escOk = took == 1 &&
                                     g_escProbe.size() >= 6 &&
                                     g_escProbe.compare(g_escProbe.size() - 6, 6, "popup;") == 0;
                        if (!escOk) keysCheckBad++;
                        fprintf(stderr, "browsekeys: esc consumed by %d layer(s), "
                                        "chain ...'%s': %s\n",
                                took, chain.c_str(), escOk ? "ok" : "FAIL");
                        fflush(stderr);
                    }
                } else if (keyPhase == 7 && a == "disc") {
                    // View > Panels > Browse lands here with nothing open, and
                    // it must not be a remote-only dead end: the panel browses
                    // this disk too, so it has to say so without the File menu.
                    bool hasLocal = rbKeysT().toolbar.emptyLocalBtn != 0;
                    if (!hasLocal) keysCheckBad++;
                    fprintf(stderr, "browsekeys: empty state offers a local browse: %s\n",
                            hasLocal ? "ok" : "FAIL");
                    fflush(stderr);
                } else if (keyPhase == 7 && g_rbForceW > 0) {
                    // The toolbar's contract at ANY width: the filter box and
                    // the item that ENDS the row are inside the panel, and the
                    // filter is wide enough to hold a glob. Both used to be
                    // submitted past the right edge at the default docked width.
                    //
                    // SPEC CHANGE (drawer removal): the row's last item was the
                    // "more" fold button (g.moreR); it is the "..." panel menu
                    // now (g.menuR). The button being asserted changed because
                    // the button changed - the contract, "whatever ends the
                    // toolbar row is reachable at every width", did not, and it
                    // is still checked at the same seven widths below.
                    const RbToolbarGeom& g = rbKeysT().toolbar;
                    float slack = 1.0f;             // one pixel of rounding
                    bool inside = g.filterR <= g.x1 + slack && g.filterL >= g.x0 - slack &&
                                  g.menuR <= g.x1 + slack;
                    bool usable = g.filterR - g.filterL >= ImGui::GetFontSize() * 3.5f;
                    // ...and the bottom status line, at the same widths: it
                    // elides middle-out into the room left beside the one verb
                    // it carries, and it NEVER takes a second row (a status line
                    // that wraps is one more thing moving the listing).
                    bool statusOk = g.statusTextW <= g.statusAvailW + slack &&
                                    g.statusAvailW > 0;
                    // ...and the "modified" column: shown means it FITS. It is
                    // allowed to be absent (a panel too narrow to afford it) and
                    // it is allowed to be short - it is not allowed to print
                    // three characters of a sixteen-character stamp.
                    bool dateOk = g.dateTextW <= 0 || g.dateTextW <= g.dateCellW + slack;
                    if (!inside || !usable || !dateOk || !statusOk) keysCheckBad++;
                    fprintf(stderr, "browsekeys: panel w=%.0f content=[%.0f,%.0f] "
                                    "filter=[%.0f,%.0f] menu_r=%.0f date=%.0f/%.0f "
                                    "status=%.0f/%.0f: %s\n",
                            g_rbForceW, g.x0, g.x1, g.filterL, g.filterR, g.menuR,
                            g.dateTextW, g.dateCellW, g.statusTextW, g.statusAvailW,
                            (inside && usable && dateOk && statusOk) ? "ok" : "FAIL");
                    fflush(stderr);
                }
                if (!hold && ++keyPhase >= 8) { keyPhase = 0; keyAct++; }
            } else if (++reproIdle > 20) {
                // KEY ROUTING. The Browse panel binds Down/Up/Left/Right/Home/
                // End for its rows and the main loop's shortcut table binds the
                // same six for stack and frame navigation. Neither claims key
                // ownership, so before the focus gate one Down BOTH stepped the
                // browse cursor and ran gotoStack(1) on the main view - and
                // with rangeScope = per frame that rewrote the stored
                // black/white of an image the user never navigated to.
                // Both of the summary claims below rest on evidence one named
                // action collects ("blur", "popupcheck"). A --browse-keys list
                // that does not contain that action is not making the claim, so
                // it must not be JUDGED on it - without this, every overridden
                // list failed on two assertions about actions it never ran, and
                // the override was useful only for reading logs. Asked for by
                // the list rather than inferred from the counters: dropping
                // "blur" from the canned list then drops the claim visibly,
                // instead of turning it green by leaving no evidence.
                auto listHas = [&](const char* op) {
                    for (const auto& s2 : keyActs) if (s2 == op) return true;
                    return false;
                };
                int afterBlur = g_navKeyAtBlur < 0 ? -1 : g_navKeyGlobal - g_navKeyAtBlur;
                bool routeAsked = listHas("blur");
                bool routeOk = !routeAsked ||
                               (g_navKeyAtBlur == 0 && g_navKeyYieldAtBlur >= 6 &&
                                afterBlur >= 4);
                fprintf(stderr, "browsekeys: key routing: with the panel focused the "
                                "main view ran %d nav key(s) and stood down for %d; "
                                "after blur it ran %d more: %s\n",
                        g_navKeyAtBlur, g_navKeyYieldAtBlur, afterBlur,
                        !routeAsked ? "not asked for" : routeOk ? "ok" : "FAIL");
                // POPUP COLLISION. ImGui's OpenPopupEx closes whatever sits at
                // the current stack LEVEL when a different id opens there. The
                // RAW dialog used to be one-shot (its flag consumed the frame it
                // opened), so any competing root-level OpenPopup destroyed it for
                // good - and with rawDlg.forQueue set, the only code that clears
                // that flag lives inside the now-unreachable modal, so every
                // later Open Folder sat at "queued" until restart.
                bool popAsked = listHas("popupcheck");
                bool popOk = !popAsked ||
                             (g_popupCheck[0] == 1 && g_popupCheck[1] == 1 &&
                              (!rawDlg.forQueue || rawDlg.open));
                fprintf(stderr, "browsekeys: root popup collision: RAW dialog open "
                                "before the competing modal=%d, after=%d; forQueue "
                                "implies a live dialog=%d: %s\n",
                        g_popupCheck[0], g_popupCheck[1],
                        (!rawDlg.forQueue || rawDlg.open) ? 1 : 0,
                        !popAsked ? "not asked for" : popOk ? "ok" : "FAIL");
                keysOk = routeOk && popOk && keysCheckBad == 0;
                fprintf(stderr, "browsekeys: %d action(s) through real frames, "
                                "no crash, %d panel check(s) failed: %s\n",
                        (int)keyActs.size(), keysCheckBad,
                        keysOk ? "ok" : "FAILED");
                fflush(stderr);
                break;
            }
        }
        redrawNow();
        if (benchFrames && !benchWarm) {
            glFinish();               // include GPU work in the measurement
            benchMs.push_back((nowSec() - frameT0) * 1000.0);
            if (--benchLeft <= 0) break;
        }
    }
    if (benchFrames && !benchMs.empty()) {
        std::vector<double> s(benchMs.begin() + std::min<size_t>(5, benchMs.size() - 1), benchMs.end());
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double v : s) sum += v;
        fprintf(stderr,
                "bench: frames=%zu mean=%.2fms median=%.2fms p95=%.2fms max=%.2fms (%.0f fps median)\n",
                s.size(), sum / s.size(), s[s.size() / 2], s[(size_t)(s.size() * 0.95)],
                s.back(), 1000.0 / std::max(s[s.size() / 2], 1e-6));
    }
    if (benchTiles) {
        fprintf(stderr, "benchtiles: drawCanvas tiled %d pane(s) on the last frame "
                        "(canvas %.0f px; -1 = not tiled, 0 = refused as too narrow)\n",
                g_tilePanesDrawn, g_tileCanvasW);
        for (int i = 0; i < g_tilePanesDrawn && i < 16; i++)
            fprintf(stderr, "benchtiles: pane %d x0=%.2f w=%.2f\n",
                    i, g_tilePaneX0[i], g_tilePaneW[i]);
    }

    // it captures main's locals by reference, and teardown can still fire callbacks
    g_drawFrame = nullptr;
    // a selftest must not leave its scripted clicks in the user's session or
    // their preferences (the frameless ones return before ever getting here)
    if (g_browseKeys.empty()) {
        autosaveSession();            // also covers a normal quit
        // Only what the user changed in this run: a one-off --stack flag or
        // the gamma inside a --session must not quietly become the default.
        if (app.prefsDirty) savePrefs();
    }
    stopSequenceLoader();             // join the workers before tearing anything down
    stopRemoteFetcher();
    stopMeasureWorker();
    stopRbWorker();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    plugin_host::unloadAll();
    if (!g_browseKeys.empty() && !keysOk) {
        fprintf(stderr, "browsekeys: FAILED (the action list did not finish)\n");
        return 1;
    }
    return 0;
}
