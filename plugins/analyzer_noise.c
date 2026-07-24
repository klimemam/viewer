/* analyzer_noise.c — noise analysis (解析系):
 * per-channel (CFA-aware) mean / spatial std / tile-median noise floor / SNR.
 * The tile-median estimator is robust against texture: the image is split into
 * 16x16 tiles, each tile's std is computed, and the median tile std is reported
 * as the noise floor (textured tiles inflate std and land in the upper tail). */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE 16
#define MAX_BUCKETS 4

static const int CFA_MAP[4][4] = { {0,1,2,3},{3,2,1,0},{1,0,3,2},{2,3,0,1} };
static const char* CFA_NAMES[4] = { "R", "Gr", "Gb", "B" };
static const char* RGB_NAMES[4] = { "ch0", "ch1", "ch2", "ch3" };

static int cfa_channel(const psFrame* f, uint32_t x, uint32_t y) {
    int cx = f->cfa_type == PS_CFA_QUAD ? (int)((x >> 1) & 1) : (int)(x & 1);
    int cy = f->cfa_type == PS_CFA_QUAD ? (int)((y >> 1) & 1) : (int)(y & 1);
    return CFA_MAP[f->cfa_pattern & 3][cy * 2 + cx];
}

static int cmpf(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink* sink, char* err, size_t err_cap) {
    psRect r;
    int cfa, nb, b;
    uint32_t tilesX, tilesY;
    float* tileStd[MAX_BUCKETS];
    size_t tileCnt[MAX_BUCKETS];
    double gSum[MAX_BUCKETS], gSum2[MAX_BUCKETS];
    size_t gN[MAX_BUCKETS];
    const char** names;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU) {
        snprintf(err, err_cap, "noise: expected CPU f32 frame");
        return 1;
    }
    r.x = 0; r.y = 0; r.w = in->w; r.h = in->h;
    if (roi) {
        r = *roi;
        if (r.x >= in->w || r.y >= in->h || r.w == 0 || r.h == 0) {
            snprintf(err, err_cap, "noise: ROI outside frame");
            return 1;
        }
        if (r.x + r.w > in->w) r.w = in->w - r.x;
        if (r.y + r.h > in->h) r.h = in->h - r.y;
    }
    cfa = in->ch == 1 && in->cfa_type != PS_CFA_NONE;
    nb = cfa ? 4 : (int)(in->ch < MAX_BUCKETS ? in->ch : MAX_BUCKETS);
    names = cfa ? CFA_NAMES : RGB_NAMES;

    tilesX = (r.w + TILE - 1) / TILE;
    tilesY = (r.h + TILE - 1) / TILE;
    for (b = 0; b < nb; b++) {
        tileStd[b] = (float*)malloc((size_t)tilesX * tilesY * sizeof(float));
        if (!tileStd[b]) {
            while (--b >= 0) free(tileStd[b]);
            snprintf(err, err_cap, "noise: out of memory");
            return 1;
        }
        tileCnt[b] = 0;
        gSum[b] = gSum2[b] = 0;
        gN[b] = 0;
    }

    for (uint32_t ty = 0; ty < tilesY; ty++) {
        for (uint32_t tx = 0; tx < tilesX; tx++) {
            double s[MAX_BUCKETS] = { 0 }, s2[MAX_BUCKETS] = { 0 };
            size_t n[MAX_BUCKETS] = { 0 };
            uint32_t y0 = r.y + ty * TILE, y1 = y0 + TILE < r.y + r.h ? y0 + TILE : r.y + r.h;
            uint32_t x0 = r.x + tx * TILE, x1 = x0 + TILE < r.x + r.w ? x0 + TILE : r.x + r.w;
            for (uint32_t y = y0; y < y1; y++) {
                const float* row = (const float*)((const char*)in->data + (size_t)y * in->pitch_bytes);
                for (uint32_t x = x0; x < x1; x++) {
                    if (cfa) {
                        float v = row[x];
                        int bb = cfa_channel(in, x, y);
                        if (!isfinite(v)) continue;
                        s[bb] += v; s2[bb] += (double)v * v; n[bb]++;
                    } else {
                        int c;
                        for (c = 0; c < nb; c++) {
                            float v = row[(size_t)x * in->ch + c];
                            if (!isfinite(v)) continue;
                            s[c] += v; s2[c] += (double)v * v; n[c]++;
                        }
                    }
                }
            }
            for (b = 0; b < nb; b++) {
                gSum[b] += s[b]; gSum2[b] += s2[b]; gN[b] += n[b];
                if (n[b] >= 16) {   /* enough samples for a stable tile std */
                    double m = s[b] / (double)n[b];
                    double v = s2[b] / (double)n[b] - m * m;
                    tileStd[b][tileCnt[b]++] = (float)sqrt(v > 0 ? v : 0);
                }
            }
        }
    }

    for (b = 0; b < nb; b++) {
        char key[48];
        if (gN[b] == 0) {
            snprintf(key, sizeof key, "%s", names[b]);
            sink->emit_text(sink->ctx, key, "no samples");
            continue;
        }
        {
            double mean = gSum[b] / (double)gN[b];
            double var = gSum2[b] / (double)gN[b] - mean * mean;
            double noise = 0;
            if (var < 0) var = 0;
            if (tileCnt[b]) {
                qsort(tileStd[b], tileCnt[b], sizeof(float), cmpf);
                noise = tileStd[b][tileCnt[b] / 2];   /* median tile std */
            }
            snprintf(key, sizeof key, "%s.mean", names[b]);  sink->emit_number(sink->ctx, key, mean);
            snprintf(key, sizeof key, "%s.std", names[b]);   sink->emit_number(sink->ctx, key, sqrt(var));
            snprintf(key, sizeof key, "%s.noise", names[b]); sink->emit_number(sink->ctx, key, noise);
            if (noise > 0 && mean > 0) {
                snprintf(key, sizeof key, "%s.snr_db", names[b]);
                sink->emit_number(sink->ctx, key, 20.0 * log10(mean / noise));
            }
        }
    }
    sink->emit_text(sink->ctx, "method", "noise = median std of 16x16 tiles (texture-robust)");
    for (b = 0; b < nb; b++) free(tileStd[b]);
    return 0;
}

static const psAnalyzerV1 DESC = { PS_ABI_VERSION, PS_CAP_CPU, "noise", analyze, {0} };

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < PS_ABI_VERSION) return 1;
    return host->register_analyzer(host->ctx, &DESC);
}
