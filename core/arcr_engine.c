/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * core/arcr_engine.c - Public C11 Embedding API, Async Multi-Core Loader,
 *                      Media/Animation Pipeline & Tier-4 Dynamic Module Registry
 *
 * Pure C11 & Windows 2000 SP4 compatible:
 *   - Zero VLAs, msvcrt-safe printf specifiers, strict ISO C dlsym memcpy casts.
 *   - Strict On-Demand Lazy Loading for arcr_net, arcr_js_portable, arcr_media, arcr_devtools.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../include/arcradient_embed.h"
#include "../include/waterscape_modules.h"
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../base/arcr_threadpool.h"
#include "../core/arcr_dom.h"
#include "../css/arcr_css.h"
#include "../js/arcr_js.h"
#include "../layout/arcr_layout.h"
#include "../media/arcr_media.h"
#include "../net/arcr_net.h"
#include "../paint/arcr_paint.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ARCR_STORAGE_SLOTS     64u
#define ARCR_MAX_FOCUS_NODES   256u
#define ARCR_MAX_JS_NAV_DEPTH  4u
#define ARCR_URL_BUF_CAP       2048u
#define ARCR_SUBRES_BATCH_MAX  24u
#define ARCR_MAX_VIEW_VIDEOS   8u

/* -------------------------------------------------------------------------
 * 1. Small Cross-Platform Mutex & Detached Worker Helpers (Win2000 + POSIX)
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
typedef CRITICAL_SECTION EngineMutex;
static void eng_mutex_init(EngineMutex *m)    { InitializeCriticalSection(m); }
static void eng_mutex_destroy(EngineMutex *m) { DeleteCriticalSection(m); }
static void eng_mutex_lock(EngineMutex *m)    { EnterCriticalSection(m); }
static void eng_mutex_unlock(EngineMutex *m)  { LeaveCriticalSection(m); }
static void eng_sleep_1ms(void)               { Sleep(1); }
#else
typedef pthread_mutex_t EngineMutex;
static void eng_mutex_init(EngineMutex *m)    { (void)pthread_mutex_init(m, NULL); }
static void eng_mutex_destroy(EngineMutex *m) { (void)pthread_mutex_destroy(m); }
static void eng_mutex_lock(EngineMutex *m)    { (void)pthread_mutex_lock(m); }
static void eng_mutex_unlock(EngineMutex *m)  { (void)pthread_mutex_unlock(m); }
static void eng_sleep_1ms(void) {
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 1000000L;
    (void)nanosleep(&ts, NULL);
}
#endif

/* -------------------------------------------------------------------------
 * 2. Internal Growable UTF-8 String Buffer
 * ------------------------------------------------------------------------- */
typedef struct EngineStrBuf {
    char  *data;
    size_t len;
    size_t cap;
} EngineStrBuf;

static bool strbuf_reserve(EngineStrBuf *sb, size_t needed) {
    if (!sb) return false;
    if (needed <= sb->cap) return true;
    size_t ncap = (sb->cap == 0u) ? 256u : sb->cap;
    while (ncap < needed) {
        ncap *= 2u;
    }
    char *grown = (char *)realloc(sb->data, ncap);
    if (!grown) return false;
    if (sb->cap == 0u) {
        grown[0] = '\0';
    }
    sb->data = grown;
    sb->cap  = ncap;
    return true;
}

static bool strbuf_append(EngineStrBuf *sb, const char *s, size_t n) {
    if (!sb) return false;
    if (!s || n == 0u) {
        if (!sb->data && !strbuf_reserve(sb, 1u)) return false;
        return true;
    }
    if (!strbuf_reserve(sb, sb->len + n + 1u)) return false;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

static bool strbuf_append_cstr(EngineStrBuf *sb, const char *s) {
    if (!s) return true;
    return strbuf_append(sb, s, strlen(s));
}

static bool strbuf_append_char(EngineStrBuf *sb, char c) {
    return strbuf_append(sb, &c, 1u);
}

static void strbuf_clear(EngineStrBuf *sb) {
    if (!sb) return;
    sb->len = 0u;
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static void strbuf_free(EngineStrBuf *sb) {
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0u;
    sb->cap  = 0u;
}

/* -------------------------------------------------------------------------
 * 3. Engine, Tier-4 Module Slot, Async Nav Job & WebView State Structures
 * ------------------------------------------------------------------------- */
typedef struct ModuleSlot {
    const char      *stem;
    const char      *role_summary;
    ArcrDynLib      *lib;
    const void      *vtable;
    bool             load_attempted;
    ArcrModuleStatus last_status;
} ModuleSlot;

typedef struct StorageSlot {
    bool used;
    char key[128];
    char val[512];
} StorageSlot;

typedef struct ViewVideoSlot {
    uint32_t         node_uid;
    ArcrMediaPlayer *mp;
    int32_t          w;
    int32_t          h;
} ViewVideoSlot;

typedef struct AsyncNavJob {
    EngineMutex          mtx;
    int                  refs;
    bool                 canceled;
    bool                 finished;
    bool                 net_ok;
    int32_t              progress;
    ArcrEngine          *eng;
    const ArcrNetVTable *net_vt;
    bool                 private_browsing;
    char                 req_url[ARCR_URL_BUF_CAP];
    char                 referer[ARCR_URL_BUF_CAP];
    char                 method[16];
    char                *post_body;
    size_t               post_len;
    ArcrHttpResponse     resp;
} AsyncNavJob;

struct ArcrEngine {
    ArcrEngineConfig  cfg;
    char              module_dir[512];
    ArcrAtomTable     atoms;
    ArcrAccelContext *accel;
    ArcrFontSystem   *fonts;
    ArcrThreadPool   *pool;
    ArcrWebView      *active_views_head;

    /* Systematized Tier-4 On-Demand Dynamic Module Registry */
    ModuleSlot        slots[WS_DYN_SLOT_COUNT];

    /* Async background worker tracking so arcr_engine_destroy waits before dlclose */
    EngineMutex       async_mtx;
    uint32_t          active_async_workers;

    /* URL Shield / Ad & Tracker Filter Hook */
    ArcrUrlFilterFn   url_filter;
    void             *url_filter_ud;
    uint32_t          blocked_requests_count;

    /* Frame & Layout Telemetry */
    uint32_t          tiles_rendered_last_frame;
    uint64_t          last_layout_usec;
    uint64_t          last_paint_usec;
    char              gpu_summary[256];

    /* Sandbox Key-Value Store for JS storage_get / storage_set */
    StorageSlot       storage[ARCR_STORAGE_SLOTS];
    size_t            storage_cursor;
};

struct ArcrWebView {
    ArcrEngine          *engine;
    ArcrWebViewCallbacks cb;
    int32_t              width;
    int32_t              height;
    int32_t              scroll_y;
    int32_t              zoom_percent;
    bool                 prefers_dark;

    char                 url[ARCR_URL_BUF_CAP];
    char                 title[256];
    char                 ime_comp[128];
    char                 last_hover_href[ARCR_URL_BUF_CAP];
    ArcrSecurityState    security;

    EngineStrBuf         user_css;
    EngineStrBuf         console_log;
    EngineStrBuf         raw_html;

    char                 find_query[128];
    uint32_t             find_active_idx;
    uint32_t             find_total_matches;

    char                 pending_js_nav_url[ARCR_URL_BUF_CAP];
    uint32_t             js_nav_depth;

    /* Async Non-Blocking Navigation State */
    AsyncNavJob         *async_job;
    int32_t              load_progress;

    /* CSS @keyframes, Animated GIF & <video> State */
    uint64_t             anim_start_usec;
    uint64_t             last_css_anim_usec;
    bool                 has_css_animations;
    bool                 has_anim_images;
    ViewVideoSlot        videos[ARCR_MAX_VIEW_VIDEOS];
    size_t               video_count;

    ArcrDocument        *doc;
    ArcrStylesheet      *sheet;
    ArcrLayoutTree      *layout;
    ArcrDisplayList     *display_list;
    ArcrJsVm            *js_vm;
    size_t               last_style_share_hits;

    struct ArcrWebView  *next;
};

/* -------------------------------------------------------------------------
 * 4. Tier-4 Dynamic Module Registry & Lazy Loaders
 * ------------------------------------------------------------------------- */
static void init_module_registry(ArcrEngine *eng) {
    eng->slots[WS_DYN_SLOT_NET].stem         = WS_MOD_STEM_NET;
    eng->slots[WS_DYN_SLOT_NET].role_summary = "HTTP/1.1 + HTTPS TLS 1.3, Brotli/Gzip, Cookie & Parallel Pool";

    eng->slots[WS_DYN_SLOT_JS].stem          = WS_MOD_STEM_JS;
    eng->slots[WS_DYN_SLOT_JS].role_summary  = "QuickJS-NG ES2026 Runtime & Live Browser Web API Bridge";

    eng->slots[WS_DYN_SLOT_MEDIA].stem         = WS_MOD_STEM_MEDIA;
    eng->slots[WS_DYN_SLOT_MEDIA].role_summary = "Hardware/FFmpeg <video>/<audio> Decoder & Cinema Policy";

    eng->slots[WS_DYN_SLOT_DEVTOOLS].stem         = WS_MOD_STEM_DEVTOOLS;
    eng->slots[WS_DYN_SLOT_DEVTOOLS].role_summary = "Diagnostics, Telemetry & Memory Profiler";
}

static const void *ensure_module_slot(ArcrEngine *eng, WaterscapeDynamicSlot slot_id) {
    if (!eng || (int)slot_id < 0 || slot_id >= WS_DYN_SLOT_COUNT) return NULL;
    ModuleSlot *slot = &eng->slots[slot_id];
    if (slot->vtable) return slot->vtable;

    slot->load_attempted = true;

    char path[512];
    arcr_platform_format_module_path(path, sizeof(path), eng->module_dir, slot->stem);
    ArcrDynLib *lib = arcr_platform_dlopen(path);
    if (!lib) {
        char exe_dir[512];
        if (arcr_platform_get_executable_dir(exe_dir, sizeof(exe_dir)) && exe_dir[0] != '\0') {
            arcr_platform_format_module_path(path, sizeof(path), exe_dir, slot->stem);
            lib = arcr_platform_dlopen(path);
        }
    }
    if (!lib) {
        slot->last_status = ARCR_MOD_ERR_NULL;
        return NULL;
    }

    void *sym = arcr_platform_dlsym(lib, WS_MOD_ENTRY_PRIMARY);
    if (!sym) {
        sym = arcr_platform_dlsym(lib, WS_MOD_ENTRY_ALIAS);
    }

    ArcrModuleQueryFn query_fn = NULL;
    memcpy(&query_fn, &sym, sizeof(query_fn));
    if (!query_fn) {
        slot->last_status = ARCR_MOD_ERR_NULL;
        arcr_platform_dlclose(lib);
        return NULL;
    }

    const ArcrModuleDescriptor *desc = query_fn();
    ArcrModuleStatus st = arcr_module_validate(
        desc,
        arcr_host_compile_arch(),
        arcr_platform_os_version(),
        arcr_detect_cpu_flags()
    );
    slot->last_status = st;
    if (st != ARCR_MOD_OK) {
        arcr_platform_dlclose(lib);
        return NULL;
    }

    slot->lib    = lib;
    slot->vtable = desc->vtable;
    return slot->vtable;
}

static inline const ArcrNetVTable *get_net_vt(const ArcrEngine *eng) {
    return eng ? (const ArcrNetVTable *)eng->slots[WS_DYN_SLOT_NET].vtable : NULL;
}

static inline const ArcrJsVTable *get_js_vt(const ArcrEngine *eng) {
    return eng ? (const ArcrJsVTable *)eng->slots[WS_DYN_SLOT_JS].vtable : NULL;
}

static inline const ArcrMediaVTable *get_media_vt(const ArcrEngine *eng) {
    return eng ? (const ArcrMediaVTable *)eng->slots[WS_DYN_SLOT_MEDIA].vtable : NULL;
}

static bool ensure_net_module(ArcrEngine *eng) {
    return ensure_module_slot(eng, WS_DYN_SLOT_NET) != NULL;
}

static bool ensure_js_module(ArcrEngine *eng) {
    return ensure_module_slot(eng, WS_DYN_SLOT_JS) != NULL;
}

static bool ensure_media_module(ArcrEngine *eng) {
    return ensure_module_slot(eng, WS_DYN_SLOT_MEDIA) != NULL;
}

size_t arcr_engine_query_modules(const ArcrEngine *engine,
                                 WaterscapeModuleStatusEntry *out_entries,
                                 size_t max_entries) {
    if (!engine) return 0u;
    if (!out_entries) return (size_t)WS_DYN_SLOT_COUNT;

    size_t count = ARCR_MIN(max_entries, (size_t)WS_DYN_SLOT_COUNT);
    for (size_t i = 0u; i < count; ++i) {
        const ModuleSlot *s = &engine->slots[i];
        out_entries[i].stem           = s->stem;
        out_entries[i].role_summary   = s->role_summary;
        out_entries[i].tier           = WS_TIER_4_ONDEMAND;
        out_entries[i].is_loaded      = (s->vtable != NULL);
        out_entries[i].load_attempted = s->load_attempted;
        out_entries[i].last_status    = s->last_status;
    }
    return count;
}

/* -------------------------------------------------------------------------
 * 5. Engine Lifecycle, Hardware Telemetry & Memory Pressure
 * ------------------------------------------------------------------------- */
void arcr_engine_default_config(ArcrEngineConfig *out_cfg, ArcrProfileMode profile) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->profile = profile;
    if (profile == ARCR_PROFILE_MINIMUM) {
        out_cfg->process_mode        = ARCR_PROC_LEGACY;
        out_cfg->force_cpu_rendering = true;
        out_cfg->enable_javascript   = true;
        out_cfg->worker_threads      = 1u;
        out_cfg->max_cache_bytes     = 4u * 1024u * 1024u;
    } else if (profile == ARCR_PROFILE_BALANCED) {
        out_cfg->process_mode        = ARCR_PROC_STANDARD;
        out_cfg->force_cpu_rendering = false;
        out_cfg->enable_javascript   = true;
        out_cfg->worker_threads      = 0u;
        out_cfg->max_cache_bytes     = 16u * 1024u * 1024u;
    } else {
        out_cfg->process_mode        = ARCR_PROC_ISOLATED;
        out_cfg->force_cpu_rendering = false;
        out_cfg->enable_javascript   = true;
        out_cfg->worker_threads      = 0u;
        out_cfg->max_cache_bytes     = 64u * 1024u * 1024u;
    }
    out_cfg->module_search_dir     = ".";
    out_cfg->default_cjk_font_path = NULL;
}

static void refresh_gpu_summary(ArcrEngine *eng) {
    if (!eng || !eng->accel) return;

    ArcrAccelBackendId cpu_id   = arcr_accel_active_cpu_backend(eng->accel);
    ArcrAccelBackendId gpu_id   = arcr_accel_active_gpu_backend(eng->accel);
    const char        *cpu_name = arcr_accel_backend_name(cpu_id);
    bool               gpu_ok   = arcr_accel_is_gpu_healthy(eng->accel);
    bool               fallback = arcr_accel_did_fallback_occur(eng->accel);

    ArcrGpuProbeResult probe;
    memset(&probe, 0, sizeof(probe));
    arcr_accel_get_gpu_probe(eng->accel, &probe);

    if (gpu_ok && gpu_id >= ARCR_BACKEND_GPU_DDRAW7) {
        const char *gpu_name = arcr_accel_backend_name(gpu_id);
        if (probe.adapter_description[0] != '\0') {
            snprintf(eng->gpu_summary, sizeof(eng->gpu_summary),
                     "CPU: %s | GPU: %s [%s] (Healthy)",
                     cpu_name ? cpu_name : "Scalar",
                     gpu_name ? gpu_name : "Hardware",
                     probe.adapter_description);
        } else {
            snprintf(eng->gpu_summary, sizeof(eng->gpu_summary),
                     "CPU: %s | GPU: %s (Healthy)",
                     cpu_name ? cpu_name : "Scalar",
                     gpu_name ? gpu_name : "Hardware");
        }
    } else if (fallback) {
        snprintf(eng->gpu_summary, sizeof(eng->gpu_summary),
                 "CPU: %s | GPU: Disabled (Seamless CPU Fallback)",
                 cpu_name ? cpu_name : "Scalar");
    } else {
        snprintf(eng->gpu_summary, sizeof(eng->gpu_summary),
                 "CPU: %s | GPU: None (CPU Tile Path)",
                 cpu_name ? cpu_name : "Scalar");
    }
}

ArcrEngine *arcr_engine_create(const ArcrEngineConfig *cfg) {
    ArcrEngine *eng = (ArcrEngine *)calloc(1u, sizeof(ArcrEngine));
    if (!eng) return NULL;

    eng_mutex_init(&eng->async_mtx);
    if (cfg) {
        eng->cfg = *cfg;
    } else {
        arcr_engine_default_config(&eng->cfg, ARCR_PROFILE_BALANCED);
    }
    arcr_str_copy(eng->module_dir, sizeof(eng->module_dir),
                  eng->cfg.module_search_dir ? eng->cfg.module_search_dir : ".");

    init_module_registry(eng);

    if (!arcr_atom_table_init(&eng->atoms, 512u)) {
        eng_mutex_destroy(&eng->async_mtx);
        free(eng);
        return NULL;
    }

    uint32_t allowed_simd = 0xFFFFFFFFu;
    if (eng->cfg.profile == ARCR_PROFILE_MINIMUM) {
        allowed_simd = ARCR_CPU_MMX | ARCR_CPU_SSE1;
    }
    eng->accel = arcr_accel_create(allowed_simd, eng->cfg.force_cpu_rendering,
                                   !eng->cfg.force_cpu_rendering);
    eng->fonts = arcr_font_system_create(eng->cfg.default_cjk_font_path);
    eng->pool  = arcr_threadpool_create(
        eng->cfg.profile == ARCR_PROFILE_MINIMUM ? 1u : eng->cfg.worker_threads);

    if (!eng->accel || !eng->fonts || !eng->pool) {
        arcr_engine_destroy(eng);
        return NULL;
    }

    refresh_gpu_summary(eng);
    return eng;
}

void arcr_engine_destroy(ArcrEngine *eng) {
    if (!eng) return;
    while (eng->active_views_head) {
        arcr_webview_destroy(eng->active_views_head);
    }

    /* Wait for any in-flight detached async navigation workers before dlclose */
    for (;;) {
        eng_mutex_lock(&eng->async_mtx);
        uint32_t active = eng->active_async_workers;
        eng_mutex_unlock(&eng->async_mtx);
        if (active == 0u) break;
        eng_sleep_1ms();
    }

    for (size_t i = 0u; i < (size_t)WS_DYN_SLOT_COUNT; ++i) {
        if (eng->slots[i].lib) {
            void *raw_shut = arcr_platform_dlsym(eng->slots[i].lib, "arcr_module_shutdown");
            if (raw_shut) {
                typedef void (*ArcrModuleShutdownFn)(void);
                ArcrModuleShutdownFn fn_shut = NULL;
                memcpy(&fn_shut, &raw_shut, sizeof(fn_shut));
                if (fn_shut) fn_shut();
            }
            arcr_platform_dlclose(eng->slots[i].lib);
            eng->slots[i].lib    = NULL;
            eng->slots[i].vtable = NULL;
        }
    }
    if (eng->pool)  arcr_threadpool_destroy(eng->pool);
    if (eng->fonts) arcr_font_system_destroy(eng->fonts);
    if (eng->accel) arcr_accel_destroy(eng->accel);
    arcr_atom_table_destroy(&eng->atoms);
    eng_mutex_destroy(&eng->async_mtx);
    free(eng);
}

void arcr_engine_set_url_filter(ArcrEngine *eng, ArcrUrlFilterFn fn, void *userdata) {
    if (!eng) return;
    eng->url_filter    = fn;
    eng->url_filter_ud = userdata;
}

const char *arcr_engine_gpu_backend_summary(const ArcrEngine *eng) {
    if (!eng) return "CPU: Unknown | GPU: None";
    refresh_gpu_summary((ArcrEngine *)eng);
    return eng->gpu_summary;
}

void arcr_engine_get_stats(const ArcrEngine *eng, ArcrEngineStats *out_stats) {
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!eng) return;

    out_stats->detected_cpu_flags        = arcr_detect_cpu_flags();
    out_stats->active_simd_level         = (uint32_t)arcr_accel_active_cpu_backend(eng->accel);
    out_stats->active_gpu_backend        = (uint32_t)arcr_accel_active_gpu_backend(eng->accel);
    out_stats->worker_thread_count       = arcr_threadpool_worker_count(eng->pool);
    out_stats->gpu_available             = arcr_accel_is_gpu_healthy(eng->accel);
    out_stats->gpu_fallback_triggered    = arcr_accel_did_fallback_occur(eng->accel);
    out_stats->net_module_loaded         = (eng->slots[WS_DYN_SLOT_NET].vtable != NULL);
    out_stats->js_module_loaded          = (eng->slots[WS_DYN_SLOT_JS].vtable != NULL);
    out_stats->media_module_loaded       = (eng->slots[WS_DYN_SLOT_MEDIA].vtable != NULL);
    out_stats->devtools_module_loaded    = (eng->slots[WS_DYN_SLOT_DEVTOOLS].vtable != NULL);
    out_stats->interned_atom_count       = eng->atoms.count;
    out_stats->arena_allocated_bytes     = eng->atoms.arena.total_allocated;
    out_stats->arena_used_bytes          = eng->atoms.arena.total_used;
    out_stats->tiles_rendered_last_frame = eng->tiles_rendered_last_frame;
    out_stats->last_layout_usec          = eng->last_layout_usec;
    out_stats->last_paint_usec           = eng->last_paint_usec;
    out_stats->blocked_requests_count    = eng->blocked_requests_count;

    arcr_font_system_cache_stats(eng->fonts,
                                 &out_stats->glyph_cache_entries,
                                 &out_stats->glyph_cache_hits);

    const ArcrJsVTable *js_vt = get_js_vt(eng);
    for (const ArcrWebView *v = eng->active_views_head; v != NULL; v = v->next) {
        if (v->doc) {
            out_stats->arena_allocated_bytes += v->doc->arena.total_allocated + v->doc->style_arena.total_allocated;
            out_stats->arena_used_bytes      += v->doc->arena.total_used + v->doc->style_arena.total_used;
            out_stats->dom_node_count        += v->doc->node_count;
        }
        if (v->layout) {
            out_stats->arena_allocated_bytes += v->layout->arena.total_allocated;
            out_stats->arena_used_bytes      += v->layout->arena.total_used;
            out_stats->layout_box_count      += v->layout->box_count;
        }
        if (v->display_list) {
            out_stats->arena_allocated_bytes += v->display_list->arena.total_allocated;
            out_stats->arena_used_bytes      += v->display_list->arena.total_used;
            out_stats->display_cmd_count     += v->display_list->count;
        }
        if (v->js_vm && js_vt && js_vt->get_used_heap_bytes) {
            out_stats->js_heap_bytes += js_vt->get_used_heap_bytes(v->js_vm);
        }
        out_stats->style_shared_hits += v->last_style_share_hits;
    }
}

