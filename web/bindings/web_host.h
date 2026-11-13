/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * web/bindings/web_host.h - Clean-Room Pure C11 Web IDL Bindings & Browser Host State
 *
 * Strictly separates the Pure C11 ECMAScript 2026 VM (`js/vm/ajs_vm.h`) from
 * the Browser Web Host (`ArcrJsHostCallbacks`, Live DOM, CSSOM, Canvas 2D,
 * Fetch/Streams, WebCrypto, Storage/IndexedDB, BroadcastChannel & Workers).
 *
 * Zero QuickJS dependency. Zero link dependency on `arcr_engine`.
 * Safe on 32-bit x86 SSE1 (Windows 2000 SP4) & 64-bit x86-64.
 */
#ifndef ARCR_WEB_HOST_H
#define ARCR_WEB_HOST_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../../js/arcr_js.h"
#include "../../js/vm/ajs_vm.h"
#include "../../base/arcr_base.h"
#include "../../base/arcr_codec.h"
#include "../../css/arcr_css.h"
#include "../../platform/arcr_platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

#define WEB_CLASS_DOM_NODE         0x4152444Du /* "ARDM" */
#define ARCR_JS_TURN_DEADLINE_USEC 750000ULL   /* 750 ms max synchronous JS turn */
#define ARCR_CANVAS_MAX_PTS        2048
#define ARCR_CANVAS_STACK_MAX      16
#define ARCR_GRAD_MAX_STOPS        16
#define ARCR_FRAG_TAG_ATOM_NAME    "#document-fragment"
#define ARCR_SHADOW_TAG_ATOM_NAME  "#shadow-root"

/* -------------------------------------------------------------------------
 * 1. Cross-Platform Thread & Mutex Primitives (Win2000 SP4 + POSIX)
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
typedef CRITICAL_SECTION WebMutex;
typedef HANDLE           WebThread;

static inline void web_mutex_init(WebMutex *m)    { InitializeCriticalSection(m); }
static inline void web_mutex_destroy(WebMutex *m) { DeleteCriticalSection(m); }
static inline void web_mutex_lock(WebMutex *m)    { EnterCriticalSection(m); }
static inline void web_mutex_unlock(WebMutex *m)  { LeaveCriticalSection(m); }
static inline void web_sleep_ms(uint32_t ms)      { Sleep((DWORD)ms); }

typedef struct WebWinThreadThunk {
    void (*fn)(void *arg);
    void  *arg;
} WebWinThreadThunk;

static inline unsigned __stdcall web_win_thread_entry(void *raw) {
    WebWinThreadThunk thunk = *(WebWinThreadThunk *)raw;
    free(raw);
    thunk.fn(thunk.arg);
    return 0u;
}

static inline bool web_thread_spawn(WebThread *out_th, void (*fn)(void *), void *arg) {
    WebWinThreadThunk *thunk = (WebWinThreadThunk *)malloc(sizeof(WebWinThreadThunk));
    if (!thunk) return false;
    thunk->fn  = fn;
    thunk->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0u, web_win_thread_entry, thunk, 0u, NULL);
    if (h == 0u) {
        free(thunk);
        return false;
    }
    *out_th = (HANDLE)h;
    return true;
}

static inline void web_thread_join(WebThread th) {
    if (th) {
        (void)WaitForSingleObject(th, INFINITE);
        (void)CloseHandle(th);
    }
}
#else
typedef pthread_mutex_t WebMutex;
typedef pthread_t       WebThread;

static inline void web_mutex_init(WebMutex *m)    { (void)pthread_mutex_init(m, NULL); }
static inline void web_mutex_destroy(WebMutex *m) { (void)pthread_mutex_destroy(m); }
static inline void web_mutex_lock(WebMutex *m)    { (void)pthread_mutex_lock(m); }
static inline void web_mutex_unlock(WebMutex *m)  { (void)pthread_mutex_unlock(m); }
static inline void web_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
}

typedef struct WebPosixThreadThunk {
    void (*fn)(void *arg);
    void  *arg;
} WebPosixThreadThunk;

static inline void *web_posix_thread_entry(void *raw) {
    WebPosixThreadThunk thunk = *(WebPosixThreadThunk *)raw;
    free(raw);
    thunk.fn(thunk.arg);
    return NULL;
}

static inline bool web_thread_spawn(WebThread *out_th, void (*fn)(void *), void *arg) {
    WebPosixThreadThunk *thunk = (WebPosixThreadThunk *)malloc(sizeof(WebPosixThreadThunk));
    if (!thunk) return false;
    thunk->fn  = fn;
    thunk->arg = arg;
    if (pthread_create(out_th, NULL, web_posix_thread_entry, thunk) != 0) {
        free(thunk);
        return false;
    }
    return true;
}

static inline void web_thread_join(WebThread th) {
    (void)pthread_join(th, NULL);
}
#endif

/* -------------------------------------------------------------------------
 * 2. Canvas 2D State & Surface Tracking
 * ------------------------------------------------------------------------- */
