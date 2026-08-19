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
                                     // the host's own table (docs/reference/analyzers.md).
    uint32_t abi = 1;                // which descriptor registered it: 1 / 2 / 3
    psAnalyzerV1 v1{};
    psAnalyzerV2 v2{};
    psAnalyzerV3 v3{};
};
// A STACK analyzer (ABI v3 §5.2). Its own list, not a flag on the one above:
// the two have different signatures, and the descriptor a caller must reach for
// is decided by which list it came out of rather than by reading a kind field
// and hoping. The ledger columns are the frame analyzer's, for the same reasons
// - the file/path half is the host's knowledge (#46 stage 1), the version half
// is the plugin's declaration (#46 stage 2) and stays verbatim.
struct StackAnalyzerPluginInfo {
    std::string name, desc, file, path, version, headline;
    // The DECLARATION the host gates on before it calls (docs/reference/abi-v3.md §6).
    // Never defaulted: 0 is refused at registration, so a value here was
    // written by the plugin's author on purpose.
    uint32_t minFrames = 1;
    uint32_t abi = 3;                // no V1/V2 stack analyzer exists, or ever will
    psStackAnalyzerV3 v3{};
};

struct ProcessorPluginInfo { std::string name; psProcessorV1 v; };

// ---- how an analyzer is CITED, spelled once for every mouth that cites one --
//
// Lives here, beside the ledger these two read from, for the reason
// rp::npyNotNativeText lives in the header both doors include: a result
// measured on a peer and the same result measured locally are read by one
// person in one window, and a citation that differs by a space or a bracket
// between the two reads as two different plugins. There were two mouths when
// this was a static in panel_analysis.inc; §10 makes it four (status line,
// tooltip, TSV/CSV comment, and the peer's ledger row coming back over the
// wire), which is one more than a copy survives.
//
// The version is angle-bracketed and comes FIRST because a path may contain
// spaces and a trailing token after one cannot be told from part of it. An
// empty version is ABSENT, not blank: those descriptors are V1/V2, which have
// no version field, and an empty <> would read as "declared nothing" where the
// truth is "was never asked". Nothing here invents one.
inline std::string analyzerCitation(const std::string& version, const std::string& file) {
    return version.empty() ? file : "<" + version + "> " + file;
}
// The provenance line's tail: "name <version> (dll)" - what a screenshot has to
// answer without the session around it. Same absence rule as above.
inline std::string analyzerTag(const std::string& name, const std::string& version,
                               const std::string& file) {
    std::string s = name;
    if (!version.empty()) s += " <" + version + ">";
    if (!file.empty())    s += " (" + file + ")";
    return s;
}

namespace plugin_host {

// Scan each dir (non-recursive) for *.dll / *.so / *.dylib, load and register.
// logFn(message, isError) — the host wires this to a toast / stderr.
void loadAll(const std::vector<std::string>& dirsUtf8,
             const std::function<void(const std::string&, bool)>& logFn);
void unloadAll();                                   // call at exit only (v1: no hot reload)

const std::vector<DisplayPluginInfo>&   displays();
const std::vector<AnalyzerPluginInfo>&  analyzers();       // frame analyzers
const std::vector<StackAnalyzerPluginInfo>& stackAnalyzers();
const std::vector<ProcessorPluginInfo>& processors();

const psHostApi* hostApi();                         // for calling process()
std::string exeDir();                               // absolute dir of the binary, UTF-8
void frameFree(void* p);                            // host-side free for psFrame::data

} // namespace plugin_host
