/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * core/arcr_engine.c - Public C11 Embedding API & Tier-4 Dynamic Module Registry
 *
 * Pure C11 & Windows 2000 SP4 compatible:
 *   - Zero VLAs, msvcrt-safe printf specifiers, strict ISO C dlsym memcpy casts.
 *   - Strict On-Demand Lazy Loading for arcr_net, arcr_js_portable, arcr_media, arcr_devtools.
 */
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

#define ARCR_STORAGE_SLOTS     64u
#define ARCR_MAX_FOCUS_NODES   256u
#define ARCR_MAX_JS_NAV_DEPTH  4u

/* -------------------------------------------------------------------------
 * 1. Internal Growable UTF-8 String Buffer
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
 * 2. Engine, Tier-4 Module Slot & WebView State Structures
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

struct ArcrEngine {
    ArcrEngineConfig  cfg;
    char              module_dir[256];
    ArcrAtomTable     atoms;
    ArcrAccelContext *accel;
    ArcrFontSystem   *fonts;
    ArcrThreadPool   *pool;
    ArcrWebView      *active_views_head;

    /* Systematized Tier-4 On-Demand Dynamic Module Registry */
    ModuleSlot        slots[WS_DYN_SLOT_COUNT];

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

    char                 url[512];
    char                 title[256];
    char                 ime_comp[128];
    char                 last_hover_href[512];
    ArcrSecurityState    security;

    EngineStrBuf         user_css;
    EngineStrBuf         console_log;

    char                 find_query[128];
    uint32_t             find_active_idx;
    uint32_t             find_total_matches;

    char                 pending_js_nav_url[512];
    uint32_t             js_nav_depth;

    ArcrDocument        *doc;
    ArcrStylesheet      *sheet;
    ArcrLayoutTree      *layout;
    ArcrDisplayList     *display_list;
    ArcrJsVm            *js_vm;
    size_t               last_style_share_hits;

    struct ArcrWebView  *next;
};

/* -------------------------------------------------------------------------
 * 3. Tier-4 Dynamic Module Registry & Lazy Loaders
 * ------------------------------------------------------------------------- */
