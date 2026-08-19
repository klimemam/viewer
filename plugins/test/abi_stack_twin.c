/* abi_stack_twin.c - a REAL stack analyzer, written against docs/reference/abi-v3.md §5
 * and §6, and the fixture the host side of both is tested against.
 *
 * It exists because the interesting half of §5 is not the struct: it is what a
 * plugin has to DO with it. Three descriptors, one per claim:
 *
 *   abitest/stack-tmean    pulls every frame one at a time, releases each
 *                          before taking the next, aggregates over time with
 *                          NaN excluded PER PIXEL and COUNTED, and DECLARES
 *                          the count in the result (§5.1's plugin obligation).
 *                          min_frames 2 - a temporal mean of one frame is that
 *                          frame, and calling it a temporal anything is a lie.
 *   abitest/stack-needs-8  min_frames 8, and its body emits a row saying it
 *                          RAN. On a shorter stack the host must refuse before
 *                          calling, so that row must never appear: the gate is
 *                          proved by the silence of a body that would shout.
 *   abitest/stack-overrun  asks for one frame past the end. §5.1 makes NULL
 *                          the honest failure, so this must come back nonzero
 *                          with a reason - never a fabricated frame, never a
 *                          partial answer wearing a success return.
 *
 * Not shipped: CMake puts it in build/plugins-abitest.
 */
#include "ps/ps_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define ABI_STACK_VERSION  "stack-2.0.0-twin (2026-08-10)"
#define ABI_STACK_HEADLINE "tmean"

/* The ROI window, clamped to the stack's geometry. Same shape as the frame
 * twin's: roi == NULL is the whole thing, and a roi whose origin is outside is
 * an error rather than an empty success. */
static int stackWindow(const psStack* in, const psRect* roi,
                       uint32_t* x0, uint32_t* y0, uint32_t* w, uint32_t* h,
                       char* err, size_t err_cap) {
    *x0 = 0; *y0 = 0; *w = in->w; *h = in->h;
    if (!roi) return 0;
    if (roi->x >= in->w || roi->y >= in->h) {
        snprintf(err, err_cap, "abi stack twin: roi is outside the stack");
        return 1;
    }
    *x0 = roi->x;
    *y0 = roi->y;
    *w = roi->w < in->w - *x0 ? roi->w : in->w - *x0;
    *h = roi->h < in->h - *y0 ? roi->h : in->h - *y0;
    return 0;
}

static int32_t stackTMean(const psStack* in, const psRect* roi,
                          const psAnalyzeSink3* sink, char* err, size_t err_cap) {
    uint32_t x0, y0, w, h, i, y;
    size_t samples, s;
    double* sum;
    uint32_t* cnt;
    double excluded = 0.0, noValue = 0.0, acc = 0.0, valued = 0.0;

    if (!in || !sink || !in->get_frame) {
        snprintf(err, err_cap, "abi stack twin: no stack to read");
        return 1;
    }
    if (in->dtype != PS_DTYPE_F32) {
        snprintf(err, err_cap, "abi stack twin: needs float32 frames");
        return 1;
    }
    if (stackWindow(in, roi, &x0, &y0, &w, &h, err, err_cap)) return 1;
    samples = (size_t)w * h * in->ch;
    if (!samples) {
        snprintf(err, err_cap, "abi stack twin: the region is empty");
        return 1;
    }
    /* The plugin's OWN memory. Nothing here crosses the ABI, so it is allocated
     * and freed with plain malloc/free - host->frame_alloc exists for pixel
     * buffers the host will later free, which is not this. */
    sum = (double*)calloc(samples, sizeof(double));
    cnt = (uint32_t*)calloc(samples, sizeof(uint32_t));
    if (!sum || !cnt) {
        free(sum); free(cnt);
        snprintf(err, err_cap, "abi stack twin: out of memory for %u x %u x %u",
                 w, h, in->ch);
        return 1;
    }

    /* PULL, one frame at a time, and give each one back before taking the next.
     * A plugin that never released would still be correct (§5.1) - it would
     * just pin the whole stack, which is exactly what a bounded shared-memory
     * window (§9.2) cannot afford. Releasing costs one call and keeps this
     * analyzer runnable on that transport unchanged. */
    for (i = 0; i < in->frames; i++) {
        const psFrame* f = in->get_frame(in->ctx, i);
        if (!f) {
            /* §5.1: NULL is an honest failure, and a partial aggregate must
             * never be reported with a success return. */
            free(sum); free(cnt);
            snprintf(err, err_cap,
                     "abi stack twin: the host could not serve frame %u of %u",
                     i, in->frames);
            return 1;
        }
        /* The host promises one shape for every served frame. Checking it is
         * cheap, and the alternative to checking is reading someone else's
         * memory when the promise is one day broken. */
        if (f->w != in->w || f->h != in->h || f->ch != in->ch ||
            f->dtype != PS_DTYPE_F32 || !f->data) {
            free(sum); free(cnt);
            snprintf(err, err_cap,
                     "abi stack twin: frame %u is %ux%ux%u, the stack declared %ux%ux%u",
                     i, f->w, f->h, f->ch, in->w, in->h, in->ch);
            return 1;
        }
        for (y = 0; y < h; y++) {
            const float* row = (const float*)(const void*)
                ((const char*)f->data + (size_t)(y0 + y) * f->pitch_bytes);
            row += (size_t)x0 * f->ch;
            for (s = 0; s < (size_t)w * f->ch; s++) {
                double v = row[s];
                size_t k = (size_t)y * w * in->ch + s;
                /* THE discipline (§5.1): a non-finite sample is excluded from
                 * this pixel's population and counted. It is never folded in
                 * as a value, and the divisor never keeps counting it - both
                 * of those pull the mean toward zero by exactly the fraction
                 * that is missing, and both look entirely plausible. */
                if (!isfinite(v)) { excluded += 1.0; continue; }
                sum[k] += v;
                cnt[k]++;
            }
        }
        in->release_frame(in->ctx, i);
    }

    for (s = 0; s < samples; s++) {
        if (!cnt[s]) { noValue += 1.0; continue; }   /* no frame had a value here */
        acc += sum[s] / (double)cnt[s];
        valued += 1.0;
    }

    /* The FACTS the host passed in, restated verbatim (§6). The plugin does not
     * judge them - it was already allowed to run - it carries them, so that a
     * result read on its own still says what it is a measurement OF. */
    sink->emit_number(sink->ctx, "frames", (double)in->frames);
    sink->emit_number(sink->ctx, "expected", (double)in->expected);
    /* ...and the declaration §5.1 requires of every temporal aggregate. Both
     * numbers, always, including the zeroes: "0 excluded" is a measurement,
     * an absent row is a silence. */
    sink->emit_number(sink->ctx, "nan_excluded", excluded);
    sink->emit_number(sink->ctx, "no_value", noValue);
    sink->emit_number(sink->ctx, "ch0.tmean", valued > 0.0 ? acc / valued : 0.0);
    /* One named pixel, so a wrong exclusion cannot hide in an average: it is
     * the sample the fixture puts a NaN in the middle of. */
    if (cnt[0]) sink->emit_number(sink->ctx, "s0.tmean", sum[0] / (double)cnt[0]);
    sink->emit_text(sink->ctx, "nan_policy",
                    "non-finite samples excluded per pixel and counted; "
                    "never folded into a divisor");
    free(sum);
    free(cnt);
    return 0;
}

