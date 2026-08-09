/* abi_v2_twin.c - the V2 half of the ABI compatibility pair.
 *
 * Deliberately written the way a THIRD-PARTY dll built before v3 was written:
 * V2 descriptor, `host->abi_version < 2u` gate, register_analyzer2. Nothing in
 * this file knows that v3 exists, and nothing in it may ever need to - that is
 * what docs/abi-v3.md §2.1 promises ("既存バイナリの動作そのもの。何も要らない")
 * and what --anaprov-selftest checks against the v3 host it is loaded into.
 *
 * Not shipped: CMake puts it in build/plugins-abitest, which only the selftest
 * loads by name, so the viewer's analyzer list is unchanged.
 */
#include "ps/ps_plugin.h"
#include "abi_twin_body.h"

static int32_t analyzeV2(const psFrame* in, const psRect* roi,
                         const psAnalyzeSink2* sink, char* err, size_t err_cap) {
    return abiTwinAnalyze(in, roi, sink->ctx, sink->emit_number, sink->emit_text,
                          err, err_cap);
}

static const psAnalyzerV2 DESC = {
    2u, PS_CAP_CPU, "abitest/twin-v2",
    "compat fixture: the same measurement as twin-v3, declared as V2",
    NULL, analyzeV2, { 0 }
};

PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host) {
    if (!host || host->abi_version < 2u || !host->register_analyzer2) return 1;
    return host->register_analyzer2(host->ctx, &DESC);
}
