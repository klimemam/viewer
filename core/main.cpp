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
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"

#include <algorithm>
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
#include <string>
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

    float sample(int x, int y, int c) const { return data[((size_t)y * w + x) * ch + c]; }
};

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
    std::string toast; double toastUntil = 0; bool toastErr = false;
    bool fitRequested = false;
    std::unique_ptr<pfd::open_file> openDlg;   // polled each frame; never blocks render
    bool showHelp = false, showAbout = false;
    // hover state (image coords, -1 = none)
    int hoverX = -1, hoverY = -1;
};
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
    for (size_t p = 0; p < (size_t)im.w * im.h; p++) {
        const float* src = &im.data[p * im.ch];
        float r, g, b;
        if (im.ch == 1)      { r = g = b = src[0]; }
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
    if (!im.tex) glGenTextures(1, &im.tex);
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
static void closeCurrent() {
    ImageDoc* im = cur();
    if (!im) return;
    if (im->tex) glDeleteTextures(1, &im->tex);
    app.images.erase(app.images.begin() + app.current);
    app.current = app.images.empty() ? -1 : std::min(app.current, (int)app.images.size() - 1);
    app.fitRequested = true;
}
static void closeAll() {
    for (auto& d : app.images)
        if (d->tex) glDeleteTextures(1, &d->tex);
    app.images.clear();
    app.current = -1;
}

static void addImage(std::unique_ptr<ImageDoc> im) {
    computeMinMax(*im);
    defaultRange(*im);
    im->texDirty = true;
    app.images.push_back(std::move(im));
    app.current = (int)app.images.size() - 1;
    app.fitRequested = true;
}

// ---------------------------------------------------------------- npy loader
// returns error string, empty = ok
static std::string loadNpy(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read file";
    if (buf.size() < 10 || buf[0] != 0x93 || memcmp(&buf[1], "NUMPY", 5) != 0)
        return "not a .npy file (bad magic)";
    int major = buf[6];
    size_t hlen, hoff;
    if (major >= 2) {
        if (buf.size() < 12) return "corrupt npy header";
        uint32_t v; memcpy(&v, &buf[8], 4); hlen = v; hoff = 12;
    } else {
        uint16_t v; memcpy(&v, &buf[8], 2); hlen = v; hoff = 10;
    }
    if (hoff + hlen > buf.size()) return "corrupt npy header";
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
    if (descr.empty()) return "cannot parse descr";
    bool fortran = hdr.find("'fortran_order': True") != std::string::npos;

    size_t sp = hdr.find("'shape'");
    size_t p1 = hdr.find('(', sp), p2 = hdr.find(')', sp);
    if (p1 == std::string::npos || p2 == std::string::npos) return "cannot parse shape";
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
    else return "unsupported dtype: " + descr;

    size_t count = 1;
    for (int64_t d : shape) count *= (size_t)d;
    if (hoff + hlen + count * esize > buf.size()) return "file too small for shape";
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
        else return "shape not interpretable as image";
    }
    if (W < 1 || H < 1 || W > 32768 || H > 32768) return "unsupported image size";

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
    addImage(std::move(im));
    return {};
}

// ---------------------------------------------------------------- raw loader
struct RawFormat { const char* label; float bpp; int ch; };
static const RawFormat RAW_FORMATS[] = {
    { "Gray 8bit",    1,  1 }, { "Gray 16bit", 2, 1 }, { "Gray float32", 4, 1 }, { "Gray float64", 8, 1 },
    { "RGB 8bit",     3,  3 }, { "BGR 8bit",   3, 3 }, { "RGBA 8bit",    4, 4 }, { "BGRA 8bit",    4, 4 },
    { "RGB 16bit",    6,  3 }, { "RGB float32", 12, 3 },
};
enum RawFmtIdx { RF_G8, RF_G16, RF_GF32, RF_GF64, RF_RGB8, RF_BGR8, RF_RGBA8, RF_BGRA8, RF_RGB16, RF_RGBF32 };

struct RawDialog {
    bool open = false;
    std::string path;
    size_t fileSize = 0;
    int w = 1920, h = 1080, offset = 0, fmt = RF_G8;
    bool littleEndian = true;
    std::vector<std::pair<int,int>> guesses;
} rawDlg;

