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
    bool texNearest = true;
    // CFA (Bayer) metadata
    int batchId = 0;                  // which 塊 (Files header) this belongs to
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
    } hist;
    bool histLog = true;

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
        // linearity: the exposure level this stack was captured at (lux, ms,
        // photons - the unit lives in App::lin.unit). NaN = not set.
        double level = std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<SeqInfo> seqs;
    // A batch is the unit the Files panel groups by: created per OPEN ACTION
    // (not per folder - reopening the same folder makes a NEW batch), named
    // after the folder only as a starting value, renameable, session-saved.
    // It is the user's analysis grouping, deliberately decoupled from disk.
    struct Batch { int id; std::string name; };
    std::vector<Batch> batches;
    int nextBatchId = 1;
    int loadBatchId = 0;              // batch newly opened images join; 0 = derive
    int nextSeqId = 1;
    uint64_t nextUid = 1;
    uint64_t imagesRev = 1;           // bumped whenever the image list changes
    int seqLoadMode = 0;              // 0 = ask, 1 = always, 2 = never
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
    enum RbKind { RbConnect = 0, RbList = 1, RbUpdatePeer = 2, RbScan = 3, RbGlob = 4 };
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
    // Linearity: one row per stack (level in, response out), fits per CFA plane.
    // Recomputed only on demand - this walks hundreds of frames.
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
    int forceCfa = -1, forceCfaPattern = 0;   // --cfa: how 1ch files arrive
    bool showRemote = false;          // the server browser, its own panel
    bool focusRemote = false;         // bring it forward when a menu item asks
    struct Msg { std::string text; bool err; };
    std::vector<Msg> msgLog;          // every toast, kept so it can be copied
    bool showMessages = false, msgUnreadErr = false;
    struct ServerTemporal {
        uint64_t token = 0;           // matches the MJob that produced it
        int seqId = -1;
        bool valid = false, pending = false;
        std::string host, err;
        int frames = 0;
        double tempNoise = 0, fixedPattern = 0, totalNoise = 0, mean = 0;
        std::vector<float> idx, frameMean, frameStd;
    } srvTemporal;
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
    // queued stacks from "Open Folder" (loaded one after another)
    struct PendingGroup { std::string name; std::vector<std::string> files;
                          bool isRaw = false; int batchId = 0; };
    std::vector<PendingGroup> seqQueue;
    bool folderRecipeValid = false;   // raw recipe shared by the queue (see g_folderRecipe)
    // "which sequences do you want?" picker shown after scanning a folder
    struct FolderPick { PendingGroup g; bool selected = true; };
    std::vector<FolderPick> folderPick;
    bool folderPickOpen = false;
    std::string folderPickRoot;
    // remote variant: the same picker, but accepted groups go through
    // rbOpenQueue / openRemoteStack instead of the local sequence loader
    bool folderPickRemote = false;
    std::string folderPickHost;
    char pickInclude[128] = "";       // glob applied to "folder/pattern" (empty = all)
    char pickExclude[128] = "";
    std::unique_ptr<pfd::select_folder> folderDlg;
    // temporal analysis cache (built-in, follows the selected ROI)
    struct TemporalState {
        int seqId = -1;
        int frames = 0;
        int rx = -1, ry = -1, rw = -1, rh = -1;   // resolved ROI, not annRev
        std::vector<float> idx, frameMean, frameStd;
        double tempNoise = 0, fixedPattern = 0, totalNoise = 0;
        bool valid = false;
        bool roiUsed = false;
    } temporal;
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
    } proj;
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
static void openRemote(const std::string& url);
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name = std::string());
static std::string makeRemoteUrl(const std::string& host, const std::string& path);
static bool ensureRemoteSession(const std::string& host, std::string& err, int port = 0);
static void remoteBrowseTo(const std::string& dir);
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

// The B side of an A/B compare, or null when compare is off / B is gone / B is A.
static ImageDoc* resolveB() {
    if (app.compareBUid) {
        for (const auto& d : app.images)
            if (d->uid == app.compareBUid) return d.get() == cur() ? nullptr : d.get();
        app.compareBUid = 0;                     // B was closed
        return nullptr;
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

// remember B as an identity, not as a name
static void setCompareB(const ImageDoc* d) {
    app.compareBUid = d ? d->uid : 0;
    app.compareB = d ? d->name : std::string();
    app.compareBSeq = d && d->seqId != 0 ? d->seqIndex : -1;
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
    toast("B = " + app.compareB + "  (move A somewhere else)");
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
    std::string an = a->name, bn = b->name;
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
static float effBlack(const ImageDoc& im) { return app.linkRange ? app.linkBlack : im.black; }
static float effWhite(const ImageDoc& im) { return app.linkRange ? app.linkWhite : im.white; }

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
    if (app.hist.img == im) app.hist.img = nullptr;
    if (app.proj.img == im) app.proj.img = nullptr;
    app.temporal.seqId = -1;
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
        app.rfQueue.push_back(std::move(job));
    }
    if (!app.rfThread.joinable()) app.rfThread = std::thread(rfWorker);
}

static void requestFullRemote(const ImageDoc* d) {
    if (d->remoteUrl.empty() || d->remoteStep <= 1) return;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        for (const auto& j : app.rfQueue) if (j.uid == d->uid) return;   // queued already
    }
    App::RFetchJob j;
    j.url = d->remoteUrl; j.frame = d->remoteFrame; j.uid = d->uid;
    rfEnqueue(std::move(j));
}

