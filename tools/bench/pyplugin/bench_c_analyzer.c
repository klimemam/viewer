/* bench_c_analyzer.c - how long the EXISTING C analyzers take on one frame.
 *
 *   STUDY HARNESS ONLY - docs/python-plugins.md. NOT part of the viewer build,
 *   NOT referenced by CMakeLists.txt. It is a standalone main() that dlopens
 *   the already-built plugin DLLs; deleting this directory changes no shipped
 *   byte.
 *
 * Why it exists: every overhead in the Python study has to be judged against
 * the work it would wrap. "40 ms of transport" means one thing next to a 5 ms
 * analyzer and something else entirely next to a 300 ms one.
 *
 * Build (mingw, from the repo root):
 *   gcc -O2 -Iinclude tools/pyplugin-bench/bench_c_analyzer.c \
 *       -o build-mingw/bench_c_analyzer.exe
 * Run:
 *   build-mingw/bench_c_analyzer.exe build-mingw/plugins [reps]
 *
 * It loads every plugin in the directory, then runs each registered analyzer
 * over a synthetic 4000x3000 float32 frame (whole frame, and a centre-quarter
 * ROI) and reports median / min / max in ms.
 */
#include "ps/ps_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HMODULE lib_t;
static lib_t lib_open(const char* p) { return LoadLibraryA(p); }
static void* lib_sym(lib_t h, const char* n) { return (void*)GetProcAddress(h, n); }
static double now_ms(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}
#else
#  include <dlfcn.h>
#  include <time.h>
typedef void* lib_t;
static lib_t lib_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* lib_sym(lib_t h, const char* n) { return dlsym(h, n); }
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

#define MAXA 32
static struct { char name[128]; int isV2; psAnalyzerV1 v1; psAnalyzerV2 v2; } g_a[MAXA];
static int g_n = 0;

static void h_log(void* c, int32_t lvl, const char* m) { (void)c; (void)lvl; (void)m; }
static void* h_alloc(void* c, size_t n) { (void)c; return malloc(n); }
static void  h_free(void* c, void* p) { (void)c; free(p); }
static int32_t h_disp(void* c, const psDisplayV1* d) { (void)c; (void)d; return 0; }
static int32_t h_proc(void* c, const psProcessorV1* p) { (void)c; (void)p; return 0; }
static int32_t h_ana(void* c, const psAnalyzerV1* a) {
    (void)c;
    if (g_n >= MAXA || !a || !a->name) return 1;
    snprintf(g_a[g_n].name, sizeof g_a[g_n].name, "%s", a->name);
    g_a[g_n].isV2 = 0; g_a[g_n].v1 = *a; g_n++;
    return 0;
}
static int32_t h_ana2(void* c, const psAnalyzerV2* a) {
    (void)c;
    if (g_n >= MAXA || !a || !a->name) return 1;
    snprintf(g_a[g_n].name, sizeof g_a[g_n].name, "%s", a->name);
    g_a[g_n].isV2 = 1; g_a[g_n].v2 = *a; g_n++;
    return 0;
}