static void init_module_registry(ArcrEngine *eng) {
    eng->slots[WS_DYN_SLOT_NET].stem         = WS_MOD_STEM_NET;
    eng->slots[WS_DYN_SLOT_NET].role_summary = "HTTP/1.1 + HTTPS TLS, Gzip, Cookie & Cache Stack";

    eng->slots[WS_DYN_SLOT_JS].stem          = WS_MOD_STEM_JS;
    eng->slots[WS_DYN_SLOT_JS].role_summary  = "ArcradientJS Bytecode VM & Live DOM Bridge";

    eng->slots[WS_DYN_SLOT_MEDIA].stem         = WS_MOD_STEM_MEDIA;
    eng->slots[WS_DYN_SLOT_MEDIA].role_summary = "Adaptive Fluid Media Quality & Decode Policy";

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
 * 4. Engine Lifecycle, Hardware Telemetry & Memory Pressure
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

    if (cfg) {
        eng->cfg = *cfg;
    } else {
        arcr_engine_default_config(&eng->cfg, ARCR_PROFILE_BALANCED);
    }
    arcr_str_copy(eng->module_dir, sizeof(eng->module_dir),
                  eng->cfg.module_search_dir ? eng->cfg.module_search_dir : ".");

    init_module_registry(eng);

    if (!arcr_atom_table_init(&eng->atoms, 512u)) {
        free(eng);
        return NULL;
    }

    uint32_t allowed_simd = 0xFFFFFFFFu;
    if (eng->cfg.profile == ARCR_PROFILE_MINIMUM) {
        /* Legacy x86 Minimum profile targets SSE1 baseline */
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
    for (size_t i = 0u; i < (size_t)WS_DYN_SLOT_COUNT; ++i) {
        if (eng->slots[i].lib) {
            arcr_platform_dlclose(eng->slots[i].lib);
            eng->slots[i].lib    = NULL;
            eng->slots[i].vtable = NULL;
        }
    }
    if (eng->pool)  arcr_threadpool_destroy(eng->pool);
    if (eng->fonts) arcr_font_system_destroy(eng->fonts);
    if (eng->accel) arcr_accel_destroy(eng->accel);
    arcr_atom_table_destroy(&eng->atoms);
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
            out_stats->arena_allocated_bytes += v->doc->arena.total_allocated;
            out_stats->arena_used_bytes      += v->doc->arena.total_used;
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
 * 5. URL Host Extraction, Local Relative URL Resolution & Shield Filter
 * ------------------------------------------------------------------------- */
static void extract_url_host(const char *url, char *out_host, size_t cap) {
    if (!out_host || cap == 0u) return;
    out_host[0] = '\0';
    if (!url || url[0] == '\0') return;

    const char *sep = strstr(url, "://");
    if (!sep) return;
    if (arcr_str_starts_with_nocase(url, "file://")) return;

    const char *host_start = sep + 3;
    const char *at = strchr(host_start, '@');
    const char *slash = strchr(host_start, '/');
    const char *qmark = strchr(host_start, '?');
    const char *hash  = strchr(host_start, '#');

    const char *host_end = host_start + strlen(host_start);
    if (slash && slash < host_end) host_end = slash;
    if (qmark && qmark < host_end) host_end = qmark;
    if (hash  && hash  < host_end) host_end = hash;
    if (at && at < host_end) host_start = at + 1;

    /* Strip optional :port (handling IPv6 brackets cleanly) */
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
           arcr_str_starts_with_nocase(s, "javascript:") ||
           arcr_str_starts_with_nocase(s, "mailto:") ||
           arcr_str_starts_with_nocase(s, "waterscape:") ||
           arcr_str_starts_with_nocase(s, "arcr:");
}

static void normalize_dot_segments(char *path) {
    if (!path || path[0] != '/') return;
    char out[512];
    size_t out_len = 0u;
    const char *p = path;

    while (*p != '\0') {
        if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            p += (p[2] == '/') ? 2 : 2;
            if (*p == '\0' && out_len == 0u) {
                out[out_len++] = '/';
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            p += (p[3] == '/') ? 3 : 3;
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
    arcr_str_copy(path, 512u, out);
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

    char trimmed[512];
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

    /* Protocol-relative //cdn.example.com/script.js */
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

    /* Query-only or Fragment-only relative reference */
    if (trimmed[0] == '?') {
        char base_clean[512];
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
        char base_clean[512];
        arcr_str_copy(base_clean, sizeof(base_clean), base_url);
        char *h = strchr(base_clean, '#');
        if (h) *h = '\0';
        arcr_str_copy(out_url, out_cap, base_clean);
        buf_append_cstr(out_url, out_cap, trimmed);
        return;
    }

    /* Extract origin (scheme://authority or file://) and base directory */
    const char *after_scheme = sep + 3;
    bool is_file = arcr_str_starts_with_nocase(base_url, "file://");
    const char *path_start = is_file ? after_scheme : strchr(after_scheme, '/');

    char origin[256];
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

    char dir_path[512];
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

    /* Split query/hash off trimmed before dot-segment normalization */
    char rel_path_only[512];
    const char *suffix = "";
    arcr_str_copy(rel_path_only, sizeof(rel_path_only), trimmed);
    for (size_t i = 0u; rel_path_only[i] != '\0'; ++i) {
        if (rel_path_only[i] == '?' || rel_path_only[i] == '#') {
            suffix = trimmed + i;
            rel_path_only[i] = '\0';
            break;
        }
    }

    char combined_path[512];
    arcr_str_copy(combined_path, sizeof(combined_path), dir_path);
    buf_append_cstr(combined_path, sizeof(combined_path), rel_path_only);
    normalize_dot_segments(combined_path);

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

static bool engine_fetch_resource(ArcrEngine *eng,
                                  const char *target_url,
                                  const char *context_page_url,
                                  const char *method,
                                  const char *post_body,
                                  size_t      post_body_len,
                                  uint8_t   **out_bytes,
                                  size_t     *out_len,
                                  char       *out_final_url,
                                  size_t      final_url_cap,
                                  ArcrSecurityState *out_sec,
                                  char       *out_detail,
                                  size_t      detail_cap,
                                  bool       *out_was_blocked) {
    if (out_bytes) *out_bytes = NULL;
    if (out_len)   *out_len   = 0u;
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
        req.method           = is_post ? "POST" : "GET";
        req.post_body        = post_body;
        req.post_body_len    = post_body_len;
        req.content_type     = is_post ? "application/x-www-form-urlencoded" : NULL;
        req.private_browsing = eng->cfg.private_browsing;
        req.bypass_cache     = is_post;

        ArcrHttpResponse resp;
        memset(&resp, 0, sizeof(resp));
        bool ok = net_vt->fetch_http(&req, &resp);
        if (out_sec) *out_sec = resp.security;
        if (out_detail && detail_cap > 0u) {
            arcr_str_copy(out_detail, detail_cap, resp.detail);
        }
        if (out_final_url && final_url_cap > 0u && resp.final_url[0] != '\0') {
            arcr_str_copy(out_final_url, final_url_cap, resp.final_url);
        }
        if (!ok || !resp.body) {
            if (resp.body && net_vt->free_resource) {
                net_vt->free_resource((char *)resp.body);
            }
            return false;
        }
        if (out_bytes) *out_bytes = resp.body;
        if (out_len)   *out_len   = resp.body_len;
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
        if (out_len)   *out_len   = len;
        return true;
    }

    return false;
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

/* -------------------------------------------------------------------------
 * 6. Style, Layout & Display List Pipeline
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

    view->layout = arcr_layout_build_zoomed(
        view->doc, view->engine->fonts, view->width, view->zoom_percent);
    build_view_display_list(view, NULL);

    uint64_t t1 = arcr_platform_monotonic_usec();
    view->engine->last_layout_usec = (t1 >= t0) ? (t1 - t0) : 0u;

    clamp_view_scroll(view);
    notify_view_dirty(view);
}

static void restyle_and_relayout(ArcrWebView *view) {
    if (!view || !view->doc) return;
    ArcrCssMediaEnv env = {
        .viewport_width  = view->width,
        .viewport_height = view->height,
        .prefers_dark    = view->prefers_dark
    };
    view->sheet = arcr_css_parse_document_styles_ex(view->doc, &env);
    arcr_css_resolve_tree(view->doc, view->sheet, &view->last_style_share_hits);
    view->doc->dom_dirty = false;
    rebuild_layout_and_paint(view);
}

/* -------------------------------------------------------------------------
 * 7. JavaScript VM Host Bridge & Live DOM Synchronization
 * ------------------------------------------------------------------------- */
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

static bool ensure_view_js_vm(ArcrWebView *view) {
    if (!view || !view->doc || !view->engine) return false;
    if (!view->engine->cfg.enable_javascript) return false;
    if (view->js_vm) return true;

    if (!ensure_js_module(view->engine)) return false;
    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->create_vm) return false;

    size_t heap_cap = (view->engine->cfg.profile == ARCR_PROFILE_MINIMUM)
                          ? (2u * 1024u * 1024u)
                          : (16u * 1024u * 1024u);
    ArcrJsHostCallbacks hcb;
    memset(&hcb, 0, sizeof(hcb));
    hcb.userdata           = view;
    hcb.doc                = view->doc;
    hcb.set_document_title = js_host_set_title;
    hcb.console_log        = js_host_console_log;
    hcb.navigate_to        = js_host_navigate_to;
    hcb.storage_get        = js_host_storage_get;
    hcb.storage_set        = js_host_storage_set;

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

    char raw_target[512];
    arcr_str_copy(raw_target, sizeof(raw_target), view->pending_js_nav_url);
    view->pending_js_nav_url[0] = '\0';

    char resolved[512];
    resolve_url_with_engine(view->engine, view->url, raw_target, resolved, sizeof(resolved));
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

static bool has_script_element_recursive(const ArcrDomNode *n, ArcrAtom script_atom) {
    if (!n) return false;
    if (n->kind == ARCR_NODE_ELEMENT && n->tag == script_atom) return true;
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        if (has_script_element_recursive(ch, script_atom)) return true;
    }
    return false;
}

static void run_scripts_in_tree(ArcrWebView *view, ArcrAtom script_atom, const ArcrDomNode *n) {
    if (!n || !view || !view->js_vm) return;
    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->eval_script) return;

    if (n->kind == ARCR_NODE_ELEMENT && n->tag == script_atom && n->text && n->text_len > 0u) {
        (void)js_vt->eval_script(view->js_vm, n->text, n->text_len);
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        run_scripts_in_tree(view, script_atom, ch);
    }
}

/* -------------------------------------------------------------------------
 * 8. WebView Lifecycle, Viewport Controls & Document Loading
 * ------------------------------------------------------------------------- */
ArcrWebView *arcr_webview_create(ArcrEngine *eng, int32_t width, int32_t height,
                                 const ArcrWebViewCallbacks *callbacks) {
    if (!eng) return NULL;
    ArcrWebView *view = (ArcrWebView *)calloc(1u, sizeof(ArcrWebView));
    if (!view) return NULL;

    view->engine       = eng;
    view->width        = (width > 64) ? width : 640;
    view->height       = (height > 64) ? height : 480;
    view->zoom_percent = 100;
    view->prefers_dark = false;
    view->security     = ARCR_SEC_LOCAL_OR_NONE;

    arcr_str_copy(view->url, sizeof(view->url), "about:blank");
    arcr_str_copy(view->title, sizeof(view->title), "새 탭");
    if (callbacks) view->cb = *callbacks;

    view->next             = eng->active_views_head;
    eng->active_views_head = view;
    return view;
}

static void free_view_document_state(ArcrWebView *view) {
    if (!view) return;
    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (view->js_vm && js_vt && js_vt->destroy_vm) {
        js_vt->destroy_vm(view->js_vm);
    }
    view->js_vm = NULL;

    arcr_paint_destroy_display_list(view->display_list);
    view->display_list = NULL;
    arcr_layout_destroy(view->layout);
    view->layout = NULL;
    arcr_dom_document_destroy(view->doc);
    view->doc                   = NULL;
    view->sheet                 = NULL;
    view->last_style_share_hits = 0u;
    view->scroll_y              = 0;
    view->ime_comp[0]           = '\0';
    view->last_hover_href[0]    = '\0';
    view->find_query[0]         = '\0';
    view->find_active_idx       = 0u;
    view->find_total_matches    = 0u;
    view->pending_js_nav_url[0] = '\0';
    strbuf_clear(&view->console_log);
}

void arcr_webview_destroy(ArcrWebView *view) {
    if (!view) return;
    ArcrEngine *eng = view->engine;
    if (eng) {
        ArcrWebView **pp = &eng->active_views_head;
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
    free(view);
}

void arcr_webview_resize(ArcrWebView *view, int32_t width, int32_t height) {
    if (!view) return;
    int32_t nw = (width > 64) ? width : 64;
    int32_t nh = (height > 64) ? height : 64;
    bool changed = (nw != view->width) || (nh != view->height);
    view->width  = nw;
    view->height = nh;
    if (changed && view->doc) {
        restyle_and_relayout(view);
    }
}

void arcr_webview_set_scroll(ArcrWebView *view, int32_t scroll_y) {
    if (!view) return;
    view->scroll_y = scroll_y;
    clamp_view_scroll(view);
    /* Display list is reused directly on scroll without re-running layout! */
    notify_view_dirty(view);
}

int32_t arcr_webview_get_scroll(const ArcrWebView *view) {
    return view ? view->scroll_y : 0;
}

void arcr_webview_set_zoom(ArcrWebView *view, int32_t zoom_percent) {
    if (!view) return;
    int32_t clamped = ARCR_CLAMP(zoom_percent, 50, 200);
    if (clamped != view->zoom_percent) {
        view->zoom_percent = clamped;
        if (view->doc) {
            rebuild_layout_and_paint(view);
        }
    }
}

int32_t arcr_webview_get_zoom(const ArcrWebView *view) {
    return view ? view->zoom_percent : 100;
}

void arcr_webview_set_prefers_dark(ArcrWebView *view, bool prefers_dark) {
    if (!view) return;
    if (view->prefers_dark != prefers_dark) {
        view->prefers_dark = prefers_dark;
        if (view->doc) {
            restyle_and_relayout(view);
        }
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

static void fetch_document_subresources(ArcrWebView *view) {
    if (!view || !view->doc || !view->engine) return;
    ArcrDocument *doc = view->doc;
    ArcrEngine   *eng = view->engine;

    /* 1. Handle inline data: image URIs without touching arcr_net! */
    for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
        if (r->href_or_src && arcr_str_starts_with_nocase(r->href_or_src, "data:")) {
            if (r->owner_node && !r->owner_node->surface) {
                ArcrImage img;
                memset(&img, 0, sizeof(img));
                if (arcr_image_decode_data_uri(r->href_or_src, strlen(r->href_or_src), &img)) {
                    attach_decoded_image(doc, r->owner_node, &img);
                    arcr_image_free(&img);
                }
            }
        }
    }

    bool has_ext = (doc->ext_styles_head != NULL) ||
                   (doc->ext_scripts_head != NULL && eng->cfg.enable_javascript) ||
                   (doc->ext_images_head != NULL);
    if (!has_ext || !is_fetchable_base_url(view->url)) {
        return;
    }

    /* Check if at least one non-data external reference exists before loading arcr_net */
    bool needs_net = (doc->ext_styles_head != NULL) ||
                     (doc->ext_scripts_head != NULL && eng->cfg.enable_javascript);
    if (!needs_net) {
        for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
            if (r->href_or_src && !arcr_str_starts_with_nocase(r->href_or_src, "data:")) {
                needs_net = true;
                break;
            }
        }
    }
    if (!needs_net) return;

    /* Ensure arcr_net (even if missing in offline tests, URL filter still runs below) */
    (void)ensure_net_module(eng);

    /* 2. External <link rel="stylesheet" href="..."> */
    for (ArcrExternalResourceRef *r = doc->ext_styles_head; r != NULL; r = r->next) {
        if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
        char resolved[512];
        resolve_url_with_engine(eng, view->url, r->href_or_src, resolved, sizeof(resolved));
        uint8_t *bytes = NULL;
        size_t   blen  = 0u;
        if (engine_fetch_resource(eng, resolved, view->url, "GET", NULL, 0u,
                                  &bytes, &blen, NULL, 0u, NULL, NULL, 0u, NULL)) {
            if (bytes && blen > 0u) {
                arcr_dom_append_style_block(doc, (const char *)bytes, blen);
            }
            engine_free_resource(eng, bytes);
        }
    }

    /* 3. External <img src="..."> */
    for (ArcrExternalResourceRef *r = doc->ext_images_head; r != NULL; r = r->next) {
        if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
        if (arcr_str_starts_with_nocase(r->href_or_src, "data:")) continue;
        char resolved[512];
        resolve_url_with_engine(eng, view->url, r->href_or_src, resolved, sizeof(resolved));
        uint8_t *bytes = NULL;
        size_t   blen  = 0u;
        if (engine_fetch_resource(eng, resolved, view->url, "GET", NULL, 0u,
                                  &bytes, &blen, NULL, 0u, NULL, NULL, 0u, NULL)) {
            if (bytes && blen > 0u && r->owner_node) {
                ArcrImage img;
                memset(&img, 0, sizeof(img));
                if (arcr_image_decode_mem(bytes, blen, &img)) {
                    attach_decoded_image(doc, r->owner_node, &img);
                    arcr_image_free(&img);
                }
            }
            engine_free_resource(eng, bytes);
        }
    }

    /* 4. External <script src="..."> (attached onto owner <script> node for document-order eval) */
    if (eng->cfg.enable_javascript) {
        for (ArcrExternalResourceRef *r = doc->ext_scripts_head; r != NULL; r = r->next) {
            if (!r->href_or_src || r->href_or_src[0] == '\0') continue;
            char resolved[512];
            resolve_url_with_engine(eng, view->url, r->href_or_src, resolved, sizeof(resolved));
            uint8_t *bytes = NULL;
            size_t   blen  = 0u;
            if (engine_fetch_resource(eng, resolved, view->url, "GET", NULL, 0u,
                                      &bytes, &blen, NULL, 0u, NULL, NULL, 0u, NULL)) {
                if (bytes && blen > 0u && r->owner_node) {
                    r->owner_node->text     = arcr_arena_strdup(&doc->arena, (const char *)bytes, blen);
                    r->owner_node->text_len = blen;
                    doc->has_scripts        = true;
                }
                engine_free_resource(eng, bytes);
            }
        }
    }
}

bool arcr_webview_load_html(ArcrWebView *view, const char *base_url,
                            const char *utf8_html, size_t html_len) {
    if (!view || !utf8_html) return false;
    free_view_document_state(view);

    if (base_url && base_url[0] != '\0') {
        arcr_str_copy(view->url, sizeof(view->url), base_url);
        if (arcr_str_starts_with_nocase(base_url, "https://")) {
            view->security = ARCR_SEC_HTTPS_VERIFIED;
        } else if (arcr_str_starts_with_nocase(base_url, "http://")) {
            view->security = ARCR_SEC_HTTP_INSECURE;
        } else {
            view->security = ARCR_SEC_LOCAL_OR_NONE;
        }
        if (view->cb.on_url_changed) {
            view->cb.on_url_changed(view->cb.userdata, view->url);
        }
    }

    view->doc = arcr_html_parse(&view->engine->atoms, utf8_html, html_len);
    if (!view->doc) return false;

    ArcrAtom input_atom    = arcr_atom_intern_lower(&view->engine->atoms, "input", 5u);
    ArcrAtom textarea_atom = arcr_atom_intern_lower(&view->engine->atoms, "textarea", 8u);
    init_form_defaults_recursive(view->doc, view->doc->root, input_atom, textarea_atom);
    view->doc->dom_dirty = false;

    if (view->doc->title && view->doc->title[0] != '\0') {
        arcr_str_copy(view->title, sizeof(view->title), view->doc->title);
    } else {
        arcr_str_copy(view->title, sizeof(view->title), view->url);
    }
    if (view->cb.on_title_changed) {
        view->cb.on_title_changed(view->cb.userdata, view->title);
    }

    /* Fetch external <link>, <img>, <script src> when base_url is http/https/file */
    fetch_document_subresources(view);

    /* Append persistent injected user/extension CSS if present */
    if (view->user_css.len > 0u && view->user_css.data) {
        arcr_dom_append_style_block(view->doc, view->user_css.data, view->user_css.len);
    }

    /* On-demand Media module load ONLY if <video> or <audio> exists */
    if (view->doc->has_media) {
        (void)ensure_media_module(view->engine);
    }

    /* On-demand JS module load ONLY if <script> exists! (onclick loads lazily on click) */
    if (view->engine->cfg.enable_javascript) {
        ArcrAtom script_atom = arcr_atom_intern_lower(&view->engine->atoms, "script", 6u);
        if (has_script_element_recursive(view->doc->root, script_atom)) {
            if (ensure_view_js_vm(view)) {
                run_scripts_in_tree(view, script_atom, view->doc->root);
                sync_doc_title_effects(view);
                if (handle_pending_js_navigation(view, NULL, 0u)) {
                    return true;
                }
            }
        }
    }

    restyle_and_relayout(view);
    return true;
}

static bool navigate_or_post_internal(ArcrWebView *view, const char *utf8_url,
                                      const char *method,
                                      const char *body_payload, size_t payload_len) {
    if (!view || !utf8_url || utf8_url[0] == '\0') return false;

    uint8_t          *body        = NULL;
    size_t            len         = 0u;
    char              final_url[512];
    ArcrSecurityState sec         = ARCR_SEC_LOCAL_OR_NONE;
    char              detail[256] = {0};
    bool              blocked     = false;

    bool ok = engine_fetch_resource(view->engine, utf8_url, view->url,
                                    method, body_payload, payload_len,
                                    &body, &len, final_url, sizeof(final_url),
                                    &sec, detail, sizeof(detail), &blocked);
    if (blocked) return false;

    view->security = sec;
    if (view->cb.on_security_changed) {
        view->cb.on_security_changed(view->cb.userdata, sec, detail);
    }
    if (!ok || !body) {
        engine_free_resource(view->engine, body);
        return false;
    }

    const char *effective_url = (final_url[0] != '\0') ? final_url : utf8_url;
    bool loaded = arcr_webview_load_html(view, effective_url, (const char *)body, len);
    view->security = sec;
    engine_free_resource(view->engine, body);
    return loaded;
}

bool arcr_webview_navigate_url(ArcrWebView *view, const char *utf8_url) {
    return navigate_or_post_internal(view, utf8_url, "GET", NULL, 0u);
}

bool arcr_webview_post_url(ArcrWebView *view, const char *utf8_url,
                           const char *form_urlencoded_body, size_t body_len) {
    return navigate_or_post_internal(
        view, utf8_url, "POST", form_urlencoded_body ? form_urlencoded_body : "", body_len);
}

/* -------------------------------------------------------------------------
 * 9. Extension / Reader CSS Injection, Live JS Eval & Console Log
 * ------------------------------------------------------------------------- */
bool arcr_webview_inject_user_css(ArcrWebView *view, const char *utf8_css, size_t css_len) {
    if (!view || !utf8_css) return false;
    if (css_len == 0u) return true;

    if (!strbuf_append(&view->user_css, utf8_css, css_len)) return false;
    (void)strbuf_append_char(&view->user_css, '\n');

    if (view->doc) {
        arcr_dom_append_style_block(view->doc, utf8_css, css_len);
        restyle_and_relayout(view);
    }
    return true;
}

bool arcr_webview_eval_js(ArcrWebView *view, const char *utf8_js, size_t js_len) {
    if (!view || !view->doc || !utf8_js) return false;
    if (!ensure_view_js_vm(view)) return false;

    const ArcrJsVTable *js_vt = get_js_vt(view->engine);
    if (!js_vt || !js_vt->eval_script) return false;

    uint32_t prev_nodes  = view->doc->node_count;
    view->doc->dom_dirty = false;

    bool ok = js_vt->eval_script(view->js_vm, utf8_js, js_len);
    sync_doc_title_effects(view);
    if (handle_pending_js_navigation(view, NULL, 0u)) {
        return ok;
    }

    if (view->doc->dom_dirty || view->doc->node_count != prev_nodes) {
        restyle_and_relayout(view);
    }
    return ok;
}

const char *arcr_webview_get_console_log(const ArcrWebView *view) {
    if (!view || !view->console_log.data) return "";
    return view->console_log.data;
}

/* -------------------------------------------------------------------------
 * 10. Interactive Mouse, Keyboard, Forms, Korean IME & Find-in-Page
 * ------------------------------------------------------------------------- */
static ArcrDomNode *nearest_element_mutable(const ArcrDomNode *n) {
    const ArcrDomNode *cur = n;
    while (cur && cur->kind != ARCR_NODE_ELEMENT) {
        cur = cur->parent;
    }
    return (ArcrDomNode *)cur;
}

static bool is_text_editable_input(const ArcrDocument *doc, const ArcrDomNode *el) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    const char *tag = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_str_eq_nocase(tag, "textarea")) return true;
    if (!arcr_str_eq_nocase(tag, "input")) return false;

    const char *type = arcr_dom_get_attr_cstr(doc, el, "type");
    if (!type || type[0] == '\0') return true;
    return !arcr_str_eq_nocase(type, "checkbox") &&
           !arcr_str_eq_nocase(type, "radio") &&
           !arcr_str_eq_nocase(type, "button") &&
           !arcr_str_eq_nocase(type, "submit") &&
           !arcr_str_eq_nocase(type, "reset") &&
           !arcr_str_eq_nocase(type, "hidden");
}

static bool is_focusable_control(const ArcrDocument *doc, const ArcrDomNode *el) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    if (arcr_dom_get_attr_cstr(doc, el, "disabled") != NULL) return false;

    const char *tag = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_str_eq_nocase(tag, "textarea") || arcr_str_eq_nocase(tag, "button")) {
        return true;
    }
    if (arcr_str_eq_nocase(tag, "input")) {
        const char *type = arcr_dom_get_attr_cstr(doc, el, "type");
        if (type && arcr_str_eq_nocase(type, "hidden")) return false;
        return true;
    }
    return false;
}

