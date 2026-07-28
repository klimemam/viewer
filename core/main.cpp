// viewer v0.1 — native image viewer for engineering data
// Features: .npy / .bin/.raw loading, hover pixel inspection, coordinate rulers,
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
#include "remote.h"
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
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>           // sysctlbyname for the physical-memory budget
#endif
#include <memory>
#include <unordered_map>
#include <mutex>
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
static bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out) {
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
static float niceStep(float raw) {   // smallest 1/2/5*10^k >= raw
    float mag = powf(10.0f, floorf(log10f(std::max(raw, 1e-9f))));
    for (float m : {1.0f, 2.0f, 5.0f, 10.0f})
        if (m * mag >= raw) return m * mag;
    return 10.0f * mag;
}

// ---------------------------------------------------------------- image model
struct ImageDoc {
    std::string name, path, dtype, note;
    int w = 0, h = 0, ch = 1;
    std::vector<float> data;          // raw values, size w*h*ch
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
    // raw reload parameters (sessions + post-open reinterpretation; -1 = not raw)
    int rawDtype = -1, rawInterp = 0, rawOffset = 0;
    bool rawLE = true;
    // crop bookkeeping: srcW/srcH = full source dims (0 = unknown), cropX/Y = origin in source
    int srcW = 0, srcH = 0, cropX = 0, cropY = 0;
    int displayLut = -1;              // index into plugin_host::displays(), -1 = gray
    std::string npzMember;            // array name when this came from a .npz
    int dataRev = 0;                  // bumped on in-place pixel changes (crop)
    uint64_t uid = 0;                 // stable identity for caches (pointers ABA on reopen)
    int seqId = 0;                    // 0 = standalone, >0 = frame of that sequence
    int seqIndex = 0;                 // position within the sequence (file number order)
    // remote frames: opened as a decimated preview, replaced in place by the full
    // frame when the background fetch lands - after that, indistinguishable from
    // a local image. remoteStep > 1 means "still the preview".
    std::string remoteUrl;
    int remoteFrame = 0;
    int remoteStep = 1;
    std::string remoteErr;            // background fetch failed; preview is all we have
    float pendingViewScale = 1;       // full-res swap while NOT current: applied on select

    float sample(int x, int y, int c) const { return data[((size_t)y * w + x) * ch + c]; }
};

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
    std::string pendingCsv;                    // built at click time: the results may
                                               // change while the OS dialog is open
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
    // caches are NOT recomputed (see selectImage). glfwGetTime() deadline.
    double abStepBusyUntil = 0;
    int pendingCompare = -1;          // --compare, applied once two images exist
    uint64_t prevImageUid = 0;        // the doc looked at before this one (B default)
    bool prefsDirty = false;          // a preference actually changed in this run
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
        std::vector<std::string> remoteFiles;
        int expectedFrames = 0;
        int cfaType = 0, cfaPattern = 0;
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
    struct Batch { int id; std::string name; };
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
    // sequence without opening it: the group's member paths and the index
    // currently shown. Cleared with the preview.
    std::vector<std::string> previewFiles;
    int previewIndex = 0;
    std::string previewLabel;
    int nextSeqId = 1;
    uint64_t nextUid = 1;
    uint64_t imagesRev = 1;           // bumped whenever the image list changes
    int seqLoadMode = 0;              // 0 = ask, 1 = always, 2 = never
    int rangeScope = 1;               // value range: 0 frame, 1 stack, 2 all
    float memBudgetGB = 0;            // 0 = auto (60% of physical RAM)
    // remote viewing: one peer process per host, reached over ssh
    std::unique_ptr<remote::Session> remoteSession;
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
    } rbrowse;
    // The connect / install / list sequence runs on THIS worker, never on the UI
    // thread: a git clone on the far side takes seconds, and "Connect froze the
    // app" is precisely the bug class this tool exists to avoid.
    // RbTreeList is an ordinary LIST whose answer lands in the tree cache
    // instead of replacing the listing: expanding a node must not navigate.
    enum RbKind { RbConnect = 0, RbList = 1, RbUpdatePeer = 2, RbScan = 3, RbGlob = 4,
                  RbTreeList = 5 };
    struct RbJob {
        int kind = RbConnect;
        std::string host, dir;
        int port = 0;
        std::string pattern;          // RbGlob only
        uint32_t gen = 0;             // RbGlob: Stop bumps it, stale results drop
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
        int peerVersion = 0;              // what the peer answered in HELLO
    };
    std::thread rbThread;
    std::mutex rbMtx;                 // guards rbQueue / rbDone / rbPhase
    std::vector<RbJob> rbQueue;
    std::vector<RbResult> rbDone;
    std::string rbPhase;              // what the worker is doing, for the UI
    std::atomic<bool> rbBusy{ false };
    std::atomic<bool> rbStop{ false };
    std::string pendingRemoteOpen;    // opened once the session is up
    // Stacks a remote folder scan decided to open, one at a time: the next
    // stack starts only when the fetcher is idle, so the memory budget each
    // openRemoteStack computes reflects what the previous one actually loaded.
    // name: folder-qualified stack name ("10lx/frame_###.npy") - seven stacks
    // all called frame_000.npy would be indistinguishable, and the linearity
    // Auto-levels reads the level from exactly this folder prefix.
    struct RemoteOpen { std::string host; std::vector<std::string> files;
                        std::string name; int batchId = 0; };
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
    } rbSearch;
    bool showRemoteError = false;     // the full failure text, on demand
    std::mutex sesMtx;                // app.remoteSession is shared with that worker
    // where analysis runs. auto: remote data -> server, local -> local. server:
    // even a resident frame is measured server-side (one engine for a whole
    // batch). local-fetch: never use the server for compute (today's behavior).
    enum { PolAuto = 0, PolServer = 1, PolLocalFetch = 2 };
    int procPolicy = PolAuto;
    // background MEASURE worker (own ssh connection: a 300-frame aggregate must
    // not stall the tile fetches). Results carry provenance for the panel.
    struct MJob { std::string url; int op; uint64_t token; std::vector<std::string> files;
                  int cfaType = 0, cfaPattern = 0; float black = 0, white = 1;
                  int rx = 0, ry = 0, rw = 0, rh = 0; };
    struct MDone { uint64_t token; bool ok = false; std::string err, host;
                   remote::MeasureResult res; };
    std::thread mThread;
    std::atomic<bool> mStop{ false };
    std::atomic<int> mPending{ 0 };
    std::mutex mMtx;
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
    bool showRemote = false;          // the server browser, its own panel
    // Browse listing view: false = the collapsed group rows the peer sends,
    // true = every frame of every sequence as its own row. Expansion is a pure
    // CLIENT-SIDE view over the same reply (the peer always sends `.members`),
    // so the toggle costs no round trip. Persisted: it is a way of working.
    bool rbFlat = false;
    // Browse header: false = just the path bar and the toolbar (the common
    // case), true = also the connection row and the server-side search. Also
    // persisted - "I always search" and "I never do" are both ways of working.
    bool rbAdvanced = false;
    // Tree mode: a directory expands IN PLACE instead of replacing the listing,
    // so a folder of folders can be compared without losing your place. LAZY -
    // expanding a node costs exactly one LIST, issued on the browse worker and
    // never on the UI thread, and collapsing KEEPS the answer: re-expanding is
    // free. Cache keyed by absolute path, so it survives navigation; dropped
    // when the machine changes or on an explicit refresh.
    bool rbTree = false;                                        // persisted
    std::map<std::string, std::vector<remote::Entry>> rbTreeCache;
    std::vector<std::string> rbExpanded;      // absolute dirs currently open
    std::vector<std::string> rbTreePending;   // ...and those still being listed
    int rbTreeLists = 0;                      // node LISTs issued (selftest)
    bool focusRemote = false;         // bring it forward when a menu item asks
    bool focusTemporal = false;       // ditto for Temporal (browser-fired stats)
    struct Msg { std::string text; bool err; };
    std::vector<Msg> msgLog;          // every toast, kept so it can be copied
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
        double tempNoise = 0, fixedPattern = 0, totalNoise = 0, mean = 0;
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
        std::string exe;              // snapshot at enqueue: the UI may edit
                                      // remoteExe while the worker connects
        int frame = 0;
        uint64_t uid = 0;
        int seqId = 0, seqIndex = 0;
        uint32_t gen = 0;             // closeAll bumps the generation; stale
                                      // results must not graft onto a new list
        bool low = false;             // preview buffering: yields to registered opens
    };
    struct RFetchDone {
        uint64_t uid = 0;
        int w = 0, h = 0, ch = 0;
        float vmin = 0, vmax = 1;     // computed on the worker: 12M floats is a
                                      // visible hitch on the UI thread
        std::string dtype, err;
        std::vector<float> data;
        std::string url, name;
        int frame = 0, seqId = 0, seqIndex = 0;
        uint32_t gen = 0;
    };
    std::atomic<uint32_t> rfGen{ 0 };
    std::thread rfThread;
    std::atomic<bool> rfStop{ false };
    std::atomic<int> rfPending{ 0 };
    std::atomic<int> rfTotal{ 0 }, rfFetched{ 0 };   // progress for the Files panel
    std::mutex rfMtx;
    std::vector<RFetchJob> rfQueue;   // guarded by rfMtx
    std::vector<RFetchDone> rfDone;   // guarded by rfMtx
    // pending "load the rest of the folder?" question
    int seqAskImage = -1;
    std::vector<std::string> seqAskFiles;
    std::string seqAskPattern;
    // queued stacks from "Open Folder" (loaded one after another).
    // shape: "24x1200x1600 u16" when known (npy header peek locally, v3 listing
    // metadata remotely); the picker shows it and the merge warning compares it.
    struct PendingGroup { std::string name; std::vector<std::string> files;
                          bool isRaw = false; int batchId = 0; std::string shape; };
    std::vector<PendingGroup> seqQueue;
    // Sibling loads a session asked for, drained one at a time.
    // startSequenceLoad calls stopSequenceLoader, so restoring N stacks by
    // calling it N times in the parse loop cancelled every load but the last:
    // a 3-stack session came back with 7 of its 15 frames.
    struct SeqRestore { uint64_t uid; std::vector<std::string> files; std::string pattern; };
    std::vector<SeqRestore> seqRestore;
    // Series a session asked for, resolved LAZILY for the same reason: at parse
    // time the stacks do not exist yet (a folder stack is one loose image plus a
    // queued rescan), so a member cannot be looked up. Members are named by the
    // PATH OF THEIR FIRST FRAME - a stack NAME would not do, because seqname is
    // dropped on restore for exactly this reason (cur()->seqId is still 0 when
    // the line is parsed; verified in loadSession).
    struct SeriesRestore {
        std::string name, batchName, paramName, unit;
        int kind = 0;
        int badValues = 0;                // value fields the parser could not read
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
    // reason. Members are named by their GROUP name, which becomes the stack's
    // name (PendingGroup::name / RemoteOpen::name -> SeqInfo::name): the remote
    // queue holds no seqId, so a name is the only handle there is.
    struct SeriesPending {
        int batchId = 0;
        std::string name, paramName, unit;
        int kind = Series::KLinearity;
        std::vector<std::pair<std::string, double>> byName;   // stack name -> value
    };
    // A QUEUE, not a slot. Resolution waits for every load to drain, which for a
    // folder-of-folders is seconds and for a remote sweep much longer, and File >
    // Open Folder is available throughout - a single slot meant the second sweep
    // silently threw the first one's ticked box, typed parameter and unit away.
    std::vector<SeriesPending> seriesPending;
    bool folderRecipeValid = false;   // raw recipe shared by the queue (see g_folderRecipe)
    // "which sequences do you want?" picker shown after scanning a folder.
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
    std::unique_ptr<pfd::select_folder> folderDlg;
    int folderDlgMode = 0;            // 0 = Open Folder (load stacks), 1 = Browse
                                      // Folder (local peer in the Browse panel)
    // temporal analysis cache (built-in, follows the selected ROI)
    struct TemporalState {
        int seqId = -1;
        int frames = 0;
        int rx = -1, ry = -1, rw = -1, rh = -1;   // resolved ROI, not annRev
        std::vector<float> idx, frameMean, frameStd;
        double tempNoise = 0, fixedPattern = 0, totalNoise = 0;
        bool valid = false;
        bool roiUsed = false;
    } temporal[2];                    // 0 = A, 1 = B (compare)
    // H/V projections (line profiles) of the selected ROI / whole image
    struct ProjState {
        const ImageDoc* img = nullptr;
        uint64_t uid = 0;
        int dataRev = -1;
        int mode = -1, cfa = -1, cfaPattern = -1;
        int rx = 0, ry = 0, rw = 0, rh = 0;
        int nSeries = 0;
        const char* seriesNames[4] = {};
        std::vector<float> h[4], v[4];    // per series: mean along columns / rows
        float hMin = 0, hMax = 1, vMin = 0, vMax = 1;
        // statistics of the profiles themselves (sigma of column means = column FPN)
        struct Stats { double mean = 0, sd = 0, mn = 0, mx = 0, pp = 0, pct = 0; bool valid = false; };
        Stats hStat[4], vStat[4];
        bool roiUsed = false;
    } proj[2];                        // 0 = A, 1 = B (compare)
    int projMode = 0;                 // 0 = mean, 1 = max, 2 = min
    bool showProjH = true, showProjV = true;
    int projYMode = 0;                // value axis: 0 auto (H/V shared), 1 display range, 2 fixed
    float projYLo = 0, projYHi = 1;   // used by mode 2
    std::vector<ImageDoc*> texLru;    // GPU textures kept for the N most recent frames
    int roiChannel = -1;              // channel shown in the ROI table (-1 = all)
    int npyAxis = 0;                  // ambiguous 3D npy: 0 = auto, 1 = leading axis is frames
    // shared display range: every open image (and every newly loaded one) uses it
    bool linkRange = false;
    float linkBlack = 0, linkWhite = 1;
    // panel visibility (persisted with the ImGui layout)
    bool showFiles = true, showInspector = true, showRois = true, showAnalysis = true;
    bool showHistogram = true, showTemporal = true, showProjection = false;
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
#define ICON_LINK "[ssh]"      // plain ASCII: the bundled font has no icon set
static void openRemote(const std::string& url, bool asPreview = false);
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name = std::string());
static std::string makeRemoteUrl(const std::string& host, const std::string& path);
static bool ensureRemoteSession(const std::string& host, std::string& err, int port = 0);
static void remoteBrowseTo(const std::string& dir);
static void rbTreeForget();       // drop the tree's cached children
static std::string placeUrl(const std::string& host, int port, const std::string& path);
static void rbEnqueue(App::RbJob job);
static void pumpRemoteBrowse();
static void stopRbWorker();
static std::string bootstrapScript();
static std::string updateScript();
static void startRemote(const std::string& hostSpec);
static void drawPanelRemote();
static bool deployPeer(const std::string& host, int port, bool force, std::string& log);
static bool isNpyName(const std::string& n);
static void sortFramesNumerically(std::vector<std::string>& files);

static ImageDoc* cur() { return app.current >= 0 && app.current < (int)app.images.size() ? app.images[app.current].get() : nullptr; }

static void promotePreview(ImageDoc* d);   // fwd: preview -> registered open

// The B side of an A/B compare, or null when compare is off / B is gone / B is A.
static ImageDoc* resolveB() {
    if (app.compareBUid) {
        ImageDoc* b = nullptr;
        for (const auto& d : app.images)
            if (d->uid == app.compareBUid) { b = d.get(); break; }
        if (!b) { app.compareBUid = 0; return nullptr; }      // B was closed
        // B tracks A's frame number when BOTH sides are stacks. A single-image
        // B (a dark frame, a golden sample) has no frame axis and stays put -
        // which is the other half of what stack comparison has to support.
        ImageDoc* a = cur();
        if (app.compareFollowFrame && a && b->seqId != 0 && a->seqId != 0 &&
            b->seqId != a->seqId && b->seqIndex != a->seqIndex) {
            for (const auto& d : app.images)
                if (d->seqId == b->seqId && d->seqIndex == a->seqIndex) { b = d.get(); break; }
        }
        return b == cur() ? nullptr : b;
    }
    if (app.compareB.empty()) return nullptr;
    // session fallback: match the saved name (and frame, for a stack), then latch
    // the uid so navigation cannot re-point B at a different frame
    for (const auto& d : app.images) {
        if (d->name != app.compareB || d.get() == cur()) continue;
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

// remember B as an identity, not as a name
static void setCompareB(const ImageDoc* d) {
    // making a preview the B side IS using it: promote before it can vanish
    if (d && d->preview) promotePreview(const_cast<ImageDoc*>(d));
    app.compareBUid = d ? d->uid : 0;
    app.compareB = d ? d->name : std::string();
    app.compareBSeq = d && d->seqId != 0 ? d->seqIndex : -1;
}

// The B side FOR THE STATISTICS PANELS: the compare partner, but only when it
// has pixels to measure - a remote preview placeholder has none.
static ImageDoc* abStatsB() {
    ImageDoc* b = cmpB();
    if (!b || b->w < 1 || b->h < 1 || b->data.empty()) return nullptr;
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
    return !g_abNoThrottle && app.abStepBusyUntil > glfwGetTime();
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

static void toast(const std::string& msg, bool err = false) {
    app.toast = msg; app.toastErr = err;
    app.toastUntil = ImGui::GetTime() + (err ? 6.0 : 2.5);
    // A toast fades after six seconds, which is not long enough to paste it into
    // a bug report. Every message is kept here and shown, selectable, in the
    // Messages panel.
    if (app.msgLog.empty() || app.msgLog.back().text != msg) {
        app.msgLog.push_back({ msg, err });
        if (app.msgLog.size() > 300) app.msgLog.erase(app.msgLog.begin());
        if (err) app.msgUnreadErr = true;
    }
}

// One batch per OPEN ACTION: reopening a folder deliberately makes a fresh
// one - "calling the same folder twice" must give two groupings you can name
// apart, or the second load is invisible.
static int newBatch(const std::string& name) {
    app.batches.push_back({ app.nextBatchId, name.empty() ? "opened" : name });
    app.imagesRev++;                      // the Files grouping caches on this
    return app.nextBatchId++;
}
// Loose single-file opens share the folder-named batch instead (the old
// behavior, and the right one for "clicked three files in a row").
static int batchReuse(const std::string& name) {
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

static void selectImage(int idx);   // fwd (defined with the sequence helpers)

// Default B = the image next to A in the list: with two images open, "compare"
// should just work without first picking a partner.
static void ensureCompareB() {
    if (resolveB()) return;       // mode-independent: called before the mode changes
    setCompareB(nullptr);
    if (app.images.empty()) return;
    // the image you were just looking at is the one you mean to compare against
    // (after Process > demosaic that is the source, not images[0])
    if (app.prevImageUid)
        for (const auto& d : app.images)
            if (d->uid == app.prevImageUid && d.get() != cur()) { setCompareB(d.get()); return; }
    for (int i = 0; i < (int)app.images.size(); i++) {
        int j = (app.current + 1 + i) % (int)app.images.size();
        if (app.images[j].get() != cur()) { setCompareB(app.images[j].get()); break; }
    }
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

// Pin the frame you are looking at as B, then walk A somewhere else: this is how
// you compare frame 12 against frame 13 of one stack, or a source against its
// processed result.
static void pinCurrentAsB() {
    if (!cur()) return;
    setCompareB(cur());
    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
    toast("B = " + abDocLabel(cur()) + "  (move A somewhere else)");
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
    std::string an = abDocLabel(a), bn = abDocLabel(b);
    uint64_t bUid = b->uid;
    setCompareB(a);                       // set B before moving A: cur() changes
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->uid == bUid) { selectImage(i); break; }
    toast("A / B swapped:  A = " + bn + "   B = " + an);
}

static void computeMinMax(ImageDoc& im) {
    float mn = FLT_MAX, mx = -FLT_MAX;
    for (float v : im.data) {
        if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
    }
    if (mn > mx) { mn = 0; mx = 1; }
    if (mn == mx) mx = mn + 1;
    im.vmin = mn; im.vmax = mx;
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

    double t0 = glfwGetTime();
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
            (glfwGetTime() - t0) * 1000);
}

// upload/normalize into RGBA8 texture
static void rebuildTexture(ImageDoc& im) {
    // One scratch buffer for the whole app: a 12 Mpx image is a 48 MB allocation,
    // and this runs on every range/gamma/LUT change.
    static std::vector<uint8_t> rgba;
    rgba.resize((size_t)im.w * im.h * 4);
    const float ib = effBlack(im), iw = effWhite(im);
    im.texBlack = ib; im.texWhite = iw;
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
        const float* src = &im.data[p * im.ch];
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
// Linking is an overlay, never a rewrite: each image keeps its own range, so
// unlinking restores exactly what every image had before.
static ImageDoc* cmpB();          // fwd: the B side, or null when compare is off
// The A/B display range. Mode 2 fits BOTH sides to the union of what the two
// current frames actually contain, recomputed as you step - the "auto but the
// same auto on both sides" a stack-vs-stack comparison needs. cur() is never
// B (resolveB guarantees it), so none of this recurses.
static bool abRange(const ImageDoc& im, float& lo, float& hi) {
    if (app.compareRangeMode == 0) return false;
    ImageDoc* a = cur();
    ImageDoc* b = cmpB();
    if (!a || !b || (&im != a && &im != b)) return false;
    if (app.compareRangeMode == 1) { lo = a->black; hi = a->white; return true; }
    lo = std::min(a->vmin, b->vmin);          // union of the CONTENT, not of the
    hi = std::max(a->vmax, b->vmax);          // two stretches
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
    forgetTexture(im);
    app.imagesRev++;
}
// ---- background full-resolution fetch (remote frames) -------------------------
// The preview is already on screen; the real pixels arrive here on their own ssh
// connection and are swapped in by the UI thread.
static void rfWorker() {
    remote::Session ses;
    std::string sesHost = "\n";                  // impossible: force first connect
    while (!app.rfStop) {
        App::RFetchJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(app.rfMtx);
            if (!app.rfQueue.empty()) {
                job = std::move(app.rfQueue.front());
                app.rfQueue.erase(app.rfQueue.begin());
                have = true;
            }
        }
        if (!have) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        App::RFetchDone d;
        d.uid = job.uid;
        d.url = job.url; d.name = job.name;
        d.frame = job.frame; d.seqId = job.seqId; d.seqIndex = job.seqIndex;
        d.gen = job.gen;
        std::string host, rpath, err;
        if (!remote::parseUrl(job.url, host, rpath)) {
            d.err = "bad remote url";
        } else {
            if (!ses.alive() || sesHost != host) {
                if (!ses.start(host, job.exe, err)) d.err = err;
                else sesHost = host;
            }
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
    if (!app.rfThread.joinable()) app.rfThread = std::thread(rfWorker);
}

static void requestFullRemote(const ImageDoc* d, bool low = false) {
    if (d->remoteUrl.empty() || d->remoteStep <= 1) return;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
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
    j.url = d->remoteUrl; j.frame = d->remoteFrame; j.uid = d->uid;
    j.low = low;
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
        app.rfFetched++;
        if (app.rfPending <= 0) {
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
            doc->path = d.url;
            doc->remoteUrl = d.url;
            doc->remoteFrame = d.frame;
            doc->dtype = d.dtype;
            doc->w = d.w; doc->h = d.h; doc->ch = d.ch;
            doc->data = std::move(d.data);
            doc->vmin = d.vmin; doc->vmax = d.vmax;
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
        if (!d.err.empty()) {
            im->remoteErr = d.err;
            toast("remote: " + d.err, true);
            continue;
        }
        int stepBefore = im->remoteStep;
        im->data = std::move(d.data);
        im->w = d.w; im->h = d.h; im->ch = d.ch;
        im->dtype = d.dtype;
        im->remoteStep = 1;
        im->remoteErr.clear();
        size_t p = im->name.find("  (1/");       // drop the preview marker
        if (p != std::string::npos) im->name.erase(p);
        im->dataRev++;
        im->vmin = d.vmin; im->vmax = d.vmax;    // measured on the worker
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
    app.rfStop = true;
    if (app.rfThread.joinable()) app.rfThread.join();
}

// ---- background MEASURE (server-side analysis) --------------------------------
// Its own ssh connection: a 300-frame aggregate can hold the pipe for seconds,
// and that must not stall the tile fetches on the other worker.
static void mWorker() {
    remote::Session ses;
    std::string sesHost = "\n";
    while (!app.mStop) {
        App::MJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(app.mMtx);
            if (!app.mQueue.empty()) { job = std::move(app.mQueue.front());
                                       app.mQueue.erase(app.mQueue.begin()); have = true; }
        }
        if (!have) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        App::MDone d;
        d.token = job.token;
        std::string host, rpath, err;
        if (!remote::parseUrl(job.url, host, rpath)) { d.err = "bad remote url"; }
        else {
            d.host = host;
            if (!ses.alive() || sesHost != host) {
                std::string exe = app.remoteExe.empty()
                    ? (host.empty() ? app.exePath : std::string(REMOTE_HOME) + "/viewer-serve")
                    : app.remoteExe;
                if (!ses.start(host, exe, err)) d.err = err;
                else sesHost = host;
            }
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
    {
        std::lock_guard<std::mutex> lk(app.mMtx);
        app.mPending++;
        app.mQueue.push_back(std::move(job));
    }
    if (!app.mThread.joinable()) app.mThread = std::thread(mWorker);
}

static void stopMeasureWorker() {
    app.mStop = true;
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
        // CFA-all is column 0's un-suffixed keys; a mosaiced stack reports per
        // plane, and the panel shows the overall figure from those when present.
        S.mean = mFindNum(d.res, "mean [DN]", mFindNum(d.res, "mean [DN] R"));
        S.tempNoise = mFindNum(d.res, "sigma_t [DN]", mFindNum(d.res, "sigma_t [DN] R"));
        S.fixedPattern = mFindNum(d.res, "sigma_fpn [DN]", mFindNum(d.res, "sigma_fpn [DN] R"));
        S.totalNoise = mFindNum(d.res, "sigma_tot [DN]", mFindNum(d.res, "sigma_tot [DN] R"));
        if (const auto* fm = mFindSeries(d.res, "frame mean")) { S.idx = fm->xs; S.frameMean = fm->ys; }
        if (const auto* fs = mFindSeries(d.res, "frame std"))  { S.frameStd = fs->ys; }
        fprintf(stderr, "remote: server temporal over %d frames - sigma_t %.4g, "
                        "sigma_fpn %.4g [%s]\n", S.frames, S.tempNoise, S.fixedPattern,
                d.res.serverLoc ? "gpu" : "cpu");
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

// Close every frame of a stack plus its SeqInfo, and stop everything that
// would quietly regrow it: the sequence loader (pumpSequence stamps
// seqLoadingId on frames as they land), the remote prefetch queue, the
// linearity row, the server temporal result.
static void closeStack(int seqId) {
    if (seqId == 0) return;
    if (app.seqLoadingId == seqId) { stopSequenceLoader(); app.seqLoadingId = 0; }
    {   // queued remote prefetches. The ONE job the worker may be running right
        // now cannot be removed here - pumpRemoteFetch drops its result on
        // arrival instead (seqInfo(d.seqId) == nullptr). Both are needed.
        std::lock_guard<std::mutex> lk(app.rfMtx);
        int removed = 0;
        for (auto it = app.rfQueue.begin(); it != app.rfQueue.end();)
            if (it->seqId == seqId) { it = app.rfQueue.erase(it); removed++; }
            else ++it;
        app.rfPending -= removed;
        if (app.rfPending <= 0) { app.rfPending = 0; app.rfTotal = 0; app.rfFetched = 0; }
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
    for (int s : seqIds) closeStack(s);
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
    forgetImage(im);
    if (im->tex) glDeleteTextures(1, &im->tex);
    app.images.erase(app.images.begin() + app.current);
    app.current = app.images.empty() ? -1 : std::min(app.current, (int)app.images.size() - 1);
    app.fitRequested = true;
    app.imagesRev++;
    // Same rule as closeImages: a dangling B must not re-latch by NAME onto a
    // same-named frame of another stack (every stack has a frame_001.npy).
    // ensureCompareB keeps B != cur() so the UI cannot reach this today, but
    // this path duplicates closeImages' body and inherited the omission.
    if (!resolveB()) {
        app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
    }
    // the escape hatch emptied the stack: drop the SeqInfo and its bookkeeping
    // too, or a zero-frame stack haunts the linearity table
    if (seqId != 0 && framesOfSeq(seqId).empty()) closeStack(seqId);
    pruneEmptyBatches();
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
    for (int s : ids) closeStack(s);
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
    app.ana.img = nullptr;
    for (int k = 0; k < 2; k++) {     // both A and B slots
        app.hist[k] = App::HistState{};
        app.proj[k] = App::ProjState{};
        app.temporal[k] = App::TemporalState{};
    }
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
    // The batches go with their contents: an empty batch that survives Close
    // All keeps its NAME reserved, so reopening the same folder came back as
    // "multi (2)" - the uniquifier colliding with a ghost.
    app.batches.clear();
    app.loadBatchId = 0;
    app.previewUid = 0;
    app.previewFiles.clear();
    app.previewLabel.clear();
    app.current = -1;
    // compare state refers to docs that no longer exist; leaving it would let a
    // later file with the same name silently become B again
    app.compareMode = App::CmpOff;
    app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
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
    if (cur() && cur()->remoteStep > 1) {
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
    f.data = (void*)im.data.data();
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

static void addImage(std::unique_ptr<ImageDoc> im) {
    if (im->batchId == 0) {
        if (app.loadBatchId) im->batchId = app.loadBatchId;
        else {
            size_t sl = im->path.find_last_of("/\\");
            std::string dir = sl == std::string::npos ? std::string() : im->path.substr(0, sl);
            size_t s2 = dir.find_last_of("/\\");
            std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
            im->batchId = batchReuse(leaf.empty() ? "generated" : leaf);
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
    computeMinMax(*im);
    defaultRange(*im);            // own range is always meaningful; link only overlays it
    im->texDirty = true;
    app.images.push_back(std::move(im));
    if (!g_quietLoad || app.current < 0) {      // first image still gets shown
        app.current = (int)app.images.size() - 1;
        // the view is shared: only frame the very first image, keep it afterwards
        if (app.images.size() == 1 || app.fitOnSwitch) app.fitRequested = true;
    }
}

// ---------------------------------------------------------------- .npz (zip)
// Minimal zip reader for npz: central-directory walk, stored (0) and deflate (8),
// with zip64 sizes. Inflate comes from miniz.
static std::unique_ptr<ImageDoc> decodeNpyBuffer(const std::vector<uint8_t>& buf,
                                                 const std::string& path,
                                                 const std::string& displayName,
                                                 std::string& errOut, int frameIdx,
                                                 int& framesOut, int64_t& frameStrideOut);
struct NpzEntry { std::string name; size_t localOff, csize, usize; uint16_t method; };

static bool npzList(const std::vector<uint8_t>& buf, std::vector<NpzEntry>& out, std::string& err) {
    auto rd16 = [&](size_t o) { return (uint16_t)(buf[o] | buf[o + 1] << 8); };
    auto rd32 = [&](size_t o) {
        return (uint32_t)buf[o] | (uint32_t)buf[o + 1] << 8 |
               (uint32_t)buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24;
    };
    auto rd64 = [&](size_t o) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)buf[o + i] << (8 * i);
        return v;
    };
    if (buf.size() < 22) { err = "not a zip file"; return false; }
    size_t eocd = SIZE_MAX;
    size_t start = buf.size() > 65557 ? buf.size() - 65557 : 0;
    for (size_t i = buf.size() - 22 + 1; i-- > start;)
        if (rd32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == SIZE_MAX) { err = "not a zip file (no end record)"; return false; }
    uint64_t count = rd16(eocd + 10);
    uint64_t cdOff = rd32(eocd + 16);
    if (cdOff == 0xffffffffu || count == 0xffffu) {          // zip64
        if (eocd >= 20 && rd32(eocd - 20) == 0x07064b50) {
            uint64_t z64 = rd64(eocd - 20 + 8);
            if (z64 + 56 <= buf.size() && rd32((size_t)z64) == 0x06064b50) {
                count = rd64((size_t)z64 + 32);
                cdOff = rd64((size_t)z64 + 48);
            }
        }
    }
    size_t p = (size_t)cdOff;
    for (uint64_t i = 0; i < count && p + 46 <= buf.size(); i++) {
        if (rd32(p) != 0x02014b50) break;
        NpzEntry e{};
        e.method = rd16(p + 10);
        e.csize = rd32(p + 20);
        e.usize = rd32(p + 24);
        uint16_t nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
        e.localOff = rd32(p + 42);
        if (p + 46 + nlen > buf.size()) break;
        e.name.assign((const char*)&buf[p + 46], nlen);
        if (e.csize == 0xffffffffu || e.usize == 0xffffffffu || e.localOff == 0xffffffffu) {
            size_t ep = p + 46 + nlen, eend = ep + elen;      // zip64 extra field
            while (ep + 4 <= eend && ep + 4 <= buf.size()) {
                uint16_t id = rd16(ep), sz = rd16(ep + 2);
                if (id == 1) {
                    size_t q = ep + 4;
                    if (e.usize == 0xffffffffu) { e.usize = (size_t)rd64(q); q += 8; }
                    if (e.csize == 0xffffffffu) { e.csize = (size_t)rd64(q); q += 8; }
                    if (e.localOff == 0xffffffffu) { e.localOff = (size_t)rd64(q); }
                    break;
                }
                ep += 4 + sz;
            }
        }
        out.push_back(std::move(e));
        p += 46 + nlen + elen + clen;
    }
    if (out.empty()) { err = "zip contains no entries"; return false; }
    return true;
}

static bool npzExtract(const std::vector<uint8_t>& zip, const NpzEntry& e,
                       std::vector<uint8_t>& out, std::string& err) {
    if (e.localOff + 30 > zip.size()) { err = "corrupt local header"; return false; }
    auto rd16 = [&](size_t o) { return (uint16_t)(zip[o] | zip[o + 1] << 8); };
    size_t nlen = rd16(e.localOff + 26), elen = rd16(e.localOff + 28);
    size_t data = e.localOff + 30 + nlen + elen;
    if (data + e.csize > zip.size()) { err = "truncated zip member"; return false; }
    if (e.method == 0) {                                     // stored
        out.assign(zip.begin() + data, zip.begin() + data + e.csize);
        return true;
    }
    if (e.method != 8) { err = "unsupported zip compression method"; return false; }
    // zip members are RAW deflate (no zlib header), and the uncompressed size is
    // known from the directory, so decompress straight into the output buffer
    out.resize(e.usize);
    size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(),
                                             zip.data() + data, e.csize, 0);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { err = "inflate failed"; return false; }
    out.resize(got);
    return true;
}

// Load a .npy; an array with a frame axis becomes one stack (塊), which is what
// the temporal analysis operates on.
static std::unique_ptr<ImageDoc> decodeNpyFrame(const std::string& path, std::string& errOut,
                                                int frameIdx, int& framesOut,
                                                int64_t& frameStrideOut);   // defined below

// Shared by .npy files and .npz members: build a stack when the array has a
// frame axis, otherwise a single image.
static std::string loadNpyBuffer(const std::vector<uint8_t>& buf, const std::string& path,
                                 const std::string& displayName) {
    std::string err;
    int frames = 1;
    int64_t fstride = 0;
    auto first = decodeNpyBuffer(buf, path, displayName, err, 0, frames, fstride);
    if (!first) return err.empty() ? "decode failed" : err;
    std::string label = first->name;
    if (frames <= 1) { addImage(std::move(first)); return {}; }

    App::SeqInfo info;
    info.id = app.nextSeqId++;
    info.name = label + "  (" + std::to_string(frames) + " frames)";
    app.seqs.push_back(info);
    first->seqId = info.id;
    first->seqIndex = 0;
    int firstIdx = (int)app.images.size();
    addImage(std::move(first));
    const ImageDoc* ref = app.images[firstIdx].get();
    for (int f = 1; f < frames; f++) {
        std::string e2;
        int fr = 1; int64_t fs = 0;
        auto doc = decodeNpyBuffer(buf, path, displayName, e2, f, fr, fs);
        if (!doc) { toast(label + ": frame " + std::to_string(f) + ": " + e2, true); break; }
        doc->seqId = info.id;
        doc->seqIndex = f;
        computeMinMax(*doc);
        doc->black = ref->black; doc->white = ref->white;   // frames stay comparable
        doc->texDirty = true;
        doc->uid = app.nextUid++;
        app.imagesRev++;
        app.images.push_back(std::move(doc));
    }
    for (auto& s : app.seqs)
        if (s.id == info.id) s.lastImageIdx = firstIdx;
    int got = 0;
    for (const auto& d : app.images) if (d->seqId == info.id) got++;
    fprintf(stderr, "npy stack: %s - %d frames (%dx%d %dch)\n", label.c_str(), got,
            ref->w, ref->h, ref->ch);
    return {};
}

// onlyMember != "" restores a single array (sessions record which one).
static std::string loadNpz(const std::string& path, const std::string& onlyMember = "") {
    std::vector<uint8_t> zip;
    if (!readFileBytes(path, zip)) return "cannot read file";
    std::vector<NpzEntry> entries;
    std::string err;
    if (!npzList(zip, entries, err)) return err;
    int loaded = 0, stored = 0, deflated = 0;
    for (const auto& e : entries) {
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".npy") != 0) continue;
        std::string arrayName = e.name.substr(0, e.name.size() - 4);
        if (!onlyMember.empty() && arrayName != onlyMember) continue;
        std::vector<uint8_t> member;
        std::string mErr;
        if (!npzExtract(zip, e, member, mErr)) { toast(e.name + ": " + mErr, true); continue; }
        std::string label = baseName(path) + ":" + arrayName;
        size_t before = app.images.size();
        std::string lErr = loadNpyBuffer(member, path, label);
        if (!lErr.empty()) { toast(label + ": " + lErr, true); continue; }
        for (size_t k = before; k < app.images.size(); k++)
            app.images[k]->npzMember = arrayName;    // identity for session restore
        loaded++;
        (e.method == 0 ? stored : deflated)++;
    }
    if (!loaded) return "no readable arrays in npz";
    fprintf(stderr, "npz: %s - %d array(s) (%d stored, %d deflate)\n",
            baseName(path).c_str(), loaded, stored, deflated);
    return {};
}

static std::string loadNpy(const std::string& path) {
    std::string err;
    int frames = 1;
    int64_t fstride = 0;
    auto first = decodeNpyFrame(path, err, 0, frames, fstride);
    if (!first) return err.empty() ? "decode failed" : err;
    if (frames <= 1) { addImage(std::move(first)); return {}; }

    App::SeqInfo info;
    info.id = app.nextSeqId++;
    info.name = baseName(path) + "  (" + std::to_string(frames) + " frames)";
    app.seqs.push_back(info);
    first->seqId = info.id;
    first->seqIndex = 0;
    int firstIdx = (int)app.images.size();
    addImage(std::move(first));
    info.lastImageIdx = firstIdx;
    const ImageDoc* ref = app.images[firstIdx].get();
    for (int f = 1; f < frames; f++) {
        std::string e2;
        int fr = 1; int64_t fs = 0;
        auto doc = decodeNpyFrame(path, e2, f, fr, fs);
        if (!doc) { toast(baseName(path) + ": frame " + std::to_string(f) + ": " + e2, true); break; }
        doc->seqId = info.id;
        doc->seqIndex = f;
        computeMinMax(*doc);
        doc->black = ref->black; doc->white = ref->white;   // frames stay comparable
        doc->texDirty = true;
        doc->uid = app.nextUid++;
        app.imagesRev++;
        app.images.push_back(std::move(doc));
    }
    for (auto& s : app.seqs)
        if (s.id == info.id) s.lastImageIdx = firstIdx;
    int got = 0;
    for (const auto& d : app.images) if (d->seqId == info.id) got++;
    fprintf(stderr, "npy stack: %s - %d frames (%dx%d %dch)\n", baseName(path).c_str(), got,
            ref->w, ref->h, ref->ch);
    return {};
}

static void runProcessor(int idx) {
    ImageDoc* im = cur();
    if (!im || idx < 0 || idx >= (int)plugin_host::processors().size()) return;
    const ProcessorPluginInfo& p = plugin_host::processors()[idx];
    psFrame in = makeFrame(*im), out = {};
    char err[256] = { 0 };
    if (p.v.process(&in, &out, plugin_host::hostApi(), err, sizeof err) != 0) {
        toast(p.name + ": " + (err[0] ? err : "failed"), true);
        return;
    }
    // contract validation — a misbehaving plugin must never crash the host
    if (!out.data || out.dtype != PS_DTYPE_F32 || out.loc != PS_MEM_CPU ||
        out.ch < 1 || out.ch > 4 || out.w < 1 || out.h < 1 || out.w > 32768 || out.h > 32768 ||
        out.pitch_bytes < (size_t)out.w * out.ch * sizeof(float)) {
        if (out.data) plugin_host::frameFree(out.data);
        toast(p.name + ": plugin returned an invalid frame", true);
        return;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = im->name + " [" + p.name + "]";
    doc->w = (int)out.w; doc->h = (int)out.h; doc->ch = (int)out.ch;
    doc->dtype = "f32";
    doc->note = "processed by " + p.name;
    doc->cfa = out.cfa_type; doc->cfaPattern = out.cfa_pattern & 3;
    doc->data.resize((size_t)out.w * out.h * out.ch);
    size_t rowFloats = (size_t)out.w * out.ch;
    for (uint32_t y = 0; y < out.h; y++)   // pitch-aware copy into the ImageDoc
        memcpy(doc->data.data() + (size_t)y * rowFloats,
               (const char*)out.data + (size_t)y * out.pitch_bytes, rowFloats * sizeof(float));
    plugin_host::frameFree(out.data);      // host frees, always
    float bk = out.black, wt = out.white;
    // processed results have no file path: sessions skip them (v2: re-run recipe)
    addImage(std::move(doc));
    if (wt > bk) { cur()->black = bk; cur()->white = wt; cur()->texDirty = true; }
    toast("processed: " + cur()->name);
}

// ---------------------------------------------------------------- npy loader
// returns error string, empty = ok
// Pure decoder: no app state, no GL — safe to call from the sequence loader thread.
// framesOut/frameStrideOut report a frame axis (F,H,W[,C]); frameIdx selects one.
static std::unique_ptr<ImageDoc> decodeNpyBuffer(const std::vector<uint8_t>& buf,
                                                 const std::string& path,
                                                 const std::string& displayName,
                                                 std::string& errOut, int frameIdx,
                                                 int& framesOut, int64_t& frameStrideOut) {
    auto fail = [&](const char* m) { errOut = m; return std::unique_ptr<ImageDoc>(); };
    framesOut = 1;
    frameStrideOut = 0;
    if (buf.size() < 10 || buf[0] != 0x93 || memcmp(&buf[1], "NUMPY", 5) != 0)
        return fail("not a .npy file (bad magic)");
    int major = buf[6];
    size_t hlen, hoff;
    if (major >= 2) {
        if (buf.size() < 12) return fail("corrupt npy header");
        uint32_t v; memcpy(&v, &buf[8], 4); hlen = v; hoff = 12;
    } else {
        uint16_t v; memcpy(&v, &buf[8], 2); hlen = v; hoff = 10;
    }
    if (hoff + hlen > buf.size()) return fail("corrupt npy header");
    std::string hdr((char*)&buf[hoff], hlen);

    auto findQuoted = [&](const char* key) -> std::string {
        size_t k = hdr.find(key);
        if (k == std::string::npos) return {};
        size_t q1 = hdr.find('\'', hdr.find(':', k));
        if (q1 == std::string::npos) return {};
        size_t q2 = hdr.find('\'', q1 + 1);
        return hdr.substr(q1 + 1, q2 - q1 - 1);
    };
    std::string descr = findQuoted("'descr'");
    if (descr.empty()) return fail("cannot parse descr");
    bool fortran = hdr.find("'fortran_order': True") != std::string::npos;

    size_t sp = hdr.find("'shape'");
    size_t p1 = hdr.find('(', sp), p2 = hdr.find(')', sp);
    if (p1 == std::string::npos || p2 == std::string::npos) return fail("cannot parse shape");
    std::vector<int64_t> shape;
    {
        std::string s = hdr.substr(p1 + 1, p2 - p1 - 1);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t c = s.find(',', pos);
            std::string tok = s.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
            if (tok.find_first_of("0123456789") != std::string::npos)
                shape.push_back(std::stoll(tok));
            if (c == std::string::npos) break;
            pos = c + 1;
        }
    }
    if (shape.empty()) shape.push_back(1);

    char bo = '<';
    std::string code = descr;
    if (!code.empty() && (code[0] == '<' || code[0] == '>' || code[0] == '|' || code[0] == '=')) {
        bo = code[0]; code = code.substr(1);
    }
    bool be = (bo == '>');
    int esize = 0;
    std::string dtypeName;
    if      (code == "u1") { esize = 1; dtypeName = "u8"; }
    else if (code == "i1") { esize = 1; dtypeName = "i8"; }
    else if (code == "b1") { esize = 1; dtypeName = "bool"; }
    else if (code == "u2") { esize = 2; dtypeName = "u16"; }
    else if (code == "i2") { esize = 2; dtypeName = "i16"; }
    else if (code == "u4") { esize = 4; dtypeName = "u32"; }
    else if (code == "i4") { esize = 4; dtypeName = "i32"; }
    else if (code == "f4") { esize = 4; dtypeName = "f32"; }
    else if (code == "f8") { esize = 8; dtypeName = "f64"; }
    else { errOut = "unsupported dtype: " + descr; return {}; }

    size_t count = 1;
    for (int64_t d : shape) count *= (size_t)d;
    if (hoff + hlen + count * esize > buf.size()) return fail("file too small for shape");
    const uint8_t* raw = &buf[hoff + hlen];

    auto bswap = [&](uint64_t v, int n) -> uint64_t {
        uint64_t r = 0;
        for (int i = 0; i < n; i++) r = (r << 8) | ((v >> (8 * i)) & 0xff);
        return r;
    };
    auto getVal = [&](size_t i) -> float {
        const uint8_t* p = raw + i * esize;
        switch (esize) {
        case 1:
            if (code == "i1") return (float)*(int8_t*)p;
            return (float)*p;
        case 2: {
            uint16_t u; memcpy(&u, p, 2);
            if (be) u = (uint16_t)bswap(u, 2);
            return code == "i2" ? (float)(int16_t)u : (float)u;
        }
        case 4: {
            uint32_t u; memcpy(&u, p, 4);
            if (be) u = (uint32_t)bswap(u, 4);
            if (code == "f4") { float f; memcpy(&f, &u, 4); return f; }
            return code == "i4" ? (float)(int32_t)u : (float)u;
        }
        case 8: {
            uint64_t u; memcpy(&u, p, 8);
            if (be) u = bswap(u, 8);
            double d; memcpy(&d, &u, 8); return (float)d;
        }
        }
        return 0;
    };

    // strides in elements
    std::vector<int64_t> strides(shape.size());
    if (fortran) { int64_t s = 1; for (size_t i = 0; i < shape.size(); i++) { strides[i] = s; s *= shape[i]; } }
    else         { int64_t s = 1; for (int i = (int)shape.size() - 1; i >= 0; i--) { strides[i] = s; s *= shape[i]; } }

    // Layout decision. A leading axis that is not a plausible channel count is a
    // FRAME axis: (F,H,W) / (F,H,W,1) / (F,H,W,C) load as a stack, not as one
    // image. app.npyAxis can force the ambiguous small-leading-axis case.
    std::string note;
    int64_t F = 1, sf = 0;
    while (shape.size() > 4) {          // deeper than (F,H,W,C): take [0]
        if (shape[0] != 1) note = "showing [0] of leading axis";
        shape.erase(shape.begin());
        strides.erase(strides.begin());
    }
    if (shape.size() == 4) {            // (F,H,W,C) or (F,C,H,W)
        F = shape[0]; sf = strides[0];
        shape.erase(shape.begin());
        strides.erase(strides.begin());
    } else if (shape.size() == 3) {
        bool lastIsChannels = shape[2] <= 4;
        bool firstIsChannels = shape[0] <= 4;
        bool asFrames = !lastIsChannels &&
                        (!firstIsChannels || app.npyAxis == 1);   // 1 = force frames
        if (asFrames || (firstIsChannels && app.npyAxis == 1)) {
            F = shape[0]; sf = strides[0];
            shape.erase(shape.begin());
            strides.erase(strides.begin());
        }
    }
    int64_t H, W, C, sh, sw, sc;
    if (shape.size() == 1) { H = 1; W = shape[0]; C = 1; sh = 0; sw = strides[0]; sc = 0; }
    else if (shape.size() == 2) { H = shape[0]; W = shape[1]; C = 1; sh = strides[0]; sw = strides[1]; sc = 0; }
    else {
        if (shape[2] <= 4)      { H = shape[0]; W = shape[1]; C = shape[2]; sh = strides[0]; sw = strides[1]; sc = strides[2]; }
        else if (shape[0] <= 4) { C = shape[0]; H = shape[1]; W = shape[2]; sc = strides[0]; sh = strides[1]; sw = strides[2];
                                  note = note.empty() ? "CHW->HWC" : note + ", CHW->HWC"; }
        else return fail("shape not interpretable as image");
    }
    if (W < 1 || H < 1 || W > 32768 || H > 32768) return fail("unsupported image size");
    if (F > 1) {                        // multi-frame: caller turns this into a stack
        framesOut = (int)F;
        frameStrideOut = sf;
        note = note.empty() ? "frame axis" : note + ", frame axis";
    }

    auto im = std::make_unique<ImageDoc>();
    im->name = displayName.empty() ? baseName(path) : displayName;
    im->path = path;
    im->w = (int)W; im->h = (int)H; im->ch = (int)C;
    im->dtype = dtypeName; im->note = note;
    im->data.resize((size_t)W * H * C);
    size_t di = 0;
    for (int64_t y = 0; y < H; y++)
        for (int64_t x = 0; x < W; x++)
            for (int64_t c = 0; c < C; c++)
                im->data[di++] = getVal((size_t)(frameIdx * sf + y * sh + x * sw + c * sc));
    return im;
}

static std::unique_ptr<ImageDoc> decodeNpyFrame(const std::string& path, std::string& errOut,
                                                int frameIdx, int& framesOut,
                                                int64_t& frameStrideOut) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) { errOut = "cannot read file"; return {}; }
    return decodeNpyBuffer(buf, path, "", errOut, frameIdx, framesOut, frameStrideOut);
}
static std::unique_ptr<ImageDoc> decodeNpy(const std::string& path, std::string& errOut) {
    int frames = 1; int64_t fstride = 0;
    return decodeNpyFrame(path, errOut, 0, frames, fstride);
}

// ---------------------------------------------------------------- raw loader
// Two orthogonal axes:
//   dtype  = how ONE sample is stored in the file (u8/u16/f32/f64 + endian)
//   interp = what the samples MEAN (gray / RGB / BGR / RGBA / BGRA / Bayer / Quad Bayer)
// Any combination is valid (e.g. RGGB float32).
enum RawDtype  { RD_U8, RD_U16, RD_F32, RD_F64, RD_COUNT };
static const char* RAW_DTYPE_NAMES[] = { "u8", "u16", "f32", "f64" };
static const int   RAW_DTYPE_SIZE[]  = { 1, 2, 4, 8 };
enum RawInterp { RI_GRAY, RI_RGB, RI_BGR, RI_RGBA, RI_BGRA, RI_BAYER, RI_QUAD, RI_COUNT };
static const char* RAW_INTERP_NAMES[] =
    { "Gray (1ch)", "RGB (3ch)", "BGR (3ch)", "RGBA (4ch)", "BGRA (4ch)",
      "Bayer (1ch CFA)", "Quad Bayer (1ch CFA)" };
static const char* RAW_INTERP_CLI[] = { "gray", "rgb", "bgr", "rgba", "bgra", "bayer", "quad-bayer" };
static const int   RAW_INTERP_CH[]  = { 1, 3, 3, 4, 4, 1, 1 };

// legacy combined names (--raw-format, session v1 "raw" lines) -> (dtype, interp)
static const int LEGACY_RAW_MAP[][2] = {
    { RD_U8,  RI_GRAY }, { RD_U16, RI_GRAY }, { RD_F32, RI_GRAY }, { RD_F64, RI_GRAY },
    { RD_U8,  RI_RGB  }, { RD_U8,  RI_BGR  }, { RD_U8,  RI_RGBA }, { RD_U8,  RI_BGRA },
    { RD_U16, RI_RGB  }, { RD_F32, RI_RGB  }, { RD_U8,  RI_BAYER }, { RD_U16, RI_BAYER },
};
static const char* LEGACY_RAW_NAMES[] = { "gray8", "gray16", "grayf32", "grayf64", "rgb8", "bgr8",
                                          "rgba8", "bgra8", "rgb16", "rgbf32", "bayer8", "bayer16" };

struct RawDialog {
    bool open = false;
    bool forQueue = false;            // settings will be applied to every queued stack
    int queueCount = 0;               // stacks waiting on these settings (for the prompt)
    std::string path;
    size_t fileSize = 0;
    int w = 1920, h = 1080, offset = 0;
    int dtype = RD_U8, interp = RI_GRAY;
    bool littleEndian = true;
    int cfaPattern = 0;               // RGGB/BGGR/GRBG/GBRG
    int replaceIdx = -1;              // >=0: reload INTO this image slot (reinterpret)
    bool cropOn = false;              // decode only a window of the source frame
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    std::vector<std::pair<int,int>> guesses;
} rawDlg;
// raw settings captured once and reused for every stack queued by "Open Folder"
static RawDialog g_folderRecipe;
static void openRawDialogFor(const std::string& path);

static void rawGuessDims(RawDialog& d) {
    d.guesses.clear();
    size_t bpp = (size_t)RAW_DTYPE_SIZE[d.dtype] * RAW_INTERP_CH[d.interp];
    size_t nbytes = d.fileSize > (size_t)d.offset ? d.fileSize - d.offset : 0;
    if (!bpp || nbytes % bpp != 0) return;   // no exact pixel count -> no candidates
    int64_t n = (int64_t)(nbytes / bpp);
    static const int commons[][2] = { {3840,2160},{1920,1080},{1280,720},{640,480},{512,512},{1024,1024},
        {2048,2048},{4096,4096},{256,256},{2560,1440},{720,480},{640,360},{320,240},{128,128} };
    for (auto& c : commons)
        if ((int64_t)c[0] * c[1] == n)
            d.guesses.push_back({ c[0], c[1] });
    for (int64_t w = 16; w <= 8192 && (int)d.guesses.size() < 30; w++) {
        if (n % w == 0) {
            int64_t h = n / w;
            if (h >= 16 && h <= 8192 && w <= h * 8 && h <= w * 8) {
                bool dup = false;
                for (auto& g : d.guesses) if (g.first == w && g.second == h) dup = true;
                if (!dup) d.guesses.push_back({ (int)w, (int)h });
            }
        }
    }
}

// Pure decoder: no app state, no GL — safe on the sequence loader thread.
static std::unique_ptr<ImageDoc> decodeRawFrame(const RawDialog& d, std::string& errOut) {
    auto fail = [&](const char* m) { errOut = m; return std::unique_ptr<ImageDoc>(); };
    if (d.dtype < 0 || d.dtype >= RD_COUNT || d.interp < 0 || d.interp >= RI_COUNT)
        return fail("invalid raw format");
    if (d.w < 1 || d.h < 1 || d.w > 32768 || d.h > 32768)
        return fail("unsupported image size");   // sessions can carry unclamped values
    std::vector<uint8_t> buf;
    if (!readFileBytes(d.path, buf)) return fail("cannot read file");
    const int elem = RAW_DTYPE_SIZE[d.dtype];
    const int ch = RAW_INTERP_CH[d.interp];
    size_t px = (size_t)d.w * d.h;
    size_t count = px * ch;
    if (count * elem + d.offset > buf.size()) return fail("file too small for this size/format");
    const uint8_t* p = buf.data() + d.offset;
    bool le = d.littleEndian;

    auto rd = [&](size_t i) -> float {   // one sample, any dtype/endian -> float
        const uint8_t* q = p + i * elem;
        switch (d.dtype) {
        case RD_U8:  return q[0];
        case RD_U16: return le ? (float)(q[0] | q[1] << 8) : (float)(q[0] << 8 | q[1]);
        case RD_F32: {
            uint32_t u = le ? (uint32_t)q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24
                            : (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
            float f; memcpy(&f, &u, 4); return f;
        }
        case RD_F64: {
            uint64_t u = 0;
            for (int b = 0; b < 8; b++) u |= (uint64_t)q[le ? b : 7 - b] << (8 * b);
            double v; memcpy(&v, &u, 8); return (float)v;
        }
        }
        return 0;
    };

    // optional crop window (decode only the window; source layout is full-frame)
    int cx = 0, cy = 0, outW = d.w, outH = d.h;
    if (d.cropOn && d.cropW > 0 && d.cropH > 0) {
        cx = std::clamp(d.cropX, 0, d.w - 1);
        cy = std::clamp(d.cropY, 0, d.h - 1);
        if (d.interp == RI_BAYER) { cx &= ~1; cy &= ~1; }        // keep CFA phase
        if (d.interp == RI_QUAD)  { cx &= ~3; cy &= ~3; }
        outW = std::clamp(d.cropW, 1, d.w - cx);
        outH = std::clamp(d.cropH, 1, d.h - cy);
    }

    auto im = std::make_unique<ImageDoc>();
    im->name = baseName(d.path); im->path = d.path;
    im->w = outW; im->h = outH; im->ch = ch;
    im->dtype = RAW_DTYPE_NAMES[d.dtype];
    im->data.resize((size_t)outW * outH * ch);
    float* out = im->data.data();
    bool sw = d.interp == RI_BGR || d.interp == RI_BGRA;   // channel 0/2 swap
    for (int y = 0; y < outH; y++)
        for (int x = 0; x < outW; x++) {
            size_t src = ((size_t)(cy + y) * d.w + (cx + x)) * ch;
            size_t dst = ((size_t)y * outW + x) * ch;
            for (int c = 0; c < ch; c++) {
                int oc = (sw && c == 0) ? 2 : (sw && c == 2) ? 0 : c;
                out[dst + oc] = rd(src + c);
            }
        }
    im->srcW = d.w; im->srcH = d.h;
    im->cropX = cx; im->cropY = cy;
    im->note = std::string(RAW_INTERP_NAMES[d.interp]) + " " + RAW_DTYPE_NAMES[d.dtype];
    if (d.interp == RI_BAYER || d.interp == RI_QUAD) {
        im->cfa = d.interp == RI_QUAD ? 2 : 1;
        im->cfaPattern = d.cfaPattern & 3;
        im->note = std::string(d.interp == RI_QUAD ? "Quad Bayer " : "Bayer ")
                 + CFA_PATTERNS[im->cfaPattern] + " " + RAW_DTYPE_NAMES[d.dtype];
    }
    im->rawDtype = d.dtype;           // remember raw params: sessions + reinterpret
    im->rawInterp = d.interp;
    im->rawOffset = d.offset;
    im->rawLE = d.littleEndian;
    return im;
}

static std::string loadRaw(const RawDialog& d) {
    std::string err;
    auto im = decodeRawFrame(d, err);
    if (!im) return err.empty() ? "decode failed" : err;

    if (d.replaceIdx >= 0 && d.replaceIdx < (int)app.images.size()) {
        // reinterpret in place: keep list position, selection and view
        ImageDoc* old = app.images[d.replaceIdx].get();
        if (app.ana.img == old) app.ana.img = nullptr;
        if (old->tex) glDeleteTextures(1, &old->tex);
        computeMinMax(*im);
        defaultRange(*im);
        im->texDirty = true;
        forgetImage(old);
        im->uid = app.nextUid++;
        app.images[d.replaceIdx] = std::move(im);
        app.current = d.replaceIdx;
    } else {
        addImage(std::move(im));
    }
    return {};
}

// ---------------------------------------------------------------- dynamic crop
// Crop the in-memory data (no file IO); origin snaps to the CFA period so the
// pattern stays valid. appliedX/Y report the snapped origin.
static void cropInPlace(ImageDoc& im, int x, int y, int w, int h,
                        int* appliedX = nullptr, int* appliedY = nullptr) {
    if (im.srcW == 0) { im.srcW = im.w; im.srcH = im.h; }
    x = std::clamp(x, 0, im.w - 1);
    y = std::clamp(y, 0, im.h - 1);
    if (im.cfa == 1) { x &= ~1; y &= ~1; }
    if (im.cfa == 2) { x &= ~3; y &= ~3; }
    w = std::clamp(w, 1, im.w - x);
    h = std::clamp(h, 1, im.h - y);
    std::vector<float> nd((size_t)w * h * im.ch);
    for (int yy = 0; yy < h; yy++)
        memcpy(&nd[(size_t)yy * w * im.ch],
               &im.data[((size_t)(y + yy) * im.w + x) * im.ch],
               (size_t)w * im.ch * sizeof(float));
    im.data = std::move(nd);
    im.w = w; im.h = h;
    im.cropX += x; im.cropY += y;
    im.dataRev++;
    computeMinMax(im);
    im.texDirty = true;
    if (appliedX) *appliedX = x;
    if (appliedY) *appliedY = y;
}

static void shiftAnnotations(int dx, int dy) {
    if (app.anns.empty() || (dx == 0 && dy == 0)) return;
    for (auto& a : app.anns) { a.x += dx; a.y += dy; }
    app.annRev++;
}

static bool isCropped(const ImageDoc& im) {
    return im.srcW > 0 && (im.w != im.srcW || im.h != im.srcH || im.cropX != 0 || im.cropY != 0);
}

static void cropCurrentToSelectedRoi() {
    ImageDoc* im = cur();
    App::Ann* sel = findAnn(app.selectedAnn);
    if (!im || !sel || sel->type != 0) return;
    int ax = 0, ay = 0;
    cropInPlace(*im, sel->x, sel->y, sel->w, sel->h, &ax, &ay);
    shiftAnnotations(-ax, -ay);        // annotations follow into the cropped frame
    if (app.ana.img == im) app.ana.img = nullptr;
    app.fitRequested = true;
    toast("cropped to " + sel->label);
}

static void restoreFull() {
    ImageDoc* im = cur();
    if (!im || !isCropped(*im)) return;
    int sx = im->cropX, sy = im->cropY;
    int idx = app.current;
    float ob = im->black, ow = im->white;
    if (im->rawDtype >= 0) {
        RawDialog d;
        d.path = im->path;
        d.dtype = im->rawDtype;
        d.interp = im->rawInterp;
        if (RAW_INTERP_CH[d.interp] == 1)
            d.interp = im->cfa == 2 ? RI_QUAD : im->cfa == 1 ? RI_BAYER : RI_GRAY;
        d.w = im->srcW; d.h = im->srcH;
        d.offset = im->rawOffset; d.littleEndian = im->rawLE;
        d.cfaPattern = im->cfaPattern & 3;
        d.replaceIdx = idx;
        std::string err = loadRaw(d);
        if (!err.empty()) { toast("restore failed: " + err, true); return; }
    } else if (!im->path.empty()) {
        int before = (int)app.images.size();
        std::string err = loadNpy(im->path);
        if (!err.empty() || (int)app.images.size() == before) {
            toast("restore failed: " + (err.empty() ? std::string("reload error") : err), true);
            return;
        }
        auto doc = std::move(app.images.back());   // loadNpy appended; move into old slot
        app.images.pop_back();
        ImageDoc* old = app.images[idx].get();
        doc->cfa = old->cfa; doc->cfaPattern = old->cfaPattern;
        doc->cfaColorize = old->cfaColorize;
        doc->texDirty = true;
        if (app.ana.img == old) app.ana.img = nullptr;
        if (old->tex) glDeleteTextures(1, &old->tex);
        forgetImage(old);
        doc->uid = app.nextUid++;
        app.images[idx] = std::move(doc);
        app.current = idx;
    } else {
        toast("no source file to restore from (processed image)", true);
        return;
    }
    cur()->black = ob; cur()->white = ow; cur()->texDirty = true;   // keep user range
    shiftAnnotations(sx, sy);
    app.fitRequested = true;
    toast("restored full frame");
}

static void openRemote(const std::string& url, bool asPreview);   // fwd (default on the first decl)
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name);   // default lives on the first decl
static std::string makeRemoteUrl(const std::string& host, const std::string& path);
// ---------------------------------------------------------------- session save/load
// (sequence helpers are defined further down; sessions can restore a stack)
static std::vector<std::string> findSequenceSiblings(const std::string& path,
                                                     std::string& patternOut);
static void startSequenceLoad(int imageIdx, const std::vector<std::string>& files,
                              const std::string& pattern);
static void stopSequenceLoader();
static std::vector<int> framesOfSeq(int seqId);
static App::SeqInfo* seqInfo(int id);
static void selectImage(int idx);
// Plain line-based text format (.vsession): view state + per-image reload recipes.
// Rendering is separate from writing so the crash handler can keep a pre-rendered
// copy and never has to build one while the process is already dying.
static void writeSessionTo(std::ostream& f, int* lostSeriesOut = nullptr,
                           int* lostMembersOut = nullptr) {
    f << "viewer-session 1\n";
    f << std::setprecision(9);        // 6 digits silently rounded measured values
    f << "gamma " << app.dispGamma << "\n";
    f << "grid " << (app.showGrid ? 1 : 0) << "\n";
    f << "zoom " << app.view.zoom << "\n";
    f << "center " << app.view.center.x << " " << app.view.center.y << "\n";
    // One image line per stack means saved-line order != app.images order, and a
    // restored stack expands to N frames, so a raw index cannot survive. Save the
    // ordinal of the line that carries the current image instead.
    // A stack contributes exactly one line: the frame it was left on.
    auto isSavedLine = [&](const ImageDoc* d) {
        if (d->path.empty()) return false;
        if (d->seqId == 0) return true;
        int rep = -1;
        if (App::SeqInfo* si = seqInfo(d->seqId))
            if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
                app.images[si->lastImageIdx]->seqId == d->seqId)
                rep = si->lastImageIdx;
        if (rep < 0) {                              // never navigated: first frame
            std::vector<int> fr = framesOfSeq(d->seqId);
            if (!fr.empty()) rep = fr.front();
        }
        return rep >= 0 && app.images[rep].get() == d;
    };
    auto savedLineOf = [&](const ImageDoc* d) {
        int ord = 0;
        for (const auto& o : app.images) {
            if (!isSavedLine(o.get())) continue;
            // the current image may not be its stack's saved frame, but it lands
            // on that stack's line either way
            if (o.get() == d || (d->seqId != 0 && o->seqId == d->seqId)) return ord;
            ord++;
        }
        return 0;
    };
    f << "current " << (cur() ? savedLineOf(cur()) : 0) << "\n";
    // display state that lives outside the per-image range
    f << "linkrange " << (app.linkRange ? 1 : 0) << " " << app.linkBlack << " "
      << app.linkWhite << "\n";
    f << "rangescope " << app.rangeScope << "\n";
    f << "cmpfollow " << (app.compareFollowFrame ? 1 : 0) << "\n";
    f << "cmprange " << app.compareRangeMode << "\n";
    f << "abstats " << app.abStatsLayout << " " << app.histPlane << "\n";
    f << "roichannel " << app.roiChannel << "\n";
    f << "projection " << app.projMode << " " << app.projYMode << " " << app.projYLo << " "
      << app.projYHi << " " << (app.showProjH ? 1 : 0) << " " << (app.showProjV ? 1 : 0) << "\n";
    f << "analysis " << app.anaSel << " " << (app.anaAuto ? 1 : 0) << " "
      << (app.anaRefOn ? 1 : 0) << " " << app.anaRef << "\n";
    f << "histlog " << (app.histLog ? 1 : 0) << "\n";
    // must precede the image lines: it decides whether (F,H,W) reloads as a stack
    f << "npyaxis " << app.npyAxis << "\n";
    f << "compare " << app.compareMode << " " << app.wipeFrac << " " << app.splitFrac << " "
      << app.diffGain << " " << (app.diffAbs ? 1 : 0) << " "
      << (app.flipAuto ? 1 : 0) << " " << app.flipPeriod << "\n";
    // uids are per-run, so the file carries name + frame index; the name is last
    // because it may contain spaces
    if (ImageDoc* b = resolveB())
        f << "compareb " << (b->seqId != 0 ? b->seqIndex : -1) << " " << b->name << "\n";
    f << "panels " << (app.showFiles ? 1 : 0) << " " << (app.showInspector ? 1 : 0) << " "
      << (app.showRois ? 1 : 0) << " " << (app.showAnalysis ? 1 : 0) << " "
      << (app.showHistogram ? 1 : 0) << " " << (app.showTemporal ? 1 : 0) << " "
      << (app.showProjection ? 1 : 0) << " " << (app.showLinearity ? 1 : 0) << " "
      << (app.showRemote ? 1 : 0) << "\n";
    // the whole dock arrangement, so a session reopens looking like it did
    if (const char* ini = ImGui::SaveIniSettingsToMemory()) {
        f << "layout_begin\n";
        for (const char* p = ini; *p; ) {          // indent so parsing stays trivial
            const char* e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            f << "|" << std::string(p, n) << "\n";
            if (!e) break;
            p = e + 1;
        }
        f << "layout_end\n";
    }
    for (auto& d : app.images) {
        // Sequences: one line per STACK (the frame that stack was left on), not
        // one per frame - and not only the stack that happens to be on screen.
        if (!isSavedLine(d.get())) continue;
        if (!d->npzMember.empty()) f << "member " << d->npzMember << "\n";
        f << "lut " << d->displayLut << "\n";
        f << "image " << d->black << " " << d->white << " ";
        if (d->rawDtype >= 0) {
            int interp = d->rawInterp;
            if (RAW_INTERP_CH[interp] == 1)   // 1ch family: honor the CURRENT interpretation
                interp = d->cfa == 2 ? RI_QUAD : d->cfa == 1 ? RI_BAYER : RI_GRAY;
            f << "raw3 " << d->rawDtype << " " << interp << " "
              << (d->srcW > 0 ? d->srcW : d->w) << " " << (d->srcH > 0 ? d->srcH : d->h) << " "
              << d->rawOffset << " " << (d->rawLE ? 1 : 0) << " " << d->cfaPattern << " "
              << (d->cfaColorize ? 1 : 0) << " "
              << d->cropX << " " << d->cropY << " " << d->w << " " << d->h << " ";
        } else {
            f << "npy3 " << d->cfa << " " << d->cfaPattern << " " << (d->cfaColorize ? 1 : 0) << " "
              << d->cropX << " " << d->cropY << " " << d->w << " " << d->h << " ";
        }
        f << d->path << "\n";           // path last: may contain spaces
        // the batch travels BY NAME: ids do not survive sessions, and equal
        // names merging on restore is the least surprising failure mode
        for (const auto& b : app.batches)
            if (b.id == d->batchId) { f << "imgbatch " << b.name << "\n"; break; }
        if (d->seqId != 0) {
            f << "seqframe " << d->seqIndex << "\n";   // come back to the same frame
            // A folder sequence must rescan its siblings; an in-file frame axis
            // rebuilds the whole stack from the file itself, so asking for a
            // sibling scan there would split the stack in two.
            bool inFile = true;
            for (const auto& o : app.images)
                if (o->seqId == d->seqId && o->path != d->path) { inFile = false; break; }
            if (!inFile) f << "seqload 1\n";
            // AFTER seqload: for a folder stack the SeqInfo only exists once the
            // rescan ran, and a name applied before that lands on nothing. The
            // name may be user-given ("25C dark") - it must survive the session.
            if (const App::SeqInfo* sqi = seqInfo(d->seqId))
                if (!sqi->name.empty()) f << "seqname " << sqi->name << "\n";
        }
    }
    // ---- series (系列), after the image lines -------------------------------
    // A FLAT block with unique keys, on purpose: an older viewer skips every
    // line it does not know and still opens the session, and this one opens a
    // session with no block as zero series. Nothing else in the format moves.
    //
    // Members travel by the PATH OF THEIR FIRST FRAME. A stack NAME would be
    // the obvious choice and is the wrong one: seqname is written but silently
    // dropped on restore for folder stacks (when the line is parsed the stack
    // does not exist yet - the frames arrive from a queued rescan), so half the
    // members would resolve to nothing. A path is what the session already
    // proves it can round-trip: it is how the image lines themselves work.
    int lostSeries = 0, lostMembers = 0;
    for (const auto& S : app.series) {
        std::string bn = batchNameOf(S.batchId);
        // no batch, nothing to restore it into - and nothing silently: the load
        // side counts every member it cannot resolve, so the save side owes the
        // same. A series that leaves the file is a sweep's worth of hand-typed
        // parameter values gone.
        if (bn.empty()) { lostSeries++; lostMembers += (int)S.members.size(); continue; }
        f << "series " << S.name << "\n";              // name last on its line: spaces
        f << "seriesbatch " << bn << "\n";             // by NAME, like imgbatch
        f << "seriesparam " << S.paramName << "\n";
        f << "seriesunit " << S.unit << "\n";          // empty line = unit not set
        f << "serieskind " << S.kind << "\n";
        for (const auto& m : S.members) {
            std::vector<int> fr = framesOfSeq(m.seqId);
            if (fr.empty()) { lostMembers++; continue; }
            f << "seriesmember ";
            // EXACTLY, not to the file-wide 9 digits: this is the axis a fit is
            // taken against, and the modal no longer rounds it either.
            if (std::isfinite(m.value)) f << fmtExact(m.value);
            else f << "-";                             // "-" = unset, NOT zero
            f << " " << (m.include ? 1 : 0) << " "
              << app.images[fr.front()]->path << "\n";   // path last: spaces
        }
        f << "seriesend\n";
    }
    if (lostSeries || lostMembers)
        fprintf(stderr, "series: NOT saved: %d series and %d member(s) had nothing to "
                        "restore them from\n", lostSeries, lostMembers);
    if (lostSeriesOut) *lostSeriesOut = lostSeries;
    if (lostMembersOut) *lostMembersOut = lostMembers;
    for (const auto& a : app.anns)   // label last: may contain spaces
        f << "ann " << a.type << " " << a.x << " " << a.y << " " << a.w << " " << a.h << " "
          << a.color << " " << (a.visible ? 1 : 0) << " "
          << a.prevX << " " << a.prevW << " " << a.prevY << " " << a.prevH << " "
          << a.label << "\n";     // label last: it may contain spaces
    {   // which annotation was selected, by position in the list above
        int selIdx = -1;
        for (int i = 0; i < (int)app.anns.size(); i++)
            if (app.anns[i].id == app.selectedAnn) selIdx = i;
        f << "selann " << selIdx << "\n";
    }
}

static void saveSession(std::string path, bool quiet = false) {
    for (auto& d : app.images)                  // a saved session has no transients
        if (d->preview) promotePreview(d.get());
    if (path.find('.') == std::string::npos) path += ".vsession";
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { if (!quiet) toast("cannot write session file", true); return; }
    int lostSeries = 0, lostMembers = 0;
    writeSessionTo(f, &lostSeries, &lostMembers);
    if (quiet) return;
    std::string msg = "session saved: " + baseName(path);
    // the load side counts what it could not restore; so does this one now
    if (lostSeries || lostMembers)
        msg += " (" + std::to_string(lostSeries) + " series and " +
               std::to_string(lostMembers) + " series member(s) could NOT be saved)";
    toast(msg, lostSeries > 0 || lostMembers > 0);
}

static std::string loadNpy(const std::string& path);   // fwd (defined above, decl for clarity)

// ---- crash / exit safety net -------------------------------------------------
// The work that is expensive to redo is the arrangement (which files, which
// ROIs, which range), not the pixels: autosave it so a crash costs nothing.
static std::string autosavePath() {
    std::error_code ec;
    std::filesystem::path cfg;
#if defined(_WIN32)
    if (const char* ad = getenv("APPDATA")) cfg = std::filesystem::u8path(ad);
#else
    if (const char* hm = getenv("HOME")) cfg = std::filesystem::u8path(hm) / ".config";
#endif
    if (cfg.empty()) return {};
    cfg /= "viewer";
    std::filesystem::create_directories(cfg, ec);
    if (ec) return {};
    return (cfg / "autosave.vsession").u8string();
}

static void autosaveSession() {
    std::string p = autosavePath();
    if (!p.empty() && !app.images.empty()) saveSession(p, true);
}

// ---- preferences -------------------------------------------------------------
// How the app behaves, as opposed to what is open: it belongs to the user, not to
// a session file, and must survive a machine that never gets a clean shutdown.
static std::string prefsPath() {
    std::string p = autosavePath();
    if (p.empty()) return p;
    size_t slash = p.find_last_of("/\\");
    return p.substr(0, slash == std::string::npos ? 0 : slash + 1) + "prefs.txt";
}

static void savePrefs() {
    std::string p = prefsPath();
    if (p.empty()) return;
    std::ofstream f(pathFromUtf8(p), std::ios::binary);
    if (!f) return;
    f << "viewer-prefs 1\n";
    f << "theme " << app.themeVariant << " " << app.themeAccent << "\n";
    f << "compact " << (app.compactUi ? 1 : 0) << "\n";
    f << "dragpans " << (app.dragPans ? 1 : 0) << "\n";
    f << "wheelzoom " << (app.wheelZoomPlain ? 1 : 0) << "\n";
    f << "fitonswitch " << (app.fitOnSwitch ? 1 : 0) << "\n";
    f << "seqload " << app.seqLoadMode << "\n";
    f << "showfps " << (app.showFps ? 1 : 0) << "\n";
    f << "lowbandwidth " << (app.lowBandwidth ? 1 : 0) << "\n";
    f << "membudget " << app.memBudgetGB << "\n";
    f << "procpolicy " << app.procPolicy << "\n";
    f << "linunit " << app.lin.unit << "\n";
    f << "gamma " << app.dispGamma << "\n";
    f << "grid " << (app.showGrid ? 1 : 0) << "\n";
    f << "frame " << app.frameMode << "\n";
    f << "rbflat " << (app.rbFlat ? 1 : 0) << "\n";
    f << "rbadv " << (app.rbAdvanced ? 1 : 0) << "\n";
    f << "rbtree " << (app.rbTree ? 1 : 0) << "\n";
    // paths may contain spaces: value is the rest of the line
    if (!app.remoteExe.empty()) f << "remoteexe " << app.remoteExe << "\n";
    if (!app.lastRemoteUrl.empty()) f << "remoteurl " << app.lastRemoteUrl << "\n";
    for (const auto& b : app.rbBookmarks) f << "rbookmark " << b << "\n";
    for (const auto& r : app.rbRecents)   f << "rbrecent " << r << "\n";
}

static void loadPrefs() {
    std::vector<uint8_t> buf;
    std::string p = prefsPath();
    if (p.empty() || !readFileBytes(p, buf)) return;
    std::istringstream ss(std::string(buf.begin(), buf.end()));
    std::string line;
    std::getline(ss, line);                       // header
    if (line.compare(0, 13, "viewer-prefs ") != 0) return;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string key; ls >> key;
        int v = 0;
        if      (key == "theme")       { ls >> app.themeVariant >> app.themeAccent; }
        else if (key == "compact")     { ls >> v; app.compactUi = v != 0; }
        else if (key == "dragpans")    { ls >> v; app.dragPans = v != 0; }
        else if (key == "wheelzoom")   { ls >> v; app.wheelZoomPlain = v != 0; }
        else if (key == "fitonswitch") { ls >> v; app.fitOnSwitch = v != 0; }
        else if (key == "seqload")     { ls >> app.seqLoadMode; }
        else if (key == "showfps")     { ls >> v; app.showFps = v != 0; }
        else if (key == "lowbandwidth"){ ls >> v; app.lowBandwidth = v != 0; }
        else if (key == "membudget")   { ls >> app.memBudgetGB;
                                         if (app.memBudgetGB > 0)
                                             app.memBudgetGB = std::clamp(app.memBudgetGB, 0.5f, 4096.0f); }
        else if (key == "procpolicy")  { ls >> app.procPolicy;
                                         app.procPolicy = std::clamp(app.procPolicy, 0, 2); }
        else if (key == "gamma")       { ls >> app.dispGamma; }
        else if (key == "grid")        { ls >> v; app.showGrid = v != 0; }
        else if (key == "frame")       { ls >> app.frameMode;
                                         app.frameMode = std::clamp(app.frameMode, 0, 1); }
        else if (key == "rbflat")      { ls >> v; app.rbFlat = v != 0; }
        else if (key == "rbadv")       { ls >> v; app.rbAdvanced = v != 0; }
        else if (key == "rbtree")      { ls >> v; app.rbTree = v != 0; }
        else if (key == "remoteexe" || key == "remoteurl" ||
                 key == "rbookmark" || key == "rbrecent") {
            std::string s;
            std::getline(ls, s);
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            if (s.empty()) continue;
            if      (key == "remoteexe") app.remoteExe = s;
            else if (key == "remoteurl") app.lastRemoteUrl = s;
            else if (key == "rbookmark") app.rbBookmarks.push_back(s);
            else if (app.rbRecents.size() < 10) app.rbRecents.push_back(s);
        }
    }
    app.themeVariant = std::clamp(app.themeVariant, 0, 1);
    app.themeAccent = std::clamp(app.themeAccent, 0, ui_theme::accentCount() - 1);
    app.seqLoadMode = std::clamp(app.seqLoadMode, 0, 2);
    app.dispGamma = app.dispGamma > 1.5f ? 2.2f : 1.0f;
}

// Pre-rendered session text and a file already open for it. A SIGSEGV handler may
// be running on a corrupted heap: allocating, opening files, or touching ImGui
// there is how a crash-recovery feature turns into a hang. Everything expensive
// happens while the process is still healthy; the handler only write()s.
static std::string g_crashText;
static int g_crashFd = -1;

static void refreshCrashSnapshot() {
    if (app.images.empty()) { g_crashText.clear(); return; }
    std::ostringstream os;
    writeSessionTo(os);
    g_crashText = os.str();
}

static void openCrashFile() {
    std::string p = autosavePath();
    if (p.empty()) return;
#if defined(_WIN32)
    g_crashFd = _wopen(pathFromUtf8(p).wstring().c_str(),
                       _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    g_crashFd = open(pathFromUtf8(p).c_str(), O_CREAT | O_RDWR, 0644);
#endif
}

static void crashHandler(int sig) {
    static volatile sig_atomic_t inHandler = 0;
    if (!inHandler) {
        inHandler = 1;
        if (g_crashFd >= 0 && !g_crashText.empty()) {
            const char* p = g_crashText.data();
            size_t n = g_crashText.size();
#if defined(_WIN32)
            _lseek(g_crashFd, 0, SEEK_SET);
            while (n) { int w = _write(g_crashFd, p, (unsigned)n); if (w <= 0) break; p += w; n -= (size_t)w; }
            _chsize(g_crashFd, (long)g_crashText.size());
            _commit(g_crashFd);
#else
            lseek(g_crashFd, 0, SEEK_SET);
            while (n) { ssize_t w = write(g_crashFd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
            if (ftruncate(g_crashFd, (off_t)g_crashText.size()) != 0) { /* best effort */ }
#endif
            static const char msg[] = "\nviewer crashed; session written to the autosave file\n";
#if defined(_WIN32)
            _write(2, msg, (unsigned)(sizeof msg - 1));
#else
            ssize_t ign = write(2, msg, sizeof msg - 1); (void)ign;
#endif
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static std::string loadSession(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read session file";
    std::string text((const char*)buf.data(), buf.size());
    std::istringstream ss(text);
    std::string line;
    if (!std::getline(ss, line) || line.rfind("viewer-session", 0) != 0)
        return "not a viewer session file";
    stopSequenceLoader();          // orphan frames from a previous load must not
    app.seqQueue.clear();          // graft themselves onto the restored list
    closeAll();
    app.anns.clear();
    app.selectedAnn = 0;
    app.annRev++;
    std::vector<std::string> failures;   // reported as one summary, not one toast each
    int selAnnIndex = -1;
    float zoom = 0; ImVec2 center(0, 0); int current = 0;
    int pendingLut = -1;
    std::string pendingMember;         // .npz array name for the next image line
    bool lastImageOk = false;         // seqload applies to the image just loaded
    bool haveView = false;
    // "current" is an ordinal over saved image lines; a line can expand into a
    // whole stack, so resolve it to a real index once that line is fully applied
    // (seqframe, which picks the frame, comes after the image line).
    int lineOrd = -1, wantCurrent = -1, resolvedCurrent = -1;
    int curSeries = -1;               // index into app.seriesRestore, -1 = outside a block
    bool capturing = false;
    auto settleCurrent = [&]() {
        if (capturing) { resolvedCurrent = app.current; capturing = false; }
    };
    auto restOfLine = [](std::istringstream& ls) {
        std::string p; std::getline(ls, p);
        while (!p.empty() && (p[0] == ' ' || p[0] == '\t')) p.erase(0, 1);
        while (!p.empty() && (p.back() == '\r' || p.back() == '\n')) p.pop_back();
        return p;
    };
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key; ls >> key;
        if      (key == "gamma")   ls >> app.dispGamma;
        else if (key == "grid")  { int g = 0; ls >> g; app.showGrid = g != 0; }
        else if (key == "zoom")  { ls >> zoom; haveView = true; }
        else if (key == "center")  ls >> center.x >> center.y;
        else if (key == "current") ls >> wantCurrent;
        else if (key == "linkrange") { int on = 0; ls >> on >> app.linkBlack >> app.linkWhite;
                                       app.linkRange = on != 0; }
        else if (key == "rangescope") { ls >> app.rangeScope; }
        else if (key == "cmpfollow") { int on = 1; ls >> on; app.compareFollowFrame = on != 0; }
        else if (key == "cmprange") { ls >> app.compareRangeMode; }
        // pre-tri-state prefs: the old bool maps onto "B uses A's" / "each own".
        // NOT onto the new default - it records a choice somebody actually made.
        else if (key == "cmpshare") { int on = 1; ls >> on; app.compareRangeMode = on ? 1 : 0; }
        else if (key == "abstats") { ls >> app.abStatsLayout >> app.histPlane;
                                     app.abStatsLayout = std::clamp(app.abStatsLayout, 0, 2);
                                     app.histPlane = std::clamp(app.histPlane, -1, 3); }
        else if (key == "roichannel") ls >> app.roiChannel;
        else if (key == "projection") { int h = 1, v = 1;
                                        ls >> app.projMode >> app.projYMode >> app.projYLo
                                           >> app.projYHi >> h >> v;
                                        app.showProjH = h != 0; app.showProjV = v != 0; }
        else if (key == "analysis") {
            int a = 0, r = 0;
            float rv = 0.5f;
            ls >> app.anaSel >> a;
            app.anaAuto = a != 0;
            if (ls >> r >> rv) { app.anaRefOn = r != 0; app.anaRef = rv; }   // >= this rev
        }
        else if (key == "histlog") { int on = 1; ls >> on; app.histLog = on != 0; }
        else if (key == "npyaxis") { ls >> app.npyAxis; app.npyAxis = std::clamp(app.npyAxis, 0, 1); }
        else if (key == "compare") { int ab = 0, fa = 0;
                                     ls >> app.compareMode >> app.wipeFrac >> app.splitFrac;
                                     if (ls >> app.diffGain >> ab) app.diffAbs = ab != 0;  // v2
                                     if (ls >> fa >> app.flipPeriod) app.flipAuto = fa != 0;  // v3
                                     app.compareMode = std::clamp(app.compareMode, 0, 4);
                                     app.flipPeriod = std::clamp(app.flipPeriod, 0.05f, 10.0f);
                                     app.wipeFrac = std::clamp(app.wipeFrac, 0.03f, 0.97f);
                                     app.splitFrac = std::clamp(app.splitFrac, 0.03f, 0.97f); }
        else if (key == "compareb") {   // "<frameIndex|-1> <name>"; v1 files: just a name
            std::streampos save = ls.tellg();
            int fi = -1;
            if (ls >> fi) { app.compareBSeq = fi; }
            else { ls.clear(); ls.seekg(save); app.compareBSeq = -1; }
            app.compareB = restOfLine(ls);
            app.compareBUid = 0;        // resolved by name+frame on first use
        }
        else if (key == "panels") {
            // trailing fields are optional: sessions written before a panel
            // existed keep their meaning, and its default stays put
            int a = 1, b = 1, c2 = 1, d2 = 1, e2 = 1, f2 = 1, g2 = 0, h2 = 0, i2 = 0;
            ls >> a >> b >> c2 >> d2 >> e2 >> f2 >> g2 >> h2 >> i2;
            app.showFiles = a != 0; app.showInspector = b != 0; app.showRois = c2 != 0;
            app.showAnalysis = d2 != 0; app.showHistogram = e2 != 0; app.showTemporal = f2 != 0;
            app.showProjection = g2 != 0; app.showLinearity = h2 != 0; app.showRemote = i2 != 0;
        }
        else if (key == "layout_begin") {
            std::string ini, l2;
            while (std::getline(ss, l2)) {
                if (l2.rfind("layout_end", 0) == 0) break;
                if (!l2.empty() && l2[0] == '|') l2.erase(l2.begin());
                while (!l2.empty() && (l2.back() == '\r' || l2.back() == '\n')) l2.pop_back();
                ini += l2;
                ini += "\n";
            }
            {   // sessions saved before the "Remote" -> "Browse###Remote"
                // rename: migrate the window entry or the panel undocks
                size_t p;
                while ((p = ini.find("[Window][Remote]")) != std::string::npos)
                    ini.replace(p, strlen("[Window][Remote]"), "[Window][Browse###Remote]");
            }
            // applied between frames: loading dock settings mid-frame is not safe
            app.pendingLayout = std::move(ini);
            app.resetLayout = false;   // otherwise the default rebuild wipes it
        }
        else if (key == "lut") ls >> pendingLut;   // applies to the next image line
        else if (key == "member") pendingMember = restOfLine(ls);
        else if (key == "selann") ls >> selAnnIndex;
        else if (key == "imgbatch") {   // batch of the image above, by name
            std::string nm = restOfLine(ls);
            if (!nm.empty() && lastImageOk && cur()) {
                cur()->batchId = batchReuse(nm);
                app.imagesRev++;
            }
        }
        else if (key == "seqname") {    // user-given stack name (may contain spaces)
            std::string nm = restOfLine(ls);
            if (!nm.empty() && lastImageOk && cur() && cur()->seqId != 0)
                if (App::SeqInfo* si2 = seqInfo(cur()->seqId)) si2->name = nm;
        }
        // MIGRATION of the old per-stack level. saveSession has never written
        // this key (verified over the whole history: no commit ever emitted
        // it), so it fires only for hand-written or third-party files. Note the
        // old reader also required cur()->seqId != 0, which is FALSE for a
        // folder stack at parse time - it could only ever have worked for an
        // in-file frame axis. This one keys on the PATH, which works for both,
        // and turns into a series only where there is a sweep to speak of.
        else if (key == "seqlevel") {
            double lv = 0; ls >> lv;
            if (lastImageOk && cur() && !cur()->path.empty() && std::isfinite(lv))
                app.seqLevelLegacy.push_back({ cur()->path, lv });
        }
        // ---- series (系列) block: collected here, RESOLVED later -------------
        // Nothing can be looked up yet - a folder stack is still one loose image
        // with a queued rescan behind it. See resolveSeriesRestore.
        else if (key == "series") {
            App::SeriesRestore r;
            r.name = restOfLine(ls);
            app.seriesRestore.push_back(std::move(r));
            curSeries = (int)app.seriesRestore.size() - 1;
        }
        else if (key == "seriesend") curSeries = -1;
        else if (key == "seriesbatch") {
            if (curSeries >= 0) app.seriesRestore[curSeries].batchName = restOfLine(ls);
        }
        else if (key == "seriesparam") {
            if (curSeries >= 0) app.seriesRestore[curSeries].paramName = restOfLine(ls);
        }
        else if (key == "seriesunit") {
            if (curSeries >= 0) app.seriesRestore[curSeries].unit = restOfLine(ls);
        }
        else if (key == "serieskind") {
            if (curSeries >= 0) { int k = 0; ls >> k; app.seriesRestore[curSeries].kind = k; }
        }
        else if (key == "seriesmember") {   // "<value|-> <0|1> <path of frame 0>"
            // Both fields as TEXT. A value this program cannot read is UNSET,
            // not 0: atof() turns "notanumber", "1e" or a half-written line into
            // a hard measurement at zero, which the fit then treats as the dark
            // stack. A loader is exactly where that must not be assumed.
            std::string v, inc;
            ls >> v >> inc;
            std::string p = restOfLine(ls);
            if (curSeries >= 0 && !p.empty()) {
                double val = parseSeriesValue(v.c_str());
                if (v != "-" && !std::isfinite(val)) app.seriesRestore[curSeries].badValues++;
                app.seriesRestore[curSeries].members.push_back({ val, inc != "0", p });
            }
        }
        else if (key == "linunit") { std::string u = restOfLine(ls);
                                     snprintf(app.lin.unit, sizeof app.lin.unit, "%s", u.c_str()); }
        else if (key == "seqframe") {   // select the frame this stack was left on
            int want = 0; ls >> want;
            if (lastImageOk && cur() && cur()->seqId != 0) {
                for (int idx : framesOfSeq(cur()->seqId))
                    if (app.images[idx]->seqIndex == want) { selectImage(idx); break; }
            }
        }
        else if (key == "seqload") {   // applies to the image loaded just above
            int on = 0; ls >> on;
            // only if that image actually loaded, and not when it is already a
            // stack (an in-file frame axis) - that would split it in two
            if (on && lastImageOk && cur() && !cur()->path.empty() && cur()->seqId == 0) {
                std::string pat;
                std::vector<std::string> files = findSequenceSiblings(cur()->path, pat);
                // queued, not started: see App::seqRestore
                if (files.size() >= 2)
                    app.seqRestore.push_back({ cur()->uid, std::move(files), pat });
            }
        }
        else if (key == "ann") {
            App::Ann a; int vis = 1;
            ls >> a.type >> a.x >> a.y >> a.w >> a.h >> a.color >> vis;
            // v1 files stop here; v2 adds the band-toggle memory before the label
            std::streampos save = ls.tellg();
            if (!(ls >> a.prevX >> a.prevW >> a.prevY >> a.prevH)) {
                ls.clear(); ls.seekg(save);
                a.prevW = a.prevH = -1;
            }
            if (a.type < 0 || a.type > 1 || a.x < 0 || a.y < 0 || a.w < 0 || a.h < 0)
                continue;                 // reject malformed lines (would read out of bounds)
            a.color &= 7;
            a.visible = vis != 0;
            a.label = restOfLine(ls);
            if (a.label.empty()) a.label = a.type == 0 ? "ROI" : "P";
            a.id = app.nextAnnId++;
            app.anns.push_back(std::move(a));
            app.annRev++;
        }
        // legacy (pre-annotation sessions)
        else if (key == "pin") { int x = 0, y = 0; ls >> x >> y;
                                 if (x >= 0 && y >= 0) addAnn(1, x, y, 0, 0); }
        else if (key == "roi") { int x = 0, y = 0, w = 0, h = 0; ls >> x >> y >> w >> h;
                                 if (w > 0 && h > 0) addAnn(0, x, y, w, h); }
        else if (key == "image") {
            settleCurrent();              // the previous image line is done
            lineOrd++;
            float bk = 0, wt = 1; std::string kind;
            ls >> bk >> wt >> kind;
            std::string err;
            if (kind == "raw3" || kind == "raw2" || kind == "raw") {
                RawDialog d;
                int le = 1, col = 0;
                if (kind == "raw3") {
                    int ccx = 0, ccy = 0, ccw = 0, cch = 0;
                    ls >> d.dtype >> d.interp >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> col
                       >> ccx >> ccy >> ccw >> cch;
                    if (ccw > 0 && cch > 0 && (ccx != 0 || ccy != 0 || ccw != d.w || cch != d.h)) {
                        d.cropOn = true;
                        d.cropX = ccx; d.cropY = ccy; d.cropW = ccw; d.cropH = cch;
                    }
                } else if (kind == "raw2") {
                    ls >> d.dtype >> d.interp >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> col;
                } else {                     // legacy v1 line: combined format index + quad flag
                    int fmt = 0, quad = 0;
                    ls >> fmt >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> quad >> col;
                    if (fmt < 0 || fmt >= 12) { toast("session: bad raw format", true); continue; }
                    d.dtype = LEGACY_RAW_MAP[fmt][0];
                    d.interp = LEGACY_RAW_MAP[fmt][1];
                    if (quad && d.interp == RI_BAYER) d.interp = RI_QUAD;
                }
                d.littleEndian = le != 0;
                d.path = restOfLine(ls);
                err = loadRaw(d);
                if (err.empty()) cur()->cfaColorize = col != 0;
            } else if (kind == "npy2" || kind == "npy3") {
                int cfa = 0, pat = 0, col = 0, ccx = 0, ccy = 0, ccw = 0, cch = 0;
                ls >> cfa >> pat >> col;
                if (kind == "npy3") ls >> ccx >> ccy >> ccw >> cch;
                std::string p = restOfLine(ls);
                // an .npz member must go back through the zip reader; loadNpy
                // would fail the magic check and the array would silently vanish
                std::string lowp = p;
                std::transform(lowp.begin(), lowp.end(), lowp.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                bool isNpz = lowp.size() > 4 && lowp.compare(lowp.size() - 4, 4, ".npz") == 0;
                bool isRemote = p.compare(0, 6, "ssh://") == 0 || p.compare(0, 8, "local://") == 0;
                if (isRemote) {
                    // a remote image has no local file to decode: reconnect instead
                    size_t before = app.images.size();
                    openRemote(p);
                    err = app.images.size() > before ? "" : "cannot reopen " + p;
                } else {
                    err = isNpz ? loadNpz(p, pendingMember) : loadNpy(p);
                }
                if (err.empty()) {
                    cur()->cfa = std::clamp(cfa, 0, 2);
                    cur()->cfaPattern = pat & 3;
                    cur()->cfaColorize = col != 0;
                    if (ccw > 0 && cch > 0 && (ccx != 0 || ccy != 0 || ccw != cur()->w || cch != cur()->h))
                        cropInPlace(*cur(), ccx, ccy, ccw, cch);
                }
            } else {                          // legacy "npy"
                std::string p = restOfLine(ls);
                err = loadNpy(p);
            }
            pendingMember.clear();
            if (!err.empty()) {
                failures.push_back(err);
                lastImageOk = false;
                pendingLut = -1;          // must not leak onto the next image
                continue;
            }
            lastImageOk = true;
            cur()->black = bk; cur()->white = wt; cur()->texDirty = true;
            cur()->displayLut = pendingLut; pendingLut = -1;
            if (lineOrd == wantCurrent) capturing = true;
        }
    }
    settleCurrent();                      // last image line in the file
    if (resolvedCurrent >= 0) current = resolvedCurrent;
    if (current >= 0 && current < (int)app.images.size()) app.current = current;
    if (haveView && !app.images.empty()) {
        app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        app.view.center = center;
        app.fitRequested = false;      // restored view wins over fit-on-load
    }
    // label counters must continue past the restored names, or the next ROI
    // collides and the analysis grid gets two identical column headers
    for (const auto& a : app.anns) {
        int n = atoi(a.label.c_str() + (a.type == 0 ? std::min<size_t>(4, a.label.size())
                                                    : std::min<size_t>(1, a.label.size())));
        if (a.type == 0) app.roiSeq = std::max(app.roiSeq, n);
        else app.poiSeq = std::max(app.poiSeq, n);
    }
    if (selAnnIndex >= 0 && selAnnIndex < (int)app.anns.size())
        app.selectedAnn = app.anns[selAnnIndex].id;
    else
        app.selectedAnn = 0;
    if (!failures.empty()) {
        std::string msg = std::to_string(failures.size()) + " image(s) could not be restored:\n";
        for (size_t i = 0; i < failures.size() && i < 5; i++) msg += "  " + failures[i] + "\n";
        if (failures.size() > 5) msg += "  ...";
        toast(msg, true);
    }
    markAllTexDirty();                 // gamma may have changed
    return {};
}

// ---------------------------------------------------------------- sequences (連番)
// A sequence is one "stack": frames from numbered files in a folder that share a
// decode recipe. It is the unit temporal analysis operates on.
// How much RAM the loader may fill with frames. A hardcoded 6 GB meant a machine
// with 128 GB stopped 300-frame folders at 60 frames, with no way to say "use the
// memory I have" - and 6 GB is at the same time too much for a small laptop.
static size_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return (size_t)ms.ullTotalPhys;
#elif defined(__APPLE__)
    uint64_t v = 0; size_t len = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &len, nullptr, 0) == 0) return (size_t)v;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) return (size_t)pages * (size_t)page;
#endif
    return (size_t)8 << 30;                       // unknown: assume a modest box
}
static size_t seqMemBudget() {
    if (app.memBudgetGB > 0) return (size_t)(app.memBudgetGB * 1073741824.0);
    // default: most of the machine, but never the whole of it
    size_t phys = physicalMemoryBytes();
    return std::max((size_t)2 << 30, (size_t)(phys * 0.6));
}
// frames already resident, so the budget covers everything open and not just the
// stack being loaded right now
static size_t residentImageBytes() {
    size_t n = 0;
    for (const auto& d : app.images) n += d->data.size() * sizeof(float);
    return n;
}

// split a stem into alternating text / digit segments: "flat_0007_640x480" ->
// ["flat_"]["0007"]["_"]["640"]["x"]["480"]
struct NameSeg { bool digit; std::string s; };
static std::vector<NameSeg> segmentName(const std::string& stem) {
    std::vector<NameSeg> segs;
    size_t i = 0;
    while (i < stem.size()) {
        bool d = isdigit((unsigned char)stem[i]) != 0;
        size_t j = i;
        while (j < stem.size() && (isdigit((unsigned char)stem[j]) != 0) == d) j++;
        segs.push_back({ d, stem.substr(i, j - i) });
        i = j;
    }
    return segs;
}

// Siblings = files whose names differ ONLY in one digit field. The field is
// chosen by which one actually varies in the folder, so "shot_0007_1920x1080.raw"
// groups on 0007 and not on the resolution.
static std::vector<std::string> findSequenceSiblings(const std::string& path,
                                                     std::string& patternOut) {
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::path p = pathFromUtf8(path);
    std::filesystem::path dir = p.parent_path();
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    const std::string stem = p.stem().u8string(), ext = p.extension().u8string();
    std::vector<NameSeg> segs = segmentName(stem);

    // candidate files: same extension, same segment structure, all text segments equal
    struct Cand { std::vector<NameSeg> segs; std::string path; };
    std::vector<Cand> cands;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        if (e.path().extension().u8string() != ext) continue;
        std::vector<NameSeg> s2 = segmentName(e.path().stem().u8string());
        if (s2.size() != segs.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < segs.size() && ok; k++) {
            if (segs[k].digit != s2[k].digit) ok = false;
            else if (!segs[k].digit && segs[k].s != s2[k].s) ok = false;
        }
        if (ok) cands.push_back({ std::move(s2), e.path().u8string() });
    }
    if (cands.size() < 2) return out;

    // The frame axis is the LAST digit field that varies among the siblings -
    // capture software puts the counter last (frame_001, IMG_0001, lv000_f02).
    // Picking the field with the most distinct values instead made a folder of
    // 10 illuminances x 3 frames group by ILLUMINANCE, so each "stack" spanned
    // ten levels and its sigma_t meant nothing. The peer uses the same rule.
    int best = -1;
    for (size_t k = 0; k < segs.size(); k++) {
        if (!segs[k].digit) continue;
        std::vector<std::string> vals;
        bool othersMatch = true;
        for (const auto& c : cands) {
            bool same = true;
            for (size_t m = 0; m < segs.size(); m++)
                if (m != k && segs[m].digit && c.segs[m].s != segs[m].s) { same = false; break; }
            if (!same) continue;
            bool dup = false;
            for (const auto& v : vals) if (v == c.segs[k].s) { dup = true; break; }
            if (!dup) vals.push_back(c.segs[k].s);
        }
        (void)othersMatch;
        if (vals.size() >= 2) best = (int)k;     // keep overwriting: the last wins
    }
    if (best < 0) return out;

    std::vector<std::pair<long long, std::string>> found;
    for (const auto& c : cands) {
        bool same = true;
        for (size_t m = 0; m < segs.size(); m++)
            if ((int)m != best && segs[m].digit && c.segs[m].s != segs[m].s) { same = false; break; }
        if (!same) continue;
        found.emplace_back(atoll(c.segs[best].s.c_str()), c.path);
    }
    if (found.size() < 2) return out;
    std::sort(found.begin(), found.end());
    patternOut.clear();
    for (size_t k = 0; k < segs.size(); k++)
        // '?' and not '#': ImGui truncates any label at "##", which blanked
        // every all-digit sequence name in the picker ('?' also reads as glob)
        patternOut += ((int)k == best) ? std::string(segs[k].s.size(), '?') : segs[k].s;
    patternOut += ext;
    std::vector<std::string> bases;
    bases.reserve(found.size());
    for (auto& f : found) {
        out.push_back(f.second);
        bases.push_back(baseName(f.second));
    }
    // ...then say what the '?' run actually spans. Same function the peer runs
    // (rp::patternWithExtent): a folder opened locally and the same folder
    // listed over ssh must produce the identical stack name.
    patternOut = rp::patternWithExtent(patternOut, bases);
    return out;
}

static void stopSequenceLoader() {
    app.seqCancel = true;
    if (app.seqThread.joinable()) app.seqThread.join();
    app.seqRunning = false;
    app.seqCancel = false;
    std::lock_guard<std::mutex> lk(app.seqMtx);
    app.seqReady.clear();
}

// Spawn the worker. The thread only decodes (no app state, no GL) and pushes
// finished frames into seqReady; the UI thread integrates them in pumpSequence().
static void startSequenceLoad(int imageIdx, const std::vector<std::string>& files,
                              const std::string& pattern) {
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    stopSequenceLoader();
    ImageDoc* ref = app.images[imageIdx].get();
    App::SeqInfo info;
    info.id = app.nextSeqId++;
    info.name = pattern.empty() ? ref->name : pattern;
    info.lastImageIdx = imageIdx;
    app.seqs.push_back(info);
    ref->seqId = info.id;
    // position of the already-open frame inside the file list
    int selfIdx = 0;
    for (int i = 0; i < (int)files.size(); i++)
        if (files[i] == ref->path) { selfIdx = i; break; }
    ref->seqIndex = selfIdx;

    // capture everything the worker needs BY VALUE
    struct Job { std::string path; int index; };
    std::vector<Job> jobs;
    for (int i = 0; i < (int)files.size(); i++)
        if (i != selfIdx) jobs.push_back({ files[i], i });
    bool isRaw = ref->rawDtype >= 0;
    RawDialog recipe;
    if (isRaw) {
        recipe.dtype = ref->rawDtype;
        recipe.interp = ref->rawInterp;
        if (RAW_INTERP_CH[recipe.interp] == 1)
            recipe.interp = ref->cfa == 2 ? RI_QUAD : ref->cfa == 1 ? RI_BAYER : RI_GRAY;
        recipe.w = ref->srcW > 0 ? ref->srcW : ref->w;
        recipe.h = ref->srcH > 0 ? ref->srcH : ref->h;
        recipe.offset = ref->rawOffset;
        recipe.littleEndian = ref->rawLE;
        recipe.cfaPattern = ref->cfaPattern & 3;
        recipe.cropOn = isCropped(*ref);
        recipe.cropX = ref->cropX; recipe.cropY = ref->cropY;
        recipe.cropW = ref->w; recipe.cropH = ref->h;
        recipe.replaceIdx = -1;
    }
    app.seqLoadingId = info.id;
    app.seqBytes = ref->data.size() * sizeof(float);
    app.seqDone = 0;
    app.seqTotal = (int)jobs.size();
    app.seqCancel = false;
    app.seqRunning = true;
    {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        app.seqErr.clear();
    }
    app.seqNote.clear();
    fprintf(stderr, "sequence: %s - %d files (%s)\n", info.name.c_str(),
            (int)files.size(), isRaw ? "raw recipe" : "npy");
    const size_t startBytes = residentImageBytes();
    const size_t budget = seqMemBudget();
    app.seqThread = std::thread([jobs, isRaw, recipe, startBytes, budget]() {
        size_t bytes = startBytes;
        int failures = 0;
        double lastPost = 0;
        for (const auto& j : jobs) {
            if (app.seqCancel) break;
            std::string err;
            std::unique_ptr<ImageDoc> doc;
            if (isRaw) {
                RawDialog d = recipe;
                d.path = j.path;
                doc = decodeRawFrame(d, err);
            } else {
                doc = decodeNpy(j.path, err);
            }
            if (!doc) {
                if (++failures <= 3) {
                    std::lock_guard<std::mutex> lk(app.seqMtx);
                    app.seqErr += baseName(j.path) + ": " + err + "\n";
                }
                app.seqDone++;
                continue;
            }
            computeMinMax(*doc);      // pure: keep this off the UI thread
            bytes += doc->data.size() * sizeof(float);
            {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(j.index, std::move(doc));
            }
            app.seqDone++;
            // wake the UI at most ~10 Hz: one post per decoded frame would defeat
            // the idle throttle on a fast disk
            double now = glfwGetTime();
            if (now - lastPost > 0.1) { lastPost = now; glfwPostEmptyEvent(); }
            if (bytes > budget) {
                // say what stopped it, with the numbers: silently loading 60 of
                // 300 frames is how a measurement quietly becomes wrong
                char m[192];
                snprintf(m, sizeof m,
                         "memory budget %.1f GB reached - stopped after %d of %d frames\n"
                         "(File > Sequence loading > Memory budget)\n",
                         budget / 1073741824.0, app.seqDone.load(), (int)jobs.size());
                fputs(m, stderr);
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqErr += m;
                break;
            }
        }
        app.seqRunning = false;
    });
}

// UI-thread integration of decoded frames (also owns GL/texture lifetime).
static void pumpSequence() {
    std::vector<std::pair<int, std::unique_ptr<ImageDoc>>> batch;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        // cap per UI frame: a fast decoder could otherwise hand us dozens of
        // 48 MB frames at once and stall the frame
        const size_t MAX_PER_FRAME = 4;
        if (app.seqReady.size() <= MAX_PER_FRAME) {
            batch.swap(app.seqReady);
        } else {
            batch.insert(batch.end(), std::make_move_iterator(app.seqReady.begin()),
                         std::make_move_iterator(app.seqReady.begin() + MAX_PER_FRAME));
            app.seqReady.erase(app.seqReady.begin(), app.seqReady.begin() + MAX_PER_FRAME);
        }
        err.swap(app.seqErr);
    }
    // A toast expires; "60 of 300 frames" must not. Keep it in the Files panel
    // until the next load, because a partially loaded stack silently produces
    // wrong temporal statistics.
    if (!err.empty()) { toast(err, true); app.seqNote = err; }
    if (batch.empty()) {
        if (!app.seqRunning && app.seqThread.joinable()) app.seqThread.join();
        return;
    }
    // inherit display settings from the sequence reference frame so frames are
    // directly comparable (same range, same colormap, same CFA interpretation)
    const ImageDoc* ref = nullptr;
    for (const auto& d : app.images)
        if (d->seqId == app.seqLoadingId) { ref = d.get(); break; }
    for (auto& pr : batch) {
        auto& doc = pr.second;
        doc->seqId = app.seqLoadingId;
        doc->seqIndex = pr.first;
        if (doc->vmax <= doc->vmin) computeMinMax(*doc);   // worker normally did this
        if (ref) {
            doc->black = ref->black; doc->white = ref->white;
            doc->cfa = ref->cfa; doc->cfaPattern = ref->cfaPattern;
            doc->cfaColorize = ref->cfaColorize;
            doc->displayLut = ref->displayLut;
            // frame ⊂ stack ⊂ batch: every frame carries its stack's batch, or
            // "close batch" / "move to batch" sees only the head frame
            doc->batchId = ref->batchId;
        } else {
            defaultRange(*doc);
        }
        doc->texDirty = true;
        doc->uid = app.nextUid++;
        app.imagesRev++;
        app.images.push_back(std::move(doc));
    }
    // invalidating per pump made this O(frames^2) over a load; refresh at ~2 Hz
    static double lastTemporalInvalidate = 0;
    double nowT = glfwGetTime();
    if (!app.seqRunning || nowT - lastTemporalInvalidate > 0.5) {
        lastTemporalInvalidate = nowT;
        app.temporal[0].seqId = app.temporal[1].seqId = -1;
    }
    if (!app.seqRunning && app.seqThread.joinable()) {
        app.seqThread.join();
        int n = 0;
        for (const auto& d : app.images) if (d->seqId == app.seqLoadingId) n++;
        fprintf(stderr, "sequence: loaded %d frames\n", n);
    }
}

static void startNextQueuedGroup();   // defined with the folder-open code below

// Frames decoded but not yet integrated. They are stamped with seqLoadingId when
// they land, so the next stack must NOT start while any are still waiting - it
// would move seqLoadingId and file the tail of one stack under another.
static bool seqReadyPending() {
    std::lock_guard<std::mutex> lk(app.seqMtx);
    return !app.seqReady.empty();
}

// The stack a session's series member names, found by the path of its FIRST
// frame. 0 = that frame is not open (or is not part of a stack).
//
// preferBatch disambiguates the case the canon explicitly blesses: reopening
// the same folder makes a NEW batch, so one file is legitimately resident in two
// stacks at once. Taking "the first one" then hands every member of the second
// copy's series a stack in the wrong batch, strict containment rejects them all,
// and the whole series is lost - permanently, at the next autosave.
static int seqIdOfFirstFramePath(const std::string& path, int preferBatch = 0) {
    int any = 0;
    for (const auto& d : app.images) {
        if (d->seqId == 0 || d->path != path) continue;
        if (preferBatch && d->batchId == preferBatch) return d->seqId;
        if (!any) any = d->seqId;
    }
    return any;
}

// Turn what the session file said into real series. Runs once, when every
// stack the file asked for exists - a member cannot be looked up before then.
// Members that cannot be found are COUNTED and reported: silently dropping
// points out of a measurement is the one thing this must not do.
static void resolveSeriesRestore() {
    int made = 0, lost = 0, lostSeries = 0, badValues = 0;
    for (const auto& R : app.seriesRestore) {
        badValues += R.badValues;
        int bid = 0;
        for (const auto& b : app.batches) if (b.name == R.batchName) bid = b.id;
        if (!bid) { lostSeries++; lost += (int)R.members.size(); continue; }
        std::vector<App::Series::Member> ms;
        for (const auto& m : R.members) {
            int sid = seqIdOfFirstFramePath(m.path, bid);
            // strict containment survives the round trip too, or not at all
            if (!sid || batchOfStack(sid) != bid) { lost++; continue; }
            bool dup = false;
            for (const auto& x : ms) if (x.seqId == sid) dup = true;
            if (dup) continue;
            ms.push_back({ sid, m.value, m.include });
        }
        if (ms.empty()) { lostSeries++; continue; }
        int id = newSeries(bid, R.name);
        for (const auto& m : ms) {           // at most one series per stack
            App::Series* other = seriesOfStack(m.seqId);
            if (other && other->id != id) removeFromSeries(m.seqId);
        }
        if (App::Series* S = seriesById(id)) {
            S->paramName = R.paramName;
            snprintf(S->unit, sizeof S->unit, "%s", R.unit.c_str());
            S->kind = std::clamp(R.kind, 0, 3);
            S->members = std::move(ms);
            made++;
        }
    }
    app.seriesRestore.clear();
    pruneEmptySeries();
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
    if (made || lost || lostSeries || badValues) {
        std::string msg = "restored " + std::to_string(made) + " series";
        if (lost || lostSeries)
            msg += "; " + std::to_string(lost) + " member(s) and " +
                   std::to_string(lostSeries) + " series could not be resolved";
        // a value the file states but this program cannot read is left UNSET,
        // and that is a fit point fewer than the file claims - say so
        if (badValues)
            msg += "; " + std::to_string(badValues) +
                   " unreadable value(s) left unset (never 0)";
        toast(msg, lost > 0 || lostSeries > 0 || badValues > 0);
        fprintf(stderr, "series: %s\n", msg.c_str());
    }
}

// Old sessions that carried a per-stack "seqlevel": one series per BATCH that
// actually has two or more levelled stacks, named "<batch> 掃引", unit = the
// prefill, kind = linearity. A batch with fewer gets NOTHING - guessing a sweep
// out of the folder structure is precisely what the canon forbids.
static void migrateLegacyLevels() {
    std::vector<std::pair<int, double>> lvl;      // seqId -> level
    for (const auto& p : app.seqLevelLegacy) {
        int sid = seqIdOfFirstFramePath(p.first);
        if (!sid || seriesOfStack(sid)) continue;   // an explicit series wins
        bool dup = false;
        for (const auto& q : lvl) if (q.first == sid) dup = true;
        if (!dup) lvl.emplace_back(sid, p.second);
    }
    app.seqLevelLegacy.clear();
    std::vector<int> batches;
    for (const auto& q : lvl) {
        int b = batchOfStack(q.first);
        if (!b) continue;
        bool seen = false;
        for (int x : batches) if (x == b) seen = true;
        if (!seen) batches.push_back(b);
    }
    int made = 0;
    for (int b : batches) {
        int n = 0;
        for (const auto& q : lvl) if (batchOfStack(q.first) == b) n++;
        if (n < 2) continue;                        // not a sweep, do not invent one
        int id = newSeries(b, "");
        if (App::Series* S = seriesById(id)) {
            S->paramName = "level";
            snprintf(S->unit, sizeof S->unit, "%s", app.lin.unit);
            S->kind = App::Series::KLinearity;
        }
        for (const auto& q : lvl)
            if (batchOfStack(q.first) == b) addToSeries(id, q.first, q.second);
        made++;
    }
    pruneEmptySeries();
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
    if (made) {
        toast("migrated " + std::to_string(made) +
              " series from an old session's stack levels");
        fprintf(stderr, "series: migrated %d from seqlevel\n", made);
    }
}

// The picker's "open as a sweep", once its stacks exist. Matching is BY NAME:
// the group name the picker accepted becomes the stack's name, and for a remote
// stack it is that name plus " [remote xN]" (openRemoteStack). The remote queue
// carries no seqId at all, so there is nothing else to match on.
//
// Nothing here invents anything: the values were read from the group names and
// shown in the picker before Load was pressed, and a group whose stack never
// opened is COUNTED and said out loud rather than dropped.
static void resolveOnePendingSeries(const App::SeriesPending& P) {
    std::vector<int> used;
    int id = 0, made = 0, lost = 0;
    for (const auto& e : P.byName) {
        int seqId = 0;
        for (const auto& si : app.seqs) {
            if (batchOfStack(si.id) != P.batchId) continue;
            if (std::find(used.begin(), used.end(), si.id) != used.end()) continue;
            bool same = si.name == e.first ||
                        (si.name.size() > e.first.size() + 10 &&
                         si.name.compare(0, e.first.size(), e.first) == 0 &&
                         si.name.compare(e.first.size(), 10, " [remote x") == 0);
            if (same) { seqId = si.id; break; }
        }
        if (!seqId) { lost++; continue; }
        used.push_back(seqId);
        if (!id) {                       // the first member that resolves makes it
            id = newSeries(P.batchId, P.name);
            if (App::Series* S = seriesById(id)) {
                S->paramName = P.paramName;
                snprintf(S->unit, sizeof S->unit, "%s", P.unit.c_str());
                S->kind = P.kind;
            }
        }
        if (addToSeries(id, seqId, e.second)) made++;
        else lost++;
    }
    // In VALUE order, not the order the folders happened to sort in: the groups
    // arrive as 0,10,160,20,320,40,80 (lexicographic), and a sweep is a
    // parameter axis - reading it out of order is reading it wrong. Unset
    // values keep their relative order and go last (they are not "before 0").
    // The modal's arrows and "Sort by value" still own the order after this.
    if (App::Series* S = seriesById(id))
        std::stable_sort(S->members.begin(), S->members.end(),
                         [](const App::Series::Member& a, const App::Series::Member& b) {
                             bool fa = std::isfinite(a.value), fb = std::isfinite(b.value);
                             if (fa != fb) return fa;          // set before unset
                             return fa && a.value < b.value;
                         });
    pruneEmptySeries();
    if (id && seriesById(id)) app.curSeriesId = id;
    const App::Series* S = seriesById(id);
    std::string msg = "sweep: " + std::to_string(made) + " stack(s) in series \"" +
                      (S ? S->name : std::string("(none)")) + "\"";
    if (lost) msg += "; " + std::to_string(lost) + " could not be matched";
    toast(msg, lost > 0);
    fprintf(stderr, "series: %s\n", msg.c_str());
}
// Every sweep the picker was told to open, in the order it was told. They all
// wait for the same drain (a member cannot be looked up before its stack
// exists), so they all resolve here - none of them is thrown away because a
// second Open happened while the first was still loading.
static void resolvePendingSeries() {
    std::vector<App::SeriesPending> queue;
    queue.swap(app.seriesPending);
    for (const auto& P : queue) resolveOnePendingSeries(P);
}

// called once per frame: integrate decoded frames, then chain the next stack
static void pumpSequenceAndQueue() {
    pumpSequence();
    // one restore at a time, matched by uid: indices shift as frames land
    if (!app.seqRestore.empty() && !app.seqRunning && !seqReadyPending()) {
        App::SeqRestore r = std::move(app.seqRestore.front());
        app.seqRestore.erase(app.seqRestore.begin());
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->uid == r.uid && app.images[i]->seqId == 0) {
                startSequenceLoad(i, r.files, r.pattern);
                break;
            }
        return;                 // let it start before chaining anything else
    }
    if (!app.seqRunning && !app.seqQueue.empty() && !seqReadyPending()) startNextQueuedGroup();
    // Series LAST, and only once everything a member could name is open: the
    // stacks arrive over many frames, and a lookup one frame too early would
    // report perfectly good members as missing.
    if ((!app.seriesRestore.empty() || !app.seqLevelLegacy.empty() ||
         !app.seriesPending.empty()) &&
        !app.seqRunning && app.seqQueue.empty() && app.seqRestore.empty() &&
        app.rbOpenQueue.empty() && !seqReadyPending()) {
        if (!app.seriesRestore.empty()) resolveSeriesRestore();
        if (!app.seqLevelLegacy.empty()) migrateLegacyLevels();
        if (!app.seriesPending.empty()) resolvePendingSeries();
    }
}

// ---- stacks & navigation ------------------------------------------------------
// image indices of one sequence, ordered by frame number
static std::vector<int> framesOfSeq(int seqId) {
    std::vector<std::pair<int, int>> v;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->seqId == seqId) v.emplace_back(app.images[i]->seqIndex, i);
    std::sort(v.begin(), v.end());
    std::vector<int> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}
// every stack in list order: a sequence is one stack, a lone image is its own
static std::vector<std::vector<int>> stacksOf() {
    std::vector<std::vector<int>> out;
    std::vector<int> seen;
    for (int i = 0; i < (int)app.images.size(); i++) {
        int sid = app.images[i]->seqId;
        if (sid == 0) { out.push_back({ i }); continue; }
        bool dup = false;
        for (int s : seen) if (s == sid) { dup = true; break; }
        if (dup) continue;
        seen.push_back(sid);
        out.push_back(framesOfSeq(sid));
    }
    return out;
}
// Cached view of the above: rebuilding it per frame cost one allocation per
// image plus a sort per sequence, just to draw a handful of rows.
static const std::vector<std::vector<int>>& stacksCached() {
    static std::vector<std::vector<int>> cache;
    static uint64_t rev = 0;
    if (rev != app.imagesRev) { rev = app.imagesRev; cache = stacksOf(); }
    return cache;
}
static App::SeqInfo* seqInfo(int id) {
    for (auto& s : app.seqs) if (s.id == id) return &s;
    return nullptr;
}
static void selectImage(int idx) {
    if (idx < 0 || idx >= (int)app.images.size()) return;
    // Walking A onto the image pinned as B would silently switch compare off
    // (B must not be A). Swap instead, so stepping through a stack against a
    // pinned frame keeps comparing in both directions.
    if (app.compareMode != App::CmpOff && cur() && app.images[idx].get() != cur() &&
        app.compareBUid && app.images[idx]->uid == app.compareBUid) {
        setCompareB(cur());
        toast("A / B swapped");
    }
    ImageDoc* prev = cur();
    if (prev && app.images[idx].get() != prev) app.prevImageUid = prev->uid;
    // Held-down frame stepping: B's caches cost real milliseconds per step
    // (measured, see the A/B stats commits), so a key repeat would drag the
    // whole UI down for as long as the key is held. Two switches inside 300 ms
    // means "still stepping"; the B slots then hold their last result and say
    // so, and refresh once the stepping stops. Same shape as annBusy.
    {
        static double lastSwitch = -1e9;
        double now = glfwGetTime();
        if (now - lastSwitch < 0.30) app.abStepBusyUntil = now + 0.30;
        lastSwitch = now;
    }
    app.current = idx;
    ImageDoc* d = app.images[idx].get();
    // Value-range scope. The stack default (frames inherit the reference
    // frame's range so they compare directly) is only ONE of three sensible
    // policies, and it silently defeated Auto while stepping frames:
    //   0 per frame  - every frame shows its own min..max
    //   1 per stack  - the inherited behavior (default, unchanged)
    //   2 everything - the range follows you across stacks too
    if (prev && d != prev) {
        // linked mode overlays one range non-destructively (effBlack/effWhite),
        // so only auto-per-frame touches the image's own numbers here
        if (app.rangeScope == 0 && !app.linkRange) { defaultRange(*d); d->texDirty = true; }
    }
    if (d->pendingViewScale != 1.0f) {   // its preview->full swap happened off screen
        app.view.zoom = std::max(app.view.zoom / d->pendingViewScale, 1.0f / 512);
        app.view.center.x *= d->pendingViewScale;
        app.view.center.y *= d->pendingViewScale;
        d->pendingViewScale = 1;
    }
    if (App::SeqInfo* si = seqInfo(d->seqId)) si->lastImageIdx = idx;
}
// time axis: previous / next frame of the current stack
static void gotoFrame(int delta, bool absoluteEdge = false, bool toFirst = true) {
    ImageDoc* im = cur();
    if (!im) return;
    if (im->seqId == 0) return;                       // lone image: no time axis
    std::vector<int> f = framesOfSeq(im->seqId);
    if (f.empty()) return;
    int pos = 0;
    for (int i = 0; i < (int)f.size(); i++) if (f[i] == app.current) pos = i;
    int np = absoluteEdge ? (toFirst ? 0 : (int)f.size() - 1)
                          : std::clamp(pos + delta, 0, (int)f.size() - 1);
    selectImage(f[np]);
}
// stack axis: previous / next stack, resuming its last viewed frame
static void gotoStack(int delta) {
    const auto& st = stacksCached();
    if (st.empty()) return;
    int cs = 0;
    for (int i = 0; i < (int)st.size(); i++)
        for (int idx : st[i]) if (idx == app.current) cs = i;
    int ns = std::clamp(cs + delta, 0, (int)st.size() - 1);
    if (ns == cs) return;
    const std::vector<int>& target = st[ns];
    int pick = target.front();
    if (App::SeqInfo* si = seqInfo(app.images[target.front()]->seqId))
        if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
            app.images[si->lastImageIdx]->seqId == app.images[target.front()]->seqId)
            pick = si->lastImageIdx;
    selectImage(pick);
}

// ---- open a whole folder tree: every numbered group becomes its own stack ----
static const char* SEQ_EXTS[] = { ".npy", ".npz", ".bin", ".raw", ".yuv", ".dat", ".rggb" };
static bool isLoadableExt(const std::string& extLower) {
    for (const char* e : SEQ_EXTS) if (extLower == e) return true;
    return false;
}

// Walk root recursively; each directory yields one group per numeric pattern.
static std::vector<App::PendingGroup> scanFolderGroups(const std::string& root) {
    std::vector<App::PendingGroup> groups;
    std::error_code ec;
    std::filesystem::path rootPath = pathFromUtf8(root);
    if (!std::filesystem::is_directory(rootPath, ec)) return groups;
    const size_t MAX_GROUPS = 256;    // a truncated scan is reported, never silent
    // collect loadable files per directory (depth-limited walk)
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> perDir;
    auto addFile = [&](const std::filesystem::path& p) {
        std::string ext = p.extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!isLoadableExt(ext)) return;
        std::filesystem::path dir = p.parent_path();
        for (auto& d : perDir)
            if (d.first == dir) { d.second.push_back(p.u8string()); return; }
        perDir.push_back({ dir, { p.u8string() } });
    };
    // Manual breadth-first walk: recursive_directory_iterator aborts the whole
    // scan when a single entry cannot be read (a permission error or a dangling
    // link would silently truncate the folder list).
    {
        std::vector<std::pair<std::filesystem::path, int>> todo{ { rootPath, 0 } };
        while (!todo.empty()) {
            auto [dir, depth] = todo.back();
            todo.pop_back();
            std::error_code dec;
            std::filesystem::directory_iterator dit(dir, dec), dend;
            if (dec) continue;                       // unreadable dir: skip just this one
            for (; dit != dend; dit.increment(dec)) {
                if (dec) { dec.clear(); break; }
                std::error_code fec;
                if (dit->is_directory(fec)) {
                    if (depth < 3) todo.push_back({ dit->path(), depth + 1 });
                } else if (dit->is_regular_file(fec)) {
                    addFile(dit->path());
                }
            }
        }
    }
    std::sort(perDir.begin(), perDir.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string rootStr = rootPath.u8string();
    for (auto& d : perDir) {
        std::vector<std::string> files = d.second;
        std::sort(files.begin(), files.end());
        std::vector<bool> used(files.size(), false);
        // name the stack by its folder relative to the opened root
        std::string rel = d.first.u8string();
        if (rel.size() > rootStr.size()) rel = rel.substr(rootStr.size() + 1);
        else rel.clear();
        auto push = [&](std::vector<std::string> fs, std::string pattern) {
            if (groups.size() >= MAX_GROUPS) return;
            App::PendingGroup g;
            g.files = std::move(fs);
            g.name = rel.empty() ? pattern : rel + "/" + pattern;
            std::string ext = std::filesystem::u8path(g.files[0]).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            g.isRaw = ext != ".npy";
            groups.push_back(std::move(g));
        };
        std::vector<std::string> leftover;
        for (size_t i = 0; i < files.size(); i++) {
            if (used[i]) continue;
            std::string pattern;
            std::vector<std::string> sibs = findSequenceSiblings(files[i], pattern);
            if (sibs.size() >= 2) {
                for (const auto& s : sibs)
                    for (size_t k = 0; k < files.size(); k++)
                        if (files[k] == s) used[k] = true;
                push(std::move(sibs), pattern);
            } else {
                used[i] = true;
                leftover.push_back(files[i]);
            }
        }
        // Two or more unnumbered leftovers = ONE stack in natural order, same
        // fold the remote SCAN does: capture sets are not always numbered, and
        // N single-frame stacks from one folder is never what Open Folder
        // meant. Mixed extensions stay apart (raw needs one recipe per group).
        std::map<std::string, std::vector<std::string>> byExt;
        for (auto& f : leftover) {
            std::string ext = std::filesystem::u8path(f).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            byExt[ext].push_back(std::move(f));
        }
        for (auto& [ext, fs] : byExt) {
            if (fs.size() >= 2) {
                std::vector<std::string> sorted = fs;
                sortFramesNumerically(sorted);
                push(std::move(sorted), "*" + ext);
            } else {
                push({ fs[0] }, baseName(fs[0]));
            }
        }
    }
    return groups;
}

// Start the next queued stack once the loader is idle. Raw stacks need format
// settings: the dialog is shown once and the recipe is reused for the rest.
static void startNextQueuedGroup() {
    if (app.seqQueue.empty() || app.seqRunning || rawDlg.open || rawDlg.forQueue ||
        app.folderPickOpen) return;
    App::PendingGroup g = app.seqQueue.front();
    if (g.isRaw && !app.folderRecipeValid) {
        openRawDialogFor(g.files[0]);          // ask once for the whole batch
        if (rawDlg.open) {
            rawDlg.forQueue = true;
            rawDlg.queueCount = (int)app.seqQueue.size();
        } else {
            app.seqQueue.erase(app.seqQueue.begin());   // unreadable: skip
        }
        return;
    }
    app.seqQueue.erase(app.seqQueue.begin());
    std::string err;
    g_quietLoad = true;               // keep the user's current image selected
    app.loadBatchId = g.batchId;      // the head frame lands in the group's batch
    if (g.isRaw) {
        RawDialog d = g_folderRecipe;
        d.path = g.files[0];
        d.replaceIdx = -1;
        err = loadRaw(d);
    } else {
        err = loadNpy(g.files[0]);
    }
    g_quietLoad = false;
    app.loadBatchId = 0;
    if (!err.empty()) { toast(baseName(g.files[0]) + ": " + err, true); return; }
    // reference the image we just appended, not app.current (which may be elsewhere)
    int idx = (int)app.images.size() - 1;
    if (g.files.size() >= 2) startSequenceLoad(idx, g.files, g.name);
}

static void enqueueGroups(std::vector<App::PendingGroup> groups) {
    if (groups.empty()) return;
    app.folderRecipeValid = false;
    // APPEND. This used to be `app.seqQueue = std::move(groups)`, so a second
    // Open while the first was still loading silently CANCELLED every stack the
    // first Open had not started yet - the second Open's folder came up looking
    // complete while most of the first one was simply gone. startNextQueuedGroup
    // already refuses to start while one is running, so the queue just gets
    // longer. (Found while reproducing the series pending-slot finding: fixing
    // the slot alone still left the first sweep with one stack out of seven.)
    int added = (int)groups.size(), frames = 0;
    for (const auto& g : groups) frames += (int)g.files.size();
    app.seqQueue.insert(app.seqQueue.end(), std::make_move_iterator(groups.begin()),
                        std::make_move_iterator(groups.end()));
    toast("opening " + std::to_string(added) + " stack(s), " +
          std::to_string(frames) + " files");
    startNextQueuedGroup();
}

// Minimal glob: * (any run), ? (one char), case-insensitive, no character classes.
static bool globMatch(const char* pat, const char* str) {
    const char *star = nullptr, *ss = str;
    while (*str) {
        char p = *pat, s = *str;
        if (p == '?' || (p && tolower((unsigned char)p) == tolower((unsigned char)s))) { pat++; str++; continue; }
        if (p == '*') { star = pat++; ss = str; continue; }
        if (star) { pat = star + 1; str = ++ss; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}
// Comma-separated patterns; a bare word is treated as a substring (*word*).
// Used by the remote listing filter (the picker has its own live filter).
static bool globListMatch(const char* list, const std::string& subject) {
    if (!list || !*list) return false;
    std::string cur;
    bool any = false;
    auto test = [&](std::string p) {
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
        if (p.empty()) return;
        if (p.find('*') == std::string::npos && p.find('?') == std::string::npos)
            p = "*" + p + "*";
        if (globMatch(p.c_str(), subject.c_str())) any = true;
    };
    for (const char* c = list; ; c++) {
        if (*c == ',' || *c == 0) { test(cur); cur.clear(); if (!*c) break; }
        else cur.push_back(*c);
    }
    return any;
}

// Read just enough of a .npy header for "24x1200x1600 u16": the picker shows it
// per group and the merge warning compares it. Never touches pixel data; any
// oddity returns "" (rendered as nothing, the pre-metadata behavior).
static std::string peekNpyShape(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return {};
    uint8_t h[12] = {};
    f.read((char*)h, 12);
    if (f.gcount() < 10 || h[0] != 0x93 || memcmp(h + 1, "NUMPY", 5) != 0) return {};
    size_t hlen, hoff;
    if (h[6] >= 2) { uint32_t v; memcpy(&v, h + 8, 4); hlen = v; hoff = 12; }
    else           { uint16_t v; memcpy(&v, h + 8, 2); hlen = v; hoff = 10; }
    if (hlen == 0 || hlen > 65536) return {};
    std::string hdr(hlen, '\0');
    f.seekg((std::streamoff)hoff);
    f.read(&hdr[0], (std::streamsize)hlen);
    if ((size_t)f.gcount() != hlen) return {};
    size_t k = hdr.find("'descr'");
    if (k == std::string::npos) return {};
    size_t q1 = hdr.find('\'', hdr.find(':', k));
    if (q1 == std::string::npos) return {};
    size_t q2 = hdr.find('\'', q1 + 1);
    if (q2 == std::string::npos) return {};
    std::string code = hdr.substr(q1 + 1, q2 - q1 - 1);
    if (!code.empty() && (code[0] == '<' || code[0] == '>' || code[0] == '|' || code[0] == '='))
        code = code.substr(1);
    static const struct { const char* np; const char* name; } DT[] = {
        { "u1", "u8" }, { "i1", "i8" }, { "b1", "bool" }, { "u2", "u16" },
        { "i2", "i16" }, { "u4", "u32" }, { "i4", "i32" }, { "f4", "f32" }, { "f8", "f64" },
    };
    for (const auto& d : DT) if (code == d.np) { code = d.name; break; }
    size_t sp = hdr.find("'shape'");
    size_t p1 = hdr.find('(', sp), p2 = hdr.find(')', sp);
    if (sp == std::string::npos || p1 == std::string::npos || p2 == std::string::npos) return {};
    std::string dims;
    for (size_t i = p1 + 1; i < p2; i++) {
        char c = hdr[i];
        if (c >= '0' && c <= '9') dims.push_back(c);
        else if (c == ',' && !dims.empty() && dims.back() != 'x') dims.push_back('x');
    }
    while (!dims.empty() && dims.back() == 'x') dims.pop_back();
    if (dims.empty()) return {};
    return dims + " " + code;
}

// The picker's ONE live filter, applied as the user types. Space-separated
// terms AND together; a term matches if it appears ANYWHERE in a file's
// "folder/name" relative to the scanned root (case-insensitive, * and ?
// wildcards allowed); a '!' prefix turns a term into an exclusion. This cuts
// each group's FILE list - accepted groups load only the surviving files - and
// never touches the checkboxes: the filter narrows what is visible, the
// checkboxes choose which groups open.
static void applyPickFilter() {
    std::vector<std::string> inc, exc;
    {
        std::string t;
        auto flush = [&]() {
            if (t.empty()) return;
            if (t[0] == '!') { if (t.size() > 1) exc.push_back(t.substr(1)); }
            else inc.push_back(t);
            t.clear();
        };
        for (const char* c = app.pickFilter; ; c++) {
            if (*c == 0 || *c == ' ' || *c == '\t') { flush(); if (!*c) break; }
            else t.push_back(*c);
        }
    }
    auto hit = [](const std::string& term, const std::string& subj) {
        std::string p = "*" + term + "*";      // match anywhere: "frame_0??" must
        return globMatch(p.c_str(), subj.c_str());   // not need to cover ".npy"
    };
    for (auto& e : app.folderPick) {
        e.match.assign(e.g.files.size(), 1);
        e.nMatch = 0;
        for (size_t i = 0; i < e.rel.size() && i < e.match.size(); i++) {
            bool ok = true;
            for (const auto& t : inc) if (!hit(t, e.rel[i])) { ok = false; break; }
            if (ok) for (const auto& t : exc) if (hit(t, e.rel[i])) { ok = false; break; }
            e.match[i] = ok ? 1 : 0;
            if (ok) e.nMatch++;
        }
    }
}

// Turn scan results into an open picker: per-file relative paths for the
// filter, npy shapes for the merge warning, fresh filter and load mode.
// stripRoot is the scanned directory (local path or remote dir); displayRoot
// is what the dialog's first line shows (local path or ssh:// url).
static void openPickerWith(std::vector<App::PendingGroup> groups,
                           const std::string& displayRoot, const std::string& stripRoot,
                           bool remoteMode, const std::string& host) {
    std::string rootN = stripRoot;
    std::replace(rootN.begin(), rootN.end(), '\\', '/');
    while (!rootN.empty() && rootN.back() == '/') rootN.pop_back();
    auto ieq = [](const std::string& a, const std::string& b, size_t n) {
        for (size_t i = 0; i < n; i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    };
    app.folderPick.clear();
    for (auto& g : groups) {
        App::FolderPick e;
        e.g = std::move(g);
        if (e.g.shape.empty() && !e.g.isRaw && !remoteMode &&
            e.g.files[0].size() > 4 &&
            e.g.files[0].compare(e.g.files[0].size() - 4, 4, ".npy") == 0)
            e.g.shape = peekNpyShape(e.g.files[0]);
        e.rel.reserve(e.g.files.size());
        for (const auto& f : e.g.files) {
            std::string r = f;
            std::replace(r.begin(), r.end(), '\\', '/');
            if (r.size() > rootN.size() + 1 && r[rootN.size()] == '/' &&
                ieq(r, rootN, rootN.size()))
                r = r.substr(rootN.size() + 1);
            else
                r = baseName(r);
            e.rel.push_back(std::move(r));
        }
        app.folderPick.push_back(std::move(e));
    }
    app.folderPickRoot = displayRoot;
    app.folderPickRemote = remoteMode;     // a stale remote flag would misroute these
    app.folderPickHost = host;
    app.pickFilter[0] = 0;                 // a leftover filter would silently cut
    app.pickMerge = 0;                     // the new scan - start every scan clean
    app.pickBatchMode = 0;                 // batch layout too: one batch is the canon
    // ...and the sweep, which is the one that could invent a measurement: a
    // parameter name and a unit left over from the last Open would be applied to
    // a folder that has nothing to do with it, and "単位を仮定しない" means not
    // assuming the PREVIOUS one either.
    app.pickSweep = false;
    app.pickSweepParam[0] = '\0';
    app.pickSweepUnit[0] = '\0';
    {   // batch names stem from the scanned root's leaf ("batchset", "scanroot")
        std::string leaf = rootN;
        size_t sl = leaf.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < leaf.size()) leaf = leaf.substr(sl + 1);
        app.folderPickBatchBase = leaf.empty() ? "opened" : leaf;
    }
    applyPickFilter();
    app.folderPickOpen = true;
}

static void openFolder(const std::string& path) {
    std::vector<App::PendingGroup> groups = scanFolderGroups(path);
    if (groups.empty()) { toast("no loadable files under " + baseName(path), true); return; }
    if (groups.size() >= 256)
        toast("scan stopped at 256 sequences - narrow the folder or use the filter", true);
    // ALWAYS ask, one group included - same rule the remote scan follows. The
    // picker is where the file filter lives (UC3: "open only the *_dark*
    // frames of this folder" needs the dialog even when the folder groups into
    // a single sequence), and "Always load folder" no longer mutes it: that
    // setting keeps its original job - loading a single FILE's numbered
    // siblings without asking. Headless callers auto-accept via pickerAccept().
    // Batches are created at ACCEPT time (pickerAccept), not here: creating one
    // per scan left an empty batch behind on Cancel, and the picker's batch
    // mode (one / per top folder) is only known once the user answers.
    openPickerWith(std::move(groups), path, path, false, "");
}

// Stack name for a merged load: the common prefix of the merged group names,
// trimmed of separator/placeholder debris; the root folder's name when the
// prefix says nothing (three characters or less).
static std::string mergedStackName(const std::vector<App::PendingGroup>& sel) {
    std::string p = sel.empty() ? std::string() : sel[0].name;
    for (const auto& g : sel) {
        size_t n = 0;
        while (n < p.size() && n < g.name.size() &&
               tolower((unsigned char)p[n]) == tolower((unsigned char)g.name[n])) n++;
        p.resize(n);
    }
    while (!p.empty() && strchr("?*_-/. ", p.back())) p.pop_back();
    if (p.size() <= 3) p = baseName(app.folderPickRoot);
    return p + " (merged)";
}

// The picker's answer: selected groups with their file lists cut to the live
// filter (UC3), folded into ONE group when merge mode is on (UC2). Shared by
// the Load button and every headless auto-accept, so the selftests exercise
// exactly what the button does. Returns empty and sets err for a merge the
// loader could not honor (raw and npy need different decode paths).
static std::vector<App::PendingGroup> pickerSelection(std::string* errOut = nullptr) {
    std::vector<App::PendingGroup> sel;
    for (const auto& e : app.folderPick) {
        if (!e.selected || e.nMatch == 0) continue;
        App::PendingGroup g;
        g.name = e.g.name; g.isRaw = e.g.isRaw; g.batchId = e.g.batchId; g.shape = e.g.shape;
        for (size_t i = 0; i < e.g.files.size(); i++)
            if (i < e.match.size() && e.match[i]) g.files.push_back(e.g.files[i]);
        sel.push_back(std::move(g));
    }
    if (app.pickMerge == 1 && !sel.empty()) {
        bool anyRaw = false, anyNpy = false;
        for (const auto& g : sel) (g.isRaw ? anyRaw : anyNpy) = true;
        if (anyRaw && anyNpy) {
            if (errOut) *errOut = "cannot merge raw and npy files into one stack";
            return {};
        }
        App::PendingGroup m;
        m.isRaw = anyRaw;
        m.batchId = sel[0].batchId;
        m.shape = sel[0].shape;
        m.name = mergedStackName(sel);
        for (auto& g : sel)
            m.files.insert(m.files.end(), std::make_move_iterator(g.files.begin()),
                           std::make_move_iterator(g.files.end()));
        sortFramesNumerically(m.files);    // natural order across the union
        sel.clear();
        sel.push_back(std::move(m));
    }
    return sel;
}

// Close the picker and open its selection through the route the scan came from.
// No ImGui calls: the headless selftests and the auto-accept blocks in main()
// go through this exact function.
static void pickerAccept() {
    std::string err;
    std::vector<App::PendingGroup> sel = pickerSelection(&err);
    bool remote = app.folderPickRemote;
    std::string host = app.folderPickHost;
    // "as a sweep" is exclusive with batch-per-top-folder HERE, not only in the
    // dialog: a series lives in one batch, and that has to hold for every
    // caller, headless ones included.
    bool sweep = app.pickSweep && app.pickMerge == 0;
    int batchMode = (app.pickMerge == 1 || sweep) ? 0 : app.pickBatchMode;
    std::string swParam = app.pickSweepParam, swUnit = app.pickSweepUnit;
    app.pickSweep = false;            // a per-Open choice, never sticky
    std::string batchBase = app.folderPickBatchBase;
    app.folderPick.clear();
    app.folderPickOpen = false;
    if (sel.empty()) { toast(err.empty() ? "nothing selected" : err, true); return; }
    // Batches are created HERE, on accept - Cancel used to leave an empty one
    // behind. Mode 0 (default): the whole Open is one batch named for the root.
    // Mode 1: one batch per TOP folder of the group name, "root/00" style, so
    // per-condition folders open as ready-made analysis groupings.
    if (batchMode == 1) {
        std::vector<std::pair<std::string, int>> made;   // top folder -> batchId
        for (auto& g : sel) {
            size_t sl = g.name.find('/');
            std::string top = sl == std::string::npos ? std::string() : g.name.substr(0, sl);
            int id = 0;
            for (const auto& m : made) if (m.first == top) id = m.second;
            if (!id) {
                id = newBatch(uniqueBatchName(top.empty() ? batchBase
                                                          : batchBase + "/" + top));
                made.emplace_back(top, id);
            }
            g.batchId = id;
        }
    } else {
        int id = newBatch(uniqueBatchName(batchBase));
        for (auto& g : sel) g.batchId = id;
    }
    // The sweep is only a NOTE here - the stacks do not exist yet, and half of
    // them will arrive over the next several seconds. Built before sel is moved
    // away below; two groups minimum, because one point is not a sweep.
    if (sweep && sel.size() >= 2) {
        App::SeriesPending P;
        P.batchId = sel.front().batchId;
        P.paramName = swParam;
        P.unit = swUnit;                       // may be empty: then no fit, and
        P.kind = App::Series::KLinearity;      // the panel says exactly that
        for (const auto& g : sel)
            P.byName.emplace_back(g.name, extractLevelFromName(g.name));
        app.seriesPending.push_back(std::move(P));   // QUEUED, never overwritten
    }
    if (remote) {
        // remote groups: through the open queue, one stack at a time, so the
        // memory budget is applied against reality
        int frames = 0;
        for (const auto& g : sel) frames += (int)g.files.size();
        toast("opening " + std::to_string(sel.size()) + " remote stack(s), " +
              std::to_string(frames) + " frames");
        for (auto& g : sel)
            app.rbOpenQueue.push_back({ host, std::move(g.files), g.name, g.batchId });
    } else {
        enqueueGroups(std::move(sel));
    }
}

// Tree of what the scan found. One live filter narrows FILES as you type;
// checkboxes choose groups; the footer picks between "one stack per sequence"
// and "everything as ONE stack". Group count is capped at 256 by the scans, so
// the tree needs no clipper - the per-group FILE lists (unbounded) get one.
static void drawFolderPickModal() {
    if (app.folderPickOpen && !ImGui::IsPopupOpen("Select sequences")) {
        fprintf(stderr, "picker: OpenPopup (remote=%d, %d groups)\n",
                app.folderPickRemote ? 1 : 0, (int)app.folderPick.size());
        ImGui::OpenPopup("Select sequences");
    }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * app.uiScale, 520 * app.uiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 18),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("Select sequences", nullptr)) return;

    ImGui::TextDisabled("%s", app.folderPickRoot.c_str());
    int totalFiles = 0, matchFiles = 0, selGroups = 0, selFiles = 0;
    for (const auto& e : app.folderPick) {
        totalFiles += (int)e.g.files.size();
        matchFiles += e.nMatch;
        if (e.nMatch == 0) continue;               // fully filtered out: not loadable
        if (e.selected) { selGroups++; selFiles += e.nMatch; }
    }

    // ONE live filter, no Apply button: each keystroke re-cuts the tree below.
    {
        float countW = ImGui::CalcTextSize("00000 / 00000 files match").x;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - countW -
                                         ImGui::GetStyle().ItemSpacing.x,
                                         ImGui::GetFontSize() * 8));
        if (ImGui::InputTextWithHint("##pickfilter", "filter files - e.g. dark  !ng  *_A.npy",
                                     app.pickFilter, sizeof app.pickFilter))
            applyPickFilter();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "Narrows WHICH FILES will load, as you type.\n"
                "Each word is looked for anywhere in a file's \"folder/filename\"\n"
                "(shown when you expand a sequence below). All words must match.\n"
                "    dark          keep files whose folder or name contains \"dark\"\n"
                "    !ng           drop files matching \"ng\"\n"
                "    10lx *_A.npy  words combine; * and ? wildcards work anywhere\n"
                "Sequences show \"kept / total\"; only the kept files are loaded.");
        ImGui::SameLine();
        if (app.pickFilter[0]) ImGui::Text("%d / %d files match", matchFiles, totalFiles);
        else ImGui::TextDisabled("%d sequence(s), %d files", (int)app.folderPick.size(), totalFiles);
    }
    if (ImGui::SmallButton("All")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = false; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Invert")) { for (auto& e : app.folderPick) if (e.nMatch) e.selected = !e.selected; }
    ImGui::Separator();

    // footer: mode row + status/warning line(s) + button row, measured not guessed
    std::vector<std::string> shapes;               // distinct known shapes in selection
    bool mixedRawNpy = false;
    {
        bool anyRaw = false, anyNpy = false;
        for (const auto& e : app.folderPick) {
            if (!e.selected || e.nMatch == 0) continue;
            (e.g.isRaw ? anyRaw : anyNpy) = true;
            if (e.g.shape.empty()) continue;
            bool dup = false;
            for (const auto& s : shapes) if (s == e.g.shape) { dup = true; break; }
            if (!dup) shapes.push_back(e.g.shape);
        }
        mixedRawNpy = anyRaw && anyNpy;
    }
    bool mergeWarn = app.pickMerge == 1 && (mixedRawNpy || shapes.size() > 1);
    // The sweep row exists only where a sweep could: separate stacks, two or
    // more of them. Hidden, the box is FORCED OFF - a control nobody can see
    // must not be able to create a series (docs/terminology.md: never auto).
    const bool sweepRow = app.pickMerge == 0 && selGroups >= 2;
    if (!sweepRow) app.pickSweep = false;
    float footer = ImGui::GetFrameHeightWithSpacing() * (sweepRow ? 4 : 3) +
                   ImGui::GetTextLineHeightWithSpacing() *
                       ((mergeWarn ? 2 : 1) + (app.pickSweep ? 1 : 0));
    ImGui::BeginChild("picktree", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    // group the flat list by the folder part of "folder/pattern"
    std::vector<std::string> folders;
    auto folderOf = [](const App::FolderPick& e) {
        size_t s = e.g.name.find_last_of('/');
        return s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
    };
    for (const auto& e : app.folderPick) {
        if (e.nMatch == 0) continue;               // the filter emptied it: hide
        std::string f = folderOf(e);
        bool dup = false;
        for (const auto& x : folders) if (x == f) { dup = true; break; }
        if (!dup) folders.push_back(f);
    }
    if (folders.empty())
        ImGui::TextDisabled("no files match \"%s\"", app.pickFilter);
    for (const auto& f : folders) {
        ImGui::PushID(f.c_str());
        bool all = true, any = false;
        int files = 0;
        for (const auto& e : app.folderPick) {
            if (e.nMatch == 0 || folderOf(e) != f) continue;
            files += e.nMatch;
            e.selected ? (any = true) : (all = false);
        }
        bool parent = all;
        if (ImGui::Checkbox("##folder", &parent)) {
            for (auto& e : app.folderPick)
                if (e.nMatch > 0 && folderOf(e) == f) e.selected = parent;
        }
        ImGui::SameLine();
        if (!all && any) ImGui::TextDisabled("~");     // partial selection marker
        else ImGui::TextDisabled(" ");
        ImGui::SameLine();
        char hdr[320];
        snprintf(hdr, sizeof hdr, "%s   (%d files)", f.c_str(), files);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (auto& e : app.folderPick) {
                if (e.nMatch == 0 || folderOf(e) != f) continue;
                size_t s = e.g.name.find_last_of('/');
                std::string leaf = s == std::string::npos ? e.g.name : e.g.name.substr(s + 1);
                ImGui::PushID(&e);
                ImGui::Checkbox("##sel", &e.selected);
                ImGui::SameLine();
                // expandable: the files behind the pattern, so "what does the
                // filter keep?" has a visible answer. Label text goes through
                // TextUnformatted ('#' and '?' in names stay visible).
                bool open = ImGui::TreeNodeEx("##files", ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::SameLine();
                char lb[352];
                if (e.nMatch == (int)e.g.files.size())
                    snprintf(lb, sizeof lb, "%s   %d file(s)%s%s%s", leaf.c_str(), e.nMatch,
                             e.g.isRaw ? "  [raw]" : "",
                             e.g.shape.empty() ? "" : "  ", e.g.shape.c_str());
                else
                    snprintf(lb, sizeof lb, "%s   %d / %d files%s%s%s", leaf.c_str(), e.nMatch,
                             (int)e.g.files.size(), e.g.isRaw ? "  [raw]" : "",
                             e.g.shape.empty() ? "" : "  ", e.g.shape.c_str());
                ImGui::TextUnformatted(lb);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.g.files.front().c_str());
                // the sweep preview: the value this group's NAME suggests, on
                // the row it was read from, BEFORE Load is pressed. A proposal
                // nobody looked at is not a confirmation.
                if (app.pickSweep && e.selected) {
                    std::string from;
                    double v = extractLevelFromName(e.g.name, &from);
                    ImGui::SameLine();
                    if (std::isfinite(v))   // a number per NOTHING says so, here too
                        ImGui::TextDisabled("-> %.6g %s", v,
                                            app.pickSweepUnit[0] ? app.pickSweepUnit
                                                                 : "[unit not set]");
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "-> no value in the name");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(std::isfinite(v)
                            ? "read from \"%s\" - correct it later in Edit series..."
                            : "nothing numeric in \"%s\": this stack joins UNSET,\n"
                              "and an unset value never enters a fit as 0.",
                            from.empty() ? e.g.name.c_str() : from.c_str());
                }
                if (open) {
                    std::vector<int> vis;          // files the filter keeps
                    vis.reserve(e.nMatch);
                    for (int i = 0; i < (int)e.g.files.size(); i++)
                        if (i < (int)e.match.size() && e.match[i]) vis.push_back(i);
                    ImGuiListClipper cl;           // thousands of frames stay cheap
                    cl.Begin((int)vis.size());
                    while (cl.Step())
                        for (int row = cl.DisplayStart; row < cl.DisplayEnd; row++) {
                            ImGui::TextDisabled("%s", e.rel[vis[row]].c_str());
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", e.g.files[vis[row]].c_str());
                        }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // load mode: UC1 (one stack per sequence) vs UC2/UC3 (everything as ONE stack)
    {
        char m0[64], m1[64];
        snprintf(m0, sizeof m0, "%d separate stack(s)###mode0", selGroups);
        snprintf(m1, sizeof m1, "ONE stack of %d file(s)###mode1", selFiles);
        ImGui::RadioButton(m0, &app.pickMerge, 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("each checked sequence becomes its own stack");
        ImGui::SameLine();
        ImGui::RadioButton(m1, &app.pickMerge, 1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ALL checked files merge into a single stack,\n"
                              "frames in natural (numeric) order - for a capture\n"
                              "split across folders, or a filtered subset");
    }
    // batch row: how the accepted selection is GROUPED in the Files panel.
    // One batch per Open is the canon's default; per-top-folder splits a root
    // of condition folders into ready-made analysis groupings. Merge wins over
    // this (a single merged stack is one batch by construction).
    {
        int topFolders = 0;
        {
            std::vector<std::string> tops;
            for (const auto& e : app.folderPick) {
                if (!e.selected || e.nMatch == 0) continue;
                size_t sl = e.g.name.find('/');
                std::string top = sl == std::string::npos ? std::string()
                                                          : e.g.name.substr(0, sl);
                bool dup = false;
                for (const auto& t : tops) if (t == top) { dup = true; break; }
                if (!dup) tops.push_back(top);
            }
            topFolders = (int)tops.size();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("batch:");
        ImGui::SameLine();
        ImGui::BeginDisabled(app.pickMerge == 1 || app.pickSweep);
        ImGui::RadioButton("one batch###batch0", &app.pickBatchMode, 0);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("everything from this Open lands under ONE Files heading");
        ImGui::SameLine();
        char b1[80];
        snprintf(b1, sizeof b1, "%d batches (one per top folder)###batch1", topFolders);
        ImGui::RadioButton(b1, &app.pickBatchMode, 1);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(app.pickSweep
                ? "not while \"open as a sweep\" is on: a series lives in ONE\n"
                  "batch, so one batch is the only grouping that can hold it"
                : "each top-level folder of the scanned root becomes\n"
                  "its own batch (its own Files heading)");
        ImGui::EndDisabled();
    }
    // ---- row 3: open as a sweep (docs/series-plan.md §5) --------------------
    // The one place in the program that creates a series without the modal, and
    // it does it because a human ticked a box that says so. Unticked, NOTHING
    // is created - a sweep guessed from a folder tree is a lie that fits.
    if (sweepRow) {
        ImGui::Checkbox("open as a sweep (creates a series)", &app.pickSweep);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The %d selected sequences become one SERIES: each stack keeps\n"
                              "the value read from its name, and linearity / PTC measure the\n"
                              "series as a whole.\n"
                              "Nothing is created unless this is ticked.", selGroups);
        if (app.pickSweep) {
            app.pickBatchMode = 0;              // series ⊂ batch, strictly
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            ImGui::InputTextWithHint("##swparam", "parameter", app.pickSweepParam,
                                     sizeof app.pickSweepParam);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("what was swept: illuminance, exposure, temperature ...");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            ImGui::InputTextWithHint("##swunit", "unit", app.pickSweepUnit,
                                     sizeof app.pickSweepUnit);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("lx, ms, photons/px ... LEAVE IT EMPTY if you do not\n"
                                  "know yet: an unset unit means no fit, which is the\n"
                                  "honest answer, and Edit series... sets it later.");
        }
    }
    if (app.pickSweep) {
        int valued = 0;
        for (const auto& e : app.folderPick)
            if (e.selected && e.nMatch && std::isfinite(extractLevelFromName(e.g.name)))
                valued++;
        if (valued < selGroups)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1),
                               "%d of %d have a value in their name; the rest join UNSET "
                               "(never 0)", valued, selGroups);
        else if (!app.pickSweepUnit[0])
            ImGui::TextDisabled("all %d have a value. No unit yet - the series opens, "
                                "the fit waits for one.", valued);
        else
            ImGui::TextDisabled("all %d have a value, in %s", valued, app.pickSweepUnit);
    }
    bool loadable = selGroups > 0 && !(app.pickMerge == 1 && mixedRawNpy);
    if (app.pickMerge == 1 && mixedRawNpy)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "cannot merge raw and npy files into one stack");
    else if (app.pickMerge == 1 && shapes.size() > 1)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "shapes differ (%s vs %s) - mismatched frames are skipped at load",
                           shapes[0].c_str(), shapes[1].c_str());
    else
        ImGui::Text("selected: %d sequence(s), %d files", selGroups, selFiles);
    if (mergeWarn && app.pickMerge == 1 && shapes.size() > 1 && !mixedRawNpy)
        ImGui::TextDisabled("check the shape column above to see which sequences differ");
    ImGui::BeginDisabled(!loadable);
    bool load = ImGui::Button(app.pickMerge == 1 ? "Load as ONE stack"
                              : app.pickSweep   ? "Load as a sweep"
                                                : "Load selected",
                              ImVec2(150 * app.uiScale, 0));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        app.folderPick.clear();
        app.folderPickOpen = false;
        app.pickSweep = false;         // Cancel decides nothing, here least of all
        ImGui::CloseCurrentPopup();
    }
    if (load) {
        ImGui::CloseCurrentPopup();
        pickerAccept();                // same call the headless selftests make
    }
    ImGui::EndPopup();
}

// After a single file is opened: offer (or silently start) loading its siblings.
static void maybeOfferSequence(int imageIdx) {
    if (app.seqLoadMode == 2) return;                 // never
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    ImageDoc* im = app.images[imageIdx].get();
    if (im->seqId != 0 || im->path.empty()) return;
    std::string pattern;
    std::vector<std::string> files = findSequenceSiblings(im->path, pattern);
    if ((int)files.size() < 2) return;
    if (app.seqLoadMode == 1) {                       // always
        startSequenceLoad(imageIdx, files, pattern);
        toast("loading sequence: " + pattern + " (" + std::to_string(files.size()) + " files)");
        return;
    }
    app.seqAskImage = imageIdx;                       // ask
    app.seqAskFiles = files;
    app.seqAskPattern = pattern;
}

// ---------------------------------------------------------------- temporal analysis
// Built-in (L2): per-frame mean/std over the current ROI plus the temporal /
// fixed-pattern noise split — the same decomposition EMVA 1288 is built on.
// The cache slot is a parameter, not app.temporal: slot 0 is A, slot 1 is the
// compare B side. The body is otherwise unchanged - which is the point, A's
// numbers must not move because a B exists.
static void recomputeTemporalIfNeeded(const ImageDoc* im, App::TemporalState& T) {
    if (!im || im->seqId == 0) { T.valid = false; T.seqId = -1; return; }
    std::vector<int> f = framesOfSeq(im->seqId);
    if ((int)f.size() < 2) { T.valid = false; T.seqId = -1; return; }
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), chh = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && chh > 0) { rx = cx; ry = cy; rw = cw; rh = chh; roiUsed = true; }
        }
    }
    // keyed on the resolved rect; this pass costs samples x frames, so it must
    // not run for annotation churn or mid-drag
    if (T.seqId == im->seqId && T.frames == (int)f.size() &&
        T.rx == rx && T.ry == ry && T.rw == rw && T.rh == rh)
        return;
    if (app.annBusy && T.seqId == im->seqId) return;
    T.seqId = im->seqId; T.frames = (int)f.size();
    T.rx = rx; T.ry = ry; T.rw = rw; T.rh = rh; T.roiUsed = roiUsed;
    T.idx.clear(); T.frameMean.clear(); T.frameStd.clear();
    T.tempNoise = T.fixedPattern = T.totalNoise = 0;
    T.valid = false;
    // fixed sampling grid, capped so full-frame sequences stay interactive
    const size_t MAX_SAMPLES = 40000;
    size_t total = (size_t)rw * rh;
    size_t step = std::max<size_t>(1, total / MAX_SAMPLES);
    std::vector<size_t> offs;
    for (size_t p = 0; p < total; p += step) {
        int x = rx + (int)(p % rw), y = ry + (int)(p / rw);
        offs.push_back(((size_t)y * im->w + x) * im->ch);   // channel 0 (or CFA sample)
    }
    if (offs.empty()) return;
    std::vector<double> sum(offs.size(), 0), sum2(offs.size(), 0);
    int used = 0;
    for (int fi : f) {
        const ImageDoc& fr = *app.images[fi];
        if (fr.w != im->w || fr.h != im->h || fr.ch != im->ch) continue;   // dims changed
        double s = 0, s2 = 0;
        size_t n = 0;
        for (size_t k = 0; k < offs.size(); k++) {
            float v = fr.data[offs[k]];
            if (!std::isfinite(v)) continue;
            sum[k] += v; sum2[k] += (double)v * v;
            s += v; s2 += (double)v * v; n++;
        }
        if (!n) continue;
        double m = s / n, var = s2 / n - m * m;
        T.idx.push_back((float)fr.seqIndex);
        T.frameMean.push_back((float)m);
        T.frameStd.push_back((float)sqrt(var > 0 ? var : 0));
        used++;
    }
    if (used < 2) return;
    // per-sample temporal variance -> temporal noise; spatial spread of the
    // time-averaged samples -> fixed pattern (DSNU/PRNU-like)
    double tvarSum = 0, pmSum = 0, pmSum2 = 0;
    for (size_t k = 0; k < offs.size(); k++) {
        double m = sum[k] / used;
        double v = sum2[k] / used - m * m;
        tvarSum += v > 0 ? v : 0;
        pmSum += m; pmSum2 += m * m;
    }
    double tvar = tvarSum / offs.size();
    double pmean = pmSum / offs.size();
    double pvar = pmSum2 / offs.size() - pmean * pmean;
    T.tempNoise = sqrt(tvar);
    T.fixedPattern = sqrt(std::max(0.0, pvar));           // includes real scene detail
    T.totalNoise = sqrt(tvar + std::max(0.0, pvar));
    T.valid = true;
}

// ---------------------------------------------------------------- open dispatch
static void openRawDialogFor(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) { toast("cannot open: " + baseName(path), true); return; }
    rawDlg.open = true;
    rawDlg.path = path;
    rawDlg.fileSize = (size_t)f.tellg();
    rawDlg.replaceIdx = -1;           // fresh open (reinterpret sets this explicitly after)
    // guess dtype/interpretation/pattern from filename hints
    std::string n = baseName(path);
    {
        std::string nl = n;
        std::transform(nl.begin(), nl.end(), nl.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (int i = 0; i < 4; i++) {
            std::string pat = CFA_PATTERNS[i];
            std::transform(pat.begin(), pat.end(), pat.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (nl.find(pat) != std::string::npos) {
                rawDlg.interp = RI_BAYER; rawDlg.cfaPattern = i;
                if (rawDlg.dtype == RD_U8) rawDlg.dtype = RD_U16;   // bayer dumps are usually 16bit
            }
        }
        if (nl.find("bayer") != std::string::npos && rawDlg.interp != RI_BAYER && rawDlg.interp != RI_QUAD) {
            rawDlg.interp = RI_BAYER;
            if (rawDlg.dtype == RD_U8) rawDlg.dtype = RD_U16;
        }
        if (nl.find("quad") != std::string::npos &&
            (rawDlg.interp == RI_BAYER || nl.find("bayer") != std::string::npos))
            rawDlg.interp = RI_QUAD;
        if (nl.find("f32") != std::string::npos || nl.find("float") != std::string::npos)
            rawDlg.dtype = RD_F32;
        else if (nl.find("f64") != std::string::npos || nl.find("double") != std::string::npos)
            rawDlg.dtype = RD_F64;
    }
    for (size_t i = 0; i + 1 < n.size(); i++) {
        if ((n[i] == 'x' || n[i] == 'X') && isdigit((unsigned char)n[i + 1]) && i > 0 && isdigit((unsigned char)n[i - 1])) {
            size_t s = i; while (s > 0 && isdigit((unsigned char)n[s - 1])) s--;
            size_t e = i + 1; while (e < n.size() && isdigit((unsigned char)n[e])) e++;
            int W = atoi(n.substr(s, i - s).c_str());
            int H = atoi(n.substr(i + 1, e - i - 1).c_str());
            if (W >= 16 && H >= 16 && W <= 32768 && H <= 32768) { rawDlg.w = W; rawDlg.h = H; }
            break;
        }
    }
    rawGuessDims(rawDlg);
}

// ssh://user@host/path - the UI stays here, the pixels stay there. What arrives is
// the region being looked at, at the resolution it is being looked at.
// Numeric frame order: frame_1, frame_10, frame_100, frame_2 must not become the
// time axis. Shared by the browser and anything else that builds a stack.
// Frames need not be a strict counter pattern to stack in the order a human
// expects - rp::naturalLess (shared with the peer) is the contract.
static void sortFramesNumerically(std::vector<std::string>& files) {
    std::sort(files.begin(), files.end(), rp::naturalLess);
}

// Put the peer on the server without the user copying anything: check
// ~/.viewer/viewer-serve, and if it is missing, have the SERVER pull the
// prebuilt binary from the repository's binaries branch (it has git and network;
// this machine may not even be the same OS). Everything lives under ~/.viewer,
// so uninstalling is one rm -rf.
static std::string g_bootstrapLog;

// The Linux/macOS peer, found on THIS machine. A binaries-branch checkout has
// every platform side by side, and the source tree has the built one - so the
// client can hand the server its peer directly. That path works when the server
// has no internet and when the repository is private, which is the normal case
// for a lab compute box; server-side git is only the fallback.
static std::string findLocalPeer(const std::string& unameOut) {
    std::string plat = unameOut.find("Darwin") != std::string::npos ? "macos-arm64" : "linux-x64";
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::u8path(app.exePath);
    std::filesystem::path dir = exe.has_parent_path() ? exe.parent_path()
                                                      : std::filesystem::current_path(ec);
    std::vector<std::filesystem::path> cand;
    for (int up = 0; up < 4 && !dir.empty(); up++) {
        cand.push_back(dir / plat / "viewer-serve");              // binaries checkout
        cand.push_back(dir / "binaries" / plat / "viewer-serve");
        cand.push_back(dir / "viewer-serve");                     // same-dir build
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    for (const auto& c : cand)
        if (std::filesystem::exists(c, ec) && std::filesystem::is_regular_file(c, ec))
            return c.u8string();
    return {};
}

static bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

static std::string bootstrapScript() {
    // POSIX sh, delivered over stdin so no quoting layer can mangle it.
    // Prints VIEWER_SERVE_OK on success; anything else is shown to the user.
    return
        "set -e\n"
        "d=$HOME/.viewer\n"
        "mkdir -p \"$d\"\n"
        "if [ -x \"$d/viewer-serve\" ]; then echo VIEWER_SERVE_OK; exit 0; fi\n"
        "if ! command -v git >/dev/null 2>&1; then\n"
        "  echo 'no git on the server: copy viewer-serve to ~/.viewer/ manually'; exit 1\n"
        "fi\n"
        // A private repo would sit forever on a credential prompt, and an
        // offline box would sit on a TCP connect. Both must fail, loudly.
        "export GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/echo SSH_ASKPASS=/bin/echo\n"
        "export GIT_HTTP_LOW_SPEED_LIMIT=1000 GIT_HTTP_LOW_SPEED_TIME=20\n"
        "T=''; command -v timeout >/dev/null 2>&1 && T='timeout 45'\n"
        "if [ -d \"$d/bin/.git\" ]; then\n"
        "  $T git -C \"$d/bin\" fetch --depth 1 origin binaries 2>&1 || { echo \"git fetch failed\"; exit 1; }\n"
        "  git -C \"$d/bin\" reset --hard origin/binaries >/dev/null 2>&1\n"
        "else\n"
        "  $T git clone --depth 1 -b binaries " VIEWER_REPO_URL " \"$d/bin\" 2>&1 || { echo \"git clone failed (private repo or no network on this server?)\"; exit 1; }\n"
        "fi\n"
        "u=$(uname -s)\n"
        "case \"$u\" in\n"
        "  Linux)  src=\"$d/bin/linux-x64\" ;;\n"
        "  Darwin) src=\"$d/bin/macos-arm64\" ;;\n"
        "  *)      echo \"unsupported server OS: $u\"; exit 1 ;;\n"
        "esac\n"
        "[ -f \"$src/viewer-serve\" ] || { echo 'binaries branch has no viewer-serve'; exit 1; }\n"
        "cp \"$src/viewer-serve\" \"$d/viewer-serve\"\n"
        "chmod +x \"$d/viewer-serve\"\n"
        "rm -rf \"$d/plugins\"; [ -d \"$src/plugins\" ] && cp -r \"$src/plugins\" \"$d/plugins\"\n"
        "echo VIEWER_SERVE_OK\n";
}

// Update an already-installed peer (menu action): same script, forced refresh.
static std::string updateScript() {
    std::string s = bootstrapScript();
    size_t p = s.find("if [ -x \"$d/viewer-serve\" ]");
    if (p != std::string::npos) s.erase(p, s.find('\n', p) + 1 - p);   // skip the early-out
    return s;
}

// Install the peer, best method first. Returns a human-readable log either way.
static bool deployPeer(const std::string& host, int port, bool force, std::string& log) {
    std::string out, err;
    // 1. what machine is over there, and is a WORKING peer already in place?
    //    (--help must actually run: a peer that was copied but dies on a glibc
    //    mismatch reported only "connection lost" before this check existed)
    if (!remote::runSshScript(host, port,
            "uname -s; uname -m\n"
            "\"$HOME/.viewer/viewer-serve\" --version 2>&1 || echo NO_PEER\n",
            out, err, 20.0)) {
        log = "cannot reach " + host + ": " + err;
        return false;
    }
    // "it runs" is not "it is current": an old peer answers --help happily and
    // then cannot serve MEASURE. Compare the protocol version it reports.
    int peerProto = 0;
    size_t vp = out.find("viewer-serve protocol ");
    if (vp != std::string::npos) peerProto = atoi(out.c_str() + vp + 22);
    if (!force && peerProto >= (int)rp::VERSION) {
        log = "peer already installed (protocol v" + std::to_string(peerProto) + ")";
        return true;
    }
    if (!force && peerProto == 0 && out.find("NO_PEER") == std::string::npos &&
        out.find("not found") == std::string::npos)
        log = "the installed peer is older than this viewer; replacing it\n";
    bool isLinux = out.find("Linux") != std::string::npos;
    bool isDarwin = out.find("Darwin") != std::string::npos;
    if ((isLinux && out.find("x86_64") == std::string::npos) ||
        (isDarwin && out.find("arm64") == std::string::npos) ||
        (!isLinux && !isDarwin)) {
        log = "no prebuilt peer for this machine:\n" + out +
              "(binaries exist for Linux x86_64 and macOS arm64; build viewer-serve "
              "from source on the server and place it at ~/.viewer/viewer-serve)";
        return false;
    }

    // 2. hand it our own copy - no network and no credentials needed on the far
    //    side, which is what a lab compute box usually has
    std::string local = findLocalPeer(out);
    if (!local.empty()) {
        std::string bytes;
        if (readWholeFile(local, bytes)) {
            std::string o2, e2;
            bool ok = remote::runSshCommand(host, port,
                "sh -c 'mkdir -p ~/.viewer && cat > ~/.viewer/viewer-serve.new && "
                "chmod +x ~/.viewer/viewer-serve.new && "
                "mv ~/.viewer/viewer-serve.new ~/.viewer/viewer-serve && echo VIEWER_SERVE_OK'",
                bytes, o2, e2, 120.0);
            if (ok && o2.find("VIEWER_SERVE_OK") != std::string::npos) {
                // PROVE it runs before calling this a success: a binary built on
                // a newer distro dies on glibc with nothing but "connection
                // lost" further down. Surface the loader's actual words.
                std::string vout, verr3;
                remote::runSshScript(host, port,
                    "\"$HOME/.viewer/viewer-serve\" --version 2>&1 || true\n",
                    vout, verr3, 15.0);
                if (vout.find("viewer-serve protocol") == std::string::npos) {
                    log = "copied to " + host + ", but it does not run there:\n" + vout +
                          "\n(usually an OS/glibc mismatch - run update.cmd in viewer-bin "
                          "to get the compat build, or build viewer-serve on the server: "
                          "cmake --build <dir> --target viewer-serve)";
                    return false;
                }
                // plugins too, so server-side MEASURE has the same analyzers
                std::filesystem::path pdir = std::filesystem::u8path(local).parent_path() / "plugins";
                std::error_code ec;
                if (std::filesystem::exists(pdir, ec)) {
                    for (auto& e : std::filesystem::directory_iterator(pdir, ec)) {
                        std::string pb;
                        if (!e.is_regular_file(ec) || !readWholeFile(e.path().u8string(), pb)) continue;
                        std::string o3, e3;
                        remote::runSshCommand(host, port,
                            "sh -c 'mkdir -p ~/.viewer/plugins && cat > ~/.viewer/plugins/" +
                                e.path().filename().u8string() + "'",
                            pb, o3, e3, 60.0);
                    }
                }
                log = "sent the peer from this machine (" + local + ")";
                return true;
            }
            log = "could not copy the peer to " + host + ": " + (e2.empty() ? o2 : e2);
            // fall through and let the server try git
        }
    }

    // 3. last resort: the server fetches it itself (needs git + network + access)
    std::string o4, e4;
    if (!remote::runSshScript(host, port, force ? updateScript() : bootstrapScript(),
                              o4, e4, 90.0)) {
        log += (log.empty() ? "" : "\n") + std::string("server-side install: ") +
               (e4.empty() ? "failed" : e4);
        if (local.empty())
            log += "\nno local viewer-serve to send either - looked next to " + app.exePath;
        return false;
    }
    if (o4.find("VIEWER_SERVE_OK") == std::string::npos) {
        log += (log.empty() ? "" : "\n") + o4;
        return false;
    }
    log = "the server installed its own peer";
    return true;
}


static bool ensureRemoteSession(const std::string& host, std::string& err, int port) {
    if (!app.remoteSession) app.remoteSession.reset(new remote::Session());
    if (app.remoteSession->alive() && app.remoteSession->host() == host) return true;
    // the peer is the same binary: ~/.viewer/viewer-serve over ssh, or this very
    // executable when testing through local:// (--remote-exe overrides both,
    // which is how the standalone viewer-serve peer gets exercised)
    std::string exe = (host.empty() && app.remoteExe.empty()) ? app.exePath
                    : (app.remoteExe.empty() ? std::string(REMOTE_HOME) + "/viewer-serve"
                                             : app.remoteExe);
    if (app.remoteSession->startOn(host, port, exe, err)) {
        toast("connected to " + (host.empty() ? std::string("local peer") : host));
        return true;
    }
    if (host.empty()) return false;                 // nothing to bootstrap locally
    // Not there (or too old to answer): install it, then try once more. The user
    // never copies a binary or types a path.
    toast("installing the viewer peer on " + host + "...");
    std::string log;
    if (!deployPeer(host, port, false, log)) {
        g_bootstrapLog = log;
        err = "could not install the peer on " + host + ":\n" + log;
        return false;
    }
    if (!app.remoteSession->startOn(host, port, exe, err)) return false;
    toast("installed and connected to " + host);
    return true;
}

static std::string makeRemoteUrl(const std::string& host, const std::string& path) {
    if (host.empty()) return "local://" + path;
    // A home-relative path ("~/data/x.npy") pasted straight after the host would
    // read as part of the HOST ("ssh://trc2~/data/..."), and the mis-split host
    // then looks like a machine we have never connected to - which is what made
    // picking a file in the browser answer "could not install the peer".
    // git's ssh:// extension spells it /~/rel, and parseUrl unwraps that.
    std::string p = path;
    if (!p.empty() && p[0] != '/') p = "/" + p;
    return "ssh://" + host + p;
}

// Does analysis of this stack run on the server? Yes for remote data unless the
// user chose local-fetch. (Single-frame policy nuance lives in the Analysis
// panel; this is the stack-aggregate decision.)
static bool serverComputes(const App::SeqInfo& si) {
    if (si.remoteUrl.empty() && si.remoteFiles.empty()) return false;   // local data
    return app.procPolicy != App::PolLocalFetch;
}

static uint64_t g_measureToken = 1;

// Fire the server-side temporal aggregate for a remote stack, if policy allows.
// The result feeds the Temporal panel with a [server, N frames] tag, in seconds,
// without waiting for any frame transfer.
// `into` is the slot the answer lands in: app.srvTemporal for A (automatic),
// app.srvTemporalB for the compare side (button only - see the struct).
static void requestServerTemporal(int seqId, App::ServerTemporal& S) {
    App::SeqInfo* si = nullptr;
    for (auto& s : app.seqs) if (s.id == seqId) { si = &s; break; }
    if (!si || !serverComputes(*si)) return;
    S = App::ServerTemporal{};
    S.seqId = seqId;
    S.token = g_measureToken++;
    S.pending = true;
    App::MJob j;
    j.token = S.token;
    j.op = rp::MOP_TEMPORAL_STATS;
    j.cfaType = si->cfaType; j.cfaPattern = si->cfaPattern;
    if (!si->remoteFiles.empty()) {
        j.url = makeRemoteUrl(si->remoteHost, si->remoteFiles[0]);
        for (const auto& f : si->remoteFiles) j.files.push_back(f);
    } else {
        j.url = si->remoteUrl;
    }
    mEnqueue(std::move(j));
}
static void maybeRequestServerTemporal(int seqId) {
    requestServerTemporal(seqId, app.srvTemporal);
}

// Temporal stats for a stack that is NOT opened: fired from the browser's group
// row / multi-selection, so the answer to "is this set worth transferring?"
// costs zero pixels. Runs on the MEASURE worker - the browse worker serializes
// behind LIST/SCAN and shares the browsing ssh session, so a 300-frame
// aggregate there would freeze navigation. cfaType stays 0 on purpose:
// guessing a mosaic for a file nobody opened would silently split the planes
// wrong - the panel tag says plane=all instead.
static void requestBrowseTemporal(const std::string& host,
                                  std::vector<std::string> files,
                                  const std::string& label) {
    if (files.empty()) return;
    sortFramesNumerically(files);
    App::ServerTemporal& S = app.srvTemporal;
    S = App::ServerTemporal{};
    S.seqId = -2;                     // sentinel: no SeqInfo backs this result
    S.detached = true;
    S.label = label;
    S.host = host;
    S.files = files;
    S.token = g_measureToken++;
    S.pending = true;
    App::MJob j;
    j.token = S.token;
    j.op = rp::MOP_TEMPORAL_STATS;
    j.url = makeRemoteUrl(host, files[0]);
    if (files.size() > 1)
        for (const auto& f : files) j.files.push_back(f);
    mEnqueue(std::move(j));
    app.showTemporal = true;          // the result lands in the Temporal panel
    app.focusTemporal = true;
}

// ---- the connect/browse worker -------------------------------------------------
static void rbSetPhase(const std::string& p) {
    std::lock_guard<std::mutex> lk(app.rbMtx);
    app.rbPhase = p;
}

static void rbWorker() {
    while (!app.rbStop) {
        App::RbJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(app.rbMtx);
            if (!app.rbQueue.empty()) {
                job = std::move(app.rbQueue.front());
                app.rbQueue.erase(app.rbQueue.begin());
                have = true;
            }
        }
        if (!have) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        app.rbBusy = true;
        App::RbResult r;
        r.kind = job.kind;
        r.host = job.host;
        r.port = job.port;
        r.dir = job.dir;
        std::string exe = app.remoteExe.empty()
            ? (job.host.empty() ? app.exePath : std::string(REMOTE_HOME) + "/viewer-serve")
            : app.remoteExe;
        if (job.kind == App::RbUpdatePeer) {
            rbSetPhase("updating the peer on " + job.host + " (git)...");
            {
                std::lock_guard<std::mutex> lk(app.sesMtx);
                if (app.remoteSession) app.remoteSession->stop();
            }
            std::string log;
            r.ok = deployPeer(job.host, job.port, true, log);
            r.info = log;
            r.err = r.ok ? "" : log;
        } else {
            std::lock_guard<std::mutex> lk(app.sesMtx);
            if (!app.remoteSession) app.remoteSession.reset(new remote::Session());
            std::string err;
            bool alive = app.remoteSession->alive() && app.remoteSession->host() == job.host;
            if (!alive) {
                rbSetPhase("connecting to " + (job.host.empty() ? "local peer" : job.host) + "...");
                alive = app.remoteSession->startOn(job.host, job.port, exe, err);
                if (!alive && !job.host.empty()) {
                    // not there: the server installs its own peer from the
                    // binaries branch. This is the multi-second step that used
                    // to freeze the window.
                    rbSetPhase("installing the peer on " + job.host + "...");
                    std::string log;
                    bool got = deployPeer(job.host, job.port, false, log);
                    g_bootstrapLog = log;
                    if (got) {
                        rbSetPhase("connecting to " + job.host + "...");
                        alive = app.remoteSession->startOn(job.host, job.port, exe, err);
                    } else {
                        err = "could not install the peer on " + job.host + ":\n" + log;
                    }
                }
            }
            if (!alive) {
                r.err = err.empty() ? "connection failed" : err;
            } else if (job.kind == App::RbScan) {
                rbSetPhase("scanning " + job.dir + " for stacks...");
                bool trunc = false;
                int skipped = 0;
                if (app.remoteSession->scan(job.dir, 6, 256, r.scanGroups,
                                            trunc, skipped, err)) {
                    r.ok = true;
                    r.truncated = trunc;
                    r.skippedDirs = skipped;
                } else {
                    r.err = err;
                }
            } else if (job.kind == App::RbGlob) {
                rbSetPhase("searching " + job.pattern + " under " + job.dir + "...");
                r.gen = job.gen;
                bool trunc = false;
                int skipped = 0;
                if (app.remoteSession->glob(job.dir, job.pattern, 6, 2000, r.hits,
                                            trunc, skipped, err)) {
                    r.ok = true;
                    r.truncated = trunc;
                    r.skippedDirs = skipped;
                } else {
                    r.err = err;
                }
            } else {
                rbSetPhase("listing " + job.dir + "...");
                std::vector<remote::Entry> got;
                if (app.remoteSession->list(job.dir, got, err)) {
                    r.ok = true;
                    r.entries = std::move(got);
                } else {
                    r.err = err;
                }
            }
            if (app.remoteSession) r.peerVersion = app.remoteSession->peerVersion();
        }
        rbSetPhase("");
        {
            std::lock_guard<std::mutex> lk(app.rbMtx);
            app.rbDone.push_back(std::move(r));
        }
        app.rbBusy = false;
        glfwPostEmptyEvent();
    }
}

static void rbEnqueue(App::RbJob job) {
    {
        std::lock_guard<std::mutex> lk(app.rbMtx);
        app.rbQueue.push_back(std::move(job));
    }
    if (!app.rbThread.joinable()) app.rbThread = std::thread(rbWorker);
}

static void stopRbWorker() {
    app.rbStop = true;
    if (app.rbThread.joinable()) app.rbThread.join();
}

// UI thread: apply what the worker produced.
static void pumpRemoteBrowse() {
    std::vector<App::RbResult> batch;
    {
        std::lock_guard<std::mutex> lk(app.rbMtx);
        batch.swap(app.rbDone);
    }
    for (auto& r : batch) {
        App::RemoteBrowse& B = app.rbrowse;
        if (r.kind == App::RbUpdatePeer) {
            g_bootstrapLog = r.info;
            toast(r.ok ? "remote peer updated on " + r.host
                       : "remote update failed: " + r.err, !r.ok);
            // The update stopped the session; reconnect to where the user was,
            // so the listing comes back with the new peer's metadata without
            // another trip through Start Remote.
            if (r.ok && B.host == r.host) {
                App::RbJob j;
                j.kind = App::RbConnect;
                j.host = B.host; j.port = B.port; j.dir = B.dir;
                rbEnqueue(std::move(j));
            }
            continue;
        }
        if (r.kind == App::RbGlob) {
            App::RemoteSearch& S = app.rbSearch;
            if (r.gen != S.gen) continue;          // stopped or superseded: drop
            S.running = false;
            if (!r.ok) { toast("remote search: " + r.err, true); continue; }
            S.hits = std::move(r.hits);
            S.truncated = r.truncated;
            S.skippedDirs = r.skippedDirs;
            S.active = true;
            continue;
        }
        if (r.kind == App::RbScan) {
            if (!r.ok) { toast("remote scan: " + r.err, true); continue; }
            auto joinR = [](const std::string& a, const std::string& b) {
                if (b.empty()) return a;
                return a == "/" ? "/" + b : a + "/" + b;
            };
            std::vector<App::PendingGroup> groups;
            // batchId stays 0 here: batches are made at ACCEPT time in
            // pickerAccept (Cancel must not leave an empty batch behind)
            for (const auto& g : r.scanGroups) {
                App::PendingGroup pg;
                pg.name = g.dir.empty() ? g.entry.name : g.dir + "/" + g.entry.name;
                std::string base = joinR(r.dir, g.dir);
                for (const auto& m : g.entry.members) pg.files.push_back(joinR(base, m));
                if (g.entry.hasMeta && g.entry.ndim > 0) {   // v3 metadata -> the
                    for (int d = 0; d < g.entry.ndim; d++)   // picker's shape column
                        pg.shape += (d ? "x" : "") + std::to_string(g.entry.dims[d]);
                    if (!g.entry.dtype.empty()) pg.shape += " " + g.entry.dtype;
                }
                groups.push_back(std::move(pg));
            }
            if (r.truncated)
                toast("scan stopped at 256 stacks - open a narrower folder", true);
            if (r.skippedDirs)
                toast(std::to_string(r.skippedDirs) +
                      " unreadable folder(s) skipped in the scan", true);
            if (groups.empty()) { toast("no .npy stacks under " + r.dir, true); continue; }
            // A remote scan ALWAYS goes through the picker - one group included.
            // Every frame here is a transfer, the modal is where the filters
            // live, and the "1 group -> just open it" shortcut turned every
            // scan STARTED INSIDE a leaf folder into "it opened everything
            // without asking" (verbatim, reported three times).
            {
                // the picker "not appearing" has now been reported three times
                // with three different causes; the trail stays in
                fprintf(stderr, "remote scan: %d groups -> picker requested\n",
                        (int)groups.size());
                openPickerWith(std::move(groups), makeRemoteUrl(r.host, r.dir),
                               r.dir, true, r.host);
            }
            continue;
        }
        if (r.kind == App::RbTreeList) {
            // A node's children. It must NOT touch B.dir / B.entries / recents:
            // expanding a folder in the tree is not a navigation.
            app.rbTreePending.erase(std::remove(app.rbTreePending.begin(),
                                                app.rbTreePending.end(), r.dir),
                                    app.rbTreePending.end());
            if (r.ok) app.rbTreeCache[r.dir] = std::move(r.entries);
            else {
                // an unreadable folder collapses again, with a reason
                app.rbExpanded.erase(std::remove(app.rbExpanded.begin(),
                                                 app.rbExpanded.end(), r.dir),
                                     app.rbExpanded.end());
                toast(r.dir + ": " + r.err, true);
            }
            continue;
        }
        if (!r.ok) {
            B.err = r.err;
            if (r.kind == App::RbConnect) {
                B.connected = false;
                app.showRemote = true;    // so the failure text has somewhere to live
                app.focusRemote = true;
                toast("remote: " + r.err, true);
            }
            continue;
        }
        B.err.clear();
        B.host = r.host;
        B.port = r.port;
        B.dir = r.dir;
        B.entries = std::move(r.entries);
        {   // every directory actually listed becomes the newest "recent place"
            std::string u = placeUrl(r.host, r.port, r.dir);
            auto& v = app.rbRecents;
            v.erase(std::remove(v.begin(), v.end(), u), v.end());
            v.insert(v.begin(), u);
            if (v.size() > 10) v.resize(10);
            app.prefsDirty = true;
        }
        if (r.kind == App::RbConnect && !B.connected) {
            B.connected = true;
            rbTreeForget();               // another machine: other children entirely
            // a listing nobody can see is not a connection anyone believes in
            app.showRemote = true;
            app.focusRemote = true;
            toast("connected to " + (r.host.empty() ? std::string("local peer") : r.host));
        }
        // An outdated peer ANSWERS perfectly well, so the install-on-failure
        // path never runs for it - and every listing would show "-" for shape
        // and mtime until someone finds the update menu item. Update it now,
        // unasked, once per connect: this is the vscode-server model the whole
        // remote design follows. The reconnect rides on the RbUpdatePeer
        // result above; autoUpdateTried stops a loop if the update binary is
        // still old (e.g. the binaries branch has not rebuilt yet).
        if (r.kind == App::RbConnect && !r.host.empty() && r.peerVersion > 0 &&
            r.peerVersion < (int)rp::VERSION && !B.autoUpdateTried) {
            B.autoUpdateTried = true;
            toast("peer on " + r.host + " is protocol " + std::to_string(r.peerVersion) +
                  " - updating it now...");
            App::RbJob j;
            j.kind = App::RbUpdatePeer;
            j.host = r.host; j.port = r.port;
            rbEnqueue(std::move(j));
        }
        if (!app.pendingRemoteOpen.empty()) {   // a url was pasted with the host
            std::string u = app.pendingRemoteOpen;
            app.pendingRemoteOpen.clear();
            openRemote(u);
        }
    }
}

// ---- tree mode: expand / collapse one node ------------------------------------
// Expanding is LAZY and asynchronous: the LIST goes to the browse worker, and
// the node shows "(listing...)" until the answer lands in the cache. Collapsing
// keeps the cache, so opening the same node again costs nothing at all - which
// is the whole reason a tree is usable over ssh.
static bool rbHas(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
static void rbTreeExpand(const std::string& dir) {
    if (!rbHas(app.rbExpanded, dir)) app.rbExpanded.push_back(dir);
    if (app.rbTreeCache.count(dir) || rbHas(app.rbTreePending, dir)) return;
    app.rbTreePending.push_back(dir);
    app.rbTreeLists++;
    App::RbJob j;
    j.kind = App::RbTreeList;
    j.host = app.rbrowse.host;
    j.port = app.rbrowse.port;
    j.dir = dir;
    rbEnqueue(std::move(j));
}
static void rbTreeCollapse(const std::string& dir) {
    app.rbExpanded.erase(std::remove(app.rbExpanded.begin(), app.rbExpanded.end(), dir),
                         app.rbExpanded.end());
}
// A different machine, or "list this again": the cached children are stale.
static void rbTreeForget() {
    app.rbTreeCache.clear();
    app.rbExpanded.clear();
    app.rbTreePending.clear();
}

// Browse one directory on the connected server (async: a dead link hangs the
// worker, never the window).
static void remoteBrowseTo(const std::string& dir) {
    App::RbJob j;
    j.kind = App::RbList;
    j.host = app.rbrowse.host;
    j.port = app.rbrowse.port;
    j.dir = dir;
    rbEnqueue(std::move(j));
}

// One canonical url for "this directory on this host", used by the places list
// (bookmarks / recents). Unlike makeRemoteUrl it keeps a non-default port, and
// "~" travels as git's /~/ extension so parseUrl round-trips it.
static std::string placeUrl(const std::string& host, int port, const std::string& path) {
    if (host.empty()) return "local://" + path;
    std::string p = path;
    if (!p.empty() && p[0] != '/') p = "/" + p;
    return "ssh://" + host + (port > 0 ? ":" + std::to_string(port) : "") + p;
}

// Navigate to a places url. Same host and a live session: just browse there.
// Anything else goes through the normal async connect - never a handshake on
// the UI thread.
static void goToPlace(const std::string& url) {
    std::string host, path;
    int port = 0;
    if (!remote::parseUrl(url, host, path, &port)) {
        toast("cannot parse place: " + url, true);
        return;
    }
    if (app.rbrowse.connected && app.rbrowse.host == host && app.rbrowse.port == port) {
        remoteBrowseTo(path);
        return;
    }
    app.rbrowse = App::RemoteBrowse{};
    app.rbrowse.host = host;
    app.rbrowse.port = port;
    App::RbJob j;
    j.kind = App::RbConnect;
    j.host = host;
    j.port = port;
    j.dir = path;
    rbEnqueue(std::move(j));
}

// Kick a server-side recursive find; the result lands in app.rbSearch through
// the browse worker. Any previous search is superseded by the gen bump.
static void remoteStartSearch(const std::string& root, const std::string& pattern) {
    App::RemoteSearch& S = app.rbSearch;
    S.gen++;
    S.running = true;
    S.active = true;
    S.root = root;
    S.pattern = pattern;
    S.hits.clear();
    S.truncated = false;
    S.skippedDirs = 0;
    App::RbJob j;
    j.kind = App::RbGlob;
    j.host = app.rbrowse.host;
    j.port = app.rbrowse.port;
    j.dir = root;
    j.pattern = pattern;
    j.gen = S.gen;
    rbEnqueue(std::move(j));
}

// The remote openFolder(): the worker asks the server to walk the subtree and
// report every stack below it; the result feeds the picker / open queue on
// the UI thread (pumpRemoteBrowse).
static void remoteScanFolder(const std::string& root) {
    App::RbJob j;
    j.kind = App::RbScan;
    j.host = app.rbrowse.host;
    j.port = app.rbrowse.port;
    j.dir = root;
    rbEnqueue(std::move(j));
}

// Chained opens from a folder scan: the next stack starts only when the remote
// fetcher is idle, so the memory budget openRemoteStack applies reflects what
// the previous stack actually loaded.
static void pumpRemoteOpenQueue() {
    if (app.rbOpenQueue.empty() || app.rfPending > 0 || app.seqRunning) return;
    App::RemoteOpen ro = std::move(app.rbOpenQueue.front());
    app.rbOpenQueue.erase(app.rbOpenQueue.begin());
    sortFramesNumerically(ro.files);
    app.loadBatchId = ro.batchId;
    openRemoteStack(ro.host, ro.files, ro.name);
    app.loadBatchId = 0;
}

// File > Start Remote: connect (installing the peer if needed), then browse.
// Everything slow happens on the worker; this returns immediately.
static void startRemote(const std::string& hostSpec) {
    std::string host = hostSpec, dir = "~";
    int port = 0;
    // accept a full url here too, so a pasted path still works
    if (hostSpec.find("://") != std::string::npos || hostSpec.find(':') != std::string::npos) {
        std::string h, p;
        if (remote::parseUrl(hostSpec, h, p, &port)) { host = h; dir = p; }
    }
    while (host.size() > 1 && host.back() == '/') host.pop_back();
    if (dir.size() > 4 && isNpyName(dir)) {          // a pasted file path: open it
        size_t s = dir.find_last_of('/');
        std::string parent = s == std::string::npos ? "~" : dir.substr(0, s);
        app.rbrowse = App::RemoteBrowse{};
        app.rbrowse.host = host;
        app.rbrowse.port = port;
        App::RbJob j;
        j.kind = App::RbConnect; j.host = host; j.port = port; j.dir = parent;
        rbEnqueue(std::move(j));
        // opened once the worker has the session; opening it here would put the
        // ssh handshake back on the UI thread
        app.pendingRemoteOpen = makeRemoteUrl(host, dir);
        return;
    }
    app.rbrowse = App::RemoteBrowse{};
    app.rbrowse.host = host;
    app.rbrowse.port = port;
    App::RbJob j;
    j.kind = App::RbConnect;
    j.host = host;
    j.port = port;
    j.dir = dir;
    rbEnqueue(std::move(j));
}

// Close the transient preview, if one is still unpromoted.
static void dropPreview() {
    if (!app.previewUid) return;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->uid == app.previewUid) {
            if (!app.images[i]->preview) break;      // promoted: not ours anymore
            forgetImage(app.images[i].get());
            if (app.images[i]->tex) glDeleteTextures(1, &app.images[i]->tex);
            app.images.erase(app.images.begin() + i);
            if (app.current >= (int)app.images.size())
                app.current = (int)app.images.size() - 1;
            app.imagesRev++;
            break;
        }
    app.previewUid = 0;
    // the sequence it belonged to goes with it (stepPreviewFrame restores its
    // own copy across the openRemote inside a step)
    app.previewFiles.clear();
    app.previewIndex = 0;
    app.previewLabel.clear();
}

// Walk the previewed sequence without opening it. Each step replaces the one
// preview slot, so browsing a 300-frame capture costs one frame of transfer at
// a time - the whole point of previewing rather than opening.
static void stepPreviewFrame(int delta) {
    if (app.previewFiles.size() < 2) return;
    int n = (int)app.previewFiles.size();
    int want = std::clamp(app.previewIndex + delta, 0, n - 1);
    if (want == app.previewIndex) return;
    std::string host = app.rbrowse.host;
    std::vector<std::string> files = app.previewFiles;   // openRemote drops the preview
    std::string label = app.previewLabel;
    app.previewIndex = want;
    openRemote(makeRemoteUrl(host, files[want]), true);
    app.previewFiles = std::move(files);
    app.previewIndex = want;
    app.previewLabel = std::move(label);
}

static void openRemote(const std::string& url, bool asPreview) {
    std::string host, rpath;
    if (!remote::parseUrl(url, host, rpath)) {
        toast("expected ssh://user@host/path/to/file.npy", true);
        return;
    }
    if (asPreview) dropPreview();          // ONE slot: the next look replaces it
    std::string err;
    if (!ensureRemoteSession(host, err)) { toast("remote: " + err, true); return; }
    remote::Meta m;
    if (!app.remoteSession->meta(rpath, m, err)) { toast("remote: " + err, true); return; }

    // First view: the whole frame decimated to something a screen can show. A
    // zoom or a pan asks for a better tile; nothing else on screen costs a byte.
    int step = std::max(1, (int)ceilf(std::max(m.w, m.h) / 1600.0f));
    std::vector<float> px;
    int tw = 0, th = 0, tch = 0;
    std::string dt;
    if (!app.remoteSession->tile(rpath, 0, 0, 0, m.w, m.h, step, px, tw, th, tch, dt, err)) {
        toast("remote: " + err, true);
        return;
    }
    if (tw < 1 || th < 1 || tch < 1 || tch > 4) {   // a 0ch doc would crash later
        toast("remote: peer returned a degenerate frame", true);
        return;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = baseName(rpath) + (step > 1 ? "  (1/" + std::to_string(step) + ")" : "");
    doc->path = url;
    doc->dtype = dt;
    doc->w = tw; doc->h = th; doc->ch = tch;
    doc->data = std::move(px);
    doc->note = "remote " + host + "  -  " + std::to_string(m.w) + "x" + std::to_string(m.h) +
                (m.frames > 1 ? "  " + std::to_string(m.frames) + " frames" : "");
    doc->uid = app.nextUid++;
    doc->remoteUrl = url;
    doc->remoteFrame = 0;
    doc->remoteStep = step;
    doc->remoteFrames = (int)m.frames;
    doc->preview = asPreview;
    // remote opens never went through addImage, so the batch is assigned here:
    // previews sit in their own "preview" pseudo-batch until promoted
    if (asPreview) doc->batchId = batchReuse("preview");
    else if (app.loadBatchId) doc->batchId = app.loadBatchId;
    else {
        size_t sl = rpath.find_last_of('/');
        std::string dir = sl == std::string::npos ? std::string() : rpath.substr(0, sl);
        size_t s2 = dir.find_last_of('/');
        std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
        doc->batchId = batchReuse(leaf.empty() ? host : leaf);
    }
    computeMinMax(*doc);
    defaultRange(*doc);
    doc->texDirty = true;
    app.images.push_back(std::move(doc));
    app.imagesRev++;
    selectImage((int)app.images.size() - 1);
    app.fitRequested = true;
    if (asPreview) app.previewUid = app.images.back()->uid;
    // The full-resolution follow-up. Registered opens always get it. A preview
    // gets it too when the frame is cheap enough to just have (small file or
    // fast link - "problem-free" full fetch, verbatim), but only at LOW
    // priority: an unpromoted preview never delays anything registered.
    if (step > 1) {
        size_t estBytes = (size_t)m.w * m.h * tch * rp::dtypeSize(rp::dtypeFromName(dt.c_str()));
        if (!asPreview) requestFullRemote(app.images.back().get());
        else if (estBytes <= (size_t)48 << 20)
            requestFullRemote(app.images.back().get(), true);
    }
    // A frame axis makes this a stack: prefetch the rest in the background, and
    // they become ordinary local frames as they land - temporal analysis, frame
    // stepping, every analyzer, all unchanged. Processing stays local by design;
    // the remote side only ever ships pixels. A PREVIEW is only ever its first
    // frame: the rest of the stack is exactly the buffering promotion defers.
    if (m.frames > 1 && !asPreview) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = baseName(rpath) + " [remote]";
        si.remoteUrl = url;           // frame-axis: one file, N frames
        si.remoteHost = host;
        si.expectedFrames = m.frames;
        app.seqs.push_back(si);
        ImageDoc* first = app.images.back().get();
        first->seqId = si.id;
        first->seqIndex = 0;
        // server-side temporal stats fire NOW, regardless of transfer, when the
        // policy allows the server to compute (auto/server for remote data)
        maybeRequestServerTemporal(si.id);
        size_t perFrame = (size_t)m.w * m.h * m.ch * sizeof(float);
        size_t room = seqMemBudget() - std::min(seqMemBudget(), residentImageBytes());
        int fit = (int)std::min<size_t>((size_t)m.frames - 1, perFrame ? room / perFrame : 0);
        for (int i = 1; i <= fit; i++) {
            App::RFetchJob j;
            j.url = url;
            j.name = baseName(rpath) + " #" + std::to_string(i);
            j.frame = i;
            j.seqId = si.id;
            j.seqIndex = i;
            rfEnqueue(std::move(j));
        }
        if (fit < m.frames - 1) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "memory budget: fetching %d of %d frames\n"
                     "(File > Sequence loading > Memory budget)", fit + 1, m.frames);
            app.seqNote = msg;
        }
    }
    toast("opened " + baseName(rpath) + " from " + (host.empty() ? "local peer" : host));
}

// A remote folder of numbered .npy files, opened as one stack: the first file
// shows immediately, the rest arrive in the background and slot in as ordinary
// local frames. Processing never moves - the pixels do, once each.
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name) {
    if (files.empty()) return;
    size_t before = app.images.size();
    openRemote(makeRemoteUrl(host, files[0]));
    if (app.images.size() == before) return;      // first frame failed; toasted already
    ImageDoc* first = app.images.back().get();
    if (first->seqId != 0) return;                // it was a frame-axis file: done
    App::SeqInfo si;
    si.id = app.nextSeqId++;
    // A scan names the stack by its folder ("10lx/frame_#.npy"): seven stacks
    // all called frame_000.npy would be indistinguishable in every panel, and
    // the linearity Auto-levels reads the level from exactly this name.
    si.name = !name.empty() ? name + " [remote x" + std::to_string(files.size()) + "]"
              : baseName(files[0]) + " [remote x" + std::to_string(files.size()) + "]";
    si.remoteHost = host;             // folder stack: one file per frame
    si.expectedFrames = (int)files.size();
    for (const auto& f : files) si.remoteFiles.push_back(f);
    app.seqs.push_back(si);
    first->seqId = si.id;
    first->seqIndex = 0;
    maybeRequestServerTemporal(si.id);
    size_t perFrame = (size_t)first->w * first->h * first->ch * sizeof(float);
    if (first->remoteStep > 1)                    // preview dims: scale the estimate
        perFrame *= (size_t)first->remoteStep * first->remoteStep;
    size_t room = seqMemBudget() - std::min(seqMemBudget(), residentImageBytes());
    int fit = (int)std::min<size_t>(files.size() - 1, perFrame ? room / perFrame : 0);
    for (int i = 1; i <= fit; i++) {
        App::RFetchJob j;
        j.url = makeRemoteUrl(host, files[i]);
        j.name = baseName(files[i]);
        j.seqId = si.id;
        j.seqIndex = i;
        rfEnqueue(std::move(j));
    }
    if (fit < (int)files.size() - 1) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "memory budget: fetching %d of %d frames\n"
                 "(File > Sequence loading > Memory budget)", fit + 1, (int)files.size());
        app.seqNote = msg;
    }
}

// A preview becomes a registered open the moment it is USED: double-click,
// Enter, being measured, being made compare-B, or a session save. The user
// never answers "did you mean to keep this?" - their actions already did.
static void promotePreview(ImageDoc* d) {
    if (!d || !d->preview) return;
    d->preview = false;
    if (app.previewUid == d->uid) app.previewUid = 0;
    std::string host, rpath;
    remote::parseUrl(d->remoteUrl, host, rpath);
    {   // out of the "preview" pseudo-batch, into a folder-named one
        size_t sl = rpath.find_last_of('/');
        std::string dir = sl == std::string::npos ? std::string() : rpath.substr(0, sl);
        size_t s2 = dir.find_last_of('/');
        std::string leaf = s2 == std::string::npos ? dir : dir.substr(s2 + 1);
        d->batchId = batchReuse(leaf.empty() ? (host.empty() ? "opened" : host) : leaf);
    }
    // the buffering the preview deferred, at REGISTERED priority now
    requestFullRemote(d);
    if (d->remoteFrames > 1 && d->seqId == 0) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = baseName(rpath) + " [remote]";
        si.remoteUrl = d->remoteUrl;
        si.remoteHost = host;
        si.expectedFrames = d->remoteFrames;
        app.seqs.push_back(si);
        d->seqId = si.id;
        d->seqIndex = 0;
        maybeRequestServerTemporal(si.id);
        size_t perFrame = (size_t)d->w * d->h * d->ch * sizeof(float) *
                          (d->remoteStep > 1 ? (size_t)d->remoteStep * d->remoteStep : 1);
        size_t room = seqMemBudget() - std::min(seqMemBudget(), residentImageBytes());
        int fit = (int)std::min<size_t>((size_t)d->remoteFrames - 1,
                                        perFrame ? room / perFrame : 0);
        for (int i = 1; i <= fit; i++) {
            App::RFetchJob j;
            j.url = d->remoteUrl;
            j.name = baseName(rpath) + " #" + std::to_string(i);
            j.seqId = si.id;
            j.seqIndex = i;
            j.frame = i;
            rfEnqueue(std::move(j));
        }
    }
    app.imagesRev++;
    toast(d->name + ": opened");
}

static void openPath(const std::string& path) {
    if (path.compare(0, 6, "ssh://") == 0 || path.compare(0, 8, "local://") == 0) {
        // A url naming a file opens that file. A url naming a host or a
        // directory means "start here": connect and browse it, the same thing
        // File > Start Remote does - which is what a desktop shortcut to a
        // machine passes, and what a half-remembered path pasted on the command
        // line usually is. startRemote keeps the ssh handshake off this thread.
        if (isNpyName(path)) openRemote(path);
        else                 startRemote(path);
        return;
    }
    std::string low = path;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto ends = [&](const char* suf) {
        size_t n = strlen(suf);
        return low.size() >= n && low.compare(low.size() - n, n, suf) == 0;
    };
    if (ends(".npy")) {
        std::string err = loadNpy(path);
        if (!err.empty()) toast(baseName(path) + ": " + err, true);
        else { toast("loaded " + baseName(path)); maybeOfferSequence(app.current); }
    } else if (ends(".npz")) {
        std::string err = loadNpz(path);
        if (!err.empty()) toast(baseName(path) + ": " + err, true);
        else toast("loaded " + baseName(path));
    } else if (ends(".vsession")) {
        std::string err = loadSession(path);
        if (!err.empty()) toast(baseName(path) + ": " + err, true);
        else toast("session restored: " + baseName(path));
    } else if (std::filesystem::is_directory(pathFromUtf8(path))) {
        openFolder(path);                     // dropping a folder loads every stack below it
    } else {
        openRawDialogFor(path);
    }
}

static void openFileDialog() {
    if (!pfd::settings::available()) {   // e.g. Linux without zenity/kdialog
        toast("no file-dialog backend found (install zenity or kdialog) - drag & drop files instead", true);
        return;
    }
    if (app.openDlg) return;             // one dialog at a time
    app.openDlg = std::make_unique<pfd::open_file>("Open image / session", "",
        std::vector<std::string>{ "Images (*.npy *.npz *.bin *.raw *.yuv *.dat)", "*.npy *.npz *.bin *.raw *.yuv *.dat",
          "Session (*.vsession)", "*.vsession",
          "All files", "*" },
        pfd::opt::multiselect);
}
static void openFolderDialog() {
    if (!pfd::settings::available()) {
        toast("no file-dialog backend found (install zenity or kdialog)", true);
        return;
    }
    if (app.folderDlg) return;
    app.folderDlgMode = 0;
    app.folderDlg = std::make_unique<pfd::select_folder>("Open folder (all sequences below it)");
}
// The dialog's mode-1 action, shared with --localbrowse-selftest: the picked
// folder opens in the Browse panel through the LOCAL peer - the same listing,
// grouping, filters and server stats a remote machine gets, on this disk.
static void browseLocalFolder(std::string p) {
    if (p.empty()) return;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    startRemote("local://" + p);
    app.showRemote = true;
    app.focusRemote = true;
}
static void browseFolderDialog() {
    if (!pfd::settings::available()) {
        toast("no file-dialog backend found (install zenity or kdialog)", true);
        return;
    }
    if (app.folderDlg) return;
    app.folderDlgMode = 1;
    app.folderDlg = std::make_unique<pfd::select_folder>("Browse folder (local)");
}
static void saveSessionDialog() {
    if (app.images.empty()) { toast("nothing to save - no images loaded", true); return; }
    if (!pfd::settings::available()) { toast("no file-dialog backend found (install zenity or kdialog)", true); return; }
    if (app.saveDlg) return;
    app.saveDlg = std::make_unique<pfd::save_file>("Save session", "session.vsession",
        std::vector<std::string>{ "viewer session (*.vsession)", "*.vsession" });
}
static void openFolder(const std::string& path);   // defined with the sequence code

static void pollFileDialog() {           // called once per frame from the main loop
    if (app.openDlg && app.openDlg->ready(0)) {
        for (const std::string& p : app.openDlg->result()) openPath(p);
        app.openDlg.reset();
    }
    if (app.saveDlg && app.saveDlg->ready(0)) {
        std::string p = app.saveDlg->result();
        if (!p.empty()) saveSession(p);
        app.saveDlg.reset();
    }
    if (app.csvDlg && app.csvDlg->ready(0)) {
        std::string p = app.csvDlg->result();
        if (!p.empty()) {
            if (p.find('.') == std::string::npos) p += ".csv";
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            if (f) { f << app.pendingCsv; toast("curves exported: " + baseName(p)); }
            else toast("cannot write: " + baseName(p), true);
        }
        app.csvDlg.reset();
        app.pendingCsv.clear();
    }
    if (app.folderDlg && app.folderDlg->ready(0)) {
        std::string p = app.folderDlg->result();
        app.folderDlg.reset();
        if (!p.empty()) {
            if (app.folderDlgMode == 1) browseLocalFolder(p);   // Browse panel
            else openFolder(p);                                 // load stacks
        }
    }
}

// (dynamic crop helpers are defined above the session code)

// ---------------------------------------------------------------- view helpers
static void fitToCanvas(ImVec2 canvasSize) {
    ImageDoc* im = cur();
    if (!im || canvasSize.x < 10 || canvasSize.y < 10) return;
    app.view.zoom = std::min(canvasSize.x / im->w, canvasSize.y / im->h) * 0.97f;
    app.view.center = ImVec2(im->w * 0.5f, im->h * 0.5f);
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
static void drawRawModal() {
    if (rawDlg.open) { ImGui::OpenPopup("RAW load settings"); rawDlg.open = false; }
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("RAW load settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("%s  (%zu bytes)%s", baseName(rawDlg.path).c_str(), rawDlg.fileSize,
                rawDlg.replaceIdx >= 0 ? "  -  reinterpret" : "");
    if (rawDlg.forQueue)
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1),
                           "these settings apply to all %d queued stack(s)", rawDlg.queueCount);
    ImGui::Separator();
    int prevDt = rawDlg.dtype, prevIn = rawDlg.interp, prevOff = rawDlg.offset;
    // axis 1: how one sample is stored
    if (ImGui::BeginCombo("pixel format", RAW_DTYPE_NAMES[rawDlg.dtype])) {
        for (int i = 0; i < RD_COUNT; i++)
            if (ImGui::Selectable(RAW_DTYPE_NAMES[i], i == rawDlg.dtype)) rawDlg.dtype = i;
        ImGui::EndCombo();
    }
    // axis 2: what the samples mean
    if (ImGui::BeginCombo("interpretation", RAW_INTERP_NAMES[rawDlg.interp])) {
        for (int i = 0; i < RI_COUNT; i++)
            if (ImGui::Selectable(RAW_INTERP_NAMES[i], i == rawDlg.interp)) rawDlg.interp = i;
        ImGui::EndCombo();
    }
    if (rawDlg.interp == RI_BAYER || rawDlg.interp == RI_QUAD) {
        if (ImGui::BeginCombo("CFA pattern", CFA_PATTERNS[rawDlg.cfaPattern])) {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(CFA_PATTERNS[i], i == rawDlg.cfaPattern)) rawDlg.cfaPattern = i;
            ImGui::EndCombo();
        }
    }
    if (!rawDlg.guesses.empty()) {
        char cursz[64]; snprintf(cursz, 64, "%d x %d", rawDlg.w, rawDlg.h);
        if (ImGui::BeginCombo("size candidates", cursz)) {
            for (auto& g : rawDlg.guesses) {
                char lb[64]; snprintf(lb, 64, "%d x %d", g.first, g.second);
                if (ImGui::Selectable(lb)) { rawDlg.w = g.first; rawDlg.h = g.second; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputInt("width (px)", &rawDlg.w, 1, 16);          // step / fast-step: round numbers
    ImGui::InputInt("height (px)", &rawDlg.h, 1, 16);
    ImGui::InputInt("offset (bytes)", &rawDlg.offset, 1, 512);
    rawDlg.w = std::clamp(rawDlg.w, 1, 32768);
    rawDlg.h = std::clamp(rawDlg.h, 1, 32768);
    rawDlg.offset = std::max(0, rawDlg.offset);
    ImGui::Checkbox("little endian", &rawDlg.littleEndian);
    ImGui::Checkbox("crop on load", &rawDlg.cropOn);
    if (rawDlg.cropOn) {
        if (rawDlg.cropW <= 0) { rawDlg.cropW = rawDlg.w; rawDlg.cropH = rawDlg.h; }
        ImGui::InputInt("crop x (px)", &rawDlg.cropX, 1, 16);
        ImGui::InputInt("crop y (px)", &rawDlg.cropY, 1, 16);
        ImGui::InputInt("crop width (px)", &rawDlg.cropW, 1, 16);
        ImGui::InputInt("crop height (px)", &rawDlg.cropH, 1, 16);
        rawDlg.cropX = std::clamp(rawDlg.cropX, 0, rawDlg.w - 1);
        rawDlg.cropY = std::clamp(rawDlg.cropY, 0, rawDlg.h - 1);
        rawDlg.cropW = std::clamp(rawDlg.cropW, 1, rawDlg.w - rawDlg.cropX);
        rawDlg.cropH = std::clamp(rawDlg.cropH, 1, rawDlg.h - rawDlg.cropY);
    }
    if (rawDlg.dtype != prevDt || rawDlg.interp != prevIn || rawDlg.offset != prevOff)
        rawGuessDims(rawDlg);

    size_t need = (size_t)rawDlg.w * rawDlg.h *
                  RAW_DTYPE_SIZE[rawDlg.dtype] * RAW_INTERP_CH[rawDlg.interp] + rawDlg.offset;
    if (need > rawDlg.fileSize)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "need %zu bytes > file %zu bytes", need, rawDlg.fileSize);
    else if (need < rawDlg.fileSize)
        ImGui::TextDisabled("need %zu bytes (%zu bytes unused)", need, rawDlg.fileSize - need);
    else
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "size matches exactly");

    ImGui::Separator();
    bool ok = ImGui::Button(rawDlg.replaceIdx >= 0 ? "Reload" : "Load", ImVec2(120 * app.uiScale, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        rawDlg.replaceIdx = -1;
        if (rawDlg.forQueue) { rawDlg.forQueue = false; app.seqQueue.clear(); }
        ImGui::CloseCurrentPopup();
    }
    if (ok) {
        std::string err = loadRaw(rawDlg);
        if (!err.empty()) toast(err, true);
        else if (rawDlg.forQueue) {
            // first frame of the first queued stack: reuse these settings for the rest
            g_folderRecipe = rawDlg;
            g_folderRecipe.forQueue = false;
            g_folderRecipe.replaceIdx = -1;
            app.folderRecipeValid = true;
            rawDlg.forQueue = false;
            App::PendingGroup g = app.seqQueue.front();
            app.seqQueue.erase(app.seqQueue.begin());
            if (g.files.size() >= 2) startSequenceLoad(app.current, g.files, g.name);
            ImGui::CloseCurrentPopup();
        } else {
            bool fresh = rawDlg.replaceIdx < 0;
            toast((fresh ? "loaded " : "reinterpreted ") + baseName(rawDlg.path));
            rawDlg.replaceIdx = -1;
            ImGui::CloseCurrentPopup();
            if (fresh) maybeOfferSequence(app.current);
        }
    }
    ImGui::EndPopup();
}

static void drawCanvas(ImVec2 avail) {
    // DPI/font-aware ruler geometry (fixed px constants break on 150-200% Windows scaling)
    const float s = app.uiScale;
    const float RULER_H = ImGui::GetFontSize() + 5.0f * s;
    const float RULER_W = ImGui::CalcTextSize("00000").x + 8.0f * s;
    const float TICK = 7.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    ImageDoc* im = cur();
    // The footer strip (name + scrub bar) is RESERVED below the canvas, never
    // drawn over the pixels: overlapping the image was vetoed outright, and a
    // bar over the bottom rows would sit exactly where shading is judged.
    const float FOOT_H = im ? ImGui::GetFontSize() + 10.0f * s : 0.0f;
    ImVec2 canvasP0 = ImVec2(origin.x + RULER_W, origin.y + RULER_H);
    ImVec2 canvasSize = ImVec2(std::max(avail.x - RULER_W, 50.0f),
                               std::max(avail.y - RULER_H - FOOT_H, 50.0f));
    ImVec2 canvasP1 = ImVec2(canvasP0.x + canvasSize.x, canvasP0.y + canvasSize.y);

    // interaction region = canvas (excluding rulers)
    ImGui::SetCursorScreenPos(canvasP0);
    ImGui::InvisibleButton("canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    // ---- A/B compare geometry -------------------------------------------------
    // Split: two panes side by side, each showing the same image region, so the
    // eye compares by saccade. Wipe: one pane, B clipped to the right of a
    // draggable divider, so the eye compares by edge. Both share app.view.
    ImageDoc* imB = cmpB();
    // A shared range moves as you step (mode 2 refits to the frame pair), and a
    // texture built with the old one is a wrong picture, not a stale label - so
    // both sides are checked against what they would be built with NOW.
    for (ImageDoc* d : { im, imB })
        if (d && (d->texBlack != effBlack(*d) || d->texWhite != effWhite(*d)))
            d->texDirty = true;
    const bool split = imB && app.compareMode == App::CmpSplit;
    const bool wipe  = imB && app.compareMode == App::CmpWipe;
    const bool diffMode = imB && im && app.compareMode == App::CmpDiff &&
                          imB->w == im->w && imB->h == im->h;
    const bool flipMode = imB && app.compareMode == App::CmpFlip;
    if (flipMode && app.flipAuto) {          // alternate on its own
        double now = ImGui::GetTime();
        if (now >= app.flipNext) {
            app.flipShowB = !app.flipShowB;
            app.flipNext = now + std::max(app.flipPeriod, 0.05f);
        }
    }
    // split panes are clamped harder than the wipe divider: a 3%-wide pane is a
    // useless pane, and "fit" into it would shrink the image to nothing
    if (split) app.splitFrac = std::clamp(app.splitFrac, 0.12f, 0.88f);
    float splitX = canvasP0.x + canvasSize.x * app.splitFrac;   // pane boundary (split)
    float wipeX  = canvasP0.x + canvasSize.x * app.wipeFrac;    // divider (wipe)
    const float GUTTER = 3.0f * s;
    ImVec2 paneAp0 = canvasP0, paneAsz = canvasSize, paneBp0 = canvasP0, paneBsz = canvasSize;
    if (split) {
        // no artificial floor here: the pane the image is centred on must be the
        // pane it is clipped to, or it draws off-centre from the visible strip
        paneAsz.x = std::max(splitX - canvasP0.x - GUTTER, 1.0f);
        paneBp0.x = std::min(splitX + GUTTER, canvasP1.x - 1.0f);
        paneBsz.x = std::max(canvasP1.x - paneBp0.x, 1.0f);
    }
    // In split mode the pane under the cursor defines image coordinates; in wipe
    // mode both images occupy the same pane, so there is nothing to switch.
    auto paneP0 = [&](bool b) { return b ? paneBp0 : paneAp0; };
    auto paneSz = [&](bool b) { return b ? paneBsz : paneAsz; };
    auto mapIn = [&](bool b, float ix, float iy) {
        ImVec2 p = paneP0(b), z = paneSz(b);
        return ImVec2(p.x + z.x * 0.5f + (ix - app.view.center.x) * app.view.zoom,
                      p.y + z.y * 0.5f + (iy - app.view.center.y) * app.view.zoom);
    };
    auto unmapIn = [&](bool b, ImVec2 sc) {
        ImVec2 p = paneP0(b), z = paneSz(b);
        return ImVec2(app.view.center.x + (sc.x - p.x - z.x * 0.5f) / app.view.zoom,
                      app.view.center.y + (sc.y - p.y - z.y * 0.5f) / app.view.zoom);
    };
    auto inBPane = [&](ImVec2 sc) { return split && sc.x >= paneBp0.x; };
    // "fit" must fit the pane the image is drawn in, not the whole canvas. A is
    // the reference, so fit A's pane; B is the same view by construction.
    ImVec2 fitSize = split ? ImVec2(paneAsz.x, canvasSize.y) : canvasSize;
    if (im && app.fitRequested) { fitToCanvas(fitSize); app.fitRequested = false; }

    auto imgToScr = [&](float ix, float iy) { return mapIn(false, ix, iy); };
    auto scrToImg = [&](ImVec2 sc) { return unmapIn(inBPane(sc), sc); };
    // A drag must stay in the pane it started in. Re-resolving per call would jump
    // the coordinates by half a canvas the moment the cursor crosses the boundary,
    // collapsing the ROI being drawn (or teleporting the one being moved).
    static bool dragPaneB = false;
    auto dragToImg = [&](ImVec2 sc) { return unmapIn(dragPaneB, sc); };

    // the divider is grabbed anywhere along its height, not just on the handle
    bool onDivider = imB && app.compareMode != App::CmpDiff &&
                     fabsf(io.MousePos.x - (split ? splitX : wipeX)) <= 7.0f * s &&
                     io.MousePos.y >= canvasP0.y && io.MousePos.y <= canvasP1.y;
    if (onDivider && (hovered || ImGui::IsItemActive())) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // drag state: pan / new-ROI / move / resize (tool-mode interaction model)
    enum { DK_NONE, DK_PAN, DK_ROI_NEW, DK_ANN_MOVE, DK_ANN_RESIZE, DK_DIVIDER };
    static int dk = DK_NONE;
    static bool dragMoved = false;
    static bool clickEligible = false;   // left-button, no pan modifiers: clicks act on tools
    static ImVec2 drag0;
    static int dragAnnId = -1, dragCorner = -1;
    static bool dragSplitDivider = false;   // which divider a DK_DIVIDER drag grabbed
    static float dragDividerDx = 0;         // cursor offset from it, so it does not snap
    static int annOrig[4] = {};
    static int tmpRect[4] = {};
    static bool tmpActive = false;

    auto hitTest = [&](ImVec2 mimg, int& cornerOut) -> int {   // returns Ann::id or -1
        cornerOut = -1;
        float tol = 8.0f / app.view.zoom;
        for (int i = (int)app.anns.size() - 1; i >= 0; i--) {  // topmost first
            const App::Ann& a = app.anns[i];
            if (!a.visible) continue;
            if (a.type == 1) {
                if (fabsf(mimg.x - (a.x + 0.5f)) < tol && fabsf(mimg.y - (a.y + 0.5f)) < tol)
                    return a.id;
            } else {
                float xs[2] = { (float)a.x, (float)(a.x + a.w) };
                float ys[2] = { (float)a.y, (float)(a.y + a.h) };
                for (int cy = 0; cy < 2; cy++)
                    for (int cx = 0; cx < 2; cx++)
                        if (fabsf(mimg.x - xs[cx]) < tol && fabsf(mimg.y - ys[cy]) < tol) {
                            cornerOut = cy * 2 + cx;
                            return a.id;
                        }
                if (mimg.x >= a.x && mimg.x <= a.x + a.w && mimg.y >= a.y && mimg.y <= a.y + a.h)
                    return a.id;
            }
        }
        return -1;
    };

    if (!im) {   // image closed mid-drag: do not leave a stale preview or drag kind
        dk = DK_NONE; tmpActive = false; dragAnnId = -1; app.annBusy = false;
    }
    if (im) {
        if (ImGui::IsItemActivated()) {
            dragPaneB = inBPane(io.MousePos);      // latched for the whole drag
            drag0 = dragToImg(io.MousePos);
            dragMoved = false;
            dragAnnId = -1; dragCorner = -1; tmpActive = false;
            bool mid = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            bool space = ImGui::IsKeyDown(ImGuiKey_Space);
            clickEligible = !mid && !space && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            // Modeless: middle / Space always pan. Otherwise the press position
            // decides - on a handle = resize, inside a ROI = move, empty = new ROI
            // (or pan, if the user prefers that; Shift inverts either way).
            if (mid || space) {
                dk = DK_PAN;
            } else {
                int corner; int hit = hitTest(drag0, corner);
                App::Ann* a = hit >= 0 ? findAnn(hit) : nullptr;
                // the divider yields to annotations: "the press position decides"
                // must keep meaning what it says even under the divider
                if (onDivider && hit < 0) {
                    dk = DK_DIVIDER;
                    dragSplitDivider = split;
                    dragDividerDx = io.MousePos.x - (split ? splitX : wipeX);
                    clickEligible = false;   // grabbing the divider is not a click on the image
                } else if (a && a->type == 0 && corner >= 0) {
                    dk = DK_ANN_RESIZE; dragAnnId = hit; dragCorner = corner;
                    annOrig[0] = a->x; annOrig[1] = a->y; annOrig[2] = a->w; annOrig[3] = a->h;
                    app.selectedAnn = hit;
                } else if (a && a->type == 0) {
                    dk = DK_ANN_MOVE; dragAnnId = hit;
                    annOrig[0] = a->x; annOrig[1] = a->y;
                    app.selectedAnn = hit;
                } else {
                    bool wantPan = app.dragPans != io.KeyShift;   // Shift inverts the default
                    dk = wantPan ? DK_PAN : DK_ROI_NEW;
                }
            }
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f)))
            dragMoved = true;
        bool draggingAny = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ||
                           ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0);
        if (active && draggingAny) {
            if (dk == DK_DIVIDER) {
                // keep the grab offset: the divider must not snap to the cursor
                float f = (io.MousePos.x - dragDividerDx - canvasP0.x) / std::max(canvasSize.x, 1.0f);
                f = std::clamp(f, dragSplitDivider ? 0.12f : 0.03f, dragSplitDivider ? 0.88f : 0.97f);
                // which divider was grabbed is decided at press time: switching
                // mode mid-drag must not move the other one
                (dragSplitDivider ? app.splitFrac : app.wipeFrac) = f;
            } else if (dk == DK_PAN) {
                app.view.center.x -= io.MouseDelta.x / app.view.zoom;
                app.view.center.y -= io.MouseDelta.y / app.view.zoom;
            } else if (dk == DK_ROI_NEW && dragMoved) {
                ImVec2 q = dragToImg(io.MousePos);
                float x0 = std::clamp(std::min(drag0.x, q.x), 0.0f, (float)im->w);
                float x1 = std::clamp(std::max(drag0.x, q.x), 0.0f, (float)im->w);
                float y0 = std::clamp(std::min(drag0.y, q.y), 0.0f, (float)im->h);
                float y1 = std::clamp(std::max(drag0.y, q.y), 0.0f, (float)im->h);
                tmpRect[0] = (int)x0; tmpRect[1] = (int)y0;
                tmpRect[2] = std::max(0, (int)ceilf(x1) - tmpRect[0]);
                tmpRect[3] = std::max(0, (int)ceilf(y1) - tmpRect[1]);
                tmpActive = true;
            } else if (dk == DK_ANN_MOVE && dragMoved) {
                if (App::Ann* a = findAnn(dragAnnId)) {
                    ImVec2 q = dragToImg(io.MousePos);
                    int dx = (int)roundf(q.x - drag0.x), dy = (int)roundf(q.y - drag0.y);
                    a->x = std::clamp(annOrig[0] + dx, 0, std::max(0, im->w - std::max(a->w, 1)));
                    a->y = std::clamp(annOrig[1] + dy, 0, std::max(0, im->h - std::max(a->h, 1)));
                    app.annRev++;
                }
            } else if (dk == DK_ANN_RESIZE && dragMoved) {
                if (App::Ann* a = findAnn(dragAnnId)) {
                    int fx = (dragCorner % 2 == 0) ? annOrig[0] + annOrig[2] : annOrig[0];
                    int fy = (dragCorner / 2 == 0) ? annOrig[1] + annOrig[3] : annOrig[1];
                    ImVec2 q = dragToImg(io.MousePos);
                    int mx = std::clamp((int)roundf(q.x), 0, im->w);
                    int my = std::clamp((int)roundf(q.y), 0, im->h);
                    a->x = std::min(fx, mx); a->w = std::max(1, std::abs(mx - fx));
                    a->y = std::min(fy, my); a->h = std::max(1, std::abs(my - fy));
                    app.annRev++;
                }
            }
        }
        if (ImGui::IsItemDeactivated()) {
            ImVec2 q = dragToImg(io.MousePos);
            if (dk == DK_ROI_NEW && dragMoved && tmpActive) {
                // A long thin drag is a deliberate row/column band, so require
                // extent in EITHER axis, not both - the old "3px in both" rule
                // made a 1px-tall band impossible to draw.
                if (tmpRect[2] >= 1 && tmpRect[3] >= 1 &&
                    (tmpRect[2] >= 3 || tmpRect[3] >= 3) && !annBlockedOnPreview())
                    addAnn(0, tmpRect[0], tmpRect[1], tmpRect[2], tmpRect[3]);
            } else if (!dragMoved && clickEligible) {   // middle/Space clicks never select
                int corner;
                int hit = hitTest(q, corner);
                app.selectedAnn = hit >= 0 ? hit : 0;   // empty click -> back to "All"
            }
            tmpActive = false;
            dk = DK_NONE; dragAnnId = -1;
        }
        app.annBusy = dk == DK_ANN_MOVE || dk == DK_ANN_RESIZE || dk == DK_ROI_NEW;

        // wheel: Ctrl(/Cmd)+wheel = zoom, plain wheel = pan (View menu can invert)
        if (hovered && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
            bool zoomMod = io.KeyCtrl || io.KeySuper;
            // plain-wheel-zoom mode still leaves Shift+wheel as horizontal pan
            bool zoomGesture = app.wheelZoomPlain ? (!zoomMod && !io.KeyShift) : zoomMod;
            if (zoomGesture && io.MouseWheel != 0) {
                float wheel = std::clamp(io.MouseWheel, -3.0f, 3.0f);   // tame trackpad inertia
                ImVec2 mImg = scrToImg(io.MousePos);
                float z = std::clamp(app.view.zoom * powf(1.25f, wheel), 1.0f / 512, 256.0f);
                // anchor in the pane under the cursor: in split mode the image is
                // centred on its pane, not on the canvas
                ImVec2 ap0 = paneP0(inBPane(io.MousePos)), asz = paneSz(inBPane(io.MousePos));
                app.view.center.x = mImg.x - (io.MousePos.x - ap0.x - asz.x * 0.5f) / z;
                app.view.center.y = mImg.y - (io.MousePos.y - ap0.y - asz.y * 0.5f) / z;
                app.view.zoom = z;
            } else {
                float step = 80.0f / app.view.zoom;                     // image px per notch
                if (io.MouseWheel != 0) {
                    if (io.KeyShift) app.view.center.x -= io.MouseWheel * step;
                    else app.view.center.y -= io.MouseWheel * step;
                }
                if (io.MouseWheelH != 0) app.view.center.x += io.MouseWheelH * step;
            }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fitToCanvas(fitSize);

        // right-click: actions at this point, no modes and no modifiers needed
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) {
            ImVec2 q = scrToImg(io.MousePos);
            app.ctxX = (int)floorf(q.x); app.ctxY = (int)floorf(q.y);
            int corner; int hit = hitTest(q, corner);
            if (hit >= 0) app.selectedAnn = hit;
            ImGui::OpenPopup("canvasctx");
        }
        if (ImGui::BeginPopup("canvasctx")) {
            bool inRange = app.ctxX >= 0 && app.ctxY >= 0 && app.ctxX < im->w && app.ctxY < im->h;
            if (ImGui::MenuItem("Add pin here", "P", false, inRange) && !annBlockedOnPreview())
                addAnn(1, app.ctxX, app.ctxY, 0, 0);
            App::Ann* sel = findAnn(app.selectedAnn);
            ImGui::Separator();
            // a single row / column is one pixel thick and cannot be dragged out
            if (ImGui::MenuItem("Select this row (1 px)", "X", false, inRange)) {
                addAnn(0, 0, app.ctxY, im->w, 1);
                if (App::Ann* n = findAnn(app.selectedAnn)) {
                    n->prevX = app.ctxX; n->prevW = 1;
                    n->label = "row " + std::to_string(app.ctxY);
                }
            }
            if (ImGui::MenuItem("Select this column (1 px)", "Y", false, inRange)) {
                addAnn(0, app.ctxX, 0, 1, im->h);
                if (App::Ann* n = findAnn(app.selectedAnn)) {
                    n->prevY = app.ctxY; n->prevH = 1;
                    n->label = "col " + std::to_string(app.ctxX);
                }
            }
            if (ImGui::MenuItem("Widen ROI to full width", nullptr, false, sel && sel->type == 0)) {
                sel->prevX = sel->x; sel->prevW = sel->w; sel->x = 0; sel->w = im->w; app.annRev++;
            }
            if (ImGui::MenuItem("Widen ROI to full height", nullptr, false, sel && sel->type == 0)) {
                sel->prevY = sel->y; sel->prevH = sel->h; sel->y = 0; sel->h = im->h; app.annRev++;
            }
            if (ImGui::MenuItem("Crop image to this ROI", nullptr, false, sel && sel->type == 0))
                cropCurrentToSelectedRoi();
            if (ImGui::MenuItem("Delete", "Del", false, sel != nullptr))
                deleteAnn(app.selectedAnn);
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to window", "F")) app.fitRequested = true;
            if (ImGui::MenuItem("Actual size", "1")) app.view.zoom = 1.0f;
            ImGui::EndPopup();
        }
    }

    // hover position; in split mode the pane under the cursor decides which image
    // the readout is about, so a bigger B is not cut off at A's extent
    app.hoverX = app.hoverY = -1;
    app.hoverInB = false;
    if (im && hovered) {
        bool inB = inBPane(io.MousePos);
        const ImageDoc* ref = inB && imB ? imB : im;
        ImVec2 q = unmapIn(inB, io.MousePos);
        int ix = (int)floorf(q.x), iy = (int)floorf(q.y);
        if (ix >= 0 && iy >= 0 && ix < ref->w && iy < ref->h) {
            app.hoverX = ix; app.hoverY = iy; app.hoverInB = inB && imB != nullptr;
        }
    }

    // ---- draw canvas ----
    dl->AddRectFilled(canvasP0, canvasP1, IM_COL32(12, 14, 16, 255));
    dl->PushClipRect(canvasP0, canvasP1, true);
    // One image and its overlays, drawn through the given pane's mapping. Used
    // once when compare is off, twice (A and B) when it is on.
    auto drawImageOnly = [&](ImageDoc* d, bool b) {
        auto map = [&](float ix, float iy) { return mapIn(b, ix, iy); };
        if (d->texDirty) rebuildTexture(*d);
        else touchTex(*d);                 // keep it resident: it is on screen
        setFilter(*d, app.view.zoom >= 1.0f);
        ImVec2 p0 = map(0, 0), p1 = map((float)d->w, (float)d->h);
        dl->AddImage((ImTextureID)(intptr_t)d->tex, p0, p1);
        dl->AddRect(p0, p1, IM_COL32(90, 100, 110, 255));
    };
    // Overlays are drawn ONCE per pane. In wipe mode both images share one pane,
    // so drawing them per image would double the alpha of every ROI fill and grid
    // line right of the divider - a brightness step exactly where you are looking.
    auto drawOverlays = [&](ImageDoc* d, bool b) {
        auto map = [&](float ix, float iy) { return mapIn(b, ix, iy); };
        // pixel grid at high zoom
        if (app.showGrid && app.view.zoom >= 8.0f) {
            ImVec2 tl = unmapIn(b, canvasP0), br = unmapIn(b, canvasP1);
            int x0 = std::max(0, (int)floorf(tl.x)), x1 = std::min(d->w, (int)ceilf(br.x));
            int y0 = std::max(0, (int)floorf(tl.y)), y1 = std::min(d->h, (int)ceilf(br.y));
            ImU32 gc = IM_COL32(120, 120, 120, 70);
            for (int x = x0; x <= x1; x++) { ImVec2 a = map((float)x, (float)y0), b2 = map((float)x, (float)y1); dl->AddLine(a, b2, gc); }
            for (int y = y0; y <= y1; y++) { ImVec2 a = map((float)x0, (float)y), b2 = map((float)x1, (float)y); dl->AddLine(a, b2, gc); }
        }
        // hovered pixel outline
        if (app.hoverX >= 0 && app.view.zoom >= 2.0f) {
            ImVec2 a = map((float)app.hoverX, (float)app.hoverY);
            ImVec2 b2 = map((float)app.hoverX + 1, (float)app.hoverY + 1);
            dl->AddRect(a, b2, IM_COL32(255, 184, 77, 255), 0, 0, 1.5f);
        }
        // annotations (ROIs + POIs)
        for (const auto& a : app.anns) {
            if (!a.visible) continue;
            ImU32 col = ANN_COLORS[a.color & 7];
            bool sel = a.id == app.selectedAnn;
            if (a.type == 0) {
                ImVec2 ra = map((float)a.x, (float)a.y);
                ImVec2 rb = map((float)(a.x + a.w), (float)(a.y + a.h));
                dl->AddRectFilled(ra, rb, (col & 0x00FFFFFF) | 0x1E000000);
                dl->AddRect(ra, rb, col, 0, 0, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), col, a.label.c_str());
                if (sel) {                                        // resize handles
                    ImVec2 hs[4] = { ra, ImVec2(rb.x, ra.y), ImVec2(ra.x, rb.y), rb };
                    for (const auto& hp : hs)
                        dl->AddRectFilled(ImVec2(hp.x - 3, hp.y - 3), ImVec2(hp.x + 3, hp.y + 3), col);
                }
            } else {
                if (a.x >= d->w || a.y >= d->h) continue;         // outside this image
                ImVec2 cpt = map(a.x + 0.5f, a.y + 0.5f);
                float r = sel ? 10.0f : 8.0f;
                dl->AddLine(ImVec2(cpt.x - r, cpt.y), ImVec2(cpt.x + r, cpt.y), col, sel ? 2.5f : 1.5f);
                dl->AddLine(ImVec2(cpt.x, cpt.y - r), ImVec2(cpt.x, cpt.y + r), col, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(cpt.x + 5, cpt.y + 4), col, a.label.c_str());
            }
        }
        // ROI being created (live preview)
        if (tmpActive && tmpRect[2] > 0 && tmpRect[3] > 0) {
            ImVec2 ra = map((float)tmpRect[0], (float)tmpRect[1]);
            ImVec2 rb = map((float)(tmpRect[0] + tmpRect[2]), (float)(tmpRect[1] + tmpRect[3]));
            dl->AddRectFilled(ra, rb, IM_COL32(77, 163, 255, 26));
            dl->AddRect(ra, rb, IM_COL32(77, 163, 255, 220), 0, 0, 1.5f);
            char lb[48];
            snprintf(lb, 48, "%dx%d", tmpRect[2], tmpRect[3]);
            dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), IM_COL32(120, 190, 255, 255), lb);
        }
    };

    // Hold B to see B full-frame: flicking between two full images is how small
    // differences become visible - a fixed divider cannot show them.
    bool flashB = imB && !flipMode && !ImGui::GetIO().WantTextInput && ImGui::IsKeyDown(ImGuiKey_B) &&
                  !ImGui::IsKeyDown(ImGuiKey_LeftShift) && !ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (im) {
        if (flipMode) {
            ImageDoc* d = app.flipShowB ? imB : im;
            drawImageOnly(d, false);
            drawOverlays(d, false);
        } else if (flashB) {
            drawImageOnly(imB, false);
            drawOverlays(imB, false);
        } else if (diffMode) {
            ensureDiffTexture(*im, *imB);
            if (app.diff.tex) {
                ImVec2 p0 = mapIn(false, 0, 0);
                ImVec2 p1 = mapIn(false, (float)app.diff.w, (float)app.diff.h);
                dl->AddImage((ImTextureID)(intptr_t)app.diff.tex, p0, p1);
                dl->AddRect(p0, p1, IM_COL32(90, 100, 110, 255));
            }
            drawOverlays(im, false);
        } else if (split) {
            dl->PushClipRect(paneAp0, ImVec2(paneAp0.x + paneAsz.x, canvasP1.y), true);
            drawImageOnly(im, false); drawOverlays(im, false);
            dl->PopClipRect();
            dl->PushClipRect(paneBp0, ImVec2(paneBp0.x + paneBsz.x, canvasP1.y), true);
            drawImageOnly(imB, true); drawOverlays(imB, true);
            dl->PopClipRect();
        } else if (wipe) {
            drawImageOnly(im, false);                   // A everywhere...
            dl->PushClipRect(ImVec2(wipeX, canvasP0.y), canvasP1, true);
            drawImageOnly(imB, false);                  // ...B over the right side
            dl->PopClipRect();
            drawOverlays(im, false);                    // ...and one set of overlays
        } else {
            drawImageOnly(im, false);
            drawOverlays(im, false);
        }
        // ---- compare chrome: A/B badges + the divider you drag ----
        if (imB) {
            const ImU32 colA = IM_COL32(120, 200, 255, 255), colB = IM_COL32(255, 190, 120, 255);
            const float pad = 4.0f * s;
            // A badge must never sit over B's pixels (or the reverse), so each one
            // is fitted to its own side: elide the name, then drop it entirely.
            auto badge = [&](float x0, float x1, bool rightAlign, const char* tag,
                             const std::string& nm, ImU32 col) {
                float avail = x1 - x0 - pad * 2;
                std::string t = std::string(tag) + "  " + nm;
                if (ImGui::CalcTextSize(t.c_str()).x > avail) {          // middle-elide
                    std::string best = tag;
                    for (size_t keep = nm.size(); keep >= 6; keep--) {
                        std::string cand = std::string(tag) + "  " + nm.substr(0, keep / 2) +
                                           "..." + nm.substr(nm.size() - (keep - keep / 2));
                        if (ImGui::CalcTextSize(cand.c_str()).x <= avail) { best = cand; break; }
                    }
                    t = best;
                }
                ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                if (ts.x > avail) return;                                 // no room at all
                float bx = rightAlign ? x1 - ts.x - pad * 2 : x0;
                float by = canvasP0.y + 6 * s;
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + ts.x + pad * 2, by + ts.y + pad),
                                  IM_COL32(0, 0, 0, 180), 3.0f * s);
                dl->AddText(ImVec2(bx + pad, by + pad * 0.5f), col, t.c_str());
            };
            bool hot = onDivider || (ImGui::IsItemActive() && dk == DK_DIVIDER);
            if (flipMode) {
                // which side is on screen has to be readable at a glance, or a
                // blink comparison tells you nothing about WHICH one differs
                ImageDoc* d = app.flipShowB ? imB : im;
                badge(canvasP0.x + 6 * s, canvasP1.x, false,
                      app.flipShowB ? "B" : "A",
                      d->name + (app.flipAuto ? "   (auto)" : "   (B / Space to toggle)"),
                      app.flipShowB ? colB : colA);
                // a coloured edge so peripheral vision registers the switch too
                dl->AddRect(canvasP0, canvasP1, app.flipShowB ? colB : colA, 0, 0, 3.0f * s);
            } else if (app.compareMode == App::CmpDiff) {
                badge(canvasP0.x + 6 * s, canvasP1.x, false,
                      app.diffAbs ? "|A-B|" : "A-B", im->name + "  -  " + imB->name,
                      IM_COL32(220, 225, 235, 255));
                if (!diffMode) {
                    const char* msg = "difference needs A and B to be the same size";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    dl->AddRectFilled(ImVec2(canvasP0.x + 6 * s, canvasP0.y + 30 * s),
                                      ImVec2(canvasP0.x + 18 * s + ts.x, canvasP0.y + 30 * s + ts.y + 6 * s),
                                      IM_COL32(90, 60, 20, 210), 3.0f * s);
                    dl->AddText(ImVec2(canvasP0.x + 12 * s, canvasP0.y + 33 * s),
                                IM_COL32(255, 200, 120, 255), msg);
                } else {
                    // Colour bar with the scale spelled out: a difference image
                    // without its scale is a picture, not a measurement.
                    float gain = app.diff.gain;
                    bool intType = !im->dtype.empty() && (im->dtype[0] == 'u' || im->dtype[0] == 'i');
                    char loS[48], hiS[48];
                    snprintf(hiS, sizeof hiS, "+%g%s", gain, intType ? " DN" : "");
                    snprintf(loS, sizeof loS, app.diffAbs ? "0" : "-%g%s", gain, intType ? " DN" : "");
                    float barW = 180 * s, barH = 10 * s;
                    float bx = canvasP0.x + 10 * s;
                    float by = canvasP1.y - barH - ImGui::GetFontSize() - 12 * s;
                    dl->AddRectFilled(ImVec2(bx - 6 * s, by - 6 * s),
                                      ImVec2(bx + barW + 6 * s, by + barH + ImGui::GetFontSize() + 8 * s),
                                      IM_COL32(0, 0, 0, 180), 3.0f * s);
                    for (int i = 0; i < (int)barW; i++) {   // same ramp as the pixels
                        float t = app.diffAbs ? (float)i / barW : (float)i / barW * 2.0f - 1.0f;
                        float r, g2, bl;
                        if (app.diffAbs)   { float m = t; r = g2 = bl = m; }
                        else if (t >= 0)   { r = t; g2 = 0.35f * t; bl = 0.10f * t; }
                        else               { float m = -t; r = 0.10f * m; g2 = 0.45f * m; bl = m; }
                        ImU32 c = IM_COL32((int)(std::clamp(r, 0.f, 1.f) * 255),
                                           (int)(std::clamp(g2, 0.f, 1.f) * 255),
                                           (int)(std::clamp(bl, 0.f, 1.f) * 255), 255);
                        dl->AddLine(ImVec2(bx + i, by), ImVec2(bx + i, by + barH), c);
                    }
                    dl->AddRect(ImVec2(bx, by), ImVec2(bx + barW, by + barH), IM_COL32(90, 100, 110, 255));
                    ImU32 tc = IM_COL32(210, 216, 226, 255);
                    dl->AddText(ImVec2(bx, by + barH + 2 * s), tc, loS);
                    ImVec2 hs = ImGui::CalcTextSize(hiS);
                    dl->AddText(ImVec2(bx + barW - hs.x, by + barH + 2 * s), tc, hiS);
                    if (app.diff.clipped > 0) {
                        char cs[64];
                        snprintf(cs, sizeof cs, "%.2f%% off scale", app.diff.clipped * 100);
                        ImVec2 cts = ImGui::CalcTextSize(cs);
                        dl->AddText(ImVec2(bx + (barW - cts.x) * 0.5f, by - cts.y - 2 * s),
                                    IM_COL32(255, 235, 120, 255), cs);
                    }
                }
            } else if (flashB) {
                badge(canvasP0.x + 6 * s, canvasP1.x, false, "B", imB->name + "  (hold B)", colB);
            } else if (split) {
                badge(paneAp0.x + 6 * s, paneAp0.x + paneAsz.x, false, "A", im->name, colA);
                badge(paneBp0.x + 6 * s, paneBp0.x + paneBsz.x, false, "B", imB->name, colB);
                dl->AddRectFilled(ImVec2(splitX - GUTTER, canvasP0.y), ImVec2(splitX + GUTTER, canvasP1.y),
                                  hot ? IM_COL32(120, 132, 148, 255) : IM_COL32(60, 66, 74, 255));
            } else {
                badge(canvasP0.x + 6 * s, wipeX, false, "A", im->name, colA);
                badge(wipeX + 8 * s, canvasP1.x - 6 * s, true, "B", imB->name, colB);
                dl->AddLine(ImVec2(wipeX, canvasP0.y), ImVec2(wipeX, canvasP1.y),
                            IM_COL32(255, 255, 255, hot ? 255 : 200), 1.5f * s);
            }
            // grab handle: a stubby bar at mid-height, the thing you aim at
            if (!flashB && !flipMode && app.compareMode != App::CmpDiff) {
                float dx = split ? splitX : wipeX;
                float hy = (canvasP0.y + canvasP1.y) * 0.5f, hh = 22 * s, hw = 5 * s;
                dl->AddRectFilled(ImVec2(dx - hw, hy - hh), ImVec2(dx + hw, hy + hh),
                                  hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 234, 240, 220), 3.0f * s);
                for (float o = -1.5f; o <= 1.5f; o += 3.0f)
                    dl->AddLine(ImVec2(dx + o * s, hy - hh * 0.4f), ImVec2(dx + o * s, hy + hh * 0.4f),
                                IM_COL32(40, 44, 50, 255), 1.0f * s);
            }
        } else if (app.compareMode != App::CmpOff) {
            // compare is on but B is gone (closed, renamed, never picked): say so
            // instead of rendering exactly like compare-off
            const char* msg = "compare is on, but no B image - View > Compare A/B > B image";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            float y = canvasP0.y + 6 * s;
            dl->AddRectFilled(ImVec2(canvasP0.x + 6 * s, y),
                              ImVec2(canvasP0.x + 6 * s + ts.x + 12 * s, y + ts.y + 6 * s),
                              IM_COL32(90, 60, 20, 210), 3.0f * s);
            dl->AddText(ImVec2(canvasP0.x + 12 * s, y + 3 * s), IM_COL32(255, 200, 120, 255), msg);
        }
        // Remote preview: say so ON the image. Measuring a 1/3-sampled preview
        // without knowing it is how a wrong number gets written down.
        if (im->remoteStep > 1) {
            char msg[160];
            if (im->remoteErr.empty())
                snprintf(msg, sizeof msg, "PREVIEW 1/%d - fetching full resolution...",
                         im->remoteStep);
            else
                snprintf(msg, sizeof msg, "PREVIEW 1/%d - fetch failed: %s",
                         im->remoteStep, im->remoteErr.c_str());
            ImVec2 ts = ImGui::CalcTextSize(msg);
            float bx = std::max(canvasP0.x + 6 * s, (canvasP0.x + canvasP1.x - ts.x) * 0.5f);
            float by = canvasP0.y + 8 * s;
            ImU32 bg = im->remoteErr.empty() ? IM_COL32(120, 80, 20, 225) : IM_COL32(120, 30, 30, 225);
            dl->AddRectFilled(ImVec2(bx - 8 * s, by - 4 * s),
                              ImVec2(bx + ts.x + 8 * s, by + ts.y + 4 * s), bg, 4.0f * s);
            dl->AddText(ImVec2(bx, by), IM_COL32(255, 215, 140, 255), msg);
        }
    } else {
        const char* msg = "Drop .npy / .bin / .raw files here   (O: open file)";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((canvasP0.x + canvasP1.x - ts.x) / 2, (canvasP0.y + canvasP1.y - ts.y) / 2),
                    IM_COL32(120, 130, 140, 255), msg);
    }
    dl->PopClipRect();

    // ---- rulers ----
    ImU32 rulerBg = IM_COL32(24, 27, 31, 255), tickCol = IM_COL32(140, 150, 160, 255),
          txtCol = IM_COL32(170, 180, 190, 255), markCol = IM_COL32(255, 184, 77, 255);
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(canvasP1.x, origin.y + RULER_H), rulerBg);          // top
    dl->AddRectFilled(ImVec2(origin.x, canvasP0.y), ImVec2(origin.x + RULER_W, canvasP1.y), rulerBg);        // left
    dl->AddText(ImVec2(origin.x + 6, origin.y + 3), IM_COL32(100, 110, 120, 255), "px");

    if (im) {
        // tick spacing derived from label width so 5-digit coords never collide
        float minSpacing = std::max(48.0f * s, ImGui::CalcTextSize("00000").x * 1.5f);
        float step = niceStep(minSpacing / app.view.zoom);
        if (step < 1) step = 1;
        // Top ruler (X): once per pane. A single ruler across both panes would
        // label pane B with pane A's coordinates - a ruler that lies.
        auto xRuler = [&](bool b) {
            ImVec2 p0 = paneP0(b), sz = paneSz(b);
            dl->PushClipRect(ImVec2(p0.x, origin.y), ImVec2(p0.x + sz.x, origin.y + RULER_H), true);
            float ix0 = unmapIn(b, p0).x, ix1 = unmapIn(b, ImVec2(p0.x + sz.x, p0.y)).x;
            for (float t = floorf(ix0 / step) * step; t <= ix1; t += step) {
                if (t < 0 || t > im->w) continue;
                float sx = mapIn(b, t, 0).x;
                dl->AddLine(ImVec2(sx, origin.y + RULER_H - TICK), ImVec2(sx, origin.y + RULER_H), tickCol);
                char lb[32]; snprintf(lb, 32, "%.0f", t);
                dl->AddText(ImVec2(sx + 3 * s, origin.y + 2), txtCol, lb);
            }
            if (app.hoverX >= 0) {
                float sx = mapIn(b, (float)app.hoverX + 0.5f, 0).x;
                dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + RULER_H), markCol, 1.5f);
            }
            dl->PopClipRect();
        };
        xRuler(false);
        if (split) xRuler(true);
        // left ruler (Y)
        dl->PushClipRect(ImVec2(origin.x, canvasP0.y), ImVec2(origin.x + RULER_W, canvasP1.y), true);
        {
            float iy0 = scrToImg(canvasP0).y, iy1 = scrToImg(canvasP1).y;
            for (float t = floorf(iy0 / step) * step; t <= iy1; t += step) {
                if (t < 0 || t > im->h) continue;
                float sy = imgToScr(0, t).y;
                dl->AddLine(ImVec2(origin.x + RULER_W - TICK, sy), ImVec2(origin.x + RULER_W, sy), tickCol);
                char lb[32]; snprintf(lb, 32, "%.0f", t);
                dl->AddText(ImVec2(origin.x + 4 * s, sy + 2), txtCol, lb);
            }
            if (app.hoverY >= 0) {
                float sy = imgToScr(0, (float)app.hoverY + 0.5f).y;
                dl->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + RULER_W, sy), markCol, 1.5f);
            }
        }
        dl->PopClipRect();
    }

    // ---- the footer strip: whose pixels these are, and where in the stack ----
    // Its own reserved band under the canvas (FOOT_H above): name at the left,
    // scrub bar filling the middle, frame counter at the right. Nothing here
    // ever covers a pixel being judged.
    if (im) {
        float fy0 = canvasP1.y + 3.0f * s;
        float fy1 = origin.y + avail.y - 2.0f * s;
        float cy = (fy0 + fy1) * 0.5f;
        std::vector<int> fr = im->seqId != 0 ? framesOfSeq(im->seqId) : std::vector<int>();
        const App::SeqInfo* si = im->seqId != 0 ? seqInfo(im->seqId) : nullptr;
        const char* name = si && fr.size() > 1 ? si->name.c_str() : im->name.c_str();
        ImVec2 ts = ImGui::CalcTextSize(name);
        float nameW = std::min(ts.x, canvasSize.x * 0.45f);
        dl->PushClipRect(ImVec2(canvasP0.x, fy0), ImVec2(canvasP0.x + nameW, fy1), true);
        dl->AddText(ImVec2(canvasP0.x, cy - ts.y * 0.5f), IM_COL32(175, 183, 191, 200), name);
        dl->PopClipRect();
        if (si && fr.size() > 1) {
            int pos = 0;
            for (int k = 0; k < (int)fr.size(); k++) if (fr[k] == app.current) pos = k;
            char cnt[32];
            snprintf(cnt, sizeof cnt, "%d/%d", pos + 1, (int)fr.size());
            ImVec2 cs = ImGui::CalcTextSize(cnt);
            dl->AddText(ImVec2(canvasP1.x - cs.x, cy - cs.y * 0.5f),
                        IM_COL32(175, 183, 191, 200), cnt);
            float barH = 5.0f * s;
            ImVec2 b0(canvasP0.x + nameW + 12 * s, cy - barH * 0.5f);
            ImVec2 b1(canvasP1.x - cs.x - 12 * s, cy + barH * 0.5f);
            if (b1.x > b0.x + 40 * s) {
                ImGui::SetCursorScreenPos(ImVec2(b0.x, fy0));
                ImGui::InvisibleButton("scrub", ImVec2(b1.x - b0.x, fy1 - fy0));
                bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
                int alpha = sh || sa ? 235 : 150;
                dl->AddRectFilled(b0, b1, IM_COL32(120, 130, 140, 60), barH * 0.5f);
                float fx = b0.x + (b1.x - b0.x) * ((float)pos / (float)(fr.size() - 1));
                dl->AddRectFilled(b0, ImVec2(fx, b1.y), IM_COL32(110, 160, 210, alpha), barH * 0.5f);
                dl->AddCircleFilled(ImVec2(fx, cy), barH * (sh || sa ? 1.4f : 1.0f),
                                    IM_COL32(160, 200, 240, alpha));
                if (sa) {
                    float t = std::clamp((io.MousePos.x - b0.x) / (b1.x - b0.x), 0.0f, 1.0f);
                    int want = (int)(t * (float)(fr.size() - 1) + 0.5f);
                    if (want != pos) selectImage(fr[want]);
                }
                if (sh) ImGui::SetTooltip("frame %d / %d  (drag; . and , step)", pos + 1, (int)fr.size());
            }
        }
    }
}

// The cache slot is a parameter (slot 0 = A, slot 1 = compare B). Everything
// below is the previous body with `app.hist` replaced by `H`.
//
// binBlack / binWhite: the value range the 256 bins span. NaN (the default, and
// what A always passes) means the image's own display range. The B slot is
// handed A's range instead: two histograms sharing one x axis have to be BINNED
// alike or the overlay is a lie - and it keeps the axis A's, which is what the
// user set. It is a cache key, so changing A's range re-bins B.
static void recomputeHistogramIfNeeded(ImageDoc* im, App::HistState& H,
                                       float binBlack = std::numeric_limits<float>::quiet_NaN(),
                                       float binWhite = std::numeric_limits<float>::quiet_NaN()) {
    const float wantBlack = std::isfinite(binBlack) ? binBlack : effBlack(*im);
    const float wantWhite = std::isfinite(binWhite) ? binWhite : effWhite(*im);
    // Key on the RESOLVED rect, never on annRev: annotation churn (dragging,
    // renaming, toggling visibility) must not re-scan a million pixels.
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {           // selected ROI drives the histogram
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), ch2 = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && ch2 > 0) { rx = cx; ry = cy; rw = cw; rh = ch2; roiUsed = true; }
        }
    }
    if (H.uid == im->uid && H.dataRev == im->dataRev && H.black == wantBlack &&
        H.white == wantWhite && H.cfa == im->cfa && H.cfaPattern == im->cfaPattern &&
        H.rx == rx && H.ry == ry && H.rw == rw && H.rh == rh)
        return;
    if (app.annBusy && H.uid == im->uid) return;   // mid-drag: keep the last result
    H.img = im; H.uid = im->uid; H.dataRev = im->dataRev;
    H.black = wantBlack; H.white = wantWhite;
    H.cfa = im->cfa; H.cfaPattern = im->cfaPattern;
    H.rx = rx; H.ry = ry; H.rw = rw; H.rh = rh; H.roiUsed = roiUsed;
    memset(H.bins, 0, sizeof H.bins);
    H.maxBin = 1; H.clipLo = H.clipHi = 0; H.sampled = 0;
    bool cfa = im->ch == 1 && im->cfa != 0;
    H.nSeries = cfa ? 4 : std::min(im->ch, 3);
    static const char* CFA_SERIES[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_SERIES[4] = { "ch0", "ch1", "ch2", "ch3" };
    for (int s = 0; s < 4; s++) H.seriesNames[s] = cfa ? CFA_SERIES[s] : CH_SERIES[s];
    float inv = 256.0f / std::max(H.white - H.black, 1e-20f);
    size_t total = (size_t)rw * rh;
    size_t step = std::max<size_t>(1, total / 1000000);   // sample <= ~1M px
    size_t below = 0, above = 0, cnt = 0, values = 0;
    double sum[4] = {}, sum2[4] = {};
    size_t n[4] = {};
    for (size_t p = 0; p < total; p += step) {
        int x = rx + (int)(p % rw), y = ry + (int)(p / rw);
        const float* src = &im->data[((size_t)y * im->w + x) * im->ch];
        if (cfa) {
            float v = src[0];
            if (std::isfinite(v)) {
                int s = cfaChannelAt(*im, x, y);
                float t = (v - H.black) * inv;
                if (t < 0) { below++; H.bins[s][0]++; }
                else if (t >= 256) { above++; H.bins[s][255]++; }
                else H.bins[s][(int)t]++;
                sum[s] += v; sum2[s] += (double)v * v; n[s]++;
                values++;
            }
        } else {
            for (int c = 0; c < H.nSeries; c++) {
                float v = src[c];
                if (!std::isfinite(v)) continue;
                float t = (v - H.black) * inv;
                if (t < 0) { below++; H.bins[c][0]++; }
                else if (t >= 256) { above++; H.bins[c][255]++; }
                else H.bins[c][(int)t]++;
                sum[c] += v; sum2[c] += (double)v * v; n[c]++;
                values++;
            }
        }
        cnt++;
    }
    for (int s = 0; s < H.nSeries; s++) {          // always-on mean / std
        if (!n[s]) { H.mean[s] = H.sd[s] = 0; continue; }
        double m = sum[s] / n[s], var = sum2[s] / n[s] - m * m;
        H.mean[s] = m;
        H.sd[s] = sqrt(var > 0 ? var : 0);
    }
    for (int s = 0; s < H.nSeries; s++)
        for (int b = 0; b < 256; b++)
            H.maxBin = std::max(H.maxBin, H.bins[s][b]);
    H.sampled = cnt;
    H.clipLo = values ? (double)below / values : 0;
    H.clipHi = values ? (double)above / values : 0;
}

// ---- L2 plot service ---------------------------------------------------------
// Every plot in the app goes through this: axes are always labelled (quantity +
// unit) and ticks land on 1/2/5*10^k values (integer axes never show fractions).
struct PlotRect {
    ImVec2 p0, p1;                       // inner drawing area
    float xmin, xmax, ymin, ymax;
    bool ok = false;
    ImVec2 at(float x, float y) const {
        return ImVec2(p0.x + (x - xmin) / (xmax - xmin) * (p1.x - p0.x),
                      p1.y - (y - ymin) / (ymax - ymin) * (p1.y - p0.y));
    }
};

// ImDrawList has no dashed line, and the A/B panels need one: hue is already
// spoken for by the CFA plane (R/Gr/Gb/B each own a colour), so the DASH is the
// only thing left that can say "this curve is B". Dash length is measured along
// the polyline, so a dense curve does not turn the dashes into a solid line.
static void addDashedPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col,
                              float thick, float dash, float gap) {
    if (n < 2 || dash <= 0 || gap <= 0) return;
    float phase = 0;                          // distance walked inside dash+gap
    for (int i = 1; i < n; i++) {
        ImVec2 a = pts[i - 1], b = pts[i];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (!(len > 0) || !std::isfinite(len)) continue;
        float t = 0;
        while (t < len) {
            float period = dash + gap;
            float inPeriod = fmodf(phase, period);
            bool on = inPeriod < dash;
            float left = on ? dash - inPeriod : period - inPeriod;
            float step = std::min(left, len - t);
            if (on) {
                ImVec2 p0(a.x + dx * (t / len), a.y + dy * (t / len));
                ImVec2 p1(a.x + dx * ((t + step) / len), a.y + dy * ((t + step) / len));
                dl->AddLine(p0, p1, col, thick);
            }
            t += step;
            phase += step;
        }
    }
}

// Long file names would push the legend off the plot; elide the FRONT, because
// the tail ("_gain10_000.npy") is the part that tells two captures apart.
static std::string elideFront(const std::string& s, size_t keep) {
    return s.size() <= keep ? s : ("..." + s.substr(s.size() - keep));
}

// The one amber this UI warns in: the fallback notices, the mismatch notices,
// and the [stale] marker below. Anything the reader must NOTICE is this colour.
static const ImVec4 AB_AMBER(0.95f, 0.72f, 0.35f, 1.0f);
static const ImU32  AB_AMBER32 = IM_COL32(242, 184, 89, 255);
// B's numbers are one frame behind while A is being stepped. The marker used to
// be appended to B's NAME and drawn in the surrounding grey, which during fast
// stepping is indistinguishable from part of the file name - the one moment it
// exists to be read. It is a separate token now, in the warning amber.
static const char* AB_STALE_TOKEN = "[stale]";

// What A or B is CALLED, everywhere the two are set against each other.
// A capture series names every stack's frames identically - 00/frame_000.npy
// and 01/frame_000.npy are both "frame_000.npy" - so the FILE name identifies
// neither side, and the legend, the side-by-side bands, the status bar, the
// Inspector and the B-image menu all read "frame_000.npy" twice. The STACK is
// what tells them apart, and SeqInfo::name already carries the folder the
// Files panel disambiguates with ("00/frame_000..004.npy"). A loose frame
// belongs to no stack and keeps its own name, which is unambiguous by
// definition - there is only one of it.
static std::string abDocLabel(const ImageDoc* d) {
    if (!d) return std::string();
    if (d->seqId != 0)
        if (const App::SeqInfo* si = seqInfo(d->seqId))
            if (!si->name.empty()) return si->name;
    return d->name;
}

static void fmtTick(char* buf, size_t n, double v, bool integer) {
    if (integer) snprintf(buf, n, "%.0f", v);
    else if (v != 0 && (fabs(v) >= 1e5 || fabs(v) < 1e-3)) snprintf(buf, n, "%.2e", v);
    else snprintf(buf, n, "%.4g", v);
}

// xLabel / yLabel must carry the quantity and its unit, e.g. "frequency (cycles/px)"
static PlotRect beginPlot(const char* xLabel, const char* yLabel,
                          float xmin, float xmax, float ymin, float ymax,
                          bool xInt, bool yInt, float height) {
    PlotRect pr;
    if (!(xmax > xmin)) xmax = xmin + 1;
    if (!(ymax > ymin)) ymax = ymin + 1;
    if (xInt && xmax - xmin < 1) xmax = xmin + 1;
    pr.xmin = xmin; pr.xmax = xmax; pr.ymin = ymin; pr.ymax = ymax;

    const float s = app.uiScale;
    const float fh = ImGui::GetFontSize();
    char tb[48];
    // measure EVERY tick label, not just the endpoints: on a 0..1 axis the
    // intermediate "0.25" is wider than both ends and would draw off-panel
    double ystep = niceStep((float)((ymax - ymin) / 4.0));
    if (yInt && ystep < 1) ystep = 1;
    float wy = 0.0f;
    for (double v = ceil(ymin / ystep) * ystep; v <= ymax + 1e-9; v += ystep) {
        fmtTick(tb, sizeof tb, v, yInt);
        wy = std::max(wy, ImGui::CalcTextSize(tb).x);
    }
    const float marginL = wy + 9 * s;   // 5*s tick gap + breathing room
    const float marginB = fh * 2 + 6 * s;          // x tick labels + x axis title
    const float marginT = fh + 4 * s;              // y axis title sits on top

    float wid = ImGui::GetContentRegionAvail().x;
    ImVec2 org = ImGui::GetCursorScreenPos();
    pr.p0 = ImVec2(org.x + marginL, org.y + marginT);
    pr.p1 = ImVec2(org.x + wid, org.y + marginT + height);
    if (pr.p1.x - pr.p0.x < 20 || height < 20) { ImGui::Dummy(ImVec2(wid, height)); return pr; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = IM_COL32(12, 14, 16, 255), grid = IM_COL32(46, 52, 58, 255),
          txt = IM_COL32(150, 160, 170, 255), axis = IM_COL32(90, 100, 110, 255);
    dl->AddRectFilled(pr.p0, pr.p1, bg);

    // Y ticks (ystep computed above, where the margin was measured)
    for (double v = ceil(ymin / ystep) * ystep; v <= ymax + 1e-9; v += ystep) {
        float y = pr.at(pr.xmin, (float)v).y;
        dl->AddLine(ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y), grid);
        fmtTick(tb, sizeof tb, v, yInt);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(pr.p0.x - 5 * s - ts.x, y - fh * 0.5f), txt, tb);
    }
    // X ticks
    double xstep = niceStep((float)((xmax - xmin) / 4.0));
    if (xInt && xstep < 1) xstep = 1;
    for (double v = ceil(xmin / xstep) * xstep; v <= xmax + 1e-9; v += xstep) {
        float x = pr.at((float)v, pr.ymin).x;
        dl->AddLine(ImVec2(x, pr.p0.y), ImVec2(x, pr.p1.y), grid);
        fmtTick(tb, sizeof tb, v, xInt);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, pr.p0.x, pr.p1.x - ts.x),
                           pr.p1.y + 2 * s), txt, tb);
    }
    dl->AddRect(pr.p0, pr.p1, axis);
    // axis titles: quantity + unit (mandatory)
    dl->AddText(ImVec2(org.x, org.y), txt, yLabel ? yLabel : "y");
    {
        const char* xl = xLabel ? xLabel : "x";
        ImVec2 ts = ImGui::CalcTextSize(xl);
        dl->AddText(ImVec2((pr.p0.x + pr.p1.x - ts.x) * 0.5f, pr.p1.y + 2 * s + fh), txt, xl);
    }
    ImGui::Dummy(ImVec2(wid, height + marginT + marginB));
    pr.ok = true;
    return pr;
}

// The A/B legend, on its own row BENEATH the plot. It used to be a filled box
// pinned inside the plot rect, top right, where it covered the data it was
// explaining: on a narrow panel it took roughly half the plot width, and any
// distribution with its peak on the right disappeared behind it. (The union
// range default makes that rarer; it does not make a box that hides data
// right.) One line, left to right, like everything else in these panels, at
// the cost of one text row - which the layouts reserve whether or not compare
// is on, so turning it on never moves the plot out from under the cursor.
//
// The swatches show a SAMPLE of each stroke - solid for A, dashed for B -
// because a text-only "A" and "B" leaves the reader matching words to lines.
// A is the neutral ink every plot draws in; B carries the blue tint it is
// actually drawn with (a mono series would otherwise give both sides the same
// grey). Names are elided from the FRONT: the tail is what tells two captures
// apart. They come from abDocLabel, so two stacks of one series do not both
// read "frame_000.npy".
static float abLegendSw()  { return 24 * app.uiScale; }
static float abLegendGap() { return 6 * app.uiScale; }
static std::string abLegendText(const char* side, const std::string& name) {
    return std::string(side) + ": " + elideFront(name, 26);
}
static float abLegendEntryW(const char* side, const std::string& name, bool stale) {
    float w = abLegendSw() + abLegendGap() +
              ImGui::CalcTextSize(abLegendText(side, name).c_str()).x;
    if (stale) w += abLegendGap() + ImGui::CalcTextSize(AB_STALE_TOKEN).x;
    return w;
}
// Too narrow for both entries side by side: stack them rather than clip one.
static bool abLegendOneLine(const std::string& aName, const std::string& bName, bool bStale) {
    return abLegendEntryW("A", aName, false) + 18 * app.uiScale +
           abLegendEntryW("B", bName, bStale) <= ImGui::GetContentRegionAvail().x;
}
static float abLegendH(const std::string& aName, const std::string& bName, bool bStale) {
    return ImGui::GetTextLineHeightWithSpacing() *
           (abLegendOneLine(aName, bName, bStale) ? 1.0f : 2.0f);
}
static void drawABLegendRow(const std::string& aName, const std::string& bName,
                            bool bStale = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = app.uiScale, fh = ImGui::GetFontSize();
    const float sw = abLegendSw(), gap = abLegendGap(), sep = 18 * s;
    const ImU32 ink = IM_COL32(215, 222, 228, 255);
    const ImU32 inkB = IM_COL32(120, 190, 255, 255);
    const bool one = abLegendOneLine(aName, bName, bStale);
    const float lineAdvance = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const std::string la = abLegendText("A", aName), lb = abLegendText("B", bName);
    dl->AddLine(ImVec2(p.x, p.y + fh * 0.5f), ImVec2(p.x + sw, p.y + fh * 0.5f), ink, 1.6f);
    dl->AddText(ImVec2(p.x + sw + gap, p.y), ink, la.c_str());
    const float bx = one ? p.x + abLegendEntryW("A", aName, false) + sep : p.x;
    const float by = one ? p.y : p.y + lineAdvance;
    ImVec2 dash[2] = { ImVec2(bx, by + fh * 0.5f), ImVec2(bx + sw, by + fh * 0.5f) };
    addDashedPolyline(dl, dash, 2, inkB, 1.6f, 4 * s, 3 * s);
    dl->AddText(ImVec2(bx + sw + gap, by), ink, lb.c_str());
    if (bStale)
        dl->AddText(ImVec2(bx + sw + gap + ImGui::CalcTextSize(lb.c_str()).x + gap, by),
                    AB_AMBER32, AB_STALE_TOKEN);
    // consume exactly what abLegendH promised (ImGui adds one ItemSpacing after)
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x,
                        one ? ImGui::GetTextLineHeight()
                            : ImGui::GetTextLineHeight() + lineAdvance));
}

// The heading strip over one half of a side-by-side pair. Neutral colour: which
// side you are looking at is the message, not which channel.
static void drawABBand(const char* side, const std::string& name, bool stale = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = app.uiScale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetTextLineHeight() + 4 * s;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(52, 58, 66, 255), 3 * s);
    std::string t = std::string(side) + "   " + elideFront(name, 34);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddText(ImVec2(p.x + 6 * s, p.y + 2 * s), IM_COL32(225, 230, 236, 255), t.c_str());
    if (stale)
        dl->AddText(ImVec2(p.x + 6 * s + ImGui::CalcTextSize(t.c_str()).x + 8 * s,
                           p.y + 2 * s), AB_AMBER32, AB_STALE_TOKEN);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, h + 2 * s));
}

// Why the side-by-side layout fell back to an overlay. Two things it has to get
// right and did not: it must WRAP (Text does not, and at exactly the width that
// triggers it the line was cut mid-word - "panel narrower than 480 px: overlaid
// ins..."), and it must quote the LOGICAL threshold. It used to print the
// DPI-scaled minSide, so a 150% display was told to widen past 480 px while the
// code compares against 320 logical ones - a number the user cannot act on.
static const char* AB_NARROW_MSG =
    "panel narrower than 320 px (logical width, before display scaling): "
    "overlaid instead of side by side";
static const float AB_MIN_SIDE = 320.0f;
// The height it will take at the current width, so the plot layout can reserve
// exactly what the wrapped text uses. A note that appears and pushes the plot
// down is the same defect as the browser's scrub bar and the preview row.
static float abNarrowNoteH() {
    return ImGui::CalcTextSize(AB_NARROW_MSG, nullptr, false,
                               ImGui::GetContentRegionAvail().x).y +
           ImGui::GetStyle().ItemSpacing.y;
}
static void abNarrowNote() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(AB_NARROW_MSG);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A and B series correspond BY NAME (R <-> R), never by index: the canon says
// CFA planes are never mixed, and ch0 is not R. -1 = this series exists on one
// side only, and is drawn as such.
static int abSeriesMatch(const char* const* names, int n, const char* want) {
    if (!want) return -1;
    for (int i = 0; i < n; i++)
        if (names[i] && strcmp(names[i], want) == 0) return i;
    return -1;
}

// L2 host service: line plot for analyzer curve output (the only curve UI —
// plugins never draw; they emit_series and this renders every curve the same way)
static void drawAnalysisPlots() {
    const auto& S = app.ana.series;
    if (S.empty()) return;
    std::vector<std::string> names;
    for (const auto& s : S) {
        bool seen = false;
        for (const auto& n : names) if (n == s.name) { seen = true; break; }
        if (!seen) names.push_back(s.name);
    }
    for (const auto& nm : names) {
        float xmin = FLT_MAX, xmax = -FLT_MAX, ymin = FLT_MAX, ymax = -FLT_MAX;
        const App::AnalysisState::Series* first = nullptr;
        for (const auto& s : S) {
            if (s.name != nm || s.ys.empty()) continue;
            if (!first) first = &s;
            for (size_t i = 0; i < s.ys.size(); i++) {
                float xv = s.xs.empty() ? (float)i : s.xs[i];
                float yv = s.ys[i];
                if (!std::isfinite(xv) || !std::isfinite(yv)) continue;
                xmin = std::min(xmin, xv); xmax = std::max(xmax, xv);
                ymin = std::min(ymin, yv); ymax = std::max(ymax, yv);
            }
        }
        if (!first || xmin > xmax || ymin > ymax) continue;
        // the user's reference line is part of the picture: keep it in range
        // instead of silently clipping the one line they asked to see
        if (app.anaRefOn && std::isfinite(app.anaRef)) {
            ymin = std::min(ymin, app.anaRef);
            ymax = std::max(ymax, app.anaRef);
        }
        ImGui::TextDisabled("%s", nm.c_str());
        bool xInt = true;                     // integer axis when the plugin sends no x
        for (const auto& s : S)
            if (s.name == nm && !s.xs.empty()) { xInt = false; break; }
        PlotRect pr = beginPlot(first->xLabel.empty() ? (xInt ? "sample index" : "x")
                                                     : first->xLabel.c_str(),
                                first->yLabel.empty() ? nm.c_str() : first->yLabel.c_str(),
                                xmin, xmax, ymin, ymax, xInt, false, 140.0f * app.uiScale);
        if (!pr.ok) continue;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pr.p0, pr.p1, true);
        for (const auto& s : S) {
            if (s.name != nm || s.ys.size() < 2) continue;
            ImU32 col = s.colorIdx >= 0 ? ANN_COLORS[s.colorIdx & 7]
                                        : IM_COL32(200, 205, 210, 255);
            ImVec2 prev(0, 0); bool has = false;
            for (size_t i = 0; i < s.ys.size(); i++) {
                float xv = s.xs.empty() ? (float)i : s.xs[i];
                float yv = s.ys[i];
                if (!std::isfinite(xv) || !std::isfinite(yv)) { has = false; continue; }
                ImVec2 pt = pr.at(xv, yv);
                if (has) dl->AddLine(prev, pt, col, 1.5f);
                prev = pt; has = true;
            }
        }
        // reference line, dashed: data and reference must never read as one
        // curve. Amber, like the frame markers on the temporal plots.
        if (app.anaRefOn && std::isfinite(app.anaRef) &&
            app.anaRef >= pr.ymin && app.anaRef <= pr.ymax) {
            float y = pr.at(pr.xmin, app.anaRef).y;
            ImU32 rc = IM_COL32(255, 184, 77, 220);
            float dash = 6.0f * app.uiScale;
            ImVec2 seg[2] = { ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y) };
            addDashedPolyline(dl, seg, 2, rc, 1.0f, dash, dash);
            char rb[32];
            snprintf(rb, sizeof rb, "ref %.4g", app.anaRef);
            ImVec2 rts = ImGui::CalcTextSize(rb);
            dl->AddText(ImVec2(pr.p1.x - rts.x - 4 * app.uiScale,
                               y - rts.y - 2 * app.uiScale), rc, rb);
        }
        // hover readout: crosshair snapped to the nearest sample, values for
        // every curve in a tooltip. Nobody should eyeball numbers off a plot
        // in a measurement tool - the curve is the shape, the tooltip is the
        // number. (A tooltip, so the layout never moves.)
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (ImGui::IsWindowHovered() && mp.x >= pr.p0.x && mp.x <= pr.p1.x &&
            mp.y >= pr.p0.y && mp.y <= pr.p1.y) {
            float fx = pr.xmin + (mp.x - pr.p0.x) / (pr.p1.x - pr.p0.x) * (pr.xmax - pr.xmin);
            int best = -1;
            float bd = FLT_MAX;
            for (size_t i = 0; i < first->ys.size(); i++) {
                float xv = first->xs.empty() ? (float)i : first->xs[i];
                float d = fabsf(xv - fx);
                if (d < bd) { bd = d; best = (int)i; }
            }
            if (best >= 0) {
                float sx = first->xs.empty() ? (float)best : first->xs[best];
                float lx = pr.at(sx, pr.ymin).x;
                dl->AddLine(ImVec2(lx, pr.p0.y), ImVec2(lx, pr.p1.y),
                            IM_COL32(150, 160, 170, 120));
                ImGui::BeginTooltip();
                // quantity + unit on the readout too: the tooltip may end up
                // in a screenshot without the axes that explain it
                ImGui::TextDisabled("%s = %.5g   (y: %s)",
                                    first->xLabel.empty() ? "x" : first->xLabel.c_str(), sx,
                                    first->yLabel.empty() ? nm.c_str() : first->yLabel.c_str());
                for (const auto& s : S) {
                    if (s.name != nm || (size_t)best >= s.ys.size()) continue;
                    ImU32 col = s.colorIdx >= 0 ? ANN_COLORS[s.colorIdx & 7]
                                                : IM_COL32(200, 205, 210, 255);
                    const char* lbl = s.col >= 0 && s.col < (int)app.ana.cols.size()
                                    ? app.ana.cols[s.col].c_str() : "?";
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col),
                                       "%s  %.5g", lbl, s.ys[best]);
                }
                ImGui::EndTooltip();
            }
        }
        dl->PopClipRect();
    }
}

// Numeric cell: fixed column width + right alignment, so digits keep their
// position while flipping through frames (values must be comparable at a glance).
static void textNum(const char* fmt, double v) {
    char buf[64];
    snprintf(buf, sizeof buf, fmt, v);
    float w = ImGui::CalcTextSize(buf).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    ImGui::TextUnformatted(buf);
}
static void textNumStr(const std::string& s) {
    float w = ImGui::CalcTextSize(s.c_str()).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w));
    ImGui::TextUnformatted(s.c_str());
}
// content width for a full-precision number; the table adds CellPadding itself
static float numColW() { return ImGui::CalcTextSize("-0.00000e+00").x; }

static void drawInspector() {
    ImageDoc* im = cur();
    ImGui::SeparatorText("Pixel");
    // Layout must not depend on hover state: the section always occupies the same
    // number of rows, otherwise everything below jumps as the cursor enters/leaves.
    {
        // hover may sit outside A when the cursor is over a larger B pane
        bool live = im && app.hoverX >= 0 && app.hoverX < im->w && app.hoverY < im->h;
        if (live && im->cfa)
            ImGui::Text("(%d, %d)  [%s]", app.hoverX, app.hoverY,
                        CFA_CH_NAMES[cfaChannelAt(*im, app.hoverX, app.hoverY)]);
        else if (live)
            ImGui::Text("(%d, %d)", app.hoverX, app.hoverY);
        else
            ImGui::TextDisabled("(-, -)  hover the image");
        static const char* LB1[] = { "V" };
        static const char* LB2[] = { "C0", "C1" };            // 2ch is usually UV/complex, not RG
        static const char* LB3[] = { "R", "G", "B", "A" };
        int nch = im ? im->ch : 1;
        const char** lb = nch == 1 ? LB1 : nch == 2 ? LB2 : LB3;
        float inv = im ? 1.0f / std::max(effWhite(*im) - effBlack(*im), 1e-20f) : 1.0f;
        // With compare on, the same pixel in B (and A-B) is the number the eye
        // cannot read off a wipe: two extra columns, present whenever compare is.
        ImageDoc* b = cmpB();
        bool bHere = b && app.hoverX >= 0 && app.hoverX < b->w && app.hoverY < b->h;
        // "DN" is only true for integer sensor data; a float .npy may hold
        // reflectance, e-, or anything else, so do not assert a unit for it.
        auto isInt = [](const std::string& t) {
            return !t.empty() && (t[0] == 'u' || t[0] == 'i' || t == "bool");
        };
        auto unitOf = [&](const std::string& t) { return isInt(t) ? " [DN]" : ""; };
        std::string tA = im ? im->dtype : "f32";
        // A-B in the wider of the two types: u16 minus f32 rounded to whole DN
        // would hide exactly the sub-DN residual you opened the compare for
        std::string tDiff = (b && !isInt(b->dtype)) ? b->dtype : tA;
        std::string hRaw = std::string(b ? "A" : "raw") + unitOf(tA);
        std::string hB = std::string("B") + (b ? unitOf(b->dtype) : "");
        std::string hDiff = std::string("A-B") + unitOf(tDiff);
        // fixed widths: the numbers must not shift as the cursor moves
        if (ImGui::BeginTable("px", b ? 5 : 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
            ImGui::TableSetupColumn(hRaw.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("norm [-]", ImGuiTableColumnFlags_WidthFixed, numColW());
            if (b) {
                ImGui::TableSetupColumn(hB.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
                ImGui::TableSetupColumn(hDiff.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
            }
            ImGui::TableHeadersRow();
            for (int c = 0; c < nch; c++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c < 4 ? lb[std::min(c, 3)] : "?");
                float v = 0;
                if (live) {
                    v = im->sample(app.hoverX, app.hoverY, c);
                    ImGui::TableNextColumn(); textNumStr(fmtVal(v, im->dtype));
                    ImGui::TableNextColumn(); textNum("%.4f", (v - effBlack(*im)) * inv);
                } else {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                }
                if (b) {
                    if (bHere && c < b->ch) {
                        float bv = b->sample(app.hoverX, app.hoverY, c);
                        ImGui::TableNextColumn(); textNumStr(fmtVal(bv, b->dtype));
                        ImGui::TableNextColumn(); textNumStr(fmtVal(v - bv, tDiff));
                    } else {
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    }
                }
            }
            ImGui::EndTable();
        }
        if (b && im) {
            ImGui::TextDisabled("A: %s", abDocLabel(im).c_str());
            ImGui::TextDisabled("B: %s", abDocLabel(b).c_str());
            if (b->w != im->w || b->h != im->h)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "size differs: B is %dx%d",
                                   b->w, b->h);
            // Two images auto-ranged to their own min/max look different even when
            // the pixels are identical - the fastest way to a wrong conclusion.
            if (!app.linkRange) {
                // Sharing is the default because two images stretched
                // differently cannot be compared: the difference you see would
                // be the stretch. When it is off, SAY the numbers differ - that
                // is the case where a reading can mislead.
                ImGui::SetNextItemWidth(-1);
                static const char* AB_RANGE_ITEMS[3] = {
                    "A/B: each keeps its own", "A/B: B uses A's range",
                    "A/B: auto over both (union)" };
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##abrange", &app.compareRangeMode,
                                 AB_RANGE_ITEMS, 3)) {
                    b->texDirty = true; im->texDirty = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "How the two sides are STRETCHED while comparing. Display only -\n"
                        "both keep their own numbers and get them back when compare ends.\n"
                        "  auto over both (union), the default: both re-fit to min/max\n"
                        "    across A and B, so neither side clips and neither is\n"
                        "    favoured. Two stacks at different exposures need this - B\n"
                        "    against A's range alone saturates into a single bin.\n"
                        "  B uses A's range: one range, A's, and it does not move while\n"
                        "    you step A. Right when B really is measured against A.\n"
                        "  each keeps its own: comparing SHAPES at wildly different\n"
                        "    exposures - the one case where the two are not directly\n"
                        "    comparable, and it says so.");
                if (app.compareRangeMode == 0 &&
                    (b->black != im->black || b->white != im->white)) {
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "display range differs");
                    ImGui::TextDisabled("A %s-%s / B %s-%s", fmtVal(im->black, im->dtype).c_str(),
                                        fmtVal(im->white, im->dtype).c_str(),
                                        fmtVal(b->black, b->dtype).c_str(),
                                        fmtVal(b->white, b->dtype).c_str());
                }
            }
            if (b->dtype != im->dtype)
                ImGui::TextDisabled("dtype differs: A %s / B %s", im->dtype.c_str(), b->dtype.c_str());
            if (app.compareMode == App::CmpFlip) {
                ImGui::Checkbox("auto blink", &app.flipAuto);
                ImGui::SetNextItemWidth(numColW() * 2);
                // round numbers: 0.5 s per side, not 0.4736
                if (ImGui::InputFloat("seconds per side", &app.flipPeriod, 0.1f, 0.5f, "%.2f"))
                    app.flipPeriod = std::clamp(app.flipPeriod, 0.05f, 10.0f);
                ImGui::TextDisabled("showing %s   (Space or B toggles)",
                                    app.flipShowB ? "B" : "A");
            }
            if (app.compareMode == App::CmpDiff) {
                bool intType = !im->dtype.empty() && (im->dtype[0] == 'u' || im->dtype[0] == 'i');
                bool autoGain = app.diffGain <= 0;
                if (ImGui::Checkbox("auto scale", &autoGain))
                    app.diffGain = autoGain ? 0 : app.diff.gain;
                if (!autoGain) {
                    // shadow buffer: each keystroke would otherwise rebuild the
                    // whole difference image (107 ms at 12 Mpx)
                    static float g = 0;
                    static bool gEditing = false;
                    if (!gEditing) g = app.diffGain > 0 ? app.diffGain : app.diff.gain;
                    ImGui::SetNextItemWidth(numColW() * 2);
                    // step on round numbers, as everywhere else
                    ImGui::InputFloat(intType ? "full scale [DN]" : "full scale", &g,
                                      niceCeil(g) * 0.1f, niceCeil(g), "%g");
                    gEditing = ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivatedAfterEdit()) app.diffGain = std::max(g, 1e-9f);
                } else {
                    ImGui::TextDisabled("full scale +-%g%s (99.9%% of |A-B|)",
                                        app.diff.gain, intType ? " DN" : "");
                }
                ImGui::Checkbox("magnitude only |A-B|", &app.diffAbs);
            }
        }
    }

    ImGui::SeparatorText("Image");
    if (im) {
        // two short lines instead of one long one: the merged line was clipped in
        // a ~300px dock (Text never wraps and there is no horizontal scrollbar)
        if (im->seqId != 0) {
            std::vector<int> fr = framesOfSeq(im->seqId);
            int pos = 0;
            for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
            ImGui::Text("%dx%d  %dch  %s   frame %d/%d", im->w, im->h, im->ch, im->dtype.c_str(),
                        pos + 1, (int)fr.size());
        } else {
            ImGui::Text("%dx%d  %dch  %s", im->w, im->h, im->ch, im->dtype.c_str());
        }
        ImGui::TextDisabled("min %s / max %s", fmtVal(im->vmin, im->dtype).c_str(),
                            fmtVal(im->vmax, im->dtype).c_str());
        if (!im->note.empty()) ImGui::TextWrapped("%s", im->note.c_str());
        if (im->ch == 1) {
            // interpretation axis: change what the 1ch data means AFTER opening
            const char* modes[3] = { "Gray", "Bayer", "Quad Bayer" };
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            if (ImGui::BeginCombo("Interpret", modes[std::clamp(im->cfa, 0, 2)])) {
                for (int i = 0; i < 3; i++)
                    if (ImGui::Selectable(modes[i], i == im->cfa) && i != im->cfa) {
                        im->cfa = i;
                        im->texDirty = true;
                    }
                ImGui::EndCombo();
            }
            if (im->cfa) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##cfapat", CFA_PATTERNS[im->cfaPattern & 3])) {
                    for (int i = 0; i < 4; i++)
                        if (ImGui::Selectable(CFA_PATTERNS[i], i == im->cfaPattern)) {
                            im->cfaPattern = i;
                            im->texDirty = true;
                        }
                    ImGui::EndCombo();
                }
            }
        }
        if (im->cfa) {
            if (ImGui::Checkbox("Colorize CFA pattern", &im->cfaColorize)) im->texDirty = true;
        }
        if (im->rawDtype >= 0 && ImGui::Button("Reinterpret raw...")) {
            openRawDialogFor(im->path);                 // prefill size guesses from the file
            if (rawDlg.open) {                          // only if the file was readable
            rawDlg.dtype = im->rawDtype;
            rawDlg.interp = im->rawInterp;
            if (RAW_INTERP_CH[rawDlg.interp] == 1)      // 1ch family: honor current interpretation
                rawDlg.interp = im->cfa == 2 ? RI_QUAD : im->cfa == 1 ? RI_BAYER : RI_GRAY;
            rawDlg.w = im->srcW > 0 ? im->srcW : im->w;
            rawDlg.h = im->srcH > 0 ? im->srcH : im->h;
            rawDlg.offset = im->rawOffset;
            rawDlg.littleEndian = im->rawLE;
            rawDlg.cfaPattern = im->cfaPattern & 3;
            rawDlg.cropOn = isCropped(*im);
            rawDlg.cropX = im->cropX; rawDlg.cropY = im->cropY;
            rawDlg.cropW = im->w; rawDlg.cropH = im->h;
            rawDlg.replaceIdx = app.current;
            rawGuessDims(rawDlg);
            }
        }
        {
            App::Ann* selAnn = findAnn(app.selectedAnn);
            bool roiSel = selAnn && selAnn->type == 0;
            if (isCropped(*im))
                ImGui::TextDisabled("crop %dx%d @ (%d,%d) of %dx%d",
                                    im->w, im->h, im->cropX, im->cropY, im->srcW, im->srcH);
            if (roiSel && ImGui::Button("Crop to selected ROI")) cropCurrentToSelectedRoi();
            if (isCropped(*im)) {
                if (roiSel) ImGui::SameLine();
                if (ImGui::Button("Restore full")) restoreFull();
            }
        }
        if (im->ch == 1 && !plugin_host::displays().empty()) {
            if (im->cfa && im->cfaColorize) {
                ImGui::TextDisabled("colormap disabled while CFA colorize is on");
            } else {
                const char* curName = (im->displayLut >= 0 &&
                                       im->displayLut < (int)plugin_host::displays().size())
                    ? plugin_host::displays()[im->displayLut].name.c_str() : "Gray";
                if (ImGui::BeginCombo("Colormap", curName)) {
                    if (ImGui::Selectable("Gray", im->displayLut < 0)) { im->displayLut = -1; im->texDirty = true; }
                    for (int i = 0; i < (int)plugin_host::displays().size(); i++)
                        if (ImGui::Selectable(plugin_host::displays()[i].name.c_str(), im->displayLut == i)) {
                            im->displayLut = i; im->texDirty = true;
                        }
                    ImGui::EndCombo();
                }
            }
        }

        ImGui::SeparatorText("Range (black / white)");
        {   // ONE mode control. It used to be a "link across all images"
            // checkbox PLUS an on-switch combo whose third entry re-implemented
            // linking destructively (copying the range into each image instead
            // of overlaying one) - two mechanisms for the same intent, spotted
            // as such immediately. Modes:
            //   0 auto per frame - every frame re-fits to its own min..max
            //   1 per stack      - frames of a stack share the reference range
            //   2 linked         - one display range overlays every open image
            int mode = app.linkRange ? 2 : (app.rangeScope == 0 ? 0 : 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##rangemode", &mode,
                             "Auto per frame\0Per stack (default)\0Linked across all images\0")) {
                bool wasLinked = app.linkRange;
                app.linkRange = mode == 2;
                app.rangeScope = mode == 0 ? 0 : 1;
                if (app.linkRange && !wasLinked) {      // seed from what is on screen
                    app.linkBlack = im->black; app.linkWhite = im->white;
                }
                if (app.linkRange != wasLinked) markAllTexDirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Auto per frame: every frame re-fits to its own min..max\n"
                                  "Per stack: frames of one stack share the reference\n"
                                  "  frame's range, so they compare directly\n"
                                  "Linked: one display range for every open image - display\n"
                                  "  only, each image keeps its own and gets it back on unlink");
        }
        // EnterReturnsTrue is not allowed on InputScalar-family widgets (asserts in
        // debug builds); edit a shadow buffer and commit on deactivate-after-edit.
        static float bwEdit[2];
        static bool bwEditing = false;
        if (!bwEditing) { bwEdit[0] = effBlack(*im); bwEdit[1] = effWhite(*im); }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputFloat2("##bw", bwEdit, "%.5g");
        bwEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && bwEdit[1] > bwEdit[0])
            setRange(*im, bwEdit[0], bwEdit[1]);
        {   // four equal buttons measured from the panel, never wider than it
            float bw = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
            bw = std::max(bw, ImGui::GetFontSize() * 2.0f);
            if (ImGui::Button("Auto", ImVec2(bw, 0))) {
                float lo = im->vmin, hi = im->vmax;
                if (app.linkRange)                       // fit every image, not just this one
                    for (const auto& d : app.images) { lo = std::min(lo, d->vmin); hi = std::max(hi, d->vmax); }
                setRange(*im, lo, hi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(app.linkRange ? "min/max over all open images" : "min/max of this image");
            ImGui::SameLine();
            if (ImGui::Button("0-1", ImVec2(bw, 0))) setRange(*im, 0, 1);
            ImGui::SameLine();
            if (ImGui::Button("0-255", ImVec2(bw, 0))) setRange(*im, 0, 255);
            ImGui::SameLine();
            if (ImGui::Button("0-65535", ImVec2(bw, 0))) setRange(*im, 0, 65535);
        }

        int g = app.dispGamma > 1.5f ? 1 : 0;
        ImGui::TextUnformatted("gamma"); ImGui::SameLine();
        if (ImGui::RadioButton("1.0", g == 0)) { app.dispGamma = 1.0f; markAllTexDirty(); } ImGui::SameLine();
        if (ImGui::RadioButton("2.2", g == 1)) { app.dispGamma = 2.2f; markAllTexDirty(); }
        {   // only share the row if the checkbox actually fits (fails at 200% DPI)
            const ImGuiStyle& st = ImGui::GetStyle();
            float need = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                         ImGui::CalcTextSize("grid (G)").x + st.ItemSpacing.x;
            float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x -
                         ImGui::GetCursorStartPos().x;
            if (ImGui::GetContentRegionAvail().x - used >= need) ImGui::SameLine();
            ImGui::Checkbox("grid (G)", &app.showGrid);
        }
    } else {
        ImGui::TextDisabled("no image");
    }

}

// The unit a PIXEL VALUE is quoted in, anywhere one is labelled: DN, always.
// A float .npy is still a digital number - f32 is how the value is STORED, not
// what it measures, and an axis reading "pixel value (f32)" states a storage
// class where a unit belongs. The per-frame TSV has always written [DN]
// unconditionally. Lives here, above the first plot, because every axis label
// below goes through it; the dtype argument is kept so the rule has one place
// to change if a file ever carries a real unit.
// (Statements ABOUT the file - the Inspector's "1920x1080 1ch f32", the browse
// listing's shape column, a dtype-mismatch warning - are not this: they say how
// the data is stored, which is exactly what they mean.)
static std::string abValueUnit(const std::string&) {
    return "DN";
}

// ...and WHOSE range that value is plotted against. It has to name the range
// actually in force: with the union default the x axis spans A and B together,
// and an axis that still says "A's black-white range" is simply telling the
// reader something untrue about the picture in front of them.
// Mode 0 is not an exception. Each side keeps its own STRETCH, but the two
// histograms are still binned on A's black/white - one bin grid, or the curves
// are not comparable at all (see recomputeHistogramIfNeeded) - so naming A's
// range there stays true.
static const char* abRangeSaid(bool haveB) {
    if (!haveB) return "black-white range";
    return app.compareRangeMode == 2 ? "A and B combined range" : "A's black-white range";
}

// The histogram's x axis label, built in one place so --range-selftest can
// print the exact string the panel draws.
static std::string abHistXLabel(const ImageDoc* a, const ImageDoc* b) {
    char xl[192];
    const std::string xu = abValueUnit(a->dtype);
    const char* xr = abRangeSaid(b != nullptr);
    if (b && b->dtype != a->dtype)
        snprintf(xl, sizeof xl, "pixel value (%s, %s - bins both sides)"
                                "  -  A %s / B %s, DTYPE MISMATCH",
                 xu.c_str(), xr, a->dtype.c_str(), b->dtype.c_str());
    else
        snprintf(xl, sizeof xl, "pixel value (%s, %s)", xu.c_str(), xr);
    return xl;
}

static void drawPanelHistogram() {
    ImageDoc* im = cur();
    if (im && im->w > 0 && im->h > 0) {
        ImageDoc* Bim = abStatsB();
        recomputeHistogramIfNeeded(im, app.hist[0]);
        // B is binned on A's black/white: one x axis, one bin grid, or the two
        // curves are not comparable. Sizes may differ freely - the y axis below
        // normalises that away. Skipped while frames are being stepped: an
        // empty slot is always filled, a filled one waits for the stepping to
        // stop and is labelled stale until then.
        if (Bim && (!abStepBusy() || app.hist[1].uid == 0))
            recomputeHistogramIfNeeded(Bim, app.hist[1], effBlack(*im), effWhite(*im));
        const App::HistState& H = app.hist[0];
        const App::HistState& HB = app.hist[1];
        const bool bStale = Bim && HB.uid != Bim->uid;
        const std::string bLabel = Bim ? abDocLabel(Bim) : std::string();
        ImGui::Text("Statistics");
        ImGui::SameLine();
        // with a B on screen, an unlabelled table of numbers is ambiguous:
        // say whose numbers these are
        if (Bim) ImGui::TextDisabled("A: %s   (%s)", elideFront(abDocLabel(im), 26).c_str(),
                                     H.roiUsed ? "selected ROI" : "whole image");
        else     ImGui::TextDisabled("(%s)", H.roiUsed ? "selected ROI" : "whole image");
        ImGui::Separator();
        // fixed widths + right-aligned numbers: columns must not reflow while
        // stepping through frames, otherwise values are impossible to compare
        if (ImGui::BeginTable("quickstats", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
            ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("std", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableSetupColumn("var", ImGuiTableColumnFlags_WidthFixed, numColW());
            ImGui::TableHeadersRow();
            for (int s = 0; s < H.nSeries; s++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", H.seriesNames[s]);
                ImGui::TableNextColumn(); textNum("%.6g", H.mean[s]);
                ImGui::TableNextColumn(); textNum("%.6g", H.sd[s]);
                ImGui::TableNextColumn(); textNum("%.6g", H.sd[s] * H.sd[s]);
            }
            ImGui::EndTable();
        }

        // SeparatorText spans to the work rect edge, so a plain SameLine() lands
        // outside it and the widget is neither drawn nor clickable: use an
        // absolute offset instead.
        ImGui::SeparatorText("Histogram");
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            float cbW = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("log").x;
            ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - cbW, 0.0f));
            ImGui::Checkbox("log##hist", &app.histLog);
        }
        static const ImU32 CFA_COLS[4] = { IM_COL32(255, 92, 92, 170), IM_COL32(120, 230, 120, 170),
                                           IM_COL32(60, 180, 140, 170), IM_COL32(92, 155, 255, 170) };
        static const ImU32 RGB_COLS[3] = { IM_COL32(255, 92, 92, 150), IM_COL32(79, 221, 107, 150),
                                           IM_COL32(92, 155, 255, 150) };
        const ImU32 ODD_COL = IM_COL32(185, 192, 200, 190);   // a series only one side has
        bool cfaHist = im->ch == 1 && im->cfa != 0;

        // ---- plane selector: four CFA planes times two sides is eight curves
        // on one axis. Same control as the ROI table's, and it filters BOTH
        // sides, so a comparison is always plane against the same plane.
        app.histPlane = std::clamp(app.histPlane, -1, std::max(H.nSeries - 1, -1));
        if (H.nSeries > 1) {
            const char* selName = app.histPlane < 0 ? "all" : H.seriesNames[app.histPlane];
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
            if (ImGui::BeginCombo("plane##hist", selName)) {
                if (ImGui::Selectable("all", app.histPlane < 0)) app.histPlane = -1;
                for (int s = 0; s < H.nSeries; s++)
                    if (ImGui::Selectable(H.seriesNames[s], app.histPlane == s)) app.histPlane = s;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Which plane the curves show. The bins are computed for\n"
                                  "all of them either way - this only chooses what is drawn.");
        }
        auto drawn = [&](int s) { return app.histPlane < 0 || app.histPlane == s; };
        int nDrawn = 0;
        for (int s = 0; s < H.nSeries; s++) if (drawn(s)) nDrawn++;

        // ---- y axis. Without a B: pixel counts, exactly as before. With a B:
        // a share of each side's OWN sampled pixels, because two images of
        // different size put incomparable bar heights on one axis.
        const bool norm = Bim != nullptr;
        const double scA = 100.0 / (double)std::max<size_t>(H.sampled, 1);
        const double scB = 100.0 / (double)std::max<size_t>(HB.sampled, 1);
        double yTop = 0;
        for (int s = 0; s < H.nSeries; s++) {
            if (!drawn(s)) continue;
            for (int b = 0; b < 256; b++)
                yTop = std::max(yTop, H.bins[s][b] * (norm ? scA : 1.0));
        }
        if (norm)
            for (int s = 0; s < HB.nSeries; s++) {
                int a = abSeriesMatch(H.seriesNames, H.nSeries, HB.seriesNames[s]);
                if (a >= 0 ? !drawn(a) : app.histPlane >= 0) continue;
                for (int b = 0; b < 256; b++) yTop = std::max(yTop, HB.bins[s][b] * scB);
            }
        if (!(yTop > 0)) yTop = 1;
        const double logTop = log1p(yTop);
        char yl[96];
        if (norm)
            snprintf(yl, sizeof yl, app.histLog ? "share of sampled px [%%] (log, max %.3g %%)"
                                                : "share of sampled px [%%] (max %.3g %%)", yTop);
        else
            snprintf(yl, sizeof yl, app.histLog ? "pixel count (log, max %u)"
                                                : "pixel count (max %u)", H.maxBin);
        const std::string xlBuf = abHistXLabel(im, Bim);
        const char* xl = xlBuf.c_str();

        // one curve, one way to draw it: filled bars, solid outline, dashed
        // outline. The staircase is built once and reused by all three.
        auto plotSeries = [&](const PlotRect& pr, const uint32_t bins[256], double sc,
                              ImU32 col, int style) {          // 0 fill, 1 solid, 2 dashed
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float pw = pr.p1.x - pr.p0.x, ph = pr.p1.y - pr.p0.y;
            auto yOf = [&](uint32_t v) {
                double u = v * sc;
                double f = app.histLog ? (logTop > 0 ? log1p(u) / logTop : 0.0) : u / yTop;
                return pr.p1.y - (float)std::clamp(f, 0.0, 1.0) * ph;
            };
            if (style == 0) {
                for (int b = 0; b < 256; b++) {
                    if (!bins[b]) continue;
                    float bx0 = pr.p0.x + (float)b / 256.0f * pw;
                    dl->AddRectFilled(ImVec2(bx0, yOf(bins[b])),
                                      ImVec2(bx0 + pw / 256.0f + 0.5f, pr.p1.y), col);
                }
                return;
            }
            std::vector<ImVec2> pts;
            pts.reserve(512);
            for (int b = 0; b < 256; b++) {
                float bx0 = pr.p0.x + (float)b / 256.0f * pw;
                float bx1 = pr.p0.x + (float)(b + 1) / 256.0f * pw;
                float y = yOf(bins[b]);
                pts.push_back(ImVec2(bx0, y));
                pts.push_back(ImVec2(bx1, y));
            }
            if (style == 1) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.4f);
            else addDashedPolyline(dl, pts.data(), (int)pts.size(), col, 1.4f,
                                   5 * app.uiScale, 4 * app.uiScale);
        };
        // Four CFA planes (or three RGB ones) as eight filled areas is unreadable,
        // so at three drawn curves or more BOTH sides become outlines. Below that
        // A keeps its fill and only B is an outline.
        const bool outlineA = Bim && nDrawn >= 3;
        auto drawAll = [&](const PlotRect& pr, bool wantA, bool wantB) {
            if (!pr.ok) return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            if (wantA)
                for (int s = 0; s < H.nSeries; s++) {
                    if (!drawn(s)) continue;
                    ImU32 col = cfaHist ? CFA_COLS[s]
                              : (H.nSeries == 1 ? IM_COL32(200, 205, 210, 200) : RGB_COLS[s]);
                    plotSeries(pr, H.bins[s], norm ? scA : 1.0, col, outlineA ? 1 : 0);
                }
            if (wantB && Bim)
                for (int s = 0; s < HB.nSeries; s++) {
                    // matched by NAME; a series only B has is drawn neutral, and
                    // never coloured as if it were one of A's planes
                    int a = abSeriesMatch(H.seriesNames, H.nSeries, HB.seriesNames[s]);
                    if (a >= 0 ? !drawn(a) : app.histPlane >= 0) continue;
                    // A single grey series gave B the SAME colour as A's fill,
                    // leaving a 1.4 px dashed line at ~30/255 contrast wherever
                    // it crossed that fill - the dash pattern was B's only cue.
                    // Mono B gets a cool tint so it reads against A's grey; the
                    // coloured cases already differ by hue per plane.
                    ImU32 col = a < 0 ? ODD_COL
                              : (cfaHist ? CFA_COLS[a]
                                         : (H.nSeries == 1 ? IM_COL32(120, 190, 255, 235)
                                                           : RGB_COLS[a]));
                    plotSeries(pr, HB.bins[s], norm ? scB : 1.0, col, 2);
                }
            dl->PopClipRect();
        };

        const float minSide = AB_MIN_SIDE * app.uiScale;
        bool side = Bim && abSideBySide();
        bool tooNarrow = side && ImGui::GetContentRegionAvail().x < minSide;
        if (tooNarrow) side = false;
        // fill the rest of the panel: a fixed height overflowed the bottom dock
        float footerH = ImGui::GetTextLineHeightWithSpacing() * (Bim ? 2.0f : 1.0f)
                      + (tooNarrow ? abNarrowNoteH() : 0.0f);
        // the legend row lives under the overlaid plot, and its height comes off
        // the plot BEFORE the plot is laid out
        float legendH = Bim && !side ? abLegendH(abDocLabel(im), bLabel, bStale) : 0.0f;
        float hAvail = ImGui::GetContentRegionAvail().y
                     - (ImGui::GetFontSize() * 3 + 12 * app.uiScale)   // axes + footer
                     - footerH - legendH;
        if (side) hAvail -= ImGui::GetTextLineHeight() + 6 * app.uiScale;   // heading band
        float plotH = std::max(hAvail, 70.0f * app.uiScale);

        if (!side) {
            PlotRect hp = beginPlot(xl, yl, effBlack(*im), effWhite(*im), 0.0f, 1.0f,
                                    false, false, plotH);
            drawAll(hp, true, true);
            if (Bim) drawABLegendRow(abDocLabel(im), bLabel, bStale);
        } else {
            // Always 50/50, never splitFrac: comparing two shapes needs two
            // plots of the SAME width. What the layout copies from the image is
            // the ORDER (A on the left), not the divider position.
            const ImGuiStyle& st = ImGui::GetStyle();
            float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
            float childH = plotH + ImGui::GetFontSize() * 3 + 12 * app.uiScale
                         + ImGui::GetTextLineHeight() + 8 * app.uiScale;
            ImGui::BeginChild("##histA", ImVec2(half, childH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            drawABBand("A", abDocLabel(im));
            drawAll(beginPlot(xl, yl, effBlack(*im), effWhite(*im), 0.0f, 1.0f,
                              false, false, plotH), true, false);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##histB", ImVec2(half, childH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            drawABBand("B", bLabel, bStale);
            // identical axis ranges by construction: same xl/yl, same limits
            drawAll(beginPlot(xl, yl, effBlack(*im), effWhite(*im), 0.0f, 1.0f,
                              false, false, plotH), false, true);
            ImGui::EndChild();
        }
        if (tooNarrow) abNarrowNote();
        // the real pixel counts survive the normalised axis
        ImGui::TextDisabled("A  %zu px | <black %.2f%%  >white %.2f%%%s", H.sampled,
                            H.clipLo * 100.0, H.clipHi * 100.0,
                            cfaHist ? " | R/Gr/Gb/B" : "");
        if (Bim) {
            ImGui::TextDisabled("B  %zu px | <black %.2f%%  >white %.2f%%  | %s",
                                HB.sampled, HB.clipLo * 100.0, HB.clipHi * 100.0,
                                bLabel.c_str());
            if (bStale) {   // never inside the disabled string: it must stand out
                ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
                ImGui::TextColored(AB_AMBER, "%s", AB_STALE_TOKEN);
            }
        }
    }

}

// H/V projections: column means (horizontal profile) and row means (vertical
// profile) over the selected ROI. Column FPN, banding and shading show up here
// far more clearly than in the image itself.
// Slot 0 = A, slot 1 = compare B; the body is unchanged apart from the slot.
static void recomputeProjectionIfNeeded(ImageDoc* im, App::ProjState& P) {
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), chh = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && chh > 0) { rx = cx; ry = cy; rw = cw; rh = chh; roiUsed = true; }
        }
    }
    if (P.uid == im->uid && P.dataRev == im->dataRev && P.mode == app.projMode &&
        P.cfa == im->cfa && P.cfaPattern == im->cfaPattern &&
        P.rx == rx && P.ry == ry && P.rw == rw && P.rh == rh)
        return;
    if (app.annBusy && P.uid == im->uid) return;   // mid-drag: keep the last profile
    P.img = im; P.uid = im->uid; P.dataRev = im->dataRev;
    P.mode = app.projMode; P.cfa = im->cfa; P.cfaPattern = im->cfaPattern;
    P.rx = rx; P.ry = ry; P.rw = rw; P.rh = rh; P.roiUsed = roiUsed;

    bool cfa = im->ch == 1 && im->cfa != 0;
    static const char* CFA_N[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_N[4] = { "ch0", "ch1", "ch2", "ch3" };
    P.nSeries = cfa ? 4 : std::min(im->ch, 3);
    for (int s = 0; s < 4; s++) P.seriesNames[s] = cfa ? CFA_N[s] : CH_N[s];

    for (int s = 0; s < P.nSeries; s++) {
        P.h[s].assign(rw, 0.0f);
        P.v[s].assign(rh, 0.0f);
    }
    std::vector<std::vector<int>> hN(P.nSeries, std::vector<int>(rw, 0));
    std::vector<std::vector<int>> vN(P.nSeries, std::vector<int>(rh, 0));
    bool useMax = app.projMode == 1, useMin = app.projMode == 2;
    if (useMax || useMin)
        for (int s = 0; s < P.nSeries; s++) {
            std::fill(P.h[s].begin(), P.h[s].end(), useMax ? -FLT_MAX : FLT_MAX);
            std::fill(P.v[s].begin(), P.v[s].end(), useMax ? -FLT_MAX : FLT_MAX);
        }
    // Sample cap (this was the only uncapped full-image pass). Each profile keeps
    // full resolution along its OWN axis and strides the orthogonal one, so no
    // column or row is ever left without data.
    const size_t PROJ_MAX_SAMPLES = 2000000;
    int step = (int)std::max<size_t>(1, ((size_t)rw * rh) / PROJ_MAX_SAMPLES);
    if (im->cfa) step = ((step + 1) / 2) * 2;   // keep the CFA phase intact
    auto accumulate = [&](int x, int y, bool toH) {
        const float* src = &im->data[((size_t)(ry + y) * im->w + (rx + x)) * im->ch];
        int lo = 0, hi = P.nSeries;
        if (cfa) { lo = cfaChannelAt(*im, rx + x, ry + y); hi = lo + 1; }
        for (int s = lo; s < hi; s++) {
            float val = cfa ? src[0] : src[std::min(s, im->ch - 1)];
            if (!std::isfinite(val)) continue;
            float& acc = toH ? P.h[s][x] : P.v[s][y];
            if (useMax) acc = std::max(acc, val);
            else if (useMin) acc = std::min(acc, val);
            else acc += val;
            (toH ? hN[s][x] : vN[s][y])++;
        }
    };
    for (int y = 0; y < rh; y += step)          // H profile: every column
        for (int x = 0; x < rw; x++) accumulate(x, y, true);
    for (int y = 0; y < rh; y++)                // V profile: every row
        for (int x = 0; x < rw; x += step) accumulate(x, y, false);
    P.hMin = P.vMin = FLT_MAX; P.hMax = P.vMax = -FLT_MAX;
    for (int s = 0; s < P.nSeries; s++) {
        for (int x = 0; x < rw; x++) {
            if (!hN[s][x]) { P.h[s][x] = std::numeric_limits<float>::quiet_NaN(); continue; }
            if (!useMax && !useMin) P.h[s][x] /= hN[s][x];
            P.hMin = std::min(P.hMin, P.h[s][x]); P.hMax = std::max(P.hMax, P.h[s][x]);
        }
        for (int y = 0; y < rh; y++) {
            if (!vN[s][y]) { P.v[s][y] = std::numeric_limits<float>::quiet_NaN(); continue; }
            if (!useMax && !useMin) P.v[s][y] /= vN[s][y];
            P.vMin = std::min(P.vMin, P.v[s][y]); P.vMax = std::max(P.vMax, P.v[s][y]);
        }
    }
    if (P.hMin > P.hMax) { P.hMin = 0; P.hMax = 1; }
    if (P.vMin > P.vMax) { P.vMin = 0; P.vMax = 1; }

    // statistics OF THE PROFILE itself: the spread of column means is column FPN,
    // the spread of row means is row FPN / banding - that is the number people
    // actually want out of a projection, and eyeballing a curve cannot give it.
    for (int s = 0; s < P.nSeries; s++) {
        for (int axis = 0; axis < 2; axis++) {
            const std::vector<float>& d = axis == 0 ? P.h[s] : P.v[s];
            App::ProjState::Stats& st = axis == 0 ? P.hStat[s] : P.vStat[s];
            st = {};
            double sum = 0, sum2 = 0;
            double mn = DBL_MAX, mx = -DBL_MAX;
            size_t n = 0;
            for (float v : d) {
                if (!std::isfinite(v)) continue;
                sum += v; sum2 += (double)v * v;
                mn = std::min(mn, (double)v); mx = std::max(mx, (double)v);
                n++;
            }
            if (!n) continue;
            st.mean = sum / n;
            double var = sum2 / n - st.mean * st.mean;
            st.sd = sqrt(var > 0 ? var : 0);
            st.mn = mn; st.mx = mx;
            st.pp = mx - mn;
            st.pct = st.mean != 0 ? st.sd / fabs(st.mean) * 100.0 : 0.0;
            st.valid = true;
        }
    }
}

static void drawPanelProjection() {
    ImageDoc* im = cur();
    if (!im || im->w < 1 || im->h < 1) { ImGui::TextDisabled("no image"); return; }
    const char* modes[3] = { "mean", "max", "min" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::BeginCombo("reduce", modes[std::clamp(app.projMode, 0, 2)])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(modes[i], app.projMode == i)) app.projMode = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::Checkbox("H", &app.showProjH);
    ImGui::SameLine(); ImGui::Checkbox("V", &app.showProjV);
    ImageDoc* Bim = abStatsB();
    recomputeProjectionIfNeeded(im, app.proj[0]);
    // skipped while frames are being stepped (see abStepBusy), except on the
    // first fill - an empty B plot would be worse than a stale one
    if (Bim && (!abStepBusy() || app.proj[1].uid == 0))
        recomputeProjectionIfNeeded(Bim, app.proj[1]);
    const App::ProjState& P = app.proj[0];
    const App::ProjState& PB = app.proj[1];
    const bool bStale = Bim && PB.uid != Bim->uid;
    const std::string bLabel = Bim ? abDocLabel(Bim) : std::string();
    ImGui::SameLine();
    if (Bim) ImGui::TextDisabled("A %dx%d  B %dx%d  (%s)", P.rw, P.rh, PB.rw, PB.rh,
                                 P.roiUsed ? "ROI" : "whole image");
    else     ImGui::TextDisabled("%s  %dx%d", P.roiUsed ? "ROI" : "whole image", P.rw, P.rh);

    // value axis: a rescaling y axis makes profiles impossible to compare
    const char* ymodes[3] = { "auto", "display range", "fixed" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("value axis", ymodes[std::clamp(app.projYMode, 0, 2)])) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(ymodes[i], app.projYMode == i)) {
                if (i == 2 && app.projYMode != 2) {   // seed 'fixed' from what is shown
                    app.projYLo = std::min(P.hMin, P.vMin);
                    app.projYHi = std::max(P.hMax, P.vMax);
                }
                app.projYMode = i;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("auto: fits the data (H and V share one scale)\n"
                          "display range: the black/white points - link those across images\n"
                          "and every image's profile lands on the same axis\n"
                          "fixed: whatever you type below");
    float yLo, yHi;
    if (app.projYMode == 1) { yLo = effBlack(*im); yHi = effWhite(*im); }
    else if (app.projYMode == 2) { yLo = app.projYLo; yHi = app.projYHi; }
    else {                                   // shared across H, V - and A and B
        yLo = std::min(P.hMin, P.vMin); yHi = std::max(P.hMax, P.vMax);
        if (Bim) {
            yLo = std::min(yLo, std::min(PB.hMin, PB.vMin));
            yHi = std::max(yHi, std::max(PB.hMax, PB.vMax));
        }
    }
    if (app.projYMode == 2) {
        ImGui::SameLine();
        // shadow buffer: committing per keystroke would re-project the image on
        // every digit typed
        static float pv[2] = {};
        static bool pvEditing = false;
        if (!pvEditing) { pv[0] = app.projYLo; pv[1] = app.projYHi; }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        ImGui::InputFloat2("##projy", pv, "%.5g");
        pvEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && pv[1] > pv[0]) {
            app.projYLo = pv[0]; app.projYHi = pv[1];
        }
    }

    static const ImU32 CFA_COLS[4] = { IM_COL32(255, 92, 92, 220), IM_COL32(120, 230, 120, 220),
                                       IM_COL32(60, 180, 140, 220), IM_COL32(92, 155, 255, 220) };
    static const ImU32 RGB_COLS[3] = { IM_COL32(255, 92, 92, 210), IM_COL32(79, 221, 107, 210),
                                       IM_COL32(92, 155, 255, 210) };
    const ImU32 ODD_COL = IM_COL32(185, 192, 200, 210);   // a series only one side has
    bool cfa = im->ch == 1 && im->cfa != 0;
    int plots = (app.showProjH ? 1 : 0) + (app.showProjV ? 1 : 0);
    if (!plots) { ImGui::TextDisabled("enable H or V"); return; }

    // Overlay is only honest when the two profiles are the SAME axis: same
    // length and same origin. Different lengths cannot be aligned without
    // knowing the physical mapping between the two captures, and stretching one
    // to fit the other would invent a correspondence that does not exist. So:
    // fall back to side by side and say why.
    const bool canOverlay = !Bim || abProjOverlayable(P, PB);
    const float minSide = AB_MIN_SIDE * app.uiScale;
    bool side = Bim && (abSideBySide() || !canOverlay);
    // the narrow-panel fallback yields to correctness: mismatched profiles are
    // never overlaid, however little room there is
    bool tooNarrow = side && canOverlay && ImGui::GetContentRegionAvail().x < minSide;
    if (tooNarrow) side = false;

    // reserve the statistics table first: the plots must not push it off-panel
    int statRows = (P.nSeries + (Bim ? PB.nSeries : 0)) * plots;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float statsH = ImGui::GetFrameHeightWithSpacing()      // "profile statistics" separator
                 + lineH * (statRows + 1)                  // header + one row per axis/channel
                 + lineH;                                  // footnote
    if (Bim && !canOverlay) statsH += lineH;               // the mismatch notice
    if (tooNarrow) statsH += abNarrowNoteH();
    // one legend row under each overlaid plot, reserved before they are sized
    const std::string aLabel = abDocLabel(im);
    float legendH = Bim && !side ? abLegendH(aLabel, bLabel, bStale) : 0.0f;
    float avail = ImGui::GetContentRegionAvail().y - statsH - legendH * plots;
    if (side) avail -= (ImGui::GetTextLineHeight() + 6 * app.uiScale);   // heading band
    float each = std::max((avail - lineH * plots) / plots
                          - (ImGui::GetFontSize() * 3 + 12 * app.uiScale), 60.0f * app.uiScale);

    // x range: the UNION of A's and B's sample ranges, so both plots (side by
    // side or overlaid) carry the same axis and neither is rescaled to fit.
    auto xRange = [&](bool horizontal, float& x0, float& x1) {
        int o = horizontal ? P.rx : P.ry, n = horizontal ? P.rw : P.rh;
        x0 = (float)o; x1 = (float)(o + std::max(n - 1, 1));
        if (Bim) {
            int ob = horizontal ? PB.rx : PB.ry, nb = horizontal ? PB.rw : PB.rh;
            x0 = std::min(x0, (float)ob);
            x1 = std::max(x1, (float)(ob + std::max(nb - 1, 1)));
        }
    };
    // Stroke one side's profiles. dashed = this is B. bars = draw the min-max
    // range of a decimated bucket; A only, per the plan - two sets of range
    // bars on one plot is noise, and the reader needs one reference.
    auto stroke = [&](const PlotRect& pr, const App::ProjState& S, bool horizontal,
                      bool dashed, bool bars) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::vector<float>* series = horizontal ? S.h : S.v;
        int origin = horizontal ? S.rx : S.ry;
        for (int s = 0; s < S.nSeries; s++) {
            // colour follows A's series OF THE SAME NAME (R is always R); a
            // series only one side has gets a neutral stroke, never a plane hue
            int a = dashed ? abSeriesMatch(P.seriesNames, P.nSeries, S.seriesNames[s]) : s;
            ImU32 col = a < 0 ? ODD_COL
                      : (cfa ? CFA_COLS[a]
                             : (P.nSeries == 1 ? IM_COL32(215, 220, 225, 230) : RGB_COLS[a]));
            const std::vector<float>& d = series[s];
            // decimate to the plot's pixel width: 4000 samples into 400 px was
            // 4000 line segments per series per frame
            int px = std::max(1, (int)(pr.p1.x - pr.p0.x));
            int stride = std::max(1, (int)d.size() / px);
            std::vector<ImVec2> run;
            auto flush = [&]() {
                if (run.size() >= 2) {
                    if (dashed) addDashedPolyline(dl, run.data(), (int)run.size(), col, 1.2f,
                                                  5 * app.uiScale, 4 * app.uiScale);
                    else dl->AddPolyline(run.data(), (int)run.size(), col, 0, 1.2f);
                }
                run.clear();
            };
            for (int i = 0; i < (int)d.size(); i += stride) {
                float lo = FLT_MAX, hi = -FLT_MAX;
                for (int k = i; k < std::min(i + stride, (int)d.size()); k++)
                    if (std::isfinite(d[k])) { lo = std::min(lo, d[k]); hi = std::max(hi, d[k]); }
                if (lo > hi) { flush(); continue; }
                if (bars && stride > 1) {
                    ImVec2 a2 = pr.at((float)(origin + i), lo), b2 = pr.at((float)(origin + i), hi);
                    if (b2.y != a2.y) dl->AddLine(a2, b2, col, 1.2f);   // min-max bar
                }
                run.push_back(pr.at((float)(origin + i), (lo + hi) * 0.5f));
            }
            flush();
        }
    };
    // "There is a spike - WHICH column is it?" The readout answers with the
    // exact index and the values under the cursor, without decimation: the
    // marker snaps to the true sample, not to the plotted min-max bucket.
    auto hoverReadout = [&](const PlotRect& pr, bool horizontal, bool wantA, bool wantB) {
        if (!ImGui::IsMouseHoveringRect(pr.p0, pr.p1)) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float mx = ImGui::GetMousePos().x;
        float t = (mx - pr.p0.x) / std::max(pr.p1.x - pr.p0.x, 1.0f);
        int at = (int)(pr.xmin + t * (pr.xmax - pr.xmin) + 0.5f);
        dl->AddLine(pr.at((float)at, pr.ymin), pr.at((float)at, pr.ymax),
                    IM_COL32(230, 200, 90, 140), 1.0f);
        char tip[512];
        int off = snprintf(tip, sizeof tip, "%s %d", horizontal ? "column x" : "row y", at);
        auto one = [&](const App::ProjState& S, const char* side2, const std::string& dt) {
            const std::vector<float>* series = horizontal ? S.h : S.v;
            int origin = horizontal ? S.rx : S.ry;
            int i = at - origin;
            for (int s2 = 0; s2 < S.nSeries; s2++) {
                if (i < 0 || i >= (int)series[s2].size() || !std::isfinite(series[s2][i])) continue;
                const char* nm = S.nSeries == 1 ? "" : S.seriesNames[s2];
                off += snprintf(tip + off, sizeof tip - off, "\n%s%s%s%.6g %s",
                                side2, nm, *nm ? ": " : "", series[s2][i], dt.c_str());
                dl->AddCircleFilled(pr.at((float)at, series[s2][i]), 3.0f,
                                    IM_COL32(230, 200, 90, 230));
            }
        };
        if (wantA) one(P, Bim ? "A " : "", abValueUnit(im->dtype));
        if (wantB && Bim) one(PB, "B ", abValueUnit(Bim->dtype));
        ImGui::SetTooltip("%s", tip);
    };
    auto onePlot = [&](bool horizontal, bool wantA, bool wantB) {
        float x0, x1;
        xRange(horizontal, x0, x1);
        char yl[176];
        // "display range" is effBlack/effWhite, which with compare on is the A/B
        // range - so say which one, exactly as the histogram's x axis does
        char yr[80] = "";
        if (app.projYMode == 1)
            snprintf(yr, sizeof yr, ", %s", abRangeSaid(Bim != nullptr));
        if (Bim && Bim->dtype != im->dtype)
            snprintf(yl, sizeof yl, "%s value (%s%s)  -  A %s / B %s, DTYPE MISMATCH",
                     modes[std::clamp(app.projMode, 0, 2)], abValueUnit(im->dtype).c_str(),
                     yr, im->dtype.c_str(), Bim->dtype.c_str());
        else
            snprintf(yl, sizeof yl, "%s value (%s%s)", modes[std::clamp(app.projMode, 0, 2)],
                     abValueUnit(im->dtype).c_str(), yr);
        PlotRect pr = beginPlot(horizontal ? "column x (px)" : "row y (px)", yl,
                                x0, x1, yLo, yHi, true, false, each);
        if (!pr.ok) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pr.p0, pr.p1, true);
        if (wantA) stroke(pr, P, horizontal, false, true);     // solid, with min-max bars
        if (wantB && Bim) stroke(pr, PB, horizontal, true, false);   // dashed, no bars
        dl->PopClipRect();
        hoverReadout(pr, horizontal, wantA, wantB);
        if (wantA && wantB && Bim) drawABLegendRow(abDocLabel(im), bLabel, bStale);
    };

    if (!side) {
        if (app.showProjH) onePlot(true, true, true);
        if (app.showProjV) onePlot(false, true, true);
    } else {
        // always 50/50 and always A on the left (docs/ab-stats-plan.md 3)
        const ImGuiStyle& st = ImGui::GetStyle();
        float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
        float childH = ImGui::GetTextLineHeight() + 8 * app.uiScale
                     + plots * (each + ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        ImGui::BeginChild("##projA", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("A", abDocLabel(im));
        if (app.showProjH) onePlot(true, true, false);
        if (app.showProjV) onePlot(false, true, false);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##projB", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("B", bLabel, bStale);
        if (app.showProjH) onePlot(true, false, true);
        if (app.showProjV) onePlot(false, false, true);
        ImGui::EndChild();
    }
    if (Bim && !canOverlay)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "profiles do not share an axis (A x %d..%d, y %d..%d px; "
                           "B x %d..%d, y %d..%d px): shown side by side, never stretched",
                           P.rx, P.rx + P.rw - 1, P.ry, P.ry + P.rh - 1,
                           PB.rx, PB.rx + PB.rw - 1, PB.ry, PB.ry + PB.rh - 1);
    if (tooNarrow) abNarrowNote();

    // numbers to go with the curves
    ImGui::SeparatorText("profile statistics");
    int nCols = (Bim ? 3 : 2) + 4;
    if (ImGui::BeginTable("projstats", nCols, ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX)) {
        // one row per (side, axis, plane). The table's axis is the QUANTITY, so
        // A and B cannot be a column pair here - they are rows, and the side
        // column says which is which (docs/ab-stats-plan.md 4).
        if (Bim)
            ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 1.6f);
        ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
        ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("sigma", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("sigma %", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("p-p", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        auto rows = [&](const App::ProjState& S, const char* sideName, bool horizontal) {
            for (int s = 0; s < S.nSeries; s++) {
                const App::ProjState::Stats& st = horizontal ? S.hStat[s] : S.vStat[s];
                if (!st.valid) continue;
                ImGui::TableNextRow();
                if (Bim) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sideName); }
                ImGui::TableNextColumn(); ImGui::TextDisabled(horizontal ? "H (col)" : "V (row)");
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", S.seriesNames[s]);
                ImGui::TableNextColumn(); textNum("%.6g", st.mean);
                ImGui::TableNextColumn(); textNum("%.6g", st.sd);
                ImGui::TableNextColumn(); textNum("%.3f", st.pct);
                ImGui::TableNextColumn(); textNum("%.6g", st.pp);
            }
        };
        if (app.showProjH) { rows(P, "A", true);  if (Bim) rows(PB, "B", true); }
        if (app.showProjV) { rows(P, "A", false); if (Bim) rows(PB, "B", false); }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("sigma of the column means = column FPN; of the row means = row FPN");
}

// ---- linearity ---------------------------------------------------------------
// The folder structure IS the experiment: each stack was captured at one
// exposure level, so level comes from the stack (auto-read from its name,
// editable), the response comes from the frames, and the fit answers the two
// questions a sensor evaluation starts with - is the response linear, and what
// is the conversion gain. CFA planes stay separate throughout: mixing them
// turns channel sensitivity differences into fake nonlinearity.

// "100lx/frame_###.npy" -> 100; "00/f_###" -> 0; "run42_200ms.npy [remote]" -> 42.
// Rule: the first number in the FOLDER part when there is one, else in the whole
// name. Deliberately simple - this is only a SUGGESTION a human confirms, and
// srcOut reports the text it was read out of so the modal can show its work.
// (the default for srcOut is on the forward declaration, with the model.)
static double extractLevelFromName(const std::string& name, std::string* srcOut) {
    std::string part = name;
    size_t slash = part.find_last_of('/');
    if (slash != std::string::npos && slash > 0) part = part.substr(0, slash);
    else {
        // No folder qualifier (a stack opened straight out of its own folder):
        // the FILE part is all there is, and its frame-axis extent is a frame
        // count, never an illuminance. Cut the extent run out before reading -
        // "0000..0003.npy" must stay "no level here", exactly as "????.npy"
        // did before the extent replaced the wildcard.
        size_t d = part.find("\xE2\x80\xA5");
        if (d != std::string::npos) {
            size_t a = d, b = d + 3;
            while (a > 0 && isdigit((unsigned char)part[a - 1])) a--;
            while (b < part.size() && isdigit((unsigned char)part[b])) b++;
            part.erase(a, b - a);
        }
    }
    if (srcOut) srcOut->clear();
    for (size_t i = 0; i < part.size(); i++) {
        if (isdigit((unsigned char)part[i])) {
            // avoid the "###" pattern placeholder and sizes like 640x480
            if (srcOut) *srcOut = part;
            return atof(part.c_str() + i);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

struct StackStats {
    bool valid = false;
    int frames = 0, nPl = 1;
    double mean[4] = {}, sigmaT[4] = {};
    std::string err;
};

// Per-plane mean and temporal sigma of one stack's resident frames, over the
// given ROI. The same statistic the server aggregate computes, evaluated on
// whatever is local - which covers NAS data and fully fetched remote stacks.
static StackStats computeStackStats(int seqId, int rx, int ry, int rw, int rh) {
    StackStats out;
    std::vector<int> fr = framesOfSeq(seqId);
    const ImageDoc* first = nullptr;
    std::vector<const ImageDoc*> use;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->remoteStep > 1 || d->data.empty()) continue;   // previews lie
        if (!first) first = d;
        if (d->w != first->w || d->h != first->h || d->ch != first->ch) continue;
        use.push_back(d);
    }
    if (use.size() < 2) { out.err = "needs >= 2 loaded frames"; return out; }
    int W = first->w, H = first->h, C = first->ch;
    rx = std::clamp(rx, 0, W - 1); ry = std::clamp(ry, 0, H - 1);
    rw = rw <= 0 ? W - rx : std::min(rw, W - rx);
    rh = rh <= 0 ? H - ry : std::min(rh, H - ry);
    size_t samples = (size_t)rw * rh * C;
    if (samples > (size_t)32 << 20) { out.err = "ROI too large (max 32M samples)"; return out; }
    std::vector<double> sum(samples, 0.0), sum2(samples, 0.0);
    for (const ImageDoc* d : use)
        for (int y = 0; y < rh; y++) {
            const float* row = &d->data[((size_t)(ry + y) * W + rx) * C];
            double* s1 = &sum[(size_t)y * rw * C];
            double* s2 = &sum2[(size_t)y * rw * C];
            for (size_t i = 0; i < (size_t)rw * C; i++) {
                double v = row[i];
                if (!std::isfinite(v)) continue;   // counted as N anyway: rare
                s1[i] += v; s2[i] += v * v;
            }
        }
    const double N = (double)use.size();
    out.nPl = first->cfa ? 4 : 1;
    double plM[4] = {}, plV[4] = {};
    size_t plC[4] = {};
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int p = first->cfa ? cfaChannelAt(*first, rx + x, ry + y) : 0;
            for (int c = 0; c < C; c++) {
                size_t i = ((size_t)y * rw + x) * C + c;
                double m = sum[i] / N;
                double var = std::max(0.0, sum2[i] / N - m * m) * (N / (N - 1.0));
                plM[p] += m; plV[p] += var; plC[p]++;
            }
        }
    for (int p = 0; p < out.nPl; p++) {
        if (!plC[p]) continue;
        out.mean[p] = plM[p] / (double)plC[p];
        out.sigmaT[p] = sqrt(plV[p] / (double)plC[p]);
    }
    out.frames = (int)use.size();
    out.valid = true;
    return out;
}

// least squares y = a*x + b; returns false with fewer than 2 distinct points
static bool linFit(const std::vector<double>& xs, const std::vector<double>& ys,
                   double& a, double& b, double& r2) {
    size_t n = xs.size();
    if (n < 2) return false;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (size_t i = 0; i < n; i++) {
        sx += xs[i]; sy += ys[i];
        sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; syy += ys[i] * ys[i];
    }
    double d = n * sxx - sx * sx;
    if (fabs(d) < 1e-12) return false;
    a = (n * sxy - sx * sy) / d;
    b = (sy - a * sx) / n;
    double sst = syy - sy * sy / n;
    double ssr = 0;
    for (size_t i = 0; i < n; i++) {
        double e = ys[i] - (a * xs[i] + b);
        ssr += e * e;
    }
    r2 = sst > 1e-12 ? 1.0 - ssr / sst : 1.0;
    return true;
}

static const char* LIN_PLANES[4] = { "R", "Gr", "Gb", "B" };
static const ImU32 LIN_COLS[4] = {
    IM_COL32(255, 92, 92, 255), IM_COL32(79, 221, 107, 255),
    IM_COL32(90, 200, 190, 255), IM_COL32(96, 156, 255, 255),
};

// Everything the panel shows, recomputed only when Compute is pressed: this is
// a measurement action over potentially hundreds of frames, not a per-frame UI.
//
// ONE SERIES, not "every open stack". The old walk over app.seqs meant two
// sweeps open at the same time were silently fitted as one curve, and the
// parameter values had nowhere to live but the stacks themselves. The rows are
// the series' members, in the series' order; include and value come straight
// off Series::Member, so a table row IS members[i] and there is nothing to
// carry across a recompute.
static void linRecompute(int seriesId) {
    App::LinState& L = app.lin;
    L.seriesId = seriesId;
    // the ROI is shared with everything else: the selected rect, or whole frame
    int rx = 0, ry = 0, rw = 0, rh = 0;
    L.roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn))
        if (a->type == 0) { rx = a->x; ry = a->y; rw = a->w; rh = a->h; L.roiUsed = true; }
    L.rows.clear();
    App::Series* S = seriesById(seriesId);
    if (!S) {
        L.fitValid = false;
        L.nPl = 1; L.nPts = 0;
        L.computedRev = ++L.rev;
        return;
    }
    for (const auto& m : S->members) {
        App::SeqInfo* si = seqInfo(m.seqId);
        if (!si) continue;              // seriesAudit says this cannot happen
        App::LinState::Row r;
        r.seqId = m.seqId;
        r.name = si->name;
        r.level = m.value;
        r.include = m.include;
        StackStats st = computeStackStats(m.seqId, rx, ry, rw, rh);
        r.valid = st.valid;
        r.err = st.err;
        r.frames = st.frames;
        r.nPl = st.nPl;
        for (int p = 0; p < 4; p++) { r.mean[p] = st.mean[p]; r.sigmaT[p] = st.sigmaT[p]; }
        L.rows.push_back(std::move(r));
    }
    // fits, per plane, over rows that have both a value and a measurement
    L.fitValid = false;
    L.nPl = 1;
    for (const auto& r : L.rows) if (r.valid) L.nPl = std::max(L.nPl, r.nPl);
    int fitted = 0;
    // No unit, no fit - and no fit for a kind these equations do not describe.
    // The per-stack MEANS above are DN either way and stay on screen; a
    // sensitivity is DN per SOMETHING measured by a KNOWN law, and inventing
    // either is exactly the assumption the series layer exists to stop.
    if (S->unit[0] == '\0' || !seriesFitKind(*S)) {
        L.nPts = 0;
        L.computedRev = ++L.rev;
        return;
    }
    for (int p = 0; p < L.nPl; p++) {
        std::vector<double> lx, ly, pm, pv;
        for (const auto& r : L.rows) {
            if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
            lx.push_back(r.level);
            ly.push_back(r.mean[p]);
            pm.push_back(r.mean[p]);
            pv.push_back(r.sigmaT[p] * r.sigmaT[p]);
        }
        L.nPts = (int)lx.size();
        if (!linFit(lx, ly, L.slope[p], L.offs[p], L.r2[p])) continue;
        fitted++;
        // linearity error: worst deviation from the fit, relative to the fit
        double worst = 0;
        for (size_t i = 0; i < lx.size(); i++) {
            double fit = L.slope[p] * lx[i] + L.offs[p];
            if (fabs(fit) > 1e-9)
                worst = std::max(worst, fabs((ly[i] - fit) / fit) * 100.0);
        }
        L.leMax[p] = worst;
        // photon transfer: sigma_t^2 vs mean; the slope is the system gain K
        double kr2;
        if (!linFit(pm, pv, L.ptcK[p], L.ptcRead2[p], kr2)) { L.ptcK[p] = 0; L.ptcRead2[p] = 0; }
        // Read noise is the noise left at ZERO SIGNAL, and zero signal sits at
        // the black level - not at mean = 0 DN. Extrapolating the PTC to 0 DN
        // lands K*offset below that, which for any sensor with an offset is a
        // large negative number and reads out as "0".
        //
        // Even corrected, the extrapolation is a difference of two large numbers
        // (verified: injecting 3.0 DN of read noise comes back as 0-3.4 DN when
        // the lowest level is already 80 DN above black). So it is only reported
        // as a measurement when there is a DARK stack to measure it in; from a
        // lit-only series it is marked as an extrapolation.
        L.readDN[p] = sqrt(std::max(0.0, L.ptcRead2[p] + L.ptcK[p] * L.offs[p]));
        L.readFromDark = false;
        for (const auto& r : L.rows) {
            if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
            if (fabs(r.level) > 1e-9) continue;             // not a dark stack
            L.readDN[p] = r.sigmaT[p];                      // measured, not fitted
            L.readFromDark = true;
            break;
        }
    }
    L.fitValid = fitted > 0;
    L.computedRev = ++L.rev;
}

// SELFTEST ONLY - the one place in the program that creates a series without a
// human. The canon forbids auto-creation because a guessed sweep is a lie that
// fits; a headless test has nobody to press Create, so it presses it here, in
// the open: ONE series over every stack of one batch, values from
// extractLevelFromName (exactly the proposal the Create modal puts on screen),
// unit = the same prefill that modal starts from. Called only from --*-selftest
// branches in main(); returns 0 when the batch held no stacks.
static int selftestMakeSeries(int batchId, const char* unitPrefill) {
    if (!batchId) return 0;
    int id = newSeries(batchId, "");
    if (App::Series* S = seriesById(id)) {
        S->paramName = "level";
        snprintf(S->unit, sizeof S->unit, "%s",
                 unitPrefill && *unitPrefill ? unitPrefill : "lx");
    }
    for (const auto& si : app.seqs) {
        if (batchOfStack(si.id) != batchId) continue;
        addToSeries(id, si.id, extractLevelFromName(si.name));
    }
    App::Series* S = seriesById(id);
    if (!S || S->members.empty()) { pruneEmptySeries(); return 0; }
    return id;
}

static const char* SERIES_KINDS = "linearity\0PTC\0temperature\0other\0";

// What one row of the create/edit table means as a number. An untouched row of
// an existing member is its stored double, not a re-read of the six digits the
// box happens to be showing; anything else is the text, and text the program
// cannot read is UNSET, never 0 (parseSeriesValue).
static double seriesEditRowValue(const App::SeriesEdit::Row& r) {
    if (r.haveOrig && !r.touched) return r.orig;
    return parseSeriesValue(r.value);
}

// Fill the create/edit modal. Creating: every stack of the batch, all ticked,
// each with extractLevelFromName's proposal ALREADY in its value box and marked
// as a proposal. Editing: the members first, in their order, with their own
// values, then the batch's non-members, unticked.
//
// The proposal is the whole point of the old "Auto levels" button, moved to
// where it belongs: the user sees the numbers and the text they were read out
// of BEFORE pressing Create, so pressing it is a confirmation. Nothing here
// runs by itself - series are never auto-created (docs/terminology.md).
static void openSeriesModal(int batchId, int editId) {
    App::SeriesEdit& E = app.seriesEdit;
    E = App::SeriesEdit{};
    E.editId = editId;
    E.batchId = batchId;
    const App::Series* S = editId ? seriesById(editId) : nullptr;
    if (S) {
        E.batchId = S->batchId;
        snprintf(E.name, sizeof E.name, "%s", S->name.c_str());
        snprintf(E.param, sizeof E.param, "%s", S->paramName.c_str());
        snprintf(E.unit, sizeof E.unit, "%s", S->unit);
        E.kind = S->kind;
    } else {
        snprintf(E.name, sizeof E.name, "%s 掃引", batchNameOf(batchId).c_str());
        // the prefill, and only that: an empty box stays empty (prefs linunit)
        snprintf(E.unit, sizeof E.unit, "%s", app.lin.unit);
    }
    auto addRow = [&](int seqId, bool checked, double value, bool haveValue) {
        App::SeriesEdit::Row r;
        r.seqId = seqId;
        r.check = checked;
        App::SeqInfo* si = seqInfo(seqId);
        r.name = si ? si->name : std::string();
        double guess = extractLevelFromName(r.name, &r.from);
        if (haveValue) {
            // An EXISTING member. Its value is the user's - INCLUDING the
            // decision not to set one. A guess proposed over that is a proposal
            // on a row nobody came here to look at (Edit... is mostly opened to
            // rename), and Save would commit it: "value unset" becomes a fit
            // point with zero keystrokes. Creating is where a proposal belongs
            // (docs/series-plan.md §2: 見た上で Create を押させる); here it is
            // offered in the column beside the box instead of inside it.
            r.orig = value;
            r.haveOrig = true;
            if (std::isfinite(value)) snprintf(r.value, sizeof r.value, "%.6g", value);
            else r.guess = guess;
        } else if (std::isfinite(guess)) {
            snprintf(r.value, sizeof r.value, "%.6g", guess);
            r.suggested = true;           // dim + "(guess)" until it is touched
        }
        E.rows.push_back(std::move(r));
    };
    if (S)
        for (const auto& m : S->members) addRow(m.seqId, true, m.value, true);
    for (const auto& si : app.seqs) {
        if (batchOfStack(si.id) != E.batchId) continue;
        bool already = false;
        for (const auto& r : E.rows) if (r.seqId == si.id) already = true;
        if (!already) addRow(si.id, S == nullptr, 0, false);
    }
    E.open = true;
}

// The modal's Save / Create button, as a plain function. Every other command in
// this layer already is one (that is what let the selftest drive them); this one
// was inline in the draw call, which is exactly why "open Edit... and press Save
// without touching anything" had to be found by hand instead of by a test.
// Returns the series' id, or 0 when nothing was accepted.
static int seriesModalAccept() {
    App::SeriesEdit& E = app.seriesEdit;
    // Build the member list in TABLE ORDER: the row order is the series
    // order, which is why the arrows exist.
    std::vector<App::Series::Member> ms;
    for (const auto& r : E.rows) {
        if (!r.check || !seqInfo(r.seqId)) continue;
        if (batchOfStack(r.seqId) != E.batchId) continue;   // strict containment
        App::Series::Member m;
        m.seqId = r.seqId;
        m.value = seriesEditRowValue(r);
        m.include = true;
        if (const App::Series* old = seriesById(E.editId))   // keep the fit flag
            for (const auto& om : old->members)
                if (om.seqId == r.seqId) m.include = om.include;
        ms.push_back(m);
    }
    if (ms.empty()) return 0;
    int id = E.editId ? E.editId : newSeries(E.batchId, E.name);
    // a stack belongs to AT MOST ONE series: taking one in takes it out of
    // wherever it was
    for (const auto& m : ms) {
        App::Series* other = seriesOfStack(m.seqId);
        if (other && other->id != id) removeFromSeries(m.seqId);
    }
    if (App::Series* S = seriesById(id)) {
        S->name = E.name[0] ? E.name : batchNameOf(E.batchId) + " 掃引";
        S->paramName = E.param;
        snprintf(S->unit, sizeof S->unit, "%s", E.unit);
        S->kind = std::clamp(E.kind, 0, 3);
        S->members = std::move(ms);
    }
    // the next new series starts from the unit this one ended with
    if (E.unit[0]) snprintf(app.lin.unit, sizeof app.lin.unit, "%s", E.unit);
    app.prefsDirty = true;
    pruneEmptySeries();
    if (seriesById(id)) app.curSeriesId = id;
    // Whatever was on screen measured the series as it WAS. The panel reads its
    // labels live off the series, so a fit left standing here does not merely go
    // stale - it gets relabelled with the new unit and the new parameter name.
    linInvalidate();
    app.imagesRev++;
    return seriesById(id) ? id : 0;
}

static void drawSeriesModal() {
    App::SeriesEdit& E = app.seriesEdit;
    // "###" fixes the popup's identity while the title changes: ImGui hashes
    // only what follows it, so create and edit really are one modal.
    const char* POPUP = "Series###seriesmodal";
    char title[64];
    snprintf(title, sizeof title, "%s###seriesmodal",
             E.editId ? "Edit series" : "Create series");
    if (E.open && !ImGui::IsPopupOpen(POPUP)) ImGui::OpenPopup(POPUP);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640 * app.uiScale, 480 * app.uiScale), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_None)) return;
    if (!E.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    ImGui::Text("batch: %s", batchNameOf(E.batchId).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(a series lives in exactly one batch)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    ImGui::InputText("name", E.name, sizeof E.name);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("parameter", E.param, sizeof E.param);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("what was swept: illuminance, exposure, temperature ...");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputText("unit", E.unit, sizeof E.unit);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("lx, ms, lx*s, photons/px ... printed on every axis and\n"
                          "column. LEAVE IT EMPTY if you do not know it yet: an\n"
                          "unset unit means no fit, which is the honest answer.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::Combo("kind", &E.kind, SERIES_KINDS);
    if (!E.unit[0])
        ImGui::TextDisabled("unit not set - members and means still show, the fit will not.");

    ImGui::SeparatorText("members  (value = the parameter this stack was captured at)");
    if (ImGui::SmallButton("Sort by value")) {
        std::stable_sort(E.rows.begin(), E.rows.end(),
                         [](const App::SeriesEdit::Row& a, const App::SeriesEdit::Row& b) {
                             double av = seriesEditRowValue(a), bv = seriesEditRowValue(b);
                             if (!std::isfinite(av)) av = HUGE_VAL;   // unset goes last
                             if (!std::isfinite(bv)) bv = HUGE_VAL;
                             return av < bv;
                         });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) for (auto& r : E.rows) r.check = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) for (auto& r : E.rows) r.check = false;
    ImGui::SameLine();
    int nChecked = 0, nValued = 0;
    for (const auto& r : E.rows) {
        if (!r.check) continue;
        nChecked++;
        // what the row would BE, not whether the box has characters in it: a box
        // holding "-" or "1e" is an unset member, and the count has to say so
        if (std::isfinite(seriesEditRowValue(r))) nValued++;
    }
    ImGui::TextDisabled("| %d of %d stacks, %d with a value", nChecked,
                        (int)E.rows.size(), nValued);

    const ImGuiTableFlags TF = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    int moveFrom = -1, moveTo = -1;
    if (ImGui::BeginTable("sermembers", 5, TF, ImVec2(0, std::max(tableH, 80.0f * app.uiScale)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("stack", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("read from", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("order", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFrameHeight() * 2.4f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)E.rows.size(); i++) {
            App::SeriesEdit::Row& r = E.rows[i];
            ImGui::PushID(r.seqId);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Checkbox("##m", &r.check);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (r.suggested)      // a proposal reads as one until it is confirmed
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            if (ImGui::InputText("##v", r.value, sizeof r.value,
                                 ImGuiInputTextFlags_CharsScientific)) {
                r.suggested = false;
                r.touched = true;         // from here the TEXT is the value
            }
            if (r.suggested) ImGui::PopStyleColor();
            if (r.value[0] && !std::isfinite(seriesEditRowValue(r)) &&
                ImGui::IsItemHovered())
                ImGui::SetTooltip("\"%s\" is not a number - this member saves as UNSET,\n"
                                  "which is left out of the fit. It is never read as 0.",
                                  r.value);
            ImGui::TableNextColumn();
            if (r.suggested) ImGui::TextDisabled("(guess) %s", r.from.c_str());
            else if (!r.value[0] && std::isfinite(r.guess)) {
                // offered, NOT filled in: this member's value is deliberately
                // unset and only a human may change that
                ImGui::TextDisabled("guess: %.6g", r.guess);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("\"%s\" suggests %.6g. The box is empty because this\n"
                                      "member has NO value set - type it in to use the\n"
                                      "suggestion; saving as it is keeps it unset.",
                                      r.from.c_str(), r.guess);
            }
            else if (r.from.empty() && !r.value[0]) ImGui::TextDisabled("-");
            else ImGui::TextDisabled("%s", r.from.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("^")) { moveFrom = i; moveTo = i - 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i == (int)E.rows.size() - 1);
            if (ImGui::SmallButton("v")) { moveFrom = i; moveTo = i + 1; }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (moveFrom >= 0) std::swap(E.rows[moveFrom], E.rows[moveTo]);

    ImGui::Separator();
    bool canAccept = nChecked > 0;
    ImGui::BeginDisabled(!canAccept);
    if (ImGui::Button(E.editId ? "Save" : "Create", ImVec2(140 * app.uiScale, 0))) {
        seriesModalAccept();
        E.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * app.uiScale, 0))) {
        E.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!canAccept) {
        ImGui::SameLine();
        ImGui::TextDisabled("tick at least one stack");
    }
    ImGui::EndPopup();
}

static void drawPanelLinearity() {
    App::LinState& L = app.lin;
    // ---- which series? Nothing here creates one on its own -------------------
    if (app.series.empty()) {
        ImGui::TextWrapped("No series yet. Linearity is measured over a SERIES: the stacks "
                           "of one swept parameter, each with its value, and the parameter's "
                           "name and unit belong to that series - not to the application.");
        ImGui::Spacing();
        ImGui::TextDisabled("A sweep is never guessed from the folder tree. One wrong");
        ImGui::TextDisabled("guess would quietly fit data that was never a sweep.");
        ImGui::Spacing();
        int b = cur() ? cur()->batchId : 0;
        ImGui::BeginDisabled(b == 0);
        if (ImGui::Button("Create a series from this batch's stacks..."))
            openSeriesModal(b, 0);
        ImGui::EndDisabled();
        if (b == 0) ImGui::TextDisabled("(nothing is open yet)");
        else ImGui::TextDisabled("batch: %s", batchNameOf(b).c_str());
        return;
    }
    if (!seriesById(app.curSeriesId)) app.curSeriesId = app.series.front().id;
    App::Series* S = seriesById(app.curSeriesId);
    {
        char label[320];
        snprintf(label, sizeof label, "%s / %s", batchNameOf(S->batchId).c_str(),
                 S->name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 17);
        if (ImGui::BeginCombo("##series", label)) {
            for (const auto& s : app.series) {
                char it[384];
                // "##" would cut a batch or series name in half: give every row
                // an explicit id and let the visible part be whatever it is.
                // The KIND is on the row because this panel cannot measure them
                // all: picking a temperature run here has to be a visible choice.
                snprintf(it, sizeof it, "%s / %s%s%s##ser%d", batchNameOf(s.batchId).c_str(),
                         s.name.c_str(), seriesFitKind(s) ? "" : "   ",
                         seriesFitKind(s) ? "" : seriesKindName(s.kind), s.id);
                if (ImGui::Selectable(it, s.id == app.curSeriesId)) app.curSeriesId = s.id;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Edit...")) openSeriesModal(S->batchId, S->id);
        ImGui::SameLine();
        if (ImGui::Button("New...")) openSeriesModal(cur() ? cur()->batchId : S->batchId, 0);
        S = seriesById(app.curSeriesId);
    }
    const bool haveUnit = S->unit[0] != '\0';
    const bool fitKind = seriesFitKind(*S);
    if (S->paramName.empty()) ImGui::TextDisabled("(parameter unnamed)");
    else ImGui::TextUnformatted(S->paramName.c_str());
    ImGui::SameLine();
    if (haveUnit) ImGui::Text("[%s]", S->unit);
    else ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "[unit not set]");
    if (!haveUnit && ImGui::IsItemHovered())
        ImGui::SetTooltip("A sensitivity is DN per SOMETHING. Set the unit in\n"
                          "Edit... - it is not assumed for you.");
    if (!fitKind) {   // the kind picks the equation; this panel knows two of them
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1), "[%s series]",
                           seriesKindName(S->kind));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This panel fits a linearity / PTC sweep. A %s series\n"
                              "is a different measurement with a different equation,\n"
                              "so no fit is printed for it - the per-stack means are\n"
                              "measured either way.\n"
                              "Change the kind in Edit... if this really is a sweep.",
                              seriesKindName(S->kind));
    }
    ImGui::SameLine();
    if (ImGui::Button("Compute")) linRecompute(S->id);
    ImGui::SameLine();
    ImGui::TextDisabled(L.roiUsed ? "| selected ROI" : "| whole frame");
    // The per-stack table has room for ONE response column. Averaging the CFA
    // planes into it would be a different measurement, so it shows the plane you
    // pick and says which one; the fits and the plots stay per-plane.
    if (L.nPl > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::Combo("table plane", &L.tablePlane, "R\0Gr\0Gb\0B\0");
        L.tablePlane = std::clamp(L.tablePlane, 0, L.nPl - 1);
    }
    const bool fresh = L.seriesId == S->id;

    // one row per MEMBER, in the series' order: value in, response out
    const ImGuiTableFlags TF = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    char lvlHdr[64], meanHdr[32], sigHdr[32];
    snprintf(lvlHdr, sizeof lvlHdr, "value [%s]", haveUnit ? S->unit : "unit not set");
    const char* pl = L.nPl > 1 ? LIN_PLANES[std::clamp(L.tablePlane, 0, 3)] : "";
    snprintf(meanHdr, sizeof meanHdr, "mean %s [DN]", pl);
    snprintf(sigHdr, sizeof sigHdr, "sigma_t %s [DN]", pl);
    float tableH = ImGui::GetFontSize() * std::min<int>(10, (int)S->members.size() + 2) * 1.6f;
    if (ImGui::BeginTable("lintab", 7, TF, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("stack", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
        ImGui::TableSetupColumn(lvlHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.2f);
        ImGui::TableSetupColumn(meanHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(sigHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2);
        ImGui::TableHeadersRow();
        for (auto& m : S->members) {
            App::SeqInfo* si = seqInfo(m.seqId);
            if (!si) continue;
            ImGui::PushID(m.seqId);
            const App::LinState::Row* row = nullptr;
            if (fresh)
                for (const auto& r : L.rows) if (r.seqId == m.seqId) { row = &r; break; }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##inc", &m.include)) {
                for (auto& r : L.rows) if (r.seqId == m.seqId) r.include = m.include;
                // the fit below was measured WITH this point (or without it):
                // it is now describing a set of points that is not on screen
                linFitStale();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("include this point in the fit");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(si->name.c_str());
            ImGui::TableNextColumn();
            // NEVER 0 for "not set": the whole reason values live on the member
            if (std::isfinite(m.value)) textNum("%.6g", m.value);
            else ImGui::TextDisabled("unset");
            ImGui::TableNextColumn();
            if (row && row->valid) ImGui::Text("%d", row->frames);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            int tp = row ? std::clamp(L.tablePlane, 0, std::max(0, row->nPl - 1)) : 0;
            if (row && row->valid) textNum("%.6g", row->mean[tp]);
            else ImGui::TextDisabled(row ? row->err.c_str() : "-");
            ImGui::TableNextColumn();
            if (row && row->valid) textNum("%.6g", row->sigmaT[tp]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("values are edited in Edit... - a blank one is UNSET, not zero");

    if (!L.fitValid || !fresh) {
        if (!haveUnit)
            ImGui::TextDisabled("no unit: set one in Edit... and the fit follows.");
        else if (!fitKind)
            ImGui::TextDisabled("kind is \"%s\": the linearity / PTC equations are not the "
                                "ones this series asked for.", seriesKindName(S->kind));
        else if (seriesFitPoints(*S) < 2)
            ImGui::TextDisabled("a fit needs 2+ members with a value (3+ to see linearity).");
        else if (!fresh)
            ImGui::TextDisabled("press Compute to measure this series.");
        else
            ImGui::TextDisabled("press Compute; CFA planes stay separate throughout.");
        return;
    }

    // ---- the answers ----
    ImGui::SeparatorText("fit  (response = sensitivity * value + offset)");
    if (ImGui::BeginTable("linfit", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        char sensHdr[64];
        snprintf(sensHdr, sizeof sensHdr, "sensitivity [DN/%s]", S->unit);
        ImGui::TableSetupColumn("plane", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn(sensHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("offset [DN]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("R^2", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("LE max [%]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("K [DN/e-]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(L.readFromDark ? "read noise [DN]" : "read noise [DN] (extrap.)",
                                ImGuiTableColumnFlags_WidthFixed, numColW() * 1.3f);
        ImGui::TableHeadersRow();
        for (int p = 0; p < L.nPl; p++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(LIN_COLS[p]));
            ImGui::TextUnformatted(L.nPl > 1 ? LIN_PLANES[p] : "all");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); textNum("%.6g", L.slope[p]);
            ImGui::TableNextColumn(); textNum("%.6g", L.offs[p]);
            ImGui::TableNextColumn(); textNum("%.5f", L.r2[p]);
            ImGui::TableNextColumn(); textNum("%.3f", L.leMax[p]);
            ImGui::TableNextColumn(); textNum("%.5g", L.ptcK[p]);
            ImGui::TableNextColumn();
            if (L.readFromDark) textNum("%.4g", L.readDN[p]);
            else {                        // an extrapolation is not a measurement
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                textNum("%.4g", L.readDN[p]);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Extrapolated from the photon transfer fit to the black\n"
                                      "level, so it is a difference of two large numbers and\n"
                                      "can be off by several DN.\n"
                                      "Add a dark stack (level 0) to MEASURE it instead.");
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%d points | K from photon transfer (sigma_t^2 vs mean); "
                        "LE = worst deviation from the fit | read noise %s", L.nPts,
                        L.readFromDark ? "measured in the level-0 stack"
                                       : "extrapolated (no dark stack)");

    // ---- plots: the response, and the part of it that is NOT linear ----
    struct Pt { double x, y; int p; };
    std::vector<Pt> pts;
    double x0 = DBL_MAX, x1 = -DBL_MAX, y0 = DBL_MAX, y1 = -DBL_MAX, e1 = 0;
    for (const auto& r : L.rows) {
        if (!r.include || !r.valid || !std::isfinite(r.level)) continue;
        for (int p = 0; p < r.nPl; p++) {
            pts.push_back({ r.level, r.mean[p], p });
            x0 = std::min(x0, r.level); x1 = std::max(x1, r.level);
            y0 = std::min(y0, r.mean[p]); y1 = std::max(y1, r.mean[p]);
            double fit = L.slope[p] * r.level + L.offs[p];
            if (fabs(fit) > 1e-9)
                e1 = std::max(e1, fabs((r.mean[p] - fit) / fit) * 100.0);
        }
    }
    if (pts.empty()) return;
    // A plotted axis states the quantity AND the unit, and the unit is the
    // series' - there is no fit without one, and editing the series drops the
    // fit (linInvalidate), so it cannot have been emptied out from under this.
    char xl[96];
    snprintf(xl, sizeof xl, "%s [%s]",
             S->paramName.empty() ? "value" : S->paramName.c_str(), S->unit);
    float half = std::max((ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 5) * 0.5f,
                          70.0f * app.uiScale);
    {
        PlotRect pr = beginPlot(xl, "ROI mean [DN]", (float)x0, (float)x1,
                                (float)y0, (float)y1, false, false, half);
        if (pr.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            for (int p = 0; p < L.nPl; p++) {   // the fit first, under the points
                ImVec2 a = pr.at((float)x0, (float)(L.slope[p] * x0 + L.offs[p]));
                ImVec2 b = pr.at((float)x1, (float)(L.slope[p] * x1 + L.offs[p]));
                dl->AddLine(a, b, (LIN_COLS[p] & 0x00FFFFFF) | 0x60000000, 1.5f);
            }
            for (const auto& q : pts)
                dl->AddCircleFilled(pr.at((float)q.x, (float)q.y), 3.5f, LIN_COLS[q.p]);
            dl->PopClipRect();
        }
    }
    {
        float lim = (float)std::max(e1 * 1.2, 0.1);
        PlotRect pr = beginPlot(xl, "deviation from fit [%]", (float)x0, (float)x1,
                                -lim, lim, false, false, half);
        if (pr.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(pr.p0, pr.p1, true);
            ImVec2 z0 = pr.at((float)x0, 0), z1 = pr.at((float)x1, 0);
            dl->AddLine(z0, z1, IM_COL32(140, 150, 160, 160));
            for (const auto& q : pts) {
                double fit = L.slope[q.p] * q.x + L.offs[q.p];
                if (fabs(fit) < 1e-9) continue;
                double res = (q.y - fit) / fit * 100.0;
                dl->AddCircleFilled(pr.at((float)q.x, (float)res), 3.5f, LIN_COLS[q.p]);
            }
            dl->PopClipRect();
        }
    }
}

// Server-computed temporal stats: shown for a remote stack under a policy that
// lets the server compute. The numbers come over the wire in seconds; they are
// never quietly recomputed locally (that would change a displayed measurement,
// and a partial local stack would make it WORSE). The tag says where and over
// how many frames, always.
static bool drawServerTemporal(const App::SeqInfo* si) {
    App::ServerTemporal& S = app.srvTemporal;
    if (!si || !serverComputes(*si) || S.seqId != si->id) return false;
    ImGui::Text("Temporal");
    ImGui::SameLine();
    if (S.pending) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s - measuring...]",
                           S.host.empty() ? "local peer" : S.host.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("computing on the server over all %d frames", si->expectedFrames);
        return true;
    }
    if (!S.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[server failed]");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", S.err.c_str());
        ImGui::TextDisabled("frames arrive locally in the background; the panel will\n"
                            "switch to local computation once enough are here.");
        return false;                 // fall through to the local path
    }
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s, %d frames]",
                       S.host.empty() ? "local peer" : S.host.c_str(), S.frames);
    ImGui::Separator();
    if (ImGui::BeginTable("srvtemporal", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, numColW());
        auto row = [](const char* k, double v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); textNum("%.6g", v);
        };
        row("temporal noise (sigma_t)", S.tempNoise);
        row("fixed pattern (sigma_fpn)", S.fixedPattern);
        row("total (quadrature)", S.totalNoise);
        ImGui::EndTable();
    }
    if (!S.frameMean.empty()) {
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (float v : S.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        float fx0 = S.idx.empty() ? 0 : S.idx.front(), fx1 = S.idx.empty() ? 1 : S.idx.back();
        float tAvail = ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        PlotRect tp = beginPlot("frame number", "ROI mean value [DN]",
                                fx0, fx1, mn, mx, true, false, std::max(tAvail, 70.0f * app.uiScale));
        if (tp.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            for (size_t i = 1; i < S.frameMean.size(); i++)
                dl->AddLine(tp.at(S.idx[i - 1], S.frameMean[i - 1]),
                            tp.at(S.idx[i], S.frameMean[i]), IM_COL32(105, 180, 240, 255), 1.5f);
            if (ImageDoc* c2 = cur()) {
                float mxp = tp.at((float)c2->seqIndex, tp.ymin).x;
                dl->AddLine(ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y), IM_COL32(255, 184, 77, 200));
            }
            dl->PopClipRect();
        }
    }
    return true;
}

// Server temporal for a stack that is NOT opened (fired from the browser's
// group row or multi-selection). Takes over the panel until dismissed (x) or
// superseded: the user just asked for it, and it has no current-image to hang
// off. "Open as stack" turns the measurement into a real open in one click -
// whose own server temporal then replaces this one.
static void drawBrowseTemporal() {
    App::ServerTemporal& S = app.srvTemporal;
    ImGui::Text("Temporal");
    ImGui::SameLine();
    if (S.pending) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s - measuring...]",
                           S.host.empty() ? "local peer" : S.host.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
        ImGui::Separator();
        ImGui::TextDisabled("not opened: %s", S.label.c_str());
        return;
    }
    if (!S.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[server failed]");
        ImGui::SameLine();
        if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
        ImGui::Separator();
        ImGui::TextDisabled("not opened: %s", S.label.c_str());
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", S.err.c_str());
        return;
    }
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s, %d frames - not opened: %s]",
                       S.host.empty() ? "local peer" : S.host.c_str(), S.frames,
                       S.label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open as stack")) {
        // copy first: openRemoteStack fires its own server temporal, which
        // resets S while we are still reading it
        std::string host = S.host, label = S.label;
        std::vector<std::string> files = S.files;
        openRemoteStack(host, files, label);
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("open %s for real (frames transfer in the background)",
                          S.label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
    ImGui::Separator();
    // cfaType was 0 by design (no open file to read a mosaic from): say so,
    // or a Bayer stack's plane-blind sigma reads as a wrong number
    ImGui::TextDisabled("plane=all (file not opened - no CFA split)");
    if (ImGui::BeginTable("srvbtemporal", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, numColW());
        auto row = [](const char* k, double v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); textNum("%.6g", v);
        };
        row("mean [DN]", S.mean);
        row("temporal noise (sigma_t)", S.tempNoise);
        row("fixed pattern (sigma_fpn)", S.fixedPattern);
        row("total (quadrature)", S.totalNoise);
        ImGui::EndTable();
    }
    if (!S.frameMean.empty()) {
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (float v : S.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        float fx0 = S.idx.empty() ? 0 : S.idx.front(), fx1 = S.idx.empty() ? 1 : S.idx.back();
        float tAvail = ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        PlotRect tp = beginPlot("frame number", "frame mean value [DN]",
                                fx0, fx1, mn, mx, true, false, std::max(tAvail, 70.0f * app.uiScale));
        if (tp.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            for (size_t i = 1; i < S.frameMean.size(); i++)
                dl->AddLine(tp.at(S.idx[i - 1], S.frameMean[i - 1]),
                            tp.at(S.idx[i], S.frameMean[i]), IM_COL32(105, 180, 240, 255), 1.5f);
            dl->PopClipRect();
        }
    }
}

// Per-frame S1 stats of a stack as TSV on the clipboard, one row per frame:
// file, path, per-plane spatial mean/sigma, and the H/V projection non-
// uniformity (sigma of the column-mean / row-mean profile, as % of the plane
// mean - shading and banding in one number each). Deliberately NO temporal
// columns: sigma_t is a property of the stack, not of a frame (a per-frame
// "temporal noise" would be a category error - see docs/stats-taxonomy.md).
static void copyPerFrameStats(int seqId) {
    // the ROI convention every other measurement uses: selected rect, else full
    int rx = 0, ry = 0, rw = 0, rh = 0;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn))
        if (a->type == 0) { rx = a->x; ry = a->y; rw = a->w; rh = a->h; roiUsed = true; }
    std::vector<int> fr = framesOfSeq(seqId);
    int nPl = 0, skipped = 0;
    bool cfa = false;
    std::string out;
    char b[512];
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->data.empty() || d->remoteStep > 1) { skipped++; continue; }   // previews lie
        int W = d->w, H = d->h, C = d->ch;
        int x0 = std::clamp(rx, 0, W - 1), y0 = std::clamp(ry, 0, H - 1);
        int ww = rw <= 0 ? W - x0 : std::min(rw, W - x0);
        int hh = rh <= 0 ? H - y0 : std::min(rh, H - y0);
        if (!nPl) {                                    // header from the first frame
            cfa = d->cfa != 0;
            nPl = cfa ? 4 : std::min(C, 4);
            out = "file\tpath\tframe";
            for (int p = 0; p < nPl; p++) {
                static const char* CHN[4] = { "ch0", "ch1", "ch2", "ch3" };
                const char* pn = cfa ? CFA_CH_NAMES[p] : (C > 1 ? CHN[p] : "");
                std::string sfx = *pn ? std::string("_") + pn : std::string();
                snprintf(b, sizeof b,
                         "\tmean%s [DN]\tsigma%s [DN]\tHproj sigma%s [%%]\tVproj sigma%s [%%]",
                         sfx.c_str(), sfx.c_str(), sfx.c_str(), sfx.c_str());
                out += b;
            }
            out += "\n";
        }
        double sum[4] = {}, sum2[4] = {};
        size_t cnt[4] = {};
        // projection profiles per plane; mean per column/row of that plane only
        std::vector<double> colS((size_t)ww * nPl, 0.0), rowS((size_t)hh * nPl, 0.0);
        std::vector<uint32_t> colN((size_t)ww * nPl, 0), rowN((size_t)hh * nPl, 0);
        for (int y = 0; y < hh; y++) {
            const float* row = &d->data[((size_t)(y0 + y) * W + x0) * C];
            for (int x = 0; x < ww; x++)
                for (int c = 0; c < C; c++) {
                    int p = cfa ? cfaChannelAt(*d, x0 + x, y0 + y) : std::min(c, nPl - 1);
                    double v = row[(size_t)x * C + c];
                    if (!std::isfinite(v)) continue;
                    sum[p] += v; sum2[p] += v * v; cnt[p]++;
                    colS[(size_t)p * ww + x] += v; colN[(size_t)p * ww + x]++;
                    rowS[(size_t)p * hh + y] += v; rowN[(size_t)p * hh + y]++;
                }
        }
        snprintf(b, sizeof b, "%s\t%s\t%d", d->name.c_str(),
                 d->path.empty() ? "-" : d->path.c_str(), d->seqIndex);
        out += b;
        for (int p = 0; p < nPl; p++) {
            double m = cnt[p] ? sum[p] / (double)cnt[p] : 0.0;
            double var = cnt[p] > 1
                ? std::max(0.0, sum2[p] / (double)cnt[p] - m * m) * (cnt[p] / (cnt[p] - 1.0)) : 0.0;
            auto profCv = [&](const std::vector<double>& S, const std::vector<uint32_t>& N,
                              size_t off, int n) {
                double ps = 0, ps2 = 0; int pc = 0;
                for (int i = 0; i < n; i++) {
                    if (!N[off + i]) continue;
                    double pm = S[off + i] / N[off + i];
                    ps += pm; ps2 += pm * pm; pc++;
                }
                if (pc < 2 || fabs(m) < 1e-12) return 0.0;
                double pmn = ps / pc;
                double pv = std::max(0.0, ps2 / pc - pmn * pmn) * (pc / (pc - 1.0));
                return sqrt(pv) / fabs(m) * 100.0;
            };
            snprintf(b, sizeof b, "\t%.6g\t%.6g\t%.4g\t%.4g", m, sqrt(var),
                     profCv(colS, colN, (size_t)p * ww, ww),
                     profCv(rowS, rowN, (size_t)p * hh, hh));
            out += b;
        }
        out += "\n";
    }
    if (out.empty()) { toast("no resident frames to tabulate", true); return; }
    ImGui::SetClipboardText(out.c_str());
    snprintf(b, sizeof b, "copied %d frame row(s) (%s)%s%s",
             (int)fr.size() - skipped, roiUsed ? "selected ROI" : "whole frame",
             skipped ? " - " : "", skipped ? (std::to_string(skipped) + " not resident").c_str() : "");
    toast(b, skipped != 0);
}

// One side's temporal numbers, wherever they came from. The A/B table takes
// both sides through this, so a server-measured A and a locally computed B land
// in the same rows under the same headers - and "there is no number here" is a
// stated reason instead of a blank.
struct AbTemporal {
    bool valid = false, isStack = false, fromServer = false;
    double sigT = 0, sigS = 0, sigTot = 0;
    int frames = 0, expected = 0;
    const std::vector<float>* idx = nullptr;
    const std::vector<float>* mean = nullptr;
    const char* note = "";
};
static AbTemporal abTemporalOf(const ImageDoc* d, const App::TemporalState& T,
                               const App::ServerTemporal& S) {
    AbTemporal o;
    if (!d || d->seqId == 0) { o.note = "not a stack"; return o; }
    o.isStack = true;
    if (const App::SeqInfo* si = seqInfo(d->seqId)) o.expected = si->expectedFrames;
    if (S.valid && S.seqId == d->seqId) {          // server-measured wins: it saw
        o.valid = true; o.fromServer = true;       // every frame, not the resident ones
        o.sigT = S.tempNoise; o.sigS = S.fixedPattern; o.sigTot = S.totalNoise;
        o.frames = S.frames;
        o.idx = &S.idx; o.mean = &S.frameMean;
        return o;
    }
    if (T.valid && T.seqId == d->seqId) {
        o.valid = true;
        o.sigT = T.tempNoise; o.sigS = T.fixedPattern; o.sigTot = T.totalNoise;
        o.frames = T.frames;
        o.idx = &T.idx; o.mean = &T.frameMean;
        return o;
    }
    o.note = S.pending && S.seqId == d->seqId ? "measuring on the server..."
                                              : "needs >= 2 loaded frames";
    return o;
}

// A difference only means something when both sides measured the same quantity.
// A 1-channel stack's sigma_t and a 3-channel one's are not the same number, so
// the delta columns stay empty rather than subtract two unlike things
// (docs/ab-stats-plan.md 5).
static bool abDeltaMeaningful(const ImageDoc* a, const ImageDoc* b) {
    return a && b && a->ch == b->ch;
}

// The A/B temporal view: rows are quantities, columns are A | B | delta | delta%.
// (docs/ab-stats-plan.md 4.) The sign is A-B, matching the difference image.
static void drawTemporalAB(ImageDoc* im, ImageDoc* Bim) {
    recomputeTemporalIfNeeded(im, app.temporal[0]);
    // NOT throttled: temporal is keyed on the STACK and its ROI, so stepping
    // frames never invalidates it (see the A4 check in --abstats-selftest).
    recomputeTemporalIfNeeded(Bim, app.temporal[1]);
    AbTemporal A = abTemporalOf(im, app.temporal[0], app.srvTemporal);
    AbTemporal B = abTemporalOf(Bim, app.temporal[1], app.srvTemporalB);

    const std::string uA = abValueUnit(im->dtype), uB = abValueUnit(Bim->dtype);
    // the UNIT is DN on both sides; the STORAGE can still differ, and a delta
    // between an f32 and a u16 stack is worth a warning even so
    const bool dtypeMix = im->dtype != Bim->dtype;
    const std::string unit = uA == uB ? uA : (uA + "/" + uB);
    const bool canDelta = abDeltaMeaningful(im, Bim);

    ImGui::Text("Temporal");
    ImGui::SameLine();
    ImGui::TextDisabled("[%s, %s]", app.temporal[0].roiUsed ? "selected ROI" : "whole image",
                        A.fromServer || B.fromServer ? "server + local" : "local");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy per-frame stats (A)")) copyPerFrameStats(im->seqId);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One row per resident frame of A's stack, tab-separated.");
    // B's server aggregate is never fired automatically - one explicit press,
    // one remote job (docs/ab-stats-plan.md 4).
    if (const App::SeqInfo* sb = seqInfo(Bim->seqId))
        if (serverComputes(*sb)) {
            ImGui::SameLine();
            bool busy = app.srvTemporalB.pending && app.srvTemporalB.seqId == Bim->seqId;
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(busy ? "Measuring B..." : "Measure B"))
                requestServerTemporal(Bim->seqId, app.srvTemporalB);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Run the server-side aggregate over B's stack.\n"
                                  "It is never started on its own: B follows A, and\n"
                                  "an automatic B measurement would double every\n"
                                  "remote job you did not ask for.");
        }
    ImGui::Separator();

    char hA[64], hB[64];
    auto frames = [](char* buf, size_t n, const char* side, const AbTemporal& s) {
        if (!s.isStack) snprintf(buf, n, "%s", side);
        else if (s.expected > s.frames) snprintf(buf, n, "%s (n=%d/%d)", side, s.frames, s.expected);
        else snprintf(buf, n, "%s (n=%d/%d)", side, s.frames, s.frames);
    };
    frames(hA, sizeof hA, "A", A);
    frames(hB, sizeof hB, "B", B);
    std::string cQ = "quantity [" + unit + "]";
    std::string cD = "delta A-B [" + unit + "]";

    if (ImGui::BeginTable("abtemporal", 5, ImGuiTableFlags_SizingFixedFit |
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn(cQ.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(hA, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(hB, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(cD.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("delta [%]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        // isPct: the quantity is already a percentage, so a relative difference
        // of a percentage is meaningless - the absolute one, in points, is the
        // honest answer. Kept general; today's three rows are all in DN.
        auto row = [&](const char* k, double a, double b, bool isPct) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            A.valid ? textNum("%.6g", a) : textNumStr("-");
            ImGui::TableNextColumn();
            if (B.valid) textNum("%.6g", b);
            else textNumStr(B.isStack ? "-" : "- (not a stack)");
            ImGui::TableNextColumn();
            if (A.valid && B.valid && canDelta) textNum("%.6g", a - b); else textNumStr("");
            ImGui::TableNextColumn();
            if (!A.valid || !B.valid || !canDelta) textNumStr("");
            // a quantity that is ALREADY a percentage has no meaningful relative
            // difference: points are the honest answer. (Today's three rows are
            // all in DN; the rule is here so the next row cannot get it wrong.)
            else if (isPct) { char t[48]; snprintf(t, sizeof t, "%.4g pt", a - b); textNumStr(t); }
            else if (a == 0) textNumStr("-  (A = 0)");
            else textNum("%.4g", (a - b) / fabs(a) * 100.0);
        };
        row("temporal noise (sigma_t)", A.sigT, B.sigT, false);
        row("fixed pattern (sigma_s)", A.sigS, B.sigS, false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("spatial std of the time-averaged frame;\n"
                              "includes scene detail unless the ROI is flat");
        row("total (quadrature)", A.sigTot, B.sigTot, false);
        ImGui::EndTable();
    }
    if (!A.valid || !B.valid)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "A: %s   |   B: %s",
                           A.valid ? "ok" : A.note, B.valid ? "ok" : B.note);
    if (dtypeMix)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "dtype mismatch (A %s, B %s): both sides are DN, but the "
                           "delta spans two storage classes", im->dtype.c_str(),
                           Bim->dtype.c_str());
    if (!canDelta)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "channel counts differ (A %dch, B %dch): no delta - the two "
                           "columns are not the same quantity", im->ch, Bim->ch);
    if (A.fromServer || B.fromServer)
        ImGui::TextDisabled("source: A %s, B %s", A.fromServer ? "server" : "local",
                            B.fromServer ? "server" : "local");

    // ---- per-frame mean over time: A solid, B dashed, one shared axis ----
    if (!A.mean || A.mean->empty()) return;
    float mn = FLT_MAX, mx = -FLT_MAX, fx0 = FLT_MAX, fx1 = -FLT_MAX;
    auto span = [&](const AbTemporal& s) {
        if (!s.mean || s.mean->empty()) return;
        for (float v : *s.mean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        if (s.idx && !s.idx->empty()) {
            fx0 = std::min(fx0, s.idx->front()); fx1 = std::max(fx1, s.idx->back());
        }
    };
    span(A); span(B);
    if (fx0 > fx1) { fx0 = 0; fx1 = 1; }
    char yl[128];
    if (im->dtype != Bim->dtype)
        snprintf(yl, sizeof yl, "ROI mean value (%s)  -  A %s / B %s, DTYPE MISMATCH",
                 unit.c_str(), im->dtype.c_str(), Bim->dtype.c_str());
    else snprintf(yl, sizeof yl, "ROI mean value (%s)", unit.c_str());
    const ImU32 CURVE = IM_COL32(105, 220, 130, 255);

    auto curve = [&](const PlotRect& tp, const AbTemporal& s, bool dashed) {
        if (!tp.ok || !s.mean || s.mean->size() < 2 || !s.idx) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::vector<ImVec2> pts;
        pts.reserve(s.mean->size());
        for (size_t i = 0; i < s.mean->size() && i < s.idx->size(); i++)
            pts.push_back(tp.at((*s.idx)[i], (*s.mean)[i]));
        if (pts.size() < 2) return;
        if (dashed) addDashedPolyline(dl, pts.data(), (int)pts.size(), CURVE, 1.5f,
                                      5 * app.uiScale, 4 * app.uiScale);
        else dl->AddPolyline(pts.data(), (int)pts.size(), CURVE, 0, 1.5f);
    };
    auto marker = [&](const PlotRect& tp, const ImageDoc* d) {
        if (!tp.ok || !d) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float mxp = tp.at((float)d->seqIndex, tp.ymin).x;
        dl->AddLine(ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y), IM_COL32(255, 184, 77, 200));
    };
    const float minSide = AB_MIN_SIDE * app.uiScale;
    bool side = abSideBySide();
    bool tooNarrow = side && ImGui::GetContentRegionAvail().x < minSide;
    if (tooNarrow) side = false;
    const std::string aLabel = abDocLabel(im), bLabel = abDocLabel(Bim);
    float tAvail = ImGui::GetContentRegionAvail().y
                 - (ImGui::GetFontSize() * 3 + 12 * app.uiScale)
                 - (tooNarrow ? abNarrowNoteH() : 0.0f)
                 - (side ? 0.0f : abLegendH(aLabel, bLabel, false));
    if (side) tAvail -= ImGui::GetTextLineHeight() + 6 * app.uiScale;
    float plotH = std::max(tAvail, 70.0f * app.uiScale);
    const char* xlab = "frame number (index in sequence)";
    if (!side) {
        PlotRect tp = beginPlot(xlab, yl, fx0, fx1, mn, mx, true, false, plotH);
        if (tp.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            curve(tp, A, false);
            curve(tp, B, true);
            marker(tp, im);
            dl->PopClipRect();
        }
        drawABLegendRow(aLabel, bLabel);
    } else {
        const ImGuiStyle& st = ImGui::GetStyle();
        float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
        float childH = plotH + ImGui::GetFontSize() * 3 + 12 * app.uiScale
                     + ImGui::GetTextLineHeight() + 8 * app.uiScale;
        ImGui::BeginChild("##tempA", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("A", aLabel);
        {
            PlotRect tp = beginPlot(xlab, yl, fx0, fx1, mn, mx, true, false, plotH);
            if (tp.ok) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(tp.p0, tp.p1, true);
                curve(tp, A, false); marker(tp, im);
                dl->PopClipRect();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##tempB", ImVec2(half, childH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawABBand("B", abDocLabel(Bim));
        {   // same limits as A's plot, by construction
            PlotRect tp = beginPlot(xlab, yl, fx0, fx1, mn, mx, true, false, plotH);
            if (tp.ok) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(tp.p0, tp.p1, true);
                curve(tp, B, true); marker(tp, Bim);
                dl->PopClipRect();
            }
        }
        ImGui::EndChild();
    }
    if (tooNarrow) abNarrowNote();
}

static void drawPanelTemporal() {
    // a browser-fired aggregate (files nobody opened) owns the panel until
    // dismissed - it is the freshest thing the user asked for and has no
    // current image to attach to
    if (app.srvTemporal.seqId == -2) { drawBrowseTemporal(); return; }
    ImageDoc* im = cur();
    // Compare on, and A is a stack: the A|B|delta|delta% table. A B that is not
    // a stack still gets its column, saying exactly that. With no B at all the
    // panel is exactly what it always was.
    if (ImageDoc* Bim = abStatsB())
        if (im && im->seqId != 0) { drawTemporalAB(im, Bim); return; }
    if (im && im->seqId != 0) {
        App::SeqInfo* si = seqInfo(im->seqId);
        if (drawServerTemporal(si)) {         // remote stack, computed on the server
            // the per-frame table still works here - over the frames that are
            // resident locally; the copy says how many that is
            if (ImGui::SmallButton("Copy per-frame stats"))
                copyPerFrameStats(im->seqId);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("One row per RESIDENT frame (fetched so far),\n"
                                  "tab-separated for Excel.");
            return;
        }
        recomputeTemporalIfNeeded(im, app.temporal[0]);
        const App::TemporalState& T = app.temporal[0];
        ImGui::Text("Temporal");
        ImGui::SameLine();
        // for a remote stack whose server measure failed, be explicit that this
        // is a PARTIAL local computation, and of how many
        int expected = si ? si->expectedFrames : 0;
        if (expected > T.frames)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[local, %d of %d frames]",
                               T.frames, expected);
        else
            ImGui::TextDisabled("[local, %d frames, %s]", T.frames,
                                T.roiUsed ? "selected ROI" : "whole image");
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy per-frame stats"))
            copyPerFrameStats(im->seqId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("One row per resident frame, tab-separated for Excel:\n"
                              "file, path, per-plane mean and sigma [DN], and the\n"
                              "H/V projection non-uniformity [%% of mean].\n"
                              "Uses the selected ROI if there is one.");
        ImGui::Separator();
        if (!T.valid) {
            ImGui::TextDisabled("need >= 2 loaded frames of equal size");
        } else {
            if (ImGui::BeginTable("temporalstats", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, numColW());
                auto row = [](const char* k, double v) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
                    ImGui::TableNextColumn(); textNum("%.6g", v);
                };
                row("temporal noise (sigma_t)", T.tempNoise);
                row("fixed pattern (sigma_s)", T.fixedPattern);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("spatial std of the time-averaged frame;\n"
                                      "includes scene detail unless the ROI is flat");
                row("total (quadrature)", T.totalNoise);
                ImGui::EndTable();
            }
            // per-frame mean over time
            float mn = FLT_MAX, mx = -FLT_MAX;
            for (float v : T.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
            float fx0 = T.idx.empty() ? 0 : T.idx.front(), fx1 = T.idx.empty() ? 1 : T.idx.back();
            char yl[64];
            snprintf(yl, sizeof yl, "ROI mean value (%s)", abValueUnit(im->dtype).c_str());
            float tAvail = ImGui::GetContentRegionAvail().y
                         - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
            PlotRect tp = beginPlot("frame number (index in sequence)", yl,
                                    fx0, fx1, mn, mx, true, false,
                                    std::max(tAvail, 70.0f * app.uiScale));
            if (tp.ok) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(tp.p0, tp.p1, true);
                for (size_t i = 1; i < T.frameMean.size(); i++)
                    dl->AddLine(tp.at(T.idx[i - 1], T.frameMean[i - 1]),
                                tp.at(T.idx[i], T.frameMean[i]),
                                IM_COL32(105, 220, 130, 255), 1.5f);
                if (ImageDoc* c2 = cur()) {   // marker for the frame on screen
                    float mxp = tp.at((float)c2->seqIndex, tp.ymin).x;
                    dl->AddLine(ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y),
                                IM_COL32(255, 184, 77, 200));
                }
                dl->PopClipRect();
            }
        }
    }

}

// Basic per-ROI statistics (host-computed, always on). Detailed measurements
// live in the Analysis window; this is the "at a glance" layer.
struct RoiStat { double mean, sd, mn, mx; size_t n; bool valid; };
static RoiStat roiBasicStatsUncached(const ImageDoc& im, int rx, int ry, int rw, int rh, int chSel) {
    RoiStat s{ 0, 0, 0, 0, 0, false };
    rx = std::clamp(rx, 0, im.w); ry = std::clamp(ry, 0, im.h);
    rw = std::clamp(rw, 0, im.w - rx); rh = std::clamp(rh, 0, im.h - ry);
    if (rw < 1 || rh < 1) return s;
    bool cfa = im.ch == 1 && im.cfa != 0;
    size_t total = (size_t)rw * rh;
    size_t step = std::max<size_t>(1, total / 200000);      // cap the cost per ROI
    double sum = 0, sum2 = 0, mn = DBL_MAX, mx = -DBL_MAX;
    size_t n = 0;
    for (size_t p = 0; p < total; p += step) {
        int x = rx + (int)(p % rw), y = ry + (int)(p / rw);
        const float* src = &im.data[((size_t)y * im.w + x) * im.ch];
        if (cfa) {
            if (chSel >= 0 && cfaChannelAt(im, x, y) != chSel) continue;
            float v = src[0];
            if (!std::isfinite(v)) continue;
            sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
        } else if (chSel >= 0 && chSel < im.ch) {
            float v = src[chSel];
            if (!std::isfinite(v)) continue;
            sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
        } else {
            for (int c = 0; c < im.ch; c++) {
                float v = src[c];
                if (!std::isfinite(v)) continue;
                sum += v; sum2 += (double)v * v; mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); n++;
            }
        }
    }
    if (!n) return s;
    s.mean = sum / n;
    double var = sum2 / n - s.mean * s.mean;
    s.sd = sqrt(var > 0 ? var : 0);
    s.mn = mn; s.mx = mx; s.n = n; s.valid = true;
    return s;
}

// The ROI table is drawn every frame; recomputing hundreds of thousands of
// samples per row each time made the whole UI (typing included) crawl.
struct RoiStatCacheEntry {
    uint64_t uid; int dataRev, x, y, w, h, ch, cfa, cfaPat;
    RoiStat s;
};
static RoiStat roiBasicStats(const ImageDoc& im, int rx, int ry, int rw, int rh, int chSel) {
    // Hashed, not scanned: the table asks once per row, so a linear cache made the
    // whole panel O(ROIs^2) - measured 1.37 ms/frame at 400 ROIs against 0.34 ms
    // at none. The key still carries every input that changes the numbers.
    static std::unordered_map<uint64_t, RoiStatCacheEntry> cache;
    uint64_t k = im.uid * 1000003ull;
    k = k * 31 + (uint64_t)(uint32_t)im.dataRev;
    k = k * 31 + (uint64_t)(uint32_t)rx;   k = k * 31 + (uint64_t)(uint32_t)ry;
    k = k * 31 + (uint64_t)(uint32_t)rw;   k = k * 31 + (uint64_t)(uint32_t)rh;
    k = k * 31 + (uint64_t)(uint32_t)chSel;
    k = k * 31 + (uint64_t)(uint32_t)im.cfa;
    k = k * 31 + (uint64_t)(uint32_t)im.cfaPattern;
    auto it = cache.find(k);
    if (it != cache.end()) {
        const RoiStatCacheEntry& e = it->second;
        if (e.uid == im.uid && e.dataRev == im.dataRev && e.x == rx && e.y == ry &&
            e.w == rw && e.h == rh && e.ch == chSel && e.cfa == im.cfa && e.cfaPat == im.cfaPattern)
            return e.s;                    // hash collisions must not return wrong numbers
    }
    RoiStat s = roiBasicStatsUncached(im, rx, ry, rw, rh, chSel);
    // room for every ROI of a few frames, so scrubbing does not thrash
    size_t cap = std::max<size_t>(64, (app.anns.size() + 1) * 8);
    if (cache.size() > cap) cache.clear();
    cache[k] = { im.uid, im.dataRev, rx, ry, rw, rh, chSel, im.cfa, im.cfaPattern, s };
    return s;
}

static void drawPanelRois() {
    ImageDoc* im = cur();
    if (!im) { ImGui::TextDisabled("no image"); return; }

    // channel selector keeps the table to a fixed, readable width
    bool cfa = im->ch == 1 && im->cfa != 0;
    static const char* CFA_SEL[5] = { "all", "R", "Gr", "Gb", "B" };
    static const char* CH_SEL[5] = { "all", "ch0", "ch1", "ch2", "ch3" };
    int maxSel = cfa ? 4 : std::min(im->ch, 4);
    app.roiChannel = std::clamp(app.roiChannel, -1, maxSel - (cfa ? 0 : 1));
    const char* curSel = app.roiChannel < 0 ? "all"
                                            : (cfa ? CFA_SEL[app.roiChannel + 1] : CH_SEL[app.roiChannel + 1]);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    if (ImGui::BeginCombo("channel", curSel)) {
        if (ImGui::Selectable("all", app.roiChannel < 0)) app.roiChannel = -1;
        for (int c = 0; c < maxSel; c++)
            if (ImGui::Selectable(cfa ? CFA_SEL[c + 1] : CH_SEL[c + 1], app.roiChannel == c))
                app.roiChannel = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear all")) { app.anns.clear(); app.selectedAnn = 0; app.annRev++; }
    ImGui::SameLine();
    ImGui::TextDisabled("(R: drag = ROI, P: click = pin)");
    ImGui::Separator();

    const ImGuiTableFlags TF = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable;
    // fill the window instead of a fixed 9-text-line box: rows are frame-height,
    // so the old constant showed 4 ROIs no matter how large the window was
    const float editH = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginTable("roitable", 8, TF,
                          ImVec2(0, -(editH + ImGui::GetStyle().ItemSpacing.y * 2 + 1.0f)))) {
        // stretch weights, not fixed widths: four fixed numeric columns would eat
        // the whole table and collapse name/region to a few pixels
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("region", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("std", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::TableHeadersRow();

        // "All" is a real, selectable entry so the table is never empty and the
        // measurement target is always explicit
        {
            RoiStat s = roiBasicStats(*im, 0, 0, im->w, im->h, app.roiChannel);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (ImGui::Selectable("All (whole image)", app.selectedAnn <= 0,
                                  ImGuiSelectableFlags_SpanAllColumns))
                app.selectedAnn = 0;
            ImGui::TableNextColumn(); ImGui::Text("%dx%d", im->w, im->h);
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mean) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.sd) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mn) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mx) : ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
        }
        int removeId = -1;
        // Only the rows you can see. Submitting all of them cost 1.35 ms/frame at
        // 400 ROIs (0.33 ms at none) - and each row also asked for its statistics,
        // so scrolled-away ROIs were being measured for nobody.
        ImGuiListClipper clipper;
        clipper.Begin((int)app.anns.size());
        while (clipper.Step())
        for (int ai = clipper.DisplayStart; ai < clipper.DisplayEnd; ai++) {
            App::Ann& a = app.anns[ai];
            ImGui::PushID(a.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##vis", &a.visible)) app.annRev++;
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ANN_COLORS[a.color & 7]));
            bool sel = app.selectedAnn == a.id;
            if (ImGui::Selectable(a.label.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns))
                app.selectedAnn = a.id;
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (a.type == 0) ImGui::Text("%dx%d @%d,%d", a.w, a.h, a.x, a.y);
            else if (im->cfa && a.x < im->w && a.y < im->h)
                ImGui::Text("%d,%d [%s]", a.x, a.y, CFA_CH_NAMES[cfaChannelAt(*im, a.x, a.y)]);
            else ImGui::Text("%d,%d", a.x, a.y);
            if (a.type == 0) {                          // ROI: area statistics
                RoiStat s = roiBasicStats(*im, a.x, a.y, a.w, a.h, app.roiChannel);
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mean) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.sd) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mn) : ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); s.valid ? ImGui::Text("%.6g", s.mx) : ImGui::TextDisabled("-");
            } else {                                     // POI: the pixel value itself
                bool inside = a.x >= 0 && a.y >= 0 && a.x < im->w && a.y < im->h;
                int c0 = app.roiChannel < 0 ? 0 : std::min(app.roiChannel, im->ch - 1);
                ImGui::TableNextColumn();
                if (inside) ImGui::TextUnformatted(fmtVal(im->sample(a.x, a.y, c0), im->dtype).c_str());
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeId = a.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (removeId >= 0) deleteAnn(removeId);
    }

    // editor block: fixed height so the window never shrinks with the selection
    ImGui::Separator();
    ImGui::BeginChild("roiedit", ImVec2(0, editH));
    if (App::Ann* sel = findAnn(app.selectedAnn)) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", sel->label.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rename", buf, sizeof buf))
            sel->label = buf;                       // live update is cheap...
        if (ImGui::IsItemDeactivatedAfterEdit())
            app.annRev++;                           // ...but re-analyze only on commit
        if (sel->type == 0) {
            // Commit on Enter/focus-loss, not per keystroke. The caches key on the
            // resolved rect, so applying "5", "51", "512" as you type would rescan
            // the histogram, the ROI stats and every analyzer three times - that is
            // what made typing in these fields feel like the app had died.
            static int xywh[4] = {};
            static bool xywhEditing = false;
            static int xywhId = -1;
            if (!xywhEditing || xywhId != sel->id) {   // follow the ROI until edited
                xywh[0] = sel->x; xywh[1] = sel->y; xywh[2] = sel->w; xywh[3] = sel->h;
                xywhId = sel->id;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt4("##xywh", xywh);      // full width: a visible label would clip
            xywhEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                sel->x = std::clamp(xywh[0], 0, im->w - 1);
                sel->y = std::clamp(xywh[1], 0, im->h - 1);
                sel->w = std::clamp(xywh[2], 1, im->w - sel->x);
                sel->h = std::clamp(xywh[3], 1, im->h - sel->y);
                app.annRev++;
            }
        } else {
            int v[2] = { sel->x, sel->y };
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt2("##pinxy", v)) {
                sel->x = std::clamp(v[0], 0, im->w - 1);
                sel->y = std::clamp(v[1], 0, im->h - 1);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) app.annRev++;
            ImGui::TextDisabled("X / Y: select this pin's row / column");
        }
    } else {
        ImGui::TextDisabled("target: whole image (%dx%d, %s)", im->w, im->h, im->dtype.c_str());
        ImGui::TextDisabled("drag a ROI, or X / Y for the row / column under the cursor");
    }
    ImGui::EndChild();
}

// Unit resolution for grid rows: every numeric row gets a unit column entry.
// Keys that can know their unit carry it in the name ("snr_db", "*_pct",
// "mtf50 (cy/px)" - the documented plugin convention); image-value rows are in
// the FILE's units, which only integer dtypes let us name honestly (DN). For
// float data the dtype itself is shown instead of a unit we would be inventing
// - a float .npy may hold reflectance, electrons, or anything (same rule the
// Inspector applies to its value columns).
static std::string unitForAnalysisKey(const std::string& key, const std::string& dtype) {
    auto ends = [&](const char* s) {
        size_t n = strlen(s);
        return key.size() >= n && key.compare(key.size() - n, n, s) == 0;
    };
    bool isInt = !dtype.empty() && (dtype[0] == 'u' || dtype[0] == 'i' || dtype == "bool");
    std::string img = isInt ? "DN" : dtype;            // the file's own value unit
    if (ends("snr_db"))                              return "dB";
    if (ends("_pct"))                                return "%";
    if (key.find("(cy/px)") != std::string::npos)    return "cy/px";
    if (key == "sfr@nyquist" || key == "finite ratio") return "ratio";
    if (ends("_deg"))                                return "deg";
    if (key == "lines_used")                         return "lines";
    if (key == "pixels")                             return "px";
    if (ends(".entropy"))                            return "bit";
    if (ends(".var"))                                return img + "^2";
    if (key == "varlap" || key == "tenengrad" || key == "grad_mean")
        return "a.u.";                               // relative-only metrics
    if (ends(".mean") || ends(".std") || ends(".noise") || ends(".min") ||
        ends(".max") || ends(".p1") || ends(".p50") || ends(".p99"))
        return img;
    return "";     // text rows (method / note) and keys we cannot vouch for
}

// The number the user came for, per analyzer: mtf50, prnu%, the noise floor.
// Host knowledge until an ABI v3 lets plugins declare it - same trade-off as
// the question groups in the Measure menu, and just as additive.
static const char* analyzerHeadlineSuffix(const std::string& name) {
    if (name == "noise/floor")         return ".noise";
    if (name == "uniformity/prnu-fpn") return ".prnu_pct";
    if (name == "sharpness/gradient")  return "tenengrad";
    if (name == "iso12233/e-sfr")      return "mtf50 (cy/px)";
    return nullptr;                    // stats/moments: all rows are peers
}

static void drawPanelAnalysis() {
    ImageDoc* im = cur();
    if (im && !plugin_host::analyzers().empty()) {
        const auto& anas = plugin_host::analyzers();
        app.anaSel = std::clamp(app.anaSel, 0, (int)anas.size() - 1);
        std::string curCat, curItem;
        splitAnalyzerName(anas[app.anaSel].name, curCat, curItem);
        // reserve the measured tail (Run + auto + target note) before splitting
        // the rest between the two combos, otherwise the note is clipped away
        std::vector<const App::Ann*> rois;
        for (const auto& a : app.anns)
            if (a.type == 0 && a.visible) rois.push_back(&a);
        char tgt[48];
        if (rois.empty()) snprintf(tgt, sizeof tgt, "| whole image");
        else snprintf(tgt, sizeof tgt, "| %d ROI(s)", (int)rois.size());
        const ImGuiStyle& st = ImGui::GetStyle();
        float tailW = ImGui::CalcTextSize("Run").x + st.FramePadding.x * 2 + st.ItemSpacing.x
                    + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("auto").x
                    + st.ItemSpacing.x + ImGui::CalcTextSize(tgt).x + st.ItemSpacing.x;
        float comboW = std::max(ImGui::GetFontSize() * 5.0f,
                                (ImGui::GetContentRegionAvail().x - tailW - st.ItemSpacing.x) * 0.5f);
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##anacat", curCat.c_str())) {   // level 1: category
            std::vector<std::string> seen;
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                bool dup = false;
                for (const auto& s : seen) if (s == c) { dup = true; break; }
                if (dup) continue;
                seen.push_back(c);
                if (ImGui::Selectable(c.c_str(), c == curCat) && c != curCat)
                    for (int j = 0; j < (int)anas.size(); j++) {
                        std::string c2, n2;
                        splitAnalyzerName(anas[j].name, c2, n2);
                        if (c2 == c) { app.anaSel = j; break; }
                    }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##anaitem", curItem.c_str())) { // level 2: measurement
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                if (c != curCat) continue;
                if (ImGui::Selectable(n.c_str(), i == app.anaSel)) app.anaSel = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool runClicked = ImGui::Button("Run") || app.anaRunRequest;
        app.anaRunRequest = false;
        ImGui::SameLine();
        ImGui::Checkbox("auto", &app.anaAuto);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("re-run when the image, ROI set, display range or CFA layout\n"
                              "changes. Off: results keep the run they came from and the\n"
                              "status line says so when the inputs have moved on.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tgt);

        auto& ana = app.ana;
        // every input makeFrame() hands the plugin belongs in the key, or the
        // grid silently keeps showing numbers from the previous settings
        bool stale = ana.uid != im->uid || ana.plugin != app.anaSel || ana.rev != app.annRev ||
                     ana.dataRev != im->dataRev || ana.black != effBlack(*im) ||
                     ana.white != effWhite(*im) || ana.cfa != im->cfa ||
                     ana.cfaPattern != im->cfaPattern;
        bool ranNow = runClicked || (app.anaAuto && stale && !app.annBusy);
        if (ranNow) {
            double runT0 = glfwGetTime();
            ana.cols.clear(); ana.keys.clear(); ana.vals.clear(); ana.series.clear(); ana.err.clear();
            ana.colColor.clear(); ana.units.clear(); ana.headline.clear();
            if (im->preview) promotePreview(im);   // measuring it = keeping it
            ana.img = im; ana.uid = im->uid; ana.plugin = app.anaSel; ana.rev = app.annRev;
            ana.dataRev = im->dataRev; ana.black = effBlack(*im); ana.white = effWhite(*im);
            ana.cfa = im->cfa; ana.cfaPattern = im->cfaPattern;
            auto runOne = [&](const psRect* roi, const std::string& colLabel, int colorIdx) {
                std::vector<std::pair<std::string, std::string>> rows;
                psFrame f = makeFrame(*im);
                char err[256] = { 0 };
                int32_t rc;
                if (anas[app.anaSel].isV2) {
                    AnaEmit2Ctx ectx = { &rows, &ana, (int)ana.cols.size(), colorIdx };
                    psAnalyzeSink2 sink = { &ectx, ana2Number, ana2Text, ana2Series, {} };
                    rc = anas[app.anaSel].v2.analyze(&f, roi, &sink, err, sizeof err);
                } else {
                    psAnalyzeSink sink = { &rows, anaEmitNumber, anaEmitText };
                    rc = anas[app.anaSel].v1.analyze(&f, roi, &sink, err, sizeof err);
                }
                if (rc != 0) {
                    ana.err += colLabel + ": " + (err[0] ? err : "failed") + "\n";
                    rows.clear();
                }
                ana.cols.push_back(colLabel);
                ana.colColor.push_back(colorIdx);   // header wears the ROI's color
                for (auto& r : ana.vals) r.resize(ana.cols.size());
                for (const auto& kv : rows) {
                    if (kv.first == "roi") continue;        // grid header already says which ROI
                    int rowIdx = -1;
                    for (int k = 0; k < (int)ana.keys.size(); k++)
                        if (ana.keys[k] == kv.first) { rowIdx = k; break; }
                    if (rowIdx < 0) {
                        ana.keys.push_back(kv.first);
                        ana.vals.emplace_back(ana.cols.size());
                        rowIdx = (int)ana.keys.size() - 1;
                    }
                    ana.vals[rowIdx][ana.cols.size() - 1] = kv.second;
                }
            };
            if (rois.empty()) {
                runOne(nullptr, "whole", -1);
            } else {
                for (const App::Ann* a : rois) {
                    // annotations are global across images: clamp to THIS image before
                    // handing the rect to a plugin (the ABI does not promise in-bounds)
                    int rx = std::clamp(a->x, 0, im->w);
                    int ry = std::clamp(a->y, 0, im->h);
                    int rw = std::clamp(a->w, 0, im->w - rx);
                    int rh = std::clamp(a->h, 0, im->h - ry);
                    if (rw < 1 || rh < 1) {
                        ana.cols.push_back(a->label + " (off)");
                        ana.colColor.push_back(a->color & 7);
                        for (auto& r : ana.vals) r.resize(ana.cols.size());
                        continue;
                    }
                    psRect rr = { (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh };
                    runOne(&rr, a->label, a->color & 7);
                }
            }
            ana.runMs = (float)((glfwGetTime() - runT0) * 1000.0);
            // per-row presentation, derived once here so the draw loop below
            // only reads cached strings (no per-frame string building)
            {
                const char* hl = analyzerHeadlineSuffix(anas[app.anaSel].name);
                size_t hn = hl ? strlen(hl) : 0;
                for (const auto& k : ana.keys) {
                    ana.units.push_back(unitForAnalysisKey(k, im->dtype));
                    ana.headline.push_back(hl && k.size() >= hn &&
                                           k.compare(k.size() - hn, hn, hl) == 0);
                }
            }
            {   // Provenance, built once per run and shown verbatim until the
                // next one: where this ran, on what, over which target, when,
                // and how long. [local] is a promise, not decoration - numbers
                // in this grid are never quietly recomputed anywhere else.
                char ts[32] = "";
                time_t now = time(nullptr);
                if (struct tm* lt = localtime(&now))
                    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
                char fi[40] = "";
                if (im->seqId != 0) {
                    std::vector<int> fr = framesOfSeq(im->seqId);
                    int pos = 0;
                    for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
                    snprintf(fi, sizeof fi, "  frame %d/%d", pos + 1, (int)fr.size());
                }
                char pv[384];
                snprintf(pv, sizeof pv,
                         "[local] %s%s  %dx%d %s  |  %s  |  range %g..%g  |  %s  |  %.1f ms",
                         im->name.c_str(), fi, im->w, im->h, im->dtype.c_str(),
                         rois.empty() ? "whole image" : tgt + 2,   // tgt = "| N ROI(s)"
                         ana.black, ana.white, ts, ana.runMs);
                ana.prov = pv;
            }
        }
        // Run-state line: one line, every state, constant height - the grid
        // below must not jump when a result appears or goes stale.
        if (ana.img != im || ana.prov.empty()) {
            ImGui::TextDisabled("not measured yet  -  Run, M, or the Measure menu");
        } else {
            ImGui::TextDisabled("%s", ana.prov.c_str());
            if (stale && !ranNow && !app.anaAuto) {
                // amber, matching the temporal panel's partial-data warning:
                // old numbers are still shown (they were true when measured),
                // but they must not be read as describing the current inputs
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                                   "|  inputs changed - Run (M)");
            }
        }
        // results toolbar: a constant row (disabled while empty, never absent,
        // so the grid below sits at a fixed height in every state)
        ImGui::BeginDisabled(ana.img != im || ana.keys.empty());
        if (ImGui::Button("Copy table (TSV)")) {
            // provenance rides along as a # comment: a pasted grid must carry
            // the same evidence as a screenshot of the panel
            std::string tsv = "# " + ana.prov + "\n";
            tsv += "metric\tunit";
            for (const auto& cn : ana.cols) tsv += "\t" + cn;
            tsv += "\n";
            for (int k = 0; k < (int)ana.keys.size(); k++) {
                tsv += ana.keys[k] + "\t" + (k < (int)ana.units.size() ? ana.units[k] : "");
                for (int c = 0; c < (int)ana.cols.size(); c++)
                    tsv += "\t" + (c < (int)ana.vals[k].size() ? ana.vals[k][c] : std::string());
                tsv += "\n";
            }
            ImGui::SetClipboardText(tsv.c_str());
            toast("result grid copied as TSV (provenance included)");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ana.img != im || ana.series.empty() || app.csvDlg != nullptr);
        if (ImGui::Button("Export curves (CSV)...")) {
            // wide per series name - x, then one column per measured target -
            // so a spreadsheet plots it without reshaping. Column labels are
            // the same ROI labels the grid and canvas use; provenance rides
            // along as # comment rows. Built NOW: the results may change
            // while the OS dialog is open.
            std::string csv = "# " + ana.prov + "\n";
            std::vector<std::string> names;
            for (const auto& s : ana.series) {
                bool seen = false;
                for (const auto& n2 : names) if (n2 == s.name) { seen = true; break; }
                if (!seen) names.push_back(s.name);
            }
            char b[64];
            for (const auto& nm : names) {
                const App::AnalysisState::Series* first = nullptr;
                size_t maxN = 0;
                for (const auto& s : ana.series) {
                    if (s.name != nm) continue;
                    if (!first) first = &s;
                    maxN = std::max(maxN, s.ys.size());
                }
                if (!first) continue;
                csv += "# series: " + nm +
                       (first->yLabel.empty() ? "" : "  (y: " + first->yLabel + ")") + "\n";
                csv += first->xLabel.empty() ? "index" : first->xLabel;
                for (const auto& s : ana.series)
                    if (s.name == nm)
                        csv += "," + (s.col >= 0 && s.col < (int)ana.cols.size()
                                      ? ana.cols[s.col] : std::string("?"));
                csv += "\n";
                // x comes from the first curve: ROIs measured by one analyzer
                // share the sample grid; shorter curves leave trailing blanks
                for (size_t i = 0; i < maxN; i++) {
                    float xv = first->xs.empty() ? (float)i
                             : (i < first->xs.size() ? first->xs[i] : 0.0f);
                    snprintf(b, sizeof b, "%.6g", xv);
                    csv += b;
                    for (const auto& s : ana.series) {
                        if (s.name != nm) continue;
                        csv += ",";
                        if (i < s.ys.size()) { snprintf(b, sizeof b, "%.6g", s.ys[i]); csv += b; }
                    }
                    csv += "\n";
                }
            }
            if (!pfd::settings::available()) {
                toast("no file-dialog backend found (install zenity or kdialog)", true);
            } else {
                app.pendingCsv = std::move(csv);
                app.csvDlg = std::make_unique<pfd::save_file>("Export curves (CSV)",
                    "curves.csv", std::vector<std::string>{ "CSV (*.csv)", "*.csv" });
            }
        }
        ImGui::EndDisabled();
        // reference line: a spec limit the curves must clear reads instantly
        // off the plot; typing it beats remembering it
        ImGui::SameLine();
        ImGui::Checkbox("ref", &app.anaRefOn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("horizontal reference line on the plots below\n"
                              "(a spec value / pass line; saved with the session)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        ImGui::BeginDisabled(!app.anaRefOn);
        ImGui::DragFloat("##anaref", &app.anaRef, 0.005f, -FLT_MAX, FLT_MAX, "%.3g");
        ImGui::EndDisabled();
        if (ana.img == im) {
            if (!ana.err.empty()) {
                ImGui::PushTextWrapPos(0.0f);       // plugin errors carry paths
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ana.err.c_str());
                ImGui::PopTextWrapPos();
            }
            int nCols = 2 + (int)ana.cols.size();       // metric + unit + one per target
            // SizingFixedFit + explicit inner width: with StretchProp, ScrollX never
            // scrolled and simply clipped the numbers (hence the old 16-column cap)
            float colW = ImGui::GetFontSize() * 7.0f;
            float unitW = ImGui::GetFontSize() * 3.0f;
            float pad = ImGui::GetStyle().CellPadding.x * 2;
            if (!ana.keys.empty() &&
                ImGui::BeginTable("anagrid", nCols,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0, 0),
                                  (nCols - 1) * (colW + pad) + unitW + pad + nCols + 1)) {
                ImGui::TableSetupScrollFreeze(2, 1);   // metric + unit stay in view
                ImGui::TableSetupColumn("metric", ImGuiTableColumnFlags_WidthFixed, colW);
                ImGui::TableSetupColumn("unit", ImGuiTableColumnFlags_WidthFixed, unitW);
                for (const auto& cn : ana.cols)
                    ImGui::TableSetupColumn(cn.c_str(), ImGuiTableColumnFlags_WidthFixed, colW);
                // custom header row so each target column wears its ROI's
                // color: the frame on the canvas, the curve in the plot and
                // the column in the grid identify each other without a legend
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn(); ImGui::TableHeader("metric");
                ImGui::TableNextColumn(); ImGui::TableHeader("unit");
                for (int c = 0; c < (int)ana.cols.size(); c++) {
                    ImGui::TableNextColumn();
                    int ci = c < (int)ana.colColor.size() ? ana.colColor[c] : -1;
                    if (ci >= 0)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(ANN_COLORS[ci & 7]));
                    ImGui::TableHeader(ana.cols[c].c_str());
                    if (ci >= 0) ImGui::PopStyleColor();
                }
                // headline rows (mtf50, prnu%, noise floor) wear the theme
                // accent + a tinted row: the grid keeps every number, the
                // emphasis says which one the analyzer exists to produce
                ImVec4 acc = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImU32 accBg = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.14f));
                for (int k = 0; k < (int)ana.keys.size(); k++) {
                    bool hl = k < (int)ana.headline.size() && ana.headline[k];
                    ImGui::TableNextRow();
                    if (hl) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, accBg);
                    ImGui::TableNextColumn();
                    if (hl) ImGui::TextUnformatted(ana.keys[k].c_str());
                    else    ImGui::TextDisabled("%s", ana.keys[k].c_str());
                    ImGui::TableNextColumn();
                    if (k < (int)ana.units.size() && !ana.units[k].empty())
                        ImGui::TextDisabled("%s", ana.units[k].c_str());
                    for (int c = 0; c < (int)ana.cols.size(); c++) {
                        ImGui::TableNextColumn();
                        if (c >= (int)ana.vals[k].size() || ana.vals[k][c].empty()) {
                            ImGui::TextDisabled("-");
                            continue;
                        }
                        const std::string& v = ana.vals[k][c];
                        // numbers right-aligned in their fixed column (textNum
                        // rule: digits keep their place across runs); text rows
                        // (method / note) stay left-aligned prose
                        bool num = v[0] == '-' || v[0] == '+' || v[0] == '.' ||
                                   (v[0] >= '0' && v[0] <= '9');
                        if (hl) ImGui::PushStyleColor(ImGuiCol_Text, acc);
                        if (num) textNumStr(v);
                        else     ImGui::TextUnformatted(v.c_str());
                        if (hl) ImGui::PopStyleColor();
                    }
                }
                ImGui::EndTable();
            }
            drawAnalysisPlots();   // curves from V2 analyzers (one plot per series name)
        }
    }
}

// The connected server, in its own panel. It used to hang off the bottom of
// Files, but the two do different jobs: Files lists what is already OPEN (and
// closes it), this one BROWSES a machine and decides what to open next. Sharing
// a window made both harder to read once a server had more than a few folders.
// ---- remote listing row formatting ----
static std::string fmtBytesHuman(uint64_t n) {
    char b[32];
    if (n >= (1ull << 30))      snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else if (n >= 1024)         snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else                        snprintf(b, sizeof b, "%llu B", (unsigned long long)n);
    return b;
}
static std::string fmtUnixTime(int64_t t) {
    if (t <= 0) return "-";                    // v2 peer / unreadable: unknown
    time_t tt = (time_t)t;
    struct tm tmv {};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}
// "(3000,4000) u16" - the file's declared shape, so what the browser promises is
// what META will later confirm.
static std::string fmtEntryShape(const remote::Entry& e) {
    if (!e.hasMeta) return "-";
    std::string s = "(";
    for (int i = 0; i < e.ndim; i++)
        s += (i ? "," : "") + std::to_string(e.dims[i]);
    s += ") " + e.dtype;
    if (e.fortran) s += " F";                  // Fortran order: rare enough to flag
    return s;
}

// ---- listing view: one row of the Browse table --------------------------------
// A numbered sequence arrives as ONE synthetic entry carrying `.members`, so
// "show me the individual frames" is a view over the reply we already have -
// no LIST, no round trip, nothing to invalidate. `member` picks which face the
// row wears: -1 = the entry itself (folder, plain file, or the collapsed group
// row), >= 0 = the n-th frame of a group.
//
// What an expanded frame does NOT have is its own size and mtime: the group
// reply carries the SUM of the members' bytes and the NEWEST member's time,
// and there is no per-file breakdown in it. Those cells stay blank rather than
// repeating the group's numbers on 24 rows, which would be a lie 24 times.
// shape/dtype are shared by construction (the peer only groups files that
// agree), so they are shown.
// In TREE mode rows no longer all come from one directory, so a row carries the
// directory it was listed from. `dir` points at a string that outlives the
// frame: either App::RemoteBrowse::dir or a key of App::rbTreeCache (std::map
// nodes do not move).
struct RbRow {
    const remote::Entry* e = nullptr;
    const std::string* dir = nullptr;
    int member = -1;
    int depth = 0;                 // tree indent level; 0 = the listed folder
    bool ph = false;               // "(listing...)": a node whose LIST is in flight
    const std::string& name() const { return member < 0 ? e->name : e->members[member]; }
    bool isDir()   const { return !ph && member < 0 && e->dir; }
    bool isGroup() const { return !ph && member < 0 && e->group; }
    bool ownFile() const { return member < 0; }   // has its own size / mtime
    // absolute path of this row, and of one of its members
    std::string join(const std::string& n) const {
        return *dir == "/" ? "/" + n : *dir + "/" + n;
    }
    std::string full() const { return join(name()); }
};
// The listing table's columns, and the sort the tree builder has to honour.
// Stashed from the table (TableGetSortSpecs only exists between Begin/EndTable,
// and a tree has to be sorted per LEVEL while it is being built, one frame
// earlier). A sort change therefore lands on the next frame - invisible, and
// far cheaper than building the view twice.
enum { RB_COL_NAME = 0, RB_COL_SHAPE, RB_COL_SIZE, RB_COL_MTIME };
static int  g_rbSortCol = RB_COL_NAME;
static bool g_rbSortDesc = false;

static void rbAddRows(const std::string* dir, const std::vector<remote::Entry>& ents,
                      bool flat, bool tree, int depth, std::vector<RbRow>& out);

// Grouped or flat, listing or tree, from the same entries. Free function so the
// headless selftest drives exactly what the panel draws.
static std::vector<RbRow> rbBuildView(const std::string* dir,
                                      const std::vector<remote::Entry>& entries,
                                      bool flat, bool tree) {
    std::vector<RbRow> v;
    v.reserve(entries.size());
    rbAddRows(dir, entries, flat, tree, 0, v);
    return v;
}
static void rbAddRows(const std::string* dir, const std::vector<remote::Entry>& ents,
                      bool flat, bool tree, int depth, std::vector<RbRow>& out) {
    if (depth > 24) return;                    // a symlink loop is not a tree
    std::vector<int> order(ents.size());
    for (int i = 0; i < (int)ents.size(); i++) order[i] = i;
    if (tree) {
        // Per LEVEL: a global sort over a flattened tree would tear children
        // away from their parents. Directories first, as in the flat listing.
        std::stable_sort(order.begin(), order.end(), [&](int ia, int ib) {
            const remote::Entry& a = ents[ia];
            const remote::Entry& b = ents[ib];
            if (a.dir != b.dir) return a.dir;
            int cmp = 0;
            switch (g_rbSortCol) {
                case RB_COL_SIZE:  cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                case RB_COL_MTIME: cmp = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
                default:           cmp = a.name.compare(b.name); break;
            }
            return g_rbSortDesc ? cmp > 0 : cmp < 0;
        });
    }
    for (int oi : order) {
        const remote::Entry& e = ents[oi];
        if (flat && e.group && !e.members.empty()) {
            for (int m = 0; m < (int)e.members.size(); m++)
                out.push_back({ &e, dir, m, depth, false });
            continue;
        }
        out.push_back({ &e, dir, -1, depth, false });
        if (!tree || !e.dir) continue;
        std::string sub = *dir == "/" ? "/" + e.name : *dir + "/" + e.name;
        if (!rbHas(app.rbExpanded, sub)) continue;
        auto it = app.rbTreeCache.find(sub);
        if (it != app.rbTreeCache.end())
            rbAddRows(&it->first, it->second, flat, tree, depth + 1, out);
        else {
            // the LIST is still in flight: say so where the children will be
            static const remote::Entry busy = [] {
                remote::Entry b; b.name = "(listing...)"; return b;
            }();
            out.push_back({ &busy, dir, -1, depth + 1, true });
        }
    }
}

// ---- deferred panel actions -------------------------------------------------
// Every row the listing draws is an RbRow: RAW POINTERS into app.rbrowse.entries
// and into the tree cache, held in a `view` vector that is rebuilt each frame
// and read from the moment it is built until the table ends. An action that
// REPLACES either container therefore cannot run where it is clicked - the rest
// of the frame would read freed memory.
//
// The tree cache knew this ("a mid-frame clear would dangle every row below the
// refresh button") and hand-rolled a one-flag deferral. The Places combo did
// not: picking a bookmark on another host runs goToPlace, which assigns a fresh
// App::RemoteBrowse over the live one, and the table below then dereferenced
// nine destroyed entries - reproduced as a SIGSEGV inside strlen, one click
// after connecting.
//
// So it is a rule now instead of three careful call sites: navigation, the tree
// cache and the connection state are QUEUED here and run once `view` is dead.
// A new button inherits the rule instead of having to remember it.
static std::vector<std::function<void()>> g_rbDeferred;
static void rbDefer(std::function<void()> f) { g_rbDeferred.push_back(std::move(f)); }
static size_t rbDeferredPending() { return g_rbDeferred.size(); }
// RAII, so the panel's early returns run the queue too. Declared BEFORE `view`
// in drawPanelRemote: reverse destruction order is what guarantees the rows are
// already gone when the queue runs.
struct RbDeferredActions {
    ~RbDeferredActions() {
        std::vector<std::function<void()>> q;
        q.swap(g_rbDeferred);          // an action may queue another
        for (auto& f : q) f();
    }
};
// Every navigation the panel offers. remoteBrowseTo only enqueues a job today,
// so this changes nothing that can be seen - it makes "the panel never
// navigates mid-draw" true by construction rather than by inspection.
static void rbGoTo(const std::string& dir) {
    rbDefer([dir] { remoteBrowseTo(dir); });
}

// Bookmarks + recents in one dropdown. Available connected or not: picking a
// place while disconnected is exactly how a session starts next morning.
static void drawRemotePlacesCombo() {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##places", "places  (bookmarks + recent)",
                           ImGuiComboFlags_HeightLarge))
        return;
    // display only: a local place reads better as "[local] path" than as the
    // url scheme (the stored prefs string stays the url)
    auto placeLabel = [](const std::string& u) {
        return u.rfind("local://", 0) == 0 ? "[local] " + u.substr(8) : u;
    };
    int rm = -1;
    if (!app.rbBookmarks.empty()) ImGui::TextDisabled("bookmarks");
    for (int i = 0; i < (int)app.rbBookmarks.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) rm = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("remove this bookmark");
        ImGui::SameLine();
        if (ImGui::Selectable(placeLabel(app.rbBookmarks[i]).c_str()))
            rbDefer([u = app.rbBookmarks[i]] { goToPlace(u); });   // see rbDefer
        ImGui::PopID();
    }
    if (rm >= 0) {
        app.rbBookmarks.erase(app.rbBookmarks.begin() + rm);
        app.prefsDirty = true;
        savePrefs();
    }
    if (!app.rbRecents.empty()) ImGui::TextDisabled("recent");
    for (int i = 0; i < (int)app.rbRecents.size(); i++) {
        ImGui::PushID(1000 + i);
        if (ImGui::Selectable(placeLabel(app.rbRecents[i]).c_str()))
            rbDefer([u = app.rbRecents[i]] { goToPlace(u); });
        ImGui::PopID();
    }
    if (app.rbBookmarks.empty() && app.rbRecents.empty())
        ImGui::TextDisabled("nothing yet - the * button bookmarks the open folder");
    ImGui::EndCombo();
}

static void drawPanelRemote() {
    App::RemoteBrowse& B = app.rbrowse;
    // FIRST, so it is destroyed LAST - after `view` and after every row that
    // points into the browse state. Anything that replaces that state is queued
    // through rbDefer and runs here. (This replaces a one-flag "forget the tree
    // next frame" deferral that covered the tree cache and nothing else.)
    RbDeferredActions rbActions;
    ImGui::PushID("remotetree");
    if (!B.connected) {
        if (ImGui::Button("Start Remote (ssh)...")) app.remoteDlgOpen = true;
        if (app.rbBusy) { ImGui::SameLine(); ImGui::TextDisabled("connecting..."); }
        drawRemotePlacesCombo();
        if (!B.err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.4f, 1));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(B.err.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("details / copy")) app.showRemoteError = true;
        }
        ImGui::TextDisabled("Connect to a machine, then pick the file here.");
        ImGui::PopID();
        return;
    }
    // The panel used to stack FIVE things above the listing: a host row with
    // three buttons, the breadcrumb bar, the filter, the server-search row, and
    // (when a preview was alive) the scrub bar. Four of them were there for the
    // rare case. What is left permanently on screen is what browsing actually
    // needs - where am I (breadcrumbs) and narrow it down (toolbar + filter) -
    // and everything else is one click away under "more", with nothing removed.
    bool& rbAdvanced = app.rbAdvanced;
    // Server-side search: a different thing from the filter (which only narrows
    // what is already listed). Declared up here because the path bar's context
    // menu can aim it at a folder.
    static char rbSearchBuf[256] = "";
    static bool rbSearchFocus = false;
    static std::string rbSearchRoot;   // set by "Search under here"; empty = this folder
    // where we are, and how to leave
    bool atRoot = B.dir == "~" || B.dir == "/";
    auto rbGoParent = [&]() {
        if (atRoot) return;
        std::string d = B.dir;
        size_t s = d.find_last_of('/');
        rbGoTo(s == std::string::npos || s == 0 ? (d[0] == '~' ? "~" : "/")
                                                : d.substr(0, s));
    };
    // ---- row 1: path bar - breadcrumbs, or one text field while editing ----
    static char rbPathEdit[1024];
    static bool rbPathEditing = false, rbPathFocus = false;
    if (!rbPathEditing) {
        struct Seg { std::string label, path; };
        std::vector<Seg> segs;
        const std::string& d = B.dir;
        size_t i = 0;
        if (!d.empty() && d[0] == '/')      { segs.push_back({ "/", "/" }); i = 1; }
        else if (!d.empty() && d[0] == '~') { segs.push_back({ "~", "~" });
                                              i = d.size() > 1 && d[1] == '/' ? 2 : 1; }
        while (i < d.size()) {
            size_t s = d.find('/', i);
            std::string part = d.substr(i, s == std::string::npos ? std::string::npos : s - i);
            if (!part.empty()) {
                std::string prefix = segs.empty() ? part
                                   : segs.back().path == "/" ? "/" + part
                                   : segs.back().path + "/" + part;
                segs.push_back({ part, prefix });
            }
            if (s == std::string::npos) break;
            i = s + 1;
        }
        bool editReq = false;
        for (size_t k = 0; k < segs.size(); k++) {
            ImGui::PushID((int)k);
            if (k) {
                ImGui::SameLine(0, 2);
                // capture paths run long: wrap instead of clipping the tail off
                if (ImGui::GetContentRegionAvail().x <
                    ImGui::CalcTextSize(segs[k].label.c_str()).x + ImGui::GetFontSize())
                    ImGui::NewLine();
            }
            if (ImGui::SmallButton(segs[k].label.c_str())) rbGoTo(segs[k].path);
            // Right-click used to jump straight into path editing. It is a menu
            // now: the actions that take a FOLDER as their subject all belong on
            // the bar that names the folder.
            if (ImGui::BeginPopupContextItem("crumbctx")) {
                const std::string& target = segs[k].path;
                // the same item a folder ROW carries, aimed at a folder that is
                // not in any listing - the one you are inside, or an ancestor
                if (ImGui::MenuItem("Open folder (all stacks below)"))
                    remoteScanFolder(target);
                if (ImGui::MenuItem("Search under here")) {
                    rbSearchRoot = target;
                    rbSearchFocus = true;
                    rbAdvanced = true;          // the search row lives under "more"
                }
                if (ImGui::MenuItem("Bookmark")) {
                    std::string u = placeUrl(B.host, B.port, target);
                    if (std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(), u) ==
                        app.rbBookmarks.end()) {
                        app.rbBookmarks.push_back(u);
                        app.prefsDirty = true;
                        savePrefs();
                    }
                    toast("bookmarked " + u);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Copy path")) {
                    ImGui::SetClipboardText(target.c_str());
                    toast("copied " + target);
                }
                if (ImGui::MenuItem("Edit path...")) editReq = true;
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(right-click for what can be done to this folder)",
                                  segs[k].path.c_str());
            ImGui::PopID();
        }
        if (!segs.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("edit##path")) editReq = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("type or paste a path (right-clicking a crumb works too)");
        if (app.rbBusy) { ImGui::SameLine(); ImGui::TextDisabled("(listing...)"); }
        if (editReq) {
            snprintf(rbPathEdit, sizeof rbPathEdit, "%s", d.c_str());
            rbPathEditing = true;
            rbPathFocus = true;
        }
    } else {
        if (rbPathFocus) { ImGui::SetKeyboardFocusHere(); rbPathFocus = false; }
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
        bool entered = ImGui::InputText("##rbpath", rbPathEdit, sizeof rbPathEdit,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered) {
            std::string p = rbPathEdit;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(0, 1);
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            rbPathEditing = false;
            rbGoTo(p.empty() ? "~" : p);
        } else if (ImGui::IsItemDeactivated()) {
            rbPathEditing = false;    // Esc (ImGui reverts the text) or focus loss
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel##path")) rbPathEditing = false;
    }
    if (!B.err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.4f, 1));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(B.err.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("copy##rberr")) {
            ImGui::SetClipboardText(B.err.c_str());
            toast("copied");
        }
    }
    // The listing as ROWS: grouped (one row per sequence) or flat (one row per
    // frame), listing or tree. Rebuilt every frame - see rbBuildView.
    std::vector<RbRow> view = rbBuildView(&B.dir, B.entries, app.rbFlat, app.rbTree);
    // Multi-select (Ctrl / Shift + click on file rows), indexed by ROW. A
    // navigation or a refresh invalidates the indices, so it resets rather
    // than pointing at different rows. A grouped/flat TOGGLE does not: the
    // selection carries across by owning entry, so expanding a selected
    // sequence selects its frames and collapsing them selects the sequence.
    static std::vector<char> rbSel;
    static int rbSelAnchor = -1;               // row index of the last click
    static bool rbSelFlat = false;             // which view rbSel was built for
    static bool rbSelTree = false;
    {
        static std::string selSig;
        std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.entries.size());
        if (sig != selSig) {
            selSig = sig;
            rbSel.assign(view.size(), 0);
            rbSelAnchor = -1;
        } else if (rbSelFlat != app.rbFlat || rbSelTree != app.rbTree) {
            std::vector<RbRow> old = rbBuildView(&B.dir, B.entries, rbSelFlat, rbSelTree);
            std::vector<const remote::Entry*> sel;
            for (size_t i = 0; i < old.size() && i < rbSel.size(); i++)
                if (rbSel[i]) sel.push_back(old[i].e);
            rbSel.assign(view.size(), 0);
            for (size_t i = 0; i < view.size(); i++)
                if (std::find(sel.begin(), sel.end(), view[i].e) != sel.end()) rbSel[i] = 1;
            rbSelAnchor = -1;
        }
        rbSelFlat = app.rbFlat;
        rbSelTree = app.rbTree;
        // an expand or a collapse moved every row below it: start clean
        if (rbSel.size() != view.size()) rbSel.assign(view.size(), 0);
    }
    // What a plain click does: enter a folder, or show a throwaway PREVIEW of
    // a file / of a sequence's poster frame. Nothing is registered. Factored
    // out because the keyboard (arrow keys) has to do exactly the same thing.
    auto rbActivateRow = [&](const RbRow& r) {
        if (r.ph) return;
        if (r.isDir()) {
            // In a TREE a folder opens where it is; the listing still enters it.
            if (app.rbTree) {
                if (rbHas(app.rbExpanded, r.full())) rbTreeCollapse(r.full());
                else rbTreeExpand(r.full());
            } else rbGoTo(r.full());
            return;
        }
        if (!isNpyName(r.name())) return;
        // Everything this needs is read out of the row BEFORE the open, as
        // VALUES: an RbRow is a pair of raw pointers into B.entries / the tree
        // cache, and `r` is a reference into a vector rebuilt every frame.
        // Nothing on the open path mutates either container today, but the row
        // list is one refresh away from being replaced underneath us, and the
        // tree work already had to defer a mid-frame clear for exactly this.
        // stepping context for the scrub bar / , and . : the whole sequence
        // when the row belongs to one, so a flat row still steps its siblings
        std::vector<std::string> seq;
        if ((r.isGroup() || r.member >= 0) && !r.e->members.empty())
            for (const auto& m : r.e->members) seq.push_back(r.join(m));
        int seqAt = r.member >= 0 ? r.member : 0;
        std::string seqLabel = r.e->name;
        std::string target = r.isGroup()
            ? r.join(r.e->members.empty() ? r.e->name : r.e->members[0])
            : r.full();
        // idempotent: clicking the same file again re-shows the preview
        // instead of re-fetching it
        ImageDoc* pv = nullptr;
        for (const auto& di : app.images)
            if (di->uid == app.previewUid && di->preview) pv = di.get();
        std::string u = makeRemoteUrl(B.host, target);
        if (pv && pv->remoteUrl == u) selectImage(app.current);
        else openRemote(u, true);
        app.previewFiles = std::move(seq);
        if (!app.previewFiles.empty()) {
            app.previewIndex = seqAt;
            app.previewLabel = std::move(seqLabel);
        }
    };
    // What a double-click (and Enter) does: a REGISTERED open. A sequence row
    // opens the whole stack; a frame promotes the preview it just made.
    auto rbOpenRow = [&](const RbRow& r) {
        if (r.ph) return;
        if (r.isDir()) { rbGoTo(r.full()); return; }
        if (!isNpyName(r.name())) return;
        if (r.isGroup()) {
            dropPreview();                   // the poster frame did its job
            std::vector<std::string> files;
            for (const auto& m : r.e->members) files.push_back(r.join(m));
            openRemoteStack(B.host, files);
            return;
        }
        for (const auto& di : app.images)
            if (di->uid == app.previewUid && di->preview) promotePreview(di.get());
    };
    // ---- row 2: the toolbar. Move, refresh, choose the shape of the listing,
    // narrow it down. Everything else is behind "more".
    static char rbFilter[256] = "";
    {
        ImGui::BeginDisabled(atRoot);
        if (ImGui::SmallButton("up")) rbGoParent();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("parent folder (Backspace)");
        ImGui::SameLine();
        if (ImGui::SmallButton("home")) rbGoTo("~");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("the login home directory");
        ImGui::SameLine();
        if (ImGui::SmallButton("refresh")) {
            rbDefer([] { rbTreeForget(); });        // in order: forget, then list
            rbGoTo(B.dir);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("list this folder again (and forget the tree's cached children)");
        ImGui::SameLine();
        // Open Folder for the folder you are IN. It used to exist only on a
        // folder ROW, so opening the directory being browsed meant going up a
        // level to find its own name in the parent's listing - and if that
        // directory was the home or the root, there was no level to go up to.
        if (ImGui::SmallButton("open folder")) remoteScanFolder(B.dir);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("scan THIS folder and everything below it, then pick\n"
                              "which stacks to open:\n%s", B.dir.c_str());
        ImGui::SameLine();
        // Grouped <-> flat. No round trip: the peer already sent every member.
        if (ImGui::SmallButton(app.rbFlat ? "flat##rbview" : "grouped##rbview")) {
            app.rbFlat = !app.rbFlat;
            app.prefsDirty = true;
            savePrefs();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(app.rbFlat
                ? "flat: every frame is its own row.\nclick to collapse numbered "
                  "sequences back into one row each.\n(per-frame size and date are "
                  "not in the listing reply - those cells stay blank)"
                : "grouped: a numbered sequence is ONE row.\nclick to list its frames "
                  "individually (no request to the server).");
        ImGui::SameLine();
        // List <-> tree. Expanding a node costs ONE list, once.
        if (ImGui::SmallButton(app.rbTree ? "tree##rbtree" : "list##rbtree")) {
            app.rbTree = !app.rbTree;
            app.prefsDirty = true;
            savePrefs();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(app.rbTree
                ? "tree: a folder opens IN PLACE (click it; Right/Left also do).\n"
                  "double-click or Enter still goes INTO it.\neach node is listed "
                  "once and kept - collapsing costs nothing to undo.\nclick to go "
                  "back to one folder at a time."
                : "list: one folder at a time, a click enters it.\nclick to expand "
                  "folders in place instead.");
        ImGui::SameLine();
        // Filter what is already listed - no round trip. Substring by default,
        // glob when * or ? appears (globListMatch's contract), because a capture
        // dump directory holds hundreds of entries and one condition matters.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9);
        ImGui::InputTextWithHint("##rbfilter", "filter (Ctrl+F), * ? glob",
                                 rbFilter, sizeof rbFilter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("filters the listing below without asking the server\n"
                              "bare text matches anywhere; * and ? make it a glob;\n"
                              "comma separates alternatives");
    }
    // filtered view, by row index (the clipper needs random access)
    std::vector<int> shown;
    shown.reserve(view.size());
    if (!app.rbTree || !rbFilter[0]) {
        for (int i = 0; i < (int)view.size(); i++)
            if (!rbFilter[0] || globListMatch(rbFilter, view[i].name()))
                shown.push_back(i);
    } else {
        // In a tree, dropping a folder because its own NAME does not match
        // would orphan the matching files inside it. Rows are in pre-order, so
        // one backward pass keeps every match and every ancestor of a match.
        std::vector<char> keep(view.size(), 0);
        int need = -1;                 // an ancestor shallower than this is wanted
        for (int i = (int)view.size() - 1; i >= 0; i--) {
            bool m = !view[i].ph && globListMatch(rbFilter, view[i].name());
            if (m || (need >= 0 && view[i].depth < need)) {
                keep[i] = 1;
                need = view[i].depth;
            }
        }
        for (int i = 0; i < (int)view.size(); i++) if (keep[i]) shown.push_back(i);
    }
    // ...and in SCREEN order, here, before anything indexes it. The sort used to
    // happen inside the table, after the keyboard block had already turned the
    // cursor's row index into a screen position: with any sort but the default
    // the two disagreed, so the clipper was told to keep a different row alive
    // than the one the cursor was on and SetScrollHereY never fired. The spec
    // itself is stashed from the table one frame earlier (g_rbSortCol), exactly
    // as the tree builder needs it - see RB_COL_NAME.
    // Directories sort before files no matter the key: this is a browser, not a
    // table of numbers. In TREE mode the sort has already happened per level,
    // inside the builder; sorting the flattened tree here would tear children
    // away from their parents.
    if (!app.rbTree)
        std::stable_sort(shown.begin(), shown.end(), [&](int ia, int ib) {
            const RbRow& a = view[ia];
            const RbRow& b = view[ib];
            if (a.isDir() != b.isDir()) return a.isDir();
            // an expanded frame has no size / mtime of its own: it sorts as
            // unknown (0) rather than borrowing the group's totals
            uint64_t sa = a.ownFile() ? a.e->size : 0, sb = b.ownFile() ? b.e->size : 0;
            int64_t ma = a.ownFile() ? a.e->mtime : 0, mb = b.ownFile() ? b.e->mtime : 0;
            int cmp = 0;
            switch (g_rbSortCol) {
                case RB_COL_SIZE:  cmp = sa < sb ? -1 : sa > sb ? 1 : 0; break;
                case RB_COL_MTIME: cmp = ma < mb ? -1 : ma > mb ? 1 : 0; break;
                default:           cmp = a.name().compare(b.name()); break;
            }
            return g_rbSortDesc ? cmp > 0 : cmp < 0;
        });
    if (rbFilter[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d", (int)shown.size(), (int)view.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("rows shown of rows listed");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(rbAdvanced ? "less##rbadv" : "more##rbadv")) {
        rbAdvanced = !rbAdvanced;
        app.prefsDirty = true;
        savePrefs();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("the connection, the places list and the server-side\n"
                          "recursive search - the things a browse does not need\n"
                          "every minute");
    // ---- row 3, on request: the connection and the server-side search ----
    if (rbAdvanced) {
        ImGui::TextUnformatted(B.host.empty() ? "local peer" : B.host.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("disconnect")) {
            rbDefer([] {                 // another machine, other children
                app.rbrowse = App::RemoteBrowse{};
                rbTreeForget();
            });
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        {   // star = bookmark the place being looked at; the combo recalls them
            std::string curUrl = placeUrl(B.host, B.port, B.dir);
            bool starred = std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(),
                                     curUrl) != app.rbBookmarks.end();
            if (starred) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.83f, 0.35f, 1));
            if (ImGui::SmallButton("*")) {
                if (starred)
                    app.rbBookmarks.erase(std::remove(app.rbBookmarks.begin(),
                                                      app.rbBookmarks.end(), curUrl),
                                          app.rbBookmarks.end());
                else
                    app.rbBookmarks.push_back(curUrl);
                app.prefsDirty = true;
                savePrefs();
            }
            if (starred) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(starred ? "remove bookmark:\n%s" : "bookmark this place:\n%s",
                                  curUrl.c_str());
            ImGui::SameLine();
            drawRemotePlacesCombo();
        }
        if (rbSearchFocus) { ImGui::SetKeyboardFocusHere(); rbSearchFocus = false; }
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7);
        bool go = ImGui::InputTextWithHint("##rbsearch",
                                           "search server (recursive): frame_* or **/dark.npy",
                                           rbSearchBuf, sizeof rbSearchBuf,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("the server walks below the folder (depth 6, first 2000 hits);\n"
                              "bare text matches anywhere in the relative path;\n"
                              "* and ? glob across '/'");
        ImGui::SameLine();
        if (app.rbSearch.running) {
            if (ImGui::SmallButton("Stop##rbsearch")) {
                app.rbSearch.gen++;               // in-flight result becomes stale
                app.rbSearch.running = false;
            }
        } else if ((ImGui::SmallButton("Search") || go) && rbSearchBuf[0]) {
            remoteStartSearch(rbSearchRoot.empty() ? B.dir : rbSearchRoot, rbSearchBuf);
        }
        if (!rbSearchRoot.empty()) {
            ImGui::TextDisabled("search under: %s", rbSearchRoot.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x##sroot")) rbSearchRoot.clear();
        }
    } else if (!rbSearchRoot.empty() || app.rbSearch.running) {
        // a search aimed or running is state the user set: never silently
        // hidden by a fold, even though the row that made it is folded away
        if (app.rbSearch.running) ImGui::TextDisabled("searching... (\"more\" to stop)");
        else ImGui::TextDisabled("search aimed at: %s  (\"more\")", rbSearchRoot.c_str());
    }
    {   // "Open N selected as stack" - enabled only when the v3 metadata proves
        // the frames can actually stack, BEFORE any pixel is transferred
        int nSel = 0;
        const remote::Entry* first = nullptr;
        std::string reason;
        for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
            if (!rbSel[i]) continue;
            const remote::Entry& e = *view[i].e;
            nSel++;
            if (!isNpyName(view[i].name())) { reason = "only .npy files can form a stack"; continue; }
            if (!e.hasMeta) {
                reason = "shape unknown - the peer is protocol 2 (File > Update remote peer)";
                continue;
            }
            if (!first) { first = &e; continue; }
            if (e.ndim != first->ndim || e.dtype != first->dtype ||
                memcmp(e.dims, first->dims, sizeof e.dims) != 0)
                reason = "selected files differ: " + fmtEntryShape(*first) + " vs " +
                         fmtEntryShape(e);
        }
        if (nSel >= 2) {
            char lb[64];
            snprintf(lb, sizeof lb, "Open %d selected as stack", nSel);
            if (!reason.empty()) ImGui::BeginDisabled();
            if (ImGui::Button(lb)) {
                std::vector<std::string> files;
                for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
                    if (!rbSel[i]) continue;
                    if (view[i].isGroup())
                        for (const auto& m : view[i].e->members) files.push_back(view[i].join(m));
                    else files.push_back(view[i].full());
                }
                sortFramesNumerically(files);
                openRemoteStack(B.host, files);
                rbSel.assign(view.size(), 0);
            }
            if (!reason.empty()) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", reason.empty()
                    ? "frames stack in numeric name order" : reason.c_str());
            ImGui::SameLine();
            {   // the server aggregate for the selection, without opening it.
                // MEASURE exists from protocol 2, but v2 has no hasMeta to
                // pre-validate shapes - a mismatch falls to [server failed].
                int pv2 = 0;
                {
                    std::lock_guard<std::mutex> lk(app.sesMtx);
                    if (app.remoteSession) pv2 = app.remoteSession->peerVersion();
                }
                bool tempOk = pv2 >= 2;
                for (size_t i = 0; i < view.size() && i < rbSel.size(); i++)
                    if (rbSel[i] && !isNpyName(view[i].name())) tempOk = false;
                ImGui::BeginDisabled(!tempOk);
                if (ImGui::Button("Temporal stats (server)")) {
                    std::vector<std::string> files;
                    for (size_t i = 0; i < view.size() && i < rbSel.size(); i++) {
                        if (!rbSel[i]) continue;
                        if (view[i].isGroup())
                            for (const auto& m : view[i].e->members) files.push_back(view[i].join(m));
                        else files.push_back(view[i].full());
                    }
                    std::string leaf = B.dir;
                    size_t sl2 = leaf.find_last_of('/');
                    if (sl2 != std::string::npos && sl2 + 1 < leaf.size())
                        leaf = leaf.substr(sl2 + 1);
                    requestBrowseTemporal(B.host, files,
                                          leaf + " (" + std::to_string(files.size()) +
                                          " selected)");
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(tempOk
                        ? "sigma_t / sigma_fpn computed on the server over the\n"
                          "selected files, shown in the Temporal panel - nothing\n"
                          "opens, no pixel transfers. plane=all (no CFA split)."
                        : pv2 < 2 ? "needs a protocol 2+ peer (File > Update remote peer)"
                                  : "only .npy files can form a stack");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("clear##sel")) rbSel.assign(view.size(), 0);
        }
    }
    // The metadata columns exist from protocol 3 on. Say so once, up here - a
    // "-" in every row of every column explains nothing.
    {
        int pv = 0;
        {
            std::lock_guard<std::mutex> lk(app.sesMtx);
            if (app.remoteSession) pv = app.remoteSession->peerVersion();
        }
        if (pv > 0 && pv < 3)
            ImGui::TextColored(ImVec4(0.98f, 0.76f, 0.35f, 1),
                               "peer is protocol %d - File > Update remote peer "
                               "enables shape / date columns", pv);
    }
    ImGui::Separator();
    if (app.rbSearch.active) {         // search results stand in for the listing
        App::RemoteSearch& S = app.rbSearch;
        auto joinS = [&S](const std::string& rel) {
            return S.root == "/" ? "/" + rel : S.root + "/" + rel;
        };
        if (S.running) {
            ImGui::TextDisabled("searching %s under %s ...", S.pattern.c_str(), S.root.c_str());
        } else {
            ImGui::Text("%d result(s) for %s under %s%s", (int)S.hits.size(),
                        S.pattern.c_str(), S.root.c_str(),
                        S.truncated ? "  (first 2000 - narrow the pattern)" : "");
            if (S.skippedDirs)
                ImGui::TextDisabled("%d unreadable folder(s) skipped", S.skippedDirs);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("close results")) S.active = false;
        if (ImGui::BeginChild("searchhits", ImVec2(0, 0), ImGuiChildFlags_None)) {
            ImGuiListClipper clip;
            clip.Begin((int)S.hits.size());
            while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                const remote::GlobHit& h = S.hits[row];
                ImGui::PushID(row);
                std::string lb = h.dir ? h.rel + "/" : h.rel;   // trailing / marks dirs
                if (ImGui::Selectable(lb.c_str())) {
                    std::string full = joinS(h.rel);
                    if (h.dir) {
                        rbGoTo(full);
                    } else if (isNpyName(h.rel)) {
                        openRemote(makeRemoteUrl(B.host, full));
                    } else {
                        // not servable: at least go where it lives
                        size_t sl = full.find_last_of('/');
                        rbGoTo(sl == std::string::npos || sl == 0 ? "/"
                                                                 : full.substr(0, sl));
                    }
                }
                if (ImGui::BeginPopupContextItem("sctx")) {
                    std::string full = joinS(h.rel);
                    if (!h.dir && ImGui::MenuItem("Go to containing folder")) {
                        size_t sl = full.find_last_of('/');
                        rbGoTo(sl == std::string::npos || sl == 0 ? "/"
                                                                 : full.substr(0, sl));
                    }
                    if (ImGui::MenuItem("Copy path")) {
                        ImGui::SetClipboardText(full.c_str());
                        toast("copied " + full);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }
    // The "[..]" row that used to sit here is gone: the toolbar's "up" button
    // and Backspace both do it, and a row that exists only outside the home
    // directory shifted every listing row by one line on the way in and out.
    // ---- keyboard navigation of the listing --------------------------------
    // Up / Down walk the rows and preview as they go (what a plain click does),
    // Enter opens for real (what a double-click does), Backspace leaves for the
    // parent. Gated on IsAnyItemActive so the filter box, the path field and
    // the search field keep every key they type; disjoint from the , / . frame
    // stepping under the listing, which owns different keys entirely.
    static int rbCursor = -1;            // row index, or -1 = no cursor yet
    static bool rbCursorScroll = false;  // bring it into view this frame
    {
        static std::string curSig;
        static bool curFlat = false, curTree = false;
        std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.entries.size());
        if (sig != curSig) { curSig = sig; rbCursor = -1; }
        else if (curFlat != app.rbFlat || curTree != app.rbTree) {
            // follow the row across a grouped/flat or list/tree toggle
            std::vector<RbRow> old = rbBuildView(&B.dir, B.entries, curFlat, curTree);
            const remote::Entry* was = rbCursor >= 0 && rbCursor < (int)old.size()
                                     ? old[rbCursor].e : nullptr;
            rbCursor = -1;
            if (was)
                for (size_t i = 0; i < view.size(); i++)
                    if (view[i].e == was) { rbCursor = (int)i; break; }
            rbCursorScroll = rbCursor >= 0;
        }
        curFlat = app.rbFlat;
        curTree = app.rbTree;
        if (rbCursor >= (int)view.size()) rbCursor = -1;
    }
    int rbCursorPos = -1;                // ...where it sits on SCREEN
    for (int k = 0; k < (int)shown.size(); k++) if (shown[k] == rbCursor) rbCursorPos = k;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        int want = rbCursorPos;
        int last = (int)shown.size() - 1;
        if (!shown.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                want = rbCursorPos < 0 ? 0 : std::min(rbCursorPos + 1, last);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                want = rbCursorPos < 0 ? last : std::max(rbCursorPos - 1, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) want = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_End, false))  want = last;
        }
        if (want != rbCursorPos && want >= 0) {
            rbCursorPos = want;
            rbCursor = shown[want];
            rbCursorScroll = true;
            // moving onto a FOLDER must not enter it - walking a list of
            // folders would then dive into the first one and never come back
            if (!view[rbCursor].isDir()) rbActivateRow(view[rbCursor]);
        }
        if (rbCursor >= 0 && rbCursor < (int)view.size() &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            rbOpenRow(view[rbCursor]);
        // Right / Left expand and collapse the folder under the cursor. Only in
        // tree mode: in the flat listing there is nothing to open in place.
        if (app.rbTree && rbCursor >= 0 && rbCursor < (int)view.size() &&
            view[rbCursor].isDir()) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                rbTreeExpand(view[rbCursor].full());
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                rbTreeCollapse(view[rbCursor].full());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) rbGoParent();
    }
    // The listing scrolls on its own so the header above never leaves the view.
    // Properties target: a snapshot, because the row may scroll out of the
    // clipper (or the listing may refresh) while the popup is up.
    static remote::Entry rbPropsEntry;
    static std::string rbPropsPath;
    static bool rbPropsOpen = false;
    static bool rbPropsNoSize = false;   // an expanded frame: no size/mtime of its own
    // footer space for the preview scrub bar: RESERVED even when no preview is
    // alive, so starting one never shifts the rows under the cursor (a bar
    // that appeared above the list moved every row mid-double-click)
    float rbFootH = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("rblist", 4, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SortTristate,
                                       ImVec2(0, -rbFootH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_DefaultSort, 0.0f, RB_COL_NAME);
        ImGui::TableSetupColumn("shape / dtype", ImGuiTableColumnFlags_WidthFixed |
                                                 ImGuiTableColumnFlags_NoSort, 0.0f, RB_COL_SHAPE);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending, 0.0f, RB_COL_SIZE);
        ImGui::TableSetupColumn("modified", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_PreferSortDescending, 0.0f, RB_COL_MTIME);
        ImGui::TableHeadersRow();
        // The spec is STASHED, not applied: `shown` was already sorted with it,
        // above, where the keyboard and the clipper can agree with the screen.
        // A sort change therefore lands on the next frame - invisible, and the
        // same one-frame contract the tree builder has always had.
        if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            if (sp->SpecsCount > 0) {
                g_rbSortCol = (int)sp->Specs[0].ColumnUserID;
                g_rbSortDesc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            } else {
                g_rbSortCol = RB_COL_NAME;
                g_rbSortDesc = false;
            }
        }
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        // The cursor row must be SUBMITTED even when it is scrolled out, or
        // there is no item for SetScrollHereY to scroll to. AFTER Begin(): the
        // clipper allocates its range list there, and IncludeItemByIndex writes
        // straight through the null TempData pointer otherwise - the two
        // IM_ASSERTs that say so are compiled out of a release build, so a
        // single Down arrow segfaulted the process before any handler ran.
        if (rbCursorScroll && rbCursorPos >= 0 && rbCursorPos < (int)shown.size())
            clip.IncludeItemByIndex(rbCursorPos);
        while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
            const RbRow& r = view[shown[row]];
            const remote::Entry& e = *r.e;
            const std::string& rname = r.name();
            ImGui::PushID(shown[row]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // vscode-quiet rows: a dim chevron marks a folder, a stack gets
            // three hairlines, a file gets nothing - the name is the row.
            // (First cut had drawn folder/page pictograms; they collided with
            // the text and were, verbatim, "くどい".)
            // the tree's indent is drawn as leading spaces, so the Selectable
            // still spans the whole row and the hit target never narrows
            std::string lb(2 + (size_t)r.depth * 3, ' ');
            lb += rname;
            if (r.isGroup()) lb += "   [" + std::to_string(e.frames) + " frames]";
            bool servable = !r.ph && (r.isDir() || isNpyName(rname));   // (ph draws dimmed)
            if (!servable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            int ei = shown[row];
            bool isSel = ei < (int)rbSel.size() && rbSel[ei] != 0;
            bool rowClicked = ImGui::Selectable(lb.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns);
            if (shown[row] == rbCursor) {
                // the keyboard cursor: an outline, not a fill - the fill means
                // "selected for a multi-file action" and the two are not the same
                if (rbCursorScroll) { ImGui::SetScrollHereY(0.5f); rbCursorScroll = false; }
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
                                                    ImGui::GetItemRectMax(),
                                                    IM_COL32(150, 180, 215, 190), 0.0f, 0, 1.0f);
            }
            if (r.isDir() || r.isGroup()) {   // inside the two-space gutter the label reserves
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetItemRectMin();
                float h = ImGui::GetTextLineHeight();
                float gut = ImGui::CalcTextSize("  ").x;      // never touch the name
                float indent = ImGui::CalcTextSize("   ").x * (float)r.depth;
                float y = p.y + (ImGui::GetItemRectSize().y - h) * 0.5f;
                float cxm = p.x + indent + gut * 0.45f, cym = y + h * 0.5f;
                if (r.isDir()) {      // › chevron, the way a tree hints "enter me"
                    ImU32 c = IM_COL32(150, 158, 166, 170);
                    float a = std::min(h * 0.16f, gut * 0.30f);
                    // in a tree it also SAYS which way the node is: › closed,
                    // v open, the one glyph carrying both meanings
                    if (app.rbTree && rbHas(app.rbExpanded, r.full())) {
                        rdl->AddLine(ImVec2(cxm - a, cym - a * 0.5f), ImVec2(cxm, cym + a * 0.5f), c, 1.4f);
                        rdl->AddLine(ImVec2(cxm, cym + a * 0.5f), ImVec2(cxm + a, cym - a * 0.5f), c, 1.4f);
                    } else {
                        rdl->AddLine(ImVec2(cxm - a * 0.5f, cym - a), ImVec2(cxm + a * 0.5f, cym), c, 1.4f);
                        rdl->AddLine(ImVec2(cxm + a * 0.5f, cym), ImVec2(cxm - a * 0.5f, cym + a), c, 1.4f);
                    }
                } else {              // stack: three hairlines, barely there
                    ImU32 c = IM_COL32(130, 165, 200, 150);
                    float w = std::min(h * 0.36f, gut * 0.8f);
                    for (int k = -1; k <= 1; k++)
                        rdl->AddLine(ImVec2(cxm - w * 0.5f, cym + k * h * 0.18f),
                                     ImVec2(cxm + w * 0.5f, cym + k * h * 0.18f), c, 1.0f);
                }
            }
            if (rowClicked && servable) {
                ImGuiIO& sio = ImGui::GetIO();
                bool canSel = !r.isDir() && ei < (int)rbSel.size();
                if (sio.KeyCtrl && canSel) {
                    rbSel[ei] ^= 1;                    // toggle, plain click still opens
                    rbSelAnchor = ei;
                } else if (sio.KeyShift && canSel) {
                    // range in the order on SCREEN (sorted + filtered), anchor kept
                    int a = -1;
                    for (int k = 0; k < (int)shown.size(); k++)
                        if (shown[k] == rbSelAnchor) a = k;
                    if (a < 0) a = row;
                    for (int k = std::min(a, row); k <= std::max(a, row); k++)
                        if (!view[shown[k]].isDir() && (size_t)shown[k] < rbSel.size())
                            rbSel[shown[k]] = 1;
                } else {
                    rbActivateRow(r);
                    rbSelAnchor = ei;
                }
                rbCursor = ei;          // the keyboard picks up where the mouse left off
            }
            // Double-click = a registered open (the VSCode pinning gesture).
            // The first of the two clicks already made the preview; this
            // promotes it - or, for a stack row, opens the whole stack.
            if (servable && !r.isDir() && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                rbOpenRow(r);
            if (!servable) ImGui::PopStyleColor();   // before the popup, or it tints the menu
            if (!r.ph && ImGui::BeginPopupContextItem("ctx")) {
                std::string full = r.full();
                if (r.isDir()) {
                    if (ImGui::MenuItem("Open folder (all stacks below)"))
                        remoteScanFolder(full);
                    if (ImGui::MenuItem("Search under here")) {
                        rbSearchRoot = full;      // the search row shows and clears it
                        rbSearchFocus = true;
                    }
                    if (ImGui::MenuItem("Bookmark")) {
                        std::string u = placeUrl(B.host, B.port, full);
                        if (std::find(app.rbBookmarks.begin(), app.rbBookmarks.end(), u) ==
                            app.rbBookmarks.end()) {
                            app.rbBookmarks.push_back(u);
                            app.prefsDirty = true;
                            savePrefs();
                        }
                        toast("bookmarked " + u);
                    }
                    ImGui::Separator();
                } else if (r.isGroup()) {
                    if (ImGui::MenuItem("Open as stack")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(r.join(m));
                        openRemoteStack(B.host, files);
                    }
                    // the server aggregates WITHOUT opening: "is this set even
                    // worth transferring?" costs zero pixels this way. Group
                    // rows only exist from protocol 3 on, so no version gate.
                    if (ImGui::MenuItem("Temporal stats (server)")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(r.join(m));
                        std::string leaf = B.dir;
                        size_t sl2 = leaf.find_last_of('/');
                        if (sl2 != std::string::npos && sl2 + 1 < leaf.size())
                            leaf = leaf.substr(sl2 + 1);
                        requestBrowseTemporal(B.host, files, leaf + "/" + e.name);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("sigma_t / sigma_fpn computed on the server,\n"
                                          "shown in the Temporal panel - nothing opens,\n"
                                          "no pixel transfers. plane=all (no CFA split).");
                    ImGui::Separator();
                } else if (isNpyName(rname)) {
                    if (ImGui::MenuItem("Open"))
                        openRemote(makeRemoteUrl(B.host, full));
                    // one file as a stack: a frame-axis file becomes its frames
                    if (ImGui::MenuItem("Open as stack"))
                        openRemoteStack(B.host, { full });
                    // an expanded frame still knows the sequence it came from
                    if (r.member >= 0) {
                        char sl[64];
                        snprintf(sl, sizeof sl, "Open the whole sequence (%u frames)", e.frames);
                        if (ImGui::MenuItem(sl)) {
                            std::vector<std::string> files;
                            for (const auto& m : e.members) files.push_back(r.join(m));
                            openRemoteStack(B.host, files);
                        }
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Copy path")) {
                    ImGui::SetClipboardText(full.c_str());
                    toast("copied " + full);
                }
                if (!r.isDir() && ImGui::MenuItem("Properties...")) {
                    rbPropsEntry = e;
                    if (r.member >= 0) {          // an expanded frame: the group's
                        rbPropsEntry.name = rname;   // meta, but none of its totals
                        rbPropsEntry.group = false;
                        rbPropsEntry.frames = 0;
                        rbPropsEntry.members.clear();
                    }
                    rbPropsNoSize = r.member >= 0;
                    rbPropsPath = full;
                    rbPropsOpen = true;
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (!r.ph && !r.isDir() && isNpyName(rname))
                ImGui::TextDisabled("%s", fmtEntryShape(e).c_str());
            ImGui::TableNextColumn();
            // blank, not zero: the group reply has no per-frame size or mtime
            if (!r.ph && !r.isDir() && r.ownFile())
                ImGui::TextDisabled("%s", fmtBytesHuman(e.size).c_str());
            ImGui::TableNextColumn();
            if (!r.ph && r.ownFile() && e.mtime > 0)
                ImGui::TextDisabled("%s", fmtUnixTime(e.mtime).c_str());
            else if (!r.ph && !r.isDir() && r.ownFile()) ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Preview scrub bar lives BELOW the listing, in space reserved above:
    // appearing must never move the rows (double-click depends on it).
    if (app.previewFiles.size() >= 2) {
        int n = (int)app.previewFiles.size();
        ImGui::PushID("pvstep");
        if (ImGui::SmallButton("<")) stepPreviewFrame(-1);
        ImGui::SameLine();
        if (ImGui::SmallButton(">")) stepPreviewFrame(+1);
        ImGui::SameLine();
        int slider = app.previewIndex;
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9);
        if (ImGui::SliderInt("##pv", &slider, 0, n - 1, "frame %d") && slider != app.previewIndex)
            stepPreviewFrame(slider - app.previewIndex);
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d", app.previewIndex + 1, n);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("previewing %s\n(, and . step too; double-click the row to open it)",
                              app.previewLabel.c_str());
        // , and . while the browser has focus: the same keys the image view uses
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Comma, true))  stepPreviewFrame(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) stepPreviewFrame(+1);
        }
        ImGui::PopID();
    }
    if (rbPropsOpen) { ImGui::OpenPopup("Remote properties"); rbPropsOpen = false; }
    if (ImGui::BeginPopup("Remote properties")) {
        const remote::Entry& e = rbPropsEntry;
        // the path is the thing people need to paste into scripts: selectable
        char pathBuf[1024];
        snprintf(pathBuf, sizeof pathBuf, "%s", rbPropsPath.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        ImGui::InputText("##proppath", pathBuf, sizeof pathBuf, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy")) {
            ImGui::SetClipboardText(rbPropsPath.c_str());
            toast("copied " + rbPropsPath);
        }
        if (e.group)
            ImGui::Text("stack: %u frames (%s)", e.frames, e.name.c_str());
        if (rbPropsNoSize) {
            // Never invent one: the listing reply carries the group's total
            // bytes and its newest mtime, and no per-member breakdown.
            ImGui::TextDisabled("size      -   (not in the sequence listing)");
            ImGui::TextDisabled("modified  -   (not in the sequence listing)");
        } else {
        ImGui::Text("size      %s (%llu bytes)%s", fmtBytesHuman(e.size).c_str(),
                    (unsigned long long)e.size, e.group ? "  - all frames" : "");
        ImGui::Text("modified  %s (this machine's timezone)", fmtUnixTime(e.mtime).c_str());
        }
        ImGui::Text("shape     %s%s", fmtEntryShape(e).c_str(),
                    e.hasMeta && e.group ? "  - per frame" : "");
        if (!e.hasMeta)
            ImGui::TextDisabled("shape/dtype need a protocol 3 peer (File > Update remote peer)");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void drawFileList() {
    if (ImGui::Button("Open (O)")) openFileDialog();
    ImGui::SameLine();
    if (ImGui::Button("Close")) closeCurrent();
    if (app.seqRunning || !app.seqQueue.empty()) {
        int done = app.seqDone, total = app.seqTotal;
        if (app.seqRunning) ImGui::TextDisabled("loading %d/%d", done, total);
        else ImGui::TextDisabled("queued");
        if (!app.seqQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d stacks)", (int)app.seqQueue.size());
        }
        {   // keep Stop reachable: it is the only way to cancel a huge load
            const ImGuiStyle& st = ImGui::GetStyle();
            float stopW = ImGui::CalcTextSize("Stop").x + st.FramePadding.x * 2;
            float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x -
                         ImGui::GetCursorStartPos().x;
            if (ImGui::GetContentRegionAvail().x - used - st.ItemSpacing.x >= stopW) ImGui::SameLine();
        }
        if (ImGui::SmallButton("Stop")) { app.seqCancel = true; app.seqQueue.clear(); }
    }
    if (app.rfPending > 0) {
        ImGui::SameLine();
        // the ONLY way to cancel a remote fetch used to be quitting the app
        if (ImGui::SmallButton("Stop##rf")) {
            app.rfGen++;
            app.rbOpenQueue.clear();   // or the queue would just start the next stack
            std::lock_guard<std::mutex> lk(app.rfMtx);
            app.rfQueue.clear();
            app.rfPending = 0; app.rfTotal = 0; app.rfFetched = 0;
        }
    }
    if (app.rfPending > 0)
        ImGui::TextDisabled("remote: fetching %d/%d", app.rfFetched.load(), app.rfTotal.load());
    if (!app.rbOpenQueue.empty()) {   // stacks a folder scan has still to open
        ImGui::TextDisabled("remote: %d stack(s) queued", (int)app.rbOpenQueue.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##rq")) app.rbOpenQueue.clear();
    }
    if (!app.seqNote.empty()) {          // why the stack is short, kept in view
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(app.seqNote.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("dismiss##seqnote")) app.seqNote.clear();
    }
    ImGui::Separator();
    // Context-menu closes and moves are DEFERRED to the end of the walk:
    // closing erases images mid-iteration while the row loop still holds
    // indices into the pre-close stacksCached() snapshot, and a move prunes
    // app.batches while the submenu is iterating it. Every series command is
    // deferred for the same reason squared - the walk is iterating app.series
    // itself, and one join would resize it under the loop drawing it.
    int pendingCloseSeq = 0, pendingCloseBatch = 0;
    int pendingMoveSeq = 0, pendingMoveImg = -1, pendingMoveTarget = 0;
    int pendingSerEdit = 0, pendingSerMove = 0, pendingSerMoveTo = 0;
    int pendingSerUngroup = 0, pendingSerClose = 0, pendingSerRename = 0;
    std::string pendingSerName;
    int pendingJoinSeq = 0, pendingJoinSer = 0, pendingLeaveSeq = 0, pendingNewSerBatch = 0;
    // "Move to batch" submenu, shared by the stack row (seqctx) and the
    // standalone frame row (imgctx). Returns the chosen batch id, 0 = none.
    // Batch names go through a ### suffix: user text must not become the ID.
    auto moveToBatchMenu = [&](int curBatch) -> int {
        int target = 0;
        if (ImGui::BeginMenu("Move to batch")) {
            for (const auto& b : app.batches) {
                if (b.id == curBatch) continue;           // already there
                if (b.name == "preview") continue;        // pseudo-batch
                char lb2[300];
                snprintf(lb2, sizeof lb2, "%s###mb%d", b.name.c_str(), b.id);
                if (ImGui::MenuItem(lb2)) target = b.id;
            }
            ImGui::Separator();
            static char nbBuf[256] = "";
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            bool go = ImGui::InputTextWithHint("##newbatch", "New batch...",
                                               nbBuf, sizeof nbBuf,
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            go |= ImGui::SmallButton("create");
            if (go && nbBuf[0]) {
                target = newBatch(uniqueBatchName(nbBuf));
                nbBuf[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        return target;
    };
    // Group by source folder. Opening a folder of folders gives one stack per
    // subfolder, which reads fine - but opening several leaf folders in a row
    // produced a flat list of bare filenames with nothing saying where each came
    // from. The folder is the only thing that distinguishes them.
    const auto& stacks = stacksCached();
    // Grouped by BATCH, not by folder: a batch is one open action, named by the
    // user (folder name is only the starting value). Two loads from the same
    // folder are two batches - that is the point.
    struct FileGroup { int batch; std::string dir, label; std::vector<const std::vector<int>*> stacks; };
    static std::vector<FileGroup> groups;
    static uint64_t groupsRev = 0;
    if (groupsRev != app.imagesRev) {
        groupsRev = app.imagesRev;
        groups.clear();
        for (const auto& stack : stacks) {
            const ImageDoc& head = *app.images[stack.front()];
            FileGroup* g = nullptr;
            for (auto& q : groups) if (q.batch == head.batchId) { g = &q; break; }
            if (!g) {
                const std::string& p = head.path;
                size_t slash = p.find_last_of("/\\");
                std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
                std::string label = "opened";
                for (const auto& b : app.batches) if (b.id == head.batchId) label = b.name;
                groups.push_back({ head.batchId, dir, label, {} });
                g = &groups.back();
            }
            g->stacks.push_back(&stack);
        }
        // The transient preview is PINNED LAST and never reorders the rest.
        // It used to sort in wherever its batch was created, so glancing at a
        // file in the browser inserted a heading in the middle of the list and
        // pushed everything the user was working with down a row - the opposite
        // of what a throwaway look should cost.
        for (size_t i = 0; i + 1 < groups.size(); i++)
            if (groups[i].label == "preview") { std::rotate(groups.begin() + i,
                                                            groups.begin() + i + 1,
                                                            groups.end()); break; }
    }
    // The batch heading is ALWAYS drawn. It used to appear only once there was
    // more than one thing open, so opening a second folder made a heading
    // materialise above rows that had none - and a batch you can rename and
    // move things into cannot be a thing that exists only sometimes.
    const bool showHeaders = !groups.empty();
    for (const auto& group : groups) {
      ImGui::PushID(group.batch);
      bool open = true;
      if (showHeaders) {
          bool isPreview = group.label == "preview";
          if (isPreview) ImGui::PushStyleColor(ImGuiCol_Text,
                                               ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
          open = isPreview
              ? ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                           ImGuiTreeNodeFlags_SpanAvailWidth,
                                  "preview (transient)   (%d)", (int)group.stacks.size())
              : ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                           ImGuiTreeNodeFlags_SpanAvailWidth,
                                  "%s   (%d)", group.label.c_str(), (int)group.stacks.size());
          if (isPreview) ImGui::PopStyleColor();
          if (ImGui::IsItemHovered() && !group.dir.empty())
              ImGui::SetTooltip("%s\n\n(right-click to rename this batch)", group.dir.c_str());
          // the batch is the user's grouping, so its name is theirs to change
          if (ImGui::BeginPopupContextItem("batchctx")) {
              static char nameBuf[256];
              if (ImGui::IsWindowAppearing())
                  snprintf(nameBuf, sizeof nameBuf, "%s", group.label.c_str());
              ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
              bool done = ImGui::InputText("##bname", nameBuf, sizeof nameBuf,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
              ImGui::SameLine();
              done |= ImGui::SmallButton("rename");
              if (done && nameBuf[0]) {
                  for (auto& b : app.batches)
                      if (b.id == group.batch) b.name = nameBuf;
                  app.imagesRev++;               // rebuild the cached labels
                  ImGui::CloseCurrentPopup();
              }
              ImGui::Separator();
              if (ImGui::MenuItem("Close batch"))    // the batch layer's Close
                  pendingCloseBatch = group.batch;
              ImGui::EndPopup();
          }
      }
      if (open) {
        // name and format share one row: the dim/format part is right-aligned and dimmed
        auto rowWithMeta = [](const ImageDoc& d, const char* label, bool selected,
                              const char* extra = nullptr, bool isB = false) -> bool {
            const ImGuiStyle& st = ImGui::GetStyle();
            char meta[96];
            snprintf(meta, sizeof meta, "%dx%d %dch %s%s", d.w, d.h, d.ch, d.dtype.c_str(),
                     extra ? extra : "");
            float avail = ImGui::GetContentRegionAvail().x;
            float metaW = ImGui::CalcTextSize(meta).x;
            // The row is ONE item spanning the full width, and the metadata is
            // painted onto it rather than being a second item. It used to be a
            // name-width Selectable followed by a TextDisabled, which made the
            // metadata the "last item" - so right-clicking a stack opened the
            // context menu only over that dim strip on the right, and never on
            // the name, where everyone aims.
            float nameW = std::max(avail - metaW - st.ItemSpacing.x, ImGui::GetFontSize() * 4.0f);
            bool room = avail > metaW + ImGui::GetFontSize() * 6.0f;   // else: name only
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(p0, ImVec2(p0.x + (room ? nameW : avail),
                                           p0.y + ImGui::GetFrameHeight()), true);
            bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns,
                                             ImVec2(avail, 0));
            ImGui::PopClipRect();
            bool hov = ImGui::IsItemHovered();
            if (isB) {   // which row is the compare partner, without hunting a menu
                ImVec2 m = ImGui::GetItemRectMin();
                float h = ImGui::GetTextLineHeight();
                ImVec2 tp(m.x + 2 * app.uiScale,
                          m.y + (ImGui::GetItemRectSize().y - h) * 0.5f);
                ImGui::GetWindowDrawList()->AddText(tp, IM_COL32(120, 190, 255, 255), "B");
            }
            if (room) {   // draw-list, not an item: the row must stay the last item
                ImVec2 m = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(m.x + avail - metaW, m.y + (ImGui::GetItemRectSize().y -
                                                       ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), meta);
            }
            if (hov) ImGui::SetTooltip("%s\n%s", d.path.c_str(), meta);
            return clicked;
        };
        // One row. mem != nullptr means the row is being drawn INSIDE a series
        // node: it is indented by the node and the member's VALUE leads the
        // label. Everything else about the row is identical - a member is not
        // a different kind of stack, it is a stack that is also in a sweep.
        auto drawStackRow = [&](const std::vector<int>& stack, const App::Series* ser,
                                const App::Series::Member* mem) {
          const ImageDoc& head = *app.images[stack.front()];
          if (head.seqId == 0) {
            int i = stack.front();
            char lb[512];
            snprintf(lb, 512, "  %s##%d", head.name.c_str(), i);
            ImGui::PushID(i);
            const ImageDoc* bnow = cmpB();
            if (rowWithMeta(head, lb, i == app.current, nullptr, bnow == &head)) {
                selectImage(i);
                if (app.fitOnSwitch) app.fitRequested = true;
            }
            // a standalone frame hangs off the batch directly, so it gets the
            // move menu itself (stacks move via seqctx below)
            if (ImGui::BeginPopupContextItem("imgctx")) {
                // by IDENTITY, not by name: the B-image menu lists names, and
                // two batches full of frame_001.npy made it a coin toss
                if (ImGui::MenuItem("Set as compare B", nullptr, false,
                                    app.images[i].get() != cur())) {
                    setCompareB(app.images[i].get());
                    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
                }
                ImGui::Separator();
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveImg = i; pendingMoveTarget = t; }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            return;
          }
          App::SeqInfo* si = seqInfo(head.seqId);
          int pos = 0;
          bool active = false;
          for (int k = 0; k < (int)stack.size(); k++)
              if (stack[k] == app.current) { pos = k; active = true; }
          ImGui::PushID(head.seqId);
          char lb[600];
          const char* sname = si ? si->name.c_str() : "sequence";
          if (mem) {
              // The value LEADS the label. It is the reason the row is in this
              // series at all, and it is not metadata: an unset one has to be
              // readable as unset, not inferable from a blank column at the
              // right edge. The "##m<seqId>" suffix is the row's ID, so renaming
              // the stack does not reset its widget state - it does NOT protect
              // the text: ImGui cuts a label at the FIRST "##", so a stack the
              // user renames to "a##b" renders as "a" in either shape of row.
              char vb[64];
              if (std::isfinite(mem->value)) {
                  if (ser->unit[0]) snprintf(vb, sizeof vb, "%.6g %s", mem->value, ser->unit);
                  else snprintf(vb, sizeof vb, "%.6g", mem->value);
              } else {
                  snprintf(vb, sizeof vb, "value unset");
              }
              snprintf(lb, sizeof lb, "%s · %s##m%d", vb, sname, head.seqId);
          } else {
              snprintf(lb, sizeof lb, "  %s", sname);
          }
          char frames[24];
          snprintf(frames, sizeof frames, "  %df", (int)stack.size());   // frame count
          const ImageDoc* bnow = cmpB();
          bool stackHasB = false;      // B is a FRAME; mark the stack it lives in
          if (bnow) for (int idx : stack) if (app.images[idx].get() == bnow) stackHasB = true;
          if (rowWithMeta(head, lb, active, frames, stackHasB)) {
              selectImage(stack[pos]);
              if (app.fitOnSwitch) app.fitRequested = true;
          }
          // The stack is the unit measurements attach to, so it earns a name of
          // its own: "the 25C dark set", not whatever the capture script called
          // the folder. F2 / right-click renames; the folder name is only the
          // starting value.
          if (si && ImGui::BeginPopupContextItem("seqctx")) {
            static char renameBuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(renameBuf, sizeof renameBuf, "%s", si->name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##rename", renameBuf, sizeof renameBuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && renameBuf[0]) {
                si->name = renameBuf;
                app.lin.rev++;            // linearity rows show stack names
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            {   // by identity, not name - same reason as the frame row's item.
                // B = the frame this stack is showing; with "B follows A's
                // frame number" on, it tracks A from there anyway.
                ImageDoc* bpick = app.images[stack.front()].get();
                if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
                    app.images[si->lastImageIdx]->seqId == si->id)
                    bpick = app.images[si->lastImageIdx].get();
                if (ImGui::MenuItem("Set as compare B", nullptr, false, bpick != cur())) {
                    setCompareB(bpick);
                    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
                }
            }
            ImGui::Separator();
            // The series (系列) this stack is in, or could be. Multi-select is
            // a later phase; until then this menu is how a stack joins a sweep
            // without going through the Linearity panel.
            if (ImGui::BeginMenu("Series")) {
                const App::Series* mine = seriesOfStack(si->id);
                // The value joining would give this stack, BEFORE the click. The
                // modal shows its proposal and the picker paints one per row;
                // this was the one place a number entered a measurement without
                // having been on screen first (docs/series-plan.md §2 = 確認).
                std::string jfrom;
                double jv = extractLevelFromName(si->name, &jfrom);
                if (std::isfinite(jv)) ImGui::TextDisabled("joins at %.6g (from \"%s\")",
                                                           jv, jfrom.c_str());
                else ImGui::TextDisabled("joins with NO value (nothing numeric in the name)");
                int listed = 0;
                for (const auto& s : app.series) {
                    bool same = s.batchId == head.batchId;
                    bool isMine = mine && mine->id == s.id;
                    char lb2[416];
                    if (std::isfinite(jv) && same && !isMine)
                        snprintf(lb2, sizeof lb2, "%s   -> %.6g %s###addser%d", s.name.c_str(),
                                 jv, s.unit[0] ? s.unit : "[unit not set]", s.id);
                    else
                        snprintf(lb2, sizeof lb2, "%s###addser%d", s.name.c_str(), s.id);
                    if (ImGui::MenuItem(lb2, nullptr, isMine, same && !isMine)) {
                        pendingJoinSeq = si->id;
                        pendingJoinSer = s.id;
                    }
                    // A series cannot widen to reach another batch: the answer
                    // is to move the stack first, and the menu says so instead
                    // of leaving a dead item with no reason.
                    if (!same && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("\"%s\" is in batch \"%s\".\n"
                                          "A series lives in ONE batch - use Move to batch\n"
                                          "first, then add it here.",
                                          s.name.c_str(), batchNameOf(s.batchId).c_str());
                    listed++;
                }
                if (!listed) ImGui::TextDisabled("(no series yet)");
                ImGui::Separator();
                if (ImGui::MenuItem("New series...")) pendingNewSerBatch = head.batchId;
                if (mine && ImGui::MenuItem("Remove from this series"))
                    pendingLeaveSeq = si->id;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            {   // the STACK moves, whole - per the canon's frame ⊂ stack ⊂ batch
                if (const App::Series* mine = seriesOfStack(si->id))
                    ImGui::TextDisabled("in series \"%s\": moving it alone leaves it",
                                        mine->name.c_str());
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveSeq = si->id; pendingMoveTarget = t; }
            }
            if (ImGui::MenuItem("Close stack"))     // the stack layer's Close
                pendingCloseSeq = si->id;
            ImGui::EndPopup();
          }
          if (active && ImGui::IsKeyPressed(ImGuiKey_F2, false))
              ImGui::OpenPopup("seqctx");
          // No frame slider here: it used to appear under the active row, and a
          // row that grows on selection makes the whole list jump - the scrub bar
          // lives at the bottom of the Image View now, where the frames are.
          ImGui::PopID();
        };
        // The stack of one seqId, out of this batch's cached list (members are
        // named by seqId; the panel walks by stack).
        auto stackOfSeq = [&](int seqId) -> const std::vector<int>* {
            for (const auto* sp : group.stacks)
                if (app.images[sp->front()]->seqId == seqId) return sp;
            return nullptr;
        };
        // ---- the batch's series come FIRST, each with its members inside -----
        // Non-member stacks keep their place and their indentation below: a
        // sweep appearing must not push everything else down a level.
        for (const auto& S : app.series) {
            if (S.batchId != group.batch) continue;
            ImGui::PushID(S.id);
            const ImGuiStyle& st = ImGui::GetStyle();
            char smeta[96];
            // the unit is the series', and "not set" is a state to read, not a
            // blank to guess at (the panel says the same thing the same way)
            if (S.unit[0]) snprintf(smeta, sizeof smeta, "[%s]  (%d)", S.unit,
                                    (int)S.members.size());
            else snprintf(smeta, sizeof smeta, "[unit not set]  (%d)", (int)S.members.size());
            float avail = ImGui::GetContentRegionAvail().x;
            float mw = ImGui::CalcTextSize(smeta).x;
            bool room = avail > mw + ImGui::GetFontSize() * 8.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            // same rule as the stack rows: ONE item spanning the row, the unit
            // and the count PAINTED on it. A second widget on the right is what
            // made a context menu unreachable on the name once already.
            if (room) ImGui::PushClipRect(p0, ImVec2(p0.x + avail - mw - st.ItemSpacing.x,
                                                     p0.y + ImGui::GetFrameHeight()), true);
            bool sopen = ImGui::TreeNodeEx("##ser", ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_SpanAvailWidth,
                                           "%s", S.name.c_str());
            if (room) ImGui::PopClipRect();
            if (room) {
                ImVec2 m = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(m.x + avail - mw, m.y + (ImGui::GetItemRectSize().y -
                                                    ImGui::GetTextLineHeight()) * 0.5f),
                    S.unit[0] ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                              : IM_COL32(255, 184, 90, 255), smeta);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("series (系列): %d stack(s) of one swept %s\n\n"
                                  "(right-click: rename, edit, move, ungroup, close)",
                                  (int)S.members.size(),
                                  S.paramName.empty() ? "parameter" : S.paramName.c_str());
            if (ImGui::BeginPopupContextItem("serctx")) {
                static char sbuf[256];
                if (ImGui::IsWindowAppearing())
                    snprintf(sbuf, sizeof sbuf, "%s", S.name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                bool done = ImGui::InputText("##sname", sbuf, sizeof sbuf,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                done |= ImGui::SmallButton("rename");
                if (done && sbuf[0]) {
                    pendingSerRename = S.id;
                    pendingSerName = sbuf;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                // the same modal creation uses: members, values, order, unit
                if (ImGui::MenuItem("Edit series...")) {
                    pendingSerEdit = S.id;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                {   // the SERIES moves - every member with it, so that
                    // series ⊂ batch never stops being true on the way
                    int t = moveToBatchMenu(S.batchId);
                    if (t) { pendingSerMove = S.id; pendingSerMoveTo = t; }
                    ImGui::TextDisabled("   (all %d member(s) move with it)",
                                        (int)S.members.size());
                }
                ImGui::Separator();
                // Two DIFFERENT operations, spelled out rather than one word
                // with a modifier: one keeps the data, the other destroys it.
                if (ImGui::MenuItem("Ungroup (keep the stacks)")) pendingSerUngroup = S.id;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Removes the series only. All %d stack(s) stay open,\n"
                                      "exactly where they are - they just stop being a sweep.",
                                      (int)S.members.size());
                if (ImGui::MenuItem("Close series (discard its stacks)"))
                    pendingSerClose = S.id;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Closes the %d stack(s) in it, every frame with them.",
                                      (int)S.members.size());
                ImGui::EndPopup();
            }
            if (sopen) {
                for (const auto& m : S.members)
                    if (const std::vector<int>* sp = stackOfSeq(m.seqId))
                        drawStackRow(*sp, &S, &m);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // ---- then everything this batch holds that is NOT in a series -------
        for (const auto& stackPtr : group.stacks) {
            const ImageDoc& head = *app.images[stackPtr->front()];
            if (head.seqId != 0 && seriesOfStack(head.seqId)) continue;   // drawn above
            drawStackRow(*stackPtr, nullptr, nullptr);
        }
      }
      if (showHeaders && open) ImGui::TreePop();
      ImGui::PopID();
    }
    if (pendingCloseSeq) closeStack(pendingCloseSeq);
    if (pendingCloseBatch) closeBatch(pendingCloseBatch);
    if (pendingMoveTarget && pendingMoveSeq) moveStackToBatch(pendingMoveSeq, pendingMoveTarget);
    if (pendingMoveTarget && pendingMoveImg >= 0) moveImageToBatch(pendingMoveImg, pendingMoveTarget);
    // ---- the series commands, after the walk that drew them -----------------
    if (pendingSerRename)
        if (App::Series* S = seriesById(pendingSerRename)) {
            S->name = pendingSerName;
            app.imagesRev++;
            app.lin.rev++;                // the panel's selector shows this name
        }
    if (pendingSerEdit)
        if (App::Series* S = seriesById(pendingSerEdit)) openSeriesModal(S->batchId, S->id);
    if (pendingSerMove && pendingSerMoveTo) moveSeriesToBatch(pendingSerMove, pendingSerMoveTo);
    if (pendingSerUngroup) ungroupSeries(pendingSerUngroup);
    if (pendingSerClose) closeSeries(pendingSerClose);
    if (pendingNewSerBatch) openSeriesModal(pendingNewSerBatch, 0);
    if (pendingLeaveSeq) {
        App::Series* S = seriesOfStack(pendingLeaveSeq);
        App::SeqInfo* si = seqInfo(pendingLeaveSeq);
        if (S) {
            std::string n = S->name;
            removeFromSeries(pendingLeaveSeq);
            pruneEmptySeries();
            linFitStale();          // the fit below counted this point
            toast((si ? si->name : std::string("stack")) + " left series \"" + n + "\"");
        }
    }
    if (pendingJoinSeq && pendingJoinSer) {
        std::string msg;
        bool joined = seriesJoinFromMenu(pendingJoinSer, pendingJoinSeq, &msg);
        if (!msg.empty()) toast(msg, !joined);
    }
}

static void drawSequenceModal() {
    if (app.seqAskImage >= 0 && !ImGui::IsPopupOpen("Load sequence?"))
        ImGui::OpenPopup("Load sequence?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Load sequence?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("%d files match %s", (int)app.seqAskFiles.size(), app.seqAskPattern.c_str());
    ImGui::TextDisabled("Loading them as one stack enables temporal analysis.");
    ImGui::TextDisabled("Frames decode in the background; you can keep working.");
    ImGui::Separator();
    static bool remember = false;
    ImGui::Checkbox("remember my choice (File > Sequence loading)", &remember);
    if (ImGui::Button("Load sequence", ImVec2(150 * app.uiScale, 0))) {
        startSequenceLoad(app.seqAskImage, app.seqAskFiles, app.seqAskPattern);
        if (remember) app.seqLoadMode = 1;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("This file only", ImVec2(150 * app.uiScale, 0))) {
        if (remember) app.seqLoadMode = 2;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- menu bar / dialogs
#if defined(__APPLE__)
  #define SC_MOD "Cmd"
#else
  #define SC_MOD "Ctrl"
#endif

// Is what we are looking at somewhere else? The session being up counts, and so
// does an image that came from one: a dropped connection does not make the
// pixels local. Drives the icon variant and the "[host]" in the title.
static bool viewingRemote() {
    const ImageDoc* im = cur();
    return app.rbrowse.connected || (im && !im->remoteUrl.empty());
}

// One title, three places: the OS title bar (when there is one), the taskbar
// button, and the integrated title bar this app draws for itself.
static std::string windowTitleText() {
    const ImageDoc* im = cur();
    std::string t = im ? im->name + " - viewer" : "viewer v0.1";
    if (viewingRemote()) {
        std::string host = app.rbrowse.host;
        if (host.empty() && im && !im->remoteUrl.empty()) {
            std::string rp;
            remote::parseUrl(im->remoteUrl, host, rp);
        }
        t += "  [" + (host.empty() ? std::string("local peer") : host) + "]";
    }
    return t;
}

// The integrated title bar, drawn into the menu bar the app already has: the
// window title in the middle, the three window buttons on the right, and
// everything else registered as "drag here to move the window".
//
// Called from inside BeginMainMenuBar, after the last menu, so the menus'
// right edge is simply where the cursor has got to.
static void drawTitleBarExtras() {
    if (window_frame::mode() != window_frame::Integrated) return;
    const float s = app.uiScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;   // screen -> window coords
    const ImVec2 barPos = ImGui::GetWindowPos();
    const ImVec2 barSize = ImGui::GetWindowSize();
    const float menusEnd = ImGui::GetCursorScreenPos().x;
    const float bw = std::floor(46 * s), bh = barSize.y;
    const float btnX0 = barPos.x + barSize.x - bw * 3;

    // everything but the menus and the buttons drags the window
    window_frame::addCaption(barPos.x - vpPos.x, barPos.y - vpPos.y,
                             barPos.x + barSize.x - vpPos.x, barPos.y + bh - vpPos.y);
    window_frame::addExclusion(barPos.x - vpPos.x, barPos.y - vpPos.y,
                               menusEnd - vpPos.x, barPos.y + bh - vpPos.y);
    window_frame::addExclusion(btnX0 - vpPos.x, barPos.y - vpPos.y,
                               barPos.x + barSize.x - vpPos.x, barPos.y + bh - vpPos.y);

    // Title, centred in the bar but never under the menus or the buttons: it is
    // the only place the file name appears once the system title bar is gone.
    {
        const std::string title = windowTitleText();
        const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
        const float pad = 12 * s;
        const float lo = menusEnd + pad, hi = btnX0 - pad;
        if (hi - lo > 40 * s) {
            float tx = barPos.x + (barSize.x - ts.x) * 0.5f;
            tx = std::clamp(tx, lo, std::max(lo, hi - ts.x));
            dl->PushClipRect(ImVec2(lo, barPos.y), ImVec2(hi, barPos.y + bh), true);
            dl->AddText(ImVec2(tx, barPos.y + (bh - ts.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), title.c_str());
            dl->PopClipRect();
        }
    }

    // Minimize / maximize / close. Drawn rather than typed: a glyph font would
    // have to be shipped and would still not match at every UI scale.
    const bool isMax = window_frame::maximized();
    const char* ids[3] = {"##wf_min", "##wf_max", "##wf_close"};
    for (int i = 0; i < 3; i++) {
        const ImVec2 p(btnX0 + bw * i, barPos.y);
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton(ids[i], ImVec2(bw, bh));
        const bool clicked = ImGui::IsItemClicked();
        // On Windows the OS owns the maximize button (it opens Snap Layouts on
        // hover), so its hover state has to come from there - no mouse message
        // for that rectangle ever reaches us.
        const bool hovered = ImGui::IsItemHovered() ||
                             (i == 1 && window_frame::maximizeButtonHot());
        if (hovered)
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh),
                              i == 2 ? IM_COL32(232, 72, 72, 255)
                                     : ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        const ImU32 fg = ImGui::GetColorU32(i == 2 && hovered ? ImVec4(1, 1, 1, 1)
                                                              : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const ImVec2 c(p.x + bw * 0.5f, p.y + bh * 0.5f);
        const float g = std::floor(5 * s), th = std::max(1.0f, std::floor(s));
        if (i == 0) {
            dl->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), fg, th);
        } else if (i == 1 && !isMax) {
            dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, 0, 0, th);
        } else if (i == 1) {                       // restore: the "behind" pane too
            dl->AddRect(ImVec2(c.x - g, c.y - g + 2 * th), ImVec2(c.x + g - 2 * th, c.y + g),
                        fg, 0, 0, th);
            dl->AddLine(ImVec2(c.x - g + 2 * th, c.y - g), ImVec2(c.x + g, c.y - g), fg, th);
            dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x + g, c.y + g - 2 * th), fg, th);
        } else {
            dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, th);
            dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x - g, c.y + g), fg, th);
        }
        if (i == 1)
            window_frame::setMaximizeButton(p.x - vpPos.x, p.y - vpPos.y,
                                            p.x + bw - vpPos.x, p.y + bh - vpPos.y);
        if (clicked) {
            if (i == 0) window_frame::minimize();
            else if (i == 1) window_frame::toggleMaximize();
            else window_frame::requestClose();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i == 0 ? "Minimize"
                                           : i == 1 ? (isMax ? "Restore" : "Maximize") : "Close");
    }
}

static void drawMenuBar(GLFWwindow* win) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", SC_MOD "+O")) openFileDialog();
        if (ImGui::MenuItem("Open Folder...", SC_MOD "+Shift+O")) openFolderDialog();
        // The local mirror of browsing a server: look around, preview, use the
        // server-side stats - without loading anything. Same Browse panel, the
        // peer just runs on this machine.
        if (ImGui::MenuItem("Browse Folder (Local)...")) browseFolderDialog();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("open a folder in the Browse panel: list, preview\n"
                              "and measure without loading anything");
        // The OS dialog can only show THIS machine's disks (the NAS included,
        // since it is mounted). Files on a server need the ssh:// path, so they
        // need a place to type it.
        // The ONE remote entry point. "Open File (Remote)..." and "Open Folder
        // (Remote)..." used to sit under it and are gone: see the panel's own
        // toolbar. Revealing the panel here is what both of them really did.
        if (ImGui::MenuItem("Start Remote (ssh)...")) {
            app.showRemote = true;
            app.focusRemote = true;
            app.remoteDlgOpen = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("connect to a machine. Opening files and folders\n"
                              "happens in the Browse panel: click a file to look\n"
                              "at it, double-click to open it, \"open folder\" for\n"
                              "every stack below the folder you are in.");
        if (ImGui::MenuItem("Update remote peer", nullptr, false, app.rbrowse.connected)) {
            App::RbJob j; j.kind = App::RbUpdatePeer;
            j.host = app.rbrowse.host; j.port = app.rbrowse.port;
            rbEnqueue(std::move(j));
        }
        if (ImGui::MenuItem("Save Session...", SC_MOD "+S", false, !app.images.empty())) saveSessionDialog();
        {   // recovery: the autosave is written on exit, on crash and every 60 s
            std::string ap = autosavePath();
            std::error_code aec;
            bool has = !ap.empty() && std::filesystem::exists(std::filesystem::u8path(ap), aec);
            if (ImGui::MenuItem("Restore last session", nullptr, false, has)) openPath(ap);
            if (has && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ap.c_str());
        }
        ImGui::Separator();
        {   // Close follows the layer canon (docs/terminology.md): Ctrl+W closes
            // the STACK when the current frame is in one, the image otherwise.
            // Ctrl+Alt+W is the one-frame escape hatch; Ctrl+Shift+W the batch.
            bool inStack = cur() && cur()->seqId != 0;
            if (ImGui::MenuItem(inStack ? "Close Stack" : "Close Image", SC_MOD "+W",
                                false, cur() != nullptr))
                closeCurrent();
            if (ImGui::MenuItem("Close Frame", SC_MOD "+Alt+W", false, inStack))
                closeCurrent(true);
            if (ImGui::MenuItem("Close Batch", SC_MOD "+Shift+W", false, cur() != nullptr))
                closeBatch(cur()->batchId);
        }
        if (ImGui::MenuItem("Close All", nullptr, false, !app.images.empty())) closeAll();
        ImGui::Separator();
        if (ImGui::BeginMenu("Sequence loading")) {
            if (ImGui::MenuItem("Ask each time", nullptr, app.seqLoadMode == 0)) app.seqLoadMode = 0;
            if (ImGui::MenuItem("Always load folder", nullptr, app.seqLoadMode == 1)) app.seqLoadMode = 1;
            if (ImGui::MenuItem("Never (single file)", nullptr, app.seqLoadMode == 2)) app.seqLoadMode = 2;
            ImGui::Separator();
            {   // how many frames fit is a memory question, so say it in memory terms
                ImGui::TextDisabled("Memory budget");
                float gb = app.memBudgetGB > 0 ? app.memBudgetGB
                                               : (float)(seqMemBudget() / 1073741824.0);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                if (ImGui::InputFloat("GB##membudget", &gb, 1.0f, 8.0f, "%.1f"))
                    app.memBudgetGB = std::clamp(gb, 0.5f, 4096.0f);
                if (ImGui::MenuItem("auto (60% of RAM)", nullptr, app.memBudgetGB <= 0))
                    app.memBudgetGB = 0;
                ImGui::TextDisabled("  in use: %.1f GB of %.1f GB",
                                    residentImageBytes() / 1073741824.0,
                                    seqMemBudget() / 1073741824.0);
                ImGui::Separator();
            }
            {   // where remote analysis runs
                ImGui::TextDisabled("Remote processing");
                if (ImGui::MenuItem("  auto (server for remote data)", nullptr,
                                    app.procPolicy == App::PolAuto))
                    app.procPolicy = App::PolAuto;
                if (ImGui::MenuItem("  server (measure on the server)", nullptr,
                                    app.procPolicy == App::PolServer))
                    app.procPolicy = App::PolServer;
                if (ImGui::MenuItem("  local fetch (bring frames here)", nullptr,
                                    app.procPolicy == App::PolLocalFetch))
                    app.procPolicy = App::PolLocalFetch;
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Load sequence for current image", nullptr, false,
                                cur() && cur()->seqId == 0 && !cur()->path.empty())) {
                std::string pat;
                std::vector<std::string> files = findSequenceSiblings(cur()->path, pat);
                if (files.size() >= 2) startSequenceLoad(app.current, files, pat);
                else toast("no numbered siblings found next to this file", true);
            }
            if (ImGui::MenuItem("Stop background loading", nullptr, false, app.seqRunning))
                app.seqCancel = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
#if defined(__APPLE__)
        if (ImGui::MenuItem("Quit", "Cmd+Q")) glfwSetWindowShouldClose(win, 1);
#else
        if (ImGui::MenuItem("Exit", "Alt+F4")) glfwSetWindowShouldClose(win, 1);
#endif
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        bool has = cur() != nullptr;
        bool inSeq = cur() && cur()->seqId != 0;
        if (ImGui::MenuItem("Next frame", "Right / Ctrl+F", false, inSeq)) gotoFrame(1);
        if (ImGui::MenuItem("Previous frame", "Left / Ctrl+B", false, inSeq)) gotoFrame(-1);
        if (ImGui::MenuItem("First frame", "Home / Ctrl+A", false, inSeq)) gotoFrame(0, true, true);
        if (ImGui::MenuItem("Last frame", "End / Ctrl+E", false, inSeq)) gotoFrame(0, true, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Next stack", "Down / Ctrl+N", false, app.images.size() > 1)) gotoStack(1);
        if (ImGui::MenuItem("Previous stack", "Up / Ctrl+P", false, app.images.size() > 1)) gotoStack(-1);
        ImGui::Separator();
        if (ImGui::BeginMenu("Compare A/B", app.images.size() > 1)) {
            // every mode entry picks a B if there is none: a checked mode that
            // shows nothing is the worst state this menu could leave you in
            if (ImGui::MenuItem("Off", "\\ or C", app.compareMode == App::CmpOff))
                app.compareMode = App::CmpOff;
            if (ImGui::MenuItem("Wipe (drag the divider)", nullptr, app.compareMode == App::CmpWipe))
                { app.compareMode = App::CmpWipe; ensureCompareB(); }
            if (ImGui::MenuItem("Side by side", nullptr, app.compareMode == App::CmpSplit))
                { app.compareMode = App::CmpSplit; ensureCompareB(); }
            if (ImGui::MenuItem("Difference (A-B)", nullptr, app.compareMode == App::CmpDiff))
                { app.compareMode = App::CmpDiff; ensureCompareB(); }
            if (ImGui::MenuItem("Blink (flip A/B in place)", nullptr, app.compareMode == App::CmpFlip))
                { app.compareMode = App::CmpFlip; ensureCompareB(); }
            if (app.compareMode == App::CmpFlip) {
                if (ImGui::MenuItem("  auto blink", "Space / B toggles", app.flipAuto))
                    app.flipAuto = !app.flipAuto;
            }
            if (app.compareMode == App::CmpDiff) {
                if (ImGui::MenuItem("  magnitude only |A-B|", nullptr, app.diffAbs))
                    app.diffAbs = !app.diffAbs;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("B follows A's frame number", nullptr, app.compareFollowFrame))
                app.compareFollowFrame = !app.compareFollowFrame;
            {   // the display range relationship, where the compare modes live
                static const char* RN[3] = { "  each side keeps its own range",
                                             "  B uses A's range",
                                             "  auto over both (union)" };
                ImGui::TextDisabled("Display range");
                for (int r = 0; r < 3; r++)
                    if (ImGui::MenuItem(RN[r], nullptr, app.compareRangeMode == r)) {
                        app.compareRangeMode = r;
                        if (ImageDoc* bb = cmpB()) bb->texDirty = true;
                        if (cur()) cur()->texDirty = true;
                    }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Two images stretched differently cannot be compared -\n"
                                  "what you would be looking at is the stretch. Display\n"
                                  "only: B keeps its own black/white.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Two stacks: stepping A steps B to the SAME frame number,\n"
                                  "so frame 42 is compared against frame 42.\n"
                                  "Turn it off to hold B still - which is what you want when\n"
                                  "B is one reference image (a dark, a golden sample).\n"
                                  "A single-image B never moves either way.");
            if (ImGui::MenuItem("Pin this frame as B", "Shift+B", false, cur() != nullptr)) pinCurrentAsB();
            if (ImGui::MenuItem("Swap A and B", "Shift+\\ or Shift+C", false, cmpB() != nullptr))
                swapCompare();
            ImGui::Separator();
            // ONE setting for every statistics panel: "same arrangement as the
            // image" is a property of the comparison, not of the histogram.
            ImGui::TextDisabled("Statistics panels");
            if (ImGui::MenuItem("  Auto (follow the image layout)", nullptr,
                                app.abStatsLayout == App::AbAuto))
                app.abStatsLayout = App::AbAuto;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Side by side images (split) -> side by side plots.\n"
                                  "Wipe / blink / difference have one image area,\n"
                                  "so their plots overlay.");
            if (ImGui::MenuItem("  Overlay (A solid, B dashed)", nullptr,
                                app.abStatsLayout == App::AbOverlay))
                app.abStatsLayout = App::AbOverlay;
            if (ImGui::MenuItem("  Side by side (A on the left)", nullptr,
                                app.abStatsLayout == App::AbSide))
                app.abStatsLayout = App::AbSide;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Always 50/50 and always A on the left: two shapes\n"
                                  "can only be compared on plots of equal width.\n"
                                  "Narrow panels fall back to the overlay.");
            ImGui::Separator();
            ImGui::TextDisabled("B image");
            // one row per stack (its frame in view), not one per frame: a
            // 120-frame stack must not produce a 120-row menu
            int shown = 0;
            for (int i = 0; i < (int)app.images.size(); i++) {
                ImageDoc* d = app.images[i].get();
                if (d == cur()) continue;
                if (d->seqId != 0) {
                    App::SeqInfo* si = seqInfo(d->seqId);
                    int rep = si && si->lastImageIdx >= 0 ? si->lastImageIdx : -1;
                    if (rep < 0) { std::vector<int> fr = framesOfSeq(d->seqId); rep = fr.empty() ? -1 : fr.front(); }
                    if (rep != i) continue;                  // this stack is listed once
                }
                if (++shown > 40) { ImGui::TextDisabled("... %d more (use Files)",
                                                        (int)app.images.size() - shown); break; }
                // by STACK name, and with the uid as the ImGui id: two stacks of
                // one series list identically named frames, so "frame_000.npy"
                // twice was the whole menu's content (### also keeps a name
                // containing ## from eating its own label)
                char lbl[320];
                snprintf(lbl, sizeof lbl, "%s%s###b%llu", abDocLabel(d).c_str(),
                         d->seqId != 0 ? "   [stack]" : "",
                         (unsigned long long)d->uid);
                if (ImGui::MenuItem(lbl, nullptr, app.compareBUid == d->uid)) {
                    setCompareB(d);
                    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fit to Window", "F", false, has)) app.fitRequested = true;
        if (ImGui::MenuItem("Actual Size (100%)", "1", false, has)) app.view.zoom = 1.0f;
        if (ImGui::MenuItem("Zoom In", "+", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
        if (ImGui::MenuItem("Zoom Out", "-", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        ImGui::Separator();
        ImGui::MenuItem("Pixel Grid", "G", &app.showGrid);
        ImGui::MenuItem("Wheel zooms without Ctrl", nullptr, &app.wheelZoomPlain);
        ImGui::MenuItem("Left drag pans (Shift = new ROI)", nullptr, &app.dragPans);
        if (ImGui::MenuItem("Compact UI (dense rows)", nullptr, &app.compactUi))
            ui_theme::apply(app.themeVariant, app.themeAccent, app.uiScale, app.compactUi);
        ImGui::MenuItem("Show frame time", nullptr, &app.showFps);
        ImGui::MenuItem("Low bandwidth (remote / ssh)", nullptr, &app.lowBandwidth);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cap the redraw rate at ~20-30 fps and drop the\n"
                              "post-input tail. Saves bandwidth over X11 forwarding,\n"
                              "at ~32 ms of extra latency while dragging or scrolling\n"
                              "(0.4 ms otherwise). Locally, leave this off.");
        ImGui::MenuItem("Fit view when switching images", nullptr, &app.fitOnSwitch);
        {   // the title bar: ours (in this menu bar) or the desktop's, above us
            const bool can = window_frame::available();
            bool integrated = app.frameMode != 0;
            if (ImGui::MenuItem("Integrated title bar", nullptr, &integrated, can)) {
                app.frameMode = integrated ? 1 : 0;
                window_frame::setMode(integrated ? window_frame::Integrated
                                                 : window_frame::System);
                app.prefsDirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(can ? "Menus, title and window buttons in one bar,\n"
                                        "with no system title bar above the app.\n"
                                        "(--frame system starts with the system one,\n"
                                        "in case a window manager makes a mess of it.)"
                                      : "Not available here: %s",
                                  window_frame::unavailableReason());
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Files", nullptr, &app.showFiles);
            ImGui::MenuItem("Browse", nullptr, &app.showRemote);
            ImGui::MenuItem("Inspector", nullptr, &app.showInspector);
            ImGui::MenuItem("ROIs", nullptr, &app.showRois);
            ImGui::MenuItem("Analysis", nullptr, &app.showAnalysis);
            ImGui::MenuItem("Histogram", nullptr, &app.showHistogram);
            ImGui::MenuItem("Temporal", nullptr, &app.showTemporal);
            ImGui::MenuItem("Projection (H/V)", nullptr, &app.showProjection);
            ImGui::MenuItem("Linearity", nullptr, &app.showLinearity);
            if (ImGui::MenuItem("Messages", nullptr, &app.showMessages)) app.msgUnreadErr = false;
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                app.showFiles = app.showInspector = app.showRois = true;
                app.showAnalysis = app.showHistogram = app.showTemporal = true;
                app.showProjection = false;
                app.resetLayout = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Display Gamma")) {
            bool lin = app.dispGamma < 1.5f;
            if (ImGui::MenuItem("1.0 (linear)", nullptr, lin) && !lin) { app.dispGamma = 1.0f; markAllTexDirty(); }
            if (ImGui::MenuItem("2.2", nullptr, !lin) && lin)         { app.dispGamma = 2.2f; markAllTexDirty(); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Theme")) {
            bool changed = false;
            if (ImGui::MenuItem("Dark", nullptr, app.themeVariant == ui_theme::VariantDark))
                { app.themeVariant = ui_theme::VariantDark; changed = true; }
            if (ImGui::MenuItem("Light", nullptr, app.themeVariant == ui_theme::VariantLight))
                { app.themeVariant = ui_theme::VariantLight; changed = true; }
            ImGui::Separator();
            ImGui::TextDisabled("accent");
            for (int i = 0; i < ui_theme::accentCount(); i++)
                if (ImGui::MenuItem(ui_theme::accents()[i].name, nullptr, app.themeAccent == i))
                    { app.themeAccent = i; changed = true; }
            if (changed) ui_theme::apply(app.themeVariant, app.themeAccent, app.uiScale, app.compactUi);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (!plugin_host::analyzers().empty() && ImGui::BeginMenu("Measure")) {
        const auto& anas = plugin_host::analyzers();
        ImageDoc* im = cur();
        app.anaSel = std::clamp(app.anaSel, 0, (int)anas.size() - 1);
        // Rerun accelerator first: stepping through frames/images and pressing
        // M is the comparison loop, and the menu is where M gets discovered.
        std::string again = "Measure again: " + anas[app.anaSel].name;
        if (ImGui::MenuItem(again.c_str(), "M", false, im != nullptr))
            requestMeasure(app.anaSel);
        if (ImGui::MenuItem("Auto re-run on change", nullptr, app.anaAuto))
            app.anaAuto = !app.anaAuto;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("re-measure whenever the image, ROI set,\n"
                              "display range or CFA layout changes");
        ImGui::Separator();
        // one pass per question group so co-answering plugins sit together
        // regardless of registration order; then unknown categories verbatim
        std::vector<std::string> headers;
        for (const auto& g : MEASURE_GROUPS) {
            bool present = false, seen = false;
            for (const auto& a : anas) {
                std::string c, n;
                splitAnalyzerName(a.name, c, n);
                if (c == g.cat) present = true;
            }
            for (const auto& h : headers) if (h == g.group) seen = true;
            if (present && !seen) headers.push_back(g.group);
        }
        for (const auto& a : anas) {
            std::string c, n;
            splitAnalyzerName(a.name, c, n);
            if (measureGroupOf(c)) continue;
            bool seen = false;
            for (const auto& h : headers) if (h == c) seen = true;
            if (!seen) headers.push_back(c);
        }
        for (size_t hi = 0; hi < headers.size(); hi++) {
            if (hi) ImGui::Separator();
            ImGui::TextDisabled("%s", headers[hi].c_str());
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                const char* grp = measureGroupOf(c);
                if ((grp ? grp : c.c_str()) != headers[hi]) continue;
                const char* reason = analyzerDisabledReason(anas[i], im);
                // the right column keeps the plugin's identity (the category is
                // the standard number for iso* measurements); the precondition
                // moved into the tooltip, where there is room to say it fully
                if (ImGui::MenuItem(n.c_str(), c.c_str(), app.anaSel == i, !reason))
                    requestMeasure(i);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (reason) ImGui::SetTooltip("%s", reason);
                    else if (!anas[i].desc.empty()) ImGui::SetTooltip("%s", anas[i].desc.c_str());
                }
            }
            // Temporal noise is the stack half of the noise question. It is
            // host-computed (Temporal panel), not a frame analyzer, but the
            // user asking "how noisy?" must find it HERE, not by knowing the
            // implementation boundary.
            if (headers[hi] == "Noise / SNR") {
                bool inStack = im && im->seqId != 0;
                if (ImGui::MenuItem("temporal (Temporal panel)", nullptr, false, inStack)) {
                    app.showTemporal = true;
                    ImGui::SetWindowFocus("Temporal");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(inStack
                        ? "sigma_t / sigma_fpn over the loaded stack (whole frames or the selected ROI)"
                        : "needs a stack: load a numbered sequence first");
            }
        }
        ImGui::EndMenu();
    }
    if (!plugin_host::processors().empty() && ImGui::BeginMenu("Process")) {
        for (int i = 0; i < (int)plugin_host::processors().size(); i++)
            if (ImGui::MenuItem(plugin_host::processors()[i].name.c_str(), nullptr, false, cur() != nullptr))
                runProcessor(i);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts", "H")) app.showHelp = true;
        if (ImGui::MenuItem("About viewer")) app.showAbout = true;
        ImGui::EndMenu();
    }
    drawTitleBarExtras();
    ImGui::EndMainMenuBar();
}

// File > Open Remote: the one dialog the OS cannot provide, because the files it
// lists live on another machine. URL + peer path, both remembered in prefs.
static bool isNpyName(const std::string& n) {   // FRAME_001.NPY is still a .npy
    if (n.size() < 4) return false;
    std::string ext = n.substr(n.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".npy";
}

// File > Start Remote: ask for a host, not a path. Connecting first is what
// removes the guesswork - once the session is up, the Files panel browses the
// server and nobody has to know whether their data is under ~ or /data.
static void drawRemoteOpenModal() {
    if (app.remoteDlgOpen && !ImGui::IsPopupOpen("Start remote (ssh)")) {
        ImGui::OpenPopup("Start remote (ssh)");
        app.remoteDlgOpen = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Start remote (ssh)", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    static char hostbuf[512];
    static bool advanced = false;
    static char exebuf[256];
    if (ImGui::IsWindowAppearing()) {
        snprintf(hostbuf, sizeof hostbuf, "%s",
                 app.lastRemoteUrl.empty() ? "user@host" : app.lastRemoteUrl.c_str());
        snprintf(exebuf, sizeof exebuf, "%s", app.remoteExe.c_str());
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::TextDisabled("The window stays here; only the pixels being looked at travel.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    bool go = ImGui::InputText("host", hostbuf, sizeof hostbuf,
                               ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("user@host, or a Host alias from ~/.ssh/config.\n"
                          "A full ssh://host/path or host:~/path works too.\n"
                          "Public-key authentication is required - this pipe has\n"
                          "no way to ask for a password.");
    ImGui::TextDisabled("the peer installs itself into ~/.viewer on first connect");
    if (ImGui::TreeNode("advanced")) {
        advanced = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
        ImGui::InputTextWithHint("peer path", "~/.viewer/viewer-serve (default)",
                                 exebuf, sizeof exebuf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Override where viewer-serve lives on that machine.\n"
                              "Leave empty to use ~/.viewer/viewer-serve.");
        ImGui::TreePop();
    }
    if (!g_bootstrapLog.empty() && g_bootstrapLog.find("VIEWER_SERVE_OK") == std::string::npos) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", g_bootstrapLog.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    if (ImGui::Button("Connect") || go) {
        if (advanced) app.remoteExe = exebuf;
        app.lastRemoteUrl = hostbuf;
        app.prefsDirty = true;
        savePrefs();
        ImGui::CloseCurrentPopup();
        startRemote(hostbuf);         // errors arrive as toasts and in the panel
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The whole failure text, copyable. A remote install that fails deserves more
// than a truncated line: the loader's own words are what identify a glibc
// mismatch, and the user needs to be able to paste them.
static void drawRemoteErrorWindow() {
    if (!app.showRemoteError) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 42, ImGui::GetFontSize() * 18),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Remote connection details", &app.showRemoteError)) {
        const App::RemoteBrowse& B = app.rbrowse;
        std::string all = "host: " + (B.host.empty() ? std::string("(none)") : B.host) + "\n";
        if (!B.err.empty()) all += B.err + "\n";
        if (!g_bootstrapLog.empty()) all += "\ninstall log:\n" + g_bootstrapLog;
        if (ImGui::Button("copy to clipboard")) {
            ImGui::SetClipboardText(all.c_str());
            toast("copied");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or select any part of it below and press %s+C", SC_MOD);
        ImGui::Separator();
        // selectable, because this text exists to be pasted somewhere else
        ImGui::InputTextMultiline("##rberrtext", all.data(), all.size() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::End();
}

// Everything the app has said, as text you can select with the mouse and copy
// with Ctrl+C. ImGui's Text() is painted, not selectable - so a read-only
// InputTextMultiline is what makes an error message quotable.
static void drawMessagesPanel() {
    static std::string flat;          // rebuilt only when the log changes
    static size_t builtFrom = (size_t)-1;
    if (builtFrom != app.msgLog.size()) {
        builtFrom = app.msgLog.size();
        flat.clear();
        for (const auto& m : app.msgLog) {
            flat += m.err ? "[error] " : "        ";
            flat += m.text;
            flat += "\n";
        }
    }
    if (ImGui::Button("copy all")) { ImGui::SetClipboardText(flat.c_str()); toast("copied"); }
    ImGui::SameLine();
    if (ImGui::Button("clear")) { app.msgLog.clear(); builtFrom = (size_t)-1; }
    ImGui::SameLine();
    ImGui::TextDisabled("select any part and press %s+C", SC_MOD);
    ImGui::InputTextMultiline("##msgs", flat.data(), flat.size() + 1,
                              ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
}

static void drawHelpAbout() {
    if (app.showHelp) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Keyboard Shortcuts", &app.showHelp, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::BeginTable("sc", 2)) {
                auto row = [](const char* k, const char* d) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", d);
                };
                row("Right / Left",  "next / previous frame (time axis)");
                row("Down / Up",     "next / previous stack (sequence)");
                row("Ctrl+F / Ctrl+B", "next / previous frame (Emacs style)");
                row("Ctrl+N / Ctrl+P", "next / previous stack");
                row("Ctrl+A / Ctrl+E", "first / last frame");
                row(SC_MOD "+O / O", "open files");
                row(SC_MOD "+S",     "save session (view state + images)");
                row(SC_MOD "+W",     "close current image");
                row("F / double-click", "fit to window");
                row("1",             "actual size (100%)");
                row("+ / -",         "zoom in / out");
                row("drag",          "new ROI (inside a ROI = move, on a corner = resize)");
                row("P",             "drop a pin at the cursor");
                row("right-click",   "actions here (pin / band / crop / delete)");
                row("Ctrl+wheel",    "zoom at cursor (invertible in View menu)");
                row("wheel / Shift+wheel", "pan vertical / horizontal");
                row("middle-drag / Space+drag", "pan");
                row("Shift+drag",    "the other one of pan / new ROI");
                row("X / Y",         "select the row / column: through the selected pin, the "
                                     "cursor, or widen the selected ROI (press again to restore)");
                row("Del / Esc",     "delete / deselect annotation");
                row("\\ or C",       "A/B compare: off -> wipe -> side by side");
                row("Shift+\\ or Shift+C", "swap A and B (also the status bar button)");
                row("B (hold)",      "show B full-frame while held");
                row("Shift+B",       "pin this frame as B (then move A: frame vs frame)");
                row("[ / ]",         "move the divider 1% (Shift: 10%)");
                row("G",             "pixel grid (zoom >= 8x)");
                row("M",             "measure again (rerun the selected analyzer, focus Analysis)");
                row("H",             "this help");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("--- Browse panel (when it has focus) ---");
                row("up / down",     "walk the listing, previewing as it goes");
                row("Home / End",    "first / last row");
                row("Enter",         "open for real (the double-click: a stack opens whole)");
                row("right / left",  "tree mode: expand / collapse the folder under the cursor");
                row("Backspace",     "up to the parent folder");
                row(SC_MOD "+F",     "focus the filter box");
                row(", / .",         "step the previewed sequence");
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    if (app.showAbout) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("About viewer", &app.showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("viewer v0.1");
            ImGui::TextDisabled("cross-platform image viewer for engineering data");
            ImGui::Separator();
            ImGui::TextDisabled("Dear ImGui %s  |  GLFW %s", IMGUI_VERSION, glfwGetVersionString());
            ImGui::TextDisabled("npy / bin / raw loaders, pixel inspection, coordinate rulers");
        }
        ImGui::End();
    }
}

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
static std::string g_abstatsSelftest;   // --abstats-selftest <dir>: A/B stats caches, exit
static std::string g_seriesSelftest;    // --series-selftest <dir>: series invariants, exit

// --browse-keys-selftest <dir>: the Browse panel's KEYBOARD, driven through real
// frames. The other browse selftests call the panel's helpers directly, which
// is enough for what the rows contain but blind to everything that only exists
// inside a frame - and a single Down arrow segfaulted the process on an ImGui
// clipper call that a frameless test can never reach. So this one connects a
// LOCAL peer to <dir> in a hidden window and replays real UI actions into the
// real input queue, one per script slot: the panel cannot tell them from a
// human. Actions (--browse-keys overrides the canned list): focus, down, up,
// left, right, enter, home, end, back, flat, tree.
static int g_abRangeDefault = -1;      // App's compareRangeMode before loadPrefs
static std::string g_browseKeys;        // <dir>, empty = not running
static std::string g_browseKeysActs =
    "down,down,down,enter,flat,down,down,down,flat,down,tree,down,right,down,"
    "left,end,home,up,down,more,down,back";

static void printUsage() {
    printf(
        "usage: viewer [options] [files or folders...]\n"
        "  files:  .npy, .npz, .vsession (saved session), or raw binaries (.bin/.raw/...)\n"
        "  folder: loads every numbered sequence below it, one stack per group\n"
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
        "  --sequence <mode>           numbered siblings: ask (default) | always | never\n"
        "  --npy-axis <auto|frames>    (N,H,W) with N<=4: channels (auto) or frames\n"
        "  ssh://user@host/path.npy    view a file on another machine: the UI stays\n"
        "                              here, only the visible region is fetched\n"
        "  ssh://user@host/~/dir       connect and browse there instead (a host on\n"
        "                              its own works too) - what a desktop shortcut\n"
        "                              made by tools/install_shortcut.* passes\n"
        "  --remote-exe <path>         how to start the peer there (default: viewer)\n"
        "  --serve                     BE that peer: answer requests on stdin/stdout\n"
        "  --compare <off|wipe|split|diff>  A/B compare the first two images given\n"
        "  --bench <frames>            render N frames offscreen, print frame-time stats, exit\n"
        "  --bench-step                ...and step A one frame per bench frame (A/B\n"
        "                              follow-frame cost: both sides recompute)\n"
        "  --no-ab-throttle            do NOT hold the B statistics while stepping\n"
        "  --lin-selftest              load, fit linearity over the stacks, print, exit\n"
        "  --frame <system|integrated> title bar: the desktop's, or the one drawn\n"
        "                              in the menu bar (default: last used)\n"
        "  --abstats-selftest <dir>    A/B statistics caches: two slots, print, exit\n"
        "  --series-selftest <dir>     series (系列) model + invariants, print, exit\n"
        "  --zoom <z>                  initial zoom (1 = 100%%)\n"
        "  --center <x,y>              initial view center in image pixels\n"
        "  -h, --help                  show this help\n");
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
        } else if (a == "--quad-bayer") {
            cliQuad = true;                        // applied at load; order-independent
            rawReady = true;
        } else if (a == "--npy-axis") {
            std::string v = next();
            if (v == "frames") app.npyAxis = 1;
            else if (v == "auto" || v == "channels") app.npyAxis = 0;
            else fprintf(stderr, "--npy-axis expects auto|frames\n");
        } else if (a == "--sequence") {            // ask | always | never
            std::string v = next();
            if (v == "always") app.seqLoadMode = 1;
            else if (v == "never") app.seqLoadMode = 2;
            else if (v == "ask") app.seqLoadMode = 0;
            else fprintf(stderr, "--sequence expects ask|always|never\n");
        } else if (a == "--bench" || a == "--crash-test" || a == "--frame") {
            next();                                // consumed in main(), not an error
        } else if (a == "--bench-step") {
            /* consumed in main(): no value */
        } else if (a == "--no-ab-throttle") {
            g_abNoThrottle = true;     // measure what the B-slot throttle saves
        } else if (a == "--cfa") {                 // none | bayer | quad
            std::string v = next();
            app.forceCfa = v == "bayer" ? 1 : v == "quad" || v == "quad-bayer" ? 2
                         : v == "none" ? 0 : -1;
            if (app.forceCfa < 0) fprintf(stderr, "--cfa expects none|bayer|quad\n");
            app.forceCfaPattern = d.cfaPattern;    // --bayer-pattern, if it came first
        } else if (a == "--lin-selftest") {
            g_linSelftest = true;                  // handled in main() after loading
        } else if (a == "--range-selftest") {
            g_rangeSelftest = next();
        } else if (a == "--framestats-selftest") {
            g_fstatSelftest = true;                // handled in main() after loading
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
        } else if (a == "--abstats-selftest") {
            g_abstatsSelftest = next();            // handled in main()
        } else if (a == "--series-selftest") {
            g_seriesSelftest = next();             // handled in main()
        } else if (a == "--remote-selftest") {
            next();
        } else if (a == "--remote-policy") {        // auto | server | local-fetch
            std::string v = next();
            if (v == "auto") app.procPolicy = App::PolAuto;
            else if (v == "server") app.procPolicy = App::PolServer;
            else if (v == "local-fetch" || v == "local") app.procPolicy = App::PolLocalFetch;
            else fprintf(stderr, "--remote-policy expects auto|server|local-fetch\n");
        } else if (a == "--remote-exe") {          // how to invoke the peer over ssh
            app.remoteExe = next();
        } else if (a == "--mem-budget") {          // GB the sequence loader may use
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
                           (low.size() > 9 && low.compare(low.size() - 9, 9, ".vsession") == 0);
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
    // Applied later, not here: with --sequence always the sibling frames are still
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
            // 24 in rb/ itself + 3 x 8 under scanroot/
            bool ok = hits.size() == 48 && !trunc;
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
    if (g_lastInputAt == 0) g_lastInputAt = glfwGetTime();   // first of a burst
    if (!app.lowBandwidth) g_wakeUntil = glfwGetTime() + 0.25;
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
    // --bench N: render N frames in a hidden window and report frame times, so
    // performance can be measured (and regressions caught) instead of guessed.
    int benchFrames = 0, crashAfter = 0, cliFrame = -1;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--bench")) benchFrames = std::max(1, atoi(argv[i + 1]));
        // developer flag: verify the crash safety net actually writes a session
        if (!strcmp(argv[i], "--crash-test")) crashAfter = std::max(1, atoi(argv[i + 1]));
        // the window frame has to be decided before the window exists
        if (!strcmp(argv[i], "--frame")) cliFrame = !strcmp(argv[i + 1], "system") ? 0 : 1;
    }
    // --bench-step: advance A by one frame before every benched frame. --bench
    // alone holds one image still, so every cache key stays hit and the loop
    // measures drawing only. The A/B question is the opposite one - what a
    // FRAME STEP costs when compareFollowFrame moves B too and both sides'
    // histogram / projection caches miss - and it cannot be measured without
    // actually stepping.
    bool benchStep = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-step")) benchStep = true;
    app.exePath = argv[0];
    // the out-of-the-box A/B range mode, read before anything can override it:
    // --range-selftest checks it, and prefs on the machine running the test
    // would otherwise decide what "the default" is
    g_abRangeDefault = app.compareRangeMode;
    loadPrefs();       // before the theme is applied and before the CLI is parsed
    if (cliFrame >= 0) app.frameMode = cliFrame;   // for this run only: not saved
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    const char* glslVersion = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glslVersion = "#version 130";
#endif
    if (benchFrames || !g_browseKeys.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    // How a Linux desktop matches a window to its launcher: without these the
    // WM_CLASS is GLFW's own "GLFW-Application", the .desktop file written by
    // tools/install_shortcut.sh cannot claim the window, and GNOME/KDE show a
    // generic icon in a second, ungrouped dock entry. Ignored elsewhere.
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "viewer");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "viewer");
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "viewer");
    GLFWwindow* win = glfwCreateWindow(1600, 1000, "viewer v0.1", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window creation failed\n"); return 1; }
    applyWindowIcon(win, false);
    // The frame comes up before the first frame is drawn, so the window never
    // flashes a system title bar it is about to lose. --frame on the command
    // line wins over the preference for this run and is not written back: it is
    // the way out if a window manager makes a mess of the integrated one.
    window_frame::init(win);
    window_frame::setMode(app.frameMode ? window_frame::Integrated : window_frame::System);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(benchFrames ? 0 : 1);   // benchmark must not be vsync-limited
    installWakeCallbacks(win);        // before ImGui's backend: it chains to these
    glfwSetDropCallback(win, dropCallback);

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
    glfwGetWindowContentScale(win, &xs, &ys);
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

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

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

    // Remote Open Folder, verifiable without a human or a second machine: connect
    // to the local peer, scan a folder-of-folders, and count what opened. Goes
    // through the REAL path - rb worker, LIST protocol, grouping, stack open.
    // The picker's two new promises, verifiable without a human. UC3: a live
    // filter cuts each group's FILE list, and accepting loads exactly the cut.
    // UC2: merge mode folds the whole selection into ONE stack, union of files
    // in natural order. Runs against an abset-style folder (A/B variants of
    // the same numbering in one directory).
    if (!g_pickerSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto stackFiles = [&](int seqId) {
            std::vector<std::string> out;
            for (int idx : framesOfSeq(seqId)) out.push_back(app.images[idx]->path);
            return out;
        };
        auto joinBase = [&](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& f : v) { s += s.empty() ? "" : ","; s += baseName(f); }
            return s;
        };
        bool ok = true;

        // ---- UC3: filter "_A", one stack per group -> only the A files load
        openFolder(g_pickerSelftest);
        if (!app.folderPickOpen) {
            fprintf(stderr, "pickerselftest: picker did not open\n");
            return 1;
        }
        int total = 0;
        for (const auto& e : app.folderPick) total += (int)e.g.files.size();
        snprintf(app.pickFilter, sizeof app.pickFilter, "_A");
        applyPickFilter();
        app.pickMerge = 0;
        std::vector<App::PendingGroup> want = pickerSelection();
        std::vector<std::string> wantFiles;
        for (const auto& g : want)
            wantFiles.insert(wantFiles.end(), g.files.begin(), g.files.end());
        pickerAccept();
        loadAll();
        std::vector<std::string> got =
            app.seqs.empty() ? std::vector<std::string>() : stackFiles(app.seqs[0].id);
        fprintf(stderr, "pickerselftest: UC3 filter \"_A\" kept %d of %d files -> "
                        "%d stack(s), loaded [%s]\n",
                (int)wantFiles.size(), total, (int)app.seqs.size(), joinBase(got).c_str());
        if ((int)wantFiles.size() >= total || wantFiles.empty()) {
            fprintf(stderr, "pickerselftest: UC3 FAILED - filter cut nothing\n");
            ok = false;
        }
        if (app.seqs.size() != want.size() || got != wantFiles) {
            fprintf(stderr, "pickerselftest: UC3 FAILED - loaded files != filtered selection\n");
            ok = false;
        }

        // ---- UC2: no filter, merge mode -> ONE stack, union, natural order
        size_t seqsBefore = app.seqs.size();
        openFolder(g_pickerSelftest);          // reopens the picker, filter cleared
        app.pickMerge = 1;
        std::vector<App::PendingGroup> mwant = pickerSelection();
        pickerAccept();
        loadAll();
        std::vector<std::string> mgot = app.seqs.size() == seqsBefore + 1
            ? stackFiles(app.seqs.back().id) : std::vector<std::string>();
        fprintf(stderr, "pickerselftest: UC2 merge -> +%d stack(s) '%s', order [%s]\n",
                (int)(app.seqs.size() - seqsBefore),
                app.seqs.empty() ? "" : app.seqs.back().name.c_str(), joinBase(mgot).c_str());
        if (mwant.size() != 1 || app.seqs.size() != seqsBefore + 1) {
            fprintf(stderr, "pickerselftest: UC2 FAILED - merge did not make ONE stack\n");
            ok = false;
        } else if (mgot != mwant[0].files || (int)mgot.size() != total) {
            fprintf(stderr, "pickerselftest: UC2 FAILED - merged stack != union in natural order\n");
            ok = false;
        }
        // ---- UC5: batch mode "one per top folder" -> N batches, unique names
        {
            std::string bsdir;
            {   // fixture: batchset/ next to the given folder, else testdata
                std::string d2 = g_pickerSelftest;
                std::replace(d2.begin(), d2.end(), '\\', '/');
                size_t sl = d2.find_last_of('/');
                bsdir = (sl == std::string::npos ? std::string(".") : d2.substr(0, sl))
                      + "/batchset";
                std::error_code ec;
                if (!std::filesystem::is_directory(pathFromUtf8(bsdir), ec))
                    bsdir = "tools/testdata/batchset";
            }
            size_t imagesBefore = app.images.size();
            openFolder(bsdir);
            if (!app.folderPickOpen) {
                fprintf(stderr, "pickerselftest: UC5 FAILED - picker did not open\n");
                ok = false;
            } else {
                app.pickBatchMode = 1;             // the new footer radio
                pickerAccept();
                loadAll();
                std::vector<int> bids;             // distinct batches actually used
                for (size_t i = imagesBefore; i < app.images.size(); i++) {
                    int b = app.images[i]->batchId;
                    if (std::find(bids.begin(), bids.end(), b) == bids.end())
                        bids.push_back(b);
                }
                bool stacksOneBatch = true;        // a stack never straddles batches
                for (const auto& si : app.seqs) {
                    int b0 = -1;
                    for (size_t i = imagesBefore; i < app.images.size(); i++) {
                        if (app.images[i]->seqId != si.id) continue;
                        if (b0 < 0) b0 = app.images[i]->batchId;
                        else if (app.images[i]->batchId != b0) stacksOneBatch = false;
                    }
                }
                bool namesUnique = true;           // sessions restore by NAME
                for (size_t a2 = 0; a2 < app.batches.size(); a2++)
                    for (size_t b2 = a2 + 1; b2 < app.batches.size(); b2++)
                        if (app.batches[a2].name == app.batches[b2].name)
                            namesUnique = false;
                std::string names;
                for (int b : bids)
                    for (const auto& bb : app.batches)
                        if (bb.id == b) names += (names.empty() ? "" : ",") + bb.name;
                fprintf(stderr, "pickerselftest: UC5 batch-per-top-folder -> %d batch(es) "
                                "[%s], stacks one-batch=%d, names unique=%d\n",
                        (int)bids.size(), names.c_str(), stacksOneBatch ? 1 : 0,
                        namesUnique ? 1 : 0);
                if (bids.size() != 3 || !stacksOneBatch || !namesUnique) {
                    fprintf(stderr, "pickerselftest: UC5 FAILED - batches wrong\n");
                    ok = false;
                }
            }
        }
        fprintf(stderr, "pickerselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        return ok ? 0 : 1;
    }

    // The local-browse entry, verifiable without a human: the function behind
    // File > Browse Folder (Local)... must land the picked folder in the Browse
    // panel through the LOCAL peer - connected, empty host, entries listed.
    if (!g_localbrowseSelftest.empty()) {
        browseLocalFolder(g_localbrowseSelftest);   // exactly what the menu does
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (app.rbrowse.connected && !app.rbrowse.entries.empty()) break;
            if (!app.rbrowse.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = app.rbrowse;
        bool ok = B.connected && B.host.empty() && !B.entries.empty() &&
                  app.showRemote;
        fprintf(stderr, "localbrowseselftest: connected=%d host='%s' dir=%s "
                        "entries=%d showBrowse=%d%s%s\n",
                B.connected ? 1 : 0, B.host.c_str(), B.dir.c_str(),
                (int)B.entries.size(), app.showRemote ? 1 : 0,
                B.err.empty() ? "" : " err=", B.err.c_str());
        fprintf(stderr, "localbrowseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }

    // The Browse panel's own behaviour, verifiable without a human. Connects a
    // LOCAL peer to <dir> and drives the same functions the panel draws with.
    if (!g_browseSelftest.empty()) {
        std::string dir = g_browseSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        startRemote("local://" + dir);
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (app.rbrowse.connected && !app.rbrowse.entries.empty()) break;
            if (!app.rbrowse.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = app.rbrowse;
        if (!B.connected || B.entries.empty()) {
            fprintf(stderr, "browseselftest: no listing for %s (%s)\n",
                    dir.c_str(), B.err.c_str());
            stopRbWorker(); stopRemoteFetcher(); stopMeasureWorker();
            return 1;
        }
        bool ok = true;
        {   // Grouped <-> flat is a CLIENT-SIDE view over one reply: the flat
            // listing must be exactly the group's members, in the peer's order,
            // and the grouped one exactly one row for them.
            const remote::Entry* g = nullptr;
            int nGroups = 0, nMembers = 0;
            for (const auto& e : B.entries) {
                if (!e.group) continue;
                nGroups++;
                nMembers += (int)e.members.size();
                if (!g) g = &e;
            }
            std::vector<RbRow> gv = rbBuildView(&B.dir, B.entries, false, false);
            std::vector<RbRow> fv = rbBuildView(&B.dir, B.entries, true, false);
            bool sizes = gv.size() == B.entries.size() &&
                         fv.size() == B.entries.size() - nGroups + nMembers;
            // one grouped row for the sequence, and it is the group entry
            int gRows = 0;
            for (const auto& r : gv) if (r.isGroup()) gRows++;
            // the expanded rows, in order, ARE the members
            std::vector<std::string> expanded;
            bool memberCells = true;
            for (const auto& r : fv) {
                if (r.e != g || r.member < 0) continue;
                expanded.push_back(r.name());
                // an expanded frame has no size / mtime of its own, but keeps
                // the shape/dtype the group shares by construction
                if (r.ownFile() || r.isGroup() || r.isDir()) memberCells = false;
                if (r.e->hasMeta != g->hasMeta || r.e->dtype != g->dtype) memberCells = false;
            }
            bool same = g && expanded == g->members;
            bool nogroups = true;
            for (const auto& r : fv) if (r.isGroup()) nogroups = false;
            fprintf(stderr, "browseselftest: view %s: grouped %d row(s) [%d sequence row(s), "
                            "'%s' x%d], flat %d row(s), members match=%d, no group rows "
                            "when flat=%d, member size/mtime blank=%d: %s\n",
                    dir.c_str(), (int)gv.size(), gRows, g ? g->name.c_str() : "?",
                    g ? (int)g->members.size() : 0, (int)fv.size(), same ? 1 : 0,
                    nogroups ? 1 : 0, memberCells ? 1 : 0,
                    (sizes && gRows == nGroups && nGroups >= 1 && same && nogroups &&
                     memberCells) ? "ok" : "FAIL");
            if (!(sizes && gRows == nGroups && nGroups >= 1 && same && nogroups && memberCells))
                ok = false;
        }
        {   // The deferred-action invariant. Every row the listing draws is a
            // pair of raw pointers into B.entries; the Places combo used to
            // replace the whole browse state from inside the draw, and the
            // table below it then read nine destroyed entries (SIGSEGV in
            // strlen). Nothing that replaces that state may run while a row
            // list is alive - it is queued and runs when the rows are gone.
            int ranAt = -1, marker = 0;
            bool queued = false, aliveOk = false;
            size_t pend0 = rbDeferredPending();
            {
                RbDeferredActions guard;
                std::vector<RbRow> rows = rbBuildView(&B.dir, B.entries, false, false);
                rbDefer([&] { ranAt = marker; });     // what picking a place queues
                marker = 1;
                queued = rbDeferredPending() == pend0 + 1 && ranAt == -1;
                // ...and the rows are still readable, which is the whole point
                aliveOk = !rows.empty() && rows[0].e == &B.entries[0] &&
                          rows[0].e->name == B.entries[0].name;
                marker = 2;                            // the row list dies here
            }
            bool defOk = queued && aliveOk && ranAt == 2 && rbDeferredPending() == 0;
            fprintf(stderr, "browseselftest: deferred actions: queued while the rows "
                            "were alive=%d, rows valid throughout=%d, ran only after "
                            "they died=%d, queue drained=%d: %s\n",
                    queued ? 1 : 0, aliveOk ? 1 : 0, ranAt == 2 ? 1 : 0,
                    rbDeferredPending() == 0 ? 1 : 0, defOk ? "ok" : "FAIL");
            if (!defOk) ok = false;
        }
        {   // Tree mode, lazily: expanding a node issues exactly ONE LIST, on
            // the worker; the children appear under the folder at depth+1;
            // collapsing keeps the cache, so re-expanding issues nothing.
            std::string sub;
            for (const auto& e : B.entries)
                if (e.dir && (sub.empty() || e.name == "scanroot")) sub = e.name;
            if (sub.empty()) {
                fprintf(stderr, "browseselftest: tree: no subfolder in %s to expand\n",
                        dir.c_str());
                ok = false;
            } else {
                std::string subPath = dir + "/" + sub;
                int before = app.rbTreeLists;
                // nothing is listed until it is asked for
                bool lazy0 = app.rbTreeCache.empty();
                rbTreeExpand(subPath);
                double t1 = glfwGetTime();
                while (glfwGetTime() - t1 < 120.0) {
                    pumpRemoteBrowse();
                    if (app.rbTreeCache.count(subPath)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                int afterExpand = app.rbTreeLists;
                std::vector<RbRow> tv = rbBuildView(&B.dir, B.entries, false, true);
                // the children sit directly under their folder, one level in
                int at = -1;
                for (int i = 0; i < (int)tv.size(); i++)
                    if (tv[i].isDir() && tv[i].name() == sub) { at = i; break; }
                size_t nKids = app.rbTreeCache.count(subPath)
                             ? app.rbTreeCache[subPath].size() : 0;
                int kidsUnder = 0;
                bool contiguous = at >= 0;
                for (int i = at + 1; i < (int)tv.size() && tv[i].depth > tv[at].depth; i++)
                    kidsUnder++;
                if (at < 0 || (size_t)kidsUnder != nKids) contiguous = false;
                // ...and they know which directory they came from
                bool paths = at >= 0;
                for (int i = at + 1; i <= at + kidsUnder && i < (int)tv.size(); i++)
                    if (tv[i].full() != subPath + "/" + tv[i].name()) paths = false;
                // collapse: rows go, the cache stays
                rbTreeCollapse(subPath);
                std::vector<RbRow> cv = rbBuildView(&B.dir, B.entries, false, true);
                bool collapsed = cv.size() == B.entries.size() &&
                                 app.rbTreeCache.count(subPath) == 1;
                // re-expand: no second round trip
                rbTreeExpand(subPath);
                int afterAgain = app.rbTreeLists;
                std::vector<RbRow> rv = rbBuildView(&B.dir, B.entries, false, true);
                bool freeAgain = afterAgain == afterExpand && rv.size() == tv.size();
                // and a grandchild: the recursion must indent, not flatten
                int deep = 0, atDeep = -1;
                if (at >= 0)
                    for (int i = at + 1; i <= at + kidsUnder && i < (int)tv.size(); i++)
                        if (tv[i].isDir()) { atDeep = i; break; }
                if (atDeep >= 0) {
                    rbTreeExpand(rv[atDeep].full());
                    double t2 = glfwGetTime();
                    while (glfwGetTime() - t2 < 120.0) {
                        pumpRemoteBrowse();
                        if (app.rbTreeCache.count(rv[atDeep].full())) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    std::vector<RbRow> dv = rbBuildView(&B.dir, B.entries, false, true);
                    for (const auto& q : dv) if (q.depth == 2) deep++;
                }
                bool treeOk = lazy0 && afterExpand == before + 1 && nKids > 0 &&
                              contiguous && paths && collapsed && freeAgain &&
                              (atDeep < 0 || deep > 0);
                fprintf(stderr, "browseselftest: tree lazy: nothing cached before=%d, "
                                "expand %s -> %d LIST(s) (%d children at depth %d, paths "
                                "ok=%d), collapse -> %d row(s) with the cache kept=%d, "
                                "re-expand -> %d extra LIST(s), grandchild rows at "
                                "depth 2 = %d: %s\n",
                        lazy0 ? 1 : 0, sub.c_str(), afterExpand - before, kidsUnder,
                        at >= 0 && at + 1 < (int)tv.size() ? tv[at + 1].depth : -1,
                        paths ? 1 : 0, (int)cv.size(), collapsed ? 1 : 0,
                        afterAgain - afterExpand, deep, treeOk ? "ok" : "FAIL");
                if (!treeOk) ok = false;
                rbTreeForget();
            }
        }
        {   // "Open folder" on the folder being browsed: the same call the
            // toolbar button and the breadcrumb menu make. The scan must
            // include the CURRENT directory itself (a stack whose name has no
            // folder prefix), which is exactly what the old row-only entry
            // could not reach without first going up a level.
            remoteScanFolder(B.dir);
            double t1 = glfwGetTime();
            while (glfwGetTime() - t1 < 120.0) {
                pumpRemoteBrowse();
                if (app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int here = 0, below = 0;
            std::string names;
            for (const auto& fp : app.folderPick) {
                if (fp.g.name.find('/') == std::string::npos) here++;
                else below++;
                if (names.size() < 160)
                    names += (names.empty() ? "" : ",") + fp.g.name;
            }
            bool scanOk = app.folderPickOpen && app.folderPickRemote && here >= 1 && below >= 1;
            fprintf(stderr, "browseselftest: open-folder on the current dir %s -> picker "
                            "open=%d remote=%d, %d stack(s) in this folder, %d below "
                            "[%s]: %s\n",
                    B.dir.c_str(), app.folderPickOpen ? 1 : 0, app.folderPickRemote ? 1 : 0,
                    here, below, names.c_str(), scanOk ? "ok" : "FAIL");
            if (!scanOk) ok = false;
            app.folderPick.clear();          // Cancel: nothing must be opened
            app.folderPickOpen = false;
        }
        fprintf(stderr, "browseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }

    // Browser-fired temporal, verifiable without a human (docs/terminology.md):
    // the server aggregate for a stack NOBODY OPENED must land in the Temporal
    // panel as a detached [not opened] result and match an independent local
    // computation to full float64 precision.
    if (!g_rtemporalSelftest.empty()) {
        std::string dir = g_rtemporalSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        std::vector<std::string> files;
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), ec)) {
                std::string n = e.path().filename().u8string();
                if (isNpyName(n)) files.push_back(dir + "/" + n);
            }
        }
        sortFramesNumerically(files);
        if (files.size() < 2) {
            fprintf(stderr, "rtemporalselftest: need >= 2 npy under %s\n", dir.c_str());
            return 1;
        }
        // independent reference: per-pixel f64 accumulation over the local files
        double refMean = 0, refSt = 0, refFpn = 0;
        {
            std::vector<double> sum, sum2;
            size_t samples = 0;
            int N = 0;
            for (const auto& f : files) {
                std::string err;
                std::unique_ptr<ImageDoc> d = decodeNpy(f, err);
                if (!d) {
                    fprintf(stderr, "rtemporalselftest: %s: %s\n", f.c_str(), err.c_str());
                    return 1;
                }
                if (!samples) {
                    samples = d->data.size();
                    sum.assign(samples, 0.0);
                    sum2.assign(samples, 0.0);
                }
                if (d->data.size() != samples) {
                    fprintf(stderr, "rtemporalselftest: shape mismatch in fixture\n");
                    return 1;
                }
                for (size_t i = 0; i < samples; i++) {
                    double v = d->data[i];
                    sum[i] += v; sum2[i] += v * v;
                }
                N++;
            }
            double aM = 0, aM2 = 0, aV = 0;
            for (size_t i = 0; i < samples; i++) {
                double mm = sum[i] / N;
                aM += mm; aM2 += mm * mm;
                aV += std::max(0.0, sum2[i] / N - mm * mm) * (N / (N - 1.0));
            }
            refMean = aM / samples;
            refSt = sqrt(aV / samples);
            refFpn = sqrt(std::max(0.0, aM2 / samples - refMean * refMean));
        }
        // connect the browser, then fire the SAME call the group row's menu makes
        startRemote("local://" + dir);
        double t0 = glfwGetTime();
        bool fired = false, ok = true;
        while (glfwGetTime() - t0 < 120.0) {
            pumpRemoteBrowse();
            pumpMeasure();
            if (app.rbrowse.connected && !fired) {
                std::string leaf = dir.substr(dir.find_last_of('/') + 1);
                requestBrowseTemporal(app.rbrowse.host, files, leaf + "/frame_???.npy");
                fired = true;
            }
            if (fired && !app.srvTemporal.pending) break;
            if (!app.rbrowse.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        App::ServerTemporal& S = app.srvTemporal;
        auto rel = [](double a, double b) {
            return fabs(a - b) / std::max({ fabs(a), fabs(b), 1e-12 });
        };
        fprintf(stderr, "rtemporalselftest: [server %s, %d frames - not opened: %s] "
                        "valid=%d seqId=%d, %d image(s) open\n",
                S.host.empty() ? "local peer" : S.host.c_str(), S.frames,
                S.label.c_str(), S.valid ? 1 : 0, S.seqId, (int)app.images.size());
        if (!fired || !S.valid || S.seqId != -2 || S.frames != (int)files.size() ||
            !app.images.empty()) {
            fprintf(stderr, "rtemporalselftest: FAILED - no valid detached result (%s)\n",
                    S.err.c_str());
            ok = false;
        } else {
            fprintf(stderr, "rtemporalselftest: sigma_t server %.12g vs ref %.12g "
                            "(rel %.3g), sigma_fpn %.12g vs %.12g (rel %.3g), "
                            "mean rel %.3g\n",
                    S.tempNoise, refSt, rel(S.tempNoise, refSt),
                    S.fixedPattern, refFpn, rel(S.fixedPattern, refFpn),
                    rel(S.mean, refMean));
            if (rel(S.tempNoise, refSt) > 1e-9 || rel(S.fixedPattern, refFpn) > 1e-9 ||
                rel(S.mean, refMean) > 1e-9) {
                fprintf(stderr, "rtemporalselftest: FAILED - mismatch vs reference\n");
                ok = false;
            }
        }
        fprintf(stderr, "rtemporalselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopMeasureWorker();
        stopRemoteFetcher();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }

    // Move-to-batch, verifiable without a human (docs/terminology.md): stacks
    // move between batches WHOLE, emptied batches are pruned, and the batch
    // name survives a session save/load round-trip (imgbatch travels by name).
    if (!g_batchSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        openFolder(g_batchSelftest);
        if (!app.folderPickOpen) {
            fprintf(stderr, "batchselftest: picker did not open\n");
            return 1;
        }
        pickerAccept();
        loadAll();
        if (app.seqs.empty() || app.images.empty()) {
            fprintf(stderr, "batchselftest: nothing loaded\n");
            return 1;
        }
        int oldBatch = app.images[0]->batchId;
        int nb = newBatch(uniqueBatchName("moved"));
        std::vector<int> sids;
        for (const auto& si : app.seqs) sids.push_back(si.id);
        for (int s : sids) moveStackToBatch(s, nb);
        int offBatch = 0;
        for (const auto& d : app.images) if (d->batchId != nb) offBatch++;
        bool oldGone = true;
        for (const auto& b : app.batches) if (b.id == oldBatch) oldGone = false;
        fprintf(stderr, "batchselftest: moved %d stack(s) to batch '%s': "
                        "%d of %d frames off-batch, old batch %d pruned=%d\n",
                (int)sids.size(), "moved", offBatch, (int)app.images.size(),
                oldBatch, oldGone ? 1 : 0);
        if (offBatch != 0 || !oldGone) {
            fprintf(stderr, "batchselftest: FAILED - move left strays\n");
            ok = false;
        }
        // ---- session round-trip: the batch travels BY NAME ----
        std::error_code tec;
        std::string sess = (std::filesystem::temp_directory_path(tec) /
                            "viewer_batchselftest.vsession").u8string();
        saveSession(sess, true);
        std::string lerr = loadSession(sess);      // closeAll happens inside
        if (!lerr.empty()) {
            fprintf(stderr, "batchselftest: FAILED - session reload: %s\n", lerr.c_str());
            ok = false;
        }
        loadAll();                                 // seqload rescans the folders
        int movedId = 0;
        for (const auto& b : app.batches) if (b.name == "moved") movedId = b.id;
        int strays = 0;
        for (const auto& d : app.images) if (d->batchId != movedId) strays++;
        fprintf(stderr, "batchselftest: reloaded session: %d image(s), %d stack(s), "
                        "batch 'moved' %s, %d stray frame(s)\n",
                (int)app.images.size(), (int)app.seqs.size(),
                movedId ? "restored" : "MISSING", strays);
        if (!movedId || strays != 0 || app.images.empty() ||
            app.seqs.size() != sids.size()) {
            fprintf(stderr, "batchselftest: FAILED - session did not restore the batch\n");
            ok = false;
        }
        fprintf(stderr, "batchselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        std::filesystem::remove(std::filesystem::u8path(sess), tec);
        return ok ? 0 : 1;
    }

    // The series (系列) layer - the MODEL, with no UI behind it (phase 1 draws
    // nothing at all). Every invariant docs/terminology.md and docs/series-plan.md
    // state, checked exhaustively (seriesAudit) after every mutation, on the real
    // load path: Open Folder -> picker -> stacks.
    if (!g_seriesSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 600.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](const char* what, bool cond) {
            fprintf(stderr, "seriesselftest: %-62s %s\n", what, cond ? "ok" : "FAILED");
            if (!cond) ok = false;
        };
        auto audit = [&]() {
            std::string why;
            if (seriesAudit(why)) return true;
            fprintf(stderr, "seriesselftest: AUDIT: %s\n", why.c_str());
            return false;
        };
        openFolder(g_seriesSelftest);
        loadAll();
        std::vector<int> sids;
        for (const auto& si : app.seqs) sids.push_back(si.id);
        if (sids.size() < 4 || app.images.empty()) {
            fprintf(stderr, "seriesselftest: need >= 4 stacks under %s, got %d\n",
                    g_seriesSelftest.c_str(), (int)sids.size());
            return 1;
        }
        int b0 = app.images[0]->batchId;

        // ---- 1. build one series out of every stack of one batch --------------
        int S1 = newSeries(b0, "");                 // "" = the canon's default name
        {
            App::Series* S = seriesById(S1);
            S->paramName = "illuminance";
            snprintf(S->unit, sizeof S->unit, "lx");
        }
        int added = 0;
        for (int s : sids)
            if (addToSeries(S1, s, extractLevelFromName(seqInfo(s)->name))) added++;
        App::Series* S = seriesById(S1);
        int valued = 0, batchOk = 0;
        for (const auto& m : S->members) {
            if (std::isfinite(m.value)) valued++;
            for (const auto& d : app.images)
                if (d->seqId == m.seqId && d->batchId == S->batchId) { batchOk++; break; }
        }
        fprintf(stderr, "seriesselftest: created series '%s' in batch '%s': %d member(s), "
                        "%d valued, param '%s' unit '%s' kind %d\n",
                S->name.c_str(), batchNameOf(b0).c_str(), (int)S->members.size(), valued,
                S->paramName.c_str(), S->unit, S->kind);
        check("every stack of the batch joined", added == (int)sids.size() &&
                                                 S->members.size() == sids.size());
        check("every member's value came from its name", valued == (int)S->members.size());
        check("every member's frames are in the series' batch", batchOk == (int)S->members.size());
        check("default name is \"<batch> 掃引\"", S->name == batchNameOf(b0) + " 掃引");
        check("invariant 1: audit after create", audit());
        check("N members + a unit -> seriesCanFit", seriesCanFit(*S));
        {   // the unit is never assumed: without one there is no fit, ever
            char keep[16];
            snprintf(keep, sizeof keep, "%s", S->unit);
            S->unit[0] = '\0';
            check("unit unset -> no fit (never assumed)", !seriesCanFit(*S));
            snprintf(S->unit, sizeof S->unit, "%s", keep);
        }

        // ---- persistence: save -> load -> lazy resolve ------------------------
        std::error_code tec;
        std::string sess = (std::filesystem::temp_directory_path(tec) /
                            "viewer_seriesselftest.vsession").u8string();
        std::string legacy = (std::filesystem::temp_directory_path(tec) /
                              "viewer_serieslegacy.vsession").u8string();
        int legacyCount = 0;
        {   // an OLD-format session, written now while the paths are known. It
            // has to be built by hand because saveSession has NEVER emitted
            // "seqlevel" - which is the honest reason the migration below is
            // nearly always a no-op.
            std::ofstream lf(pathFromUtf8(legacy), std::ios::binary);
            lf << "viewer-session 1\n" << std::setprecision(9);
            for (const auto& m : S->members) {
                std::vector<int> fr = framesOfSeq(m.seqId);
                if (fr.empty() || !std::isfinite(m.value)) continue;
                lf << "image 0 1 npy3 0 0 0 0 0 0 0 " << app.images[fr.front()]->path << "\n";
                lf << "imgbatch legacyset\n";
                lf << "seqload 1\n";
                lf << "seqlevel " << m.value << "\n";
                legacyCount++;
            }
        }
        const char* RENAMED = "renamed member stack";
        {
            // Doctor the series first, so the round trip has something to lose:
            // a renamed stack, an UNSET value, an excluded member, and a value
            // with more significant digits than the file's default precision -
            // the axis of a fit is the last place to accept a rounded number.
            seqInfo(S->members[1].seqId)->name = RENAMED;
            S->members[2].value = std::numeric_limits<double>::quiet_NaN();
            S->members[3].include = false;
            S->members[4].value = 1234567.891234;
            struct Want { std::string path; double value; bool include; };
            std::vector<Want> want;
            for (const auto& m : S->members) {
                std::vector<int> fr = framesOfSeq(m.seqId);
                want.push_back({ fr.empty() ? std::string() : app.images[fr.front()]->path,
                                 m.value, m.include });
            }
            std::string wantName = S->name, wantParam = S->paramName, wantUnit = S->unit;
            std::string wantBatch = batchNameOf(S->batchId);
            int wantKind = S->kind;
            saveSession(sess, true);
            std::string lerr = loadSession(sess);       // closeAll happens inside
            check("session reloaded", lerr.empty());
            loadAll();                                  // stacks come back first
            double tr = glfwGetTime();                  // ...the series after them
            while (glfwGetTime() - tr < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            App::Series* R = app.series.empty() ? nullptr : &app.series.front();
            fprintf(stderr, "seriesselftest: round trip: %d series; '%s' param '%s' "
                            "unit '%s' kind %d, %d member(s), batch '%s'\n",
                    (int)app.series.size(), R ? R->name.c_str() : "",
                    R ? R->paramName.c_str() : "", R ? R->unit : "", R ? R->kind : -1,
                    R ? (int)R->members.size() : -1,
                    R ? batchNameOf(R->batchId).c_str() : "");
            check("exactly one series came back", app.series.size() == 1);
            check("name / parameter / unit / kind survive",
                  R && R->name == wantName && R->paramName == wantParam &&
                  wantUnit == R->unit && R->kind == wantKind);
            check("the series is in its batch, restored BY NAME",
                  R && batchNameOf(R->batchId) == wantBatch);
            bool memOk = R && R->members.size() == want.size();
            if (memOk)
                for (size_t i = 0; i < want.size(); i++) {
                    std::vector<int> fr = framesOfSeq(R->members[i].seqId);
                    std::string p = fr.empty() ? std::string() : app.images[fr.front()]->path;
                    double a = want[i].value, b = R->members[i].value;
                    bool vOk = std::isfinite(a) == std::isfinite(b) &&
                               (!std::isfinite(a) ||
                                fabs(a - b) <= 1e-9 * std::max(1.0, fabs(a)));
                    if (p != want[i].path || !vOk ||
                        R->members[i].include != want[i].include) memOk = false;
                }
            check("members: order, first-frame path, value and include all survive", memOk);
            check("a value with more digits than the file's precision survives EXACTLY",
                  R && R->members.size() > 4 && R->members[4].value == 1234567.891234);
            check("the unset member came back UNSET, not 0",
                  R && R->members.size() > 2 && !std::isfinite(R->members[2].value));
            check("the excluded member came back excluded",
                  R && R->members.size() > 3 && !R->members[3].include);
            check("invariant 1: audit after restore", audit());
            // Why members are keyed by PATH and not by stack name: the name of a
            // FOLDER stack does not survive at all. seqname is written, but when
            // it is parsed the stack does not exist yet (the frames come from a
            // queued rescan), so the line lands on nothing.
            bool nameSurvived = false;
            for (const auto& si : app.seqs) if (si.name == RENAMED) nameSurvived = true;
            fprintf(stderr, "seriesselftest: the renamed stack's name after restore: %s\n",
                    nameSurvived ? "SURVIVED" : "lost (folder stack: seqname lands on nothing)");
            check("stack names do NOT survive for folder stacks (hence path keys)",
                  !nameSurvived);
        }
        // everything below works on what came back: the reload minted new ids
        if (app.series.empty() || app.images.empty() || app.series.front().members.size() < 4) {
            fprintf(stderr, "seriesselftest: nothing usable survived the round trip\n");
            return 1;
        }
        S1 = app.series.front().id;
        S = seriesById(S1);
        b0 = S->batchId;
        sids.clear();
        for (const auto& m : S->members) sids.push_back(m.seqId);
        // Undo the exclusion; the value deliberately left UNSET stays unset - it
        // is the point of the next block. (It cannot be re-derived from the name
        // either: a restored folder stack is called "frame_000‥023.npy", with no
        // folder part to read a level out of. That is the same seqname hole,
        // seen from the other side.)
        for (auto& m : S->members) m.include = true;

        // ---- invariant 6: an unset value is UNSET, never 0 --------------------
        int ptsAll = seriesFitPoints(*S);
        {
            double keep = S->members[0].value;
            S->members[0].value = std::numeric_limits<double>::quiet_NaN();
            int nanPts = seriesFitPoints(*S);
            S->members[0].value = keep;
            S->members[0].include = false;
            int excPts = seriesFitPoints(*S);
            S->members[0].include = true;
            fprintf(stderr, "seriesselftest: fit points %d -> %d with one value unset, "
                            "%d with one excluded\n", ptsAll, nanPts, excPts);
            check("invariant 6: an unset value leaves the fit", nanPts == ptsAll - 1);
            check("an excluded member leaves the fit", excPts == ptsAll - 1);
            check("both are reversible", seriesFitPoints(*S) == ptsAll);
        }

        // ---- invariant 2: a stack is in AT MOST ONE series --------------------
        int S2 = newSeries(b0, "second");
        {
            int moved = sids[0];
            bool didMove = addToSeries(S2, moved, 1.0);
            S = seriesById(S1);
            App::Series* T = seriesById(S2);
            fprintf(stderr, "seriesselftest: stack %d added to '%s': '%s' now %d member(s), "
                            "'%s' %d\n", moved, T->name.c_str(), S->name.c_str(),
                    (int)S->members.size(), T->name.c_str(), (int)T->members.size());
            check("invariant 2: adding elsewhere MOVES the stack",
                  didMove && T->members.size() == 1 && S->members.size() == sids.size() - 1 &&
                  seriesOfStack(moved) == T);
            check("invariant 5: a single member cannot be fitted", !seriesCanFit(*T));
            check("invariant 1: audit after the move", audit());
            addToSeries(S1, moved, extractLevelFromName(seqInfo(moved)->name));
            pruneEmptySeries();
            check("emptied series is pruned", seriesById(S2) == nullptr &&
                  seriesById(S1)->members.size() == sids.size());
        }

        // ---- invariant 4: a member moved out alone LEAVES the series ----------
        {
            int other = newBatch(uniqueBatchName("other"));
            int mv = sids[1];
            size_t before = seriesById(S1)->members.size();
            app.toast.clear();
            moveStackToBatch(mv, other);
            S = seriesById(S1);
            fprintf(stderr, "seriesselftest: moveStackToBatch(%d -> '%s'): %d -> %d member(s), "
                            "toast \"%s\"\n", mv, "other", (int)before,
                    (int)S->members.size(), app.toast.c_str());
            check("invariant 4: the moved stack left the series",
                  S->members.size() == before - 1 && seriesOfStack(mv) == nullptr);
            check("...and the screen was told", app.toast.find("left series") != std::string::npos);
            check("invariant 1: audit after the move-to-batch", audit());
            bool crossed = addToSeries(S1, mv, 1.0);
            fprintf(stderr, "seriesselftest: addToSeries across batches returned %d\n",
                    crossed ? 1 : 0);
            check("a stack of another batch cannot join", !crossed && seriesOfStack(mv) == nullptr);
        }

        // ---- invariant 3: closeStack removes the member ----------------------
        {
            int cl = seriesById(S1)->members.front().seqId;
            size_t before = seriesById(S1)->members.size();
            closeStack(cl);
            S = seriesById(S1);
            fprintf(stderr, "seriesselftest: closeStack(%d): %d -> %d member(s)\n",
                    cl, (int)before, S ? (int)S->members.size() : -1);
            check("invariant 3: closeStack drops the member",
                  S && S->members.size() == before - 1 && seriesOfStack(cl) == nullptr);
            check("invariant 1: audit after closeStack", audit());
        }

        // ---- invariant 5: one member is legal, just not fittable --------------
        while (seriesById(S1) && seriesById(S1)->members.size() > 1)
            removeFromSeries(seriesById(S1)->members.back().seqId);
        S = seriesById(S1);
        check("a one-member series is legal", S != nullptr && S->members.size() == 1);
        check("invariant 5: ...and seriesCanFit() is false", S && !seriesCanFit(*S));

        // ---- invariant 3: closeBatch takes its series with it -----------------
        {
            int before = (int)app.series.size();
            closeBatch(b0);
            fprintf(stderr, "seriesselftest: closeBatch(%d): %d -> %d series\n",
                    b0, before, (int)app.series.size());
            check("invariant 3: closeBatch discards its series",
                  seriesById(S1) == nullptr && app.series.empty());
            check("invariant 1: audit after closeBatch", audit());
        }

        // ---- migration: an OLD session that carried per-stack levels -----------
        {
            std::string lerr = loadSession(legacy);
            check("legacy (seqlevel) session loaded", lerr.empty());
            loadAll();
            double tm = glfwGetTime();
            while (glfwGetTime() - tm < 120.0 && !app.seqLevelLegacy.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* M = app.series.empty() ? nullptr : &app.series.front();
            fprintf(stderr, "seriesselftest: seqlevel migration: %d series, %d member(s), "
                            "name '%s' unit '%s' kind %d (%d levelled stacks in the file)\n",
                    (int)app.series.size(), M ? (int)M->members.size() : -1,
                    M ? M->name.c_str() : "", M ? M->unit : "", M ? M->kind : -1,
                    legacyCount);
            check("one series migrated out of the levels", app.series.size() == 1);
            check("...holding every levelled stack",
                  M && (int)M->members.size() == legacyCount);
            check("...named \"<batch> 掃引\", kind linearity",
                  M && M->name == "legacyset 掃引" && M->kind == App::Series::KLinearity);
            check("invariant 1: audit after migration", audit());
            // The negative half of the same rule: ONE levelled stack is not a
            // sweep, and nothing may be invented from it.
            std::string onePath;
            if (M && !M->members.empty()) {
                std::vector<int> fr = framesOfSeq(M->members.front().seqId);
                if (!fr.empty()) onePath = app.images[fr.front()]->path;
            }
            {
                std::ofstream lf(pathFromUtf8(legacy), std::ios::binary);
                lf << "viewer-session 1\n";
                lf << "image 0 1 npy3 0 0 0 0 0 0 0 " << onePath << "\n";
                lf << "imgbatch lonelyset\n";
                lf << "seqload 1\n";
                lf << "seqlevel 42\n";
            }
            loadSession(legacy);
            loadAll();
            double tm2 = glfwGetTime();
            while (glfwGetTime() - tm2 < 120.0 && !app.seqLevelLegacy.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: one levelled stack alone -> %d series, "
                            "%d stack(s) open\n", (int)app.series.size(),
                    (int)app.seqs.size());
            check("a single levelled stack makes NO series", app.series.empty() &&
                                                            !app.seqs.empty());
        }

        // ---- the create/edit modal, pressed the way a human presses it --------
        // A number that nobody chose is the failure this whole layer exists to
        // prevent, and the modal is the shortest path to one: it is opened to
        // rename a series, and its Save rebuilds every member from the boxes.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int b = app.images.empty() ? 0 : app.images[0]->batchId;
            int sid = selftestMakeSeries(b, "lx");
            App::Series* M = seriesById(sid);
            if (!M || M->members.size() < 4) {
                fprintf(stderr, "seriesselftest: modal fixture failed\n");
                return 1;
            }
            // one member deliberately UNSET, one carrying more digits than the
            // box can show. Pick the unset one where the NAME does suggest a
            // number, so the guess has something to fabricate.
            int unsetIdx = -1;
            for (int i = 0; i < (int)M->members.size(); i++) {
                double g = extractLevelFromName(seqInfo(M->members[i].seqId)->name);
                if (std::isfinite(g) && fabs(g) > 1e-9) { unsetIdx = i; break; }
            }
            const double PRECISE = 1234567.89;
            int preciseIdx = unsetIdx == 0 ? 1 : 0;
            if (unsetIdx < 0) { fprintf(stderr, "seriesselftest: no named level\n"); return 1; }
            int unsetSeq = M->members[unsetIdx].seqId;
            std::string unsetName = seqInfo(unsetSeq)->name;
            double guess = extractLevelFromName(unsetName);
            M->members[unsetIdx].value = std::numeric_limits<double>::quiet_NaN();
            M->members[preciseIdx].value = PRECISE;
            int preciseSeq = M->members[preciseIdx].seqId;
            // "Edit series..." and then "Save", with NOTHING touched between.
            openSeriesModal(b, sid);
            const App::SeriesEdit::Row* r0 = nullptr;
            for (const auto& r : app.seriesEdit.rows) if (r.seqId == unsetSeq) r0 = &r;
            int nValued = 0;
            for (const auto& r : app.seriesEdit.rows)
                if (r.check && std::isfinite(seriesEditRowValue(r))) nValued++;
            fprintf(stderr, "seriesselftest: Edit... on '%s' (value unset, name suggests "
                            "%.6g): box \"%s\", %d of %d rows have a value\n",
                    unsetName.c_str(), guess, r0 ? r0->value : "?", nValued,
                    (int)app.seriesEdit.rows.size());
            check("Edit... leaves an unset member's box EMPTY (no re-guess)",
                  r0 && r0->value[0] == '\0' && std::isfinite(guess));
            check("...and the modal's counter does not claim it has a value",
                  nValued == (int)app.seriesEdit.rows.size() - 1);
            seriesModalAccept();
            M = seriesById(sid);
            double vu = 0, vp = 0;
            for (const auto& m : M->members) {
                if (m.seqId == unsetSeq) vu = m.value;
                if (m.seqId == preciseSeq) vp = m.value;
            }
            fprintf(stderr, "seriesselftest: after a Save that touched nothing: unset "
                            "member %s, %.11g -> %.11g\n",
                    std::isfinite(vu) ? "HAS A VALUE NOW" : "still unset", PRECISE, vp);
            check("a Save that edited nothing leaves the unset member UNSET",
                  !std::isfinite(vu));
            check("...and does not round the values it never touched", vp == PRECISE);
            check("invariant 1: audit after the no-op Save", audit());

            // Text the program cannot read is UNSET, not 0. 0 is not "missing":
            // it is the dark stack the offset is anchored to and the read noise
            // is measured in (linRecompute), so a typo lands on the one value
            // that changes the answer most.
            openSeriesModal(b, sid);
            const char* JUNK[] = { "-", "1e", "12x", "" };
            std::vector<int> junkSeq;
            for (int i = 0; i < 4 && i < (int)app.seriesEdit.rows.size(); i++) {
                App::SeriesEdit::Row& r = app.seriesEdit.rows[i];
                snprintf(r.value, sizeof r.value, "%s", JUNK[i]);
                r.touched = true;
                junkSeq.push_back(r.seqId);
            }
            seriesModalAccept();
            M = seriesById(sid);
            int zeroed = 0, unset = 0;
            for (const auto& m : M->members)
                if (std::find(junkSeq.begin(), junkSeq.end(), m.seqId) != junkSeq.end()) {
                    if (!std::isfinite(m.value)) unset++;
                    else if (fabs(m.value) < 1e-9) zeroed++;
                }
            fprintf(stderr, "seriesselftest: value boxes \"-\" \"1e\" \"12x\" \"\" -> "
                            "%d unset, %d at 0\n", unset, zeroed);
            check("a value box that is not a number saves as UNSET, never 0",
                  unset == (int)junkSeq.size() && zeroed == 0);

            // Editing a series makes the fit on screen a measurement of a series
            // that no longer exists - and the panel reads its LABELS live, so a
            // fit left standing gets relabelled with the new unit.
            for (auto& m : M->members) m.value = extractLevelFromName(seqInfo(m.seqId)->name);
            linRecompute(sid);
            bool wasFit = app.lin.fitValid;
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "ms");
            seriesModalAccept();
            fprintf(stderr, "seriesselftest: fit before the unit edit %s, after %s\n",
                    wasFit ? "valid" : "invalid", app.lin.fitValid ? "STILL VALID" : "dropped");
            check("a fit exists before the edit", wasFit);
            check("editing the series drops the fit it no longer describes",
                  !app.lin.fitValid && app.lin.seriesId == 0);
            // ...and with the unit CLEARED there is no fit at all, which is the
            // one rule this layer was built to enforce.
            openSeriesModal(b, sid);
            app.seriesEdit.unit[0] = '\0';
            seriesModalAccept();
            linRecompute(sid);
            fprintf(stderr, "seriesselftest: unit cleared -> fit %s, %d point(s)\n",
                    app.lin.fitValid ? "PRINTED" : "refused", app.lin.nPts);
            check("a series whose unit was cleared cannot be fitted",
                  !app.lin.fitValid && !seriesCanFit(*seriesById(sid)));

            // The KIND decides the equation (docs/terminology.md). This panel
            // knows linearity and photon transfer; a temperature run pushed
            // through them prints a "sensitivity" in DN per degree.
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "degC");
            app.seriesEdit.kind = App::Series::KTemperature;
            seriesModalAccept();
            linRecompute(sid);
            fprintf(stderr, "seriesselftest: kind=temperature -> canFit %d, fit %s\n",
                    seriesCanFit(*seriesById(sid)) ? 1 : 0,
                    app.lin.fitValid ? "PRINTED" : "refused");
            check("a temperature series is not fitted as linearity",
                  !seriesCanFit(*seriesById(sid)) && !app.lin.fitValid);
            openSeriesModal(b, sid);
            snprintf(app.seriesEdit.unit, sizeof app.seriesEdit.unit, "lx");
            app.seriesEdit.kind = App::Series::KLinearity;
            seriesModalAccept();
            linRecompute(sid);
            check("...and putting the kind back brings the fit back",
                  app.lin.fitValid && seriesCanFit(*seriesById(sid)));

            // seqctx > Series > <name>: joining the SOLE member of one series
            // into another empties the first, and an empty series is the state
            // the audit rejects (plan §1 invariant 3).
            int lone = newSeries(b, "lone");
            if (App::Series* L2 = seriesById(lone))
                snprintf(L2->unit, sizeof L2->unit, "lx");
            int moved = seriesById(sid)->members.front().seqId;
            addToSeries(lone, moved, 1.0);
            std::string jmsg;
            bool joined = seriesJoinFromMenu(sid, moved, &jmsg);
            fprintf(stderr, "seriesselftest: join -> %d, \"%s\"; %d series left\n",
                    joined ? 1 : 0, jmsg.c_str(), (int)app.series.size());
            check("the menu join reports the value and the text it read it from",
                  joined && jmsg.find("read from") != std::string::npos);
            check("...and does not leave the emptied series behind",
                  seriesById(lone) == nullptr);
            check("invariant 1: audit after the menu join", audit());
        }

        // ---- sessions this program did not write ------------------------------
        // The writer honours "unset is not 0"; the reader is where a hand-made,
        // third-party or half-written file gets to claim otherwise.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            std::vector<std::string> paths;
            for (const auto& si : app.seqs) {
                std::vector<int> fr = framesOfSeq(si.id);
                if (!fr.empty()) paths.push_back(app.images[fr.front()]->path);
            }
            std::string bn = batchNameOf(app.images.empty() ? 0 : app.images[0]->batchId);
            std::string hostile = (std::filesystem::temp_directory_path(tec) /
                                   "viewer_serieshostile.vsession").u8string();
            {
                std::ofstream hf(pathFromUtf8(hostile), std::ios::binary);
                hf << "viewer-session 1\n";
                for (const auto& p : paths)
                    hf << "image 0 1 npy3 0 0 0 0 0 0 0 " << p << "\n"
                       << "imgbatch " << bn << "\n" << "seqload 1\n";
                hf << "series hostile\nseriesbatch " << bn << "\n"
                   << "seriesparam illuminance\nseriesunit lx\nserieskind 0\n";
                const char* VALS[] = { "notanumber", "-", "1e", "nan", "12x", "160", "320" };
                for (size_t i = 0; i < paths.size(); i++)
                    hf << "seriesmember " << VALS[std::min<size_t>(i, 6)] << " 1 "
                       << paths[i] << "\n";
                hf << "seriesend\n";
            }
            loadSession(hostile);
            loadAll();
            double th = glfwGetTime();
            while (glfwGetTime() - th < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* H = app.series.empty() ? nullptr : &app.series.front();
            std::string got;
            int atZero = 0;
            if (H)
                for (const auto& m : H->members) {
                    char bb[32];
                    snprintf(bb, sizeof bb, "%.6g", m.value);
                    got += (got.empty() ? "" : ",");
                    got += std::isfinite(m.value) ? bb : "unset";
                    if (std::isfinite(m.value) && fabs(m.value) < 1e-9) atZero++;
                }
            fprintf(stderr, "seriesselftest: hostile values -> [%s]\n", got.c_str());
            check("one series came back from the hostile file", app.series.size() == 1);
            check("no unreadable value became 0", atZero == 0);
            check("...they are UNSET, and the readable ones survived",
                  H && H->members.size() == paths.size() &&
                  std::isfinite(H->members.back().value));
            check("invariant 1: audit after the hostile file", audit());
            std::filesystem::remove(std::filesystem::u8path(hostile), tec);

            // The same folder open TWICE is two batches (the canon blesses it),
            // so one path names two stacks. A restore that takes "the first one"
            // rejects every member of the second copy's series on containment
            // and loses the whole series - permanently, at the next autosave.
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int bA = app.images.empty() ? 0 : app.images[0]->batchId;
            openFolder(g_seriesSelftest);
            loadAll();
            int bB = 0;
            for (const auto& bt : app.batches) if (bt.id != bA) bB = bt.id;
            int sA = selftestMakeSeries(bA, "lx"), sB = selftestMakeSeries(bB, "ms");
            size_t nA = seriesById(sA) ? seriesById(sA)->members.size() : 0;
            size_t nB = seriesById(sB) ? seriesById(sB)->members.size() : 0;
            std::string dup = (std::filesystem::temp_directory_path(tec) /
                               "viewer_seriesdup.vsession").u8string();
            saveSession(dup, true);
            loadSession(dup);
            loadAll();
            double td = glfwGetTime();
            while (glfwGetTime() - td < 120.0 && !app.seriesRestore.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::vector<int> sbatch;
            for (const auto& s : app.series)
                if (std::find(sbatch.begin(), sbatch.end(), s.batchId) == sbatch.end())
                    sbatch.push_back(s.batchId);
            fprintf(stderr, "seriesselftest: same folder in two batches: saved %d + %d "
                            "member(s), restored %d series in %d batch(es)\n",
                    (int)nA, (int)nB, (int)app.series.size(), (int)sbatch.size());
            check("both series survive when one folder is open in two batches",
                  app.series.size() == 2 && nA > 0 && nB > 0 &&
                  app.series[0].members.size() == nA &&
                  app.series[1].members.size() == nB);
            check("...each in its own batch", app.series.size() == 2 &&
                  app.series[0].batchId != app.series[1].batchId);
            check("invariant 1: audit after the two-batch restore", audit());
            std::filesystem::remove(std::filesystem::u8path(dup), tec);
        }

        // ---- phase 4: what the Files panel's series row actually does ---------
        // The rows themselves are ImGui, but every command behind them is a
        // function, and these are the three that can lose data if they are
        // wrong: a move that drops members, an ungroup that closes something,
        // a Close that leaves stacks behind.
        {
            closeAll();
            openFolder(g_seriesSelftest);
            loadAll();
            int b = app.images.empty() ? 0 : app.images[0]->batchId;
            int sid = selftestMakeSeries(b, "lx");
            App::Series* P = seriesById(sid);
            int nStacks = (int)app.seqs.size();
            check("phase 4 fixture: one series over the whole batch",
                  P && (int)P->members.size() == nStacks && nStacks >= 4);
            if (!P) { fprintf(stderr, "seriesselftest: phase 4 fixture failed\n"); return 1; }
            // Move the SERIES: unlike a lone member, it takes everything with it
            // and stays whole (the row's "Move to batch (all members move)").
            {
                int dest = newBatch(uniqueBatchName("moved"));
                std::vector<int> was;
                for (const auto& m : P->members) was.push_back(m.seqId);
                app.toast.clear();
                moveSeriesToBatch(sid, dest);
                P = seriesById(sid);
                bool kept = P && P->members.size() == was.size() && P->batchId == dest;
                if (kept)
                    for (size_t i = 0; i < was.size(); i++)
                        if (P->members[i].seqId != was[i] || batchOfStack(was[i]) != dest)
                            kept = false;
                fprintf(stderr, "seriesselftest: moveSeriesToBatch -> batch '%s', "
                                "%d member(s), toast \"%s\"\n", batchNameOf(dest).c_str(),
                        P ? (int)P->members.size() : -1, app.toast.c_str());
                check("a series moved to another batch keeps every member", kept);
                check("...and nothing was told it 'left the series'",
                      app.toast.find("left series") == std::string::npos);
                check("invariant 1: audit after the series move", audit());
            }
            // UNGROUP: the fence goes, the data stays. This is the operation
            // that must NOT be confused with Close.
            {
                int stacksBefore = (int)app.seqs.size();
                size_t imagesBefore = app.images.size();
                app.toast.clear();
                ungroupSeries(sid);
                fprintf(stderr, "seriesselftest: ungroup -> %d series, %d stack(s), "
                                "%d frame(s), toast \"%s\"\n", (int)app.series.size(),
                        (int)app.seqs.size(), (int)app.images.size(), app.toast.c_str());
                check("ungroup removes the series", seriesById(sid) == nullptr);
                check("...and keeps every stack and frame",
                      (int)app.seqs.size() == stacksBefore &&
                      app.images.size() == imagesBefore);
                check("invariant 1: audit after ungroup", audit());
            }
            // Close series: the canon's Close, which discards the CONTENTS.
            {
                int b2 = app.images.empty() ? 0 : app.images[0]->batchId;
                int sid2 = selftestMakeSeries(b2, "lx");
                App::Series* Q = seriesById(sid2);
                int members = Q ? (int)Q->members.size() : 0;
                int stacksBefore = (int)app.seqs.size();
                app.toast.clear();
                closeSeries(sid2);
                fprintf(stderr, "seriesselftest: close series (%d member(s)) -> %d series, "
                                "%d stack(s) left, toast \"%s\"\n", members,
                        (int)app.series.size(), (int)app.seqs.size(), app.toast.c_str());
                check("Close series discards its stacks",
                      seriesById(sid2) == nullptr &&
                      (int)app.seqs.size() == stacksBefore - members);
                check("invariant 1: audit after Close series", audit());
            }
        }

        // ---- phase 5: the picker's "open as a sweep" --------------------------
        // Both halves. Ticked: one series, values off the group names, one
        // batch even though batch-per-top-folder was asked for. UNTICKED: the
        // same Open, and NOTHING is created - that half is the whole point.
        {
            closeAll();
            openFolder(g_seriesSelftest);          // local scan: synchronous
            check("phase 5: the picker opened", app.folderPickOpen);
            app.pickSweep = true;
            app.pickBatchMode = 1;                 // ...and gets overridden
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            check("the box is cleared on accept (a sweep is never sticky)",
                  !app.pickSweep);
            loadAll();
            double t5 = glfwGetTime();
            while (glfwGetTime() - t5 < 120.0 && !app.seriesPending.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const App::Series* W = app.series.empty() ? nullptr : &app.series.front();
            std::vector<int> bids;                 // batches that actually hold data
            for (const auto& d : app.images)
                if (std::find(bids.begin(), bids.end(), d->batchId) == bids.end())
                    bids.push_back(d->batchId);
            bool valuesOk = W != nullptr && !W->members.empty();
            std::string vals;
            if (W)
                for (const auto& m : W->members) {
                    App::SeqInfo* si = seqInfo(m.seqId);
                    double want = si ? extractLevelFromName(si->name) : 0;
                    char b[32];
                    snprintf(b, sizeof b, "%.6g", m.value);
                    vals += (vals.empty() ? "" : ",");
                    vals += std::isfinite(m.value) ? b : "unset";
                    if (!si || !std::isfinite(m.value) || !std::isfinite(want) ||
                        fabs(m.value - want) > 1e-9 * std::max(1.0, fabs(want)))
                        valuesOk = false;
                }
            fprintf(stderr, "seriesselftest: sweep -> %d series '%s' param '%s' unit '%s' "
                            "kind %d, %d member(s) [%s], %d batch(es), %d stack(s)\n",
                    (int)app.series.size(), W ? W->name.c_str() : "",
                    W ? W->paramName.c_str() : "", W ? W->unit : "", W ? W->kind : -1,
                    W ? (int)W->members.size() : -1, vals.c_str(), (int)bids.size(),
                    (int)app.seqs.size());
            check("open as a sweep makes exactly ONE series", app.series.size() == 1);
            check("...over every stack the picker loaded",
                  W && W->members.size() == app.seqs.size() && app.seqs.size() >= 4);
            check("...with the parameter and unit typed in the picker",
                  W && W->paramName == "illuminance" && std::string(W->unit) == "lx" &&
                  W->kind == App::Series::KLinearity);
            check("...named \"<batch> 掃引\"",
                  W && W->name == batchNameOf(W->batchId) + " 掃引");
            check("...each member at the value its NAME gave", valuesOk);
            {   // a sweep is a parameter axis: it comes out in value order, not
                // in the folders' lexicographic order (0,10,160,20,320,40,80)
                bool asc = true;
                for (size_t i = 1; W && i < W->members.size(); i++)
                    if (W->members[i - 1].value > W->members[i].value) asc = false;
                check("...and the members are in VALUE order", W && asc);
            }
            check("a sweep forces ONE batch (series ⊂ batch beats the batch radio)",
                  bids.size() == 1 && W && W->batchId == bids.front());
            check("invariant 1: audit after the sweep", audit());
            // ...and the half that must NOT happen.
            closeAll();
            openFolder(g_seriesSelftest);
            check("phase 5: the picker opened again", app.folderPickOpen);
            check("the sweep box came up UNTICKED", !app.pickSweep);
            pickerAccept();
            loadAll();
            double t6 = glfwGetTime();             // give a late resolve every chance
            while (glfwGetTime() - t6 < 1.0) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            fprintf(stderr, "seriesselftest: sweep unticked -> %d series, %d stack(s)\n",
                    (int)app.series.size(), (int)app.seqs.size());
            check("unticked: the same Open creates NO series",
                  app.series.empty() && !app.seqs.empty());
            app.pickBatchMode = 0;

            // A second Open before the first sweep has resolved. Resolution
            // waits for every load to drain, which is seconds locally and much
            // longer over the wire, and File > Open Folder is available the
            // whole time: a single pending slot threw the first sweep away -
            // its ticked box, its typed parameter and its unit - in silence.
            closeAll();
            openFolder(g_seriesSelftest);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "illuminance");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "lx");
            pickerAccept();
            check("the first sweep is pending, not applied yet",
                  app.seriesPending.size() == 1);
            openFolder(g_seriesSelftest);       // ...while the first is still loading
            // the picker starts clean on every scan, the sweep boxes included:
            // a unit left over from the last Open would be applied to a folder
            // that has nothing to do with it
            fprintf(stderr, "seriesselftest: second scan's sweep row: box %d, param \"%s\", "
                            "unit \"%s\"\n", app.pickSweep ? 1 : 0, app.pickSweepParam,
                    app.pickSweepUnit);
            check("a new scan resets the sweep box, its parameter and its unit",
                  !app.pickSweep && !app.pickSweepParam[0] && !app.pickSweepUnit[0]);
            app.pickSweep = true;
            snprintf(app.pickSweepParam, sizeof app.pickSweepParam, "exposure");
            snprintf(app.pickSweepUnit, sizeof app.pickSweepUnit, "ms");
            pickerAccept();
            check("the second does not overwrite it - both are queued",
                  app.seriesPending.size() == 2);
            loadAll();
            double t7 = glfwGetTime();
            while (glfwGetTime() - t7 < 240.0 && !app.seriesPending.empty()) {
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string units;
            for (const auto& s : app.series)
                units += std::string(s.unit) + "x" + std::to_string(s.members.size()) + " ";
            fprintf(stderr, "seriesselftest: two sweeps opened back to back -> %d series "
                            "[%s], %d stack(s)\n", (int)app.series.size(), units.c_str(),
                    (int)app.seqs.size());
            check("a sweep opened while another is loading is NOT thrown away",
                  app.series.size() == 2);
            check("...and neither Open loses the stacks it had queued",
                  app.series.size() == 2 && app.series[0].members.size() == 7 &&
                  app.series[1].members.size() == 7);
            check("...and each keeps the parameter and unit it was given",
                  app.series.size() == 2 && app.series[0].paramName == "illuminance" &&
                  std::string(app.series[0].unit) == "lx" &&
                  app.series[1].paramName == "exposure" &&
                  std::string(app.series[1].unit) == "ms");
            check("invariant 1: audit after two queued sweeps", audit());
        }
        fprintf(stderr, "seriesselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        std::filesystem::remove(std::filesystem::u8path(sess), tec);
        std::filesystem::remove(std::filesystem::u8path(legacy), tec);
        return ok ? 0 : 1;
    }

    // Close per layer, verifiable without a human (docs/terminology.md): Ctrl+W's
    // closeCurrent() on a stack member removes the WHOLE stack and every trace of
    // it - and a stack closed mid-fetch must not regrow from the prefetch queue.
    if (!g_closeSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        openFolder(g_closeSelftest);
        if (!app.folderPickOpen) {
            fprintf(stderr, "closeselftest: picker did not open\n");
            return 1;
        }
        pickerAccept();
        loadAll();
        // rows exist, so residue is detectable - and the stack being closed is a
        // series member, so closeStack's series hook is on the hook too
        linRecompute(selftestMakeSeries(app.images.empty() ? 0 : app.images[0]->batchId,
                                        app.lin.unit));
        int imagesBefore = (int)app.images.size(), seqsBefore = (int)app.seqs.size();
        if (seqsBefore < 3) {
            fprintf(stderr, "closeselftest: expected 3 stacks under %s, got %d\n",
                    g_closeSelftest.c_str(), seqsBefore);
            return 1;
        }
        int sid = app.seqs[seqsBefore / 2].id;
        std::vector<int> fr = framesOfSeq(sid);
        int stackFrames = (int)fr.size();
        selectImage(fr[fr.size() / 2]);    // a MIDDLE frame: the reported bug
        closeCurrent();                    // Ctrl+W's path: must close the stack
        bool linResidue = false;
        for (const auto& r : app.lin.rows) if (r.seqId == sid) linResidue = true;
        fprintf(stderr, "closeselftest: closeCurrent on mid frame of stack %d: "
                        "images %d->%d, seqs %d->%d, frames(seq)=%d, "
                        "temporal.seqId=%d, lin rows for seq: %s\n",
                sid, imagesBefore, (int)app.images.size(), seqsBefore,
                (int)app.seqs.size(), (int)framesOfSeq(sid).size(),
                app.temporal[0].seqId, linResidue ? "RESIDUE" : "none");
        if ((int)app.images.size() != imagesBefore - stackFrames ||
            (int)app.seqs.size() != seqsBefore - 1 || !framesOfSeq(sid).empty() ||
            app.temporal[0].seqId != -1 || linResidue) {
            fprintf(stderr, "closeselftest: FAILED - stack close left residue\n");
            ok = false;
        }
        // ---- a stack closed while its remote prefetch is still in flight ----
        std::string scanroot = g_closeSelftest;
        std::replace(scanroot.begin(), scanroot.end(), '\\', '/');
        {
            size_t sl = scanroot.find_last_of('/');
            scanroot = (sl == std::string::npos ? std::string(".")
                                                : scanroot.substr(0, sl)) + "/rb/scanroot";
        }
        int seqsNow = (int)app.seqs.size();
        startRemote("local://" + scanroot);
        double t0 = glfwGetTime();
        bool scanSent = false, closed = false;
        int rsid = 0, rfLeft = -1;
        while (glfwGetTime() - t0 < 120.0) {
            pumpRemoteBrowse();
            pumpRemoteFetch();
            pumpRemoteOpenQueue();
            pumpSequenceAndQueue();
            if (app.rbrowse.connected && !scanSent) {
                App::RbJob j;
                j.kind = App::RbScan;
                j.host = app.rbrowse.host; j.port = app.rbrowse.port; j.dir = scanroot;
                rbEnqueue(std::move(j));
                scanSent = true;
            }
            if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
            if (!closed && (int)app.seqs.size() > seqsNow && app.rfPending > 0) {
                rsid = app.seqs.back().id;     // the stack the fetcher is filling
                closeStack(rsid);
                closed = true;
                std::lock_guard<std::mutex> lk(app.rfMtx);
                rfLeft = 0;
                for (const auto& jj : app.rfQueue) if (jj.seqId == rsid) rfLeft++;
                break;
            }
            if (!app.rbrowse.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!closed) {
            fprintf(stderr, "closeselftest: FAILED - never caught a stack mid-fetch (%s)\n",
                    app.rbrowse.err.c_str());
            ok = false;
        } else {
            // the in-flight job (and the rest of the queue) gets 2 s to land;
            // nothing of the closed stack may grow back
            double t1 = glfwGetTime();
            while (glfwGetTime() - t1 < 2.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int orphans = 0;
            for (const auto& d : app.images) if (d->seqId == rsid) orphans++;
            fprintf(stderr, "closeselftest: closeStack(%d) mid-fetch: %d queued job(s) "
                            "for it left, %d frame(s) regrew after 2 s pump\n",
                    rsid, rfLeft, orphans);
            if (rfLeft != 0 || orphans != 0) {
                fprintf(stderr, "closeselftest: FAILED - closed stack regrew\n");
                ok = false;
            }
        }
        fprintf(stderr, "closeselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }

    // VERIFICATION ADDITION (functional-verification agent, not part of the
    // feature work): the corners of the close / batch rules that the six
    // feature selftests do not reach - non-contiguous stacks, the Ctrl+Alt+W
    // escape hatch, compare-B left dangling, prune's reference set, a move
    // issued mid-load, closeBatch against a queued remote open, and the
    // remote in-flight drop actually being the path that catches a fetch.
    if (!g_verifySelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        auto reload = [&]() {
            closeAll();
            openFolder(g_verifySelftest);
            if (app.folderPickOpen) pickerAccept();
            loadAll();
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "verifyselftest: %-46s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };

        // ---- V1: a stack whose frames are NOT contiguous in app.images ----
        reload();
        if (app.seqs.size() < 3) {
            fprintf(stderr, "verifyselftest: need 3 stacks under %s\n", g_verifySelftest.c_str());
            return 1;
        }
        {   // interleave: round-robin the three stacks so no stack is a run
            std::vector<std::unique_ptr<ImageDoc>> mixed;
            for (size_t k = 0; mixed.size() < app.images.size(); k++)
                for (auto& d : app.images)
                    if (d && (size_t)d->seqIndex == k) mixed.push_back(std::move(d));
            for (auto& d : app.images) if (d) mixed.push_back(std::move(d));
            app.images.swap(mixed);
            app.current = 0;
        }
        {
            int a0 = app.seqs[0].id, a1 = app.seqs[1].id, a2 = app.seqs[2].id;
            std::vector<int> f1 = framesOfSeq(a1);
            bool contiguous = true;                 // prove the fixture is nasty
            for (size_t i = 1; i < f1.size(); i++)
                if (f1[i] != f1[i - 1] + 1) contiguous = false;
            int before = (int)app.images.size(), n1 = (int)f1.size();
            closeStack(a1);
            fprintf(stderr, "verifyselftest: V1 interleaved(contig=%d) closeStack(%d): "
                            "images %d->%d (-%d), survivors %zu/%zu frames\n",
                    contiguous ? 1 : 0, a1, before, (int)app.images.size(), n1,
                    framesOfSeq(a0).size(), framesOfSeq(a2).size());
            check(!contiguous, "V1 fixture really is non-contiguous");
            check((int)app.images.size() == before - n1 && framesOfSeq(a1).empty() &&
                  framesOfSeq(a0).size() == 5 && framesOfSeq(a2).size() == 5,
                  "V1 non-contiguous closeStack keeps neighbours");
        }

        // ---- V2: Ctrl+Alt+W on a middle frame leaves a coherent stack ----
        reload();
        {
            int sid = app.seqs[1].id;
            std::vector<int> fr = framesOfSeq(sid);
            int want = (int)fr.size() - 1;
            uint64_t revBefore = app.imagesRev;
            selectImage(fr[fr.size() / 2]);
            int goneIndex = cur()->seqIndex;
            closeCurrent(true);                     // Ctrl+Alt+W
            std::vector<int> after = framesOfSeq(sid);
            bool gap = false;                       // the removed seqIndex is absent
            for (int idx : after) if (app.images[idx]->seqIndex == goneIndex) gap = true;
            fprintf(stderr, "verifyselftest: V2 Ctrl+Alt+W frame %d of stack %d: "
                            "%zu frames left, seqs=%zu, imagesRev %llu->%llu\n",
                    goneIndex, sid, after.size(), app.seqs.size(),
                    (unsigned long long)revBefore, (unsigned long long)app.imagesRev);
            check((int)after.size() == want && !gap, "V2 single-frame close leaves N-1 frames");
            check(seqInfo(sid) != nullptr, "V2 stack survives a single-frame close");
            check(app.imagesRev != revBefore, "V2 imagesRev bumped (Files cache)");
        }

        // ---- V3: closing the LAST frame of a stack drops the SeqInfo ----
        reload();
        {
            int sid = app.seqs[1].id;
            while (framesOfSeq(sid).size() > 1) {
                selectImage(framesOfSeq(sid).front());
                closeCurrent(true);
            }
            selectImage(framesOfSeq(sid).front());
            closeCurrent(true);                     // the last one
            fprintf(stderr, "verifyselftest: V3 emptied stack %d one frame at a time: "
                            "seqInfo=%s, frames=%zu\n",
                    sid, seqInfo(sid) ? "STILL THERE" : "gone", framesOfSeq(sid).size());
            check(seqInfo(sid) == nullptr && framesOfSeq(sid).empty(),
                  "V3 last-frame close drops the SeqInfo");
        }

        // ---- V4: compare-B pointing into a closed stack ----
        reload();
        {
            int sid = app.seqs[2].id;
            selectImage(framesOfSeq(app.seqs[0].id).front());
            setCompareB(app.images[framesOfSeq(sid)[1]].get());
            app.compareMode = App::CmpWipe;
            bool had = resolveB() != nullptr;
            closeStack(sid);
            fprintf(stderr, "verifyselftest: V4 closeStack(%d) with B inside: "
                            "hadB=%d uid=%llu name='%s' seq=%d resolveB=%p\n",
                    sid, had ? 1 : 0, (unsigned long long)app.compareBUid,
                    app.compareB.c_str(), app.compareBSeq, (void*)resolveB());
            check(had, "V4 B really pointed into the stack");
            check(app.compareBUid == 0 && app.compareB.empty() && !resolveB(),
                  "V4 closeStack clears a dangling compare-B");
        }
        // ---- V4b: the Ctrl+Alt+W escape hatch and a dangling compare-B.
        // ensureCompareB() re-points B away from cur(), so "close the frame that
        // IS B" cannot be reached from the UI; this pins B by hand to probe
        // whether closeCurrent(frameOnly) does the closeImages() cleanup at all.
        reload();
        {
            int sid = app.seqs[2].id;
            selectImage(framesOfSeq(sid)[1]);
            app.compareBUid = cur()->uid;           // white-box: B == the current frame
            app.compareB = cur()->name;
            app.compareBSeq = cur()->seqIndex;
            app.compareMode = App::CmpWipe;
            std::string bname = app.compareB;
            closeCurrent(true);                     // close exactly the B frame
            std::string leftName = app.compareB;
            // first resolveB() only drops the stale uid and returns null; the
            // SECOND one falls through to the name path, which is why
            // closeImages() clears the name and this path must too
            ImageDoc* b1 = resolveB();
            ImageDoc* b2 = resolveB();
            fprintf(stderr, "verifyselftest: V4b Ctrl+Alt+W on the B frame (B pinned by "
                            "hand): saved '%s', name left behind '%s', "
                            "resolveB #1 -> %s, resolveB #2 -> %s\n",
                    bname.c_str(), leftName.c_str(),
                    b1 ? b1->name.c_str() : "(null)",
                    b2 ? (b2->path + " seq " + std::to_string(b2->seqId)).c_str() : "(null)");
            check(leftName.empty(), "V4b frameOnly close clears the compare-B name");
            check(b2 == nullptr, "V4b closed B does not re-latch onto a same-named frame");
        }

        // ---- V5: prune must keep batches referenced only by a queue ----
        reload();
        {
            int b1 = newBatch(uniqueBatchName("qseq"));
            int b2 = newBatch(uniqueBatchName("qrb"));
            int b3 = newBatch(uniqueBatchName("qload"));
            int b4 = newBatch(uniqueBatchName("qnone"));
            App::PendingGroup pg; pg.name = "x"; pg.files.push_back("x.npy"); pg.batchId = b1;
            app.seqQueue.push_back(pg);
            app.rbOpenQueue.push_back({ "", { "y.npy" }, "y", b2 });
            app.loadBatchId = b3;
            pruneEmptyBatches();
            auto live = [&](int id) {
                for (const auto& b : app.batches) if (b.id == id) return true;
                return false;
            };
            fprintf(stderr, "verifyselftest: V5 prune with refs: seqQueue=%d rbOpen=%d "
                            "loadBatchId=%d unreferenced=%d\n",
                    live(b1) ? 1 : 0, live(b2) ? 1 : 0, live(b3) ? 1 : 0, live(b4) ? 1 : 0);
            check(live(b1) && live(b2) && live(b3), "V5 prune keeps queue-referenced batches");
            check(!live(b4), "V5 prune drops the unreferenced batch");
            // ---- V5b: closeBatch removes the queued remote open too ----
            size_t rbBefore = app.rbOpenQueue.size();
            closeBatch(b2);
            fprintf(stderr, "verifyselftest: V5b closeBatch(qrb): rbOpenQueue %zu->%zu, "
                            "batch %s\n", rbBefore, app.rbOpenQueue.size(),
                    live(b2) ? "STILL THERE" : "gone");
            check(app.rbOpenQueue.size() == rbBefore - 1 && !live(b2),
                  "V5b closeBatch purges its rbOpenQueue entry");
            app.seqQueue.clear();
            app.rbOpenQueue.clear();
            app.loadBatchId = 0;
        }

        // ---- V6: move a stack that is still loading ----
        {
            closeAll();
            openFolder(g_verifySelftest);
            if (app.folderPickOpen) pickerAccept();
            int movedSeq = 0, target = 0;
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 120.0) {    // catch it mid-load
                pumpSequenceAndQueue();
                if (!movedSeq && app.seqRunning && app.seqLoadingId &&
                    framesOfSeq(app.seqLoadingId).size() >= 1 &&
                    framesOfSeq(app.seqLoadingId).size() < 5) {
                    movedSeq = app.seqLoadingId;
                    target = newBatch(uniqueBatchName("midload"));
                    moveStackToBatch(movedSeq, target);
                }
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            int off = 0;
            std::vector<int> fr = movedSeq ? framesOfSeq(movedSeq) : std::vector<int>();
            for (int idx : fr) if (app.images[idx]->batchId != target) off++;
            fprintf(stderr, "verifyselftest: V6 move-mid-load stack %d: %zu frames, "
                            "%d off-batch\n", movedSeq, fr.size(), off);
            check(movedSeq != 0, "V6 caught a stack mid-load");
            check(fr.size() == 5 && off == 0, "V6 late frames follow the moved stack");
        }

        // ---- V7: two same-named batches survive a session round trip apart ----
        reload();
        {
            // create-then-move one at a time: moveStackToBatch prunes, and an
            // empty second batch would be pruned before it could be filled
            int b1 = newBatch(uniqueBatchName("dup"));
            moveStackToBatch(app.seqs[0].id, b1);
            int b2 = newBatch(uniqueBatchName("dup"));
            moveStackToBatch(app.seqs[1].id, b2);
            std::string n1, n2;
            for (const auto& b : app.batches) {
                if (b.id == b1) n1 = b.name;
                if (b.id == b2) n2 = b.name;
            }
            std::error_code vec;
            std::string sess = (std::filesystem::temp_directory_path(vec) /
                                "viewer_verifyselftest.vsession").u8string();
            saveSession(sess, true);
            loadSession(sess);
            loadAll();
            int f1 = 0, f2 = 0, id1 = 0, id2 = 0;
            for (const auto& b : app.batches) {
                if (b.name == n1) id1 = b.id;
                if (b.name == n2) id2 = b.id;
            }
            for (const auto& d : app.images) {
                if (d->batchId == id1) f1++;
                if (d->batchId == id2) f2++;
            }
            fprintf(stderr, "verifyselftest: V7 '%s' + '%s' round trip: both present=%d, "
                            "frames %d / %d, merged=%d\n",
                    n1.c_str(), n2.c_str(), (id1 && id2) ? 1 : 0, f1, f2,
                    (id1 && id1 == id2) ? 1 : 0);
            check(n1 != n2, "V7 uniqueBatchName disambiguates the collision");
            check(id1 && id2 && id1 != id2, "V7 both batches survive the round trip apart");
            std::filesystem::remove(std::filesystem::u8path(sess), vec);
        }

        // ---- V8: the remote IN-FLIGHT drop, not just the queue sweep ----
        {
            closeAll();
            std::string scanroot = g_verifySelftest;
            std::replace(scanroot.begin(), scanroot.end(), '\\', '/');
            size_t sl = scanroot.find_last_of('/');
            scanroot = (sl == std::string::npos ? std::string(".")
                                                : scanroot.substr(0, sl)) + "/rb/scanroot";
            startRemote("local://" + scanroot);
            double t0 = glfwGetTime();
            bool scanSent = false, closed = false;
            int rsid = 0, queuedForSeq = -1, pendingAfter = -1;
            while (glfwGetTime() - t0 < 120.0) {
                pumpRemoteBrowse();
                pumpRemoteFetch();
                pumpRemoteOpenQueue();
                pumpSequenceAndQueue();
                if (app.rbrowse.connected && !scanSent) {
                    App::RbJob j;
                    j.kind = App::RbScan;
                    j.host = app.rbrowse.host; j.port = app.rbrowse.port; j.dir = scanroot;
                    rbEnqueue(std::move(j));
                    scanSent = true;
                }
                if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
                // Wait for a moment when the WORKER holds a job: rfPending counts
                // queued + in-flight, so rfPending > (jobs still in rfQueue) means
                // exactly one is out of the queue's reach. That is the only state
                // in which the pumpRemoteFetch drop is load-bearing.
                if (!closed && !app.seqs.empty() && app.rfPending > 0) {
                    int sid2 = app.seqs.back().id, q = 0;
                    {
                        std::lock_guard<std::mutex> lk(app.rfMtx);
                        for (const auto& jj : app.rfQueue) if (jj.seqId == sid2) q++;
                    }
                    if (app.rfPending > q) {        // a job is IN FLIGHT right now
                        rsid = sid2; queuedForSeq = q;
                        closeStack(rsid);
                        pendingAfter = app.rfPending;
                        closed = true;
                        break;
                    }
                }
                if (!app.rbrowse.err.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            double t1 = glfwGetTime();
            while (glfwGetTime() - t1 < 3.0) {
                pumpRemoteBrowse(); pumpRemoteFetch();
                pumpRemoteOpenQueue(); pumpSequenceAndQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int orphans = 0;
            for (const auto& d : app.images) if (d->seqId == rsid) orphans++;
            fprintf(stderr, "verifyselftest: V8 closeStack(%d): %d job(s) swept from the "
                            "queue, rfPending after = %d (in flight), %d frame(s) regrew\n",
                    rsid, queuedForSeq, pendingAfter, orphans);
            check(closed, "V8 caught a remote stack with a fetch outstanding");
            check(orphans == 0, "V8 nothing of the closed stack regrew");
            fprintf(stderr, "verifyselftest: V8 note: the in-flight drop path was%s "
                            "the one that had to catch it\n",
                    pendingAfter > 0 ? "" : " NOT");
        }

        fprintf(stderr, "verifyselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }

    // ---- A/B statistics caches (docs/ab-stats-plan.md, section 6) -------------
    // The five statistics panels keep TWO cache slots now: 0 = A (the current
    // frame), 1 = B (the compare side). Headless, what has to hold:
    //   A1  both slots fill, each keyed to its OWN document
    //   A2  B's numbers really are B's - recomputed here from B's pixels by a
    //       second implementation, not by calling the panel's code
    //   A3  A's numbers are byte-identical to the compare-off run: B's existence
    //       must not move a single digit on the A side
    //   A4  a frame step follows on both sides, and temporal (keyed on the STACK,
    //       not the frame) does NOT re-run for it
    //   A6  compare off invalidates slot 1 exactly once, and closing B leaves no
    //       dangling ImageDoc* behind in it
    if (!g_abstatsSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 300.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "abstatsselftest: %-54s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        auto relEq = [](double x, double y, double tol) {
            double m = std::max(std::max(fabs(x), fabs(y)), 1e-300);
            return fabs(x - y) <= tol * m;
        };

        openFolder(g_abstatsSelftest);
        loadAll();
        if (app.seqs.size() < 2) {
            fprintf(stderr, "abstatsselftest: need 2 stacks under %s\n",
                    g_abstatsSelftest.c_str());
            stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
            return 1;
        }

        // ---- the reference implementations, written from the rule, not shared
        // with the panel: 256 bins across black..white, one sample every
        // `step`-th pixel of the resolved rect, CFA planes never mixed.
        struct RefHist {
            uint32_t bins[4][256] = {};
            double mean[4] = {}, sd[4] = {};
            size_t sampled = 0;
            int nSeries = 0;
        };
        auto refHistogram = [](const ImageDoc& im) {
            RefHist R;
            const int rw = im.w, rh = im.h;             // whole image: no ROI here
            bool cfa = im.ch == 1 && im.cfa != 0;
            R.nSeries = cfa ? 4 : std::min(im.ch, 3);
            const float black = effBlack(im), white = effWhite(im);
            const float inv = 256.0f / std::max(white - black, 1e-20f);
            const size_t total = (size_t)rw * rh;
            const size_t step = std::max<size_t>(1, total / 1000000);
            double s1[4] = {}, s2[4] = {};
            size_t n[4] = {};
            // row-major walk, keeping every step-th linear index: the same
            // sample SET as the panel, reached a different way
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    size_t p = (size_t)y * rw + x;
                    if (p % step) continue;
                    const float* src = &im.data[((size_t)y * im.w + x) * im.ch];
                    if (cfa) {
                        float v = src[0];
                        if (std::isfinite(v)) {
                            int s = cfaChannelAt(im, x, y);
                            float t = (v - black) * inv;
                            int b = t < 0 ? 0 : (t >= 256 ? 255 : (int)t);
                            R.bins[s][b]++;
                            s1[s] += v; s2[s] += (double)v * v; n[s]++;
                        }
                    } else {
                        for (int c = 0; c < R.nSeries; c++) {
                            float v = src[c];
                            if (!std::isfinite(v)) continue;
                            float t = (v - black) * inv;
                            int b = t < 0 ? 0 : (t >= 256 ? 255 : (int)t);
                            R.bins[c][b]++;
                            s1[c] += v; s2[c] += (double)v * v; n[c]++;
                        }
                    }
                    R.sampled++;
                }
            for (int s = 0; s < R.nSeries; s++) {
                if (!n[s]) continue;
                double m = s1[s] / n[s], var = s2[s] / n[s] - m * m;
                R.mean[s] = m;
                R.sd[s] = sqrt(var > 0 ? var : 0);
            }
            return R;
        };
        // Independent temporal noise over a stack: per-sample variance across
        // the resident frames, on the same 40000-sample grid.
        auto refSigmaT = [](const ImageDoc& im) {
            std::vector<int> f = framesOfSeq(im.seqId);
            const size_t total = (size_t)im.w * im.h;
            const size_t step = std::max<size_t>(1, total / 40000);
            std::vector<size_t> offs;
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) {
                    size_t p = (size_t)y * im.w + x;
                    if (p % step == 0) offs.push_back(((size_t)y * im.w + x) * im.ch);
                }
            std::vector<double> s1(offs.size(), 0), s2(offs.size(), 0);
            int used = 0;
            for (int fi : f) {
                const ImageDoc& fr = *app.images[fi];
                if (fr.w != im.w || fr.h != im.h || fr.ch != im.ch) continue;
                bool any = false;
                for (size_t k = 0; k < offs.size(); k++) {
                    float v = fr.data[offs[k]];
                    if (!std::isfinite(v)) continue;
                    s1[k] += v; s2[k] += (double)v * v; any = true;
                }
                if (any) used++;
            }
            double tvar = 0;
            if (used >= 2)
                for (size_t k = 0; k < offs.size(); k++) {
                    double m = s1[k] / used, v = s2[k] / used - m * m;
                    tvar += v > 0 ? v : 0;
                }
            return offs.empty() ? 0.0 : sqrt(tvar / offs.size());
        };
        // "byte-identical" as the plan means it: every number the panel puts on
        // screen, compared bit for bit. (Not memcmp over the whole struct - its
        // padding bytes are unspecified and would make the test lie either way.)
        auto histSame = [](const App::HistState& a, const App::HistState& b) {
            return a.nSeries == b.nSeries && a.maxBin == b.maxBin &&
                   a.sampled == b.sampled && a.rx == b.rx && a.ry == b.ry &&
                   a.rw == b.rw && a.rh == b.rh && a.roiUsed == b.roiUsed &&
                   memcmp(&a.black, &b.black, sizeof a.black) == 0 &&
                   memcmp(&a.white, &b.white, sizeof a.white) == 0 &&
                   memcmp(a.bins, b.bins, sizeof a.bins) == 0 &&
                   memcmp(a.mean, b.mean, sizeof a.mean) == 0 &&
                   memcmp(a.sd, b.sd, sizeof a.sd) == 0 &&
                   memcmp(&a.clipLo, &b.clipLo, sizeof a.clipLo) == 0 &&
                   memcmp(&a.clipHi, &b.clipHi, sizeof a.clipHi) == 0;
        };
        auto vecSame = [](const std::vector<float>& a, const std::vector<float>& b) {
            return a.size() == b.size() &&
                   (a.empty() || memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
        };
        auto projSame = [&](const App::ProjState& a, const App::ProjState& b) {
            if (a.nSeries != b.nSeries || a.rw != b.rw || a.rh != b.rh) return false;
            if (memcmp(&a.hMin, &b.hMin, sizeof a.hMin) || memcmp(&a.hMax, &b.hMax, sizeof a.hMax) ||
                memcmp(&a.vMin, &b.vMin, sizeof a.vMin) || memcmp(&a.vMax, &b.vMax, sizeof a.vMax))
                return false;
            for (int s = 0; s < 4; s++) {
                if (!vecSame(a.h[s], b.h[s]) || !vecSame(a.v[s], b.v[s])) return false;
                const App::ProjState::Stats& x = a.hStat[s]; const App::ProjState::Stats& y = b.hStat[s];
                const App::ProjState::Stats& u = a.vStat[s]; const App::ProjState::Stats& w2 = b.vStat[s];
                if (memcmp(&x, &y, sizeof(double) * 6) || x.valid != y.valid) return false;
                if (memcmp(&u, &w2, sizeof(double) * 6) || u.valid != w2.valid) return false;
            }
            return true;
        };
        auto tempSame = [&](const App::TemporalState& a, const App::TemporalState& b) {
            return a.seqId == b.seqId && a.frames == b.frames && a.valid == b.valid &&
                   a.rx == b.rx && a.ry == b.ry && a.rw == b.rw && a.rh == b.rh &&
                   vecSame(a.idx, b.idx) && vecSame(a.frameMean, b.frameMean) &&
                   vecSame(a.frameStd, b.frameStd) &&
                   memcmp(&a.tempNoise, &b.tempNoise, sizeof a.tempNoise) == 0 &&
                   memcmp(&a.fixedPattern, &b.fixedPattern, sizeof a.fixedPattern) == 0 &&
                   memcmp(&a.totalNoise, &b.totalNoise, sizeof a.totalNoise) == 0;
        };

        int sidA = app.seqs[0].id, sidB = app.seqs[1].id;
        std::vector<int> frA = framesOfSeq(sidA), frB = framesOfSeq(sidB);
        selectImage(frA.front());

        // ---- A3 baseline: everything A, with compare OFF ----
        app.compareMode = App::CmpOff;
        abStatsFrame();
        recomputeHistogramIfNeeded(cur(), app.hist[0]);
        recomputeProjectionIfNeeded(cur(), app.proj[0]);
        recomputeTemporalIfNeeded(cur(), app.temporal[0]);
        App::HistState hOff = app.hist[0];
        App::ProjState pOff = app.proj[0];
        App::TemporalState tOff = app.temporal[0];
        fprintf(stderr, "abstatsselftest: compare OFF, A='%s' (stack %d): "
                        "hist sampled=%zu maxBin=%u mean0=%.9g, sigma_t=%.9g\n",
                cur()->name.c_str(), sidA, hOff.sampled, hOff.maxBin, hOff.mean[0],
                tOff.tempNoise);
        check(app.hist[1].uid == 0 && app.proj[1].uid == 0 &&
              app.temporal[1].seqId == -1,
              "A0 compare off leaves slot 1 empty");

        // ---- A1: compare on, both slots fill ----
        // Explicitly on "B uses A's range". A3 below asserts that adding B does
        // not move A, and that is only the contract in this mode: under the
        // union default A's bin axis is SUPPOSED to widen to cover B - that is
        // the whole mode - and the union's own contract is checked in A3b.
        app.compareRangeMode = 1;
        setCompareB(app.images[frB.front()].get());
        app.compareMode = App::CmpSplit;
        app.compareFollowFrame = true;
        abStatsFrame();
        ImageDoc* B = cmpB();
        if (!B) { fprintf(stderr, "abstatsselftest: no B after setCompareB\n"); return 1; }
        recomputeHistogramIfNeeded(cur(), app.hist[0]);
        recomputeProjectionIfNeeded(cur(), app.proj[0]);
        recomputeTemporalIfNeeded(cur(), app.temporal[0]);
        recomputeHistogramIfNeeded(B, app.hist[1]);
        recomputeProjectionIfNeeded(B, app.proj[1]);
        recomputeTemporalIfNeeded(B, app.temporal[1]);
        fprintf(stderr, "abstatsselftest: compare SPLIT+follow, B='%s' (stack %d): "
                        "hist[0].uid=%llu (A=%llu)  hist[1].uid=%llu (B=%llu)  "
                        "proj[1].uid=%llu  temporal[1].seqId=%d frames=%d\n",
                B->name.c_str(), sidB,
                (unsigned long long)app.hist[0].uid, (unsigned long long)cur()->uid,
                (unsigned long long)app.hist[1].uid, (unsigned long long)B->uid,
                (unsigned long long)app.proj[1].uid,
                app.temporal[1].seqId, app.temporal[1].frames);
        check(app.hist[0].uid == cur()->uid && app.hist[1].uid == B->uid &&
              app.hist[0].uid != app.hist[1].uid, "A1 histogram: both slots, own uid");
        check(app.proj[0].uid == cur()->uid && app.proj[1].uid == B->uid,
              "A1 projection: both slots, own uid");
        check(app.temporal[0].seqId == sidA && app.temporal[1].seqId == sidB &&
              app.temporal[0].valid && app.temporal[1].valid,
              "A1 temporal: both slots, own stack");
        check(app.hist[1].img == B && app.proj[1].img == B,
              "A1 slot 1 names B, not A");

        // ---- A2: B's numbers recomputed here, from B's pixels ----
        {
            RefHist R = refHistogram(*B);
            const App::HistState& HB = app.hist[1];
            bool binsEq = R.nSeries == HB.nSeries &&
                          memcmp(R.bins, HB.bins, sizeof R.bins) == 0 &&
                          R.sampled == HB.sampled;
            bool statEq = true;
            for (int s = 0; s < R.nSeries; s++)
                if (!relEq(R.mean[s], HB.mean[s], 1e-6) || !relEq(R.sd[s], HB.sd[s], 1e-6))
                    statEq = false;
            double refSig = refSigmaT(*B);
            fprintf(stderr, "abstatsselftest: B independent recompute: series=%d "
                            "sampled=%zu/%zu  mean0 %.12g vs %.12g  sd0 %.12g vs %.12g  "
                            "sigma_t %.12g vs %.12g\n",
                    R.nSeries, R.sampled, HB.sampled, R.mean[0], HB.mean[0],
                    R.sd[0], HB.sd[0], refSig, app.temporal[1].tempNoise);
            check(binsEq, "A2 B histogram bins match an independent pass exactly");
            check(statEq, "A2 B mean/sd match within 1e-6 relative");
            check(relEq(refSig, app.temporal[1].tempNoise, 1e-6),
                  "A2 B sigma_t matches within 1e-6 relative");
            // the two sides really are different data, or A2/A3 prove nothing
            check(memcmp(app.hist[0].bins, app.hist[1].bins, sizeof app.hist[0].bins) != 0,
                  "A2 fixture: A and B histograms actually differ");
        }

        // ---- A3: A did not move (on "B uses A's range") ----
        check(histSame(hOff, app.hist[0]), "A3 A histogram byte-identical with B present");
        check(projSame(pOff, app.proj[0]), "A3 A projection byte-identical with B present");
        check(tempSame(tOff, app.temporal[0]), "A3 A temporal byte-identical with B present");

        // ---- A3b: the union default bins BOTH sides on A and B together ----
        {
            app.compareRangeMode = 2;
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            recomputeHistogramIfNeeded(B, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            float uLo = std::min(cur()->vmin, B->vmin), uHi = std::max(cur()->vmax, B->vmax);
            bool axis = app.hist[0].black == uLo && app.hist[0].white == uHi;
            bool shared = app.hist[0].black == app.hist[1].black &&
                          app.hist[0].white == app.hist[1].white;
            fprintf(stderr, "abstatsselftest: union: A bins %.9g..%.9g, B bins %.9g..%.9g "
                            "| content A %.9g..%.9g, B %.9g..%.9g | A alone %.9g..%.9g\n",
                    app.hist[0].black, app.hist[0].white, app.hist[1].black, app.hist[1].white,
                    cur()->vmin, cur()->vmax, B->vmin, B->vmax, hOff.black, hOff.white);
            check(axis, "A3b union: A is binned on the A-and-B union, not on A alone");
            check(shared, "A3b union: both sides land on the same bin axis");
            // ...and it is a DISPLAY overlay: going back restores A exactly
            app.compareRangeMode = 1;
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            check(histSame(hOff, app.hist[0]),
                  "A3b back on A's range: A is byte-identical to compare-off again");
        }

        // ---- A4: five frame steps, both keys follow; temporal does not re-run ----
        {
            App::TemporalState tB0 = app.temporal[1];
            uint64_t bUid0 = B->uid;
            bool keysFollow = true, temporalMoved = false, bMoved = false;
            for (int k = 0; k < 5; k++) {
                gotoFrame(1);
                abStatsFrame();
                ImageDoc* b2 = cmpB();
                if (!b2) { keysFollow = false; break; }
                recomputeHistogramIfNeeded(cur(), app.hist[0]);
                recomputeHistogramIfNeeded(b2, app.hist[1]);
                recomputeTemporalIfNeeded(cur(), app.temporal[0]);
                recomputeTemporalIfNeeded(b2, app.temporal[1]);
                if (app.hist[0].uid != cur()->uid || app.hist[1].uid != b2->uid)
                    keysFollow = false;
                if (b2->uid != bUid0) bMoved = true;
                if (!tempSame(tB0, app.temporal[1])) temporalMoved = true;
            }
            fprintf(stderr, "abstatsselftest: 5x gotoFrame(1): A frame=%d, B frame=%d, "
                            "B uid %llu->%llu, temporal[1].seqId=%d frames=%d\n",
                    cur()->seqIndex, cmpB() ? cmpB()->seqIndex : -1,
                    (unsigned long long)bUid0,
                    (unsigned long long)(cmpB() ? cmpB()->uid : 0),
                    app.temporal[1].seqId, app.temporal[1].frames);
            check(keysFollow, "A4 both hist slots follow the frame step");
            check(bMoved && cmpB() && cmpB()->seqIndex == cur()->seqIndex,
                  "A4 follow-frame put B on A's frame number");
            check(!temporalMoved && app.temporal[1].seqId == sidB,
                  "A4 temporal[1] unchanged by a frame step (keyed on the stack)");
        }

        // ---- P1: the histogram's own rules ----
        {
            ImageDoc* b4 = cmpB();
            // B gets a deliberately DIFFERENT display range; the bins must still
            // be A's, or the two curves share an x axis they were not binned on
            float bBlack0 = b4->black, bWhite0 = b4->white;
            b4->black = bBlack0 - 137.5f; b4->white = bWhite0 + 211.25f;
            recomputeHistogramIfNeeded(b4, app.hist[1], effBlack(*cur()), effWhite(*cur()));
            fprintf(stderr, "abstatsselftest: P1 bin axis: A black/white %.6g/%.6g, "
                            "B's own %.6g/%.6g, hist[1] binned on %.6g/%.6g\n",
                    effBlack(*cur()), effWhite(*cur()), effBlack(*b4), effWhite(*b4),
                    app.hist[1].black, app.hist[1].white);
            // b4->black/white, NOT effBlack: since the A/B range modes landed,
            // effBlack(B) deliberately returns A's range, so asking it whether
            // B "differs" answers a different question and always says no.
            check(b4->black != cur()->black || b4->white != cur()->white,
                  "P1 fixture: B's own range really differs");
            check(app.hist[1].black == effBlack(*cur()) &&
                  app.hist[1].white == effWhite(*cur()),
                  "P1 B is binned on A's black/white, not its own");
            b4->black = bBlack0; b4->white = bWhite0;
            recomputeHistogramIfNeeded(b4, app.hist[1], effBlack(*cur()), effWhite(*cur()));
        }
        {   // the plane selector is presentation only: it must not touch a bin
            App::HistState hA = app.hist[0], hB = app.hist[1];
            int keep = app.histPlane;
            bool same = true;
            for (int p = -1; p < 4; p++) {
                app.histPlane = p;
                recomputeHistogramIfNeeded(cur(), app.hist[0]);
                recomputeHistogramIfNeeded(cmpB(), app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
                if (!histSame(hA, app.hist[0]) || !histSame(hB, app.hist[1])) same = false;
            }
            app.histPlane = keep;
            check(same, "P1 plane selector changes nothing that was measured");
        }
        {   // Auto mirrors the image; an explicit choice overrides it both ways
            int keep = app.abStatsLayout;
            app.abStatsLayout = App::AbAuto;
            app.compareMode = App::CmpSplit; bool autoSplit = abSideBySide();
            app.compareMode = App::CmpWipe;  bool autoWipe = abSideBySide();
            app.abStatsLayout = App::AbSide; bool forcedSide = abSideBySide();
            app.abStatsLayout = App::AbOverlay;
            app.compareMode = App::CmpSplit; bool forcedOver = abSideBySide();
            fprintf(stderr, "abstatsselftest: P1 layout: auto/split=%d auto/wipe=%d "
                            "forced-side/wipe=%d forced-overlay/split=%d\n",
                    autoSplit, autoWipe, forcedSide, forcedOver);
            check(autoSplit && !autoWipe && forcedSide && !forcedOver,
                  "P1 Auto follows the image layout, explicit overrides it");
            app.abStatsLayout = keep;
            app.compareMode = App::CmpSplit;
        }

        // ---- P2: a B that does not share A's axis must never be overlaid ----
        {
            check(abProjOverlayable(app.proj[0], app.proj[1]),
                  "P2 same-size B shares A's profile axis");
            std::string root = g_abstatsSelftest;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            size_t sl = root.find_last_of('/');
            root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
            size_t before = app.images.size();
            openPath(root + "/grad_u16.npy");           // 640x480 against A's 80x64
            loadAll();
            if (app.images.size() > before) {
                ImageDoc* odd = app.images.back().get();
                selectImage(frA.front());
                setCompareB(odd);
                app.compareMode = App::CmpSplit;
                abStatsFrame();
                ImageDoc* b5 = cmpB();
                recomputeProjectionIfNeeded(cur(), app.proj[0]);
                recomputeProjectionIfNeeded(b5, app.proj[1]);
                bool over = abProjOverlayable(app.proj[0], app.proj[1]);
                fprintf(stderr, "abstatsselftest: P2 odd B '%s' %dx%d vs A %dx%d: "
                                "profile A x %d..%d / B x %d..%d, overlayable=%d\n",
                        b5->name.c_str(), b5->w, b5->h, cur()->w, cur()->h,
                        app.proj[0].rx, app.proj[0].rx + app.proj[0].rw - 1,
                        app.proj[1].rx, app.proj[1].rx + app.proj[1].rw - 1, over ? 1 : 0);
                check(b5->w != cur()->w || b5->h != cur()->h,
                      "P2 fixture: the odd B really is a different size");
                check(!over, "P2 size-mismatched B is refused the overlay");
                // and the channel counts differ too, which P3's delta columns use
                check(app.proj[0].nSeries != app.proj[1].nSeries ||
                      cur()->ch == b5->ch, "P2 series count reflects the channel count");
                selectImage((int)app.images.size() - 1);  // the extra image ITSELF
                closeCurrent(true);                       // ...and only that one
                selectImage(frA.front());
                setCompareB(app.images[framesOfSeq(sidB).front()].get());
                abStatsFrame();
            } else {
                fprintf(stderr, "abstatsselftest: P2 skipped (%s/grad_u16.npy not there)\n",
                        root.c_str());
            }
        }

        // ---- P2b: held-down stepping holds B's slots instead of recomputing ----
        {
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            app.abStepBusyUntil = 0;
            abStatsFrame();
            recomputeHistogramIfNeeded(cur(), app.hist[0]);
            recomputeHistogramIfNeeded(cmpB(), app.hist[1],
                                       effBlack(*cur()), effWhite(*cur()));
            uint64_t held = app.hist[1].uid;
            gotoFrame(1); gotoFrame(1);            // two switches inside 300 ms
            bool armed = abStepBusy();
            ImageDoc* b6 = cmpB();
            // exactly what the panel does
            if (b6 && (!abStepBusy() || app.hist[1].uid == 0))
                recomputeHistogramIfNeeded(b6, app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
            bool heldStale = b6 && app.hist[1].uid == held && app.hist[1].uid != b6->uid;
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            bool released = !abStepBusy();
            if (b6 && (!abStepBusy() || app.hist[1].uid == 0))
                recomputeHistogramIfNeeded(b6, app.hist[1],
                                           effBlack(*cur()), effWhite(*cur()));
            fprintf(stderr, "abstatsselftest: P2b throttle: armed=%d held uid=%llu vs B "
                            "uid=%llu, released=%d, after release uid=%llu\n",
                    armed ? 1 : 0, (unsigned long long)held,
                    (unsigned long long)(b6 ? b6->uid : 0), released ? 1 : 0,
                    (unsigned long long)app.hist[1].uid);
            check(armed, "P2b rapid stepping arms the B throttle");
            check(heldStale, "P2b B slot holds its last result while stepping (stale)");
            check(released && b6 && app.hist[1].uid == b6->uid,
                  "P2b B refreshes once the stepping stops");
            selectImage(frA.front());
            app.abStepBusyUntil = 0;
        }

        // ---- P3: the A|B|delta|delta% table's inputs ----
        {
            selectImage(frA.front());
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            app.compareMode = App::CmpSplit;
            abStatsFrame();
            ImageDoc* b7 = cmpB();
            recomputeTemporalIfNeeded(cur(), app.temporal[0]);
            recomputeTemporalIfNeeded(b7, app.temporal[1]);
            AbTemporal TA = abTemporalOf(cur(), app.temporal[0], app.srvTemporal);
            AbTemporal TB = abTemporalOf(b7, app.temporal[1], app.srvTemporalB);
            double dAbs = TA.sigT - TB.sigT;
            double dPct = TA.sigT != 0 ? (TA.sigT - TB.sigT) / fabs(TA.sigT) * 100.0 : 0.0;
            fprintf(stderr, "abstatsselftest: P3 table inputs: A sigma_t=%.9g (n=%d/%d, "
                            "server=%d), B sigma_t=%.9g (n=%d/%d, server=%d), "
                            "delta A-B=%.9g, delta=%.6g %%, unit=[%s]\n",
                    TA.sigT, TA.frames, TA.expected, TA.fromServer ? 1 : 0,
                    TB.sigT, TB.frames, TB.expected, TB.fromServer ? 1 : 0,
                    dAbs, dPct, abValueUnit(cur()->dtype).c_str());
            check(TA.valid && TB.valid && TA.isStack && TB.isStack,
                  "P3 both sides resolve to stack temporal numbers");
            check(!TA.fromServer && !TB.fromServer,
                  "P3 local stacks come from the local computation");
            check(dAbs == TA.sigT - TB.sigT && !TA.fromServer,
                  "P3 delta is A - B (the difference image's sign)");
            check(abDeltaMeaningful(cur(), b7), "P3 equal channel counts allow a delta");
            // B's server aggregate must never have been fired on its own
            check(app.srvTemporalB.token == 0 && !app.srvTemporalB.pending &&
                  app.srvTemporalB.seqId == -1,
                  "P3 B's server temporal is never fired automatically");
        }
        {   // a B that is not a stack, and a B with a different channel count
            std::string root = g_abstatsSelftest;
            std::replace(root.begin(), root.end(), '\\', '/');
            while (!root.empty() && root.back() == '/') root.pop_back();
            size_t sl = root.find_last_of('/');
            root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
            size_t before = app.images.size();
            openPath(root + "/rgb_u8.npy");            // 3 channels, single frame
            loadAll();
            if (app.images.size() > before) {
                ImageDoc* lone = app.images.back().get();
                selectImage(frA.front());
                setCompareB(lone);
                abStatsFrame();
                ImageDoc* b8 = cmpB();
                recomputeTemporalIfNeeded(b8, app.temporal[1]);
                AbTemporal TB2 = abTemporalOf(b8, app.temporal[1], app.srvTemporalB);
                fprintf(stderr, "abstatsselftest: P3 lone B '%s' %dch: isStack=%d valid=%d "
                                "note='%s', delta meaningful=%d (A %dch)\n",
                        b8->name.c_str(), b8->ch, TB2.isStack ? 1 : 0, TB2.valid ? 1 : 0,
                        TB2.note, abDeltaMeaningful(cur(), b8) ? 1 : 0, cur()->ch);
                check(!TB2.isStack && !TB2.valid && std::string(TB2.note) == "not a stack",
                      "P3 a B that is not a stack says so instead of a number");
                check(cur()->ch != b8->ch && !abDeltaMeaningful(cur(), b8),
                      "P3 different channel counts suppress the delta columns");
                selectImage((int)app.images.size() - 1);  // the extra image ITSELF
                closeCurrent(true);                       // ...and only that one
                selectImage(frA.front());
                setCompareB(app.images[framesOfSeq(sidB).front()].get());
                abStatsFrame();
            } else {
                fprintf(stderr, "abstatsselftest: P3 lone-B checks skipped (%s/rgb_u8.npy "
                                "not there)\n", root.c_str());
            }
        }

        // ---- A6: compare off invalidates slot 1 once; closing B leaves nothing ----
        app.compareMode = App::CmpOff;
        abStatsFrame();
        fprintf(stderr, "abstatsselftest: CmpOff: hist[1].uid=%llu img=%p, "
                        "proj[1].uid=%llu img=%p, temporal[1].seqId=%d, slot1Live=%d\n",
                (unsigned long long)app.hist[1].uid, (const void*)app.hist[1].img,
                (unsigned long long)app.proj[1].uid, (const void*)app.proj[1].img,
                app.temporal[1].seqId, app.abSlot1Live ? 1 : 0);
        check(app.hist[1].uid == 0 && app.hist[1].img == nullptr &&
              app.proj[1].uid == 0 && app.proj[1].img == nullptr &&
              app.temporal[1].seqId == -1 && !app.abSlot1Live,
              "A6 CmpOff invalidates slot 1");
        {   // and once only: a second frame with compare off must not touch it
            app.hist[1].uid = 12345;              // white-box tripwire
            abStatsFrame();
            check(app.hist[1].uid == 12345, "A6 compare-off frames after the first do nothing");
            app.hist[1] = App::HistState{};
        }
        {   // B closed while slot 1 held it: no dangling ImageDoc* may survive
            app.compareMode = App::CmpSplit;
            setCompareB(app.images[framesOfSeq(sidB).front()].get());
            abStatsFrame();
            ImageDoc* b3 = cmpB();
            recomputeHistogramIfNeeded(b3, app.hist[1]);
            recomputeProjectionIfNeeded(b3, app.proj[1]);
            const void* held = app.hist[1].img;
            closeStack(sidB);
            fprintf(stderr, "abstatsselftest: closeStack(B=%d): slot 1 held %p, now "
                            "hist[1].img=%p uid=%llu, proj[1].img=%p, cmpB()=%p\n",
                    sidB, held, (const void*)app.hist[1].img,
                    (unsigned long long)app.hist[1].uid,
                    (const void*)app.proj[1].img, (const void*)cmpB());
            check(held != nullptr, "A6 fixture: slot 1 really held B");
            check(app.hist[1].img == nullptr && app.hist[1].uid == 0 &&
                  app.proj[1].img == nullptr && app.proj[1].uid == 0 && !cmpB(),
                  "A6 closing B leaves no dangling pointer in slot 1");
        }

        fprintf(stderr, "abstatsselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }

    if (!g_scanSelftest.empty()) {
        std::string dir = g_scanSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        startRemote("local://" + dir);
        double t0 = glfwGetTime();
        bool scanSent = false;
        while (glfwGetTime() - t0 < 120.0) {
            pumpRemoteBrowse();
            pumpRemoteFetch();
            pumpRemoteOpenQueue();
            pumpSequenceAndQueue();
            if (app.rbrowse.connected && !scanSent) {
                App::RbJob j; j.kind = App::RbScan;
                j.host = app.rbrowse.host; j.port = app.rbrowse.port; j.dir = dir;
                rbEnqueue(std::move(j));
                scanSent = true;
            }
            // headless "Load selected": accept the picker with everything checked,
            // through the SAME function the Load button calls
            if (app.folderPickOpen && app.folderPickRemote) pickerAccept();
            if (scanSent && !app.rbBusy && !app.seqs.empty() &&
                app.rbOpenQueue.empty() && app.rfPending == 0) break;
            if (!app.rbrowse.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (const auto& si : app.seqs)
            fprintf(stderr, "scanselftest: stack '%s' expected=%d\n",
                    si.name.c_str(), si.expectedFrames);
        bool ok = !app.seqs.empty() && app.rbrowse.err.empty();
        fprintf(stderr, "scanselftest: %d stack(s): %s\n", (int)app.seqs.size(),
                ok ? "ok" : ("FAILED " + app.rbrowse.err).c_str());
        stopRbWorker();
        stopSequenceLoader();
        // opening stacks spun up the fetch and measure workers too; an unjoined
        // std::thread at exit is std::terminate, i.e. a "passing" test that dies
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }

    // Every A/B range combination, printed. See scratchpad/rangetest.py.
    if (!g_rangeSelftest.empty()) {
        std::string dir = g_rangeSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        app.seqLoadMode = 1;
        openFolder(dir);
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 300.0) {
            if (app.folderPickOpen && !app.folderPickRemote) {
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen && app.seqs.size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (app.seqs.size() < 2) {
            fprintf(stderr, "rangeselftest: need 2 stacks under %s\n", dir.c_str());
            return 1;
        }
        std::vector<int> fa = framesOfSeq(app.seqs[0].id), fb = framesOfSeq(app.seqs[1].id);
        if (fa.size() < 2 || fb.size() < 2) {
            fprintf(stderr, "rangeselftest: stacks need 2+ frames\n");
            return 1;
        }
        selectImage(fa[0]);
        setCompareB(app.images[fb[0]].get());
        app.compareMode = App::CmpSplit;
        app.compareFollowFrame = true;
        static const char* MODE[3] = { "auto-per-frame", "per-stack", "linked" };
        static const char* SHARE[3] = { "each-own", "B-uses-A", "union-auto" };
        int bad = 0;
        {   // The two stacks of a capture series hold identically NAMED frames
            // (00/frame_000.npy and 01/frame_000.npy are both frame_000.npy),
            // so this is also the check that the compare UI can tell them apart
            // at all: every label there goes through abDocLabel.
            std::string la = abDocLabel(cur()), lb = abDocLabel(cmpB());
            fprintf(stderr, "rangeselftest: labels | A = %s | B = %s | files %s / %s | %s\n",
                    la.c_str(), lb.c_str(), cur()->name.c_str(), cmpB()->name.c_str(),
                    la != lb ? "DISTINCT" : "AMBIGUOUS");
            if (la == lb) bad++;
        }
        {   // The out-of-the-box display-range mode. "B uses A's range" renders
            // a B captured at another level at >white 99.7% - one dashed line
            // jammed against the right edge, and an empty half in side by side.
            // Union bins both sides identically and clips neither.
            fprintf(stderr, "rangeselftest: default compareRangeMode at startup = %d (%s)%s\n",
                    g_abRangeDefault,
                    g_abRangeDefault >= 0 && g_abRangeDefault < 3 ? SHARE[g_abRangeDefault] : "?",
                    g_abRangeDefault == 2 ? "" : "  <- EXPECTED 2 (union-auto)");
            if (g_abRangeDefault != 2) bad++;
        }
        {   // The axis label has to name the range ACTUALLY in force. It read
            // "A's black-white range" in every mode, including the union - a
            // claim about the picture that the picture does not support.
            int save = app.compareRangeMode;
            for (int sh = 0; sh < 3; sh++) {
                app.compareRangeMode = sh;
                std::string lab = abHistXLabel(cur(), cmpB());
                bool named = sh == 2 ? lab.find("A and B combined") != std::string::npos
                                     : lab.find("A\'s black-white") != std::string::npos;
                fprintf(stderr, "rangeselftest: hist x axis | %-11s | %s | %s\n",
                        SHARE[sh], lab.c_str(),
                        named ? "names the range in force" : "WRONG");
                if (!named) bad++;
            }
            app.compareRangeMode = save;
        }
        for (int m = 0; m < 3; m++) {
            app.rangeScope = m == 0 ? 0 : 1;
            app.linkRange = m == 2;
            if (app.linkRange) { app.linkBlack = app.images[fa[0]]->vmin;
                                 app.linkWhite = app.images[fa[0]]->vmax; }
            for (int sh = 0; sh < 3; sh++) {
                app.compareRangeMode = sh;
                for (int step = 0; step < 2; step++) {
                    selectImage(fa[step]);
                    ImageDoc* A = cur();
                    ImageDoc* B = cmpB();
                    if (!A || !B) { fprintf(stderr, "rangeselftest: no A/B\n"); return 1; }
                    bool same = effBlack(*A) == effBlack(*B) && effWhite(*A) == effWhite(*B);
                    fprintf(stderr,
                            "rangeselftest: %-14s %-11s frame %d | A %.6g..%.6g  B %.6g..%.6g  "
                            "| B frame %d | %s\n",
                            MODE[m], SHARE[sh], step, effBlack(*A), effWhite(*A),
                            effBlack(*B), effWhite(*B), B->seqIndex,
                            same ? "MATCHED" : "differs");
                    // the contract: anything but "each-own" must produce one range
                    if (sh != 0 && !same) bad++;
                    // follow-frame must keep the sides on the same frame number
                    if (B->seqIndex != A->seqIndex) {
                        fprintf(stderr, "rangeselftest: FOLLOW BROKEN A#%d vs B#%d\n",
                                A->seqIndex, B->seqIndex);
                        bad++;
                    }
                }
            }
        }
        fprintf(stderr, "rangeselftest: %s\n", bad ? "FAILED" : "ok");
        if (app.seqThread.joinable()) app.seqThread.join();
        return bad ? 1 : 0;
    }

    // The per-frame table, verifiable without a human: load the stack given on
    // the command line, run the exact clipboard path, print the TSV to stdout.
    // An independent numpy implementation must reproduce every number.
    if (g_fstatSelftest) {
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 600.0) {
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();   // headless accept
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (app.seqs.empty()) { fprintf(stderr, "fstatselftest: no stack loaded\n"); return 1; }
        copyPerFrameStats(app.seqs[0].id);
        const char* clip = ImGui::GetClipboardText();
        if (!clip || !*clip) { fprintf(stderr, "fstatselftest: empty clipboard\n"); return 1; }
        fputs(clip, stdout);
        if (app.seqThread.joinable()) app.seqThread.join();
        return 0;
    }

    // Linearity, verifiable without a human: load everything, fit, print the
    // numbers. A synthetic set with a known sensitivity and a known gain has to
    // come back out of this, or the panel is showing decoration.
    if (g_linSelftest) {
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 600.0) {       // wall clock: the loader is a thread
            // headless "Load selected": Open Folder ALWAYS shows the picker,
            // so a selftest accepts it the way the Load button would
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // The fit is per SERIES now, and the application never creates one by
        // itself (docs/terminology.md) - so this selftest creates it, in the
        // open, through selftestMakeSeries: one series per BATCH over that
        // batch's stacks, member values from extractLevelFromName (the same
        // proposal the Create modal shows a human), unit = the prefill
        // (prefs linunit, "lx" unless the user changed it). That is the ONLY
        // auto-creation in the program and it lives behind this flag.
        bool fitOk = false;
        std::vector<int> bids;
        for (const auto& b : app.batches) bids.push_back(b.id);
        for (int bid : bids) {
            int sid = selftestMakeSeries(bid, app.lin.unit);
            if (!sid) continue;
            linRecompute(sid);
            const App::LinState& L = app.lin;
            const App::Series* S = seriesById(sid);
            fprintf(stderr, "linselftest: series '%s' (batch '%s') unit '%s': %d member(s)\n",
                    S->name.c_str(), batchNameOf(bid).c_str(), S->unit,
                    (int)S->members.size());
            for (const auto& r : L.rows)
                fprintf(stderr, "linselftest: stack '%s' level=%.6g frames=%d planes=%d "
                                "mean=%.6g sigma_t=%.6g %s\n",
                        r.name.c_str(), r.level, r.frames, r.nPl, r.mean[0], r.sigmaT[0],
                        r.valid ? "" : ("ERR:" + r.err).c_str());
            for (int p = 0; p < L.nPl; p++)
                fprintf(stderr, "linselftest: plane %s sens=%.6g offs=%.6g r2=%.8f "
                                "LEmax=%.4f K=%.6g read=%.5g\n",
                        L.nPl > 1 ? LIN_PLANES[p] : "all", L.slope[p], L.offs[p], L.r2[p],
                        L.leMax[p], L.ptcK[p], L.readDN[p]);
            fprintf(stderr, "linselftest: %d points, fit %s\n", L.nPts,
                    L.fitValid ? "ok" : "FAILED");
            fitOk |= L.fitValid;
        }
        if (!fitOk) fprintf(stderr, "linselftest: no series could be fitted\n");
        if (app.seqThread.joinable()) app.seqThread.join();
        return fitOk ? 0 : 1;
    }

    // --browse-keys-selftest: connect the local peer and open the panel; the
    // action list is replayed inside the loop, below.
    std::vector<std::string> keyActs;
    size_t keyAct = 0;
    int keyPhase = 0;
    bool keysOk = false;
    if (!g_browseKeys.empty()) {
        std::string d = g_browseKeys;
        std::replace(d.begin(), d.end(), '\\', '/');
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        startRemote("local://" + d);
        app.showRemote = true;
        for (size_t i = 0, j; i <= g_browseKeysActs.size(); i = j + 1) {
            j = g_browseKeysActs.find(',', i);
            if (j == std::string::npos) j = g_browseKeysActs.size();
            if (j > i) keyActs.push_back(g_browseKeysActs.substr(i, j - i));
        }
        fprintf(stderr, "browsekeys: %d action(s) on %s\n", (int)keyActs.size(), d.c_str());
    }

    std::vector<double> benchMs;
    int benchLeft = benchFrames;
    if (benchFrames) {                 // exercise every panel, not just the defaults
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = true;
        app.showHistogram = app.showTemporal = app.showProjection = true;
        app.showLinearity = true;      // ...which now includes the series selector
        benchMs.reserve(benchFrames);
    }
    double lastFrameEnd = glfwGetTime();
    // The frame body lives in a callable so it can also run from the window
    // refresh/size callbacks. Win32 runs a MODAL message loop while the user
    // drags a window edge: DispatchMessage never returns until the drag ends, so
    // a main loop that only draws between events shows a frozen window for the
    // whole resize. GLFW 3.4 sets no timer there, so the repaint has to come from
    // inside the callback.
    g_drawFrame = [&]() {
        double frameBodyT0 = glfwGetTime();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // ImGui hands input out over several frames (ConfigInputTrickleEventQueue):
        // NewFrame stops mid-queue on a wheel-after-move or a second action on one
        // button and KEEPS the rest for later. An event-driven loop that sleeps
        // there replays stale input at the idle timeout instead - one wheel notch
        // every 250 ms, for seconds, after the user has stopped. Measured: a
        // 30-event burst took ~15 s to drain; with this line, ~7 ms.
        if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) wakeUi(1);

        // shortcuts
        pollFileDialog();
#if defined(__APPLE__)
        const ImGuiKeyChord MODK = ImGuiMod_Super;    // Cmd on macOS
#else
        const ImGuiKeyChord MODK = ImGuiMod_Ctrl;
#endif
        // modals (RAW dialog etc.) own the keyboard: no global shortcuts underneath —
        // Ctrl+W during reinterpret would shift the replaceIdx target image
        bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (!io.WantTextInput && !popupOpen) {
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Shift | ImGuiKey_O)) openFolderDialog();
            else if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_O)) openFileDialog();
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
            ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow;
            bool remoteFocused = nav && nav->RootWindow &&
                                 strcmp(nav->RootWindow->Name, "Browse###Remote") == 0;
            if (!remoteFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) gotoFrame(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) gotoFrame(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) gotoStack(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) gotoStack(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) gotoFrame(0, true, true);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) gotoFrame(0, true, false);
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
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) openFileDialog();
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
            // arrows: horizontal = time (frames), vertical = stacks
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) gotoFrame(1);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) gotoFrame(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) gotoStack(1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) gotoStack(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) gotoFrame(0, true, true);
            if (ImGui::IsKeyPressed(ImGuiKey_End, false)) gotoFrame(0, true, false);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && app.selectedAnn >= 0)
                deleteAnn(app.selectedAnn);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) app.selectedAnn = 0;   // back to All
            if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        }
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            (ImGui::IsKeyPressed(ImGuiKey_Backslash, false) || ImGui::IsKeyPressed(ImGuiKey_C, false)))
            swapCompare();                      // Shift+\ : flip which one is on top
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
            ImGui::DockBuilderDockWindow("Histogram", bottom);
            ImGui::DockBuilderDockWindow("Temporal", bottom);
            ImGui::DockBuilderDockWindow("Projection", bottom);
            ImGui::DockBuilderFinish(dockId);
            // ROIs and Analysis stay floating (they follow the work, not the frame)
            ImGui::SetWindowPos("ROIs", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                               vp->WorkPos.y + vp->WorkSize.y * 0.08f));
            ImGui::SetWindowSize("ROIs", ImVec2(620 * uiScale, 360 * uiScale));
            ImGui::SetWindowPos("Analysis", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                                   vp->WorkPos.y + vp->WorkSize.y * 0.42f));
            ImGui::SetWindowSize("Analysis", ImVec2(560 * uiScale, 420 * uiScale));
        }

        if (app.showFiles) { if (ImGui::Begin("Files", &app.showFiles)) drawFileList(); ImGui::End(); }
        if (app.showRemote) {
            if (app.focusRemote) { ImGui::SetNextWindowFocus(); app.focusRemote = false; }
            // Renamed "Browse" (it browses local folders too, via the local
            // peer), with the ImGui ID pinned by the ### suffix so the title
            // can change again without orphaning docked layouts.
            if (ImGui::Begin("Browse###Remote", &app.showRemote)) drawPanelRemote();
            ImGui::End();
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
            if (app.focusTemporal) { ImGui::SetNextWindowFocus(); app.focusTemporal = false; }
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
                char zs[32];
                if (app.view.zoom >= 0.095f) snprintf(zs, 32, "%.0f%%", app.view.zoom * 100);
                else                         snprintf(zs, 32, "%.3g%%", app.view.zoom * 100);
                snprintf(st, 512, "%s%s   %dx%d %dch %s  |  zoom %s%s",
                         im->name.c_str(), seqInfoStr, im->w, im->h, im->ch,
                         im->dtype.c_str(), zs, hover.c_str());
            }
            // Remote link indicator, VSCode-style: the leftmost thing on the
            // status bar, always present while a server is involved. A toast
            // expires; "am I connected?" must be answerable at any moment.
            {
                std::string phase;
                { std::lock_guard<std::mutex> lk(app.rbMtx); phase = app.rbPhase; }
                const App::RemoteBrowse& B = app.rbrowse;
                if (!phase.empty()) {
                    const char* spin = "|/-\\";
                    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.4f, 1), "%c %s",
                                       spin[(int)(ImGui::GetTime() * 8) & 3], phase.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel##rb")) {
                        std::lock_guard<std::mutex> lk(app.rbMtx);
                        app.rbQueue.clear();      // the in-flight ssh still finishes
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                } else if (B.connected) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s  %s",
                                       ICON_LINK, B.host.empty() ? "local peer" : B.host.c_str());
                    if (ImGui::IsItemHovered()) {
                        uint64_t rx = app.remoteSession ? app.remoteSession->bytesReceived() : 0;
                        ImGui::SetTooltip("connected  -  %s\npeer protocol v%d, %.1f MB received\n"
                                          "click to disconnect",
                                          B.dir.c_str(),
                                          app.remoteSession ? app.remoteSession->peerVersion() : 0,
                                          rx / 1048576.0);
                    }
                    if (ImGui::IsItemClicked()) {
                        std::lock_guard<std::mutex> lk(app.sesMtx);
                        if (app.remoteSession) app.remoteSession->stop();
                        app.rbrowse = App::RemoteBrowse{};
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
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s  %s", ICON_LINK, first.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n\n(click for details)", B.err.c_str());
                    if (ImGui::IsItemClicked()) app.showRemoteError = true;
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                }
            }
            ImGui::TextUnformatted(st);
            // compare is a persistent state: a toast that expires is not enough
            if (app.compareMode != App::CmpOff) {
                ImageDoc* b = cmpB();
                ImGui::SameLine();
                if (!b) {
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "   |  A/B: no B image");
                } else if (app.compareMode == App::CmpDiff) {
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "   |  %s +-%g  B: %s",
                                       app.diffAbs ? "|A-B|" : "A-B", app.diff.gain,
                                       abDocLabel(b).c_str());
                } else {
                    float fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "   |  A/B %s %.0f%%  B: %s",
                                       app.compareMode == App::CmpSplit ? "split" : "wipe",
                                       fr * 100, abDocLabel(b).c_str());
                }
                // swapping A and B is a thing you reach for constantly, so it needs
                // a visible control and not only a keyboard shortcut
                if (b) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("swap A/B")) swapCompare();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Put %s on the A side and %s on the B side"
                                          "   (Shift+\\ or Shift+C)",
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
            const bool remoteNow = viewingRemote();
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
        drawFolderPickModal();
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
        double swapT0 = glfwGetTime();
        glfwSwapBuffers(win);
        app.swapMs = (float)((glfwGetTime() - swapT0) * 1000.0);
        app.cpuMs = (float)((swapT0 - frameBodyT0) * 1000.0);
        if (g_lastInputAt != 0) {      // this frame answered an input: how late was it?
            float ms = (float)((glfwGetTime() - g_lastInputAt) * 1000.0);
            app.inputLagMs = ms;
            app.inputLagMaxMs = std::max(app.inputLagMaxMs, ms);
            g_lastInputAt = 0;
        }
    };

    while (!glfwWindowShouldClose(win)) {
        double frameT0 = glfwGetTime();
        // work that must keep animating even without input
        // rbBusy / mPending: a connect, a peer install or a server measurement is
        // in flight. Without these the idle path draws NOTHING while they run -
        // the window stops repainting and Windows paints it white and calls it
        // "not responding", which is exactly what a spinner is supposed to deny.
        bool busy = app.seqRunning || !app.seqQueue.empty() || app.rfPending > 0 ||
                    app.rbBusy || app.mPending > 0 ||
                    app.openDlg || app.saveDlg ||
                    app.folderDlg || (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) ||
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
                        glfwGetTime() < 600.0;
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
                     left = budget - (glfwGetTime() - lastFrameEnd);
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
            glfwWaitEventsTimeout(app.rbBusy || app.mPending > 0 ? 0.05 : 1.0);
            bool working = app.rbBusy || app.mPending > 0 || app.rfPending > 0 || app.seqRunning;
            // Results a worker has FINISHED but no frame has integrated yet, and
            // modals waiting for a frame to open them. Without these the remote
            // scan's picker never appeared: the worker went idle (working=false),
            // no input arrived, every frame was skipped - and the pumps live
            // inside the frame, so the result that would have opened the dialog
            // sat in rbDone until the user happened to move the mouse. Twice.
            if (!working) {
                { std::lock_guard<std::mutex> lk(app.rbMtx); working |= !app.rbDone.empty(); }
                { std::lock_guard<std::mutex> lk(app.rfMtx); working |= !app.rfDone.empty(); }
                { std::lock_guard<std::mutex> lk(app.mMtx);  working |= !app.mDone.empty(); }
                working |= seqReadyPending();
                working |= app.folderPickOpen || app.seqAskImage >= 0 || app.remoteDlgOpen;
                working |= app.seriesEdit.open;   // the create/edit modal wants a frame
                working |= !app.rbOpenQueue.empty() || !app.seqQueue.empty();
                working |= !app.seqRestore.empty();
                // a session's series wait for their stacks, then need one frame
                working |= !app.seriesRestore.empty() || !app.seqLevelLegacy.empty();
                working |= !app.seriesPending.empty();   // ...and so do the picker's
                working |= glfwGetTime() < app.abStepBusyUntil;   // B refresh pending
                // a tree node whose LIST is still out: its "(listing...)" row
                // has to become children without waiting for the mouse to move
                working |= !app.rbTreePending.empty();
            }
            // (--crash-test counts frames, so it must not be skipped)
            if (g_inputSeq == before && !typing && !working && !crashAfter) continue;
            app.wakeFrames = std::max(app.wakeFrames, 1);   // not wakeUi: no tail
        }
        lastFrameEnd = glfwGetTime();
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
                               : (d->path != a0->path || d->npzMember != a0->npzMember);
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
            double nowA = glfwGetTime();
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
            if (!benchFrames && due && !app.images.empty()) {
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
            static double reproT0 = glfwGetTime();
            static int reproIdle = 0;
            static bool reproReady = false;
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
                return ImGuiKey_None;
            };
            if (!reproReady) {
                if (app.rbrowse.connected && !app.rbrowse.entries.empty()) {
                    reproReady = true;
                    app.focusRemote = true;      // = clicking the panel
                    fprintf(stderr, "browsekeys: listing ready, %d entr(ies)\n",
                            (int)app.rbrowse.entries.size());
                } else if (glfwGetTime() - reproT0 > 60.0) {
                    fprintf(stderr, "browsekeys: no listing for %s (%s)\n",
                            g_browseKeys.c_str(), app.rbrowse.err.c_str());
                    break;
                }
            } else if (keyAct < keyActs.size()) {
                const std::string& a = keyActs[keyAct];
                if (keyPhase == 0) {
                    // every action names what the panel is showing when it runs,
                    // so a crash log ends on the action that caused it
                    fprintf(stderr, "browsekeys: %2d %-6s dir=%s rows=%d preview=%s\n",
                            (int)keyAct, a.c_str(), app.rbrowse.dir.c_str(),
                            (int)app.rbrowse.entries.size(),
                            app.previewLabel.empty() ? "-" : app.previewLabel.c_str());
                    fflush(stderr);
                    if (a == "more")       app.rbAdvanced = !app.rbAdvanced;
                    else if (a == "flat")  app.rbFlat = !app.rbFlat;
                    else if (a == "tree")  app.rbTree = !app.rbTree;
                    else if (a == "focus") app.focusRemote = true;
                    else if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), true);
                } else if (keyPhase == 1) {
                    if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), false);
                }
                if (++keyPhase >= 8) { keyPhase = 0; keyAct++; }
            } else if (++reproIdle > 20) {
                keysOk = true;
                fprintf(stderr, "browsekeys: %d action(s) through real frames, "
                                "no crash: ok\n", (int)keyActs.size());
                fflush(stderr);
                break;
            }
        }
        redrawNow();
        if (benchFrames && !benchWarm) {
            glFinish();               // include GPU work in the measurement
            benchMs.push_back((glfwGetTime() - frameT0) * 1000.0);
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

    // it captures main's locals by reference, and teardown can still fire callbacks
    g_drawFrame = nullptr;
    // a selftest must not leave its scripted clicks in the user's session or
    // their preferences (the frameless ones return before ever getting here)
    if (g_browseKeys.empty()) {
        autosaveSession();            // also covers a normal quit
        // Only what the user changed in this run: a one-off --sequence flag or
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