typedef struct CanvasTransform {
    double   a, b, c, d, e, f;
    uint32_t fill_argb;
    uint32_t stroke_argb;
    double   line_width;
    double   global_alpha;
    int32_t  font_px;
    bool     font_bold;
    bool     font_mono;
    uint8_t  text_align;    /* 0=start/left, 1=center, 2=end/right */
    uint8_t  text_baseline; /* 0=alphabetic, 1=top, 2=middle, 3=bottom */
} CanvasTransform;

typedef struct CanvasGradStop {
    double   offset;
    uint32_t argb;
} CanvasGradStop;

typedef struct CanvasGrad {
    bool           active;
    bool           is_radial;
    double         x0, y0, r0, x1, y1, r1;
    CanvasGradStop stops[ARCR_GRAD_MAX_STOPS];
    int32_t        stop_count;
} CanvasGrad;

typedef struct CanvasPt {
    float x;
    float y;
    bool  move_to;
} CanvasPt;

typedef struct Canvas2DCtx {
    uint32_t            node_uid;
    bool                is_offscreen;
    ArcrDomSurface      off_surf;
    CanvasTransform     cur;
    CanvasTransform     stack[ARCR_CANVAS_STACK_MAX];
    int32_t             stack_top;
    int32_t             fill_grad_id;   /* -1 if solid color */
    int32_t             stroke_grad_id; /* -1 if solid color */
    CanvasPt            pts[ARCR_CANVAS_MAX_PTS];
    int32_t             pt_count;
    float               sub_start_x;
    float               sub_start_y;
    struct Canvas2DCtx *next;
} Canvas2DCtx;

typedef struct DetachedSurfTrack {
    ArcrDomSurface           *surf;
    struct DetachedSurfTrack *next;
} DetachedSurfTrack;

/* -------------------------------------------------------------------------
 * 3. Shadow DOM, Import Maps, Module Cache, Async Fetch & Workers
 * ------------------------------------------------------------------------- */
typedef struct WebShadowLink {
    uint32_t shadow_uid;
    uint32_t host_uid;
    bool     is_open;
} WebShadowLink;

typedef struct WebImportMapEntry {
    char *key;
    char *target_url; /* NULL when explicitly mapped to invalid address */
} WebImportMapEntry;

typedef struct WebImportScope {
    char              *scope_prefix;
    WebImportMapEntry *entries;
    size_t             count;
    size_t             cap;
} WebImportScope;

typedef struct WebImportMap {
    WebImportMapEntry *imports;
    size_t             import_count;
    size_t             import_cap;
    WebImportScope    *scopes;
    size_t             scope_count;
    size_t             scope_cap;
    bool               locked; /* Per HTML spec: locked once any module load begins */
} WebImportMap;

typedef struct WebModuleCacheEntry {
    char            *requested_url;
    char            *final_url;
    AjsModuleRecord *mod;
    uint32_t         ns_pin;
    bool             linked;
    bool             evaluated;
    bool             eval_ok;
    uint32_t         eval_promise_pin;
    bool             has_err;
    AjsErrorInfo     err_info;
} WebModuleCacheEntry;