static bool style_blocks_contain_hover(const ArcrWebView *view) {
    if (!view || !view->doc) return false;
    for (const ArcrStyleBlock *sb = view->doc->style_blocks_head; sb != NULL; sb = sb->next) {
        if (sb->css_text && strstr(sb->css_text, ":hover") != NULL) {
            return true;
        }
    }
    return false;
}

static ArcrCursorKind classify_dom_cursor(const ArcrDocument *doc, const ArcrDomNode *el,
                                          ArcrCursorKind fallback_cursor) {
    for (const ArcrDomNode *p = el; p != NULL; p = p->parent) {
        if (p->kind != ARCR_NODE_ELEMENT) continue;
        const char *tag = arcr_atom_cstr(doc->atoms, p->tag);
        if (arcr_str_eq_nocase(tag, "a") && arcr_dom_get_attr_cstr(doc, p, "href") != NULL) {
            return ARCR_CURSOR_POINTER;
        }
        if (arcr_str_eq_nocase(tag, "button") || arcr_str_eq_nocase(tag, "summary")) {
            return ARCR_CURSOR_POINTER;
        }
        if (arcr_str_eq_nocase(tag, "textarea")) {
            return ARCR_CURSOR_TEXT;
        }
        if (arcr_str_eq_nocase(tag, "input")) {
            if (is_text_editable_input(doc, p)) return ARCR_CURSOR_TEXT;
            return ARCR_CURSOR_POINTER;
        }
    }
    return fallback_cursor;
}

