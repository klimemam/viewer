/* analyzer_prnu.c — flat-field non-uniformity analysis (解析系), CFA-aware.
 * Feed it a defocused flat field (bright, unsaturated). Per channel it reports:
 *   mean        - signal level
 *   prnu_pct    - std of the high-pass residual / mean * 100  (pixel-level gain
 *                 non-uniformity; single-frame, so temporal noise is included --
 *                 average several frames upstream for EMVA-strict numbers)
 *   row_fpn_pct - std of detrended row means / mean * 100 (horizontal banding)
 *   col_fpn_pct - std of detrended column means / mean * 100 (vertical banding)
 *   shading_pct - peak-to-peak of the low-pass plane / mean * 100 */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RADIUS 4          /* 9-tap box for the low-pass */
#define MAX_BUCKETS 4
#define MAX_PLANE_PX (16u * 1024u * 1024u)

static const int CFA_MAP[4][4] = { {0,1,2,3},{3,2,1,0},{1,0,3,2},{2,3,0,1} };
static const char* CFA_NAMES[4] = { "R", "Gr", "Gb", "B" };
static const char* RGB_NAMES[4] = { "ch0", "ch1", "ch2", "ch3" };

/* separable box blur with edge clamp; tmp must hold w*h floats */
static void boxblur2d(const float* src, float* dst, float* tmp, int w, int h) {
    int x, y, k;
    for (y = 0; y < h; y++) {          /* horizontal pass -> tmp */
        const float* s = src + (size_t)y * w;
        float* t = tmp + (size_t)y * w;
        for (x = 0; x < w; x++) {
            double a = 0;
            for (k = -RADIUS; k <= RADIUS; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                a += s[xx];
            }
            t[x] = (float)(a / (2 * RADIUS + 1));
        }
    }
    for (y = 0; y < h; y++) {          /* vertical pass -> dst */
        float* d = dst + (size_t)y * w;
        for (x = 0; x < w; x++) {
            double a = 0;
            for (k = -RADIUS; k <= RADIUS; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                a += tmp[(size_t)yy * w + x];
            }
            d[x] = (float)(a / (2 * RADIUS + 1));
        }
    }
}

