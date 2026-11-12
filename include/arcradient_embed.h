/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * include/arcradient_embed.h - Public Pure C Engine Embedding API
 *
 * Strictly decouples Waterscape Browser Shell (and external host apps)
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

#define WATERSCAPE_VERSION_STR "2.0.0"
#define ARCRADIENT_VERSION_STR "2.0.0"

typedef struct ArcrEngine  ArcrEngine;
typedef struct ArcrWebView ArcrWebView;

typedef enum ArcrProfileMode {
    ARCR_PROFILE_MINIMUM  = 0, /* Win2000 / Low-RAM x86: CPU-first, minimal caches */
    ARCR_PROFILE_BALANCED = 1, /* Classic/Mid PC: Multi-core SIMD + verified GPU paths */
    ARCR_PROFILE_MODERN   = 2  /* Modern x64: AVX2 + Full Multi-core Tile + D3D11/VK/GL */
} ArcrProfileMode;

typedef enum ArcrProcessMode {
    ARCR_PROC_LEGACY   = 0, /* Single/bounded process with strict download/file guards */
    ARCR_PROC_STANDARD = 1, /* Browser shell + shared content process */
    ARCR_PROC_ISOLATED = 2  /* Site-isolated content processes */
} ArcrProcessMode;

typedef enum ArcrSecurityState {
    ARCR_SEC_LOCAL_OR_NONE  = 0,
    ARCR_SEC_HTTP_INSECURE  = 1,
    ARCR_SEC_HTTPS_VERIFIED = 2,
    ARCR_SEC_CERT_ERROR     = 3,
    ARCR_SEC_CLOCK_SKEW_ERR = 4 /* CMOS battery / system clock skew detected */
} ArcrSecurityState;

typedef enum ArcrCursorKind {
    ARCR_CURSOR_ARROW   = 0,
    ARCR_CURSOR_POINTER = 1, /* Link / Button hand cursor */
    ARCR_CURSOR_TEXT    = 2  /* Input / Textarea I-beam */
} ArcrCursorKind;

typedef enum ArcrSpecialKey {
    ARCR_KEY_NONE       = 0,
    ARCR_KEY_BACKSPACE  = 1,
    ARCR_KEY_DELETE     = 2,
    ARCR_KEY_ENTER      = 3,
    ARCR_KEY_TAB        = 4,
    ARCR_KEY_LEFT       = 5,
    ARCR_KEY_RIGHT      = 6,
    ARCR_KEY_HOME       = 7,
    ARCR_KEY_END        = 8,
    ARCR_KEY_ESCAPE     = 9,
    ARCR_KEY_UP         = 10,
    ARCR_KEY_DOWN       = 11,
    ARCR_KEY_PAGE_UP    = 12,
    ARCR_KEY_PAGE_DOWN  = 13,
    ARCR_KEY_SELECT_ALL = 14,
    ARCR_KEY_COPY       = 15,
    ARCR_KEY_CUT        = 16,
    ARCR_KEY_PASTE      = 17
} ArcrSpecialKey;

typedef struct ArcrRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} ArcrRect;

typedef bool (*ArcrUrlFilterFn)(void *userdata, const char *url, const char *page_host);

typedef struct ArcrEngineConfig {
    ArcrProfileMode profile;
    ArcrProcessMode process_mode;
    bool            force_cpu_rendering;
    bool            enable_javascript;
    bool            private_browsing;  /* Ephemeral cookies/storage when true */
    uint32_t        worker_threads;    /* 0 = auto hardware core count */
    size_t          max_cache_bytes;
    const char     *module_search_dir; /* Directory containing arcr_*.dll / libarcr_*.so */
    const char     *default_cjk_font_path;
} ArcrEngineConfig;

typedef struct ArcrEngineStats {
    uint32_t detected_cpu_flags;
    uint32_t active_simd_level;
    uint32_t active_gpu_backend;
    uint32_t worker_thread_count;
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
    size_t   glyph_cache_entries;
    size_t   glyph_cache_hits;
    size_t   js_heap_bytes;
    uint32_t dom_node_count;
    uint32_t layout_box_count;
    uint32_t display_cmd_count;
    uint32_t tiles_rendered_last_frame;
    uint64_t last_layout_usec;
    uint64_t last_paint_usec;
    uint32_t blocked_requests_count;
} ArcrEngineStats;