ArcrCursorKind arcr_webview_mouse_move(ArcrWebView *view, int32_t x, int32_t y) {
    if (!view || !view->layout || !view->doc) return ARCR_CURSOR_ARROW;

    ArcrHitTestResult hit;
    memset(&hit, 0, sizeof(hit));
    arcr_layout_hit_test(view->layout, view->doc, x, y + view->scroll_y, &hit);

    ArcrDomNode *new_hover = nearest_element_mutable(hit.hit_node);
    if (new_hover != view->doc->hovered_node) {
        bool state_dep = false;
        for (ArcrDomNode *p = view->doc->hovered_node; p != NULL; p = p->parent) {
            if (p->kind == ARCR_NODE_ELEMENT) {
                if (p->computed_style &&
                    (p->computed_style->dependency_flags & ARCR_STYLE_DEP_STATE)) {
                    state_dep = true;
                }
                p->state_flags &= ~((uint32_t)ARCR_STATE_HOVER);
            }
        }
        view->doc->hovered_node = new_hover;
        for (ArcrDomNode *p = new_hover; p != NULL; p = p->parent) {
            if (p->kind == ARCR_NODE_ELEMENT) {
                p->state_flags |= (uint32_t)ARCR_STATE_HOVER;
                if (p->computed_style &&
                    (p->computed_style->dependency_flags & ARCR_STYLE_DEP_STATE)) {
                    state_dep = true;
                }
            }
        }
        if (state_dep || style_blocks_contain_hover(view)) {
            restyle_and_relayout(view);
        }
    }

    const char *raw_href = hit.link_href;
    if (!raw_href) {
        for (const ArcrDomNode *p = new_hover; p != NULL; p = p->parent) {
            if (p->kind == ARCR_NODE_ELEMENT &&
                arcr_str_eq_nocase(arcr_atom_cstr(view->doc->atoms, p->tag), "a")) {
                raw_href = arcr_dom_get_attr_cstr(view->doc, p, "href");
                break;
            }
        }
    }

    char resolved_href[512] = {0};
    if (raw_href && raw_href[0] != '\0') {
        resolve_url_with_engine(view->engine, view->url, raw_href,
                                resolved_href, sizeof(resolved_href));
    }
    if (strcmp(resolved_href, view->last_hover_href) != 0) {
        arcr_str_copy(view->last_hover_href, sizeof(view->last_hover_href), resolved_href);
        if (view->cb.on_hover_link) {
            view->cb.on_hover_link(view->cb.userdata, view->last_hover_href);
        }
    }

    return classify_dom_cursor(view->doc, new_hover, hit.cursor);
}

