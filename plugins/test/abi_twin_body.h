/* The measurement the ABI compatibility twins perform - ONE implementation,
 * reached through two ABI paths (docs/reference/abi-v3.md §2.1: "V1/V2 構造体と登録経路は
 * 永久に不変").
 *
 * abi_v2_twin.c registers it as a psAnalyzerV2 - the exact shape every bundled
 * analyzer uses today - and abi_v3_twin.c registers it as a psAnalyzerV3 with a
 * declared version and headline. "The V2 path still produces identical results"
 * is then not a claim about two implementations that happen to agree: there is
 * one implementation, and the tests check that the two paths carry it through
 * unchanged. A drift would have to be in the ABI, which is the thing under test.
 *
 * The emits are passed as plain function pointers because sink2 and sink3 spell
 * emit_number / emit_text identically - which is itself the compatibility claim
 * of §4 ("呼び出し契約は V2 から不変"), stated as code that would not compile if
 * it stopped being true.
 */
#ifndef ABI_TWIN_BODY_H
#define ABI_TWIN_BODY_H

#include "ps/ps_plugin.h"
#include <stdio.h>

static int32_t abiTwinAnalyze(const psFrame* in, const psRect* roi, void* ctx,
                              void (*emit_number)(void*, const char*, double),
                              void (*emit_text)(void*, const char*, const char*),
                              char* err, size_t err_cap) {
    uint32_t x0 = 0, y0 = 0, w, h, x, y;
    double sum = 0.0, n = 0.0;
    if (!in || !in->data || in->dtype != PS_DTYPE_F32) {
        snprintf(err, err_cap, "abi twin: needs a float32 frame");
        return 1;
    }
    w = in->w;
    h = in->h;
    if (roi) {
        if (roi->x >= in->w || roi->y >= in->h) {
            snprintf(err, err_cap, "abi twin: roi is outside the frame");
            return 1;
        }
        x0 = roi->x;
        y0 = roi->y;
        w = roi->w < in->w - x0 ? roi->w : in->w - x0;
        h = roi->h < in->h - y0 ? roi->h : in->h - y0;
    }
    for (y = y0; y < y0 + h; y++) {
        const float* row = (const float*)(const void*)
            ((const char*)in->data + (size_t)y * in->pitch_bytes);
        for (x = x0; x < x0 + w; x++) {
            sum += (double)row[(size_t)x * in->ch];
            n += 1.0;
        }
    }
    emit_number(ctx, "pixels", n);
    emit_number(ctx, "ch0.mean", n > 0.0 ? sum / n : 0.0);
    emit_text(ctx, "method", "abi twin: mean of channel 0 over the target");
    return 0;
}

#endif /* ABI_TWIN_BODY_H */
