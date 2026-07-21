/* analyzer_stats.c — sample Analyzer plugin (解析系):
 * per-channel mean/std/min/max + p1/p50/p99 over finite values, ROI-aware. */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmpf(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink* sink, char* err, size_t err_cap) {
    psRect r;
    size_t npx, finiteTotal = 0;
    float* scratch;
    uint32_t c;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU) {
        snprintf(err, err_cap, "stats: expected CPU f32 frame");
        return 1;
    }
    r.x = 0; r.y = 0; r.w = in->w; r.h = in->h;
    if (roi) {
        r = *roi;
        if (r.x >= in->w || r.y >= in->h || r.w == 0 || r.h == 0) {
            snprintf(err, err_cap, "stats: ROI outside frame");
            return 1;
        }
        if (r.x + r.w > in->w) r.w = in->w - r.x;
        if (r.y + r.h > in->h) r.h = in->h - r.y;
    }
    npx = (size_t)r.w * r.h;
    if (roi) {
        char buf[64];
        snprintf(buf, sizeof buf, "%u,%u %ux%u", r.x, r.y, r.w, r.h);
        sink->emit_text(sink->ctx, "roi", buf);
    }
    sink->emit_number(sink->ctx, "pixels", (double)npx);

    scratch = (float*)malloc(npx * sizeof(float));   /* plugin-internal, freed below */
    if (!scratch) { snprintf(err, err_cap, "stats: out of memory"); return 1; }

    for (c = 0; c < in->ch; c++) {
        size_t n = 0;
        double sum = 0, sum2 = 0, mean, var;
        uint32_t x, y;
        char key[32];
        for (y = r.y; y < r.y + r.h; y++) {
            const float* row = (const float*)((const char*)in->data + (size_t)y * in->pitch_bytes);
            for (x = r.x; x < r.x + r.w; x++) {
                float v = row[(size_t)x * in->ch + c];
                if (!isfinite(v)) continue;   /* NaN/Inf never reach qsort */
                scratch[n++] = v;
                sum += v;
                sum2 += (double)v * v;
            }
        }
        finiteTotal += n;
        if (n == 0) {
            snprintf(key, sizeof key, "ch%u", c);
            sink->emit_text(sink->ctx, key, "no finite values");
            continue;
        }
        mean = sum / (double)n;
        var = sum2 / (double)n - mean * mean;
        qsort(scratch, n, sizeof(float), cmpf);
        snprintf(key, sizeof key, "ch%u.mean", c); sink->emit_number(sink->ctx, key, mean);
        snprintf(key, sizeof key, "ch%u.std", c);  sink->emit_number(sink->ctx, key, sqrt(var > 0 ? var : 0));
        snprintf(key, sizeof key, "ch%u.min", c);  sink->emit_number(sink->ctx, key, scratch[0]);
        snprintf(key, sizeof key, "ch%u.max", c);  sink->emit_number(sink->ctx, key, scratch[n - 1]);
        snprintf(key, sizeof key, "ch%u.p1", c);   sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.01)]);
        snprintf(key, sizeof key, "ch%u.p50", c);  sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.50)]);
        snprintf(key, sizeof key, "ch%u.p99", c);  sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.99)]);
    }
    if (npx * in->ch > 0)
        sink->emit_number(sink->ctx, "finite ratio", (double)finiteTotal / ((double)npx * in->ch));
    free(scratch);
    return 0;
}

static const psAnalyzerV1 DESC = { PS_ABI_VERSION, PS_CAP_CPU, "stats", analyze, {0} };

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < PS_ABI_VERSION) return 1;
    return host->register_analyzer(host->ctx, &DESC);
}