static int g_nums = 0, g_txts = 0, g_sers = 0;
static void s_num(void* c, const char* k, double v) { (void)c; (void)k; (void)v; g_nums++; }
static void s_txt(void* c, const char* k, const char* v) { (void)c; (void)k; (void)v; g_txts++; }
static void s_ser(void* c, const char* n, const char* xl, const char* yl,
                  const float* x, const float* y, uint32_t cnt) {
    (void)c; (void)n; (void)xl; (void)yl; (void)x; (void)y; (void)cnt; g_sers++;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "build-mingw/plugins";
    int reps = argc > 2 ? atoi(argv[2]) : 15;
    const uint32_t W = 4000, Hh = 3000;

    psHostApi api;
    memset(&api, 0, sizeof api);
    api.abi_version = PS_ABI_VERSION;
    api.struct_size = (uint32_t)sizeof(psHostApi);
    api.log = h_log; api.frame_alloc = h_alloc; api.frame_free = h_free;
    api.register_display = h_disp; api.register_analyzer = h_ana;
    api.register_processor = h_proc; api.register_analyzer2 = h_ana2;

    static const char* names[] = {
        "analyzer_stats", "analyzer_noise", "analyzer_prnu",
        "analyzer_sharpness", "analyzer_esfr", NULL
    };
    for (int i = 0; names[i]; i++) {
        char path[512];
#if defined(_WIN32)
        snprintf(path, sizeof path, "%s/%s.dll", dir, names[i]);
#else
        snprintf(path, sizeof path, "%s/lib%s.so", dir, names[i]);
#endif
        lib_t h = lib_open(path);
        if (!h) { fprintf(stderr, "skip (cannot load): %s\n", path); continue; }
        psRegisterPluginsFn fn = (psRegisterPluginsFn)lib_sym(h, "psRegisterPlugins");
        if (!fn) { fprintf(stderr, "skip (no export): %s\n", path); continue; }
        if (fn(&api) != 0) fprintf(stderr, "note: %s returned nonzero\n", path);
    }
    printf("loaded %d analyzer(s)\n", g_n);

    size_t n = (size_t)W * Hh;
    float* px = (float*)malloc(n * sizeof(float));
    if (!px) { fprintf(stderr, "oom\n"); return 1; }
    /* deterministic pseudo-random field with structure, so tile statistics and
     * gradients are not degenerate */
    uint32_t s = 12345u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        float r = (float)((s >> 8) & 0xFFFFu) / 65535.0f;
        size_t x = i % W, y = i / W;
        px[i] = 0.35f + 0.25f * (float)((x / 512 + y / 512) & 1) + 0.05f * r;
    }

    psFrame fr;
    memset(&fr, 0, sizeof fr);
    fr.w = W; fr.h = Hh; fr.ch = 1; fr.dtype = PS_DTYPE_F32; fr.loc = PS_MEM_CPU;
    fr.data = px; fr.pitch_bytes = (size_t)W * 1 * sizeof(float);
    fr.black = 0.0f; fr.white = 1.0f;
    fr.cfa_type = PS_CFA_NONE; fr.cfa_pattern = PS_CFA_RGGB;
    fr.pts_us = -1; fr.name = "bench";

    /* the same ROI ladder the Python side sweeps, so the two tables can be
     * read against each other size for size */
    const uint32_t sides[] = { 256, 512, 1024, 2048, 0 };   /* 0 = whole frame */
    const char* tags[] = { "256x256", "512x512", "1024x1024", "2048x2048", "whole" };
    const int NSZ = 5;

    printf("frame %ux%ux1 f32 = %.1f MB, reps=%d\n\n",
           W, Hh, (double)(n * 4) / 1e6, reps);
    printf("%-24s %-11s %9s %9s %9s %9s\n",
           "analyzer", "roi", "median", "min", "max", "per-s");

    double* t = (double*)malloc(sizeof(double) * (size_t)reps);
    for (int a = 0; a < g_n; a++) {
        for (int which = 0; which < NSZ; which++) {
            psRect rc_;
            if (sides[which] == 0) { rc_.x = 0; rc_.y = 0; rc_.w = W; rc_.h = Hh; }
            else { rc_.x = (W - sides[which]) / 2; rc_.y = (Hh - sides[which]) / 2;
                   rc_.w = sides[which]; rc_.h = sides[which]; }
            const psRect* roi = &rc_;
            char err[256];
            int bad = 0, keys = 0;
            for (int r = -2; r < reps; r++) {          /* 2 warmups */
                g_nums = g_txts = g_sers = 0;
                err[0] = 0;
                double t0 = now_ms();
                int32_t rc;
                if (g_a[a].isV2) {
                    psAnalyzeSink2 sk; memset(&sk, 0, sizeof sk);
                    sk.emit_number = s_num; sk.emit_text = s_txt; sk.emit_series = s_ser;
                    rc = g_a[a].v2.analyze(&fr, roi, &sk, err, sizeof err);
                } else {
                    psAnalyzeSink sk; memset(&sk, 0, sizeof sk);
                    sk.emit_number = s_num; sk.emit_text = s_txt;
                    rc = g_a[a].v1.analyze(&fr, roi, &sk, err, sizeof err);
                }
                double dt = now_ms() - t0;
                if (r >= 0) t[r] = dt;
                if (rc != 0) bad = 1;
                keys = g_nums + g_txts;
            }
            (void)keys;
            if (bad) {
                printf("%-24s %-11s %9s   (rc!=0: %s)\n",
                       g_a[a].name, tags[which], "-", err);
                continue;
            }
            qsort(t, (size_t)reps, sizeof(double), cmp_d);
            printf("%-24s %-11s %9.2f %9.2f %9.2f %9.1f\n",
                   g_a[a].name, tags[which],
                   t[reps / 2], t[0], t[reps - 1],
                   t[reps / 2] > 0 ? 1000.0 / t[reps / 2] : 0.0);
        }
    }
    free(t);
    free(px);
    return 0;
}
