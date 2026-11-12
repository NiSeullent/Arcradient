/*
 * Arcradient (MSHTML-Next)
 * js/arcr_js.h - ArcradientJS Common C Interface (Portable / JSC Wrapper)
 */
#ifndef ARCR_JS_H
#define ARCR_JS_H

#include "../include/arcr_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArcrJsVm ArcrJsVm;

typedef struct ArcrJsHostCallbacks {
    void *userdata;
    void (*set_document_title)(void *userdata, const char *utf8_title);
    void (*console_log)(void *userdata, const char *utf8_msg);
} ArcrJsHostCallbacks;

typedef struct ArcrJsVTable {
    uint32_t    struct_size;
    const char *engine_backend_name;
    ArcrJsVm *(*create_vm)(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb);
    void      (*destroy_vm)(ArcrJsVm *vm);
    bool      (*eval_script)(ArcrJsVm *vm, const char *utf8_code, size_t code_len);
    size_t    (*get_used_heap_bytes)(const ArcrJsVm *vm);
    void      (*release_caches_on_pressure)(ArcrJsVm *vm, int pressure_level);
} ArcrJsVTable;

#endif /* ARCR_JS_H */
