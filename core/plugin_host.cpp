#include "plugin_host.h"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
  #include <unistd.h>
  #include <limits.h>
#endif
#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

// The v3 promise, asked of the compiler instead of trusted (docs/abi-v3.md
// §2.1: "psHostApi の寸法は変わらない"). A third-party dll built against the v1
// or v2 header is a fixed byte layout on someone's disk; v3 renames reserved
// slots and adds structs, and MUST move nothing. If a future edit inserts a
// field instead of taking a reserved seat, this is where it stops - at compile
// time, in the host, rather than as garbage read out of an old plugin.
static_assert(sizeof(psHostApi) == 2 * sizeof(uint32_t) + 15 * sizeof(void*),
              "psHostApi changed size - every already-built plugin now reads "
              "the wrong bytes (docs/abi-v3.md §2.1, §12)");
static_assert(offsetof(psHostApi, register_analyzer2) ==
                  2 * sizeof(uint32_t) + 7 * sizeof(void*),
              "the v2 register slot moved - v2 plugins would call the wrong one");
static_assert(offsetof(psHostApi, register_analyzer3) ==
                  offsetof(psHostApi, register_analyzer2) + sizeof(void*),
              "register_analyzer3 must be the v2 header's reserved[0], nothing else");
static_assert(offsetof(psHostApi, register_stack_analyzer3) ==
                  offsetof(psHostApi, register_analyzer3) + sizeof(void*),
              "the stack mouth must be the seat right after register_analyzer3 "
              "(docs/abi-v3.md §12) - a plugin built against a header that said "
              "so would otherwise call whatever moved into it");

