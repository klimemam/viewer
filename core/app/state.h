// core/app/state.h — 共有状態の自己完結ヘッダ。担当: 全テーマ共有
// P6 (docs/background/project/split-plan.md §5): state.inc がこのヘッダになった。単独で include できる
// (必要なものは全て自分で include する)ので、新規コードは実 TU として生まれられる
// (core/analysis/、最初の客は #84)。App app の定義は §6 により背骨(main.cpp)に
// 残る — ここにあるのは extern 宣言だけ。
// ファイルスコープの static は inline になった: このヘッダは複数 TU から include
// されるので、per-TU コピー(static)ではプログラム全体で 1 個であるべきもの
// (ソースレジストリ、その mutex、srcId カウンタ)が TU ごとに分裂する。
#pragma once

// GLuint (ImageDoc::tex, DiffTex::tex) — 背骨と同じプラットフォーム分岐。
#if defined(__APPLE__)
  #ifndef GL_SILENCE_DEPRECATION
    #define GL_SILENCE_DEPRECATION
  #endif
  #include <OpenGL/gl3.h>            // 3.2+ core declarations
#elif defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>               // GL/gl.h needs APIENTRY/WINGDIAPI
  #include <GL/gl.h>
#else
  #include <GL/gl.h>
#endif

#include "imgui.h"                   // ImVec2 / ImU32 / ImGuiKeyChord
#include "portable-file-dialogs.h"   // the pfd dialogs App holds
// core/ neighbours, spelled relative to THIS file (quoted includes resolve
// against the including file's directory - the ../browse respell precedent):
// core/ itself is not on the viewer target's include path.
#include "../ui_theme.h"             // ui_theme::VariantDark
#include "../remote.h"               // remote::Session / Entry / ScanGroup / GlobHit / MeasureResult
#include "../remote_proto.h"         // rp::F32Loss - what float32 cost THESE pixels
#include "../adapter.h"              // adapter::Run (App::ReaderJob)
#include "../watch.h"                // watch::Finding - what Watch has CONFIRMED
                                     // about a stack's files (watch-design §4)
#include "../videowrite.h"           // videowrite::VideoSink - the open file a
                                     // stack-to-video export is writing into (#253)
#include "../browse/browse_state.h"  // browse::Instance and the rb types (P7 §3);
                                     // App aliases them below, so every existing
                                     // App::BrowseInstance reference is unchanged

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// UTF-8 -> native path (wide on Windows, bytes on POSIX). Lived in util.inc
// until P6: statSourceFile / statLocalUrl / srcKeyPath below need it, and this
// header must not depend on a fragment. util.inc is included after this header
// in the spine, so its remaining functions keep seeing it.
inline std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::u8path(s);
}

// Display gamma has two convenient presets, but its persisted value is not an
// enum: Preferences, prefs.txt, settings.jsonc and .vsession all accept the
// same positive finite float domain (#226). Keep the validation and the exact
// preset test here so no entry point can quietly round a Custom value to 1.0
// or 2.2.
inline bool displayGammaValue(double candidate, float& out) {
    if (!(candidate > 0.0) || !std::isfinite(candidate) ||
        candidate > (double)std::numeric_limits<float>::max())
        return false;
    const float value = (float)candidate;
    // The double may overflow to infinity, or a tiny positive value may
    // underflow to zero when stored in App. Both would break 1/gamma.
    if (!(value > 0.0f) || !std::isfinite(value)) return false;
    out = value;
    return true;
}

inline int displayGammaPreset(float value) {
    if (value == 1.0f) return 0;
    if (value == 2.2f) return 1;
    return -1;                         // Custom: neither preset is selected
}

inline bool displayGammaNeedsTransform(float value) {
    return value != 1.0f;
}

inline uint32_t displayGammaBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof bits == sizeof value, "float32 display gamma");
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline bool parseDisplayGamma(const char* text, float& out) {
    if (!text) return false;
    char* end = nullptr;
    const double candidate = std::strtod(text, &end);
    if (end == text) return false;
    while (*end && std::isspace((unsigned char)*end)) ++end;
    return *end == '\0' && displayGammaValue(candidate, out);
}

