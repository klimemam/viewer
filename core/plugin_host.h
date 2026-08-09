#pragma once
#include "ps/ps_plugin.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

struct DisplayPluginInfo   { std::string name; std::array<uint8_t, 256 * 3> lut; };
struct AnalyzerPluginInfo  {
    std::string name, desc;          // desc: V2/V3 self-declared precondition hint
    // The host's own ledger (#46 stage 1): which file registered this analyzer.
    // Results cite it as provenance. This never crosses the ABI - the host is
    // the one doing the loading, so it needs no field in psAnalyzerV1/V2.
    std::string file;                // basename, e.g. "analyzer_stats.dll"
    std::string path;                // absolute path of that file, UTF-8
    // ...and the other half (#46 stage 2, ABI v3 §3.1/§11): what only the
    // PLUGIN can know. Carried verbatim, never parsed, never ordered.
    // EMPTY for V1/V2 descriptors and it stays empty: those structs have no
    // version field, and a host that filled it from the dll's file-version
    // resource or from a guess would be citing itself as the declarer.
    std::string version;
    std::string headline;            // v3 §3.2: the accented key, plugin-declared.
                                     // Empty for V1/V2 - those still go through
                                     // the host's own table (analyzers.md).
    uint32_t abi = 1;                // which descriptor registered it: 1 / 2 / 3
    psAnalyzerV1 v1{};
    psAnalyzerV2 v2{};
    psAnalyzerV3 v3{};
};
struct ProcessorPluginInfo { std::string name; psProcessorV1 v; };

namespace plugin_host {

// Scan each dir (non-recursive) for *.dll / *.so / *.dylib, load and register.
// logFn(message, isError) — the host wires this to a toast / stderr.
void loadAll(const std::vector<std::string>& dirsUtf8,
             const std::function<void(const std::string&, bool)>& logFn);
void unloadAll();                                   // call at exit only (v1: no hot reload)

const std::vector<DisplayPluginInfo>&   displays();
const std::vector<AnalyzerPluginInfo>&  analyzers();
const std::vector<ProcessorPluginInfo>& processors();

const psHostApi* hostApi();                         // for calling process()
std::string exeDir();                               // absolute dir of the binary, UTF-8
void frameFree(void* p);                            // host-side free for psFrame::data

} // namespace plugin_host
