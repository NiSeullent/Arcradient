/*
 * Arcradient (MSHTML-Next)
 * include/arcradient_embed.h - Public Pure C Engine Embedding API
 *
 * Strictly decouples Arcradient Browser Shell (and external host apps)
 * from internal engine DOM, CSS, layout, and acceleration structures.
 */
#ifndef ARCRADIENT_EMBED_H
#define ARCRADIENT_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArcrEngine  ArcrEngine;
typedef struct ArcrWebView ArcrWebView;

typedef enum ArcrProfileMode {
    ARCR_PROFILE_MINIMUM  = 0, /* Win2000 / Low-RAM x86: CPU-first, minimal caches */
    ARCR_PROFILE_BALANCED = 1, /* Classic/Mid PC: CPU-first + verified GPU paths */
    ARCR_PROFILE_MODERN   = 2  /* Modern x64: Isolated, modern SIMD/GPU, JSC ready */
} ArcrProfileMode;

typedef enum ArcrProcessMode {
    ARCR_PROC_LEGACY   = 0, /* Single/bounded process with strict download/file guards */
    ARCR_PROC_STANDARD = 1, /* Browser shell + shared content process */
    ARCR_PROC_ISOLATED = 2  /* Site-isolated content processes */
} ArcrProcessMode;

typedef enum ArcrSecurityState {
    ARCR_SEC_LOCAL_OR_NONE = 0,
    ARCR_SEC_HTTP_INSECURE = 1,
    ARCR_SEC_HTTPS_VERIFIED = 2,
    ARCR_SEC_CERT_ERROR     = 3,
    ARCR_SEC_CLOCK_SKEW_ERR = 4 /* CMOS battery / system clock skew detected */
} ArcrSecurityState;

typedef struct ArcrRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} ArcrRect;

typedef struct ArcrEngineConfig {
    ArcrProfileMode profile;
    ArcrProcessMode process_mode;
    bool            force_cpu_rendering;
    bool            enable_javascript;
    size_t          max_cache_bytes;
    const char     *module_search_dir; /* Directory containing arcr_*.dll / libarcr_*.so */
    const char     *default_cjk_font_path;
} ArcrEngineConfig;

typedef struct ArcrEngineStats {
    uint32_t detected_cpu_flags;
    uint32_t active_simd_level;
    bool     gpu_available;
    bool     gpu_fallback_triggered;
    bool     net_module_loaded;
    bool     js_module_loaded;
    bool     media_module_loaded;
    bool     devtools_module_loaded;
    size_t   arena_allocated_bytes;
    size_t   arena_used_bytes;
    size_t   interned_atom_count;
    size_t   style_shared_hits;
    size_t   js_heap_bytes;
    uint32_t dom_node_count;
    uint32_t layout_box_count;
    uint32_t display_cmd_count;
} ArcrEngineStats;

typedef struct ArcrWebViewCallbacks {
    void *userdata;
    void (*on_title_changed)(void *userdata, const char *utf8_title);
    void (*on_url_changed)(void *userdata, const char *utf8_url);
    void (*on_security_changed)(void *userdata, ArcrSecurityState state, const char *detail);
    void (*on_hover_link)(void *userdata, const char *utf8_href);
    void (*on_dirty_rect)(void *userdata, ArcrRect dirty);
} ArcrWebViewCallbacks;

/* Engine lifecycle */
void        arcr_engine_default_config(ArcrEngineConfig *out_cfg, ArcrProfileMode profile);
ArcrEngine *arcr_engine_create(const ArcrEngineConfig *cfg);
void        arcr_engine_destroy(ArcrEngine *engine);
void        arcr_engine_get_stats(const ArcrEngine *engine, ArcrEngineStats *out_stats);
void        arcr_engine_notify_memory_pressure(ArcrEngine *engine, int pressure_level);

/* WebView lifecycle & document loading */
ArcrWebView *arcr_webview_create(ArcrEngine *engine, int32_t width, int32_t height,
                                 const ArcrWebViewCallbacks *callbacks);
void         arcr_webview_destroy(ArcrWebView *view);
void         arcr_webview_resize(ArcrWebView *view, int32_t width, int32_t height);
void         arcr_webview_set_scroll(ArcrWebView *view, int32_t scroll_y);

bool arcr_webview_load_html(ArcrWebView *view, const char *base_url,
                            const char *utf8_html, size_t html_len);
bool arcr_webview_navigate_url(ArcrWebView *view, const char *utf8_url);

/* Korean IME & keyboard input hooks */
void arcr_webview_ime_composition(ArcrWebView *view, const char *utf8_comp_str,
                                  int32_t cursor_offset, bool is_committed);

/* Renders the viewport into a caller-supplied ARGB8888 buffer (stride in pixels) */
bool arcr_webview_paint(ArcrWebView *view, uint32_t *argb_pixels,
                        int32_t width, int32_t height, int32_t stride_pixels);

const char       *arcr_webview_get_title(const ArcrWebView *view);
const char       *arcr_webview_get_url(const ArcrWebView *view);
ArcrSecurityState arcr_webview_get_security_state(const ArcrWebView *view);
int32_t           arcr_webview_get_content_height(const ArcrWebView *view);

#ifdef __cplusplus
}
#endif

#endif /* ARCRADIENT_EMBED_H */
