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

/* Three percentiles do not need the array sorted, only three of its order
 * statistics in place. A full qsort of 12 M elements to answer p1/p50/p99 is
 * what made this analyzer look slow next to numpy, which uses partition -- an
 * algorithm difference, not a language one.
 *
 * Lomuto partition with a median-of-three pivot. Worst case is quadratic on an
 * adversarial ordering; pixel data is not adversarial, and the median-of-three
 * removes the already-sorted case that would otherwise be the common one here. */
static size_t partitionAt(float* a, size_t lo, size_t hi) {
    size_t mid = lo + (hi - lo) / 2, i, store;
    float pivot, t;
    if (a[mid] < a[lo])  { t = a[mid]; a[mid] = a[lo];  a[lo]  = t; }
    if (a[hi]  < a[lo])  { t = a[hi];  a[hi]  = a[lo];  a[lo]  = t; }
    if (a[hi]  < a[mid]) { t = a[hi];  a[hi]  = a[mid]; a[mid] = t; }
    t = a[mid]; a[mid] = a[hi]; a[hi] = t;         /* park the median at hi */
    pivot = a[hi];
    store = lo;
    for (i = lo; i < hi; i++)
        if (a[i] < pivot) { t = a[i]; a[i] = a[store]; a[store] = t; store++; }
    t = a[store]; a[store] = a[hi]; a[hi] = t;
    return store;
}

/* After this a[k] holds what a sorted a[lo..hi] would hold at k, and nothing
 * before k is greater than it. That second half is what lets the next call
 * search only the part above k. */
static void selectNth(float* a, size_t lo, size_t hi, size_t k) {
    while (lo < hi) {
        size_t p = partitionAt(a, lo, hi);
        if (k == p) return;
        /* k < p implies p > lo, so p - 1 cannot wrap */
        if (k < p) hi = p - 1;
        else       lo = p + 1;
    }
}

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink3* sink, char* err, size_t err_cap) {
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
        /* min and max come from this pass now. They used to be scratch[0] and
         * scratch[n-1] of a fully sorted array; a partially selected one has
         * neither at those ends. */
        float vmin = 0, vmax = 0;
        uint32_t x, y;
        char key[32];
        for (y = r.y; y < r.y + r.h; y++) {
            const float* row = (const float*)((const char*)in->data + (size_t)y * in->pitch_bytes);
            for (x = r.x; x < r.x + r.w; x++) {
                float v;
                if (cfa && (uint32_t)cfa_channel(in, x, y) != c) continue;
                v = row[(size_t)x * in->ch + (cfa ? 0 : c)];
                if (!isfinite(v)) continue;   /* NaN/Inf never reach the select */
                if (n == 0 || v < vmin) vmin = v;
                if (n == 0 || v > vmax) vmax = v;
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
        {   /* The three the array is actually needed for. Each search is bounded
             * below by the previous one: once p1 sits at its index nothing
             * before it is larger, so p50 can only be above it.
             *
             * Each value is READ AS SOON AS IT IS SELECTED, before the next call
             * runs. A later selection rearranges a range that still contains the
             * earlier index, so reading all three at the end depends on the
             * partition happening to leave that index alone. This one does --
             * Lomuto keeps a range's minimum at lo -- but that is a property of
             * this implementation and not of the operation: std::nth_element,
             * chained the same way, returns the wrong p1 and p50 (measured). A
             * future change of pivot or partition would break it silently. */
            size_t k1  = (size_t)((double)(n - 1) * 0.01);
            size_t k50 = (size_t)((double)(n - 1) * 0.50);
            size_t k99 = (size_t)((double)(n - 1) * 0.99);
            float p1, p50, p99;
            selectNth(scratch, 0,   n - 1, k1);   p1  = scratch[k1];
            selectNth(scratch, k1,  n - 1, k50);  p50 = scratch[k50];
            selectNth(scratch, k50, n - 1, k99);  p99 = scratch[k99];
            snprintf(key, sizeof key, "%s.min", names[c]); sink->emit_number(sink->ctx, key, vmin);
            snprintf(key, sizeof key, "%s.max", names[c]); sink->emit_number(sink->ctx, key, vmax);
            snprintf(key, sizeof key, "%s.p1", names[c]);  sink->emit_number(sink->ctx, key, p1);
            snprintf(key, sizeof key, "%s.p50", names[c]); sink->emit_number(sink->ctx, key, p50);
            snprintf(key, sizeof key, "%s.p99", names[c]); sink->emit_number(sink->ctx, key, p99);
        }
    }
    if (npx * in->ch > 0)
        sink->emit_number(sink->ctx, "finite ratio", (double)finiteTotal / ((double)npx * in->ch));
    free(scratch);
    return 0;
}

/* V3 registration (docs/abi-v3.md §3-§4). Two fields are new and neither is a
 * formality:
 *
 *   version  - the one thing the host cannot source for itself, and the one
 *              string ABI v3 §10 compares for EQUALITY between a client and a
 *              peer. So it names THE COMPUTATION and nothing else: it changes
 *              when some input could produce a different result document (a key
 *              gained or lost, a unit, a value, an error sentence), and it does
 *              NOT change for a comment, a refactor, a compiler or a commit.
 *              Deliberately not derived from the build (cmake/gitversion.cmake
 *              gives the viewer "<hash>+local"): that would be right for a
 *              binary and would make two peers that agree about every line of
 *              this file refuse each other - the failure parity exists to
 *              prevent, arriving from the other side. 1.0.0 is not a claim that
 *              this analyzer is new; it is the first version it has ever
 *              DECLARED. The full rule is in docs/analyzers.md.
 *   headline  - NULL here, and NULL is an answer: every row this emits is a
 *              peer of every other, and §3.2 honours a declared "none" instead
 *              of falling back to the host's V1/V2 table.
 *
 * description is unchanged and still self-declared: the menu shows the
 * precondition where the user picks the tool, instead of a host-side table. */
static const psAnalyzerV3 DESC = {
    3u, PS_CAP_CPU, "stats/moments", "1.0.0",
    "any image / ROI; CFA planes split; finite ratio < 1 flags NaN or Inf",
    NULL, NULL, analyze, {0}
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < PS_ABI_VERSION || !host->register_analyzer3) return 1;
    return host->register_analyzer3(host->ctx, &DESC);
}
