/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/vm/ajs_vm.h - Public Opaque-Handle C11 API
 *
 * Strictly owns:
 * - Values, UTF-16 observable strings, Objects, Descriptors, Proxies, Symbols, BigInt
 * - Lexer/Parser (Script & Module, strict/sloppy, ASI, TDZ, early errors, source line:col)
 * - Bytecode Compiler & Interpreter, Closures, Classes, Generators, Async/Await
 * - Tracing Mark-Sweep Garbage Collector with explicit AjsHandleScope roots & cycle hooks
 * - Built-ins (ES2026 + Annex B + RegExp + JSON + TypedArray/Atomics + ECMA-402 Intl)
 * - Source Text Module Record (parse -> link -> evaluate, live bindings, circular, TLA, import())
 *
 * Zero DOM/Browser/QuickJS dependencies. Safe on 32-bit x86 SSE1 (Win2000 SP4) & 64-bit x86-64.
 */
#ifndef AJS_VM_H
#define AJS_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AJS_API_VERSION 0x00010000u
#define AJS_ENGINE_NAME "ArcradientJS/1.0-PureC11-ES2026"

typedef struct AjsVm           AjsVm;
typedef struct AjsRealm        AjsRealm;
typedef struct AjsModuleRecord AjsModuleRecord;
typedef uint64_t               AjsValue;

/* Explicit stack-allocated GC root handle scope for native C embeddings */
typedef struct AjsHandleScope {
    AjsVm                 *vm;
    size_t                 saved_top;
    struct AjsHandleScope *prev;
} AjsHandleScope;

typedef enum AjsType {
    AJS_TYPE_UNDEFINED = 0,
    AJS_TYPE_NULL      = 1,
    AJS_TYPE_BOOLEAN   = 2,
    AJS_TYPE_NUMBER    = 3,
    AJS_TYPE_STRING    = 4,
    AJS_TYPE_SYMBOL    = 5,
    AJS_TYPE_BIGINT    = 6,
    AJS_TYPE_OBJECT    = 7,
    AJS_TYPE_FUNCTION  = 8
} AjsType;

typedef enum AjsErrorPhase {
    AJS_ERR_NONE       = 0,
    AJS_ERR_PARSE      = 1, /* Lexer / Parser / Static Early Error */
    AJS_ERR_RESOLUTION = 2, /* ES Module resolve / link / export binding error */
    AJS_ERR_RUNTIME    = 3  /* Evaluation / runtime thrown exception */
} AjsErrorPhase;

typedef struct AjsErrorInfo {
    AjsErrorPhase phase;
    char          name[64];     /* e.g. "SyntaxError", "TypeError", "ReferenceError" */
    char          message[512];
    char          url[512];
    uint32_t      line;         /* 1-based */
    uint32_t      column;       /* 1-based */
    char          stack[2048];
} AjsErrorInfo;

typedef AjsValue (*AjsNativeFn)(AjsVm *vm, AjsValue this_val,
                                int argc, const AjsValue *argv, void *userdata);
typedef void     (*AjsFinalizerFn)(AjsVm *vm, void *opaque);
typedef void     (*AjsGcMarkFn)(AjsVm *vm, void *opaque, void (*mark_val)(AjsVm *vm, AjsValue v));
typedef bool     (*AjsInterruptFn)(AjsVm *vm, void *userdata);
typedef void     (*AjsRejectionTrackerFn)(AjsVm *vm, AjsValue promise, AjsValue reason,
                                          bool is_handled, void *userdata);

/* Host ES Module Resolution & Loading Hooks */
typedef bool (*AjsModuleResolveFn)(AjsVm *vm, const char *specifier, const char *referrer_url,
                                   char *out_resolved_url, size_t out_cap,
                                   char *out_err_msg, size_t err_cap, void *userdata);

typedef AjsModuleRecord *(*AjsModuleLoadFn)(AjsVm *vm, const char *resolved_url,
                                            const char *referrer_url,
                                            char *out_err_msg, size_t err_cap, void *userdata);

typedef struct AjsVmConfig {
    size_t                heap_limit_bytes;  /* 0 = default 32 MB */
    size_t                max_call_depth;    /* 0 = default 512 frames */
    AjsInterruptFn        interrupt_fn;
    void                 *interrupt_userdata;
    AjsRejectionTrackerFn rejection_fn;
    void                 *rejection_userdata;
    AjsModuleResolveFn    module_resolve_fn;
    AjsModuleLoadFn       module_load_fn;
    void                 *module_userdata;
} AjsVmConfig;

/* VM & Realm Lifecycle */
void      ajs_default_config(AjsVmConfig *out_cfg);
AjsVm    *ajs_vm_create(const AjsVmConfig *cfg);
void      ajs_vm_destroy(AjsVm *vm);
void      ajs_vm_set_userdata(AjsVm *vm, void *userdata);
void     *ajs_vm_get_userdata(const AjsVm *vm);
AjsRealm *ajs_vm_main_realm(AjsVm *vm);
AjsRealm *ajs_realm_create(AjsVm *vm); /* Separate realm for Test262 $262.createRealm / iframes */
AjsValue  ajs_realm_global_object(AjsRealm *realm);
AjsValue  ajs_global_object(AjsVm *vm);
void      ajs_vm_set_interrupt_handler(AjsVm *vm, AjsInterruptFn fn, void *userdata);
void      ajs_vm_set_module_hooks(AjsVm *vm, AjsModuleResolveFn resolve_fn,
                                  AjsModuleLoadFn load_fn, void *userdata);

