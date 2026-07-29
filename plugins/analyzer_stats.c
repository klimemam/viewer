/* analyzer_stats.c — sample Analyzer plugin (解析系):
 * per-channel mean/std/min/max + p1/p50/p99 over finite values, ROI-aware.
 * CFA-aware: a 1-channel mosaiced frame is bucketed by PLANE (R/Gr/Gb/B), not
 * pooled into one "ch0". docs/stats-taxonomy.md's iron rule - on a Bayer flat
 * field a pooled .std is mostly the inter-plane level difference, i.e. a
 * plausible-looking number that measures the wrong thing, and it rides into
 * the clipboard verbatim through "Copy table (TSV)". The mapping is lifted
 * from analyzer_noise.c rather than invented a second time. */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUCKETS 4

static const int CFA_MAP[4][4] = { {0,1,2,3},{3,2,1,0},{1,0,3,2},{2,3,0,1} };
static const char* CFA_NAMES[4] = { "R", "Gr", "Gb", "B" };
static const char* CH_NAMES[4] = { "ch0", "ch1", "ch2", "ch3" };

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
                       const psAnalyzeSink2* sink, char* err, size_t err_cap) {
    psRect r;
    size_t npx, finiteTotal = 0;
    float* scratch;
    uint32_t c;
    int cfa, nb;
    const char** names;

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

    /* one bucket per CFA plane for a mosaiced 1ch frame, else one per channel */
    cfa = in->ch == 1 && in->cfa_type != PS_CFA_NONE;
    nb = cfa ? 4 : (int)(in->ch < MAX_BUCKETS ? in->ch : MAX_BUCKETS);
    names = cfa ? CFA_NAMES : CH_NAMES;
    if (cfa)
        sink->emit_text(sink->ctx, "planes",
                        in->cfa_type == PS_CFA_QUAD ? "Quad Bayer R/Gr/Gb/B (never pooled)"
                                                    : "Bayer R/Gr/Gb/B (never pooled)");

    for (c = 0; c < (uint32_t)nb; c++) {
        size_t n = 0;
        double sum = 0, sum2 = 0, mean, var;
        uint32_t x, y;
        char key[32];
        for (y = r.y; y < r.y + r.h; y++) {
            const float* row = (const float*)((const char*)in->data + (size_t)y * in->pitch_bytes);
            for (x = r.x; x < r.x + r.w; x++) {
                float v;
                if (cfa && (uint32_t)cfa_channel(in, x, y) != c) continue;
                v = row[(size_t)x * in->ch + (cfa ? 0 : c)];
                if (!isfinite(v)) continue;   /* NaN/Inf never reach qsort */
                scratch[n++] = v;
                sum += v;
                sum2 += (double)v * v;
            }
        }
        finiteTotal += n;
        if (n == 0) {
            snprintf(key, sizeof key, "%s", names[c]);
            sink->emit_text(sink->ctx, key, "no finite values");
            continue;
        }
        mean = sum / (double)n;
        var = sum2 / (double)n - mean * mean;
        if (var < 0) var = 0;
        qsort(scratch, n, sizeof(float), cmpf);
        snprintf(key, sizeof key, "%s.mean", names[c]); sink->emit_number(sink->ctx, key, mean);
        snprintf(key, sizeof key, "%s.var", names[c]);  sink->emit_number(sink->ctx, key, var);
        snprintf(key, sizeof key, "%s.std", names[c]);  sink->emit_number(sink->ctx, key, sqrt(var));
        {   /* entropy over 256 bins across the black/white display range */
            double lo = in->black, hi = in->white, H = 0;
            if (hi > lo) {
                size_t hist[256]; size_t k2;
                memset(hist, 0, sizeof hist);
                for (k2 = 0; k2 < n; k2++) {
                    double t = (scratch[k2] - lo) / (hi - lo) * 255.0;
                    int b = t < 0 ? 0 : t > 255 ? 255 : (int)t;
                    hist[b]++;
                }
                for (k2 = 0; k2 < 256; k2++)
                    if (hist[k2]) {
                        double pr = (double)hist[k2] / (double)n;
                        H -= pr * log(pr) / log(2.0);
                    }
                snprintf(key, sizeof key, "%s.entropy", names[c]);
                sink->emit_number(sink->ctx, key, H);
            }
        }
        snprintf(key, sizeof key, "%s.min", names[c]);  sink->emit_number(sink->ctx, key, scratch[0]);
        snprintf(key, sizeof key, "%s.max", names[c]);  sink->emit_number(sink->ctx, key, scratch[n - 1]);
        snprintf(key, sizeof key, "%s.p1", names[c]);   sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.01)]);
        snprintf(key, sizeof key, "%s.p50", names[c]);  sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.50)]);
        snprintf(key, sizeof key, "%s.p99", names[c]);  sink->emit_number(sink->ctx, key, scratch[(size_t)((double)(n - 1) * 0.99)]);
    }
    if (npx * in->ch > 0)
        sink->emit_number(sink->ctx, "finite ratio", (double)finiteTotal / ((double)npx * in->ch));
    free(scratch);
    return 0;
}

/* V2 registration for the description alone: the menu shows the precondition
 * where the user picks the tool, instead of a host-side lookup table. */
static const psAnalyzerV2 DESC = {
    2u, PS_CAP_CPU, "stats/moments",
    "any image / ROI; CFA planes split; finite ratio < 1 flags NaN or Inf", NULL, analyze, {0}
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < 2u || !host->register_analyzer2) return 1;
    return host->register_analyzer2(host->ctx, &DESC);
}
