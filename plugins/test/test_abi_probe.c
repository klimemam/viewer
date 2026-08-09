/* test_abi_probe.c - the probe rule (docs/abi-v3.md §2.2) from the plugin's
 * side, which is the side nobody can observe from inside the viewer.
 *
 * §2.2 is the reason v3 is meant to be the last version bump: new host
 * functions arrive in RESERVED SEATS, and a plugin asks "is that seat taken?"
 * with a NULL test rather than with a version number. The claim has three
 * halves and all three are about hosts this repo does not contain - an ABI-1
 * host, an ABI-2 host, and a v3 host that has not implemented the seat yet -
 * so they are built here, as four plain structs, and the real plugin is
 * #included and asked what it does with each. The alternative (drive it
 * through plugin_host) can only ever present the ONE host this build has.
 *
 * #including the .c is the test_prnu / test_display_falsecolor pattern, for
 * the same reason: psRegisterPlugins is the dll's only export and the
 * descriptor behind it is static, so there is nothing to link against.
 */
#include "abi_v3_twin.c"

#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void check(int cond, const char* what) {
    printf("abiprobe: %-62s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_fails++;
}

/* ---- a host, assembled seat by seat ---------------------------------- */

static int g_registered3 = 0;
static const psAnalyzerV3* g_last3 = NULL;

static int32_t takeAnalyzer3(void* ctx, const psAnalyzerV3* a) {
    (void)ctx;
    g_registered3++;
    g_last3 = a;
    return 0;
}
static int32_t takeAnalyzer2(void* ctx, const psAnalyzerV2* a) {
    (void)ctx; (void)a;
    printf("abiprobe: FAIL - a v3 plugin reached the V2 register slot\n");
    g_fails++;
    return 1;
}
/* The stack mouth's seat (docs/abi-v3.md §5). A frame analyzer must never
 * arrive here: the two seats are adjacent, so a one-slot offset error lands
 * exactly in this function, and reaching it silently is precisely the failure
 * an already-built dll would suffer. */
static int32_t takeStackAnalyzer3(void* ctx, const psStackAnalyzerV3* a) {
    (void)ctx; (void)a;
    printf("abiprobe: FAIL - a v3 FRAME analyzer reached the STACK register slot\n");
    g_fails++;
    return 1;
}
static void hostLog(void* ctx, int32_t level, const char* msg) {
    (void)ctx; (void)level; (void)msg;
}

/* Zero-initialized, then filled: a reserved seat's value is "whatever the host
 * left there", and every host since v1 has been required to leave zero. */
static psHostApi makeHost(uint32_t abi, int takeSeat3) {
    psHostApi h;
    memset(&h, 0, sizeof h);
    h.abi_version = abi;
    h.struct_size = (uint32_t)sizeof(psHostApi);
    h.log = hostLog;
    if (abi >= 2) h.register_analyzer2 = takeAnalyzer2;
    if (takeSeat3) {
        h.register_analyzer3 = takeAnalyzer3;
        /* The v3 CORE is TWO seats since stage 2 (§2.2), so a host that has
         * taken the frame one has taken this one as well - and filling it here
         * is what makes an off-by-one-slot call land in a named function that
         * says so, rather than in whatever zero happens to be next. */
        h.register_stack_analyzer3 = takeStackAnalyzer3;
    }
    return h;
}

/* ---- the sink a stage-1 host offers: three emits, every seat empty ---- */

static int g_numbers = 0, g_texts = 0;
static char g_lastKey[64] = { 0 };
static double g_lastNum = 0.0;

static void sinkNumber(void* ctx, const char* key, double v) {
    (void)ctx;
    g_numbers++;
    snprintf(g_lastKey, sizeof g_lastKey, "%s", key);
    g_lastNum = v;
}
static void sinkText(void* ctx, const char* key, const char* v) {
    (void)ctx; (void)key; (void)v;
    g_texts++;
}
static void sinkSeries(void* ctx, const char* name, const char* xl, const char* yl,
                       const float* x, const float* y, uint32_t n) {
    (void)ctx; (void)name; (void)xl; (void)yl; (void)x; (void)y; (void)n;
}