typedef struct WebAsyncFetchJob {
    uint32_t                 job_id;
    volatile int             cancel_flag;
    volatile int             finished;
    WebThread                thread;
    bool                     thread_started;
    ArcrJsHostCallbacks      host_snap;

    char                    *url;
    char                    *method;
    char                    *req_headers_raw;
    uint8_t                 *req_body;
    size_t                   req_body_len;
    bool                     include_credentials;

    bool                     net_ok;
    int                      status;
    char                     final_url[2048];
    char                     content_type[256];
    char                    *resp_headers_raw;
    size_t                   resp_headers_len;
    uint8_t                 *resp_bytes;
    size_t                   resp_len;
    char                     blocked_reason[128];

    struct WebAsyncFetchJob *next;
} WebAsyncFetchJob;

typedef struct WebWorkerMsg {
    bool                 is_error;
    char                *payload_json;
    size_t               payload_len;
    uint8_t            **buffers;
    size_t              *buffer_lens;
    size_t               buffer_count;
    struct WebWorkerMsg *next;
} WebWorkerMsg;

typedef struct WebWorkerInstance {
    uint32_t                  worker_id;
    volatile int              closing;
    volatile int              finished;
    volatile int              busy;
    WebMutex                  mtx;
    WebThread                 thread;
    bool                      thread_started;
    ArcrJsHostCallbacks       host_snap;
    char                      origin[512];
    char                      script_url[2048];
    char                     *inline_code;
    size_t                    inline_code_len;
    bool                      is_module;
    size_t                    heap_limit;
    WebWorkerMsg             *to_worker_head;
    WebWorkerMsg             *to_worker_tail;
    WebWorkerMsg             *to_main_head;
    WebWorkerMsg             *to_main_tail;
    struct WebWorkerInstance *next;
} WebWorkerInstance;

typedef struct WebBlobUrlEntry {
    char                     url[256];
    char                     mime[128];
    uint8_t                 *bytes;
    size_t                   len;
    struct WebBlobUrlEntry  *next;
} WebBlobUrlEntry;

/* -------------------------------------------------------------------------
 * 4. Per-Realm Browser Host VM (`struct ArcrJsVm`)
 * ------------------------------------------------------------------------- */
struct ArcrJsVm {
    AjsVm               *avm;
    ArcrJsHostCallbacks  host;
    size_t               heap_limit;
    bool                 is_worker_vm;
    char                 worker_script_url[2048];
    uint64_t             vm_instance_id;
    bool                 counted_private_vm;

    uint64_t             boot_usec;
    uint64_t             deadline_usec;
    bool                 interrupt_armed;
    int                  host_call_depth;

    /* O(1) UID -> ArcrDomNode* lookup table (covers light DOM, <template>, ShadowRoot & detached) */
    ArcrDomNode        **uid_table;
    uint32_t             uid_cap;
    ArcrAtom             frag_atom;
    ArcrAtom             shadow_atom;

    /* ShadowRoot UID <-> Host Element UID table */
    WebShadowLink       *shadow_links;
    size_t               shadow_link_count;
    size_t               shadow_link_cap;

    /* Persistent GC root pins into AjsVm */
    uint32_t             pin_bridge;
    uint32_t             pin_dispatch_event;
    uint32_t             pin_tick_timers;
    uint32_t             pin_has_timers;
    uint32_t             pin_wrap_uid;
    uint32_t             pin_set_ready_state;
    uint32_t             pin_notify_popstate;
    uint32_t             pin_unhandled_rejection;

    /* HTML Import Maps & ES Module Map Cache */
    WebImportMap         import_map;
    WebModuleCacheEntry *mod_cache;
    size_t               mod_cache_count;
    size_t               mod_cache_cap;

    /* Canvas 2D contexts, gradients, font system & tracked surfaces */
    Canvas2DCtx         *canvas_head;
    uint32_t             next_offscreen_uid;
    CanvasGrad          *grads;
    int32_t              grad_count;
    int32_t              grad_cap;
    ArcrFontSystem      *canvas_fonts;
    DetachedSurfTrack   *surf_track_head;

    /* Non-blocking Async Fetch jobs, Dedicated Workers & Blob URLs */
    uint32_t             next_fetch_id;
    WebAsyncFetchJob    *fetch_jobs_head;
    uint32_t             next_worker_id;
    WebWorkerInstance   *workers_head;
    WebBlobUrlEntry     *blob_urls_head;
};