typedef struct ArcrWebViewCallbacks {
    void *userdata;
    void (*on_title_changed)(void *userdata, const char *utf8_title);
    void (*on_url_changed)(void *userdata, const char *utf8_url);
    void (*on_security_changed)(void *userdata, ArcrSecurityState state, const char *detail);
    void (*on_hover_link)(void *userdata, const char *utf8_href);
    void (*on_dirty_rect)(void *userdata, ArcrRect dirty);
} ArcrWebViewCallbacks;

/* Engine lifecycle & Hardware Controls */
void        arcr_engine_default_config(ArcrEngineConfig *out_cfg, ArcrProfileMode profile);
ArcrEngine *arcr_engine_create(const ArcrEngineConfig *cfg);
void        arcr_engine_destroy(ArcrEngine *engine);
void        arcr_engine_get_stats(const ArcrEngine *engine, ArcrEngineStats *out_stats);
void        arcr_engine_notify_memory_pressure(ArcrEngine *engine, int pressure_level);
void        arcr_engine_set_url_filter(ArcrEngine *engine, ArcrUrlFilterFn fn, void *userdata);
const char *arcr_engine_gpu_backend_summary(const ArcrEngine *engine);

/* WebView lifecycle & document loading */
ArcrWebView *arcr_webview_create(ArcrEngine *engine, int32_t width, int32_t height,
                                 const ArcrWebViewCallbacks *callbacks);
void         arcr_webview_destroy(ArcrWebView *view);
void         arcr_webview_set_callbacks(ArcrWebView *view, const ArcrWebViewCallbacks *callbacks);
void         arcr_webview_resize(ArcrWebView *view, int32_t width, int32_t height);
void         arcr_webview_set_scroll(ArcrWebView *view, int32_t scroll_y);
int32_t      arcr_webview_get_scroll(const ArcrWebView *view);
void         arcr_webview_set_zoom(ArcrWebView *view, int32_t zoom_percent);
int32_t      arcr_webview_get_zoom(const ArcrWebView *view);
void         arcr_webview_set_prefers_dark(ArcrWebView *view, bool prefers_dark);

bool arcr_webview_load_html(ArcrWebView *view, const char *base_url,
                            const char *utf8_html, size_t html_len);
bool arcr_webview_navigate_url(ArcrWebView *view, const char *utf8_url);
bool arcr_webview_post_url(ArcrWebView *view, const char *utf8_url,
                           const char *form_urlencoded_body, size_t body_len);

/* Multi-core Non-Blocking Async Navigation, Animation Tick & Inspection */
bool    arcr_webview_navigate_async(ArcrWebView *view, const char *utf8_url);
bool    arcr_webview_post_async(ArcrWebView *view, const char *utf8_url,
                                const char *form_urlencoded_body, size_t body_len);
void    arcr_webview_stop_loading(ArcrWebView *view);
bool    arcr_webview_is_loading(const ArcrWebView *view);
int32_t arcr_webview_get_load_progress(const ArcrWebView *view);
bool    arcr_webview_tick(ArcrWebView *view, uint64_t now_usec);
bool    arcr_webview_has_active_animations(const ArcrWebView *view);
const char *arcr_webview_get_html_source(const ArcrWebView *view, size_t *out_len);
void    arcr_webview_query_context_at(const ArcrWebView *view, int32_t x, int32_t y,
                                      char *out_link_url, size_t link_cap,
                                      char *out_media_url, size_t media_cap,
                                      bool *out_is_editable);

/* Extension / Reader / DevTools live injection */
bool arcr_webview_inject_user_css(ArcrWebView *view, const char *utf8_css, size_t css_len);
bool arcr_webview_eval_js(ArcrWebView *view, const char *utf8_js, size_t js_len);
const char *arcr_webview_get_console_log(const ArcrWebView *view);

/* Interactive Mouse, Keyboard, Korean IME & Find-in-Page */
ArcrCursorKind arcr_webview_mouse_move(ArcrWebView *view, int32_t x, int32_t y);
bool           arcr_webview_mouse_click(ArcrWebView *view, int32_t x, int32_t y, int button,
                                        char *out_navigated_or_newtab_url, size_t url_cap,
                                        bool *out_open_in_new_tab);
bool           arcr_webview_key_input(ArcrWebView *view, const char *utf8_text,
                                      ArcrSpecialKey special_key,
                                      char *out_submitted_url, size_t url_cap);
void           arcr_webview_ime_composition(ArcrWebView *view, const char *utf8_comp_str,
                                            int32_t cursor_offset, bool is_committed);

uint32_t arcr_webview_find_in_page(ArcrWebView *view, const char *utf8_query,
                                   bool advance_next, uint32_t *out_active_index);

/* Renders the viewport into a caller-supplied ARGB8888 buffer (multi-core + SIMD accelerated) */
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