static bool url_encode_append(EngineStrBuf *sb, const char *s) {
    if (!sb) return false;
    if (!s) return true;
    static const char kHex[17] = "0123456789ABCDEF";
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)s[i];
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            if (!strbuf_append_char(sb, (char)c)) return false;
        } else if (c == ' ') {
            if (!strbuf_append_char(sb, '+')) return false;
        } else {
            char enc[3];
            enc[0] = '%';
            enc[1] = kHex[(c >> 4) & 0x0Fu];
            enc[2] = kHex[c & 0x0Fu];
            if (!strbuf_append(sb, enc, 3u)) return false;
        }
    }
    return true;
}

static void collect_form_urlencoded_recursive(const ArcrDocument *doc,
                                              const ArcrDomNode *node,
                                              const ArcrDomNode *submitter,
                                              EngineStrBuf *out_q) {
    if (!doc || !node || !out_q) return;
    if (node->kind == ARCR_NODE_ELEMENT) {
        const char *tag  = arcr_atom_cstr(doc->atoms, node->tag);
        const char *name = arcr_dom_get_attr_cstr(doc, node, "name");
        bool disabled    = (arcr_dom_get_attr_cstr(doc, node, "disabled") != NULL);

        if (!disabled && name && name[0] != '\0') {
            bool        include_field = false;
            const char *val           = "";
            char        ta_fallback[1024];

            if (arcr_str_eq_nocase(tag, "input")) {
                const char *type = arcr_dom_get_attr_cstr(doc, node, "type");
                if (!type || type[0] == '\0') type = "text";

                if (arcr_str_eq_nocase(type, "checkbox") || arcr_str_eq_nocase(type, "radio")) {
                    if ((node->state_flags & (uint32_t)ARCR_STATE_CHECKED) != 0u) {
                        const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
                        val = v ? v : "on";
                        include_field = true;
                    }
                } else if (arcr_str_eq_nocase(type, "submit")) {
                    if (node == submitter) {
                        const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
                        if (v) {
                            val = v;
                            include_field = true;
                        }
                    }
                } else if (!arcr_str_eq_nocase(type, "button") &&
                           !arcr_str_eq_nocase(type, "reset")) {
                    const char *live = arcr_dom_get_input_value(doc, node);
                    if (!live) live = arcr_dom_get_attr_cstr(doc, node, "value");
                    val = live ? live : "";
                    include_field = true;
                }
            } else if (arcr_str_eq_nocase(tag, "textarea")) {
                const char *live = arcr_dom_get_input_value(doc, node);
                if (!live) {
                    (void)arcr_dom_get_text_content(node, ta_fallback, sizeof(ta_fallback));
                    live = ta_fallback;
                }
                val = live ? live : "";
                include_field = true;
            } else if (arcr_str_eq_nocase(tag, "button") && node == submitter) {
                const char *v = arcr_dom_get_attr_cstr(doc, node, "value");
                if (v) {
                    val = v;
                    include_field = true;
                }
            }

            if (include_field) {
                if (out_q->len > 0u) {
                    (void)strbuf_append_char(out_q, '&');
                }
                (void)url_encode_append(out_q, name);
                (void)strbuf_append_char(out_q, '=');
                (void)url_encode_append(out_q, val);
            }
        }
    }

    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_form_urlencoded_recursive(doc, ch, submitter, out_q);
    }
}

