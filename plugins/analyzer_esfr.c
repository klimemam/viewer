/* analyzer_esfr.c — slanted-edge SFR (simplified ISO 12233 e-SFR), ABI v2.
 * Put an ROI over a single slanted edge (~5-40 deg tilt, either orientation).
 * Pipeline: per-line subpixel edge location (derivative centroid) -> linear
 * edge fit -> 4x oversampled ESF by projection -> LSF (central difference)
 * -> Hamming window -> DTFT magnitude normalized at DC -> SFR curve.
 * Emits the SFR curve via emit_series plus MTF50 / MTF20 / SFR@Nyquist.
 * Simplifications vs full ISO 12233: no derivative sinc correction, no
 * Tukey/edge-length weighting — values are comparable between images, small
 * absolute bias at high frequencies. */
#include "ps/ps_plugin.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OS 4                       /* 4x oversampling per ISO 12233 */
#define NFREQ 101                  /* SFR samples: 0 .. 1.0 cy/px step 0.01 */

static int32_t analyze(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink2* sink, char* err, size_t err_cap) {
    psRect r;
    int pw, ph, x, y;
    float* plane;

    if (in->dtype != PS_DTYPE_F32 || in->loc != PS_MEM_CPU) {
        snprintf(err, err_cap, "e-sfr: expected CPU f32 frame");
        return 1;
    }
    if (in->ch == 1 && in->cfa_type != PS_CFA_NONE) {
        snprintf(err, err_cap, "e-sfr: CFA mosaic input - demosaic first");
        return 1;
    }
    r.x = 0; r.y = 0; r.w = in->w; r.h = in->h;
    if (roi) {
        r = *roi;
        if (r.x >= in->w || r.y >= in->h || r.w == 0 || r.h == 0) {
            snprintf(err, err_cap, "e-sfr: ROI outside frame");
            return 1;
        }
        if (r.x + r.w > in->w) r.w = in->w - r.x;
        if (r.y + r.h > in->h) r.h = in->h - r.y;
    }
    if (r.w < 16 || r.h < 16) {
        snprintf(err, err_cap, "e-sfr: ROI too small (need >= 16x16 over the edge)");
        return 1;
    }
    if ((size_t)r.w * r.h > 4u * 1024u * 1024u) {
        snprintf(err, err_cap, "e-sfr: ROI too large (keep it around the edge)");
        return 1;
    }

    /* luma plane */
    pw = (int)r.w; ph = (int)r.h;
    plane = (float*)malloc((size_t)pw * ph * sizeof(float));
    if (!plane) { snprintf(err, err_cap, "e-sfr: out of memory"); return 1; }
    for (y = 0; y < ph; y++) {
        const float* row = (const float*)((const char*)in->data +
                                          (size_t)(r.y + y) * in->pitch_bytes);
        for (x = 0; x < pw; x++) {
            const float* p = row + (size_t)(r.x + x) * in->ch;
            float v = in->ch >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[0];
            plane[(size_t)y * pw + x] = isfinite(v) ? v : 0.0f;
        }
    }

    /* orientation: does intensity change mostly along x (vertical edge)? */
    {
        double sdx = 0, sdy = 0;
        for (y = 1; y < ph - 1; y++)
            for (x = 1; x < pw - 1; x++) {
                const float* p = plane + (size_t)y * pw + x;
                sdx += fabs((double)p[1] - p[-1]);
                sdy += fabs((double)p[pw] - p[-pw]);
            }
        if (sdy > sdx) {   /* horizontal edge: transpose so the edge is vertical */
            float* t = (float*)malloc((size_t)pw * ph * sizeof(float));
            if (!t) { free(plane); snprintf(err, err_cap, "e-sfr: out of memory"); return 1; }
            for (y = 0; y < ph; y++)
                for (x = 0; x < pw; x++)
                    t[(size_t)x * ph + y] = plane[(size_t)y * pw + x];
            free(plane);
            plane = t;
            { int tmpwh = pw; pw = ph; ph = tmpwh; }
        }
    }

    /* per-line subpixel edge position: centroid of squared derivative */
    {
        double sumY = 0, sumP = 0, sumYY = 0, sumYP = 0;
        int lines = 0;
        double a, b, angleDeg;
        double* esfSum;
        double* lsf;
        int bins = pw * OS;

        for (y = 0; y < ph; y++) {
            const float* row = plane + (size_t)y * pw;
            double wsum = 0, wpos = 0, wmax = 0;
            for (x = 1; x < pw - 1; x++) {
                double d = (double)row[x + 1] - row[x - 1];
                double w2 = d * d;
                wsum += w2;
                wpos += w2 * x;
                if (w2 > wmax) wmax = w2;
            }
            if (wsum <= 0 || wmax < wsum * 0.02) continue;   /* no clear edge */
            {
                double pos = wpos / wsum;
                sumY += y; sumP += pos; sumYY += (double)y * y; sumYP += (double)y * pos;
                lines++;
            }
        }
        if (lines < 8) {
            free(plane);
            snprintf(err, err_cap, "e-sfr: no clear single edge found in the ROI");
            return 1;
        }
        {
            double det = lines * sumYY - sumY * sumY;
            if (fabs(det) < 1e-12) { free(plane); snprintf(err, err_cap, "e-sfr: degenerate edge fit"); return 1; }
            b = (lines * sumYP - sumY * sumP) / det;   /* px per line  */
            a = (sumP * sumYY - sumY * sumYP) / det;   /* intercept    */
        }
        angleDeg = atan(fabs(b)) * 180.0 / 3.14159265358979323846;

        /* project every pixel into edge-relative distance, bin at 4x */
        esfSum = (double*)calloc((size_t)bins * 2, sizeof(double));   /* sum + count */
        lsf = (double*)malloc((size_t)bins * sizeof(double));
        if (!esfSum || !lsf) {
            free(plane); free(esfSum); free(lsf);
            snprintf(err, err_cap, "e-sfr: out of memory");
            return 1;
        }
        for (y = 0; y < ph; y++) {
            const float* row = plane + (size_t)y * pw;
            double edge = a + b * y;
            for (x = 0; x < pw; x++) {
                double d = (x - edge) + pw * 0.5;      /* recentred distance */
                int bin = (int)floor(d * OS + 0.5);
                if (bin < 0 || bin >= bins) continue;
                esfSum[bin * 2] += row[x];
                esfSum[bin * 2 + 1] += 1.0;
            }
        }
        {
            /* ESF with gap fill, then LSF by central difference */
            double prev = 0;
            int havePrev = 0;
            int i;
            for (i = 0; i < bins; i++) {
                if (esfSum[i * 2 + 1] > 0) {
                    prev = esfSum[i * 2] / esfSum[i * 2 + 1];
                    havePrev = 1;
                } /* empty bin: carry previous value (rare with |b|>0.03) */
                lsf[i] = havePrev ? prev : 0.0;
            }
            for (i = bins - 1; i >= 1; i--) lsf[i] = lsf[i] - lsf[i - 1];   /* in-place diff */
            lsf[0] = 0;

            /* Hamming window centred on the LSF energy centroid */
            {
                double esum = 0, ecent = 0;
                int half = bins / 2;
                for (i = 0; i < bins; i++) { double e = lsf[i] * lsf[i]; esum += e; ecent += e * i; }
                if (esum > 0) half = (int)(ecent / esum);
                for (i = 0; i < bins; i++) {
                    double t = (double)(i - half) / (double)(pw * OS / 2);
                    if (t < -1 || t > 1) lsf[i] = 0;
                    else lsf[i] *= 0.54 + 0.46 * cos(3.14159265358979323846 * t);
                }
            }

            /* DTFT -> SFR normalized at DC */
            {
                float freq[NFREQ], sfr[NFREQ];
                double dc = 0;
                double mtf50 = -1, mtf20 = -1;
                int fi;
                for (i = 0; i < bins; i++) dc += lsf[i];
                if (fabs(dc) < 1e-12) {
                    free(plane); free(esfSum); free(lsf);
                    snprintf(err, err_cap, "e-sfr: flat edge profile (no contrast?)");
                    return 1;
                }
                for (fi = 0; fi < NFREQ; fi++) {
                    double f = fi * 0.01;              /* cycles per pixel */
                    double re = 0, im = 0;
                    for (i = 0; i < bins; i++) {
                        double ph2 = -2.0 * 3.14159265358979323846 * f * (i / (double)OS);
                        re += lsf[i] * cos(ph2);
                        im += lsf[i] * sin(ph2);
                    }
                    freq[fi] = (float)f;
                    sfr[fi] = (float)(sqrt(re * re + im * im) / fabs(dc));
                }
                for (fi = 1; fi < NFREQ; fi++) {       /* first crossings, linear interp */
                    if (mtf50 < 0 && sfr[fi] < 0.5 && sfr[fi - 1] >= 0.5)
                        mtf50 = (fi - 1 + (sfr[fi - 1] - 0.5) / (sfr[fi - 1] - sfr[fi])) * 0.01;
                    if (mtf20 < 0 && sfr[fi] < 0.2 && sfr[fi - 1] >= 0.2)
                        mtf20 = (fi - 1 + (sfr[fi - 1] - 0.2) / (sfr[fi - 1] - sfr[fi])) * 0.01;
                }
                sink->emit_series(sink->ctx, "sfr", "cycles/px", "SFR", freq, sfr, NFREQ);
                if (mtf50 > 0) sink->emit_number(sink->ctx, "mtf50 (cy/px)", mtf50);
                else sink->emit_text(sink->ctx, "mtf50 (cy/px)", "> 1.0");
                if (mtf20 > 0) sink->emit_number(sink->ctx, "mtf20 (cy/px)", mtf20);
                sink->emit_number(sink->ctx, "sfr@nyquist", sfr[50]);
                sink->emit_number(sink->ctx, "edge_angle_deg", angleDeg);
                sink->emit_number(sink->ctx, "lines_used", (double)lines);
                if (angleDeg < 2.0)
                    sink->emit_text(sink->ctx, "note", "edge tilt < 2 deg: binning artifacts likely");
                if (angleDeg > 45.0)
                    sink->emit_text(sink->ctx, "note", "edge tilt > 45 deg: use the other orientation");
            }
        }
        free(esfSum);
        free(lsf);
    }
    free(plane);
    sink->emit_text(sink->ctx, "method",
                    "simplified ISO 12233 e-SFR: 4x binning, Hamming, DTFT; no sinc correction");
    return 0;
}

static const psAnalyzerV2 DESC = {
    2u, PS_CAP_CPU, "iso12233/e-sfr",
    "ROI over one slanted edge (5-40 deg)", NULL, analyze, {0}
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < 2u || !host->register_analyzer2) return 1;
    return host->register_analyzer2(host->ctx, &DESC);
}