/* -------------------------------------------------------------------------
 * 5. Cross-File Internal Bindings API
 * ------------------------------------------------------------------------- */

/* URL, Origin & Worker VM helpers (web_js_module.c) */
void      web_effective_base_url(const ArcrJsVm *vm, char *out_url, size_t cap);
void      web_compute_origin(const char *url, char *out_origin, size_t cap);
bool      web_resolve_url(const char *base_url, const char *rel_url, char *out_url, size_t out_cap);
void      web_console_log_line(ArcrJsVm *vm, const char *level_prefix, const char *msg);
void      web_log_ajs_error(ArcrJsVm *vm, const char *tag_override, const AjsErrorInfo *err);
bool      web_drain_microtasks_guarded(ArcrJsVm *vm);
ArcrJsVm *web_create_worker_vm(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb,
                               const char *worker_script_url);
void      web_destroy_vm_internal(ArcrJsVm *vm);
bool      web_eval_worker_entry(ArcrJsVm *vm, const char *utf8_code, size_t code_len,
                                const char *script_url, bool is_module);
bool      web_tick_worker_vm(ArcrJsVm *vm, uint64_t now_ms);
bool      web_worker_vm_has_work(const ArcrJsVm *vm);

/* Pause interrupt deadline while waiting inside legitimate host callbacks */
static inline void web_host_call_enter(ArcrJsVm *vm) {
    if (vm) vm->host_call_depth++;
}
static inline void web_host_call_leave(ArcrJsVm *vm, uint64_t paused_start_usec) {
    if (!vm) return;
    if (vm->host_call_depth > 0) vm->host_call_depth--;
    uint64_t now_us = arcr_platform_monotonic_usec();
    if (now_us > paused_start_usec && vm->deadline_usec > 0u) {
        vm->deadline_usec += (now_us - paused_start_usec);
    }
}

/* DOM, CSSOM, Layout, Shadow DOM, Template, Storage & History bridge (web_dom_bridge.c) */
void         web_uid_register(ArcrJsVm *vm, ArcrDomNode *n);
void         web_uid_sync_document(ArcrJsVm *vm);
ArcrDomNode *web_dom_by_uid(ArcrJsVm *vm, uint32_t uid);
ArcrDomSurface *web_ensure_node_surface(ArcrJsVm *vm, ArcrDomNode *el, int32_t w, int32_t h);
bool         web_parse_css_color(const char *str, size_t len, uint32_t *out_argb);
void         web_dom_register_bridge(ArcrJsVm *vm, AjsValue bridge_obj);
void         web_dom_cleanup(ArcrJsVm *vm);
void         web_private_session_retain(ArcrJsVm *vm);
void         web_private_session_release(ArcrJsVm *vm);

/* Pure C11 2D <canvas> & OffscreenCanvas rasterizer (web_canvas2d.c) */
void web_canvas_register_bridge(ArcrJsVm *vm, AjsValue bridge_obj);
void web_canvas_cleanup(ArcrJsVm *vm);

/* WebCrypto, Async Fetch, BroadcastChannel, Blob URLs & Multi-Threaded Workers (web_crypto_streams.c) */
bool web_lookup_blob_url(ArcrJsVm *vm, const char *url,
                         const uint8_t **out_bytes, size_t *out_len,
                         char *out_mime, size_t mime_cap);
bool web_has_pending_async_work(const ArcrJsVm *vm);
void web_poll_async_work(ArcrJsVm *vm, bool *io_mutated);
void web_crypto_streams_register_bridge(ArcrJsVm *vm, AjsValue bridge_obj);
void web_crypto_streams_cleanup(ArcrJsVm *vm);

/* Pure ES2026 Web IDL Bootstrap (web_bootstrap.c) */
bool web_bootstrap_install(ArcrJsVm *vm, AjsValue bridge_obj);

/* Native registration helper */
static inline void web_bind_fn(AjsVm *avm, AjsValue obj, const char *name,
                               AjsNativeFn fn, int length, void *userdata) {
    AjsValue fn_val = ajs_new_function(avm, fn, name, length, userdata);
    (void)ajs_set_prop_str(avm, obj, name, fn_val);
}

#endif /* ARCR_WEB_HOST_H */
