/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * js/arcr_js.h - ArcradientJS Bytecode VM & Live Browser DOM Bridge Interface
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
    void        *userdata;
    ArcrDocument *doc; /* Optional live DOM document bound to this VM */

    void (*set_document_title)(void *userdata, const char *utf8_title);
    void (*console_log)(void *userdata, const char *utf8_msg);
    void (*navigate_to)(void *userdata, const char *utf8_url);
    bool (*storage_get)(void *userdata, const char *key, char *out_val, size_t cap);
    void (*storage_set)(void *userdata, const char *key, const char *val);
} ArcrJsHostCallbacks;

typedef struct ArcrJsVTable {
    uint32_t    struct_size;
    const char *engine_backend_name;
    ArcrJsVm *(*create_vm)(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb);
    void      (*destroy_vm)(ArcrJsVm *vm);
    bool      (*eval_script)(ArcrJsVm *vm, const char *utf8_code, size_t code_len);
    /* Dispatches DOM event ("click", "input", "submit") to registered JS listeners & onclick="" */
    bool      (*dispatch_dom_event)(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type);
    size_t    (*get_used_heap_bytes)(const ArcrJsVm *vm);
    void      (*release_caches_on_pressure)(ArcrJsVm *vm, int pressure_level);
} ArcrJsVTable;

#endif /* ARCR_JS_H */
