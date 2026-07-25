/* analyzer_sharpness.c — focus / sharpness metrics (解析系).
 * varlap    - variance of the 4-neighbor Laplacian (classic focus measure)
 * tenengrad - mean squared Sobel gradient magnitude
 * grad_mean - mean absolute Sobel gradient
 * Computed on luma (0.2126R+0.7152G+0.0722B) or the single channel. Values are
 * relative: compare across shots of the same scene / normalization. */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink* sink, char* err, size_t err_cap) {
    psRect r;
    int pw, ph, x, y;
    float* luma;
    double lapSum = 0, lapSum2 = 0, tgSum = 0, gSum = 0;
    size_t n = 0;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU) {
        snprintf(err, err_cap, "sharpness: expected CPU f32 frame");
        return 1;
    }
    r.x = 0; r.y = 0; r.w = in->w; r.h = in->h;
    if (roi) {
        r = *roi;
        if (r.x >= in->w || r.y >= in->h || r.w == 0 || r.h == 0) {
            snprintf(err, err_cap, "sharpness: ROI outside frame");
            return 1;
        }
        if (r.x + r.w > in->w) r.w = in->w - r.x;
        if (r.y + r.h > in->h) r.h = in->h - r.y;
    }
    pw = (int)r.w; ph = (int)r.h;
    if (pw < 3 || ph < 3) {
        snprintf(err, err_cap, "sharpness: region too small (need >= 3x3)");
        return 1;
    }
    if ((size_t)pw * ph > 64u * 1024u * 1024u) {
        snprintf(err, err_cap, "sharpness: region too large (use an ROI)");
        return 1;
    }
    luma = (float*)malloc((size_t)pw * ph * sizeof(float));
    if (!luma) { snprintf(err, err_cap, "sharpness: out of memory"); return 1; }
    for (y = 0; y < ph; y++) {
        const float* row = (const float*)((const char*)in->data +
                                          (size_t)(r.y + y) * in->pitch_bytes);
        for (x = 0; x < pw; x++) {
            const float* p = row + (size_t)(r.x + x) * in->ch;
            float v = in->ch >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[0];
            luma[(size_t)y * pw + x] = isfinite(v) ? v : 0.0f;
        }
    }
    for (y = 1; y < ph - 1; y++) {
        const float* r0 = luma + (size_t)(y - 1) * pw;
        const float* r1 = luma + (size_t)y * pw;
        const float* r2 = luma + (size_t)(y + 1) * pw;
        for (x = 1; x < pw - 1; x++) {
            double lap = 4.0 * r1[x] - r1[x - 1] - r1[x + 1] - r0[x] - r2[x];
            double gx = (r0[x + 1] + 2.0 * r1[x + 1] + r2[x + 1]) -
                        (r0[x - 1] + 2.0 * r1[x - 1] + r2[x - 1]);
            double gy = (r2[x - 1] + 2.0 * r2[x] + r2[x + 1]) -
                        (r0[x - 1] + 2.0 * r0[x] + r0[x + 1]);
            double g2 = gx * gx + gy * gy;
            lapSum += lap; lapSum2 += lap * lap;
            tgSum += g2; gSum += sqrt(g2);
            n++;
        }
    }
    free(luma);
    if (!n) { snprintf(err, err_cap, "sharpness: no samples"); return 1; }
    {
        double lm = lapSum / (double)n;
        double lv = lapSum2 / (double)n - lm * lm;
        sink->emit_number(sink->ctx, "varlap", lv > 0 ? lv : 0);
        sink->emit_number(sink->ctx, "tenengrad", tgSum / (double)n);
        sink->emit_number(sink->ctx, "grad_mean", gSum / (double)n);
    }
    if (in->ch == 1 && in->cfa_type != PS_CFA_NONE)
        sink->emit_text(sink->ctx, "note", "CFA mosaic inflates gradients: demosaic first for absolute numbers");
    return 0;
}

static const psAnalyzerV1 DESC = { 1u, PS_CAP_CPU, "sharpness/gradient", analyze, {0} };

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < 1u) return 1;
    return host->register_analyzer(host->ctx, &DESC);
}