static ArcrDomNode *find_ancestor_by_tag(const ArcrDocument *doc, ArcrDomNode *start,
                                         const char *tag_cstr) {
    if (!doc) return NULL;
    for (ArcrDomNode *p = start; p != NULL; p = p->parent) {
        if (p->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, p->tag), tag_cstr)) {
            return p;
        }
    }
    return NULL;
}

static ArcrDomNode *find_ancestor_with_attr(const ArcrDocument *doc, ArcrDomNode *start,
                                            const char *attr_name) {
    if (!doc) return NULL;
    for (ArcrDomNode *p = start; p != NULL; p = p->parent) {
        if (p->kind == ARCR_NODE_ELEMENT &&
            arcr_dom_get_attr_cstr(doc, p, attr_name) != NULL) {
            return p;
        }
    }
    return NULL;
}

static void uncheck_radio_group_recursive(const ArcrDocument *doc, ArcrDomNode *scope,
                                          const char *radio_name) {
    if (!doc || !scope || !radio_name || radio_name[0] == '\0') return;
    if (scope->kind == ARCR_NODE_ELEMENT &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, scope->tag), "input")) {
        const char *type = arcr_dom_get_attr_cstr(doc, scope, "type");
        const char *name = arcr_dom_get_attr_cstr(doc, scope, "name");
        if (type && arcr_str_eq_nocase(type, "radio") &&
            name && strcmp(name, radio_name) == 0) {
            scope->state_flags &= ~((uint32_t)ARCR_STATE_CHECKED);
        }
    }
    for (ArcrDomNode *ch = scope->first_child; ch != NULL; ch = ch->next_sibling) {
        uncheck_radio_group_recursive(doc, ch, radio_name);
    }
}

static bool submit_enclosing_form(ArcrWebView *view, ArcrDomNode *form_el,
                                  const ArcrDomNode *submitter,
                                  char *out_url, size_t url_cap) {
    if (!view || !view->doc || !form_el) return false;

    /* Dispatch onsubmit handler if active or inline onsubmit exists */
    if (view->engine->cfg.enable_javascript &&
        (view->js_vm != NULL || arcr_dom_get_attr_cstr(view->doc, form_el, "onsubmit") != NULL)) {
        if (ensure_view_js_vm(view)) {
            const ArcrJsVTable *js_vt = get_js_vt(view->engine);
            if (js_vt && js_vt->dispatch_dom_event) {
                (void)js_vt->dispatch_dom_event(view->js_vm, form_el->node_uid, "submit");
                sync_doc_title_effects(view);
                if (view->doc->dom_dirty) {
                    restyle_and_relayout(view);
                }
            }
        }
    }

    const char *action = arcr_dom_get_attr_cstr(view->doc, form_el, "action");
    const char *method = arcr_dom_get_attr_cstr(view->doc, form_el, "method");
    bool is_post       = (method && arcr_str_eq_nocase(method, "post"));

    char action_url[512];
    if (action && action[0] != '\0') {
        resolve_url_with_engine(view->engine, view->url, action, action_url, sizeof(action_url));
    } else {
        arcr_str_copy(action_url, sizeof(action_url), view->url);
    }

    EngineStrBuf query = {0};
    (void)strbuf_reserve(&query, 128u);
    collect_form_urlencoded_recursive(view->doc, form_el, submitter, &query);
    const char *qstr = query.data ? query.data : "";

    if (is_post) {
        bool ok = arcr_webview_post_url(view, action_url, qstr, query.len);
        if (out_url && url_cap > 0u) {
            arcr_str_copy(out_url, url_cap, ok ? view->url : action_url);
        }
        strbuf_free(&query);
        return ok;
    }

    /* GET form submission: strip existing query/hash on action_url and append ?k1=v1&k2=v2 */
    char base_action[512];
    arcr_str_copy(base_action, sizeof(base_action), action_url);
    char *qmark = strchr(base_action, '?');
    char *hash  = strchr(base_action, '#');
    if (qmark) *qmark = '\0';
    if (hash)  *hash  = '\0';

    char final_get[512];
    arcr_str_copy(final_get, sizeof(final_get), base_action);
    if (query.len > 0u) {
        buf_append_cstr(final_get, sizeof(final_get), "?");
        buf_append_cstr(final_get, sizeof(final_get), qstr);
    }
    if (out_url && url_cap > 0u) {
        arcr_str_copy(out_url, url_cap, final_get);
    }
    strbuf_free(&query);
    return true;
}

static bool set_focused_element(ArcrWebView *view, ArcrDomNode *new_focus) {
    if (!view || !view->doc) return false;
    ArcrDomNode *prev = view->doc->focused_node;
    if (prev == new_focus) return false;

    if (prev) {
        prev->state_flags &= ~((uint32_t)ARCR_STATE_FOCUS);
    }
    view->doc->focused_node = new_focus;
    view->ime_comp[0]       = '\0';

    if (new_focus) {
        new_focus->state_flags |= (uint32_t)ARCR_STATE_FOCUS;
        if (is_text_editable_input(view->doc, new_focus) && !new_focus->input_value) {
            const char *tag = arcr_atom_cstr(view->doc->atoms, new_focus->tag);
            if (arcr_str_eq_nocase(tag, "textarea")) {
                char buf[1024];
                (void)arcr_dom_get_text_content(new_focus, buf, sizeof(buf));
                arcr_dom_set_input_value(view->doc, new_focus, buf);
            } else {
                const char *v = arcr_dom_get_attr_cstr(view->doc, new_focus, "value");
                arcr_dom_set_input_value(view->doc, new_focus, v ? v : "");
            }
        }
    }
    return true;
}