void arcr_engine_notify_memory_pressure(ArcrEngine *eng, int pressure_level) {
    if (!eng) return;

    const ArcrJsVTable *js_vt = get_js_vt(eng);
    if (js_vt && js_vt->release_caches_on_pressure) {
        for (ArcrWebView *v = eng->active_views_head; v != NULL; v = v->next) {
            if (v->js_vm) {
                js_vt->release_caches_on_pressure(v->js_vm, pressure_level);
            }
        }
    }

    if (pressure_level >= 1 && eng->fonts) {
        arcr_font_system_purge_cache(eng->fonts);
    }

    if (pressure_level >= 2 && eng->cfg.private_browsing) {
        const ArcrNetVTable *net_vt = get_net_vt(eng);
        if (net_vt && net_vt->clear_cookies_and_cache) {
            net_vt->clear_cookies_and_cache();
        }
        memset(eng->storage, 0, sizeof(eng->storage));
        eng->storage_cursor = 0u;
    }
}

/* -------------------------------------------------------------------------
 * 6. URL Resolution, <base href>, Shield Filter & HTTP 2xx Resource Fetcher
 * ------------------------------------------------------------------------- */
static void extract_url_host(const char *url, char *out_host, size_t cap) {
    if (!out_host || cap == 0u) return;
    out_host[0] = '\0';
    if (!url || url[0] == '\0') return;

    const char *sep = strstr(url, "://");
    if (!sep) return;
    if (arcr_str_starts_with_nocase(url, "file://")) return;

    const char *host_start = sep + 3;
    const char *at    = strchr(host_start, '@');
    const char *slash = strchr(host_start, '/');
    const char *qmark = strchr(host_start, '?');
    const char *hash  = strchr(host_start, '#');

    const char *host_end = host_start + strlen(host_start);
    if (slash && slash < host_end) host_end = slash;
    if (qmark && qmark < host_end) host_end = qmark;
    if (hash  && hash  < host_end) host_end = hash;
    if (at && at < host_end) host_start = at + 1;

    const char *port_colon = NULL;
    if (host_start < host_end && *host_start != '[') {
        for (const char *p = host_start; p < host_end; ++p) {
            if (*p == ':') {
                port_colon = p;
                break;
            }
        }
    }
    if (port_colon) host_end = port_colon;

    size_t len = (size_t)(host_end - host_start);
    if (len >= cap) len = cap - 1u;
    for (size_t i = 0u; i < len; ++i) {
        out_host[i] = arcr_ascii_tolower(host_start[i]);
    }
    out_host[len] = '\0';
}

static bool has_explicit_scheme(const char *s) {
    if (!s || s[0] == '\0') return false;
    if (strstr(s, "://") != NULL) return true;
    return arcr_str_starts_with_nocase(s, "about:") ||
           arcr_str_starts_with_nocase(s, "data:") ||
           arcr_str_starts_with_nocase(s, "blob:") ||
           arcr_str_starts_with_nocase(s, "javascript:") ||
           arcr_str_starts_with_nocase(s, "mailto:") ||
           arcr_str_starts_with_nocase(s, "waterscape:") ||
           arcr_str_starts_with_nocase(s, "arcr:");
}

static void normalize_dot_segments(char *path, size_t path_cap) {
    if (!path || path[0] != '/' || path_cap < 2u) return;
    char out[ARCR_URL_BUF_CAP];
    size_t out_len = 0u;
    const char *p = path;

    while (*p != '\0') {
        if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            p += 2;
            if (*p == '\0' && out_len == 0u) {
                out[out_len++] = '/';
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            p += 3;
            while (out_len > 0u && out[out_len - 1u] != '/') {
                --out_len;
            }
            if (out_len > 0u) --out_len;
            if (*p == '\0' && out_len == 0u) {
                out[out_len++] = '/';
            }
            continue;
        }
        if (out_len + 1u < sizeof(out)) {
            out[out_len++] = *p;
        }
        ++p;
    }
    if (out_len == 0u) {
        out[out_len++] = '/';
    }
    out[out_len] = '\0';
    arcr_str_copy(path, path_cap, out);
}

static void buf_append_cstr(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u || !src) return;
    size_t cur = strlen(dst);
    if (cur + 1u >= cap) return;
    arcr_str_copy(dst + cur, cap - cur, src);
}

static void resolve_relative_url_local(const char *base_url, const char *rel_url,
                                       char *out_url, size_t out_cap) {
    if (!out_url || out_cap == 0u) return;
    out_url[0] = '\0';
    if (!rel_url) return;

    const char *rel = rel_url;
    size_t rel_len = strlen(rel_url);
    arcr_str_trim_view(&rel, &rel_len);
    if (rel_len == 0u) {
        arcr_str_copy(out_url, out_cap, base_url ? base_url : "");
        return;
    }

    char trimmed[ARCR_URL_BUF_CAP];
    if (rel_len >= sizeof(trimmed)) rel_len = sizeof(trimmed) - 1u;
    memcpy(trimmed, rel, rel_len);
    trimmed[rel_len] = '\0';

    if (has_explicit_scheme(trimmed) || !base_url || base_url[0] == '\0') {
        arcr_str_copy(out_url, out_cap, trimmed);
        return;
    }

    const char *sep = strstr(base_url, "://");
    if (!sep) {
        arcr_str_copy(out_url, out_cap, trimmed);
        return;
    }

    if (trimmed[0] == '/' && trimmed[1] == '/') {
        size_t scheme_len = (size_t)(sep - base_url);
        if (scheme_len >= 16u) scheme_len = 15u;
        char scheme[16];
        memcpy(scheme, base_url, scheme_len);
        scheme[scheme_len] = '\0';
        arcr_str_copy(out_url, out_cap, scheme);
        buf_append_cstr(out_url, out_cap, ":");
        buf_append_cstr(out_url, out_cap, trimmed);
        return;
    }

    if (trimmed[0] == '?') {
        char base_clean[ARCR_URL_BUF_CAP];
        arcr_str_copy(base_clean, sizeof(base_clean), base_url);
        char *q = strchr(base_clean, '?');
        char *h = strchr(base_clean, '#');
        if (q) *q = '\0';
        if (h) *h = '\0';
        arcr_str_copy(out_url, out_cap, base_clean);
        buf_append_cstr(out_url, out_cap, trimmed);
        return;
    }
    if (trimmed[0] == '#') {
        char base_clean[ARCR_URL_BUF_CAP];
        arcr_str_copy(base_clean, sizeof(base_clean), base_url);
        char *h = strchr(base_clean, '#');
        if (h) *h = '\0';
        arcr_str_copy(out_url, out_cap, base_clean);
        buf_append_cstr(out_url, out_cap, trimmed);
        return;
    }

    const char *after_scheme = sep + 3;
    bool is_file = arcr_str_starts_with_nocase(base_url, "file://");
    const char *path_start = is_file ? after_scheme : strchr(after_scheme, '/');

    char origin[512];
    if (is_file) {
        arcr_str_copy(origin, sizeof(origin), "file://");
    } else if (path_start) {
        size_t olen = (size_t)(path_start - base_url);
        if (olen >= sizeof(origin)) olen = sizeof(origin) - 1u;
        memcpy(origin, base_url, olen);
        origin[olen] = '\0';
    } else {
        arcr_str_copy(origin, sizeof(origin), base_url);
    }

    if (trimmed[0] == '/') {
        arcr_str_copy(out_url, out_cap, origin);
        buf_append_cstr(out_url, out_cap, trimmed);
        return;
    }

    char dir_path[ARCR_URL_BUF_CAP];
    if (path_start) {
        arcr_str_copy(dir_path, sizeof(dir_path), path_start);
        char *q = strchr(dir_path, '?');
        char *h = strchr(dir_path, '#');
        if (q) *q = '\0';
        if (h) *h = '\0';
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash) {
            last_slash[1] = '\0';
        } else {
            arcr_str_copy(dir_path, sizeof(dir_path), "/");
        }
    } else {
        arcr_str_copy(dir_path, sizeof(dir_path), "/");
    }

    char rel_path_only[ARCR_URL_BUF_CAP];
    const char *suffix = "";
    arcr_str_copy(rel_path_only, sizeof(rel_path_only), trimmed);
    for (size_t i = 0u; rel_path_only[i] != '\0'; ++i) {
        if (rel_path_only[i] == '?' || rel_path_only[i] == '#') {
            suffix = trimmed + i;
            rel_path_only[i] = '\0';
            break;
        }
    }

    char combined_path[ARCR_URL_BUF_CAP];
    arcr_str_copy(combined_path, sizeof(combined_path), dir_path);
    buf_append_cstr(combined_path, sizeof(combined_path), rel_path_only);
    normalize_dot_segments(combined_path, sizeof(combined_path));

    arcr_str_copy(out_url, out_cap, origin);
    buf_append_cstr(out_url, out_cap, combined_path);
    buf_append_cstr(out_url, out_cap, suffix);
}

static void resolve_url_with_engine(const ArcrEngine *eng, const char *base_url,
                                    const char *rel_url, char *out_url, size_t out_cap) {
    const ArcrNetVTable *net_vt = get_net_vt(eng);
    if (net_vt && net_vt->resolve_url && base_url && base_url[0] != '\0') {
        if (net_vt->resolve_url(base_url, rel_url, out_url, out_cap)) {
            return;
        }
    }
    resolve_relative_url_local(base_url, rel_url, out_url, out_cap);
}

static void view_effective_base_url(const ArcrWebView *view, char *out_base, size_t cap) {
    if (!out_base || cap == 0u) return;
    out_base[0] = '\0';
    if (!view) return;
    if (view->doc && view->doc->base_href && view->doc->base_href[0] != '\0') {
        resolve_url_with_engine(view->engine, view->url, view->doc->base_href, out_base, cap);
        if (out_base[0] != '\0') return;
    }
    arcr_str_copy(out_base, cap, view->url);
}

static bool check_url_blocked(ArcrEngine *eng, const char *resolved_url, const char *page_url) {
    if (!eng || !eng->url_filter || !resolved_url || resolved_url[0] == '\0') {
        return false;
    }
    char page_host[192];
    extract_url_host(page_url ? page_url : "", page_host, sizeof(page_host));
    if (eng->url_filter(eng->url_filter_ud, resolved_url, page_host)) {
        eng->blocked_requests_count++;
        return true;
    }
    return false;
}

static bool is_fetchable_base_url(const char *url) {
    if (!url || url[0] == '\0') return false;
    if (arcr_str_starts_with_nocase(url, "about:") ||
        arcr_str_starts_with_nocase(url, "waterscape://")) {
        return false;
    }
    return arcr_str_starts_with_nocase(url, "http://") ||
           arcr_str_starts_with_nocase(url, "https://") ||
           arcr_str_starts_with_nocase(url, "file://");
}

static void free_net_http_response(const ArcrNetVTable *net_vt, ArcrHttpResponse *resp) {
    if (!resp) return;
    if (net_vt && net_vt->free_response) {
        net_vt->free_response(resp);
        resp->body     = NULL;
        resp->body_len = 0u;
    } else if (resp->body) {
        if (net_vt && net_vt->free_resource) {
            net_vt->free_resource((char *)resp->body);
        } else {
            free(resp->body);
        }
        resp->body     = NULL;
        resp->body_len = 0u;
    }
}

static void engine_free_resource(const ArcrEngine *eng, uint8_t *bytes) {
    if (!bytes) return;
    const ArcrNetVTable *net_vt = get_net_vt(eng);
    if (net_vt && net_vt->free_resource) {
        net_vt->free_resource((char *)bytes);
    } else {
        free(bytes);
    }
}

static bool is_http_status_2xx_or_local(int status_code) {
    return status_code == 0 || (status_code >= 200 && status_code < 300);
}

static bool engine_fetch_resource_ex(ArcrEngine        *eng,
                                     const char        *target_url,
                                     const char        *context_page_url,
                                     const char        *method,
                                     const char        *post_body,
                                     size_t             post_body_len,
                                     bool               require_2xx,
                                     uint8_t          **out_bytes,
                                     size_t            *out_len,
                                     char              *out_final_url,
                                     size_t             final_url_cap,
                                     ArcrSecurityState *out_sec,
                                     char              *out_detail,
                                     size_t             detail_cap,
                                     bool              *out_was_blocked,
                                     int               *out_http_status) {
    if (out_bytes)       *out_bytes       = NULL;
    if (out_len)         *out_len         = 0u;
    if (out_http_status) *out_http_status = 0;
    if (out_final_url && final_url_cap > 0u) {
        arcr_str_copy(out_final_url, final_url_cap, target_url ? target_url : "");
    }
    if (out_sec) *out_sec = ARCR_SEC_LOCAL_OR_NONE;
    if (out_detail && detail_cap > 0u) out_detail[0] = '\0';
    if (out_was_blocked) *out_was_blocked = false;

    if (!eng || !target_url || target_url[0] == '\0') return false;

    if (check_url_blocked(eng, target_url, context_page_url)) {
        if (out_was_blocked) *out_was_blocked = true;
        return false;
    }

    if (!ensure_net_module(eng)) return false;
    const ArcrNetVTable *net_vt = get_net_vt(eng);
    if (!net_vt) return false;

    bool is_post = (method && arcr_str_eq_nocase(method, "POST"));
    if (net_vt->fetch_http) {
        ArcrHttpRequest req;
        memset(&req, 0, sizeof(req));
        req.url              = target_url;
        req.method           = (method && method[0]) ? method : "GET";
        req.post_body        = post_body;
        req.post_body_len    = post_body_len;
        req.content_type     = is_post ? "application/x-www-form-urlencoded" : NULL;
        req.referer          = (context_page_url && is_fetchable_base_url(context_page_url))
                                   ? context_page_url
                                   : NULL;
        req.private_browsing = eng->cfg.private_browsing;
        req.bypass_cache     = is_post;

        ArcrHttpResponse resp;
        memset(&resp, 0, sizeof(resp));
        bool ok = net_vt->fetch_http(&req, &resp);
        if (out_http_status) *out_http_status = resp.status_code;
        if (out_sec)         *out_sec         = resp.security;
        if (out_detail && detail_cap > 0u) {
            arcr_str_copy(out_detail, detail_cap, resp.detail);
        }
        if (out_final_url && final_url_cap > 0u && resp.final_url[0] != '\0') {
            arcr_str_copy(out_final_url, final_url_cap, resp.final_url);
        }

        if (!ok || !resp.body || (require_2xx && !is_http_status_2xx_or_local(resp.status_code))) {
            free_net_http_response(net_vt, &resp);
            return false;
        }
        if (out_bytes) {
            *out_bytes = resp.body;
            resp.body  = NULL;
        }
        if (out_len) *out_len = resp.body_len;
        free_net_http_response(net_vt, &resp);
        return true;
    }

    if (net_vt->load_url_resource) {
        char  *body = NULL;
        size_t len  = 0u;
        ArcrSecurityState sec = ARCR_SEC_LOCAL_OR_NONE;
        char detail_buf[256] = {0};

        bool ok = net_vt->load_url_resource(target_url, &body, &len, &sec,
                                            detail_buf, sizeof(detail_buf));
        if (out_sec) *out_sec = sec;
        if (out_detail && detail_cap > 0u) {
            arcr_str_copy(out_detail, detail_cap, detail_buf);
        }
        if (!ok || !body) {
            if (body && net_vt->free_resource) net_vt->free_resource(body);
            return false;
        }
        if (out_bytes) *out_bytes = (uint8_t *)body;
        else if (net_vt->free_resource) net_vt->free_resource(body);
        if (out_len)   *out_len   = len;
        if (out_http_status) *out_http_status = 200;
        return true;
    }

    return false;
}

