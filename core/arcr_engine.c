/*
 * Arcradient (MSHTML-Next)
 * core/arcr_engine.c - Public C Embedding API Implementation & On-Demand Module Loader
 */
#include "../include/arcradient_embed.h"
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../css/arcr_css.h"
#include "../js/arcr_js.h"
#include "../layout/arcr_layout.h"
#include "../media/arcr_media.h"
#include "../net/arcr_net.h"
#include "../paint/arcr_paint.h"
#include "../platform/arcr_platform.h"
#include <stdlib.h>
#include <string.h>

struct ArcrEngine {
    ArcrEngineConfig    cfg;
    char                module_dir[256];
    ArcrAtomTable       atoms;
    ArcrAccelContext   *accel;
    ArcrFontSystem     *fonts;
    ArcrWebView        *active_views_head;

    /* On-demand dynamically loaded modules (NULL until actually needed!) */
    ArcrDynLib         *net_lib;
    const ArcrNetVTable   *net_vt;

    ArcrDynLib         *js_lib;
    const ArcrJsVTable    *js_vt;

    ArcrDynLib         *media_lib;
    const ArcrMediaVTable *media_vt;

    ArcrDynLib         *devtools_lib;
    const void         *devtools_vt;
};

struct ArcrWebView {
    ArcrEngine          *engine;
    ArcrWebViewCallbacks cb;
    int32_t              width;
    int32_t              height;
    int32_t              scroll_y;

    char                 url[512];
    char                 title[256];
    char                 ime_comp[128];
    ArcrSecurityState    security;

    ArcrDocument        *doc;
    ArcrStylesheet      *sheet;
    ArcrLayoutTree      *layout;
    ArcrDisplayList     *display_list;
    ArcrJsVm            *js_vm;
    size_t               last_style_share_hits;

    struct ArcrWebView  *next;
};

void arcr_engine_default_config(ArcrEngineConfig *out_cfg, ArcrProfileMode profile) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->profile = profile;
    if (profile == ARCR_PROFILE_MINIMUM) {
        out_cfg->process_mode        = ARCR_PROC_LEGACY;
        out_cfg->force_cpu_rendering = true;
        out_cfg->enable_javascript   = true;
        out_cfg->max_cache_bytes     = 4u * 1024u * 1024u;
    } else if (profile == ARCR_PROFILE_BALANCED) {
        out_cfg->process_mode        = ARCR_PROC_STANDARD;
        out_cfg->force_cpu_rendering = false;
        out_cfg->enable_javascript   = true;
        out_cfg->max_cache_bytes     = 16u * 1024u * 1024u;
    } else {
        out_cfg->process_mode        = ARCR_PROC_ISOLATED;
        out_cfg->force_cpu_rendering = false;
        out_cfg->enable_javascript   = true;
        out_cfg->max_cache_bytes     = 64u * 1024u * 1024u;
    }
    out_cfg->module_search_dir     = ".";
    out_cfg->default_cjk_font_path = NULL;
}

