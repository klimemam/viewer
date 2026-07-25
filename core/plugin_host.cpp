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

namespace {

std::vector<void*>               g_handles;
std::vector<DisplayPluginInfo>   g_displays;
std::vector<AnalyzerPluginInfo>  g_analyzers;
std::vector<ProcessorPluginInfo> g_processors;
std::function<void(const std::string&, bool)> g_log;
std::string g_loading;            // filename currently registering, for log prefixes

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
    info.isV2 = false;
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
    info.isV2 = true;
    info.v2 = *a;
    g_analyzers.push_back(std::move(info));
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
    hostRegisterAnalyzer2, {}
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
            size_t before = g_displays.size() + g_analyzers.size() + g_processors.size();
            int32_t rc = fn(&g_api);
            size_t added = g_displays.size() + g_analyzers.size() + g_processors.size() - before;
            g_loading.clear();
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
    g_displays.clear(); g_analyzers.clear(); g_processors.clear();
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
const std::vector<ProcessorPluginInfo>& processors() { return g_processors; }
const psHostApi* hostApi() { return &g_api; }
void frameFree(void* p) { hostFrameFree(nullptr, p); }

} // namespace plugin_host
