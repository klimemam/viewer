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
#include "ui_theme.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
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
    int cfa = 0;                      // 0 none, 1 Bayer, 2 Quad Bayer
    int cfaPattern = 0;               // index into CFA_PATTERNS
    bool cfaColorize = false;
    // raw reload parameters (sessions + post-open reinterpretation; -1 = not raw)
    int rawDtype = -1, rawInterp = 0, rawOffset = 0;
    bool rawLE = true;
    // crop bookkeeping: srcW/srcH = full source dims (0 = unknown), cropX/Y = origin in source
    int srcW = 0, srcH = 0, cropX = 0, cropY = 0;
    int displayLut = -1;              // index into plugin_host::displays(), -1 = gray
    int dataRev = 0;                  // bumped on in-place pixel changes (crop)
    int seqId = 0;                    // 0 = standalone, >0 = frame of that sequence
    int seqIndex = 0;                 // position within the sequence (file number order)

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
    bool showHelp = false, showAbout = false;
    // hover state (image coords, -1 = none)
    int hoverX = -1, hoverY = -1;
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
    int tool = 0;                     // 0 Navigate (V), 1 ROI (R), 2 POI (P)
    bool wheelZoomPlain = false;      // false: Ctrl+wheel zooms, plain wheel pans
    // analyzer plugin state: cached result grid (rows = keys, cols = ROIs)
    struct AnalysisState {
        const ImageDoc* img = nullptr;
        int plugin = -1;
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
    } ana;
    bool anaAuto = false;
    int anaSel = 0;
    bool anaRunRequest = false;       // set by the Measure menu; consumed by the panel
    // host-built-in histogram cache (ROI-aware, CFA-split)
    struct HistState {
        const ImageDoc* img = nullptr;
        int dataRev = -1;
        float black = 0, white = 0;
        uint64_t annRev = (uint64_t)-1;
        int selAnn = -2, cfa = -1, cfaPattern = -1;
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
    };
    std::vector<SeqInfo> seqs;
    int nextSeqId = 1;
    int seqLoadMode = 0;              // 0 = ask, 1 = always, 2 = never
    // background loader
    std::thread seqThread;
    std::atomic<bool> seqCancel{ false };
    std::atomic<bool> seqRunning{ false };
    std::atomic<int> seqDone{ 0 }, seqTotal{ 0 };
    std::mutex seqMtx;
    std::vector<std::pair<int, std::unique_ptr<ImageDoc>>> seqReady;   // (seqIndex, frame)
    std::string seqErr;               // guarded by seqMtx
    int seqLoadingId = 0;
    size_t seqBytes = 0;
    // pending "load the rest of the folder?" question
    int seqAskImage = -1;
    std::vector<std::string> seqAskFiles;
    std::string seqAskPattern;
    // queued stacks from "Open Folder" (loaded one after another)
    struct PendingGroup { std::string name; std::vector<std::string> files; bool isRaw = false; };
    std::vector<PendingGroup> seqQueue;
    bool folderRecipeValid = false;   // raw recipe shared by the queue (see g_folderRecipe)
    // "which sequences do you want?" picker shown after scanning a folder
    struct FolderPick { PendingGroup g; bool selected = true; };
    std::vector<FolderPick> folderPick;
    bool folderPickOpen = false;
    std::string folderPickRoot;
    char pickInclude[128] = "";       // glob applied to "folder/pattern" (empty = all)
    char pickExclude[128] = "";
    std::unique_ptr<pfd::select_folder> folderDlg;
    // temporal analysis cache (built-in, follows the selected ROI)
    struct TemporalState {
        int seqId = -1;
        int frames = 0;
        uint64_t annRev = (uint64_t)-1;
        int selAnn = -2;
        std::vector<float> idx, frameMean, frameStd;
        double tempNoise = 0, fixedPattern = 0, totalNoise = 0;
        bool valid = false;
        bool roiUsed = false;
    } temporal;
    std::vector<ImageDoc*> texLru;    // GPU textures kept for the N most recent frames
    int roiChannel = -1;              // channel shown in the ROI table (-1 = all)
    // panel visibility (persisted with the ImGui layout)
    bool showFiles = true, showInspector = true, showRois = true, showAnalysis = true;
    bool showHistogram = true, showTemporal = true;
    bool resetLayout = false;
};
static const ImU32 ANN_COLORS[8] = {
    IM_COL32(77, 163, 255, 255), IM_COL32(105, 220, 130, 255), IM_COL32(255, 184, 77, 255),
    IM_COL32(255, 120, 120, 255), IM_COL32(200, 120, 255, 255), IM_COL32(90, 220, 220, 255),
    IM_COL32(255, 150, 200, 255), IM_COL32(180, 200, 90, 255),
};
static const char* TOOL_NAMES[3] = { "Nav", "ROI", "Pin" };
static App app;

static ImageDoc* cur() { return app.current >= 0 && app.current < (int)app.images.size() ? app.images[app.current].get() : nullptr; }

