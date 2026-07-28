// Rasterizer for the mark described in app_icon.h.
//
// The mark is what this tool does, in two elements: a 2x2 CFA quad (R/Gr/Gb/B -
// the thing a JPEG viewer cannot show you) inside a rounded frame. The frame is
// the only colored outline, so it survives being scaled to a 16 px taskbar
// button and is the element the Remote variant repaints.
//
// No AA library: shapes are point-sampled on a 4x grid and box-filtered down,
// which is exact enough for a mark made of rounded rectangles and keeps this
// file free of dependencies (tools/mkicon.cpp links it as a host tool, and the
// remote peer must never grow a GL/UI dependency).
#include "app_icon.h"

#include <algorithm>
#include <cmath>

namespace app_icon {
namespace {

constexpr int SS = 4;   // supersamples per axis

struct RGBA { float r, g, b, a; };

struct Buf {
    int n;
    std::vector<float> c;                       // premultiplied rgba
    explicit Buf(int n_) : n(n_), c((size_t)n_ * n_ * 4, 0.0f) {}
    void over(int x, int y, RGBA s) {
        float* d = &c[((size_t)y * n + x) * 4];
        const float ia = 1.0f - s.a;
        d[0] = s.r * s.a + d[0] * ia;
        d[1] = s.g * s.a + d[1] * ia;
        d[2] = s.b * s.a + d[2] * ia;
        d[3] = s.a       + d[3] * ia;
    }
};

// Distance test for a rounded rectangle in normalized (0..1) coordinates.
bool inRound(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    const float cx = std::min(std::max(x, x0 + r), x1 - r);
    const float cy = std::min(std::max(y, y0 + r), y1 - r);
    const float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

void fillRound(Buf& b, float x0, float y0, float x1, float y1, float r, RGBA col) {
    for (int py = 0; py < b.n; py++)
        for (int px = 0; px < b.n; px++) {
            const float u = (px + 0.5f) / b.n, v = (py + 0.5f) / b.n;
            if (inRound(u, v, x0, y0, x1, y1, r)) b.over(px, py, col);
        }
}

// Ring of thickness t inside the given rounded rect (drawn on top of the fill,
// so the frame reads as a border rather than a second silhouette).
void strokeRound(Buf& b, float x0, float y0, float x1, float y1, float r, float t, RGBA col) {
    const float ir = std::max(r - t, 0.0f);
    for (int py = 0; py < b.n; py++)
        for (int px = 0; px < b.n; px++) {
            const float u = (px + 0.5f) / b.n, v = (py + 0.5f) / b.n;
            if (inRound(u, v, x0, y0, x1, y1, r) &&
                !inRound(u, v, x0 + t, y0 + t, x1 - t, y1 - t, ir))
                b.over(px, py, col);
        }
}

RGBA mix(RGBA a, RGBA b, float k) {
    return RGBA{a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k,
                a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k};
}

// Palette. The frame colors are the two the UI already uses for this
// distinction: ui_theme's Aurora Blue accent, and the green the status bar
// paints the link indicator with while a peer is connected.
constexpr RGBA PANEL  = {0.086f, 0.098f, 0.137f, 1.0f};
constexpr RGBA BLUE   = {0.420f, 0.550f, 1.000f, 1.0f};
constexpr RGBA GREEN  = {0.400f, 0.840f, 0.520f, 1.0f};
constexpr RGBA CFA_R  = {0.882f, 0.341f, 0.290f, 1.0f};
constexpr RGBA CFA_GR = {0.494f, 0.788f, 0.416f, 1.0f};
constexpr RGBA CFA_GB = {0.353f, 0.624f, 0.333f, 1.0f};
constexpr RGBA CFA_B  = {0.310f, 0.490f, 0.882f, 1.0f};

}  // namespace

std::vector<unsigned char> render(int size, Variant v) {
    size = std::max(size, 8);
    Buf b(size * SS);

    const RGBA accent = (v == Remote) ? GREEN : BLUE;
    // a remote window is tinted, not recolored: the panel picks up a hint of the
    // frame so the two variants differ even where the frame is clipped away
    const RGBA panel = mix(PANEL, accent, v == Remote ? 0.10f : 0.04f);

    const float x0 = 0.035f, x1 = 0.965f, r = 0.235f, t = 0.065f;
    fillRound(b, x0, x0, x1, x1, r, panel);

    // 2x2 CFA quad, RGGB - the pattern the viewer opens by default
    const float q0 = 0.205f, q1 = 0.795f, gap = 0.040f, cr = 0.050f;
    const float mid = (q0 + q1) * 0.5f, cw = (q1 - q0 - gap) * 0.5f;
    const RGBA cell[4] = {CFA_R, CFA_GR, CFA_GB, CFA_B};
    for (int i = 0; i < 4; i++) {
        const float cx = (i & 1) ? mid + gap * 0.5f : q0;
        const float cy = (i & 2) ? mid + gap * 0.5f : q0;
        fillRound(b, cx, cy, cx + cw, cy + cw, cr, cell[i]);
    }

    strokeRound(b, x0, x0, x1, x1, r, t, accent);

    // box filter back down, and undo the premultiply: GLFW and PNG both want
    // straight alpha
    std::vector<unsigned char> out((size_t)size * size * 4, 0);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    const float* s = &b.c[(((size_t)(y * SS + sy)) * b.n + (x * SS + sx)) * 4];
                    for (int k = 0; k < 4; k++) acc[k] += s[k];
                }
            const float inv = 1.0f / (SS * SS);
            for (int k = 0; k < 4; k++) acc[k] *= inv;
            unsigned char* d = &out[((size_t)y * size + x) * 4];
            const float a = acc[3];
            for (int k = 0; k < 3; k++) {
                const float c = a > 1e-6f ? acc[k] / a : 0.0f;
                d[k] = (unsigned char)std::lround(std::min(std::max(c, 0.0f), 1.0f) * 255.0f);
            }
            d[3] = (unsigned char)std::lround(std::min(std::max(a, 0.0f), 1.0f) * 255.0f);
        }
    return out;
}

}  // namespace app_icon