static const void *load_verified_module(ArcrEngine *eng, const char *stem, ArcrDynLib **out_lib) {
    char path[512];
    arcr_platform_format_module_path(path, sizeof(path), eng->module_dir, stem);
    ArcrDynLib *lib = arcr_platform_dlopen(path);
    if (!lib) return NULL;

    ArcrModuleQueryFn query_fn = NULL;
    void *sym = arcr_platform_dlsym(lib, "arcr_module_query");
    memcpy(&query_fn, &sym, sizeof(query_fn));
    if (!query_fn) {
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
    if (st != ARCR_MOD_OK) {
        arcr_platform_dlclose(lib);
        return NULL;
    }
    *out_lib = lib;
    return desc->vtable;
}

static bool ensure_net_module(ArcrEngine *eng) {
    if (eng->net_vt) return true;
    eng->net_vt = (const ArcrNetVTable *)load_verified_module(eng, "arcr_net", &eng->net_lib);
    return eng->net_vt != NULL;
}

static bool ensure_js_module(ArcrEngine *eng) {
    if (eng->js_vt) return true;
    eng->js_vt = (const ArcrJsVTable *)load_verified_module(eng, "arcr_js_portable", &eng->js_lib);
    return eng->js_vt != NULL;
}

static bool ensure_media_module(ArcrEngine *eng) {
    if (eng->media_vt) return true;
    eng->media_vt = (const ArcrMediaVTable *)load_verified_module(eng, "arcr_media", &eng->media_lib);
    return eng->media_vt != NULL;
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
    if (!eng->accel || !eng->fonts) {
        arcr_engine_destroy(eng);
        return NULL;
    }
    return eng;
}

void arcr_engine_destroy(ArcrEngine *eng) {
    if (!eng) return;
    while (eng->active_views_head) {
        arcr_webview_destroy(eng->active_views_head);
    }
    if (eng->js_lib)       arcr_platform_dlclose(eng->js_lib);
    if (eng->net_lib)      arcr_platform_dlclose(eng->net_lib);
    if (eng->media_lib)    arcr_platform_dlclose(eng->media_lib);
    if (eng->devtools_lib) arcr_platform_dlclose(eng->devtools_lib);
    arcr_font_system_destroy(eng->fonts);
    arcr_accel_destroy(eng->accel);
    arcr_atom_table_destroy(&eng->atoms);
    free(eng);
}

void arcr_engine_get_stats(const ArcrEngine *eng, ArcrEngineStats *out_stats) {
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!eng) return;

    out_stats->detected_cpu_flags     = arcr_detect_cpu_flags();
    out_stats->active_simd_level      = (uint32_t)arcr_accel_active_cpu_backend(eng->accel);
    out_stats->gpu_available          = arcr_accel_is_gpu_healthy(eng->accel);
    out_stats->gpu_fallback_triggered = arcr_accel_did_fallback_occur(eng->accel);
    out_stats->net_module_loaded      = (eng->net_vt != NULL);
    out_stats->js_module_loaded       = (eng->js_vt != NULL);
    out_stats->media_module_loaded    = (eng->media_vt != NULL);
    out_stats->devtools_module_loaded = (eng->devtools_vt != NULL);
    out_stats->interned_atom_count    = eng->atoms.count;
    out_stats->arena_allocated_bytes  = eng->atoms.arena.total_allocated;
    out_stats->arena_used_bytes       = eng->atoms.arena.total_used;

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
            out_stats->display_cmd_count += v->display_list->count;
        }
        if (v->js_vm && eng->js_vt) {
            out_stats->js_heap_bytes += eng->js_vt->get_used_heap_bytes(v->js_vm);
        }
        out_stats->style_shared_hits += v->last_style_share_hits;
    }
}

void arcr_engine_notify_memory_pressure(ArcrEngine *eng, int pressure_level) {
    if (!eng) return;
    for (ArcrWebView *v = eng->active_views_head; v != NULL; v = v->next) {
        if (v->js_vm && eng->js_vt) {
            eng->js_vt->release_caches_on_pressure(v->js_vm, pressure_level);
        }
    }
}

ArcrWebView *arcr_webview_create(ArcrEngine *eng, int32_t width, int32_t height,
                                 const ArcrWebViewCallbacks *callbacks) {
    if (!eng) return NULL;
    ArcrWebView *view = (ArcrWebView *)calloc(1u, sizeof(ArcrWebView));
    if (!view) return NULL;
    view->engine   = eng;
    view->width    = (width > 64) ? width : 640;
    view->height   = (height > 64) ? height : 480;
    view->security = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(view->url, sizeof(view->url), "about:blank");
    arcr_str_copy(view->title, sizeof(view->title), "새 탭");
    if (callbacks) view->cb = *callbacks;

    view->next             = eng->active_views_head;
    eng->active_views_head = view;
    return view;
}

static void free_view_document_state(ArcrWebView *view) {
    if (view->js_vm && view->engine->js_vt) {
        view->engine->js_vt->destroy_vm(view->js_vm);
        view->js_vm = NULL;
    }
    arcr_paint_destroy_display_list(view->display_list);
    view->display_list = NULL;
    arcr_layout_destroy(view->layout);
    view->layout = NULL;
    arcr_dom_document_destroy(view->doc);
    view->doc   = NULL;
    view->sheet = NULL;
    view->last_style_share_hits = 0u;
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
    free(view);
}

static void rebuild_layout_and_paint(ArcrWebView *view) {
    if (!view || !view->doc) return;
    arcr_paint_destroy_display_list(view->display_list);
    arcr_layout_destroy(view->layout);

    view->layout       = arcr_layout_build(view->doc, view->engine->fonts, view->width);
    view->display_list = arcr_paint_build_display_list(view->layout);

    if (view->cb.on_dirty_rect) {
        view->cb.on_dirty_rect(view->cb.userdata,
                               (ArcrRect){0, 0, view->width, view->height});
    }
}

void arcr_webview_resize(ArcrWebView *view, int32_t width, int32_t height) {
    if (!view) return;
    int32_t nw = (width > 64) ? width : 64;
    int32_t nh = (height > 64) ? height : 64;
    bool width_changed = (nw != view->width);
    view->width  = nw;
    view->height = nh;
    if (width_changed && view->doc) {
        rebuild_layout_and_paint(view);
    }
}