// UI thread: swap arrived frames in. After this an ex-preview is a local image.
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
            // frames of one stack share the display range, like the local loader
            bool ranged = false;
            for (const auto& q : app.images)
                if (q->seqId == d.seqId) {
                    doc->black = q->black; doc->white = q->white;
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
        if (d.token != app.srvTemporal.token) continue;   // superseded request
        App::ServerTemporal& S = app.srvTemporal;
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

static void closeCurrent() {
    ImageDoc* im = cur();
    if (!im) return;
    forgetImage(im);
    if (im->tex) glDeleteTextures(1, &im->tex);
    app.images.erase(app.images.begin() + app.current);
    app.current = app.images.empty() ? -1 : std::min(app.current, (int)app.images.size() - 1);
    app.fitRequested = true;
}
static void closeAll() {
    app.ana.img = nullptr;
    app.hist.img = nullptr;
    app.proj.img = nullptr;
    app.temporal.seqId = -1;
    app.texLru.clear();
    app.imagesRev++;
    for (auto& d : app.images)
        if (d->tex) glDeleteTextures(1, &d->tex);
    app.images.clear();
    app.seqs.clear();
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

static void openRemote(const std::string& url);   // fwd: sessions can hold ssh:// images
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
static void writeSessionTo(std::ostream& f) {
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
    if (path.find('.') == std::string::npos) path += ".vsession";
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { if (!quiet) toast("cannot write session file", true); return; }
    writeSessionTo(f);
    if (!quiet) toast("session saved: " + baseName(path));
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
        else if (key == "seqlevel") {   // the exposure level of the stack above
            double lv = 0; ls >> lv;
            if (lastImageOk && cur() && cur()->seqId != 0)
                if (App::SeqInfo* si2 = seqInfo(cur()->seqId)) si2->level = lv;
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
                if (files.size() >= 2) startSequenceLoad(app.current, files, pat);
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

    // pick the digit field that varies (most distinct values); later field wins ties
    int best = -1;
    size_t bestCount = 0;
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
        if (vals.size() >= 2 && vals.size() >= bestCount) { bestCount = vals.size(); best = (int)k; }
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
        patternOut += ((int)k == best) ? std::string(segs[k].s.size(), '#') : segs[k].s;
    patternOut += ext;
    for (auto& f : found) out.push_back(f.second);
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
        app.temporal.seqId = -1;
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

// called once per frame: integrate decoded frames, then chain the next stack
static void pumpSequenceAndQueue() {
    pumpSequence();
    if (!app.seqRunning && !app.seqQueue.empty() && !seqReadyPending()) startNextQueuedGroup();
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
    if (cur() && app.images[idx].get() != cur()) app.prevImageUid = cur()->uid;
    app.current = idx;
    ImageDoc* d = app.images[idx].get();
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
    app.seqQueue = std::move(groups);
    int frames = 0;
    for (const auto& g : app.seqQueue) frames += (int)g.files.size();
    toast("opening " + std::to_string(app.seqQueue.size()) + " stack(s), " +
          std::to_string(frames) + " files");
    startNextQueuedGroup();
}

static void openFolder(const std::string& path) {
    std::vector<App::PendingGroup> groups = scanFolderGroups(path);
    if (groups.empty()) { toast("no loadable files under " + baseName(path), true); return; }
    if (groups.size() >= 256)
        toast("scan stopped at 256 sequences - narrow the folder or use the filters", true);
    // One group opens outright. Several ALWAYS ask - including under "Always
    // load folder", which used to bypass this. That setting is remembered from
    // a long-ago "Load sequence?" prompt, and silently swallowing the picker
    // because of it read as "the dialog stopped appearing" (verbatim, twice):
    // the include/exclude filters ARE the way a capture tree gets narrowed.
    // "Always load folder" keeps its original job - loading a single file's
    // numbered siblings without asking - and no longer mutes this dialog.
    {   // one fresh batch per Open Folder, named for the root (rename later)
        int b = newBatch(baseName(path));
        for (auto& g : groups) g.batchId = b;
    }
    if (groups.size() == 1) { enqueueGroups(std::move(groups)); return; }
    app.folderPick.clear();
    for (auto& g : groups) app.folderPick.push_back({ std::move(g), true });
    app.folderPickRoot = path;
    app.folderPickRemote = false;      // a stale remote flag would misroute these
    app.folderPickOpen = true;
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
static void applyPickFilters() {
    for (auto& e : app.folderPick) {
        bool inc = app.pickInclude[0] == 0 || globListMatch(app.pickInclude, e.g.name);
        bool exc = globListMatch(app.pickExclude, e.g.name);
        e.selected = inc && !exc;
    }
}

// Tree of what the scan found, with per-folder / per-sequence checkboxes.
static void drawFolderPickModal() {
    if (app.folderPickOpen && !ImGui::IsPopupOpen("Select sequences"))
        ImGui::OpenPopup("Select sequences");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * app.uiScale, 520 * app.uiScale), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 18),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("Select sequences", nullptr)) return;

    ImGui::TextDisabled("%s", app.folderPickRoot.c_str());
    int selGroups = 0, selFiles = 0, allFiles = 0;
    for (const auto& e : app.folderPick) {
        allFiles += (int)e.g.files.size();
        if (e.selected) { selGroups++; selFiles += (int)e.g.files.size(); }
    }
    ImGui::Text("%d sequence(s), %d files found", (int)app.folderPick.size(), allFiles);
    // glob filters beat clicking dozens of checkboxes.
    // Widths are measured, not guessed: the fields share the row, the buttons
    // get their own row so nothing is pushed off the right edge.
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        float labelW = ImGui::CalcTextSize("exclude").x + st.ItemInnerSpacing.x;
        float fieldW = (ImGui::GetContentRegionAvail().x - 2 * labelW - st.ItemSpacing.x) * 0.5f;
        fieldW = std::max(fieldW, ImGui::GetFontSize() * 6);
        ImGui::SetNextItemWidth(fieldW);
        bool ch = ImGui::InputTextWithHint("include", "* or 00/*,*_dark*", app.pickInclude,
                                           sizeof app.pickInclude);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("comma separated; * and ? wildcards; a bare word matches anywhere\n"
                              "matched against \"folder/pattern\", e.g. 01/frame_###.npy");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fieldW);
        ch |= ImGui::InputTextWithHint("exclude", "e.g. *_ng*,02/*", app.pickExclude,
                                       sizeof app.pickExclude);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("comma separated; * and ? wildcards; a bare word matches anywhere");
        if (ch) applyPickFilters();
    }
    if (ImGui::SmallButton("Apply filters")) applyPickFilters();
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) for (auto& e : app.folderPick) e.selected = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) for (auto& e : app.folderPick) e.selected = false;
    ImGui::SameLine();
    if (ImGui::SmallButton("Invert")) for (auto& e : app.folderPick) e.selected = !e.selected;
    ImGui::Separator();

    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("picktree", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    // group the flat list by the folder part of "folder/pattern"
    std::vector<std::string> folders;
    for (const auto& e : app.folderPick) {
        size_t s = e.g.name.find_last_of('/');
        std::string f = s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
        bool dup = false;
        for (const auto& x : folders) if (x == f) { dup = true; break; }
        if (!dup) folders.push_back(f);
    }
    for (const auto& f : folders) {
        ImGui::PushID(f.c_str());
        bool all = true, any = false;
        int files = 0;
        for (const auto& e : app.folderPick) {
            size_t s = e.g.name.find_last_of('/');
            std::string ef = s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
            if (ef != f) continue;
            files += (int)e.g.files.size();
            e.selected ? (any = true) : (all = false);
        }
        bool parent = all;
        if (ImGui::Checkbox("##folder", &parent)) {
            for (auto& e : app.folderPick) {
                size_t s = e.g.name.find_last_of('/');
                std::string ef = s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
                if (ef == f) e.selected = parent;
            }
        }
        ImGui::SameLine();
        if (!all && any) ImGui::TextDisabled("~");     // partial selection marker
        else ImGui::TextDisabled(" ");
        ImGui::SameLine();
        char hdr[320];
        snprintf(hdr, sizeof hdr, "%s   (%d files)", f.c_str(), files);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (auto& e : app.folderPick) {
                size_t s = e.g.name.find_last_of('/');
                std::string ef = s == std::string::npos ? std::string("(root)") : e.g.name.substr(0, s);
                if (ef != f) continue;
                std::string leaf = s == std::string::npos ? e.g.name : e.g.name.substr(s + 1);
                ImGui::PushID(&e);
                char lb[320];
                snprintf(lb, sizeof lb, "%s   %d file(s)%s", leaf.c_str(), (int)e.g.files.size(),
                         e.g.isRaw ? "  [raw]" : "");
                ImGui::Checkbox(lb, &e.selected);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.g.files.front().c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Text("selected: %d sequence(s), %d files", selGroups, selFiles);
    bool load = ImGui::Button("Load selected", ImVec2(150 * app.uiScale, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        app.folderPick.clear();
        app.folderPickOpen = false;
        ImGui::CloseCurrentPopup();
    }
    if (load) {
        std::vector<App::PendingGroup> sel;
        for (auto& e : app.folderPick) if (e.selected) sel.push_back(std::move(e.g));
        app.folderPick.clear();
        app.folderPickOpen = false;
        ImGui::CloseCurrentPopup();
        if (sel.empty()) {
            toast("nothing selected", true);
        } else if (app.folderPickRemote) {
            // remote groups: through the open queue, one stack at a time, so
            // the memory budget is applied against reality
            int frames = 0;
            for (const auto& g : sel) frames += (int)g.files.size();
            toast("opening " + std::to_string(sel.size()) + " remote stack(s), " +
                  std::to_string(frames) + " frames");
            for (auto& g : sel)
                app.rbOpenQueue.push_back({ app.folderPickHost, std::move(g.files), g.name, g.batchId });
        } else {
            enqueueGroups(std::move(sel));
        }
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
static void recomputeTemporalIfNeeded() {
    ImageDoc* im = cur();
    App::TemporalState& T = app.temporal;
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
static void maybeRequestServerTemporal(int seqId) {
    App::SeqInfo* si = nullptr;
    for (auto& s : app.seqs) if (s.id == seqId) { si = &s; break; }
    if (!si || !serverComputes(*si)) return;
    App::ServerTemporal& S = app.srvTemporal;
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
            std::string broot = r.dir;
            while (broot.size() > 1 && broot.back() == '/') broot.pop_back();
            size_t bsl = broot.find_last_of('/');
            int scanBatch = newBatch(bsl == std::string::npos || bsl + 1 >= broot.size()
                                     ? broot : broot.substr(bsl + 1));
            for (const auto& g : r.scanGroups) {
                App::PendingGroup pg;
                pg.name = g.dir.empty() ? g.entry.name : g.dir + "/" + g.entry.name;
                std::string base = joinR(r.dir, g.dir);
                for (const auto& m : g.entry.members) pg.files.push_back(joinR(base, m));
                pg.batchId = scanBatch;
                groups.push_back(std::move(pg));
            }
            if (r.truncated)
                toast("scan stopped at 256 stacks - open a narrower folder", true);
            if (r.skippedDirs)
                toast(std::to_string(r.skippedDirs) +
                      " unreadable folder(s) skipped in the scan", true);
            if (groups.empty()) { toast("no .npy stacks under " + r.dir, true); continue; }
            // One group opens outright; several ALWAYS go through the picker,
            // even under "Always load folder". Unlike the local scan, every
            // frame here is a transfer, and the include/exclude filters in
            // that modal are how a big capture tree gets narrowed to the
            // levels actually wanted (asked for explicitly).
            if (groups.size() == 1) {
                int frames = 0;
                for (const auto& g : groups) frames += (int)g.files.size();
                toast("opening " + std::to_string(groups.size()) + " remote stack(s), " +
                      std::to_string(frames) + " frames");
                for (auto& g : groups)
                    app.rbOpenQueue.push_back({ r.host, std::move(g.files), g.name, g.batchId });
            } else {
                app.folderPick.clear();
                for (auto& g : groups) app.folderPick.push_back({ std::move(g), true });
                app.folderPickRoot = makeRemoteUrl(r.host, r.dir);
                app.folderPickRemote = true;
                app.folderPickHost = r.host;
                app.folderPickOpen = true;
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

static void openRemote(const std::string& url) {
    std::string host, rpath;
    if (!remote::parseUrl(url, host, rpath)) {
        toast("expected ssh://user@host/path/to/file.npy", true);
        return;
    }
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
    computeMinMax(*doc);
    defaultRange(*doc);
    doc->texDirty = true;
    app.images.push_back(std::move(doc));
    app.imagesRev++;
    selectImage((int)app.images.size() - 1);
    app.fitRequested = true;
    // the preview is for orientation; the pixels you can measure come right after
    if (step > 1) requestFullRemote(app.images.back().get());
    // A frame axis makes this a stack: prefetch the rest in the background, and
    // they become ordinary local frames as they land - temporal analysis, frame
    // stepping, every analyzer, all unchanged. Processing stays local by design;
    // the remote side only ever ships pixels.
    if (m.frames > 1) {
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
    app.folderDlg = std::make_unique<pfd::select_folder>("Open folder (all sequences below it)");
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
        if (!p.empty()) openFolder(p);
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

static void recomputeHistogramIfNeeded(ImageDoc* im) {
    App::HistState& H = app.hist;
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
    if (H.uid == im->uid && H.dataRev == im->dataRev && H.black == effBlack(*im) &&
        H.white == effWhite(*im) && H.cfa == im->cfa && H.cfaPattern == im->cfaPattern &&
        H.rx == rx && H.ry == ry && H.rw == rw && H.rh == rh)
        return;
    if (app.annBusy && H.uid == im->uid) return;   // mid-drag: keep the last result
    H.img = im; H.uid = im->uid; H.dataRev = im->dataRev;
    H.black = effBlack(*im); H.white = effWhite(*im);
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
            for (float x = pr.p0.x; x < pr.p1.x; x += dash * 2)
                dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + dash, pr.p1.x), y), rc);
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
            ImGui::TextDisabled("B: %s", b->name.c_str());
            if (b->w != im->w || b->h != im->h)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "size differs: B is %dx%d",
                                   b->w, b->h);
            // Two images auto-ranged to their own min/max look different even when
            // the pixels are identical - the fastest way to a wrong conclusion.
            if (!app.linkRange && (effBlack(*b) != effBlack(*im) || effWhite(*b) != effWhite(*im))) {
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "display range differs");
                ImGui::TextDisabled("A %s-%s / B %s-%s", fmtVal(effBlack(*im), im->dtype).c_str(),
                                    fmtVal(effWhite(*im), im->dtype).c_str(),
                                    fmtVal(effBlack(*b), b->dtype).c_str(),
                                    fmtVal(effWhite(*b), b->dtype).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("match B to A")) {
                    b->black = effBlack(*im); b->white = effWhite(*im); b->texDirty = true;
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
        {   // link: one range for every open image, so frames/files stay comparable
            bool wasLinked = app.linkRange;
            if (ImGui::Checkbox("link across all images", &app.linkRange)) {
                if (app.linkRange && !wasLinked) {      // seed from what is on screen
                    app.linkBlack = im->black; app.linkWhite = im->white;
                }
                markAllTexDirty();                      // unlink -> each image returns to its own
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("shared range for display only - each image keeps its own,\n"
                                  "unlink to get them back");
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

static void drawPanelHistogram() {
    ImageDoc* im = cur();
    if (im && im->w > 0 && im->h > 0) {
        recomputeHistogramIfNeeded(im);
        const App::HistState& H = app.hist;
        ImGui::Text("Statistics");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", H.roiUsed ? "selected ROI" : "whole image");
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
        bool cfaHist = im->ch == 1 && im->cfa != 0;
        double logMax = log1p((double)H.maxBin);
        char yl[64];
        snprintf(yl, sizeof yl, app.histLog ? "pixel count (log, max %u)" : "pixel count (max %u)",
                 H.maxBin);
        char xl[80];
        snprintf(xl, sizeof xl, "pixel value (%s, black-white range)", im->dtype.c_str());
        // fill the rest of the panel: a fixed height overflowed the bottom dock
        float hAvail = ImGui::GetContentRegionAvail().y
                     - (ImGui::GetFontSize() * 3 + 12 * app.uiScale)   // axes + footer
                     - ImGui::GetTextLineHeightWithSpacing();
        PlotRect hp = beginPlot(xl, yl, effBlack(*im), effWhite(*im), 0.0f, 1.0f, false, false,
                                std::max(hAvail, 70.0f * app.uiScale));
        if (hp.ok) {
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            hdl->PushClipRect(hp.p0, hp.p1, true);
            float pw2 = hp.p1.x - hp.p0.x;
            for (int s = 0; s < H.nSeries; s++) {
                ImU32 col = cfaHist ? CFA_COLS[s]
                                    : (H.nSeries == 1 ? IM_COL32(200, 205, 210, 200) : RGB_COLS[s]);
                for (int b = 0; b < 256; b++) {
                    uint32_t v = H.bins[s][b];
                    if (!v) continue;
                    float f = app.histLog ? (float)(log1p((double)v) / logMax)
                                          : (float)v / (float)H.maxBin;
                    float bx0 = hp.p0.x + (float)b / 256.0f * pw2;
                    hdl->AddRectFilled(ImVec2(bx0, hp.p1.y - f * (hp.p1.y - hp.p0.y)),
                                       ImVec2(bx0 + pw2 / 256.0f + 0.5f, hp.p1.y), col);
                }
            }
            hdl->PopClipRect();
        }
        ImGui::TextDisabled("%zu px | <black %.2f%%  >white %.2f%%%s", H.sampled,
                            H.clipLo * 100.0, H.clipHi * 100.0,
                            cfaHist ? " | R/Gr/Gb/B" : "");
    }

}

// H/V projections: column means (horizontal profile) and row means (vertical
// profile) over the selected ROI. Column FPN, banding and shading show up here
// far more clearly than in the image itself.
static void recomputeProjectionIfNeeded(ImageDoc* im) {
    App::ProjState& P = app.proj;
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
    recomputeProjectionIfNeeded(im);
    const App::ProjState& P = app.proj;
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %dx%d", P.roiUsed ? "ROI" : "whole image", P.rw, P.rh);

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
    else { yLo = std::min(P.hMin, P.vMin); yHi = std::max(P.hMax, P.vMax); }   // shared H/V
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
    bool cfa = im->ch == 1 && im->cfa != 0;
    int plots = (app.showProjH ? 1 : 0) + (app.showProjV ? 1 : 0);
    if (!plots) { ImGui::TextDisabled("enable H or V"); return; }
    // reserve the statistics table first: the plots must not push it off-panel
    int statRows = P.nSeries * plots;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float statsH = ImGui::GetFrameHeightWithSpacing()      // "profile statistics" separator
                 + lineH * (statRows + 1)                  // header + one row per axis/channel
                 + lineH;                                  // footnote
    float avail = ImGui::GetContentRegionAvail().y - statsH;
    float each = std::max((avail - lineH * plots) / plots
                          - (ImGui::GetFontSize() * 3 + 12 * app.uiScale), 60.0f * app.uiScale);

    auto plotSeries = [&](bool horizontal) {
        const std::vector<float>* series = horizontal ? P.h : P.v;
        float lo = yLo, hi = yHi;      // one value axis for H, V and every image
        int n = horizontal ? P.rw : P.rh;
        int origin = horizontal ? P.rx : P.ry;
        char yl[80];
        snprintf(yl, sizeof yl, "%s value (%s)", modes[std::clamp(app.projMode, 0, 2)],
                 im->dtype.c_str());
        PlotRect pr = beginPlot(horizontal ? "column x (px)" : "row y (px)", yl,
                                (float)origin, (float)(origin + std::max(n - 1, 1)),
                                lo, hi, true, false, each);
        if (!pr.ok) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pr.p0, pr.p1, true);
        for (int s = 0; s < P.nSeries; s++) {
            ImU32 col = cfa ? CFA_COLS[s]
                            : (P.nSeries == 1 ? IM_COL32(215, 220, 225, 230) : RGB_COLS[s]);
            const std::vector<float>& d = series[s];
            // decimate to the plot's pixel width: 4000 samples into 400 px was
            // 4000 line segments per series per frame
            int px = std::max(1, (int)(pr.p1.x - pr.p0.x));
            int stride = std::max(1, (int)d.size() / px);
            ImVec2 prev(0, 0); bool has = false;
            for (int i = 0; i < (int)d.size(); i += stride) {
                float lo = FLT_MAX, hi = -FLT_MAX;
                for (int k = i; k < std::min(i + stride, (int)d.size()); k++)
                    if (std::isfinite(d[k])) { lo = std::min(lo, d[k]); hi = std::max(hi, d[k]); }
                if (lo > hi) { has = false; continue; }
                ImVec2 a = pr.at((float)(origin + i), lo), b = pr.at((float)(origin + i), hi);
                if (stride > 1 && b.y != a.y) dl->AddLine(a, b, col, 1.2f);   // min-max bar
                ImVec2 pt = pr.at((float)(origin + i), (lo + hi) * 0.5f);
                if (has) dl->AddLine(prev, pt, col, 1.2f);
                prev = pt; has = true;
            }
        }
        // "There is a spike - WHICH column is it?" The readout answers with the
        // exact index and the values under the cursor, without decimation: the
        // marker snaps to the true sample, not to the plotted min-max bucket.
        if (ImGui::IsMouseHoveringRect(pr.p0, pr.p1)) {
            float mx = ImGui::GetMousePos().x;
            float t = (mx - pr.p0.x) / std::max(pr.p1.x - pr.p0.x, 1.0f);
            int i = std::clamp((int)(t * (float)(n - 1) + 0.5f), 0, std::max(n - 1, 0));
            dl->AddLine(pr.at((float)(origin + i), lo), pr.at((float)(origin + i), hi),
                        IM_COL32(230, 200, 90, 140), 1.0f);
            char tip[256];
            int off = snprintf(tip, sizeof tip, "%s %d", horizontal ? "column x" : "row y",
                               origin + i);
            static const char* CFA_N[4] = { "R", "Gr", "Gb", "B" };
            static const char* RGB_N[4] = { "R", "G", "B", "A" };
            for (int s2 = 0; s2 < P.nSeries; s2++) {
                if (i >= (int)series[s2].size() || !std::isfinite(series[s2][i])) continue;
                const char* nm = P.nSeries == 1 ? "" : (cfa ? CFA_N[s2] : RGB_N[s2]);
                off += snprintf(tip + off, sizeof tip - off, "\n%s%s%.6g %s",
                                nm, *nm ? ": " : "", series[s2][i], im->dtype.c_str());
                dl->AddCircleFilled(pr.at((float)(origin + i), series[s2][i]), 3.0f,
                                    IM_COL32(230, 200, 90, 230));
            }
            ImGui::SetTooltip("%s", tip);
        }
        dl->PopClipRect();
    };
    if (app.showProjH) plotSeries(true);
    if (app.showProjV) plotSeries(false);

    // numbers to go with the curves
    ImGui::SeparatorText("profile statistics");
    int nCols = 2 + 4;
    if (ImGui::BeginTable("projstats", nCols, ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn("ch", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.4f);
        ImGui::TableSetupColumn("mean", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("sigma", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("sigma %", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("p-p", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        auto rows = [&](bool horizontal) {
            for (int s = 0; s < P.nSeries; s++) {
                const App::ProjState::Stats& st = horizontal ? P.hStat[s] : P.vStat[s];
                if (!st.valid) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled(horizontal ? "H (col)" : "V (row)");
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", P.seriesNames[s]);
                ImGui::TableNextColumn(); textNum("%.6g", st.mean);
                ImGui::TableNextColumn(); textNum("%.6g", st.sd);
                ImGui::TableNextColumn(); textNum("%.3f", st.pct);
                ImGui::TableNextColumn(); textNum("%.6g", st.pp);
            }
        };
        if (app.showProjH) rows(true);
        if (app.showProjV) rows(false);
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
// name. Deliberately simple - the field is editable, this is only the default.
static double extractLevelFromName(const std::string& name) {
    std::string part = name;
    size_t slash = part.find_last_of('/');
    if (slash != std::string::npos && slash > 0) part = part.substr(0, slash);
    for (size_t i = 0; i < part.size(); i++) {
        if (isdigit((unsigned char)part[i])) {
            // avoid the "###" pattern placeholder and sizes like 640x480
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
static void linRecompute() {
    App::LinState& L = app.lin;
    // the ROI is shared with everything else: the selected rect, or whole frame
    int rx = 0, ry = 0, rw = 0, rh = 0;
    L.roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn))
        if (a->type == 0) { rx = a->x; ry = a->y; rw = a->w; rh = a->h; L.roiUsed = true; }
    std::vector<std::pair<int, bool>> keepInc;
    for (const auto& r : L.rows) keepInc.push_back({ r.seqId, r.include });
    L.rows.clear();
    for (const auto& si : app.seqs) {
        std::vector<int> fr = framesOfSeq(si.id);
        if (fr.size() < 2 && si.expectedFrames < 2) continue;
        App::LinState::Row r;
        r.seqId = si.id;
        r.name = si.name;
        r.level = si.level;
        StackStats st = computeStackStats(si.id, rx, ry, rw, rh);
        r.valid = st.valid;
        r.err = st.err;
        r.frames = st.frames;
        r.nPl = st.nPl;
        for (int p = 0; p < 4; p++) { r.mean[p] = st.mean[p]; r.sigmaT[p] = st.sigmaT[p]; }
        for (const auto& k : keepInc) if (k.first == si.id) r.include = k.second;
        L.rows.push_back(std::move(r));
    }
    // fits, per plane, over rows that have both a level and a measurement
    L.fitValid = false;
    L.nPl = 1;
    for (const auto& r : L.rows) if (r.valid) L.nPl = std::max(L.nPl, r.nPl);
    int fitted = 0;
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

static void drawPanelLinearity() {
    App::LinState& L = app.lin;
    int nStacks = 0;
    for (const auto& si : app.seqs)
        if (framesOfSeq(si.id).size() >= 2 || si.expectedFrames >= 2) nStacks++;
    if (nStacks < 2) {
        ImGui::TextDisabled("linearity needs several stacks, one per exposure level");
        ImGui::TextDisabled("(open a folder of folders: each subfolder = one level)");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    ImGui::InputText("level unit", L.unit, sizeof L.unit);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("lx, ms, lx*s, photons/px ... whatever the x axis of\n"
                          "your experiment is. Written on every axis and column.");
    ImGui::SameLine();
    if (ImGui::Button("Auto levels")) {
        for (auto& si : app.seqs)
            if (!std::isfinite(si.level)) si.level = extractLevelFromName(si.name);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("read the level from each stack's folder name\n(first number; only fills empty fields)");
    ImGui::SameLine();
    if (ImGui::Button("Compute")) linRecompute();
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

    // one row per stack: level in, response out
    const ImGuiTableFlags TF = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    char lvlHdr[48], meanHdr[32], sigHdr[32];
    snprintf(lvlHdr, sizeof lvlHdr, "level [%s]", L.unit);
    const char* pl = L.nPl > 1 ? LIN_PLANES[std::clamp(L.tablePlane, 0, 3)] : "";
    snprintf(meanHdr, sizeof meanHdr, "mean %s [DN]", pl);
    snprintf(sigHdr, sizeof sigHdr, "sigma_t %s [DN]", pl);
    float tableH = ImGui::GetFontSize() * std::min(10, nStacks + 2) * 1.6f;
    if (ImGui::BeginTable("lintab", 5 + (L.fitValid && L.nPl > 1 ? 2 : 2), TF, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("stack", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11);
        ImGui::TableSetupColumn(lvlHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.2f);
        ImGui::TableSetupColumn(meanHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(sigHdr, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2);
        ImGui::TableHeadersRow();
        for (auto& si : app.seqs) {
            if (framesOfSeq(si.id).size() < 2 && si.expectedFrames < 2) continue;
            ImGui::PushID(si.id);
            const App::LinState::Row* row = nullptr;
            for (const auto& r : L.rows) if (r.seqId == si.id) { row = &r; break; }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool inc = true;
            for (auto& r : L.rows) if (r.seqId == si.id) inc = r.include;
            if (ImGui::Checkbox("##inc", &inc))
                for (auto& r : L.rows) if (r.seqId == si.id) r.include = inc;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(si.name.c_str());
            ImGui::TableNextColumn();
            double lv = std::isfinite(si.level) ? si.level : 0.0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##lv", &lv, 0, 0, "%.6g")) si.level = lv;
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

    if (!L.fitValid) {
        ImGui::TextDisabled("set levels (Auto levels or type them), then Compute.");
        ImGui::TextDisabled("3+ stacks with levels give a fit; CFA planes stay separate.");
        return;
    }

    // ---- the answers ----
    ImGui::SeparatorText("fit  (response = sensitivity * level + offset)");
    if (ImGui::BeginTable("linfit", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        char sensHdr[64];
        snprintf(sensHdr, sizeof sensHdr, "sensitivity [DN/%s]", L.unit);
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
    char xl[64];
    snprintf(xl, sizeof xl, "level [%s]", L.unit);
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

static void drawPanelTemporal() {
    ImageDoc* im = cur();
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
        recomputeTemporalIfNeeded();
        const App::TemporalState& T = app.temporal;
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
            snprintf(yl, sizeof yl, "ROI mean value (%s)", im->dtype.c_str());
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

// Bookmarks + recents in one dropdown. Available connected or not: picking a
// place while disconnected is exactly how a session starts next morning.
static void drawRemotePlacesCombo() {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##places", "places  (bookmarks + recent)",
                           ImGuiComboFlags_HeightLarge))
        return;
    int rm = -1;
    if (!app.rbBookmarks.empty()) ImGui::TextDisabled("bookmarks");
    for (int i = 0; i < (int)app.rbBookmarks.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) rm = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("remove this bookmark");
        ImGui::SameLine();
        if (ImGui::Selectable(app.rbBookmarks[i].c_str())) goToPlace(app.rbBookmarks[i]);
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
        if (ImGui::Selectable(app.rbRecents[i].c_str())) goToPlace(app.rbRecents[i]);
        ImGui::PopID();
    }
    if (app.rbBookmarks.empty() && app.rbRecents.empty())
        ImGui::TextDisabled("nothing yet - the * button bookmarks the open folder");
    ImGui::EndCombo();
}

static void drawPanelRemote() {
    App::RemoteBrowse& B = app.rbrowse;
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
    // Where we are, and how to leave: host row first, path row under it.
    ImGui::TextUnformatted(B.host.empty() ? "local peer" : B.host.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("disconnect")) { B = App::RemoteBrowse{}; ImGui::PopID(); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton("home")) remoteBrowseTo("~");
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh")) remoteBrowseTo(B.dir);
    if (app.rbBusy) { ImGui::SameLine(); ImGui::TextDisabled("(listing...)"); }
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
    // ---- path bar: breadcrumbs, or one text field while editing ----
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
            if (ImGui::SmallButton(segs[k].label.c_str())) remoteBrowseTo(segs[k].path);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) editReq = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(right-click to edit the path)", segs[k].path.c_str());
            ImGui::PopID();
        }
        if (!segs.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("edit##path")) editReq = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("type or paste a path (right-clicking the bar works too)");
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
            remoteBrowseTo(p.empty() ? "~" : p);
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
    // full remote path of a listed name
    auto joined = [&B](const std::string& n) {
        return B.dir == "/" ? "/" + n : B.dir + "/" + n;
    };
    // Multi-select (Ctrl / Shift + click on file rows). Selection is keyed to
    // the exact listing: a navigation or a refresh invalidates the indices, so
    // it resets rather than pointing at different rows.
    static std::vector<char> rbSel;
    static int rbSelAnchor = -1;               // entry index of the last click
    {
        static std::string selSig;
        std::string sig = B.host + "|" + B.dir + "|" + std::to_string(B.entries.size());
        if (sig != selSig) {
            selSig = sig;
            rbSel.assign(B.entries.size(), 0);
            rbSelAnchor = -1;
        }
    }
    {   // "Open N selected as stack" - enabled only when the v3 metadata proves
        // the frames can actually stack, BEFORE any pixel is transferred
        int nSel = 0;
        const remote::Entry* first = nullptr;
        std::string reason;
        for (size_t i = 0; i < B.entries.size() && i < rbSel.size(); i++) {
            if (!rbSel[i]) continue;
            const remote::Entry& e = B.entries[i];
            nSel++;
            if (!isNpyName(e.name)) { reason = "only .npy files can form a stack"; continue; }
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
                for (size_t i = 0; i < B.entries.size() && i < rbSel.size(); i++) {
                    if (!rbSel[i]) continue;
                    const remote::Entry& e = B.entries[i];
                    if (e.group) for (const auto& m : e.members) files.push_back(joined(m));
                    else files.push_back(joined(e.name));
                }
                sortFramesNumerically(files);
                openRemoteStack(B.host, files);
                rbSel.assign(B.entries.size(), 0);
            }
            if (!reason.empty()) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", reason.empty()
                    ? "frames stack in numeric name order" : reason.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("clear##sel")) rbSel.assign(B.entries.size(), 0);
        }
    }
    // Filter what is already listed - no round-trip to the server. Substring by
    // default, glob when * or ? appears (globListMatch's contract), because a
    // capture dump directory holds hundreds of entries and one condition matters.
    static char rbFilter[256] = "";
    {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7);
        ImGui::InputTextWithHint("##rbfilter", "filter (Ctrl+F), * ? glob",
                                 rbFilter, sizeof rbFilter);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("filters the listing below without asking the server\n"
                              "bare text matches anywhere; * and ? make it a glob;\n"
                              "comma separates alternatives");
    }
    // filtered view of B.entries, by index (the clipper needs random access)
    std::vector<int> shown;
    shown.reserve(B.entries.size());
    for (int i = 0; i < (int)B.entries.size(); i++)
        if (!rbFilter[0] || globListMatch(rbFilter, B.entries[i].name))
            shown.push_back(i);
    if (rbFilter[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d of %d", (int)shown.size(), (int)B.entries.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("entries shown of entries listed");
    }
    // Server-side search - a different thing from the filter above (which only
    // narrows what is already listed), so it gets its own labelled row.
    static char rbSearchBuf[256] = "";
    static bool rbSearchFocus = false;
    static std::string rbSearchRoot;   // set by "Search under here"; empty = this folder
    {
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
                        remoteBrowseTo(full);
                    } else if (isNpyName(h.rel)) {
                        openRemote(makeRemoteUrl(B.host, full));
                    } else {
                        // not servable: at least go where it lives
                        size_t sl = full.find_last_of('/');
                        remoteBrowseTo(sl == std::string::npos || sl == 0 ? "/"
                                                                          : full.substr(0, sl));
                    }
                }
                if (ImGui::BeginPopupContextItem("sctx")) {
                    std::string full = joinS(h.rel);
                    if (!h.dir && ImGui::MenuItem("Go to containing folder")) {
                        size_t sl = full.find_last_of('/');
                        remoteBrowseTo(sl == std::string::npos || sl == 0 ? "/"
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
    if (B.dir != "~" && B.dir != "/" && ImGui::Selectable("[..]")) {
        std::string d = B.dir;
        size_t s = d.find_last_of('/');
        remoteBrowseTo(s == std::string::npos || s == 0 ? (d[0] == '~' ? "~" : "/")
                                                        : d.substr(0, s));
    }
    // The listing scrolls on its own so the header above never leaves the view.
    // Properties target: a snapshot, because the row may scroll out of the
    // clipper (or the listing may refresh) while the popup is up.
    static remote::Entry rbPropsEntry;
    static std::string rbPropsPath;
    static bool rbPropsOpen = false;
    enum { RB_COL_NAME = 0, RB_COL_SHAPE, RB_COL_SIZE, RB_COL_MTIME };
    if (ImGui::BeginTable("rblist", 4, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_SortTristate)) {
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
        // `shown` is rebuilt every frame, so sorting it here honors a changed
        // sort spec, a fresh listing and the filter in one place. Directories
        // sort before files no matter the key - this is a browser, not a table
        // of numbers.
        if (const ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
            std::stable_sort(shown.begin(), shown.end(), [&](int ia, int ib) {
                const remote::Entry& a = B.entries[ia];
                const remote::Entry& b = B.entries[ib];
                if (a.dir != b.dir) return a.dir;
                for (int s = 0; s < sp->SpecsCount; s++) {
                    const ImGuiTableColumnSortSpecs& c = sp->Specs[s];
                    int cmp = 0;
                    switch (c.ColumnUserID) {
                        case RB_COL_SIZE:  cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                        case RB_COL_MTIME: cmp = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
                        default:           cmp = a.name.compare(b.name); break;
                    }
                    if (cmp) return c.SortDirection == ImGuiSortDirection_Descending ? cmp > 0
                                                                                     : cmp < 0;
                }
                return false;
            });
        }
        ImGuiListClipper clip;
        clip.Begin((int)shown.size());
        while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
            const remote::Entry& e = B.entries[shown[row]];
            ImGui::PushID(shown[row]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // vscode-quiet rows: a dim chevron marks a folder, a stack gets
            // three hairlines, a file gets nothing - the name is the row.
            // (First cut had drawn folder/page pictograms; they collided with
            // the text and were, verbatim, "くどい".)
            std::string lb = "  " + e.name;
            if (e.group) lb += "   [" + std::to_string(e.frames) + " frames]";
            bool servable = e.dir || isNpyName(e.name);
            if (!servable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            int ei = shown[row];
            bool isSel = ei < (int)rbSel.size() && rbSel[ei] != 0;
            bool rowClicked = ImGui::Selectable(lb.c_str(), isSel, ImGuiSelectableFlags_SpanAllColumns);
            if (e.dir || e.group) {   // inside the two-space gutter the label reserves
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetItemRectMin();
                float h = ImGui::GetTextLineHeight();
                float gut = ImGui::CalcTextSize("  ").x;      // never touch the name
                float y = p.y + (ImGui::GetItemRectSize().y - h) * 0.5f;
                float cxm = p.x + gut * 0.45f, cym = y + h * 0.5f;
                if (e.dir) {          // › chevron, the way a tree hints "enter me"
                    ImU32 c = IM_COL32(150, 158, 166, 170);
                    float a = std::min(h * 0.16f, gut * 0.30f);
                    rdl->AddLine(ImVec2(cxm - a * 0.5f, cym - a), ImVec2(cxm + a * 0.5f, cym), c, 1.4f);
                    rdl->AddLine(ImVec2(cxm + a * 0.5f, cym), ImVec2(cxm - a * 0.5f, cym + a), c, 1.4f);
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
                bool canSel = !e.dir && ei < (int)rbSel.size();
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
                        if (!B.entries[shown[k]].dir && (size_t)shown[k] < rbSel.size())
                            rbSel[shown[k]] = 1;
                } else if (e.dir) {
                    remoteBrowseTo(joined(e.name));
                } else if (e.group) {
                    // one row = one stack; members arrive numerically sorted
                    std::vector<std::string> files;
                    for (const auto& m : e.members) files.push_back(joined(m));
                    openRemoteStack(B.host, files);
                    rbSelAnchor = ei;
                } else {
                    openRemote(makeRemoteUrl(B.host, joined(e.name)));
                    rbSelAnchor = ei;
                }
            }
            if (!servable) ImGui::PopStyleColor();   // before the popup, or it tints the menu
            if (ImGui::BeginPopupContextItem("ctx")) {
                std::string full = joined(e.name);
                if (e.dir) {
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
                } else if (e.group) {
                    if (ImGui::MenuItem("Open as stack")) {
                        std::vector<std::string> files;
                        for (const auto& m : e.members) files.push_back(joined(m));
                        openRemoteStack(B.host, files);
                    }
                    ImGui::Separator();
                } else if (isNpyName(e.name)) {
                    if (ImGui::MenuItem("Open"))
                        openRemote(makeRemoteUrl(B.host, full));
                    // one file as a stack: a frame-axis file becomes its frames
                    if (ImGui::MenuItem("Open as stack"))
                        openRemoteStack(B.host, { full });
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Copy path")) {
                    ImGui::SetClipboardText(full.c_str());
                    toast("copied " + full);
                }
                if (!e.dir && ImGui::MenuItem("Properties...")) {
                    rbPropsEntry = e;
                    rbPropsPath = full;
                    rbPropsOpen = true;
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (!e.dir && isNpyName(e.name)) ImGui::TextDisabled("%s", fmtEntryShape(e).c_str());
            ImGui::TableNextColumn();
            if (!e.dir) ImGui::TextDisabled("%s", fmtBytesHuman(e.size).c_str());
            ImGui::TableNextColumn();
            if (e.mtime > 0) ImGui::TextDisabled("%s", fmtUnixTime(e.mtime).c_str());
            else if (!e.dir) ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
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
        ImGui::Text("size      %s (%llu bytes)%s", fmtBytesHuman(e.size).c_str(),
                    (unsigned long long)e.size, e.group ? "  - all frames" : "");
        ImGui::Text("modified  %s (this machine's timezone)", fmtUnixTime(e.mtime).c_str());
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
    }
    // one file open needs no header
    bool showHeaders = groups.size() > 1 || (groups.size() == 1 && stacks.size() > 1);
    for (const auto& group : groups) {
      ImGui::PushID(group.batch);
      bool open = true;
      if (showHeaders) {
          open = ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                            ImGuiTreeNodeFlags_SpanAvailWidth,
                                   "%s   (%d)", group.label.c_str(), (int)group.stacks.size());
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
              ImGui::EndPopup();
          }
      }
      if (open)
      for (const auto& stackPtr : group.stacks) {
        const auto& stack = *stackPtr;
        const ImageDoc& head = *app.images[stack.front()];
        // name and format share one row: the dim/format part is right-aligned and dimmed
        auto rowWithMeta = [](const ImageDoc& d, const char* label, bool selected,
                              const char* extra = nullptr) -> bool {
            const ImGuiStyle& st = ImGui::GetStyle();
            char meta[96];
            snprintf(meta, sizeof meta, "%dx%d %dch %s%s", d.w, d.h, d.ch, d.dtype.c_str(),
                     extra ? extra : "");
            float avail = ImGui::GetContentRegionAvail().x;
            float metaW = ImGui::CalcTextSize(meta).x;
            // one source of truth for the split, so the two halves cannot overlap
            float nameW = std::max(avail - metaW - st.ItemSpacing.x, ImGui::GetFontSize() * 4.0f);
            bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowOverlap,
                                             ImVec2(nameW, 0));
            bool hov = ImGui::IsItemHovered();     // the NAME, not the dimmed metadata
            ImGui::SameLine(nameW + st.ItemSpacing.x);
            ImGui::TextDisabled("%s", meta);
            if (hov) ImGui::SetTooltip("%s", d.path.c_str());
            return clicked;
        };
        if (head.seqId == 0) {
            int i = stack.front();
            char lb[512];
            snprintf(lb, 512, "%s##%d", head.name.c_str(), i);
            if (rowWithMeta(head, lb, i == app.current, nullptr)) {
                selectImage(i);
                if (app.fitOnSwitch) app.fitRequested = true;
            }
            continue;
        }
        App::SeqInfo* si = seqInfo(head.seqId);
        int pos = 0;
        bool active = false;
        for (int k = 0; k < (int)stack.size(); k++)
            if (stack[k] == app.current) { pos = k; active = true; }
        ImGui::PushID(head.seqId);
        char lb[512];
        snprintf(lb, 512, "%s", si ? si->name.c_str() : "sequence");
        char frames[24];
        snprintf(frames, sizeof frames, "  %df", (int)stack.size());   // frame count
        if (rowWithMeta(head, lb, active, frames)) {
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
            ImGui::EndPopup();
        }
        if (active && ImGui::IsKeyPressed(ImGuiKey_F2, false))
            ImGui::OpenPopup("seqctx");
        // No frame slider here: it used to appear under the active row, and a
        // row that grows on selection makes the whole list jump - the scrub bar
        // lives at the bottom of the Image View now, where the frames are.
        ImGui::PopID();
      }
      if (showHeaders && open) ImGui::TreePop();
      ImGui::PopID();
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

static void drawMenuBar(GLFWwindow* win) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", SC_MOD "+O")) openFileDialog();
        if (ImGui::MenuItem("Open Folder...", SC_MOD "+Shift+O")) openFolderDialog();
        // The OS dialog can only show THIS machine's disks (the NAS included,
        // since it is mounted). Files on a server need the ssh:// path, so they
        // need a place to type it.
        if (ImGui::MenuItem("Start Remote (ssh)...")) app.remoteDlgOpen = true;
        // Where the muscle memory goes looking. Connected: raise the browser.
        // Not connected: there is nothing to browse yet, so ask for the host -
        // the same two steps either way, just entered from the familiar place.
        if (ImGui::MenuItem("Open File (Remote)...")) {
            app.showRemote = true;
            app.focusRemote = true;
            if (!app.rbrowse.connected) app.remoteDlgOpen = true;
        }
        // The remote mirror of Open Folder: every stack below the current
        // browse directory, one stack per subfolder. Not connected yet: ask
        // for the host first - the same two steps as Open File (Remote).
        if (ImGui::MenuItem("Open Folder (Remote)...")) {
            app.showRemote = true;
            app.focusRemote = true;
            if (app.rbrowse.connected) remoteScanFolder(app.rbrowse.dir);
            else app.remoteDlgOpen = true;
        }
        if (app.rbrowse.connected && ImGui::IsItemHovered())
            ImGui::SetTooltip("scans %s - browse to the folder you want first",
                              app.rbrowse.dir.c_str());
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
        if (ImGui::MenuItem("Close Image", SC_MOD "+W", false, cur() != nullptr)) closeCurrent();
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
            if (ImGui::MenuItem("Pin this frame as B", "Shift+B", false, cur() != nullptr)) pinCurrentAsB();
            if (ImGui::MenuItem("Swap A and B", "Shift+\\ or Shift+C", false, cmpB() != nullptr))
                swapCompare();
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
                std::string lbl = d->name + (d->seqId != 0 ? "   [stack]" : "");
                if (ImGui::MenuItem(lbl.c_str(), nullptr, app.compareBUid == d->uid)) {
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
        ImGui::Separator();
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Files", nullptr, &app.showFiles);
            ImGui::MenuItem("Remote", nullptr, &app.showRemote);
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
static std::string g_scanSelftest;      // --scan-selftest <dir>: remote Open Folder, print, exit

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
        "  --lin-selftest              load, fit linearity over the stacks, print, exit\n"
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
        } else if (a == "--bench" || a == "--crash-test") {
            next();                                // consumed in main(), not an error
        } else if (a == "--cfa") {                 // none | bayer | quad
            std::string v = next();
            app.forceCfa = v == "bayer" ? 1 : v == "quad" || v == "quad-bayer" ? 2
                         : v == "none" ? 0 : -1;
            if (app.forceCfa < 0) fprintf(stderr, "--cfa expects none|bayer|quad\n");
            app.forceCfaPattern = d.cfaPattern;    // --bayer-pattern, if it came first
        } else if (a == "--lin-selftest") {
            g_linSelftest = true;                  // handled in main() after loading
        } else if (a == "--framestats-selftest") {
            g_fstatSelftest = true;                // handled in main() after loading
        } else if (a == "--scan-selftest") {
            g_scanSelftest = next();               // handled in main()
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
            bool ok = nGroups == 1 && nPlain == 3 && g && g->frames == 24 &&
                      g->members.size() == 24 && g->hasMeta && g->dtype == "f32" &&
                      g->name.find('#') != std::string::npos;
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
    int benchFrames = 0, crashAfter = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--bench")) benchFrames = std::max(1, atoi(argv[i + 1]));
        // developer flag: verify the crash safety net actually writes a session
        if (!strcmp(argv[i], "--crash-test")) crashAfter = std::max(1, atoi(argv[i + 1]));
    }
    app.exePath = argv[0];
    loadPrefs();       // before the theme is applied and before the CLI is parsed
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
    if (benchFrames) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
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
    ImFont* jp = fontPath.empty() ? nullptr
        : io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f * fontScale, nullptr,
                                       io.Fonts->GetGlyphRangesJapanese());
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
            // exactly what the Load button does with the default selection
            if (app.folderPickOpen && app.folderPickRemote) {
                for (auto& e : app.folderPick)
                    app.rbOpenQueue.push_back({ app.folderPickHost, std::move(e.g.files), e.g.name });
                app.folderPick.clear();
                app.folderPickOpen = false;
            }
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

    // The per-frame table, verifiable without a human: load the stack given on
    // the command line, run the exact clipboard path, print the TSV to stdout.
    // An independent numpy implementation must reproduce every number.
    if (g_fstatSelftest) {
        double t0 = glfwGetTime();
        while (glfwGetTime() - t0 < 600.0) {
            if (app.folderPickOpen && !app.folderPickRemote) {   // headless accept
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
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
            // headless "Load selected": Open Folder now ALWAYS shows the picker,
            // so a selftest accepts it the way the Load button would
            if (app.folderPickOpen && !app.folderPickRemote) {
                std::vector<App::PendingGroup> sel;
                for (auto& e : app.folderPick) sel.push_back(std::move(e.g));
                app.folderPick.clear();
                app.folderPickOpen = false;
                enqueueGroups(std::move(sel));
            }
            pumpSequenceAndQueue();
            if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                !app.folderPickOpen) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (auto& si : app.seqs)
            if (!std::isfinite(si.level)) si.level = extractLevelFromName(si.name);
        linRecompute();
        const App::LinState& L = app.lin;
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
        if (app.seqThread.joinable()) app.seqThread.join();
        return L.fitValid ? 0 : 1;
    }

    std::vector<double> benchMs;
    int benchLeft = benchFrames;
    if (benchFrames) {                 // exercise every panel, not just the defaults
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = true;
        app.showHistogram = app.showTemporal = app.showProjection = true;
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
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_S)) saveSessionDialog();
            // Emacs-style navigation: time axis = C-f/C-b, stack axis = C-n/C-p,
            // sequence start/end = C-a/C-e (always Ctrl, also on macOS).
            // Ctrl+F is "find" while the Remote browser owns focus (it jumps to
            // the filter box there), so frame-stepping must yield to it.
            ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow;
            bool remoteFocused = nav && nav->RootWindow &&
                                 strcmp(nav->RootWindow->Name, "Remote") == 0;
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
        drawMenuBar(win);

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
            ImGui::DockBuilderDockWindow("Remote", left);   // tabbed with Files
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
            if (ImGui::Begin("Remote", &app.showRemote)) drawPanelRemote();
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
                                       app.diffAbs ? "|A-B|" : "A-B", app.diff.gain, b->name.c_str());
                } else {
                    float fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "   |  A/B %s %.0f%%  B: %s",
                                       app.compareMode == App::CmpSplit ? "split" : "wipe",
                                       fr * 100, b->name.c_str());
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
            // Either the session is up, or the image on screen came from one -
            // a dropped connection does not make the pixels local.
            const bool remoteNow = app.rbrowse.connected || (im && !im->remoteUrl.empty());
            std::string rhost = app.rbrowse.host;
            if (rhost.empty() && im && !im->remoteUrl.empty()) {
                std::string rp;
                remote::parseUrl(im->remoteUrl, rhost, rp);
            }
            static std::string lastTitle;
            std::string title = im ? im->name + " - viewer" : "viewer v0.1";
            if (remoteNow) title += "  [" + (rhost.empty() ? std::string("local peer") : rhost) + "]";
            if (title != lastTitle) { glfwSetWindowTitle(win, title.c_str()); lastTitle = title; }
            static int lastIconVariant = -1;
            if ((int)remoteNow != lastIconVariant) {
                applyWindowIcon(win, remoteNow);
                lastIconVariant = (int)remoteNow;
            }
        }

        drawRawModal();
        drawSequenceModal();
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
                    // auto blink alternates on a timer, so it needs frames with
                    // no input at all - same case as a background load
                    (app.compareMode == App::CmpFlip && app.flipAuto);
        if (benchFrames) { glfwPollEvents(); app.wakeFrames = 1; busy = true; }
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
                for (const auto& d : app.images)
                    if (d->path != app.images[0]->path || d->npzMember != app.images[0]->npzMember) {
                        setCompareB(d.get()); break;
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
        redrawNow();
        if (benchFrames) {
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
    autosaveSession();                // also covers a normal quit
    // Only what the user changed in this run: a one-off --sequence flag or the
    // gamma inside a --session must not quietly become the default.
    if (app.prefsDirty) savePrefs();
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
    return 0;
}