/* -------------------------------------------------------------------------
 * 7. Cached Stylesheet, Zero-Leak Hover/@keyframes & Layout/Paint Pipeline
 * ------------------------------------------------------------------------- */
static void clamp_view_scroll(ArcrWebView *view) {
    if (!view) return;
    int32_t max_scroll = 0;
    if (view->layout && view->layout->total_height > view->height) {
        max_scroll = view->layout->total_height - view->height;
    }
    view->scroll_y = ARCR_CLAMP(view->scroll_y, 0, max_scroll);
}

static void notify_view_dirty(ArcrWebView *view) {
    if (view && view->cb.on_dirty_rect) {
        view->cb.on_dirty_rect(view->cb.userdata,
                               (ArcrRect){0, 0, view->width, view->height});
    }
}

static void build_view_display_list(ArcrWebView *view, int32_t *out_active_find_y) {
    if (out_active_find_y) *out_active_find_y = -1;
    if (!view) return;

    arcr_paint_destroy_display_list(view->display_list);
    view->display_list = NULL;
    if (!view->layout) return;

    if (view->find_query[0] != '\0') {
        uint32_t total_matches = 0u;
        int32_t  active_y      = -1;
        view->display_list = arcr_paint_build_display_list_ex(
            view->layout, view->find_query, view->find_active_idx,
            &total_matches, &active_y);
        if (total_matches > 0u && view->find_active_idx >= total_matches) {
            view->find_active_idx = 0u;
            arcr_paint_destroy_display_list(view->display_list);
            view->display_list = arcr_paint_build_display_list_ex(
                view->layout, view->find_query, 0u,
                &total_matches, &active_y);
        }
        view->find_total_matches = total_matches;
        if (total_matches == 0u) {
            view->find_active_idx = 0u;
        }
        if (out_active_find_y) *out_active_find_y = active_y;
    } else {
        view->display_list       = arcr_paint_build_display_list(view->layout);
        view->find_total_matches = 0u;
    }
}

static void rebuild_layout_and_paint(ArcrWebView *view) {
    if (!view || !view->doc) return;

    uint64_t t0 = arcr_platform_monotonic_usec();

    arcr_paint_destroy_display_list(view->display_list);
    view->display_list = NULL;
    arcr_layout_destroy(view->layout);

    view->layout = arcr_layout_build_ex(
        view->doc, view->engine->fonts, view->width, view->height, view->zoom_percent);
    build_view_display_list(view, NULL);

    uint64_t t1 = arcr_platform_monotonic_usec();
    view->engine->last_layout_usec = (t1 >= t0) ? (t1 - t0) : 0u;

    clamp_view_scroll(view);
    notify_view_dirty(view);
}

/* Re-parses CSS stylesheet (only when DOM/<style>/viewport/theme changes) */
static void restyle_and_relayout(ArcrWebView *view) {
    if (!view || !view->doc) return;

    if (view->sheet) {
        arcr_css_destroy_stylesheet(view->sheet);
        view->sheet = NULL;
    }
    ArcrCssMediaEnv env = {
        .viewport_width  = view->width,
        .viewport_height = view->height,
        .prefers_dark    = view->prefers_dark
    };
    view->sheet = arcr_css_parse_document_styles_ex(view->doc, &env);

    uint64_t now_usec = arcr_platform_monotonic_usec();
    if (view->anim_start_usec == 0u) {
        view->anim_start_usec = now_usec;
    }
    uint64_t elapsed_ms = (now_usec >= view->anim_start_usec)
                              ? ((now_usec - view->anim_start_usec) / 1000ULL)
                              : 0ULL;
    arcr_css_resolve_tree_animated(view->doc, view->sheet, elapsed_ms,
                                   &view->last_style_share_hits,
                                   &view->has_css_animations);
    view->doc->dom_dirty = false;
    rebuild_layout_and_paint(view);
}

/* Reuses cached ArcrStylesheet with resettable style_arena (0 hover/animation OOM!) */
static void restyle_cached_sheet_only(ArcrWebView *view, uint64_t now_usec) {
    if (!view || !view->doc) return;
    if (!view->sheet || view->doc->dom_dirty) {
        restyle_and_relayout(view);
        return;
    }
    if (now_usec == 0u) {
        now_usec = arcr_platform_monotonic_usec();
    }
    if (view->anim_start_usec == 0u) {
        view->anim_start_usec = now_usec;
    }
    uint64_t elapsed_ms = (now_usec >= view->anim_start_usec)
                              ? ((now_usec - view->anim_start_usec) / 1000ULL)
                              : 0ULL;
    arcr_css_resolve_tree_animated(view->doc, view->sheet, elapsed_ms,
                                   &view->last_style_share_hits,
                                   &view->has_css_animations);
    rebuild_layout_and_paint(view);
}

/* -------------------------------------------------------------------------
 * 8. Parallel Subresources, Animated GIFs & <video> Media Players
 * ------------------------------------------------------------------------- */
static void free_dom_anim_surfaces_recursive(ArcrDomNode *node) {
    if (!node) return;
    if (node->surface) {
        free(node->surface->anim_frames_argb);
        free(node->surface->anim_delays_ms);
        node->surface->anim_frames_argb = NULL;
        node->surface->anim_delays_ms   = NULL;
        node->surface->anim_frame_count = 0;
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        free_dom_anim_surfaces_recursive(ch);
    }
}

static void destroy_view_video_players(ArcrWebView *view) {
    if (!view) return;
    const ArcrMediaVTable *mvt = get_media_vt(view->engine);
    for (size_t i = 0u; i < view->video_count; ++i) {
        if (view->videos[i].mp && mvt && mvt->player_destroy) {
            mvt->player_destroy(view->videos[i].mp);
        }
        view->videos[i].mp       = NULL;
        view->videos[i].node_uid = 0u;
    }
    view->video_count = 0u;
}

static void sync_view_video_players(ArcrWebView *view) {
    if (!view || !view->doc || !view->engine) return;
    if (!view->doc->has_media) return;

    ArcrDomNode *vid_nodes[ARCR_MAX_VIEW_VIDEOS];
    size_t found = arcr_dom_query_selector_all(
        view->doc, view->doc->root, "video", vid_nodes, ARCR_MAX_VIEW_VIDEOS);
    if (found == 0u) {
        /* Still ensure media module if <audio> set has_media (preserves test_13!) */
        (void)ensure_media_module(view->engine);
        return;
    }
    if (!ensure_media_module(view->engine)) return;
    const ArcrMediaVTable *mvt = get_media_vt(view->engine);
    if (!mvt || !mvt->player_create) return;

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    uint64_t now_usec = arcr_platform_monotonic_usec();

    for (size_t i = 0u; i < found && view->video_count < ARCR_MAX_VIEW_VIDEOS; ++i) {
        ArcrDomNode *vn = vid_nodes[i];
        if (!vn || vn->node_uid == 0u) continue;

        bool already_bound = false;
        for (size_t k = 0u; k < view->video_count; ++k) {
            if (view->videos[k].node_uid == vn->node_uid) {
                already_bound = true;
                break;
            }
        }
        if (already_bound) continue;

        const char *raw_src = arcr_dom_get_attr_cstr(view->doc, vn, "src");
        if (!raw_src || raw_src[0] == '\0') {
            ArcrDomNode *src_el = NULL;
            if (arcr_dom_query_selector_all(view->doc, vn, "source", &src_el, 1u) > 0u && src_el) {
                raw_src = arcr_dom_get_attr_cstr(view->doc, src_el, "src");
            }
        }

        char resolved_src[ARCR_URL_BUF_CAP] = {0};
        if (raw_src && raw_src[0] != '\0') {
            resolve_url_with_engine(view->engine, eff_base, raw_src,
                                    resolved_src, sizeof(resolved_src));
        } else {
            arcr_str_copy(resolved_src, sizeof(resolved_src), view->url);
        }

        int32_t vw = 640, vh = 360;
        const char *wa = arcr_dom_get_attr_cstr(view->doc, vn, "width");
        const char *ha = arcr_dom_get_attr_cstr(view->doc, vn, "height");
        int32_t pw = 0, ph = 0;
        if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw >= 64 && pw <= 1920) vw = pw;
        if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph >= 48 && ph <= 1080) vh = ph;

        const char *title_hint = arcr_dom_get_attr_cstr(view->doc, vn, "title");
        if (!title_hint || title_hint[0] == '\0') title_hint = view->title;

        ArcrMediaPlayer *mp = mvt->player_create(resolved_src, NULL, 0u, vw, vh, title_hint);
        if (!mp) continue;

        const char *preload = arcr_dom_get_attr_cstr(view->doc, vn, "preload");
        bool autoplay = (arcr_dom_get_attr_cstr(view->doc, vn, "autoplay") != NULL);
        if (preload && arcr_str_eq_nocase(preload, "none") && !autoplay && mvt->player_set_paused) {
            mvt->player_set_paused(mp, true);
        }

        ArcrDomSurface *surf = arcr_dom_ensure_surface(view->doc, vn, vw, vh);
        if (surf && surf->argb_pixels && mvt->player_tick_frame) {
            (void)mvt->player_tick_frame(mp, now_usec, surf->argb_pixels, vw, vh, NULL, NULL);
        }

        ViewVideoSlot *slot = &view->videos[view->video_count++];
        slot->node_uid = vn->node_uid;
        slot->mp       = mp;
        slot->w        = vw;
        slot->h        = vh;
    }
}

static void init_form_defaults_recursive(ArcrDocument *doc, ArcrDomNode *node,
                                         ArcrAtom input_atom, ArcrAtom textarea_atom) {
    if (!doc || !node) return;
    if (node->kind == ARCR_NODE_ELEMENT) {
        if (node->tag == input_atom) {
            if (arcr_dom_get_attr_cstr(doc, node, "checked") != NULL) {
                node->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
            }
            const char *type = arcr_dom_get_attr_cstr(doc, node, "type");
            bool is_non_text = type && (arcr_str_eq_nocase(type, "submit") ||
                                        arcr_str_eq_nocase(type, "button") ||
                                        arcr_str_eq_nocase(type, "reset") ||
                                        arcr_str_eq_nocase(type, "checkbox") ||
                                        arcr_str_eq_nocase(type, "radio"));
            if (!is_non_text && !node->input_value) {
                const char *val = arcr_dom_get_attr_cstr(doc, node, "value");
                if (val) {
                    arcr_dom_set_input_value(doc, node, val);
                }
            }
        } else if (node->tag == textarea_atom && !node->input_value) {
            char buf[1024];
            size_t n = arcr_dom_get_text_content(node, buf, sizeof(buf));
            if (n > 0u) {
                arcr_dom_set_input_value(doc, node, buf);
            }
        }
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        init_form_defaults_recursive(doc, ch, input_atom, textarea_atom);
    }
}

static void attach_decoded_image(ArcrDocument *doc, ArcrDomNode *img_node, const ArcrImage *img) {
    if (!doc || !img_node || !img || img->width <= 0 || img->height <= 0 || !img->pixels) return;
    ArcrDomSurface *surf = arcr_dom_ensure_surface(doc, img_node, img->width, img->height);
    if (surf && surf->argb_pixels) {
        memcpy(surf->argb_pixels, img->pixels,
               (size_t)img->width * (size_t)img->height * sizeof(uint32_t));
    }
}

static void attach_image_bytes_to_node(ArcrWebView *view, ArcrDomNode *img_node,
                                       const char *content_type,
                                       const uint8_t *bytes, size_t blen) {
    if (!view || !view->doc || !img_node || !bytes || blen == 0u) return;
    ArcrDocument *doc = view->doc;

    /* 1. Multi-frame Animated GIF check ("GIF87a" / "GIF89a") */
    if (blen >= 6u && (memcmp(bytes, "GIF87a", 6u) == 0 || memcmp(bytes, "GIF89a", 6u) == 0)) {
        ArcrAnimImage anim;
        memset(&anim, 0, sizeof(anim));
        if (arcr_image_decode_anim_mem(bytes, blen, &anim)) {
            if (anim.frame_count > 1 && anim.width > 0 && anim.height > 0 && anim.frames_argb) {
                ArcrDomSurface *surf = arcr_dom_ensure_surface(doc, img_node, anim.width, anim.height);
                if (surf && surf->argb_pixels) {
                    memcpy(surf->argb_pixels, anim.frames_argb,
                           (size_t)anim.width * (size_t)anim.height * sizeof(uint32_t));
                    free(surf->anim_frames_argb);
                    free(surf->anim_delays_ms);
                    surf->anim_frames_argb   = anim.frames_argb;
                    surf->anim_delays_ms     = anim.delays_ms;
                    surf->anim_frame_count   = anim.frame_count;
                    surf->anim_current_frame = 0;
                    int32_t d0 = (anim.delays_ms && anim.delays_ms[0] >= 20) ? anim.delays_ms[0] : 80;
                    surf->anim_next_tick_usec = arcr_platform_monotonic_usec() +
                                                (uint64_t)d0 * 1000ULL;
                    view->has_anim_images = true;
                    return;
                }
            }
            arcr_image_free_anim(&anim);
        }
    }

    /* 2. Standard PNG / JPEG / WebP / GIF / BMP / ICO decode */
    ArcrImage img;
    memset(&img, 0, sizeof(img));
    if (arcr_image_decode_mem(bytes, blen, &img)) {
        attach_decoded_image(doc, img_node, &img);
        arcr_image_free(&img);
        return;
    }

    /* 3. Vector SVG <img src="icon.svg"> fallback */
    bool looks_svg = (content_type && strstr(content_type, "svg") != NULL);
    if (!looks_svg && blen >= 4u) {
        const char *s = (const char *)bytes;
        for (size_t i = 0u; i + 4u <= blen && i < 256u; ++i) {
            if (s[i] == '<' && arcr_str_eq_nocase_n(s + i + 1u, 3u, "svg", 3u)) {
                looks_svg = true;
                break;
            }
        }
    }
    if (looks_svg) {
        int32_t tw = 0, th = 0;
        const char *wa = arcr_dom_get_attr_cstr(doc, img_node, "width");
        const char *ha = arcr_dom_get_attr_cstr(doc, img_node, "height");
        if (wa) (void)arcr_parse_i32(wa, strlen(wa), &tw);
        if (ha) (void)arcr_parse_i32(ha, strlen(ha), &th);
        if (arcr_svg_rasterize_mem((const char *)bytes, blen, tw, th, 0xFF0F172Au, &img)) {
            attach_decoded_image(doc, img_node, &img);
            arcr_image_free(&img);
        }
    }
}

typedef enum SubresKind {
    SUBRES_STYLE  = 0,
    SUBRES_IMAGE  = 1,
    SUBRES_SCRIPT = 2
} SubresKind;

typedef struct SubresPendingItem {
    SubresKind               kind;
    ArcrExternalResourceRef *ref;
    uint32_t                 owner_uid;
    char                     url[ARCR_URL_BUF_CAP];
} SubresPendingItem;