static void rawGuessDims(RawDialog& d) {
    d.guesses.clear();
    float bpp = RAW_FORMATS[d.fmt].bpp;
    size_t nbytes = d.fileSize > (size_t)d.offset ? d.fileSize - d.offset : 0;
    static const int commons[][2] = { {3840,2160},{1920,1080},{1280,720},{640,480},{512,512},{1024,1024},
        {2048,2048},{4096,4096},{256,256},{2560,1440},{720,480},{640,360},{320,240},{128,128} };
    for (auto& c : commons)
        if (fabs((double)c[0] * c[1] * bpp - (double)nbytes) < 0.5)
            d.guesses.push_back({ c[0], c[1] });
    double npx = nbytes / bpp;
    if (npx == floor(npx)) {
        int64_t n = (int64_t)npx;
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
}

static std::string loadRaw(const RawDialog& d) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(d.path, buf)) return "cannot read file";
    const RawFormat& F = RAW_FORMATS[d.fmt];
    size_t px = (size_t)d.w * d.h;
    size_t need = (size_t)ceil(px * F.bpp) + d.offset;
    if (need > buf.size()) return "file too small for this size/format";
    const uint8_t* p = buf.data() + d.offset;
    bool le = d.littleEndian;

    auto rd16 = [&](size_t o) -> uint16_t {
        return le ? (uint16_t)(p[o] | p[o + 1] << 8) : (uint16_t)(p[o] << 8 | p[o + 1]);
    };
    auto rdf32 = [&](size_t o) -> float {
        uint32_t u = le ? (uint32_t)p[o] | p[o+1]<<8 | p[o+2]<<16 | (uint32_t)p[o+3]<<24
                        : (uint32_t)p[o]<<24 | p[o+1]<<16 | p[o+2]<<8 | p[o+3];
        float f; memcpy(&f, &u, 4); return f;
    };
    auto rdf64 = [&](size_t o) -> double {
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u |= (uint64_t)p[o + i] << (8 * (le ? i : 7 - i));
        double f; memcpy(&f, &u, 8); return f;
    };

    auto im = std::make_unique<ImageDoc>();
    im->name = baseName(d.path); im->path = d.path;
    im->w = d.w; im->h = d.h; im->ch = F.ch;
    im->note = F.label;
    im->data.resize(px * F.ch);
    float* out = im->data.data();
    switch (d.fmt) {
    case RF_G8:   im->dtype = "u8";  for (size_t i = 0; i < px; i++) out[i] = p[i]; break;
    case RF_G16:  im->dtype = "u16"; for (size_t i = 0; i < px; i++) out[i] = rd16(i * 2); break;
    case RF_GF32: im->dtype = "f32"; for (size_t i = 0; i < px; i++) out[i] = rdf32(i * 4); break;
    case RF_GF64: im->dtype = "f64"; for (size_t i = 0; i < px; i++) out[i] = (float)rdf64(i * 8); break;
    case RF_RGB8: case RF_BGR8: {
        im->dtype = "u8";
        bool sw = d.fmt == RF_BGR8;
        for (size_t i = 0; i < px; i++) {
            out[i * 3]     = p[i * 3 + (sw ? 2 : 0)];
            out[i * 3 + 1] = p[i * 3 + 1];
            out[i * 3 + 2] = p[i * 3 + (sw ? 0 : 2)];
        }
        break;
    }
    case RF_RGBA8: case RF_BGRA8: {
        im->dtype = "u8";
        bool sw = d.fmt == RF_BGRA8;
        for (size_t i = 0; i < px; i++) {
            out[i * 4]     = p[i * 4 + (sw ? 2 : 0)];
            out[i * 4 + 1] = p[i * 4 + 1];
            out[i * 4 + 2] = p[i * 4 + (sw ? 0 : 2)];
            out[i * 4 + 3] = p[i * 4 + 3];
        }
        break;
    }
    case RF_RGB16:  im->dtype = "u16"; for (size_t i = 0; i < px * 3; i++) out[i] = rd16(i * 2); break;
    case RF_RGBF32: im->dtype = "f32"; for (size_t i = 0; i < px * 3; i++) out[i] = rdf32(i * 4); break;
    }
    addImage(std::move(im));
    return {};
}