int main(void) {
    /* ---- the seats a v3 plugin may not assume ------------------------- */
    {
        psHostApi h1 = makeHost(1u, 0);          /* an ABI-1 host: zero tail  */
        psHostApi h2 = makeHost(2u, 0);          /* an ABI-2 host             */
        psHostApi h3empty = makeHost(3u, 0);     /* says 3, seat NOT taken    */
        int32_t rc1, rc2, rc3;

        g_registered3 = 0;
        rc1 = psRegisterPlugins(&h1);
        check(rc1 != 0 && g_registered3 == 0,
              "PR1 ABI-1 host: registers nothing, returns nonzero");

        g_registered3 = 0;
        rc2 = psRegisterPlugins(&h2);
        check(rc2 != 0 && g_registered3 == 0,
              "PR2 ABI-2 host: registers nothing, returns nonzero");

        /* The one the version number cannot answer: a host whose abi_version
         * says 3 while the seat is still empty. Reading it is safe only
         * because reserved fields are zero-filled - the whole basis of §2.2. */
        g_registered3 = 0;
        rc3 = psRegisterPlugins(&h3empty);
        check(rc3 != 0 && g_registered3 == 0,
              "PR3 v3 host, seat NOT taken: NULL-probed, declined, no call");

        check(h3empty.register_analyzer3 == NULL &&
              h1.register_analyzer3 == NULL && h2.register_analyzer3 == NULL,
              "PR4 an untaken seat reads NULL on every host generation");
        /* The same question for the seat stage 2 named. It matters most on the
         * ABI-1 host: that struct was written before this field had a name, and
         * the ONLY reason reading it is defined is that reserved fields have
         * been zero-filled since v1. */
        check(h3empty.register_stack_analyzer3 == NULL &&
              h1.register_stack_analyzer3 == NULL &&
              h2.register_stack_analyzer3 == NULL,
              "PR4b ...including the stack seat, on an ABI-1 host that predates it");
    }

    /* ---- and the seat taken: it registers, once, with its declaration -- */
    {
        psHostApi h = makeHost(3u, 1);
        int32_t rc;
        g_registered3 = 0;
        g_last3 = NULL;
        rc = psRegisterPlugins(&h);
        check(rc == 0 && g_registered3 == 1 && g_last3 != NULL,
              "PR5 v3 host, seat taken: registers exactly one descriptor");
        if (g_last3) {
            check(g_last3->abi_version == 3u && (g_last3->caps & PS_CAP_CPU) != 0,
                  "PR6 the descriptor is v3 and declares a CPU implementation");
            check(g_last3->version != NULL && g_last3->version[0] != '\0' &&
                  strcmp(g_last3->version, ABI_TWIN_V3_VERSION) == 0,
                  "PR7 version is non-empty and is the declared string, verbatim");
            check(g_last3->headline != NULL &&
                  strcmp(g_last3->headline, ABI_TWIN_V3_HEADLINE) == 0,
                  "PR8 headline is the declared key (§3.2), not a host guess");
        }
    }

    /* ---- a v3 analyzer run by a host that implements NO sink seat ------
     * The stage-1 host has emit_number_u / emit_map still empty (§8 is a later
     * stage). A v3 analyzer must produce its numbers anyway - if it could not,
     * "filling a seat is not a version bump" would be false the first time it
     * mattered. */
    {
        psAnalyzeSink3 sink;
        psFrame f;
        float px[4 * 4];
        char err[128];
        int32_t rc;
        int i;
        size_t s;
        const unsigned char* seats;
        int allZero = 1;

        memset(&sink, 0, sizeof sink);
        sink.emit_number = sinkNumber;
        sink.emit_text = sinkText;
        sink.emit_series = sinkSeries;

        seats = (const unsigned char*)(const void*)&sink.reserved[0];
        for (s = 0; s < sizeof sink.reserved; s++)
            if (seats[s] != 0) allZero = 0;
        check(allZero, "PR9 the sink's unfilled seats are zero, so probing them is safe");

        memset(&f, 0, sizeof f);
        for (i = 0; i < 16; i++) px[i] = (float)(100 + i);
        f.w = 4; f.h = 4; f.ch = 1;
        f.dtype = PS_DTYPE_F32;
        f.loc = PS_MEM_CPU;
        f.data = px;
        f.pitch_bytes = 4 * sizeof(float);
        f.pts_us = -1;
        f.name = "abiprobe";

        g_numbers = 0; g_texts = 0;
        err[0] = 0;
        rc = g_last3 ? g_last3->analyze(&f, NULL, &sink, err, sizeof err) : 1;
        check(rc == 0 && g_numbers == 2 && g_texts == 1,
              "PR10 v3 analyze() runs on a sink whose §8 seats are empty");
        check(rc == 0 && strcmp(g_lastKey, "ch0.mean") == 0 && g_lastNum == 107.5,
              "PR11 ...and produces the measurement, unchanged");
    }

    /* ---- the layout an already-built plugin depends on ------------------
     * v3 renames reserved slots and adds structs. A dll compiled against the
     * v1 or v2 header is a fixed byte layout on someone's disk: if any of this
     * moved, that dll would call the wrong function pointer. */
    {
        check(sizeof(psHostApi) == 2 * sizeof(uint32_t) + 15 * sizeof(void*),
              "PR12 psHostApi is the size it has been since v1");
        check(offsetof(psHostApi, register_analyzer2) ==
                  2 * sizeof(uint32_t) + 7 * sizeof(void*) &&
              offsetof(psHostApi, register_analyzer3) ==
                  offsetof(psHostApi, register_analyzer2) + sizeof(void*),
              "PR13 register_analyzer3 IS the v2 header's reserved[0]");
        /* Stage 2 spends the next seat. Naming one is not a version bump, and
         * the price of that promise is that it may only ever be the NEXT one:
         * anything else moves a slot an already-built dll calls by offset. */
        check(offsetof(psHostApi, register_stack_analyzer3) ==
                  offsetof(psHostApi, register_analyzer3) + sizeof(void*),
              "PR13b register_stack_analyzer3 is the seat right after it");
        check(sizeof(psStack) > 0 && sizeof(psStackAnalyzerV3) > 0 &&
              offsetof(psStackAnalyzerV3, name) ==
                  offsetof(psStackAnalyzerV3, _pad) + sizeof(uint32_t),
              "PR13c psStackAnalyzerV3's _pad really does align the pointers");
        check(PS_ABI_VERSION == 3u, "PR14 PS_ABI_VERSION is 3");
    }

    printf("abiprobe: %s\n", g_fails ? "FAILED" : "ok");
    return g_fails ? 1 : 0;
}