namespace {

std::vector<void*>               g_handles;
std::vector<DisplayPluginInfo>   g_displays;
std::vector<AnalyzerPluginInfo>  g_analyzers;
std::vector<StackAnalyzerPluginInfo> g_stackAnalyzers;
std::vector<ProcessorPluginInfo> g_processors;
std::function<void(const std::string&, bool)> g_log;
std::string g_loading;            // filename currently registering, for log prefixes
std::string g_loadingPath;        // ...and its absolute path: the ledger's source
                                  // of truth for "which file registered this"

void logMsg(const std::string& m, bool err) {
    if (g_log) g_log(m, err);
    else fprintf(stderr, "%s\n", m.c_str());
}

/* Pixel memory crossing the ABI is allocated and freed HERE, in the host,
 * so mismatched CRTs on Windows can never corrupt a heap. */
void* hostFrameAlloc(void*, size_t n) { return std::malloc(n); }
void  hostFrameFree(void*, void* p)   { std::free(p); }

void hostLog(void*, int32_t level, const char* msg) {
    std::string m = msg ? msg : "";
    logMsg(g_loading.empty() ? m : g_loading + ": " + m, level >= 2);
}

bool validCommon(uint32_t abi, uint32_t expected, uint32_t caps, const char* name, const void* fn) {
    if (abi != expected) {
        logMsg(g_loading + ": descriptor version v" + std::to_string(abi) +
               " does not match expected v" + std::to_string(expected) + " - rejected", true);
        return false;
    }
    if (!(caps & PS_CAP_CPU)) {   // contract: CPU implementation is mandatory
        logMsg(g_loading + ": plugin has no CPU implementation - rejected", true);
        return false;
    }
    if (!name || !name[0] || !fn) {
        logMsg(g_loading + ": invalid plugin descriptor - rejected", true);
        return false;
    }
    return true;
}
template <class V>
bool dupName(const V& v, const char* name) {
    for (const auto& e : v)
        if (e.name == name) {
            logMsg(g_loading + ": duplicate plugin name '" + name + "' - skipped", true);
            return true;
        }
    return false;
}

int32_t hostRegisterDisplay(void*, const psDisplayV1* d) {
    if (!d || !validCommon(d->abi_version, 1, d->caps, d->name, (const void*)d->fill_lut)) return 1;
    if (dupName(g_displays, d->name)) return 1;
    DisplayPluginInfo info;
    info.name = d->name;
    d->fill_lut(info.lut.data(), 256);   // called once; LUT cached, plugin fn never hot
    g_displays.push_back(std::move(info));
    return 0;
}
int32_t hostRegisterAnalyzer(void*, const psAnalyzerV1* a) {
    if (!a || !validCommon(a->abi_version, 1, a->caps, a->name, (const void*)a->analyze)) return 1;
    if (dupName(g_analyzers, a->name)) return 1;
    AnalyzerPluginInfo info;
    info.name = a->name;
    // the ledger (#46 stage 1): registration happens inside fn(&g_api), so the
    // file being loaded RIGHT NOW is the one this analyzer came from - recorded
    // here because no later moment can know it
    info.file = g_loading;
    info.path = g_loadingPath;
    info.abi = 1;                     // version stays empty: V1 declares none
    info.v1 = *a;
    g_analyzers.push_back(std::move(info));
    return 0;
}
int32_t hostRegisterAnalyzer2(void*, const psAnalyzerV2* a) {
    if (!a || !validCommon(a->abi_version, 2, a->caps, a->name, (const void*)a->analyze)) return 1;
    if (dupName(g_analyzers, a->name)) return 1;
    AnalyzerPluginInfo info;
    info.name = a->name;
    info.desc = a->description ? a->description : "";
    info.file = g_loading;            // the ledger, same as the V1 register above
    info.path = g_loadingPath;
    info.abi = 2;                     // version stays empty: V2 declares none
    info.v2 = *a;
    g_analyzers.push_back(std::move(info));
    return 0;
}
int32_t hostRegisterAnalyzer3(void*, const psAnalyzerV3* a) {
    if (!a || !validCommon(a->abi_version, 3, a->caps, a->name, (const void*)a->analyze)) return 1;
    // The one check v3 adds (docs/abi-v3.md §3.1). Declaring v3 IS declaring a
    // version: the freedom to stay anonymous lives in V1/V2, which load
    // forever, so a v3 descriptor without one is a contradiction. Refused with
    // its reason on the line rather than filled in by the host - a version the
    // host invented would be the host citing itself.
    if (!a->version || !a->version[0]) {
        logMsg(g_loading + ": v3 analyzer '" + a->name +
               "' declares no version - rejected (ABI v3 requires a non-empty one)", true);
        return 1;
    }
    if (dupName(g_analyzers, a->name)) return 1;
    AnalyzerPluginInfo info;
    info.name = a->name;
    info.desc = a->description ? a->description : "";
    info.version = a->version;        // verbatim, and that is the whole contract:
                                      // never parsed, never ordered, never normalized
    info.headline = a->headline ? a->headline : "";   // NULL = no headline (§3.2)
    info.file = g_loading;            // the ledger, same as the V1/V2 registers above
    info.path = g_loadingPath;
    info.abi = 3;
    info.v3 = *a;
    g_analyzers.push_back(std::move(info));
    return 0;
}
int32_t hostRegisterStackAnalyzer3(void*, const psStackAnalyzerV3* a) {
    if (!a || !validCommon(a->abi_version, 3, a->caps, a->name,
                           (const void*)a->analyze_stack)) return 1;
    // §3.1, exactly as the frame register above: declaring v3 IS declaring a
    // version, and the host never invents one.
    if (!a->version || !a->version[0]) {
        logMsg(g_loading + ": v3 stack analyzer '" + a->name +
               "' declares no version - rejected (ABI v3 requires a non-empty one)", true);
        return 1;
    }
    // ...and the check §6 adds. min_frames is the plugin's ONE veto over how
    // little data it will be run on, and it is spent at registration rather
    // than per call: sigma_t needs 2, an RTS study needs orders more, and the
    // host cannot know which. A 0 here would have to mean something, and every
    // meaning available ("any", "don't care", "the author forgot") is a
    // default the host would be inventing on the author's behalf. So it is
    // refused, and the refusal is also the one moment that asks an author why
    // this is a stack analyzer at all.
    if (a->min_frames == 0) {
        logMsg(g_loading + ": v3 stack analyzer '" + a->name +
               "' declares min_frames = 0 - rejected (ABI v3 §6: declare how many "
               "frames the measurement needs; write 1 if one will do)", true);
        return 1;
    }
    // One namespace across both mouths: "category/name" is the identity the
    // Measure menu shows and the identity remote parity compares (§10), so a
    // frame analyzer and a stack analyzer may not share one.
    if (dupName(g_analyzers, a->name) || dupName(g_stackAnalyzers, a->name)) return 1;
    StackAnalyzerPluginInfo info;
    info.name = a->name;
    info.desc = a->description ? a->description : "";
    info.version = a->version;        // verbatim: never parsed, ordered or normalized
    info.headline = a->headline ? a->headline : "";
    info.minFrames = a->min_frames;
    info.file = g_loading;            // the ledger, same as every register above
    info.path = g_loadingPath;
    info.abi = 3;
    info.v3 = *a;
    g_stackAnalyzers.push_back(std::move(info));
    return 0;
}
int32_t hostRegisterProcessor(void*, const psProcessorV1* p) {
    if (!p || !validCommon(p->abi_version, 1, p->caps, p->name, (const void*)p->process)) return 1;
    if (dupName(g_processors, p->name)) return 1;
    g_processors.push_back({ p->name, *p });
    return 0;
}

psHostApi g_api = {
    PS_ABI_VERSION, (uint32_t)sizeof(psHostApi), nullptr,
    hostLog, hostFrameAlloc, hostFrameFree,
    hostRegisterDisplay, hostRegisterAnalyzer, hostRegisterProcessor,
    hostRegisterAnalyzer2, hostRegisterAnalyzer3, hostRegisterStackAnalyzer3,
    {}                                // the seats after it stay NULL on purpose:
                                      // docs/abi-v3.md §7 (series) and §8 are later
                                      // stages, and a plugin asks by NULL test, not
                                      // by version
};

} // namespace

