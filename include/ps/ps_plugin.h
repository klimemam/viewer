/* ps_plugin.h — viewer plugin ABI v3 (PS_ABI_VERSION below is the one truth).
 * Pure C header. This file IS the contract; never break it, only extend
 * via new V2/V3 structs and the reserved fields. */
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

#define PS_ABI_VERSION 3u
/* Host API version history:
 *   1 - display / analyzer / processor V1 structs
 *   2 - adds psAnalyzerV2 (emit_series curves + description) via
 *       psHostApi::register_analyzer2. V1 structs keep abi_version = 1
 *       and remain loadable forever.
 *   3 - layer-typed analyzers (docs/abi-v3.md): psAnalyzerV3 - the frame
 *       descriptor, +version/+headline - over psAnalyzeSink3, registered
 *       through psHostApi::register_analyzer3; and psStack + psStackAnalyzerV3
 *       (§5: frames PULLED one at a time, +min_frames) through
 *       psHostApi::register_stack_analyzer3. V1/V2 structs and their two
 *       register slots stay loadable forever; psHostApi does not change size.
 *       The rest of v3 (psSeriesAnalyzerV3 §7, emit_number_u / emit_map §8)
 *       arrives in the reserved seats below and needs NO further version
 *       bump - see the probe rule. */

/* THE PROBE RULE (docs/abi-v3.md §2.2) - why v3 is meant to be the last bump.
 *
 *   - abi_version >= 3 guarantees the v3 CORE and nothing else: the host has
 *     register_analyzer3 and register_stack_analyzer3. Everything added
 *     afterwards arrives in a RESERVED SEAT: a slot whose name and contract
 *     are written here while the host may still leave it NULL.
 *   - Reserved fields have been zero-filled by whoever creates the struct
 *     since v1, so "is that seat taken?" is answerable by a plain NULL test -
 *     safe even against an ABI-1 host, which zero-filled the same bytes.
 *     A plugin MUST NULL-check every seat documented as possibly-NULL before
 *     calling it, and must degrade rather than fail when one is empty.
 *   - Taking a seat is NOT a version bump. The number changes only when the
 *     MEANING of something that already exists changes.
 *
 * So: ask abi_version about the core, and NULL-probe everything else. */

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

/* -- Analyzer V3 (docs/abi-v3.md §3, §4): same one-frame signature as V2 -
 * what is new is the descriptor's own fields and the sink type. -- */

/* Sink for v3 analyze functions. The three emits below are byte-for-byte the
 * sink2 ones, so a V2 analyzer's BODY ports over untouched; everything is
 * still copied by the host during the call. V1/V2 sinks are frozen forever -
 * this type is only ever handed to a v3 analyze(). */
typedef struct psAnalyzeSink3 {
    void* ctx;
    void (*emit_number)(void* ctx, const char* key, double value);
    void (*emit_text)  (void* ctx, const char* key, const char* value);
    /* Named curve (SFR, OECF, ...). x may be NULL (host uses 0..n-1). */
    void (*emit_series)(void* ctx, const char* name, const char* x_label,
                        const char* y_label, const float* x, const float* y,
                        uint32_t n);
    /* Reserved seats (probe rule above), zero-filled. docs/abi-v3.md §8 spends
     * the first two on the declared-unit scalar and the pixel-shaped result;
     * until a host implements them they read NULL and emit_number carries the
     * unit in the key name, which §8.2 keeps as the documented fallback. */
    void* reserved[8];
} psAnalyzeSink3;

typedef struct psAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 (version of THIS struct)                  */
    uint32_t    caps;           /* PS_CAP_CPU mandatory                          */
    const char* name;           /* "category/name", static lifetime              */
    const char* version;        /* REQUIRED, non-empty, static, UTF-8 free-form.
                                   Carried VERBATIM: the host never parses,
                                   orders or normalizes it, and compares it for
                                   equality only. Declaring v3 IS declaring a
                                   version - NULL or "" is refused at
                                   registration (docs/abi-v3.md §3.1)            */
    const char* description;    /* one-line precondition hint; may be NULL       */
    const char* params_schema;  /* reserved: pass NULL (future: JSON Schema UI)  */
    const char* headline;       /* the one key this analyzer exists to produce,
                                   channel prefix ("ch0." / "R." ...) stripped;
                                   the host accents that row. NULL = no headline
                                   (docs/abi-v3.md §3.2)                         */
    int32_t (*analyze)(const psFrame* in, const psRect* roi,
                       const psAnalyzeSink3* sink, char* err, size_t err_cap);
    void*       reserved[4];
} psAnalyzerV3;

/* -- Stack (docs/abi-v3.md §5): the whole time axis of one stack, PULLED.
 *
 * The host does not hand over an array of frames, and this is the load-bearing
 * decision of §5 rather than a convenience: an array would make "every frame is
 * in memory at once" a promise of the ABI, which forecloses the two transports
 * §9 keeps open (a bounded shared-memory window for the resident Python worker,
 * and running peer-side instead of shipping the pixels). Pull costs one
 * indirection and buys both. -- */

/* A stack as shown to a plugin. The host serves frames on demand; what lives
 * behind get_frame - resident memory, a shared-memory segment, a file - is host
 * business and invisible here (docs/abi-v3.md §9). */
