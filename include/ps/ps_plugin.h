/* ps_plugin.h — viewer plugin ABI v1.
 * Pure C header. This file IS the contract; never break it, only extend
 * via new V2 structs and the reserved fields. */
#ifndef PS_PLUGIN_H
#define PS_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#  define PS_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define PS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PS_ABI_VERSION 2u
/* Host API version history:
 *   1 - display / analyzer / processor V1 structs
 *   2 - adds psAnalyzerV2 (emit_series curves + description) via
 *       psHostApi::register_analyzer2. V1 structs keep abi_version = 1
 *       and remain loadable forever. */

/* ---- enums (fixed values; never renumber) ---- */
typedef enum psDtype {
    PS_DTYPE_U8 = 0, PS_DTYPE_U16 = 1, PS_DTYPE_F16 = 2,
    PS_DTYPE_F32 = 3, PS_DTYPE_BF16 = 4
} psDtype;                      /* v1 host always passes PS_DTYPE_F32 */

typedef enum psMemLoc { PS_MEM_CPU = 0, PS_MEM_CUDA = 1 /* reserved, v2 */ } psMemLoc;

typedef enum psCaps { PS_CAP_CPU = 1, PS_CAP_CUDA = 2 /* reserved, v2 */ } psCaps;

typedef enum psCfaType { PS_CFA_NONE = 0, PS_CFA_BAYER = 1, PS_CFA_QUAD = 2 } psCfaType;

/* Same order as the host's CFA_PATTERNS table. */
typedef enum psCfaPattern {
    PS_CFA_RGGB = 0, PS_CFA_BGGR = 1, PS_CFA_GRBG = 2, PS_CFA_GBRG = 3
} psCfaPattern;

typedef enum psPluginKind {
    PS_PLUGIN_DISPLAY   = 1,    /* value -> color, data untouched      */
    PS_PLUGIN_ANALYZER  = 2,    /* numbers out, pixels untouched       */
    PS_PLUGIN_PROCESSOR = 3,    /* Frame -> new Frame                  */
    PS_PLUGIN_LOADER    = 4,    /* reserved, v2 */
    PS_PLUGIN_SOURCE    = 5     /* reserved, v2 */
} psPluginKind;

/* ---- frame descriptor ---- */
typedef struct psFrame {
    uint32_t    w, h, ch;       /* interleaved HWC layout                        */
    psDtype     dtype;          /* v1: always PS_DTYPE_F32 from the host         */
    psMemLoc    loc;            /* v1: always PS_MEM_CPU                         */
    void*       data;           /* row-major; row i at (char*)data + i*pitch     */
    size_t      pitch_bytes;    /* >= w * ch * elem_size                         */
    float       black, white;   /* current display range hint                    */
    int32_t     cfa_type;       /* psCfaType                                     */
    int32_t     cfa_pattern;    /* psCfaPattern, meaningful if cfa_type != 0     */
    int64_t     pts_us;         /* -1 for stills (reserved for v2 Source)        */
    const char* name;           /* UTF-8; valid ONLY for the duration of a call  */
    const char* meta_json;      /* reserved; may be NULL                         */
    uint64_t    reserved[4];    /* zero-filled by whoever creates the frame      */
} psFrame;

typedef struct psRect { uint32_t x, y, w, h; } psRect;

/* ---- host API ---- */
typedef struct psHostApi psHostApi;

typedef struct psAnalyzeSink {
    void* ctx;
    void (*emit_number)(void* ctx, const char* key, double value);
    void (*emit_text)  (void* ctx, const char* key, const char* value);
} psAnalyzeSink;

/* -- Display: 1ch normalized value -> RGB via LUT.
 * Host requests 256 entries in v1, calls fill_lut ONCE at registration and
 * caches the table. The LUT buffer belongs to the host. -- */
typedef struct psDisplayV1 {
    uint32_t    abi_version;    /* = 1 (version of THIS struct)                  */
    uint32_t    caps;           /* must include PS_CAP_CPU                       */
    const char* name;           /* static lifetime, UTF-8                        */
    void (*fill_lut)(uint8_t* rgb, uint32_t entries);   /* entries*3 bytes RGB   */
    void*       reserved[4];
} psDisplayV1;

