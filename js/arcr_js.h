/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * js/arcr_js.h - QuickJS-NG ES2026 Bytecode Runtime & Full Modern Browser Web API Bridge
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
    ArcrDocument *doc; /* Live DOM document bound to this VM */
    int32_t       viewport_width;
    int32_t       viewport_height;
    bool          prefers_dark;

    const char *(*get_current_url)(void *userdata);
    void (*set_document_title)(void *userdata, const char *utf8_title);
    void (*console_log)(void *userdata, const char *utf8_msg);
    void (*navigate_to)(void *userdata, const char *utf8_url);
    bool (*storage_get)(void *userdata, const char *key, char *out_val, size_t cap);
    void (*storage_set)(void *userdata, const char *key, const char *val);

    /* Host HTTP fetch bridge for window.fetch() and XMLHttpRequest */
    bool (*http_fetch)(void *userdata, const char *url, const char *method,
                       const char *body, size_t body_len, const char *content_type,
                       int *out_status, char *out_resp_ctype, size_t ctype_cap,
                       uint8_t **out_resp_bytes, size_t *out_resp_len);
    void (*http_free)(void *userdata, uint8_t *resp_bytes);
} ArcrJsHostCallbacks;

typedef struct ArcrJsVTable {
    uint32_t    struct_size;
    const char *engine_backend_name;
    ArcrJsVm *(*create_vm)(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb);
    void      (*destroy_vm)(ArcrJsVm *vm);
    bool      (*eval_script)(ArcrJsVm *vm, const char *utf8_code, size_t code_len);

    /* Dispatches DOM event ("click", "input", "submit", "DOMContentLoaded", "load", etc.) */
    bool      (*dispatch_dom_event)(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type);
    bool      (*dispatch_dom_event_ex)(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type,
                                       int32_t client_x, int32_t client_y, int key_code,
                                       bool *out_default_prevented);

    /* Advances Promise microtasks, setTimeout/setInterval, and requestAnimationFrame(ts) */
    bool      (*tick_timers_and_raf)(ArcrJsVm *vm, uint64_t now_ms, bool *out_dom_mutated);
    bool      (*has_pending_timers)(const ArcrJsVm *vm);

    size_t    (*get_used_heap_bytes)(const ArcrJsVm *vm);
    void      (*release_caches_on_pressure)(ArcrJsVm *vm, int pressure_level);
} ArcrJsVTable;

#endif /* ARCR_JS_H */