static void boxblur1d(const float* src, float* dst, int n) {
    int i, k;
    for (i = 0; i < n; i++) {
        double a = 0;
        for (k = -RADIUS; k <= RADIUS; k++) {
            int j = i + k;
            if (j < 0) j = 0;
            if (j >= n) j = n - 1;
            a += src[j];
        }
        dst[i] = (float)(a / (2 * RADIUS + 1));
    }
}

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink* sink, char* err, size_t err_cap) {
    psRect r;
    int cfa, quad, nb, b;
    const char** names;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU) {
        snprintf(err, err_cap, "prnu: expected CPU f32 frame");
        return 1;
    }
    r.x = 0; r.y = 0; r.w = in->w; r.h = in->h;
    if (roi) {
        r = *roi;
        if (r.x >= in->w || r.y >= in->h || r.w == 0 || r.h == 0) {
            snprintf(err, err_cap, "prnu: ROI outside frame");
            return 1;
        }
        if (r.x + r.w > in->w) r.w = in->w - r.x;
        if (r.y + r.h > in->h) r.h = in->h - r.y;
    }
    cfa = in->ch == 1 && in->cfa_type != PS_CFA_NONE;
    quad = in->cfa_type == PS_CFA_QUAD;
    nb = cfa ? 4 : (int)(in->ch < MAX_BUCKETS ? in->ch : MAX_BUCKETS);
    names = cfa ? CFA_NAMES : RGB_NAMES;

    for (b = 0; b < nb; b++) {
        /* ---- extract the channel plane (subsampled grid for CFA) ----
         * ROI bounds are converted to int once so the phase arithmetic below
         * stays signed (mixing uint32_t with int promotes to unsigned). */
        const int rx0 = (int)r.x, ry0 = (int)r.y;
        const int rx1 = (int)(r.x + r.w), ry1 = (int)(r.y + r.h);
        int stepX = 1, stepY = 1, firstX = rx0, firstY = ry0;
        int pw, ph, x, y;
        float *plane, *lp, *tmp;
        char key[48];
        if (cfa) {
            int bx = 0, by = 0, i;
            for (i = 0; i < 4; i++)
                if (CFA_MAP[in->cfa_pattern & 3][i] == b) { by = i / 2; bx = i % 2; }
            if (quad) {
                stepX = stepY = 4;
                for (i = 0; i < 4; i++)      /* first x with matching cell, block-aligned */
                    if ((((rx0 + i) >> 1) & 1) == bx && ((rx0 + i) & 1) == 0) { firstX = rx0 + i; break; }
                for (i = 0; i < 4; i++)
                    if ((((ry0 + i) >> 1) & 1) == by && ((ry0 + i) & 1) == 0) { firstY = ry0 + i; break; }
            } else {
                stepX = stepY = 2;
                firstX = rx0 + ((bx - (rx0 & 1)) & 1);
                firstY = ry0 + ((by - (ry0 & 1)) & 1);
            }
        }
        pw = (rx1 - firstX + stepX - 1) / stepX;
        ph = (ry1 - firstY + stepY - 1) / stepY;
        if (pw < 2 * RADIUS + 2 || ph < 2 * RADIUS + 2) {
            snprintf(key, sizeof key, "%s", names[b]);
            sink->emit_text(sink->ctx, key, "region too small");
            continue;
        }
        if ((size_t)pw * ph > MAX_PLANE_PX) {
            snprintf(err, err_cap, "prnu: region too large (use an ROI up to ~16Mpx per channel)");
            return 1;
        }
        plane = (float*)malloc((size_t)pw * ph * sizeof(float));
        lp = (float*)malloc((size_t)pw * ph * sizeof(float));
        tmp = (float*)malloc((size_t)pw * ph * sizeof(float));
        if (!plane || !lp || !tmp) {
            free(plane); free(lp); free(tmp);
            snprintf(err, err_cap, "prnu: out of memory");
            return 1;
        }
        for (y = 0; y < ph; y++) {
            int sy = firstY + y * stepY;
            const float* row = (const float*)((const char*)in->data + (size_t)sy * in->pitch_bytes);
            for (x = 0; x < pw; x++) {
                int sx = firstX + x * stepX;
                float v = cfa ? row[sx] : row[(size_t)sx * in->ch + b];
                plane[(size_t)y * pw + x] = isfinite(v) ? v : 0.0f;
            }
        }

        {
            double mean = 0, resVar = 0, lpMin, lpMax;
            double rowFpn = 0, colFpn = 0;
            size_t n = (size_t)pw * ph, i;
            for (i = 0; i < n; i++) mean += plane[i];
            mean /= (double)n;
            snprintf(key, sizeof key, "%s.mean", names[b]);
            sink->emit_number(sink->ctx, key, mean);
            if (mean <= 0) {
                snprintf(key, sizeof key, "%s", names[b]);
                sink->emit_text(sink->ctx, key, "mean <= 0: not a flat field?");
                free(plane); free(lp); free(tmp);
                continue;
            }
            /* high-pass residual -> PRNU%; low-pass extrema -> shading% */
            boxblur2d(plane, lp, tmp, pw, ph);
            lpMin = lpMax = lp[0];
            for (i = 0; i < n; i++) {
                double d = plane[i] - lp[i];
                resVar += d * d;
                if (lp[i] < lpMin) lpMin = lp[i];
                if (lp[i] > lpMax) lpMax = lp[i];
            }
            resVar /= (double)n;
            snprintf(key, sizeof key, "%s.prnu_pct", names[b]);
            sink->emit_number(sink->ctx, key, sqrt(resVar) / mean * 100.0);
            snprintf(key, sizeof key, "%s.shading_pct", names[b]);
            sink->emit_number(sink->ctx, key, (lpMax - lpMin) / mean * 100.0);

            /* banding: std of detrended row/column means (reuse tmp as scratch) */
            {
                float* m1 = tmp;             /* means   */
                float* m2 = tmp + (ph > pw ? ph : pw);   /* detrend */
                for (y = 0; y < ph; y++) {
                    double a = 0;
                    for (x = 0; x < pw; x++) a += plane[(size_t)y * pw + x];
                    m1[y] = (float)(a / pw);
                }
                boxblur1d(m1, m2, ph);
                for (y = 0; y < ph; y++) { double d = m1[y] - m2[y]; rowFpn += d * d; }
                rowFpn = sqrt(rowFpn / ph);
                for (x = 0; x < pw; x++) {
                    double a = 0;
                    for (y = 0; y < ph; y++) a += plane[(size_t)y * pw + x];
                    m1[x] = (float)(a / ph);
                }
                boxblur1d(m1, m2, pw);
                for (x = 0; x < pw; x++) { double d = m1[x] - m2[x]; colFpn += d * d; }
                colFpn = sqrt(colFpn / pw);
            }
            snprintf(key, sizeof key, "%s.row_fpn_pct", names[b]);
            sink->emit_number(sink->ctx, key, rowFpn / mean * 100.0);
            snprintf(key, sizeof key, "%s.col_fpn_pct", names[b]);
            sink->emit_number(sink->ctx, key, colFpn / mean * 100.0);
        }
        free(plane); free(lp); free(tmp);
    }
    sink->emit_text(sink->ctx, "method",
                    "single-frame: prnu_pct includes temporal noise (average frames for EMVA-strict)");
    return 0;
}

static const psAnalyzerV1 DESC = { 1u, PS_CAP_CPU, "uniformity/prnu-fpn", analyze, {0} };

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < 1u) return 1;
    return host->register_analyzer(host->ctx, &DESC);
}