/* Garbage Collection & Explicit Root Handles */
void     ajs_handle_scope_open(AjsVm *vm, AjsHandleScope *scope);
void     ajs_handle_scope_close(AjsVm *vm, AjsHandleScope *scope);
AjsValue ajs_handle_escape(AjsVm *vm, AjsHandleScope *scope, AjsValue val);
uint32_t ajs_pin_value(AjsVm *vm, AjsValue val);     /* Persistent GC root slot (1-based; 0 = invalid) */
AjsValue ajs_get_pinned(const AjsVm *vm, uint32_t pin_id);
void     ajs_unpin_value(AjsVm *vm, uint32_t pin_id);
void     ajs_gc_collect(AjsVm *vm);
size_t   ajs_gc_used_bytes(const AjsVm *vm);

/* Primitive & Value Constructors / Type Predicates */
AjsValue ajs_undefined(void);
AjsValue ajs_null(void);
AjsValue ajs_bool(bool b);
AjsValue ajs_number(double d);
AjsValue ajs_int32(int32_t i);
AjsValue ajs_uint32(uint32_t u);
AjsValue ajs_string_utf8(AjsVm *vm, const char *utf8, size_t byte_len);
AjsValue ajs_string_cstr(AjsVm *vm, const char *cstr);
AjsValue ajs_string_utf16(AjsVm *vm, const uint16_t *units, size_t unit_len);
AjsValue ajs_symbol_new(AjsVm *vm, const char *desc_or_null);
AjsValue ajs_bigint_from_i64(AjsVm *vm, int64_t v);

AjsType  ajs_typeof(AjsVm *vm, AjsValue v);
bool     ajs_is_undefined(AjsValue v);
bool     ajs_is_null(AjsValue v);
bool     ajs_is_nullish(AjsValue v);
bool     ajs_is_bool(AjsValue v);
bool     ajs_is_number(AjsValue v);
bool     ajs_is_string(AjsVm *vm, AjsValue v);
bool     ajs_is_object(AjsVm *vm, AjsValue v);
bool     ajs_is_callable(AjsVm *vm, AjsValue v);
bool     ajs_is_array(AjsVm *vm, AjsValue v);
bool     ajs_is_promise(AjsVm *vm, AjsValue v);
bool     ajs_same_value(AjsVm *vm, AjsValue a, AjsValue b);

/* Conversions (ECMA-262 ToBoolean, ToNumber, ToInt32, ToUint32, ToString) */
bool     ajs_to_bool(AjsVm *vm, AjsValue v);
bool     ajs_to_number(AjsVm *vm, AjsValue v, double *out_num);
bool     ajs_to_int32(AjsVm *vm, AjsValue v, int32_t *out_i32);
bool     ajs_to_uint32(AjsVm *vm, AjsValue v, uint32_t *out_u32);
/* Returns newly malloc'd UTF-8 NUL-terminated buffer; caller frees via ajs_free_cstring() */
char    *ajs_to_cstring_len(AjsVm *vm, AjsValue v, size_t *out_byte_len);
char    *ajs_to_cstring(AjsVm *vm, AjsValue v);
void     ajs_free_cstring(AjsVm *vm, char *str);

/* Objects, Prototypes, Descriptors, Native Functions & Host Opaque Wrappers */
AjsValue ajs_new_object(AjsVm *vm);
AjsValue ajs_new_object_with_proto(AjsVm *vm, AjsValue proto);
AjsValue ajs_new_array(AjsVm *vm, uint32_t initial_len);
AjsValue ajs_new_function(AjsVm *vm, AjsNativeFn fn, const char *name,
                          int length, void *userdata);
AjsValue ajs_new_constructor(AjsVm *vm, AjsNativeFn ctor_fn, const char *name,
                             int length, AjsValue prototype_obj, void *userdata);

void     ajs_set_opaque(AjsVm *vm, AjsValue obj, uint32_t class_tag,
                        void *opaque, AjsFinalizerFn finalizer, AjsGcMarkFn gc_mark);
void    *ajs_get_opaque(AjsVm *vm, AjsValue obj, uint32_t expected_class_tag);
bool     ajs_set_prototype(AjsVm *vm, AjsValue obj, AjsValue proto);
AjsValue ajs_get_prototype(AjsVm *vm, AjsValue obj);

#define AJS_PROP_WRITABLE     (1u << 0)
#define AJS_PROP_ENUMERABLE   (1u << 1)
#define AJS_PROP_CONFIGURABLE (1u << 2)
#define AJS_PROP_DEFAULT      (AJS_PROP_WRITABLE | AJS_PROP_ENUMERABLE | AJS_PROP_CONFIGURABLE)