static void fetch_document_subresources(ArcrWebView *view) {
    if (!view || !view->doc || !view->engine) return;
    ArcrDocument *doc = view->doc;
    ArcrEngine   *eng = view->engine;

    /* 1. Handle inline data: image URIs without touching arcr_net! */
    for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
        if (r->href_or_src && arcr_str_starts_with_nocase(r->href_or_src, "data:")) {
            const char *uri = r->href_or_src;
            r->href_or_src = NULL; /* Mark processed so repeat passes stay idempotent */
            ArcrDomNode *owner = (r->owner_uid != 0u)
                                     ? arcr_dom_find_by_uid(doc->root, r->owner_uid)
                                     : r->owner_node;
            if (owner && !owner->surface) {
                ArcrImage img;
                memset(&img, 0, sizeof(img));
                if (arcr_image_decode_data_uri(uri, strlen(uri), &img)) {
                    attach_decoded_image(doc, owner, &img);
                    arcr_image_free(&img);
                }
            }
        }
    }

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    if (!is_fetchable_base_url(eff_base) && !is_fetchable_base_url(view->url)) {
        return;
    }

    /* Count unprocessed external refs first so static pages never load arcr_net */
    size_t raw_candidates = 0u;
    for (ArcrExternalResourceRef *r = doc->ext_styles_head; r != NULL; r = r->next) {
        if (r->href_or_src && r->href_or_src[0] != '\0') ++raw_candidates;
    }
    for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
        if (r->href_or_src && r->href_or_src[0] != '\0') ++raw_candidates;
    }
    if (eng->cfg.enable_javascript) {
        for (ArcrExternalResourceRef *r = doc->ext_scripts_head; r != NULL; r = r->next) {
            if (r->href_or_src && r->href_or_src[0] != '\0') ++raw_candidates;
        }
    }
    if (raw_candidates == 0u) return;

    (void)ensure_net_module(eng);
    const ArcrNetVTable *net_vt = get_net_vt(eng);

    SubresPendingItem *items = (SubresPendingItem *)calloc(raw_candidates, sizeof(SubresPendingItem));
    if (!items) return;
    size_t item_count = 0u;

    for (ArcrExternalResourceRef *r = doc->ext_styles_head; r != NULL; r = r->next) {
        if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
        const char *raw = r->href_or_src;
        r->href_or_src = NULL;
        SubresPendingItem *it = &items[item_count];
        resolve_url_with_engine(eng, eff_base, raw, it->url, sizeof(it->url));
        if (check_url_blocked(eng, it->url, view->url)) continue;
        it->kind      = SUBRES_STYLE;
        it->ref       = r;
        it->owner_uid = r->owner_uid;
        ++item_count;
    }

    for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
        if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
        const char *raw = r->href_or_src;
        r->href_or_src = NULL;
        SubresPendingItem *it = &items[item_count];
        resolve_url_with_engine(eng, eff_base, raw, it->url, sizeof(it->url));
        if (check_url_blocked(eng, it->url, view->url)) continue;
        it->kind      = SUBRES_IMAGE;
        it->ref       = r;
        it->owner_uid = r->owner_uid;
        ++item_count;
    }

    if (eng->cfg.enable_javascript) {
        for (ArcrExternalResourceRef *r = doc->ext_scripts_head; r != NULL; r = r->next) {
            if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
            const char *raw = r->href_or_src;
            r->href_or_src = NULL;
            SubresPendingItem *it = &items[item_count];
            resolve_url_with_engine(eng, eff_base, raw, it->url, sizeof(it->url));
            if (check_url_blocked(eng, it->url, view->url)) continue;
            it->kind      = SUBRES_SCRIPT;
            it->ref       = r;
            it->owner_uid = r->owner_uid;
            ++item_count;
        }
    }

    if (!net_vt || item_count == 0u) {
        free(items);
        return;
    }

    /* Parallel multi-core batch fetch when fetch_batch_parallel is available */
    if (net_vt->fetch_batch_parallel && item_count > 1u) {
        const char *ref_hdr = is_fetchable_base_url(view->url) ? view->url : NULL;
        for (size_t base = 0u; base < item_count; base += ARCR_SUBRES_BATCH_MAX) {
            size_t bn = item_count - base;
            if (bn > ARCR_SUBRES_BATCH_MAX) bn = ARCR_SUBRES_BATCH_MAX;

            ArcrHttpRequest  reqs[ARCR_SUBRES_BATCH_MAX];
            ArcrHttpResponse resps[ARCR_SUBRES_BATCH_MAX];
            memset(reqs, 0, sizeof(reqs));
            memset(resps, 0, sizeof(resps));

            for (size_t i = 0u; i < bn; ++i) {
                reqs[i].url              = items[base + i].url;
                reqs[i].method           = "GET";
                reqs[i].referer          = ref_hdr;
                reqs[i].private_browsing = eng->cfg.private_browsing;
            }

            net_vt->fetch_batch_parallel(reqs, resps, bn);

            for (size_t i = 0u; i < bn; ++i) {
                SubresPendingItem *it = &items[base + i];
                ArcrHttpResponse  *rp = &resps[i];
                if (rp->body && rp->body_len > 0u && is_http_status_2xx_or_local(rp->status_code)) {
                    ArcrDomNode *owner = (it->owner_uid != 0u)
                                             ? arcr_dom_find_by_uid(doc->root, it->owner_uid)
                                             : (it->ref ? it->ref->owner_node : NULL);
                    if (it->kind == SUBRES_STYLE) {
                        arcr_dom_append_style_block(doc, (const char *)rp->body, rp->body_len);
                    } else if (it->kind == SUBRES_IMAGE && owner) {
                        attach_image_bytes_to_node(view, owner, rp->content_type,
                                                   rp->body, rp->body_len);
                    } else if (it->kind == SUBRES_SCRIPT && owner) {
                        owner->text     = arcr_arena_strdup(&doc->arena, (const char *)rp->body, rp->body_len);
                        owner->text_len = rp->body_len;
                        doc->has_scripts = true;
                    }
                }
                free_net_http_response(net_vt, rp);
            }
        }
        free(items);
        return;
    }

    /* Single-item or legacy load_url_resource fallback */
    for (size_t i = 0u; i < item_count; ++i) {
        SubresPendingItem *it = &items[i];
        uint8_t *bytes = NULL;
        size_t   blen  = 0u;
        if (net_vt->fetch_http) {
            ArcrHttpRequest req;
            ArcrHttpResponse rp;
            memset(&req, 0, sizeof(req));
            memset(&rp, 0, sizeof(rp));
            req.url              = it->url;
            req.method           = "GET";
            req.referer          = is_fetchable_base_url(view->url) ? view->url : NULL;
            req.private_browsing = eng->cfg.private_browsing;
            if (net_vt->fetch_http(&req, &rp) && rp.body && rp.body_len > 0u &&
                is_http_status_2xx_or_local(rp.status_code)) {
                ArcrDomNode *owner = (it->owner_uid != 0u)
                                         ? arcr_dom_find_by_uid(doc->root, it->owner_uid)
                                         : (it->ref ? it->ref->owner_node : NULL);
                if (it->kind == SUBRES_STYLE) {
                    arcr_dom_append_style_block(doc, (const char *)rp.body, rp.body_len);
                } else if (it->kind == SUBRES_IMAGE && owner) {
                    attach_image_bytes_to_node(view, owner, rp.content_type, rp.body, rp.body_len);
                } else if (it->kind == SUBRES_SCRIPT && owner) {
                    owner->text      = arcr_arena_strdup(&doc->arena, (const char *)rp.body, rp.body_len);
                    owner->text_len  = rp.body_len;
                    doc->has_scripts = true;
                }
            }
            free_net_http_response(net_vt, &rp);
        } else if (engine_fetch_resource_ex(eng, it->url, view->url, "GET", NULL, 0u,
                                            true, &bytes, &blen, NULL, 0u, NULL, NULL, 0u, NULL, NULL)) {
            ArcrDomNode *owner = (it->owner_uid != 0u)
                                     ? arcr_dom_find_by_uid(doc->root, it->owner_uid)
                                     : (it->ref ? it->ref->owner_node : NULL);
            if (bytes && blen > 0u) {
                if (it->kind == SUBRES_STYLE) {
                    arcr_dom_append_style_block(doc, (const char *)bytes, blen);
                } else if (it->kind == SUBRES_IMAGE && owner) {
                    attach_image_bytes_to_node(view, owner, NULL, bytes, blen);
                } else if (it->kind == SUBRES_SCRIPT && owner) {
                    owner->text      = arcr_arena_strdup(&doc->arena, (const char *)bytes, blen);
                    owner->text_len  = blen;
                    doc->has_scripts = true;
                }
            }
            engine_free_resource(eng, bytes);
        }
    }
    free(items);
}

/* -------------------------------------------------------------------------
 * 9. JavaScript VM Host Bridge & UID-Safe Script Execution
 * ------------------------------------------------------------------------- */
static const char *js_host_get_current_url(void *userdata) {
    const ArcrWebView *view = (const ArcrWebView *)userdata;
    return view ? view->url : "about:blank";
}

static void js_host_set_title(void *userdata, const char *utf8_title) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !utf8_title) return;
    arcr_str_copy(view->title, sizeof(view->title), utf8_title);
    if (view->doc) {
        view->doc->title = arcr_arena_strdup(&view->doc->arena, utf8_title, strlen(utf8_title));
    }
    if (view->cb.on_title_changed) {
        view->cb.on_title_changed(view->cb.userdata, view->title);
    }
}

static void js_host_console_log(void *userdata, const char *utf8_msg) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !utf8_msg) return;
    strbuf_append_cstr(&view->console_log, utf8_msg);
    strbuf_append_char(&view->console_log, '\n');
}

static void js_host_navigate_to(void *userdata, const char *utf8_url) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !utf8_url || utf8_url[0] == '\0') return;
    arcr_str_copy(view->pending_js_nav_url, sizeof(view->pending_js_nav_url), utf8_url);
}

static bool js_host_storage_get(void *userdata, const char *key, char *out_val, size_t cap) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (out_val && cap > 0u) out_val[0] = '\0';
    if (!view || !view->engine || !key) return false;

    const ArcrEngine *eng = view->engine;
    for (size_t i = 0u; i < ARCR_STORAGE_SLOTS; ++i) {
        if (eng->storage[i].used && strcmp(eng->storage[i].key, key) == 0) {
            if (out_val && cap > 0u) {
                arcr_str_copy(out_val, cap, eng->storage[i].val);
            }
            return true;
        }
    }
    return false;
}

static void js_host_storage_set(void *userdata, const char *key, const char *val) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !view->engine || !key || key[0] == '\0') return;

    ArcrEngine *eng = view->engine;
    for (size_t i = 0u; i < ARCR_STORAGE_SLOTS; ++i) {
        if (eng->storage[i].used && strcmp(eng->storage[i].key, key) == 0) {
            if (!val) {
                eng->storage[i].used   = false;
                eng->storage[i].key[0] = '\0';
                eng->storage[i].val[0] = '\0';
            } else {
                arcr_str_copy(eng->storage[i].val, sizeof(eng->storage[i].val), val);
            }
            return;
        }
    }
    if (!val) return;

    for (size_t i = 0u; i < ARCR_STORAGE_SLOTS; ++i) {
        if (!eng->storage[i].used) {
            eng->storage[i].used = true;
            arcr_str_copy(eng->storage[i].key, sizeof(eng->storage[i].key), key);
            arcr_str_copy(eng->storage[i].val, sizeof(eng->storage[i].val), val);
            return;
        }
    }

    size_t idx = eng->storage_cursor % ARCR_STORAGE_SLOTS;
    eng->storage_cursor++;
    eng->storage[idx].used = true;
    arcr_str_copy(eng->storage[idx].key, sizeof(eng->storage[idx].key), key);
    arcr_str_copy(eng->storage[idx].val, sizeof(eng->storage[idx].val), val);
}

static bool js_host_http_fetch(void *userdata, const char *url, const char *method,
                               const char *body, size_t body_len, const char *content_type,
                               int *out_status, char *out_resp_ctype, size_t ctype_cap,
                               uint8_t **out_resp_bytes, size_t *out_resp_len) {
    if (out_status) *out_status = 0;
    if (out_resp_ctype && ctype_cap > 0u) out_resp_ctype[0] = '\0';
    if (out_resp_bytes) *out_resp_bytes = NULL;
    if (out_resp_len)   *out_resp_len   = 0u;

    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !view->engine || !url || url[0] == '\0') return false;

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    char resolved[ARCR_URL_BUF_CAP];
    resolve_url_with_engine(view->engine, eff_base, url, resolved, sizeof(resolved));

    if (check_url_blocked(view->engine, resolved, view->url)) return false;
    if (!ensure_net_module(view->engine)) return false;
    const ArcrNetVTable *net_vt = get_net_vt(view->engine);
    if (!net_vt) return false;

    if (net_vt->fetch_http) {
        ArcrHttpRequest  req;
        ArcrHttpResponse resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        req.url              = resolved;
        req.method           = (method && method[0]) ? method : "GET";
        req.post_body        = body;
        req.post_body_len    = body_len;
        req.content_type     = content_type;
        req.referer          = is_fetchable_base_url(view->url) ? view->url : NULL;
        req.private_browsing = view->engine->cfg.private_browsing;
        req.bypass_cache     = (body != NULL && body_len > 0u);

        bool ok = net_vt->fetch_http(&req, &resp);
        if (out_status) *out_status = resp.status_code;
        if (out_resp_ctype && ctype_cap > 0u) {
            arcr_str_copy(out_resp_ctype, ctype_cap, resp.content_type);
        }
        if (!ok || !resp.body) {
            free_net_http_response(net_vt, &resp);
            return false;
        }
        if (out_resp_bytes) {
            *out_resp_bytes = resp.body;
            resp.body       = NULL;
        }
        if (out_resp_len) *out_resp_len = resp.body_len;
        free_net_http_response(net_vt, &resp);
        return true;
    }

    int status = 0;
    return engine_fetch_resource_ex(view->engine, resolved, view->url, method,
                                    body, body_len, false, out_resp_bytes, out_resp_len,
                                    NULL, 0u, NULL, NULL, 0u, NULL, &status) &&
           (out_status ? (*out_status = status, true) : true);
}

static void js_host_http_free(void *userdata, uint8_t *resp_bytes) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!resp_bytes) return;
    engine_free_resource(view ? view->engine : NULL, resp_bytes);
}

static bool ensure_view_js_vm(ArcrWebView *view) {
    if (!view || !view->doc || !view->engine) return false;
    if (!view->engine->cfg.enable_javascript) return false;
    if (view->js_vm) return true;

    if (!ensure_js_module(view->engine)) return false;
    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->create_vm) return false;

    size_t heap_cap = (view->engine->cfg.profile == ARCR_PROFILE_MINIMUM)
                          ? (4u * 1024u * 1024u)
                          : (32u * 1024u * 1024u);
    ArcrJsHostCallbacks hcb;
    memset(&hcb, 0, sizeof(hcb));
    hcb.userdata           = view;
    hcb.doc                = view->doc;
    hcb.viewport_width     = view->width;
    hcb.viewport_height    = view->height;
    hcb.prefers_dark       = view->prefers_dark;
    hcb.get_current_url    = js_host_get_current_url;
    hcb.set_document_title = js_host_set_title;
    hcb.console_log        = js_host_console_log;
    hcb.navigate_to        = js_host_navigate_to;
    hcb.storage_get        = js_host_storage_get;
    hcb.storage_set        = js_host_storage_set;
    hcb.http_fetch         = js_host_http_fetch;
    hcb.http_free          = js_host_http_free;

    view->js_vm = js_vt->create_vm(heap_cap, &hcb);
    return view->js_vm != NULL;
}

static void sync_doc_title_effects(ArcrWebView *view) {
    if (!view || !view->doc) return;
    if (view->doc->title && view->doc->title[0] != '\0' &&
        strcmp(view->doc->title, view->title) != 0) {
        arcr_str_copy(view->title, sizeof(view->title), view->doc->title);
        if (view->cb.on_title_changed) {
            view->cb.on_title_changed(view->cb.userdata, view->title);
        }
    }
}

static bool handle_pending_js_navigation(ArcrWebView *view, char *out_url, size_t url_cap) {
    if (!view || view->pending_js_nav_url[0] == '\0') return false;

    char raw_target[ARCR_URL_BUF_CAP];
    arcr_str_copy(raw_target, sizeof(raw_target), view->pending_js_nav_url);
    view->pending_js_nav_url[0] = '\0';

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    char resolved[ARCR_URL_BUF_CAP];
    resolve_url_with_engine(view->engine, eff_base, raw_target, resolved, sizeof(resolved));
    if (out_url && url_cap > 0u) {
        arcr_str_copy(out_url, url_cap, resolved);
    }

    if (view->js_nav_depth < ARCR_MAX_JS_NAV_DEPTH) {
        view->js_nav_depth++;
        (void)arcr_webview_navigate_url(view, resolved);
        view->js_nav_depth--;
        return true;
    }
    return false;
}

static bool is_executable_js_script(const ArcrDocument *doc, const ArcrDomNode *el) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    const char *tp = arcr_dom_get_attr_cstr(doc, el, "type");
    if (!tp) return true;

    const char *s = tp;
    size_t len = strlen(tp);
    const char *semi = strchr(tp, ';');
    if (semi) len = (size_t)(semi - tp);
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return true;

    return arcr_str_eq_nocase_n(s, len, "text/javascript", 15u) ||
           arcr_str_eq_nocase_n(s, len, "application/javascript", 22u) ||
           arcr_str_eq_nocase_n(s, len, "application/x-javascript", 24u) ||
           arcr_str_eq_nocase_n(s, len, "text/ecmascript", 15u) ||
           arcr_str_eq_nocase_n(s, len, "application/ecmascript", 22u) ||
           arcr_str_eq_nocase_n(s, len, "module", 6u);
}

static void collect_executable_script_uids(const ArcrDocument *doc, const ArcrDomNode *n,
                                           ArcrAtom script_atom,
                                           uint32_t *out_uids, size_t *io_count, size_t max_cap) {
    if (!doc || !n || *io_count >= max_cap) return;
    if (n->kind == ARCR_NODE_ELEMENT && n->tag == script_atom) {
        if (is_executable_js_script(doc, n) && n->text && n->text_len > 0u && n->node_uid != 0u) {
            out_uids[(*io_count)++] = n->node_uid;
        }
        return;
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_executable_script_uids(doc, ch, script_atom, out_uids, io_count, max_cap);
    }
}

static void run_document_scripts_safe(ArcrWebView *view, ArcrAtom script_atom,
                                      const uint32_t *uids, size_t count) {
    (void)script_atom;
    if (!view || !view->doc || !view->js_vm || !uids || count == 0u) return;
    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->eval_script) return;

    for (size_t i = 0u; i < count; ++i) {
        if (view->pending_js_nav_url[0] != '\0') break;
        const ArcrDomNode *sn = arcr_dom_find_by_uid(view->doc->root, uids[i]);
        if (sn && sn->text && sn->text_len > 0u) {
            (void)js_vt->eval_script(view->js_vm, sn->text, sn->text_len);
        }
    }
}

#include "arcr_site_hydrators.h"
/* -------------------------------------------------------------------------
 * 10. Multi-Core Non-Blocking Async Navigation Worker & Lifecycle
 * ------------------------------------------------------------------------- */
static bool navigate_or_post_internal(ArcrWebView *view, const char *utf8_url,
                                      const char *method,
                                      const char *post_body, size_t post_len);

static void async_job_release(AsyncNavJob *job) {
    if (!job) return;
    eng_mutex_lock(&job->mtx);
    int rem = --job->refs;
    eng_mutex_unlock(&job->mtx);
    if (rem <= 0) {
        eng_mutex_destroy(&job->mtx);
        free(job->post_body);
        job->post_body = NULL;
        free_net_http_response(job->net_vt, &job->resp);
        free(job);
    }
}

static void async_nav_worker_run(AsyncNavJob *job) {
    if (!job) return;
    ArcrEngine *eng = job->eng;

    eng_mutex_lock(&job->mtx);
    bool was_canceled = job->canceled;
    if (!was_canceled) {
        job->progress = 35;
    }
    eng_mutex_unlock(&job->mtx);

    if (!was_canceled && job->net_vt) {
        const ArcrNetVTable *net_vt = job->net_vt;
        ArcrHttpResponse local_resp;
        memset(&local_resp, 0, sizeof(local_resp));
        bool ok = false;

        bool is_post = arcr_str_eq_nocase(job->method, "POST");
        if (net_vt->fetch_http) {
            ArcrHttpRequest req;
            memset(&req, 0, sizeof(req));
            req.url              = job->req_url;
            req.method           = job->method[0] ? job->method : "GET";
            req.post_body        = job->post_body;
            req.post_body_len    = job->post_len;
            req.content_type     = is_post ? "application/x-www-form-urlencoded" : NULL;
            req.referer          = job->referer[0] ? job->referer : NULL;
            req.private_browsing = job->private_browsing;
            req.bypass_cache     = is_post;
            ok = net_vt->fetch_http(&req, &local_resp);
        } else if (net_vt->load_url_resource) {
            char  *body = NULL;
            size_t len  = 0u;
            ArcrSecurityState sec = ARCR_SEC_LOCAL_OR_NONE;
            char detail[256] = {0};
            ok = net_vt->load_url_resource(job->req_url, &body, &len, &sec,
                                           detail, sizeof(detail));
            local_resp.status_code = ok ? 200 : 0;
            local_resp.security    = sec;
            arcr_str_copy(local_resp.final_url, sizeof(local_resp.final_url), job->req_url);
            arcr_str_copy(local_resp.detail, sizeof(local_resp.detail), detail);
            local_resp.body     = (uint8_t *)body;
            local_resp.body_len = len;
        }

        eng_mutex_lock(&job->mtx);
        if (job->canceled) {
            eng_mutex_unlock(&job->mtx);
            free_net_http_response(net_vt, &local_resp);
        } else {
            job->resp     = local_resp;
            job->net_ok   = ok && (local_resp.body != NULL);
            job->progress = 85;
            job->finished = true;
            eng_mutex_unlock(&job->mtx);
        }
    } else if (!was_canceled) {
        eng_mutex_lock(&job->mtx);
        job->net_ok   = false;
        job->progress = 85;
        job->finished = true;
        eng_mutex_unlock(&job->mtx);
    }

    async_job_release(job);

    if (eng) {
        eng_mutex_lock(&eng->async_mtx);
        if (eng->active_async_workers > 0u) {
            eng->active_async_workers--;
        }
        eng_mutex_unlock(&eng->async_mtx);
    }
}