void arcr_webview_set_scroll(ArcrWebView *view, int32_t scroll_y) {
    if (!view) return;
    int32_t max_scroll = 0;
    if (view->layout && view->layout->total_height > view->height) {
        max_scroll = view->layout->total_height - view->height;
    }
    view->scroll_y = ARCR_CLAMP(scroll_y, 0, max_scroll);
    /* Display list is reused directly on scroll without re-running layout! */
    if (view->cb.on_dirty_rect) {
        view->cb.on_dirty_rect(view->cb.userdata,
                               (ArcrRect){0, 0, view->width, view->height});
    }
}

static void js_host_set_title(void *userdata, const char *utf8_title) {
    ArcrWebView *view = (ArcrWebView *)userdata;
    if (!view || !utf8_title) return;
    arcr_str_copy(view->title, sizeof(view->title), utf8_title);
    if (view->cb.on_title_changed) {
        view->cb.on_title_changed(view->cb.userdata, view->title);
    }
}

static void run_scripts_in_tree(ArcrWebView *view, ArcrAtom script_atom, const ArcrDomNode *n) {
    if (!n) return;
    if (n->kind == ARCR_NODE_ELEMENT && n->tag == script_atom && n->text && n->text_len > 0u) {
        view->engine->js_vt->eval_script(view->js_vm, n->text, n->text_len);
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        run_scripts_in_tree(view, script_atom, ch);
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

    if (view->doc->title && view->doc->title[0] != '\0') {
        arcr_str_copy(view->title, sizeof(view->title), view->doc->title);
    } else {
        arcr_str_copy(view->title, sizeof(view->title), view->url);
    }
    if (view->cb.on_title_changed) {
        view->cb.on_title_changed(view->cb.userdata, view->title);
    }

    /* On-demand Media module load ONLY if <video> or <audio> exists */
    if (view->doc->has_media) {
        (void)ensure_media_module(view->engine);
    }

    /* On-demand JS module load ONLY if <script> or inline handler exists! */
    if (view->doc->has_scripts && view->engine->cfg.enable_javascript) {
        if (ensure_js_module(view->engine)) {
            size_t heap_cap = (view->engine->cfg.profile == ARCR_PROFILE_MINIMUM)
                                  ? (2u * 1024u * 1024u)
                                  : (16u * 1024u * 1024u);
            ArcrJsHostCallbacks hcb = {
                .userdata           = view,
                .set_document_title = js_host_set_title,
                .console_log        = NULL
            };
            view->js_vm = view->engine->js_vt->create_vm(heap_cap, &hcb);
            if (view->js_vm) {
                ArcrAtom script_atom = arcr_atom_intern_lower(&view->engine->atoms, "script", 6u);
                run_scripts_in_tree(view, script_atom, view->doc->root);
            }
        }
    }

    view->sheet = arcr_css_parse_document_styles(view->doc);
    arcr_css_resolve_tree(view->doc, view->sheet, &view->last_style_share_hits);
    rebuild_layout_and_paint(view);
    return true;
}

bool arcr_webview_navigate_url(ArcrWebView *view, const char *utf8_url) {
    if (!view || !utf8_url) return false;
    if (!ensure_net_module(view->engine)) return false;

    char *body = NULL;
    size_t len = 0u;
    ArcrSecurityState sec = ARCR_SEC_LOCAL_OR_NONE;
    char detail[256] = {0};

    bool ok = view->engine->net_vt->load_url_resource(
        utf8_url, &body, &len, &sec, detail, sizeof(detail));
    view->security = sec;
    if (view->cb.on_security_changed) {
        view->cb.on_security_changed(view->cb.userdata, sec, detail);
    }
    if (!ok || !body) return false;

    bool loaded = arcr_webview_load_html(view, utf8_url, body, len);
    view->security = sec;
    view->engine->net_vt->free_resource(body);
    return loaded;
}

void arcr_webview_ime_composition(ArcrWebView *view, const char *utf8_comp_str,
                                  int32_t cursor_offset, bool is_committed) {
    (void)cursor_offset;
    if (!view) return;
    if (is_committed || !utf8_comp_str) {
        view->ime_comp[0] = '\0';
    } else {
        arcr_str_copy(view->ime_comp, sizeof(view->ime_comp), utf8_comp_str);
    }
}

bool arcr_webview_paint(ArcrWebView *view, uint32_t *argb_pixels,
                        int32_t width, int32_t height, int32_t stride_pixels) {
    if (!view || !argb_pixels) return false;
    ArcrRect vp = {0, 0, width, height};
    arcr_paint_rasterize(view->display_list, view->engine->accel, view->engine->fonts,
                         argb_pixels, width, height, stride_pixels, vp, view->scroll_y);
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