AjsValue ajs_get_prop_str(AjsVm *vm, AjsValue obj, const char *name);
AjsValue ajs_get_prop_index(AjsVm *vm, AjsValue obj, uint32_t idx);
bool     ajs_set_prop_str(AjsVm *vm, AjsValue obj, const char *name, AjsValue val);
bool     ajs_set_prop_index(AjsVm *vm, AjsValue obj, uint32_t idx, AjsValue val);
bool     ajs_define_prop_str(AjsVm *vm, AjsValue obj, const char *name,
                             AjsValue val, uint32_t prop_flags);
bool     ajs_define_accessor_str(AjsVm *vm, AjsValue obj, const char *name,
                                 AjsValue getter_fn, AjsValue setter_fn, uint32_t prop_flags);
bool     ajs_has_prop_str(AjsVm *vm, AjsValue obj, const char *name);
bool     ajs_delete_prop_str(AjsVm *vm, AjsValue obj, const char *name);

/* ArrayBuffer & TypedArray Zero-Copy / Copy Access */
typedef enum AjsTypedArrayKind {
    AJS_TA_NONE          = 0,
    AJS_TA_INT8          = 1,
    AJS_TA_UINT8         = 2,
    AJS_TA_UINT8_CLAMPED = 3,
    AJS_TA_INT16         = 4,
    AJS_TA_UINT16        = 5,
    AJS_TA_INT32         = 6,
    AJS_TA_UINT32        = 7,
    AJS_TA_FLOAT32       = 8,
    AJS_TA_FLOAT64       = 9,
    AJS_TA_BIGINT64      = 10,
    AJS_TA_BIGUINT64     = 11
} AjsTypedArrayKind;

AjsValue ajs_new_array_buffer_copy(AjsVm *vm, const uint8_t *bytes, size_t byte_len);
bool     ajs_detach_array_buffer(AjsVm *vm, AjsValue ab); /* For transfer / $262.detachArrayBuffer */
uint8_t *ajs_get_array_buffer_data(AjsVm *vm, AjsValue ab, size_t *out_byte_len);
AjsValue ajs_new_typed_array(AjsVm *vm, AjsTypedArrayKind kind, const uint8_t *bytes, size_t elem_count);
uint8_t *ajs_get_buffer_source_bytes(AjsVm *vm, AjsValue ab_or_view,
                                     size_t *out_byte_len, AjsTypedArrayKind *out_kind);

/* Promises & Microtask Job Queue */
AjsValue ajs_promise_create(AjsVm *vm, AjsValue *out_resolve_fn, AjsValue *out_reject_fn);
void     ajs_enqueue_microtask(AjsVm *vm, AjsValue callback_fn);
bool     ajs_has_pending_jobs(const AjsVm *vm);
bool     ajs_drain_microtasks(AjsVm *vm);

/* Exceptions & Diagnostics */
bool     ajs_has_exception(const AjsVm *vm);
AjsValue ajs_throw(AjsVm *vm, AjsValue exc_val);
AjsValue ajs_throw_error(AjsVm *vm, const char *error_ctor_name, const char *fmt, ...);
AjsValue ajs_take_exception(AjsVm *vm, AjsErrorInfo *out_info_or_null);
void     ajs_clear_exception(AjsVm *vm);

/* Invocation & Classic Script Evaluation */
AjsValue ajs_call(AjsVm *vm, AjsValue fn, AjsValue this_val, int argc, const AjsValue *argv);
AjsValue ajs_construct(AjsVm *vm, AjsValue ctor, int argc, const AjsValue *argv);
bool     ajs_eval_script(AjsVm *vm, const char *utf8_code, size_t code_len,
                         const char *source_url, bool force_strict,
                         AjsValue *out_result_or_null, AjsErrorInfo *out_err_or_null);
bool     ajs_eval_script_in_realm(AjsRealm *realm, const char *utf8_code, size_t code_len,
                                  const char *source_url, bool force_strict,
                                  AjsValue *out_result_or_null, AjsErrorInfo *out_err_or_null);

/* JSON Convenience */
AjsValue ajs_json_parse(AjsVm *vm, const char *utf8_json, size_t byte_len);
char    *ajs_json_stringify(AjsVm *vm, AjsValue val, size_t *out_byte_len);

/* ECMAScript Source Text Module Record (Parse -> Link -> Evaluate) */
AjsModuleRecord *ajs_module_parse(AjsVm *vm, const char *utf8_code, size_t code_len,
                                  const char *module_url, AjsErrorInfo *out_err_or_null);
const char      *ajs_module_url(const AjsModuleRecord *mod);
bool             ajs_module_link(AjsVm *vm, AjsModuleRecord *mod, AjsErrorInfo *out_err_or_null);
/* Evaluates module graph (supporting circular live bindings & top-level await).
 * Returns Promise or completion value; sets *out_err_or_null on synchronous/rejected error. */
bool             ajs_module_evaluate(AjsVm *vm, AjsModuleRecord *mod,
                                     AjsValue *out_promise_or_val, AjsErrorInfo *out_err_or_null);
AjsValue         ajs_module_namespace(AjsVm *vm, AjsModuleRecord *mod);

#ifdef __cplusplus
}
#endif

#endif /* AJS_VM_H */
