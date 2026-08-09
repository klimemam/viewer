/* abi_v3_twin.c - the V3 half of the ABI compatibility pair, and the worked
 * example of the probe rule (docs/abi-v3.md §2.2).
 *
 * Same measurement as abi_v2_twin.c, reached through the v3 descriptor: it
 * declares a version (§3.1) and a headline (§3.2), and it registers through
 * the seat register_analyzer3.
 *
 * The version string is a fixture value, not a semver claim - the host is
 * required to carry it verbatim and to never parse or order it, so a value
 * that is NOT a plain version number is the more honest fixture.
 *
 * Not shipped: CMake puts it in build/plugins-abitest.
 */
#include "ps/ps_plugin.h"
#include "abi_twin_body.h"

/* The literals live in macros because test_abi_probe.c #includes this file and
 * asserts on the exact bytes the host must carry - one copy, or the test can
 * agree with itself while disagreeing with the dll. */
#define ABI_TWIN_V3_VERSION  "3.1.4-twin (2026-08-10)"
#define ABI_TWIN_V3_HEADLINE "mean"

static int32_t analyzeV3(const psFrame* in, const psRect* roi,
                         const psAnalyzeSink3* sink, char* err, size_t err_cap) {
    /* The probe rule applies inside the sink too: the seats docs/abi-v3.md §8
     * spends on emit_number_u / emit_map are NULL until that stage lands, so
     * this analyzer emits through the three that abi_version 3 promises and
     * touches no seat it has not tested. §8.2 keeps the key-name unit
     * convention working as exactly this fallback. */
    return abiTwinAnalyze(in, roi, sink->ctx, sink->emit_number, sink->emit_text,
                          err, err_cap);
}

static const psAnalyzerV3 DESC = {
    3u, PS_CAP_CPU, "abitest/twin-v3",
    ABI_TWIN_V3_VERSION,
    "compat fixture: the same measurement as twin-v2, declared as V3",
    NULL,                               /* params_schema: reserved */
    ABI_TWIN_V3_HEADLINE,
    analyzeV3, { 0 }
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    /* the header's standing rule: too old a host, register NOTHING and say so */
    if (!host || host->abi_version < PS_ABI_VERSION) return 1;
    /* ...and the probe (§2.2): a version number promises the v3 CORE, and a
     * seat is still asked for by NULL test. A host that says 3 with this seat
     * empty is a host that cannot take v3 analyzers - decline, do not call. */
    if (!host->register_analyzer3) return 1;
    return host->register_analyzer3(host->ctx, &DESC);
}