#if defined(_WIN32)
static unsigned __stdcall win32_async_nav_proc(void *arg) {
    async_nav_worker_run((AsyncNavJob *)arg);
    return 0u;
}
#else
static void *posix_async_nav_proc(void *arg) {
    async_nav_worker_run((AsyncNavJob *)arg);
    return NULL;
}
#endif

void arcr_webview_stop_loading(ArcrWebView *view) {
    if (!view) return;
    if (view->async_job) {
        AsyncNavJob *job = view->async_job;
        view->async_job     = NULL;
        view->load_progress = 100;
        eng_mutex_lock(&job->mtx);
        job->canceled = true;
        eng_mutex_unlock(&job->mtx);
        async_job_release(job);
    }
}

bool arcr_webview_is_loading(const ArcrWebView *view) {
    return view && (view->async_job != NULL);
}

int32_t arcr_webview_get_load_progress(const ArcrWebView *view) {
    if (!view) return 100;
    if (!view->async_job) return view->load_progress;
    AsyncNavJob *job = view->async_job;
    eng_mutex_lock(&job->mtx);
    int32_t p = job->progress;
    eng_mutex_unlock(&job->mtx);
    return ARCR_CLAMP(p, 0, 100);
}

/* -------------------------------------------------------------------------
 * 11. WebView Lifecycle & Viewport Controls
 * ------------------------------------------------------------------------- */
static void free_view_document_state(ArcrWebView *view) {
    if (!view) return;

    if (view->js_vm) {
        const ArcrJsVTable *js_vt = get_js_vt(view->engine);
        if (js_vt && js_vt->destroy_vm) {
            js_vt->destroy_vm(view->js_vm);
        }
        view->js_vm = NULL;
    }

    destroy_view_video_players(view);

    if (view->display_list) {
        arcr_paint_destroy_display_list(view->display_list);
        view->display_list = NULL;
    }
    if (view->layout) {
        arcr_layout_destroy(view->layout);
        view->layout = NULL;
    }
    if (view->sheet) {
        arcr_css_destroy_stylesheet(view->sheet);
        view->sheet = NULL;
    }
    if (view->doc) {
        free_dom_anim_surfaces_recursive(view->doc->root);
        arcr_dom_document_destroy(view->doc);
        view->doc = NULL;
    }

    view->last_style_share_hits = 0u;
    view->anim_start_usec       = 0u;
    view->last_css_anim_usec    = 0u;
    view->has_css_animations    = false;
    view->has_anim_images       = false;
}

ArcrWebView *arcr_webview_create(ArcrEngine *engine, int32_t width, int32_t height,
                                 const ArcrWebViewCallbacks *callbacks) {
    if (!engine) return NULL;
    ArcrWebView *view = (ArcrWebView *)calloc(1u, sizeof(ArcrWebView));
    if (!view) return NULL;

    view->engine        = engine;
    view->width         = ARCR_MAX(width, 64);
    view->height        = ARCR_MAX(height, 64);
    view->zoom_percent  = 100;
    view->load_progress = 100;
    view->security      = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(view->url, sizeof(view->url), "about:blank");
    arcr_str_copy(view->title, sizeof(view->title), "새 탭");

    if (callbacks) {
        view->cb = *callbacks;
    }

    view->next = engine->active_views_head;
    engine->active_views_head = view;
    return view;
}

void arcr_webview_destroy(ArcrWebView *view) {
    if (!view) return;
    arcr_webview_stop_loading(view);

    if (view->engine) {
        ArcrWebView **pp = &view->engine->active_views_head;
        while (*pp) {
            if (*pp == view) {
                *pp = view->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    free_view_document_state(view);
    strbuf_free(&view->user_css);
    strbuf_free(&view->console_log);
    strbuf_free(&view->raw_html);
    free(view);
}

void arcr_webview_set_callbacks(ArcrWebView *view, const ArcrWebViewCallbacks *callbacks) {
    if (!view) return;
    if (callbacks) {
        view->cb = *callbacks;
    } else {
        memset(&view->cb, 0, sizeof(view->cb));
    }
}

void arcr_webview_resize(ArcrWebView *view, int32_t width, int32_t height) {
    if (!view) return;
    int32_t nw = ARCR_MAX(width, 64);
    int32_t nh = ARCR_MAX(height, 64);
    if (nw == view->width && nh == view->height) return;
    view->width  = nw;
    view->height = nh;
    restyle_and_relayout(view);
}

void arcr_webview_set_scroll(ArcrWebView *view, int32_t scroll_y) {
    if (!view) return;
    int32_t prev = view->scroll_y;
    view->scroll_y = scroll_y;
    clamp_view_scroll(view);
    if (view->scroll_y != prev) {
        notify_view_dirty(view);
    }
}

int32_t arcr_webview_get_scroll(const ArcrWebView *view) {
    return view ? view->scroll_y : 0;
}

void arcr_webview_set_zoom(ArcrWebView *view, int32_t zoom_percent) {
    if (!view) return;
    int32_t clamped = ARCR_CLAMP(zoom_percent, 25, 400);
    if (clamped == view->zoom_percent) return;
    view->zoom_percent = clamped;
    rebuild_layout_and_paint(view);
}

int32_t arcr_webview_get_zoom(const ArcrWebView *view) {
    return view ? view->zoom_percent : 100;
}

void arcr_webview_set_prefers_dark(ArcrWebView *view, bool prefers_dark) {
    if (!view || view->prefers_dark == prefers_dark) return;
    view->prefers_dark = prefers_dark;
    restyle_and_relayout(view);
}

/* -------------------------------------------------------------------------
 * 12. Document Loading, Site Hydrators & Diagnostic Error Page
 * ------------------------------------------------------------------------- */
static bool load_html_internal(ArcrWebView      *view,
                               const char       *base_url,
                               const char       *utf8_html,
                               size_t            html_len,
                               ArcrSecurityState sec_state,
                               const char       *sec_detail) {
    if (!view || !view->engine) return false;
    const char *safe_html = utf8_html ? utf8_html : "";
    if (!utf8_html) html_len = 0u;

    free_view_document_state(view);
    view->scroll_y              = 0;
    view->ime_comp[0]           = '\0';
    view->last_hover_href[0]    = '\0';
    view->pending_js_nav_url[0] = '\0';
    view->find_active_idx       = 0u;
    view->find_total_matches    = 0u;
    strbuf_clear(&view->console_log);
    strbuf_clear(&view->raw_html);
    (void)strbuf_append(&view->raw_html, safe_html, html_len);

    const char *eff_url = (base_url && base_url[0] != '\0') ? base_url : "about:blank";
    arcr_str_copy(view->url, sizeof(view->url), eff_url);
    view->security = sec_state;

    if (view->cb.on_url_changed) {
        view->cb.on_url_changed(view->cb.userdata, view->url);
    }
    if (view->cb.on_security_changed) {
        view->cb.on_security_changed(view->cb.userdata, view->security,
                                     sec_detail ? sec_detail : "");
    }

    ArcrDocument *doc = arcr_html_parse(&view->engine->atoms, safe_html, html_len);
    if (!doc) return false;
    view->doc = doc;

    ArcrAtom input_atom    = arcr_atom_intern_lower(&view->engine->atoms, "input", 5u);
    ArcrAtom textarea_atom = arcr_atom_intern_lower(&view->engine->atoms, "textarea", 8u);
    init_form_defaults_recursive(doc, doc->root, input_atom, textarea_atom);

    if (doc->title && doc->title[0] != '\0') {
        arcr_str_copy(view->title, sizeof(view->title), doc->title);
    } else {
        arcr_str_copy(view->title, sizeof(view->title), eff_url);
    }
    if (view->cb.on_title_changed) {
        view->cb.on_title_changed(view->cb.userdata, view->title);
    }

    /* Fetch <link rel="stylesheet">, <img src>, <script src> in parallel */
    fetch_document_subresources(view);

    /* Apply live injected user/extension CSS if present */
    if (view->user_css.len > 0u && view->user_css.data) {
        arcr_dom_append_style_block(doc, view->user_css.data, view->user_css.len);
    }

    /* Pre-script hydrator hook (<noscript> hide, Google CSS, Next.js SSR snapshot) */
    char  *ssr_snap          = NULL;
    size_t ssr_len           = 0u;
    bool   needs_nextjs_shim = false;
    arcr_hydrator_before_scripts(view, safe_html, html_len,
                                 &ssr_snap, &ssr_len, &needs_nextjs_shim);

    /* Execute <script> blocks safely via UID array (lazy-loads arcr_js_portable ONLY if needed!) */
    if (view->engine->cfg.enable_javascript && doc->has_scripts) {
        ArcrAtom script_atom = arcr_atom_intern_lower(&view->engine->atoms, "script", 6u);
        size_t max_scripts = (size_t)doc->node_count + 1u;
        uint32_t *script_uids = (uint32_t *)malloc(max_scripts * sizeof(uint32_t));
        size_t script_count = 0u;
        if (script_uids) {
            collect_executable_script_uids(doc, doc->root, script_atom,
                                           script_uids, &script_count, max_scripts);
        }

        if (script_count > 0u && ensure_view_js_vm(view)) {
            const ArcrJsVTable *js_vt = get_js_vt(view->engine);
            if (needs_nextjs_shim && js_vt && js_vt->eval_script) {
                const char *nx_shim =
                    "window.__next_f = window.__next_f || [];"
                    "window.process = window.process || {env:{NODE_ENV:'production'}};"
                    "window.global = window;";
                (void)js_vt->eval_script(view->js_vm, nx_shim, strlen(nx_shim));
            }

            run_document_scripts_safe(view, script_atom, script_uids, script_count);
            free(script_uids);
            script_uids = NULL;

            if (handle_pending_js_navigation(view, NULL, 0u)) {
                free(ssr_snap);
                return true;
            }

            if (js_vt && js_vt->dispatch_dom_event_ex) {
                bool dummy_prev = false;
                (void)js_vt->dispatch_dom_event_ex(view->js_vm, 0u, "DOMContentLoaded",
                                                   0, 0, 0, &dummy_prev);
                (void)js_vt->dispatch_dom_event_ex(view->js_vm, 0u, "load",
                                                   0, 0, 0, &dummy_prev);
            } else if (js_vt && js_vt->dispatch_dom_event) {
                (void)js_vt->dispatch_dom_event(view->js_vm, 0u, "DOMContentLoaded");
                (void)js_vt->dispatch_dom_event(view->js_vm, 0u, "load");
            }

            if (handle_pending_js_navigation(view, NULL, 0u)) {
                free(ssr_snap);
                return true;
            }

            sync_doc_title_effects(view);
            fetch_document_subresources(view);
        }
        free(script_uids);
    }

    /* Post-script hydrator (Next.js/zuzunza SSR recovery, YouTube cinema, X/Twitter card) */
    if (arcr_hydrator_after_scripts(view, safe_html, html_len, ssr_snap, ssr_len)) {
        sync_doc_title_effects(view);
    }
    free(ssr_snap);

    /* Attach <video>/<audio> media sessions on demand */
    sync_view_video_players(view);

    restyle_and_relayout(view);
    view->load_progress = 100;
    return true;
}

bool arcr_webview_load_html(ArcrWebView *view, const char *base_url,
                            const char *utf8_html, size_t html_len) {
    if (!view) return false;
    arcr_webview_stop_loading(view);

    ArcrSecurityState sec = ARCR_SEC_LOCAL_OR_NONE;
    if (base_url) {
        if (arcr_str_starts_with_nocase(base_url, "https://")) {
            sec = ARCR_SEC_HTTPS_VERIFIED;
        } else if (arcr_str_starts_with_nocase(base_url, "http://")) {
            sec = ARCR_SEC_HTTP_INSECURE;
        }
    }
    return load_html_internal(view, base_url, utf8_html, html_len, sec, "");
}

static void render_navigation_error_page(ArcrWebView      *view,
                                         const char       *failed_url,
                                         ArcrSecurityState sec_state,
                                         const char       *detail,
                                         bool              was_blocked) {
    if (!view) return;
    EngineStrBuf sb = {0};
    const char *hdr =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>연결 오류 — Waterscape</title>"
        "<style>"
        "body{margin:0;padding:40px 24px;background:#f8fafc;color:#0f172a;"
        "font-family:sans-serif;}"
        ".card{max-width:640px;margin:0 auto;background:#ffffff;"
        "border:1px solid #cbd5e1;border-radius:12px;padding:28px;}"
        "h1{margin:0 0 12px 0;font-size:20px;color:#b91c1c;}"
        "p{margin:8px 0;font-size:14px;line-height:22px;color:#334155;}"
        "code{display:block;margin-top:12px;padding:10px 12px;background:#f1f5f9;"
        "border-radius:8px;font-family:monospace;font-size:12px;color:#0f172a;}"
        "</style></head><body><div class=\"card\">";
    strbuf_append_cstr(&sb, hdr);

    if (was_blocked) {
        strbuf_append_cstr(&sb, "<h1>Waterscape Shield에 의해 차단됨</h1>");
        strbuf_append_cstr(&sb, "<p>보안/광고 필터 정책에 의해 요청이 차단되었습니다.</p>");
    } else if (sec_state == ARCR_SEC_CLOCK_SKEW_ERR) {
        strbuf_append_cstr(&sb, "<h1>시스템 시계 오차 감지 (TLS 인증서 검증 보류)</h1>");
        strbuf_append_cstr(&sb, "<p>PC의 날짜/시간(CMOS 배터리)을 확인한 뒤 다시 시도해 주세요.</p>");
    } else if (sec_state == ARCR_SEC_CERT_ERROR) {
        strbuf_append_cstr(&sb, "<h1>보안 연결(TLS) 인증서 오류</h1>");
        strbuf_append_cstr(&sb, "<p>원격 서버의 TLS 인증서를 신뢰할 수 없습니다.</p>");
    } else {
        strbuf_append_cstr(&sb, "<h1>사이트에 연결할 수 없습니다</h1>");
        strbuf_append_cstr(&sb, "<p>원격 서버 응답이 없거나 네트워크 연결에 실패했습니다.</p>");
    }

    strbuf_append_cstr(&sb, "<code>URL: ");
    hydrator_append_html_escaped(&sb, failed_url ? failed_url : "");
    if (detail && detail[0] != '\0') {
        strbuf_append_cstr(&sb, " | Detail: ");
        hydrator_append_html_escaped(&sb, detail);
    }
    strbuf_append_cstr(&sb, "</code></div></body></html>");

    if (sb.data) {
        (void)load_html_internal(view, failed_url, sb.data, sb.len, sec_state, detail);
    }
    strbuf_free(&sb);
}

static bool navigate_or_post_internal(ArcrWebView *view, const char *utf8_url,
                                      const char *method,
                                      const char *post_body, size_t post_len) {
    if (!view || !view->engine || !utf8_url || utf8_url[0] == '\0') return false;
    arcr_webview_stop_loading(view);

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    char target_url[ARCR_URL_BUF_CAP];
    if (is_fetchable_base_url(eff_base)) {
        resolve_url_with_engine(view->engine, eff_base, utf8_url, target_url, sizeof(target_url));
    } else {
        arcr_str_copy(target_url, sizeof(target_url), utf8_url);
    }

    if (arcr_str_starts_with_nocase(target_url, "about:blank")) {
        return load_html_internal(view, "about:blank", "<html><body></body></html>", 26u,
                                  ARCR_SEC_LOCAL_OR_NONE, "");
    }

    uint8_t          *bytes       = NULL;
    size_t            blen        = 0u;
    char              final_url[ARCR_URL_BUF_CAP];
    ArcrSecurityState sec         = ARCR_SEC_LOCAL_OR_NONE;
    char              detail[256] = {0};
    bool              was_blocked = false;
    int               http_status = 0;

    bool ok = engine_fetch_resource_ex(
        view->engine, target_url, view->url, method, post_body, post_len,
        false, &bytes, &blen, final_url, sizeof(final_url),
        &sec, detail, sizeof(detail), &was_blocked, &http_status);

    if (!ok || !bytes) {
        engine_free_resource(view->engine, bytes);
        render_navigation_error_page(view, target_url, sec, detail, was_blocked);
        return false;
    }

    bool loaded = load_html_internal(view, final_url[0] ? final_url : target_url,
                                     (const char *)bytes, blen, sec, detail);
    engine_free_resource(view->engine, bytes);
    return loaded;
}

bool arcr_webview_navigate_url(ArcrWebView *view, const char *utf8_url) {
    return navigate_or_post_internal(view, utf8_url, "GET", NULL, 0u);
}

bool arcr_webview_post_url(ArcrWebView *view, const char *utf8_url,
                           const char *form_urlencoded_body, size_t body_len) {
    return navigate_or_post_internal(view, utf8_url, "POST", form_urlencoded_body, body_len);
}

static bool start_async_nav_internal(ArcrWebView *view, const char *utf8_url,
                                     const char *method,
                                     const char *post_body, size_t body_len) {
    if (!view || !view->engine || !utf8_url || utf8_url[0] == '\0') return false;
    arcr_webview_stop_loading(view);

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));
    char target_url[ARCR_URL_BUF_CAP];
    if (is_fetchable_base_url(eff_base)) {
        resolve_url_with_engine(view->engine, eff_base, utf8_url, target_url, sizeof(target_url));
    } else {
        arcr_str_copy(target_url, sizeof(target_url), utf8_url);
    }

    if (arcr_str_starts_with_nocase(target_url, "about:blank")) {
        return load_html_internal(view, "about:blank", "<html><body></body></html>", 26u,
                                  ARCR_SEC_LOCAL_OR_NONE, "");
    }

    if (check_url_blocked(view->engine, target_url, view->url)) {
        render_navigation_error_page(view, target_url, ARCR_SEC_LOCAL_OR_NONE,
                                     "Blocked by URL Filter", true);
        return false;
    }

    if (!ensure_net_module(view->engine)) {
        render_navigation_error_page(view, target_url, ARCR_SEC_LOCAL_OR_NONE,
                                     "arcr_net module unavailable", false);
        return false;
    }

    AsyncNavJob *job = (AsyncNavJob *)calloc(1u, sizeof(AsyncNavJob));
    if (!job) {
        return navigate_or_post_internal(view, target_url, method, post_body, body_len);
    }

    eng_mutex_init(&job->mtx);
    job->refs             = 2; /* 1 owned by WebView, 1 owned by worker thread */
    job->progress         = 15;
    job->eng              = view->engine;
    job->net_vt           = get_net_vt(view->engine);
    job->private_browsing = view->engine->cfg.private_browsing;
    arcr_str_copy(job->req_url, sizeof(job->req_url), target_url);
    if (is_fetchable_base_url(view->url)) {
        arcr_str_copy(job->referer, sizeof(job->referer), view->url);
    }
    arcr_str_copy(job->method, sizeof(job->method), (method && method[0]) ? method : "GET");

    if (post_body && body_len > 0u) {
        job->post_body = (char *)malloc(body_len + 1u);
        if (job->post_body) {
            memcpy(job->post_body, post_body, body_len);
            job->post_body[body_len] = '\0';
            job->post_len            = body_len;
        }
    }

    view->async_job     = job;
    view->load_progress = 15;

    eng_mutex_lock(&view->engine->async_mtx);
    view->engine->active_async_workers++;
    eng_mutex_unlock(&view->engine->async_mtx);

    bool spawned = false;
#if defined(_WIN32)
    uintptr_t th = _beginthreadex(NULL, 0u, win32_async_nav_proc, job, 0u, NULL);
    if (th != 0u) {
        CloseHandle((HANDLE)th);
        spawned = true;
    }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, posix_async_nav_proc, job) == 0) {
        (void)pthread_detach(th);
        spawned = true;
    }