namespace plugin_host {

std::string exeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    return fs::path(std::wstring(buf, buf + n)).parent_path().u8string();
#elif defined(__APPLE__)
    uint32_t sz = 0;
    _NSGetExecutablePath(nullptr, &sz);
    std::vector<char> buf(sz + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &sz) != 0) return ".";
    return fs::path(buf.data()).parent_path().u8string();
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return fs::path(buf).parent_path().u8string();
#endif
}

void loadAll(const std::vector<std::string>& dirsUtf8,
             const std::function<void(const std::string&, bool)>& logFn) {
    g_log = logFn;
    std::set<std::string> seenBasenames;   // same file in dev+deploy dirs: load once
    for (const auto& dirU8 : dirsUtf8) {
        std::error_code ec;
        fs::path dir = fs::u8path(dirU8);
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2)) continue;
            std::string ext = e.path().extension().u8string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
#if defined(_WIN32)
            bool ok = ext == ".dll";
#elif defined(__APPLE__)
            bool ok = ext == ".so" || ext == ".dylib";   // CMake MODULE default is .so
#else
            bool ok = ext == ".so";
#endif
            if (ok) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());           // deterministic UI order
        for (const auto& p : files) {
            std::string base = p.filename().u8string();
            if (!seenBasenames.insert(base).second) continue;
#if defined(_WIN32)
            void* h = (void*)LoadLibraryW(p.wstring().c_str());
#else
            void* h = dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
            if (!h) { logMsg("plugin load failed: " + base, true); continue; }
#if defined(_WIN32)
            auto fn = (psRegisterPluginsFn)(void*)GetProcAddress((HMODULE)h, "psRegisterPlugins");
#else
            auto fn = (psRegisterPluginsFn)dlsym(h, "psRegisterPlugins");
#endif
            if (!fn) {
                logMsg(base + ": no psRegisterPlugins export - not a viewer plugin, skipped", true);
#if defined(_WIN32)
                FreeLibrary((HMODULE)h);
#else
                dlclose(h);
#endif
                continue;
            }
            g_loading = base;
            g_loadingPath = p.u8string();
            // Counted over EVERY list, stack analyzers included: this number is
            // what decides "registered nothing, so unload it again", and a dll
            // that registers only a stack analyzer would otherwise be dropped
            // on the floor the moment it succeeded.
            auto registeredCount = [] {
                return g_displays.size() + g_analyzers.size() +
                       g_stackAnalyzers.size() + g_processors.size();
            };
            size_t before = registeredCount();
            int32_t rc = fn(&g_api);
            size_t added = registeredCount() - before;
            g_loading.clear();
            g_loadingPath.clear();
            if (rc != 0 && added == 0) {
                logMsg(base + ": registration failed (ABI mismatch?)", true);
#if defined(_WIN32)
                FreeLibrary((HMODULE)h);
#else
                dlclose(h);
#endif
                continue;
            }
            g_handles.push_back(h);
            logMsg("plugin loaded: " + base + " (" + std::to_string(added) + " plugin(s))", false);
        }
    }
}

void unloadAll() {
    g_displays.clear(); g_analyzers.clear(); g_stackAnalyzers.clear();
    g_processors.clear();
    for (void* h : g_handles) {
#if defined(_WIN32)
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
    }
    g_handles.clear();
}

const std::vector<DisplayPluginInfo>&   displays()   { return g_displays; }
const std::vector<AnalyzerPluginInfo>&  analyzers()  { return g_analyzers; }
const std::vector<StackAnalyzerPluginInfo>& stackAnalyzers() { return g_stackAnalyzers; }
const std::vector<ProcessorPluginInfo>& processors() { return g_processors; }
const psHostApi* hostApi() { return &g_api; }
void frameFree(void* p) { hostFrameFree(nullptr, p); }

} // namespace plugin_host