static void toast(const std::string& msg, bool err = false) {
    app.toast = msg; app.toastErr = err;
    app.toastUntil = ImGui::GetTime() + (err ? 6.0 : 2.5);
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
static void defaultRange(ImageDoc& im) {
    if (im.dtype == "u8" || im.dtype == "i8")       { im.black = 0; im.white = 255; }
    else if (im.dtype == "u16")                     { im.black = 0; im.white = 65535; }
    else if (im.dtype == "i16")                     { im.black = 0; im.white = 32767; }
    else if (im.vmin >= -0.001f && im.vmax <= 1.2f && im.vmax > 0.005f) { im.black = 0; im.white = 1; }
    else                                            { im.black = im.vmin; im.white = im.vmax; }
}

// upload/normalize into RGBA8 texture
static void rebuildTexture(ImageDoc& im) {
    std::vector<uint8_t> rgba((size_t)im.w * im.h * 4);
    float inv = 1.0f / std::max(im.white - im.black, 1e-20f);
    float invGamma = 1.0f / app.dispGamma;
    bool doGamma = fabsf(app.dispGamma - 1.0f) > 1e-3f;
    bool cfaColor = im.ch == 1 && im.cfa != 0 && im.cfaColorize;
    const uint8_t* lut = nullptr;   // display plugin colormap (1ch only; CFA colorize wins)
    if (im.ch == 1 && !cfaColor && im.displayLut >= 0 &&
        im.displayLut < (int)plugin_host::displays().size())
        lut = plugin_host::displays()[im.displayLut].lut.data();
    for (size_t p = 0; p < (size_t)im.w * im.h; p++) {
        const float* src = &im.data[p * im.ch];
        float r, g, b;
        if (lut) {
            float x = std::clamp((src[0] - im.black) * inv, 0.0f, 1.0f);
            if (doGamma) x = powf(x, invGamma);
            int idx = (int)(x * 255.0f + 0.5f);
            rgba[p * 4 + 0] = lut[idx * 3];
            rgba[p * 4 + 1] = lut[idx * 3 + 1];
            rgba[p * 4 + 2] = lut[idx * 3 + 2];
            rgba[p * 4 + 3] = 255;
            continue;
        }
        if (cfaColor) {
            int c = cfaChannelAt(im, (int)(p % im.w), (int)(p / im.w));
            r = c == 0 ? src[0] : im.black;
            g = (c == 1 || c == 2) ? src[0] : im.black;
            b = c == 3 ? src[0] : im.black;
        }
        else if (im.ch == 1) { r = g = b = src[0]; }
        else if (im.ch == 2) { r = src[0]; g = src[1]; b = im.black; }
        else                 { r = src[0]; g = src[1]; b = src[2]; }
        float v[3] = { (r - im.black) * inv, (g - im.black) * inv, (b - im.black) * inv };
        for (int c = 0; c < 3; c++) {
            float x = std::clamp(v[c], 0.0f, 1.0f);
            if (doGamma) x = powf(x, invGamma);
            rgba[p * 4 + c] = (uint8_t)(x * 255.0f + 0.5f);
        }
        rgba[p * 4 + 3] = 255;
    }
    if (!im.tex) {
        glGenTextures(1, &im.tex);
        // keep only the most recent textures resident: a 200-frame sequence would
        // otherwise pin gigabytes of VRAM
        app.texLru.erase(std::remove(app.texLru.begin(), app.texLru.end(), &im), app.texLru.end());
        app.texLru.push_back(&im);
        const size_t TEX_KEEP = 12;
        while (app.texLru.size() > TEX_KEEP) {
            ImageDoc* old = app.texLru.front();
            app.texLru.erase(app.texLru.begin());
            if (old != &im && old->tex) {
                glDeleteTextures(1, &old->tex);
                old->tex = 0;
                old->texDirty = true;
            }
        }
    }
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
static void forgetTexture(ImageDoc* im) {
    app.texLru.erase(std::remove(app.texLru.begin(), app.texLru.end(), im), app.texLru.end());
}
static void closeCurrent() {
    ImageDoc* im = cur();
    if (!im) return;
    if (app.ana.img == im) app.ana.img = nullptr;   // drop cached analysis of a dead image
    if (app.hist.img == im) app.hist.img = nullptr;
    app.temporal.seqId = -1;
    forgetTexture(im);
    if (im->tex) glDeleteTextures(1, &im->tex);
    app.images.erase(app.images.begin() + app.current);
    app.current = app.images.empty() ? -1 : std::min(app.current, (int)app.images.size() - 1);
    app.fitRequested = true;
}
static void closeAll() {
    app.ana.img = nullptr;
    app.hist.img = nullptr;
    app.temporal.seqId = -1;
    app.texLru.clear();
    for (auto& d : app.images)
        if (d->tex) glDeleteTextures(1, &d->tex);
    app.images.clear();
    app.seqs.clear();
    app.current = -1;
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
    f.black = im.black; f.white = im.white;
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
// precondition hints; ABI v2 moves this into a plugin-declared description field
static const char* analyzerHint(const std::string& name) {
    if (name == "stats/moments")       return "any image / ROI";
    if (name == "noise/floor")         return "flat-ish ROI";
    if (name == "uniformity/prnu-fpn") return "bright flat field";
    if (name == "sharpness/gradient")  return "same-scene compare";
    return "";
}

static void addImage(std::unique_ptr<ImageDoc> im) {
    computeMinMax(*im);
    defaultRange(*im);
    im->texDirty = true;
    app.images.push_back(std::move(im));
    app.current = (int)app.images.size() - 1;
    app.fitRequested = true;
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
static std::unique_ptr<ImageDoc> decodeNpy(const std::string& path, std::string& errOut) {
    auto fail = [&](const char* m) { errOut = m; return std::unique_ptr<ImageDoc>(); };
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return fail("cannot read file");
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

    std::string note;
    while (shape.size() > 3) {          // take [0] of batch dims
        if (shape[0] != 1) note = "showing [0] of batch";
        shape.erase(shape.begin());
        strides.erase(strides.begin());
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

    auto im = std::make_unique<ImageDoc>();
    im->name = baseName(path); im->path = path;
    im->w = (int)W; im->h = (int)H; im->ch = (int)C;
    im->dtype = dtypeName; im->note = note;
    im->data.resize((size_t)W * H * C);
    size_t di = 0;
    for (int64_t y = 0; y < H; y++)
        for (int64_t x = 0; x < W; x++)
            for (int64_t c = 0; c < C; c++)
                im->data[di++] = getVal((size_t)(y * sh + x * sw + c * sc));
    return im;
}

static std::string loadNpy(const std::string& path) {
    std::string err;
    auto im = decodeNpy(path, err);
    if (!im) return err.empty() ? "decode failed" : err;
    addImage(std::move(im));
    return {};
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

// ---------------------------------------------------------------- session save/load
// (sequence helpers are defined further down; sessions can restore a stack)
static std::vector<std::string> findSequenceSiblings(const std::string& path,
                                                     std::string& patternOut);
static void startSequenceLoad(int imageIdx, const std::vector<std::string>& files,
                              const std::string& pattern);
// Plain line-based text format (.vsession): view state + per-image reload recipes.
static void saveSession(std::string path) {
    if (path.find('.') == std::string::npos) path += ".vsession";
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { toast("cannot write session file", true); return; }
    f << "viewer-session 1\n";
    f << "gamma " << app.dispGamma << "\n";
    f << "grid " << (app.showGrid ? 1 : 0) << "\n";
    f << "zoom " << app.view.zoom << "\n";
    f << "center " << app.view.center.x << " " << app.view.center.y << "\n";
    f << "current " << app.current << "\n";
    for (auto& d : app.images) {
        if (d->path.empty()) continue;
        // sequences: store only the frame in view + a marker to reload the stack
        // (writing every frame would bloat the file and slow restore)
        if (d->seqId != 0 && d.get() != cur()) continue;
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
        if (d->seqId != 0) f << "seqload 1\n";
    }
    for (const auto& a : app.anns)   // label last: may contain spaces
        f << "ann " << a.type << " " << a.x << " " << a.y << " " << a.w << " " << a.h << " "
          << a.color << " " << (a.visible ? 1 : 0) << " " << a.label << "\n";
    toast("session saved: " + baseName(path));
}

static std::string loadNpy(const std::string& path);   // fwd (defined above, decl for clarity)

static std::string loadSession(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read session file";
    std::string text((const char*)buf.data(), buf.size());
    std::istringstream ss(text);
    std::string line;
    if (!std::getline(ss, line) || line.rfind("viewer-session", 0) != 0)
        return "not a viewer session file";
    closeAll();
    app.anns.clear();
    app.selectedAnn = 0;
    app.annRev++;
    float zoom = 0; ImVec2 center(0, 0); int current = 0;
    bool haveView = false;
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
        else if (key == "current") ls >> current;
        else if (key == "seqload") {   // applies to the image loaded just above
            int on = 0; ls >> on;
            if (on && cur() && !cur()->path.empty()) {
                std::string pat;
                std::vector<std::string> files = findSequenceSiblings(cur()->path, pat);
                if (files.size() >= 2) startSequenceLoad(app.current, files, pat);
            }
        }
        else if (key == "ann") {
            App::Ann a; int vis = 1;
            ls >> a.type >> a.x >> a.y >> a.w >> a.h >> a.color >> vis;
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
                err = loadNpy(p);
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
            if (!err.empty()) { toast("session: " + err, true); continue; }
            cur()->black = bk; cur()->white = wt; cur()->texDirty = true;
        }
    }
    app.selectedAnn = 0;
    if (current >= 0 && current < (int)app.images.size()) app.current = current;
    if (haveView && !app.images.empty()) {
        app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        app.view.center = center;
        app.fitRequested = false;      // restored view wins over fit-on-load
    }
    markAllTexDirty();                 // gamma may have changed
    return {};
}

// ---------------------------------------------------------------- sequences (連番)
// A sequence is one "stack": frames from numbered files in a folder that share a
// decode recipe. It is the unit temporal analysis operates on.
static const size_t SEQ_MEM_BUDGET = (size_t)6 << 30;   // stop loading past ~6 GB

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
    fprintf(stderr, "sequence: %s - %d files (%s)\n", info.name.c_str(),
            (int)files.size(), isRaw ? "raw recipe" : "npy");
    app.seqThread = std::thread([jobs, isRaw, recipe]() {
        size_t bytes = 0;
        int failures = 0;
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
            bytes += doc->data.size() * sizeof(float);
            {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqReady.emplace_back(j.index, std::move(doc));
            }
            app.seqDone++;
            if (bytes > SEQ_MEM_BUDGET) {
                std::lock_guard<std::mutex> lk(app.seqMtx);
                app.seqErr += "memory budget reached - stopped loading\n";
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
        if (!app.seqReady.empty()) batch.swap(app.seqReady);
        err.swap(app.seqErr);
    }
    if (!err.empty()) toast(err, true);
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
        computeMinMax(*doc);
        if (ref) {
            doc->black = ref->black; doc->white = ref->white;
            doc->cfa = ref->cfa; doc->cfaPattern = ref->cfaPattern;
            doc->cfaColorize = ref->cfaColorize;
            doc->displayLut = ref->displayLut;
        } else {
            defaultRange(*doc);
        }
        doc->texDirty = true;
        app.images.push_back(std::move(doc));
    }
    app.temporal.seqId = -1;          // new frames invalidate temporal stats
    if (!app.seqRunning && app.seqThread.joinable()) {
        app.seqThread.join();
        int n = 0;
        for (const auto& d : app.images) if (d->seqId == app.seqLoadingId) n++;
        fprintf(stderr, "sequence: loaded %d frames\n", n);
    }
}

static void startNextQueuedGroup();   // defined with the folder-open code below

// called once per frame: integrate decoded frames, then chain the next stack
static void pumpSequenceAndQueue() {
    pumpSequence();
    if (!app.seqRunning && !app.seqQueue.empty()) startNextQueuedGroup();
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
static App::SeqInfo* seqInfo(int id) {
    for (auto& s : app.seqs) if (s.id == id) return &s;
    return nullptr;
}
static void selectImage(int idx) {
    if (idx < 0 || idx >= (int)app.images.size()) return;
    app.current = idx;
    if (App::SeqInfo* si = seqInfo(app.images[idx]->seqId)) si->lastImageIdx = idx;
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
    auto st = stacksOf();
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
static const char* SEQ_EXTS[] = { ".npy", ".bin", ".raw", ".yuv", ".dat", ".rggb" };
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
    const size_t MAX_GROUPS = 64;
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
    std::filesystem::recursive_directory_iterator it(
        rootPath, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
        std::error_code ec2;
        if (it->is_regular_file(ec2)) addFile(it->path());
    }
    std::sort(perDir.begin(), perDir.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string rootStr = rootPath.u8string();
    for (auto& d : perDir) {
        std::vector<std::string> files = d.second;
        std::sort(files.begin(), files.end());
        std::vector<bool> used(files.size(), false);
        for (size_t i = 0; i < files.size() && groups.size() < MAX_GROUPS; i++) {
            if (used[i]) continue;
            std::string pattern;
            std::vector<std::string> sibs = findSequenceSiblings(files[i], pattern);
            App::PendingGroup g;
            if (sibs.size() >= 2) {
                g.files = sibs;
                for (const auto& s : sibs)
                    for (size_t k = 0; k < files.size(); k++)
                        if (files[k] == s) used[k] = true;
            } else {
                g.files = { files[i] };
                pattern = baseName(files[i]);
                used[i] = true;
            }
            // name the stack by its folder relative to the opened root
            std::string rel = d.first.u8string();
            if (rel.size() > rootStr.size()) rel = rel.substr(rootStr.size() + 1);
            else rel.clear();
            g.name = rel.empty() ? pattern : rel + "/" + pattern;
            std::string ext = std::filesystem::u8path(g.files[0]).extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            g.isRaw = ext != ".npy";
            groups.push_back(std::move(g));
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
    if (g.isRaw) {
        RawDialog d = g_folderRecipe;
        d.path = g.files[0];
        d.replaceIdx = -1;
        err = loadRaw(d);
    } else {
        err = loadNpy(g.files[0]);
    }
    if (!err.empty()) { toast(baseName(g.files[0]) + ": " + err, true); return; }
    if (g.files.size() >= 2) startSequenceLoad(app.current, g.files, g.name);
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
    // one group, or "always load": no point asking
    if (groups.size() == 1 || app.seqLoadMode == 1) { enqueueGroups(std::move(groups)); return; }
    app.folderPick.clear();
    for (auto& g : groups) app.folderPick.push_back({ std::move(g), true });
    app.folderPickRoot = path;
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
    if (!ImGui::BeginPopupModal("Select sequences", nullptr)) return;

    ImGui::TextDisabled("%s", app.folderPickRoot.c_str());
    int selGroups = 0, selFiles = 0, allFiles = 0;
    for (const auto& e : app.folderPick) {
        allFiles += (int)e.g.files.size();
        if (e.selected) { selGroups++; selFiles += (int)e.g.files.size(); }
    }
    ImGui::Text("%d sequence(s), %d files found", (int)app.folderPick.size(), allFiles);
    // glob filters beat clicking dozens of checkboxes
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 10) * 0.5f;
    ImGui::SetNextItemWidth(half);
    bool ch = ImGui::InputTextWithHint("include", "* or 00/*,*_dark*", app.pickInclude,
                                       sizeof app.pickInclude);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(half);
    ch |= ImGui::InputTextWithHint("exclude", "e.g. *_ng*,02/*", app.pickExclude,
                                   sizeof app.pickExclude);
    if (ch) applyPickFilters();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetTooltip("comma separated; * and ? wildcards; a bare word matches anywhere\n"
                          "matched against \"folder/pattern\"");
    ImGui::SameLine();
    if (ImGui::SmallButton("Apply")) applyPickFilters();
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
        if (sel.empty()) toast("nothing selected", true);
        else enqueueGroups(std::move(sel));
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
    if (T.seqId == im->seqId && T.frames == (int)f.size() &&
        T.annRev == app.annRev && T.selAnn == app.selectedAnn)
        return;
    T.seqId = im->seqId; T.frames = (int)f.size();
    T.annRev = app.annRev; T.selAnn = app.selectedAnn;
    T.idx.clear(); T.frameMean.clear(); T.frameStd.clear();
    T.tempNoise = T.fixedPattern = T.totalNoise = 0;
    T.valid = false; T.roiUsed = false;

    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), chh = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && chh > 0) { rx = cx; ry = cy; rw = cw; rh = chh; T.roiUsed = true; }
        }
    }
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

static void openPath(const std::string& path) {
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
        std::vector<std::string>{ "Images (*.npy *.bin *.raw *.yuv *.dat)", "*.npy *.bin *.raw *.yuv *.dat",
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
    ImVec2 canvasP0 = ImVec2(origin.x + RULER_W, origin.y + RULER_H);
    ImVec2 canvasSize = ImVec2(std::max(avail.x - RULER_W, 50.0f), std::max(avail.y - RULER_H, 50.0f));
    ImVec2 canvasP1 = ImVec2(canvasP0.x + canvasSize.x, canvasP0.y + canvasSize.y);

    ImageDoc* im = cur();
    if (im && app.fitRequested) { fitToCanvas(canvasSize); app.fitRequested = false; }

    // interaction region = canvas (excluding rulers)
    ImGui::SetCursorScreenPos(canvasP0);
    ImGui::InvisibleButton("canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    auto imgToScr = [&](float ix, float iy) {
        return ImVec2(canvasP0.x + canvasSize.x * 0.5f + (ix - app.view.center.x) * app.view.zoom,
                      canvasP0.y + canvasSize.y * 0.5f + (iy - app.view.center.y) * app.view.zoom);
    };
    auto scrToImg = [&](ImVec2 s) {
        return ImVec2(app.view.center.x + (s.x - canvasP0.x - canvasSize.x * 0.5f) / app.view.zoom,
                      app.view.center.y + (s.y - canvasP0.y - canvasSize.y * 0.5f) / app.view.zoom);
    };

    // drag state: pan / new-ROI / move / resize (tool-mode interaction model)
    enum { DK_NONE, DK_PAN, DK_ROI_NEW, DK_ANN_MOVE, DK_ANN_RESIZE };
    static int dk = DK_NONE;
    static bool dragMoved = false;
    static bool clickEligible = false;   // left-button, no pan modifiers: clicks act on tools
    static ImVec2 drag0;
    static int dragAnnId = -1, dragCorner = -1;
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

    if (im) {
        if (ImGui::IsItemActivated()) {
            drag0 = scrToImg(io.MousePos);
            dragMoved = false;
            dragAnnId = -1; dragCorner = -1; tmpActive = false;
            bool mid = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            bool space = ImGui::IsKeyDown(ImGuiKey_Space);
            clickEligible = !mid && !space && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (mid || space) dk = DK_PAN;                       // universal pan
            else if (app.tool == 0) dk = io.KeyShift ? DK_ROI_NEW : DK_PAN;
            else if (app.tool == 1) {
                int corner; int hit = hitTest(drag0, corner);
                App::Ann* a = hit >= 0 ? findAnn(hit) : nullptr;
                if (a && a->type == 0 && corner >= 0) {
                    dk = DK_ANN_RESIZE; dragAnnId = hit; dragCorner = corner;
                    annOrig[0] = a->x; annOrig[1] = a->y; annOrig[2] = a->w; annOrig[3] = a->h;
                    app.selectedAnn = hit;
                } else if (a && a->type == 0) {
                    dk = DK_ANN_MOVE; dragAnnId = hit;
                    annOrig[0] = a->x; annOrig[1] = a->y;
                    app.selectedAnn = hit;
                } else dk = DK_ROI_NEW;
            } else dk = DK_NONE;                                 // POI tool: click on release
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f)))
            dragMoved = true;
        bool draggingAny = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ||
                           ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0);
        if (active && draggingAny) {
            if (dk == DK_PAN) {
                app.view.center.x -= io.MouseDelta.x / app.view.zoom;
                app.view.center.y -= io.MouseDelta.y / app.view.zoom;
            } else if (dk == DK_ROI_NEW && dragMoved) {
                ImVec2 q = scrToImg(io.MousePos);
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
                    ImVec2 q = scrToImg(io.MousePos);
                    int dx = (int)roundf(q.x - drag0.x), dy = (int)roundf(q.y - drag0.y);
                    a->x = std::clamp(annOrig[0] + dx, 0, std::max(0, im->w - std::max(a->w, 1)));
                    a->y = std::clamp(annOrig[1] + dy, 0, std::max(0, im->h - std::max(a->h, 1)));
                    app.annRev++;
                }
            } else if (dk == DK_ANN_RESIZE && dragMoved) {
                if (App::Ann* a = findAnn(dragAnnId)) {
                    int fx = (dragCorner % 2 == 0) ? annOrig[0] + annOrig[2] : annOrig[0];
                    int fy = (dragCorner / 2 == 0) ? annOrig[1] + annOrig[3] : annOrig[1];
                    ImVec2 q = scrToImg(io.MousePos);
                    int mx = std::clamp((int)roundf(q.x), 0, im->w);
                    int my = std::clamp((int)roundf(q.y), 0, im->h);
                    a->x = std::min(fx, mx); a->w = std::max(1, std::abs(mx - fx));
                    a->y = std::min(fy, my); a->h = std::max(1, std::abs(my - fy));
                    app.annRev++;
                }
            }
        }
        if (ImGui::IsItemDeactivated()) {
            ImVec2 q = scrToImg(io.MousePos);
            int px = (int)floorf(q.x), py = (int)floorf(q.y);
            bool inside = px >= 0 && py >= 0 && px < im->w && py < im->h;
            if (dk == DK_ROI_NEW && dragMoved && tmpActive) {
                if (tmpRect[2] >= 1 && tmpRect[3] >= 1)
                    addAnn(0, tmpRect[0], tmpRect[1], tmpRect[2], tmpRect[3]);
            } else if (!dragMoved && clickEligible) {   // middle/Space clicks never act on tools
                if (app.tool == 2 || (app.tool == 0 && io.KeyCtrl)) {
                    if (inside) addAnn(1, px, py, 0, 0);         // add POI
                } else {
                    int corner; app.selectedAnn = hitTest(q, corner);   // select / deselect
                }
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
                app.view.center.x = mImg.x - (io.MousePos.x - canvasP0.x - canvasSize.x * 0.5f) / z;
                app.view.center.y = mImg.y - (io.MousePos.y - canvasP0.y - canvasSize.y * 0.5f) / z;
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
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fitToCanvas(canvasSize);
    }

    // hover position
    app.hoverX = app.hoverY = -1;
    if (im && hovered) {
        ImVec2 q = scrToImg(io.MousePos);
        int ix = (int)floorf(q.x), iy = (int)floorf(q.y);
        if (ix >= 0 && iy >= 0 && ix < im->w && iy < im->h) { app.hoverX = ix; app.hoverY = iy; }
    }

    // ---- draw canvas ----
    dl->AddRectFilled(canvasP0, canvasP1, IM_COL32(12, 14, 16, 255));
    dl->PushClipRect(canvasP0, canvasP1, true);
    if (im) {
        if (im->texDirty) rebuildTexture(*im);
        setFilter(*im, app.view.zoom >= 1.0f);
        ImVec2 p0 = imgToScr(0, 0), p1 = imgToScr((float)im->w, (float)im->h);
        dl->AddImage((ImTextureID)(intptr_t)im->tex, p0, p1);
        dl->AddRect(p0, p1, IM_COL32(90, 100, 110, 255));

        // pixel grid at high zoom
        if (app.showGrid && app.view.zoom >= 8.0f) {
            ImVec2 tl = scrToImg(canvasP0), br = scrToImg(canvasP1);
            int x0 = std::max(0, (int)floorf(tl.x)), x1 = std::min(im->w, (int)ceilf(br.x));
            int y0 = std::max(0, (int)floorf(tl.y)), y1 = std::min(im->h, (int)ceilf(br.y));
            ImU32 gc = IM_COL32(120, 120, 120, 70);
            for (int x = x0; x <= x1; x++) { ImVec2 a = imgToScr((float)x, (float)y0), b = imgToScr((float)x, (float)y1); dl->AddLine(a, b, gc); }
            for (int y = y0; y <= y1; y++) { ImVec2 a = imgToScr((float)x0, (float)y), b = imgToScr((float)x1, (float)y); dl->AddLine(a, b, gc); }
        }
        // hovered pixel outline
        if (app.hoverX >= 0 && app.view.zoom >= 2.0f) {
            ImVec2 a = imgToScr((float)app.hoverX, (float)app.hoverY);
            ImVec2 b = imgToScr((float)app.hoverX + 1, (float)app.hoverY + 1);
            dl->AddRect(a, b, IM_COL32(255, 184, 77, 255), 0, 0, 1.5f);
        }
        // annotations (ROIs + POIs)
        for (const auto& a : app.anns) {
            if (!a.visible) continue;
            ImU32 col = ANN_COLORS[a.color & 7];
            bool sel = a.id == app.selectedAnn;
            if (a.type == 0) {
                ImVec2 ra = imgToScr((float)a.x, (float)a.y);
                ImVec2 rb = imgToScr((float)(a.x + a.w), (float)(a.y + a.h));
                dl->AddRectFilled(ra, rb, (col & 0x00FFFFFF) | 0x1E000000);
                dl->AddRect(ra, rb, col, 0, 0, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), col, a.label.c_str());
                if (sel && app.tool == 1) {                       // resize handles
                    ImVec2 hs[4] = { ra, ImVec2(rb.x, ra.y), ImVec2(ra.x, rb.y), rb };
                    for (const auto& hp : hs)
                        dl->AddRectFilled(ImVec2(hp.x - 3, hp.y - 3), ImVec2(hp.x + 3, hp.y + 3), col);
                }
            } else {
                if (a.x >= im->w || a.y >= im->h) continue;       // outside this image
                ImVec2 cpt = imgToScr(a.x + 0.5f, a.y + 0.5f);
                float r = sel ? 10.0f : 8.0f;
                dl->AddLine(ImVec2(cpt.x - r, cpt.y), ImVec2(cpt.x + r, cpt.y), col, sel ? 2.5f : 1.5f);
                dl->AddLine(ImVec2(cpt.x, cpt.y - r), ImVec2(cpt.x, cpt.y + r), col, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(cpt.x + 5, cpt.y + 4), col, a.label.c_str());
            }
        }
        // ROI being created (live preview)
        if (tmpActive && tmpRect[2] > 0 && tmpRect[3] > 0) {
            ImVec2 ra = imgToScr((float)tmpRect[0], (float)tmpRect[1]);
            ImVec2 rb = imgToScr((float)(tmpRect[0] + tmpRect[2]), (float)(tmpRect[1] + tmpRect[3]));
            dl->AddRectFilled(ra, rb, IM_COL32(77, 163, 255, 26));
            dl->AddRect(ra, rb, IM_COL32(77, 163, 255, 220), 0, 0, 1.5f);
            char lb[48];
            snprintf(lb, 48, "%dx%d", tmpRect[2], tmpRect[3]);
            dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), IM_COL32(120, 190, 255, 255), lb);
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
        // top ruler (X)
        dl->PushClipRect(ImVec2(canvasP0.x, origin.y), ImVec2(canvasP1.x, origin.y + RULER_H), true);
        {
            float ix0 = scrToImg(canvasP0).x, ix1 = scrToImg(canvasP1).x;
            for (float t = floorf(ix0 / step) * step; t <= ix1; t += step) {
                if (t < 0 || t > im->w) continue;
                float sx = imgToScr(t, 0).x;
                dl->AddLine(ImVec2(sx, origin.y + RULER_H - TICK), ImVec2(sx, origin.y + RULER_H), tickCol);
                char lb[32]; snprintf(lb, 32, "%.0f", t);
                dl->AddText(ImVec2(sx + 3 * s, origin.y + 2), txtCol, lb);
            }
            if (app.hoverX >= 0) {
                float sx = imgToScr((float)app.hoverX + 0.5f, 0).x;
                dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + RULER_H), markCol, 1.5f);
            }
        }
        dl->PopClipRect();
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

    // floating tool strip (submitted after the canvas item, so it wins hover)
    ImGui::SetCursorScreenPos(ImVec2(canvasP0.x + 8, canvasP0.y + 8));
    {
        const char* labels[3] = { "Nav (V)", "ROI (R)", "Pin (P)" };
        for (int t = 0; t < 3; t++) {
            if (t) ImGui::SameLine();
            bool on = app.tool == t;
            if (on) {   // solid accent pill: readable on the dark canvas in both themes
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            }
            if (ImGui::SmallButton(labels[t])) app.tool = t;
            if (on) ImGui::PopStyleColor(2);
        }
    }
}

static void recomputeHistogramIfNeeded(ImageDoc* im) {
    App::HistState& H = app.hist;
    if (H.img == im && H.dataRev == im->dataRev && H.black == im->black && H.white == im->white &&
        H.annRev == app.annRev && H.selAnn == app.selectedAnn &&
        H.cfa == im->cfa && H.cfaPattern == im->cfaPattern)
        return;
    H.img = im; H.dataRev = im->dataRev;
    H.black = im->black; H.white = im->white;
    H.annRev = app.annRev; H.selAnn = app.selectedAnn;
    H.cfa = im->cfa; H.cfaPattern = im->cfaPattern;
    memset(H.bins, 0, sizeof H.bins);
    H.maxBin = 1; H.clipLo = H.clipHi = 0; H.sampled = 0; H.roiUsed = false;

    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {           // selected ROI drives the histogram
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), ch2 = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && ch2 > 0) { rx = cx; ry = cy; rw = cw; rh = ch2; H.roiUsed = true; }
        }
    }
    bool cfa = im->ch == 1 && im->cfa != 0;
    H.nSeries = cfa ? 4 : std::min(im->ch, 3);
    static const char* CFA_SERIES[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_SERIES[4] = { "ch0", "ch1", "ch2", "ch3" };
    for (int s = 0; s < 4; s++) H.seriesNames[s] = cfa ? CFA_SERIES[s] : CH_SERIES[s];
    float inv = 256.0f / std::max(im->white - im->black, 1e-20f);
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
                float t = (v - im->black) * inv;
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
                float t = (v - im->black) * inv;
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
    fmtTick(tb, sizeof tb, ymax, yInt);
    float wy = ImGui::CalcTextSize(tb).x;
    fmtTick(tb, sizeof tb, ymin, yInt);
    wy = std::max(wy, ImGui::CalcTextSize(tb).x);
    const float marginL = wy + 8 * s;
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

    // Y ticks
    double ystep = niceStep((float)((ymax - ymin) / 4.0));
    if (yInt && ystep < 1) ystep = 1;
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
        dl->AddText(ImVec2(std::min(x - ts.x * 0.5f, pr.p1.x - ts.x), pr.p1.y + 2 * s), txt, tb);
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
        ImGui::Dummy(ImVec2(0, 4));
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
        dl->PopClipRect();
    }
}