#endif

    if (!spawned) {
        eng_mutex_lock(&view->engine->async_mtx);
        if (view->engine->active_async_workers > 0u) {
            view->engine->active_async_workers--;
        }
        eng_mutex_unlock(&view->engine->async_mtx);

        view->async_job     = NULL;
        view->load_progress = 100;
        job->refs           = 1;
        async_job_release(job);
        return navigate_or_post_internal(view, target_url, method, post_body, body_len);
    }

    notify_view_dirty(view);
    return true;
}

bool arcr_webview_navigate_async(ArcrWebView *view, const char *utf8_url) {
    return start_async_nav_internal(view, utf8_url, "GET", NULL, 0u);
}

bool arcr_webview_post_async(ArcrWebView *view, const char *utf8_url,
                             const char *form_urlencoded_body, size_t body_len) {
    return start_async_nav_internal(view, utf8_url, "POST", form_urlencoded_body, body_len);
}
/* -------------------------------------------------------------------------
 * 13. Animation Tick (60 FPS JS + CSS @keyframes + GIF + <video>) & Inspection
 * ------------------------------------------------------------------------- */
static void step_dom_anim_gifs_recursive(ArcrDomNode *node, uint64_t now_usec,
                                         bool *io_changed, bool *io_any_anim) {
    if (!node) return;
    ArcrDomSurface *s = node->surface;
    if (s && s->anim_frame_count > 1 && s->anim_frames_argb && s->argb_pixels &&
        s->width > 0 && s->height > 0) {
        *io_any_anim = true;
        if (now_usec >= s->anim_next_tick_usec) {
            int32_t next_f = (s->anim_current_frame + 1) % s->anim_frame_count;
            s->anim_current_frame = next_f;
            size_t frame_px = (size_t)s->width * (size_t)s->height;
            memcpy(s->argb_pixels,
                   s->anim_frames_argb + (size_t)next_f * frame_px,
                   frame_px * sizeof(uint32_t));
            int32_t delay_ms = (s->anim_delays_ms && s->anim_delays_ms[next_f] >= 20)
                                   ? s->anim_delays_ms[next_f]
                                   : 80;
            s->anim_next_tick_usec = now_usec + (uint64_t)delay_ms * 1000ULL;
            *io_changed = true;
        }
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        step_dom_anim_gifs_recursive(ch, now_usec, io_changed, io_any_anim);
    }
}

bool arcr_webview_tick(ArcrWebView *view, uint64_t now_usec) {
    if (!view || !view->engine) return false;
    if (now_usec == 0u) {
        now_usec = arcr_platform_monotonic_usec();
    }

    bool any_updated = false;

    /* 1. Complete finished background async navigation on main thread */
    if (view->async_job) {
        AsyncNavJob *job = view->async_job;
        eng_mutex_lock(&job->mtx);
        bool done = job->finished;
        if (!done && job->progress < 80) {
            job->progress += 2;
        }
        eng_mutex_unlock(&job->mtx);

        if (done) {
            view->async_job     = NULL;
            view->load_progress = 100;
            if (job->net_ok && job->resp.body) {
                const char *furl = job->resp.final_url[0] ? job->resp.final_url : job->req_url;
                (void)load_html_internal(view, furl,
                                         (const char *)job->resp.body, job->resp.body_len,
                                         job->resp.security, job->resp.detail);
            } else {
                render_navigation_error_page(view, job->req_url, job->resp.security,
                                             job->resp.detail, false);
            }
            async_job_release(job);
            return true;
        }
    }

    /* 2. Advance JS timers, Promise microtasks & requestAnimationFrame(ts) */
    if (view->js_vm && view->doc) {
        const ArcrJsVTable *js_vt = get_js_vt(view->engine);
        if (js_vt && js_vt->tick_timers_and_raf) {
            bool js_mutated = false;
            (void)js_vt->tick_timers_and_raf(view->js_vm, now_usec / 1000ULL, &js_mutated);
            if (handle_pending_js_navigation(view, NULL, 0u)) {
                return true;
            }
            sync_doc_title_effects(view);
            if (js_mutated || view->doc->dom_dirty) {
                fetch_document_subresources(view);
                sync_view_video_players(view);
                restyle_and_relayout(view);
                any_updated = true;
            }
        }
    }

    /* 3. Advance CSS @keyframes at ~60 Hz using cached stylesheet (0 arena leak!) */
    if (view->doc && view->has_css_animations) {
        if (view->last_css_anim_usec == 0u ||
            (now_usec >= view->last_css_anim_usec &&
             (now_usec - view->last_css_anim_usec) >= 16000ULL)) {
            view->last_css_anim_usec = now_usec;
            restyle_cached_sheet_only(view, now_usec);
            any_updated = true;
        }
    }

    /* 4. Step multi-frame Animated GIFs in-place */
    if (view->doc && view->has_anim_images) {
        bool gif_changed = false;
        bool still_anim  = false;
        step_dom_anim_gifs_recursive(view->doc->root, now_usec, &gif_changed, &still_anim);
        view->has_anim_images = still_anim;
        if (gif_changed) {
            notify_view_dirty(view);
            any_updated = true;
        }
    }

    /* 5. Step active <video> decoders / cinema frame buffers in-place */
    if (view->doc && view->video_count > 0u) {
        const ArcrMediaVTable *mvt = get_media_vt(view->engine);
        bool vid_changed = false;
        if (mvt && mvt->player_tick_frame) {
            for (size_t i = 0u; i < view->video_count; ++i) {
                ViewVideoSlot *slot = &view->videos[i];
                if (!slot->mp || slot->node_uid == 0u) continue;
                ArcrDomNode *vn = arcr_dom_find_by_uid(view->doc->root, slot->node_uid);
                if (!vn) continue;

                const char *js_paused = arcr_dom_get_attr_cstr(view->doc, vn, "data-arcr-paused");
                if (js_paused && mvt->player_set_paused) {
                    mvt->player_set_paused(slot->mp, js_paused[0] == '1');
                    arcr_dom_remove_attr(view->doc, vn, "data-arcr-paused");
                }
                const char *js_seek = arcr_dom_get_attr_cstr(view->doc, vn, "data-arcr-seek");
                if (js_seek && mvt->player_seek) {
                    double target_s = strtod(js_seek, NULL);
                    if (target_s >= 0.0) mvt->player_seek(slot->mp, target_s);
                    arcr_dom_remove_attr(view->doc, vn, "data-arcr-seek");
                }

                ArcrDomSurface *surf = arcr_dom_ensure_surface(view->doc, vn, slot->w, slot->h);
                if (surf && surf->argb_pixels) {
                    if (mvt->player_tick_frame(slot->mp, now_usec, surf->argb_pixels,
                                               surf->width, surf->height, NULL, NULL)) {
                        vid_changed = true;
                    }
                }
            }
        }
        if (vid_changed) {
            notify_view_dirty(view);
            any_updated = true;
        }
    }

    return any_updated;
}

bool arcr_webview_has_active_animations(const ArcrWebView *view) {
    if (!view || !view->engine) return false;
    if (arcr_webview_is_loading(view)) return true;
    if (view->has_css_animations || view->has_anim_images) return true;

    if (view->js_vm) {
        const ArcrJsVTable *js_vt = get_js_vt(view->engine);
        if (js_vt && js_vt->has_pending_timers && js_vt->has_pending_timers(view->js_vm)) {
            return true;
        }
    }

    if (view->video_count > 0u) {
        const ArcrMediaVTable *mvt = get_media_vt(view->engine);
        for (size_t i = 0u; i < view->video_count; ++i) {
            if (view->videos[i].mp) {
                if (!mvt || !mvt->player_is_paused || !mvt->player_is_paused(view->videos[i].mp)) {
                    return true;
                }
            }
        }
    }
    return false;
}

const char *arcr_webview_get_html_source(const ArcrWebView *view, size_t *out_len) {
    if (!view) {
        if (out_len) *out_len = 0u;
        return "";
    }
    if (view->raw_html.data) {
        if (out_len) *out_len = view->raw_html.len;
        return view->raw_html.data;
    }
    if (out_len) *out_len = 0u;
    return "";
}

static bool is_editable_element(const ArcrDocument *doc, const ArcrDomNode *el) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    if (arcr_dom_get_attr_cstr(doc, el, "disabled") != NULL ||
        arcr_dom_get_attr_cstr(doc, el, "readonly") != NULL) {
        return false;
    }
    const char *tag = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_str_eq_nocase(tag, "textarea")) return true;
    if (arcr_str_eq_nocase(tag, "input")) {
        const char *tp = arcr_dom_get_attr_cstr(doc, el, "type");
        if (!tp || tp[0] == '\0') return true;
        if (arcr_str_eq_nocase(tp, "submit") ||
            arcr_str_eq_nocase(tp, "button") ||
            arcr_str_eq_nocase(tp, "reset") ||
            arcr_str_eq_nocase(tp, "checkbox") ||
            arcr_str_eq_nocase(tp, "radio") ||
            arcr_str_eq_nocase(tp, "hidden") ||
            arcr_str_eq_nocase(tp, "image") ||
            arcr_str_eq_nocase(tp, "file")) {
            return false;
        }
        return true;
    }
    const char *ce = arcr_dom_get_attr_cstr(doc, el, "contenteditable");
    return ce && (ce[0] == '\0' || arcr_str_eq_nocase(ce, "true"));
}

void arcr_webview_query_context_at(const ArcrWebView *view, int32_t x, int32_t y,
                                   char *out_link_url, size_t link_cap,
                                   char *out_media_url, size_t media_cap,
                                   bool *out_is_editable) {
    if (out_link_url && link_cap > 0u)   out_link_url[0]  = '\0';
    if (out_media_url && media_cap > 0u) out_media_url[0] = '\0';
    if (out_is_editable)                 *out_is_editable = false;
    if (!view || !view->layout || !view->doc) return;

    ArcrHitTestResult hit;
    memset(&hit, 0, sizeof(hit));
    arcr_layout_hit_test_ex(view->layout, view->doc, x, y, view->scroll_y, &hit);

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));

    if (hit.link_href && hit.link_href[0] != '\0' && out_link_url && link_cap > 0u) {
        resolve_url_with_engine(view->engine, eff_base, hit.link_href, out_link_url, link_cap);
    }

    for (const ArcrDomNode *cur = hit.hit_node; cur != NULL; cur = cur->parent) {
        if (cur->kind != ARCR_NODE_ELEMENT) continue;
        if (out_is_editable && !(*out_is_editable) && is_editable_element(view->doc, cur)) {
            *out_is_editable = true;
        }
        const char *tag = arcr_atom_cstr(view->doc->atoms, cur->tag);
        if (out_media_url && media_cap > 0u && out_media_url[0] == '\0') {
            if (arcr_str_eq_nocase(tag, "img") || arcr_str_eq_nocase(tag, "video") ||
                arcr_str_eq_nocase(tag, "audio")) {
                const char *src = arcr_dom_get_attr_cstr(view->doc, cur, "src");
                if (src && src[0] != '\0') {
                    resolve_url_with_engine(view->engine, eff_base, src, out_media_url, media_cap);
                }
            }
        }
        if (out_link_url && link_cap > 0u && out_link_url[0] == '\0' &&
            arcr_str_eq_nocase(tag, "a")) {
            const char *href = arcr_dom_get_attr_cstr(view->doc, cur, "href");
            if (href && href[0] != '\0') {
                resolve_url_with_engine(view->engine, eff_base, href, out_link_url, link_cap);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 14. Extension / Reader / DevTools Live CSS & JS Injection
 * ------------------------------------------------------------------------- */
bool arcr_webview_inject_user_css(ArcrWebView *view, const char *utf8_css, size_t css_len) {
    if (!view) return false;
    strbuf_clear(&view->user_css);
    if (utf8_css && css_len > 0u) {
        if (!strbuf_append(&view->user_css, utf8_css, css_len)) return false;
        if (view->doc) {
            arcr_dom_append_style_block(view->doc, utf8_css, css_len);
        }
    }
    if (view->doc) {
        restyle_and_relayout(view);
    }
    return true;
}

bool arcr_webview_eval_js(ArcrWebView *view, const char *utf8_js, size_t js_len) {
    if (!view || !view->doc || !utf8_js) return false;
    if (!ensure_view_js_vm(view)) return false;

    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->eval_script) return false;

    bool ok = js_vt->eval_script(view->js_vm, utf8_js, js_len);
    if (handle_pending_js_navigation(view, NULL, 0u)) {
        return ok;
    }
    sync_doc_title_effects(view);
    if (view->doc) {
        fetch_document_subresources(view);
        sync_view_video_players(view);
        restyle_and_relayout(view);
    }
    return ok;
}

const char *arcr_webview_get_console_log(const ArcrWebView *view) {
    if (!view || !view->console_log.data) return "";
    return view->console_log.data;
}

/* -------------------------------------------------------------------------
 * 15. UID-Safe Mouse, <video> Scrubber, Form Submit, Keyboard, IME & Paint
 * ------------------------------------------------------------------------- */
static const ArcrLayoutBox *find_layout_box_for_uid(const ArcrLayoutBox *box, uint32_t uid) {
    if (!box || uid == 0u) return NULL;
    if (box->dom_node && box->dom_node->node_uid == uid) return box;
    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        const ArcrLayoutBox *found = find_layout_box_for_uid(ch, uid);
        if (found) return found;
    }
    return NULL;
}

static void clear_hover_chain(ArcrDomNode *node) {
    for (ArcrDomNode *cur = node; cur != NULL; cur = cur->parent) {
        cur->state_flags &= ~(uint32_t)ARCR_STATE_HOVER;
    }
}

static void set_hover_chain(ArcrDomNode *node) {
    for (ArcrDomNode *cur = node; cur != NULL; cur = cur->parent) {
        cur->state_flags |= (uint32_t)ARCR_STATE_HOVER;
    }
}

ArcrCursorKind arcr_webview_mouse_move(ArcrWebView *view, int32_t x, int32_t y) {
    if (!view || !view->layout || !view->doc) return ARCR_CURSOR_ARROW;

    ArcrHitTestResult hit;
    memset(&hit, 0, sizeof(hit));
    arcr_layout_hit_test_ex(view->layout, view->doc, x, y, view->scroll_y, &hit);

    ArcrDomNode *new_hover = (ArcrDomNode *)hit.hit_node;
    if (new_hover != view->doc->hovered_node) {
        clear_hover_chain(view->doc->hovered_node);
        view->doc->hovered_node = new_hover;
        set_hover_chain(new_hover);
        restyle_cached_sheet_only(view, 0u);
    }

    char resolved_hover[ARCR_URL_BUF_CAP] = {0};
    if (hit.link_href && hit.link_href[0] != '\0') {
        char eff_base[ARCR_URL_BUF_CAP];
        view_effective_base_url(view, eff_base, sizeof(eff_base));
        resolve_url_with_engine(view->engine, eff_base, hit.link_href,
                                resolved_hover, sizeof(resolved_hover));
    }
    if (strcmp(resolved_hover, view->last_hover_href) != 0) {
        arcr_str_copy(view->last_hover_href, sizeof(view->last_hover_href), resolved_hover);
        if (view->cb.on_hover_link) {
            view->cb.on_hover_link(view->cb.userdata, view->last_hover_href);
        }
    }

    for (const ArcrDomNode *cur = hit.hit_node; cur != NULL; cur = cur->parent) {
        if (cur->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(view->doc->atoms, cur->tag), "video")) {
            return ARCR_CURSOR_POINTER;
        }
    }
    return hit.cursor;
}

static void url_encode_append(EngineStrBuf *sb, const char *raw) {
    static const char kHex[] = "0123456789ABCDEF";
    if (!sb || !raw) return;
    for (size_t i = 0u; raw[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)raw[i];
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (c == ' ') {
            strbuf_append_char(sb, '+');
        } else if (unreserved) {
            strbuf_append_char(sb, (char)c);
        } else {
            char esc[3];
            esc[0] = '%';
            esc[1] = kHex[(c >> 4) & 0x0Fu];
            esc[2] = kHex[c & 0x0Fu];
            strbuf_append(sb, esc, 3u);
        }
    }
}

static void append_form_kv(EngineStrBuf *sb, const char *name, const char *val) {
    if (!sb || !name || name[0] == '\0') return;
    if (sb->len > 0u) strbuf_append_char(sb, '&');
    url_encode_append(sb, name);
    strbuf_append_char(sb, '=');
    url_encode_append(sb, val ? val : "");
}

