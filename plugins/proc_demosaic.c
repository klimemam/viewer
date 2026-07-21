/* proc_demosaic.c — sample Processor plugin: bilinear demosaic, Bayer only.
 * Quad Bayer is honestly rejected in v1 (regroup to Bayer first). */
#include "ps/ps_plugin.h"
#include <stdio.h>
#include <string.h>

/* color of pixel (x,y): 0=R 1=G 2=B — pattern order matches psCfaPattern */
static const int MAP[4][4] = {
    { 0, 1, 1, 2 },   /* RGGB */
    { 2, 1, 1, 0 },   /* BGGR */
    { 1, 0, 2, 1 },   /* GRBG */
    { 1, 2, 0, 1 },   /* GBRG */
};
static int color_at(int pat, int x, int y) { return MAP[pat & 3][(y & 1) * 2 + (x & 1)]; }

static int32_t process(const psFrame* in, psFrame* out, const psHostApi* host,
                       char* err, size_t err_cap) {
    int w, h, pat, x, y, dx, dy, c;
    size_t bytes;
    float* dst;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU || in->ch != 1) {
        snprintf(err, err_cap, "demosaic: need 1ch CPU f32 frame");
        return 1;
    }
    if (in->cfa_type == PS_CFA_QUAD) {
        snprintf(err, err_cap, "demosaic: Quad Bayer not supported in v1 (regroup to Bayer first)");
        return 1;
    }
    if (in->cfa_type != PS_CFA_BAYER) {
        snprintf(err, err_cap, "demosaic: frame has no Bayer CFA metadata");
        return 1;
    }
    w = (int)in->w; h = (int)in->h; pat = in->cfa_pattern;
    bytes = (size_t)w * h * 3 * sizeof(float);
    dst = (float*)host->frame_alloc(host->ctx, bytes);   /* host memory, host frees */
    if (!dst) { snprintf(err, err_cap, "demosaic: out of memory"); return 1; }

    /* Bilinear via uniform rule: for each output pixel and each plane, average
     * every sample in the clamped 3x3 neighborhood whose CFA color matches.
     * Correct weights for all four patterns; borders handled by clamping. */
    for (y = 0; y < h; y++) {
        float* drow = dst + (size_t)y * w * 3;
        for (x = 0; x < w; x++) {
            double acc[3] = { 0, 0, 0 };
            int cnt[3] = { 0, 0, 0 };
            for (dy = -1; dy <= 1; dy++) {
                int yy = y + dy;
                const float* srow;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                srow = (const float*)((const char*)in->data + (size_t)yy * in->pitch_bytes);
                for (dx = -1; dx <= 1; dx++) {
                    int xx = x + dx;
                    int cc;
                    if (xx < 0) xx = 0;
                    if (xx >= w) xx = w - 1;
                    cc = color_at(pat, xx, yy);
                    acc[cc] += srow[xx];
                    cnt[cc]++;
                }
            }
            for (c = 0; c < 3; c++)
                drow[(size_t)x * 3 + c] = cnt[c] ? (float)(acc[c] / cnt[c]) : 0.0f;
        }
    }

    memset(out, 0, sizeof *out);
    out->w = in->w; out->h = in->h; out->ch = 3;
    out->dtype = PS_DTYPE_F32; out->loc = PS_MEM_CPU;
    out->data = dst;
    out->pitch_bytes = (size_t)w * 3 * sizeof(float);
    out->black = in->black; out->white = in->white;   /* range unchanged by demosaic */
    out->cfa_type = PS_CFA_NONE;
    out->pts_us = -1;
    return 0;
}

static const psProcessorV1 DESC =
    { PS_ABI_VERSION, PS_CAP_CPU, "demosaic (bilinear)", NULL, process, {0} };

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < PS_ABI_VERSION) return 1;
    return host->register_processor(host->ctx, &DESC);
}
