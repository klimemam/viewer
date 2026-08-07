#pragma once
#include "ps/ps_plugin.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

struct DisplayPluginInfo   { std::string name; std::array<uint8_t, 256 * 3> lut; };
struct AnalyzerPluginInfo  {
    std::string name, desc;          // desc: V2 self-declared precondition hint
    // The host's own ledger (#46 stage 1): which file registered this analyzer.
    // Results cite it as provenance. This never crosses the ABI - the host is
    // the one doing the loading, so it needs no field in psAnalyzerV1/V2. A
    // VERSION, by contrast, only the plugin can declare: that field waits for
    // ABI v3 (docs/analysis-layers.md §8, 判断record 6 - one ABI bump, not two).
    std::string file;                // basename, e.g. "analyzer_stats.dll"
    std::string path;                // absolute path of that file, UTF-8
    bool isV2 = false;
    psAnalyzerV1 v1{};
    psAnalyzerV2 v2{};
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