// Shortest ordinary decimal (up to float::max_digits10) that reads back to the
// same float. This keeps Custom 1.25 readable without losing less tidy values
// on a prefs/session round trip. At the finite upper boundary, a rounded
// 9-digit decimal can sit just above FLT_MAX; the exact-double fallback stays
// inside the accepted domain while still returning the same float.
inline std::string displayGammaText(float value) {
    if (value == 1.0f) return "1.0";
    if (value == 2.2f) return "2.2";
    char buf[64] = {};
    for (int precision = 1; precision <= std::numeric_limits<float>::max_digits10;
         ++precision) {
        const int n = std::snprintf(buf, sizeof buf, "%.*g", precision, (double)value);
        float roundTrip = 0;
        if (n > 0 && n < (int)sizeof buf && parseDisplayGamma(buf, roundTrip) &&
            roundTrip == value)
            return buf;
    }
    std::snprintf(buf, sizeof buf, "%.*g", std::numeric_limits<double>::max_digits10,
                  (double)value);
    return buf;
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
  inline constexpr ImGuiKeyChord MODK = ImGuiMod_Super;
#else
  #define SC_MOD "Ctrl"
  inline constexpr ImGuiKeyChord MODK = ImGuiMod_Ctrl;
#endif

// Copy the view dot by dot. Named here, next to the pair above, because the
// context menu advertised "Ctrl+Shift+C" as a literal string and NOTHING in the
// program listened for that chord: a shortcut that had been typed into a label
// and never bound. The menu item now takes its label FROM this constant
// (ImGui::GetKeyChordName), so the advertisement and the binding are one fact
// and cannot come apart - which is the same argument the SC_MOD / MODK pair
// above is making, one step further along.
inline constexpr ImGuiKeyChord CHORD_COPY_DOT = MODK | ImGuiMod_Shift | ImGuiKey_C;

inline std::atomic<uint64_t> g_nextSrcId{ 1 };
struct FrameSource {
    std::vector<float> data;          // raw values, size w*h*ch
    int w = 0, h = 0, ch = 1;
    std::string dtype;
    float vmin = 0, vmax = 1;         // data min/max
    // What holding these pixels as float32 COST, measured by whichever decoder
    // narrowed them (rp::F32Loss, core/remote_proto.h). Zeroed for every dtype
    // float32 holds exactly, which is all of them but u32 / i32 / f64 - so an
    // .any() here is a fact about THIS array, never a guess from its dtype.
    // The Inspector prints it; fmtVal marks the individual values it covers.
    rp::F32Loss f32loss;
    std::string path;
    // This frame's name INSIDE its container file: a .npz array name, or an
    // .exr layer. ONE field, because it is one idea - a named part of one file -
    // and the session line, the Inspector row and the identity tuple below all
    // key off it, so a parallel field would have meant a second copy of every
    // one of them. It was called npzMember while .npz was the only container
    // that had any; the session key, the sequence record and the tuple all
    // already said "member", and only this declaration still said "npz".
    // The WORD shown to the user is the container's own and comes from
    // imagefile::Backend::partWord - never spelled out here.
    std::string member;
    int fileFrame = 0;                // frame index within a multi-frame LOCAL file
                                      // (npy frame axis; npz members too). With path +
                                      // member it completes the provenance a reload
                                      // re-decodes - remoteFrame is the remote twin
    // How this array was READ (docs/features/adapters/input-adapters.md §3.1/§3.3). npyShape is
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
    // The protocol the peer announced when these pixels were opened; 0 = these
    // are not a peer's pixels. Recorded rather than asked again at draw time
    // because the Inspector has to know, for THIS document, whether §3.3's
    // re-reading can be served - and the answer must not change because the
    // session was later pointed at a different host. Below rp::VERSION it is
    // what the "read as" line says INSTEAD of a menu (issue #124).
    int remoteProto = 0;
    // ONE ARRAY INSIDE SOMETHING THE PEER MATERIALISED (protocol 12/13,
    // remote::KeyedRef). Empty key = the url names a file on the peer's disk,
    // which is every remote document that came before issue #180. When it is
    // set, META and TILE address the key and the node instead of the path - so
    // the full-resolution swap and the sibling frames of a stack have to carry
    // it too, exactly as they carry the §3.3 reading, or the refinement of a
    // .npz member would silently fetch the whole container as one array.
    std::string remoteKey;
    int remoteNode = 0;
    int remoteKeyKind = 0;            // remote::KeyedRef::Kind - which protocol
                                      // number an old peer is refused from
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
inline void statSourceFile(FrameSource& s) {
    std::error_code ec;
    auto p = pathFromUtf8(s.path);
    auto t = std::filesystem::last_write_time(p, ec);
    s.mtime = ec ? 0 : (int64_t)t.time_since_epoch().count();
    auto sz = std::filesystem::file_size(p, ec);
    s.fsize = ec ? 0 : (uint64_t)sz;
}
// WHICH FILE ON THIS DISK a url names, and "" when it names a peer's.
//
// "An empty host means this machine" is the rule the remote layer already runs
// on - makeRemoteUrl mints "local://<path>" for an empty host and
// remote::parseUrl is its inverse (docs/features/watch/watch-design.md §13.7) - so it is asked
// HERE, once, through that inverse, rather than by yet another literal prefix
// test. The two callers that used to spell it themselves (statLocalUrl below
// and watchLocalPathOf, the FrameSource-level form) now come through this, and
// srcKeyPath is the third: identity has to ask exactly the question Watch and
// Reload ask, or one file gets two answers about whose disk it is on.
inline std::string localDiskPathOfUrl(const std::string& url) {
    std::string host, path;
    if (!remote::parseUrl(url, host, path)) return std::string();
    return host.empty() ? path : std::string();
}
// The OTHER half of the same question - and it is not the negation of the half
// above, which is the whole reason it has its own name.
//
// Three strings, not two. A url can name this disk, or name a peer, or name
// NEITHER: "local://" with nothing after it parses as no path at all
// (remote::parseUrl returns false on an empty path), so it is not a file here
// and it is not a file there. The literal-prefix form `rfind("local://", 0)`
// answers "local, therefore mine" for that string and hands the caller an empty
// path to open. Every door and every greyed menu item has to fail SAFE on it,
// so the peer question is asked POSITIVELY - a peer url is one that parses AND
// names a host - and the third case falls out of both halves as it should.
inline bool isPeerUrl(const std::string& url) {
    std::string host, path;
    return remote::parseUrl(url, host, path) && !host.empty();
}
// ...and the third question, which looks like the first two and is not: does
// this string carry a SCHEME we own, i.e. is it a url rather than a path on
// this disk?
//
// This one cannot go through remote::parseUrl, which answers a WIDER question
// on purpose: it also accepts the bare scp spelling `host:path`, because that
// is what muscle memory types into a host field. Callers here are not reading a
// host field - they are handed `FrameSource::path` or an argv word and must
// decide whether it is a path on this disk, so the wider answer is wrong for
// them. A file legally named "notes:2026/x.npy" is a path; parseUrl reads it as
// host "notes" (the Windows "C:\\..." case it already excludes by hand, the
// POSIX one it cannot). The scheme test is therefore literal, honestly, and it
// is written HERE once so the honesty is in one place instead of four.
inline bool hasUrlScheme(const std::string& p) {
    return p.compare(0, 6, "ssh://") == 0 || p.compare(0, 8, "local://") == 0;
}
// The local:// twin of statSourceFile: the url EMBEDS a path on this disk, so
// unlike a true remote the disk baseline is knowable - and it has to be known,
// because a local:// tuple with mtime/fsize 0 never moves: an overwrite on
// disk followed by a re-open of the same url would ADOPT the stale resident
// and silently show the old pixels. ssh:// urls stay 0/0 on purpose (a peer's
// disk cannot be statted from here; change detection there is Watch's).
inline void statLocalUrl(FrameSource& s) {
    const std::string onDisk = localDiskPathOfUrl(s.remoteUrl);
    if (onDisk.empty()) return;
    std::error_code ec;
    auto p = pathFromUtf8(onDisk);
    auto t = std::filesystem::last_write_time(p, ec);
    s.mtime = ec ? 0 : (int64_t)t.time_since_epoch().count();
    auto sz = std::filesystem::file_size(p, ec);
    s.fsize = ec ? 0 : (uint64_t)sz;
}
// A deep copy of s with its own identity: same content, fresh srcId, rev 0.
// The two callers are the crop CoW (§2.2) and any ImageDoc copy that must not
// alias a live source.
inline std::shared_ptr<FrameSource> cloneSource(const FrameSource& s) {
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
// The tuple is (path-or-url, member, frame-within-file, raw recipe, npy
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
// slow or dead server, and a local:// url reaches that round trip too since it
// names this disk), so every key is computed BEFORE the lock is taken;
// while it is held, only map and field operations happen. Identity fields are
// only ever written by the UI thread (load, reload swap, crop) or on a source
// no other thread can see yet, so a pre-lock read of them is never torn.
inline std::mutex g_srcRegMtx;              // lookups also run on the seq loader thread
inline std::unordered_map<std::string, std::weak_ptr<FrameSource>> g_srcRegistry;

// The same file reached as "C:\data\F.npy" and "c:/data/f.npy" is ONE tuple:
// canonicalize exactly once, HERE, where the key is built. The stored path is
// provenance (reload and sessions read it verbatim) and is never rewritten.
inline std::string srcKeyCanonical(const std::string& p) {
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
// A PEER's path passes through untouched, and that is why this function has
// always looked for "://": weakly_canonical is a question about THIS
// filesystem, and asking it about "/data/cap/f.npy on trc2" answers with this
// machine's symlinks, this machine's current directory and this machine's case
// rules - it would rewrite, or silently absolutize, a name that never touches
// this disk. A url is not a filesystem path.
//
// local:// IS this disk, though (docs/features/watch/watch-design.md §13.7 - the same "empty
// host means this machine" every other consumer runs on), so it was never one
// of those, and passing it through was the defect: the local:// url EMBEDS an
// on-disk path, so the file it names is the same file the local door opens,
// and the two doors were minting two §6.2 keys for it -
//
//   the local scan   c:\users\...\openfolder\dark_001.png
//   a local:// open  local://tools/testdata/openfolder\dark_001.png
//
// - which is two registry rows, two residents and two independently reloadable
// entries for one set of bytes. Resolving the EMBEDDED path is what makes them
// one, and it is deliberately done HERE rather than at the doors: the key is
// computed, never stored, so every session line already written under the url
// spelling lands on the same tuple as one written under the path spelling, with
// no migration and no second identity for a restored session (--scan-selftest
// S4d asserts exactly that line).
inline std::string srcKeyPath(const std::string& p) {
    if (p.empty()) return p;
    if (p.find("://") != std::string::npos) {
        const std::string onDisk = localDiskPathOfUrl(p);
        return onDisk.empty() ? p : srcKeyCanonical(onDisk);   // "" = a peer's
    }
    return srcKeyCanonical(p);
}

// The reading that names this source's tuple (defined by npyLayout's §3.1
// machinery, below): NR_NATIVE stays 0 whatever the shape - a pre-decode
// probe knows no shape, and a native open must still find a native resident -
// and a DECLARATION that merely spells out what native would do anyway
// collapses onto 0 too, so "read as (H,W,C)" on a natively-HWC file and a
// plain open are ONE tuple. Only a declaration that actually changes the
// reading keys its own tuple.
int npyKeyRead(const std::vector<int64_t>& shape, int npyRead);   // defined in loader_npz.inc (external since P6: srcIdentityKey is inline here and calls it)

inline std::string srcIdentityKey(const FrameSource& s) {
    char t[160];
    // WHICH FRAME OF THAT FILE, said once. The pair exists because a LOCAL file
    // counts its frames in fileFrame and a PEER's counts them in remoteFrame -
    // two files, two counts, two slots. A local:// source fills the remote slot
    // only because the peer process did the reading, and the file it read is on
    // THIS disk: keeping the slots apart there is the path half of this key's
    // defect one field over, and it leaves frame 0 of a multi-frame .npy shared
    // between the two doors while frames 1..N-1 stay split. Nothing ever writes
    // both slots (fileFrame has two writers, both local decoders; remoteFrame
    // has two, both remote), so folding to (frame, 0) loses nothing.
    int fileFr = s.fileFrame, peerFr = s.remoteFrame;
    if (!s.remoteUrl.empty() && !localDiskPathOfUrl(s.remoteUrl).empty()) {
        fileFr = std::max(fileFr, peerFr);
        peerFr = 0;
    }
    if (s.rawDtype >= 0)    // the raw recipe (incl. its dims) decides the pixels
        snprintf(t, sizeof t, "\n%d|%d\nraw%d,%d,%d,%d,%dx%d\n%lld,%llu",
                 fileFr, peerFr, s.rawDtype, s.rawInterp, s.rawOffset,
                 (int)s.rawLE, s.srcW, s.srcH,
                 (long long)s.mtime, (unsigned long long)s.fsize);
    else                    // npy/npz: the file bytes decide, mtime+fsize name
                            // them; npyRead (§3.3) keeps two READINGS of one
                            // file from collapsing onto one key - two readings
                            // are two different sets of pixels
        snprintf(t, sizeof t, "\n%d|%d\nnr%d\n%lld,%llu",
                 fileFr, peerFr, npyKeyRead(s.npyShape, s.npyRead),
                 (long long)s.mtime, (unsigned long long)s.fsize);
    // member is LENGTH-PREFIXED: a zip may name a member anything, newlines
    // included, and an unescaped name could forge the field boundaries of this
    // key - two different tuples reading back as one string
    return srcKeyPath(s.path.empty() ? s.remoteUrl : s.path) + "\n" +
           std::to_string(s.member.size()) + ":" + s.member + t;
}
// What may satisfy (or seed) a lookup: full-frame pixels that still mirror
// their origin. A crop re-scoped them; a decimated remote preview and a failed
// fetch are not the frame; no identity or no disk baseline means no tuple.
inline bool srcShareable(const FrameSource& s) {
    if (s.path.empty() && s.remoteUrl.empty()) return false;   // montage/processed
    if (s.remoteStep > 1 || !s.remoteErr.empty()) return false;
    // no disk baseline = no tuple, and local:// counts as disk: its embedded
    // path is statLocalUrl-able, so a 0/0 local:// source is one that skipped
    // the stat - refusing it here fails SAFE (no sharing) instead of letting
    // an overwrite-then-reopen adopt stale pixels under a key that never moves
    // isPeerUrl, NOT localDiskPathOfUrl: they differ on "local://" alone, and
    // the difference is a real one here. That url has no path, so statLocalUrl
    // skipped it and its mtime/fsize are 0/0; asking "has a disk path" would
    // call it a peer, skip the 0/0 guard below, and make exactly the stale
    // source shareable that the guard exists to refuse (M8e holds this down).
    // Asking "is it the peer's" keeps it on the local side, where the guard
    // catches it.
    const bool localDisk = s.remoteUrl.empty() || !isPeerUrl(s.remoteUrl);
    if (localDisk && s.mtime == 0 && s.fsize == 0) return false;
    if (s.rawDtype >= 0)              // raw decodes always record srcW/srcH
        return s.w == s.srcW && s.h == s.srcH && s.cropX == 0 && s.cropY == 0;
    return s.srcW == 0;               // npy: srcW only appears once cropped
}
inline std::shared_ptr<FrameSource> srcRegistryFindLocked(const std::string& key) {
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
inline std::shared_ptr<FrameSource> srcRegistryAddLocked(const std::shared_ptr<FrameSource>& s,
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
inline std::shared_ptr<FrameSource> srcRegistryAdd(const std::shared_ptr<FrameSource>& s) {
    if (!s || !srcShareable(*s)) return nullptr;
    const std::string key = srcIdentityKey(*s);   // IO: before the lock
    std::lock_guard<std::mutex> lk(g_srcRegMtx);
    return srcRegistryAddLocked(s, key);
}
// How a stack's time axis is folded into one frame (docs/analysis-layers.md
// §3.2, 判断record 3): the fold is a CHOICE of mean / sum, and the default is
// mean - the mechanism `0e4d4ec`'s frame average already built. Honesty is not
// in which folds are offered but in what every fold DECLARES: NaN present is a
// warning, and both folds carry the exclusion count and the valid frame count.
enum StackFold { FOLD_MEAN = 0, FOLD_SUM = 1 };

// The detrend stage's parameters (docs/features/analysis/flat-field-stats.md 判断6/7, settled
// 2026-08-09). Here rather than in core/app/detrend.inc for the reason
// StackFold is here: SeqInfo carries one, and SeqInfo is declared below. The
// arithmetic, and the reasoning behind every default, live in detrend.inc.
//
// A DEFAULT-CONSTRUCTED DetrendSpec IS RAW. That is 判断7 in one line: the
// self-describing default is "did nothing", and the shading figure printed
// beside every result is what stops that default from being a silent choice.
// The other fields are the same judgment's "when it IS on": degree 2,
// subtractive, per plane.
enum DetrendMethod { DTR_NONE = 0, DTR_POLY = 1, DTR_BLOCKMED = 2 };
struct DetrendSpec {
    int  method = DTR_NONE;
    int  degree = 2;
    int  block  = 64;
    bool divide = false;
};
// The record a PRODUCT of that stage carries (docs/analysis-layers.md §5: a
// generated data object states its origin in its name and its note, holds no
// file, and travels through a session as a RECIPE). It hangs off the produced
// stack's SeqInfo, which is what a bound role resolves to - so a SetAnalyzer
// run over this stack can state the cutoff its numbers were measured under
// without anyone having to remember to pass it along.
struct DetrendProduct {
    DetrendSpec spec;                     // spec.method != DTR_NONE = a product
    std::string ofPath, ofMember;         // the recipe: the SOURCE stack's head frame
    std::string srcName, when;
    int srcSeqId = 0, srcRev = 0;         // §10.1's key: which stack, which revision
    bool stale = false;                   // latched by the reload walk, never cleared silently
    int nPl = 1;
    double ppBefore[4] = {}, ppAfter[4] = {};   // shading p-p, DN, per plane
    double pctBefore[4] = {}, pctAfter[4] = {}; // ...as a % of the field centre
    bool pctOk[4] = {};
    bool made() const { return spec.method != DTR_NONE; }
};

// One membership: "this frame as seen by this stack". Pixels and provenance
// live in *src; what stays here is per-membership (identity, position, display
// range, interpretation) and per-view (texture) state.
struct ImageDoc {
    std::string name, dtype, note;
    int w = 0, h = 0, ch = 1;
    float vmin = 0, vmax = 1;         // data min/max
    float black = 0, white = 255;     // display range
    // What "Auto %" would answer for THESE pixels, cached lazily (issue #248).
    // The Range row lights the button whose result is the range currently in
    // force, and that question is asked on every UI frame while the percentile
    // answer costs a full 65536-bin pass over the image - so it is computed on
    // first need and kept. computeMinMax invalidates it: reload, crop and
    // detrend all move the pixels through there.
    float pctLo = 0, pctHi = 0;
    bool pctValid = false;
    // ...and what the MEDIAN of these pixels is (issue #252), cached beside it
    // for exactly the same reason and dropped in exactly the same two places.
    // One number rather than two: the "Med" fit derives its white point from
    // the median and a black point that costs nothing to recompute.
    float medVal = 0;
    bool medValid = false;
    GLuint tex = 0;
    bool texDirty = true;
    float texBlack = 0, texWhite = 0;   // the range this texture was built with
    bool texNearest = true;
    // CFA (Bayer) metadata
    int batchId = 0;                  // which 塊 (Files header) this belongs to
    bool preview = false;             // transient: not yet a registered open
    int remoteFrames = 1;             // frame-axis count, kept for promotion
    // What the PEER says this frame is, from META - which is not what w/h say
    // while a large frame is on screen as a decimated tile (remoteStep > 1).
    // A folder stack compares its siblings against its head, and the head's
    // full shape is the only one worth comparing against: the tile it is
    // showing right now will be replaced by exactly these numbers. 0 = never
    // came from a peer.
    int remoteFullW = 0, remoteFullH = 0, remoteFullCh = 0;
    int cfa = 0;                      // 0 none, 1 Bayer, 2 Quad Bayer
    int cfaPattern = 0;               // index into CFA_PATTERNS
    bool cfaColorize = false;
    int displayLut = -1;              // index into plugin_host::displays(), -1 = gray
    int dataRev = 0;                  // bumped on in-place pixel changes (crop)
    uint64_t uid = 0;                 // stable identity for caches (pointers ABA on reopen)
    int seqId = 0;                    // 0 = standalone, >0 = frame of that sequence
    int seqIndex = 0;                 // position within the sequence (file number order)
    // (No pendingViewScale here any more. A preview->full swap on an off-screen
    // frame used to leave the view correction on the FRAME for the next select
    // to apply, but the view it corrects is shared by the whole stack, so the
    // flags compounded - #250. selectImage derives the correction from w.)
    // A COMPUTED frame - today only a stack's frame average - has no file behind
    // it, so the session cannot round-trip it the way it round-trips an open.
    // What it CAN round-trip is the RECIPE: this holds the first-frame path of
    // the stack that was averaged (the key Series members already travel by,
    // for the same reason - paths round-trip, ids and names do not), and the
    // restore recomputes the mean once that stack is back. Empty on every frame
    // that came from a file, which is every other frame in the program.
    // avgOfMember is the other half of that key: two image members of one .npz
    // share a path and are told apart by nothing else (§6.2's identity tuple),
    // so a path-only recipe folded whichever of them came first.
    std::string avgOfPath, avgOfMember;
    int avgFold = 0;                  // StackFold the recipe folds by. It rides
                                      // beside the path because the same line
                                      // must bring back the same QUANTITY - a
                                      // sum reloaded as a mean would come back
                                      // 1/n of itself under one name.
    // The GENERATION record (docs/reference-design.md §10, issue #82): a
    // reduced frame is a computation over pixels that can move on without it,
    // and it ends up in reports as if it had been captured - so it must be
    // able to SAY whether it still describes its stack.
    //   - Within a session the key is (avgSeqId, avgStackRev): the reload walk
    //     bumps SeqInfo::stackRev and latches avgStale here, and the picture is
    //     never recomputed behind the user (silent recompute = silent
    //     overwrite). Recompute is the user's one move; it renews the record.
    //   - Across sessions stackRev does not survive; the vocabulary that does
    //     is §6.2's identity tuple (path + mtime + fsize). avgSrcDigest folds
    //     the tuples of every member that went into the fold, avgSrcCount says
    //     how many, avgWhen says when - and the restore compares its own fresh
    //     digest against the session's record to say "recomputed from newer
    //     files (source changed since ...)" instead of silently presenting a
    //     different picture under the same name (avgGenChanged).
    int avgSeqId = 0;                 // source stack at compute time (this session)
    int avgStackRev = 0;              // SeqInfo::stackRev at compute time
    bool avgStale = false;            // latched by the reload walk; only Recompute clears it
    bool avgGenChanged = false;       // restored from a DIFFERENT generation than saved
    uint64_t avgSrcDigest = 0;        // FNV-1a over the folded members' identity tuples
    int avgSrcCount = 0;              // how many member tuples the digest folds
    std::string avgWhen;              // wall clock at compute time ("source changed since ...")

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
        pctValid = false;             // different pixels, different quantiles
        medValid = false;             // ...and a different median
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
inline bool shareOrRegisterSource(ImageDoc& d) {
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
inline constexpr const char* CFA_PATTERNS[] = { "RGGB", "BGGR", "GRBG", "GBRG" };
inline constexpr const char* CFA_CH_NAMES[] = { "R", "Gr", "Gb", "B" };
inline constexpr int CFA_MAP[4][4] = {
    { 0, 1, 2, 3 },   // RGGB
    { 3, 2, 1, 0 },   // BGGR
    { 1, 0, 3, 2 },   // GRBG
    { 2, 3, 0, 1 },   // GBRG
};
inline int cfaChannelAt(const ImageDoc& im, int x, int y) {
    if (im.cfa == 0) return -1;
    int cx = im.cfa == 2 ? (x >> 1) & 1 : x & 1;   // Quad Bayer: 2x2 blocks share a color
    int cy = im.cfa == 2 ? (y >> 1) & 1 : y & 1;
    return CFA_MAP[im.cfaPattern & 3][cy * 2 + cx];
}

struct ViewState {
    float zoom = 1.0f;
    ImVec2 center = ImVec2(0, 0);     // image px at canvas center
};

// (RbToolbarGeom - the Browse toolbar geometry every instance records for the
// selftests - moved to core/browse/browse_state.h with the rest of the Browse
// state types, P7 §3.)

// One array inside a .npz, CLASSIFIED BY SHAPE before anything is opened
// (docs/features/adapters/npz-design.md §2.1). An .npz is a container: the file is a batch, a
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

// What a re-fit MEANS (issue #252). "Auto per frame" used to be one thing -
// this frame's min..max - and the Range row grew two more answers to the same
// question, so the per-frame mode has to be able to say WHICH of them it
// re-applies on every step. The three are not better and worse: min..max is
// the only answer that shows every pixel you have, the percentile is the only
// one a hot pixel cannot decide, and the median fit is the only one that puts
// the BULK of the picture at a stated display level.
enum AutoFlavor { AF_MinMax = 0, AF_Pct = 1, AF_Median = 2 };
// ...and where the median fit takes its black point from. `value` is a number
// the user types (0 for data that is already offset-corrected), `min` the
// measured vmin, `min 0.1%` the same 0.1 percentile "Auto %" cuts at - which
// is the one to pick when a dead column or a cold pixel owns vmin.
enum MedBlackMode { MB_Value = 0, MB_Min = 1, MB_MinPct = 2 };

struct App {
    std::vector<std::unique_ptr<ImageDoc>> images;
    int current = -1;
    ViewState view;
    bool showGrid = false;
    float dispGamma = 1.0f;           // positive finite; presets are 1.0 and 2.2
    float uiScale = 1.0f;
    int themeVariant = ui_theme::VariantDark;   // View > Theme
    int themeAccent = 0;                        // index into ui_theme::accents()
    std::string toast; double toastUntil = 0; bool toastErr = false;
    bool fitRequested = false;
    std::unique_ptr<pfd::open_file> openDlg;   // polled each frame; never blocks render
    std::unique_ptr<pfd::save_file> saveDlg;
    std::unique_ptr<pfd::save_file> csvDlg;    // Analysis > Export curves (CSV)
    std::unique_ptr<pfd::save_file> texportDlg; // Temporal > Save (CSV)...
    std::unique_ptr<pfd::save_file> roiExportDlg; // ROIs > Save (CSV)... (#67)
    std::unique_ptr<pfd::save_file> pngDlg;    // Image > Save view as PNG
    // ...and WHICH document it was opened for. The default file name is taken
    // from that document when the dialog opens, so the pixels have to come from
    // the same one when it closes - otherwise a load or a follow-frame landing
    // while the OS dialog is up writes a different image under the name the
    // user typed for this one. The ROI export beside it already keeps this rule
    // (it builds its text at open time, "the numbers may change while the OS
    // dialog is open"); this is the same rule for pixels, kept by identity
    // rather than by copying a frame.
    uint64_t pngDlgUid = 0;
    // ---- a STACK on its way out as a lossless video (issue #253) -----------
    // The neighbour of pngDlg above and its rule is pngDlgUid's, one layer up:
    // the dialog is opened ON A STACK, and the frames written when it closes
    // must be that stack's. A seqId, not an index - indices move as documents
    // open and close, and this one survives an OS dialog.
    std::unique_ptr<pfd::save_file> videoDlg;
    int videoDlgSeqId = 0;
    // The little modal in front of the save dialog: frame rate and format have
    // to be settled BEFORE the file name, because the format decides the
    // extension the dialog defaults to.
    bool videoAskOpen = false;
    int  videoAskSeqId = 0;
    float videoFps = 30.0f;           // positive finite; travels in the session
    int   videoFormat = 0;            // 0 = uncompressed AVI, 1 = FFV1/MKV (ffmpeg)
    // The job itself. renderDocRGBA is CPU-only but NOT thread-safe (it holds a
    // static gamma LUT), so this runs on the UI thread - ONE frame per UI
    // frame, the slicing pumpSequence uses, so a 300-frame stack does not
    // freeze the window for the length of the write (#232's lesson).
    struct VideoExport {
        bool active = false;
        int seqId = 0;
        std::vector<uint64_t> uids;   // the frames, by uid, in seqIndex order,
                                      // snapshotted at start: a document that
                                      // closes mid-write must be NOTICED, and a
                                      // uid that no longer resolves says so
        size_t at = 0;
        int w = 0, h = 0;
        double fps = 30;
        std::string path;
        std::string sinkName;         // "uncompressed AVI" / "FFV1/MKV"
        std::unique_ptr<videowrite::VideoSink> sink;
        bool stop = false;            // Stop: close the file and DELETE it
    } videoExport;
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
    std::string pendingRoiExport;              // ROI export (#67), same rule again
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
    // that does. docs/background/project/todo-open.md item 19 holds the shape this should
    // eventually take.
    std::vector<uint64_t> cmpExtra;          // uids, in slot order (C, D, ...)
    uint64_t lastCompareBUid = 0;            // the B last CHOSEN, kept across compare being off
    // path + npz/exr member: the session's `cmpslot` line and the `refmember`
    // that may follow it. An empty member is "the file itself, or an older
    // session that could not say" - it matches any member, which is exactly
    // what this resolved by before the key existed.
    struct SlotWant { int frame; std::string path; std::string member; };
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
        // ...and WHO computed it (#46 stage 1): the absolute path of the dll
        // the analyzer came from, copied from the host's ledger at run time.
        // The analyzer name + dll basename ride inside `prov` itself; the full
        // path is too long for the status line, so it surfaces as the line's
        // tooltip and as a "# plugin:" comment in the TSV/CSV exports. Empty =
        // no plugin file known (a builtin result must never wear a false one).
        std::string provDll;
        // ...and the version that dll DECLARED (#46 stage 2, ABI v3 §11).
        // Verbatim, and empty whenever the analyzer is a V1/V2 descriptor -
        // those carry no version field and the host does not invent one, so an
        // empty string here means "not declared", never "unknown, probably 1.0".
        std::string provVer;
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
        // The bin GRID, which is not the same thing as the range above.
        // black/white are the range the user ASKED for - the display range - and
        // they stay that, because they are the cache key (canvas.inc) and the
        // "is this side still binned like A?" test (panel_histogram.inc) and
        // both compare against effBlack/effWhite. The 256 bins are laid on a
        // ROUND grid snapped outward from it (histBinGrid), so that a bin is a
        // whole number of DN and no bin swallows one more integer value than
        // its neighbour. bin b spans [binOrigin + b*binW, binOrigin + (b+1)*binW).
        float binOrigin = 0, binW = 0;
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

    // ---- value highlight (#68, docs/features/histogram/histogram-select-design.md) -------------
    // Drag a value interval on the histogram's x axis and the pixels that fall
    // in it are painted on the image, in one colour, with the count declared.
    //
    // AXIS state - not the document's, not an annotation's. The flagship use is
    // "select the tail, then step frames and watch which pixels enter and leave
    // it", and every step changes the uid: held on the document the selection
    // would either vanish on the step or have to be copied across hundreds of
    // frames. What was chosen is a VALUE, and the same value asks the same
    // question of every frame. Nor is it an annotation: an annotation declares
    // a PLACE, is saved in the session and has a name and a list; this is a
    // question about VALUES with no identity of its own. It is not written to
    // the session either (v1) - a lens you pick up, not a declaration. The
    // guard against a forgotten lens is that it is ALWAYS declared on screen
    // (the panel footer and the canvas badge), never that it is short-lived.
    struct HistHighlight {
        bool on = false;
        // Raw DN, half-open [lo, hi), snapped OUTWARD onto the bin grid that
        // was on the axis when the drag happened. RAW, so moving the display
        // range or the gamma does not move it: what was chosen is "pixels of
        // this value", never "pixels that look this bright".
        float lo = 0, hi = 0;
        // An END bin was touched, so that side is open. The bins fold
        // everything outside the grid into bin 0 / bin 255
        // (recomputeHistogramIfNeeded), which makes those bars mean "and
        // everything beyond" - and a selection means what the bar means, or a
        // user who dragged over the saturation tail is handed a count smaller
        // than the bar they aimed at, missing exactly the pixels they came for.
        bool loOpen = false, hiOpen = false;
        // The plane selector BOUND at drag time (-1 = "all"), and its NAME.
        // Bound, because the highlight is defined only where a pixel has
        // exactly ONE value under the plane filter; moving the selector
        // afterwards does not re-aim an existing selection. The name is what
        // other documents are matched on - abSeriesMatch's standing rule, "B's
        // plane is matched to A's by name; ch0 is not R".
        int plane = -1;
        char planeName[8] = "";
    } highlight;
    // What the PAINTING loop counted, per document uid. renderDocRGBA is the
    // ONLY writer: the histogram bins are a different POPULATION (sampled to
    // ~1M px, and limited to the ROI when a ROI drives the panel) while the
    // paint sees every pixel of the frame, so deriving the count from the bins
    // would put two numbers with different denominators behind one sentence.
    // The panel and the canvas badge only READ this.
    struct HlCount {
        bool painted = false;         // this document could answer at all
        std::string why;              // ...and when it could not, why, in words
        int plane = -1;               // the binding RESOLVED in this document
        int nSeries = 0;
        const char* names[4] = {};
        size_t hit[4] = {}, fin[4] = {};   // matched / finite, per plane
    };
    std::map<uint64_t, HlCount> hlCount;

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
        // The shape this stack IS, and the frame that says so - the remote
        // twin of the local loader's refW/refH/refCh (core/app/sequence.inc).
        // A folder stack's frames are separate FILES on the peer, so nothing
        // guarantees they agree; the local half refuses a numbered sibling of
        // another shape and this half has to say the same thing. Set only for
        // a remote FOLDER stack (attachRemoteStack): a frame-axis stack is one
        // file with one META, so its frames cannot disagree and there is
        // nothing to check. refW == 0 = no reference recorded, no check.
        int refW = 0, refH = 0, refCh = 0;
        std::string refName;
        int cfaType = 0, cfaPattern = 0;
        // Data revision of the stack AS A SET of pixels: the reload walk (§3.2)
        // bumps it whenever a member's source is swapped in place. Part of the
        // temporal cache key - (seqId, frames, ROI, CFA) alone cannot see a
        // reload that changes no shape (docs/reference-design.md §3.2).
        int stackRev = 0;
        // WHICH frame's fit the shared per-stack range came from (#254).
        // "Per stack" spreads one frame's black/white over the whole stack
        // (#248), and the Range row answers by COMPARISON - so on any OTHER
        // frame the shared numbers stopped matching that frame's own min/max
        // and the lit button went dark on the first step: 「Stack で Auto の
        // 設定が外れる」. The range was still shared; what was wrong was the
        // display. Recorded by shareStackRange and by the landing that copies
        // the reference frame's range, read only by rangeButtonMatches. A uid
        // and not a pointer: frames are closed, and a stale pointer answers.
        // 0 = nothing spread this stack's range, so there is nothing to
        // compare against and the row says nothing.
        uint64_t rangeRefUid = 0;
        // ---- the rule this stack's MEMBERSHIP came from (watch-design §6) ----
        // Set at the moment the stack is minted, and only when the file list it
        // was minted from IS its folder's whole sibling group - i.e. when
        // findSequenceSiblings(ruleDir/ruleHead) reproduces exactly these
        // members. Empty = the membership is an EXPLICIT decision (a derived
        // subset, a hand-picked open, a stack that spans two folders, a
        // frame-axis file), and §6's rebuild will not re-apply any rule to it.
        //
        // Recorded rather than re-derived at Reload time, because at Reload time
        // the two cases are genuinely indistinguishable: a folder group that
        // GAINED a file and a subset that never had it look identical from the
        // listing alone (§11.4 - the detection half decides this once, at the
        // baseline, for the same reason). A stack with no rule fails towards
        // doing nothing, never towards re-admitting frames a user excluded.
        //
        // For a stack opened from a PEER this is the PEER's directory and the
        // rule that was run is the peer's own grouper (§16, recordRemoteRule) -
        // the one that produced the Browse row the stack was opened from, and
        // the one whose group row the watcher already reads every fifteen
        // seconds. Which listing re-applies it is decided by where the FRAMES
        // are, not by what this string looks like: planStackMembership asks the
        // disk for a stack of local files and the peer for a stack of the
        // peer's, so a peer path is never handed to this machine's
        // directory_iterator.
        std::string ruleDir, ruleHead;
        // ---- what the LAST reload of this stack left behind (issue #56) ------
        // A reload that refuses one member and re-reads the others does not
        // leave a stack: it leaves frames of TWO moments under one name, and
        // every stack-level number - sigma_t first - then averages across a
        // seam nothing on screen admits to. The toast that said so scrolls
        // away, so the fact is latched HERE, on the stack itself, at the
        // moment the walk finishes.
        //
        // The state is "the last reload of this stack refused at least one
        // member", and it is recorded at the EVENT rather than inferred:
        //   - NOT from mtimes. A folder stack written one file per second has
        //     as many mtimes as frames and is perfectly coherent; mtime cannot
        //     see the only thing that matters, which is whether the copies in
        //     memory were taken at one moment.
        //   - NOT from stackRev. That counts re-reads, and counts them the
        //     same whether every member came along or one was left behind.
        // reloadOk > 0 with reloadFailed > 0 is the mixed-generation case (the
        // dangerous half); reloadOk == 0 means nothing was re-read at all, so
        // the pixels are still of one moment - just not the moment the user
        // asked for. One mark, and seqReloadNote() says which it is.
        //
        // Cleared by ONE move: a reload of this whole stack in which EVERY
        // member succeeds - the operation that actually makes the statement
        // false again. Never by a redraw, a selection, a panel, or a
        // single-frame reload (that one says nothing about the other members).
        // A stack whose file is gone for good therefore stays marked until it
        // is closed, which is true: it can no longer be re-read as one moment.
        //
        // NOT saved in a session, for stackRev's reason: a restore re-reads
        // every member from today's disk, so the restored stack is of one
        // moment by construction and a carried-over mark would be a lie.
        int reloadFailed = 0;             // members the last reload REFUSED
        int reloadOk = 0;                 // ...and members it re-read, same attempt
        std::string reloadFirstErr;       // "a_003.npy: cannot read file"
        std::string reloadWhen;           // wall clock of that attempt
        // ---- Watch (docs/features/watch/watch-design.md §5) ---------------------------------
        // What the watcher has CONFIRMED about this stack's files on disk, and
        // it is a fact about the DISK, not about these pixels: the frames in
        // memory are exactly what they always were, and the line this drives
        // says the files behind them have moved.
        //
        // Only the confirmed SUMMARY lives here. The baseline and §4's
        // candidate stay in the worker (watch::SetState): the worker cannot
        // hold a SeqInfo* at all, because SeqTable reseats its storage on every
        // add and remove precisely so that held pointers die (cc1ee8b). The UI
        // thread copies the summary across in pumpWatch.
        //
        // NOT saved in a session, for reloadFailed's reason: a restore re-reads
        // every member from today's disk, so a restored stack's baseline is
        // today's disk and a carried-over finding would describe a comparison
        // nobody made.
        watch::Finding watchFound;
        bool watchToasted = false;        // §5: the ONE toast, on first detection.
                                          // The line does not expire; the toast
                                          // does, so it is fired once and the
                                          // line carries the fact from then on.
        // ---- §9: what an AUTOMATIC reload did to this stack ------------------
        // The one thing in this program that moves a number with no click, so
        // the record of it is the LOUDEST thing Watch writes and not the
        // quietest: what reloaded, at what time, and what it did to the
        // membership - the whole reloadStackFromDisk summary, verbatim, the
        // same sentence the toast carried.
        //
        // It does NOT expire, for seqNote's reason doubled: a toast fades, and
        // the user did not even ask for this one. It is replaced by the next
        // automatic reload of the same stack, cleared by a MANUAL Reload of it
        // (the user has taken the wheel) or by the little x on the line. Empty
        // = no automatic reload has ever touched this stack.
        //
        // NOT saved in a session, for reloadFailed's reason: a restore re-reads
        // every member from today's disk, so a carried-over note would describe
        // an event that has no bearing on the frames that came back.
        std::string autoReloadNote;
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
        // Empty on every stack that was OPENED. Filled only on a stack this
        // program COMPUTED with the detrend stage (docs/features/analysis/flat-field-stats.md
        // 判断6): the method, the window, the shading it measured before and
        // after, and the recipe a session recomputes it from.
        DetrendProduct dt;
        // NO level here. The value a stack was captured at is meaningless
        // without the parameter's NAME and UNIT, and both of those belong to
        // the series - so the value does too (Series::Member::value). Keeping
        // it on the stack is what forced "one unit for the whole application".
    };

    // The open stacks. A CONTAINER of its own rather than a bare vector, for
    // one reason: seqInfo() hands out a raw SeqInfo* into this storage, and the
    // rule that governs that pointer (never hold it across an add or a remove -
    // see seqInfo() in core/app/sequence.inc) can only be checked if every add
    // and every remove goes through one place. `grep -n 'seqs\.' core` used to
    // answer "where can this table move?" with a list that had to be re-derived
    // by hand after every merge; the mutators are now exactly the three methods
    // below (push_back / erase / clear), by construction.
    //
    // And they all RESEAT: each one builds a fresh buffer, moves the elements
    // into it and lets the old one go, so a mutation ALWAYS invalidates every
    // outstanding SeqInfo*. That is deliberate, and it is the whole point.
    // std::vector reallocates only when it runs out of capacity, so whether a
    // held pointer dangles depended on the standard library's growth factor:
    // libstdc++ grows 2x and had spare room, MSVC grows 1.5x and did not - so
    // the one bug of this class we have had (cc1ee8b) was invisible on the
    // development machine and killed the process on Windows CI, with no output.
    // A defect that only one allocator can show is a defect that ships. Making
    // the invalidation unconditional costs one move of a handful of SeqInfo per
    // stack opened or closed - unmeasurable next to reading the frames - and
    // buys the same failure on every platform, which the 37 selftests then see.
    //
    // Poisoning the outgoing elements is the diagnostic half: a stale read gets
    // id == StaleId and the name "<stale SeqInfo>" instead of plausible data,
    // for as long as the freed block goes unreused. Best effort by nature (the
    // memory is not ours after the swap) - reading it is still undefined, and
    // holds() below is the defined way to ask.
    struct SeqTable {
        static constexpr int StaleId = -0x5EED;   // ids count up from 1: never real

        using iterator = std::vector<SeqInfo>::iterator;
        using const_iterator = std::vector<SeqInfo>::const_iterator;

        size_t size()  const { return v_.size(); }
        bool   empty() const { return v_.empty(); }
        iterator begin() { return v_.begin(); }
        iterator end()   { return v_.end(); }
        const_iterator begin() const { return v_.begin(); }
        const_iterator end()   const { return v_.end(); }
        SeqInfo&       operator[](size_t i)       { return v_[i]; }
        const SeqInfo& operator[](size_t i) const { return v_[i]; }
        SeqInfo&       front()       { return v_.front(); }
        const SeqInfo& front() const { return v_.front(); }
        SeqInfo&       back()        { return v_.back(); }
        const SeqInfo& back()  const { return v_.back(); }

        // How many times this table has moved. A pointer taken when this read N
        // is dangling the moment it reads anything else - which is what makes a
        // "was it held across a mutation?" assertion writable at all.
        unsigned long long rev() const { return rev_; }
        // Does p still point at a live element? The defined way to ask, and the
        // only one: comparing a stale pointer is fine, dereferencing it is not.
        bool holds(const SeqInfo* p) const {
            return p && p >= v_.data() && p < v_.data() + v_.size();
        }

        void push_back(const SeqInfo& s) {
            std::vector<SeqInfo> fresh;
            fresh.reserve(v_.size() + 1);
            for (auto& e : v_) fresh.push_back(std::move(e));
            fresh.push_back(s);
            reseat_(fresh);
        }
        iterator erase(const_iterator it) {
            if (v_.empty()) return v_.end();
            const size_t at = (size_t)(it - v_.cbegin());
            std::vector<SeqInfo> fresh;
            fresh.reserve(v_.size() - 1);
            for (size_t k = 0; k < v_.size(); k++)
                if (k != at) fresh.push_back(std::move(v_[k]));
            reseat_(fresh);
            return v_.begin() + (ptrdiff_t)at;
        }
        void clear() {
            std::vector<SeqInfo> fresh;
            reseat_(fresh);
        }

      private:
        // `fresh` holds the survivors; v_ holds the moved-from originals. Poison
        // those, then swap - `fresh` dies at the caller's return and takes the
        // old storage with it, so every SeqInfo* into it is now a freed pointer.
        void reseat_(std::vector<SeqInfo>& fresh) {
            for (auto& s : v_) {
                s.id = StaleId;
                s.name = "<stale SeqInfo>";     // short enough to stay inline
                s.lastImageIdx = -1;
                s.expectedFrames = s.stackRev = 0;
                s.reloadFailed = s.reloadOk = 0;
                s.reloadFirstErr.clear(); s.reloadWhen.clear();
                s.watchFound = watch::Finding{};
                s.watchToasted = false;
                s.autoReloadNote.clear();
                s.refW = s.refH = s.refCh = 0;
                s.remoteUrl.clear(); s.remoteHost.clear(); s.remoteFiles.clear();
                s.axisName.clear(); s.axisUnit.clear(); s.axisVals.clear();
            }
            v_.swap(fresh);
            rev_++;
        }
        std::vector<SeqInfo> v_;
        unsigned long long rev_ = 0;
    };
    SeqTable seqs;
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
        //
        // Members are frame | stack MIXED (docs/analysis-layers.md §3.3,
        // 判断record 7): a member is EITHER a stack (seqId != 0) OR a
        // standalone frame (frameUid != 0), never both. A stack member stands
        // in as a frame through §3.2's fold, DECLARED per member in `fold`;
        // the fit itself takes mean statistics only - a sum is n x the mean's
        // scale, so with differing n the points are different quantities, and
        // a sum-standing member REFUSES the fit out loud instead of entering
        // it. frameUid/fold ride AFTER include so the existing aggregate
        // initializers ({ seqId, value, include }) keep their meaning.
        struct Member {
            int seqId = 0;
            double value = std::numeric_limits<double>::quiet_NaN();
            bool include = true;
            uint64_t frameUid = 0;    // standalone frame member (seqId == 0)
            int fold = 0;             // StackFold a stack member stands as
        };
        std::vector<Member> members;  // order = display order (sorting is a button)
    };
    std::vector<Series> series;
    int nextSeriesId = 1;
    int curSeriesId = 0;              // which series the Linearity panel shows
    // ---- AnalysisSet (docs/features/adapters/reader-analysisset.md, "ras"): role bindings ------
    // A set is a NODE of one batch (ras 1.4) and its identity is (batch, set
    // name) - never its contents. Each role BINDS a member that keeps living
    // wherever it lives: binding is not containment, and it legitimately
    // crosses batches (the dark taken once, used by every set). A binding
    // target is at most one of stack / loose frame / series; all zero =
    // unbound, and `reason` says why in one line (ras 3.1: a Ref speaks about
    // the world, so its failure is a state to SHOW, never a call failure -
    // the inline side of that asymmetry fails in the harness, before here).
    struct ASetRole {
        std::string role;             // [A-Za-z0-9_]+ (ras 1.2)
        int seqId = 0;                // bound to a stack
        uint64_t frameUid = 0;        // bound to a loose frame
        int seriesId = 0;             // bound to a series
        std::string refPath;          // the Ref that made it ("" = inline member)
        std::string refMember;        // ...and its npz member, when one was named
        std::string note;             // a bound role's declaration (ras 2.2 case 2:
                                      // "bound to the open copy - ...")
        std::string reason;           // unbound: the one-line reason (ras 3.2)
        bool bound() const { return seqId != 0 || frameUid != 0 || seriesId != 0; }
    };
    struct ASet {
        int id = 0, batchId = 0;
        std::string name, note;
        std::string readerSpec;       // reader-born sets: who computed it (ras 4)
        std::string origin;           // ...and from which file
        std::vector<ASetRole> roles;  // declaration order = display order (ras 4)
    };
    std::vector<ASet> analysisSets;
    int nextASetId = 1;
    // A bind that had to wait for the drain gate (the seriesRestore gate).
    // Two kinds: a stack forming through the asynchronous group loader binds
    // by its head path once the rescan lands (kind 0); and a whole Ref whose
    // resolution was DEFERRED because a session restore is in flight (kind 1)
    // - resolving it at once would re-open files whose own image lines the
    // session is still about to load, and the restore would grow a twin stack
    // per cycle. Either way it binds at the gate, or says why not - never a
    // silent drop.
    struct ASetBind { int setId = 0; int roleIdx = 0; int batchId = 0;
                      int kind = 0;                    // 0 = by head, 1 = full Ref
                      std::string headPath;            // kind 0
                      std::string refPath, refMember, origin;   // kind 1
                      std::string what; };
    std::vector<ASetBind> aSetBindPending;
    // The session's analysisset block, held VERBATIM until every stack the
    // file asked for exists (ras 5.2 - the seriesRestore precedent, including
    // the rule that a save during the wait writes it back unchanged).
    struct ASetRestore {
        std::string name, batchName, readerSpec, origin;
        struct R { std::string role, kind, batchName, key, reason; };
        std::vector<R> roles;
        int truncated = 0;            // setrole lines cut before their key
    };
    std::vector<ASetRestore> aSetRestore;
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
    // "all stacks below": how many levels UNDER the opened folder the scan
    // walks (issue #204, ruled 2026-08-17 - the depth is a setting, 6 by
    // default). THE PREFERENCE AS WRITTEN; every walk asks scanDepthBelow()
    // below, which is where the range is enforced and which is the only reader.
    int folderScanDepth = 6;
    int rangeScope = 1;               // value range: 0 frame, 1 stack, 2 all
    // What "Auto per frame" (rangeScope 0) re-fits each frame TO (#252).
    // MinMax is what the mode has always done, and stays the default: a new
    // flavour must not change what an existing session shows.
    int autoFlavor = AF_MinMax;
    // ---- the "Med" fit: put the median at this display level ---------------
    // target is the NORMALISED display value (0..1, before gamma) the median
    // is to land on, so white = black + (median - black) / target. 0.5 by
    // default - "the middle of the bar" is the thing people mean by
    // "expose it properly", and 0.18 (18% grey) is a photographic convention
    // this tool has no other trace of. The open interval (0, 1) is enforced
    // where it is typed and where a session restores it: 0 divides, and 1 is
    // "white = median", which throws away half the picture.
    float medTarget = 0.5f;
    int medBlackMode = MB_Value;      // MedBlackMode
    float medBlackValue = 0;          // used by MB_Value
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
    // ---- Browse (rb) state types: core/browse/browse_state.h since P7 ------
    // The rb-family nested types are the Browse TUs' own state now (docs/
    // docs/background/project/split-plan.md §3, the #47 carve-out owning its state). The aliases keep
    // every existing App::BrowseInstance / App::RbJob / App::RbConnect
    // reference compiling verbatim - the alias way of moving a type without
    // touching its ~140 qualified references (churn zero; §1's "move only"
    // discipline kept on the reference side even where P7 may change meaning).
    // The enumerators need the constexpr spelling: a using-alias carries a
    // TYPE across, never an unscoped enum's members.
    using RemoteBrowse = browse::RemoteBrowse;
    using RbKind = browse::RbKind;
    static constexpr browse::RbKind RbConnect    = browse::RbConnect;
    static constexpr browse::RbKind RbList       = browse::RbList;
    static constexpr browse::RbKind RbUpdatePeer = browse::RbUpdatePeer;
    static constexpr browse::RbKind RbScan       = browse::RbScan;
    static constexpr browse::RbKind RbGlob       = browse::RbGlob;
    static constexpr browse::RbKind RbTreeList   = browse::RbTreeList;
    static constexpr browse::RbKind RbDisconnect = browse::RbDisconnect;
    static constexpr browse::RbKind RbPoll       = browse::RbPoll;
    using RbJob = browse::RbJob;
    using RbResult = browse::RbResult;
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
    using RemoteSearch = browse::RemoteSearch;
    // ==== ONE Browse instance: a view onto ONE place - browse::Instance =====
    // (The struct and its decision record live in browse_state.h now.)
    using BrowseInstance = browse::Instance;
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
    // hasRecipe/recipe: the declared geometry of a HEADERLESS stack (protocol
    // 11). Carried on the job rather than looked up on the worker thread,
    // because the stack's documents live on the UI thread and the worker holds
    // no pointer into app state - the same rule the rest of this struct follows.
    // `key`/`node`/`keyKind` say the subject is ONE ARRAY INSIDE SOMETHING THE
    // PEER MATERIALISED rather than the file `url` names (protocol 14): a
    // reader's node, or a member of a container. `url` still travels - the
    // worker connects to the host in it - but `files` stays empty and no path
    // is put on the wire, because the stack whose sigma_t is wanted is not a
    // file over there. Carried on the JOB and not looked up on the worker for
    // the reason every other field here is: the worker is a different thread
    // (and a different peer PROCESS) from the one that opened the document.
    struct MJob { std::string url; int op; uint64_t token; std::vector<std::string> files;
                  int cfaType = 0, cfaPattern = 0; float black = 0, white = 1;
                  int rx = 0, ry = 0, rw = 0, rh = 0; std::string exe;
                  bool hasRecipe = false; rp::RawWire recipe{};
                  std::string key; int node = 0, keyKind = 0; };
    struct MDone { uint64_t token; bool ok = false; std::string err, host;
                   remote::MeasureResult res; };
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<int> mPending{ 0 };
    std::mutex mMtx;
    std::condition_variable mCv;
    std::vector<MJob> mQueue;
    std::vector<MDone> mDone;
    // ---- Watch (docs/features/watch/watch-design.md §2/§3) ---------------------------------
    // ONE watched set. The UI thread rebuilds this list from the open stacks;
    // the worker only reads it, so nothing here is a pointer into app state.
    //
    //   dir + headName is a FOLDER stack: the directory is listed once per poll
    //   however many stacks share it (§2's dedupe), and headName re-applies the
    //   sibling rule to that listing - which is what makes "a new file appeared"
    //   and "a file is gone" answerable in the same round trip as "one changed".
    //   file is an IN-FILE frame-axis stack (one .npy holding F frames): one
    //   stat, and the set has exactly one member.
    //   peer is a stack that lives on a PEER (§1's remote bullet): dir is the
    //   peer's directory, members are the stack's own file names in it, and one
    //   LIST of dir answers the whole stack at once. No local path exists for
    //   any of it, so `file`/`headName`/`seed` are all empty and the first poll
    //   makes the baseline.
    struct WatchTarget {
        int seqId = 0;
        std::string dir, headName;    // folder stack
        std::string file;             // frame-axis stack (dir empty)
        // ---- §1's remote half ------------------------------------------------
        // `peer` is a FLAG and not an inference from `host`: an empty host is a
        // real peer (the same protocol over a pipe instead of ssh - remote.h's
        // "passing an empty host starts a local peer"), so "host is empty" means
        // local to no part of this program.
        bool peer = false;
        std::string host;             // "" = the peer runs here
        int port = 0;                 // non-default ssh port, or 0
        std::string hostLabel;        // what the SENTENCE calls it (peerLabel)
        std::string exe;              // how to invoke it, frozen at publish time
        std::vector<std::string> members;   // the stack's own names, sorted/unique
        // §1: the baseline from the loader's own stat (FrameSource::mtime/fsize),
        // where EVERY member has one. Empty = this stack has no recorded
        // baseline and the first poll makes it - which is the honest answer for
        // a stack opened before Watch existed, for a remote stack (an ssh:// url
        // stays 0/0 on purpose: a peer's disk cannot be statted from here - see
        // statLocalUrl), and the reason that first poll says nothing.
        watch::Obs seed;
    };
    struct WatchDone { int seqId = 0; watch::Finding found; };
    std::thread watchThread;
    std::atomic<bool> watchStop{ false };
    std::mutex watchMtx;
    std::condition_variable watchCv;
    std::vector<WatchTarget> watchTargets;   // UI -> worker (under watchMtx)
    std::vector<WatchDone> watchDone;        // worker -> UI (under watchMtx)
    // The target list carries the BASELINE (WatchTarget::seed), so anything that
    // moves FrameSource::mtime/fsize has to republish it - and a reload moves
    // exactly those, while changing neither the stack table's revision nor the
    // image count that pumpWatch otherwise watches. Set in reloadSource's walk,
    // which is the one place pixels are ever replaced (reference-design §3.2).
    bool watchTargetsDirty = true;
    // File > Watch > Watch source files (default ON: the whole feature is
    // "notify", and a notification nobody asked to suppress costs one directory
    // listing every five seconds on a thread that is asleep the rest of the
    // time). Persisted in prefs.txt.
    bool watchEnabled = true;
    // File > Watch > "Auto-reload a stack when its files change" (§9), and it
    // is the ONE setting in this feature that can move a number without a
    // click - every stage before it only ever SAID something. Hence:
    //
    //   DEFAULT OFF, and narrower than the switch above it. The settled
    //   semantics are "notify, then a manual Reload" (ruling A6, 2026-07-30);
    //   auto-update is opt-in and this is the opt. With it off, a finding
    //   behaves exactly as it did before this existed (--watch-selftest W39).
    //   IT LIVES UNDER watchEnabled. There is nothing to act on without a
    //   finding, so Watch off is auto off - one switch governs noticing and
    //   this one governs acting on what was noticed.
    //
    // Persisted in prefs.txt beside watchfiles, in that toggle's shape
    // (docs/features/settings/settings-inventory.md 9 is still open; this pre-empts nothing).
    bool watchAutoReload = false;
    // §2's interval, in seconds, as a value rather than a literal so
    // --watch-selftest never has to live through one. prefs is §9's "later".
    double watchIntervalSec = 5.0;
    // ...and §2's OTHER interval: a peer is asked every 15 s, not every 5. The
    // worker's timer is the local one, so a remote target is polled every Nth
    // round with N computed from these two (watchRemoteEvery) rather than typed
    // - changing either number here cannot then silently leave the peer polled
    // at the local rate.
    double watchRemoteIntervalSec = 15.0;
    // ---- §2's SECOND row: a Browse instance re-lists where it stands ---------
    // The same pair, for the other watched thing. A drawn Browse panel is a
    // person looking at a folder, so it is polled faster than a stack whose
    // finding is a line of text; the peer's is the same ratio question and is
    // computed from these two rather than typed (browseWatchEvery, nav.cpp).
    double browseWatchIntervalSec = 3.0;
    double browseWatchRemoteIntervalSec = 10.0;
    // §2: stacks are polled "while the app is not minimised" - the same line the
    // frame loop already draws for drawing at all. A minimised window has no row
    // to put a finding under, and the answer is not stale when it comes back:
    // the first poll after restoring reads today's disk.
    std::atomic<bool> watchPaused{ false };
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
            // frame | stack mixed members (§3.3): a frame member's row keys on
            // the doc's uid (seqId stays 0), carries the frame's spatial mean,
            // and has NO sigma_t - the canon forbids printing one for a frame
            // (σ_t is a stack's attribute). hasSigma marks rows whose sigma_t
            // column, PTC point and SNR point exist at all.
            uint64_t uid = 0;             // frame member: the doc it measured
            bool isFrame = false;
            bool hasSigma = false;        // stack with >= 2 frames
            int fold = 0;                 // the member's declared StackFold
            uint64_t nonFinite = 0;       // NaN samples excluded (§3.2 warning)
        };
        std::vector<Row> rows;
        int seriesId = 0;                 // the series these rows describe
        // ---- fit settings, the frame-linearity machinery in its series-layer
        // home (§3.3 migration; board row 104). Session-persisted ("seriespanel"
        // line) - the interim tool's settings were not, which was half of the
        // 暫定. Defaults reproduce the panel's historic fit exactly: no window,
        // ordinary least squares.
        int fitMethod = 0;                // 0 = OLS, 1 = relative-weighted (rev4-style)
        int winMode = 0;                  // 0 = all points, 1 = auto 5..95%, 2 = manual
        double winLo = 0, winHi = 0;      // manual window [DN]
        // what the window DID at Compute time, per plane - the screen and the
        // export state the applied window, never the setting alone
        double winPlaneLo[4] = {}, winPlaneHi[4] = {};
        int nFit[4] = {};                 // points the fit actually used, per plane
        bool planeFit[4] = {};            // THIS compute fitted the plane - without
                                          // it a refused plane's row would print the
                                          // previous compute's numbers as its own
        int nZero = 0;                    // in-window |mean|~0 points rev4-style dropped
        // Non-empty: Compute REFUSED to fit and this is the sentence, verbatim
        // on screen (§3.3: a sum-standing member is refused BEFORE running -
        // stated, never coerced to a mean it was not declared as).
        std::string refusal;
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
    bool showSeriesAnalysis = false;
    // The AnalysisSet layer's panel (docs/analysis-layers.md §2). Off by
    // default like Series Analysis: it says nothing until a set exists, and
    // sets are made deliberately.
    bool showSetAnalysis = false;
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
            // frame | stack mixed (§3.3): a row is EITHER a stack (seqId) or a
            // standalone frame (uid), exactly like the member it becomes. fold
            // is carried through Save untouched - the modal does not edit it
            // (the panel's member table does), but Save must not reset it.
            uint64_t uid = 0;
            int fold = 0;                 // StackFold (stack rows only)
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
    // about: docs/features/browse/browse-topbar-design.md 10.2.)
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
        // the peer's half of the same declaration (serve.cpp runTemporalStats):
        // the amount subtracted and whether it hit the clamp. Read off the reply
        // by key like every other number here - the panel must be able to say
        // "corrected by X, clamped" for a server row exactly as for a local one,
        // or the two sources would print one label over two quantities.
        double fpnCorr[4] = {};
        bool fpnClamped[4] = {};
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
        // The §3.3 reading this stack is being read under, and the shape it was
        // read from (issue #124). Both travel with the JOB, exactly as `note`
        // does and for the same reason: the frames of one stack must be the
        // same reading as its head, and a sibling that fetched itself natively
        // while the head was declared would be a stack of two different arrays.
        // The peer's protocol rides along so a minted sibling carries the same
        // answer to "can this be re-read" as the head does.
        int npyRead = 0;              // rp::NpyRead
        int remoteProto = 0;
        std::vector<int64_t> npyShape;
        // ...and WHICH ARRAY, when the url does not name one on its own: a
        // member of a container the peer listed, or a node of what a reader
        // produced there (FrameSource::remoteKey). A job that dropped these
        // would fetch the whole .npz as one array and get a refusal, or - for a
        // reader - the origin's own pixels under the reader's label, which is
        // the failure protocol 12's version gate exists to prevent.
        std::string key, member;
        int node = 0, keyKind = 0;
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
        int npyRead = 0;              // the reading it was fetched under, back
        int remoteProto = 0;          // from the job: a minted sibling records
        std::vector<int64_t> npyShape;// the same three facts its head does
        std::string key, member;      // ...and which array of a materialisation
        int node = 0, keyKind = 0;    // it is, for the same reason (see RFetchJob)
        // ...and what float32 cost THESE samples, measured on the worker where
        // the peer's exact bytes still existed. It cannot be recomputed on the
        // UI thread - by then the only copy is the float one.
        rp::F32Loss f32loss;
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
    // A session line that named something the doc had not finished BEING yet.
    //
    // A remote open lands ONE frame and streams the rest (openRemote), and it
    // lands that frame DECIMATED and swaps the full resolution in later
    // (pumpRemoteFetch). The session parser runs to the end of the file in
    // between, so two of its lines had nothing to act on:
    //
    //   seqframe  the frame the stack was left on. framesOfSeq() held only the
    //             head, the loop matched nothing, and the stack came back on
    //             frame 0 (verify-matrix G5).
    //   crop      cropInPlace REFUSES at remoteStep > 1, because the
    //             full-resolution landing replaces data/w/h wholesale and knows
    //             nothing of crop bookkeeping. The restore ignored that `false`,
    //             so above the preview threshold the saved crop vanished
    //             (verify-matrix G10).
    //
    // Neither said anything, which is what made them the same defect: a restore
    // that quietly does less than the session asked for. Parked here and
    // drained by pumpRestoreWaits() once the doc is what the line was written
    // about - the same answer the interactive path already gives the operator
    // ("still fetching the full frame - crop after it lands"). The work waits;
    // it does not disappear.
    //
    // GIVING UP IS AN EVENT TOO. A fetch that errored, a stack that stopped
    // growing without the frame in it, a doc closed meanwhile: each drops the
    // entry, and the first two SAY so, by name.
    struct RestoreWait {
        uint64_t uid = 0;                      // the doc the line was read against
        int seqFrame = -1;                     // "seqframe"; -1 = not waiting for one
        int cx = 0, cy = 0, cw = 0, ch = 0;    // "crop"; cw == 0 = not waiting for one
        std::string doc;                       // the doc's name, for the message
    };
    std::vector<RestoreWait> restoreWait;
    // "Open as frame average" (or sum) on a stack that is not here yet. Browse
    // opens are asynchronous - the first frame shows and the rest stream in -
    // so the fold cannot be taken at click time; the request is parked on the
    // seqId it is waiting for, with its StackFold, and fired by
    // pumpStackAverages() once nothing more is coming. A session restore pushes
    // into the SAME list (through avgRestore below), so there is one place
    // where a stack becomes a folded frame and one set of rules about when it
    // is allowed to happen.
    // The generation record rides along (hasGen): a session's recipe carries
    // the input tuple digest it was computed from (§10.2), and the reduction
    // compares it against what it actually folds - the restore path is the one
    // caller that has an expectation to hand in.
    struct PendingAvg {
        int seqId = 0;
        int fold = FOLD_MEAN;                              // StackFold
        bool hasGen = false;                               // a record to compare against
        int srcCount = 0;
        uint64_t srcDigest = 0;
        std::string when;                                  // "source changed since ..."
    };
    std::vector<PendingAvg> pendingAvg;
    // ...and the session's side of it, by PATH, resolved lazily exactly like
    // seriesRestore: at parse time a folder stack is one loose image plus a
    // queued rescan, so the stack this names does not exist yet.
    struct AvgRestore {
        std::string path;
        std::string member;                                // ...of that file (refmember)
        int fold = FOLD_MEAN;                              // StackFold
        bool hasGen = false;                               // stackavggen was present
        int srcCount = 0;
        uint64_t srcDigest = 0;
        std::string when;
    };
    std::vector<AvgRestore> avgRestore;
    // ...and the same shape for a detrend product (docs/analysis-layers.md §5:
    // a preprocessor's output travels as a RECIPE, never as pixels). Same key
    // as an average's - the SOURCE stack's head frame path (+ member) - and the
    // same lazy resolution, for the same reason: at parse time a folder stack
    // is one loose image with a queued rescan behind it. `name` is carried
    // because the product is a renameable node and the session owns its name.
    struct DetrendRestore {
        DetrendSpec spec;
        std::string path, member, name;
    };
    std::vector<DetrendRestore> dtRestore;
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
        // kind: 0 = stack standing as mean ("seriesmember"), 1 = stack
        // standing as sum ("seriessum"), 2 = standalone frame ("seriesframe").
        // Three KEYS, not one key plus flags (the stackavg/stacksum rule): an
        // older viewer skips lines it does not know, so a sum declaration or a
        // frame member simply does not come back there - the right failure -
        // instead of a flag being dropped and a sum quietly reading as a mean.
        // member: the container member that path names, when the file has any
        // (the `refmember` line). Empty = unconstrained, which is how every
        // session written before that key resolves - by path alone.
        struct M { double value; bool include; std::string path; int kind = 0;
                   std::string member; };
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
    // ---- the LOCAL folder scan, off the UI thread (#236) --------------------
    // Open Folder used to walk and group the tree ON the UI thread, inside
    // parseCli or inside the click, so the window did not pump until the
    // picker was ready: measured at 17.9 minutes of a dead window on one
    // folder. The walk is the browse worker's shape now - one thread, a
    // generation token, results published under a lock and read by a pump.
    //
    // Why the picker is NOT filled in as groups arrive. The frame axis is "the
    // LAST digit run that varies among the candidates", so a group derived
    // from HALF a directory can have the wrong axis, the wrong name and the
    // wrong members - #236 measured exactly that (a01_b1/a02_b1 alone group
    // one way, and the four names group another). A partial answer here is not
    // an early answer, it is a wrong one that would have to be unsaid. So the
    // scan publishes a COUNT while it runs and the whole group list once.
    struct FolderScan {
        std::thread thread;
        std::mutex mtx;
        // gen is the cancel/supersede token, bumped by Cancel and by the next
        // Open. A worker whose gen has moved drops what it has and returns:
        // openPickerWith would otherwise destroy a picker the user is already
        // filtering, the same hazard RbScan's token guards remotely.
        std::atomic<int> gen{ 0 };
        std::atomic<bool> running{ false };
        std::atomic<long long> seen{ 0 };     // files enumerated so far, for the line
        std::string root;                     // UI thread only: what is being scanned
        // handover, under mtx
        bool done = false;
        int doneGen = 0;
        std::vector<PendingGroup> groups;
    };
    FolderScan folderScan;
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
    // ---- .npz member picker (docs/features/adapters/npz-design.md §2.3) ----------------------
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
    // (docs/features/adapters/npz-design.md §4 item 3 「実装で答えたこと (v1)」).
    struct NpzMeta { std::string path; std::vector<std::pair<std::string, std::string>> items; };
    std::vector<NpzMeta> npzMeta;
    // ---- input adapters (docs/features/adapters/input-adapters.md §4.12 / §4.13) ---------------
    // v1 remembers ONE thing: which reader read which file. Folder and glob
    // RULES are deliberately absent (§8 item 7) - they need a trust rule first,
    // because a rule living in a shared data folder would run someone else's
    // Python without anyone choosing it. This list is a record of choices the
    // user made, which is why it is visible and removable rather than magic.
    std::string pythonExe;                  // configured interpreter; "" = probe PATH
    struct ReaderMemo { std::string path, spec; };
    std::vector<ReaderMemo> readerMemo;     // most recent first; bounded, see §4.12
    std::vector<std::string> readerShown;   // specs whose command was shown once (§4.13)
    // §4.13.0: ONE panel, four entrances, and it does not close. Writing an
    // adapter is write -> load -> read the failure -> fix -> load again, and a
    // modal cuts that loop every time the author leaves for their editor.
    std::string readerPickPath;             // the file or folder the panel is aimed at
    bool readerPanelOpen = false;
    bool readerPanelRaise = false;          // an entrance asked for focus
    bool readerListOpen = false;            // §4.12's visible, removable list
    // #166: the named-recipe panel for headerless RAW. Beside the reader list
    // because it answers the same question one door along - "why does THIS file
    // open the way it does" - and because a lab's own formats are consulted,
    // not answered once and dismissed.
    bool rawRecipeListOpen = false;
    // #50: the one place a setting can be changed. Not modal - gamma, compact
    // and the theme are judged by looking at the picture behind the window,
    // which a modal takes away (docs/features/settings/preferences-panel-design.md §8). Not in
    // the session either: it is chrome for changing settings, not part of a
    // workspace, and putting it in `panels` would add a resident to the
    // "what should a clean start look like" argument that has nothing to do
    // with it.
    bool prefsOpen = false;
    char readerPickFile[512] = "";
    char readerPickFunc[128] = "load";
    // §4.13.2 (issue #51): the places readers are looked for, in the order the
    // user registered them. A place is a FILE (that one reader) or a FOLDER
    // (every .py in it) - which of the two is asked of the DISK at the moment of
    // the look and never stored, because a stored kind is a second copy of a
    // fact the filesystem already owns and the two disagree the day a folder is
    // replaced by a file. Before this there was one folder here and one
    // hard-coded shipped one, so a second location could not be found at all.
    std::vector<std::string> readerPlaces;
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
    int rdOpenMode = 0;               // 0 = pick a reader .py, 1 = open a file with one,
                                      // 2 = register one .py as a place (§4.13.2)
    std::unique_ptr<pfd::select_folder> rdFolderDlg;
    int rdFolderMode = 0;             // 0 = register a folder of readers (§4.13.2),
                                      // 1 = open a folder with one
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
        // What to put in FRONT of a failure, if anything (issue #232). The
        // Reader panel's Load needs none: the file and the reader are both on
        // screen above the button that was pressed. A MEMO REPLAY does - the
        // user opened a file and a reader they chose weeks ago ran because of
        // it, so "x.dat via r.py:load: ..." is the sentence, and it must not
        // change just because the wait moved off the UI thread.
        std::string blame;
        // ---- the reader ran on a PEER (issue #180) --------------------------
        // The same job, with the process on the other machine. The thread owns
        // a SESSION OF ITS OWN rather than borrowing app.uiSession, for that
        // session's two documented reasons: a 300 s round trip must not hold
        // the UI's frame, and two threads framing requests into one ssh stdin
        // desynchronise the stream permanently.
        bool remote = false;
        std::string url, host;             // what was opened, and where it ran
        int port = 0;
        std::string files[3];              // the texts the RUN carries
        remote::ReaderRun rrun;            // what came back
        bool rok = false;                  // ...or the link failed, and why
        std::string rerr;
        // Quitting with a reader still going. A joinable std::thread that is
        // DESTROYED is std::terminate, and this object is owned by the global
        // App, so "close the window while Python is running" would abort on the
        // way out - reported as a crash, and a crash at exit is the kind nobody
        // can reproduce on purpose. Ask it to stop and wait: adapter::run polls
        // the flag every 25 ms and kills the child, and remote::Session takes
        // the same flag as its abort, so this is a short wait and not the
        // five-minute timeout. Here rather than at the exit paths because there
        // are many of them - every selftest returns from main on its own.
        ~ReaderJob() {
            cancel.store(true);
            if (th.joinable()) th.join();
        }
    };
    std::unique_ptr<ReaderJob> rdJob;
    // Opening SEVERAL files at once - a drop of five, or a multi-select in File
    // > Open - calls openPath in a LOOP, and one reader runs at a time. The
    // rest wait here rather than being refused, which is what the loop got
    // when the run was synchronous. Exactly npzPickQueue's shape and its
    // reason (issue #232): the second request must not be lost, and it must
    // not silently displace the first.
    struct ReaderWait { std::string src, spec, blame; };
    std::vector<ReaderWait> rdQueue;
    bool anyFileDialog() const {      // see the note on csvDlg
        return openDlg || saveDlg || csvDlg || texportDlg || roiExportDlg || pngDlg ||
               videoDlg || folderDlg || rdOpenDlg || rdFolderDlg || rdNewDlg;
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
        // #57 judgment 2 (docs/features/analysis/flat-field-stats.md (b)): sigma_fpn is the
        // CORRECTED quantity - the temporal residual left in an n-frame mean is
        // subtracted from the spatial variance. Both halves of that subtraction
        // are part of the result, not implementation detail:
        //   fpnCorr[p]    = sqrt(mean_i(s_t,i^2 / n_i)) in DN, so that
        //                   sigma_fpn^2 = max(0, var_spatial(M) - fpnCorr^2)
        //   fpnClamped[p] = the subtraction went negative and the value is 0
        // A clamped sigma_fpn is a STATEMENT about the measurement ("no fixed
        // pattern resolvable above this stack's own temporal floor"), and the
        // canon's guard rule is explicit that a clamp is never silent
        // (docs/features/analysis/flat-field-stats.md (a) "0 でクランプし、クランプしたことを結果に出す").
        double fpnCorr[4] = {};
        bool fpnClamped[4] = {};
        // The region is ONE PIXEL (a POI, or a 1x1 rectangle - the same thing
        // to every number here). Part of the KEY, not only of the result: a
        // POI and a 1x1 ROI resolve to the same rect, and without this the
        // cache would answer one with the other's label.
        bool poi = false;
        // Is the temporal / fixed-pattern SPLIT a thing here at all? sigma_fpn
        // is the spatial spread of the time-averaged frame, so it needs a
        // region with spatial extent; sigma_t does not. Over a single sample
        // the arithmetic produces a clamped 0, and "sigma_fpn = 0, clamped"
        // says "no fixed pattern above this stack's temporal floor" - a
        // measurement. There is no measurement to make. false means the panel
        // prints sigma_t and REFUSES the other two by name, rather than
        // printing zeros that read as findings.
        bool splitValid = true;
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
    // docs/background/project/todo-open.md.
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
    //  docs/features/adapters/input-adapters.md §3.4 replaced it with FrameSource::npyRead, which
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
    // Frames the main loop has BEGUN, counted so "is this panel on screen right
    // now" can be asked outside the draw. It is a COUNTER and not a timestamp on
    // purpose (watch-design §14.1): this program idles at 0 fps, so a panel that
    // is plainly on screen has a `lastDrawnAt` that goes stale the moment the
    // user stops touching anything - which is exactly when a Browse panel is
    // being watched. The last frame that HAPPENED is what is on the glass, and
    // that is what a frame number says and a clock does not. 0 = no frame yet;
    // the first frame is 1, so a panel that has never been drawn (drawnFrame 0)
    // is never mistaken for one that was drawn in frame 0.
    uint64_t uiFrame = 0;
    bool lowBandwidth = false;        // remote/ssh: draw the minimum, not a tail
    bool showFps = false;
    bool fitOnSwitch = false;         // view (zoom/pan) is shared; switching keeps it
    // window frame: 1 = the title bar lives in our menu bar (see window_frame.h),
    // 0 = the desktop draws one above us. Persisted; --frame overrides for one run.
    int frameMode = 1;
};
// (ANN_COLORS は P6 で core/app/annotations.inc へ — state.inc 時代の注記の履行。)

// THE app. Declared here so every TU that includes this header sees the same
// object; DEFINED in core/main.cpp — §6 keeps the definition in the spine.
extern App app;

// ---- "is anybody going to press anything?" -----------------------------------
// True for --no-window, for every *-selftest and for --bench: a run with no
// hand on it. Set once in main() before parseCli, so the folder argument's own
// openFolder already sees it.
//
// The ONE thing it changes is that a local folder scan stays SYNCHRONOUS
// (#236). Moving the scan onto a worker means openFolder returns before the
// picker exists, and 103 call sites across 26 selftest files assert the picker
// - or accept it - on the line after openFolder(). Making the scan
// asynchronous for them would be 103 waits to write and 103 chances to write a
// wait that passes for the wrong reason; making it synchronous for them is one
// branch, and it costs the selftests nothing they had (no window is pumping
// there to keep alive). Declared here rather than in cli.inc because
// sequence.inc is included first and is what asks.
inline bool g_scriptedRun = false;

// ...and the one way to take that branch BACK, because the branch above is a
// hole as well as a convenience: every selftest sets g_scriptedRun, so all 57
// of them run the synchronous scan and NOTHING in the suite ever runs the one
// the user actually gets. --async-scan is that hole's lid - a scripted run that
// asks for the worker and then waits for the picker the way a person does
// (browse-keys' `waitpick`). Only openFolder reads it; everything else about a
// scripted run is unchanged.
inline bool g_forceAsyncScan = false;

// ---- "all stacks below": HOW FAR DOWN, asked in ONE place --------------------
// Issue #204, ruled 2026-08-17: the depth is a SETTING (loading.folderScanDepth)
// and its default is 6. Before that ruling the number was a literal at each
// door - scanFolderGroups walked 3, the peer was asked for 6 - so the SAME
// folder answered differently depending on which door opened it, and neither
// literal knew the other existed. This is the one mouth both doors now put
// their lips to (#71: "a second copy is a copy that drifts"), and it lives
// beside the field rather than in either door's file so that neither door owns
// it and a third door cannot be opened at some other depth by accident.
//
// THE CLAMP IS HERE because this is the one READER. settings.jsonc refuses a
// value outside the window by name (判断8 - a file that asked for 40 meant
// something, and quietly giving it 16 would answer a question it did not ask),
// but prefs.txt is plain text a user can edit and the Preferences panel's
// InputInt has no bounds of its own, so the number that reaches a directory
// walk still has to be sane no matter who wrote it.
//   1  - the shallowest walk that is still a walk: the opened folder plus one
//        level under it. 0 would make "all stacks below" mean "nothing below".
//   16 - far past any capture tree anyone has put in front of this program, and
//        still finite. core/serve.cpp caps its own SCAN/GLOB walk at 32, so
//        every depth this can ask for is one the peer will honour.
inline int scanDepthBelow() { return std::clamp(app.folderScanDepth, 1, 16); }