bool arcr_webview_mouse_click(ArcrWebView *view, int32_t x, int32_t y, int button,
                              char *out_navigated_or_newtab_url, size_t url_cap,
                              bool *out_open_in_new_tab) {
    if (out_navigated_or_newtab_url && url_cap > 0u) {
        out_navigated_or_newtab_url[0] = '\0';
    }
    if (out_open_in_new_tab) {
        *out_open_in_new_tab = false;
    }
    if (!view || !view->layout || !view->doc) return false;

    ArcrHitTestResult hit;
    memset(&hit, 0, sizeof(hit));
    arcr_layout_hit_test(view->layout, view->doc, x, y + view->scroll_y, &hit);

    ArcrDomNode *target_el = nearest_element_mutable(hit.hit_node);
    if (!target_el && hit.clickable_ancestor) {
        target_el = nearest_element_mutable(hit.clickable_ancestor);
    }

    /* 1. Find nearest focusable control (<input>, <textarea>, <button>) */
    ArcrDomNode *focus_ctrl = NULL;
    for (ArcrDomNode *p = target_el; p != NULL; p = p->parent) {
        if (is_focusable_control(view->doc, p)) {
            focus_ctrl = p;
            break;
        }
    }
    bool state_changed = set_focused_element(view, focus_ctrl);

    /* 2. Toggle <input type="checkbox"> or select <input type="radio"> */
    if (focus_ctrl && button == 0 &&
        arcr_str_eq_nocase(arcr_atom_cstr(view->doc->atoms, focus_ctrl->tag), "input")) {
        const char *type = arcr_dom_get_attr_cstr(view->doc, focus_ctrl, "type");
        if (type && arcr_str_eq_nocase(type, "checkbox")) {
            focus_ctrl->state_flags ^= (uint32_t)ARCR_STATE_CHECKED;
            state_changed = true;
        } else if (type && arcr_str_eq_nocase(type, "radio")) {
            const char *rname = arcr_dom_get_attr_cstr(view->doc, focus_ctrl, "name");
            ArcrDomNode *scope = find_ancestor_by_tag(view->doc, focus_ctrl, "form");
            uncheck_radio_group_recursive(view->doc, scope ? scope : view->doc->root, rname);
            focus_ctrl->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
            state_changed = true;
        }
    }

    /* 3. Dispatch "click" event to JS VM (lazily creating VM if onclick="" exists!) */
    if (target_el && button == 0 && view->engine->cfg.enable_javascript) {
        ArcrDomNode *onclick_el = find_ancestor_with_attr(view->doc, target_el, "onclick");
        if (view->js_vm != NULL || onclick_el != NULL) {
            if (ensure_view_js_vm(view)) {
                const ArcrJsVTable *js_vt = get_js_vt(view->engine);
                if (js_vt && js_vt->dispatch_dom_event) {
                    uint32_t prev_nodes  = view->doc->node_count;
                    view->doc->dom_dirty = false;

                    bool handled = js_vt->dispatch_dom_event(
                        view->js_vm, target_el->node_uid, "click");
                    if (!handled && onclick_el && onclick_el != target_el) {
                        (void)js_vt->dispatch_dom_event(
                            view->js_vm, onclick_el->node_uid, "click");
                    }
                    sync_doc_title_effects(view);
                    if (handle_pending_js_navigation(view, out_navigated_or_newtab_url, url_cap)) {
                        return true;
                    }
                    if (view->doc->dom_dirty || view->doc->node_count != prev_nodes) {
                        state_changed = true;
                    }
                }
            }
        }
    }

    /* 4. Check <button type="submit"> or <input type="submit"> inside <form> */
    if (focus_ctrl && button == 0) {
        const char *tag  = arcr_atom_cstr(view->doc->atoms, focus_ctrl->tag);
        const char *type = arcr_dom_get_attr_cstr(view->doc, focus_ctrl, "type");
        bool is_submit   = false;
        if (arcr_str_eq_nocase(tag, "button")) {
            is_submit = (!type || type[0] == '\0' || arcr_str_eq_nocase(type, "submit"));
        } else if (arcr_str_eq_nocase(tag, "input")) {
            is_submit = (type && arcr_str_eq_nocase(type, "submit"));
        }
        if (is_submit) {
            ArcrDomNode *form_el = find_ancestor_by_tag(view->doc, focus_ctrl, "form");
            if (form_el) {
                if (state_changed) restyle_and_relayout(view);
                return submit_enclosing_form(
                    view, form_el, focus_ctrl, out_navigated_or_newtab_url, url_cap);
            }
        }
    }

    /* 5. Check <a href="..."> link activation */
    ArcrDomNode *link_el   = find_ancestor_by_tag(view->doc, target_el, "a");
    const char  *link_href = hit.link_href;
    if (!link_href && link_el) {
        link_href = arcr_dom_get_attr_cstr(view->doc, link_el, "href");
    }

    if (link_href && link_href[0] != '\0' && (button == 0 || button == 1)) {
        if (arcr_str_starts_with_nocase(link_href, "javascript:")) {
            const char *js_src = link_href + 11;
            (void)arcr_webview_eval_js(view, js_src, strlen(js_src));
            return false;
        }

        char resolved[512];
        resolve_url_with_engine(view->engine, view->url, link_href, resolved, sizeof(resolved));

        const char *target_attr = link_el ? arcr_dom_get_attr_cstr(view->doc, link_el, "target") : NULL;
        bool open_new_tab = (button == 1) ||
                            (target_attr && arcr_str_eq_nocase(target_attr, "_blank"));

        if (link_el) {
            link_el->state_flags |= (uint32_t)ARCR_STATE_VISITED;
            state_changed = true;
        }
        if (state_changed) {
            restyle_and_relayout(view);
        }
        if (out_open_in_new_tab) {
            *out_open_in_new_tab = open_new_tab;
        }
        if (out_navigated_or_newtab_url && url_cap > 0u) {
            arcr_str_copy(out_navigated_or_newtab_url, url_cap, resolved);
        }
        return true;
    }

    if (state_changed) {
        restyle_and_relayout(view);
    }
    return false;
}

static void collect_focusables_recursive(const ArcrDocument *doc, ArcrDomNode *node,
                                         ArcrDomNode **out_list, size_t *inout_count,
                                         size_t max_cap) {
    if (!doc || !node || *inout_count >= max_cap) return;
    if (is_focusable_control(doc, node)) {
        out_list[(*inout_count)++] = node;
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_focusables_recursive(doc, ch, out_list, inout_count, max_cap);
    }
}

static void pop_last_utf8_codepoint(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len == 0u) return;
    size_t pos = len - 1u;
    while (pos > 0u && (((unsigned char)s[pos] & 0xC0u) == 0x80u)) {
        --pos;
    }
    s[pos] = '\0';
}