// ---------------------------------------------------------------- open dispatch
static void openRawDialogFor(const std::string& path) {
    std::vector<uint8_t> probe;   // just get size cheaply
    std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) { toast("cannot open: " + baseName(path), true); return; }
    rawDlg.open = true;
    rawDlg.path = path;
    rawDlg.fileSize = (size_t)f.tellg();
    // guess WxH from filename like foo_1920x1080.raw
    std::string n = baseName(path);
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
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".npy") == 0) {
        std::string err = loadNpy(path);
        if (!err.empty()) toast(baseName(path) + ": " + err, true);
        else toast("loaded " + baseName(path));
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
    app.openDlg = std::make_unique<pfd::open_file>("Open image", "",
        std::vector<std::string>{ "Images (*.npy *.bin *.raw *.yuv *.dat)", "*.npy *.bin *.raw *.yuv *.dat",
          "All files", "*" },
        pfd::opt::multiselect);
}
static void pollFileDialog() {           // called once per frame from the main loop
    if (app.openDlg && app.openDlg->ready(0)) {
        for (const std::string& p : app.openDlg->result()) openPath(p);
        app.openDlg.reset();
    }
}

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

    ImGui::Text("%s  (%zu bytes)", baseName(rawDlg.path).c_str(), rawDlg.fileSize);
    ImGui::Separator();
    int prevFmt = rawDlg.fmt, prevOff = rawDlg.offset;
    if (ImGui::BeginCombo("format", RAW_FORMATS[rawDlg.fmt].label)) {
        for (int i = 0; i < (int)(sizeof(RAW_FORMATS) / sizeof(RAW_FORMATS[0])); i++)
            if (ImGui::Selectable(RAW_FORMATS[i].label, i == rawDlg.fmt)) rawDlg.fmt = i;
        ImGui::EndCombo();
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
    ImGui::InputInt("width", &rawDlg.w);
    ImGui::InputInt("height", &rawDlg.h);
    ImGui::InputInt("offset (bytes)", &rawDlg.offset);
    rawDlg.w = std::clamp(rawDlg.w, 1, 32768);
    rawDlg.h = std::clamp(rawDlg.h, 1, 32768);
    rawDlg.offset = std::max(0, rawDlg.offset);
    ImGui::Checkbox("little endian", &rawDlg.littleEndian);
    if (rawDlg.fmt != prevFmt || rawDlg.offset != prevOff) rawGuessDims(rawDlg);

    size_t need = (size_t)ceil((double)rawDlg.w * rawDlg.h * RAW_FORMATS[rawDlg.fmt].bpp) + rawDlg.offset;
    if (need > rawDlg.fileSize)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "need %zu bytes > file %zu bytes", need, rawDlg.fileSize);
    else if (need < rawDlg.fileSize)
        ImGui::TextDisabled("need %zu bytes (%zu bytes unused)", need, rawDlg.fileSize - need);
    else
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "size matches exactly");

    ImGui::Separator();
    bool ok = ImGui::Button("Load", ImVec2(120 * app.uiScale, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) ImGui::CloseCurrentPopup();
    if (ok) {
        std::string err = loadRaw(rawDlg);
        if (!err.empty()) toast(err, true);
        else { toast("loaded " + baseName(rawDlg.path)); ImGui::CloseCurrentPopup(); }
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
    ImGui::InvisibleButton("canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
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

    if (im) {
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
            app.view.center.x -= io.MouseDelta.x / app.view.zoom;
            app.view.center.y -= io.MouseDelta.y / app.view.zoom;
        }
        if (hovered && io.MouseWheelH != 0)      // trackpad horizontal scroll = pan
            app.view.center.x += io.MouseWheelH * 40.0f / app.view.zoom;
        if (hovered && io.MouseWheel != 0) {
            // clamp: macOS trackpads deliver large fractional inertial deltas
            float wheel = std::clamp(io.MouseWheel, -3.0f, 3.0f);
            ImVec2 mImg = scrToImg(io.MousePos);
            float z = std::clamp(app.view.zoom * powf(1.25f, wheel), 1.0f / 512, 256.0f);
            app.view.center.x = mImg.x - (io.MousePos.x - canvasP0.x - canvasSize.x * 0.5f) / z;
            app.view.center.y = mImg.y - (io.MousePos.y - canvasP0.y - canvasSize.y * 0.5f) / z;
            app.view.zoom = z;
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
}

static void drawInspector() {
    ImageDoc* im = cur();
    ImGui::Text("Pixel");
    ImGui::Separator();
    if (im && app.hoverX >= 0) {
        ImGui::Text("(%d, %d)", app.hoverX, app.hoverY);
        static const char* LB1[] = { "V" };
        static const char* LB2[] = { "C0", "C1" };            // 2ch is usually UV/complex, not RG
        static const char* LB3[] = { "R", "G", "B", "A" };
        const char** lb = im->ch == 1 ? LB1 : im->ch == 2 ? LB2 : LB3;
        float inv = 1.0f / std::max(im->white - im->black, 1e-20f);
        if (ImGui::BeginTable("px", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("ch"); ImGui::TableSetupColumn("raw"); ImGui::TableSetupColumn("norm");
            ImGui::TableHeadersRow();
            for (int c = 0; c < im->ch; c++) {
                float v = im->sample(app.hoverX, app.hoverY, c);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c < 4 ? lb[std::min(c, 3)] : "?");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtVal(v, im->dtype).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.4f", (v - im->black) * inv);
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("hover the image");
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Text("Image");
    ImGui::Separator();
    if (im) {
        ImGui::Text("%d x %d   %dch   %s", im->w, im->h, im->ch, im->dtype.c_str());
        if (!im->note.empty()) ImGui::TextDisabled("%s", im->note.c_str());
        ImGui::Text("min %s / max %s", fmtVal(im->vmin, im->dtype).c_str(), fmtVal(im->vmax, im->dtype).c_str());

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

static void drawFileList() {
    if (ImGui::Button("Open (O)")) openFileDialog();
    ImGui::SameLine();
    if (ImGui::Button("Close")) closeCurrent();
    ImGui::Separator();
    for (int i = 0; i < (int)app.images.size(); i++) {
        ImageDoc& d = *app.images[i];
        char lb[512];
        snprintf(lb, 512, "%s##%d", d.name.c_str(), i);
        if (ImGui::Selectable(lb, i == app.current)) { app.current = i; app.fitRequested = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.path.c_str());
        ImGui::TextDisabled("   %d x %d  %dch  %s", d.w, d.h, d.ch, d.dtype.c_str());
    }
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
        ImGui::Separator();
        if (ImGui::MenuItem("Close Image", SC_MOD "+W", false, cur() != nullptr)) closeCurrent();
        if (ImGui::MenuItem("Close All", nullptr, false, !app.images.empty())) closeAll();
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
        if (ImGui::MenuItem("Fit to Window", "F", false, has)) app.fitRequested = true;
        if (ImGui::MenuItem("Actual Size (100%)", "1", false, has)) app.view.zoom = 1.0f;
        if (ImGui::MenuItem("Zoom In", "+", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
        if (ImGui::MenuItem("Zoom Out", "-", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        ImGui::Separator();
        ImGui::MenuItem("Pixel Grid", "G", &app.showGrid);
        if (ImGui::BeginMenu("Display Gamma")) {
            bool lin = app.dispGamma < 1.5f;
            if (ImGui::MenuItem("1.0 (linear)", nullptr, lin) && !lin) { app.dispGamma = 1.0f; markAllTexDirty(); }
            if (ImGui::MenuItem("2.2", nullptr, !lin) && lin)         { app.dispGamma = 2.2f; markAllTexDirty(); }
            ImGui::EndMenu();
        }
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
                row(SC_MOD "+O / O", "open files");
                row(SC_MOD "+W",     "close current image");
                row("F / double-click", "fit to window");
                row("1",             "actual size (100%)");
                row("+ / -",         "zoom in / out");
                row("wheel",         "zoom at cursor");
                row("drag",          "pan");
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

// ---------------------------------------------------------------- main
static void dropCallback(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++) openPath(paths[i]);
}

int main(int argc, char** argv) {
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
    io.IniFilename = nullptr;

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
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(uiScale);
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

    for (int i = 1; i < argc; i++) openPath(argv[i]);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
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
        if (!io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_O)) openFileDialog();
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_W)) closeCurrent();
        }
        if (!io.WantTextInput && io.KeyMods == ImGuiMod_None) {   // plain keys, no chords
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) app.fitRequested = true;
            if (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad1, false))
                app.view.zoom = 1.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) app.showGrid = !app.showGrid;
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) openFileDialog();
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) app.showHelp = !app.showHelp;
            if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        }
        drawMenuBar(win);

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoSavedSettings);

        const float LEFT_W = 230 * uiScale, RIGHT_W = 300 * uiScale, STATUS_H = 26 * uiScale;
        ImVec2 total = ImGui::GetContentRegionAvail();

        ImGui::BeginChild("left", ImVec2(LEFT_W, total.y - STATUS_H), ImGuiChildFlags_Borders);
        drawFileList();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("view", ImVec2(total.x - LEFT_W - RIGHT_W - 16 * uiScale, total.y - STATUS_H));
        drawCanvas(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, total.y - STATUS_H), ImGuiChildFlags_Borders);
        drawInspector();
        ImGui::EndChild();

        // status bar
        {
            ImageDoc* im = cur();
            char st[512] = "no image";
            if (im) {
                std::string hover;
                if (app.hoverX >= 0) {
                    hover = "  |  (" + std::to_string(app.hoverX) + ", " + std::to_string(app.hoverY) + ") =";
                    for (int c = 0; c < im->ch; c++)
                        hover += " " + fmtVal(im->sample(app.hoverX, app.hoverY, c), im->dtype);
                }
                char zs[32];
                if (app.view.zoom >= 0.095f) snprintf(zs, 32, "%.0f%%", app.view.zoom * 100);
                else                         snprintf(zs, 32, "%.3g%%", app.view.zoom * 100);
                snprintf(st, 512, "%s   %dx%d %dch %s  |  zoom %s%s",
                         im->name.c_str(), im->w, im->h, im->ch, im->dtype.c_str(),
                         zs, hover.c_str());
            }
            ImGui::TextUnformatted(st);
            static std::string lastTitle;
            std::string title = im ? im->name + " - viewer" : "viewer v0.1";
            if (title != lastTitle) { glfwSetWindowTitle(win, title.c_str()); lastTitle = title; }
        }

        drawRawModal();
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
        glClearColor(0.06f, 0.07f, 0.08f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