static void drawInspector() {
    ImageDoc* im = cur();
    ImGui::Text("Pixel");
    ImGui::Separator();
    // Layout must not depend on hover state: the section always occupies the same
    // number of rows, otherwise everything below jumps as the cursor enters/leaves.
    {
        bool live = im && app.hoverX >= 0;
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
        float inv = im ? 1.0f / std::max(im->white - im->black, 1e-20f) : 1.0f;
        if (ImGui::BeginTable("px", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("ch"); ImGui::TableSetupColumn("raw"); ImGui::TableSetupColumn("norm");
            ImGui::TableHeadersRow();
            for (int c = 0; c < nch; c++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c < 4 ? lb[std::min(c, 3)] : "?");
                if (live) {
                    float v = im->sample(app.hoverX, app.hoverY, c);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtVal(v, im->dtype).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%.4f", (v - im->black) * inv);
                } else {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Text("Image");
    ImGui::Separator();
    if (im) {
        ImGui::Text("%d x %d   %dch   %s", im->w, im->h, im->ch, im->dtype.c_str());
        if (!im->note.empty()) ImGui::TextDisabled("%s", im->note.c_str());
        ImGui::Text("min %s / max %s", fmtVal(im->vmin, im->dtype).c_str(), fmtVal(im->vmax, im->dtype).c_str());
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

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Text("Range (black / white)");
        ImGui::Separator();
        // EnterReturnsTrue is not allowed on InputScalar-family widgets (asserts in
        // debug builds); edit a shadow buffer and commit on deactivate-after-edit.
        static float bwEdit[2];
        static bool bwEditing = false;
        if (!bwEditing) { bwEdit[0] = im->black; bwEdit[1] = im->white; }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputFloat2("##bw", bwEdit, "%.5g");
        bwEditing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && bwEdit[1] > bwEdit[0]) {
            im->black = bwEdit[0]; im->white = bwEdit[1]; im->texDirty = true;
        }
        if (ImGui::Button("Auto"))  { im->black = im->vmin; im->white = im->vmax; im->texDirty = true; } ImGui::SameLine();
        if (ImGui::Button("0-1"))   { im->black = 0; im->white = 1; im->texDirty = true; } ImGui::SameLine();
        if (ImGui::Button("0-255")) { im->black = 0; im->white = 255; im->texDirty = true; } ImGui::SameLine();
        if (ImGui::Button("0-65535")) { im->black = 0; im->white = 65535; im->texDirty = true; }

        ImGui::Dummy(ImVec2(0, 8));
        int g = app.dispGamma > 1.5f ? 1 : 0;
        ImGui::TextUnformatted("Display gamma"); ImGui::SameLine();
        if (ImGui::RadioButton("1.0", g == 0)) { app.dispGamma = 1.0f; markAllTexDirty(); } ImGui::SameLine();
        if (ImGui::RadioButton("2.2", g == 1)) { app.dispGamma = 2.2f; markAllTexDirty(); }
        ImGui::Checkbox("Pixel grid (G, zoom>=8)", &app.showGrid);
    } else {
        ImGui::TextDisabled("no image");
    }

}

static void drawPanelHistogram() {
    ImageDoc* im = cur();
    if (im && im->w > 0 && im->h > 0) {
        recomputeHistogramIfNeeded(im);
        const App::HistState& H = app.hist;
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Text("Statistics");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", H.roiUsed ? "selected ROI" : "whole image");
        ImGui::Separator();
        if (ImGui::BeginTable("quickstats", 4, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("ch");
            ImGui::TableSetupColumn("mean");
            ImGui::TableSetupColumn("std");
            ImGui::TableSetupColumn("var");
            ImGui::TableHeadersRow();
            for (int s = 0; s < H.nSeries; s++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", H.seriesNames[s]);
                ImGui::TableNextColumn(); ImGui::Text("%.6g", H.mean[s]);
                ImGui::TableNextColumn(); ImGui::Text("%.6g", H.sd[s]);
                ImGui::TableNextColumn(); ImGui::Text("%.6g", H.sd[s] * H.sd[s]);
            }
            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::Text("Histogram");
        ImGui::SameLine();
        ImGui::Checkbox("log##hist", &app.histLog);
        ImGui::Separator();
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
        PlotRect hp = beginPlot(xl, yl, im->black, im->white, 0.0f, 1.0f, false, false,
                                110.0f * app.uiScale);
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

static void drawPanelTemporal() {
    ImageDoc* im = cur();
    if (im && im->seqId != 0) {
        recomputeTemporalIfNeeded();
        const App::TemporalState& T = app.temporal;
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Text("Temporal");
        ImGui::SameLine();
        ImGui::TextDisabled("(%d frames, %s)", T.frames,
                            T.roiUsed ? "selected ROI" : "whole image");
        ImGui::Separator();
        if (!T.valid) {
            ImGui::TextDisabled("need >= 2 loaded frames of equal size");
        } else {
            if (ImGui::BeginTable("temporalstats", 2, ImGuiTableFlags_SizingStretchProp)) {
                auto row = [](const char* k, double v) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
                    ImGui::TableNextColumn(); ImGui::Text("%.6g", v);
                };
                row("temporal noise (sigma_t)", T.tempNoise);
                row("fixed pattern (sigma_s)", T.fixedPattern);
                row("total (quadrature)", T.totalNoise);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("sigma_s includes scene detail unless the ROI is flat");
            // per-frame mean over time
            float mn = FLT_MAX, mx = -FLT_MAX;
            for (float v : T.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
            float fx0 = T.idx.empty() ? 0 : T.idx.front(), fx1 = T.idx.empty() ? 1 : T.idx.back();
            char yl[64];
            snprintf(yl, sizeof yl, "ROI mean value (%s)", im->dtype.c_str());
            PlotRect tp = beginPlot("frame number (index in sequence)", yl,
                                    fx0, fx1, mn, mx, true, false, 90.0f * app.uiScale);
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
static RoiStat roiBasicStats(const ImageDoc& im, int rx, int ry, int rw, int rh, int chSel) {
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
                               ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    float rowsH = ImGui::GetTextLineHeightWithSpacing() * 9;
    if (ImGui::BeginTable("roitable", 8, TF, ImVec2(0, rowsH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.4f);
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("region");
        ImGui::TableSetupColumn("mean");
        ImGui::TableSetupColumn("std");
        ImGui::TableSetupColumn("min");
        ImGui::TableSetupColumn("max");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.4f);
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
        for (auto& a : app.anns) {
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
    ImGui::BeginChild("roiedit", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.4f));
    if (App::Ann* sel = findAnn(app.selectedAnn)) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", sel->label.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##rename", buf, sizeof buf))
            sel->label = buf;                       // live update is cheap...
        if (ImGui::IsItemDeactivatedAfterEdit())
            app.annRev++;                           // ...but re-analyze only on commit
        if (sel->type == 0) {
            int v[4] = { sel->x, sel->y, sel->w, sel->h };
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt4("x,y,w,h", v)) {
                sel->x = std::clamp(v[0], 0, im->w - 1);
                sel->y = std::clamp(v[1], 0, im->h - 1);
                sel->w = std::clamp(v[2], 1, im->w - sel->x);
                sel->h = std::clamp(v[3], 1, im->h - sel->y);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) app.annRev++;
        } else {
            ImGui::TextDisabled("point annotation - move it with the Pin tool");
        }
    } else {
        ImGui::TextDisabled("target: whole image (%dx%d, %s)", im->w, im->h, im->dtype.c_str());
        ImGui::TextDisabled("draw a ROI with the R tool to measure a region");
    }
    ImGui::EndChild();
}

static void drawPanelAnalysis() {
    ImageDoc* im = cur();
    if (im && !plugin_host::analyzers().empty()) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Text("Analysis");
        ImGui::Separator();
        const auto& anas = plugin_host::analyzers();
        app.anaSel = std::clamp(app.anaSel, 0, (int)anas.size() - 1);
        std::string curCat, curItem;
        splitAnalyzerName(anas[app.anaSel].name, curCat, curItem);
        float cw = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(cw * 0.30f);
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
        ImGui::SetNextItemWidth(cw * 0.34f);
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

        std::vector<const App::Ann*> rois;
        for (const auto& a : app.anns)
            if (a.type == 0 && a.visible) rois.push_back(&a);
        if (rois.empty())
            ImGui::TextDisabled("target: whole image (R tool / Shift+drag = ROI)");
        else
            ImGui::TextDisabled("target: %d ROI(s), one column each", (int)rois.size());

        auto& ana = app.ana;
        bool stale = ana.img != im || ana.plugin != app.anaSel || ana.rev != app.annRev;
        if (runClicked || (app.anaAuto && stale && !app.annBusy)) {
            ana.cols.clear(); ana.keys.clear(); ana.vals.clear(); ana.series.clear(); ana.err.clear();
            ana.img = im; ana.plugin = app.anaSel; ana.rev = app.annRev;
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
                        for (auto& r : ana.vals) r.resize(ana.cols.size());
                        continue;
                    }
                    psRect rr = { (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh };
                    runOne(&rr, a->label, a->color & 7);
                }
            }
        }
        if (ana.img == im) {
            if (!ana.err.empty())
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ana.err.c_str());
            int nCols = 1 + (int)ana.cols.size();
            if (nCols > 16)
                ImGui::TextDisabled("too many ROI columns (>15): hide some ROIs to see the grid");
            if (!ana.keys.empty() && nCols <= 16 &&
                ImGui::BeginTable("anagrid", nCols,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("");
                for (const auto& cn : ana.cols) ImGui::TableSetupColumn(cn.c_str());
                ImGui::TableHeadersRow();
                for (int k = 0; k < (int)ana.keys.size(); k++) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ana.keys[k].c_str());
                    for (int c = 0; c < (int)ana.cols.size(); c++) {
                        ImGui::TableNextColumn();
                        if (c < (int)ana.vals[k].size() && !ana.vals[k][c].empty())
                            ImGui::TextUnformatted(ana.vals[k][c].c_str());
                        else
                            ImGui::TextDisabled("-");
                    }
                }
                ImGui::EndTable();
            }
            drawAnalysisPlots();   // curves from V2 analyzers (one plot per series name)
        }
    }
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
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) { app.seqCancel = true; app.seqQueue.clear(); }
    }
    ImGui::Separator();
    // grouped view: one node per stack (sequence), lone images stay flat
    for (const auto& stack : stacksOf()) {
        const ImageDoc& head = *app.images[stack.front()];
        // name and format share one row: the dim/format part is right-aligned and dimmed
        auto rowWithMeta = [](const ImageDoc& d, const char* label, bool selected,
                              const char* extra = nullptr) -> bool {
            char meta[96];
            snprintf(meta, sizeof meta, "%dx%d %dch %s%s", d.w, d.h, d.ch, d.dtype.c_str(),
                     extra ? extra : "");
            float metaW = ImGui::CalcTextSize(meta).x;
            float avail = ImGui::GetContentRegionAvail().x;
            bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowOverlap,
                                             ImVec2(std::max(avail - metaW - 8.0f, 40.0f), 0));
            ImGui::SameLine(std::max(avail - metaW, 60.0f));
            ImGui::TextDisabled("%s", meta);
            return clicked;
        };
        if (head.seqId == 0) {
            int i = stack.front();
            char lb[512];
            snprintf(lb, 512, "%s##%d", head.name.c_str(), i);
            if (rowWithMeta(head, lb, i == app.current, nullptr)) {
                selectImage(i); app.fitRequested = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", head.path.c_str());
            continue;
        }
        App::SeqInfo* si = seqInfo(head.seqId);
        int pos = 0;
        bool active = false;
        for (int k = 0; k < (int)stack.size(); k++)
            if (stack[k] == app.current) { pos = k; active = true; }
        ImGui::PushID(head.seqId);
        char lb[512];
        snprintf(lb, 512, "%s  [%d]", si ? si->name.c_str() : "sequence", (int)stack.size());
        if (rowWithMeta(head, lb, active)) { selectImage(stack[pos]); app.fitRequested = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", head.path.c_str());
        if (active) {
            int slider = pos;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##frame", &slider, 0, (int)stack.size() - 1, "frame %d")
                && slider != pos)
                selectImage(stack[slider]);
        }
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
        if (ImGui::MenuItem("Save Session...", SC_MOD "+S", false, !app.images.empty())) saveSessionDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Close Image", SC_MOD "+W", false, cur() != nullptr)) closeCurrent();
        if (ImGui::MenuItem("Close All", nullptr, false, !app.images.empty())) closeAll();
        ImGui::Separator();
        if (ImGui::BeginMenu("Sequence loading")) {
            if (ImGui::MenuItem("Ask each time", nullptr, app.seqLoadMode == 0)) app.seqLoadMode = 0;
            if (ImGui::MenuItem("Always load folder", nullptr, app.seqLoadMode == 1)) app.seqLoadMode = 1;
            if (ImGui::MenuItem("Never (single file)", nullptr, app.seqLoadMode == 2)) app.seqLoadMode = 2;
            ImGui::Separator();
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
        if (ImGui::MenuItem("Fit to Window", "F", false, has)) app.fitRequested = true;
        if (ImGui::MenuItem("Actual Size (100%)", "1", false, has)) app.view.zoom = 1.0f;
        if (ImGui::MenuItem("Zoom In", "+", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
        if (ImGui::MenuItem("Zoom Out", "-", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        ImGui::Separator();
        ImGui::MenuItem("Pixel Grid", "G", &app.showGrid);
        ImGui::MenuItem("Wheel zooms without Ctrl", nullptr, &app.wheelZoomPlain);
        ImGui::Separator();
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Files", nullptr, &app.showFiles);
            ImGui::MenuItem("Inspector", nullptr, &app.showInspector);
            ImGui::MenuItem("ROIs", nullptr, &app.showRois);
            ImGui::MenuItem("Analysis", nullptr, &app.showAnalysis);
            ImGui::MenuItem("Histogram", nullptr, &app.showHistogram);
            ImGui::MenuItem("Temporal", nullptr, &app.showTemporal);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                app.showFiles = app.showInspector = app.showRois = true;
                app.showAnalysis = app.showHistogram = app.showTemporal = true;
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
            if (changed) ui_theme::apply(app.themeVariant, app.themeAccent, app.uiScale);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (!plugin_host::analyzers().empty() && ImGui::BeginMenu("Measure")) {
        const auto& anas = plugin_host::analyzers();
        std::string lastCat;
        for (int i = 0; i < (int)anas.size(); i++) {
            std::string c, n;
            splitAnalyzerName(anas[i].name, c, n);
            if (c != lastCat) {
                if (!lastCat.empty()) ImGui::Separator();
                ImGui::TextDisabled("%s", c.c_str());
                lastCat = c;
            }
            const char* hint = !anas[i].desc.empty() ? anas[i].desc.c_str()
                                                     : analyzerHint(anas[i].name);
            if (ImGui::MenuItem(n.c_str(), hint[0] ? hint : nullptr,
                                app.anaSel == i, cur() != nullptr)) {
                app.anaSel = i;
                app.anaRunRequest = true;   // panel runs it this frame
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
                row("V / R / P",     "tool: Navigate / ROI / Pin");
                row("Ctrl+wheel",    "zoom at cursor (invertible in View menu)");
                row("wheel / Shift+wheel", "pan vertical / horizontal");
                row("middle-drag / Space+drag", "pan (works in any tool)");
                row("Shift+drag",    "quick ROI (Navigate tool)");
                row("Ctrl+click",    "quick pin (Navigate tool)");
                row("X / Y",         "toggle selected ROI full width / height (press again to restore)");
                row("Del / Esc",     "delete / deselect annotation");
                row("G",             "pixel grid (zoom >= 8x)");
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
static void printUsage() {
    printf(
        "usage: viewer [options] [files or folders...]\n"
        "  files:  .npy, .vsession (saved session), or raw binaries (.bin/.raw/.yuv/...)\n"
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
        "  --sequence <mode>           numbered siblings: ask (default) | always | never\n"
        "  --zoom <z>                  initial zoom (1 = 100%%)\n"
        "  --center <x,y>              initial view center in image pixels\n"
        "  -h, --help                  show this help\n");
}

static void parseCli(int argc, char** argv) {
    RawDialog d;                       // accumulates --raw-* options for positional raw files
    bool rawReady = false, cliQuad = false;
    bool haveZoom = false, haveCenter = false;
    float zoom = 1, cx = 0, cy = 0;
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
        } else if (a == "--sequence") {            // ask | always | never
            std::string v = next();
            if (v == "always") app.seqLoadMode = 1;
            else if (v == "never") app.seqLoadMode = 2;
            else if (v == "ask") app.seqLoadMode = 0;
            else fprintf(stderr, "--sequence expects ask|always|never\n");
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
    if (haveZoom || haveCenter) {
        if (haveZoom) app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        if (haveCenter) app.view.center = ImVec2(cx, cy);
        else if (cur()) app.view.center = ImVec2(cur()->w * 0.5f, cur()->h * 0.5f);
        app.fitRequested = false;      // explicit view from CLI wins over fit-on-load
    }
}

// ---------------------------------------------------------------- main
static void dropCallback(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++) openPath(paths[i]);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printUsage(); return 0; }
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
    GLFWwindow* win = glfwCreateWindow(1600, 1000, "viewer v0.1", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window creation failed\n"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetDropCallback(win, dropCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
    ui_theme::apply(app.themeVariant, app.themeAccent, uiScale);
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

    plugin_host::loadAll(
        { plugin_host::exeDir() + "/plugins", plugin_host::exeDir() + "/../plugins" },
        [](const std::string& m, bool err) {
            if (err) toast(m, true);
            fprintf(stderr, "%s\n", m.c_str());
        });

    parseCli(argc, argv);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        pumpSequenceAndQueue();       // integrate decoded frames, chain queued stacks
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

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
            // sequence start/end = C-a/C-e (always Ctrl, also on macOS)
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) gotoFrame(1);
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
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) openFileDialog();
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) app.showHelp = !app.showHelp;
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) app.tool = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) app.tool = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_P, false)) app.tool = 2;
            // X/Y: TOGGLE the selected ROI between its rect and full width / height.
            // Press once = row/column band, press again = restore the remembered rect.
            if (ImGui::IsKeyPressed(ImGuiKey_X, false) && cur()) {
                if (App::Ann* a = findAnn(app.selectedAnn))
                    if (a->type == 0) {
                        bool fullW = a->x == 0 && a->w == cur()->w;
                        if (fullW && a->prevW >= 1) {
                            a->x = a->prevX; a->w = a->prevW; a->prevW = -1;
                        } else if (!fullW) {
                            a->prevX = a->x; a->prevW = a->w;
                            a->x = 0; a->w = cur()->w;
                        }
                        app.annRev++;
                    }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && cur()) {
                if (App::Ann* a = findAnn(app.selectedAnn))
                    if (a->type == 0) {
                        bool fullH = a->y == 0 && a->h == cur()->h;
                        if (fullH && a->prevH >= 1) {
                            a->y = a->prevY; a->h = a->prevH; a->prevH = -1;
                        } else if (!fullH) {
                            a->prevY = a->y; a->prevH = a->h;
                            a->y = 0; a->h = cur()->h;
                        }
                        app.annRev++;
                    }
            }
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
            ImGui::DockBuilderDockWindow("Image View", center);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Histogram", bottom);
            ImGui::DockBuilderDockWindow("Temporal", bottom);
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
            snprintf(st, 512, "[%s]  no image", TOOL_NAMES[app.tool]);
            if (im) {
                std::string hover;
                if (app.hoverX >= 0) {
                    hover = "  |  (" + std::to_string(app.hoverX) + ", " + std::to_string(app.hoverY) + ")";
                    if (im->cfa)
                        hover += std::string(" [") + CFA_CH_NAMES[cfaChannelAt(*im, app.hoverX, app.hoverY)] + "]";
                    hover += " =";
                    for (int c = 0; c < im->ch; c++)
                        hover += " " + fmtVal(im->sample(app.hoverX, app.hoverY, c), im->dtype);
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
                snprintf(st, 512, "[%s]  %s%s   %dx%d %dch %s  |  zoom %s%s",
                         TOOL_NAMES[app.tool], im->name.c_str(), seqInfoStr, im->w, im->h, im->ch,
                         im->dtype.c_str(), zs, hover.c_str());
            }
            ImGui::TextUnformatted(st);
            static std::string lastTitle;
            std::string title = im ? im->name + " - viewer" : "viewer v0.1";
            if (title != lastTitle) { glfwSetWindowTitle(win, title.c_str()); lastTitle = title; }
        }

        drawRawModal();
        drawSequenceModal();
        drawFolderPickModal();
        drawHelpAbout();

        // toast
        if (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImVec2 ts = ImGui::CalcTextSize(app.toast.c_str());
            ImVec2 p(vp->WorkPos.x + (vp->WorkSize.x - ts.x) / 2, vp->WorkPos.y + vp->WorkSize.y - 60 * uiScale);
            fg->AddRectFilled(ImVec2(p.x - 12, p.y - 6), ImVec2(p.x + ts.x + 12, p.y + ts.y + 6),
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
        glfwSwapBuffers(win);
    }

    stopSequenceLoader();             // join the worker before tearing anything down
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    plugin_host::unloadAll();
    return 0;
}