bool arcr_webview_key_input(ArcrWebView *view, const char *utf8_text,
                            ArcrSpecialKey special_key,
                            char *out_submitted_url, size_t url_cap) {
    if (out_submitted_url && url_cap > 0u) {
        out_submitted_url[0] = '\0';
    }
    if (!view || !view->doc) return false;

    /* TAB cycles through focusable controls in document order */
    if (special_key == ARCR_KEY_TAB) {
        ArcrDomNode *focusables[ARCR_MAX_FOCUS_NODES];
        size_t count = 0u;
        collect_focusables_recursive(view->doc, view->doc->root,
                                     focusables, &count, ARCR_MAX_FOCUS_NODES);
        if (count == 0u) return false;

        size_t next_idx = 0u;
        for (size_t i = 0u; i < count; ++i) {
            if (focusables[i] == view->doc->focused_node) {
                next_idx = (i + 1u) % count;
                break;
            }
        }
        if (set_focused_element(view, focusables[next_idx])) {
            restyle_and_relayout(view);
        }
        return true;
    }

    if (special_key == ARCR_KEY_ESCAPE) {
        if (set_focused_element(view, NULL)) {
            restyle_and_relayout(view);
            return true;
        }
        return false;
    }

    ArcrDomNode *focused = view->doc->focused_node;
    if (!focused || focused->kind != ARCR_NODE_ELEMENT) return false;

    const char *tag = arcr_atom_cstr(view->doc->atoms, focused->tag);
    bool is_textarea = arcr_str_eq_nocase(tag, "textarea");

    if (special_key == ARCR_KEY_ENTER) {
        if (is_textarea) {
            const char *cur = arcr_dom_get_input_value(view->doc, focused);
            EngineStrBuf sb = {0};
            (void)strbuf_append_cstr(&sb, cur ? cur : "");
            (void)strbuf_append_char(&sb, '\n');
            arcr_dom_set_input_value(view->doc, focused, sb.data ? sb.data : "");
            strbuf_free(&sb);
            rebuild_layout_and_paint(view);
            return true;
        }
        ArcrDomNode *form_el = find_ancestor_by_tag(view->doc, focused, "form");
        if (form_el) {
            return submit_enclosing_form(view, form_el, focused, out_submitted_url, url_cap);
        }
        return false;
    }

    if (!is_text_editable_input(view->doc, focused)) {
        /* Allow Spacebar toggle on focused checkbox */
        if (utf8_text && strcmp(utf8_text, " ") == 0 && arcr_str_eq_nocase(tag, "input")) {
            const char *type = arcr_dom_get_attr_cstr(view->doc, focused, "type");
            if (type && arcr_str_eq_nocase(type, "checkbox")) {
                focused->state_flags ^= (uint32_t)ARCR_STATE_CHECKED;
                restyle_and_relayout(view);
                return true;
            }
        }
        return false;
    }

    const char *cur = arcr_dom_get_input_value(view->doc, focused);
    if (!cur) cur = "";

    EngineStrBuf sb = {0};
    if (!strbuf_append_cstr(&sb, cur)) return false;

    bool edited = false;
    if (special_key == ARCR_KEY_BACKSPACE || special_key == ARCR_KEY_DELETE) {
        if (sb.len > 0u && sb.data) {
            pop_last_utf8_codepoint(sb.data);
            sb.len = strlen(sb.data);
            edited = true;
        }
    } else if (utf8_text && utf8_text[0] != '\0') {
        for (size_t i = 0u; utf8_text[i] != '\0'; ++i) {
            unsigned char c = (unsigned char)utf8_text[i];
            if (c >= 0x20u || (is_textarea && (c == '\n' || c == '\t'))) {
                (void)strbuf_append_char(&sb, (char)c);
                edited = true;
            }
        }
    } else if (special_key == ARCR_KEY_LEFT || special_key == ARCR_KEY_RIGHT ||
               special_key == ARCR_KEY_HOME || special_key == ARCR_KEY_END) {
        strbuf_free(&sb);
        return true;
    }

    if (edited) {
        arcr_dom_set_input_value(view->doc, focused, sb.data ? sb.data : "");
        if (view->engine->cfg.enable_javascript &&
            (view->js_vm != NULL || arcr_dom_get_attr_cstr(view->doc, focused, "oninput") != NULL)) {
            if (ensure_view_js_vm(view)) {
                const ArcrJsVTable *js_vt = get_js_vt(view->engine);
                if (js_vt && js_vt->dispatch_dom_event) {
                    (void)js_vt->dispatch_dom_event(view->js_vm, focused->node_uid, "input");
                    sync_doc_title_effects(view);
                }
            }
        }
        if (view->doc->dom_dirty) {
            restyle_and_relayout(view);
        } else {
            rebuild_layout_and_paint(view);
        }
    }

    strbuf_free(&sb);
    return edited;
}

void arcr_webview_ime_composition(ArcrWebView *view, const char *utf8_comp_str,
                                  int32_t cursor_offset, bool is_committed) {
    (void)cursor_offset;
    if (!view) return;
    if (is_committed) {
        view->ime_comp[0] = '\0';
        if (utf8_comp_str && utf8_comp_str[0] != '\0') {
            (void)arcr_webview_key_input(view, utf8_comp_str, ARCR_KEY_NONE, NULL, 0u);
        } else {
            notify_view_dirty(view);
        }
    } else {
        arcr_str_copy(view->ime_comp, sizeof(view->ime_comp),
                      utf8_comp_str ? utf8_comp_str : "");
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

    if (strcmp(view->find_query, utf8_query) != 0) {
        arcr_str_copy(view->find_query, sizeof(view->find_query), utf8_query);
        view->find_active_idx = 0u;
    } else if (advance_next && view->find_total_matches > 0u) {
        view->find_active_idx = (view->find_active_idx + 1u) % view->find_total_matches;
    }

    int32_t active_doc_y = -1;
    build_view_display_list(view, &active_doc_y);

    if (view->find_total_matches > 0u && active_doc_y >= 0) {
        if (active_doc_y < view->scroll_y ||
            active_doc_y + 32 > view->scroll_y + view->height) {
            view->scroll_y = active_doc_y - (view->height / 3);
            clamp_view_scroll(view);
        }
    }

    notify_view_dirty(view);
    if (out_active_index) {
        *out_active_index = view->find_active_idx;
    }
    return view->find_total_matches;
}

/* -------------------------------------------------------------------------
 * 11. Parallel 64x64 Tile Rasterization & View Metadata Accessors
 * ------------------------------------------------------------------------- */
bool arcr_webview_paint(ArcrWebView *view, uint32_t *argb_pixels,
                        int32_t width, int32_t height, int32_t stride_pixels) {
    if (!view || !argb_pixels || width <= 0 || height <= 0 || stride_pixels < width) {
        return false;
    }
    ArcrEngine *eng = view->engine;
    if (!eng) return false;

    uint64_t t0 = arcr_platform_monotonic_usec();
    ArcrRect vp = {0, 0, width, height};
    uint32_t tiles = arcr_paint_rasterize_parallel(
        view->display_list, eng->accel, eng->fonts, eng->pool,
        argb_pixels, width, height, stride_pixels, vp, view->scroll_y);
    uint64_t t1 = arcr_platform_monotonic_usec();

    eng->tiles_rendered_last_frame = tiles;
    eng->last_paint_usec           = (t1 >= t0) ? (t1 - t0) : 0u;
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
    return (view && view->layout) ? view->layout->total_height : 0;
}