/* -- Analyzer: read-only pass over pixels, emits key/value rows.
 * Everything passed to the sink is copied by the host before the call returns.
 * roi == NULL means whole frame. Return 0 on success; nonzero + UTF-8 reason
 * in err (err_cap bytes) on failure. -- */
typedef struct psAnalyzerV1 {
    uint32_t    abi_version;
    uint32_t    caps;
    const char* name;
    int32_t (*analyze)(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink* sink, char* err, size_t err_cap);
    void*       reserved[4];
} psAnalyzerV1;

/* -- Processor: in-frame -> newly allocated out-frame.
 * out->data MUST be allocated with host->frame_alloc and is freed exclusively
 * by the host with host->frame_free. out->dtype MUST be PS_DTYPE_F32 in v1.
 * On failure free your own partial allocations, write a reason into err and
 * return nonzero. -- */
typedef struct psProcessorV1 {
    uint32_t    abi_version;
    uint32_t    caps;           /* PS_CAP_CPU mandatory                          */
    const char* name;
    const char* params_schema;  /* reserved in v1: pass NULL                     */
    int32_t (*process)(const psFrame* in, psFrame* out, const psHostApi* host,
                       char* err, size_t err_cap);
    void*       reserved[4];
} psProcessorV1;

/* -- Analyzer V2: adds curve output and a self-declared description. -- */
typedef struct psAnalyzeSink2 {
    void* ctx;
    void (*emit_number)(void* ctx, const char* key, double value);
    void (*emit_text)  (void* ctx, const char* key, const char* value);
    /* Named curve (SFR, OECF, ...). x may be NULL (host uses 0..n-1).
     * All data is copied by the host during the call. */
    void (*emit_series)(void* ctx, const char* name, const char* x_label,
                        const char* y_label, const float* x, const float* y,
                        uint32_t n);
    void* reserved[4];
} psAnalyzeSink2;

typedef struct psAnalyzerV2 {
    uint32_t    abi_version;    /* = 2 (version of THIS struct)                  */
    uint32_t    caps;           /* PS_CAP_CPU mandatory                          */
    const char* name;           /* "category/name", static lifetime              */
    const char* description;    /* one-line precondition hint; may be NULL       */
    const char* params_schema;  /* reserved: pass NULL (future: JSON Schema UI)  */
    int32_t (*analyze)(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink2* sink, char* err, size_t err_cap);
    void*       reserved[4];
} psAnalyzerV2;

struct psHostApi {
    uint32_t abi_version;       /* host ABI = PS_ABI_VERSION                     */
    uint32_t struct_size;       /* sizeof(psHostApi); forward-compat probe       */
    void*    ctx;               /* opaque; pass back to every host function      */
    void  (*log)(void* ctx, int32_t level /*0=info 1=warn 2=error*/, const char* msg);
    /* THE ONLY WAY to allocate/free pixel memory that crosses the ABI: */
    void* (*frame_alloc)(void* ctx, size_t bytes);
    void  (*frame_free) (void* ctx, void* ptr);
    /* Registration. Return 0 = accepted. Descriptors are copied by the host,
     * but const char* fields inside must have process-lifetime storage. */
    int32_t (*register_display)  (void* ctx, const psDisplayV1* d);
    int32_t (*register_analyzer) (void* ctx, const psAnalyzerV1* a);
    int32_t (*register_processor)(void* ctx, const psProcessorV1* p);
    /* since host ABI 2 (was reserved[0]; NULL on ABI-1 hosts): */
    int32_t (*register_analyzer2)(void* ctx, const psAnalyzerV2* a);
    void*    reserved[7];
};

/* Every plugin exports exactly one symbol:
 *   PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host);
 * Must return 0 on success. MUST check host->abi_version first and return
 * nonzero WITHOUT registering anything if host->abi_version < PS_ABI_VERSION. */
typedef int32_t (*psRegisterPluginsFn)(const psHostApi* host);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PS_PLUGIN_H */
