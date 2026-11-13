/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * js/arcr_js.h - Versioned Web Host <-> JavaScript Engine Contract (ABI v2)
 *
 * Strictly separates the JS Virtual Machine (`js/vm`, `js/builtins`, `js/modules`)
 * from the Browser Web Host (`web/bindings`, `core/arcr_engine.c`).
 */
#ifndef ARCR_JS_H
#define ARCR_JS_H

#include "../core/arcr_dom.h"
#include "../include/arcr_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArcrJsVm ArcrJsVm;

typedef struct ArcrJsHostCallbacks {
    void         *userdata;
    ArcrDocument *doc; /* Live DOM document bound to this realm */
    int32_t       viewport_width;
    int32_t       viewport_height;
    bool          prefers_dark;
    bool          private_browsing;

    const char *(*get_current_url)(void *userdata);
    void (*set_document_title)(void *userdata, const char *utf8_title);
    void (*console_log)(void *userdata, const char *utf8_msg);
    void (*navigate_to)(void *userdata, const char *utf8_url);
    bool (*storage_get)(void *userdata, const char *key, char *out_val, size_t cap);
    void (*storage_set)(void *userdata, const char *key, const char *val);

    /* Legacy synchronous HTTP bridge (preserved for v1 compatibility) */
    bool (*http_fetch)(void *userdata, const char *url, const char *method,
                       const char *body, size_t body_len, const char *content_type,
                       int *out_status, char *out_resp_ctype, size_t ctype_cap,
                       uint8_t **out_resp_bytes, size_t *out_resp_len);
    void (*http_free)(void *userdata, uint8_t *resp_bytes);

    /* v2 Extended Browser Host Bridges (Layout, CSS, ESM Loader, Full Fetch/CORS, History) */
    void (*ensure_layout)(void *userdata);
    bool (*css_supports)(void *userdata, const char *prop_or_cond, const char *val_or_null);

    /* Fetches an external ES module source with MIME & CORS validation + request trace */
    bool (*load_module_source)(void *userdata, const char *resolved_url, const char *referrer_url,
                               char **out_utf8_code, size_t *out_code_len,
                               char *out_final_url, size_t final_url_cap,
                               char *out_mime, size_t mime_cap,
                               char *out_error, size_t err_cap);
    void (*free_host_buffer)(void *userdata, void *buf);

    /* Extended HTTP Fetch bridge with full headers, final_url, CORS, credentials & cancel_flag */
    bool (*http_fetch_ex)(void *userdata, const char *url, const char *method,
                          const char *req_headers_raw, const uint8_t *body, size_t body_len,
                          bool include_credentials, const volatile int *cancel_flag,
                          int *out_status, char *out_final_url, size_t final_url_cap,
                          char *out_content_type, size_t ctype_cap,
                          char **out_resp_headers_raw, size_t *out_resp_headers_len,
                          uint8_t **out_resp_bytes, size_t *out_resp_len,
                          char *out_blocked_reason, size_t reason_cap);

    /* Session History & HTML Parser Insertion */
    void   (*history_push_or_replace)(void *userdata, const char *new_url,
                                      const char *state_json, bool is_replace);
    bool   (*history_go)(void *userdata, int32_t delta);
    size_t (*history_length)(void *userdata);
    bool   (*document_write)(void *userdata, const char *utf8_html, size_t len);
} ArcrJsHostCallbacks;

typedef struct ArcrJsVTable {
    uint32_t    struct_size;
    uint32_t    vtable_version; /* ARCR_JS_VTABLE_VERSION */
    const char *engine_backend_name;

    ArcrJsVm *(*create_vm)(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb);
    void      (*destroy_vm)(ArcrJsVm *vm);
    bool      (*eval_script)(ArcrJsVm *vm, const char *utf8_code, size_t code_len);

    /* Dispatches DOM event ("click", "input", "submit", "DOMContentLoaded", "load", etc.) */
    bool      (*dispatch_dom_event)(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type);
    bool      (*dispatch_dom_event_ex)(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type,
                                       int32_t client_x, int32_t client_y, int key_code,
                                       bool *out_default_prevented);

    /* Advances Promise microtasks, async Fetch completions, timers, and requestAnimationFrame(ts) */
    bool      (*tick_timers_and_raf)(ArcrJsVm *vm, uint64_t now_ms, bool *out_dom_mutated);
    bool      (*has_pending_timers)(const ArcrJsVm *vm);

    size_t    (*get_used_heap_bytes)(const ArcrJsVm *vm);
    void      (*release_caches_on_pressure)(ArcrJsVm *vm, int pressure_level);

    /* v2 Script Kind, Source URL, Import Maps, ES Modules & Lifecycle State */
    bool      (*eval_script_ex)(ArcrJsVm *vm, const char *utf8_code, size_t code_len,
                                const char *source_url);
    bool      (*register_import_map)(ArcrJsVm *vm, const char *json_utf8, size_t json_len,
                                     const char *base_url);
    bool      (*eval_module)(ArcrJsVm *vm, const char *utf8_code_or_null, size_t code_len,
                             const char *module_url);
    void      (*set_ready_state)(ArcrJsVm *vm, const char *ready_state);
    void      (*notify_url_and_popstate)(ArcrJsVm *vm, const char *new_url,
                                         const char *state_json_or_null, bool fire_popstate);
} ArcrJsVTable;

#endif /* ARCR_JS_H */