static void collect_form_fields_recursive(const ArcrDocument *doc,
                                          const ArcrDomNode  *node,
                                          const ArcrDomNode  *submitter,
                                          EngineStrBuf       *out_sb) {
    if (!doc || !node || !out_sb) return;
    if (node->kind == ARCR_NODE_ELEMENT) {
        if (arcr_dom_get_attr_cstr(doc, node, "disabled") != NULL) return;
        const char *tag  = arcr_atom_cstr(doc->atoms, node->tag);
        const char *name = arcr_dom_get_attr_cstr(doc, node, "name");

        if (arcr_str_eq_nocase(tag, "input") && name && name[0] != '\0') {
            const char *tp = arcr_dom_get_attr_cstr(doc, node, "type");
            if (!tp) tp = "text";
            if (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                arcr_str_eq_nocase(tp, "image")) {
                if (node == submitter) {
                    const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
                    append_form_kv(out_sb, name, v ? v : "");
                }
            } else if (arcr_str_eq_nocase(tp, "reset") || arcr_str_eq_nocase(tp, "file")) {
                /* Excluded from URL-encoded submission */
            } else if (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio")) {
                if ((node->state_flags & (uint32_t)ARCR_STATE_CHECKED) != 0u) {
                    const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
                    append_form_kv(out_sb, name, (v && v[0]) ? v : "on");
                }
            } else {
                const char *v = arcr_dom_get_input_value(doc, node);
                append_form_kv(out_sb, name, v ? v : "");
            }
            return;
        }

        if (arcr_str_eq_nocase(tag, "textarea") && name && name[0] != '\0') {
            const char *v = arcr_dom_get_input_value(doc, node);
            append_form_kv(out_sb, name, v ? v : "");
            return;
        }

        if (arcr_str_eq_nocase(tag, "button") && node == submitter && name && name[0] != '\0') {
            const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
            append_form_kv(out_sb, name, v ? v : "");
            return;
        }

        if (arcr_str_eq_nocase(tag, "select") && name && name[0] != '\0') {
            const ArcrDomNode *chosen = NULL;
            const ArcrDomNode *first_opt = NULL;
            for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
                if (ch->kind == ARCR_NODE_ELEMENT &&
                    arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, ch->tag), "option")) {
                    if (!first_opt) first_opt = ch;
                    if (arcr_dom_get_attr_cstr(doc, ch, "selected") != NULL) {
                        chosen = ch;
                        break;
                    }
                }
            }
            if (!chosen) chosen = first_opt;
            if (chosen) {
                const char *v = arcr_dom_get_attr_cstr(doc, chosen, "value");
                if (v) {
                    append_form_kv(out_sb, name, v);
                } else {
                    char opt_txt[256] = {0};
                    (void)arcr_dom_get_text_content(chosen, opt_txt, sizeof(opt_txt));
                    append_form_kv(out_sb, name, opt_txt);
                }
            }
            return;
        }
    }

    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_form_fields_recursive(doc, ch, submitter, out_sb);
    }
}

static ArcrDomNode *find_ancestor_by_tag(const ArcrDocument *doc, ArcrDomNode *start,
                                         const char *tag_name) {
    if (!doc || !start || !tag_name) return NULL;
    for (ArcrDomNode *cur = start; cur != NULL; cur = cur->parent) {
        if (cur->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, cur->tag), tag_name)) {
            return cur;
        }
    }
    return NULL;
}

static void uncheck_radio_siblings_recursive(const ArcrDocument *doc, ArcrDomNode *scope,
                                             const char *radio_name, ArcrDomNode *keep_node) {
    if (!doc || !scope || !radio_name || radio_name[0] == '\0') return;
    if (scope->kind == ARCR_NODE_ELEMENT && scope != keep_node &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, scope->tag), "input")) {
        const char *tp = arcr_dom_get_attr_cstr(doc, scope, "type");
        const char *nm = arcr_dom_get_attr_cstr(doc, scope, "name");
        if (tp && arcr_str_eq_nocase(tp, "radio") && nm && strcmp(nm, radio_name) == 0) {
            scope->state_flags &= ~(uint32_t)ARCR_STATE_CHECKED;
        }
    }
    for (ArcrDomNode *ch = scope->first_child; ch != NULL; ch = ch->next_sibling) {
        uncheck_radio_siblings_recursive(doc, ch, radio_name, keep_node);
    }
}

static bool submit_enclosing_form(ArcrWebView *view, uint32_t form_uid, uint32_t submitter_uid,
                                  char *out_url, size_t url_cap) {
    if (!view || !view->doc || form_uid == 0u) return false;

    /* Dispatch cancelable JS 'submit' event BEFORE reading form fields */
    bool submit_prevented = false;
    if (view->js_vm) {
        const ArcrJsVTable *js_vt = get_js_vt(view->engine);
        if (js_vt && js_vt->dispatch_dom_event_ex) {
            (void)js_vt->dispatch_dom_event_ex(view->js_vm, form_uid, "submit",
                                               0, 0, 0, &submit_prevented);
        } else if (js_vt && js_vt->dispatch_dom_event) {
            (void)js_vt->dispatch_dom_event(view->js_vm, form_uid, "submit");
        }
        if (handle_pending_js_navigation(view, out_url, url_cap)) {
            return true;
        }
        sync_doc_title_effects(view);
        if (view->doc && view->doc->dom_dirty) {
            fetch_document_subresources(view);
            sync_view_video_players(view);
            restyle_and_relayout(view);
        }
    }
    if (submit_prevented || !view->doc) return true;

    ArcrDomNode *form_el   = arcr_dom_find_by_uid(view->doc->root, form_uid);
    ArcrDomNode *submitter = (submitter_uid != 0u)
                                 ? arcr_dom_find_by_uid(view->doc->root, submitter_uid)
                                 : NULL;
    if (!form_el) return false;

    char eff_base[ARCR_URL_BUF_CAP];
    view_effective_base_url(view, eff_base, sizeof(eff_base));

    const char *raw_action = arcr_dom_get_attr_cstr(view->doc, form_el, "action");
    if (submitter) {
        const char *fa = arcr_dom_get_attr_cstr(view->doc, submitter, "formaction");
        if (fa && fa[0] != '\0') raw_action = fa;
    }

    char action_url[ARCR_URL_BUF_CAP];
    if (raw_action && raw_action[0] != '\0') {
        resolve_url_with_engine(view->engine, eff_base, raw_action, action_url, sizeof(action_url));
    } else {
        arcr_str_copy(action_url, sizeof(action_url), view->url);
    }

    const char *method = arcr_dom_get_attr_cstr(view->doc, form_el, "method");
    if (submitter) {
        const char *fm = arcr_dom_get_attr_cstr(view->doc, submitter, "formmethod");
        if (fm && fm[0] != '\0') method = fm;
    }

    EngineStrBuf payload = {0};
    collect_form_fields_recursive(view->doc, form_el, submitter, &payload);

    if (method && arcr_str_eq_nocase(method, "post")) {
        if (out_url && url_cap > 0u) {
            arcr_str_copy(out_url, url_cap, action_url);
        }
        (void)arcr_webview_post_async(view, action_url,
                                      payload.data ? payload.data : "", payload.len);
        strbuf_free(&payload);
        return true;
    }

    /* Standard GET form submission: replace query string on action_url */
    char *qmark = strchr(action_url, '?');
    char *hash  = strchr(action_url, '#');
    if (qmark) *qmark = '\0';
    if (hash)  *hash  = '\0';

    EngineStrBuf full_get = {0};
    strbuf_append_cstr(&full_get, action_url);
    if (payload.len > 0u && payload.data) {
        strbuf_append_char(&full_get, '?');
        strbuf_append(&full_get, payload.data, payload.len);
    }
    if (out_url && url_cap > 0u && full_get.data) {
        arcr_str_copy(out_url, url_cap, full_get.data);
    }

    strbuf_free(&full_get);
    strbuf_free(&payload);
    return true;
}

static ViewVideoSlot *find_video_slot_by_uid(ArcrWebView *view, uint32_t uid) {
    if (!view || uid == 0u) return NULL;
    for (size_t i = 0u; i < view->video_count; ++i) {
        if (view->videos[i].node_uid == uid) return &view->videos[i];
    }
    return NULL;
}

static void focus_dom_node(ArcrWebView *view, ArcrDomNode *target) {
    if (!view || !view->doc) return;
    if (view->doc->focused_node == target) return;
    if (view->doc->focused_node) {
        view->doc->focused_node->state_flags &= ~(uint32_t)ARCR_STATE_FOCUS;
    }
    view->doc->focused_node = target;
    view->ime_comp[0]       = '\0';
    if (target) {
        target->state_flags |= (uint32_t)ARCR_STATE_FOCUS;
    }
}

bool arcr_webview_mouse_click(ArcrWebView *view, int32_t x, int32_t y, int button,
                              char *out_navigated_or_newtab_url, size_t url_cap,
                              bool *out_open_in_new_tab) {
    if (out_navigated_or_newtab_url && url_cap > 0u) out_navigated_or_newtab_url[0] = '\0';
    if (out_open_in_new_tab) *out_open_in_new_tab = false;
    if (!view || !view->layout || !view->doc) return false;
    if (button != 0 && button != 1) return false;

    ArcrHitTestResult hit;
    memset(&hit, 0, sizeof(hit));
    arcr_layout_hit_test_ex(view->layout, view->doc, x, y, view->scroll_y, &hit);
    if (!hit.hit_node) {
        if (view->doc->focused_node) {
            focus_dom_node(view, NULL);
            restyle_cached_sheet_only(view, 0u);
            return true;
        }
        return false;
    }

    /* Save UIDs BEFORE dispatching JS events so DOM mutations never UAF! */
    uint32_t hit_uid       = hit.hit_node->node_uid;
    uint32_t clickable_uid = hit.clickable_ancestor ? hit.clickable_ancestor->node_uid : 0u;

    ArcrDomNode *video_anc = find_ancestor_by_tag(view->doc, (ArcrDomNode *)hit.hit_node, "video");
    uint32_t     video_uid = video_anc ? video_anc->node_uid : 0u;

    ArcrDomNode *link_anc  = find_ancestor_by_tag(view->doc, (ArcrDomNode *)hit.hit_node, "a");
    uint32_t     link_uid  = link_anc ? link_anc->node_uid : 0u;

    /* Update :focus state on left click */
    bool focus_changed = false;
    if (button == 0) {
        ArcrDomNode *focus_cand = NULL;
        for (ArcrDomNode *cur = (ArcrDomNode *)hit.hit_node; cur != NULL; cur = cur->parent) {
            if (cur->kind != ARCR_NODE_ELEMENT) continue;
            const char *t = arcr_atom_cstr(view->doc->atoms, cur->tag);
            if (is_editable_element(view->doc, cur) ||
                arcr_str_eq_nocase(t, "button") ||
                arcr_str_eq_nocase(t, "input") ||
                arcr_str_eq_nocase(t, "select") ||
                arcr_str_eq_nocase(t, "a") ||
                arcr_dom_get_attr_cstr(view->doc, cur, "tabindex") != NULL) {
                focus_cand = cur;
                break;
            }
        }
        if (focus_cand != view->doc->focused_node) {
            focus_dom_node(view, focus_cand);
            focus_changed = true;
        }
    }

    /* Interactive <video> play/pause & bottom 28px timeline scrubber seek */
    if (button == 0 && video_uid != 0u) {
        ViewVideoSlot *vslot = find_video_slot_by_uid(view, video_uid);
        const ArcrMediaVTable *mvt = get_media_vt(view->engine);
        if (vslot && vslot->mp && mvt) {
            const ArcrLayoutBox *vbox = find_layout_box_for_uid(view->layout->root_box, video_uid);
            int32_t doc_y = y + view->scroll_y;
            if (vbox && vbox->border_rect.width > 24 && vbox->border_rect.height > 36) {
                int32_t vy = (vbox->is_fixed ? y : doc_y) - vbox->border_rect.y;
                int32_t vx = x - vbox->border_rect.x;
                if (vy >= vbox->border_rect.height - 28 && mvt->player_seek && mvt->player_get_duration) {
                    double dur = mvt->player_get_duration(vslot->mp);
                    if (dur <= 0.1) dur = 180.0;
                    int32_t bar_w = ARCR_MAX(1, vbox->border_rect.width - 28);
                    double ratio  = (double)ARCR_CLAMP(vx - 14, 0, bar_w) / (double)bar_w;
                    mvt->player_seek(vslot->mp, ratio * dur);
                } else if (mvt->player_is_paused && mvt->player_set_paused) {
                    bool paused = mvt->player_is_paused(vslot->mp);
                    mvt->player_set_paused(vslot->mp, !paused);
                }
            } else if (mvt->player_is_paused && mvt->player_set_paused) {
                mvt->player_set_paused(vslot->mp, !mvt->player_is_paused(vslot->mp));
            }

            ArcrDomNode *vn = arcr_dom_find_by_uid(view->doc->root, video_uid);
            if (vn && vn->surface && vn->surface->argb_pixels && mvt->player_tick_frame) {
                (void)mvt->player_tick_frame(vslot->mp, arcr_platform_monotonic_usec(),
                                             vn->surface->argb_pixels,
                                             vn->surface->width, vn->surface->height, NULL, NULL);
            }
            notify_view_dirty(view);
        }
    }

    /* Pre-toggle checkbox/radio state before JS click (revert if preventDefault) */
    uint32_t toggled_chk_uid = 0u;
    uint32_t prev_chk_flags  = 0u;
    if (button == 0) {
        ArcrDomNode *act = clickable_uid ? arcr_dom_find_by_uid(view->doc->root, clickable_uid)
                                         : arcr_dom_find_by_uid(view->doc->root, hit_uid);
        if (act && act->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(view->doc->atoms, act->tag), "label")) {
            const char *for_id = arcr_dom_get_attr_cstr(view->doc, act, "for");
            if (for_id && for_id[0] != '\0') {
                ArcrDomNode *linked = arcr_dom_get_element_by_id(view->doc, for_id);
                if (linked) {
                    act = linked;
                    clickable_uid = linked->node_uid;
                    focus_dom_node(view, linked);
                    focus_changed = true;
                }
            } else {
                ArcrDomNode *inner_inp = NULL;
                if (arcr_dom_query_selector_all(view->doc, act, "input", &inner_inp, 1u) > 0u && inner_inp) {
                    act = inner_inp;
                    clickable_uid = inner_inp->node_uid;
                    focus_dom_node(view, inner_inp);
                    focus_changed = true;
                }
            }
        }

        if (act && act->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(view->doc->atoms, act->tag), "input") &&
            arcr_dom_get_attr_cstr(view->doc, act, "disabled") == NULL) {
            const char *tp = arcr_dom_get_attr_cstr(view->doc, act, "type");
            if (tp && arcr_str_eq_nocase(tp, "checkbox")) {
                toggled_chk_uid = act->node_uid;
                prev_chk_flags  = act->state_flags;
                act->state_flags ^= (uint32_t)ARCR_STATE_CHECKED;
                focus_changed = true;
            } else if (tp && arcr_str_eq_nocase(tp, "radio")) {
                toggled_chk_uid = act->node_uid;
                prev_chk_flags  = act->state_flags;
                const char *rname = arcr_dom_get_attr_cstr(view->doc, act, "name");
                ArcrDomNode *fscope = find_ancestor_by_tag(view->doc, act, "form");
                uncheck_radio_siblings_recursive(view->doc, fscope ? fscope : view->doc->root,
                                                 rname, act);
                act->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
                focus_changed = true;
            }
        }
    }

    /* Dispatch JS 'click' event with coordinates & preventDefault tracking */
    bool click_prevented = false;
    if (button == 0 && view->engine->cfg.enable_javascript) {
        ArcrDomNode *hn = arcr_dom_find_by_uid(view->doc->root, hit_uid);
        bool has_inline_onclick = false;
        for (const ArcrDomNode *c = hn; c != NULL; c = c->parent) {
            if (c->kind == ARCR_NODE_ELEMENT &&
                arcr_dom_get_attr_cstr(view->doc, c, "onclick") != NULL) {
                has_inline_onclick = true;
                break;
            }
        }
        if (view->js_vm || (has_inline_onclick && ensure_view_js_vm(view))) {
            const ArcrJsVTable *js_vt = get_js_vt(view->engine);
            if (js_vt && js_vt->dispatch_dom_event_ex) {
                (void)js_vt->dispatch_dom_event_ex(view->js_vm, hit_uid, "click",
                                                   x, y, 0, &click_prevented);
            } else if (js_vt && js_vt->dispatch_dom_event) {
                (void)js_vt->dispatch_dom_event(view->js_vm, hit_uid, "click");
            }
            if (toggled_chk_uid != 0u && click_prevented && view->doc) {
                ArcrDomNode *cn = arcr_dom_find_by_uid(view->doc->root, toggled_chk_uid);
                if (cn) cn->state_flags = prev_chk_flags;
            }
            if (handle_pending_js_navigation(view, out_navigated_or_newtab_url, url_cap)) {
                return true;
            }
            sync_doc_title_effects(view);
            if (view->doc) {
                fetch_document_subresources(view);
                sync_view_video_players(view);
                restyle_and_relayout(view);
                focus_changed = false;
            }
        }
    }

    if (!view->doc) return true;
    if (focus_changed) {
        restyle_cached_sheet_only(view, 0u);
    }
    if (click_prevented) return true;

    /* Form submit / reset button activation (re-resolved safely by UID!) */
    if (button == 0) {
        ArcrDomNode *act = clickable_uid ? arcr_dom_find_by_uid(view->doc->root, clickable_uid)
                                         : arcr_dom_find_by_uid(view->doc->root, hit_uid);
        for (ArcrDomNode *cur = act; cur != NULL; cur = cur->parent) {
            if (cur->kind != ARCR_NODE_ELEMENT) continue;
            if (arcr_dom_get_attr_cstr(view->doc, cur, "disabled") != NULL) break;
            const char *tag = arcr_atom_cstr(view->doc->atoms, cur->tag);
            if (arcr_str_eq_nocase(tag, "button")) {
                const char *tp = arcr_dom_get_attr_cstr(view->doc, cur, "type");
                if (!tp || tp[0] == '\0' || arcr_str_eq_nocase(tp, "submit")) {
                    ArcrDomNode *f = find_ancestor_by_tag(view->doc, cur, "form");
                    if (f) {
                        return submit_enclosing_form(view, f->node_uid, cur->node_uid,
                                                     out_navigated_or_newtab_url, url_cap);
                    }
                }
                return true;
            }
            if (arcr_str_eq_nocase(tag, "input")) {
                const char *tp = arcr_dom_get_attr_cstr(view->doc, cur, "type");
                if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "image"))) {
                    ArcrDomNode *f = find_ancestor_by_tag(view->doc, cur, "form");
                    if (f) {
                        return submit_enclosing_form(view, f->node_uid, cur->node_uid,
                                                     out_navigated_or_newtab_url, url_cap);
                    }
                }
                return true;
            }
        }
    }

    /* Hyperlink <a href="..."> activation */
    ArcrDomNode *ln = link_uid ? arcr_dom_find_by_uid(view->doc->root, link_uid) : NULL;
    const char  *href = ln ? arcr_dom_get_attr_cstr(view->doc, ln, "href") : hit.link_href;
    if (href && href[0] != '\0') {
        if (arcr_str_starts_with_nocase(href, "javascript:")) {
            if (button == 0 && view->engine->cfg.enable_javascript) {
                const char *js_code = href + 11;
                (void)arcr_webview_eval_js(view, js_code, strlen(js_code));
            }
            return true;
        }

        if (button == 0 && href[0] == '#') {
            if (href[1] != '\0' && view->layout) {
                ArcrDomNode *anchor = arcr_dom_get_element_by_id(view->doc, href + 1);
                if (anchor) {
                    const ArcrLayoutBox *abox = find_layout_box_for_uid(view->layout->root_box,
                                                                        anchor->node_uid);
                    if (abox) {
                        arcr_webview_set_scroll(view, abox->border_rect.y);
                    }
                }
            }
            return true;
        }

        char eff_base[ARCR_URL_BUF_CAP];
        view_effective_base_url(view, eff_base, sizeof(eff_base));
        if (out_navigated_or_newtab_url && url_cap > 0u) {
            resolve_url_with_engine(view->engine, eff_base, href,
                                    out_navigated_or_newtab_url, url_cap);
        }
        if (out_open_in_new_tab) {
            const char *target = ln ? arcr_dom_get_attr_cstr(view->doc, ln, "target") : NULL;
            *out_open_in_new_tab = (button == 1) ||
                                   (target && arcr_str_eq_nocase(target, "_blank"));
        }
        return true;
    }

    return true;
}