/* The gate's witness. If this body ever runs on a stack shorter than
 * min_frames, the host did not gate - and it says so in a row rather than by
 * failing, because a wrong SUCCESS is the failure mode that ships. */
static int32_t stackNeedsEight(const psStack* in, const psRect* roi,
                               const psAnalyzeSink3* sink, char* err, size_t err_cap) {
    (void)roi; (void)err; (void)err_cap;
    if (!in || !sink) return 1;
    sink->emit_number(sink->ctx, "gate.breached", 1.0);
    sink->emit_number(sink->ctx, "frames", (double)in->frames);
    return 0;
}

/* One past the end. §5.1 says the answer is NULL, and the plugin's answer to a
 * NULL is err + nonzero. */
static int32_t stackOverrun(const psStack* in, const psRect* roi,
                            const psAnalyzeSink3* sink, char* err, size_t err_cap) {
    const psFrame* f;
    (void)roi;
    if (!in || !sink || !in->get_frame) {
        snprintf(err, err_cap, "abi stack twin: no stack to read");
        return 1;
    }
    f = in->get_frame(in->ctx, in->frames);      /* indices are 0..frames-1 */
    if (!f) {
        snprintf(err, err_cap,
                 "abi stack twin: frame %u is past the end of a %u-frame stack",
                 in->frames, in->frames);
        return 1;
    }
    /* Reached only if a host answered an out-of-range index with pixels. Say so
     * as a row, for the same reason as the gate above. */
    sink->emit_number(sink->ctx, "overrun.served", 1.0);
    return 0;
}

static const psStackAnalyzerV3 TMEAN = {
    3u, PS_CAP_CPU,
    2u,                                 /* min_frames: a temporal mean needs two */
    0u,                                 /* _pad */
    "abitest/stack-tmean",
    ABI_STACK_VERSION,
    "compat fixture: per-pixel temporal mean, NaN excluded per pixel and declared",
    NULL,                               /* params_schema: reserved */
    ABI_STACK_HEADLINE,
    stackTMean, { 0 }
};

static const psStackAnalyzerV3 NEEDS8 = {
    3u, PS_CAP_CPU,
    8u,                                 /* min_frames the host must enforce */
    0u,
    "abitest/stack-needs-8",
    ABI_STACK_VERSION,
    "compat fixture: declares min_frames 8 so the host's gate can be observed",
    NULL,
    NULL,                               /* no headline - a plugin may decline one */
    stackNeedsEight, { 0 }
};

static const psStackAnalyzerV3 OVERRUN = {
    3u, PS_CAP_CPU,
    1u,
    0u,
    "abitest/stack-overrun",
    ABI_STACK_VERSION,
    "compat fixture: reads one frame past the end; NULL must fail honestly",
    NULL,
    NULL,
    stackOverrun, { 0 }
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    int32_t rc;
    /* the header's standing rule: too old a host, register NOTHING and say so */
    if (!host || host->abi_version < PS_ABI_VERSION) return 1;
    /* ...and the probe (§2.2). abi_version 3 promises this seat, and asking
     * anyway is free: it is the same line that will guard the seats the number
     * does NOT promise, so writing it here is how the habit survives. */
    if (!host->register_stack_analyzer3) return 1;
    rc = host->register_stack_analyzer3(host->ctx, &TMEAN);
    if (rc == 0) rc = host->register_stack_analyzer3(host->ctx, &NEEDS8);
    if (rc == 0) rc = host->register_stack_analyzer3(host->ctx, &OVERRUN);
    return rc;
}