typedef struct psStack {
    uint32_t    frames;         /* n: frames the host will serve, 0..frames-1   */
    uint32_t    expected;       /* N: declared stack size. frames < expected
                                   = partial load (§6); never the reverse       */
    uint32_t    w, h, ch;       /* every served frame has this geometry         */
    psDtype     dtype;          /* v3 host: always PS_DTYPE_F32                 */
    const char* name;           /* stack name, UTF-8, valid for the call only   */
    const char* meta_json;      /* reserved; may be NULL                        */
    void*       ctx;            /* opaque; pass to get_frame / release_frame    */
    /* Returned pointer is valid until release_frame(ctx, index) or until
     * analyze_stack returns, whichever comes first. May return NULL (frame
     * lost, transport failure, pin budget exceeded): then write err and return
     * nonzero - never fabricate a frame, and never report a partial answer with
     * a success return.                                                        */
    const psFrame* (*get_frame)(void* ctx, uint32_t index);
    /* Optional to CALL, never NULL to read: both mouths are filled by any host
     * that hands you a psStack. Releasing lets the host recycle the slot (the
     * bounded shared-memory window of §9.2). Frames that are never released
     * stay valid until analyze_stack returns - a naive sequential plugin is
     * correct, just greedy.                                                    */
    void (*release_frame)(void* ctx, uint32_t index);
    uint64_t    reserved[4];
} psStack;

/* What the HOST guarantees, and what the PLUGIN then owes (docs/abi-v3.md §5.1):
 *
 *   HOST: every served frame has the w/h/ch/dtype above - frames of another
 *         shape, and decimated previews standing in for pixels not yet here,
 *         are excluded BEFORE `frames` is counted. index is stack order.
 *         Which frame you are holding is carried by psFrame::name / pts_us.
 *   HOST: analyze_stack is not called until the load has settled, so `frames`
 *         is the count of a load that really did stop there - not a race.
 *   HOST: one descriptor's calls are serialized; write no re-entrancy.
 *   PLUGIN: aggregate over time with NaN EXCLUDED PER PIXEL and COUNTED, and
 *         carry the exclusion count out in the result. Never fold a non-finite
 *         sample into a divisor, and never exclude silently - the host's own
 *         stack statistics have declared their exclusions since they existed.
 *   PLUGIN: a NULL from get_frame is reported in err with a nonzero return.
 *   PLUGIN: do not refuse for `frames < expected`. Measuring a partial load is
 *         allowed; the one veto you get is min_frames, declared once at
 *         registration (§6).
 *
 * roi means what it means for a frame (NULL = whole). get_frame always shows a
 * whole frame; transporting only the ROI's rows is a host optimization and is
 * invisible as long as the coordinate system does not appear to move. */
typedef struct psStackAnalyzerV3 {
    uint32_t    abi_version;    /* = 3 (version of THIS struct)                 */
    uint32_t    caps;           /* PS_CAP_CPU mandatory                         */
    uint32_t    min_frames;     /* >= 1. The host refuses BEFORE calling when
                                   frames < min_frames, with a one-line reason.
                                   0 is rejected at REGISTRATION: declare, do
                                   not default (docs/abi-v3.md §6). Write 1 if
                                   one frame will do - being asked once why this
                                   is a stack analyzer at all is the point       */
    uint32_t    _pad;           /* keep 8-byte alignment of the pointers below  */
    const char* name;           /* "category/name", static lifetime             */
    const char* version;        /* REQUIRED non-empty, static, carried verbatim
                                   (#46 stage 2, docs/abi-v3.md §3.1)           */
    const char* description;    /* one-line precondition hint; may be NULL      */
    const char* params_schema;  /* reserved: pass NULL (future: JSON Schema UI) */
    const char* headline;       /* accent key, channel-stripped; may be NULL    */
    int32_t (*analyze_stack)(const psStack* in, const psRect* roi,
                             const psAnalyzeSink3* sink,
                             char* err, size_t err_cap);
    void*       reserved[4];
} psStackAnalyzerV3;

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
    /* since host ABI 3 (was reserved[0]; NULL on ABI-1/2 hosts - probe it).
     * Renaming a reserved slot is how v2 added register_analyzer2 and it is
     * why sizeof(psHostApi) and every v1/v2 offset are unchanged: a plugin
     * built against ANY older header still finds its fields where they were. */
    int32_t (*register_analyzer3)(void* ctx, const psAnalyzerV3* a);
    /* ...and the second half of the v3 core, in the seat straight after it
     * (docs/abi-v3.md §5). Non-NULL on any host that says abi_version 3 -
     * but still worth a NULL test, which costs nothing and is the one habit
     * that keeps the seats after it usable. */
    int32_t (*register_stack_analyzer3)(void* ctx, const psStackAnalyzerV3* a);
    /* Reserved seats, zero-filled. docs/abi-v3.md §12 spends them on the series
     * mouth (§7.2 - SHAPE frozen in the doc, shipped NULL on purpose until the
     * built-in Series analysis has validated it) and, the last two deliberately
     * unnamed, the kind 3/4 set mouths - a named seat is a promise about a
     * shape, so the name is written the stage the implementation lands. Each
     * gets its name here as it is implemented; a name appearing is not a
     * version bump. */
    void*    reserved[5];
};

/* Every plugin exports exactly one symbol:
 *   PS_PLUGIN_EXPORT int32_t psRegisterPlugins(const psHostApi* host);
 * Must return 0 on success. MUST check host->abi_version first and return
 * nonzero WITHOUT registering anything if host->abi_version < PS_ABI_VERSION.
 * That check is about the CORE the version number promises; the seats it does
 * not promise are asked for by NULL test (probe rule above), and a plugin that
 * wants to run on older hosts as well simply builds against the older header -
 * those hosts keep loading it, forever. */
typedef int32_t (*psRegisterPluginsFn)(const psHostApi* host);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PS_PLUGIN_H */