static void collect_focusable_nodes(const ArcrDocument *doc, ArcrDomNode *node,
                                    ArcrDomNode **out_list, size_t *io_count, size_t max_cap) {
    if (!doc || !node || *io_count >= max_cap) return;
    if (node->kind == ARCR_NODE_ELEMENT &&
        arcr_dom_get_attr_cstr(doc, node, "disabled") == NULL &&
        arcr_dom_get_attr_cstr(doc, node, "hidden") == NULL) {
        const char *tag = arcr_atom_cstr(doc->atoms, node->tag);
        bool focusable = is_editable_element(doc, node) ||
                         arcr_str_eq_nocase(tag, "button") ||
                         (arcr_str_eq_nocase(tag, "a") && arcr_dom_get_attr_cstr(doc, node, "href") != NULL) ||
                         arcr_str_eq_nocase(tag, "select");
        if (!focusable && arcr_str_eq_nocase(tag, "input")) {
            const char *tp = arcr_dom_get_attr_cstr(doc, node, "type");
            focusable = !tp || !arcr_str_eq_nocase(tp, "hidden");
        }
        if (focusable) {
            out_list[(*io_count)++] = node;
        }
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_focusable_nodes(doc, ch, out_list, io_count, max_cap);
    }
}

static int dom_key_code_for_special(ArcrSpecialKey sp, const char *utf8_text) {
    switch (sp) {
        case ARCR_KEY_BACKSPACE: return 8;
        case ARCR_KEY_TAB:       return 9;
        case ARCR_KEY_ENTER:     return 13;
        case ARCR_KEY_ESCAPE:    return 27;
        case ARCR_KEY_PAGE_UP:   return 33;
        case ARCR_KEY_PAGE_DOWN: return 34;
        case ARCR_KEY_END:       return 35;
        case ARCR_KEY_HOME:      return 36;
        case ARCR_KEY_LEFT:      return 37;
        case ARCR_KEY_UP:        return 38;
        case ARCR_KEY_RIGHT:     return 39;
        case ARCR_KEY_DOWN:      return 40;
        case ARCR_KEY_DELETE:    return 46;
        default: break;
    }
    if (utf8_text && (unsigned char)utf8_text[0] < 128u && utf8_text[0] != '\0') {
        char c = utf8_text[0];
        if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 'A');
        return (int)(unsigned char)c;
    }
    return 0;
}

static bool is_single_line_search_textarea(const ArcrDocument *doc, const ArcrDomNode *el) {
    if (!doc || !el) return false;
    const char *rows = arcr_dom_get_attr_cstr(doc, el, "rows");
    if (rows && strcmp(rows, "1") == 0) return true;
    const char *name = arcr_dom_get_attr_cstr(doc, el, "name");
    if (name && (arcr_str_eq_nocase(name, "q") || arcr_str_eq_nocase(name, "search_query"))) {
        return true;
    }
    const char *role = arcr_dom_get_attr_cstr(doc, el, "role");
    return role && arcr_str_eq_nocase(role, "combobox");
}

bool arcr_webview_key_input(ArcrWebView *view, const char *utf8_text,
                            ArcrSpecialKey special_key,
                            char *out_submitted_url, size_t url_cap) {
    if (out_submitted_url && url_cap > 0u) out_submitted_url[0] = '\0';
    if (!view || !view->doc) return false;

    uint32_t focus_uid = view->doc->focused_node ? view->doc->focused_node->node_uid : 0u;

    /* Dispatch JS 'keydown' to focused node or document */
    bool key_prevented = false;
    if (view->js_vm) {
        const ArcrJsVTable *js_vt = get_js_vt(view->engine);
        int kcode = dom_key_code_for_special(special_key, utf8_text);
        if (js_vt && js_vt->dispatch_dom_event_ex) {
            (void)js_vt->dispatch_dom_event_ex(view->js_vm, focus_uid, "keydown",
                                               0, 0, kcode, &key_prevented);
        }
        if (handle_pending_js_navigation(view, out_submitted_url, url_cap)) {
            return true;
        }
        sync_doc_title_effects(view);
        if (view->doc && view->doc->dom_dirty) {
            fetch_document_subresources(view);
            sync_view_video_players(view);
            restyle_and_relayout(view);
        }
    }
    if (key_prevented || !view->doc) return true;

    /* Tab focus cycling across form & interactive controls */
    if (special_key == ARCR_KEY_TAB) {
        ArcrDomNode *focusables[ARCR_MAX_FOCUS_NODES];
        size_t fcount = 0u;
        collect_focusable_nodes(view->doc, view->doc->root, focusables,
                                &fcount, ARCR_MAX_FOCUS_NODES);
        if (fcount == 0u) return false;
        size_t next_idx = 0u;
        for (size_t i = 0u; i < fcount; ++i) {
            if (focusables[i]->node_uid == focus_uid) {
                next_idx = (i + 1u) % fcount;
                break;
            }
        }
        focus_dom_node(view, focusables[next_idx]);
        restyle_cached_sheet_only(view, 0u);
        return true;
    }

    ArcrDomNode *focused = focus_uid ? arcr_dom_find_by_uid(view->doc->root, focus_uid) : NULL;
    if (!focused) return false;

    const char *ftag = arcr_atom_cstr(view->doc->atoms, focused->tag);

    /* Space/Enter on focused button or checkbox */
    if (!is_editable_element(view->doc, focused)) {
        bool is_activate = (special_key == ARCR_KEY_ENTER) ||
                           (utf8_text && utf8_text[0] == ' ' && utf8_text[1] == '\0');
        if (is_activate) {
            if (arcr_str_eq_nocase(ftag, "button")) {
                ArcrDomNode *f = find_ancestor_by_tag(view->doc, focused, "form");
                if (f) {
                    return submit_enclosing_form(view, f->node_uid, focused->node_uid,
                                                 out_submitted_url, url_cap);
                }
                return true;
            }
            if (arcr_str_eq_nocase(ftag, "input")) {
                const char *tp = arcr_dom_get_attr_cstr(view->doc, focused, "type");
                if (tp && arcr_str_eq_nocase(tp, "checkbox")) {
                    focused->state_flags ^= (uint32_t)ARCR_STATE_CHECKED;
                    restyle_cached_sheet_only(view, 0u);
                    return true;
                }
                if (tp && arcr_str_eq_nocase(tp, "submit")) {
                    ArcrDomNode *f = find_ancestor_by_tag(view->doc, focused, "form");
                    if (f) {
                        return submit_enclosing_form(view, f->node_uid, focused->node_uid,
                                                     out_submitted_url, url_cap);
                    }
                    return true;
                }
            }
            if (special_key == ARCR_KEY_ENTER && arcr_str_eq_nocase(ftag, "a")) {
                const char *href = arcr_dom_get_attr_cstr(view->doc, focused, "href");
                if (href && href[0] != '\0' && out_submitted_url && url_cap > 0u) {
                    char eff_base[ARCR_URL_BUF_CAP];
                    view_effective_base_url(view, eff_base, sizeof(eff_base));
                    resolve_url_with_engine(view->engine, eff_base, href, out_submitted_url, url_cap);
                    return true;
                }
            }
        }
        return false;
    }

    /* Editable <input> / <textarea> handling */
    bool is_textarea = arcr_str_eq_nocase(ftag, "textarea");
    if (special_key == ARCR_KEY_ENTER) {
        if (!is_textarea || is_single_line_search_textarea(view->doc, focused)) {
            ArcrDomNode *f = find_ancestor_by_tag(view->doc, focused, "form");
            if (f) {
                return submit_enclosing_form(view, f->node_uid, 0u, out_submitted_url, url_cap);
            }
            return true;
        }
        utf8_text   = "\n";
        special_key = ARCR_KEY_NONE;
    }

    const char *cur_val = arcr_dom_get_input_value(view->doc, focused);
    if (!cur_val) cur_val = "";
    EngineStrBuf val_sb = {0};
    strbuf_append_cstr(&val_sb, cur_val);
    bool value_edited = false;

    if (special_key == ARCR_KEY_BACKSPACE) {
        if (val_sb.len > 0u && val_sb.data) {
            size_t pos = val_sb.len - 1u;
            while (pos > 0u && ((unsigned char)val_sb.data[pos] & 0xC0u) == 0x80u) {
                --pos;
            }
            val_sb.len       = pos;
            val_sb.data[pos] = '\0';
            value_edited     = true;
        } else {
            strbuf_free(&val_sb);
            return true;
        }
    } else if (special_key == ARCR_KEY_DELETE) {
        strbuf_free(&val_sb);
        return true;
    } else if (special_key == ARCR_KEY_COPY) {
        if (val_sb.data) (void)arcr_platform_clipboard_set_utf8(val_sb.data);
        strbuf_free(&val_sb);
        return true;
    } else if (special_key == ARCR_KEY_CUT) {
        if (val_sb.data) (void)arcr_platform_clipboard_set_utf8(val_sb.data);
        strbuf_clear(&val_sb);
        value_edited = true;
    } else if (special_key == ARCR_KEY_PASTE) {
        char clip[2048] = {0};
        if (arcr_platform_clipboard_get_utf8(clip, sizeof(clip)) && clip[0] != '\0') {
            strbuf_append_cstr(&val_sb, clip);
            value_edited = true;
        } else {
            strbuf_free(&val_sb);
            return true;
        }
    } else if (special_key == ARCR_KEY_SELECT_ALL ||
               special_key == ARCR_KEY_LEFT ||
               special_key == ARCR_KEY_RIGHT ||
               special_key == ARCR_KEY_HOME ||
               special_key == ARCR_KEY_END) {
        strbuf_free(&val_sb);
        return true;
    } else if (special_key == ARCR_KEY_NONE && utf8_text && utf8_text[0] != '\0') {
        strbuf_append_cstr(&val_sb, utf8_text);
        value_edited = true;
    }

    if (value_edited) {
        arcr_dom_set_input_value(view->doc, focused, val_sb.data ? val_sb.data : "");
        strbuf_free(&val_sb);

        if (view->js_vm) {
            const ArcrJsVTable *js_vt = get_js_vt(view->engine);
            if (js_vt && js_vt->dispatch_dom_event_ex) {
                bool dummy_prev = false;
                (void)js_vt->dispatch_dom_event_ex(view->js_vm, focus_uid, "input",
                                                   0, 0, 0, &dummy_prev);
            } else if (js_vt && js_vt->dispatch_dom_event) {
                (void)js_vt->dispatch_dom_event(view->js_vm, focus_uid, "input");
            }
            if (handle_pending_js_navigation(view, out_submitted_url, url_cap)) {
                return true;
            }
            sync_doc_title_effects(view);
        }

        if (view->doc && view->doc->dom_dirty) {
            fetch_document_subresources(view);
            sync_view_video_players(view);
            restyle_and_relayout(view);
        } else {
            rebuild_layout_and_paint(view);
        }
        return true;
    }

    strbuf_free(&val_sb);
    return false;
}

void arcr_webview_ime_composition(ArcrWebView *view, const char *utf8_comp_str,
                                  int32_t cursor_offset, bool is_committed) {
    (void)cursor_offset;
    if (!view || !view->doc || !utf8_comp_str) return;

    if (is_committed) {
        view->ime_comp[0] = '\0';
        if (utf8_comp_str[0] != '\0') {
            (void)arcr_webview_key_input(view, utf8_comp_str, ARCR_KEY_NONE, NULL, 0u);
        } else {
            notify_view_dirty(view);
        }
        return;
    }

    if (view->doc->focused_node && is_editable_element(view->doc, view->doc->focused_node)) {
        arcr_str_copy(view->ime_comp, sizeof(view->ime_comp), utf8_comp_str);
        notify_view_dirty(view);
    }
}

uint32_t arcr_webview_find_in_page(ArcrWebView *view, const char *utf8_query,
                                   bool advance_next, uint32_t *out_active_index) {
    if (out_active_index) *out_active_index = 0u;
    if (!view || !view->layout) return 0u;

    if (!utf8_query || utf8_query[0] == '\0') {
        view->find_query[0]      = '\0';
        view->find_active_idx    = 0u;
        view->find_total_matches = 0u;
        build_view_display_list(view, NULL);
        notify_view_dirty(view);
        return 0u;
    }

    bool same_query = arcr_str_eq_nocase(view->find_query, utf8_query);
    if (!same_query) {
        arcr_str_copy(view->find_query, sizeof(view->find_query), utf8_query);
        view->find_active_idx = 0u;
    } else if (advance_next && view->find_total_matches > 0u) {
        view->find_active_idx = (view->find_active_idx + 1u) % view->find_total_matches;
    }

    int32_t active_doc_y = -1;
    build_view_display_list(view, &active_doc_y);

    if (view->find_total_matches > 0u && active_doc_y >= 0) {
        if (active_doc_y < view->scroll_y || active_doc_y > view->scroll_y + view->height - 32) {
            view->scroll_y = active_doc_y - (view->height / 3);
            clamp_view_scroll(view);
        }
    }

    if (out_active_index) *out_active_index = view->find_active_idx;
    notify_view_dirty(view);
    return view->find_total_matches;
}

bool arcr_webview_paint(ArcrWebView *view, uint32_t *argb_pixels,
                        int32_t width, int32_t height, int32_t stride_pixels) {
    if (!view || !view->engine || !argb_pixels ||
        width <= 0 || height <= 0 || stride_pixels < width) {
        return false;
    }

    uint64_t t0 = arcr_platform_monotonic_usec();

    if (!view->display_list) {
        uint32_t bg = view->prefers_dark ? 0xFF0F172Au : 0xFFFFFFFFu;
        arcr_accel_fill_rect(view->engine->accel, argb_pixels,
                             width, height, stride_pixels,
                             (ArcrRect){0, 0, width, height}, bg);
        view->engine->tiles_rendered_last_frame = 0u;
        uint64_t t1 = arcr_platform_monotonic_usec();
        view->engine->last_paint_usec = (t1 >= t0) ? (t1 - t0) : 0u;
        return true;
    }

    uint32_t tiles = arcr_paint_rasterize_parallel(
        view->display_list,
        view->engine->accel,
        view->engine->fonts,
        view->engine->pool,
        argb_pixels,
        width,
        height,
        stride_pixels,
        (ArcrRect){0, 0, width, height},
        view->scroll_y
    );

    /* Render Korean IME composition preview pill inside focused editable control if active */
    if (view->ime_comp[0] != '\0' && view->doc && view->doc->focused_node && view->layout) {
        const ArcrLayoutBox *fbox = find_layout_box_for_uid(
            view->layout->root_box, view->doc->focused_node->node_uid);
        if (fbox) {
            int32_t vx = fbox->content_rect.x + 6;
            int32_t vy = (fbox->is_fixed ? fbox->content_rect.y
                                         : (fbox->content_rect.y - view->scroll_y)) + 2;
            if (vy >= 0 && vy + 20 <= height && vx >= 0 && vx + 36 <= width) {
                ArcrRect pill = {vx, vy, ARCR_MIN(140, width - vx - 4), 20};
                arcr_accel_fill_rounded_rect(view->engine->accel, argb_pixels,
                                             width, height, stride_pixels,
                                             pill, 4, 0xF0EFF6FFu);
                arcr_accel_stroke_rounded_rect(view->engine->accel, argb_pixels,
                                               width, height, stride_pixels,
                                               pill, 4, 1, 0xFF2563EBu);
                size_t pos = 0u, clen = strlen(view->ime_comp);
                int32_t pen_x = pill.x + 5;
                uint8_t mask[64 * 64];
                while (pos < clen && pen_x + 14 < pill.x + pill.width) {
                    uint32_t cp = 0u;
                    size_t step = arcr_utf8_decode_one(view->ime_comp + pos, clen - pos, &cp);
                    if (step == 0u) break;
                    pos += step;
                    ArcrGlyphMetrics gm;
                    if (arcr_font_rasterize_glyph(view->engine->fonts, cp, 13, false,
                                                  &gm, mask, sizeof(mask))) {
                        if (gm.width > 0 && gm.height > 0 && cp != ' ') {
                            arcr_accel_blit_glyph_alpha(view->engine->accel, argb_pixels,
                                                        width, height, stride_pixels,
                                                        pen_x + gm.bearing_x,
                                                        pill.y + 15 - gm.bearing_y,
                                                        mask, gm.width, gm.height, 0xFF1D4ED8u);
                        }
                        pen_x += gm.advance_px;
                    }
                }
            }
        }
    }

    uint64_t t1 = arcr_platform_monotonic_usec();
    view->engine->tiles_rendered_last_frame = tiles;
    view->engine->last_paint_usec           = (t1 >= t0) ? (t1 - t0) : 0u;
    return true;
}

const char *arcr_webview_get_title(const ArcrWebView *view) {
    return view ? view->title : "";
}

const char *arcr_webview_get_url(const ArcrWebView *view) {
    return view ? view->url : "";
}

ArcrSecurityState arcr_webview_get_security_state(const ArcrWebView *view) {
    return view ? view->security : ARCR_SEC_LOCAL_OR_NONE;
}

int32_t arcr_webview_get_content_height(const ArcrWebView *view) {
    if (!view || !view->layout) return view ? view->height : 0;
    return ARCR_MAX(view->layout->total_height, view->height);
}
